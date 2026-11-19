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
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func TestBazelDriver_CheckAndConfigureIdempotent(t *testing.T) {
	homeDir := t.TempDir()
	cacheRoot := filepath.Join(homeDir, ".cache", "bazel", "_bazel_test")
	driver := &BazelDriver{
		HomeDir:        homeDir,
		BazelCacheRoot: cacheRoot,
	}

	// Pre-populate user ~/.bazelrc with custom user settings
	userRc := filepath.Join(homeDir, ".bazelrc")
	originalUserLines := "# My custom Bazel settings\nbuild --jobs=32\n"
	if err := os.WriteFile(userRc, []byte(originalUserLines), 0644); err != nil {
		t.Fatalf("failed to write user .bazelrc: %v", err)
	}

	validPaths := map[string]bool{"/valid/path": true}

	// Run 1: fix=true
	items1, err := driver.CheckAndConfigure(true, validPaths)
	if err != nil {
		t.Fatalf("CheckAndConfigure run 1 failed: %v", err)
	}
	if len(items1) == 0 {
		t.Fatalf("expected checklist items")
	}

	rcAfter1, err := os.ReadFile(userRc)
	if err != nil {
		t.Fatalf("failed to read .bazelrc after run 1: %v", err)
	}
	if !strings.Contains(string(rcAfter1), "build --jobs=32") {
		t.Errorf("user custom line was clobbered in .bazelrc")
	}
	if strings.Count(string(rcAfter1), "try-import") != 1 {
		t.Errorf("expected exactly 1 try-import line, got %d", strings.Count(string(rcAfter1), "try-import"))
	}

	// Run 2: fix=true again -> must be 100% idempotent (still exactly 1 try-import line)
	_, err = driver.CheckAndConfigure(true, validPaths)
	if err != nil {
		t.Fatalf("CheckAndConfigure run 2 failed: %v", err)
	}
	rcAfter2, _ := os.ReadFile(userRc)
	if strings.Count(string(rcAfter2), "try-import") != 1 {
		t.Errorf("expected idempotent try-import count 1 after run 2, got %d", strings.Count(string(rcAfter2), "try-import"))
	}
	wantImport := "try-import " + filepath.Join(homeDir, ".config", "ghish", "bazelrc.worktrees")
	if !strings.Contains(string(rcAfter2), wantImport) {
		t.Errorf("expected %q in .bazelrc, got:\n%s", wantImport, string(rcAfter2))
	}
}

// TODO: b/586589869 - Remove legacy bazelrc migration test after rollout.
func TestBazelDriver_UpgradesLegacyTryImportInPlace(t *testing.T) {
	homeDir := t.TempDir()
	driver := &BazelDriver{
		HomeDir:        homeDir,
		BazelCacheRoot: filepath.Join(homeDir, ".cache", "bazel", "_bazel_test"),
	}

	legacyDir := filepath.Join(homeDir, ".config", "pw_ghish")
	if err := os.MkdirAll(legacyDir, 0755); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}
	legacySnippet := filepath.Join(legacyDir, "bazelrc.worktrees")
	if err := os.WriteFile(legacySnippet, []byte(driver.expectedSnippetContent()), 0644); err != nil {
		t.Fatalf("WriteFile(legacySnippet): %v", err)
	}
	userRc := filepath.Join(homeDir, ".bazelrc")
	initialRc := "build --jobs=16\ntry-import " + legacySnippet + "\n"
	if err := os.WriteFile(userRc, []byte(initialRc), 0644); err != nil {
		t.Fatalf("WriteFile(.bazelrc): %v", err)
	}

	if _, err := driver.CheckAndConfigure(true, map[string]bool{}); err != nil {
		t.Fatalf("CheckAndConfigure(true): %v", err)
	}

	updatedRc, err := os.ReadFile(userRc)
	if err != nil {
		t.Fatalf("ReadFile(.bazelrc): %v", err)
	}
	wantImport := "try-import " + filepath.Join(homeDir, ".config", "ghish", "bazelrc.worktrees")
	if runtime.GOOS == "windows" {
		wantImport = "try-import " + legacySnippet
	}
	if strings.Count(string(updatedRc), "try-import") != 1 || !strings.Contains(string(updatedRc), wantImport) {
		t.Errorf("expected legacy try-import replaced in place with %q, got:\n%s", wantImport, string(updatedRc))
	}
	if !strings.Contains(string(updatedRc), "build --jobs=16") {
		t.Errorf("expected custom user bazelrc line preserved, got:\n%s", string(updatedRc))
	}
}

