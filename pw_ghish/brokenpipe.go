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
	"errors"
	"io"
	"log/slog"
	"os"
	"os/signal"
	"sync/atomic"
	"syscall"
)

// PipeGuard wraps a process output stream that is a pipe or socket (`gh ... |
// head`) so that a reader closing its end early can never abort a command
// halfway.
//
// The first write that fails with EPIPE (or ECONNRESET, for a socket peer)
// marks the guard broken. From then on
// every write is reported as successful and discarded, so callers that check
// write errors (renderers, io.MultiWriter fan-outs, exec.Cmd copy goroutines
// feeding a git child) keep going instead of failing a mutation that already
// reached Gerrit, Buganizer, or local state. When StopEarly is set (read-only
// commands), the guard instead ends the process with ExitCodeBrokenPipe on the
// first EPIPE, the way `git log | head` stops.
//
// Calling exit from inside Write is a deliberate process-level side effect:
// in production it is os.Exit, so a read-only command never returns from the
// failing write. In tests exit is a recorder that returns, in which case
// Write still reports len(p), nil and the command simply continues.
type PipeGuard struct {
	w         io.Writer
	exit      func(int)
	broken    atomic.Bool
	stopEarly atomic.Bool
}

// NewPipeGuard returns a PipeGuard writing to w. exit is called with
// ExitCodeBrokenPipe on the first EPIPE when StopEarly is set; in production it
// is os.Exit, passed down from cmd/main.go.
func NewPipeGuard(w io.Writer, exit func(int)) *PipeGuard {
	return &PipeGuard{w: w, exit: exit}
}

// Write implements io.Writer. It never returns EPIPE: see PipeGuard.
func (g *PipeGuard) Write(p []byte) (int, error) {
	if g.broken.Load() {
		return len(p), nil
	}
	n, err := g.w.Write(p)
	if err == nil || !isReaderGone(err) {
		return n, err
	}
	if g.broken.CompareAndSwap(false, true) && g.stopEarly.Load() {
		g.exit(ExitCodeBrokenPipe)
	}
	return len(p), nil
}

// isReaderGone reports whether err means the reading end of the output has
// gone away: EPIPE for pipes, ECONNRESET for sockets whose peer reset.
func isReaderGone(err error) bool {
	return errors.Is(err, syscall.EPIPE) || errors.Is(err, syscall.ECONNRESET)
}

// Broken reports whether a write has hit a closed pipe.
func (g *PipeGuard) Broken() bool {
	return g.broken.Load()
}

// SetStopEarly controls whether the first EPIPE ends the process immediately
// (read-only commands) or is recorded and swallowed so the command runs to
// completion (mutations).
func (g *PipeGuard) SetStopEarly(stop bool) {
	g.stopEarly.Store(stop)
}

// isPipeOrSocket reports whether f (an open stdout/stderr file descriptor) is
// a pipe or socket, the only output kinds whose reader can go away mid-run.
// Terminals and regular files are left unwrapped so that child processes
// handed cmd.OutOrStdout() (git fetch, checkout, push) still inherit the real
// file descriptor and keep their TTY-dependent progress output.
func isPipeOrSocket(f *os.File) bool {
	fi, err := f.Stat()
	if err != nil {
		// OK: f is os.Stdout or os.Stderr inspected via fstat(2); if the parent
		// process closed fd 1 or fd 2 before exec (EBADF), it is not an open
		// pipe and leaving it unwrapped is safe.
		slog.Debug("failed to fstat output file descriptor; leaving unwrapped", "fd", f.Fd(), "err", err)
		return false
	}
	return fi.Mode()&(os.ModeNamedPipe|os.ModeSocket) != 0
}

