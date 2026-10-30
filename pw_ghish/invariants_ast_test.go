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
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strconv"
	"strings"
	"testing"
)

// collectProductionGoFiles returns all non-test .go source files in pw_ghish/
// and pw_ghish/worktree/ (excluding cmd/main.go unless includeMain is true).
func collectProductionGoFiles(t *testing.T, includeMain bool) []string {
	t.Helper()
	var files []string
	for _, dir := range []string{".", "worktree", "cmd"} {
		entries, err := os.ReadDir(dir)
		if err != nil {
			if os.IsNotExist(err) {
				continue
			}
			t.Fatalf("os.ReadDir(%q): %v", dir, err)
		}
		for _, e := range entries {
			if e.IsDir() || !strings.HasSuffix(e.Name(), ".go") || strings.HasSuffix(e.Name(), "_test.go") {
				continue
			}
			rel := filepath.ToSlash(filepath.Join(dir, e.Name()))
			if !includeMain && rel == "cmd/main.go" {
				continue
			}
			files = append(files, rel)
		}
	}
	slices.Sort(files)
	return files
}

// TestInvariants_CobraCommandsUseRunE verifies that every cobra.Command composite
// literal in pw_ghish uses RunE / PreRunE / PersistentPreRunE / PostRunE and never
// the error-swallowing void Run / PreRun / PersistentPreRun / PostRun fields.
func TestInvariants_CobraCommandsUseRunE(t *testing.T) {
	fset := token.NewFileSet()
	prohibitedFields := []string{"Run", "PreRun", "PersistentPreRun", "PostRun"}

	for _, path := range collectProductionGoFiles(t, true) {
		f, err := parser.ParseFile(fset, path, nil, parser.ParseComments)
		if err != nil {
			t.Fatalf("ParseFile(%q): %v", path, err)
		}

		ast.Inspect(f, func(n ast.Node) bool {
			comp, ok := n.(*ast.CompositeLit)
			if !ok {
				return true
			}
			sel, ok := comp.Type.(*ast.SelectorExpr)
			if !ok || sel.Sel.Name != "Command" {
				return true
			}
			pkgIdent, ok := sel.X.(*ast.Ident)
			if !ok || pkgIdent.Name != "cobra" {
				return true
			}

			// Allow newRootAlias in root.go which copies fields from target.
			pos := fset.Position(comp.Pos())
			if filepath.Base(path) == "root.go" && isAliasCloneLiteral(comp) {
				return true
			}

			for _, elt := range comp.Elts {
				kv, ok := elt.(*ast.KeyValueExpr)
				if !ok {
					continue
				}
				keyIdent, ok := kv.Key.(*ast.Ident)
				if !ok {
					continue
				}
				if slices.Contains(prohibitedFields, keyIdent.Name) {
					kvPos := fset.Position(kv.Pos())
					t.Errorf("%s:%d: cobra.Command defines %s (at %s:%d); must use %sE returning error so failures are never silently ignored",
						pos.Filename, pos.Line, keyIdent.Name, kvPos.Filename, kvPos.Line, keyIdent.Name)
				}
			}
			return true
		})
	}
}

func isAliasCloneLiteral(comp *ast.CompositeLit) bool {
	for _, elt := range comp.Elts {
		kv, ok := elt.(*ast.KeyValueExpr)
		if !ok {
			continue
		}
		if keyIdent, ok := kv.Key.(*ast.Ident); ok && keyIdent.Name == "Run" {
			if sel, ok := kv.Value.(*ast.SelectorExpr); ok {
				if x, ok := sel.X.(*ast.Ident); ok && x.Name == "target" && sel.Sel.Name == "Run" {
					return true
				}
			}
		}
	}
	return false
}

