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
	"context"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestDefaultBranch(t *testing.T) {
	ctx := context.Background()

	t.Run("main branch exists", func(t *testing.T) {
		cfg := &Config{
			Git: &MockGitRunner{
				RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
					return nil // Simulate 'git rev-parse --verify refs/heads/main' success
				},
			},
		}
		if got := defaultBranch(ctx, cfg, io.Discard); got != "main" {
			t.Errorf("defaultBranch() = %q, want \"main\"", got)
		}
	})

	t.Run("main branch does not exist falls back to main", func(t *testing.T) {
		cfg := &Config{
			Git: &MockGitRunner{
				RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
					return io.EOF // Simulate command failure
				},
			},
		}
		if got := defaultBranch(ctx, cfg, io.Discard); got != "main" {
			t.Errorf("defaultBranch() = %q, want \"main\"", got)
		}
	})
}

func TestCreateIntegration(t *testing.T) {
	mockGit := NewMockGit(t).WithBranch("feature-branch")

	output, err := executeCommand(RootCmd, "pr", "create")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	wantOutput := "Creating change for branch feature-branch..."
	if !strings.Contains(output, wantOutput) {
		t.Errorf("pr create output got = %q, want it to contain = %q", output, wantOutput)
	}

	if len(mockGit.Calls) < 2 {
		t.Fatalf("Expected at least 2 git calls, got = %v", mockGit.Calls)
	}

	wantPush := "push origin HEAD:refs/for/feature-branch"
	if !strings.Contains(mockGit.Calls[len(mockGit.Calls)-1], wantPush) {
		t.Errorf("last git call got = %q, want to contain = %q", mockGit.Calls[len(mockGit.Calls)-1], wantPush)
	}
}

func TestCreateStandardWorkspaceWithTitleAndBody(t *testing.T) {
	mockGit := NewMockGit(t).WithBranch("main")

	output, err := executeCommand(RootCmd, "pr", "create", "--title", "Standard Title", "--body", "Standard Body")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if !mockGit.HasCall("commit -a -m Standard Title\n\nStandard Body") {
		t.Errorf("Git calls got = %v, want to contain a commit call with standard title/body elements", mockGit.Calls)
	}
}

func TestCreate_RichPushOptions_Pigweed(t *testing.T) {
	mockGit := NewMockGit(t).WithBranch("main")
	SetTestProfile(t, "pigweed")

	_, err := executeCommand(RootCmd, "pr", "create",
		"--reviewer", "alice@google.com",
		"--cc", "bob@google.com",
		"--draft",
		"--auto",
		"--cq", "1",
		"--publish",
		"-o", "topic=my-test-topic",
		"--no-verify",
	)
	if err != nil {
		t.Fatalf("Command failed: %v", err)
	}

	var pushCall string
	for _, call := range mockGit.Calls {
		if strings.HasPrefix(call, "push") {
			pushCall = call
			break
		}
	}

	if pushCall == "" {
		t.Fatalf("Expected git push to be called, got calls: %v", mockGit.Calls)
	}

	if !strings.Contains(pushCall, "--no-verify") {
		t.Errorf("Expected push to contain --no-verify, got %q", pushCall)
	}

	expectedRefComponents := []string{
		"r=alice@google.com",
		"cc=bob@google.com",
		"wip",
		"l=Pigweed-Auto-Submit+1",
		"l=Commit-Queue+1",
		"publish-comments",
		"topic=my-test-topic",
	}
	for _, expected := range expectedRefComponents {
		if !strings.Contains(pushCall, expected) {
			t.Errorf("Push call missing expected component %q: %s", expected, pushCall)
		}
	}
}

func TestCreate_AutoSubmit_Fuchsia(t *testing.T) {
	mockGit := NewMockGit(t).WithBranch("main")
	SetTestProfile(t, "fuchsia")

	_, err := executeCommand(RootCmd, "pr", "create", "--auto")
	if err != nil {
		t.Fatalf("Command failed: %v", err)
	}

	var pushCall string
	for _, call := range mockGit.Calls {
		if strings.HasPrefix(call, "push") {
			pushCall = call
			break
		}
	}

	if !strings.Contains(pushCall, "l=Commit-Queue+2") {
		t.Errorf("Expected Fuchsia auto-submit push ref to contain Commit-Queue+2, got %q", pushCall)
	}
}

func TestCreate_ChangeID_AutoRepair(t *testing.T) {
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
	mockGit := NewMockGit(t).WithBranch("main").WithCommit("")
	mockGit.RunFn = func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
		if len(args) > 1 && args[0] == "commit" && args[1] == "--amend" {
			amended = true
		}
		if len(args) >= 2 && args[0] == "log" {
			if amended {
				stdout.Write([]byte("Commit message\n\nChange-Id: I1234567890123456789012345678901234567890\n"))
			} else {
				stdout.Write([]byte("Commit message without change ID\n"))
			}
		}
		if len(args) >= 2 && args[0] == "rev-parse" && args[1] == "--git-dir" {
			stdout.Write([]byte(tempDir + "\n"))
		}
		return nil
	}

	_, err := executeCommand(RootCmd, "pr", "create")
	if err != nil {
		t.Fatalf("Command failed: %v", err)
	}

	if !mockGit.HasCall("commit --amend --no-edit") {
		t.Errorf("Expected git commit --amend --no-edit to be called, calls: %v", mockGit.Calls)
	}
}

