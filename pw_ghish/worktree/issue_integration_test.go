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

package worktree

import (
	"context"
	"strings"
	"testing"
)

func TestWTUse_WithIssueFlag_AutoNamesAndPersistsIssueID(t *testing.T) {
	mgr, _, _, _ := setupTestManager(t, 2)

	res, err := mgr.UseWithIssue("", "", 315378787, "pw_rpc: Fix channel packet framing", LeaseModeWrite, "agent-1")
	if err != nil {
		t.Fatalf("UseWithIssue failed: %v", err)
	}
	if res.Project != "b-315378787-pw-rpc-fix-channel-packet-framing" {
		t.Errorf("expected auto-slugged project name 'b-315378787-pw-rpc-fix-channel-packet-framing', got %q", res.Project)
	}
	if res.IssueID != 315378787 {
		t.Errorf("expected UseResult.IssueID == 315378787, got %d", res.IssueID)
	}

	// Verify persisted state
	st, err := mgr.Store.Load()
	if err != nil {
		t.Fatalf("Store.Load failed: %v", err)
	}
	proj, ok := st.Projects[res.Project]
	if !ok {
		t.Fatalf("project %q not found in state.json", res.Project)
	}
	if proj.IssueID != 315378787 {
		t.Errorf("expected Project.IssueID == 315378787 in state.json, got %d", proj.IssueID)
	}

	// Verify List() includes b/315378787 in Details
	report, err := mgr.List(context.Background())
	if err != nil {
		t.Fatalf("List() failed: %v", err)
	}
	if len(report.MountedProjects) != 1 {
		t.Fatalf("expected 1 mounted project, got %d", len(report.MountedProjects))
	}
	if report.MountedProjects[0].IssueID != 315378787 {
		t.Errorf("expected ProjectListEntry.IssueID == 315378787, got %d", report.MountedProjects[0].IssueID)
	}
	if !strings.Contains(report.MountedProjects[0].Details, "b/315378787") {
		t.Errorf("expected List() Details to include 'b/315378787', got %q", report.MountedProjects[0].Details)
	}
}

func TestWorkspaceIntegrationHook_EndToEnd(t *testing.T) {
	t.Setenv("HOME", t.TempDir())

	uninitStore := NewStateStore(t.TempDir() + "/nonexistent-state.json")
	uninitMgr := NewManager(uninitStore, NewMockGitRunner(), nil, NoopIDEDriver{}, nil)
	uninitAdapter := &WorkspaceIntegrationAdapter{
		ManagerFn: func() (*Manager, error) { return uninitMgr, nil },
	}
	if uninitAdapter.IsEnabled() {
		t.Errorf("expected IsEnabled() == false when state.json does not exist")
	}

	mgr, _, _, _ := setupTestManager(t, 2)
	adapter := &WorkspaceIntegrationAdapter{
		ManagerFn: func() (*Manager, error) { return mgr, nil },
	}

	// Develop issue 315378787 in worktree
	symlinkPath, slotName, err := adapter.DevelopIssueInWorktree(context.Background(), 315378787, "Fix memory leak", "")
	if err != nil {
		t.Fatalf("DevelopIssueInWorktree failed: %v", err)
	}
	if slotName != "pw-01" {
		t.Errorf("expected slot pw-01, got %q", slotName)
	}

	// Now IsEnabled() should be true
	if !adapter.IsEnabled() {
		t.Errorf("expected IsEnabled() == true after state.json exists")
	}

	// ResolveIssueIDForPath should resolve symlinkPath to 315378787
	resolvedID, ok := adapter.ResolveIssueIDForPath(symlinkPath)
	if !ok || resolvedID != 315378787 {
		t.Errorf("expected ResolveIssueIDForPath(%q) = (315378787, true), got (%d, %v)", symlinkPath, resolvedID, ok)
	}

	// FindWorkspaceForIssue should return MOUNTED status in pw-01
	wsStatus, found := adapter.FindWorkspaceForIssue(315378787)
	if !found || wsStatus.Residency != "MOUNTED" || wsStatus.Slot != "pw-01" {
		t.Errorf("expected FindWorkspaceForIssue(315378787) to return MOUNTED in pw-01, got %+v (found=%v)", wsStatus, found)
	}
}

func TestCLI_WTUseWithIssue_And_WTCloseReminder(t *testing.T) {
	mgr, _, _, _ := setupTestManager(t, 2)

	// 1. Run `gh wt use --issue 315378787`
	outUse, err := execWTCommand(t, mgr, "use", "--issue", "315378787")
	if err != nil {
		t.Fatalf("wt use --issue 315378787 failed: %v\nOutput: %s", err, outUse)
	}
	if !strings.Contains(outUse, "b/315378787") {
		t.Errorf("expected 'wt use --issue' output to display Issue: b/315378787, got:\n%s", outUse)
	}

	// 2. Run `gh wt close b-315378787` and verify helpful issue reminder
	outClose, err := execWTCommand(t, mgr, "close", "b-315378787")
	if err != nil {
		t.Fatalf("wt close b-315378787 failed: %v\nOutput: %s", err, outClose)
	}
	if !strings.Contains(outClose, "gh issue close 315378787") {
		t.Errorf("expected 'wt close' output to remind user to close issue b/315378787, got:\n%s", outClose)
	}
}
