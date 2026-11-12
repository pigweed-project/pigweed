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

package astcheck

import (
	"fmt"
	"go/ast"
	"go/token"
	"path/filepath"
	"slices"
	"strings"
	"testing"
)

type wrapRule struct {
	wrapperName        string
	declFile           string
	declLine           int
	declStart, declEnd token.Pos
	targetGlob         string
	hint               string
}

// AssertWraps validates all `//ghish:wraps` directives across files and reports
// any bypass outside the declared wrapper functions.
func AssertWraps(tb testing.TB, files []*File) {
	tb.Helper()
	var rules []wrapRule
	for _, f := range files {
		if f == nil || f.AST == nil {
			tb.Fatalf("astcheck.AssertWraps: nil File or AST")
		}
		rules = append(rules, parseFileWraps(tb, f)...)
	}
	for _, f := range files {
		checkFileAgainstWrappers(tb, f, rules)
	}
}

func parseFileWraps(tb testing.TB, f *File) []wrapRule {
	type funcInfo struct {
		name       string
		exported   bool
		line       int
		start, end token.Pos
	}
	declByLine := make(map[int]funcInfo)
	for _, decl := range f.AST.Decls {
		d, ok := decl.(*ast.FuncDecl)
		if !ok || d.Name == nil {
			continue
		}
		recv := ""
		if d.Recv != nil && len(d.Recv.List) > 0 {
			recv = baseTypeName(d.Recv.List[0].Type)
		}
		name := d.Name.Name
		exp := ast.IsExported(name) && (recv == "" || ast.IsExported(recv))
		if recv != "" {
			name = recv + "." + name
		}
		line := f.Line(d.Pos())
		declByLine[line] = funcInfo{
			name:     name,
			exported: exp,
			line:     line,
			start:    d.Pos(),
			end:      d.End(),
		}
	}

	var out []wrapRule
	for _, dir := range f.Directives("ghish:wraps") {
		fn, ok := declByLine[dir.TargetLine]
		if !ok {
			tb.Errorf("%s:%d: orphaned //ghish:wraps directive is not attached to a function declaration", f.Path, dir.Line)
			continue
		}
		if !fn.exported {
			tb.Errorf("%s:%d: //ghish:wraps must be attached to an exported function or method", f.Path, dir.Line)
			continue
		}
		specs, hint, errMsg := parseWrapsSpec(dir.Payload)
		if errMsg != "" {
			tb.Errorf("%s:%d: %s", f.Path, dir.Line, errMsg)
			continue
		}
		for _, sp := range specs {
			sp.wrapperName, sp.declFile, sp.declLine = fn.name, f.Path, fn.line
			sp.declStart, sp.declEnd, sp.hint = fn.start, fn.end, hint
			out = append(out, sp)
		}
	}
	return out
}

func parseWrapsSpec(payload string) (specs []wrapRule, hint, errMsg string) {
	before, after, hasDash := strings.Cut(payload, " -- ")
	if !hasDash || strings.TrimSpace(after) == "" {
		return nil, "", "expected '//ghish:wraps <target>[, <target>...] -- <guidance>'"
	}
	hint = strings.TrimSpace(after)
	for part := range strings.SplitSeq(before, ",") {
		s := strings.TrimSpace(part)
		if s == "" {
			continue
		}
		spec := strings.TrimPrefix(strings.TrimSuffix(s, "{}"), "&")
		if dot := strings.LastIndexByte(spec, '.'); dot <= 0 || dot == len(spec)-1 {
			return nil, "", "invalid target '" + s + "' in //ghish:wraps directive (expected pkg.Symbol or &pkg.Type{})"
		}
		specs = append(specs, wrapRule{targetGlob: s})
	}
	if len(specs) == 0 {
		return nil, "", "missing target in //ghish:wraps directive"
	}
	return specs, hint, ""
}

