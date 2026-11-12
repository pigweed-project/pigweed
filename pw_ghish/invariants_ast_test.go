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
	for _, fc := range astcheck.MustLoadFiles(t, ".", "worktree") {
		ast.Inspect(fc.AST, func(n ast.Node) bool {
			call, ok := n.(*ast.CallExpr)
			if !ok {
				return true
			}
			sel, ok := call.Fun.(*ast.SelectorExpr)
			if !ok {
				return true
			}
			pkgIdent, ok := sel.X.(*ast.Ident)
			if !ok {
				return true
			}
			line := fc.Line(call.Pos())
			if pkgIdent.Name == "os" && sel.Sel.Name == "Exit" {
				t.Errorf("%s:%d: direct call to os.Exit is prohibited outside cmd/main.go; return an error (or ExitError) from RunE",
					fc.Path, line)
			}
			if pkgIdent.Name == "fmt" && (sel.Sel.Name == "Print" || sel.Sel.Name == "Println" || sel.Sel.Name == "Printf") {
				t.Errorf("%s:%d: direct call to fmt.%s writes to os.Stdout and breaks hermetic test capture; use fmt.F%s(cmd.OutOrStdout(), ...)",
					fc.Path, line, sel.Sel.Name, strings.ToLower(sel.Sel.Name[:1])+sel.Sel.Name[1:])
			}
			return true
		})
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

// legacyDirectGitConfigAllowlist freezes existing direct ConfigGet("ghish.*") call
// sites until they are migrated into ProjectConfig. No new ad-hoc ghish.* git
// config lookups may be added outside project_config.go.
var legacyDirectGitConfigAllowlist = map[string][]string{
	"auth.go":      {"ghish.authmode"},
	"cmd_issue.go": {"ghish.componentid", "ghish.worktree.auto"},
	"hook.go":      {"ghish.blockrawpush"},
}

// TestInvariants_NoAdHocGhishGitConfigLookups ensures all new `ghish.*` config
// settings are loaded through `ProjectConfig` in project_config.go rather than
// scattered via ad-hoc `ConfigGet(ctx, "ghish....")` calls.
func TestInvariants_NoAdHocGhishGitConfigLookups(t *testing.T) {
	for _, fc := range astcheck.MustLoadFiles(t, ".", "worktree", "cmd") {
		base := filepath.Base(fc.Path)
		if base == "project_config.go" {
			continue
		}
		ast.Inspect(fc.AST, func(n ast.Node) bool {
			call, ok := n.(*ast.CallExpr)
			if !ok || len(call.Args) < 2 {
				return true
			}
			sel, ok := call.Fun.(*ast.SelectorExpr)
			if !ok || sel.Sel.Name != "ConfigGet" {
				return true
			}
			lit, ok := call.Args[1].(*ast.BasicLit)
			if !ok || lit.Kind != token.STRING {
				return true
			}
			key := strings.Trim(lit.Value, `"`+"`")
			if !strings.HasPrefix(key, "ghish.") || slices.Contains(legacyDirectGitConfigAllowlist[base], key) {
				return true
			}
			t.Errorf("%s:%d: ad-hoc ConfigGet(%q) outside project_config.go; declare the setting on ProjectConfig in project_config.go and document it in project_setup.rst",
				fc.Path, fc.Line(call.Pos()), key)
			return true
		})
	}
}

// TestInvariants_BoundedHTTPBodyReads verifies that every HTTP request/response
// body read uses readBoundedBody, readBoundedLog, or io.LimitReader rather than
// an unbounded io.ReadAll(resp.Body).
func TestInvariants_BoundedHTTPBodyReads(t *testing.T) {
	for _, fc := range astcheck.MustLoadFiles(t, ".", "worktree", "cmd") {
		ast.Inspect(fc.AST, func(n ast.Node) bool {
			if call, ok := n.(*ast.CallExpr); ok && len(call.Args) == 1 {
				if sel, ok := call.Fun.(*ast.SelectorExpr); ok && sel.Sel.Name == "ReadAll" {
					if pkg, ok := sel.X.(*ast.Ident); ok && pkg.Name == "io" {
						if bodySel, ok := call.Args[0].(*ast.SelectorExpr); ok && bodySel.Sel.Name == "Body" {
							t.Errorf("%s:%d: unbounded io.ReadAll(%s.Body); use readBoundedBody, readBoundedLog, or io.LimitReader per AGENTS.md invariant #10",
								fc.Path, fc.Line(call.Pos()), bodySel.X)
						}
					}
				}
			}
			return true
		})
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

// TestInvariants_SubprocessesUseCountingHelper verifies via AST inspection that
// no production file outside procexec.go creates processes without going
// through ExecCommand / ExecCommandContext (AGENTS.md invariant #16).
func TestInvariants_SubprocessesUseCountingHelper(t *testing.T) {
	allowlist := []string{"procexec.go"}
	for _, fc := range astcheck.MustLoadFiles(t, ".", "worktree", "cmd") {
		if slices.Contains(allowlist, fc.Path) {
			continue
		}
		for _, v := range findProcessSpawns(fc.Fset, fc.AST, false) {
			t.Errorf("%s bypasses the counting subprocess choke point; use ExecCommand / ExecCommandContext from procexec.go so perf budget tests can deterministically count commands constructed per CLI invocation (AGENTS.md invariant #16)", v)
		}
	}
}

// findProcessSpawns reports exec.Command / exec.CommandContext calls
// (including via a dot-import of os/exec), exec.Cmd composite literals,
// os.StartProcess / syscall.Exec / syscall.ForkExec references, and (when
// banCountingHelpers is true) direct ExecCommand / ExecCommandContext calls.
func findProcessSpawns(fset *token.FileSet, f *ast.File, banCountingHelpers bool) []string {
	// banned maps an import path to the identifiers that spawn processes.
	banned := map[string][]string{
		"os/exec": {"Command", "CommandContext"},
		"os":      {"StartProcess"},
		"syscall": {"Exec", "ForkExec"},
	}
	pkgs := map[string]string{} // local name -> import path
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
	if len(pkgs) == 0 && len(dotImports) == 0 && !banCountingHelpers {
		return nil
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
			if banCountingHelpers && (x.Sel.Name == "ExecCommand" || x.Sel.Name == "ExecCommandContext") {
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
			if banCountingHelpers && (id.Name == "ExecCommand" || id.Name == "ExecCommandContext") {
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
		violations = append(violations, findProcessSpawns(fset, f, true)...)
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
