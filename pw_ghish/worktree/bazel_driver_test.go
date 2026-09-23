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
