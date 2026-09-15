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
	"net/http"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/spf13/cobra"
)

func mockCmdWithGit(ctx context.Context, mockGit *MockGitRunner) *cobra.Command {
	cmd := &cobra.Command{}
	cmd.SetContext(ctx)
	SetConfig(cmd, &Config{Git: mockGit})
	return cmd
}

func TestParseChangeAndRevision(t *testing.T) {
	tests := []struct {
		input      string
		wantChange string
		wantRev    string
	}{
		{
			input:      "12345",
			wantChange: "12345",
			wantRev:    "current",
		},
		{
			input:      "12345/3",
			wantChange: "12345",
			wantRev:    "3",
		},
		{
			input:      "12345/",
			wantChange: "12345",
			wantRev:    "current",
		},
		{
			input:      "my-project~branch~I1234567890/2",
			wantChange: "my-project~branch~I1234567890",
			wantRev:    "2",
		},
		{
			input:      "https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/472267",
			wantChange: "472267",
			wantRev:    "current",
		},
		{
			input:      "https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/472267/3",
			wantChange: "472267",
			wantRev:    "3",
		},
		{
			input:      "https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/472267/3/",
			wantChange: "472267",
			wantRev:    "3",
		},
		{
			input:      "https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/472267/3?tab=comments#message-1",
			wantChange: "472267",
			wantRev:    "3",
		},
		{
			input:      "https://pigweed-review.googlesource.com/+/472267",
			wantChange: "472267",
			wantRev:    "current",
		},
		{
			input:      "https://pigweed-review.googlesource.com/+/472267/2",
			wantChange: "472267",
			wantRev:    "2",
		},
		{
			input:      "https://pigweed-review.googlesource.com/472267",
			wantChange: "472267",
			wantRev:    "current",
		},
		{
			input:      "https://pigweed-review.googlesource.com/472267/4",
			wantChange: "472267",
			wantRev:    "4",
		},
		{
			input:      "https://pigweed-review.googlesource.com/changes/472267",
			wantChange: "472267",
			wantRev:    "current",
		},
		{
			input:      "https://pigweed-review.googlesource.com/changes/472267/revisions/4",
			wantChange: "472267",
			wantRev:    "4",
		},
		{
			input:      "pwrev/472267",
			wantChange: "472267",
			wantRev:    "current",
		},
		{
			input:      "pwrev/472267/5",
			wantChange: "472267",
			wantRev:    "5",
		},
		{
			input:      "https://pwrev.dev/472267",
			wantChange: "472267",
			wantRev:    "current",
		},
		{
			input:      "pwrev.dev/472267",
			wantChange: "472267",
			wantRev:    "current",
		},
		{
			input:      "pwrev.dev/i/472267",
			wantChange: "472267",
			wantRev:    "current",
		},
		{
			input:      "https://pwrev.dev/i/472267",
			wantChange: "472267",
			wantRev:    "current",
		},
		{
			input:      "fxrev/472267",
			wantChange: "472267",
			wantRev:    "current",
		},
		{
			input:      "fxrev.dev/i/472267",
			wantChange: "472267",
			wantRev:    "current",
		},
		{
			input:      "https://fxrev.dev/i/472267/3",
			wantChange: "472267",
			wantRev:    "3",
		},
		{
			input:      "crrev.com/c/472267",
			wantChange: "472267",
			wantRev:    "current",
		},
		{
			input:      "crrev.com/i/472267",
			wantChange: "472267",
			wantRev:    "current",
		},
		{
			input:      "cl/472267",
			wantChange: "472267",
			wantRev:    "current",
		},
		{
			input:      "cl/472267/2",
			wantChange: "472267",
			wantRev:    "2",
		},
		{
			input:      "change-472267",
			wantChange: "472267",
			wantRev:    "current",
		},
	}

	for _, tt := range tests {
		t.Run(tt.input, func(t *testing.T) {
			gotChange, gotRev := ParseChangeAndRevision(tt.input)
			if gotChange != tt.wantChange || gotRev != tt.wantRev {
				t.Errorf("ParseChangeAndRevision(%q) = (%q, %q), want (%q, %q)",
					tt.input, gotChange, gotRev, tt.wantChange, tt.wantRev)
			}
		})
	}
}

func TestHasChangeID(t *testing.T) {
	tests := []struct {
		desc string
		msg  string
		want bool
	}{
		{
			desc: "valid change ID in footer",
			msg:  "Commit subject\n\nCommit description.\n\nChange-Id: I0123456789abcdef0123456789abcdef01234567\n",
			want: true,
		},
		{
			desc: "valid change ID without trailing newline",
			msg:  "Commit subject\n\nChange-Id: I0123456789abcdef0123456789abcdef01234567",
			want: true,
		},
		{
			desc: "missing change ID",
			msg:  "Commit subject\n\nCommit description.\n",
			want: false,
		},
		{
			desc: "malformed change ID (too short)",
			msg:  "Commit subject\n\nChange-Id: I12345\n",
			want: false,
		},
		{
			desc: "malformed change ID (no leading I)",
			msg:  "Commit subject\n\nChange-Id: 0123456789abcdef0123456789abcdef01234567\n",
			want: false,
		},
	}

	for _, tt := range tests {
		t.Run(tt.desc, func(t *testing.T) {
			got := HasChangeID(tt.msg)
			if got != tt.want {
				t.Errorf("HasChangeID() = %v, want %v for %q", got, tt.want, tt.msg)
			}
		})
	}
}

