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

package pw_ghish

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"strings"
	"syscall"
	"testing"
	"time"

	"github.com/spf13/cobra"
)

// closedPipe returns the write end of a pipe whose read end is already
// closed, the state stdout is in after `gh ... | head -1` has read its line.
func closedPipe(t *testing.T) *os.File {
	t.Helper()
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatalf("os.Pipe: %v", err)
	}
	if err := r.Close(); err != nil {
		t.Fatalf("closing read end: %v", err)
	}
	t.Cleanup(func() { _ = w.Close() })
	return w
}

func TestPipeGuard_SwallowsEPIPEAndRecordsIt(t *testing.T) {
	exitCode := -1
	g := NewPipeGuard(closedPipe(t), func(code int) { exitCode = code })

	for i := range 3 {
		n, err := g.Write([]byte("line\n"))
		if err != nil || n != 5 {
			t.Fatalf("write %d = (%d, %v), want (5, nil)", i, n, err)
		}
	}
	if !g.Broken() {
		t.Error("Broken() = false after writing to a closed pipe, want true")
	}
	if exitCode != -1 {
		t.Errorf("exit called with %d; a guard without StopEarly must never exit", exitCode)
	}
}

func TestPipeGuard_StopEarlyExitsWithBrokenPipeCode(t *testing.T) {
	var codes []int
	g := NewPipeGuard(closedPipe(t), func(code int) { codes = append(codes, code) })
	g.SetStopEarly(true)

	_, _ = g.Write([]byte("first\n"))
	_, _ = g.Write([]byte("second\n"))
	if len(codes) != 1 || codes[0] != ExitCodeBrokenPipe {
		t.Errorf("exit calls = %v, want exactly [%d]", codes, ExitCodeBrokenPipe)
	}
}

type errWriter struct{ err error }

func (w errWriter) Write([]byte) (int, error) { return 0, w.err }

func TestPipeGuard_PassesThroughOtherErrors(t *testing.T) {
	wantErr := errors.New("disk full")
	g := NewPipeGuard(errWriter{err: wantErr}, func(int) { t.Error("exit called for a non-EPIPE error") })
	g.SetStopEarly(true)

	if _, err := g.Write([]byte("x")); !errors.Is(err, wantErr) {
		t.Errorf("Write error = %v, want %v", err, wantErr)
	}
	if g.Broken() {
		t.Error("Broken() = true after a non-EPIPE error, want false")
	}
}

func TestPipeGuard_HealthyPipeWritesThrough(t *testing.T) {
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatalf("os.Pipe: %v", err)
	}
	defer r.Close()
	g := NewPipeGuard(w, func(int) { t.Error("exit called on a healthy pipe") })
	if _, err := g.Write([]byte("hello")); err != nil {
		t.Fatalf("Write: %v", err)
	}
	_ = w.Close()
	buf := make([]byte, 16)
	n, _ := r.Read(buf)
	if got := string(buf[:n]); got != "hello" {
		t.Errorf("read %q, want %q", got, "hello")
	}
	if g.Broken() {
		t.Error("Broken() = true on a healthy pipe")
	}
}

func TestExitCodeAfterRun(t *testing.T) {
	broken := NewPipeGuard(nil, nil)
	broken.broken.Store(true)
	healthy := NewPipeGuard(nil, nil)
	cases := []struct {
		name   string
		err    error
		guards []*PipeGuard
		want   int
	}{
		{"success", nil, []*PipeGuard{healthy}, ExitCodeOK},
		{"success with closed output", nil, []*PipeGuard{healthy, broken}, ExitCodeBrokenPipe},
		{"failure keeps its own code", NewExitCodeError(ExitCodePending, "pending"), []*PipeGuard{broken}, ExitCodePending},
		{"plain failure", errors.New("boom"), nil, ExitCodeFailure},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := exitCodeAfterRun(tc.err, tc.guards); got != tc.want {
				t.Errorf("exitCodeAfterRun() = %d, want %d", got, tc.want)
			}
		})
	}
}

// runMainForTest drives RootCmd through Main's output wiring (without
// process-wide signal setup) with the same flag and Gerrit-client hygiene as
// executeCommand.
func runMainForTest(t *testing.T, stdout, stderr *os.File, args ...string) (code int, exits []int) {
	t.Helper()
	isolateRootCmdForTest(t)
	code = runMain(args, stdout, stderr, func(c int) { exits = append(exits, c) })
	return code, exits
}

