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
	"context"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"
	"sync"
	"testing"

	"github.com/spf13/cobra"
)

func TestGitClient_CurrentBranch(t *testing.T) {
	ctx := context.Background()

	t.Run("success", func(t *testing.T) {
		mock := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				if len(args) == 2 && args[0] == "branch" && args[1] == "--show-current" {
					stdout.Write([]byte("my-feature\n"))
					return nil
				}
				return fmt.Errorf("unexpected: %v", args)
			},
		}
		client := NewGitClient(mock)
		branch, err := client.CurrentBranch(ctx)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if branch != "my-feature" {
			t.Errorf("CurrentBranch() = %q, want my-feature", branch)
		}
	})

	t.Run("failure", func(t *testing.T) {
		mock := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				stderr.Write([]byte("fatal: not a git repo\n"))
				return fmt.Errorf("exit status 128")
			},
		}
		client := NewGitClient(mock)
		_, err := client.CurrentBranch(ctx)
		if err == nil {
			t.Fatal("expected error, got nil")
		}
		if !strings.Contains(err.Error(), "not a git repo") {
			t.Errorf("expected error to contain stderr, got: %v", err)
		}
	})
}

func TestGitClient_CommitMessage(t *testing.T) {
	ctx := context.Background()

	t.Run("head commit message", func(t *testing.T) {
		mock := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				if len(args) == 3 && args[0] == "log" && args[1] == "-1" && args[2] == "--format=%B" {
					stdout.Write([]byte("Head message\n\nChange-Id: I12345\n"))
					return nil
				}
				return fmt.Errorf("unexpected: %v", args)
			},
		}
		client := NewGitClient(mock)
		msg, err := client.HeadCommitMessage(ctx)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if msg != "Head message\n\nChange-Id: I12345" {
			t.Errorf("unexpected message: %q", msg)
		}
	})

	t.Run("ref commit message", func(t *testing.T) {
		mock := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				if len(args) == 4 && args[0] == "log" && args[1] == "-1" && args[2] == "--format=%B" && args[3] == "refs/heads/feature" {
					stdout.Write([]byte("Feature message\n"))
					return nil
				}
				return fmt.Errorf("unexpected: %v", args)
			},
		}
		client := NewGitClient(mock)
		msg, err := client.CommitMessage(ctx, "refs/heads/feature")
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if msg != "Feature message" {
			t.Errorf("unexpected message: %q", msg)
		}
	})
}

func TestGitClient_ConfigGet(t *testing.T) {
	ctx := context.Background()

	mock := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) == 3 && args[0] == "config" && args[1] == "--get" && args[2] == "user.email" {
				stdout.Write([]byte("test@example.com\n"))
				return nil
			}
			return fmt.Errorf("key not found")
		},
	}
	client := NewGitClient(mock)
	val, err := client.ConfigGet(ctx, "user.email")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if val != "test@example.com" {
		t.Errorf("ConfigGet() = %q, want test@example.com", val)
	}

	_, err = client.ConfigGet(ctx, "nonexistent")
	if err == nil {
		t.Fatal("expected error for nonexistent key, got nil")
	}
}

func TestGitClient_RevParse_And_VerifyRef(t *testing.T) {
	ctx := context.Background()

	mock := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) >= 2 && args[0] == "rev-parse" {
				if args[1] == "--git-dir" {
					stdout.Write([]byte(".git\n"))
					return nil
				}
				if args[1] == "--verify" && args[2] == "refs/heads/main" {
					return nil
				}
				if args[1] == "--verify" && args[2] == "refs/heads/bad" {
					return fmt.Errorf("not found")
				}
				if args[1] == "--abbrev-ref" && args[2] == "@{upstream}" {
					stdout.Write([]byte("origin/main\n"))
					return nil
				}
			}
			return fmt.Errorf("unexpected: %v", args)
		},
	}
	client := NewGitClient(mock)

	gitDir, err := client.GitDir(ctx)
	if err != nil || gitDir != ".git" {
		t.Errorf("GitDir() = (%q, %v), want .git, nil", gitDir, err)
	}

	ok, err := client.VerifyRef(ctx, "refs/heads/main")
	if err != nil || !ok {
		t.Errorf("VerifyRef(main) = (%v, %v), want true, nil", ok, err)
	}
	ok, err = client.VerifyRef(ctx, "refs/heads/bad")
	if err != nil || ok {
		t.Errorf("VerifyRef(bad) = (%v, %v), want false, nil", ok, err)
	}

	upstream, err := client.RevParse(ctx, "--abbrev-ref", "@{upstream}")
	if err != nil || upstream != "origin/main" {
		t.Errorf("RevParse() = (%q, %v), want origin/main, nil", upstream, err)
	}
}

func TestGitClient_Fetch_Checkout_CherryPick_CommitAmend(t *testing.T) {
	ctx := context.Background()

	gitDir := t.TempDir()
	var calls []string
	mock := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			calls = append(calls, strings.Join(args, " "))
			if slices.Equal(args, []string{"rev-parse", "--git-dir"}) {
				stdout.Write([]byte(gitDir + "\n"))
			}
			return nil
		},
	}
	client := NewGitClient(mock)

	var stdout, stderr bytes.Buffer
	if err := client.Fetch(ctx, "origin", "refs/changes/12/1234/1", &stdout, &stderr); err != nil {
		t.Fatalf("Fetch failed: %v", err)
	}
	if err := client.Checkout(ctx, "FETCH_HEAD", &stdout, &stderr); err != nil {
		t.Fatalf("Checkout failed: %v", err)
	}
	if err := client.CherryPick(ctx, "FETCH_HEAD", &stdout, &stderr); err != nil {
		t.Fatalf("CherryPick failed: %v", err)
	}
	if err := client.CommitAmendNoEdit(ctx, &stdout, &stderr); err != nil {
		t.Fatalf("CommitAmendNoEdit failed: %v", err)
	}

	expectedCalls := []string{
		"fetch origin refs/changes/12/1234/1",
		"checkout FETCH_HEAD",
		"cherry-pick FETCH_HEAD",
		"rev-parse --git-dir",
		"rev-list --parents -n 1 HEAD",
		"for-each-ref --points-at=HEAD --format=%(refname) refs/remotes/",
		"commit --amend --only --allow-empty --no-edit",
	}
	if len(calls) != len(expectedCalls) {
		t.Fatalf("Expected %d calls, got %d: %v", len(expectedCalls), len(calls), calls)
	}
	for i, exp := range expectedCalls {
		if calls[i] != exp {
			t.Errorf("call[%d] = %q, want %q", i, calls[i], exp)
		}
	}
}

func TestConfig_GitClient_Fallback(t *testing.T) {
	var cfg *Config
	client := cfg.GitClient()
	if client == nil {
		t.Fatal("expected non-nil GitClient from nil Config")
	}

	cfg2 := &Config{Git: nil}
	client2 := cfg2.GitClient()
	if client2 == nil {
		t.Fatal("expected non-nil GitClient from Config with nil Git")
	}

	mock := &MockGitRunner{}
	cfg3 := &Config{Git: mock}
	client3 := cfg3.GitClient()
	if client3 == nil {
		t.Fatal("expected non-nil GitClient from Config with mock Git")
	}
}

func TestGitClient_VerifyRef_Robustness(t *testing.T) {
	ctx := context.Background()

	t.Run("empty ref returns error", func(t *testing.T) {
		client := NewGitClient(&MockGitRunner{})
		_, err := client.VerifyRef(ctx, "")
		if err == nil {
			t.Fatal("expected error for empty ref, got nil")
		}
		_, err = client.VerifyRef(ctx, "   ")
		if err == nil {
			t.Fatal("expected error for whitespace ref, got nil")
		}
	})

	t.Run("canceled context returns error", func(t *testing.T) {
		cancCtx, cancel := context.WithCancel(ctx)
		cancel()
		client := NewGitClient(&MockGitRunner{})
		_, err := client.VerifyRef(cancCtx, "refs/heads/main")
		if err == nil {
			t.Fatal("expected error for canceled context, got nil")
		}
	})

	t.Run("not a git repository returns error", func(t *testing.T) {
		mock := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				stderr.Write([]byte("fatal: not a git repository (or any of the parent directories)\n"))
				return fmt.Errorf("exit status 128")
			},
		}
		client := NewGitClient(mock)
		_, err := client.VerifyRef(ctx, "refs/heads/main")
		if err == nil {
			t.Fatal("expected error when not a git repository, got nil")
		}
		if !strings.Contains(err.Error(), "not a git repository") {
			t.Errorf("expected error to mention 'not a git repository', got: %v", err)
		}
	})

	t.Run("ref not found returns false without error", func(t *testing.T) {
		mock := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				stderr.Write([]byte("fatal: Needed a single revision\n"))
				return fmt.Errorf("exit status 128")
			},
		}
		client := NewGitClient(mock)
		ok, err := client.VerifyRef(ctx, "refs/heads/nonexistent")
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if ok {
			t.Fatal("expected ok=false for nonexistent ref, got true")
		}
	})
}

