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
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/spf13/cobra"
)

// countCalls returns how many recorded subprocesses contain substr.
func countCalls(calls []string, substr string) int {
	n := 0
	for _, c := range calls {
		if strings.Contains(c, substr) {
			n++
		}
	}
	return n
}

// commitWithMessage creates an empty commit in dir with message msg.
func commitWithMessage(t *testing.T, dir, msg string) {
	t.Helper()
	runTestGit(t, dir, "commit", "-q", "--allow-empty", "-m", msg)
}

// TestProbeRemoteBranch_MatchesVerifyRef checks that the one-call
// for-each-ref probe agrees with `git rev-parse --verify <remote>/<branch>`
// (VerifyRef) for every ref rev-parse would accept, and for the nested-ref
// near miss that for-each-ref's prefix matching would otherwise report.
func TestProbeRemoteBranch_MatchesVerifyRef(t *testing.T) {
	for _, tc := range []struct {
		name  string
		setup func(t *testing.T, dir string)
		want  bool
	}{
		{
			name:  "remote-tracking ref",
			setup: func(t *testing.T, dir string) { runTestGit(t, dir, "update-ref", "refs/remotes/origin/topic", "HEAD") },
			want:  true,
		},
		{
			name:  "local branch named origin/topic",
			setup: func(t *testing.T, dir string) { runTestGit(t, dir, "update-ref", "refs/heads/origin/topic", "HEAD") },
			want:  true,
		},
		{
			name:  "tag named origin/topic",
			setup: func(t *testing.T, dir string) { runTestGit(t, dir, "update-ref", "refs/tags/origin/topic", "HEAD") },
			want:  true,
		},
		{
			name: "remote HEAD symref below the name",
			setup: func(t *testing.T, dir string) {
				runTestGit(t, dir, "update-ref", "refs/remotes/origin/main", "HEAD")
				runTestGit(t, dir, "symbolic-ref", "refs/remotes/origin/topic/HEAD", "refs/remotes/origin/main")
			},
			want: true,
		},
		{
			name: "only a nested ref",
			setup: func(t *testing.T, dir string) {
				runTestGit(t, dir, "update-ref", "refs/remotes/origin/topic/sub", "HEAD")
			},
			want: false,
		},
		{
			name:  "absent",
			setup: func(t *testing.T, dir string) {},
			want:  false,
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			isolateGitEnv(t)
			dir := initTestRepo(t)
			runTestGit(t, dir, "remote", "add", "origin", "https://example.com/o")
			tc.setup(t, dir)

			verified, err := NewGitClientInDir(dir).VerifyRef(t.Context(), "origin/topic")
			if err != nil {
				t.Fatalf("VerifyRef(origin/topic) failed: %v", err)
			}
			if verified != tc.want {
				t.Fatalf("test setup: VerifyRef(origin/topic) = %v, want %v", verified, tc.want)
			}

			dg := NewGitClientInDir(dir).(*defaultGitClient)
			var got bool
			calls := recordGitCalls(t, func() { got, err = dg.probeRemoteBranch(t.Context(), "origin", "topic") })
			if err != nil {
				t.Fatalf("probeRemoteBranch() failed: %v", err)
			}
			if got != verified {
				t.Errorf("probeRemoteBranch(origin, topic) = %v, VerifyRef(origin/topic) = %v", got, verified)
			}
			if n := countCalls(calls, "for-each-ref"); n != 1 {
				t.Errorf("probeRemoteBranch() ran %d for-each-ref calls, want 1 (calls: %q)", n, calls)
			}
			if n := countCalls(calls, "rev-parse"); n != 0 {
				t.Errorf("probeRemoteBranch() ran %d rev-parse calls, want 0 (calls: %q)", n, calls)
			}
		})
	}
}

