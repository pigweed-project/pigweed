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
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// [DL-03] Test that BazelDriver.CheckAndConfigure refuses to overwrite ~/.bazelrc on non-ENOENT read error.
func TestBazelDriver_AbortsOnUnreadableBazelrcWithoutClobbering(t *testing.T) {
	tmpHome := t.TempDir()
	bazelrcPath := filepath.Join(tmpHome, ".bazelrc")
	// Create a directory at bazelrcPath with a file inside so os.ReadFile(bazelrcPath)
	// returns EISDIR (a deterministic non-ENOENT error even when running as root in Bazel linux-sandbox).
	if err := os.MkdirAll(bazelrcPath, 0755); err != nil {
		t.Fatalf("failed to mkdir .bazelrc: %v", err)
	}
	sentinelFile := filepath.Join(bazelrcPath, "precious.conf")
	if err := os.WriteFile(sentinelFile, []byte("precious"), 0644); err != nil {
		t.Fatalf("failed to write sentinel file: %v", err)
	}

	driver := &BazelDriver{
		HomeDir:        tmpHome,
		BazelCacheRoot: filepath.Join(tmpHome, ".cache", "bazel"),
	}
	_, err := driver.CheckAndConfigure(true, nil)
	if err == nil {
		t.Errorf("expected CheckAndConfigure(fix=true) to return an error when .bazelrc read fails with EISDIR, got nil")
	}

	// Verify directory and sentinel file were NOT removed or clobbered
	if _, statErr := os.Stat(sentinelFile); statErr != nil {
		t.Errorf("CRITICAL DATA LOSS: existing .bazelrc entry was clobbered on non-ENOENT error: %v", statErr)
	}
}

// [DL-06] Test that JetskiIDEDriver refuses to overwrite existing project JSON on corrupt/unmarshal error
// and only copies allowlisted template keys.
func TestJetskiIDEDriver_PreservesCorruptFileAndFiltersTemplateKeys(t *testing.T) {
	projectsDir := t.TempDir()
	driver := &JetskiIDEDriver{ProjectsDir: projectsDir}

	// 1. Write a donor pigweed.json with both allowlisted keys and private IDE state keys
	donorJSON := `{
  "name": "pigweed",
  "projectResources": {"resources": []},
  "permissionGrants": {"allow": ["command(bazelisk)"]},
  "lastOpenedConversationId": "secret-conv-12345",
  "windowBounds": {"width": 1920}
}`
	if err := os.WriteFile(filepath.Join(projectsDir, "donor.json"), []byte(donorJSON), 0644); err != nil {
		t.Fatalf("failed to write donor.json: %v", err)
	}

	proj := &Project{Name: "clean-proj"}
	if err := driver.SyncProject(proj, "/home/user/wrk/projects/clean-proj"); err != nil {
		t.Fatalf("SyncProject failed: %v", err)
	}

	targetFile := filepath.Join(projectsDir, proj.JetskiProjectUUID+".json")
	syncedData, err := os.ReadFile(targetFile)
	if err != nil {
		t.Fatalf("failed to read synced project JSON: %v", err)
	}
	if strings.Contains(string(syncedData), "secret-conv-12345") || strings.Contains(string(syncedData), "windowBounds") {
		t.Errorf("expected loadBaseTemplate to filter out private donor keys (lastOpenedConversationId, windowBounds), got:\n%s", string(syncedData))
	}

	// 2. Now corrupt an existing project file and verify SyncProject does NOT overwrite it
	corruptProj := &Project{Name: "corrupt-proj"}
	corruptUUID := DeterministicProjectUUID(corruptProj.Name)
	corruptFile := filepath.Join(projectsDir, corruptUUID+".json")
	corruptContent := `{"name": "pw: corrupt-proj", "customUserGrant": true, CORRUPT_JSON`
	if err := os.WriteFile(corruptFile, []byte(corruptContent), 0644); err != nil {
		t.Fatalf("failed to write corrupt file: %v", err)
	}

	err = driver.SyncProject(corruptProj, "/home/user/wrk/projects/corrupt-proj")
	if err == nil {
		t.Errorf("expected SyncProject to return an error on corrupt existing JSON file, got nil")
	}
	afterCorrupt, _ := os.ReadFile(corruptFile)
	if string(afterCorrupt) != corruptContent {
		t.Errorf("CRITICAL DATA LOSS: existing corrupt JSON file was overwritten instead of preserved!\nGot:\n%s", string(afterCorrupt))
	}
}