func TestGitClient_ValidationErrors(t *testing.T) {
	ctx := context.Background()
	client := NewGitClient(&MockGitRunner{})

	t.Run("CommitMessage validation", func(t *testing.T) {
		// Empty ref must return error, never silently fall back to HEAD
		_, err := client.CommitMessage(ctx, "")
		if err == nil {
			t.Fatal("expected error for CommitMessage with empty ref, got nil")
		}
		_, err = client.CommitMessage(ctx, "   ")
		if err == nil {
			t.Fatal("expected error for CommitMessage with whitespace ref, got nil")
		}
		// More than one ref must return error
		_, err = client.CommitMessage(ctx, "ref1", "ref2")
		if err == nil {
			t.Fatal("expected error for CommitMessage with >1 refs, got nil")
		}
	})

	t.Run("ConfigGet validation", func(t *testing.T) {
		_, err := client.ConfigGet(ctx, "")
		if err == nil {
			t.Fatal("expected error for ConfigGet with empty key, got nil")
		}
		_, err = client.ConfigGet(ctx, "   ")
		if err == nil {
			t.Fatal("expected error for ConfigGet with whitespace key, got nil")
		}
	})

	t.Run("RevParse validation", func(t *testing.T) {
		_, err := client.RevParse(ctx)
		if err == nil {
			t.Fatal("expected error for RevParse with no args, got nil")
		}
		_, err = client.RevParse(ctx, "--verify", "")
		if err == nil {
			t.Fatal("expected error for RevParse with empty arg element, got nil")
		}
		_, err = client.RevParse(ctx, "   ")
		if err == nil {
			t.Fatal("expected error for RevParse with whitespace arg, got nil")
		}
	})

	t.Run("CountCommitsAhead validation", func(t *testing.T) {
		_, err := client.CountCommitsAhead(ctx, "")
		if err == nil {
			t.Fatal("expected error for CountCommitsAhead with empty branch, got nil")
		}
		_, err = client.CountCommitsAhead(ctx, "   ")
		if err == nil {
			t.Fatal("expected error for CountCommitsAhead with whitespace branch, got nil")
		}
	})

	t.Run("Fetch Checkout CherryPick validation", func(t *testing.T) {
		if err := client.Fetch(ctx, "", "ref", io.Discard, io.Discard); err == nil {
			t.Fatal("expected error for Fetch with empty remote")
		}
		if err := client.Fetch(ctx, "origin", "", io.Discard, io.Discard); err == nil {
			t.Fatal("expected error for Fetch with empty ref")
		}
		if err := client.Checkout(ctx, "", io.Discard, io.Discard); err == nil {
			t.Fatal("expected error for Checkout with empty ref")
		}
		if err := client.CherryPick(ctx, "", io.Discard, io.Discard); err == nil {
			t.Fatal("expected error for CherryPick with empty ref")
		}
	})
}

func TestGitClient_Run_Robustness(t *testing.T) {
	mock := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			stdout.Write([]byte("ok\n"))
			stderr.Write([]byte("warn\n"))
			return nil
		},
	}
	client := NewGitClient(mock)

	t.Run("nil stdout and stderr do not panic", func(t *testing.T) {
		err := client.Run(context.Background(), nil, nil, "status")
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
	})
}

func TestGitClient_runOutput_Diagnostics(t *testing.T) {
	ctx := context.Background()

	t.Run("empty args returns error", func(t *testing.T) {
		defClient, ok := NewGitClient(&MockGitRunner{}).(*defaultGitClient)
		if !ok {
			t.Fatal("failed to cast to *defaultGitClient")
		}
		_, err := defClient.runOutput(ctx)
		if err == nil {
			t.Fatal("expected error for empty args in runOutput")
		}
	})

	t.Run("error incorporates stderr", func(t *testing.T) {
		mock := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				stderr.Write([]byte("fatal: repository not found\n"))
				return fmt.Errorf("exit status 128")
			},
		}
		client := NewGitClient(mock)
		_, err := client.CurrentBranch(ctx)
		if err == nil {
			t.Fatal("expected error, got nil")
		}
		if !strings.Contains(err.Error(), "fatal: repository not found") {
			t.Errorf("expected error to contain stderr, got: %v", err)
		}
	})

	t.Run("error falls back to stdout if stderr is empty", func(t *testing.T) {
		mock := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				stdout.Write([]byte("error diagnostic on stdout\n"))
				return fmt.Errorf("exit status 1")
			},
		}
		client := NewGitClient(mock)
		_, err := client.CurrentBranch(ctx)
		if err == nil {
			t.Fatal("expected error, got nil")
		}
		if !strings.Contains(err.Error(), "error diagnostic on stdout") {
			t.Errorf("expected error to fall back to stdout diagnostic, got: %v", err)
		}
	})
}

func TestGitClient_IsHeadRemoteTip(t *testing.T) {
	ctx := context.Background()

	t.Run("head points to remote tip", func(t *testing.T) {
		mock := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				if len(args) >= 3 && args[0] == "for-each-ref" && args[1] == "--points-at=HEAD" {
					stdout.Write([]byte("refs/remotes/origin/main\nrefs/remotes/origin/HEAD\n"))
					return nil
				}
				return fmt.Errorf("unexpected: %v", args)
			},
		}
		client := NewGitClient(mock)
		isTip, remotes, err := client.IsHeadRemoteTip(ctx)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if !isTip {
			t.Errorf("expected isTip to be true")
		}
		if len(remotes) != 2 || remotes[0] != "refs/remotes/origin/main" || remotes[1] != "refs/remotes/origin/HEAD" {
			t.Errorf("unexpected remotes: %v", remotes)
		}
	})

	t.Run("head is ahead of remote tip", func(t *testing.T) {
		mock := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				if len(args) >= 3 && args[0] == "for-each-ref" && args[1] == "--points-at=HEAD" {
					return nil
				}
				return fmt.Errorf("unexpected: %v", args)
			},
		}
		client := NewGitClient(mock)
		isTip, remotes, err := client.IsHeadRemoteTip(ctx)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if isTip {
			t.Errorf("expected isTip to be false")
		}
		if len(remotes) != 0 {
			t.Errorf("expected empty remotes, got: %v", remotes)
		}
	})

	t.Run("command failure reports error", func(t *testing.T) {
		mock := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				stderr.Write([]byte("fatal: git error\n"))
				return fmt.Errorf("exit status 128")
			},
		}
		client := NewGitClient(mock)
		_, _, err := client.IsHeadRemoteTip(ctx)
		if err == nil {
			t.Fatal("expected error, got nil")
		}
		if !strings.Contains(err.Error(), "failed to check remote tracking branches") {
			t.Errorf("unexpected error: %v", err)
		}
	})
}

func TestGitClient_HeadAuthorEmail_And_UserEmail(t *testing.T) {
	ctx := context.Background()

	mock := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) >= 4 && args[0] == "log" && args[1] == "-1" && args[2] == "--format=%ae" {
				stdout.Write([]byte("author@example.com\n"))
				return nil
			}
			if len(args) >= 3 && args[0] == "config" && args[1] == "--get" && args[2] == "user.email" {
				stdout.Write([]byte("user@example.com\n"))
				return nil
			}
			return fmt.Errorf("unexpected: %v", args)
		},
	}
	client := NewGitClient(mock)

	author, err := client.HeadAuthorEmail(ctx)
	if err != nil {
		t.Fatalf("unexpected HeadAuthorEmail error: %v", err)
	}
	if author != "author@example.com" {
		t.Errorf("HeadAuthorEmail got %q, want author@example.com", author)
	}

	user, err := client.UserEmail(ctx)
	if err != nil {
		t.Fatalf("unexpected UserEmail error: %v", err)
	}
	if user != "user@example.com" {
		t.Errorf("UserEmail got %q, want user@example.com", user)
	}
}

func TestGitClient_CheckAmendAllowed(t *testing.T) {
	ctx := context.Background()

	t.Run("blocks amend on remote tip", func(t *testing.T) {
		gitDir := t.TempDir()
		mock := (&MockGitRunner{}).
			OnCommand("rev-parse --git-dir", gitDir+"\n").
			OnCommand("rev-list --parents -n 1 HEAD", "deadbeef parent1\n").
			OnCommand("for-each-ref --points-at=HEAD --format=%(refname) refs/remotes/", "refs/remotes/origin/main\nrefs/remotes/goog/main\n")
		err := NewGitClient(mock).CheckAmendAllowed(ctx)
		if err == nil {
			t.Fatal("expected error, got nil")
		}
		if !strings.Contains(err.Error(), "cannot amend HEAD commit") ||
			!strings.Contains(err.Error(), "refs/remotes/origin/main") ||
			!strings.Contains(err.Error(), "refs/remotes/goog/main") {
			t.Errorf("expected error to explain remote tip block across remotes, got: %v", err)
		}
	})

	t.Run("blocks amend on active sequencer or conflict marker", func(t *testing.T) {
		for _, m := range []struct {
			name    string
			isDir   bool
			wantFix string
		}{
			{name: "rebase-merge", isDir: true, wantFix: "git rebase --continue"},
			{name: "rebase-apply", isDir: true, wantFix: "git rebase --continue"},
			{name: "MERGE_HEAD", wantFix: "git merge --continue"},
			{name: "CHERRY_PICK_HEAD", wantFix: "git cherry-pick --continue"},
			{name: "REVERT_HEAD", wantFix: "git revert --continue"},
			{name: "BISECT_LOG", wantFix: "git bisect reset"},
		} {
			t.Run(m.name, func(t *testing.T) {
				gitDir := t.TempDir()
				target := filepath.Join(gitDir, m.name)
				var err error
				if m.isDir {
					err = os.MkdirAll(target, 0o755)
				} else {
					err = os.WriteFile(target, []byte("0000000\n"), 0o600)
				}
				if err != nil {
					t.Fatal(err)
				}
				mock := (&MockGitRunner{}).OnCommand("rev-parse --git-dir", gitDir+"\n")
				err = NewGitClient(mock).CheckAmendAllowed(ctx)
				if err == nil || !strings.Contains(err.Error(), m.name) || !strings.Contains(err.Error(), m.wantFix) {
					t.Fatalf("expected error mentioning %s and %q, got: %v", m.name, m.wantFix, err)
				}
				if !slices.Equal(mock.Calls, []string{"rev-parse --git-dir"}) {
					t.Fatalf("unexpected git calls after sequencer marker found: %v", mock.Calls)
				}
			})
		}
	})

	t.Run("blocks amend on merge commit", func(t *testing.T) {
		gitDir := t.TempDir()
		mock := (&MockGitRunner{}).
			OnCommand("rev-parse --git-dir", gitDir+"\n").
			OnCommand("rev-list --parents -n 1 HEAD", "deadbeef parent1 parent2\n").
			OnCommand("remote", "goog\n")
		err := NewGitClient(mock).CheckAmendAllowed(ctx)
		if err == nil || !strings.Contains(err.Error(), "merge commit with 2 parents") || !strings.Contains(err.Error(), "git rebase goog/main") {
			t.Fatalf("expected merge parent error with resolved remote remediation, got: %v", err)
		}
	})

	t.Run("propagates git subcommand errors", func(t *testing.T) {
		for _, failCmd := range []string{
			"rev-parse --git-dir",
			"rev-list --parents -n 1 HEAD",
			"for-each-ref --points-at=HEAD --format=%(refname) refs/remotes/",
		} {
			t.Run(failCmd, func(t *testing.T) {
				gitDir := t.TempDir()
				mock := (&MockGitRunner{}).
					OnCommand("rev-parse --git-dir", gitDir+"\n").
					OnCommand("rev-list --parents -n 1 HEAD", "deadbeef parent1\n").
					OnError(failCmd, fmt.Errorf("injected error on %s", failCmd))
				if err := NewGitClient(mock).CheckAmendAllowed(ctx); err == nil || !strings.Contains(err.Error(), "injected error") {
					t.Fatalf("expected propagated error from %q, got: %v", failCmd, err)
				}
			})
		}
	})

	t.Run("allows amend when not on remote tip", func(t *testing.T) {
		gitDir := t.TempDir()
		mock := (&MockGitRunner{}).
			OnCommand("rev-parse --git-dir", gitDir+"\n").
			OnCommand("rev-list --parents -n 1 HEAD", "deadbeef parent1\n").
			OnCommand("for-each-ref --points-at=HEAD --format=%(refname) refs/remotes/", "")
		if err := NewGitClient(mock).CheckAmendAllowed(ctx); err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
	})
}

