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
	mockGit := NewMockGit(t).WithBranch("main")
	SetTestProfile(t, "pigweed")

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