// TestResolvePushBranch_SingleRefProbe checks that resolvePushBranch answers
// "does <remote>/<current> exist?" and the default branch's ref probe with
// one for-each-ref between them, and still picks the same branch.
func TestResolvePushBranch_SingleRefProbe(t *testing.T) {
	for _, tc := range []struct {
		name  string
		setup func(t *testing.T, dir string)
		want  string
	}{
		{
			name: "current branch exists on the remote",
			setup: func(t *testing.T, dir string) {
				runTestGit(t, dir, "update-ref", "refs/remotes/origin/fresh", "HEAD")
			},
			want: "fresh",
		},
		{
			name: "falls back to the default branch",
			setup: func(t *testing.T, dir string) {
				runTestGit(t, dir, "update-ref", "refs/remotes/origin/main", "HEAD")
				runTestGit(t, dir, "branch", "-D", "main")
			},
			want: "main",
		},
		// inclusive-language: disable
		{
			name: "falls back to a legacy default branch",
			setup: func(t *testing.T, dir string) {
				runTestGit(t, dir, "update-ref", "refs/remotes/origin/master", "HEAD")
				runTestGit(t, dir, "branch", "-D", "main")
			},
			want: "master",
		},
		// inclusive-language: enable
	} {
		t.Run(tc.name, func(t *testing.T) {
			isolateGitEnv(t)
			dir := initTestRepo(t)
			runTestGit(t, dir, "remote", "add", "origin", "https://example.com/o")
			runTestGit(t, dir, "checkout", "-q", "-b", "fresh")
			tc.setup(t, dir)
			cfg := &Config{Git: &RealGitRunner{Dir: dir}, CWD: dir}

			var got string
			calls := recordGitCalls(t, func() { got = resolvePushBranch(t.Context(), cfg, "", io.Discard) })
			if got != tc.want {
				t.Errorf("resolvePushBranch() = %q, want %q (calls: %q)", got, tc.want, calls)
			}
			if n := countCalls(calls, "for-each-ref"); n != 1 {
				t.Errorf("resolvePushBranch() ran %d for-each-ref calls, want 1 (calls: %q)", n, calls)
			}
			if n := countCalls(calls, "rev-parse --verify"); n != 0 {
				t.Errorf("resolvePushBranch() ran %d rev-parse --verify probes, want 0 (calls: %q)", n, calls)
			}
		})
	}
}

// TestValidateCommitStack_VerifiesBaseOnce checks that the stack listing
// reuses the <remote>/<branch> ref the commit count verified, and that the
// per-commit validation still runs on that listing.
func TestValidateCommitStack_VerifiesBaseOnce(t *testing.T) {
	for _, tc := range []struct {
		name    string
		msgs    []string
		stack   bool
		wantErr string
	}{
		{
			name: "single commit",
			msgs: []string{"pw_a: One\n\nChange-Id: I1111111111111111111111111111111111111111"},
		},
		{
			name: "stack with a fixup commit",
			msgs: []string{
				"pw_a: One\n\nChange-Id: I1111111111111111111111111111111111111111",
				"fixup! pw_a: One",
			},
			stack:   true,
			wantErr: "unsquashed fixup/squash commit",
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			isolateGitEnv(t)
			dir := initTestRepo(t)
			runTestGit(t, dir, "remote", "add", "origin", "https://example.com/o")
			runTestGit(t, dir, "update-ref", "refs/remotes/origin/main", "HEAD")
			for _, msg := range tc.msgs {
				commitWithMessage(t, dir, msg)
			}
			git := NewGitClientInDir(dir)

			var err error
			calls := recordGitCalls(t, func() { err = ValidateCommitStack(t.Context(), git, "main", tc.stack, "push") })
			if tc.wantErr == "" && err != nil {
				t.Fatalf("ValidateCommitStack() failed: %v", err)
			}
			if tc.wantErr != "" && (err == nil || !strings.Contains(err.Error(), tc.wantErr)) {
				t.Fatalf("ValidateCommitStack() error = %v, want it to contain %q", err, tc.wantErr)
			}
			if n := countCalls(calls, "rev-parse --verify"); n != 1 {
				t.Errorf("ValidateCommitStack() ran %d rev-parse --verify calls, want 1 (calls: %q)", n, calls)
			}
			if n := countCalls(calls, "log --reverse"); n != 1 {
				t.Errorf("ValidateCommitStack() listed the stack %d times, want 1 (calls: %q)", n, calls)
			}
		})
	}
}

