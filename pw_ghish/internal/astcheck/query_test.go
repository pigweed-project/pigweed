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
	"testing"

	"pigweed.dev/pw_ghish/internal/astcheck"
)

// TestFindCompositeLits verifies that FindCompositeLits matches struct literals
// by type name and captures both package-level variable bindings (FuncName == "")
// and function-local variable bindings (with constant string concatenation).
func TestFindCompositeLits(t *testing.T) {
	files := astcheck.MustLoadFiles(t, writeTempTree(t, map[string]string{
		"cmd.go": `package ghish
var rootCmd = &cobra.Command{Use: "gh-ish"}
func setup() {
	sub := &cobra.Command{Use: "pr " + "view"}
	_ = sub
}
`,
	}))
	lits := astcheck.FindCompositeLits(files, "cobra.Command")
	if len(lits) != 2 {
		t.Fatalf("expected 2 cobra.Command literals, got %d", len(lits))
	}
	if u, ok := lits[0].StringField("Use"); !ok || u != "gh-ish" || lits[0].VarName != "rootCmd" || lits[0].FuncName != "" {
		t.Errorf("unexpected package-level CompositeLitMatch: %+v use=%q", lits[0], u)
	}
	if u, ok := lits[1].StringField("Use"); !ok || u != "pr view" || lits[1].VarName != "sub" || lits[1].FuncName != "setup" {
		t.Errorf("unexpected function-local CompositeLitMatch: %+v use=%q", lits[1], u)
	}
}
