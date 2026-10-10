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
	"encoding/json"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/andygrunwald/go-gerrit"
)

func TestParseSenseTargetArg(t *testing.T) {
	cases := []struct {
		arg      string
		wantType string
		wantID   string
	}{
		{arg: "b/315378787", wantType: "bug", wantID: "315378787"},
		{arg: "pwbug.dev/315378787", wantType: "bug", wantID: "315378787"},
		{arg: "https://issues.pigweed.dev/issues/315378787", wantType: "bug", wantID: "315378787"},
		{arg: "315378787", wantType: "bug", wantID: "315378787"},
		{arg: "pwrev/472267", wantType: "cl", wantID: "472267"},
		{arg: "https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/472267", wantType: "cl", wantID: "472267"},
		{arg: "472267", wantType: "cl", wantID: "472267"},
		{arg: "#472267", wantType: "cl", wantID: "472267"},
		{arg: "Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e", wantType: "cl", wantID: "Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e"},
		{arg: "https://chat.google.com/room/AAAA1234/2MLjLSl3rqA/USphHaWk4jk?cls=10", wantType: "chat_thread", wantID: "AAAA1234/2MLjLSl3rqA"},
		{arg: "https://mail.google.com/chat/u/0/#chat/space/AAAA1234/2MLjLSl3rqA", wantType: "chat_thread", wantID: "AAAA1234/2MLjLSl3rqA"},
		{arg: "spaces/AAAA1234/threads/2MLjLSl3rqA", wantType: "chat_thread", wantID: "AAAA1234/2MLjLSl3rqA"},
	}

	for _, tc := range cases {
		gotType, gotID := ParseSenseTargetArg(tc.arg)
		if gotType != tc.wantType || gotID != tc.wantID {
			t.Errorf("ParseSenseTargetArg(%q) = (%q, %q), want (%q, %q)",
				tc.arg, gotType, gotID, tc.wantType, tc.wantID)
		}
	}
}

func TestSense_CrankChatThread(t *testing.T) {
	gitRunner := &MockGitRunner{}
	SetupMockConfig(t, gitRunner)
	gitRunner.OnCommand("branch --show-current", "main\n")
	gitRunner.OnCommand("rev-list --left-right --count origin/main...HEAD", "0\t0\n")

	out, err := executeCommand(RootCmd, "sense", "--json", "https://chat.google.com/room/AAAA1234/2MLjLSl3rqA/USphHaWk4jk?cls=10")
	if err != nil {
		t.Fatalf("gh sense chat thread failed: %v\nOutput: %s", err, out)
	}

	var report SenseReport
	if err := json.Unmarshal([]byte(out), &report); err != nil {
		t.Fatalf("failed to parse SenseReport JSON: %v\nOutput: %s", err, out)
	}

	if report.Modality != "CRANK_CHAT_THREAD" {
		t.Errorf("Modality = %q, want CRANK_CHAT_THREAD", report.Modality)
	}
	if report.Target == nil {
		t.Fatal("expected Target to be populated")
	}
	if report.Target.TargetType != "chat_thread" ||
		report.Target.ChatSpaceID != "AAAA1234" ||
		report.Target.ChatThreadID != "2MLjLSl3rqA" ||
		report.Target.ChatMessageID != "USphHaWk4jk" {
		t.Errorf("unexpected Target chat fields: %+v", report.Target)
	}
}

