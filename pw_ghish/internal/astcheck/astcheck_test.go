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

package astcheck_test

import (
	"os"
	"path/filepath"
	"testing"

	"pigweed.dev/pw_ghish/internal/astcheck"
)

func writeTempTree(t *testing.T, files map[string]string) string {
	t.Helper()
	root := t.TempDir()
	for rel, content := range files {
		full := filepath.Join(root, filepath.FromSlash(rel))
		if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(full, []byte(content), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return root
}

func TestMustLoadFilesAndDirectives(t *testing.T) {
	root := writeTempTree(t, map[string]string{
		"main.go":      "package main\nfunc main() {}\n",
		"main_test.go": "package main\nfunc TestM() {}\n",
		"flags.go": "package main\nfunc register() {\n" +
			"\t//ghish:flag mode=upstream gh=\"pr view --json\"\n" +
			"\t/* bridged\n\t   block comment */\n\tregisterJSON()\n" +
			"\tinline() //ghish:flag mode=extension reason=\"inline\"\n}\n",
	})
	files := astcheck.MustLoadFiles(t, root)
	if len(files) != 2 {
		t.Fatalf("expected 2 non-test files, got %d", len(files))
	}
	if testFiles := astcheck.MustLoadTestFiles(t, root); len(testFiles) != 1 || testFiles[0].Path != filepath.ToSlash(filepath.Join(root, "main_test.go")) {
		t.Fatalf("expected 1 test file (main_test.go), got %+v", testFiles)
	}
	var dirs []astcheck.DirectiveComment
	for _, f := range files {
		dirs = append(dirs, f.Directives("//ghish:flag")...)
	}
	if len(dirs) != 2 || dirs[0].Line != 3 || dirs[0].TargetLine != 6 || dirs[1].Line != 7 || dirs[1].TargetLine != 7 {
		t.Fatalf("unexpected directives: %+v", dirs)
	}
	if kv, err := dirs[0].ParseKV(); err != nil || kv["mode"] != "upstream" || kv["gh"] != "pr view --json" {
		t.Errorf("ParseKV = (%v, %v)", kv, err)
	}
	for _, bad := range []string{"", "bare", "k=", `k=""`, `k="unclosed`, `k="v"x`} {
		if _, err := (astcheck.DirectiveComment{Payload: bad}).ParseKV(); err == nil {
			t.Errorf("expected ParseKV(%q) error", bad)
		}
	}
}