// [DL-02] Test that selectVictimSlotForSwap fails closed when StatusPorcelain returns an error.
func TestSelectVictimSlotForSwap_FailsClosedOnStatusPorcelainError(t *testing.T) {
	mgr, git, _, _ := setupTestManager(t, 1) // 1 slot pool

	// Mount project-1 in pw-01
	res1, err := mgr.Use("project-1", "", "", LeaseModeWrite, "")
	if err != nil {
		t.Fatalf("Use project-1 failed: %v", err)
	}

	// Simulate a git status error (e.g. index.lock contention) on slot pw-01
	git.StatusErrors[res1.SlotPath] = fmt.Errorf("fatal: Unable to read .git/index.lock:File exists")

	// Attempt to mount project-2, which requires evicting pw-01
	_, err = mgr.Use("project-2", "", "", LeaseModeWrite, "")
	if err == nil {
		t.Fatalf("CRITICAL FAIL-OPEN BUG: expected Use(project-2) to fail when pw-01 StatusPorcelain returns an error, but pw-01 was evicted!")
	}
}

// [DL-05] Test that unmountToParkedLocked fails closed if SwitchDetach fails.
func TestPark_FailsClosedWhenSwitchDetachErrors(t *testing.T) {
	mgr, git, _, _ := setupTestManager(t, 2)

	res, err := mgr.Use("project-1", "", "", LeaseModeWrite, "")
	if err != nil {
		t.Fatalf("Use project-1 failed: %v", err)
	}

	// Simulate SwitchDetach failing (e.g. untracked file collision with origin/main)
	git.DetachErrors[res.SlotPath] = fmt.Errorf("error: The following untracked working tree files would be overwritten by checkout")

	err = mgr.Park("project-1", false)
	if err == nil {
		t.Fatalf("expected Park to return error when SwitchDetach fails, got nil")
	}

	// Verify state did not desynchronize: project-1 must still be MOUNTED in pw-01
	st, _ := mgr.Store.Load()
	proj := st.Projects["project-1"]
	if proj.Residency != ResidencyMounted || proj.Slot != "pw-01" {
		t.Errorf("STATE DESYNC: project-1 marked %s (slot %q) even though SwitchDetach failed!", proj.Residency, proj.Slot)
	}
}

// [DL-07] Test that Close refuses to close a project with unpushed local commits unless --force is passed.
func TestClose_RefusesUnmergedLocalCommitsWithoutForce(t *testing.T) {
	mgr, git, _, _ := setupTestManager(t, 2)

	res, err := mgr.Use("wip-feature", "", "", LeaseModeWrite, "")
	if err != nil {
		t.Fatalf("Use wip-feature failed: %v", err)
	}

	// Simulate 2 local commits ahead of origin/main (working tree clean)
	git.AheadCounts[res.SlotPath] = 2

	err = mgr.Close("wip-feature", false)
	if err == nil {
		t.Fatalf("expected Close(force=false) to refuse closing project with 2 unmerged commits ahead of origin/main, got nil")
	}

	// With force=true, Close should succeed
	if err := mgr.Close("wip-feature", true); err != nil {
		t.Fatalf("expected Close(force=true) to succeed, got: %v", err)
	}
}

// [SF-01] Test that Use with --cl fetches the Gerrit CL and checks out at FETCH_HEAD.
func TestUse_WithCLFlag_FetchesAndChecksOutCL(t *testing.T) {
	mgr, git, gerrit, _ := setupTestManager(t, 2)

	gerrit.CLRefs = map[string]string{
		"477945": "refs/changes/45/477945/3",
	}
	gerrit.CLIDs = map[string]string{
		"477945": "I9988776655443322110099887766554433221100",
	}

	res, err := mgr.Use("sensor-driver", "", "477945", LeaseModeWrite, "agent-1")
	if err != nil {
		t.Fatalf("Use with --cl 477945 failed: %v", err)
	}
	if git.FetchedRefs[res.SlotPath] != "refs/changes/45/477945/3" {
		t.Errorf("expected slot %s to fetch ref 'refs/changes/45/477945/3', got %q", res.SlotPath, git.FetchedRefs[res.SlotPath])
	}
	if res.ChangeID != "I9988776655443322110099887766554433221100" {
		t.Errorf("expected ChangeID I9988776655443322110099887766554433221100, got %q", res.ChangeID)
	}
}

// [SF-03] Test that List surfaces Gerrit offline warnings and BadgeGerritOffline instead of BadgeLocalWIP.
func TestList_SurfacesGerritOfflineWarningAndBadge(t *testing.T) {
	mgr, git, gerrit, _ := setupTestManager(t, 2)

	res, err := mgr.Use("active-cl-proj", "", "", LeaseModeWrite, "")
	if err != nil {
		t.Fatalf("Use failed: %v", err)
	}
	git.AheadCounts[res.SlotPath] = 1
	git.ChangeIDs[res.SlotPath] = "I1234567890123456789012345678901234567890"

	// Simulate Gerrit outage
	gerrit.QueryErr = fmt.Errorf("HTTP 503 Service Unavailable")

	report, err := mgr.List(context.Background())
	if err != nil {
		t.Fatalf("List failed: %v", err)
	}
	if len(report.Warnings) == 0 {
		t.Errorf("expected DashboardReport.Warnings to contain Gerrit API failure warning, got empty warnings")
	}
	if len(report.MountedProjects) != 1 {
		t.Fatalf("expected 1 mounted project, got %d", len(report.MountedProjects))
	}
	if report.MountedProjects[0].StatusBadge != BadgeGerritOffline {
		t.Errorf("expected StatusBadge %q when Gerrit is offline, got %q (%s)",
			BadgeGerritOffline, report.MountedProjects[0].StatusBadge, report.MountedProjects[0].Details)
	}
}