func TestEnsureChangeID_Valid(t *testing.T) {
	ctx := context.Background()
	mockGit := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) >= 2 && args[0] == "log" {
				stdout.Write([]byte("Commit subject\n\nChange-Id: I0123456789abcdef0123456789abcdef01234567\n"))
			}
			return nil
		},
	}

	cfg := &Config{Git: mockGit}
	var outBuf, errBuf bytes.Buffer
	if err := EnsureChangeID(ctx, cfg, &outBuf, &errBuf); err != nil {
		t.Fatalf("EnsureChangeID() failed: %v", err)
	}

	// Should not invoke commit --amend because Change-Id is already present
	for _, call := range mockGit.Calls {
		if strings.Contains(call, "amend") {
			t.Errorf("Unexpected git amend call when Change-Id was already present: %s", call)
		}
	}
}

func TestExtractChangeID(t *testing.T) {
	tests := []struct {
		desc string
		msg  string
		want string
	}{
		{
			desc: "valid change ID in footer",
			msg:  "Commit subject\n\nCommit description.\n\nChange-Id: I0123456789abcdef0123456789abcdef01234567\n",
			want: "I0123456789abcdef0123456789abcdef01234567",
		},
		{
			desc: "missing change ID",
			msg:  "Commit subject\n\nCommit description.\n",
			want: "",
		},
		{
			desc: "malformed change ID",
			msg:  "Change-Id: I12345\n",
			want: "",
		},
	}
	for _, tt := range tests {
		t.Run(tt.desc, func(t *testing.T) {
			if got := ExtractChangeID(tt.msg); got != tt.want {
				t.Errorf("ExtractChangeID() = %q, want %q", got, tt.want)
			}
		})
	}
}

func TestEnsureChangeID_NilConfig(t *testing.T) {
	ctx := context.Background()
	var outBuf, errBuf bytes.Buffer
	err := EnsureChangeID(ctx, nil, &outBuf, &errBuf)
	if err == nil {
		t.Fatal("Expected error when cfg is nil, got nil")
	}
	if !strings.Contains(err.Error(), "git runner not initialized") {
		t.Errorf("Expected error to mention git runner not initialized, got: %v", err)
	}
}

func TestEnsureChangeID_NilGit(t *testing.T) {
	ctx := context.Background()
	var outBuf, errBuf bytes.Buffer
	err := EnsureChangeID(ctx, &Config{Git: nil}, &outBuf, &errBuf)
	if err == nil {
		t.Fatal("Expected error when cfg.Git is nil, got nil")
	}
	if !strings.Contains(err.Error(), "git runner not initialized") {
		t.Errorf("Expected error to mention git runner not initialized, got: %v", err)
	}
}

func TestEnsureChangeID_MissingChangeIDReturnsError(t *testing.T) {
	ctx := context.Background()
	tempDir := t.TempDir()
	mockGit := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) >= 2 && args[0] == "log" {
				stdout.Write([]byte("Commit subject without change ID\n\nBody.\n"))
			}
			if len(args) >= 2 && args[0] == "rev-parse" && args[1] == "--git-dir" {
				stdout.Write([]byte(tempDir + "\n"))
			}
			return nil
		},
	}
	cfg := &Config{Git: mockGit, Host: "invalid-review.invalid"}
	var outBuf, errBuf bytes.Buffer
	err := EnsureChangeID(ctx, cfg, &outBuf, &errBuf)
	if err == nil {
		t.Fatal("Expected error when Change-Id is missing and cannot be added, got nil")
	}
	if !strings.Contains(err.Error(), "missing required Gerrit Change-Id") {
		t.Errorf("Expected error about missing Change-Id, got: %v", err)
	}
}

func TestEnsureChangeID_BlockedWhenRemoteTip(t *testing.T) {
	ctx := context.Background()
	mockGit := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) >= 2 && args[0] == "log" {
				stdout.Write([]byte("Subject without change id\n"))
			}
			if len(args) >= 3 && args[0] == "for-each-ref" && args[1] == "--points-at=HEAD" {
				stdout.Write([]byte("refs/remotes/origin/main\n"))
			}
			return nil
		},
	}
	cfg := &Config{Git: mockGit, Host: "pigweed-review.googlesource.com"}
	var outBuf, errBuf bytes.Buffer
	err := EnsureChangeID(ctx, cfg, &outBuf, &errBuf)
	if err == nil {
		t.Fatal("Expected error when HEAD is a remote tip, got nil")
	}
	if !strings.Contains(err.Error(), "cannot amend HEAD commit") || !strings.Contains(err.Error(), "refs/remotes/origin/main") {
		t.Errorf("Expected error to explain remote tip amend guard, got: %v", err)
	}
}

func TestEnsureChangeID_AmendsWhenHookInstalled(t *testing.T) {
	ctx := context.Background()
	tempDir := t.TempDir()
	hooksDir := filepath.Join(tempDir, "hooks")
	if err := os.MkdirAll(hooksDir, 0755); err != nil {
		t.Fatalf("Failed to create hooks dir: %v", err)
	}
	hookPath := filepath.Join(hooksDir, "commit-msg")
	if err := os.WriteFile(hookPath, []byte("#!/bin/sh\nexit 0\n"), 0755); err != nil {
		t.Fatalf("Failed to write hook: %v", err)
	}

	amended := false
	mockGit := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) > 1 && args[0] == "commit" && args[1] == "--amend" {
				amended = true
			}
			if len(args) >= 2 && args[0] == "log" {
				if amended {
					stdout.Write([]byte("Commit subject\n\nChange-Id: I1234567890123456789012345678901234567890\n"))
				} else {
					stdout.Write([]byte("Commit subject without change ID\n"))
				}
			}
			if len(args) >= 2 && args[0] == "rev-parse" && args[1] == "--git-dir" {
				stdout.Write([]byte(tempDir + "\n"))
			}
			return nil
		},
	}

	cfg := &Config{Git: mockGit}
	var outBuf, errBuf bytes.Buffer
	err := EnsureChangeID(ctx, cfg, &outBuf, &errBuf)
	if err != nil {
		t.Fatalf("Expected EnsureChangeID to succeed after amend, got error: %v", err)
	}
	if !amended {
		t.Error("Expected git commit --amend to be invoked")
	}
}