func TestSense_DriveActiveCL_UnresolvedAndSelfDraftsAndFailingChecks(t *testing.T) {
	server := NewMockGerritServer(t)
	activeChange := gerrit.ChangeInfo{
		ChangeID:        "Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e",
		Number:          472267,
		Project:         "pigweed/pigweed",
		Subject:         "pw_rpc: Fix channel packet framing",
		Status:          "NEW",
		Branch:          "main",
		CurrentRevision: "rev2",
		Revisions: map[string]gerrit.RevisionInfo{
			"rev2": {Number: 2},
		},
		Labels: map[string]gerrit.LabelInfo{
			"Commit-Queue": {
				All: []gerrit.ApprovalInfo{
					{Value: 1},
				},
			},
		},
	}
	server.OnJSON("GET", "/changes/Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e*", http.StatusOK, activeChange)
	server.OnJSON("GET", "/changes/*/comments", http.StatusOK, map[string][]gerrit.CommentInfo{
		"pw_rpc/channel.cc": {
			{
				ID:         "c1",
				Line:       88,
				PatchSet:   2,
				Unresolved: boolPtr(true),
				Author:     gerrit.AccountInfo{Email: "reviewer@example.com"},
				Message:    "Check buffer bounds before copying.",
			},
		},
	})
	server.OnJSON("GET", "/changes/*/drafts", http.StatusOK, map[string][]gerrit.CommentInfo{
		"pw_rpc/channel_test.cc": {
			{
				ID:         "d_self",
				Line:       42,
				PatchSet:   2,
				Unresolved: boolPtr(true),
				Message:    "Add negative test for zero-length packet here.",
			},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "870001",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "try",
					"builder": "pigweed-linux-bazel",
				},
				"status": "FAILURE",
			},
			{
				"id": "870002",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "try",
					"builder": "pigweed-linux-gn",
				},
				"status": "STARTED",
			},
		},
	})

	gitRunner := &MockGitRunner{}
	SetupMockConfig(t, gitRunner)
	gitRunner.OnCommand("branch --show-current", "b-315378787-fix-rpc\n")
	gitRunner.OnCommand("rev-parse HEAD", "rev2\n")
	gitRunner.OnCommand("log -1 --format=%B HEAD", "pw_rpc: Fix channel packet framing\n\nBug: b/315378787\nChange-Id: Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e\n")
	gitRunner.OnCommand("rev-list --left-right --count origin/main...HEAD", "25\t1\n")
	gitRunner.OnCommand("merge-tree --write-tree HEAD origin/main", "deadbeef1234567890\n")
	gitRunner.OnCommand("status --porcelain", "")

	out, err := executeCommand(RootCmd, "sense", "--json")
	if err != nil {
		t.Fatalf("gh sense --json failed: %v\nOutput: %s", err, out)
	}

	var report SenseReport
	if err := json.Unmarshal([]byte(out), &report); err != nil {
		t.Fatalf("failed to parse SenseReport JSON: %v\nOutput: %s", err, out)
	}

	if report.Modality != "DRIVE_ACTIVE_CL" {
		t.Errorf("Modality = %q, want DRIVE_ACTIVE_CL", report.Modality)
	}
	if !report.Git.IsStale || report.Git.CommitsBehind != 25 {
		t.Errorf("expected Git.IsStale=true and CommitsBehind=25, got %+v", report.Git)
	}
	if report.Git.RebaseVerdict != "REBASE_RECOMMENDED" {
		t.Errorf("expected RebaseVerdict=REBASE_RECOMMENDED when merge-tree is clean and a check failed, got %q", report.Git.RebaseVerdict)
	}
	// When checks are passing/pending with zero failures, computeRebaseVerdict must return REBASE_SKIP_KEEP_CQ
	if v, _ := computeRebaseVerdict(report.Git, &SenseCLState{Status: "NEW", Mergeable: true, PassingChecksCount: 10}); v != "REBASE_SKIP_KEEP_CQ" {
		t.Errorf("expected computeRebaseVerdict=REBASE_SKIP_KEEP_CQ when checks are green, got %q", v)
	}
	if report.ActiveCL == nil {
		t.Fatal("expected ActiveCL to be populated")
	}
	if !report.ActiveCL.HeadUploadedToGerrit {
		t.Errorf("expected HeadUploadedToGerrit=true when HEAD matches Gerrit revision")
	}
	if len(report.ActiveCL.ExternalUnresolvedThreads) != 1 {
		t.Errorf("expected 1 external unresolved thread, got %d", len(report.ActiveCL.ExternalUnresolvedThreads))
	}
	if len(report.ActiveCL.AuthorSelfDrafts) != 1 {
		t.Errorf("expected 1 author self-draft note, got %d", len(report.ActiveCL.AuthorSelfDrafts))
	}
	if len(report.ActiveCL.FailingChecks) != 1 || report.ActiveCL.FailingChecks[0].Name != "pigweed-linux-bazel" {
		t.Errorf("expected failing check pigweed-linux-bazel, got %+v", report.ActiveCL.FailingChecks)
	}
	if report.ActiveCL.PendingChecksCount != 1 {
		t.Errorf("expected PendingChecksCount=1, got %d", report.ActiveCL.PendingChecksCount)
	}

	// Verify default output (without --json) is compact human/LLM directive text, not raw JSON.
	outCompact, err := executeCommand(RootCmd, "sense")
	if err != nil {
		t.Fatalf("gh sense compact failed: %v", err)
	}
	if strings.HasPrefix(strings.TrimSpace(outCompact), "{") {
		t.Errorf("expected default gh sense output to be compact text, got JSON:\n%s", outCompact)
	}
	for _, want := range []string{
		"Modality: DRIVE_ACTIVE_CL",
		"Rebase:   REBASE_RECOMMENDED",
		"Unresolved Threads (1):",
		"Author Self-Drafts (1):",
	} {
		if !strings.Contains(outCompact, want) {
			t.Errorf("expected compact output to contain %q, got:\n%s", want, outCompact)
		}
	}
}

