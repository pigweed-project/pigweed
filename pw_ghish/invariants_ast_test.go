// Copyright 2026 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

// This file enforces architectural, style, and safety invariants across the
// pw_ghish Go source tree using static AST inspection (go/parser and go/ast).
// It verifies that production files do not use legacy Go idioms (`interface{}`,
// `io/ioutil`), do not call `os.Exit` outside `cmd/main.go`, do not hardcode
// Fuchsia or Pigweed project policy outside `profile.go`, do not leak
// internal 1P partner names, and do not introduce new unannotated
// `if err != nil { return nil }` silent failure branches.

package pw_ghish

import (
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"path/filepath"
	"regexp"
	"slices"
	"strconv"
	"strings"
	"testing"

	"pigweed.dev/pw_ghish/internal/astcheck"
)

// TestInvariants_CobraCommandsUseRunE verifies that every cobra.Command composite
// literal in pw_ghish uses RunE / PreRunE / PersistentPreRunE / PostRunE and never
// the error-swallowing void Run / PreRun / PersistentPreRun / PostRun fields.
func TestInvariants_CobraCommandsUseRunE(t *testing.T) {
	prohibitedFields := []string{"Run", "PreRun", "PersistentPreRun", "PostRun"}
	files := astcheck.MustLoadFiles(t, ".", "worktree", "cmd")
	// FindCompositeLits locates every cobra.Command{} struct literal; m.Field
	// inspects its key-value fields, and m.FuncName identifies the enclosing
	// function (replacing the bespoke isAliasCloneLiteral helper for root.go).
	for _, m := range astcheck.FindCompositeLits(files, "cobra.Command") {
		if filepath.Base(m.File.Path) == "root.go" && m.FuncName == "newRootAlias" {
			continue
		}
		for _, field := range prohibitedFields {
			if val, ok := m.Field(field); ok {
				t.Errorf("%s:%d: cobra.Command defines %s (at %s:%d); must use %sE returning error so failures are never silently ignored",
					m.File.Path, m.Line, field, m.File.Path, m.File.Line(val.Pos()), field)
			}
		}
	}
}

// TestInvariants_NoDirectExitOrStdoutPrint verifies that no non-main production
// file calls os.Exit or writes directly to stdout via fmt.Print/Println/Printf.
func TestInvariants_NoDirectExitOrStdoutPrint(t *testing.T) {
	files := astcheck.MustLoadFiles(t, ".", "worktree")
	for _, cs := range astcheck.FindCalls(files, "os.Exit", "fmt.Print*") {
		if cs.Selector == "os.Exit" {
			t.Errorf("%s: os.Exit called outside cmd/main.go; return an error instead", cs.Pos)
			continue
		}
		t.Errorf("%s: direct call to %s writes to os.Stdout and breaks hermetic test capture; use fmt.F%s(cmd.OutOrStdout(), ...)",
			cs.Pos, cs.Selector, strings.ToLower(cs.Callee[:1])+cs.Callee[1:])
	}
}

var gerritReviewEndpointRe = regexp.MustCompile(`(?:^|/)review(?:[/?#]|$)`)

func exprTargetsReviewEndpoint(e ast.Expr, localExprs map[string]ast.Expr, seen map[string]bool) bool {
	if e == nil {
		return false
	}
	switch x := e.(type) {
	case *ast.BasicLit:
		if x.Kind != token.STRING {
			return false
		}
		s, err := strconv.Unquote(x.Value)
		if err != nil {
			s = strings.Trim(x.Value, `"`+"`")
		}
		return strings.Contains(s, "/review") && gerritReviewEndpointRe.MatchString(s)
	case *ast.Ident:
		if seen[x.Name] {
			return false
		}
		next, ok := localExprs[x.Name]
		if !ok {
			return false
		}
		seen[x.Name] = true
		return exprTargetsReviewEndpoint(next, localExprs, seen)
	case *ast.BinaryExpr:
		return exprTargetsReviewEndpoint(x.X, localExprs, seen) ||
			exprTargetsReviewEndpoint(x.Y, localExprs, seen)
	case *ast.CallExpr:
		if id, ok := x.Fun.(*ast.Ident); ok && id.Name == "revisionEndpoint" && len(x.Args) >= 3 {
			if suffixTargetsReview(x.Args[2], localExprs, seen) {
				return true
			}
		}
		if sel, ok := x.Fun.(*ast.SelectorExpr); ok {
			if pkg, ok := sel.X.(*ast.Ident); ok && pkg.Name == "fmt" && sel.Sel.Name == "Sprintf" {
				for i, arg := range x.Args {
					if i == 0 {
						if exprTargetsReviewEndpoint(arg, localExprs, seen) {
							return true
						}
						continue
					}
					if suffixTargetsReview(arg, localExprs, seen) {
						return true
					}
				}
			}
		}
		for _, arg := range x.Args {
			if exprTargetsReviewEndpoint(arg, localExprs, seen) {
				return true
			}
		}
	}
	return false
}