func TestEnsureChangeID_DownloadsHookAndAmends(t *testing.T) {
	ctx := context.Background()
	tempDir := t.TempDir()

	hookDownloaded := false
	server := NewMockGerritServer(t)
	server.On("GET", "/tools/hooks/commit-msg", func(w http.ResponseWriter, r *http.Request) {
		hookDownloaded = true
		w.WriteHeader(http.StatusOK)
		w.Write([]byte("#!/bin/sh\nexit 0\n"))
	})

	amended := false
	mockGit := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) > 1 && args[0] == "commit" && args[1] == "--amend" {
				amended = true
			}
			if len(args) >= 2 && args[0] == "log" {
				if amended {
					stdout.Write([]byte("Commit subject\n\nChange-Id: I9999999999999999999999999999999999999999\n"))
				} else {
					stdout.Write([]byte("Commit subject without change ID\n"))
				}
			}
			if len(args) >= 2 && args[0] == "rev-parse" && args[1] == "--git-dir" {
				stdout.Write([]byte(tempDir + "\n"))
			}
			return nil
		},
	}

	cfg := &Config{Git: mockGit, Host: server.URL}
	var outBuf, errBuf bytes.Buffer
	err := EnsureChangeID(ctx, cfg, &outBuf, &errBuf)
	if err != nil {
		t.Fatalf("Expected EnsureChangeID to succeed with downloaded hook, got: %v", err)
	}

	if !hookDownloaded {
		t.Error("Expected hook to be downloaded from Gerrit server")
	}
	if !amended {
		t.Error("Expected commit --amend to be called after installing hook")
	}

	hookPath := filepath.Join(tempDir, "hooks", "commit-msg")
	info, err := os.Stat(hookPath)
	if err != nil {
		t.Fatalf("Expected hook file to exist at %s: %v", hookPath, err)
	}
	if info.Mode()&0111 == 0 {
		t.Errorf("Expected hook file to be executable, mode: %v", info.Mode())
	}
}

func TestCountCommitsAhead(t *testing.T) {
	ctx := context.Background()

	t.Run("single commit", func(t *testing.T) {
		mockGit := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				if len(args) >= 2 && args[0] == "rev-list" && args[1] == "--count" {
					stdout.Write([]byte("1\n"))
					return nil
				}
				return nil
			},
		}
		count, err := CountCommitsAhead(ctx, mockGit, "main")
		if err != nil {
			t.Fatalf("Unexpected error: %v", err)
		}
		if count != 1 {
			t.Errorf("got %d, want 1", count)
		}
	})

	t.Run("multiple commits (50)", func(t *testing.T) {
		mockGit := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				if len(args) >= 2 && args[0] == "rev-list" && args[1] == "--count" {
					stdout.Write([]byte("50\n"))
					return nil
				}
				return nil
			},
		}
		count, err := CountCommitsAhead(ctx, mockGit, "main")
		if err != nil {
			t.Fatalf("Unexpected error: %v", err)
		}
		if count != 50 {
			t.Errorf("got %d, want 50", count)
		}
	})

	t.Run("remote branch does not exist", func(t *testing.T) {
		mockGit := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				if len(args) >= 2 && args[0] == "rev-parse" && args[1] == "--verify" {
					return fmt.Errorf("fatal: Needed a single revision")
				}
				return nil
			},
		}
		count, err := CountCommitsAhead(ctx, mockGit, "nonexistent")
		if err == nil {
			t.Fatal("Expected error when remote branch does not exist, got nil")
		}
		if count != 0 {
			t.Errorf("got %d, want 0", count)
		}
	})

	t.Run("nil git runner", func(t *testing.T) {
		count, err := CountCommitsAhead(ctx, nil, "main")
		if err == nil {
			t.Fatal("Expected error with nil git runner, got nil")
		}
		if count != 0 {
			t.Errorf("got %d, want 0", count)
		}
	})

	t.Run("empty branch name", func(t *testing.T) {
		mockGit := &MockGitRunner{}
		_, err := CountCommitsAhead(ctx, mockGit, "")
		if err == nil {
			t.Fatal("Expected error with empty branch name, got nil")
		}
	})
}

func TestIsChangeIdentifier(t *testing.T) {
	tests := []struct {
		input string
		want  bool
	}{
		{"472267", true},
		{"472267/13", true},
		{"Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e", true},
		{"I1234567890abcdef1234567890abcdef12345678/2", true},
		{"https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/472267", true},
		{"https://pigweed-review.googlesource.com/+/472267/2", true},
		{"pwrev/472267", true},
		{"pwrev/472267/3", true},
		{"cl/472267", true},
		{"change-472267", true},
		{"my-feature", false},
		{"docs-builder-newpatchset", false},
		{"static-checks-pigweed", false},
		{"pigweed-linux-bazel", false},
		{"", false},
	}

	for _, tt := range tests {
		t.Run(tt.input, func(t *testing.T) {
			got := isChangeIdentifier(tt.input)
			if got != tt.want {
				t.Errorf("isChangeIdentifier(%q) = %v, want %v", tt.input, got, tt.want)
			}
		})
	}
}