func TestSense_CrankBug_WithParkedWorktree_AndComments(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	srv.Install(t)
	srv.SeedIssue(&BuganizerIssue{
		IssueID: 315378787,
		State: BuganizerState{
			Status:   "ASSIGNED",
			Priority: "P1",
			Title:    "pw_rpc: Fix channel packet framing",
		},
	}, "Issue description.")

	gitRunner := &MockGitRunner{}
	SetupMockConfig(t, gitRunner)
	gitRunner.OnCommand("branch --show-current", "main\n")
	gitRunner.OnCommand("rev-list --left-right --count origin/main...HEAD", "0\t0\n")

	mockWT := &mockWorkspaceIntegration{
		enabled: true,
		issueStatuses: map[int64]WorkspaceIssueStatus{
			315378787: {
				ProjectName: "b-315378787-fix-rpc",
				Residency:   "PARKED",
				Branch:      "b-315378787-fix-rpc",
				SymlinkPath: "/tmp/projects/b-315378787-fix-rpc",
			},
		},
	}
	prevWT := RegisteredWorkspaceIntegration
	RegisteredWorkspaceIntegration = mockWT
	defer func() { RegisteredWorkspaceIntegration = prevWT }()

	out, err := executeCommand(RootCmd, "sense", "--json", "b/315378787")
	if err != nil {
		t.Fatalf("gh sense --json b/315378787 failed: %v\nOutput: %s", err, out)
	}

	var report SenseReport
	if err := json.Unmarshal([]byte(out), &report); err != nil {
		t.Fatalf("failed to parse SenseReport JSON: %v\nOutput: %s", err, out)
	}

	if report.Modality != "CRANK_BUG" {
		t.Errorf("Modality = %q, want CRANK_BUG", report.Modality)
	}
	if report.Target == nil || report.Target.Issue == nil || report.Target.Issue.Number != 315378787 {
		t.Fatalf("expected Target.Issue #315378787, got %+v", report.Target)
	}
	if report.Target.ExistingWorktree == nil || report.Target.ExistingWorktree.Residency != "PARKED" {
		t.Errorf("expected Target.ExistingWorktree to report PARKED project, got %+v", report.Target.ExistingWorktree)
	}
}

func TestSense_AdoptCL_And_StatusCard(t *testing.T) {
	server := NewMockGerritServer(t)
	targetChange := gerrit.ChangeInfo{
		ChangeID:        "Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e",
		Number:          472267,
		Project:         "pigweed/pigweed",
		Subject:         "pw_string: Add InlineString constructor",
		Status:          "NEW",
		Branch:          "main",
		CurrentRevision: "rev1",
		Revisions: map[string]gerrit.RevisionInfo{
			"rev1": {Number: 1},
		},
	}
	server.OnJSON("GET", "/changes/472267*", http.StatusOK, targetChange)
	server.OnJSON("GET", "/changes/*/comments", http.StatusOK, map[string]any{})
	server.OnJSON("GET", "/changes/*/drafts", http.StatusOK, map[string]any{})

	gitRunner := &MockGitRunner{}
	SetupMockConfig(t, gitRunner)
	gitRunner.OnCommand("branch --show-current", "main\n")
	gitRunner.OnCommand("rev-list --left-right --count origin/main...HEAD", "0\t0\n")

	out, err := executeCommand(RootCmd, "sense", "pwrev/472267")
	if err != nil {
		t.Fatalf("gh sense pwrev/472267 failed: %v\nOutput: %s", err, out)
	}

	for _, want := range []string{
		"Modality: ADOPT_CL",
		"pw_string: Add InlineString constructor",
		"Target:   type=cl  id=472267",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("expected status card to contain %q, got:\n%s", want, out)
		}
	}

	// When the target CL is already checked out on the current branch, `gh sense <cl>`
	// should classify as DRIVE_ACTIVE_CL rather than prompting to check it out again,
	// and deduplicate Target.CL (storing only in ActiveCL).
	gitRunner.OnCommand("branch --show-current", "inline-string-ctor\n")
	gitRunner.OnCommand("log -1 --format=%B HEAD", "pw_string: Add InlineString constructor\n\nChange-Id: Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e\n")
	gitRunner.OnCommand("rev-list --left-right --count origin/main...HEAD", "0\t1\n")
	server.OnJSON("GET", "/changes/Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e*", http.StatusOK, targetChange)

	outCheckedOut, err := executeCommand(RootCmd, "sense", "pwrev/472267", "--json")
	if err != nil {
		t.Fatalf("gh sense pwrev/472267 --json failed: %v\nOutput: %s", err, outCheckedOut)
	}
	var repCheckedOut SenseReport
	if err := json.Unmarshal([]byte(outCheckedOut), &repCheckedOut); err != nil {
		t.Fatalf("failed to parse SenseReport JSON: %v", err)
	}
	if repCheckedOut.Modality != "DRIVE_ACTIVE_CL" {
		t.Errorf("Modality = %q, want DRIVE_ACTIVE_CL when target CL is already checked out", repCheckedOut.Modality)
	}
	if repCheckedOut.Target != nil && repCheckedOut.Target.CL != nil {
		t.Errorf("expected Target.CL to be deduplicated (nil) when identical to ActiveCL")
	}
}

