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
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
	"time"

	"github.com/google/go-cmp/cmp"
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
	mockGit := NewMockGit(t).WithBranch("feature-branch").
		OnCommand("push origin HEAD:refs/for/feature-branch", "remote:   https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/396894 pw_ghish: Add UI [NEW]\n")

	output, err := executeCommand(RootCmd, "pr", "create")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	wantOutput := "Creating change for branch feature-branch..."
	if !strings.Contains(output, wantOutput) {
		t.Errorf("pr create output got = %q, want it to contain = %q", output, wantOutput)
	}
	if !strings.Contains(output, "✓ Change created successfully.") {
		t.Errorf("pr create output got = %q, want receipt", output)
	}
	if !strings.Contains(output, "Branch: feature-branch") {
		t.Errorf("pr create output got = %q, want Branch detail", output)
	}
	if !strings.Contains(output, "Next:   gh pr checks --watch") {
		t.Errorf("pr create output got = %q, want Next hint", output)
	}
	if !strings.Contains(output, "https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/396894") {
		t.Errorf("pr create output got = %q, want extracted Gerrit URL", output)
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
	cases := []struct {
		name           string
		existingCommit string
		wantCallPrefix string
		wantMsg        string
	}{
		{
			name:           "creates commit from dirty workspace",
			wantCallPrefix: "commit -a -F ",
			wantMsg:        "Standard Title\n\nStandard Body\n",
		},
		{
			name:           "amends unpushed commit preserving Change-Id",
			existingCommit: "Draft\n\nChange-Id: I0000000000000000000000000000000000000001\n",
			wantCallPrefix: "commit --amend --only --allow-empty -F ",
			wantMsg:        "Standard Title\n\nStandard Body\n\nChange-Id: I0000000000000000000000000000000000000001\n",
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			mockGit := NewMockGit(t).WithBranch("main")
			if tc.existingCommit != "" {
				mockGit.WithCommit(tc.existingCommit).
					OnCommand("rev-parse HEAD", "deadbeef\n").
					OnCommand("rev-list --count origin/main..HEAD", "1\n")
			}
			if out, err := executeCommand(RootCmd, "pr", "create", "--title", "Standard Title", "--body", "Standard Body"); err != nil {
				t.Fatalf("pr create failed: %v\nOutput: %s", err, out)
			}
			if !mockGit.HasCall(tc.wantCallPrefix) {
				t.Errorf("Git calls = %v, want prefix %q", mockGit.Calls, tc.wantCallPrefix)
			}
			if got := mockGit.LastCommitMessage(); got != tc.wantMsg {
				t.Errorf("committed message = %q, want %q", got, tc.wantMsg)
			}
		})
	}
}

