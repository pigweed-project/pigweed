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
	"slices"
	"strconv"
	"strings"
)

// CompositeLitMatch represents a composite literal found in a Go source file.
type CompositeLitMatch struct {
	File              *File
	FuncName, VarName string
	Line              int
	lit               *ast.CompositeLit
}

// Field returns the value expression for fieldName on the matched literal.
func (m CompositeLitMatch) Field(fieldName string) (ast.Expr, bool) {
	if m.lit == nil {
		panic("astcheck: CompositeLitMatch.Field called on zero-value CompositeLitMatch")
	}
	if fieldName == "" {
		panic("astcheck: CompositeLitMatch.Field called with empty fieldName")
	}
	for _, elt := range m.lit.Elts {
		if kv, ok := elt.(*ast.KeyValueExpr); ok {
			if keyIdent, ok := kv.Key.(*ast.Ident); ok && keyIdent.Name == fieldName {
				return kv.Value, true
			}
		}
	}
	// Optional struct field omitted from literal.
	return nil, false
}

// StringField returns the constant string value of fieldName on the matched literal.
func (m CompositeLitMatch) StringField(fieldName string) (string, bool) {
	val, ok := m.Field(fieldName)
	if !ok {
		return "", false
	}
	return evalSimpleString(val)
}

// FindCompositeLits returns composite literals across files whose type matches
// any of typeNames (such as "cobra.Command" or "Command"), capturing the
// enclosing function name (FuncName) and assigned variable name (VarName).
//
// For example, given:
//
//	func newEditCmd() *cobra.Command {
//	  editCmd := &cobra.Command{Use: "edit [<id>]", RunE: runEdit}
//	  return editCmd
//	}
//
// FindCompositeLits(files, "cobra.Command") returns a CompositeLitMatch with
// FuncName="newEditCmd", VarName="editCmd", StringField("Use")=("edit [<id>]", true),
// and Field("RunE")=(ident(runEdit), true).
func FindCompositeLits(files []*File, typeNames ...string) []CompositeLitMatch {
	if len(typeNames) == 0 {
		panic("astcheck: FindCompositeLits called with no typeNames")
	}
	for _, tn := range typeNames {
		if strings.TrimSpace(tn) == "" {
			panic("astcheck: FindCompositeLits called with empty typeName")
		}
	}
	var out []CompositeLitMatch
	for _, f := range files {
		if f == nil || f.AST == nil {
			panic("astcheck: FindCompositeLits called with nil File or AST")
		}
		varByLit := make(map[*ast.CompositeLit]string)
		for _, decl := range f.AST.Decls {
			fnName := ""
			if fn, ok := decl.(*ast.FuncDecl); ok && fn.Name != nil {
				fnName = fn.Name.Name
			}
			for n := range ast.Preorder(decl) {
				switch x := n.(type) {
				case *ast.ValueSpec:
					for i, id := range x.Names {
						if id != nil && i < len(x.Values) {
							if comp := unwrapCompositeLit(x.Values[i]); comp != nil {
								varByLit[comp] = id.Name
							}
						}
					}
				case *ast.AssignStmt:
					for i, lhs := range x.Lhs {
						if id, ok := lhs.(*ast.Ident); ok && i < len(x.Rhs) {
							if comp := unwrapCompositeLit(x.Rhs[i]); comp != nil {
								varByLit[comp] = id.Name
							}
						}
					}
				case *ast.CompositeLit:
					if slices.ContainsFunc(typeNames, func(tn string) bool {
						return matchesCompositeType(x.Type, tn)
					}) {
						out = append(out, CompositeLitMatch{
							File:     f,
							FuncName: fnName,
							VarName:  varByLit[x],
							Line:     f.Line(x.Pos()),
							lit:      x,
						})
					}
				}
			}
		}
	}
	return out
}

func matchesCompositeType(typeExpr ast.Expr, typeSuffix string) bool {
	if typeExpr == nil {
		// Implicit-type nested literal (e.g. []T{{...}}) has no explicit AST type expression.
		return false
	}
	if pkg, selName, hasDot := strings.Cut(typeSuffix, "."); hasDot {
		u := unwrapIndexExpr(typeExpr)
		if star, ok := u.(*ast.StarExpr); ok {
			u = unwrapIndexExpr(star.X)
		}
		sel, ok := u.(*ast.SelectorExpr)
		if !ok || sel.Sel.Name != selName {
			return false
		}
		id, ok := ast.Unparen(sel.X).(*ast.Ident)
		return ok && id.Name == pkg
	}
	return baseTypeName(typeExpr) == typeSuffix
}

func unwrapCompositeLit(expr ast.Expr) *ast.CompositeLit {
	e := ast.Unparen(expr)
	if u, ok := e.(*ast.UnaryExpr); ok && u.Op == token.AND {
		e = ast.Unparen(u.X)
	}
	comp, _ := e.(*ast.CompositeLit)
	return comp
}

func unwrapIndexExpr(expr ast.Expr) ast.Expr {
	for expr != nil {
		switch x := ast.Unparen(expr).(type) {
		case *ast.IndexExpr:
			expr = x.X
		case *ast.IndexListExpr:
			expr = x.X
		default:
			return x
		}
	}
	return nil
}

func baseTypeName(expr ast.Expr) string {
	for expr != nil {
		switch x := unwrapIndexExpr(expr).(type) {
		case *ast.StarExpr:
			expr = x.X
		case *ast.Ident:
			return x.Name
		case *ast.SelectorExpr:
			return x.Sel.Name
		default:
			// Composite or anonymous type expression (e.g. struct{}, func(), []T).
			return ""
		}
	}
	return ""
}

func evalSimpleString(expr ast.Expr) (string, bool) {
	switch e := ast.Unparen(expr).(type) {
	case *ast.BasicLit:
		if e.Kind == token.STRING {
			return unquoteString(e), true
		}
	case *ast.BinaryExpr:
		if e.Op == token.ADD {
			l, okL := evalSimpleString(e.X)
			r, okR := evalSimpleString(e.Y)
			if okL && okR {
				return l + r, true
			}
		}
	}
	// Non-constant expression (variable, call, etc.) is not a string literal.
	return "", false
}

func unquoteString(lit *ast.BasicLit) string {
	if lit == nil || lit.Kind != token.STRING {
		panic(fmt.Sprintf("astcheck: unquoteString called on non-string literal %v", lit))
	}
	s, err := strconv.Unquote(lit.Value)
	if err != nil {
		panic(fmt.Sprintf("astcheck: strconv.Unquote(%q): %v", lit.Value, err))
	}
	return s
}
