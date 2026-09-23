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
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestExecGitRunner_RealWorktreeAndSymlinkLifecycle(t *testing.T) {
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git binary not found in PATH")
	}

	tmpDir := t.TempDir()
	primaryRepo := filepath.Join(tmpDir, "primary-repo")
	if err := os.MkdirAll(primaryRepo, 0755); err != nil {
		t.Fatalf("failed to create primary repo dir: %v", err)
	}

	runGitCmd := func(dir string, args ...string) {
		t.Helper()
		cmd := exec.Command("git", args...)
		cmd.Dir = dir
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("git %s failed: %v\n%s", strings.Join(args, " "), err, string(out))
		}
	}

	// Initialize real git repository with an initial commit on main
	upstreamMainCID := "I1234567890123456789012345678901234567890"
	runGitCmd(primaryRepo, "init", "-b", "main")
	runGitCmd(primaryRepo, "config", "user.email", "test@pigweed.dev")
	runGitCmd(primaryRepo, "config", "user.name", "Pigweed Test")
	readmeFile := filepath.Join(primaryRepo, "README.md")
	if err := os.WriteFile(readmeFile, []byte("# Test Repo\n"), 0644); err != nil {
		t.Fatalf("failed to write README: %v", err)
	}
	runGitCmd(primaryRepo, "add", "README.md")
	runGitCmd(primaryRepo, "commit", "-m", "Initial commit\n\nChange-Id: "+upstreamMainCID)
	// Add origin remote pointing to primaryRepo itself so origin/main is a valid tracking ref
	runGitCmd(primaryRepo, "remote", "add", "origin", primaryRepo)
	runGitCmd(primaryRepo, "fetch", "origin")

	gitRunner := NewExecGitRunner()
	poolRoot := filepath.Join(tmpDir, "slots")
	projectsDir := filepath.Join(tmpDir, "projects")
	stateFile := filepath.Join(tmpDir, "state", "worktrees.json")

	store := NewStateStore(stateFile)
	st := NewEmptyState(poolRoot, projectsDir, primaryRepo, 2)
	if err := store.Save(st); err != nil {
		t.Fatalf("failed to save initial state: %v", err)
	}

	// Configure mock Gerrit where upstreamMainCID is MERGED
	mockGerrit := &MockGerritStatus{
		Statuses: map[string]ChangeStatus{
			upstreamMainCID: {
				ChangeID: upstreamMainCID,
				Number:   477945,
				Status:   "MERGED",
			},
		},
	}

	mgr := NewManager(store, gitRunner, nil, NoopIDEDriver{}, mockGerrit)

	// 1. Run Init to create 2 real git worktree slots
	items, err := mgr.Init(2, false)
	if err != nil {
		t.Fatalf("Init with real git failed: %v", err)
	}
	if len(items) == 0 {
		t.Fatalf("expected checklist items")
	}

	// Verify commit-msg hook was installed and is executable
	hookPath := filepath.Join(primaryRepo, ".git", "hooks", "commit-msg")
	if info, err := os.Stat(hookPath); err != nil || info.Mode()&0111 == 0 {
		t.Errorf("expected executable commit-msg hook at %s", hookPath)
	}

	// 2. Mount project "alpha" -> should claim pw-01 and create real branch "alpha"
	resAlpha, err := mgr.Use("alpha", "", "", LeaseModeWrite, "conv-alpha")
	if err != nil {
		t.Fatalf("Use alpha failed: %v", err)
	}
	if resAlpha.Slot != "pw-01" {
		t.Errorf("expected slot pw-01, got %s", resAlpha.Slot)
	}

	// Verify real CommitsAhead is 0 for fresh branch at origin/main
	ahead, err := gitRunner.CommitsAhead(resAlpha.SlotPath, "origin/main", "HEAD")
	if err != nil || ahead != 0 {
		t.Errorf("expected CommitsAhead=0 for fresh branch at origin/main, got %d (err=%v)", ahead, err)
	}
	if resAlpha.ChangeID != "" {
		t.Errorf("expected fresh branch ChangeID to be empty (must not inherit upstream origin/main Change-Id), got %q", resAlpha.ChangeID)
	}

	// Verify List() classifies fresh project as CLEAN_SYNCED (and NEVER CL_MERGED!)
	report, err := mgr.List(context.Background())
	if err != nil {
		t.Fatalf("List failed: %v", err)
	}
	if len(report.MountedProjects) != 1 || report.MountedProjects[0].StatusBadge != BadgeCleanSynced {
		t.Errorf("expected fresh project to be %s, got %+v", BadgeCleanSynced, report.MountedProjects)
	}

	// Verify symlink resolves via EvalSymlinks to the exact realpath of pw-01
	realSymlink, err := filepath.EvalSymlinks(resAlpha.SymlinkPath)
	if err != nil {
		t.Fatalf("EvalSymlinks failed on %s: %v", resAlpha.SymlinkPath, err)
	}
	realSlot, _ := filepath.EvalSymlinks(resAlpha.SlotPath)
	if realSymlink != realSlot {
		t.Errorf("expected realpath(symlink)=%s to match realpath(slot)=%s", realSymlink, realSlot)
	}

	// 3. Make a real commit on branch "alpha" ahead of origin/main
	featureCID := "I9999999999999999999999999999999999999999"
	featureFile := filepath.Join(resAlpha.SymlinkPath, "feature.cc")
	if err := os.WriteFile(featureFile, []byte("int main() { return 0; }\n"), 0644); err != nil {
		t.Fatalf("failed to write feature.cc: %v", err)
	}
	runGitCmd(resAlpha.SymlinkPath, "add", "feature.cc")
	runGitCmd(resAlpha.SymlinkPath, "commit", "-m", "Add feature\n\nChange-Id: "+featureCID)

	// Verify CommitsAhead is now 1 and ExtractChangeID returns featureCID
	aheadAfterCommit, err := gitRunner.CommitsAhead(resAlpha.SlotPath, "origin/main", "HEAD")
	if err != nil || aheadAfterCommit != 1 {
		t.Errorf("expected CommitsAhead=1 after commit, got %d (err=%v)", aheadAfterCommit, err)
	}
	extractedCID, _ := gitRunner.ExtractChangeID(resAlpha.SlotPath, "HEAD")
	if extractedCID != featureCID {
		t.Errorf("expected Change-Id %s, got %s", featureCID, extractedCID)
	}

	// Verify List() shows LOCAL_WIP when commit is not on Gerrit yet
	reportWIP, _ := mgr.List(context.Background())
	if reportWIP.MountedProjects[0].StatusBadge != BadgeLocalWIP {
		t.Errorf("expected %s for unuploaded local commit, got %s", BadgeLocalWIP, reportWIP.MountedProjects[0].StatusBadge)
	}

	// Simulate Gerrit merging featureCID while local commit is still ahead of origin/main
	mockGerrit.Statuses[featureCID] = ChangeStatus{
		ChangeID: featureCID,
		Number:   555555,
		Status:   "MERGED",
	}
	reportMerged, _ := mgr.List(context.Background())
	if reportMerged.MountedProjects[0].StatusBadge != BadgeCLMerged {
		t.Errorf("expected %s when Gerrit CL merges and branch is ahead of origin/main, got %s",
			BadgeCLMerged, reportMerged.MountedProjects[0].StatusBadge)
	}

	// Simulate landing on origin/main and running `mgr.Next("alpha")`
	runGitCmd(primaryRepo, "merge", "--ff-only", "alpha")
	if err := mgr.Next("alpha"); err != nil {
		t.Fatalf("Next('alpha') failed: %v", err)
	}
	reportAfterNext, _ := mgr.List(context.Background())
	if reportAfterNext.MountedProjects[0].StatusBadge != BadgeCleanSynced {
		t.Errorf("expected %s after running Next(), got %s",
			BadgeCleanSynced, reportAfterNext.MountedProjects[0].StatusBadge)
	}

	// 4. Create an uncommitted file inside alpha symlink and verify DIRTY protection
	dirtyFile := filepath.Join(resAlpha.SymlinkPath, "dirty.txt")
	if err := os.WriteFile(dirtyFile, []byte("uncommitted data"), 0644); err != nil {
		t.Fatalf("failed to write dirty file: %v", err)
	}
	if err := mgr.Park("alpha", false); err == nil {
		t.Errorf("expected Park to fail on real dirty worktree without --force")
	}

	// Clean up dirty file and park alpha cleanly
	_ = os.Remove(dirtyFile)
	if err := mgr.Park("alpha", false); err != nil {
		t.Fatalf("expected Park to succeed after cleaning dirty file: %v", err)
	}
}