// TestRunMain_MutationFinishesWhenStdoutIsClosed covers `gh pr create | head
// -1`: `pr create` prints "Creating change..." before it pushes, so the
// output pipe is already closed when the push runs. The push must still
// happen, and the exit code must report the truncated output rather than
// success.
func TestRunMain_MutationFinishesWhenStdoutIsClosed(t *testing.T) {
	mockGit := NewMockGit(t).WithBranch("feature-branch").
		OnCommand("push origin HEAD:refs/for/feature-branch", "remote:   https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/396894 pw_ghish: Add UI [NEW]\n")

	stdout := closedPipe(t)
	stderr := closedPipe(t)
	code, exits := runMainForTest(t, stdout, stderr, "pr", "create")

	if !mockGit.HasCall("push origin HEAD:refs/for/feature-branch") {
		t.Errorf("git calls = %v, want the push to have run", mockGit.Calls)
	}
	if code != ExitCodeBrokenPipe {
		t.Errorf("exit code = %d, want %d (mutation done, output truncated)", code, ExitCodeBrokenPipe)
	}
	if len(exits) != 0 {
		t.Errorf("mutation was cut short with exit(%v); it must run to completion", exits)
	}
}

func TestRunMain_HealthyPipeExitsZero(t *testing.T) {
	NewMockGit(t).WithBranch("feature-branch").
		OnCommand("push origin HEAD:refs/for/feature-branch", "remote:   https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/396894 pw_ghish: Add UI [NEW]\n")

	r, w, err := os.Pipe()
	if err != nil {
		t.Fatalf("os.Pipe: %v", err)
	}
	done := make(chan string)
	go func() {
		var sb strings.Builder
		buf := make([]byte, 4096)
		for {
			n, err := r.Read(buf)
			sb.Write(buf[:n])
			if err != nil {
				break
			}
		}
		done <- sb.String()
	}()
	code, _ := runMainForTest(t, w, w, "pr", "create")
	_ = w.Close()
	out := <-done
	_ = r.Close()

	if code != ExitCodeOK {
		t.Errorf("exit code = %d, want 0; output:\n%s", code, out)
	}
	if !strings.Contains(out, "Change created successfully.") {
		t.Errorf("output = %q, want the receipt", out)
	}
}

// TestPipeGuard_KeepsSubprocessTeeAlive covers the push pipeline shape: git
// push runs with Stdout set to io.MultiWriter(cmd.OutOrStdout(), &pushBuf)
// so the new change URL can be parsed from its output. With a raw closed pipe
// io.MultiWriter fails on the first write, exec.Cmd's copy goroutine fails,
// and Wait reports an error for a push that succeeded -- and pushBuf never
// sees the URL. Behind a PipeGuard both keep working.
func TestPipeGuard_KeepsSubprocessTeeAlive(t *testing.T) {
	sh, err := exec.LookPath("sh")
	if err != nil {
		t.Skip("sh not found")
	}
	const script = "echo remote: https://example-review.googlesource.com/c/x/+/1"
	run := func(out io.Writer) (string, error) {
		var buf bytes.Buffer
		cmd := exec.Command(sh, "-c", script)
		cmd.Stdout = io.MultiWriter(out, &buf)
		err := cmd.Run()
		return buf.String(), err
	}

	if _, err := run(closedPipe(t)); !errors.Is(err, syscall.EPIPE) {
		t.Errorf("raw closed pipe: err = %v, want EPIPE (documents the hazard)", err)
	}

	g := NewPipeGuard(closedPipe(t), func(int) { t.Error("exit called for a mutation guard") })
	got, err := run(g)
	if err != nil {
		t.Errorf("guarded closed pipe: err = %v, want nil", err)
	}
	if !strings.Contains(got, "/c/x/+/1") {
		t.Errorf("tee buffer = %q, want the change URL", got)
	}
	if !g.Broken() {
		t.Error("Broken() = false, want true")
	}
}

func TestPipeGuard_TreatsConnResetAsReaderGone(t *testing.T) {
	resetErr := &os.PathError{Op: "write", Path: "socket", Err: syscall.ECONNRESET}
	g := NewPipeGuard(errWriter{err: resetErr}, func(int) { t.Error("exit called for a mutation guard") })
	if n, err := g.Write([]byte("abc")); err != nil || n != 3 {
		t.Errorf("Write = (%d, %v), want (3, nil)", n, err)
	}
	if !g.Broken() {
		t.Error("Broken() = false after ECONNRESET, want true")
	}
}

// TestPipeGuard_SharedPipeKeepsChildOrdering covers `gh ... 2>&1 | ...`:
// with one guard as both Stdout and Stderr, exec.Cmd gives the child a single
// pipe, so interleaved stdout and stderr lines arrive in the order written.
func TestPipeGuard_SharedPipeKeepsChildOrdering(t *testing.T) {
	sh, err := exec.LookPath("sh")
	if err != nil {
		t.Skip("sh not found")
	}
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatalf("os.Pipe: %v", err)
	}
	defer r.Close()
	g := NewPipeGuard(w, func(int) { t.Error("exit called on a healthy pipe") })

	cmd := ExecCommand(sh, "-c", "echo o1; echo e1 >&2; echo o2; echo e2 >&2; echo o3")
	cmd.Stdout = g
	cmd.Stderr = g
	if err := runBoundingOrphanedPipes(cmd); err != nil {
		t.Fatalf("run: %v", err)
	}
	_ = w.Close()
	got, err := io.ReadAll(r)
	if err != nil {
		t.Fatalf("ReadAll: %v", err)
	}
	if want := "o1\ne1\no2\ne2\no3\n"; string(got) != want {
		t.Errorf("child output = %q, want %q", got, want)
	}
}