func TestGitClient_CommitAmendNoEdit_Guarded(t *testing.T) {
	ctx := context.Background()

	t.Run("amend fails and does not invoke git commit when on remote tip", func(t *testing.T) {
		gitDir := t.TempDir()
		mock := (&MockGitRunner{}).
			OnCommand("rev-parse --git-dir", gitDir+"\n").
			OnCommand("rev-list --parents -n 1 HEAD", "deadbeef parent1\n").
			OnCommand("for-each-ref --points-at=HEAD --format=%(refname) refs/remotes/", "refs/remotes/origin/main\n")
		err := NewGitClient(mock).CommitAmendNoEdit(ctx, io.Discard, io.Discard)
		if err == nil {
			t.Fatal("expected error, got nil")
		}
		if !strings.Contains(err.Error(), "cannot amend HEAD commit") {
			t.Errorf("unexpected error: %v", err)
		}
		if mock.HasCall("commit") {
			t.Fatalf("git commit was invoked despite remote tip guard: %v", mock.Calls)
		}
	})

	t.Run("amend succeeds when not on remote tip", func(t *testing.T) {
		gitDir := t.TempDir()
		mock := (&MockGitRunner{}).
			OnCommand("rev-parse --git-dir", gitDir+"\n").
			OnCommand("rev-list --parents -n 1 HEAD", "deadbeef parent1\n").
			OnCommand("for-each-ref --points-at=HEAD --format=%(refname) refs/remotes/", "")
		if err := NewGitClient(mock).CommitAmendNoEdit(ctx, io.Discard, io.Discard); err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if !mock.HasCall("commit --amend --only --allow-empty --no-edit") {
			t.Errorf("expected git commit --amend --only --allow-empty --no-edit to be invoked, got: %v", mock.Calls)
		}
	})
}

func TestResolveRemote_PriorityOrder(t *testing.T) {
	ctx := context.Background()

	t.Run("prefers ghish.gerrit.remote git config", func(t *testing.T) {
		mock := &MockGitRunner{}
		mock.OnCommand("config --get ghish.gerrit.remote", "partner\n").
			OnCommand("remote", "origin\ngoog\npartner\n")
		client := NewGitClient(mock)
		if got := client.ResolveRemote(ctx); got != "partner" {
			t.Errorf("ResolveRemote() = %q, want %q", got, "partner")
		}
	})

	t.Run("prefers current branch tracking remote", func(t *testing.T) {
		mock := &MockGitRunner{}
		mock.WithBranch("my-feature").
			OnCommand("config --get branch.my-feature.remote", "goog\n").
			OnCommand("remote", "origin\ngoog\n")
		client := NewGitClient(mock)
		if got := client.ResolveRemote(ctx); got != "goog" {
			t.Errorf("ResolveRemote() = %q, want %q", got, "goog")
		}
	})

	t.Run("prefers current branch tracking remote for branch name with slash", func(t *testing.T) {
		mock := &MockGitRunner{}
		mock.WithBranch("feature/my-fix").
			OnCommand("config --get branch.feature/my-fix.remote", "goog\n").
			OnCommand("remote", "origin\ngoog\n")
		client := NewGitClient(mock)
		if got := client.ResolveRemote(ctx); got != "goog" {
			t.Errorf("ResolveRemote() = %q, want %q", got, "goog")
		}
	})

	t.Run("selects goog when origin does not exist", func(t *testing.T) {
		mock := &MockGitRunner{}
		mock.OnCommand("remote", "goog\n")
		client := NewGitClient(mock)
		if got := client.ResolveRemote(ctx); got != "goog" {
			t.Errorf("ResolveRemote() = %q, want %q", got, "goog")
		}
	})

	t.Run("selects aosp when origin does not exist", func(t *testing.T) {
		mock := &MockGitRunner{}
		mock.OnCommand("remote", "aosp\n")
		client := NewGitClient(mock)
		if got := client.ResolveRemote(ctx); got != "aosp" {
			t.Errorf("ResolveRemote() = %q, want %q", got, "aosp")
		}
	})

	t.Run("defaults to origin when origin exists", func(t *testing.T) {
		mock := &MockGitRunner{}
		mock.OnCommand("remote", "origin\ngoog\n")
		client := NewGitClient(mock)
		if got := client.ResolveRemote(ctx); got != "origin" {
			t.Errorf("ResolveRemote() = %q, want %q", got, "origin")
		}
	})
}

func TestResolveDefaultBranch_PriorityOrder(t *testing.T) {
	ctx := context.Background()

	t.Run("prefers ghish.gerrit.defaultbranch git config", func(t *testing.T) {
		mock := &MockGitRunner{}
		mock.OnCommand("config --get ghish.gerrit.defaultbranch", "release-v2\n")
		client := NewGitClient(mock)
		if got := client.ResolveDefaultBranch(ctx); got != "release-v2" {
			t.Errorf("ResolveDefaultBranch() = %q, want %q", got, "release-v2")
		}
	})

	t.Run("uses symbolic-ref refs/remotes/<remote>/HEAD", func(t *testing.T) {
		mock := &MockGitRunner{}
		mock.OnCommand("remote", "goog\n").
			OnCommand("symbolic-ref refs/remotes/goog/HEAD", "refs/remotes/goog/dev\n")
		client := NewGitClient(mock)
		if got := client.ResolveDefaultBranch(ctx); got != "dev" {
			t.Errorf("ResolveDefaultBranch() = %q, want %q", got, "dev")
		}
	})

	t.Run("uses init.defaultBranch when symbolic-ref is absent", func(t *testing.T) {
		mock := &MockGitRunner{}
		mock.OnCommand("config --get init.defaultBranch", "trunk\n")
		client := NewGitClient(mock)
		if got := client.ResolveDefaultBranch(ctx); got != "trunk" {
			t.Errorf("ResolveDefaultBranch() = %q, want %q", got, "trunk")
		}
	})

	t.Run("prefers tracked upstream merge branch over origin/HEAD for release branches", func(t *testing.T) {
		mock := &MockGitRunner{}
		mock.WithBranch("my-hotfix").
			OnCommand("config --get branch.my-hotfix.remote", "origin\n").
			OnCommand("config --get branch.my-hotfix.merge", "refs/heads/release-1.0\n").
			OnCommand("symbolic-ref refs/remotes/origin/HEAD", "refs/remotes/origin/main\n")
		client := NewGitClient(mock)
		if got := client.ResolveDefaultBranch(ctx); got != "release-1.0" {
			t.Errorf("ResolveDefaultBranch() = %q, want %q", got, "release-1.0")
		}
	})

	t.Run("ignores self-referential branch.<cur>.merge and uses origin/HEAD", func(t *testing.T) {
		mock := &MockGitRunner{}
		mock.WithBranch("my-feature").
			OnCommand("config --get branch.my-feature.remote", "origin\n").
			OnCommand("config --get branch.my-feature.merge", "refs/heads/my-feature\n").
			OnCommand("symbolic-ref refs/remotes/origin/HEAD", "refs/remotes/origin/main\n")
		client := NewGitClient(mock)
		if got := client.ResolveDefaultBranch(ctx); got != "main" {
			t.Errorf("ResolveDefaultBranch() = %q, want %q", got, "main")
		}
	})

	// inclusive-language: disable
	t.Run("falls back to master when main does not exist", func(t *testing.T) {
		mock := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				cmd := strings.Join(args, " ")
				if strings.HasPrefix(cmd, "for-each-ref --format=%(refname) ") {
					// Only master refs resolve; nested refs below a
					// candidate pattern must not count as a match.
					fmt.Fprint(stdout, "refs/heads/main/nested\nrefs/remotes/origin/master\nrefs/heads/master\n")
					return nil
				}
				return fmt.Errorf("not found: %s", cmd)
			},
		}
		client := NewGitClient(mock)
		if got := client.ResolveDefaultBranch(ctx); got != "master" {
			t.Errorf("ResolveDefaultBranch() = %q, want %q", got, "master")
		}
	})
	// inclusive-language: enable
}