func TestResolveActiveChangeID(t *testing.T) {
	ctx := context.Background()

	t.Run("HEAD has Change-Id on feature branch", func(t *testing.T) {
		cfg := &Config{
			Git: &MockGitRunner{
				RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
					if len(args) >= 2 && args[0] == "branch" && args[1] == "--show-current" {
						stdout.Write([]byte("my-feature\n"))
						return nil
					}
					if len(args) >= 3 && args[0] == "log" && args[1] == "-1" {
						stdout.Write([]byte("commit message\n\nChange-Id: Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e\n"))
						return nil
					}
					return nil
				},
			},
		}

		id, err := ResolveActiveChangeID(ctx, cfg)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if id != "Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e" {
			t.Errorf("got %q, want Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e", id)
		}
	})

	t.Run("branch config has gerrit-change-id", func(t *testing.T) {
		cfg := &Config{
			Git: &MockGitRunner{
				RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
					if len(args) >= 2 && args[0] == "branch" && args[1] == "--show-current" {
						stdout.Write([]byte("my-feature\n"))
						return nil
					}
					if len(args) >= 3 && args[0] == "config" && args[2] == "branch.my-feature.gerrit-change-id" {
						stdout.Write([]byte("472267\n"))
						return nil
					}
					return fmt.Errorf("not handled")
				},
			},
		}

		id, err := ResolveActiveChangeID(ctx, cfg)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if id != "472267" {
			t.Errorf("got %q, want 472267", id)
		}
	})

	t.Run("branch name encodes change number", func(t *testing.T) {
		for _, branchName := range []string{"cl/472267", "change-472267", "472267"} {
			cfg := &Config{
				Git: &MockGitRunner{
					RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
						if len(args) >= 2 && args[0] == "branch" && args[1] == "--show-current" {
							stdout.Write([]byte(branchName + "\n"))
							return nil
						}
						// No Change-Id in HEAD commit
						if len(args) >= 3 && args[0] == "log" && args[1] == "-1" {
							stdout.Write([]byte("WIP commit without footer\n"))
							return nil
						}
						return fmt.Errorf("not handled")
					},
				},
			}

			id, err := ResolveActiveChangeID(ctx, cfg)
			if err != nil {
				t.Fatalf("unexpected error for branch %s: %v", branchName, err)
			}
			if id != "472267" {
				t.Errorf("for branch %s got %q, want 472267", branchName, id)
			}
		}
	})

	t.Run("on main branch with 0 commits ahead", func(t *testing.T) {
		cfg := &Config{
			Git: &MockGitRunner{
				RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
					if len(args) >= 2 && args[0] == "branch" && args[1] == "--show-current" {
						stdout.Write([]byte("main\n"))
						return nil
					}
					if len(args) >= 2 && args[0] == "rev-list" && args[1] == "--count" {
						stdout.Write([]byte("0\n"))
						return nil
					}
					return nil
				},
			},
		}

		_, err := ResolveActiveChangeID(ctx, cfg)
		if err == nil {
			t.Fatal("expected error when on main with 0 commits ahead, got nil")
		}
		if !strings.Contains(err.Error(), "synced with origin") {
			t.Errorf("unexpected error message: %v", err)
		}
		if !strings.Contains(err.Error(), "gh pr list") {
			t.Errorf("expected error to suggest 'gh pr list', got: %v", err)
		}
		if !strings.Contains(err.Error(), "gh pr checkout") {
			t.Errorf("expected error to suggest 'gh pr checkout', got: %v", err)
		}
		if !strings.Contains(err.Error(), "gh pr create") {
			t.Errorf("expected error to suggest 'gh pr create', got: %v", err)
		}
	})

	t.Run("on main branch with 1 commit ahead", func(t *testing.T) {
		cfg := &Config{
			Git: &MockGitRunner{
				RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
					if len(args) >= 2 && args[0] == "branch" && args[1] == "--show-current" {
						stdout.Write([]byte("main\n"))
						return nil
					}
					if len(args) >= 2 && args[0] == "rev-list" && args[1] == "--count" {
						stdout.Write([]byte("1\n"))
						return nil
					}
					if len(args) >= 3 && args[0] == "log" && args[1] == "-1" {
						stdout.Write([]byte("local commit\n\nChange-Id: I1111222233334444555566667777888899990000\n"))
						return nil
					}
					return nil
				},
			},
		}

		id, err := ResolveActiveChangeID(ctx, cfg)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if id != "I1111222233334444555566667777888899990000" {
			t.Errorf("got %q, want I1111222233334444555566667777888899990000", id)
		}
	})

	t.Run("nil config or git", func(t *testing.T) {
		_, err := ResolveActiveChangeID(ctx, nil)
		if err == nil {
			t.Fatal("expected error for nil config, got nil")
		}
	})
}