// sameFile reports whether a and b refer to the same open file description or
// inode, such as stdout and stderr after `2>&1`.
func sameFile(a, b *os.File) bool {
	if a == b {
		return true
	}
	ai, err := a.Stat()
	if err != nil {
		// OK: if either stdout or stderr was closed before exec (EBADF), the
		// two streams do not share an open pipe.
		slog.Debug("failed to fstat stdout for same-file check; treating streams as distinct", "err", err)
		return false
	}
	bi, err := b.Stat()
	if err != nil {
		// OK: see above.
		slog.Debug("failed to fstat stderr for same-file check; treating streams as distinct", "err", err)
		return false
	}
	return os.SameFile(ai, bi)
}

// stopsEarlyOnBrokenPipe reports whether a command of the given kind may be
// cut short when its output pipe closes. Only read-only kinds qualify;
// mutations, meta commands (hooks), and commands without a kind always run to
// completion.
func stopsEarlyOnBrokenPipe(kind string) bool {
	switch kind {
	case KindView, KindList, KindStream:
		return true
	default:
		return false
	}
}

// Main runs the CLI with the given arguments and output streams and returns
// the process exit code.
//
// Errors carry their own exit code so that commands can honor established CLI
// contracts (notably `gh pr checks`, which exits 8 when checks are still
// pending). Anything without an explicit code exits 1. A command whose output
// pipe closed early exits ExitCodeBrokenPipe (141, as if killed by SIGPIPE),
// but only after it finished: see PipeGuard.
//
// exit is called only by a read-only command cutting itself short on a closed
// pipe; cmd/main.go passes os.Exit, keeping it the only caller of os.Exit.
func Main(args []string, stdout, stderr *os.File, exit func(int)) int {
	// Without a SIGPIPE handler, the Go runtime kills the process the moment a
	// write to fd 1 or 2 hits a closed pipe, possibly between the steps of a
	// mutation. With one, such writes fail with EPIPE instead. Notify (rather
	// than Ignore) keeps child processes on the default disposition, since
	// caught signals reset on exec while ignored ones are inherited. The
	// channel is never drained: delivery is non-blocking and only the EPIPE
	// return value matters.
	signal.Notify(make(chan os.Signal, 1), syscall.SIGPIPE)
	return runMain(args, stdout, stderr, exit)
}

// runMain is Main without process-global signal setup, for tests.
func runMain(args []string, stdout, stderr *os.File, exit func(int)) int {
	setupRootCmdHelp()
	setupRootAliases()
	rejectUnknownSubcommands(RootCmd)
	args = NormalizeCQArgs(args)

	stopEarly := false
	if target, _, err := RootCmd.Find(args); err == nil && target != nil {
		stopEarly = stopsEarlyOnBrokenPipe(CommandKind(target))
	}

	var guards []*PipeGuard
	wrap := func(f *os.File) io.Writer {
		if !isPipeOrSocket(f) {
			return f
		}
		g := NewPipeGuard(f, exit)
		g.SetStopEarly(stopEarly)
		guards = append(guards, g)
		return g
	}
	out := wrap(stdout)
	errOut := out
	// With `2>&1 | ...` (the usual agent setup) both streams are one pipe.
	// Share one guard so the broken flag is shared and exec.Cmd sees
	// Stdout == Stderr, giving a child one pipe and keeping its stdout and
	// stderr lines in order.
	if !sameFile(stdout, stderr) {
		errOut = wrap(stderr)
	}
	RootCmd.SetOut(out)
	RootCmd.SetErr(errOut)
	defer func() {
		RootCmd.SetOut(nil)
		RootCmd.SetErr(nil)
	}()
	RootCmd.SetArgs(args)
	err := RootCmd.Execute()
	return exitCodeAfterRun(err, guards)
}

// exitCodeAfterRun maps the result of a finished command onto an exit code.
// A real command error keeps its own code; a command that succeeded but lost
// its output to a closed pipe exits ExitCodeBrokenPipe so `set -o pipefail`
// scripts see the truncation, as they would for `git log | head`.
func exitCodeAfterRun(err error, guards []*PipeGuard) int {
	if err != nil {
		return ExitCodeFor(err)
	}
	for _, g := range guards {
		if g.Broken() {
			return ExitCodeBrokenPipe
		}
	}
	return ExitCodeOK
}
