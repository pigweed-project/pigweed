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

// This file is the single subprocess choke point for pw_ghish and
// pw_ghish/worktree. Every external process (git, gob-curl, sso_client,
// luci-auth, gcloud, browsers, Busytown accelerators) MUST be constructed via
// ExecCommand or ExecCommandContext so perf budget tests can deterministically
// count commands constructed (≈ subprocesses spawned) per CLI invocation
// (AGENTS.md invariant #16, enforced by
// TestInvariants_SubprocessesUseCountingHelper).

package pw_ghish

import (
	"context"
	"errors"
	"log/slog"
	"os/exec"
	"strings"
	"sync"
	"time"
)

var (
	subprocessRecorderMu sync.Mutex
	subprocessRecorder   func(call string)
)

// ExecCommand is a counting drop-in replacement for exec.Command.
func ExecCommand(name string, args ...string) *exec.Cmd {
	recordSubprocess(name, args)
	return exec.Command(name, args...)
}

// ExecCommandContext is a counting drop-in replacement for exec.CommandContext.
func ExecCommandContext(ctx context.Context, name string, args ...string) *exec.Cmd {
	recordSubprocess(name, args)
	return exec.CommandContext(ctx, name, args...)
}

// orphanedPipeWaitDelay bounds how long runBoundingOrphanedPipes waits for a
// child's output pipes to close after the child itself has exited. It is a
// variable so tests can shorten it.
var orphanedPipeWaitDelay = 5 * time.Second

// runBoundingOrphanedPipes runs cmd with cmd.WaitDelay set, for call sites
// that hand the child a caller-supplied writer (cmd.OutOrStdout(), which is a
// PipeGuard when stdout is a pipe) rather than an *os.File. Such writers are
// fed by exec.Cmd copy goroutines, and Wait blocks until every process holding
// the pipe's write end closes it -- including a backgrounded grandchild such as
// a git credential helper or an ssh ControlPersist process, which may live for
// minutes. exec.ErrWaitDelay (the child exited successfully but a descendant
// still held the pipe) is treated as success: the child's own output has
// already been copied.
func runBoundingOrphanedPipes(cmd *exec.Cmd) error {
	cmd.WaitDelay = orphanedPipeWaitDelay
	err := cmd.Run()
	if errors.Is(err, exec.ErrWaitDelay) {
		// OK: the child exited 0; only a detached descendant kept the output
		// pipe open, and its later output is not part of this command's result.
		slog.Debug("subprocess exited but a descendant kept its output pipe open; stopped waiting", "cmd", cmd.String(), "err", err)
		return nil
	}
	return err
}

// RecordSubprocessesForTesting installs fn to observe every command
// constructed (≈ subprocess spawned) via ExecCommand or ExecCommandContext,
// formatted as "name arg1 arg2...". fn may be called concurrently from
// multiple goroutines. It returns a function that restores the previous
// recorder.
func RecordSubprocessesForTesting(fn func(call string)) (restore func()) {
	subprocessRecorderMu.Lock()
	prev := subprocessRecorder
	subprocessRecorder = fn
	subprocessRecorderMu.Unlock()
	return func() {
		subprocessRecorderMu.Lock()
		subprocessRecorder = prev
		subprocessRecorderMu.Unlock()
	}
}

// recordSubprocess notifies the active test recorder (if any) that a command
// was constructed via ExecCommand or ExecCommandContext. In production runs no
// recorder is installed, so this is a no-op after checking subprocessRecorder.
func recordSubprocess(name string, args []string) {
	subprocessRecorderMu.Lock()
	fn := subprocessRecorder
	subprocessRecorderMu.Unlock()
	if fn == nil {
		// No test recorder installed (normal production execution).
		return
	}
	call := name
	if len(args) > 0 {
		call += " " + strings.Join(args, " ")
	}
	fn(call)
}