func TestResolveTargetChangeID(t *testing.T) {
	ctx := context.Background()

	t.Run("direct change number arg", func(t *testing.T) {
		cmd := &cobra.Command{}
		id, err := ResolveTargetChangeID(ctx, cmd, []string{"472267"})
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if id != "472267" {
			t.Errorf("got %q, want 472267", id)
		}
	})

	t.Run("Gerrit URL arg", func(t *testing.T) {
		cmd := &cobra.Command{}
		rawURL := "https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/472267/3"
		id, err := ResolveTargetChangeID(ctx, cmd, []string{rawURL})
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if id != rawURL {
			t.Errorf("got %q, want %q", id, rawURL)
		}
	})

	t.Run("branch name with git config", func(t *testing.T) {
		cmd := mockCmdWithGit(ctx, &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				if len(args) >= 3 && args[0] == "config" && args[2] == "branch.my-feature.gerrit-change-id" {
					stdout.Write([]byte("472267\n"))
					return nil
				}
				return fmt.Errorf("not handled")
			},
		})

		id, err := ResolveTargetChangeID(ctx, cmd, []string{"my-feature"})
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if id != "472267" {
			t.Errorf("got %q, want 472267", id)
		}
	})

	t.Run("local branch with Change-Id in commit", func(t *testing.T) {
		cmd := mockCmdWithGit(ctx, &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				if len(args) >= 3 && args[0] == "rev-parse" && args[2] == "refs/heads/feature-x" {
					return nil
				}
				if len(args) >= 4 && args[0] == "log" && args[3] == "refs/heads/feature-x" {
					stdout.Write([]byte("Commit on feature-x\n\nChange-Id: I2222333344445555666677778888999900001111\n"))
					return nil
				}
				return fmt.Errorf("not handled")
			},
		})

		id, err := ResolveTargetChangeID(ctx, cmd, []string{"feature-x"})
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if id != "I2222333344445555666677778888999900001111" {
			t.Errorf("got %q, want I2222333344445555666677778888999900001111", id)
		}
	})

	t.Run("local branch without Change-Id returns error", func(t *testing.T) {
		cmd := mockCmdWithGit(ctx, &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				if len(args) >= 3 && args[0] == "rev-parse" && args[2] == "refs/heads/no-id-branch" {
					return nil
				}
				if len(args) >= 4 && args[0] == "log" {
					stdout.Write([]byte("Commit without change-id\n"))
					return nil
				}
				return fmt.Errorf("not handled")
			},
		})

		_, err := ResolveTargetChangeID(ctx, cmd, []string{"no-id-branch"})
		if err == nil {
			t.Fatal("expected error when branch has no Change-Id, got nil")
		}
		if !strings.Contains(err.Error(), "has no associated Gerrit Change-Id") {
			t.Errorf("unexpected error: %v", err)
		}
	})

	t.Run("no args defaults to active change", func(t *testing.T) {
		cmd := mockCmdWithGit(ctx, &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				if len(args) >= 2 && args[0] == "branch" && args[1] == "--show-current" {
					stdout.Write([]byte("active-branch\n"))
					return nil
				}
				if len(args) >= 3 && args[0] == "log" && args[1] == "-1" {
					stdout.Write([]byte("local commit\n\nChange-Id: I9999888877776666555544443333222211110000\n"))
					return nil
				}
				return fmt.Errorf("not handled")
			},
		})

		id, err := ResolveTargetChangeID(ctx, cmd, nil)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if id != "I9999888877776666555544443333222211110000" {
			t.Errorf("got %q, want I9999888877776666555544443333222211110000", id)
		}
	})
}

func TestExtractTrailers(t *testing.T) {
	const cid = "Change-Id: I1234567890abcdef1234567890abcdef12345678"

	tests := []struct {
		name string
		msg  string
		want []string
	}{
		{
			name: "canonical trailer block, bug value normalized",
			msg: "pw_foo: Fix issue\n\nLong explanation.\n\n" +
				"Bug: https://issues.pigweed.dev/issues/123456\nTest: bazelisk test //...\n" + cid + "\n",
			want: []string{
				"Bug: b/123456",
				"Test: bazelisk test //...",
				cid,
			},
		},
		{
			// The trailer block is not always last. A message ending in a
			// prose paragraph whose first word is capitalized-word-colon used
			// to hide the real trailer block entirely, destroying Change-Id.
			name: "trailing prose paragraph does not hide the real trailer block",
			msg: "pw_foo: Add bar\n\nSome body text.\n\nBug: b/12345\n" + cid + "\n\n" +
				"Note: reviewers please look at the retry logic.\n",
			want: []string{
				"Bug: b/12345",
				cid,
				"Note: reviewers please look at the retry logic.",
			},
		},
		{
			// Gerrit and `git cherry-pick -x` append this line inside the
			// trailer block. It used to disqualify the whole block and send
			// extraction down an allowlist that silently deleted everything
			// except Change-Id/Bug/Fixed/Reviewed-*/Tested-*/Signed-off-*.
			name: "cherry-pick footer does not disqualify the trailer block",
			msg: "pw_foo: Add bar\n\nBody.\n\nBug: b/12345\n" +
				"Co-authored-by: Alice <alice@example.com>\n" +
				"Cq-Include-Trybots: luci.pigweed.try:foo\n" + cid + "\n" +
				"(cherry picked from commit deadbeefdeadbeefdeadbeefdeadbeefdeadbeef)\n",
			want: []string{
				"Bug: b/12345",
				"Co-authored-by: Alice <alice@example.com>",
				"Cq-Include-Trybots: luci.pigweed.try:foo",
				cid,
				"(cherry picked from commit deadbeefdeadbeefdeadbeefdeadbeefdeadbeef)",
			},
		},
		{
			// The counterweight to the case above: a paragraph of ordinary
			// prose must not be promoted into trailers just because one of
			// its lines happens to contain a colon.
			name: "prose paragraph containing a colon line is not a trailer block",
			msg: "pw_foo: Add bar\n\n" +
				"Note: this paragraph explains something\nand continues on this line.\n\n" +
				"Bug: b/12345\n" + cid + "\n",
			want: []string{"Bug: b/12345", cid},
		},
		{
			name: "free-text bug value is preserved verbatim",
			msg:  "pw_foo: Add bar\n\nBody.\n\nBug: none, see the design doc\n" + cid + "\n",
			want: []string{"Bug: none, see the design doc", cid},
		},
		{
			name: "multi-line trailer continuation is kept with its trailer",
			msg:  "pw_foo: Add bar\n\nBody.\n\nBug: b/1\nTest: ran locally\n  with extra detail\n" + cid + "\n",
			want: []string{
				"Bug: b/1",
				"Test: ran locally\n  with extra detail",
				cid,
			},
		},
		{
			name: "duplicate keys are all preserved",
			msg:  "pw_foo: Add bar\n\nBody.\n\nBug: b/1\nBug: b/2\n" + cid + "\n",
			want: []string{"Bug: b/1", "Bug: b/2", cid},
		},
		{
			name: "CRLF line endings",
			msg:  "pw_foo: Add bar\r\n\r\nBody.\r\n\r\nBug: b/12345\r\n" + cid + "\r\n",
			want: []string{"Bug: b/12345", cid},
		},
		{
			name: "subject only has no trailers",
			msg:  "pw_foo: Add bar\n",
			want: nil,
		},
		{
			// A subject line is never a trailer, even though "pw_foo: Add bar"
			// matches the trailer shape exactly.
			name: "subject that looks like a trailer is not extracted",
			msg:  "pw_foo: Add bar\n\nJust a body with no trailers.\n",
			want: nil,
		},
		{
			name: "empty message",
			msg:  "",
			want: nil,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := ExtractTrailers(tt.msg)
			if !reflect.DeepEqual(got, tt.want) {
				t.Errorf("ExtractTrailers()\n got: %#v\nwant: %#v", got, tt.want)
			}
		})
	}
}

