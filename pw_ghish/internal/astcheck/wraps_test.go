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
	"fmt"
	"strings"
	"testing"

	"pigweed.dev/pw_ghish/internal/astcheck"
)

type recordingTB struct {
	testing.TB
	errs []string
}

func (r *recordingTB) Helper()                   {}
func (r *recordingTB) Errorf(f string, a ...any) { r.errs = append(r.errs, fmt.Sprintf(f, a...)) }
func (r *recordingTB) Fatalf(f string, a ...any) { r.errs = append(r.errs, fmt.Sprintf(f, a...)) }

func TestAssertWraps_SubprocessAliasesAndDotImports(t *testing.T) {
	root := writeTempTree(t, map[string]string{
		"procexec.go": `package sample
import "os/exec"

//ghish:wraps os/exec.Command, &os/exec.Cmd{}, os.StartProcess -- Use ExecCommand.
func ExecCommand(name string, args ...string) *exec.Cmd {
	return exec.Command(name, args...)
}

func shadowedInSameFileOK(exec struct{ Command func(string) }) {
	exec.Command("git")
}
`,
		"aliased.go": `package sample
import osexec "os/exec"

func badAliased() {
	_ = osexec.Command("git")
	_ = &osexec.Cmd{}
	fn := osexec.Command
	_ = fn
}

func nonImportedOK(exec struct{ Command func(string) }) {
	exec.Command("git")
}
`,
		"dotimport.go": `package sample
import . "os/exec"

func badDot() {
	_ = Command("git")
	_ = &Cmd{}
}
`,
	})
	rec := &recordingTB{TB: t}
	astcheck.AssertWraps(rec, astcheck.MustLoadFiles(t, root))
	if len(rec.errs) != 5 {
		t.Fatalf("expected 5 violations, got %d:\n%s", len(rec.errs), strings.Join(rec.errs, "\n"))
	}
}