// TODO: b/586589869 - Remove legacy bazelrc check/XDG migration tests after rollout.
func TestBazelDriver_CheckWarnsOnLegacyImportAndUpgradesAcrossCustomXDG(t *testing.T) {
	homeDir := t.TempDir()
	xdgDir := filepath.Join(t.TempDir(), "custom-xdg")
	legacyDir := filepath.Join(homeDir, ".config", "pw_ghish")
	if err := os.MkdirAll(legacyDir, 0o755); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}
	legacySnippet := filepath.Join(legacyDir, "bazelrc.worktrees")
	if err := os.WriteFile(legacySnippet, []byte("common --experimental_repository_cache_hardlinks\ncommon --experimental_disk_cache_gc_max_size=80G\n"), 0o644); err != nil {
		t.Fatalf("WriteFile(snippet): %v", err)
	}
	userRc := filepath.Join(homeDir, ".bazelrc")
	if err := os.WriteFile(userRc, []byte("build --jobs=8\ntry-import "+legacySnippet+"\n"), 0o644); err != nil {
		t.Fatalf("WriteFile(.bazelrc): %v", err)
	}

	driver := &BazelDriver{
		HomeDir:        homeDir,
		ConfigBaseDir:  xdgDir,
		BazelCacheRoot: filepath.Join(homeDir, ".cache", "bazel", "_bazel_test"),
	}

	// 1. Read-only --check warns about legacy try-import (on Unix) and does not mutate disk.
	checkItems, err := driver.CheckAndConfigure(false, nil)
	if err != nil {
		t.Fatalf("CheckAndConfigure(false): %v", err)
	}
	if _, err := os.Lstat(filepath.Join(xdgDir, "ghish")); !os.IsNotExist(err) {
		t.Fatalf("CheckAndConfigure(false) unexpectedly migrated config dir (err=%v)", err)
	}
	var sawLegacyWarning bool
	for _, it := range checkItems {
		if it.Category == "User ~/.bazelrc Hook" && it.Status == ChecklistWarning && strings.Contains(it.Summary, "Legacy `try-import") {
			sawLegacyWarning = true
		}
	}
	if runtime.GOOS != "windows" && !sawLegacyWarning {
		t.Errorf("expected ChecklistWarning for legacy try-import under --check, got: %+v", checkItems)
	}

	// 2. fix=true replaces ~/.config/pw_ghish try-import in-place without duplicating.
	if _, err := driver.CheckAndConfigure(true, nil); err != nil {
		t.Fatalf("CheckAndConfigure(true): %v", err)
	}
	updatedRc, err := os.ReadFile(userRc)
	if err != nil {
		t.Fatalf("ReadFile(.bazelrc): %v", err)
	}
	wantImport := "try-import " + filepath.Join(xdgDir, "ghish", "bazelrc.worktrees")
	if runtime.GOOS == "windows" {
		wantImport = "try-import " + legacySnippet
	}
	if strings.Count(string(updatedRc), "try-import") != 1 || !strings.Contains(string(updatedRc), wantImport) {
		t.Errorf("expected single upgraded try-import %q, got:\n%s", wantImport, string(updatedRc))
	}
}

