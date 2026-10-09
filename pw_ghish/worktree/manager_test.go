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
	"fmt"
	"os"
	"path/filepath"
	"sort"
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
	ExistingBranches     map[string]bool   // branchName -> exists in refs/heads/
	ResetBranches        map[string]string // worktreePath -> branchName:targetRef
	SwitchedFromRefs     map[string]string // branchName -> createFromRef
	ChangeIDs            map[string]string // worktreePath -> Change-Id
	AheadCounts          map[string]int    // worktreePath/rev -> commits ahead of origin/main
	UpstreamMainChangeID string            // simulates Change-Id on origin/main tip commit
	Commits              map[string]string // worktreePath -> SHA
	HookAdded            bool
}

func NewMockGitRunner() *MockGitRunner {
	return &MockGitRunner{
		DirtyPaths:       make(map[string]bool),
		StatusErrors:     make(map[string]error),
		DetachErrors:     make(map[string]error),
		FetchedRefs:      make(map[string]string),
		Branches:         make(map[string]string),
		ExistingBranches: make(map[string]bool),
		ResetBranches:    make(map[string]string),
		SwitchedFromRefs: make(map[string]string),
		ChangeIDs:        make(map[string]string),
		AheadCounts:      make(map[string]int),
		Commits:          make(map[string]string),
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
	if _, ok := m.Branches[worktreePath]; !ok {
		m.Branches[worktreePath] = "HEAD"
	}
	return nil
}

func (m *MockGitRunner) SwitchBranch(worktreePath, branchName, createFromRef string) error {
	// Enforce Git's single-worktree-per-branch invariant!
	for otherPath, otherBranch := range m.Branches {
		if otherPath != worktreePath && otherBranch == branchName && branchName != "" && branchName != "HEAD" {
			return fmt.Errorf("fatal: '%s' is already used by worktree at '%s'", branchName, otherPath)
		}
	}
	if m.ExistingBranches == nil {
		m.ExistingBranches = make(map[string]bool)
	}
	m.ExistingBranches[branchName] = true
	if m.SwitchedFromRefs == nil {
		m.SwitchedFromRefs = make(map[string]string)
	}
	m.SwitchedFromRefs[branchName] = createFromRef
	m.Branches[worktreePath] = branchName
	if m.Commits[worktreePath] == "" {
		m.Commits[worktreePath] = "deadbeef1234567890"
	}
	return nil
}

func (m *MockGitRunner) ResetBranch(worktreePath, branchName, targetRef string) error {
	// Enforce Git's single-worktree-per-branch invariant!
	for otherPath, otherBranch := range m.Branches {
		if otherPath != worktreePath && otherBranch == branchName && branchName != "" && branchName != "HEAD" {
			return fmt.Errorf("fatal: '%s' is already used by worktree at '%s'", branchName, otherPath)
		}
	}
	if m.ExistingBranches == nil {
		m.ExistingBranches = make(map[string]bool)
	}
	m.ExistingBranches[branchName] = true
	if m.ResetBranches == nil {
		m.ResetBranches = make(map[string]string)
	}
	m.ResetBranches[worktreePath] = branchName + ":" + targetRef
	if m.SwitchedFromRefs == nil {
		m.SwitchedFromRefs = make(map[string]string)
	}
	m.SwitchedFromRefs[branchName] = targetRef
	m.Branches[worktreePath] = branchName
	if m.Commits[worktreePath] == "" {
		m.Commits[worktreePath] = "deadbeef1234567890"
	}
	return nil
}

func (m *MockGitRunner) ListWorktrees(repoOrWorktreePath string) ([]WorktreeInfo, error) {
	var paths []string
	for p := range m.Branches {
		paths = append(paths, p)
	}
	sort.Strings(paths)
	var out []WorktreeInfo
	for _, p := range paths {
		b := m.Branches[p]
		sha := m.Commits[p]
		if sha == "" {
			sha = "deadbeef1234567890"
		}
		if b == "" || b == "HEAD" {
			out = append(out, WorktreeInfo{
				Path:     p,
				HeadSHA:  sha,
				Detached: true,
			})
		} else {
			out = append(out, WorktreeInfo{
				Path:     p,
				HeadSHA:  sha,
				Branch:   b,
				Detached: false,
			})
		}
	}
	return out, nil
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
	if c, ok := m.AheadCounts[rev]; ok {
		return c, nil
	}
	shortRev := strings.TrimPrefix(rev, "refs/heads/")
	if c, ok := m.AheadCounts[shortRev]; ok {
		return c, nil
	}
	if rev == "HEAD" {
		if c, ok := m.AheadCounts[repoOrWorktreePath]; ok {
			return c, nil
		}
		if cid, ok := m.ChangeIDs[repoOrWorktreePath]; ok && cid != "" {
			return 1, nil
		}
	}
	if cid, ok := m.ChangeIDs[rev]; ok && cid != "" {
		return 1, nil
	}
	if cid, ok := m.ChangeIDs[shortRev]; ok && cid != "" {
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
	if m.ExistingBranches != nil && m.ExistingBranches[branchName] {
		return true, nil
	}
	for _, b := range m.Branches {
		if b == branchName {
			return true, nil
		}
	}
	if c, ok := m.AheadCounts[branchName]; ok && c > 0 {
		return true, nil
	}
	if c, ok := m.AheadCounts["refs/heads/"+branchName]; ok && c > 0 {
		return true, nil
	}
	if cid, ok := m.ChangeIDs[branchName]; ok && cid != "" {
		return true, nil
	}
	return false, nil
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

func TestManager_CustomSlotPrefixWarmupDriverAndShortlink(t *testing.T) {
	mgr, mockGit, mockGerrit, tmpDir := setupTestManager(t, 2)
	primaryRepo := filepath.Join(tmpDir, "pigweed")
	tomlContent := `
[gerrit]
host = "https://acme-internal-review.googlesource.com"
project = "acme-fw"
shortlinks = { "acmerev" = "https://acme-internal-review.googlesource.com/c/acme-fw/+/{id}" }

[worktree]
slot_prefix = "acme-wt-"
warmup_driver = "none"
`
	if err := os.WriteFile(filepath.Join(primaryRepo, ".ghish.toml"), []byte(tomlContent), 0644); err != nil {
		t.Fatalf("failed to write .ghish.toml: %v", err)
	}

	items, err := mgr.Init(2, false)
	if err != nil {
		t.Fatalf("Init failed: %v", err)
	}
	foundNoopBuild := false
	for _, item := range items {
		if item.Category == "Build Cache Driver" && strings.Contains(item.Summary, "warmup_driver=none") {
			foundNoopBuild = true
		}
	}
	if !foundNoopBuild {
		t.Errorf("expected Build Cache Driver checklist item with warmup_driver=none, got: %+v", items)
	}

	res, err := mgr.Use("spi-driver", "", "", LeaseModeWrite, "agent-1")
	if err != nil {
		t.Fatalf("Use failed: %v", err)
	}
	if res.Slot != "acme-wt-01" {
		t.Errorf("expected slot 'acme-wt-01', got %q", res.Slot)
	}

	cid := "I9999999999999999999999999999999999999999"
	mockGit.ChangeIDs[res.SlotPath] = cid
	mockGerrit.Statuses[cid] = ChangeStatus{
		ChangeID: cid,
		Number:   88123,
		Status:   "MERGED",
	}

	report, err := mgr.List(context.Background())
	if err != nil {
		t.Fatalf("List failed: %v", err)
	}
	if len(report.MountedProjects) != 1 {
		t.Fatalf("expected 1 mounted project, got %d", len(report.MountedProjects))
	}
	if !strings.Contains(report.MountedProjects[0].Details, "acmerev/88123 (Merged)") {
		t.Errorf("expected details to contain 'acmerev/88123 (Merged)', got %q", report.MountedProjects[0].Details)
	}
}

func TestManager_UnmanagedWorktreeBranchCollisionAutoSuffixes(t *testing.T) {
	mgr, mockGit, _, tmpDir := setupTestManager(t, 3)

	// Simulate unmanaged worktrees outside ~/wrk/slots/ holding branches checked out
	unmanagedGhishPath := filepath.Join(tmpDir, "pw-ghish")
	mockGit.Branches[unmanagedGhishPath] = "ghish"

	// `gh wt init` should detect and report the unmanaged worktree holding a branch
	items, err := mgr.Init(3, true)
	if err != nil {
		t.Fatalf("Init failed: %v", err)
	}
	foundUnmanagedItem := false
	for _, item := range items {
		if item.Category == "Unmanaged Git Worktrees" && strings.Contains(item.Summary, "pw-ghish [ghish]") {
			foundUnmanagedItem = true
		}
	}
	if !foundUnmanagedItem {
		t.Errorf("expected Init checklist to report unmanaged worktree pw-ghish [ghish], got: %+v", items)
	}

	// `gh wt use ghish` must succeed in one shot without detaching unmanagedGhishPath!
	res, err := mgr.Use("ghish", "", "", LeaseModeWrite, "agent-1")
	if err != nil {
		t.Fatalf("expected Use('ghish') to succeed when unmanaged worktree holds branch 'ghish', got err: %v", err)
	}
	if res.Project != "ghish" {
		t.Errorf("expected Project='ghish', got %q", res.Project)
	}
	if res.Branch != "ghish-wt" {
		t.Errorf("expected auto-suffixed branch 'ghish-wt', got %q", res.Branch)
	}
	if mockGit.Branches[unmanagedGhishPath] != "ghish" {
		t.Errorf("unmanaged worktree %s was mutated! Expected branch 'ghish', got %q", unmanagedGhishPath, mockGit.Branches[unmanagedGhishPath])
	}
	if len(res.Warnings) == 0 || res.Warnings[0].Subsystem != "Git Branch Collision" {
		t.Errorf("expected Git Branch Collision warning in UseResult, got: %+v", res.Warnings)
	}

	// Park and resume "ghish" -> should resume "ghish-wt" cleanly without re-colliding
	if err := mgr.Park("ghish", false); err != nil {
		t.Fatalf("Park('ghish') failed: %v", err)
	}
	resResume, err := mgr.Use("ghish", "", "", LeaseModeWrite, "agent-1")
	if err != nil {
		t.Fatalf("resume Use('ghish') failed: %v", err)
	}
	if resResume.Branch != "ghish-wt" {
		t.Errorf("expected resumed project to stay on branch 'ghish-wt', got %q", resResume.Branch)
	}
}

func TestManager_StaleLocalBranchResetVsUnmergedPreservation(t *testing.T) {
	mgr, mockGit, _, _ := setupTestManager(t, 3)

	// Case A: Pre-existing local branch "stale-merged" has 0 unmerged commits ahead of origin/main.
	// `gh wt use stale-merged` should reset it to origin/main and use "stale-merged" directly.
	mockGit.ExistingBranches["stale-merged"] = true
	mockGit.AheadCounts["stale-merged"] = 0

	resStale, err := mgr.Use("stale-merged", "", "", LeaseModeWrite, "agent-1")
	if err != nil {
		t.Fatalf("Use('stale-merged') failed: %v", err)
	}
	if resStale.Branch != "stale-merged" {
		t.Errorf("expected branch 'stale-merged' to be reused when 0 commits ahead, got %q", resStale.Branch)
	}
	if mockGit.ResetBranches[resStale.SlotPath] != "stale-merged:origin/main" {
		t.Errorf("expected ResetBranch('stale-merged', 'origin/main'), got %q", mockGit.ResetBranches[resStale.SlotPath])
	}

	// Case B: Pre-existing local branch "wip-local" has 2 unmerged commits ahead of origin/main.
	// Brand-new `gh wt use wip-local` (without explicit --branch) must NOT overwrite "wip-local";
	// it should allocate "wip-local-wt" at origin/main and warn.
	mockGit.ExistingBranches["wip-local"] = true
	mockGit.AheadCounts["wip-local"] = 2

	resWIP, err := mgr.Use("wip-local", "", "", LeaseModeWrite, "agent-1")
	if err != nil {
		t.Fatalf("Use('wip-local') failed: %v", err)
	}
	if resWIP.Branch != "wip-local-wt" {
		t.Errorf("expected 'wip-local-wt' to avoid clobbering unmerged branch 'wip-local', got %q", resWIP.Branch)
	}
	if len(resWIP.Warnings) == 0 || !strings.Contains(resWIP.Warnings[0].Message, "unmerged commit(s)") {
		t.Errorf("expected unmerged commits warning, got: %+v", resWIP.Warnings)
	}

	// Case C: Explicit `--branch wip-local` checks out the existing branch with its unmerged commits.
	resExplicit, err := mgr.Use("wip-explicit", "wip-local", "", LeaseModeWrite, "agent-1")
	if err != nil {
		t.Fatalf("Use('wip-explicit', 'wip-local') failed: %v", err)
	}
	if resExplicit.Branch != "wip-local" {
		t.Errorf("expected explicit --branch 'wip-local' to check out 'wip-local', got %q", resExplicit.Branch)
	}
}

func TestManager_ResolveProjectFromSlotSymlinkAndSubdir(t *testing.T) {
	mgr, _, _, tmpDir := setupTestManager(t, 2)

	res, err := mgr.Use("ghish", "", "", LeaseModeWrite, "agent-1")
	if err != nil {
		t.Fatalf("Use('ghish') failed: %v", err)
	}

	// 1. Resolve from slot subdirectory via BUILD_WORKING_DIRECTORY (simulating ./gh wrapper in slot subdir)
	slotSubdir := filepath.Join(res.SlotPath, "pw_ghish", "worktree")
	_ = os.MkdirAll(slotSubdir, 0755)
	t.Setenv("BUILD_WORKING_DIRECTORY", slotSubdir)
	if err := mgr.Next(""); err != nil {
		t.Fatalf("expected Next('') to resolve project 'ghish' from slot subdirectory %s, got: %v", slotSubdir, err)
	}

	// 2. Resolve by slot name ("pw-01")
	if err := mgr.Next("pw-01"); err != nil {
		t.Fatalf("expected Next('pw-01') to resolve project 'ghish', got: %v", err)
	}

	// 3. Running Next("") inside an unmanaged worktree fails with actionable remediation
	unmanagedDir := filepath.Join(tmpDir, "pw-unmanaged")
	_ = os.MkdirAll(unmanagedDir, 0755)
	t.Setenv("BUILD_WORKING_DIRECTORY", unmanagedDir)
	t.Setenv("PWD", unmanagedDir)
	origWd, _ := os.Getwd()
	_ = os.Chdir(unmanagedDir)
	defer os.Chdir(origWd)

	err = mgr.Next("")
	if err == nil || !strings.Contains(err.Error(), "not an unmanaged worktree") {
		t.Errorf("expected Next('') in unmanaged worktree to return actionable error, got: %v", err)
	}
}
