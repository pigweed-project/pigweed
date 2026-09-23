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
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// MockGitRunner simulates Git worktree and branch operations in memory/tempdir.
type MockGitRunner struct {
	DirtyPaths           map[string]bool
	StatusErrors         map[string]error
	DetachErrors         map[string]error
	FetchedRefs          map[string]string
	Branches             map[string]string // worktreePath -> branch
	ChangeIDs            map[string]string // worktreePath -> Change-Id
	AheadCounts          map[string]int    // worktreePath/rev -> commits ahead of origin/main
	UpstreamMainChangeID string            // simulates Change-Id on origin/main tip commit
	Commits              map[string]string // worktreePath -> SHA
	HookAdded            bool
}

func NewMockGitRunner() *MockGitRunner {
	return &MockGitRunner{
		DirtyPaths:   make(map[string]bool),
		StatusErrors: make(map[string]error),
		DetachErrors: make(map[string]error),
		FetchedRefs:  make(map[string]string),
		Branches:     make(map[string]string),
		ChangeIDs:    make(map[string]string),
		AheadCounts:  make(map[string]int),
		Commits:      make(map[string]string),
	}
}

func (m *MockGitRunner) WorktreeAddDetached(primaryRepo, worktreePath string) error {
	if err := os.MkdirAll(worktreePath, 0755); err != nil {
		return err
	}
	// Create placeholder .git file
	if err := os.WriteFile(filepath.Join(worktreePath, ".git"), []byte("gitdir: /mock"), 0644); err != nil {
		return err
	}
	if m.Commits[worktreePath] == "" {
		m.Commits[worktreePath] = "deadbeef1234567890"
	}
	return nil
}

func (m *MockGitRunner) SwitchBranch(worktreePath, branchName, createFromRef string) error {
	m.Branches[worktreePath] = branchName
	if m.Commits[worktreePath] == "" {
		m.Commits[worktreePath] = "deadbeef1234567890"
	}
	return nil
}

func (m *MockGitRunner) SwitchDetach(worktreePath, targetRef string) error {
	if m.DetachErrors != nil {
		if err, ok := m.DetachErrors[worktreePath]; ok && err != nil {
			return err
		}
	}
	m.Branches[worktreePath] = "HEAD"
	return nil
}

func (m *MockGitRunner) StatusPorcelain(worktreePath string) (string, error) {
	if m.StatusErrors != nil {
		if err, ok := m.StatusErrors[worktreePath]; ok && err != nil {
			return "", err
		}
	}
	if m.DirtyPaths[worktreePath] {
		return " M modified_file.cc\n", nil
	}
	return "", nil
}

func (m *MockGitRunner) CurrentBranch(worktreePath string) (string, error) {
	if b, ok := m.Branches[worktreePath]; ok {
		return b, nil
	}
	return "main", nil
}

func (m *MockGitRunner) RevParse(repoOrWorktreePath, rev string) (string, error) {
	if sha, ok := m.Commits[repoOrWorktreePath]; ok {
		return sha, nil
	}
	return "deadbeef1234567890", nil
}

func (m *MockGitRunner) ExtractChangeID(repoOrWorktreePath, rev string) (string, error) {
	if cid, ok := m.ChangeIDs[repoOrWorktreePath]; ok && cid != "" {
		return cid, nil
	}
	if cid, ok := m.ChangeIDs[rev]; ok && cid != "" {
		return cid, nil
	}
	if m.UpstreamMainChangeID != "" {
		return m.UpstreamMainChangeID, nil
	}
	return "", nil
}

func (m *MockGitRunner) CommitsAhead(repoOrWorktreePath, baseRef, rev string) (int, error) {
	if c, ok := m.AheadCounts[repoOrWorktreePath+":"+rev]; ok {
		return c, nil
	}
	if c, ok := m.AheadCounts[repoOrWorktreePath]; ok {
		return c, nil
	}
	if c, ok := m.AheadCounts[rev]; ok {
		return c, nil
	}
	// If test explicitly configured a ChangeID for this worktree/branch, default to 1 commit ahead
	if cid, ok := m.ChangeIDs[repoOrWorktreePath]; ok && cid != "" {
		return 1, nil
	}
	if cid, ok := m.ChangeIDs[rev]; ok && cid != "" {
		return 1, nil
	}
	return 0, nil
}