// TestExtractTrailers_NeverLosesATrailerKey is the invariant that matters more
// than any individual case: whatever shape the message takes, a key that was
// in the original must still be there afterwards. Losing Change-Id means the
// next push creates a duplicate CL instead of a new patchset.
func TestExtractTrailers_NeverLosesATrailerKey(t *testing.T) {
	const cid = "Change-Id: I1234567890abcdef1234567890abcdef12345678"

	messages := []string{
		"s: t\n\nBody.\n\nBug: b/1\n" + cid + "\n",
		"s: t\n\nBody.\n\nBug: b/1\n" + cid + "\n\nNote: trailing prose.\n",
		"s: t\n\nBody.\n\nBug: b/1\nCo-authored-by: A <a@e.com>\n" + cid + "\n(cherry picked from commit abc)\n",
		"s: t\n\nBug: b/1\n" + cid + "\n",
		"s: t\n\nBody.\n\nFixed: b/2\nCq-Depend: chromium:123\n" + cid + "\n",
		"s: t\r\n\r\nBody.\r\n\r\nBug: b/1\r\n" + cid + "\r\n",
	}

	// Keys that carry meaning Gerrit or Buganizer acts on. Losing any of these
	// silently changes what the change does.
	criticalKeys := []string{"Change-Id", "Bug", "Fixed", "Co-authored-by", "Cq-Depend"}

	for _, msg := range messages {
		t.Run(strings.SplitN(msg, "\n", 2)[0]+"...", func(t *testing.T) {
			got := strings.Join(ExtractTrailers(msg), "\n")
			for _, key := range criticalKeys {
				if !strings.Contains(msg, key+":") {
					continue
				}
				if !strings.Contains(got, key+":") {
					t.Errorf("trailer key %q present in the original message was dropped.\nMessage:\n%s\nExtracted:\n%s",
						key, msg, got)
				}
			}
		})
	}
}

func TestDroppedTrailers(t *testing.T) {
	const cid = "Change-Id: I1234567890abcdef1234567890abcdef12345678"
	const provenance = "(cherry picked from commit deadbeef)"

	orig := "pw_foo: Subject\n\nBody.\n\nBug: b/1\nCo-authored-by: A <a@e.com>\n" + cid + "\n" + provenance + "\n"

	tests := []struct {
		name string
		// origMsg defaults to orig when empty.
		origMsg string
		newMsg  string
		want    []string
	}{
		{
			name:   "identical message drops nothing",
			newMsg: orig,
			want:   nil,
		},
		{
			name:   "bare replacement drops everything",
			newMsg: "pw_foo: Subject\n\nNew body.\n",
			want:   []string{"Bug: b/1", "Co-authored-by: A <a@e.com>", cid, provenance},
		},
		{
			// Editing a value is not deleting a trailer. If this reported
			// loss, the guard could never be satisfied by changing a bug
			// number, which is one of the most common reasons to use -m.
			name:   "rewritten value counts as carried forward",
			newMsg: "pw_foo: Subject\n\nNew body.\n\nBug: b/2\nCo-authored-by: A <a@e.com>\n" + cid + "\n" + provenance + "\n",
			want:   nil,
		},
		{
			name:   "key match is case insensitive",
			newMsg: "pw_foo: Subject\n\nNew body.\n\nbug: b/1\nco-authored-by: A <a@e.com>\n" + cid + "\n" + provenance + "\n",
			want:   nil,
		},
		{
			// The provenance footer has no key, so only a whole-line match
			// saves it.
			name:   "keyless footer must match in full",
			newMsg: "pw_foo: Subject\n\nNew body.\n\nBug: b/1\nCo-authored-by: A <a@e.com>\n" + cid + "\n(cherry picked from commit cafef00d)\n",
			want:   []string{provenance},
		},
		{
			name:    "original with no trailers drops nothing",
			origMsg: "pw_foo: Subject\n\nJust prose.\n",
			newMsg:  "pw_foo: Subject\n\nNew body.\n",
			want:    nil,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			from := tt.origMsg
			if from == "" {
				from = orig
			}
			got := DroppedTrailers(from, tt.newMsg)
			if !reflect.DeepEqual(got, tt.want) {
				t.Errorf("DroppedTrailers()\n got: %#v\nwant: %#v", got, tt.want)
			}
		})
	}
}

