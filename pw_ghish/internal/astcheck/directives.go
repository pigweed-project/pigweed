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
	"go/token"
	"strconv"
	"strings"
)

// DirectiveComment represents a structured line comment directive (such as `//ghish:flag ...`).
type DirectiveComment struct {
	RawText, Payload string
	Line, TargetLine int
}

// ParseKV parses space-separated key=value or key="quoted value" pairs from d.Payload.
func (d DirectiveComment) ParseKV() (map[string]string, error) {
	s := strings.TrimSpace(d.Payload)
	if s == "" {
		return nil, fmt.Errorf("empty directive payload")
	}
	out := make(map[string]string)
	for ; s != ""; s = strings.TrimLeft(s, " \t") {
		key, rest, ok := strings.Cut(s, "=")
		if !ok || key == "" || rest == "" || strings.ContainsAny(key, " \t\"") {
			return nil, fmt.Errorf("malformed directive payload %q", d.Payload)
		}
		var val string
		if rest[0] == '"' {
			q, err := strconv.QuotedPrefix(rest)
			if err != nil {
				return nil, fmt.Errorf("invalid quoted value for %q in %q", key, d.Payload)
			}
			after := rest[len(q):]
			if after != "" && after[0] != ' ' && after[0] != '\t' {
				return nil, fmt.Errorf("invalid quoted value for %q in %q", key, d.Payload)
			}
			unq, err := strconv.Unquote(q)
			if err != nil || unq == "" {
				return nil, fmt.Errorf("invalid quoted value for %q in %q", key, d.Payload)
			}
			val, s = unq, after
		} else {
			end := strings.IndexAny(rest, " \t")
			if end < 0 {
				end = len(rest)
			}
			val, s = rest[:end], rest[end:]
			if val == "" || strings.Contains(val, "\"") {
				return nil, fmt.Errorf("invalid value for %q in %q", key, d.Payload)
			}
		}
		out[key] = val
	}
	return out, nil
}

// Directives extracts line comments in f starting with `//<prefix>` or `// <prefix>`.
func (f *File) Directives(prefix string) []DirectiveComment {
	if f == nil || f.AST == nil {
		panic("astcheck: File.Directives called on nil File or AST")
	}
	prefix = strings.TrimSpace(strings.TrimPrefix(strings.TrimSpace(prefix), "//"))
	if prefix == "" {
		panic("astcheck: File.Directives called with empty prefix")
	}
	var out []DirectiveComment
	for _, cg := range f.AST.Comments {
		groupTarget := f.Line(cg.End()) + 1
		for _, c := range cg.List {
			payload, ok := matchDirectiveLine(c.Text, prefix)
			if !ok {
				continue
			}
			line := f.Line(c.Pos())
			target := groupTarget
			if hasCodeBefore(f, c.Pos()) {
				target = line
			}
			out = append(out, DirectiveComment{
				RawText:    strings.TrimSpace(c.Text),
				Payload:    payload,
				Line:       line,
				TargetLine: target,
			})
		}
	}
	return out
}

func matchDirectiveLine(text, prefix string) (string, bool) {
	if !strings.HasPrefix(text, "//") {
		return "", false
	}
	s := strings.TrimLeft(text[2:], " \t")
	rest, ok := strings.CutPrefix(s, prefix)
	if !ok || (rest != "" && rest[0] != ' ' && rest[0] != '\t') {
		return "", false
	}
	return strings.TrimSpace(rest), true
}

func hasCodeBefore(f *File, pos token.Pos) bool {
	tf := f.Fset.File(pos)
	if tf == nil {
		panic(fmt.Sprintf("astcheck: token.Pos %v not in FileSet for %s", pos, f.Path))
	}
	for i := tf.Offset(pos) - 1; i >= 0 && f.Src[i] != '\n'; i-- {
		if f.Src[i] != ' ' && f.Src[i] != '\t' {
			return true
		}
	}
	return false
}
