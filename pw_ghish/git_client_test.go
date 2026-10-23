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
	"strings"
	"testing"
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

	var calls []string
	mock := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			calls = append(calls, strings.Join(args, " "))
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
		"for-each-ref --points-at=HEAD --format=%(refname) refs/remotes/",
		"commit --amend --no-edit",
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

func TestGitClient_NilRunnerError(t *testing.T) {
	client := &defaultGitClient{runner: nil}
	err := client.Run(context.Background(), io.Discard, io.Discard, "status")
	if err == nil {
		t.Fatal("expected error with nil runner, got nil")
	}
	if !strings.Contains(err.Error(), "git runner not initialized") {
		t.Errorf("unexpected error: %v", err)
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

	t.Run("nil context returns error", func(t *testing.T) {
		client := NewGitClient(&MockGitRunner{})
		_, err := client.VerifyRef(nil, "refs/heads/main")
		if err == nil {
			t.Fatal("expected error for nil context, got nil")
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

	t.Run("uninitialized runner returns error", func(t *testing.T) {
		client := &defaultGitClient{runner: nil}
		_, err := client.VerifyRef(ctx, "refs/heads/main")
		if err == nil {
			t.Fatal("expected error for uninitialized runner, got nil")
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

	t.Run("nil context returns error", func(t *testing.T) {
		err := client.Run(nil, io.Discard, io.Discard, "status")
		if err == nil {
			t.Fatal("expected error for nil context, got nil")
		}
	})

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
		mock := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				if len(args) >= 3 && args[0] == "for-each-ref" && args[1] == "--points-at=HEAD" {
					stdout.Write([]byte("refs/remotes/origin/main\n"))
					return nil
				}
				return fmt.Errorf("unexpected: %v", args)
			},
		}
		client := NewGitClient(mock)
		err := client.CheckAmendAllowed(ctx)
		if err == nil {
			t.Fatal("expected error, got nil")
		}
		if !strings.Contains(err.Error(), "cannot amend HEAD commit") || !strings.Contains(err.Error(), "refs/remotes/origin/main") {
			t.Errorf("expected error to explain remote tip block, got: %v", err)
		}
	})

	t.Run("allows amend when not on remote tip", func(t *testing.T) {
		mock := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				if len(args) >= 3 && args[0] == "for-each-ref" && args[1] == "--points-at=HEAD" {
					return nil
				}
				return fmt.Errorf("unexpected: %v", args)
			},
		}
		client := NewGitClient(mock)
		if err := client.CheckAmendAllowed(ctx); err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
	})
}

func TestGitClient_CommitAmendNoEdit_Guarded(t *testing.T) {
	ctx := context.Background()

	t.Run("amend fails and does not invoke git commit when on remote tip", func(t *testing.T) {
		mock := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				if len(args) >= 3 && args[0] == "for-each-ref" && args[1] == "--points-at=HEAD" {
					stdout.Write([]byte("refs/remotes/origin/main\n"))
					return nil
				}
				if len(args) >= 1 && args[0] == "commit" {
					t.Fatalf("git commit was invoked despite remote tip guard!")
				}
				return fmt.Errorf("unexpected: %v", args)
			},
		}
		client := NewGitClient(mock)
		err := client.CommitAmendNoEdit(ctx, io.Discard, io.Discard)
		if err == nil {
			t.Fatal("expected error, got nil")
		}
		if !strings.Contains(err.Error(), "cannot amend HEAD commit") {
			t.Errorf("unexpected error: %v", err)
		}
	})

	t.Run("amend succeeds when not on remote tip", func(t *testing.T) {
		commitInvoked := false
		mock := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				if len(args) >= 3 && args[0] == "for-each-ref" && args[1] == "--points-at=HEAD" {
					return nil
				}
				if len(args) >= 3 && args[0] == "commit" && args[1] == "--amend" && args[2] == "--no-edit" {
					commitInvoked = true
					return nil
				}
				return fmt.Errorf("unexpected: %v", args)
			},
		}
		client := NewGitClient(mock)
		err := client.CommitAmendNoEdit(ctx, io.Discard, io.Discard)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if !commitInvoked {
			t.Errorf("expected git commit --amend to be invoked")
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
				if cmd == "rev-parse --verify refs/remotes/origin/master" || cmd == "rev-parse --verify refs/heads/master" {
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

func initTestRepoWithOrigin(t *testing.T) (string, GitClient) {
	t.Helper()
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git binary not found in PATH")
	}
	ctx := t.Context()
	baseDir := t.TempDir()
	remoteDir := filepath.Join(baseDir, "origin.git")
	workDir := filepath.Join(baseDir, "work")

	rawGit := &RealGitRunner{Dir: baseDir}
	if err := rawGit.Run(ctx, io.Discard, io.Discard, "init", "--bare", "--initial-branch=main", remoteDir); err != nil {
		t.Fatalf("git init --bare failed: %v", err)
	}
	if err := rawGit.Run(ctx, io.Discard, io.Discard, "init", "--initial-branch=main", workDir); err != nil {
		t.Fatalf("git init work failed: %v", err)
	}

	client := NewGitClientInDir(workDir)
	run := func(args ...string) {
		t.Helper()
		fullArgs := append([]string{"-c", "commit.gpgsign=false"}, args...)
		if err := client.Run(ctx, io.Discard, io.Discard, fullArgs...); err != nil {
			t.Fatalf("git %v failed: %v", args, err)
		}
	}
	for _, kv := range [][2]string{
		{"user.name", "Test User"},
		{"user.email", "test@example.com"},
		{"commit.gpgsign", "false"},
	} {
		run("config", kv[0], kv[1])
	}
	run("remote", "add", "origin", remoteDir)
	if err := os.WriteFile(filepath.Join(workDir, "base.txt"), []byte("initial\n"), 0o644); err != nil {
		t.Fatalf("write base.txt failed: %v", err)
	}
	run("add", "base.txt")
	run("commit", "-m", "Initial commit")
	run("push", "-u", "origin", "main")
	return workDir, client
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