func suffixTargetsReview(e ast.Expr, localExprs map[string]ast.Expr, seen map[string]bool) bool {
	switch x := e.(type) {
	case *ast.BasicLit:
		if x.Kind != token.STRING {
			return false
		}
		s, err := strconv.Unquote(x.Value)
		if err != nil {
			s = strings.Trim(x.Value, `"`+"`")
		}
		return gerritReviewEndpointRe.MatchString(s)
	case *ast.Ident:
		if seen[x.Name] {
			return false
		}
		next, ok := localExprs[x.Name]
		if !ok {
			return false
		}
		seen[x.Name] = true
		return suffixTargetsReview(next, localExprs, seen)
	}
	return exprTargetsReviewEndpoint(e, localExprs, seen)
}

func checkFuncBodyForDirectReviewCalls(t *testing.T, fc *astcheck.File, fn *ast.FuncDecl) {
	t.Helper()
	localExprs := make(map[string]ast.Expr)
	ast.Inspect(fn.Body, func(n ast.Node) bool {
		switch stmt := n.(type) {
		case *ast.AssignStmt:
			for i, lhs := range stmt.Lhs {
				if id, ok := lhs.(*ast.Ident); ok && i < len(stmt.Rhs) {
					localExprs[id.Name] = stmt.Rhs[i]
				}
			}
		case *ast.ValueSpec:
			for i, id := range stmt.Names {
				if i < len(stmt.Values) {
					localExprs[id.Name] = stmt.Values[i]
				}
			}
		}
		return true
	})

	ast.Inspect(fn.Body, func(n ast.Node) bool {
		call, ok := n.(*ast.CallExpr)
		if !ok {
			return true
		}
		sel, ok := call.Fun.(*ast.SelectorExpr)
		if !ok {
			return true
		}
		if (sel.Sel.Name == "do" || sel.Sel.Name == "doRaw") && len(call.Args) >= 3 {
			if exprTargetsReviewEndpoint(call.Args[2], localExprs, make(map[string]bool)) {
				t.Errorf("%s:%d: direct %s call to Gerrit /review endpoint in %s; use GerritClient.SetReview (via ChangeContext.SetReview) to preserve Drafts: \"KEEP\" and labels-unmarshal safety",
					fc.Path, fc.Line(call.Pos()), sel.Sel.Name, fn.Name.Name)
			}
		}
		return true
	})
}

// TestInvariants_SetReviewGoesThroughGerritClient verifies that no production
// code targets a Gerrit `/review` endpoint via `do` / `doRaw` outside
// `GerritClient.SetReview` in gerrit.go, preserving `Drafts: "KEEP"` and
// labels-unmarshal safety.
func TestInvariants_SetReviewGoesThroughGerritClient(t *testing.T) {
	for _, fc := range astcheck.MustLoadFiles(t, ".", "worktree", "cmd") {
		for _, decl := range fc.AST.Decls {
			fn, ok := decl.(*ast.FuncDecl)
			if !ok || fn.Body == nil {
				continue
			}
			if filepath.Base(fc.Path) == "gerrit.go" && fn.Name.Name == "SetReview" {
				continue
			}
			checkFuncBodyForDirectReviewCalls(t, fc, fn)
		}
	}
}

// TestInvariants_BoundedHTTPBodyReads verifies that every HTTP request/response
// body read uses readBoundedBody, readBoundedLog, or io.LimitReader rather than
// an unbounded io.ReadAll(resp.Body).
func TestInvariants_BoundedHTTPBodyReads(t *testing.T) {
	files := astcheck.MustLoadFiles(t, ".", "worktree", "cmd")
	for _, cs := range astcheck.FindCalls(files, "io.ReadAll", "ioutil.ReadAll") {
		if len(cs.Call.Args) != 1 {
			continue
		}
		if sel, ok := ast.Unparen(cs.Call.Args[0]).(*ast.SelectorExpr); ok && sel.Sel.Name == "Body" {
			t.Errorf("%s: unbounded %s(%s); use readBoundedBody, readBoundedLog, or io.LimitReader",
				cs.Pos, cs.Selector, cs.File.TextRange(cs.Call.Args[0].Pos(), cs.Call.Args[0].End()))
		}
	}
}

