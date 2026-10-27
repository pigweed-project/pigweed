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
	"strings"
	"testing"
	"time"
)

func TestPushIntegration(t *testing.T) {
	mockGit := NewMockGit(t).WithBranch("feature-branch").
		OnCommand("push origin HEAD:refs/for/feature-branch", "Counting objects: 5, done.\nDelta compression using up to 8 threads.\nWriting objects: 100% (5/5), 1.20 KiB | 1.20 MiB/s, done.\nTotal 5 (delta 3), reused 0 (delta 0)\nremote: Resolving deltas: 100% (3/3)\nremote: Processing changes: refs: 1, done\nremote:   https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/396894 pw_ghish: Add UI\n")

	output, err := executeCommand(RootCmd, "pr", "push")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	wantOutput := "Pushing patchset for branch feature-branch..."
	if !strings.Contains(output, wantOutput) {
		t.Errorf("pr push output got = %q, want it to contain = %q", output, wantOutput)
	}
	if !strings.Contains(output, "✓ Patchset pushed successfully.") {
		t.Errorf("pr push output got = %q, want receipt", output)
	}
	if !strings.Contains(output, "Branch: feature-branch") {
		t.Errorf("pr push output got = %q, want Branch detail", output)
	}
	if !strings.Contains(output, "Next:   gh pr checks --watch") {
		t.Errorf("pr push output got = %q, want Next hint", output)
	}
	if !strings.Contains(output, "https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/396894") {
		t.Errorf("pr push output got = %q, want extracted Gerrit URL", output)
	}
	for _, noisy := range []string{"Counting objects:", "Delta compression", "Writing objects:", "remote: Resolving deltas:", "remote: Processing changes:"} {
		if strings.Contains(output, noisy) {
			t.Errorf("expected noisy git progress line %q to be filtered from output:\n%s", noisy, output)
		}
	}

	wantPush := "push origin HEAD:refs/for/feature-branch"
	if !mockGit.HasCall(wantPush) {
		t.Errorf("Expected git calls to contain %q, calls: %v", wantPush, mockGit.Calls)
	}
}