func TestStackCommits_SafeResolutionAndNoFullHistoryFallback(t *testing.T) {
	ctx := context.Background()

	t.Run("resolves non-origin remote branch goog/main..HEAD", func(t *testing.T) {
		rawLog := "aaa1111\x00feat: Commit 1\x00feat: Commit 1\n\nChange-Id: I1111111111111111111111111111111111111111\n\x1e"
		mock := &MockGitRunner{}
		mock.OnCommand("remote", "goog\n").
			OnCommand("log --reverse --format=%h%x00%s%x00%B%x1e goog/main..HEAD", rawLog)

		client := NewGitClient(mock)
		commits, err := client.StackCommits(ctx, "main")
		if err != nil {
			t.Fatalf("StackCommits failed: %v", err)
		}
		if len(commits) != 1 || commits[0].Hash != "aaa1111" {
			t.Errorf("unexpected commits: %+v", commits)
		}
	})

	t.Run("falls back to HEAD@{upstream}..HEAD when remote branch ref is missing", func(t *testing.T) {
		rawLog := "bbb2222\x00feat: Commit 2\x00feat: Commit 2\n\nChange-Id: I2222222222222222222222222222222222222222\n\x1e"
		mock := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				cmd := strings.Join(args, " ")
				switch cmd {
				case "rev-parse --verify origin/main", "rev-parse --verify refs/remotes/origin/main":
					return fmt.Errorf("unknown revision")
				case "rev-parse --verify HEAD@{upstream}":
					return nil
				case "log --reverse --format=%h%x00%s%x00%B%x1e HEAD@{upstream}..HEAD":
					stdout.Write([]byte(rawLog))
					return nil
				default:
					return fmt.Errorf("unexpected command: %s", cmd)
				}
			},
		}
		client := NewGitClient(mock)
		commits, err := client.StackCommits(ctx, "main")
		if err != nil {
			t.Fatalf("StackCommits failed: %v", err)
		}
		if len(commits) != 1 || commits[0].Hash != "bbb2222" {
			t.Errorf("unexpected commits: %+v", commits)
		}
	})

	t.Run("returns actionable error and never runs unbounded git log when base cannot be resolved", func(t *testing.T) {
		mock := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				if len(args) > 0 && args[0] == "log" {
					t.Fatalf("StackCommits ran unbounded git log when base ref failed to resolve: %v", args)
				}
				return fmt.Errorf("unknown revision")
			},
		}
		client := NewGitClient(mock)
		_, err := client.StackCommits(ctx, "nonexistent-base")
		if err == nil {
			t.Fatal("expected error when no base ref can be resolved, got nil")
		}
		for _, want := range []string{"nonexistent-base", "git fetch", "--base"} {
			if !strings.Contains(err.Error(), want) {
				t.Errorf("expected error to contain %q, got: %v", want, err)
			}
		}
	})
}

type testGitRepo struct {
	WorkDir   string
	RemoteDir string
	Client    GitClient
	t         *testing.T
}

func newTestGitRepo(t *testing.T) *testGitRepo {
	t.Helper()
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git binary not found in PATH")
	}
	ctx := t.Context()
	baseDir, err := filepath.Abs(t.TempDir())
	if err != nil {
		t.Fatalf("filepath.Abs(t.TempDir()) failed: %v", err)
	}
	remoteDir := filepath.Join(baseDir, "origin.git")
	workDir := filepath.Join(baseDir, "work")

	rawGit := &RealGitRunner{Dir: baseDir}
	if err := rawGit.Run(ctx, io.Discard, io.Discard, "init", "--bare", "--initial-branch=main", remoteDir); err != nil {
		t.Fatalf("git init --bare failed: %v", err)
	}
	if err := rawGit.Run(ctx, io.Discard, io.Discard, "--git-dir="+remoteDir, "config", "receive.advertisePushOptions", "true"); err != nil {
		t.Fatalf("git config receive.advertisePushOptions failed: %v", err)
	}
	if err := rawGit.Run(ctx, io.Discard, io.Discard, "init", "--initial-branch=main", workDir); err != nil {
		t.Fatalf("git init work failed: %v", err)
	}

	r := &testGitRepo{
		WorkDir:   workDir,
		RemoteDir: remoteDir,
		Client:    NewGitClientInDir(workDir),
		t:         t,
	}
	for _, kv := range [][2]string{
		{"user.name", "Test User"},
		{"user.email", "test@example.com"},
		{"user.signingkey", ""},
		{"commit.gpgsign", "false"},
	} {
		r.GitOutput("config", kv[0], kv[1])
	}
	r.GitOutput("remote", "add", "origin", remoteDir)
	r.StageFile("base.txt", "initial\n")
	r.GitOutput("commit", "-m", "Initial commit")
	r.GitOutput("push", "-u", "origin", "main")
	return r
}

func (r *testGitRepo) GitOutput(args ...string) string {
	r.t.Helper()
	var stdout, stderr bytes.Buffer
	fullArgs := append([]string{"-c", "commit.gpgsign=false"}, args...)
	if err := r.Client.Run(r.t.Context(), &stdout, &stderr, fullArgs...); err != nil {
		r.t.Fatalf("git %v failed: %v (%s)", args, err, stderr.String())
	}
	return strings.TrimSpace(stdout.String())
}

func (r *testGitRepo) WriteFile(rel, content string) {
	r.t.Helper()
	full := filepath.Join(r.WorkDir, rel)
	if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
		r.t.Fatalf("mkdir %s failed: %v", rel, err)
	}
	if err := os.WriteFile(full, []byte(content), 0o644); err != nil {
		r.t.Fatalf("write %s failed: %v", rel, err)
	}
}

func (r *testGitRepo) StageFile(rel, content string) {
	r.t.Helper()
	r.WriteFile(rel, content)
	r.GitOutput("add", rel)
}

func (r *testGitRepo) ReadFile(rel string) string {
	r.t.Helper()
	data, err := os.ReadFile(filepath.Join(r.WorkDir, rel))
	if err != nil {
		r.t.Fatalf("read %s failed: %v", rel, err)
	}
	return string(data)
}

func (r *testGitRepo) InstallCommitMsgHook() {
	r.t.Helper()
	hookPath := filepath.Join(r.WorkDir, ".git", "hooks", "commit-msg")
	if err := os.MkdirAll(filepath.Dir(hookPath), 0o755); err != nil {
		r.t.Fatalf("mkdir hooks failed: %v", err)
	}
	hookScript := "#!/bin/sh\ngrep -q '^Change-Id:' \"$1\" || git interpret-trailers --in-place --trailer \"Change-Id: I1111111111111111111111111111111111111111\" \"$1\"\n"
	if err := os.WriteFile(hookPath, []byte(hookScript), 0o755); err != nil {
		r.t.Fatalf("write commit-msg hook failed: %v", err)
	}
}

func (r *testGitRepo) BareGitOutput(args ...string) string {
	r.t.Helper()
	var stdout, stderr bytes.Buffer
	runner := &RealGitRunner{Dir: filepath.Dir(r.RemoteDir)}
	fullArgs := append([]string{"--git-dir=" + r.RemoteDir}, args...)
	if err := runner.Run(r.t.Context(), &stdout, &stderr, fullArgs...); err != nil {
		r.t.Fatalf("bare git %v failed: %v (%s)", args, err, stderr.String())
	}
	return strings.TrimSpace(stdout.String())
}

func (r *testGitRepo) InstallHook(name, body string) {
	r.t.Helper()
	hookPath := filepath.Join(r.WorkDir, ".git", "hooks", name)
	if err := os.MkdirAll(filepath.Dir(hookPath), 0o755); err != nil {
		r.t.Fatalf("mkdir hooks failed: %v", err)
	}
	if err := os.WriteFile(hookPath, []byte(body), 0o755); err != nil {
		r.t.Fatalf("write %s hook failed: %v", name, err)
	}
}

func (r *testGitRepo) InstallGerritRefHooks() {
	r.t.Helper()
	hooksDir := filepath.Join(r.RemoteDir, "hooks")
	if err := os.MkdirAll(hooksDir, 0o755); err != nil {
		r.t.Fatalf("mkdir remote hooks failed: %v", err)
	}
	preReceive := "#!/bin/sh\nwhile read old new ref; do [ \"$(git rev-parse --verify -q refs/changes/latest)\" = \"$new\" ] && { echo 'no new changes' >&2; exit 1; }; done\nexit 0\n"
	if err := os.WriteFile(filepath.Join(hooksDir, "pre-receive"), []byte(preReceive), 0o755); err != nil {
		r.t.Fatalf("write pre-receive hook failed: %v", err)
	}
	postReceive := "#!/bin/sh\nwhile read old new ref; do git update-ref refs/changes/latest \"$new\" && git update-ref -d \"$ref\"; done\nexit 0\n"
	if err := os.WriteFile(filepath.Join(hooksDir, "post-receive"), []byte(postReceive), 0o755); err != nil {
		r.t.Fatalf("write post-receive hook failed: %v", err)
	}
}

func (r *testGitRepo) BindToCLI() {
	r.t.Helper()
	oldGit := DefaultGitRunner
	oldCWD := MockCWD
	oldPreRunE := RootCmd.PersistentPreRunE
	DefaultGitRunner = r.Client
	MockCWD = r.WorkDir
	RootCmd.PersistentPreRunE = func(cmd *cobra.Command, args []string) error {
		if oldPreRunE != nil {
			if err := oldPreRunE(cmd, args); err != nil {
				return err
			}
		}
		if cfg := GetConfig(cmd); cfg != nil {
			cfg.CWD = r.WorkDir
		}
		return nil
	}
	r.t.Cleanup(func() {
		DefaultGitRunner = oldGit
		MockCWD = oldCWD
		RootCmd.PersistentPreRunE = oldPreRunE
	})
}

func initTestRepoWithOrigin(t *testing.T) (string, GitClient) {
	t.Helper()
	r := newTestGitRepo(t)
	return r.WorkDir, r.Client
}