// TestInvariants_NoRawANSIEscapesOutsideUIGo verifies that no production Go
// file outside ui.go embeds raw ANSI CSI escape sequences (`\x1b[` or `\033[`),
// ensuring all terminal styling goes through ColorEnabled and PadRightStyled in
// ui.go (AGENTS.md invariant #15).
func TestInvariants_NoRawANSIEscapesOutsideUIGo(t *testing.T) {
	for _, fc := range astcheck.MustLoadFiles(t, ".", "worktree", "cmd") {
		if filepath.Base(fc.Path) == "ui.go" {
			continue
		}
		src := string(fc.Src)
		if strings.Contains(src, `\x1b[`) || strings.Contains(src, `\033[`) {
			t.Errorf("%s: contains raw ANSI escape sequence outside ui.go; use Style* and PadRightStyled helpers in ui.go so NO_COLOR, non-TTY pipes, and coding agents receive pure plaintext (AGENTS.md invariant #15)", fc.Path)
		}
	}
}

type wrapsErrorRecorder struct {
	testing.TB
	errs []string
}

func (r *wrapsErrorRecorder) Helper() {}
func (r *wrapsErrorRecorder) Errorf(f string, a ...any) {
	r.errs = append(r.errs, fmt.Sprintf(f, a...))
}

// TestInvariants_DomainWrappers verifies that all //ghish:wraps domain wrappers
// are respected across pw_ghish, pw_ghish/worktree, and pw_ghish/cmd.
func TestInvariants_DomainWrappers(t *testing.T) {
	files := astcheck.MustLoadFiles(t, ".", "worktree", "cmd")
	astcheck.AssertWraps(t, files)

	for _, tc := range []struct {
		src, wantWrapper string
	}{
		{`package p; import "os/exec"; func f() { _ = exec.Command("git") }`, "ExecCommand"},
		{`package p; import "os/exec"; func f() { _, _ = exec.LookPath("git") }`, "LookPath"},
		{`package p; import "net/http"; func f() { _ = &http.Client{} }`, "EnsureSafeHTTPClient"},
		{`package p; import "net/http"; func f() { _ = &http.Transport{} }`, "EnsureSafeHTTPClient"},
		{`package p; import "net/http"; func f() { _ = http.DefaultClient }`, "EnsureSafeHTTPClient"},
		{`package p; import "net/http"; func f() { _, _ = http.Get("https://example.com") }`, "EnsureSafeHTTPClient"},
		{`package p; import "net/http"; func f() { _, _ = http.NewRequest("GET", "https://example.com", nil) }`, "EnsureSafeHTTPClient"},
		{`package p; import ("context"; "net/http"); func f(ctx context.Context) { _, _ = http.NewRequestWithContext(ctx, "GET", "https://example.com", nil) }`, "ExecuteWithAuthStrategy"},
		{`package pw_ghish; func f() { _ = &RealGitRunner{} }`, "NewGitClientInDir"},
		{`package worktree; import "pigweed.dev/pw_ghish"; func f() { _ = &pw_ghish.RealGitRunner{} }`, "NewGitClientInDir"},
		{`package p; import "os"; func f() { _ = os.RemoveAll("/tmp/x") }`, "SafeRemoveAll"},
	} {
		fset := token.NewFileSet()
		f, err := parser.ParseFile(fset, "bad.go", tc.src, 0)
		if err != nil {
			t.Fatalf("ParseFile(%s): %v", tc.src, err)
		}
		rec := &wrapsErrorRecorder{TB: t}
		astcheck.AssertWraps(rec, append(slices.Clone(files), &astcheck.File{AST: f, Path: "bad.go", Src: []byte(tc.src), Fset: fset}))
		if len(rec.errs) != 1 || !strings.Contains(rec.errs[0], "bypasses wrapper "+tc.wantWrapper) {
			t.Errorf("for %s: got %v, want 1 error mentioning wrapper %s", tc.src, rec.errs, tc.wantWrapper)
		}
	}
}

