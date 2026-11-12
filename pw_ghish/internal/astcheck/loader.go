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

// Package astcheck provides Go AST loading, queries, and directive checks for pw_ghish tests.
package astcheck

import (
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// File represents a parsed Go source file.
type File struct {
	AST  *ast.File
	Path string
	Src  []byte
	Fset *token.FileSet
}

// Line returns the 1-based line number of pos.
func (f *File) Line(pos token.Pos) int {
	if f == nil || f.Fset == nil {
		panic("astcheck: File.Line called on nil File or Fset")
	}
	if !pos.IsValid() {
		panic(fmt.Sprintf("astcheck: File.Line called with invalid token.Pos in %s", f.Path))
	}
	return f.Fset.Position(pos).Line
}

// TextRange returns the source slice spanning [start, end).
func (f *File) TextRange(start, end token.Pos) string {
	if f == nil || f.Fset == nil {
		panic("astcheck: File.TextRange called on nil File or Fset")
	}
	if !start.IsValid() || !end.IsValid() {
		panic(fmt.Sprintf("astcheck: File.TextRange called with invalid pos (%v, %v) in %s", start, end, f.Path))
	}
	tf := f.Fset.File(start)
	if tf == nil || f.Fset.File(end) != tf {
		panic(fmt.Sprintf("astcheck: File.TextRange positions (%v, %v) do not belong to %s", start, end, f.Path))
	}
	s, e := tf.Offset(start), tf.Offset(end)
	if s < 0 || e < s || e > len(f.Src) {
		panic(fmt.Sprintf("astcheck: File.TextRange offsets [%d:%d] out of range for %s (len %d)", s, e, f.Path, len(f.Src)))
	}
	return string(f.Src[s:e])
}

// MustLoadFiles parses non-test Go source files in dirs and fails tb on error or 0 files.
func MustLoadFiles(tb testing.TB, dirs ...string) []*File {
	tb.Helper()
	return loadFiles(tb, false, dirs...)
}

// MustLoadTestFiles parses *_test.go source files in dirs and fails tb on error or 0 files.
func MustLoadTestFiles(tb testing.TB, dirs ...string) []*File {
	tb.Helper()
	return loadFiles(tb, true, dirs...)
}

func loadFiles(tb testing.TB, wantTest bool, dirs ...string) []*File {
	tb.Helper()
	fset := token.NewFileSet()
	var files []*File
	for _, dir := range dirs {
		entries, err := os.ReadDir(dir)
		if err != nil {
			tb.Fatalf("astcheck.loadFiles(%v): %v", dirs, err)
		}
		for _, ent := range entries {
			name := ent.Name()
			if ent.IsDir() || !strings.HasSuffix(name, ".go") ||
				strings.HasSuffix(name, "_test.go") != wantTest ||
				strings.HasPrefix(name, ".") || strings.HasPrefix(name, "_") {
				continue
			}
			rel := filepath.ToSlash(filepath.Join(dir, name))
			src, err := os.ReadFile(rel)
			if err != nil {
				tb.Fatalf("astcheck.loadFiles(%v): %v", dirs, err)
			}
			astFile, err := parser.ParseFile(fset, rel, src, parser.ParseComments|parser.SkipObjectResolution)
			if err != nil {
				tb.Fatalf("astcheck.loadFiles(%v): %v", dirs, err)
			}
			files = append(files, &File{
				AST:  astFile,
				Path: rel,
				Src:  src,
				Fset: fset,
			})
		}
	}
	if len(files) == 0 {
		tb.Fatalf("astcheck.loadFiles(%v) loaded 0 Go files", dirs)
	}
	return files
}