func TestRealGitRunner_IgnoresInheritedGitDirWhenDirExplicit(t *testing.T) {
	ctx := t.Context()
	outerDir, outerClient := initTestRepoWithOrigin(t)
	innerDir, innerClient := initTestRepoWithOrigin(t)

	if err := outerClient.Run(ctx, io.Discard, io.Discard, "config", "ghish.test.outer", "leaked"); err != nil {
		t.Fatal(err)
	}
	outerSHA, err := outerClient.RevParse(ctx, "HEAD")
	if err != nil {
		t.Fatal(err)
	}

	// Simulate running inside `git rebase -x` or a Git hook where repository
	// and per-invocation config environment overrides are set.
	t.Setenv("GIT_DIR", filepath.Join(outerDir, ".git"))
	t.Setenv("GIT_WORK_TREE", outerDir)
	t.Setenv("GIT_CONFIG", filepath.Join(outerDir, ".git", "config"))
	t.Setenv("GIT_CONFIG_PARAMETERS", "'user.name=Inherited Config Params' 'commit.gpgsign=true'")
	t.Setenv("GIT_CONFIG_COUNT", "2")
	t.Setenv("GIT_CONFIG_KEY_0", "user.email")
	t.Setenv("GIT_CONFIG_VALUE_0", "inherited-count@example.com")
	t.Setenv("GIT_CONFIG_KEY_1", "commit.gpgsign")
	t.Setenv("GIT_CONFIG_VALUE_1", "true")

	gotGitDir, err := innerClient.GitDir(ctx)
	if err != nil {
		t.Fatalf("GitDir failed: %v", err)
	}
	wantGitDir := filepath.Join(innerDir, ".git")
	if gotGitDir != wantGitDir {
		t.Errorf("GitDir() = %q, want %q", gotGitDir, wantGitDir)
	}

	if val, err := innerClient.ConfigGet(ctx, "ghish.test.outer"); err == nil {
		t.Errorf("inherited GIT_CONFIG leaked into inner repo: got %q, want error", val)
	}

	if err := os.WriteFile(filepath.Join(innerDir, "inner.txt"), []byte("inner commit\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := innerClient.Run(ctx, io.Discard, io.Discard, "add", "inner.txt"); err != nil {
		t.Fatal(err)
	}
	if err := innerClient.Run(ctx, io.Discard, io.Discard, "commit", "-m", "pw_inner: Isolated commit"); err != nil {
		t.Fatalf("commit in innerDir failed under inherited GIT_DIR/GIT_CONFIG_*: %v", err)
	}

	authorEmail, err := innerClient.HeadAuthorEmail(ctx)
	if err != nil || authorEmail != "test@example.com" {
		t.Errorf("HeadAuthorEmail() = (%q, %v), want (test@example.com, nil)", authorEmail, err)
	}
	var authorNameBuf strings.Builder
	if err := innerClient.Run(ctx, &authorNameBuf, io.Discard, "log", "-1", "--format=%an", "HEAD"); err != nil {
		t.Fatal(err)
	}
	if gotName := strings.TrimSpace(authorNameBuf.String()); gotName != "Test User" {
		t.Errorf("author name = %q, want Test User (GIT_CONFIG_PARAMETERS leaked)", gotName)
	}

	// Outer repository MUST be completely untouched.
	afterOuterSHA, err := outerClient.RevParse(ctx, "HEAD")
	if err != nil || afterOuterSHA != outerSHA {
		t.Fatalf("inherited GIT_DIR leaked into outer repo: got %s, want %s", afterOuterSHA, outerSHA)
	}
}

func TestGitClient_GitDir_MockFallbackAndEmptyOutput(t *testing.T) {
	ctx := t.Context()

	defaultMock := &MockGitRunner{}
	t.Cleanup(defaultMock.cleanupMockGitDir)
	dir1, err := NewGitClient(defaultMock).GitDir(ctx)
	if err != nil {
		t.Fatalf("default MockGitRunner GitDir() failed: %v", err)
	}
	if dir1 == "" || dir1 == ".git" {
		t.Fatalf("expected isolated temp dir from default MockGitRunner, got %q", dir1)
	}
	if info, statErr := os.Stat(dir1); statErr != nil || !info.IsDir() {
		t.Fatalf("expected existing directory %q, statErr=%v", dir1, statErr)
	}
	dir2, err := NewGitClient(defaultMock).GitDir(ctx)
	if err != nil || dir2 != dir1 {
		t.Errorf("expected cached mock git dir %q, got (%q, %v)", dir1, dir2, err)
	}

	emptyRespMock := (&MockGitRunner{}).OnCommand("rev-parse --git-dir", "  \n")
	if _, err := NewGitClient(emptyRespMock).GitDir(ctx); err == nil {
		t.Errorf("expected GitDir() to error on empty OnCommand output, got nil")
	}

	emptyRunFnMock := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			return nil
		},
	}
	if _, err := NewGitClient(emptyRunFnMock).GitDir(ctx); err == nil {
		t.Errorf("expected GitDir() to error when RunFn writes empty output, got nil")
	}
}

func TestGitClient_CheckAmendAllowed_RealGit(t *testing.T) {
	ctx := t.Context()

	t.Run("blocks upstream tip", func(t *testing.T) {
		r := newTestGitRepo(t)
		if err := r.Client.CheckAmendAllowed(ctx); err == nil || !strings.Contains(err.Error(), "refs/remotes/origin/main") {
			t.Fatalf("expected upstream tip error, got: %v", err)
		}
	})

	t.Run("allows non-tip detached HEAD", func(t *testing.T) {
		r := newTestGitRepo(t)
		r.StageFile("base.txt", "unpushed\n")
		r.GitOutput("commit", "-m", "feat: Local commit")
		r.GitOutput("checkout", "--detach", "HEAD")
		if err := r.Client.CheckAmendAllowed(ctx); err != nil {
			t.Fatalf("expected non-tip detached HEAD to allow amend, got: %v", err)
		}
	})

	t.Run("blocks 2-parent merge commit", func(t *testing.T) {
		r := newTestGitRepo(t)
		r.GitOutput("checkout", "-b", "side")
		r.StageFile("side.txt", "side\n")
		r.GitOutput("commit", "-m", "side commit")
		r.GitOutput("checkout", "main")
		r.GitOutput("merge", "--no-ff", "-m", "Merge branch 'side'", "side")
		if err := r.Client.CheckAmendAllowed(ctx); err == nil || !strings.Contains(err.Error(), "merge commit") {
			t.Fatalf("expected merge commit error, got: %v", err)
		}
	})
}

func TestGitClient_CommitAmendNoEdit_RealGit(t *testing.T) {
	ctx := t.Context()

	t.Run("preserves staged index, unstaged edits, and untracked files", func(t *testing.T) {
		r := newTestGitRepo(t)
		if err := r.Client.CommitAmendNoEdit(ctx, io.Discard, io.Discard); err == nil {
			t.Fatal("expected CheckAmendAllowed error on origin/main tip")
		}

		r.StageFile("base.txt", "commit 1\n")
		r.GitOutput("commit", "-m", "pw_foo: Initial draft")
		origHeadTree := r.GitOutput("rev-parse", "HEAD^{tree}")
		r.InstallCommitMsgHook()

		r.StageFile("staged_only.txt", "staged content\n")
		origIndexTree := r.GitOutput("write-tree")
		r.WriteFile("base.txt", "unstaged worktree edit\n")
		r.WriteFile("untracked.txt", "untracked content\n")

		if err := r.Client.CommitAmendNoEdit(ctx, io.Discard, io.Discard); err != nil {
			t.Fatalf("CommitAmendNoEdit failed: %v", err)
		}
		gotMsg, err := r.Client.HeadCommitMessage(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(gotMsg, "Change-Id: I1111111111111111111111111111111111111111") {
			t.Errorf("expected commit-msg hook output in HEAD message, got:\n%s", gotMsg)
		}
		if got := r.GitOutput("rev-parse", "HEAD^{tree}"); got != origHeadTree {
			t.Errorf("CommitAmendNoEdit swept staged changes into HEAD tree: got %s, want %s", got, origHeadTree)
		}
		if got := r.GitOutput("write-tree"); got != origIndexTree {
			t.Errorf("CommitAmendNoEdit altered .git/index tree: got %s, want %s", got, origIndexTree)
		}
		if got := r.ReadFile("base.txt"); got != "unstaged worktree edit\n" {
			t.Errorf("unstaged edit altered: got %q", got)
		}
		if got := r.ReadFile("untracked.txt"); got != "untracked content\n" {
			t.Errorf("untracked file altered: got %q", got)
		}
	})

	t.Run("amends intentional allow-empty commit", func(t *testing.T) {
		r := newTestGitRepo(t)
		r.GitOutput("commit", "--allow-empty", "-m", "pw_foo: Trigger CI")
		r.InstallCommitMsgHook()
		if err := r.Client.CommitAmendNoEdit(ctx, io.Discard, io.Discard); err != nil {
			t.Fatalf("CommitAmendNoEdit on --allow-empty commit failed: %v", err)
		}
		gotMsg, err := r.Client.HeadCommitMessage(ctx)
		if err != nil || !strings.Contains(gotMsg, "Change-Id: I1111111111111111111111111111111111111111") {
			t.Errorf("expected Change-Id in amended empty commit, got msg=%q err=%v", gotMsg, err)
		}
	})
}

func assertNoCommitTempFiles(t *testing.T, dir string) {
	t.Helper()
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatalf("ReadDir(%s) failed: %v", dir, err)
	}
	for _, entry := range entries {
		if strings.HasPrefix(entry.Name(), "ghish-commit-msg-") {
			t.Errorf("leftover temp file in %s: %s", dir, entry.Name())
		}
	}
}

