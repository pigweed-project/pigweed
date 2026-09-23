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
	"strings"
	"testing"
)

func TestPushIntegration(t *testing.T) {
	mockGit := NewMockGit(t).WithBranch("feature-branch")

	output, err := executeCommand(RootCmd, "pr", "push")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	wantOutput := "Pushing patchset for branch feature-branch..."
	if !strings.Contains(output, wantOutput) {
		t.Errorf("pr push output got = %q, want it to contain = %q", output, wantOutput)
	}

	wantPush := "push origin HEAD:refs/for/feature-branch"
	if !mockGit.HasCall(wantPush) {
		t.Errorf("Expected git calls to contain %q, calls: %v", wantPush, mockGit.Calls)
	}
}

func TestPush_RichOptions(t *testing.T) {
	SetTestProfile(t, "pigweed")
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/*", http.StatusOK, []map[string]any{})
	server.OnJSON("GET", "/projects/*", http.StatusOK, []map[string]any{
		{"name": "Code-Review"},
		{"name": "Commit-Queue"},
		{"name": "Pigweed-Auto-Submit"},
	})
	mockGit := NewMockGit(t).WithBranch("main").
		OnCommand("config --get remote.origin.url", "https://pigweed.googlesource.com/pigweed/pigweed\n")

	_, err := executeCommand(RootCmd, "pr", "push",
		"--reviewer", "alice@google.com",
		"--cc", "bob@google.com",
		"--ready",
		"--auto",
		"--cq", "1",
		"--publish",
		"-o", "topic=my-push-topic",
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
		"ready",
		"l=Pigweed-Auto-Submit+1",
		"l=Commit-Queue+1",
		"publish-comments",
		"topic=my-push-topic",
	}
	for _, expected := range expectedRefComponents {
		if !strings.Contains(pushCall, expected) {
			t.Errorf("Push call missing expected component %q: %s", expected, pushCall)
		}
	}
}

func TestPush_TopLevelAlias(t *testing.T) {
	NewMockGit(t).WithBranch("main")

	output, err := executeCommand(RootCmd, "push")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "Pushing patchset") {
		t.Errorf("Top-level push output missing expected string, got: %s", output)
	}
}

func TestPush_UploadAliases(t *testing.T) {
	NewMockGit(t).WithBranch("main")

	// Test 'pr upload'
	output, err := executeCommand(RootCmd, "pr", "upload")
	if err != nil {
		t.Fatalf("'pr upload' failed: %v\nOutput: %s", err, output)
	}
	if !strings.Contains(output, "Pushing patchset") {
		t.Errorf("'pr upload' output missing expected string, got: %s", output)
	}

	// Test top-level 'upload'
	output, err = executeCommand(RootCmd, "upload")
	if err != nil {
		t.Fatalf("'upload' failed: %v\nOutput: %s", err, output)
	}
	if !strings.Contains(output, "Pushing patchset") {
		t.Errorf("'upload' output missing expected string, got: %s", output)
	}
}

func TestPush_ErrorReturnedWhenPushFails(t *testing.T) {
	NewMockGit(t).WithBranch("main").OnError("push", fmt.Errorf("remote rejected"))

	_, err := executeCommand(RootCmd, "pr", "push")
	if err == nil {
		t.Fatal("Expected error when git push fails, got nil")
	}
	if !strings.Contains(err.Error(), "error pushing patchset") {
		t.Errorf("Expected error to mention 'error pushing patchset', got: %v", err)
	}
}

func TestPush_FailsWhenMissingChangeID(t *testing.T) {
	tempDir := t.TempDir()
	NewMockGit(t).WithBranch("main").WithCommit("Commit message without change ID\n").OnCommand("rev-parse --git-dir", tempDir+"\n")

	_, err := executeCommand(RootCmd, "pr", "push")
	if err == nil {
		t.Fatal("Expected error when Change-Id is missing, got nil")
	}
	if !strings.Contains(err.Error(), "Change-Id") {
		t.Errorf("Expected error to mention Change-Id, got: %v", err)
	}
}

func TestPush_MultiCommitStackGuard_RejectsWithoutStack(t *testing.T) {
	NewMockGit(t).WithBranch("main").OnCommand("rev-list --count origin/main..HEAD", "5\n")

	_, err := executeCommand(RootCmd, "pr", "push", "--base", "main")
	if err == nil {
		t.Fatal("Expected error when pushing 5 commits without --stack, got nil")
	}

	if !strings.Contains(err.Error(), "would push 5 commits") {
		t.Errorf("Expected error to mention 5 commits, got: %v", err)
	}
	if !strings.Contains(err.Error(), "--stack") {
		t.Errorf("Expected error to mention --stack flag, got: %v", err)
	}
}

func TestPush_MultiCommitStackGuard_AllowsWithStack(t *testing.T) {
	NewMockGit(t).WithBranch("main").OnCommand("rev-list --count origin/main..HEAD", "5\n")

	output, err := executeCommand(RootCmd, "pr", "push", "--base", "main", "--stack")
	if err != nil {
		t.Fatalf("Expected --stack to succeed, got: %v", err)
	}

	if !strings.Contains(output, "Pushing patchset for branch main...") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestPush_GerritBranchMemory(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/*", http.StatusOK, []map[string]any{
		{
			"id":        "myproj~sandbox%2Fexperiment~I0000000000000000000000000000000000000001",
			"branch":    "sandbox/experiment",
			"change_id": "I0000000000000000000000000000000000000001",
		},
	})

	mockGit := NewMockGit(t).WithBranch("local-main")

	output, err := executeCommand(RootCmd, "pr", "push")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	wantPush := "push origin HEAD:refs/for/sandbox/experiment"
	if !mockGit.HasCall(wantPush) {
		t.Errorf("Expected push to target Gerrit remembered branch 'sandbox/experiment', calls: %v", mockGit.Calls)
	}
	if !strings.Contains(output, "Pushing patchset for branch sandbox/experiment...") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestPush_ExplicitBaseOverridesGerritBranchMemory(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/*", http.StatusOK, []map[string]any{
		{
			"id":        "myproj~sandbox%2Fexperiment~I0000000000000000000000000000000000000001",
			"branch":    "sandbox/experiment",
			"change_id": "I0000000000000000000000000000000000000001",
		},
	})

	mockGit := NewMockGit(t).WithBranch("local-main")

	output, err := executeCommand(RootCmd, "pr", "push", "--base", "custom-branch")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	wantPush := "push origin HEAD:refs/for/custom-branch"
	if !mockGit.HasCall(wantPush) {
		t.Errorf("Expected push to target explicit custom-branch, calls: %v", mockGit.Calls)
	}
	if !strings.Contains(output, "Pushing patchset for branch custom-branch...") {
		t.Errorf("Unexpected output: %s", output)
	}
}

// A host whose auto-submit label is plain "Auto-Submit" must be voted under
// that name even when the profile in effect spells it Pigweed-Auto-Submit;
// Gerrit rejects a push that votes a label the project does not define.
func TestPush_AutoSubmitUsesLabelFromChange(t *testing.T) {
	SetTestProfile(t, "pigweed")
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/*", http.StatusOK, []map[string]any{
		{
			"id":        "myproj~main~I0000000000000000000000000000000000000001",
			"branch":    "main",
			"change_id": "I0000000000000000000000000000000000000001",
			"labels": map[string]any{
				"Code-Review": map[string]any{},
				"Auto-Submit": map[string]any{},
			},
		},
	})

	mockGit := NewMockGit(t).WithBranch("main")

	if _, err := executeCommand(RootCmd, "pr", "push", "--auto"); err != nil {
		t.Fatalf("Command failed: %v", err)
	}

	if !mockGit.HasCall("l=Auto-Submit+1") {
		t.Errorf("Expected push to vote the host's Auto-Submit label, calls: %v", mockGit.Calls)
	}
	if mockGit.HasCall("l=Pigweed-Auto-Submit+1") {
		t.Errorf("Expected push not to vote the profile's label when the change names its own, calls: %v", mockGit.Calls)
	}
}

// With no change to read and no remote to identify the project, there is no
// way to learn what the label is called. The profile used to answer this; it
// no longer does, because its answer was only ever right on its own host.
func TestPush_AutoSubmitWithoutAnyLabelSourceFails(t *testing.T) {
	SetTestProfile(t, "pigweed")
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/*", http.StatusOK, []map[string]any{})

	mockGit := NewMockGit(t).WithBranch("main")

	_, err := executeCommand(RootCmd, "pr", "push", "--auto")
	if err == nil {
		t.Fatal("Expected an error when the auto-submit label cannot be determined")
	}
	if mockGit.HasCall("l=Pigweed-Auto-Submit+1") {
		t.Errorf("Expected no guessed label vote, calls: %v", mockGit.Calls)
	}
	if mockGit.HasCall("push") {
		t.Errorf("Expected no push when --auto cannot be honored, calls: %v", mockGit.Calls)
	}
}

// Pushing a commit Gerrit has never seen creates the change, so there are no
// labels on it yet; the project knows what they will be called.
func TestPush_AutoSubmitUsesProjectLabelForNewChange(t *testing.T) {
	SetTestProfile(t, "pigweed")
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/*", http.StatusOK, []map[string]any{})
	server.OnJSON("GET", "/projects/*", http.StatusOK, []map[string]any{
		{"name": "Code-Review"},
		{"name": "Auto-Submit"},
	})

	mockGit := NewMockGit(t).WithBranch("main").
		OnCommand("config --get remote.origin.url", "https://example-review.googlesource.com/example/project\n")

	if _, err := executeCommand(RootCmd, "pr", "push", "--auto"); err != nil {
		t.Fatalf("Command failed: %v", err)
	}

	if !mockGit.HasCall("l=Auto-Submit+1") {
		t.Errorf("Expected push to vote the project's Auto-Submit label, calls: %v", mockGit.Calls)
	}
	if mockGit.HasCall("l=Pigweed-Auto-Submit+1") {
		t.Errorf("Expected push not to vote the profile's label when the project names its own, calls: %v", mockGit.Calls)
	}
}

// When runPush resolves a Commit-Queue+1 dry run ahead of git push and git
// then rejects with "no new changes", the REST fallback must still know the
// vote was only a dry run and not claim auto-submit was enabled.
func TestPush_NoNewChanges_AutoSubmitNoLabelDryRunsAndFails(t *testing.T) {
	SetTestProfile(t, "fuchsia")
	changeID := "I0000000000000000000000000000000000000001"
	server := NewMockGerritServer(t)
	server.OnDefaultChange(changeID, WithLabels("Code-Review", "Commit-Queue"))
	server.OnJSON("POST", "/changes/"+changeID+"/revisions/current/review", http.StatusOK, map[string]any{})

	mockGit := NewMockGit(t).WithBranch("main")
	mockGit.RunFn = func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
		if len(args) > 0 && args[0] == "push" {
			stderr.Write([]byte(" ! [remote rejected] HEAD -> refs/for/main%l=Commit-Queue+1 (no new changes)\n"))
			return fmt.Errorf("exit status 1")
		}
		return nil
	}

	output, err := executeCommand(RootCmd, "pr", "push", "--auto")
	if err == nil {
		t.Fatalf("Expected an error for a host with no auto-submit label.\nOutput: %s", output)
	}
	if !strings.Contains(err.Error(), "has no auto-submit label") {
		t.Errorf("Expected error to explain the missing auto-submit label, got: %v", err)
	}
	if strings.Contains(err.Error(), "failed to apply metadata updates") {
		t.Errorf("Expected error not to claim metadata updates failed when Commit-Queue+1 succeeded, got: %v", err)
	}
	if strings.Contains(output, "Auto-submit enabled") {
		t.Errorf("Output claims auto-submit was enabled when it was not: %s", output)
	}
	if !strings.Contains(output, "Commit-Queue+1 set.") {
		t.Errorf("Expected output to report Commit-Queue+1 set, got: %s", output)
	}
}

func TestVerifyHeadForPush(t *testing.T) {
	t.Run("valid commit with existing change lookup", func(t *testing.T) {
		server := NewMockGerritServer(t)
		server.OnDefaultChange(472267, WithBranch("sandbox/feature"))
		mockGit := NewMockGit(t).WithBranch("main").WithCommit("Subject\n\nChange-Id: I0000000000000000000000000000000000000001\n")

		cfg := &Config{Host: server.URL, Git: mockGit}
		SetConfig(RootCmd, cfg)
		state, err := VerifyHeadForPush(context.Background(), RootCmd, cfg, true)
		if err != nil {
			t.Fatalf("VerifyHeadForPush failed: %v", err)
		}
		if state.ChangeID != "I0000000000000000000000000000000000000001" {
			t.Errorf("got ChangeID %q, want I0000000000000000000000000000000000000001", state.ChangeID)
		}
		if state.ExistingChange == nil || state.ExistingChange.Branch != "sandbox/feature" {
			t.Errorf("got ExistingChange %+v, want branch sandbox/feature", state.ExistingChange)
		}
	})

	t.Run("rejects GitHub issue syntax", func(t *testing.T) {
		mockGit := NewMockGit(t).WithBranch("main").WithCommit("Subject\n\nFixes #123\nChange-Id: I0000000000000000000000000000000000000001\n")

		cfg := &Config{Git: mockGit}
		SetConfig(RootCmd, cfg)
		_, err := VerifyHeadForPush(context.Background(), RootCmd, cfg, false)
		if err == nil || !strings.Contains(err.Error(), "#123") {
			t.Errorf("expected GitHub issue syntax error, got: %v", err)
		}
	})
}