func TestRunMain_SharedPipeSharesOneGuard(t *testing.T) {
	var gotOut, gotErr any
	addTestCommand(t, &cobra.Command{
		Use:         "brokenpipe-test-streams",
		Hidden:      true,
		Annotations: map[string]string{KindAnnotation: KindMutation},
		RunE: func(cmd *cobra.Command, args []string) error {
			gotOut, gotErr = cmd.OutOrStdout(), cmd.ErrOrStderr()
			return nil
		},
	})
	newPipe := func() *os.File {
		r, w, err := os.Pipe()
		if err != nil {
			t.Fatalf("os.Pipe: %v", err)
		}
		t.Cleanup(func() { _ = r.Close(); _ = w.Close() })
		return w
	}

	shared := newPipe()
	runMainForTest(t, shared, shared, "brokenpipe-test-streams")
	if _, ok := gotOut.(*PipeGuard); !ok || gotOut != gotErr {
		t.Errorf("2>&1: stdout=%T %p, stderr=%T %p; want one shared *PipeGuard", gotOut, gotOut, gotErr, gotErr)
	}

	runMainForTest(t, newPipe(), newPipe(), "brokenpipe-test-streams")
	if gotOut == gotErr {
		t.Error("separate pipes share a guard; want one guard per pipe")
	}
}

// TestRunBoundingOrphanedPipes_DoesNotHangOnGrandchild covers a child that
// leaves a backgrounded grandchild (credential helper, ssh ControlPersist)
// holding its output pipe: Wait must return shortly after the child exits.
func TestRunBoundingOrphanedPipes_DoesNotHangOnGrandchild(t *testing.T) {
	sh, err := exec.LookPath("sh")
	if err != nil {
		t.Skip("sh not found")
	}
	orig := orphanedPipeWaitDelay
	orphanedPipeWaitDelay = 20 * time.Millisecond
	t.Cleanup(func() { orphanedPipeWaitDelay = orig })

	t.Run("successful child", func(t *testing.T) {
		var out bytes.Buffer
		cmd := ExecCommand(sh, "-c", "sleep 0.3 & echo hi")
		cmd.Stdout = &out
		cmd.Stderr = &out
		start := time.Now()
		err := runBoundingOrphanedPipes(cmd)
		if elapsed := time.Since(start); elapsed > 250*time.Millisecond {
			t.Errorf("run took %v; want it to stop waiting on the grandchild's pipe", elapsed)
		}
		if err != nil {
			t.Errorf("run err = %v, want nil (the child itself succeeded)", err)
		}
		if got := out.String(); got != "hi\n" {
			t.Errorf("output = %q, want %q", got, "hi\n")
		}
	})

	t.Run("failing child keeps its error", func(t *testing.T) {
		var out bytes.Buffer
		cmd := ExecCommand(sh, "-c", "sleep 0.3 & exit 3")
		cmd.Stdout = &out
		err := runBoundingOrphanedPipes(cmd)
		var exitErr *exec.ExitError
		if !errors.As(err, &exitErr) || exitErr.ExitCode() != 3 {
			t.Errorf("run err = %v, want exit status 3", err)
		}
	})
}

// addTestCommand registers a throwaway leaf command on RootCmd for the
// duration of the test.
func addTestCommand(t *testing.T, c *cobra.Command) {
	t.Helper()
	RootCmd.AddCommand(c)
	t.Cleanup(func() { RootCmd.RemoveCommand(c) })
}

func TestRunMain_ReadOnlyCommandStopsEarly(t *testing.T) {
	addTestCommand(t, &cobra.Command{
		Use:         "brokenpipe-test-list",
		Hidden:      true,
		Annotations: map[string]string{KindAnnotation: KindList},
		RunE: func(cmd *cobra.Command, args []string) error {
			fmt.Fprintln(cmd.OutOrStdout(), "row")
			return nil
		},
	})
	_, exits := runMainForTest(t, closedPipe(t), closedPipe(t), "brokenpipe-test-list")
	if len(exits) == 0 || exits[0] != ExitCodeBrokenPipe {
		t.Errorf("exit calls = %v, want a read-only command to stop with %d", exits, ExitCodeBrokenPipe)
	}
}