func (m *MockGitRunner) SwitchDetachForce(worktreePath, targetRef string) error {
	if m.DetachErrors != nil {
		if err, ok := m.DetachErrors[worktreePath]; ok && err != nil {
			return err
		}
	}
	delete(m.DirtyPaths, worktreePath)
	m.Branches[worktreePath] = "HEAD"
	return nil
}

func (m *MockGitRunner) FetchOrigin(repoOrWorktreePath string) error { return nil }
func (m *MockGitRunner) FetchRef(repoOrWorktreePath, remote, ref string) error {
	if m.FetchedRefs == nil {
		m.FetchedRefs = make(map[string]string)
	}
	m.FetchedRefs[repoOrWorktreePath] = ref
	return nil
}
func (m *MockGitRunner) RebaseOriginMain(worktreePath string) error { return nil }
func (m *MockGitRunner) BranchExists(primaryRepo, branchName string) (bool, error) {
	return true, nil
}
func (m *MockGitRunner) EnsureCommitMsgHook(primaryRepo string) (bool, error) {
	if m.HookAdded {
		return false, nil
	}
	m.HookAdded = true
	return true, nil
}

type MockGerritStatus struct {
	Statuses map[string]ChangeStatus
	QueryErr error
	CLRefs   map[string]string // clRef -> fetchRef
	CLIDs    map[string]string // clRef -> changeID
}

func (m *MockGerritStatus) QueryChangesByID(ctx context.Context, changeIDs []string) (map[string]ChangeStatus, error) {
	if m.QueryErr != nil {
		return nil, m.QueryErr
	}
	return m.Statuses, nil
}

func (m *MockGerritStatus) ResolveCLFetchRef(ctx context.Context, clRef string) (string, string, error) {
	if m.CLRefs != nil {
		if ref, ok := m.CLRefs[clRef]; ok {
			cid := ""
			if m.CLIDs != nil {
				cid = m.CLIDs[clRef]
			}
			return ref, cid, nil
		}
	}
	return "refs/changes/45/477945/1", "I1234567890123456789012345678901234567890", nil
}

func setupTestManager(t *testing.T, slotCount int) (*Manager, *MockGitRunner, *MockGerritStatus, string) {
	t.Helper()
	tmpDir := t.TempDir()
	primaryRepo := filepath.Join(tmpDir, "pigweed")
	if err := os.MkdirAll(primaryRepo, 0755); err != nil {
		t.Fatalf("failed to create primary repo: %v", err)
	}
	poolRoot := filepath.Join(tmpDir, "slots")
	projectsDir := filepath.Join(tmpDir, "projects")
	stateFile := filepath.Join(tmpDir, "config", "worktrees.json")

	store := NewStateStore(stateFile)
	st := NewEmptyState(poolRoot, projectsDir, primaryRepo, slotCount)
	if err := store.Save(st); err != nil {
		t.Fatalf("failed to save initial state: %v", err)
	}

	mockGit := NewMockGitRunner()
	mockGerrit := &MockGerritStatus{Statuses: make(map[string]ChangeStatus)}

	mgr := NewManager(store, mockGit, nil, NoopIDEDriver{}, mockGerrit)
	baseTime := time.Date(2026, 9, 16, 12, 0, 0, 0, time.UTC)
	mgr.Now = func() time.Time { return baseTime }

	return mgr, mockGit, mockGerrit, tmpDir
}

func TestManager_InitAndUse(t *testing.T) {
	mgr, _, _, tmpDir := setupTestManager(t, 3)

	items, err := mgr.Init(3, false)
	if err != nil {
		t.Fatalf("Init failed: %v", err)
	}
	if len(items) == 0 {
		t.Fatalf("expected checklist items from Init")
	}

	res, err := mgr.Use("rpc-fix", "", "", LeaseModeWrite, "conv-1")
	if err != nil {
		t.Fatalf("Use failed: %v", err)
	}
	if res.Slot != "pw-01" {
		t.Errorf("expected slot pw-01, got %s", res.Slot)
	}

	expectedSym := filepath.Join(tmpDir, "projects", "rpc-fix")
	target, err := os.Readlink(expectedSym)
	if err != nil {
		t.Fatalf("expected symlink at %s: %v", expectedSym, err)
	}
	if target != res.SlotPath {
		t.Errorf("expected symlink target %s, got %s", res.SlotPath, target)
	}
}