func TestCreate_FailsWhenMissingChangeID(t *testing.T) {
	tempDir := t.TempDir()
	NewMockGit(t).WithBranch("main").WithCommit("Commit message without change ID\n").OnCommand("rev-parse --git-dir", tempDir+"\n")

	_, err := executeCommand(RootCmd, "pr", "create")
	if err == nil {
		t.Fatal("Expected error when Change-Id is missing, got nil")
	}
	if !strings.Contains(err.Error(), "Change-Id") {
		t.Errorf("Expected error to mention Change-Id, got: %v", err)
	}
}

func TestCreate_ExistingChangeError(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/*", http.StatusOK, []map[string]any{
		{"_number": 472267, "project": "pigweed/pigweed", "status": "NEW"},
	})

	mockGit := NewMockGit(t).WithBranch("main").WithCommit("Subject\n\nChange-Id: I1234567890123456789012345678901234567890\n")

	output, err := executeCommand(RootCmd, "pr", "create")
	if err == nil {
		t.Fatal("Expected error when change already exists, got nil")
	}

	combined := output + " " + err.Error()
	if !strings.Contains(combined, "already exists") {
		t.Errorf("Expected output/error to contain 'already exists', got: %s", combined)
	}
	if !strings.Contains(combined, "gh pr push") {
		t.Errorf("Expected output/error to suggest 'gh pr push', got: %s", combined)
	}

	// Verify that git push was NOT called
	for _, call := range mockGit.Calls {
		if strings.HasPrefix(call, "push") {
			t.Errorf("git push should not be called when change already exists, got call: %s", call)
		}
	}
}

func TestCreate_ErrorWhenCommitFails(t *testing.T) {
	NewMockGit(t).WithBranch("main").OnError("commit", fmt.Errorf("pre-commit hook failed"))

	_, err := executeCommand(RootCmd, "pr", "create", "-t", "My Title")
	if err == nil {
		t.Fatal("Expected error when git commit fails, got nil")
	}
	if !strings.Contains(err.Error(), "error creating commit") {
		t.Errorf("Expected error to mention 'error creating commit', got: %v", err)
	}
}

func TestCreate_ErrorWhenPushFails(t *testing.T) {
	NewMockGit(t).WithBranch("main").OnError("push", fmt.Errorf("remote rejected"))

	_, err := executeCommand(RootCmd, "pr", "create")
	if err == nil {
		t.Fatal("Expected error when git push fails, got nil")
	}
	if !strings.Contains(err.Error(), "error creating change") {
		t.Errorf("Expected error to mention 'error creating change', got: %v", err)
	}
}

func TestCreate_ExistingChangeWithForce(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/*", http.StatusOK, []map[string]any{
		{"_number": 472267, "project": "pigweed/pigweed", "status": "NEW"},
	})

	mockGit := NewMockGit(t).WithBranch("main").WithCommit("Subject\n\nChange-Id: I1234567890123456789012345678901234567890\n")

	output, err := executeCommand(RootCmd, "pr", "create", "--force")
	if err != nil {
		t.Fatalf("Command returned unexpected error: %v", err)
	}

	if !mockGit.HasCall("push") {
		t.Errorf("Expected git push to be called with --force, calls: %v", mockGit.Calls)
	}
	if !strings.Contains(output, "Change created successfully.") {
		t.Errorf("Expected output to contain 'Change created successfully.', got: %s", output)
	}
}

func TestCreate_MultiCommitStackGuard_RejectsWithoutStack(t *testing.T) {
	NewMockGit(t).WithBranch("main").OnCommand("rev-list --count origin/main..HEAD", "50\n")

	_, err := executeCommand(RootCmd, "pr", "create", "--base", "main")
	if err == nil {
		t.Fatal("Expected error when pushing 50 commits without --stack, got nil")
	}

	if !strings.Contains(err.Error(), "would create 50 separate Gerrit changes") {
		t.Errorf("Expected error to mention 50 separate changes, got: %v", err)
	}
	if !strings.Contains(err.Error(), "--stack") {
		t.Errorf("Expected error to mention --stack flag, got: %v", err)
	}
}

func TestCreate_MultiCommitStackGuard_AllowsWithStack(t *testing.T) {
	NewMockGit(t).WithBranch("main").OnCommand("rev-list --count origin/main..HEAD", "50\n")

	output, err := executeCommand(RootCmd, "pr", "create", "--base", "main", "--stack")
	if err != nil {
		t.Fatalf("Expected --stack to succeed, got: %v", err)
	}

	if !strings.Contains(output, "Change created successfully.") {
		t.Errorf("Expected output to contain 'Change created successfully.', got: %s", output)
	}
}

func TestResolvePushBranch_LocalBranchNotOnRemoteFallsBackToMain(t *testing.T) {
	ctx := context.Background()
	cfg := &Config{
		Git: &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				if len(args) >= 2 && args[0] == "rev-parse" && args[1] == "--abbrev-ref" {
					return fmt.Errorf("fatal: no upstream configured")
				}
				if len(args) >= 2 && args[0] == "branch" && args[1] == "--show-current" {
					stdout.Write([]byte("local-feature\n"))
					return nil
				}
				if len(args) >= 3 && args[0] == "rev-parse" && args[1] == "--verify" && args[2] == "origin/local-feature" {
					return fmt.Errorf("fatal: Needed a single revision")
				}
				if len(args) >= 3 && args[0] == "rev-parse" && args[1] == "--verify" && args[2] == "refs/heads/main" {
					return nil
				}
				return nil
			},
		},
	}

	branch := resolvePushBranch(ctx, cfg, "", io.Discard)
	if branch != "main" {
		t.Errorf("resolvePushBranch() = %q, want \"main\"", branch)
	}
}

// TestCreate_RejectsGitHubIssueSyntaxFromFlags catches the reference before
// any commit is made, so the working tree is left exactly as it was found.
func TestCreate_RejectsGitHubIssueSyntaxFromFlags(t *testing.T) {
	mockGit := NewMockGit(t).WithBranch("main").WithCommit("pw_foo: S\n\nBody.\n")

	_, err := executeCommand(RootCmd, "pr", "create",
		"--title", "pw_foo: Rework", "--body", "Rework the loop.\n\nFixes #456")
	if err == nil {
		t.Fatal("Expected GitHub issue syntax to be rejected, got nil")
	}
	if !strings.Contains(err.Error(), "#456") {
		t.Errorf("Error should quote the reference, got: %v", err)
	}
	if mockGit.HasCall("commit") {
		t.Errorf("A commit was created despite the rejection: %v", mockGit.Calls)
	}
	if mockGit.HasCall("push") {
		t.Errorf("A push happened despite the rejection: %v", mockGit.Calls)
	}
}

// TestCreate_RejectsGitHubIssueSyntaxInHeadCommit covers the message an agent
// wrote with plain `git commit`, which never passes through a gh-ish flag.
// This is the common case, so checking only the flags would miss most of it.
func TestCreate_RejectsGitHubIssueSyntaxInHeadCommit(t *testing.T) {
	mockGit := NewMockGit(t).WithBranch("main").WithCommit(
		"pw_foo: Rework\n\nRework the loop.\n\nCloses #456\n" +
			"Change-Id: I0123456789abcdef0123456789abcdef01234567\n")

	_, err := executeCommand(RootCmd, "pr", "create")
	if err == nil {
		t.Fatal("Expected GitHub issue syntax in HEAD to be rejected, got nil")
	}
	if !strings.Contains(err.Error(), "#456") {
		t.Errorf("Error should quote the reference, got: %v", err)
	}
	if !strings.Contains(err.Error(), "Fixed: b/") {
		t.Errorf("Error should suggest the Gerrit trailer, got: %v", err)
	}
	if mockGit.HasCall("push") {
		t.Errorf("A push happened despite the rejection: %v", mockGit.Calls)
	}
}

// TestCreate_AcceptsGerritBugTrailerInHeadCommit is the false-positive guard.
func TestCreate_AcceptsGerritBugTrailerInHeadCommit(t *testing.T) {
	mockGit := NewMockGit(t).WithBranch("main").WithCommit(
		"pw_foo: Rework\n\nRework the loop.\n\nFixed: b/456\n" +
			"Change-Id: I0123456789abcdef0123456789abcdef01234567\n")

	if _, err := executeCommand(RootCmd, "pr", "create", "--force"); err != nil {
		t.Fatalf("A correct Gerrit trailer blocked the push: %v", err)
	}
	if !mockGit.HasCall("push") {
		t.Errorf("Expected a push, got calls: %v", mockGit.Calls)
	}
}

func TestPush_RejectsGitHubIssueSyntaxInHeadCommit(t *testing.T) {
	mockGit := NewMockGit(t).WithBranch("main").WithCommit(
		"pw_foo: Rework\n\nRework the loop.\n\nResolves #456\n" +
			"Change-Id: I0123456789abcdef0123456789abcdef01234567\n")

	_, err := executeCommand(RootCmd, "pr", "push")
	if err == nil {
		t.Fatal("Expected GitHub issue syntax in HEAD to be rejected, got nil")
	}
	if !strings.Contains(err.Error(), "#456") {
		t.Errorf("Error should quote the reference, got: %v", err)
	}
	if mockGit.HasCall("push") {
		t.Errorf("A push happened despite the rejection: %v", mockGit.Calls)
	}
}