func TestSense_ResumeConflict_UploadWIP_And_OncallTriage(t *testing.T) {
	tmpDir := t.TempDir()
	gitDir := filepath.Join(tmpDir, ".git")
	if err := os.MkdirAll(filepath.Join(gitDir, "rebase-merge"), 0755); err != nil {
		t.Fatal(err)
	}

	gitRunner := &MockGitRunner{}
	SetupMockConfig(t, gitRunner)
	MockCWD = tmpDir
	gitRunner.OnCommand("branch --show-current", "feature-rebase\n")
	gitRunner.OnCommand("rev-parse --git-dir", gitDir+"\n")

	// 1. Rebase in progress -> RESUME_CONFLICT_RESOLUTION
	outRebase, err := executeCommand(RootCmd, "sense", "--json")
	if err != nil {
		t.Fatalf("gh sense failed: %v", err)
	}
	var repRebase SenseReport
	if err := json.Unmarshal([]byte(outRebase), &repRebase); err != nil {
		t.Fatal(err)
	}
	if repRebase.Modality != "RESUME_CONFLICT_RESOLUTION" {
		t.Errorf("Modality = %q, want RESUME_CONFLICT_RESOLUTION", repRebase.Modality)
	}

	// Remove rebase-merge and test UPLOAD_LOCAL_WIP
	_ = os.RemoveAll(filepath.Join(gitDir, "rebase-merge"))
	gitRunner.OnCommand("status --porcelain", " M pw_string/string.cc\n")
	gitRunner.OnCommand("rev-list --left-right --count origin/main...HEAD", "0\t0\n")

	outWIP, err := executeCommand(RootCmd, "sense", "--json")
	if err != nil {
		t.Fatalf("gh sense failed: %v", err)
	}
	var repWIP SenseReport
	if err := json.Unmarshal([]byte(outWIP), &repWIP); err != nil {
		t.Fatal(err)
	}
	if repWIP.Modality != "UPLOAD_LOCAL_WIP" {
		t.Errorf("Modality = %q, want UPLOAD_LOCAL_WIP", repWIP.Modality)
	}

	// Clean tree on main with oncall schedule -> ONCALL_TRIAGE
	gitRunner.OnCommand("status --porcelain", "")
	gitRunner.OnCommand("branch --show-current", "main\n")
	gitRunner.OnCommand("worktree list --porcelain", "worktree /repo/main\nHEAD abc\nbranch refs/heads/main\n\nworktree /repo/wt2\nHEAD def\nbranch refs/heads/wt2\n")

	schedFile := filepath.Join(tmpDir, "oncall.cfg")
	if err := os.WriteFile(schedFile, []byte("primary: \"testuser\"\nsecondary: \"backupuser\"\n"), 0644); err != nil {
		t.Fatal(err)
	}
	t.Setenv("USER", "testuser")
	t.Setenv("GH_ISH_ONCALL_FILE", schedFile)

	outOncall, err := executeCommand(RootCmd, "sense", "--json")
	if err != nil {
		t.Fatalf("gh sense failed: %v", err)
	}
	var repOncall SenseReport
	if err := json.Unmarshal([]byte(outOncall), &repOncall); err != nil {
		t.Fatal(err)
	}
	if repOncall.Modality != "ONCALL_TRIAGE" {
		t.Errorf("Modality = %q, want ONCALL_TRIAGE", repOncall.Modality)
	}
	if !repOncall.Oncall.IsUserOncall || repOncall.Oncall.Role != "primary" {
		t.Errorf("expected IsUserOncall=true (primary), got %+v", repOncall.Oncall)
	}
	if !repOncall.Worktree.IsUnmanagedGitWorktree || len(repOncall.Worktree.GitWorktrees) != 2 {
		t.Errorf("expected IsUnmanagedGitWorktree=true with 2 git worktrees, got %+v", repOncall.Worktree)
	}
}

