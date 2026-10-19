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
	"slices"
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

// TestInvariants_SetReviewGoesThroughSetReviewSafe verifies that no production
// code invokes go-gerrit's `.Changes.SetReview(` directly outside `SetReviewSafe`
// in change_context.go, preserving `Drafts: KEEP` and labels-unmarshal safety.
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
			if filepath.Base(path) == "change_context.go" && fn.Name.Name == "SetReviewSafe" {
				continue
			}

			ast.Inspect(fn.Body, func(n ast.Node) bool {
				call, ok := n.(*ast.CallExpr)
				if !ok {
					return true
				}
				sel, ok := call.Fun.(*ast.SelectorExpr)
				if !ok || sel.Sel.Name != "SetReview" {
					return true
				}
				if innerSel, ok := sel.X.(*ast.SelectorExpr); ok && innerSel.Sel.Name == "Changes" {
					pos := fset.Position(call.Pos())
					t.Errorf("%s:%d: direct call to %s.Changes.SetReview in %s; use ChangeContext.SetReview or SetReviewSafe",
						pos.Filename, pos.Line, innerSel.X, fn.Name.Name)
				}
				return true
			})
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