func TestMergeTrailers(t *testing.T) {
	origTrailers := []string{
		"Bug: https://issues.pigweed.dev/issues/123456",
		"Change-Id: I1234567890abcdef1234567890abcdef12345678",
	}

	t.Run("appends missing trailers to empty body", func(t *testing.T) {
		got := MergeTrailers("", origTrailers)
		want := "Bug: b/123456\nChange-Id: I1234567890abcdef1234567890abcdef12345678\n"
		if got != want {
			t.Errorf("MergeTrailers got %q, want %q", got, want)
		}
	})

	t.Run("preserves existing body and normalizes bug in new body", func(t *testing.T) {
		body := "New body description.\n\nBug: 987654"
		got := MergeTrailers(body, origTrailers)
		// Since Bug is already in body, it should keep the new bug (normalized) and append Change-Id
		if !strings.Contains(got, "Bug: b/987654") {
			t.Errorf("expected Bug: b/987654 in merged body, got: %q", got)
		}
		if !strings.Contains(got, "Change-Id: I1234567890abcdef1234567890abcdef12345678") {
			t.Errorf("expected Change-Id preserved, got: %q", got)
		}
	})

	t.Run("does not reindent an indented code sample in the body", func(t *testing.T) {
		// Pigweed's style guide indents code samples by two spaces, and an
		// indented `key: value` line is content, not a trailer. Stripping that
		// indentation silently rewrites the user's body.
		body := "Explain the thing.\n\nExample config:\n\n  key: value\n  other: thing\n"
		got := MergeTrailers(body, origTrailers)

		for _, want := range []string{"  key: value", "  other: thing"} {
			if !strings.Contains(got, want) {
				t.Errorf("indentation of %q was not preserved, got:\n%s", want, got)
			}
		}
	})

	t.Run("keeps a keyless footer line such as the cherry-pick provenance", func(t *testing.T) {
		const provenance = "(cherry picked from commit deadbeef)"
		got := MergeTrailers("New body.", []string{
			"Change-Id: I1234567890abcdef1234567890abcdef12345678",
			provenance,
		})
		if !strings.Contains(got, provenance) {
			t.Errorf("cherry-pick provenance line was dropped, got:\n%s", got)
		}
	})

	t.Run("does not duplicate a footer line already present in the body", func(t *testing.T) {
		const provenance = "(cherry picked from commit deadbeef)"
		got := MergeTrailers("New body.\n\n"+provenance, []string{provenance})
		if n := strings.Count(got, provenance); n != 1 {
			t.Errorf("expected the footer line exactly once, got %d:\n%s", n, got)
		}
	})
}

func TestUpsertTrailer(t *testing.T) {
	const cid = "Change-Id: I1234567890abcdef1234567890abcdef12345678"

	tests := []struct {
		name    string
		msg     string
		trailer string
		want    string
	}{
		{
			name:    "replaces an existing trailer in place",
			msg:     "s: t\n\nBody.\n\nBug: b/1\n" + cid + "\n",
			trailer: "Bug: b/2",
			want:    "s: t\n\nBody.\n\nBug: b/2\n" + cid + "\n",
		},
		{
			name:    "appends to the existing trailer block when the key is absent",
			msg:     "s: t\n\nBody.\n\n" + cid + "\n",
			trailer: "Bug: b/2",
			want:    "s: t\n\nBody.\n\n" + cid + "\nBug: b/2\n",
		},
		{
			name:    "starts a trailer block when the message has none",
			msg:     "s: t\n\nJust prose, no trailers.\n",
			trailer: "Bug: b/2",
			want:    "s: t\n\nJust prose, no trailers.\n\nBug: b/2\n",
		},
		{
			name:    "starts a trailer block on a subject-only message",
			msg:     "s: t\n",
			trailer: "Bug: b/2",
			want:    "s: t\n\nBug: b/2\n",
		},
		{
			// Two Bug: lines and one --bug flag is unambiguous: the flag says
			// what the bug is now. Leaving a stale duplicate behind would link
			// a bug the user just replaced.
			name:    "collapses duplicate keys to a single trailer",
			msg:     "s: t\n\nBody.\n\nBug: b/1\nBug: b/2\n" + cid + "\n",
			trailer: "Bug: b/3",
			want:    "s: t\n\nBody.\n\nBug: b/3\n" + cid + "\n",
		},
		{
			name:    "matches the key case-insensitively",
			msg:     "s: t\n\nBody.\n\nbug: b/1\n" + cid + "\n",
			trailer: "Bug: b/2",
			want:    "s: t\n\nBody.\n\nBug: b/2\n" + cid + "\n",
		},
		{
			// The continuation belongs to the trailer being replaced, so it
			// must go with it. Leaving it behind would strand a fragment of
			// the old value under the new one.
			name:    "drops the continuation lines of the replaced trailer",
			msg:     "s: t\n\nBody.\n\nBug: b/1\n  and more detail\n" + cid + "\n",
			trailer: "Bug: b/2",
			want:    "s: t\n\nBody.\n\nBug: b/2\n" + cid + "\n",
		},
		{
			// A prose paragraph is not a trailer block, so a line inside it
			// that happens to start with the key is body text. Rewriting it
			// would silently edit the author's prose.
			name:    "leaves a lookalike line in a prose paragraph alone",
			msg:     "s: t\n\nBug: this paragraph is prose\nbecause it has a second line.\n\n" + cid + "\n",
			trailer: "Bug: b/2",
			want:    "s: t\n\nBug: this paragraph is prose\nbecause it has a second line.\n\n" + cid + "\nBug: b/2\n",
		},
		{
			name:    "other trailers are untouched",
			msg:     "s: t\n\nBody.\n\nCo-authored-by: A <a@e.com>\nBug: b/1\nTest: none\n",
			trailer: "Bug: b/2",
			want:    "s: t\n\nBody.\n\nCo-authored-by: A <a@e.com>\nBug: b/2\nTest: none\n",
		},
		{
			name:    "CRLF input is normalized",
			msg:     "s: t\r\n\r\nBody.\r\n\r\nBug: b/1\r\n",
			trailer: "Bug: b/2",
			want:    "s: t\n\nBody.\n\nBug: b/2\n",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got, err := UpsertTrailer(tt.msg, tt.trailer)
			if err != nil {
				t.Fatalf("UpsertTrailer returned an error: %v", err)
			}
			if got != tt.want {
				t.Errorf("UpsertTrailer()\n got: %q\nwant: %q", got, tt.want)
			}
		})
	}
}

