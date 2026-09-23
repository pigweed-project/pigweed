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
	"strings"
	"testing"
)

type mockWorkspaceIntegration struct {
	enabled       bool
	pathIssues    map[string]int64
	issueStatuses map[int64]WorkspaceIssueStatus
	developedIDs  []int64
}

func (m *mockWorkspaceIntegration) IsEnabled() bool {
	return m.enabled
}

func (m *mockWorkspaceIntegration) ResolveIssueIDForPath(cwd string) (int64, bool) {
	if id, ok := m.pathIssues[cwd]; ok && id > 0 {
		return id, true
	}
	return 0, false
}

func (m *mockWorkspaceIntegration) FindWorkspaceForIssue(issueID int64) (WorkspaceIssueStatus, bool) {
	if st, ok := m.issueStatuses[issueID]; ok {
		return st, true
	}
	return WorkspaceIssueStatus{}, false
}

func (m *mockWorkspaceIntegration) DevelopIssueInWorktree(ctx context.Context, issueID int64, title string, customBranch string) (string, string, error) {
	m.developedIDs = append(m.developedIDs, issueID)
	branch := customBranch
	if branch == "" {
		branch = SlugifyBranchName(issueID, title)
	}
	return "/mock/projects/" + branch, "pw-02", nil
}

func TestExtractIssueIDFromBranchName(t *testing.T) {
	cases := []struct {
		branch string
		wantID int64
		wantOK bool
	}{
		{branch: "b-315378787-fix-rpc-framing", wantID: 315378787, wantOK: true},
		{branch: "b-12345-short-slug", wantID: 12345, wantOK: true},
		{branch: "issue-315378787", wantID: 315378787, wantOK: true},
		{branch: "issue/315378787-test", wantID: 315378787, wantOK: true},
		{branch: "b/315378787", wantID: 315378787, wantOK: true},
		{branch: "315378787-fix-rpc", wantID: 315378787, wantOK: true},
		{branch: "main", wantID: 0, wantOK: false},
		{branch: "wt-manager", wantID: 0, wantOK: false},
		{branch: "feature-v2", wantID: 0, wantOK: false},
		{branch: "123-too-short-without-prefix", wantID: 0, wantOK: false},
	}

	for _, tc := range cases {
		gotID, gotOK := ExtractIssueIDFromBranchName(tc.branch)
		if gotOK != tc.wantOK || gotID != tc.wantID {
			t.Errorf("ExtractIssueIDFromBranchName(%q) = (%d, %v), want (%d, %v)",
				tc.branch, gotID, gotOK, tc.wantID, tc.wantOK)
		}
	}
}

func TestResolveTargetIssueID_ZeroCommitGap_BranchFallback(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	srv.Install(t)
	srv.SeedIssue(&BuganizerIssue{
		IssueID: 315378787,
		State: BuganizerState{
			Status: "ASSIGNED",
			Title:  "pw_rpc: Fix channel packet framing",
		},
	}, "Description of 315378787.")

	gitRunner := &MockGitRunner{}
	SetupMockConfig(t, gitRunner)

	// Simulate fresh branch where HEAD has NO Bug: trailer yet (or is still at origin/main)
	gitRunner.OnCommand("log -1 --format=%B HEAD", "Initial commit on main\n")
	gitRunner.OnCommand("branch --show-current", "b-315378787-fix-rpc-framing\n")
	gitRunner.OnCommand("rev-parse --abbrev-ref HEAD", "b-315378787-fix-rpc-framing\n")

	out, err := executeCommand(RootCmd, "issue", "view")
	if err != nil {
		t.Fatalf("expected 'gh issue view' to resolve 315378787 from branch name when HEAD has no trailer, got error: %v", err)
	}
	if !strings.Contains(out, "#315378787") {
		t.Errorf("expected output for issue #315378787, got:\n%s", out)
	}
}