func TestGitClient_CommitAmendMessage_RealGit(t *testing.T) {
	ctx := t.Context()
	r := newTestGitRepo(t)
	tmpDir := t.TempDir()
	t.Setenv("TMPDIR", tmpDir)

	r.StageFile("committed.txt", "v1\n")
	r.GitOutput("commit", "-m", "pw_foo: Initial draft\n\nChange-Id: I1111111111111111111111111111111111111111\n")

	r.StageFile("staged.txt", "staged content\n")
	r.WriteFile("committed.txt", "v2 unstaged\n")
	r.WriteFile("untracked.txt", "untracked content\n")
	headTreeBefore := r.GitOutput("rev-parse", "HEAD^{tree}")
	indexTreeBefore := r.GitOutput("write-tree")

	newMsg := "pw_foo: Polished title with `backticks`\n\n--flag-looking-line preserved verbatim\n\nBug: b/567763970\nChange-Id: I1111111111111111111111111111111111111111\n"
	if err := r.Client.CommitAmendMessage(ctx, newMsg, false, io.Discard, io.Discard); err != nil {
		t.Fatalf("CommitAmendMessage failed: %v", err)
	}
	if got := r.GitOutput("rev-parse", "HEAD^{tree}"); got != headTreeBefore {
		t.Errorf("HEAD^{tree} changed from %s to %s", headTreeBefore, got)
	}
	if got := r.GitOutput("write-tree"); got != indexTreeBefore {
		t.Errorf("staged index tree changed from %s to %s", indexTreeBefore, got)
	}
	if got := r.ReadFile("committed.txt"); got != "v2 unstaged\n" {
		t.Errorf("unstaged edit lost: %q", got)
	}
	if got := r.ReadFile("untracked.txt"); got != "untracked content\n" {
		t.Errorf("untracked file lost: %q", got)
	}
	if gotMsg, err := r.Client.HeadCommitMessage(ctx); err != nil || gotMsg != strings.TrimSpace(newMsg) {
		t.Errorf("HeadCommitMessage = %q, err=%v; want %q", gotMsg, err, strings.TrimSpace(newMsg))
	}
	assertNoCommitTempFiles(t, tmpDir)

	r.InstallHook("pre-commit", "#!/bin/sh\necho 'hook blocked' >&2\nexit 1\n")
	if err := r.Client.CommitAmendMessage(ctx, newMsg, false, io.Discard, io.Discard); err == nil {
		t.Fatal("expected failing pre-commit hook to fail CommitAmendMessage when noVerify=false")
	}
	if err := r.Client.CommitAmendMessage(ctx, "pw_foo: Bypassed\n\nChange-Id: I1111111111111111111111111111111111111111\n", true, io.Discard, io.Discard); err != nil {
		t.Fatalf("expected noVerify=true to bypass hook, got: %v", err)
	}
	assertNoCommitTempFiles(t, tmpDir)
}

func TestGitClient_CommitNewAll_RealGit(t *testing.T) {
	ctx := t.Context()

	t.Run("auto-stages modified tracked files with -a and honors noVerify", func(t *testing.T) {
		r := newTestGitRepo(t)
		tmpDir := t.TempDir()
		t.Setenv("TMPDIR", tmpDir)
		r.WriteFile("base.txt", "modified tracked content\n")
		r.InstallHook("pre-commit", "#!/bin/sh\necho 'hook blocked' >&2\nexit 1\n")

		msg := "pw_bar: Add `CommitNewAll` helper\n\n- preserves leading dashes\n--and double dashes\n\nBug: b/567763970\n"
		if _, err := r.Client.CommitNewAll(ctx, msg, false, io.Discard, io.Discard); err == nil {
			t.Fatal("expected CommitNewAll with noVerify=false to fail on pre-commit hook")
		}
		autoStaged, err := r.Client.CommitNewAll(ctx, msg, true, io.Discard, io.Discard)
		if err != nil {
			t.Fatalf("CommitNewAll(noVerify=true) failed: %v", err)
		}
		if !autoStaged {
			t.Errorf("autoStaged = false, want true when index had no staged changes")
		}
		if gotMsg, err := r.Client.HeadCommitMessage(ctx); err != nil || gotMsg != strings.TrimSpace(msg) {
			t.Errorf("HeadCommitMessage = %q, err=%v; want %q", gotMsg, err, strings.TrimSpace(msg))
		}
		assertNoCommitTempFiles(t, tmpDir)
	})

	t.Run("commits only staged files when index is non-empty", func(t *testing.T) {
		r := newTestGitRepo(t)
		r.StageFile("selected.txt", "staged feature\n")
		r.WriteFile("base.txt", "unstaged debug edit\n")
		autoStaged, err := r.Client.CommitNewAll(ctx, "pw_bar: Commit only staged file\n", false, io.Discard, io.Discard)
		if err != nil {
			t.Fatalf("CommitNewAll failed: %v", err)
		}
		if autoStaged {
			t.Errorf("autoStaged = true, want false when staged changes were already present")
		}
		if got := r.GitOutput("show", "--pretty=format:", "--name-only", "HEAD"); got != "selected.txt" {
			t.Errorf("committed files = %q, want selected.txt", got)
		}
		if got := r.GitOutput("diff", "--name-only"); got != "base.txt" {
			t.Errorf("unstaged diff = %q, want base.txt", got)
		}
	})
}

func TestGitClient_UpdateHeadRef_RealGit(t *testing.T) {
	ctx := t.Context()
	r := newTestGitRepo(t)

	if err := r.Client.UpdateHeadRef(ctx, "", "abc", "reason"); err == nil {
		t.Fatal("expected error for empty targetSHA")
	}
	if err := r.Client.UpdateHeadRef(ctx, "abc", "", "reason"); err == nil {
		t.Fatal("expected error for empty expectedCurrentSHA")
	}

	origSHA := r.GitOutput("rev-parse", "HEAD")
	r.WriteFile("base.txt", "committed change\n")
	if _, err := r.Client.CommitNewAll(ctx, "pw_baz: Temporary commit\n", false, io.Discard, io.Discard); err != nil {
		t.Fatal(err)
	}
	newSHA := r.GitOutput("rev-parse", "HEAD")
	r.StageFile("extra.txt", "staged file\n")
	r.WriteFile("base.txt", "working tree edit\n")

	if err := r.Client.UpdateHeadRef(ctx, origSHA, origSHA, "rollback mismatch"); err == nil {
		t.Fatal("expected CAS mismatch error")
	}
	if err := r.Client.UpdateHeadRef(ctx, origSHA, newSHA, "rollback after failure"); err != nil {
		t.Fatalf("UpdateHeadRef failed: %v", err)
	}
	if got := r.GitOutput("rev-parse", "HEAD"); got != origSHA {
		t.Fatalf("HEAD = %s, want %s", got, origSHA)
	}
	if got := r.ReadFile("base.txt"); got != "working tree edit\n" {
		t.Errorf("working tree altered: %q", got)
	}
	if got := r.ReadFile("extra.txt"); got != "staged file\n" {
		t.Errorf("staged file altered: %q", got)
	}
}