// TestInvariants_NoDirectExitOrStdoutPrint verifies that no non-main production
// file calls os.Exit or writes directly to stdout via fmt.Print/Println/Printf.
func TestInvariants_NoDirectExitOrStdoutPrint(t *testing.T) {
	fset := token.NewFileSet()

	for _, path := range collectProductionGoFiles(t, false) {
		f, err := parser.ParseFile(fset, path, nil, 0)
		if err != nil {
			t.Fatalf("ParseFile(%q): %v", path, err)
		}

		ast.Inspect(f, func(n ast.Node) bool {
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

			pos := fset.Position(call.Pos())
			if pkgIdent.Name == "os" && sel.Sel.Name == "Exit" {
				t.Errorf("%s:%d: direct call to os.Exit is prohibited outside cmd/main.go; return an error (or ExitError) from RunE",
					pos.Filename, pos.Line)
			}
			if pkgIdent.Name == "fmt" && (sel.Sel.Name == "Print" || sel.Sel.Name == "Println" || sel.Sel.Name == "Printf") {
				t.Errorf("%s:%d: direct call to fmt.%s writes to os.Stdout and breaks hermetic test capture; use fmt.F%s(cmd.OutOrStdout(), ...)",
					pos.Filename, pos.Line, sel.Sel.Name, strings.ToLower(sel.Sel.Name[:1])+sel.Sel.Name[1:])
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

func checkFuncBodyForDirectReviewCalls(t *testing.T, fset *token.FileSet, fn *ast.FuncDecl, checkLegacyGoGerrit bool) {
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
		if checkLegacyGoGerrit && sel.Sel.Name == "SetReview" {
			if innerSel, ok := sel.X.(*ast.SelectorExpr); ok && innerSel.Sel.Name == "Changes" {
				pos := fset.Position(call.Pos())
				t.Errorf("%s:%d: direct call to %s.Changes.SetReview in %s; use ChangeContext.SetReview or SetReviewSafe",
					pos.Filename, pos.Line, innerSel.X, fn.Name.Name)
			}
		}
		if (sel.Sel.Name == "do" || sel.Sel.Name == "doRaw") && len(call.Args) >= 3 {
			if exprTargetsReviewEndpoint(call.Args[2], localExprs, make(map[string]bool)) {
				pos := fset.Position(call.Pos())
				t.Errorf("%s:%d: direct %s call to Gerrit /review endpoint in %s; use GerritClient.SetReview (via ChangeContext.SetReview) to preserve Drafts: \"KEEP\" and labels-unmarshal safety",
					pos.Filename, pos.Line, sel.Sel.Name, fn.Name.Name)
			}
		}
		return true
	})
}

// TestInvariants_SetReviewGoesThroughSetReviewSafe verifies that no production
// code invokes go-gerrit's `.Changes.SetReview(` outside `SetReviewSafe` in
// change_context.go or targets a Gerrit `/review` endpoint via `do` / `doRaw`
// outside `GerritClient.SetReview` in gerrit.go, preserving `Drafts: "KEEP"`
// and labels-unmarshal safety.
func TestInvariants_SetReviewGoesThroughSetReviewSafe(t *testing.T) {
	fset := token.NewFileSet()

	for _, path := range collectProductionGoFiles(t, true) {
		f, err := parser.ParseFile(fset, path, nil, 0)
		if err != nil {
			t.Fatalf("ParseFile(%q): %v", path, err)
		}

		for _, decl := range f.Decls {
			fn, ok := decl.(*ast.FuncDecl)
			if !ok || fn.Body == nil {
				continue
			}
			base := filepath.Base(path)
			if base == "change_context.go" && fn.Name.Name == "SetReviewSafe" {
				continue
			}
			if base == "gerrit.go" && fn.Name.Name == "SetReview" {
				continue
			}
			checkFuncBodyForDirectReviewCalls(t, fset, fn, true)
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
	fset := token.NewFileSet()

	for _, path := range collectProductionGoFiles(t, true) {
		base := filepath.Base(path)
		if base == "project_config.go" {
			continue
		}
		f, err := parser.ParseFile(fset, path, nil, 0)
		if err != nil {
			t.Fatalf("ParseFile(%q): %v", path, err)
		}

		ast.Inspect(f, func(n ast.Node) bool {
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
			if !strings.HasPrefix(key, "ghish.") {
				return true
			}
			if slices.Contains(legacyDirectGitConfigAllowlist[base], key) {
				return true
			}
			pos := fset.Position(call.Pos())
			t.Errorf("%s:%d: ad-hoc ConfigGet(%q) outside project_config.go; declare the setting on ProjectConfig in project_config.go and document it in project_setup.rst",
				pos.Filename, pos.Line, key)
			return true
		})
	}
}

// TestInvariants_BoundedHTTPBodyReads verifies that every HTTP request/response
// body read uses readBoundedBody, readBoundedLog, or io.LimitReader rather than
// an unbounded io.ReadAll(resp.Body).
func TestInvariants_BoundedHTTPBodyReads(t *testing.T) {
	fset := token.NewFileSet()
	for _, path := range collectProductionGoFiles(t, true) {
		f, err := parser.ParseFile(fset, path, nil, 0)
		if err != nil {
			t.Fatalf("ParseFile(%q): %v", path, err)
		}
		ast.Inspect(f, func(n ast.Node) bool {
			if call, ok := n.(*ast.CallExpr); ok && len(call.Args) == 1 {
				if sel, ok := call.Fun.(*ast.SelectorExpr); ok && sel.Sel.Name == "ReadAll" {
					if pkg, ok := sel.X.(*ast.Ident); ok && pkg.Name == "io" {
						if bodySel, ok := call.Args[0].(*ast.SelectorExpr); ok && bodySel.Sel.Name == "Body" {
							pos := fset.Position(call.Pos())
							t.Errorf("%s:%d: unbounded io.ReadAll(%s.Body); use readBoundedBody, readBoundedLog, or io.LimitReader per AGENTS.md invariant #10",
								pos.Filename, pos.Line, bodySel.X)
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
	for _, path := range collectProductionGoFiles(t, true) {
		if filepath.Base(path) == "ui.go" {
			continue
		}
		data, err := os.ReadFile(path)
		if err != nil {
			t.Fatalf("os.ReadFile(%q): %v", path, err)
		}
		src := string(data)
		if strings.Contains(src, `\x1b[`) || strings.Contains(src, `\033[`) {
			t.Errorf("%s: contains raw ANSI escape sequence outside ui.go; use Style* and PadRightStyled helpers in ui.go so NO_COLOR, non-TTY pipes, and coding agents receive pure plaintext (AGENTS.md invariant #15)", path)
		}
	}
}

// TestInvariants_SubprocessesUseCountingHelper verifies via AST inspection that
// no production file outside procexec.go creates processes without going
// through ExecCommand / ExecCommandContext (AGENTS.md invariant #16).
func TestInvariants_SubprocessesUseCountingHelper(t *testing.T) {
	allowlist := []string{"procexec.go"}
	fset := token.NewFileSet()
	for _, path := range collectProductionGoFiles(t, true) {
		if slices.Contains(allowlist, path) {
			continue
		}
		f, err := parser.ParseFile(fset, path, nil, 0)
		if err != nil {
			t.Fatalf("ParseFile(%q): %v", path, err)
		}
		for _, v := range findUncountedProcessSpawns(fset, f) {
			t.Errorf("%s bypasses the counting subprocess choke point; use ExecCommand / ExecCommandContext from procexec.go so perf budget tests can deterministically count commands constructed per CLI invocation (AGENTS.md invariant #16)", v)
		}
	}
}

// findUncountedProcessSpawns reports exec.Command / exec.CommandContext calls
// (including via a dot-import of os/exec), exec.Cmd composite literals, and
// os.StartProcess / syscall.Exec / syscall.ForkExec references in f.
func findUncountedProcessSpawns(fset *token.FileSet, f *ast.File) []string {
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
	if len(pkgs) == 0 && len(dotImports) == 0 {
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