func TestCreate_RichPushOptions_Pigweed(t *testing.T) {
	SetTestProfile(t, "pigweed")
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/projects/*", http.StatusOK, []map[string]any{
		{"name": "Code-Review"},
		{"name": "Commit-Queue"},
		{"name": "Pigweed-Auto-Submit"},
	})
	server.OnJSON("GET", "/changes/*", http.StatusOK, []map[string]any{})
	mockGit := NewMockGit(t).WithBranch("main").
		OnCommand("config --get remote.origin.url", "https://pigweed.googlesource.com/pigweed/pigweed\n")

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

// A project with a Commit Queue but no auto-submit label cannot auto-submit.
// The change is still pushed and verified, and the error says so rather than
// letting the user believe it is on its way in.
func TestCreate_AutoSubmit_NoLabelDryRunsAndFails(t *testing.T) {
	SetTestProfile(t, "fuchsia")
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/projects/*", http.StatusOK, []map[string]any{
		{"name": "Code-Review"},
		{"name": "Commit-Queue", "values": map[string]string{" 0": "no", "+1": "dry run", "+2": "submit"}},
	})
	server.OnJSON("GET", "/changes/*", http.StatusOK, []map[string]any{})
	mockGit := NewMockGit(t).WithBranch("main").
		OnCommand("config --get remote.origin.url", "https://fuchsia.googlesource.com/fuchsia\n")

	output, err := executeCommand(RootCmd, "pr", "create", "--auto")
	if err == nil {
		t.Fatalf("Expected an error for a project with no auto-submit label.\nOutput: %s", output)
	}
	if !strings.Contains(err.Error(), "has no auto-submit label") {
		t.Errorf("Expected the error to name the problem, got: %v", err)
	}

	var pushCall string
	for _, call := range mockGit.Calls {
		if strings.HasPrefix(call, "push") {
			pushCall = call
			break
		}
	}
	// The change is worth creating and verifying even though nothing will
	// submit it, so the push still happens.
	if !strings.Contains(pushCall, "l=Commit-Queue+1") {
		t.Errorf("Expected the push to request a Commit-Queue dry run, got %q", pushCall)
	}
	if strings.Contains(pushCall, "l=Commit-Queue+2") {
		t.Errorf("Expected no submitting vote, got %q", pushCall)
	}
}

// A change being created has no labels to read, but the project it is going to
// does, and its spelling wins over whatever the profile was compiled with.
func TestCreate_AutoSubmitUsesProjectLabel(t *testing.T) {
	SetTestProfile(t, "pigweed")
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/projects/*", http.StatusOK, []map[string]any{
		{"name": "Code-Review", "values": map[string]string{"-2": "no", "+2": "yes"}},
		{"name": "Auto-Submit", "values": map[string]string{" 0": "no", "+1": "yes"}},
	})
	server.OnJSON("GET", "/changes/*", http.StatusOK, []map[string]any{})

	mockGit := NewMockGit(t).WithBranch("main").
		OnCommand("config --get remote.origin.url", "https://example-review.googlesource.com/example/project\n")

	if _, err := executeCommand(RootCmd, "pr", "create", "--auto"); err != nil {
		t.Fatalf("Command failed: %v", err)
	}

	if !mockGit.HasCall("l=Auto-Submit+1") {
		t.Errorf("Expected push to vote the project's Auto-Submit label, calls: %v", mockGit.Calls)
	}
	if mockGit.HasCall("l=Pigweed-Auto-Submit+1") {
		t.Errorf("Expected push not to vote the profile's label when the project names its own, calls: %v", mockGit.Calls)
	}
}

// Two plausible labels mean voting one of them would leave the change looking
// handed off while nothing submits it, so the push is refused instead.
func TestCreate_AutoSubmitAmbiguousProjectLabelsIsAnError(t *testing.T) {
	SetTestProfile(t, "pigweed")
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/projects/*", http.StatusOK, []map[string]any{
		{"name": "Auto-Submit"},
		{"name": "Owners-Auto-Submit"},
	})
	server.OnJSON("GET", "/changes/*", http.StatusOK, []map[string]any{})

	mockGit := NewMockGit(t).WithBranch("main").
		OnCommand("config --get remote.origin.url", "https://example-review.googlesource.com/example/project\n")

	_, err := executeCommand(RootCmd, "pr", "create", "--auto")
	if err == nil {
		t.Fatal("Expected an error when the project defines two auto-submit labels")
	}
	for _, want := range []string{"Auto-Submit", "Owners-Auto-Submit", "--add-label"} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("Error %q does not mention %q", err, want)
		}
	}
	if mockGit.HasCall("push") {
		t.Errorf("Expected no push when the label is ambiguous, calls: %v", mockGit.Calls)
	}
}

func TestCreate_ChangeID_AutoRepair(t *testing.T) {
	mockGit := NewMockGit(t).
		WithBranch("main").
		WithCommit("Commit message without change ID\n")

	_, err := executeCommand(RootCmd, "pr", "create")
	if err != nil {
		t.Fatalf("Command failed: %v", err)
	}

	if !mockGit.HasCall("commit --amend --only --allow-empty -F ") {
		t.Errorf("Expected git commit --amend --only --allow-empty -F to be called, calls: %v", mockGit.Calls)
	}
	if !HasChangeID(mockGit.LastCommitMessage()) {
		t.Errorf("Expected Change-Id to be injected into amended commit message, got:\n%s", mockGit.LastCommitMessage())
	}
}

func TestCreate_FailsWhenMissingChangeID(t *testing.T) {
	NewMockGit(t).WithBranch("main").WithCommit("")

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
	if !strings.Contains(output, "✓ Change created successfully.") {
		t.Errorf("Expected output to contain '✓ Change created successfully.', got: %s", output)
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

	if !strings.Contains(output, "✓ Stack pushed successfully.") {
		t.Errorf("Expected output to contain '✓ Stack pushed successfully.', got: %s", output)
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

func TestCreate_Stack_ValidBaseUpdatesAndTopNewSucceeds(t *testing.T) {
	id1 := "I1111111111111111111111111111111111111111"
	id2 := "I2222222222222222222222222222222222222222"

	server := NewMockGerritServer(t)
	server.On("GET", "/changes/*", func(w http.ResponseWriter, r *http.Request) {
		q := r.URL.Query().Get("q")
		if q == id1 {
			server.RespondJSON(w, http.StatusOK, []map[string]any{
				DefaultMockChange(488549, WithChangeID(id1)),
			})
			return
		}
		server.RespondJSON(w, http.StatusOK, []map[string]any{})
	})

	stackLog := fmt.Sprintf(
		"aaa1111\x00pw_ghish: Base commit\x00pw_ghish: Base commit\n\nChange-Id: %s\n\x1e"+
			"bbb2222\x00pw_ghish: Top commit\x00pw_ghish: Top commit\n\nChange-Id: %s\n\x1e",
		id1, id2,
	)
	mockGit := NewMockGit(t).WithBranch("main").
		WithCommit("pw_ghish: Top commit\n\nChange-Id: "+id2+"\n").
		OnCommand("rev-list --count origin/main..HEAD", "2\n").
		OnCommand("log --reverse --format=%h%x00%s%x00%B%x1e origin/main..HEAD", stackLog)

	output, err := executeCommand(RootCmd, "pr", "create", "--base", "main", "--stack")
	if err != nil {
		t.Fatalf("Expected create --stack to succeed, got: %v", err)
	}
	for _, want := range []string{"Stack plan (2 commits -> main):", "[UPDATE #488549]", "[NEW CL]"} {
		if !strings.Contains(output, want) {
			t.Errorf("Expected output to contain %q, got:\n%s", want, output)
		}
	}
	if !mockGit.HasCall("push") {
		t.Errorf("Expected git push to be called, calls: %v", mockGit.Calls)
	}
}

func TestCreate_Stack_NewCommitBelowExistingChangeFails(t *testing.T) {
	id1 := "I1111111111111111111111111111111111111111"
	id2 := "I2222222222222222222222222222222222222222"
	id3 := "I3333333333333333333333333333333333333333"

	server := NewMockGerritServer(t)
	server.On("GET", "/changes/*", func(w http.ResponseWriter, r *http.Request) {
		q := r.URL.Query().Get("q")
		if q == id2 {
			server.RespondJSON(w, http.StatusOK, []map[string]any{
				DefaultMockChange(488409, WithChangeID(id2)),
			})
			return
		}
		server.RespondJSON(w, http.StatusOK, []map[string]any{})
	})

	stackLog := fmt.Sprintf(
		"aaa1111\x00pw_ghish: Wiped Change-Id commit\x00pw_ghish: Wiped Change-Id commit\n\nChange-Id: %s\n\x1e"+
			"bbb2222\x00pw_ghish: Existing middle commit\x00pw_ghish: Existing middle commit\n\nChange-Id: %s\n\x1e"+
			"ccc3333\x00pw_ghish: New top commit\x00pw_ghish: New top commit\n\nChange-Id: %s\n\x1e",
		id1, id2, id3,
	)
	mockGit := NewMockGit(t).WithBranch("main").
		WithCommit("pw_ghish: New top commit\n\nChange-Id: "+id3+"\n").
		OnCommand("rev-list --count origin/main..HEAD", "3\n").
		OnCommand("log --reverse --format=%h%x00%s%x00%B%x1e origin/main..HEAD", stackLog)

	_, err := executeCommand(RootCmd, "pr", "create", "--base", "main", "--stack")
	if err == nil {
		t.Fatal("Expected error when a [NEW CL] commit sits below an [UPDATE] commit in the stack")
	}
	for _, want := range []string{"stack order error", "aaa1111", "#488409", "bbb2222"} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("Expected error to mention %q, got: %v", want, err)
		}
	}
	if mockGit.HasCall("push") {
		t.Errorf("Expected no git push when stack order error is detected, calls: %v", mockGit.Calls)
	}
}

func TestCreate_TopicForbiddenByProjectConfig(t *testing.T) {
	tmpDir := t.TempDir()
	toml := `
[gerrit]
forbid_topics = true
`
	if err := os.WriteFile(filepath.Join(tmpDir, ".ghish.toml"), []byte(toml), 0644); err != nil {
		t.Fatalf("failed to write .ghish.toml: %v", err)
	}

	server := NewMockGerritServer(t)
	MockCWD = tmpDir
	server.OnJSON("GET", "/changes/*", http.StatusOK, []map[string]any{})
	mockGit := NewMockGit(t).WithBranch("main")

	_, err := executeCommand(RootCmd, "pr", "create", "--topic", "my-topic")
	if err == nil {
		t.Fatal("Expected error when --topic is passed and forbid_topics = true")
	}
	for _, want := range []string{"Topics-Not-Supported", "my-topic", "without --topic"} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("Expected error to contain %q, got: %v", want, err)
		}
	}
	if mockGit.HasCall("push") {
		t.Errorf("Expected git push not to be called when topic is forbidden, calls: %v", mockGit.Calls)
	}
}

func TestCreate_Nightly(t *testing.T) {
	SetTestProfile(t, "pigweed")
	changeID := "I0000000000000000000000000000000000000001"
	server := NewMockGerritServer(t)
	// QueryChanges before push sees no existing change.
	server.OnJSON("GET", "/changes/", http.StatusOK, []map[string]any{})
	// GetChange after push resolves the newly created change number.
	server.OnJSON("GET", "/changes/"+changeID, http.StatusOK, DefaultMockChange(123456, WithChangeID(changeID), WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit")))
	server.OnFlows(123456)
	server.OnFlowsEnabled(123456, http.StatusOK, true)
	server.OnJSON("POST", "/changes/123456/flows", http.StatusCreated, map[string]any{})

	mockGit := NewMockGit(t).WithBranch("main")

	output, err := executeCommand(RootCmd, "pr", "create", "--cq", "--nightly")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if !mockGit.HasCall("push origin HEAD:refs/for/main%l=Commit-Queue+1") {
		t.Errorf("Expected git push with Commit-Queue+1 dry run, got calls: %v", mockGit.Calls)
	}
	if server.CallCount("POST", "/changes/123456/flows") != 1 {
		t.Fatalf("Expected POST /changes/123456/flows to be called once, got %d", server.CallCount("POST", "/changes/123456/flows"))
	}
	wantOut := "Created submit flow for Change 123456 (scheduled for tomorrow at 02:36 in America/Los_Angeles)."
	if !strings.Contains(output, wantOut) {
		t.Errorf("Output got %q, want %q", output, wantOut)
	}
}

func TestCreate_ScheduledFlagConflicts(t *testing.T) {
	NewMockGit(t).WithBranch("main")

	tests := []struct {
		name    string
		args    []string
		wantErr string
	}{
		{
			name:    "nightly and submit-at",
			args:    []string{"pr", "create", "--nightly", "--submit-at", "20:00"},
			wantErr: "cannot specify both --nightly and --submit-at",
		},
		{
			name:    "nightly and auto",
			args:    []string{"pr", "create", "--nightly", "--auto"},
			wantErr: "cannot specify both --auto and --nightly/--submit-at",
		},
		{
			name:    "nightly and cq=2",
			args:    []string{"pr", "create", "--nightly", "--cq", "2"},
			wantErr: "cannot specify both --cq=2 and --nightly/--submit-at",
		},
		{
			name:    "nightly and draft",
			args:    []string{"pr", "create", "--nightly", "--draft"},
			wantErr: "cannot specify both --draft and --nightly/--submit-at",
		},
		{
			name:    "submit-at and draft",
			args:    []string{"pr", "create", "--submit-at", "20:00", "--draft"},
			wantErr: "cannot specify both --draft and --nightly/--submit-at",
		},
		{
			name:    "invalid submit-at",
			args:    []string{"pr", "create", "--submit-at", "8pm"},
			wantErr: "expected 24-hour format \"HH:MM\"",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			_, err := executeCommand(RootCmd, tc.args...)
			if err == nil {
				t.Fatalf("Expected error for args %v, got nil", tc.args)
			}
			if !strings.Contains(err.Error(), tc.wantErr) {
				t.Errorf("Error got %q, want substring %q", err.Error(), tc.wantErr)
			}
		})
	}
}

// A flow failure after the change is created must not read as a failed create.
func TestCreate_Nightly_FlowFailureIsNotACreateFailure(t *testing.T) {
	SetTestProfile(t, "pigweed")
	changeID := "I0000000000000000000000000000000000000001"
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/", http.StatusOK, []map[string]any{})
	server.OnJSON("GET", "/changes/"+changeID, http.StatusOK, DefaultMockChange(123456, WithChangeID(changeID), WithLabels("Code-Review", "Commit-Queue")))

	mockGit := NewMockGit(t).WithBranch("main")
	sleeps := recordScheduleSleeps(t)

	_, err := executeCommand(RootCmd, "pr", "create", "--nightly")
	if err == nil {
		t.Fatal("expected an error when the change has no auto-submit label, got nil")
	}
	// A missing label cannot appear on retry.
	if len(*sleeps) != 0 {
		t.Errorf("expected no retries, got delays %v", *sleeps)
	}
	if !mockGit.HasCall("push") {
		t.Fatalf("expected the push to have happened, got calls: %v", mockGit.Calls)
	}
	for _, want := range []string{"change created, but scheduling the submit flow failed", "has no auto-submit label", "pr merge"} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("error missing %q, got: %v", want, err)
		}
	}
	if strings.Contains(err.Error(), "error creating change") {
		t.Errorf("error must not claim the create failed, got: %v", err)
	}
}

// A new change has no change number to ask about before it is pushed, so on a
// host without Gerrit Flows the create succeeds and the schedule fails with
// an actionable error, before the change is otherwise modified.
func TestCreate_Nightly_FlowsNotEnabled(t *testing.T) {
	SetTestProfile(t, "pigweed")
	changeID := "I0000000000000000000000000000000000000001"
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/", http.StatusOK, []map[string]any{})
	server.OnJSON("GET", "/changes/"+changeID, http.StatusOK, DefaultMockChange(123456, WithChangeID(changeID), WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit")))
	server.OnFlowsEnabled(123456, http.StatusMethodNotAllowed, false)
	server.OnJSON("POST", "/changes/123456/flows", http.StatusCreated, map[string]any{})

	mockGit := NewMockGit(t).WithBranch("main")

	_, err := executeCommand(RootCmd, "pr", "create", "--nightly")
	if !mockGit.HasCall("push") {
		t.Fatalf("expected the push to have happened, got calls: %v", mockGit.Calls)
	}
	var flowErr *ScheduleFlowError
	if !errors.As(err, &flowErr) {
		t.Fatalf("expected a *ScheduleFlowError, got %T: %v", err, err)
	}
	var notEnabled *FlowsNotEnabledError
	if !errors.As(err, &notEnabled) {
		t.Fatalf("expected it to wrap a *FlowsNotEnabledError, got: %v", err)
	}
	if n := server.CallCount("POST", "/changes/123456/flows"); n != 0 {
		t.Errorf("expected no flows to be created, got %d", n)
	}
	// 405 means no flow service; asking again cannot change that.
	if n := server.CallCount("GET", "/changes/123456/is-flows-enabled"); n != 1 {
		t.Errorf("expected is-flows-enabled to be asked once, got %d", n)
	}
}

// recordScheduleSleeps replaces scheduleSleepFn for the test and returns the
// delays it is asked to wait.
func recordScheduleSleeps(t *testing.T) *[]time.Duration {
	t.Helper()
	var sleeps []time.Duration
	orig := scheduleSleepFn
	t.Cleanup(func() { scheduleSleepFn = orig })
	scheduleSleepFn = func(_ context.Context, d time.Duration) error {
		sleeps = append(sleeps, d)
		return nil
	}
	return &sleeps
}

// nightlyReplicationFixture serves a just-created change whose
// is-flows-enabled answers 404, as a Gerrit mirror the change has not
// replicated to yet does, for the first notFound requests.
func nightlyReplicationFixture(t *testing.T, notFound int) *MockGerritServer {
	t.Helper()
	SetTestProfile(t, "pigweed")
	changeID := "I0000000000000000000000000000000000000001"
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/", http.StatusOK, []map[string]any{})
	server.OnJSON("GET", "/changes/"+changeID, http.StatusOK, DefaultMockChange(123456, WithChangeID(changeID), WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit")))
	server.OnFlows(123456)
	calls := 0
	server.On("GET", "/changes/123456/is-flows-enabled", func(w http.ResponseWriter, r *http.Request) {
		calls++
		if calls <= notFound {
			w.WriteHeader(http.StatusNotFound)
			return
		}
		server.RespondJSON(w, http.StatusOK, map[string]any{"enabled": true})
	})
	server.OnJSON("POST", "/changes/123456/flows", http.StatusCreated, map[string]any{})
	NewMockGit(t).WithBranch("main")
	return server
}

// A change that has not replicated yet is retried with backoff until the
// schedule succeeds.
func TestCreate_Nightly_RetriesWhileReplicating(t *testing.T) {
	server := nightlyReplicationFixture(t, 2)
	sleeps := recordScheduleSleeps(t)

	output, err := executeCommand(RootCmd, "pr", "create", "--nightly")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}
	if diff := cmp.Diff([]time.Duration{time.Second, 2 * time.Second}, *sleeps); diff != "" {
		t.Errorf("retry delays mismatch (-want +got):\n%s", diff)
	}
	if n := server.CallCount("POST", "/changes/123456/flows"); n != 1 {
		t.Errorf("expected one flow to be created, got %d", n)
	}
	for _, want := range []string{"retrying in 1s", "Created submit flow for Change 123456"} {
		if !strings.Contains(output, want) {
			t.Errorf("output missing %q, got: %s", want, output)
		}
	}
}

// Retries stop after 1s, 2s, 4s, and 8s, and the last error is reported as a
// schedule failure, not a create failure.
func TestCreate_Nightly_RetriesGiveUp(t *testing.T) {
	server := nightlyReplicationFixture(t, 100)
	sleeps := recordScheduleSleeps(t)

	_, err := executeCommand(RootCmd, "pr", "create", "--nightly")
	var flowErr *ScheduleFlowError
	if !errors.As(err, &flowErr) {
		t.Fatalf("expected a *ScheduleFlowError, got %T: %v", err, err)
	}
	want := []time.Duration{time.Second, 2 * time.Second, 4 * time.Second, 8 * time.Second}
	if diff := cmp.Diff(want, *sleeps); diff != "" {
		t.Errorf("retry delays mismatch (-want +got):\n%s", diff)
	}
	if n := server.CallCount("GET", "/changes/123456/is-flows-enabled"); n != len(want)+1 {
		t.Errorf("expected %d is-flows-enabled requests, got %d", len(want)+1, n)
	}
	if n := server.CallCount("POST", "/changes/123456/flows"); n != 0 {
		t.Errorf("expected no flows to be created, got %d", n)
	}
}

func TestCreate_RealGit_CommitMessageFlagsAndRollback(t *testing.T) {
	t.Run("creates commit from dirty tree on upstream tip", func(t *testing.T) {
		SetTestProfile(t, "pigweed")
		server := NewMockGerritServer(t)
		server.OnJSON("GET", "/changes/", http.StatusOK, []map[string]any{})

		repo := newTestGitRepo(t)
		repo.InstallCommitMsgHook()
		repo.BindToCLI()

		repo.WriteFile("base.txt", "new feature\n")

		if out, err := executeCommand(RootCmd, "pr", "create", "-t", "pw_foo: Add feature", "-b", "Explain why.", "--fixed", "b/567763970"); err != nil {
			t.Fatalf("pr create failed: %v\nOutput: %s", err, out)
		}

		pushedMsg := repo.BareGitOutput("log", "-1", "--format=%B", "refs/for/main")
		wantOrder := "Fixed: b/567763970\nChange-Id: I1111111111111111111111111111111111111111"
		if !strings.Contains(pushedMsg, wantOrder) {
			t.Errorf("expected Fixed above Change-Id on refs/for/main, got:\n%s", pushedMsg)
		}
	})

	t.Run("creates new commit when local branch sits behind fetched origin/main", func(t *testing.T) {
		SetTestProfile(t, "pigweed")
		server := NewMockGerritServer(t)
		server.OnJSON("GET", "/changes/", http.StatusOK, []map[string]any{})

		repo := newTestGitRepo(t)
		repo.InstallCommitMsgHook()
		repo.BindToCLI()

		ancestorSHA := repo.GitOutput("rev-parse", "HEAD")
		ancestorMsg := repo.GitOutput("log", "-1", "--format=%B", "HEAD")

		// Advance origin/main by 1 commit while local branch stays behind at ancestorSHA.
		repo.GitOutput("branch", "feature", ancestorSHA)
		repo.StageFile("upstream.txt", "merged upstream\n")
		repo.GitOutput("commit", "-m", "pw_other: Landed upstream")
		repo.GitOutput("push", "origin", "main")
		repo.GitOutput("checkout", "feature")

		repo.WriteFile("base.txt", "feature behind main\n")

		if out, err := executeCommand(RootCmd, "pr", "create", "-t", "pw_foo: Feature from behind branch", "-b", "Fresh commit."); err != nil {
			t.Fatalf("pr create failed: %v\nOutput: %s", err, out)
		}

		if gotParent := repo.GitOutput("rev-parse", "HEAD~1"); gotParent != ancestorSHA {
			t.Fatalf("HEAD~1 = %s, want untouched ancestor %s", gotParent, ancestorSHA)
		}
		if gotAncestorMsg := repo.GitOutput("log", "-1", "--format=%B", ancestorSHA); gotAncestorMsg != ancestorMsg {
			t.Errorf("ancestor commit message = %q, want %q", gotAncestorMsg, ancestorMsg)
		}
		if gotFile := repo.BareGitOutput("show", "refs/for/main:base.txt"); gotFile != "feature behind main" {
			t.Errorf("pushed base.txt = %q, want %q", gotFile, "feature behind main")
		}
		pushedMsg := repo.BareGitOutput("log", "-1", "--format=%B", "refs/for/main")
		if !strings.Contains(pushedMsg, "pw_foo: Feature from behind branch\n\nFresh commit.\n\nChange-Id: I1111111111111111111111111111111111111111") {
			t.Errorf("unexpected pushed commit message:\n%s", pushedMsg)
		}
	})

	t.Run("amends unpushed commit and rolls back on failure", func(t *testing.T) {
		SetTestProfile(t, "pigweed")
		changeID := "I1111111111111111111111111111111111111111"

		repo := newTestGitRepo(t)
		repo.InstallCommitMsgHook()
		repo.BindToCLI()

		repo.WriteFile("base.txt", "commit 1\n")
		if _, err := repo.Client.CommitNewAll(t.Context(), "pw_foo: Draft title\n\nDraft body.\n", false, io.Discard, io.Discard); err != nil {
			t.Fatal(err)
		}
		origSHA := repo.GitOutput("rev-parse", "HEAD")

		// Stage an extra file and leave an unstaged edit in the worktree.
		repo.StageFile("staged.txt", "staged next\n")
		origIndexTree := repo.GitOutput("write-tree")
		repo.WriteFile("base.txt", "unstaged worktree\n")

		// Simulate existing Gerrit CL failure AFTER local amend: HEAD, index, and worktree must roll back cleanly.
		failServer := NewMockGerritServer(t)
		failServer.OnJSON("GET", "/changes/", http.StatusOK, []map[string]any{
			DefaultMockChange(509225, WithChangeID(changeID)),
		})
		if _, err := executeCommand(RootCmd, "pr", "create", "--title", "pw_foo: Aborted title", "--bug", "42"); err == nil {
			t.Fatal("expected pr create to fail when Change-Id already exists")
		}
		if gotSHA := repo.GitOutput("rev-parse", "HEAD"); gotSHA != origSHA {
			t.Fatalf("rollback failed: HEAD = %s, want %s", gotSHA, origSHA)
		}
		if gotIndex := repo.GitOutput("write-tree"); gotIndex != origIndexTree {
			t.Fatalf("staged index altered on rollback: %s != %s", gotIndex, origIndexTree)
		}

		// Now succeed against empty Gerrit: amends message without sweeping staged.txt or unstaged base.txt.
		okServer := NewMockGerritServer(t)
		okServer.OnJSON("GET", "/changes/", http.StatusOK, []map[string]any{})
		if out, err := executeCommand(RootCmd, "pr", "create", "--title", "pw_foo: Final title", "--bug", "42"); err != nil {
			t.Fatalf("pr create failed: %v\nOutput: %s", err, out)
		}
		pushedMsg := repo.BareGitOutput("log", "-1", "--format=%B", "refs/for/main")
		if !strings.Contains(pushedMsg, "pw_foo: Final title\n\nDraft body.\n\nBug: b/42\nChange-Id: "+changeID) {
			t.Errorf("unexpected pushed commit message:\n%s", pushedMsg)
		}
		if gotIndex := repo.GitOutput("write-tree"); gotIndex != origIndexTree {
			t.Errorf("staged index altered after create: %s != %s", gotIndex, origIndexTree)
		}
		if got := repo.ReadFile("base.txt"); got != "unstaged worktree\n" {
			t.Errorf("unstaged worktree altered: %q", got)
		}
	})

	t.Run("rolls back auto-committed dirty tree when push fails", func(t *testing.T) {
		SetTestProfile(t, "pigweed")
		changeID := "I1111111111111111111111111111111111111111"
		server := NewMockGerritServer(t)
		server.OnJSON("GET", "/changes/", http.StatusOK, []map[string]any{
			DefaultMockChange(509225, WithChangeID(changeID)),
		})

		repo := newTestGitRepo(t)
		repo.InstallCommitMsgHook()
		repo.BindToCLI()
		tipSHA := repo.GitOutput("rev-parse", "HEAD")
		origIndexTree := repo.GitOutput("write-tree")

		repo.WriteFile("base.txt", "dirty pre-commit\n")
		if _, err := executeCommand(RootCmd, "pr", "create", "-t", "pw_foo: Attempted create"); err == nil {
			t.Fatal("expected error when Change-Id already exists")
		}
		if gotSHA := repo.GitOutput("rev-parse", "HEAD"); gotSHA != tipSHA {
			t.Fatalf("expected HEAD rolled back to %s, got %s", tipSHA, gotSHA)
		}
		if gotIndex := repo.GitOutput("write-tree"); gotIndex != origIndexTree {
			t.Fatalf("expected auto-staged index reset to %s, got %s", origIndexTree, gotIndex)
		}
		if got := repo.ReadFile("base.txt"); got != "dirty pre-commit\n" {
			t.Errorf("dirty working tree file lost on rollback: %q", got)
		}
	})
}

func TestCreate_Stack_ExistingHead(t *testing.T) {
	id1 := "I1111111111111111111111111111111111111111"
	id2 := "I2222222222222222222222222222222222222222"

	stackLog := fmt.Sprintf(
		"aaa1111\x00pw_ghish: Base commit\x00pw_ghish: Base commit\n\nChange-Id: %s\n\x1e"+
			"bbb2222\x00pw_ghish: Top commit\x00pw_ghish: Top commit\n\nChange-Id: %s\n\x1e",
		id1, id2,
	)

	newFixture := func(t *testing.T, existingIDs ...string) *MockGitRunner {
		t.Helper()
		server := NewMockGerritServer(t)
		server.On("GET", "/changes/", func(w http.ResponseWriter, r *http.Request) {
			q := r.URL.Query().Get("q")
			switch {
			case slices.Contains(existingIDs, id2) && strings.Contains(q, id2):
				server.RespondJSON(w, http.StatusOK, []map[string]any{
					DefaultMockChange(497839, WithChangeID(id2)),
				})
			case slices.Contains(existingIDs, id1) && strings.Contains(q, id1):
				server.RespondJSON(w, http.StatusOK, []map[string]any{
					DefaultMockChange(497800, WithChangeID(id1)),
				})
			default:
				server.RespondJSON(w, http.StatusOK, []map[string]any{})
			}
		})
		return NewMockGit(t).WithBranch("main").
			WithCommit("pw_ghish: Top commit\n\nChange-Id: "+id2+"\n").
			OnCommand("rev-list --count origin/main..HEAD", "2\n").
			OnCommand("log --reverse --format=%h%x00%s%x00%B%x1e origin/main..HEAD", stackLog)
	}

	t.Run("inspects full stack and suggests --force instead of gh pr push", func(t *testing.T) {
		mockGit := newFixture(t, id2)
		out, err := executeCommand(RootCmd, "pr", "create", "--base", "main", "--stack")
		if err == nil {
			t.Fatal("expected error when new commit sits below existing CL")
		}
		for _, want := range []string{"[NEW CL]", "[UPDATE #497839]"} {
			if !strings.Contains(out, want) {
				t.Errorf("expected Stack plan output to contain %q, got:\n%s", want, out)
			}
		}
		for _, want := range []string{"gh pr create --stack --force", "gh pr create aaa1111"} {
			if !strings.Contains(err.Error(), want) {
				t.Errorf("expected remediation to suggest %q, got: %v", want, err)
			}
		}
		if mockGit.HasCall("push") {
			t.Errorf("expected no git push on unconfirmed stack reorder, got: %v", mockGit.Calls)
		}
	})

	t.Run("all commits already exist suggests gh pr push --stack", func(t *testing.T) {
		mockGit := newFixture(t, id1, id2)
		_, err := executeCommand(RootCmd, "pr", "create", "--base", "main", "--stack")
		if err == nil {
			t.Fatal("expected error when all stack commits already exist on Gerrit")
		}
		for _, want := range []string{"already exists", "/c/pigweed/pigweed/+/497839", "gh pr push --stack"} {
			if !strings.Contains(err.Error(), want) {
				t.Errorf("expected error to contain %q, got: %v", want, err)
			}
		}
		if mockGit.HasCall("push") {
			t.Errorf("expected no git push when all commits already exist, got: %v", mockGit.Calls)
		}
	})

	t.Run("Gerrit lookup error on earlier stack commit reports stack lookup failure", func(t *testing.T) {
		server := NewMockGerritServer(t)
		server.On("GET", "/changes/", func(w http.ResponseWriter, r *http.Request) {
			if strings.Contains(r.URL.Query().Get("q"), id2) {
				server.RespondJSON(w, http.StatusOK, []map[string]any{DefaultMockChange(497839, WithChangeID(id2))})
				return
			}
			server.RespondJSON(w, http.StatusInternalServerError, map[string]any{"error": "boom"})
		})
		mockGit := NewMockGit(t).WithBranch("main").
			WithCommit("pw_ghish: Top commit\n\nChange-Id: "+id2+"\n").
			OnCommand("rev-list --count origin/main..HEAD", "2\n").
			OnCommand("log --reverse --format=%h%x00%s%x00%B%x1e origin/main..HEAD", stackLog)

		_, err := executeCommand(RootCmd, "pr", "create", "--base", "main", "--stack")
		if err == nil {
			t.Fatal("expected error on Gerrit 500 during stack lookup")
		}
		for _, want := range []string{"could not verify all commits in the stack", "gh pr push --stack"} {
			if !strings.Contains(err.Error(), want) {
				t.Errorf("expected error to contain %q, got: %v", want, err)
			}
		}
		if mockGit.HasCall("push") {
			t.Errorf("expected no git push on lookup error, got: %v", mockGit.Calls)
		}
	})

	t.Run("single-commit create --stack on existing HEAD suggests gh pr push", func(t *testing.T) {
		singleLog := fmt.Sprintf("bbb2222\x00pw_ghish: Top commit\x00pw_ghish: Top commit\n\nChange-Id: %s\n\x1e", id2)
		server := NewMockGerritServer(t)
		server.OnJSON("GET", "/changes/", http.StatusOK, []map[string]any{
			DefaultMockChange(497839, WithChangeID(id2)),
		})
		mockGit := NewMockGit(t).WithBranch("main").
			WithCommit("pw_ghish: Top commit\n\nChange-Id: "+id2+"\n").
			OnCommand("rev-list --count origin/main..HEAD", "1\n").
			OnCommand("log --reverse --format=%h%x00%s%x00%B%x1e origin/main..HEAD", singleLog)

		_, err := executeCommand(RootCmd, "pr", "create", "--base", "main", "--stack")
		if err == nil {
			t.Fatal("expected error when single-commit stack already exists on Gerrit")
		}
		for _, want := range []string{"already exists", "gh pr push"} {
			if !strings.Contains(err.Error(), want) {
				t.Errorf("expected error to contain %q, got: %v", want, err)
			}
		}
		if strings.Contains(err.Error(), "--stack") {
			t.Errorf("did not expect --stack in single-commit error, got: %v", err)
		}
		if mockGit.HasCall("push") {
			t.Errorf("expected no git push when single-commit stack already exists, got: %v", mockGit.Calls)
		}
	})
}

func TestCreate_TargetCommitArg(t *testing.T) {
	baseSHA := "1170aeea20000000000000000000000000000001"
	headSHA := "463730ae30000000000000000000000000000002"
	baseID := "Icb4065c8a7a932f8d8b42d0190cb9280b8b7ee32"
	headID := "I6367ee29192c1fff44d64e5e16d8d9446badb417"

	t.Run("pushes only specified base commit without pushing HEAD", func(t *testing.T) {
		server := NewMockGerritServer(t)
		server.On("GET", "/changes/", func(w http.ResponseWriter, r *http.Request) {
			if strings.Contains(r.URL.Query().Get("q"), headID) {
				server.RespondJSON(w, http.StatusOK, []map[string]any{DefaultMockChange(999999, WithChangeID(headID))})
				return
			}
			server.RespondJSON(w, http.StatusOK, []map[string]any{})
		})

		mockGit := NewMockGit(t).WithBranch("main").
			WithCommit("pw_bluetooth_sapphire: Top commit\n\nChange-Id: "+headID+"\n").
			OnCommand("rev-parse --verify 1170aeea2^{commit}", baseSHA+"\n").
			OnCommand("rev-parse HEAD", headSHA+"\n").
			OnCommand("log -1 --format=%B "+baseSHA, "pw_bluetooth_sapphire: Base commit\n\nChange-Id: "+baseID+"\n").
			OnCommand("rev-list --count origin/main.."+baseSHA, "1\n")

		out, err := executeCommand(RootCmd, "pr", "create", "1170aeea2", "--base", "main")
		if err != nil {
			t.Fatalf("gh pr create 1170aeea2 failed: %v\nOutput: %s", err, out)
		}
		if !mockGit.HasCall("push origin " + baseSHA + ":refs/for/main") {
			t.Errorf("expected push of %s:refs/for/main (not HEAD), got calls: %v", baseSHA, mockGit.Calls)
		}
		if n := countCalls(mockGit.Calls, "push origin "); n != 1 {
			t.Errorf("expected exactly 1 git push, got %d (%v)", n, mockGit.Calls)
		}
		if mockGit.HasCall("commit --amend") {
			t.Errorf("expected no git commit --amend, got calls: %v", mockGit.Calls)
		}
		if !strings.Contains(out, "gh pr checks 1170aeea2 --watch [--fail-fast]") {
			t.Errorf("expected Next receipt hint for target commit 1170aeea2, got:\n%s", out)
		}
	})

	t.Run("rejects combining <commit> with --stack or commit-message flags", func(t *testing.T) {
		for _, tc := range []struct {
			name    string
			args    []string
			wantErr string
		}{
			{
				name:    "--stack",
				args:    []string{"pr", "create", "1170aeea2", "--stack"},
				wantErr: "cannot specify both a commit",
			},
			{
				name:    "--title",
				args:    []string{"pr", "create", "1170aeea2", "--title", "New title"},
				wantErr: "cannot combine commit-message editing flags",
			},
		} {
			t.Run(tc.name, func(t *testing.T) {
				NewMockGerritServer(t)
				mockGit := NewMockGit(t).WithBranch("main")
				if _, err := executeCommand(RootCmd, tc.args...); err == nil || !strings.Contains(err.Error(), tc.wantErr) {
					t.Fatalf("expected error containing %q, got: %v", tc.wantErr, err)
				}
				if mockGit.HasCall("push") {
					t.Errorf("expected no git push, got: %v", mockGit.Calls)
				}
			})
		}
	})

	t.Run("schedules --nightly submit flow on target commit instead of HEAD", func(t *testing.T) {
		SetTestProfile(t, "pigweed")
		server := NewMockGerritServer(t)
		server.OnJSON("GET", "/changes/", http.StatusOK, []map[string]any{})
		server.OnJSON("GET", "/changes/"+baseID, http.StatusOK,
			DefaultMockChange(111111, WithChangeID(baseID), WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit")))
		server.OnFlows(111111)
		server.OnFlowsEnabled(111111, http.StatusOK, true)
		server.OnJSON("POST", "/changes/111111/flows", http.StatusCreated, map[string]any{})

		// scheduleSubmitFlowForPush resolves baseSHA via ResolveTargetChangeID(baseSHA);
		// stub refs/heads/<baseSHA> to fail so MockGitRunner does not treat baseSHA
		// as a branch name and fall back to WithCommit (HEAD).
		mockGit := NewMockGit(t).WithBranch("main").
			WithCommit("pw_bluetooth_sapphire: Top commit\n\nChange-Id: "+headID+"\n").
			OnCommand("rev-parse --verify 1170aeea2^{commit}", baseSHA+"\n").
			OnCommand("rev-parse --verify "+baseSHA+"^{commit}", baseSHA+"\n").
			OnError("rev-parse --verify refs/heads/"+baseSHA, errors.New("not found")).
			OnCommand("rev-parse HEAD", headSHA+"\n").
			OnCommand("log -1 --format=%B "+baseSHA, "pw_bluetooth_sapphire: Base commit\n\nChange-Id: "+baseID+"\n").
			OnCommand("rev-list --count origin/main.."+baseSHA, "1\n")

		out, err := executeCommand(RootCmd, "pr", "create", "1170aeea2", "--base", "main", "--nightly")
		if err != nil {
			t.Fatalf("gh pr create 1170aeea2 --nightly failed: %v\nOutput: %s", err, out)
		}
		if server.CallCount("POST", "/changes/111111/flows") != 1 {
			t.Errorf("expected POST /changes/111111/flows once on base commit, got %d", server.CallCount("POST", "/changes/111111/flows"))
		}
		if !mockGit.HasCall("push origin " + baseSHA + ":refs/for/main") {
			t.Errorf("expected push of %s, got: %v", baseSHA, mockGit.Calls)
		}
	})

	t.Run("enforces multi-commit and fixup guards on explicit HEAD and <commit>", func(t *testing.T) {
		t.Run("explicit HEAD multi-commit guard", func(t *testing.T) {
			NewMockGerritServer(t)
			mockGit := NewMockGit(t).WithBranch("main").
				WithCommit("pw_foo: Top commit\n\nChange-Id: "+headID+"\n").
				OnCommand("rev-parse --verify HEAD^{commit}", headSHA+"\n").
				OnCommand("rev-parse HEAD", headSHA+"\n").
				OnCommand("rev-list --count origin/main..HEAD", "3\n")

			_, err := executeCommand(RootCmd, "pr", "create", "HEAD", "--base", "main")
			if err == nil || !strings.Contains(err.Error(), "would create 3 separate Gerrit changes") {
				t.Fatalf("expected multi-commit guard on explicit HEAD target, got: %v", err)
			}
			if mockGit.HasCall("push") {
				t.Errorf("expected no git push on multi-commit HEAD target, got: %v", mockGit.Calls)
			}
		})

		t.Run("<commit> multi-commit guard", func(t *testing.T) {
			NewMockGerritServer(t)
			mockGit := NewMockGit(t).WithBranch("main").
				OnCommand("rev-parse --verify 1170aeea2^{commit}", baseSHA+"\n").
				OnCommand("rev-parse HEAD", headSHA+"\n").
				OnCommand("log -1 --format=%B "+baseSHA, "pw_bluetooth_sapphire: Middle commit\n\nChange-Id: "+baseID+"\n").
				OnCommand("rev-list --count origin/main.."+baseSHA, "3\n")
			_, err := executeCommand(RootCmd, "pr", "create", "1170aeea2", "--base", "main")
			if err == nil || !strings.Contains(err.Error(), "would create 3 separate Gerrit changes") {
				t.Fatalf("expected multi-commit guard error on <commit>, got: %v", err)
			}
			if mockGit.HasCall("push") {
				t.Errorf("expected no git push on multi-commit <commit> target, got: %v", mockGit.Calls)
			}
		})

		t.Run("<commit> fixup guard", func(t *testing.T) {
			NewMockGerritServer(t)
			mockGit := NewMockGit(t).WithBranch("main").
				OnCommand("rev-parse --verify 1170aeea2^{commit}", baseSHA+"\n").
				OnCommand("rev-parse HEAD", headSHA+"\n").
				OnCommand("log -1 --format=%B "+baseSHA, "fixup! pw_foo: Base\n\nChange-Id: "+baseID+"\n")
			_, err := executeCommand(RootCmd, "pr", "create", "1170aeea2", "--base", "main")
			if err == nil || !strings.Contains(err.Error(), "unsquashed fixup/squash commit") {
				t.Fatalf("expected fixup guard error, got: %v", err)
			}
			if mockGit.HasCall("push") {
				t.Errorf("expected no git push on fixup <commit> target, got: %v", mockGit.Calls)
			}
		})
	})

	t.Run("errors when <commit> is missing a Gerrit Change-Id trailer without amending or pushing", func(t *testing.T) {
		NewMockGerritServer(t)
		mockGit := NewMockGit(t).WithBranch("main").
			WithCommit("pw_foo: Top commit\n\nChange-Id: "+headID+"\n").
			OnCommand("rev-parse --verify 1170aeea2^{commit}", baseSHA+"\n").
			OnCommand("rev-parse HEAD", headSHA+"\n").
			OnCommand("log -1 --format=%B "+baseSHA, "pw_foo: Base commit without Change-Id\n")

		_, err := executeCommand(RootCmd, "pr", "create", "1170aeea2", "--base", "main")
		if err == nil || !strings.Contains(err.Error(), "missing a Gerrit Change-Id trailer") {
			t.Fatalf("expected missing Change-Id error for <commit>, got: %v", err)
		}
		if mockGit.HasCall("commit --amend") {
			t.Errorf("expected no git commit --amend on non-HEAD commit, got: %v", mockGit.Calls)
		}
		if mockGit.HasCall("push") {
			t.Errorf("expected no git push when <commit> lacks Change-Id, got: %v", mockGit.Calls)
		}
	})

	t.Run("suggests gh pr create <commit> --force when <commit> already has a Gerrit change", func(t *testing.T) {
		server := NewMockGerritServer(t)
		server.OnJSON("GET", "/changes/", http.StatusOK, []map[string]any{
			DefaultMockChange(111111, WithChangeID(baseID)),
		})
		mockGit := NewMockGit(t).WithBranch("main").
			WithCommit("pw_foo: Top commit\n\nChange-Id: "+headID+"\n").
			OnCommand("rev-parse --verify 1170aeea2^{commit}", baseSHA+"\n").
			OnCommand("rev-parse HEAD", headSHA+"\n").
			OnCommand("log -1 --format=%B "+baseSHA, "pw_foo: Base commit\n\nChange-Id: "+baseID+"\n")

		_, err := executeCommand(RootCmd, "pr", "create", "1170aeea2", "--base", "main")
		if err == nil || !strings.Contains(err.Error(), "gh pr create 1170aeea2 --force") {
			t.Fatalf("expected 'gh pr create 1170aeea2 --force' hint, got: %v", err)
		}
		if mockGit.HasCall("push") {
			t.Errorf("expected no git push when <commit> already exists without --force, got: %v", mockGit.Calls)
		}
	})

	t.Run("resolves @owners for <commit> files rather than HEAD files", func(t *testing.T) {
		tmpDir := t.TempDir()
		for dir, owner := range map[string]string{
			"dir_base": "base_owner@example.com\n",
			"dir_head": "head_owner@example.com\n",
		} {
			if err := os.MkdirAll(filepath.Join(tmpDir, dir), 0755); err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(filepath.Join(tmpDir, dir, "OWNERS"), []byte(owner), 0644); err != nil {
				t.Fatal(err)
			}
		}
		oldMockCWD := MockCWD
		MockCWD = tmpDir
		t.Cleanup(func() { MockCWD = oldMockCWD })

		server := NewMockGerritServer(t)
		server.OnJSON("GET", "/changes/", http.StatusOK, []map[string]any{})
		mockGit := NewMockGit(t).WithBranch("main").
			OnCommand("rev-parse --show-toplevel", tmpDir+"\n").
			OnCommand("rev-parse --verify 1170aeea2^{commit}", baseSHA+"\n").
			OnCommand("rev-parse HEAD", headSHA+"\n").
			OnCommand("log -1 --format=%B "+baseSHA, "pw_foo: Base\n\nChange-Id: "+baseID+"\n").
			OnCommand("log -1 --format=%ae "+baseSHA, "author@example.com\n").
			OnCommand("diff-tree --no-commit-id --name-only -r "+baseSHA, "dir_base/foo.cc\n").
			OnCommand("diff-tree --no-commit-id --name-only -r HEAD", "dir_head/bar.cc\n").
			OnCommand("rev-list --count origin/main.."+baseSHA, "1\n")

		if _, err := executeCommand(RootCmd, "pr", "create", "1170aeea2", "--base", "main", "--owner"); err != nil {
			t.Fatalf("gh pr create 1170aeea2 --owner failed: %v", err)
		}
		if !mockGit.HasCall("r=base_owner@example.com") {
			t.Errorf("expected r=base_owner@example.com from base commit files, got: %v", mockGit.Calls)
		}
		if mockGit.HasCall("r=head_owner@example.com") {
			t.Errorf("did not expect r=head_owner@example.com from HEAD commit files, got: %v", mockGit.Calls)
		}
	})
}