func TestPush_RichOptions(t *testing.T) {
	SetTestProfile(t, "pigweed")
	server := NewMockGerritServer(t)
	server.OnDefaultChange(472267, WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit"))
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

	_, err := executeCommand(RootCmd, "pr", "push", "--auto", "--force")
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

// Pushing a commit Gerrit has never seen (with --force) creates the change, so
// there are no labels on it yet; the project knows what they will be called.
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

	if _, err := executeCommand(RootCmd, "pr", "push", "--auto", "--force"); err != nil {
		t.Fatalf("Command failed: %v", err)
	}

	if !mockGit.HasCall("l=Auto-Submit+1") {
		t.Errorf("Expected push to vote the project's Auto-Submit label, calls: %v", mockGit.Calls)
	}
	if mockGit.HasCall("l=Pigweed-Auto-Submit+1") {
		t.Errorf("Expected push not to vote the profile's label when the project names its own, calls: %v", mockGit.Calls)
	}
}

func TestPush_NewChangeWithoutForceError(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/*", http.StatusOK, []map[string]any{})

	mockGit := NewMockGit(t).WithBranch("main").WithCommit("pw_ghish: Tighten docs\n\nChange-Id: If37a935da438c64782b46a95dea392d18d405c6b\n")

	_, err := executeCommand(RootCmd, "pr", "push")
	if err == nil {
		t.Fatal("Expected error when pushing a commit whose Change-Id does not exist on Gerrit")
	}
	for _, want := range []string{
		"no existing Gerrit change found",
		"If37a935da438c64782b46a95dea392d18d405c6b",
		"git commit --amend -m",
		"gh pr create",
		"--force",
	} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("Expected error to contain %q, got: %v", want, err)
		}
	}
	if mockGit.HasCall("push") {
		t.Errorf("Expected git push not to be called when Change-Id is missing on Gerrit, calls: %v", mockGit.Calls)
	}
}

func TestPush_ClobberedChangeIDDetectedFromReflog(t *testing.T) {
	newChangeID := "Ie8e44c008972d0001ce6e5df1a67db460765ea37"
	oldChangeID := "If37a935da438c64782b46a95dea392d18d405c6b"

	server := NewMockGerritServer(t)
	server.On("GET", "/changes/*", func(w http.ResponseWriter, r *http.Request) {
		q := r.URL.Query().Get("q")
		if q == oldChangeID {
			server.RespondJSON(w, http.StatusOK, []map[string]any{
				DefaultMockChange(488949, WithChangeID(oldChangeID)),
			})
			return
		}
		server.RespondJSON(w, http.StatusOK, []map[string]any{})
	})

	mockGit := NewMockGit(t).WithBranch("main").
		WithCommit("pw_ghish: Tighten docs\n\nChange-Id: "+newChangeID+"\n").
		OnCommand("log -1 --format=%B HEAD@{1}", "pw_ghish: Tighten docs\n\nChange-Id: "+oldChangeID+"\n")

	_, err := executeCommand(RootCmd, "pr", "push")
	if err == nil {
		t.Fatal("Expected error when HEAD's Change-Id was clobbered")
	}
	for _, want := range []string{
		"no existing Gerrit change found",
		newChangeID,
		"HEAD@{1}",
		oldChangeID,
		"#488949",
	} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("Expected error to contain %q, got:\n%v", want, err)
		}
	}
	if mockGit.HasCall("push") {
		t.Errorf("Expected git push not to be called when Change-Id was clobbered, calls: %v", mockGit.Calls)
	}
}

func TestPush_NewChangeWithForceSucceeds(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/*", http.StatusOK, []map[string]any{})

	mockGit := NewMockGit(t).WithBranch("main").WithCommit("pw_ghish: Tighten docs\n\nChange-Id: If37a935da438c64782b46a95dea392d18d405c6b\n")

	output, err := executeCommand(RootCmd, "pr", "push", "--force")
	if err != nil {
		t.Fatalf("Expected --force to succeed, got: %v", err)
	}
	if !mockGit.HasCall("push") {
		t.Errorf("Expected git push to be called with --force, calls: %v", mockGit.Calls)
	}
	if !strings.Contains(output, "✓ Patchset pushed successfully.") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestPush_Stack_MissingChangeInMiddleFails(t *testing.T) {
	id1 := "I1111111111111111111111111111111111111111"
	id2 := "I2222222222222222222222222222222222222222"

	server := NewMockGerritServer(t)
	server.On("GET", "/changes/*", func(w http.ResponseWriter, r *http.Request) {
		q := r.URL.Query().Get("q")
		if q == id2 {
			server.RespondJSON(w, http.StatusOK, []map[string]any{
				DefaultMockChange(488949, WithChangeID(id2)),
			})
			return
		}
		server.RespondJSON(w, http.StatusOK, []map[string]any{})
	})

	stackLog := fmt.Sprintf(
		"aaa1111\x00pw_ghish: First commit\x00pw_ghish: First commit\n\nChange-Id: %s\n\x1e"+
			"bbb2222\x00pw_ghish: Second commit\x00pw_ghish: Second commit\n\nChange-Id: %s\n\x1e",
		id1, id2,
	)
	mockGit := NewMockGit(t).WithBranch("main").
		WithCommit("pw_ghish: Second commit\n\nChange-Id: "+id2+"\n").
		OnCommand("rev-list --count origin/main..HEAD", "2\n").
		OnCommand("log --reverse --format=%h%x00%s%x00%B%x1e origin/main..HEAD", stackLog)

	_, err := executeCommand(RootCmd, "pr", "push", "--base", "main", "--stack")
	if err == nil {
		t.Fatal("Expected error when an earlier commit in the stack does not match any Gerrit change")
	}
	for _, want := range []string{"aaa1111", id1, "gh pr create --stack"} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("Expected error to mention %q, got: %v", want, err)
		}
	}
	if mockGit.HasCall("push") {
		t.Errorf("Expected no git push when stack verification fails, calls: %v", mockGit.Calls)
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
		state, err := VerifyHeadForPush(context.Background(), RootCmd, cfg, true, false)
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
		_, err := VerifyHeadForPush(context.Background(), RootCmd, cfg, false, false)
		if err == nil || !strings.Contains(err.Error(), "#123") {
			t.Errorf("expected GitHub issue syntax error, got: %v", err)
		}
	})

	t.Run("rejects multiple Change-Id footers", func(t *testing.T) {
		mockGit := NewMockGit(t).WithBranch("main").WithCommit(
			"Subject\n\nChange-Id: I1111111111111111111111111111111111111111\nChange-Id: I2222222222222222222222222222222222222222\n",
		)

		cfg := &Config{Git: mockGit}
		SetConfig(RootCmd, cfg)
		_, err := VerifyHeadForPush(context.Background(), RootCmd, cfg, false, false)
		if err == nil || !strings.Contains(err.Error(), "multiple Change-Id footers") {
			t.Errorf("expected multiple Change-Id footers error, got: %v", err)
		}
	})
}

func TestPush_TopicForbiddenBySubmitRequirement(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(472267, func(ch map[string]any) {
		ch["submit_requirements"] = []map[string]any{
			{"name": "Topics-Not-Supported", "status": "SATISFIED"},
		}
	})
	mockGit := NewMockGit(t).WithBranch("main")

	_, err := executeCommand(RootCmd, "pr", "push", "--topic", "my-topic")
	if err == nil {
		t.Fatal("Expected error when --topic is passed on a change with Topics-Not-Supported submit requirement")
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

func TestPush_Nightly_WithDryRunCQ(t *testing.T) {
	SetTestProfile(t, "pigweed")
	server := NewMockGerritServer(t)
	server.OnDefaultChange(123456, WithChangeID("I0000000000000000000000000000000000000001"), WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit"))
	server.OnJSON("POST", "/changes/123456/flows", http.StatusCreated, map[string]any{})

	mockGit := NewMockGit(t).WithBranch("main")

	output, err := executeCommand(RootCmd, "pr", "push", "--cq", "--nightly")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if !mockGit.HasCall("push origin HEAD:refs/for/main%l=Commit-Queue+1") {
		t.Errorf("Expected push with Commit-Queue+1 dry run, got calls: %v", mockGit.Calls)
	}
	if server.CallCount("POST", "/changes/123456/flows") != 1 {
		t.Fatalf("Expected POST /changes/123456/flows to be called once, got %d", server.CallCount("POST", "/changes/123456/flows"))
	}
	wantOut := "Created submit flow for Change 123456 (scheduled for tomorrow at 02:36 in America/Los_Angeles)."
	if !strings.Contains(output, wantOut) {
		t.Errorf("Output got %q, want %q", output, wantOut)
	}
}

func TestPush_SubmitAt(t *testing.T) {
	SetTestProfile(t, "pigweed")
	server := NewMockGerritServer(t)
	server.OnDefaultChange(123456, WithChangeID("I0000000000000000000000000000000000000001"), WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit"))
	server.OnJSON("POST", "/changes/123456/flows", http.StatusCreated, map[string]any{})

	mockGit := NewMockGit(t).WithBranch("main")

	output, err := executeCommand(RootCmd, "pr", "push", "--submit-at", "22:15")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if !mockGit.HasCall("push origin HEAD:refs/for/main") {
		t.Errorf("Expected git push to be called, got calls: %v", mockGit.Calls)
	}
	if server.CallCount("POST", "/changes/123456/flows") != 1 {
		t.Fatalf("Expected POST /changes/123456/flows to be called once, got %d", server.CallCount("POST", "/changes/123456/flows"))
	}
	wantOut := "Created submit flow for Change 123456 (scheduled for today at 22:15 in America/Los_Angeles)."
	if !strings.Contains(output, wantOut) {
		t.Errorf("Output got %q, want %q", output, wantOut)
	}
}

func TestPush_ScheduledFlagConflicts(t *testing.T) {
	NewMockGit(t).WithBranch("main")

	tests := []struct {
		name    string
		args    []string
		wantErr string
	}{
		{
			name:    "nightly and submit-at",
			args:    []string{"pr", "push", "--nightly", "--submit-at", "20:00"},
			wantErr: "cannot specify both --nightly and --submit-at",
		},
		{
			name:    "nightly and auto",
			args:    []string{"pr", "push", "--nightly", "--auto"},
			wantErr: "cannot specify both --auto and --nightly/--submit-at",
		},
		{
			name:    "nightly and cq=2",
			args:    []string{"pr", "push", "--nightly", "--cq", "2"},
			wantErr: "cannot specify both --cq=2 and --nightly/--submit-at",
		},
		{
			name:    "nightly and draft",
			args:    []string{"pr", "push", "--nightly", "--draft"},
			wantErr: "cannot specify both --draft and --nightly/--submit-at",
		},
		{
			name:    "submit-at and draft",
			args:    []string{"pr", "push", "--submit-at", "20:00", "--draft"},
			wantErr: "cannot specify both --draft and --nightly/--submit-at",
		},
		{
			name:    "invalid submit-at",
			args:    []string{"pr", "push", "--submit-at", "25:00"},
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

// A flow failure after a successful push must not read as a failed push: the
// patchset is on Gerrit, and pushing again would only risk duplicate flows.
func TestPush_Nightly_FlowFailureIsNotAPushFailure(t *testing.T) {
	SetTestProfile(t, "pigweed")
	server := NewMockGerritServer(t)
	server.OnDefaultChange(123456, WithChangeID("I0000000000000000000000000000000000000001"), WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit"))
	server.OnString("POST", "/changes/123456/flows", http.StatusInternalServerError, "text/plain", "flows disabled")

	mockGit := NewMockGit(t).WithBranch("main")

	_, err := executeCommand(RootCmd, "pr", "push", "--submit-at", "22:15")
	if err == nil {
		t.Fatal("expected an error when flow creation fails, got nil")
	}
	if !mockGit.HasCall("push origin HEAD:refs/for/main") {
		t.Fatalf("expected the push to have happened, got calls: %v", mockGit.Calls)
	}
	var flowErr *ScheduleFlowError
	if !errors.As(err, &flowErr) {
		t.Fatalf("expected a *ScheduleFlowError, got %T: %v", err, err)
	}
	for _, want := range []string{"patchset pushed, but scheduling the submit flow failed", "pr merge 123456 --submit-at 22:15"} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("error missing %q, got: %v", want, err)
		}
	}
	if strings.Contains(err.Error(), "error pushing patchset") {
		t.Errorf("error must not claim the push failed, got: %v", err)
	}
}

// When the change already exists, whether flows are enabled is checked before
// pushing, so a host without flows never ends up with a pushed patchset and a
// missing schedule.
func TestPush_Nightly_FlowsNotEnabledStopsBeforePush(t *testing.T) {
	for _, tc := range []struct {
		name   string
		status int
	}{
		{"reported disabled", http.StatusOK},
		{"no flow service", http.StatusMethodNotAllowed},
	} {
		t.Run(tc.name, func(t *testing.T) {
			SetTestProfile(t, "pigweed")
			server := NewMockGerritServer(t)
			server.OnDefaultChange(123456, WithChangeID("I0000000000000000000000000000000000000001"), WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit"))
			server.OnFlowsEnabled(123456, tc.status, false)
			server.OnJSON("POST", "/changes/123456/flows", http.StatusCreated, map[string]any{})

			mockGit := NewMockGit(t).WithBranch("main")

			_, err := executeCommand(RootCmd, "pr", "push", "--nightly")
			var notEnabled *FlowsNotEnabledError
			if !errors.As(err, &notEnabled) {
				t.Fatalf("expected a *FlowsNotEnabledError, got %T: %v", err, err)
			}
			var flowErr *ScheduleFlowError
			if errors.As(err, &flowErr) {
				t.Errorf("nothing was pushed, so the error must not say the push succeeded, got: %v", err)
			}
			if mockGit.HasCall("push") {
				t.Errorf("expected git push not to be called, calls: %v", mockGit.Calls)
			}
			if n := server.CallCount("POST", "/changes/123456/flows"); n != 0 {
				t.Errorf("expected no flows to be created, got %d", n)
			}
		})
	}
}

// stackScheduleFixture serves a two-change stack: bottom change 111111 and
// HEAD change 123456. failTop makes creating the top change's flow fail.
func stackScheduleFixture(t *testing.T, failTop bool) (*MockGerritServer, *MockGitRunner) {
	t.Helper()
	const bottomID = "I1111111111111111111111111111111111111111"
	const topID = "I2222222222222222222222222222222222222222"
	labels := WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit")

	server := NewMockGerritServer(t)
	// Registered first so Change-Id queries return the matching change
	// rather than whichever OnDefaultChange registered first.
	server.On("GET", "/changes/", func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Query().Get("q") {
		case bottomID:
			server.RespondJSON(w, http.StatusOK, []map[string]any{DefaultMockChange(111111, WithChangeID(bottomID), labels)})
		case topID:
			server.RespondJSON(w, http.StatusOK, []map[string]any{DefaultMockChange(123456, WithChangeID(topID), labels)})
		default:
			server.RespondJSON(w, http.StatusOK, []map[string]any{})
		}
	})
	server.OnDefaultChange(111111, WithChangeID(bottomID), labels)
	server.OnDefaultChange(123456, WithChangeID(topID), labels)
	server.OnJSON("POST", "/changes/111111/flows", http.StatusCreated, map[string]any{})
	if failTop {
		server.OnString("POST", "/changes/123456/flows", http.StatusInternalServerError, "text/plain", "flows disabled")
	} else {
		server.OnJSON("POST", "/changes/123456/flows", http.StatusCreated, map[string]any{})
	}

	stackLog := fmt.Sprintf(
		"aaa1111\x00pw_ghish: Bottom\x00pw_ghish: Bottom\n\nChange-Id: %s\n\x1e"+
			"bbb2222\x00pw_ghish: Top\x00pw_ghish: Top\n\nChange-Id: %s\n\x1e",
		bottomID, topID,
	)
	mockGit := NewMockGit(t).WithBranch("main").
		WithCommit("pw_ghish: Top\n\nChange-Id: "+topID+"\n").
		OnCommand("rev-list --count origin/main..HEAD", "2\n").
		OnCommand("log --reverse --format=%h%x00%s%x00%B%x1e origin/main..HEAD", stackLog)
	return server, mockGit
}

// With --stack --nightly, the bottom change gets its own nightly slot and each
// change above it 30 minutes later, rather than each taking the slot of its
// own change number.
func TestPush_Stack_Nightly_StaggersChanges(t *testing.T) {
	SetTestProfile(t, "pigweed")
	server, mockGit := stackScheduleFixture(t, false)

	output, err := executeCommand(RootCmd, "pr", "push", "--base", "main", "--stack", "--nightly")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}
	if !mockGit.HasCall("push") {
		t.Fatalf("expected a push, got calls: %v", mockGit.Calls)
	}
	for _, tc := range []struct {
		num  int
		when string
	}{
		{111111, "tomorrow at 00:51"}, // 111111 % 360 = 231 minutes after 21:00.
		{123456, "tomorrow at 01:21"}, // 30 minutes after the bottom change.
	} {
		path := fmt.Sprintf("/changes/%d/flows", tc.num)
		if n := server.CallCount("POST", path); n != 1 {
			t.Errorf("POST %s called %d times, want 1", path, n)
		}
		want := fmt.Sprintf("Created submit flow for Change %d (scheduled for %s in America/Los_Angeles).", tc.num, tc.when)
		if !strings.Contains(output, want) {
			t.Errorf("output missing %q, got: %s", want, output)
		}
	}
}

// With --stack --submit-at, the bottom change gets the requested time and each
// change above it 30 minutes later.
func TestPush_Stack_SubmitAt_StaggersChanges(t *testing.T) {
	SetTestProfile(t, "pigweed")
	_, _ = stackScheduleFixture(t, false)

	output, err := executeCommand(RootCmd, "pr", "push", "--base", "main", "--stack", "--submit-at", "23:50")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}
	for _, want := range []string{
		"Created submit flow for Change 111111 (scheduled for today at 23:50 in America/Los_Angeles).",
		// Wraps past midnight; 00:20 has already passed today.
		"Created submit flow for Change 123456 (scheduled for tomorrow at 00:20 in America/Los_Angeles).",
	} {
		if !strings.Contains(output, want) {
			t.Errorf("output missing %q, got: %s", want, output)
		}
	}
}

// When "now" falls between the bottom change's time and the next one's, every
// change still goes to the same day as the bottom change, so the stack keeps
// its order.
func TestPush_Stack_SubmitAt_KeepsOrderAcrossNow(t *testing.T) {
	SetTestProfile(t, "pigweed")
	origNow := scheduleNowFn
	t.Cleanup(func() { scheduleNowFn = origNow })
	scheduleNowFn = func() time.Time {
		return time.Date(2026, 10, 1, 4, 45, 0, 0, time.UTC) // 21:45 PDT
	}
	server, _ := stackScheduleFixture(t, false)

	output, err := executeCommand(RootCmd, "pr", "push", "--base", "main", "--stack", "--submit-at", "21:30")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}
	for _, want := range []string{
		"Created submit flow for Change 111111 (scheduled for tomorrow at 21:30 in America/Los_Angeles).",
		"Created submit flow for Change 123456 (scheduled for tomorrow at 22:00 in America/Los_Angeles).",
	} {
		if !strings.Contains(output, want) {
			t.Errorf("output missing %q, got: %s", want, output)
		}
	}
	var topBody string
	for _, req := range server.Requests() {
		if req.Method == "POST" && req.Path == "/changes/123456/flows" {
			topBody = string(req.Body)
		}
	}
	if !strings.Contains(topBody, "wait until tomorrow at 22:00 in America/Los_Angeles") {
		t.Errorf("top change's flow request = %s, want it to wait until tomorrow at 22:00", topBody)
	}
}

// If one change in the stack fails to schedule, the error says how far it got
// and gives retry commands only for the changes still unscheduled.
func TestPush_Stack_PartialFailure(t *testing.T) {
	for _, tc := range []struct {
		name      string
		args      []string
		wantRetry string
	}{
		// The bottom change's slot (00:51) is known by then, so the top
		// change's retry pins its staggered time.
		{"nightly", []string{"--nightly"}, "pr merge I2222222222222222222222222222222222222222 --submit-at 01:21"},
		{"submit-at", []string{"--submit-at", "20:00"}, "pr merge I2222222222222222222222222222222222222222 --submit-at 20:30"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			SetTestProfile(t, "pigweed")
			_, _ = stackScheduleFixture(t, true)

			args := append([]string{"pr", "push", "--base", "main", "--stack"}, tc.args...)
			_, err := executeCommand(RootCmd, args...)
			if err == nil {
				t.Fatal("expected an error when the top change's flow fails, got nil")
			}
			var flowErr *ScheduleFlowError
			if !errors.As(err, &flowErr) {
				t.Fatalf("expected a ScheduleFlowError, got %T: %v", err, err)
			}
			for _, want := range []string{"1 of 2 changes in the stack were scheduled", tc.wantRetry} {
				if !strings.Contains(err.Error(), want) {
					t.Errorf("error missing %q, got: %v", want, err)
				}
			}
			if strings.Contains(err.Error(), "pr merge I1111111111111111111111111111111111111111") {
				t.Errorf("error must not ask to retry the change that was scheduled, got: %v", err)
			}
		})
	}
}

func TestPush_CommitMessageFlags_EndToEnd(t *testing.T) {
	const changeID = "I1111111111111111111111111111111111111111"

	t.Run("amends message, emits post-amend warnings once, and skips no-op amend on retry", func(t *testing.T) {
		SetTestProfile(t, "pigweed")
		repo := newTestGitRepo(t)
		repo.InstallCommitMsgHook()
		repo.InstallGerritRefHooks()
		repo.BindToCLI()

		repo.WriteFile("base.txt", "patchset 1\n")
		draftLongSubject := "pw_foo: Draft subject intentionally exceeding seventy-two columns before amend"
		if _, err := repo.Client.CommitNewAll(t.Context(), draftLongSubject+"\n\nInitial body.\n\nBug: None\n", false, io.Discard, io.Discard); err != nil {
			t.Fatal(err)
		}
		ps1SHA := repo.GitOutput("rev-parse", "HEAD")

		// Stage a hunk and modify working tree to ensure push only rewrites message.
		repo.StageFile("staged.txt", "keep staged\n")
		origIndex := repo.GitOutput("write-tree")
		repo.WriteFile("base.txt", "unstaged edit\n")

		currentRev := ps1SHA
		server := NewMockGerritServer(t)
		server.On("GET", "/changes/", func(w http.ResponseWriter, r *http.Request) {
			ch := DefaultMockChange(123456, WithChangeID(changeID), WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit"))
			ch["current_revision"] = currentRev
			server.RespondJSON(w, http.StatusOK, []map[string]any{ch})
		})
		server.OnJSON("POST", "/changes/"+changeID+"/revisions/current/review", http.StatusOK, map[string]any{})

		bodyInput := strings.NewReader("Updated body paragraph.\n")
		out, err := executeCommandWithStdin(RootCmd, bodyInput, "push", "-t", "pw_foo: Polished title", "-F", "-", "--fixed", "b/567763970")
		if err != nil {
			t.Fatalf("gh push failed: %v\nOutput: %s", err, out)
		}
		if strings.Contains(out, "exceeds 72-character limit") {
			t.Errorf("expected post-amend commit message warnings without stale pre-amend warning, got:\n%s", out)
		}
		if gotCalls := server.CallCount("GET", "/changes/"); gotCalls != 1 {
			t.Errorf("GET /changes/ count = %d, want 1", gotCalls)
		}

		ps2SHA := repo.GitOutput("rev-parse", "HEAD")
		if ps2SHA == ps1SHA {
			t.Fatal("expected HEAD SHA to advance after commit message edit")
		}
		pushedMsg := repo.BareGitOutput("log", "-1", "--format=%B", "refs/changes/latest")
		wantMsg := "pw_foo: Polished title\n\nUpdated body paragraph.\n\nFixed: b/567763970\nChange-Id: " + changeID
		if strings.TrimSpace(pushedMsg) != wantMsg {
			t.Errorf("pushed message =\n%s\nwant:\n%s", pushedMsg, wantMsg)
		}
		if gotIndex := repo.GitOutput("write-tree"); gotIndex != origIndex {
			t.Errorf("staged index altered: %s != %s", gotIndex, origIndex)
		}
		if gotWorktree := repo.ReadFile("base.txt"); gotWorktree != "unstaged edit\n" {
			t.Errorf("unstaged worktree altered: %q", gotWorktree)
		}

		// Re-running with identical commit-message flag + --cq must NOT amend again;
		// it applies CQ via REST because HEAD SHA matches CurrentRevision.
		currentRev = ps2SHA
		if out, err := executeCommand(RootCmd, "pr", "push", "--fixed", "567763970", "--cq"); err != nil {
			t.Fatalf("idempotent retry with --cq failed: %v\nOutput: %s", err, out)
		}
		if gotSHA := repo.GitOutput("rev-parse", "HEAD"); gotSHA != ps2SHA {
			t.Errorf("idempotent retry unexpectedly re-amended HEAD: got %s, want %s", gotSHA, ps2SHA)
		}
		if n := server.CallCount("POST", "/changes/"+changeID+"/revisions/current/review"); n != 1 {
			t.Errorf("expected REST review vote on unchanged SHA %s, got %d calls", ps2SHA, n)
		}
	})

	t.Run("targets non-main Gerrit branch without --base", func(t *testing.T) {
		SetTestProfile(t, "pigweed")
		repo := newTestGitRepo(t)
		repo.InstallCommitMsgHook()
		repo.BindToCLI()

		// Advance release/v1 on origin by 1 commit so a local branch created from
		// release/v1 (without tracking upstream) is 2 commits ahead of origin/main
		// but only 1 commit ahead of origin/release/v1.
		repo.GitOutput("checkout", "-b", "release/v1")
		repo.StageFile("release.txt", "v1 base\n")
		repo.GitOutput("commit", "-m", "pw_release: Branch v1")
		repo.GitOutput("push", "origin", "release/v1")

		repo.GitOutput("checkout", "--no-track", "-b", "backport-fix", "release/v1")
		repo.WriteFile("base.txt", "backport fix\n")
		if _, err := repo.Client.CommitNewAll(t.Context(), "pw_foo: Draft backport\n", false, io.Discard, io.Discard); err != nil {
			t.Fatal(err)
		}

		server := NewMockGerritServer(t)
		server.OnJSON("GET", "/changes/", http.StatusOK, []map[string]any{
			DefaultMockChange(654321, WithChangeID(changeID), WithBranch("release/v1")),
		})

		out, err := executeCommand(RootCmd, "pr", "push", "-t", "pw_foo: Backport fix to v1", "--bug", "567763970")
		if err != nil {
			t.Fatalf("pr push to release/v1 failed: %v\nOutput: %s", err, out)
		}
		if !strings.Contains(out, "Pushing patchset for branch release/v1...") {
			t.Errorf("expected push output for branch release/v1, got:\n%s", out)
		}
		if gotCalls := server.CallCount("GET", "/changes/"); gotCalls != 1 {
			t.Errorf("GET /changes/ count = %d, want 1", gotCalls)
		}
		pushedMsg := repo.BareGitOutput("log", "-1", "--format=%B", "refs/for/release/v1")
		wantMsg := "pw_foo: Backport fix to v1\n\nBug: b/567763970\nChange-Id: " + changeID
		if strings.TrimSpace(pushedMsg) != wantMsg {
			t.Errorf("pushed message on refs/for/release/v1 =\n%s\nwant:\n%s", pushedMsg, wantMsg)
		}
	})

	t.Run("rejects when HEAD has zero unpushed commits ahead of target branch without mutating HEAD", func(t *testing.T) {
		SetTestProfile(t, "pigweed")
		repo := newTestGitRepo(t)
		repo.InstallCommitMsgHook()
		repo.BindToCLI()

		// Commit on main with a Change-Id, push to origin/main, then advance
		// origin/main by one more commit so feature sits behind origin/main (0 ahead,
		// not at remote tip so CheckAmendAllowed passes).
		repo.WriteFile("base.txt", "merged change\n")
		if _, err := repo.Client.CommitNewAll(t.Context(), "pw_foo: Merged commit\n", false, io.Discard, io.Discard); err != nil {
			t.Fatal(err)
		}
		repo.GitOutput("push", "origin", "main")
		ancestorSHA := repo.GitOutput("rev-parse", "HEAD")
		ancestorMsg := repo.GitOutput("log", "-1", "--format=%B", "HEAD")
		repo.GitOutput("branch", "stale-feature", ancestorSHA)

		repo.StageFile("upstream.txt", "next commit\n")
		repo.GitOutput("commit", "-m", "pw_other: Advance upstream")
		repo.GitOutput("push", "origin", "main")
		repo.GitOutput("checkout", "stale-feature")

		server := NewMockGerritServer(t)
		server.OnJSON("GET", "/changes/", http.StatusOK, []map[string]any{
			DefaultMockChange(777777, WithChangeID(changeID), WithBranch("main")),
		})

		_, err := executeCommand(RootCmd, "pr", "push", "-t", "pw_foo: Do not rewrite merged commit")
		if err == nil {
			t.Fatal("expected error when HEAD has 0 unpushed local commits ahead of origin/main")
		}
		for _, want := range []string{"origin/main", "gh pr create", "git commit"} {
			if !strings.Contains(err.Error(), want) {
				t.Errorf("error missing %q, got:\n%v", want, err)
			}
		}
		if gotSHA := repo.GitOutput("rev-parse", "HEAD"); gotSHA != ancestorSHA {
			t.Fatalf("HEAD SHA mutated: got %s, want %s", gotSHA, ancestorSHA)
		}
		if gotMsg := repo.GitOutput("log", "-1", "--format=%B", "HEAD"); gotMsg != ancestorMsg {
			t.Errorf("HEAD commit message mutated: got %q, want %q", gotMsg, ancestorMsg)
		}
	})

	t.Run("missing Gerrit CL on 404 or empty query leaves HEAD untouched", func(t *testing.T) {
		for _, tc := range []struct {
			name  string
			setup func(*MockGerritServer)
		}{
			{
				name:  "http 404",
				setup: func(s *MockGerritServer) { s.OnStatus(http.StatusNotFound) },
			},
			{
				name:  "empty list",
				setup: func(s *MockGerritServer) { s.OnJSON("GET", "/changes/", http.StatusOK, []map[string]any{}) },
			},
		} {
			t.Run(tc.name, func(t *testing.T) {
				SetTestProfile(t, "pigweed")
				repo := newTestGitRepo(t)
				repo.InstallCommitMsgHook()
				repo.BindToCLI()

				repo.WriteFile("base.txt", "c1\n")
				if _, err := repo.Client.CommitNewAll(t.Context(), "pw_foo: Commit 1\n", false, io.Discard, io.Discard); err != nil {
					t.Fatal(err)
				}
				origSHA := repo.GitOutput("rev-parse", "HEAD")
				origReflogCount := repo.GitOutput("rev-list", "--walk-reflogs", "--count", "HEAD")

				server := NewMockGerritServer(t)
				tc.setup(server)

				if _, err := executeCommand(RootCmd, "pr", "push", "--title", "pw_foo: Never amended"); err == nil {
					t.Fatal("expected error when no Gerrit CL exists for HEAD")
				}
				if gotSHA := repo.GitOutput("rev-parse", "HEAD"); gotSHA != origSHA {
					t.Fatalf("HEAD SHA = %s, want %s", gotSHA, origSHA)
				}
				if gotReflog := repo.GitOutput("rev-list", "--walk-reflogs", "--count", "HEAD"); gotReflog != origReflogCount {
					t.Errorf("HEAD reflog mutated (%s -> %s); missing Gerrit CL must fail before amending HEAD", origReflogCount, gotReflog)
				}
			})
		}
	})

	t.Run("rolls back amended HEAD on push failure and rejects multi-commit stack", func(t *testing.T) {
		SetTestProfile(t, "pigweed")
		repo := newTestGitRepo(t)
		repo.InstallCommitMsgHook()
		repo.InstallGerritRefHooks()
		repo.BindToCLI()

		repo.WriteFile("base.txt", "c1\n")
		if _, err := repo.Client.CommitNewAll(t.Context(), "pw_foo: Commit 1\n", false, io.Discard, io.Discard); err != nil {
			t.Fatal(err)
		}
		origSHA := repo.GitOutput("rev-parse", "HEAD")

		// Forbidden topic fails after local amend and rolls HEAD back to origSHA.
		server := NewMockGerritServer(t)
		server.OnDefaultChange(123456, WithChangeID(changeID), func(ch map[string]any) {
			ch["submit_requirements"] = []map[string]any{
				{"name": "Topics-Not-Supported", "status": "SATISFIED"},
			}
		})
		if _, err := executeCommand(RootCmd, "pr", "push", "--title", "pw_foo: Rolled back", "--topic", "forbidden"); err == nil {
			t.Fatal("expected error when topic is forbidden")
		}
		if gotSHA := repo.GitOutput("rev-parse", "HEAD"); gotSHA != origSHA {
			t.Fatalf("expected HEAD rolled back to %s, got %s", origSHA, gotSHA)
		}

		// Stack of 2 commits is rejected cleanly without mutating HEAD.
		repo.StageFile("second.txt", "c2\n")
		if _, err := repo.Client.CommitNewAll(t.Context(), "pw_foo: Commit 2\n\nChange-Id: I2222222222222222222222222222222222222222\n", false, io.Discard, io.Discard); err != nil {
			t.Fatal(err)
		}
		topSHA := repo.GitOutput("rev-parse", "HEAD")
		if _, err := executeCommand(RootCmd, "pr", "push", "--stack", "--bug", "42"); err == nil {
			t.Fatal("expected error combining --stack with commit-message flags")
		}
		if gotSHA := repo.GitOutput("rev-parse", "HEAD"); gotSHA != topSHA {
			t.Fatalf("expected stack rejection to leave HEAD at %s, got %s", topSHA, gotSHA)
		}
	})
}