// findTestProcessSpawns reports direct process-spawning calls or references
// (exec.Command, exec.CommandContext, exec.Cmd literals, os.StartProcess,
// syscall.Exec, syscall.ForkExec, and ExecCommand / ExecCommandContext) in unit
// test file f.
func findTestProcessSpawns(fset *token.FileSet, f *ast.File) []string {
	banned := map[string][]string{
		"os/exec": {"Command", "CommandContext"},
		"os":      {"StartProcess"},
		"syscall": {"Exec", "ForkExec"},
	}
	pkgs := map[string]string{}
	var dotImports []string
	for _, imp := range f.Imports {
		ipath := strings.Trim(imp.Path.Value, `"`)
		if _, ok := banned[ipath]; !ok {
			continue
		}
		name := filepath.Base(ipath)
		if imp.Name != nil {
			name = imp.Name.Name
		}
		if name == "." {
			dotImports = append(dotImports, ipath)
			continue
		}
		pkgs[name] = ipath
	}
	isExecCmdType := func(e ast.Expr) bool {
		switch typ := e.(type) {
		case *ast.SelectorExpr:
			id, ok := typ.X.(*ast.Ident)
			return ok && pkgs[id.Name] == "os/exec" && typ.Sel.Name == "Cmd"
		case *ast.Ident:
			return typ.Name == "Cmd" && slices.Contains(dotImports, "os/exec")
		}
		return false
	}
	var found []string
	ast.Inspect(f, func(n ast.Node) bool {
		switch x := n.(type) {
		case *ast.SelectorExpr:
			if x.Sel.Name == "ExecCommand" || x.Sel.Name == "ExecCommandContext" {
				found = append(found, fset.Position(x.Pos()).String()+": "+x.Sel.Name)
				return true
			}
			id, ok := x.X.(*ast.Ident)
			if !ok {
				return true
			}
			if ipath, ok := pkgs[id.Name]; ok && slices.Contains(banned[ipath], x.Sel.Name) {
				found = append(found, fset.Position(x.Pos()).String()+": "+id.Name+"."+x.Sel.Name)
			}
		case *ast.CompositeLit:
			if isExecCmdType(x.Type) {
				found = append(found, fset.Position(x.Pos()).String()+": exec.Cmd literal")
			}
		case *ast.CallExpr:
			id, ok := x.Fun.(*ast.Ident)
			if !ok {
				return true
			}
			if id.Name == "ExecCommand" || id.Name == "ExecCommandContext" {
				found = append(found, fset.Position(x.Pos()).String()+": "+id.Name)
				return true
			}
			for _, ipath := range dotImports {
				if slices.Contains(banned[ipath], id.Name) {
					found = append(found, fset.Position(x.Pos()).String()+": dot-imported "+ipath+"."+id.Name)
				}
			}
		}
		return true
	})
	return found
}

// TestInvariants_UnitTestsUseGitFixtures verifies that unit test files never
// spawn subprocesses directly (via exec.Command / ExecCommandContext) outside
// brokenpipe_test.go and workspace_hook_test.go, and that every Test* function
// using RealGitRunner, NewGitClientInDir, NewExecGitRunner, or runTestGit
// guards execution via a LookPath("git") fixture.
func TestInvariants_UnitTestsUseGitFixtures(t *testing.T) {
	directSpawnAllowlist := []string{"brokenpipe_test.go", "workspace_hook_test.go"}
	for _, fc := range astcheck.MustLoadTestFiles(t, ".", "worktree") {
		base := filepath.Base(fc.Path)
		if base == "live_test.go" {
			continue
		}
		for _, v := range findTestGitViolations(fc.Fset, fc.AST, slices.Contains(directSpawnAllowlist, base)) {
			t.Errorf("%s", v)
		}
	}
	fset := token.NewFileSet()

	for _, tc := range []struct {
		name string
		src  string
	}{
		{"direct ExecCommandContext in test", `package p; import "testing"; func TestX(t *testing.T) { _ = ExecCommandContext(t.Context(), "git", "init") }`},
		{"unguarded RealGitRunner in test", `package p; import "testing"; func TestX(t *testing.T) { r := &RealGitRunner{}; _ = r }`},
	} {
		f, err := parser.ParseFile(fset, tc.name+".go", tc.src, 0)
		if err != nil {
			t.Fatalf("ParseFile(%s): %v", tc.name, err)
		}
		if got := findTestGitViolations(fset, f, false); len(got) == 0 {
			t.Errorf("%s: expected AST violation, got none", tc.name)
		}
	}
}

func findTestGitViolations(fset *token.FileSet, f *ast.File, allowDirectSpawn bool) []string {
	realGitSymbols := []string{"RealGitRunner", "NewGitClientInDir", "NewExecGitRunner", "runTestGit"}
	gitFixtureGuards := []string{"initTestRepo", "initTestRepoWithOrigin", "newTestGitRepo", "NewKindSmokeFixture", "setupPerfBudgetPool", "LookPath"}
	var violations []string
	if !allowDirectSpawn {
		violations = append(violations, findTestProcessSpawns(fset, f)...)
	}
	for _, decl := range f.Decls {
		fn, ok := decl.(*ast.FuncDecl)
		if !ok || fn.Body == nil || !strings.HasPrefix(fn.Name.Name, "Test") {
			continue
		}
		var realGitPos token.Pos
		var hasGuard bool
		ast.Inspect(fn.Body, func(n ast.Node) bool {
			id, ok := n.(*ast.Ident)
			if !ok {
				return true
			}
			if realGitPos == token.NoPos && slices.Contains(realGitSymbols, id.Name) {
				realGitPos = id.Pos()
			}
			if slices.Contains(gitFixtureGuards, id.Name) {
				hasGuard = true
			}
			return true
		})
		if realGitPos != token.NoPos && !hasGuard {
			violations = append(violations, fset.Position(realGitPos).String()+": "+fn.Name.Name+" uses real git without calling initTestRepo, newTestGitRepo, or exec.LookPath(\"git\")")
		}
	}
	return violations
}