// [SF-04] Test that GarbageCollect deletes read-only (0555) Bazel output bases.
func TestBazelDriver_GarbageCollect_RemovesReadOnlyDirectories(t *testing.T) {
	tmpHome := t.TempDir()
	cacheRoot := filepath.Join(tmpHome, ".cache", "bazel", "_bazel_test")
	orphanDir := filepath.Join(cacheRoot, "deadbeef1234567890")
	readOnlySubdir := filepath.Join(orphanDir, "execroot", "pigweed")
	if err := os.MkdirAll(readOnlySubdir, 0755); err != nil {
		t.Fatalf("failed to create subdir: %v", err)
	}
	if err := os.WriteFile(filepath.Join(orphanDir, "DO_NOT_BUILD_HERE"), []byte("/nonexistent/deleted/worktree\n"), 0644); err != nil {
		t.Fatalf("failed to write DO_NOT_BUILD_HERE: %v", err)
	}
	readOnlyFile := filepath.Join(readOnlySubdir, "artifact.o")
	if err := os.WriteFile(readOnlyFile, []byte("binary data"), 0444); err != nil {
		t.Fatalf("failed to write artifact.o: %v", err)
	}
	// Make execroot/pigweed 0555 (read-only directory, standard Bazel behavior)
	if err := os.Chmod(readOnlySubdir, 0555); err != nil {
		t.Fatalf("failed to chmod 0555: %v", err)
	}
	defer os.Chmod(readOnlySubdir, 0755)

	driver := &BazelDriver{
		HomeDir:        tmpHome,
		BazelCacheRoot: cacheRoot,
	}
	report, err := driver.GarbageCollect(false, map[string]bool{})
	if err != nil {
		t.Fatalf("GarbageCollect failed: %v", err)
	}
	if report.RemovedCount != 1 {
		t.Errorf("expected RemovedCount == 1 for read-only orphan output_base, got %d", report.RemovedCount)
	}
	if _, statErr := os.Stat(orphanDir); !os.IsNotExist(statErr) {
		t.Errorf("expected orphanDir %s to be deleted from disk, but it still exists", orphanDir)
	}
}

// [DL-01 & DL-04] Test real ExecGitRunner: BranchExists 3-state check, non-forcing switch -c, and no silent HEAD fallback.
func TestExecGitRunner_SafeSwitchAndNoSilentHeadFallback(t *testing.T) {
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git binary not found in PATH")
	}
	runner := NewExecGitRunner()
	tmpDir := t.TempDir()
	repoDir := filepath.Join(tmpDir, "repo")
	if err := os.MkdirAll(repoDir, 0755); err != nil {
		t.Fatalf("mkdir failed: %v", err)
	}

	// Initialize real git repo with origin/main
	runCmd := func(dir string, args ...string) {
		t.Helper()
		if _, err := runner.runGit(dir, args...); err != nil {
			t.Fatalf("git %v failed: %v", args, err)
		}
	}
	runCmd(repoDir, "init", "-b", "main")
	runCmd(repoDir, "config", "user.email", "test@pigweed.dev")
	runCmd(repoDir, "config", "user.name", "Test User")
	if err := os.WriteFile(filepath.Join(repoDir, "README.md"), []byte("initial\n"), 0644); err != nil {
		t.Fatalf("write failed: %v", err)
	}
	runCmd(repoDir, "add", "README.md")
	runCmd(repoDir, "commit", "-m", "Initial commit")

	// Create branch project-a with an extra commit
	runCmd(repoDir, "checkout", "-b", "project-a")
	if err := os.WriteFile(filepath.Join(repoDir, "a.txt"), []byte("project a secret commit\n"), 0644); err != nil {
		t.Fatalf("write failed: %v", err)
	}
	runCmd(repoDir, "add", "a.txt")
	runCmd(repoDir, "commit", "-m", "Project A commit")
	projASHA, _ := runner.RevParse(repoDir, "HEAD")

	// [DL-04] Now attempt to create a new branch "project-b" from a non-existent ref "refs/nonexistent/base".
	// Previously, SwitchBranch silently fell back to "HEAD" (projASHA!), contaminating project-b with project-a's commit!
	err := runner.SwitchBranch(repoDir, "project-b", "refs/nonexistent/base")
	if err == nil {
		currentSHA, _ := runner.RevParse(repoDir, "HEAD")
		t.Fatalf("CRITICAL CONTAMINATION BUG [DL-04]: SwitchBranch with invalid createFromRef succeeded and branched off %s (projASHA=%s)!", currentSHA, projASHA)
	}
}