func checkFileAgainstWrappers(tb testing.TB, f *File, rules []wrapRule) {
	if len(rules) == 0 {
		return
	}
	identPkg := make(map[string]string, len(f.AST.Imports))
	dotImports := make(map[string]bool)
	for _, imp := range f.AST.Imports {
		ipath := unquoteString(imp.Path)
		local := filepath.Base(ipath)
		if imp.Name != nil {
			local = imp.Name.Name
		}
		if local == "." {
			dotImports[ipath] = true
		} else if local != "" && local != "_" {
			identPkg[local] = ipath
		}
	}

	matchesTarget := func(expr ast.Expr, spec string) bool {
		dot := strings.LastIndexByte(spec, '.')
		if dot < 0 {
			panic(fmt.Sprintf("astcheck: invalid wrap target %q (missing '.')", spec))
		}
		wantPkg, symGlob := spec[:dot], spec[dot+1:]
		switch x := ast.Unparen(expr).(type) {
		case *ast.SelectorExpr:
			id, ok := ast.Unparen(x.X).(*ast.Ident)
			if !ok || !matchWildcard(x.Sel.Name, symGlob) || isIdentShadowed(f.AST, id.Pos(), id.Name) {
				return false
			}
			bound, ok := identPkg[id.Name]
			if !ok {
				return false
			}
			if strings.Contains(wantPkg, "/") {
				return bound == wantPkg
			}
			return bound == wantPkg || filepath.Base(bound) == wantPkg
		case *ast.Ident:
			samePkg := f.AST.Name != nil && f.AST.Name.Name == filepath.Base(wantPkg)
			return (dotImports[wantPkg] || samePkg) && matchWildcard(x.Name, symGlob)
		}
		return false
	}

	matchesCall := func(call *ast.CallExpr, r wrapRule) bool {
		if strings.HasSuffix(r.targetGlob, "{}") {
			return false
		}
		return matchesTarget(unwrapIndexExpr(call.Fun), r.targetGlob)
	}

	report := func(r wrapRule, start, end token.Pos) {
		if f.Path == r.declFile && start >= r.declStart && end <= r.declEnd {
			return
		}
		snippet := strings.Join(strings.Fields(f.TextRange(start, end)), " ")
		tb.Errorf("%s:%d: %s bypasses wrapper %s (%s:%d): %s",
			f.Path, f.Line(start), snippet,
			r.wrapperName, filepath.Base(r.declFile), r.declLine, r.hint)
	}

	skipSel := make(map[*ast.SelectorExpr]bool)
	for n := range ast.Preorder(f.AST) {
		switch x := n.(type) {
		case *ast.CompositeLit:
			if sel := unwrapToSelector(x.Type); sel != nil {
				skipSel[sel] = true
			}
			for _, r := range rules {
				if spec, ok := strings.CutSuffix(r.targetGlob, "{}"); ok && matchesTarget(unwrapIndexExpr(x.Type), strings.TrimPrefix(spec, "&")) {
					report(r, x.Pos(), x.End())
				}
			}
		case *ast.CallExpr:
			if sel := unwrapToSelector(x.Fun); sel != nil {
				skipSel[sel] = true
			}
			for _, r := range rules {
				if matchesCall(x, r) {
					report(r, x.Pos(), x.End())
				}
			}
		case *ast.SelectorExpr:
			if skipSel[x] {
				continue
			}
			for _, r := range rules {
				if !strings.HasSuffix(r.targetGlob, "{}") && matchesTarget(x, r.targetGlob) {
					report(r, x.Pos(), x.End())
				}
			}
		}
	}
}

func isIdentShadowed(astFile *ast.File, pos token.Pos, ident string) bool {
	for n := range ast.Preorder(astFile) {
		if pos < n.Pos() || pos > n.End() {
			continue
		}
		switch x := n.(type) {
		case *ast.FuncDecl:
			if x.Body != nil && pos >= x.Body.Pos() && (fieldListDeclares(x.Recv, ident) || funcTypeDeclares(x.Type, ident)) {
				return true
			}
		case *ast.FuncLit:
			if x.Body != nil && pos >= x.Body.Pos() && funcTypeDeclares(x.Type, ident) {
				return true
			}
		case *ast.BlockStmt:
			if slices.ContainsFunc(x.List, func(st ast.Stmt) bool { return st.End() < pos && stmtDeclares(st, ident) }) {
				return true
			}
		}
	}
	return false
}

func funcTypeDeclares(ft *ast.FuncType, ident string) bool {
	return ft != nil && (fieldListDeclares(ft.Params, ident) || fieldListDeclares(ft.Results, ident))
}

func fieldListDeclares(fl *ast.FieldList, ident string) bool {
	return fl != nil && slices.ContainsFunc(fl.List, func(f *ast.Field) bool {
		return identListContains(f.Names, ident)
	})
}

func stmtDeclares(st ast.Stmt, ident string) bool {
	switch s := st.(type) {
	case *ast.AssignStmt:
		return s.Tok == token.DEFINE && slices.ContainsFunc(s.Lhs, func(e ast.Expr) bool {
			id, ok := e.(*ast.Ident)
			return ok && id.Name == ident
		})
	case *ast.DeclStmt:
		gd, ok := s.Decl.(*ast.GenDecl)
		return ok && slices.ContainsFunc(gd.Specs, func(sp ast.Spec) bool {
			vs, ok := sp.(*ast.ValueSpec)
			return ok && identListContains(vs.Names, ident)
		})
	default:
		return false
	}
}

func identListContains(names []*ast.Ident, ident string) bool {
	return slices.ContainsFunc(names, func(n *ast.Ident) bool { return n != nil && n.Name == ident })
}
