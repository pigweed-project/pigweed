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