func TestHeadRollbackGuard_RealGit(t *testing.T) {
	ctx := t.Context()
	r := newTestGitRepo(t)
	r.InstallCommitMsgHook()
	r.InstallGerritRefHooks()
	r.BindToCLI()

	cfg := &Config{Git: r.Client}
	origSHA := r.GitOutput("rev-parse", "HEAD")

	r.WriteFile("base.txt", "draft edit\n")
	guard := newHeadRollbackGuard(ctx, cfg, "test rollback")
	if err := guard.CommitNewAll(ctx, "pw_foo: Draft commit\n", false, io.Discard, io.Discard); err != nil {
		t.Fatalf("CommitNewAll failed: %v", err)
	}
	guard.Rollback(ctx, io.Discard)
	if got := r.GitOutput("rev-parse", "HEAD"); got != origSHA {
		t.Fatalf("Rollback did not restore HEAD: got %s, want %s", got, origSHA)
	}
	if hasStaged, err := r.Client.HasStagedChanges(ctx); err != nil || hasStaged {
		t.Fatalf("expected index tree restored, hasStaged=%v, err=%v", hasStaged, err)
	}

	guard2 := newHeadRollbackGuard(ctx, cfg, "test keep")
	if err := guard2.CommitNewAll(ctx, "pw_foo: Kept commit\n", false, io.Discard, io.Discard); err != nil {
		t.Fatalf("CommitNewAll failed: %v", err)
	}
	keptSHA := r.GitOutput("rev-parse", "HEAD")
	headMsg, err := r.Client.HeadCommitMessage(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if err := guard2.AmendMessageIfChanged(ctx, headMsg, headMsg+"\n", false, io.Discard, io.Discard); err != nil {
		t.Fatalf("no-op AmendMessageIfChanged failed: %v", err)
	}
	if got := r.GitOutput("rev-parse", "HEAD"); got != keptSHA {
		t.Fatalf("no-op AmendMessageIfChanged rewrote HEAD: got %s, want %s", got, keptSHA)
	}
	guard2.Keep()
	guard2.Rollback(ctx, io.Discard)
	if got := r.GitOutput("rev-parse", "HEAD"); got != keptSHA {
		t.Fatalf("Rollback after Keep() moved HEAD: got %s, want %s", got, keptSHA)
	}
	r.GitOutput("push", "origin", "HEAD:refs/for/main")
	if gotBare := r.BareGitOutput("rev-parse", "refs/changes/latest"); gotBare != keptSHA {
		t.Fatalf("expected refs/changes/latest = %s, got %s", keptSHA, gotBare)
	}
}

func TestHeadRollbackGuard_UnbornBranch_RealGit(t *testing.T) {
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git binary not found in PATH")
	}
	ctx := t.Context()
	workDir, err := filepath.Abs(t.TempDir())
	if err != nil {
		t.Fatalf("filepath.Abs(t.TempDir()) failed: %v", err)
	}
	client := NewGitClientInDir(workDir)
	if err := client.Run(ctx, io.Discard, io.Discard, "init", "--initial-branch=main"); err != nil {
		t.Fatalf("git init failed: %v", err)
	}
	for _, kv := range [][2]string{
		{"user.name", "Test User"},
		{"user.email", "test@example.com"},
		{"user.signingkey", ""},
		{"commit.gpgsign", "false"},
	} {
		if err := client.Run(ctx, io.Discard, io.Discard, "config", kv[0], kv[1]); err != nil {
			t.Fatalf("git config %s failed: %v", kv[0], err)
		}
	}

	stagedPath := filepath.Join(workDir, "staged.txt")
	if err := os.WriteFile(stagedPath, []byte("staged unborn content\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := client.Run(ctx, io.Discard, io.Discard, "add", "staged.txt"); err != nil {
		t.Fatal(err)
	}

	cfg := &Config{Git: client}
	guard := newHeadRollbackGuard(ctx, cfg, "test unborn rollback")
	if guard.origHEAD != "" {
		t.Fatalf("expected empty origHEAD on unborn branch, got %q", guard.origHEAD)
	}
	if err := guard.CommitNewAll(ctx, "pw_foo: Initial commit on unborn branch\n", false, io.Discard, io.Discard); err != nil {
		t.Fatalf("CommitNewAll on unborn branch failed: %v", err)
	}
	if sha, err := client.RevParse(ctx, "HEAD"); err != nil || sha == "" {
		t.Fatalf("expected commit on HEAD before rollback, got sha=%q err=%v", sha, err)
	}

	guard.Rollback(ctx, io.Discard)

	var symBuf bytes.Buffer
	if err := client.Run(ctx, &symBuf, io.Discard, "symbolic-ref", "HEAD"); err != nil {
		t.Fatalf("git symbolic-ref HEAD failed after Rollback: %v", err)
	}
	if gotSym := strings.TrimSpace(symBuf.String()); gotSym != "refs/heads/main" {
		t.Errorf("symbolic-ref HEAD = %q, want refs/heads/main", gotSym)
	}
	if err := client.Run(ctx, io.Discard, io.Discard, "rev-parse", "--verify", "HEAD"); err == nil {
		t.Fatal("expected git rev-parse --verify HEAD to fail after unwinding unborn branch")
	}
	if hasStaged, err := client.HasStagedChanges(ctx); err != nil || !hasStaged {
		t.Errorf("expected staged file preserved in index, got hasStaged=%v err=%v", hasStaged, err)
	}
	var lsBuf bytes.Buffer
	if err := client.Run(ctx, &lsBuf, io.Discard, "ls-files"); err != nil || strings.TrimSpace(lsBuf.String()) != "staged.txt" {
		t.Errorf("git ls-files = %q (err=%v), want staged.txt", strings.TrimSpace(lsBuf.String()), err)
	}
	if data, err := os.ReadFile(stagedPath); err != nil || string(data) != "staged unborn content\n" {
		t.Errorf("working tree staged.txt = %q (err=%v), want %q", string(data), err, "staged unborn content\n")
	}

	// Verify autoStaged=true clears index entries via read-tree --empty while preserving working tree.
	guardAuto := newHeadRollbackGuard(ctx, cfg, "test unborn auto-staged rollback")
	if err := guardAuto.CommitNewAll(ctx, "pw_foo: Auto-staged unborn commit\n", false, io.Discard, io.Discard); err != nil {
		t.Fatalf("second CommitNewAll on unborn branch failed: %v", err)
	}
	guardAuto.autoStaged = true
	guardAuto.Rollback(ctx, io.Discard)
	lsBuf.Reset()
	if err := client.Run(ctx, &lsBuf, io.Discard, "ls-files"); err != nil || strings.TrimSpace(lsBuf.String()) != "" {
		t.Errorf("expected empty index after autoStaged unborn rollback, got %q (err=%v)", lsBuf.String(), err)
	}
	if data, err := os.ReadFile(stagedPath); err != nil || string(data) != "staged unborn content\n" {
		t.Errorf("working tree staged.txt after read-tree --empty = %q (err=%v)", string(data), err)
	}
}

func TestEnsureChangeID_NoVerifyBypassesHooks_RealGit(t *testing.T) {
	ctx := t.Context()
	r := newTestGitRepo(t)
	r.StageFile("feature.txt", "new feature\n")
	r.GitOutput("commit", "-m", "pw_foo: Add feature without Change-Id")
	r.InstallCommitMsgHook()
	r.InstallHook("pre-commit", "#!/bin/sh\nexit 1\n")

	cfg := &Config{Git: r.Client}
	if err := EnsureChangeID(ctx, cfg, false, io.Discard, io.Discard); err == nil {
		t.Fatal("expected EnsureChangeID(noVerify=false) to fail when pre-commit hook exits 1")
	}
	r.InstallHook("commit-msg", "#!/bin/sh\nexit 1\n")
	if err := EnsureChangeID(ctx, cfg, true, io.Discard, io.Discard); err != nil {
		t.Fatalf("EnsureChangeID(noVerify=true) failed: %v", err)
	}

	gotMsg, err := r.Client.HeadCommitMessage(ctx)
	if err != nil {
		t.Fatalf("HeadCommitMessage failed: %v", err)
	}
	if !HasChangeID(gotMsg) {
		t.Fatalf("expected valid Change-Id in HEAD commit message, got:\n%s", gotMsg)
	}
	if err := ValidateChangeIDFooters(gotMsg, ""); err != nil {
		t.Fatalf("ValidateChangeIDFooters failed on injected Change-Id: %v", err)
	}
	if !strings.HasPrefix(gotMsg, "pw_foo: Add feature without Change-Id\n") {
		t.Errorf("unexpected commit message prefix:\n%s", gotMsg)
	}
}

func TestGitClient_MemoizationAndCacheInvalidation(t *testing.T) {
	ctx := t.Context()
	currentBranch := "feature-a"
	emailVal := "first@example.com"
	gitDirVal := "/repo/.git"

	mock := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			cmd := strings.Join(args, " ")
			switch cmd {
			case "branch --show-current":
				stdout.Write([]byte(currentBranch + "\n"))
				return nil
			case "rev-parse --git-dir":
				stdout.Write([]byte(gitDirVal + "\n"))
				return nil
			case "config --get user.email":
				stdout.Write([]byte(emailVal + "\n"))
				return nil
			case "config --get branch.feature-a.remote":
				stdout.Write([]byte("goog\n"))
				return nil
			case "config --get branch.feature-a.merge":
				stdout.Write([]byte("refs/heads/main\n"))
				return nil
			case "config --get missing.key":
				return fmt.Errorf("exit status 1")
			case "checkout feature-b", "config user.email second@example.com":
				return nil
			default:
				return fmt.Errorf("unhandled: %s", cmd)
			}
		},
	}

	client := NewGitClient(mock)

	for range 5 {
		if b, err := client.CurrentBranch(ctx); err != nil || b != "feature-a" {
			t.Fatalf("CurrentBranch() = (%q, %v), want feature-a", b, err)
		}
		if gd, err := client.GitDir(ctx); err != nil || gd != "/repo/.git" {
			t.Fatalf("GitDir() = (%q, %v), want /repo/.git", gd, err)
		}
		if v, err := client.ConfigGet(ctx, "user.email"); err != nil || v != "first@example.com" {
			t.Fatalf("ConfigGet(user.email) = (%q, %v), want first@example.com", v, err)
		}
		if _, err := client.ConfigGet(ctx, "missing.key"); err == nil {
			t.Fatal("expected error for missing.key")
		}
		if r := client.ResolveRemote(ctx); r != "goog" {
			t.Fatalf("ResolveRemote() = %q, want goog", r)
		}
		if d := client.ResolveDefaultBranch(ctx); d != "main" {
			t.Fatalf("ResolveDefaultBranch() = %q, want main", d)
		}
	}

	counts := make(map[string]int)
	for _, call := range mock.Calls {
		counts[call]++
	}
	for _, key := range []string{
		"branch --show-current",
		"rev-parse --git-dir",
		"config --get user.email",
		"config --get missing.key",
		"config --get branch.feature-a.remote",
		"config --get branch.feature-a.merge",
	} {
		if counts[key] != 1 {
			t.Errorf("call %q count = %d, want 1 (calls: %v)", key, counts[key], mock.Calls)
		}
	}

	// Mutating operations (Checkout, config write) must invalidate cached state.
	currentBranch = "feature-b"
	gitDirVal = "/repo/.git/worktrees/feature-b"
	if err := client.Checkout(ctx, "feature-b", io.Discard, io.Discard); err != nil {
		t.Fatalf("Checkout failed: %v", err)
	}
	if b, err := client.CurrentBranch(ctx); err != nil || b != "feature-b" {
		t.Fatalf("CurrentBranch() after Checkout = (%q, %v), want feature-b", b, err)
	}
	if gd, err := client.GitDir(ctx); err != nil || gd != "/repo/.git/worktrees/feature-b" {
		t.Fatalf("GitDir() after Checkout = (%q, %v), want /repo/.git/worktrees/feature-b", gd, err)
	}

	emailVal = "second@example.com"
	if err := client.Run(ctx, io.Discard, io.Discard, "config", "user.email", "second@example.com"); err != nil {
		t.Fatalf("config set failed: %v", err)
	}
	if v, err := client.ConfigGet(ctx, "user.email"); err != nil || v != "second@example.com" {
		t.Fatalf("ConfigGet(user.email) after config write = (%q, %v), want second@example.com", v, err)
	}
}