func TestCanSafelyPrepareWorktree_NonClobberingSafetyGates(t *testing.T) {
	defaultTarget := &SenseTargetState{
		TargetType:   "bug",
		NormalizedID: "315378787",
		Issue: &SenseIssueInfo{
			Number: 315378787,
			Title:  "pw_rpc: Fix channel packet framing",
		},
	}

	// Gate 1: Dirty working tree -> BLOCKED
	t.Run("Gate1_DirtyTree", func(t *testing.T) {
		status, reason := CanSafelyPrepareWorktree(
			SenseGitState{Branch: "main", IsDirty: true, DirtyFilesCount: 2},
			nil,
			defaultTarget,
			"/repo",
			nil,
		)
		if status != "BLOCKED" || !strings.Contains(reason, "GATE_1_DIRTY_TREE") {
			t.Errorf("expected BLOCKED with GATE_1_DIRTY_TREE, got status=%q reason=%q", status, reason)
		}
	})

	// Gate 2: Rebase in progress -> BLOCKED
	t.Run("Gate2_RebaseInProgress", func(t *testing.T) {
		status, reason := CanSafelyPrepareWorktree(
			SenseGitState{Branch: "feature", RebaseInProgress: true},
			nil,
			defaultTarget,
			"/repo",
			nil,
		)
		if status != "BLOCKED" || !strings.Contains(reason, "GATE_2_GIT_OP_IN_PROGRESS") {
			t.Errorf("expected BLOCKED with GATE_2_GIT_OP_IN_PROGRESS, got status=%q reason=%q", status, reason)
		}
	})

	// Gate 3a: Unpushed local commit without Change-Id -> BLOCKED
	t.Run("Gate3_UnuploadedLocalCommits", func(t *testing.T) {
		status, reason := CanSafelyPrepareWorktree(
			SenseGitState{Branch: "local-wip", CommitsAhead: 1},
			nil,
			defaultTarget,
			"/repo",
			nil,
		)
		if status != "BLOCKED" || !strings.Contains(reason, "GATE_3_UNPUSHED_COMMITS") {
			t.Errorf("expected BLOCKED with GATE_3_UNPUSHED_COMMITS, got status=%q reason=%q", status, reason)
		}
	})

	// Gate 3b: Active open CL -> BLOCKED
	t.Run("Gate3_ActiveOpenCL", func(t *testing.T) {
		status, reason := CanSafelyPrepareWorktree(
			SenseGitState{Branch: "open-cl-branch", CommitsAhead: 1, ChangeID: "I12345"},
			&SenseCLState{Number: 498000, Status: "NEW", Subject: "pw_foo: WIP change", HeadUploadedToGerrit: true},
			defaultTarget,
			"/repo",
			nil,
		)
		if status != "BLOCKED" || !strings.Contains(reason, "GATE_3_ACTIVE_OPEN_CL") {
			t.Errorf("expected BLOCKED with GATE_3_ACTIVE_OPEN_CL, got status=%q reason=%q", status, reason)
		}
	})

	// Gate 3c: Merged CL, but local HEAD was amended after merge (not in Gerrit revisions) -> BLOCKED
	t.Run("Gate3_MergedCL_LocalCommitNotUploaded", func(t *testing.T) {
		status, reason := CanSafelyPrepareWorktree(
			SenseGitState{Branch: "merged-cl-branch", CommitsAhead: 1, HeadSHA: "unpushed_amend_sha", ChangeID: "I12345"},
			&SenseCLState{Number: 498000, Status: "MERGED", Subject: "pw_foo: Merged change", HeadUploadedToGerrit: false},
			defaultTarget,
			"/repo",
			nil,
		)
		if status != "BLOCKED" || !strings.Contains(reason, "GATE_3_LOCAL_SHA_NOT_UPLOADED") {
			t.Errorf("expected BLOCKED with GATE_3_LOCAL_SHA_NOT_UPLOADED, got status=%q reason=%q", status, reason)
		}
	})

	// Gate 5: Target already tracked in another worktree -> BLOCKED
	t.Run("Gate5_TrackedInOtherWorktree", func(t *testing.T) {
		targetOtherWT := &SenseTargetState{
			TargetType:   "bug",
			NormalizedID: "315378787",
			ExistingWorktree: &WorkspaceProjectMatch{
				Project:     "b-315378787-fix-rpc",
				Residency:   "ACTIVE",
				SymlinkPath: "/repo/wt-other",
			},
		}
		status, reason := CanSafelyPrepareWorktree(
			SenseGitState{Branch: "main", CommitsAhead: 0},
			nil,
			targetOtherWT,
			"/repo/wt-current",
			nil,
		)
		if status != "BLOCKED" || !strings.Contains(reason, "GATE_5_OTHER_WORKTREE") {
			t.Errorf("expected BLOCKED with GATE_5_OTHER_WORKTREE, got status=%q reason=%q", status, reason)
		}
	})

	// Allowed Case A: Clean on main (0 commits ahead) -> READY
	t.Run("Safe_CleanOnMain", func(t *testing.T) {
		status, reason := CanSafelyPrepareWorktree(
			SenseGitState{Branch: "main", CommitsAhead: 0},
			nil,
			defaultTarget,
			"/repo",
			nil,
		)
		if status != "READY" || reason != "" {
			t.Errorf("expected READY on clean main, got status=%q reason=%q", status, reason)
		}
	})

	// Allowed Case B: Clean on MERGED CL branch where local HEAD SHA is verified in Gerrit revisions -> READY
	t.Run("Safe_MergedCLWithVerifiedUploadedSHA", func(t *testing.T) {
		status, reason := CanSafelyPrepareWorktree(
			SenseGitState{Branch: "merged-cl-branch", CommitsAhead: 1, HeadSHA: "verified_patchset_sha", ChangeID: "I12345"},
			&SenseCLState{Number: 498000, Status: "MERGED", Subject: "pw_foo: Merged change", HeadUploadedToGerrit: true},
			defaultTarget,
			"/repo",
			nil,
		)
		if status != "READY" || reason != "" {
			t.Errorf("expected READY on verified MERGED CL branch, got status=%q reason=%q", status, reason)
		}
	})

	// Gate 7a: Closed bug target -> BLOCKED
	t.Run("Gate7_ClosedBugTarget", func(t *testing.T) {
		closedBug := &SenseTargetState{
			TargetType:   "bug",
			NormalizedID: "571616997",
			Issue:        &SenseIssueInfo{Number: 571616997, State: "CLOSED", Title: "Already fixed"},
		}
		status, reason := CanSafelyPrepareWorktree(
			SenseGitState{Branch: "main", CommitsAhead: 0},
			nil,
			closedBug,
			"/repo",
			nil,
		)
		if status != "BLOCKED" || !strings.Contains(reason, "GATE_7_BUG_ALREADY_CLOSED") {
			t.Errorf("expected BLOCKED with GATE_7_BUG_ALREADY_CLOSED, got status=%q reason=%q", status, reason)
		}
	})
}