func TestManager_LRUSwapWhenSlotsFull(t *testing.T) {
	mgr, mockGit, _, tmpDir := setupTestManager(t, 2) // Only 2 slots!
	now := time.Date(2026, 9, 16, 12, 0, 0, 0, time.UTC)
	mgr.Now = func() time.Time { return now }

	// Mount proj-1 at T=0 (no active lease ID so it can be swapped later)
	res1, err := mgr.Use("proj-1", "", "", LeaseModeWrite, "")
	if err != nil {
		t.Fatalf("Use proj-1 failed: %v", err)
	}
	mockGit.ChangeIDs[res1.SlotPath] = "I1111111111111111111111111111111111111111"

	// Mount proj-2 at T=10m
	now = now.Add(10 * time.Minute)
	res2, err := mgr.Use("proj-2", "", "", LeaseModeWrite, "")
	if err != nil {
		t.Fatalf("Use proj-2 failed: %v", err)
	}
	if res2.Slot != "pw-02" {
		t.Fatalf("expected proj-2 in pw-02, got %s", res2.Slot)
	}

	// Now allocate proj-3 at T=20m -> should LRU-swap out proj-1 (oldest)!
	now = now.Add(10 * time.Minute)
	res3, err := mgr.Use("proj-3", "", "", LeaseModeWrite, "")
	if err != nil {
		t.Fatalf("Use proj-3 failed: %v", err)
	}
	if res3.SwappedOut != "proj-1" {
		t.Errorf("expected SwappedOut='proj-1', got %q", res3.SwappedOut)
	}
	if res3.Slot != "pw-01" {
		t.Errorf("expected proj-3 to claim pw-01, got %s", res3.Slot)
	}

	// Verify proj-1 symlink is removed and proj-1 is PARKED with its Change-Id preserved
	if _, err := os.Lstat(filepath.Join(tmpDir, "projects", "proj-1")); !os.IsNotExist(err) {
		t.Errorf("expected proj-1 symlink to be removed when parked")
	}
	st, _ := mgr.Store.Load()
	p1 := st.Projects["proj-1"]
	if p1.Residency != ResidencyParked {
		t.Errorf("expected proj-1 residency PARKED, got %s", p1.Residency)
	}
	if p1.LastKnownChangeID != "I1111111111111111111111111111111111111111" {
		t.Errorf("expected proj-1 Change-Id preserved, got %q", p1.LastKnownChangeID)
	}
}

func TestManager_DirtyWorktreeProtection(t *testing.T) {
	mgr, mockGit, _, _ := setupTestManager(t, 2)
	now := time.Date(2026, 9, 16, 12, 0, 0, 0, time.UTC)
	mgr.Now = func() time.Time { return now }

	// Mount proj-1 at T=0 and mark it DIRTY
	res1, _ := mgr.Use("proj-1", "", "", LeaseModeWrite, "")
	mockGit.DirtyPaths[res1.SlotPath] = true

	// Mount proj-2 at T=10m (newer, but clean!)
	now = now.Add(10 * time.Minute)
	_, _ = mgr.Use("proj-2", "", "", LeaseModeWrite, "")

	// Attempt to explicitly park proj-1 without --force -> must fail with 4-pillar error!
	err := mgr.Park("proj-1", false)
	if err == nil || !strings.Contains(err.Error(), "uncommitted working tree changes") {
		t.Fatalf("expected Park to reject dirty worktree, got: %v", err)
	}

	// Allocate proj-3 -> even though proj-1 is older, it is DIRTY so proj-2 must be swapped out instead!
	now = now.Add(10 * time.Minute)
	res3, err := mgr.Use("proj-3", "", "", LeaseModeWrite, "")
	if err != nil {
		t.Fatalf("Use proj-3 failed: %v", err)
	}
	if res3.SwappedOut != "proj-2" {
		t.Errorf("expected clean proj-2 to be swapped out instead of dirty proj-1, got SwappedOut=%q", res3.SwappedOut)
	}
}