func TestIsReadOnlyGitQuery(t *testing.T) {
	for _, tc := range []struct {
		args []string
		want bool
	}{
		{[]string{"rev-parse", "--git-dir"}, true},
		{[]string{"diff-index", "--quiet", "HEAD", "--"}, true},
		{[]string{"config", "--get", "user.email"}, true},
		{[]string{"config", "user.email"}, true},
		{[]string{"config", "user.email", "new@example.com"}, false},
		{[]string{"config", "--unset", "user.email"}, false},
		{[]string{"branch", "--show-current"}, true},
		{[]string{"branch", "-D", "old"}, false},
		{[]string{"branch", "new-branch"}, false},
		{[]string{"remote"}, true},
		{[]string{"remote", "-v"}, true},
		{[]string{"remote", "--verbose"}, true},
		{[]string{"remote", "-v", "show", "origin"}, true},
		{[]string{"remote", "get-url", "origin"}, true},
		{[]string{"remote", "add", "origin", "https://example.com"}, false},
		{[]string{"remote", "-v", "add", "origin", "https://example.com"}, false},
		{[]string{"symbolic-ref", "-q", "refs/remotes/origin/HEAD"}, true},
		{[]string{"symbolic-ref", "refs/remotes/origin/HEAD", "refs/remotes/origin/main"}, false},
		{[]string{"symbolic-ref", "-d", "refs/remotes/origin/HEAD"}, false},
	} {
		if got := isReadOnlyGitQuery(tc.args); got != tc.want {
			t.Errorf("isReadOnlyGitQuery(%q) = %v, want %v", tc.args, got, tc.want)
		}
	}
}

func TestSeedPrefetchedGitConfig(t *testing.T) {
	ctx := t.Context()
	mock := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			cmd := strings.Join(args, " ")
			if cmd == "config --get core.bare" {
				stdout.Write([]byte("false\n"))
				return nil
			}
			return fmt.Errorf("unexpected git subprocess: %s", cmd)
		},
	}

	dc := NewGitClient(mock).(*defaultGitClient)
	dc.seedPrefetchedGitConfig(strings.Join([]string{
		"user.email erahm@google.com",
		"init.defaultbranch main",
		"branch.MyFeature.remote goog",
		"remote.goog.url https://pigweed.googlesource.com/pigweed/pigweed",
		"ghish.authmode googler",
	}, "\n"))

	// Hits in prefetched sections (both exact subsection case and case-insensitive key) must spawn 0 subprocesses.
	if got, err := dc.ConfigGet(ctx, "user.email"); err != nil || got != "erahm@google.com" {
		t.Fatalf("ConfigGet(user.email) = (%q, %v), want erahm@google.com", got, err)
	}
	if got, err := dc.ConfigGet(ctx, "init.defaultBranch"); err != nil || got != "main" {
		t.Fatalf("ConfigGet(init.defaultBranch) = (%q, %v), want main", got, err)
	}
	if got, err := dc.ConfigGet(ctx, "branch.MyFeature.remote"); err != nil || got != "goog" {
		t.Fatalf("ConfigGet(branch.MyFeature.remote) = (%q, %v), want goog", got, err)
	}
	if got, err := dc.ConfigGet(ctx, "remote.goog.url"); err != nil || got != "https://pigweed.googlesource.com/pigweed/pigweed" {
		t.Fatalf("ConfigGet(remote.goog.url) = (%q, %v)", got, err)
	}

	// Negative lookups in prefetched sections must fail immediately without spawning git.
	for _, missingKey := range []string{
		"ghish.gerrit.remote",
		"branch.other.remote",
		"remote.origin.url",
		"user.signingkey",
		"http.cookiefile",
	} {
		if _, err := dc.ConfigGet(ctx, missingKey); err == nil {
			t.Fatalf("expected error for missing prefetched key %q, got nil", missingKey)
		}
	}
	if len(mock.Calls) != 0 {
		t.Fatalf("expected 0 git subprocesses for prefetched sections, got: %v", mock.Calls)
	}

	// Non-prefetched section (core.*) still delegates to git runner.
	if got, err := dc.ConfigGet(ctx, "core.bare"); err != nil || got != "false" {
		t.Fatalf("ConfigGet(core.bare) = (%q, %v), want false", got, err)
	}
	if len(mock.Calls) != 1 || mock.Calls[0] != "config --get core.bare" {
		t.Fatalf("unexpected mock calls: %v", mock.Calls)
	}
}

func TestRealGitRunner_ReadOnlyEnvFlags(t *testing.T) {
	ctx := t.Context()
	runner := &RealGitRunner{}

	readOnlyCases := [][]string{
		{"rev-parse", "HEAD"},
		{"config", "user.email"},
		{"config", "--get", "user.email"},
		{"branch", "--show-current"},
		{"branch"},
		{"log", "-1"},
		{"status", "--porcelain"},
		{"diff", "HEAD"},
		{"diff-index", "--quiet", "HEAD"},
		{"remote", "-v"},
		{"remote", "get-url", "origin"},
		{"symbolic-ref", "HEAD"},
		{"rev-list", "HEAD"},
		{"ls-files"},
		{"show", "HEAD"},
		{"cat-file", "-t", "HEAD"},
		{"for-each-ref"},
		{"name-rev", "HEAD"},
		{"describe", "--tags"},
		{"shortlog", "-s"},
	}
	for _, baseArgs := range readOnlyCases {
		for _, prefix := range [][]string{
			nil,
			{"-C", "/tmp/repo"},
			{"-c", "color.ui=false"},
			{"--git-dir", "/tmp/repo/.git", "--work-tree", "/tmp/repo"},
			{"--no-pager"},
		} {
			args := slices.Concat(prefix, baseArgs)
			if !isReadOnlyGitQuery(args) {
				t.Errorf("isReadOnlyGitQuery(%v) = false, want true", args)
			}
			env := runner.buildEnv(ctx, args)
			if !slices.Contains(env, "GIT_CONFIG_NOSYSTEM=1") {
				t.Errorf("buildEnv(%v) missing GIT_CONFIG_NOSYSTEM=1: %v", args, env)
			}
			if !slices.Contains(env, "GIT_OPTIONAL_LOCKS=0") {
				t.Errorf("buildEnv(%v) missing GIT_OPTIONAL_LOCKS=0: %v", args, env)
			}
		}
	}

	writeCases := [][]string{
		{"commit"},
		{"push"},
		{"fetch"},
		{"checkout"},
		{"cherry-pick"},
		{"update-ref"},
		{"add"},
		{"init"},
		{"merge"},
		{"rebase"},
		{"reset"},
		{"branch", "new-branch"},
		{"branch", "-D", "old-branch"},
		{"config", "user.name", "Alice"},
		{"remote", "add", "origin", "https://example.com"},
		{"symbolic-ref", "HEAD", "refs/heads/main"},
	}
	for _, baseArgs := range writeCases {
		args := slices.Concat([]string{"-c", "commit.gpgsign=false"}, baseArgs)
		if isReadOnlyGitQuery(args) {
			t.Errorf("isReadOnlyGitQuery(%v) = true, want false", args)
		}
		env := runner.buildEnv(ctx, args)
		if slices.Contains(env, "GIT_CONFIG_NOSYSTEM=1") {
			t.Errorf("buildEnv(%v) unexpectedly set GIT_CONFIG_NOSYSTEM=1", args)
		}
		if slices.Contains(env, "GIT_OPTIONAL_LOCKS=0") {
			t.Errorf("buildEnv(%v) unexpectedly set GIT_OPTIONAL_LOCKS=0", args)
		}
	}

	// Verify RealGitRunner.Run passes GIT_CONFIG_NOSYSTEM=1 to git subprocesses
	// so GIT_CONFIG_SYSTEM is ignored during read-only config lookups.
	r := newTestGitRepo(t)
	sysCfg := filepath.Join(t.TempDir(), "system-gitconfig")
	if err := os.WriteFile(sysCfg, []byte("[ghish \"test\"]\n\tsystemonly = from-system\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	t.Setenv("GIT_CONFIG_SYSTEM", sysCfg)
	if val, err := r.Client.ConfigGet(ctx, "ghish.test.systemonly"); err == nil {
		t.Errorf("expected GIT_CONFIG_NOSYSTEM=1 to skip GIT_CONFIG_SYSTEM on read-only git config, got %q", val)
	}
}

func TestConfig_MemoizesGitClientRemoteDefaultBranchAndGerritURL(t *testing.T) {
	ctx := t.Context()
	mock := (&MockGitRunner{}).
		WithBranch("my-feature").
		OnCommand("config --get branch.my-feature.remote", "goog\n").
		OnCommand("config --get branch.my-feature.merge", "refs/heads/release-v1\n").
		OnCommand("config --get remote.goog.url", "https://pigweed.googlesource.com/pigweed/pigweed\n")

	cfg := &Config{Git: mock}

	if cfg.GitClient() != cfg.GitClient() {
		t.Fatal("expected Config.GitClient() to return the cached GitClient instance")
	}

	var wg sync.WaitGroup
	for range 8 {
		wg.Add(1)
		go func() {
			defer wg.Done()
			if got := cfg.ResolveRemote(ctx); got != "goog" {
				t.Errorf("ResolveRemote() = %q, want goog", got)
			}
			if got := cfg.ResolveDefaultBranch(ctx); got != "release-v1" {
				t.Errorf("ResolveDefaultBranch() = %q, want release-v1", got)
			}
			if got, err := cfg.GerritURL(ctx); err != nil || got != "https://pigweed-review.googlesource.com/a" {
				t.Errorf("GerritURL() = (%q, %v), want https://pigweed-review.googlesource.com/a", got, err)
			}
			if got := cfg.GerritHost(ctx); got != "pigweed-review.googlesource.com" {
				t.Errorf("GerritHost() = %q, want pigweed-review.googlesource.com", got)
			}
		}()
	}
	wg.Wait()

	counts := make(map[string]int)
	for _, call := range mock.Calls {
		counts[call]++
	}
	for cmd, n := range counts {
		if n > 1 {
			t.Errorf("duplicate git subprocess %q invoked %d times (all calls: %v)", cmd, n, mock.Calls)
		}
	}
}