func TestClassifySenseModality_BugAndTargetEdgeCases(t *testing.T) {
	// 1. Closed bug recommends punting rather than creating a duplicate CL
	mod, conf, summary, steps, gates := classifySenseModality(
		SenseGitState{Branch: "main"},
		SenseWorktreeState{},
		nil,
		&SenseTargetState{
			TargetType:   "bug",
			NormalizedID: "571616997",
			Issue:        &SenseIssueInfo{Number: 571616997, State: "CLOSED", Title: "pw_build: iOS"},
		},
		SenseOncallState{},
		0,
	)
	if mod != "CRANK_BUG" || conf != "HIGH" || !strings.Contains(summary, "already CLOSED") {
		t.Errorf("unexpected closed bug classification: mod=%s conf=%s summary=%s", mod, conf, summary)
	}
	if len(gates) == 0 || gates[0] != "GATE_7_BUG_ALREADY_CLOSED" || len(steps) == 0 || !strings.Contains(steps[0], "already CLOSED") {
		t.Errorf("expected GATE_7_BUG_ALREADY_CLOSED and punt step, got gates=%v steps=%v", gates, steps)
	}

	// 2. Open bug when already on the bug's active open CL routes directly to DRIVE_ACTIVE_CL
	activeBugCL := &SenseCLState{
		Number:             498953,
		Status:             "NEW",
		Subject:            "pw_async2: Make Sender const",
		PassingChecksCount: 42,
		ChecksSummary:      "42 passing",
	}
	mod2, _, _, _, _ := classifySenseModality(
		SenseGitState{Branch: "b-469150426", CommitsAhead: 1, BugID: "b/469150426"},
		SenseWorktreeState{},
		activeBugCL,
		&SenseTargetState{
			TargetType:   "bug",
			NormalizedID: "469150426",
			Issue: &SenseIssueInfo{
				Number: 469150426,
				State:  "OPEN",
				Title:  "Make Sender const",
				RelatedCLs: []SenseRelatedCL{
					{Number: 498953, Status: "NEW", Subject: "pw_async2: Make Sender const"},
				},
			},
		},
		SenseOncallState{},
		0,
	)
	if mod2 != "DRIVE_ACTIVE_CL" {
		t.Errorf("expected DRIVE_ACTIVE_CL when already on the bug's open CL, got %s", mod2)
	}

	// 3. Non-existent CL target sets LOW confidence and GATE_7_TARGET_NOT_FOUND
	mod3, conf3, _, _, gates3 := classifySenseModality(
		SenseGitState{Branch: "main"},
		SenseWorktreeState{},
		nil,
		&SenseTargetState{
			TargetType:   "cl",
			NormalizedID: "999999999",
			NotFound:     true,
		},
		SenseOncallState{},
		0,
	)
	if mod3 != "ADOPT_CL" || conf3 != "LOW" || len(gates3) == 0 || gates3[0] != "GATE_7_TARGET_NOT_FOUND" {
		t.Errorf("expected LOW confidence and GATE_7_TARGET_NOT_FOUND for missing CL, got mod=%s conf=%s gates=%v", mod3, conf3, gates3)
	}
}