func TestManager_WriterLeaseCollisionWarmFork(t *testing.T) {
	mgr, _, _, _ := setupTestManager(t, 3)

	// Agent A acquires write lease on rpc-fix
	resA, err := mgr.Use("rpc-fix", "", "", LeaseModeWrite, "agent-A")
	if err != nil {
		t.Fatalf("Agent A Use failed: %v", err)
	}
	if resA.Slot != "pw-01" {
		t.Fatalf("expected pw-01, got %s", resA.Slot)
	}

	// Agent B requests write lease on rpc-fix concurrently -> should automatically warm-fork!
	resB, err := mgr.Use("rpc-fix", "", "", LeaseModeWrite, "agent-B")
	if err != nil {
		t.Fatalf("Agent B Use failed: %v", err)
	}
	if resB.ForkedFrom != "rpc-fix" {
		t.Errorf("expected ForkedFrom='rpc-fix', got %q", resB.ForkedFrom)
	}
	if resB.Project != "rpc-fix-fork-1" {
		t.Errorf("expected forked project name 'rpc-fix-fork-1', got %q", resB.Project)
	}
	if resB.Slot != "pw-02" {
		t.Errorf("expected warm fork in slot pw-02, got %s", resB.Slot)
	}
}

func TestManager_ListDashboardMonitorsParkedProjects(t *testing.T) {
	mgr, mockGit, mockGerrit, _ := setupTestManager(t, 2)

	res1, _ := mgr.Use("bt-proxy", "", "", LeaseModeWrite, "")
	cid := "I2222222222222222222222222222222222222222"
	mockGit.ChangeIDs[res1.SlotPath] = cid

	// Park bt-proxy
	if err := mgr.Park("bt-proxy", false); err != nil {
		t.Fatalf("Park failed: %v", err)
	}

	// Simulate reviewer leaving 3 unresolved comments on Gerrit while bt-proxy is PARKED!
	mockGerrit.Statuses[cid] = ChangeStatus{
		ChangeID:          cid,
		Number:            471888,
		Status:            "NEW",
		CodeReviewScore:   1,
		UnresolvedThreads: 3,
	}

	report, err := mgr.List(context.Background())
	if err != nil {
		t.Fatalf("List failed: %v", err)
	}
	if len(report.ParkedProjects) != 1 {
		t.Fatalf("expected 1 parked project, got %d", len(report.ParkedProjects))
	}
	parked := report.ParkedProjects[0]
	if parked.StatusBadge != BadgeNeedsAttention {
		t.Errorf("expected parked project to have badge %s, got %s", BadgeNeedsAttention, parked.StatusBadge)
	}
	if !strings.Contains(parked.RecommendedAction, "./gh wt use bt-proxy") {
		t.Errorf("expected swap-in recommendation for parked project needing attention, got %q", parked.RecommendedAction)
	}
}

func TestFreshProjectAtOriginMainIsCleanSyncedNotCLMerged(t *testing.T) {
	mgr, mockGit, mockGerrit, _ := setupTestManager(t, 2)

	// Simulate origin/main having a merged Change-Id at its tip commit (e.g. pwrev/477945)
	upstreamCID := "I4779450000000000000000000000000000000000"
	mockGit.UpstreamMainChangeID = upstreamCID
	mockGerrit.Statuses[upstreamCID] = ChangeStatus{
		ChangeID: upstreamCID,
		Number:   477945,
		Status:   "MERGED",
	}

	// Create a brand new project (0 commits ahead of origin/main)
	res, err := mgr.Use("worktree-test-project", "", "", LeaseModeWrite, "")
	if err != nil {
		t.Fatalf("Use failed: %v", err)
	}
	if res.ChangeID != "" {
		t.Errorf("expected fresh project at origin/main to have empty ChangeID, got %q", res.ChangeID)
	}

	report, err := mgr.List(context.Background())
	if err != nil {
		t.Fatalf("List failed: %v", err)
	}
	if len(report.MountedProjects) != 1 {
		t.Fatalf("expected 1 mounted project, got %d", len(report.MountedProjects))
	}
	mounted := report.MountedProjects[0]
	if mounted.StatusBadge != BadgeCleanSynced {
		t.Errorf("expected fresh project at origin/main to have badge %s, got %s (details: %s)",
			BadgeCleanSynced, mounted.StatusBadge, mounted.Details)
	}
}