func TestResolveTargetIssueID_ZeroCommitGap_WorktreeHookFallback(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	srv.Install(t)
	srv.SeedIssue(&BuganizerIssue{
		IssueID: 315378787,
		State: BuganizerState{
			Status: "ASSIGNED",
			Title:  "pw_rpc: Fix channel packet framing",
		},
	}, "Description of 315378787.")

	gitRunner := &MockGitRunner{}
	SetupMockConfig(t, gitRunner)
	MockCWD = "/home/user/wrk/projects/custom-proj"

	mockWT := &mockWorkspaceIntegration{
		enabled: true,
		pathIssues: map[string]int64{
			"/home/user/wrk/projects/custom-proj": 315378787,
		},
	}
	prevWT := RegisteredWorkspaceIntegration
	RegisteredWorkspaceIntegration = mockWT
	defer func() { RegisteredWorkspaceIntegration = prevWT }()

	// Neither HEAD commit nor branch name contains the bug ID
	gitRunner.OnCommand("log -1 --format=%B HEAD", "Initial commit\n")
	gitRunner.OnCommand("branch --show-current", "custom-proj\n")
	gitRunner.OnCommand("rev-parse --abbrev-ref HEAD", "custom-proj\n")

	out, err := executeCommand(RootCmd, "issue", "view")
	if err != nil {
		t.Fatalf("expected 'gh issue view' to resolve 315378787 via WorkspaceIntegration hook, got error: %v", err)
	}
	if !strings.Contains(out, "#315378787") {
		t.Errorf("expected output for issue #315378787, got:\n%s", out)
	}
}

func TestIssueDevelop_WithWorktreeFlag_InvokesHook(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	srv.Install(t)
	srv.SeedIssue(&BuganizerIssue{
		IssueID: 315378787,
		State: BuganizerState{
			Status: "ASSIGNED",
			Title:  "pw_rpc: Fix channel packet framing",
		},
	}, "Description.")

	gitRunner := &MockGitRunner{}
	SetupMockConfig(t, gitRunner)

	mockWT := &mockWorkspaceIntegration{enabled: true}
	prevWT := RegisteredWorkspaceIntegration
	RegisteredWorkspaceIntegration = mockWT
	defer func() { RegisteredWorkspaceIntegration = prevWT }()

	out, err := executeCommand(RootCmd, "issue", "develop", "315378787", "--worktree")
	if err != nil {
		t.Fatalf("issue develop --worktree failed: %v\nOutput: %s", err, out)
	}
	if len(mockWT.developedIDs) != 1 || mockWT.developedIDs[0] != 315378787 {
		t.Errorf("expected DevelopIssueInWorktree to be called with 315378787, got %v", mockWT.developedIDs)
	}
	if !strings.Contains(out, "pw-02") || !strings.Contains(out, "/mock/projects/b-315378787") {
		t.Errorf("expected output to mention slot pw-02 and worktree path, got:\n%s", out)
	}
}

func TestIssueStatus_AnnotatesWorktreeResidency(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	srv.Install(t)
	srv.SeedIssue(&BuganizerIssue{
		IssueID: 315378787,
		State: BuganizerState{
			Status:   "ASSIGNED",
			Priority: "P1",
			Title:    "pw_rpc: Fix channel packet framing",
			Assignee: &BuganizerUser{EmailAddress: "dev@google.com"},
		},
	}, "Description.")

	gitRunner := &MockGitRunner{}
	SetupMockConfig(t, gitRunner)
	gitRunner.OnCommand("config --get user.email", "dev@google.com\n")

	mockWT := &mockWorkspaceIntegration{
		enabled: true,
		issueStatuses: map[int64]WorkspaceIssueStatus{
			315378787: {
				ProjectName: "b-315378787-fix-rpc",
				Residency:   "MOUNTED",
				Slot:        "pw-02",
				SymlinkPath: "/home/dev/wrk/projects/b-315378787-fix-rpc",
				Branch:      "b-315378787-fix-rpc",
			},
		},
	}
	prevWT := RegisteredWorkspaceIntegration
	RegisteredWorkspaceIntegration = mockWT
	defer func() { RegisteredWorkspaceIntegration = prevWT }()

	out, err := executeCommand(RootCmd, "issue", "status")
	if err != nil {
		t.Fatalf("issue status failed: %v", err)
	}
	if !strings.Contains(out, "MOUNTED") || !strings.Contains(out, "pw-02") {
		t.Errorf("expected issue status to annotate issue #315378787 with MOUNTED (pw-02), got:\n%s", out)
	}
}