func TestSense_MultiProjectAndShortlinkNeutrality(t *testing.T) {
	defer SetCustomShortlinks(nil)

	// 1. Built-in Fuchsia shortlinks work in ParseSenseTargetArg.
	for _, arg := range []string{"fxrev/888777", "fxr/888777", "https://fuchsia-review.googlesource.com/c/fuchsia/+/888777"} {
		targetType, normID := ParseSenseTargetArg(arg)
		if targetType != "cl" || normID != "888777" {
			t.Errorf("ParseSenseTargetArg(%q) = (%q, %q), want (cl, 888777)", arg, targetType, normID)
		}
	}

	// 2. Custom .ghish.toml shortlinks work in ParseSenseTargetArg.
	SetCustomShortlinks(map[string]string{
		"customrev/": "https://custom-review.googlesource.com",
	})
	targetType, normID := ParseSenseTargetArg("customrev/424242")
	if targetType != "cl" || normID != "424242" {
		t.Errorf("ParseSenseTargetArg(customrev/424242) = (%q, %q), want (cl, 424242)", targetType, normID)
	}

	// 3. Running `gh sense --profile fuchsia` with custom git config remote/branch uses upstream/trunk, fxrev/, and fxb/.
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/accounts/self*", http.StatusOK, gerrit.AccountInfo{
		AccountID: 1001,
		Name:      "Fuchsia Author",
		Email:     "author@fuchsia.dev",
	})
	server.OnJSON("GET", "/changes/Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e*", http.StatusOK, gerrit.ChangeInfo{
		ID:              "fuchsia~trunk~Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e",
		Project:         "fuchsia",
		Branch:          "trunk",
		ChangeID:        "Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e",
		Subject:         "[zircon] Fix handle table race",
		Status:          "NEW",
		Number:          888777,
		CurrentRevision: "rev1",
		Owner: gerrit.AccountInfo{
			Name:  "Fuchsia Author",
			Email: "author@fuchsia.dev",
		},
		Revisions: map[string]gerrit.RevisionInfo{
			"rev1": {Number: 1},
		},
	})
	server.OnJSON("GET", "/changes/*/comments", http.StatusOK, map[string][]gerrit.CommentInfo{})
	server.OnJSON("GET", "/changes/*/drafts", http.StatusOK, map[string][]gerrit.CommentInfo{})

	gitRunner := &MockGitRunner{}
	SetupMockConfig(t, gitRunner)
	gitRunner.OnCommand("config --get-regexp ^ghish\\.", "ghish.gerrit.remote upstream\nghish.gerrit.defaultbranch trunk\nghish.issue.prefix fxb/\n")
	gitRunner.OnCommand("branch --show-current", "b-987654-handle-race\n")
	gitRunner.OnCommand("rev-parse HEAD", "rev1\n")
	gitRunner.OnCommand("rev-list --left-right --count upstream/trunk...HEAD", "25\t1\n")
	gitRunner.OnCommand("merge-tree --write-tree HEAD upstream/trunk", "deadbeef1234567890\n")
	gitRunner.OnCommand("log -1 --format=%B HEAD", "[zircon] Fix handle table race\n\nBug: 987654\nChange-Id: Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e\n")
	gitRunner.OnCommand("status --porcelain", "")

	outJSON, err := executeCommand(RootCmd, "sense", "--profile", "fuchsia", "--json")
	if err != nil {
		t.Fatalf("gh sense --profile fuchsia --json failed: %v\nOutput: %s", err, outJSON)
	}
	var report SenseReport
	if err := json.Unmarshal([]byte(outJSON), &report); err != nil {
		t.Fatalf("failed to parse SenseReport JSON: %v\nOutput: %s", err, outJSON)
	}
	if report.Git.BaseRef != "upstream/trunk" {
		t.Errorf("expected BaseRef=upstream/trunk, got %q", report.Git.BaseRef)
	}
	if !report.Git.IsStale || report.Git.CommitsBehind != 25 || report.Git.BugID != "fxb/987654" {
		t.Errorf("expected IsStale=true, CommitsBehind=25, BugID=fxb/987654 against upstream/trunk, got %+v", report.Git)
	}
	card := FormatSenseStatusCard(&report)
	if !strings.Contains(card, "upstream/trunk") || !strings.Contains(card, "888777") {
		t.Errorf("expected status card to reference upstream/trunk and 888777, got:\n%s", card)
	}
	if strings.Contains(card, "origin/main") || strings.Contains(card, "pwrev/") {
		t.Errorf("expected status card not to contain hardcoded origin/main or pwrev/, got:\n%s", card)
	}
}

func TestSense_NoSilentFailures(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/accounts/self*", http.StatusOK, gerrit.AccountInfo{
		AccountID: 1001,
		Email:     "dev@example.com",
	})
	server.OnJSON("GET", "/changes/*", http.StatusInternalServerError, map[string]string{
		"error": "internal Gerrit storage error",
	})

	gitRunner := &MockGitRunner{}
	SetupMockConfig(t, gitRunner)
	gitRunner.OnCommand("branch --show-current", "main\n")
	gitRunner.OnCommand("rev-parse HEAD", "deadbeef\n")
	gitRunner.OnCommand("rev-list --left-right --count origin/main...HEAD", "0\t0\n")
	gitRunner.OnCommand("config --get-regexp ^ghish\\.", "ghish.oncall.schedulefile /nonexistent/path/to/oncall_schedule.json\n")

	// 1. Invalid --profile fails fast.
	_, err := executeCommand(RootCmd, "sense", "--profile", "unknown-nonexistent-profile")
	if err == nil || !strings.Contains(err.Error(), "unknown profile") {
		t.Errorf("expected fail-fast error for invalid profile, got %v", err)
	}

	// 2. HTTP 500 on target CL sets FetchError and Warnings (NOT NotFound: true).
	outJSON, err := executeCommand(RootCmd, "sense", "--profile", "pigweed", "--json", "pwrev/555666")
	if err != nil {
		t.Fatalf("gh sense --json pwrev/555666 failed: %v\nOutput: %s", err, outJSON)
	}
	var report SenseReport
	if err := json.Unmarshal([]byte(outJSON), &report); err != nil {
		t.Fatalf("failed to parse SenseReport JSON: %v\nOutput: %s", err, outJSON)
	}
	if report.Target == nil {
		t.Fatalf("expected non-nil Target")
	}
	if report.Target.NotFound {
		t.Errorf("expected Target.NotFound=false on HTTP 500 error")
	}
	if report.Target.FetchError == "" {
		t.Errorf("expected non-empty Target.FetchError on HTTP 500 error")
	}
	if len(report.Warnings) == 0 {
		t.Errorf("expected warning for Gerrit 500 error, got %v", report.Warnings)
	}
	card := FormatSenseStatusCard(&report)
	if !strings.Contains(card, "Warnings:") || !strings.Contains(card, "Failed to fetch Gerrit CL 555666") {
		t.Errorf("expected status card to display Warnings and Failed to fetch Gerrit CL 555666, got:\n%s", card)
	}

	// 3. Missing oncall schedule file on bare `gh sense` surfaces a warning instead of failing silently.
	outOncallJSON, err := executeCommand(RootCmd, "sense", "--profile", "pigweed", "--json")
	if err != nil {
		t.Fatalf("gh sense --json failed: %v\nOutput: %s", err, outOncallJSON)
	}
	var oncallReport SenseReport
	if err := json.Unmarshal([]byte(outOncallJSON), &oncallReport); err != nil {
		t.Fatalf("failed to parse SenseReport JSON: %v", err)
	}
	if len(oncallReport.Warnings) == 0 || !strings.Contains(oncallReport.Warnings[0], "oncall schedule") {
		t.Errorf("expected warning for missing oncall schedule file, got %v", oncallReport.Warnings)
	}
}