func TestBazelDriver_GarbageCollectOrphanedOutputBases(t *testing.T) {
	tmpDir := t.TempDir()
	cacheRoot := filepath.Join(tmpDir, "_bazel_test")
	if err := os.MkdirAll(cacheRoot, 0755); err != nil {
		t.Fatalf("failed to create cache root: %v", err)
	}

	// 1. Active slot directory in pool -> KEEP
	activeSlotDir := filepath.Join(tmpDir, "wrk", "slots", "pw-01")
	_ = os.MkdirAll(activeSlotDir, 0755)
	obActive := filepath.Join(cacheRoot, "11111111111111111111111111111111")
	_ = os.MkdirAll(obActive, 0755)
	_ = os.WriteFile(filepath.Join(obActive, "DO_NOT_BUILD_HERE"), []byte(activeSlotDir+"\n"), 0644)

	// 2. Live checkout outside pool (e.g. ~/wrk/pw-wt) -> KEEP (NEVER delete active user checkouts!)
	outsideLiveDir := filepath.Join(tmpDir, "wrk", "pw-wt")
	_ = os.MkdirAll(outsideLiveDir, 0755)
	obOutsideLive := filepath.Join(cacheRoot, "22222222222222222222222222222222")
	_ = os.MkdirAll(obOutsideLive, 0755)
	_ = os.WriteFile(filepath.Join(obOutsideLive, "DO_NOT_BUILD_HERE"), []byte(outsideLiveDir+"\n"), 0644)

	// 3. Deleted workspace directory -> ORPHAN (Remove)
	obDeleted := filepath.Join(cacheRoot, "33333333333333333333333333333333")
	_ = os.MkdirAll(obDeleted, 0755)
	_ = os.WriteFile(filepath.Join(obDeleted, "README"), []byte("WORKSPACE: /tmp/does-not-exist-at-all-99999\n"), 0644)

	// 4. Retired slot directory inside ~/wrk/slots/ -> ORPHAN (Remove)
	retiredSlotDir := filepath.Join(tmpDir, "wrk", "slots", "pw-99")
	_ = os.MkdirAll(retiredSlotDir, 0755)
	obRetiredSlot := filepath.Join(cacheRoot, "44444444444444444444444444444444")
	_ = os.MkdirAll(obRetiredSlot, 0755)
	_ = os.WriteFile(filepath.Join(obRetiredSlot, "DO_NOT_BUILD_HERE"), []byte(retiredSlotDir+"\n"), 0644)

	driver := &BazelDriver{
		HomeDir:        tmpDir,
		BazelCacheRoot: cacheRoot,
	}
	validPaths := map[string]bool{
		activeSlotDir: true,
	}

	// Dry run first
	reportDry, err := driver.GarbageCollect(true, validPaths)
	if err != nil {
		t.Fatalf("GarbageCollect dryRun failed: %v", err)
	}
	if len(reportDry.OrphansFound) != 2 {
		t.Fatalf("expected 2 orphans in dry run (deleted + retired slot), got %d: %+v", len(reportDry.OrphansFound), reportDry.OrphansFound)
	}
	if _, err := os.Stat(obDeleted); err != nil {
		t.Errorf("dryRun should not have deleted %s", obDeleted)
	}

	// Real GC run
	reportReal, err := driver.GarbageCollect(false, validPaths)
	if err != nil {
		t.Fatalf("GarbageCollect real failed: %v", err)
	}
	if reportReal.RemovedCount != 2 {
		t.Errorf("expected RemovedCount=2, got %d", reportReal.RemovedCount)
	}
	if _, err := os.Stat(obActive); err != nil {
		t.Errorf("active slot output_base was wrongly deleted!")
	}
	if _, err := os.Stat(obOutsideLive); err != nil {
		t.Errorf("live checkout outside pool was wrongly deleted!")
	}
	if _, err := os.Stat(obDeleted); !os.IsNotExist(err) {
		t.Errorf("expected deleted workspace output_base to be removed")
	}
	if _, err := os.Stat(obRetiredSlot); !os.IsNotExist(err) {
		t.Errorf("expected retired slot output_base to be removed")
	}
}