// TestUpsertTrailer_RejectsMalformedTrailer covers the invariant violation.
// A caller that passes something which is not a `Key: value` line has a bug,
// and returning the message unchanged would hide it.
func TestUpsertTrailer_RejectsMalformedTrailer(t *testing.T) {
	if _, err := UpsertTrailer("s: t\n", "not a trailer"); err == nil {
		t.Error("Expected an error for a malformed trailer line, got nil")
	}
}

// TestUpsertTrailer_PreservesOtherTrailers is the counterpart to the
// ExtractTrailers invariant: setting one trailer must never cost another.
func TestUpsertTrailer_PreservesOtherTrailers(t *testing.T) {
	const provenance = "(cherry picked from commit deadbeef)"
	msg := "s: t\n\nBody.\n\nBug: b/1\nCo-authored-by: A <a@e.com>\n" +
		"Cq-Include-Trybots: luci.pigweed.try:foo\n" +
		"Change-Id: I1234567890abcdef1234567890abcdef12345678\n" + provenance + "\n"

	got, err := UpsertTrailer(msg, "Fixed: b/99")
	if err != nil {
		t.Fatalf("UpsertTrailer returned an error: %v", err)
	}
	for _, want := range []string{
		"Bug: b/1",
		"Co-authored-by: A <a@e.com>",
		"Cq-Include-Trybots: luci.pigweed.try:foo",
		"Change-Id: I1234567890abcdef1234567890abcdef12345678",
		provenance,
		"Fixed: b/99",
	} {
		if !strings.Contains(got, want) {
			t.Errorf("DATA LOSS: %q missing after upsert:\n%s", want, got)
		}
	}
}

func TestNormalizeCQArgs(t *testing.T) {
	tests := []struct {
		name string
		in   []string
		want []string
	}{
		{
			name: "rewrites --cq 1 to --cq=1",
			in:   []string{"pr", "push", "--cq", "1"},
			want: []string{"pr", "push", "--cq=1"},
		},
		{
			name: "rewrites --cq 2 to --cq=2",
			in:   []string{"pr", "push", "--cq", "2"},
			want: []string{"pr", "push", "--cq=2"},
		},
		{
			name: "rewrites --cq 0 to --cq=0",
			in:   []string{"pr", "edit", "--cq", "0"},
			want: []string{"pr", "edit", "--cq=0"},
		},
		{
			// -q was unbound because it is gh's --jq. Leaving it alone means
			// cobra reports an unknown shorthand instead of a confusing
			// "invalid argument" against a rewritten -q=1.
			name: "leaves -q alone now that the shorthand is unbound",
			in:   []string{"pr", "push", "-q", "1"},
			want: []string{"pr", "push", "-q", "1"},
		},
		{
			name: "leaves standalone --cq untouched",
			in:   []string{"pr", "push", "--cq"},
			want: []string{"pr", "push", "--cq"},
		},
		{
			name: "leaves --cq followed by another flag untouched",
			in:   []string{"pr", "push", "--cq", "--reviewer", "a@b.com"},
			want: []string{"pr", "push", "--cq", "--reviewer", "a@b.com"},
		},
		{
			name: "leaves --cq followed by non-number untouched",
			in:   []string{"pr", "push", "--cq", "my-branch"},
			want: []string{"pr", "push", "--cq", "my-branch"},
		},
		{
			name: "leaves --cq=1 untouched",
			in:   []string{"pr", "push", "--cq=1"},
			want: []string{"pr", "push", "--cq=1"},
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got := NormalizeCQArgs(tc.in)
			if !reflect.DeepEqual(got, tc.want) {
				t.Errorf("NormalizeCQArgs(%v) = %v, want %v", tc.in, got, tc.want)
			}
		})
	}
}

func TestFindChangeIDInCommitRange(t *testing.T) {
	ctx := context.Background()

	t.Run("finds first Change-Id in multi-commit range", func(t *testing.T) {
		mockGit := NewMockGit(t).
			OnCommand("log -10 --format=%B origin/main..HEAD", "Fix doc typo\n\nChange-Id: I1111111111111111111111111111111111111111\n\nEarlier commit\n\nChange-Id: I2222222222222222222222222222222222222222")
		got := findChangeIDInCommitRange(ctx, NewGitClient(mockGit), "origin/main..HEAD")
		if got != "I1111111111111111111111111111111111111111" {
			t.Errorf("got %q, want I1111111111111111111111111111111111111111", got)
		}
	})

	t.Run("returns empty when no Change-Id in range", func(t *testing.T) {
		mockGit := NewMockGit(t).
			OnCommand("log -10 --format=%B origin/main..refs/heads/feat", "Regular commit without change id\n\nAnother commit")
		got := findChangeIDInCommitRange(ctx, NewGitClient(mockGit), "origin/main..refs/heads/feat")
		if got != "" {
			t.Errorf("got %q, want empty string", got)
		}
	})

	t.Run("returns empty when git log fails", func(t *testing.T) {
		mockGit := NewMockGit(t).
			OnError("log -10 --format=%B origin/main..HEAD", fmt.Errorf("git error"))
		got := findChangeIDInCommitRange(ctx, NewGitClient(mockGit), "origin/main..HEAD")
		if got != "" {
			t.Errorf("got %q, want empty string", got)
		}
	})
}