func TestCanSafelyPrepareWorktree_SymlinkVsPhysicalPath(t *testing.T) {
	tmpDir := t.TempDir()
	physSlot := filepath.Join(tmpDir, "slots", "pw-01")
	projDir := filepath.Join(tmpDir, "projects")
	if err := os.MkdirAll(physSlot, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(projDir, 0o755); err != nil {
		t.Fatal(err)
	}
	symlinkPath := filepath.Join(projDir, "my-proj")
	if err := os.Symlink(physSlot, symlinkPath); err != nil {
		t.Fatal(err)
	}

	target := &SenseTargetState{
		TargetType:   "bug",
		NormalizedID: "315378787",
		ExistingWorktree: &WorkspaceProjectMatch{
			Project:     "my-proj",
			Residency:   "MOUNTED",
			SymlinkPath: symlinkPath,
		},
	}
	gitWTs := []SenseGitWorktreeEntry{
		{Path: physSlot, Branch: "b-315378787"},
	}

	// Navigating via symlinkPath must recognize physSlot in gitWTs as the same worktree (READY, not GATE_5_OTHER_GIT_WORKTREE).
	status, reason := CanSafelyPrepareWorktree(
		SenseGitState{Branch: "main", CommitsAhead: 0},
		nil,
		target,
		symlinkPath,
		gitWTs,
	)
	if status != "READY" {
		t.Errorf("expected READY when cwd is symlinkPath and wt.Path is physSlot, got status=%q reason=%q", status, reason)
	}

	// Navigating via physSlot must recognize symlinkPath in target.ExistingWorktree as the same worktree (READY, not GATE_5_OTHER_WORKTREE).
	status2, reason2 := CanSafelyPrepareWorktree(
		SenseGitState{Branch: "main", CommitsAhead: 0},
		nil,
		target,
		physSlot,
		gitWTs,
	)
	if status2 != "READY" {
		t.Errorf("expected READY when cwd is physSlot and ExistingWorktree.SymlinkPath is symlinkPath, got status=%q reason=%q", status2, reason2)
	}
}

func TestSense_NoWorktreePoolAssumptionsWhenUnmanaged(t *testing.T) {
	dirtyGit := SenseGitState{Branch: "feature", IsDirty: true, DirtyFilesCount: 1}
	bugTarget := &SenseTargetState{
		TargetType:   "bug",
		NormalizedID: "315378787",
		Issue:        &SenseIssueInfo{Number: 315378787, State: "OPEN", Title: "Fix bug"},
	}

	// 1. Standard Git repo (no ./gh wt pool configured) should NOT recommend `./gh wt use` or print `project=(unmanaged)`.
	_, _, _, stepsUnmanaged, _ := classifySenseModality(dirtyGit, SenseWorktreeState{}, nil, bugTarget, SenseOncallState{}, 0)
	if len(stepsUnmanaged) == 0 || strings.Contains(stepsUnmanaged[0], "gh wt use") {
		t.Errorf("expected standard Git checkout not to recommend `./gh wt use`, got steps=%v", stepsUnmanaged)
	}
	cardUnmanaged := FormatSenseStatusCard(&SenseReport{
		Modality:   "CRANK_BUG",
		Confidence: "HIGH",
		Summary:    "Bug",
		Git:        dirtyGit,
		Worktree:   SenseWorktreeState{},
	})
	if strings.Contains(cardUnmanaged, "project=(unmanaged)") || strings.Contains(cardUnmanaged, "slot=-") {
		t.Errorf("expected standard Git checkout status card not to print project=(unmanaged) or slot=-, got:\n%s", cardUnmanaged)
	}

	// 2. Pool-managed checkout (`TotalSlots > 0`) SHOULD recommend `./gh wt use` and print `Worktree: project=...`.
	poolWT := SenseWorktreeState{CurrentProject: "my-proj", CurrentSlot: "slot-1", TotalSlots: 2}
	_, _, _, stepsManaged, _ := classifySenseModality(dirtyGit, poolWT, nil, bugTarget, SenseOncallState{}, 0)
	if len(stepsManaged) == 0 || !strings.Contains(stepsManaged[0], "gh wt use") {
		t.Errorf("expected pool-managed checkout to recommend `./gh wt use`, got steps=%v", stepsManaged)
	}
	cardManaged := FormatSenseStatusCard(&SenseReport{
		Modality:   "CRANK_BUG",
		Confidence: "HIGH",
		Summary:    "Bug",
		Git:        dirtyGit,
		Worktree:   poolWT,
	})
	if !strings.Contains(cardManaged, "Worktree: project=my-proj  slot=slot-1") {
		t.Errorf("expected pool-managed status card to print project and slot, got:\n%s", cardManaged)
	}
}