// TestVerifyHeadForPush_ReadsHeadMessageOnce checks that verifyHeadForPush
// reuses the HEAD message EnsureChangeID read when no amend happened, and
// re-reads it once after EnsureChangeID amends HEAD to insert a missing
// Change-Id (both with --no-verify and when a commit-msg hook runs).
func TestVerifyHeadForPush_ReadsHeadMessageOnce(t *testing.T) {
	const changeID = "I2222222222222222222222222222222222222222"
	const hookChangeID = "I3333333333333333333333333333333333333333"
	const hookMsg = "pw_a: Rewritten by hook\n\nChange-Id: " + hookChangeID
	// A commit-msg hook that replaces the whole message, so the message
	// verifyHeadForPush reports must come from git, not from what it sent.
	hookScript := "#!/bin/sh\nprintf '%s\\n' '" + strings.ReplaceAll(hookMsg, "\n", "' '") + "' > \"$1\"\n"
	for _, tc := range []struct {
		name      string
		msg       string
		noVerify  bool
		hook      bool
		wantReads int
		wantMsg   string
	}{
		{name: "Change-Id present", msg: "pw_a: Subject\n\nChange-Id: " + changeID, noVerify: true, wantReads: 1},
		{name: "Change-Id inserted by --no-verify amend", msg: "pw_a: Subject", noVerify: true, wantReads: 2},
		{name: "Change-Id inserted by commit-msg hook", msg: "pw_a: Subject", hook: true, wantReads: 2, wantMsg: hookMsg},
	} {
		t.Run(tc.name, func(t *testing.T) {
			isolateGitEnv(t)
			dir := initTestRepo(t)
			commitWithMessage(t, dir, tc.msg)
			if tc.hook {
				hooks := filepath.Join(dir, ".git", "hooks")
				if err := os.MkdirAll(hooks, 0o755); err != nil {
					t.Fatal(err)
				}
				if err := os.WriteFile(filepath.Join(hooks, "commit-msg"), []byte(hookScript), 0o755); err != nil {
					t.Fatal(err)
				}
			}
			cfg := &Config{Git: &RealGitRunner{Dir: dir}, CWD: dir}
			cmd := &cobra.Command{}
			cmd.SetOut(io.Discard)
			cmd.SetErr(io.Discard)

			var state *VerifiedPushState
			var err error
			calls := recordGitCalls(t, func() { state, err = verifyHeadForPush(t.Context(), cmd, cfg, false, tc.noVerify, nil) })
			if err != nil {
				t.Fatalf("verifyHeadForPush() failed: %v", err)
			}
			if n := countCalls(calls, "log -1 --format=%B"); n != tc.wantReads {
				t.Errorf("verifyHeadForPush() read HEAD's message %d times, want %d (calls: %q)", n, tc.wantReads, calls)
			}
			head := runTestGit(t, dir, "log", "-1", "--format=%B")
			if tc.wantMsg != "" && head != tc.wantMsg {
				t.Fatalf("test setup: HEAD's message = %q, want the hook's %q", head, tc.wantMsg)
			}
			if strings.TrimSpace(state.CommitMsg) != head {
				t.Errorf("state.CommitMsg = %q, want HEAD's message %q", state.CommitMsg, head)
			}
			if state.ChangeID == "" || ExtractChangeID(head) != state.ChangeID {
				t.Errorf("state.ChangeID = %q, want HEAD's Change-Id %q", state.ChangeID, ExtractChangeID(head))
			}
		})
	}
}
