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
	"sync"
	"testing"
	"time"
)

func TestStateStore_ConcurrentFlockAndAtomicSave(t *testing.T) {
	tmpDir := t.TempDir()
	store := NewStateStore(filepath.Join(tmpDir, "worktrees.json"))
	initial := NewEmptyState("/slots", "/projects", "/repo", 4)
	if err := store.Save(initial); err != nil {
		t.Fatalf("initial Save failed: %v", err)
	}

	var wg sync.WaitGroup
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func(idx int) {
			defer wg.Done()
			err := store.WithLock(func() error {
				st, err := store.Load()
				if err != nil {
					return err
				}
				st.Projects[string(rune('a'+idx))] = &Project{
					Name: string(rune('a' + idx)),
				}
				return store.Save(st)
			})
			if err != nil {
				t.Errorf("concurrent WithLock failed: %v", err)
			}
		}(i)
	}
	wg.Wait()

	finalState, err := store.Load()
	if err != nil {
		t.Fatalf("final Load failed: %v", err)
	}
	if len(finalState.Projects) != 8 {
		t.Errorf("expected all 8 concurrent project writes preserved by flock, got %d", len(finalState.Projects))
	}
}

func TestSlot_LeaseExpirationAndUpsert(t *testing.T) {
	now := time.Now()
	slot := &Slot{Name: "pw-01"}

	slot.UpsertLease("agent-1", LeaseModeWrite, now.Add(-40*time.Minute))
	if active := slot.ActiveWriteLease(now); active != nil {
		t.Errorf("expected 40m old lease to be expired (TTL 30m), got active: %+v", active)
	}

	slot.UpsertLease("agent-2", LeaseModeWrite, now.Add(-10*time.Minute))
	if active := slot.ActiveWriteLease(now); active == nil || active.AgentID != "agent-2" {
		t.Errorf("expected 10m old lease for agent-2 to be active, got %+v", active)
	}
}

// TODO: b/586589869 - Remove legacy pw_ghish deferred-migration test after rollout.
func TestDefaultStateStore_DefersMigrationUntilSave(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	t.Setenv("USERPROFILE", home)
	t.Setenv("XDG_CONFIG_HOME", "")

	legacyDir := filepath.Join(home, ".config", "pw_ghish")
	if err := os.MkdirAll(legacyDir, 0o755); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}
	initial := NewEmptyState("/slots", "/projects", "/repo", 4)
	if err := NewStateStore(filepath.Join(legacyDir, "worktrees.json")).Save(initial); err != nil {
		t.Fatalf("seed legacy state: %v", err)
	}

	// DefaultStateStore() and Load() must stay strictly read-only.
	store, err := DefaultStateStore()
	if err != nil {
		t.Fatalf("DefaultStateStore: %v", err)
	}
	if _, err := store.Load(); err != nil {
		t.Fatalf("Load: %v", err)
	}
	if _, err := os.Lstat(filepath.Join(home, ".config", "ghish")); !os.IsNotExist(err) {
		t.Fatalf("DefaultStateStore/Load unexpectedly created canonical ghish dir (err=%v)", err)
	}
	if info, err := os.Lstat(legacyDir); err != nil || !info.IsDir() {
		t.Fatalf("expected legacy dir untouched before Save, got info=%v err=%v", info, err)
	}

	// Save() inside WithLock() performs the deferred migration on Unix.
	if err := store.WithLock(func() error {
		st, err := store.Load()
		if err != nil {
			return err
		}
		st.Projects["migrated"] = &Project{Name: "migrated"}
		return store.Save(st)
	}); err != nil {
		t.Fatalf("WithLock/Save: %v", err)
	}

	if runtime.GOOS != "windows" {
		wantPath := filepath.Join(home, ".config", "ghish", "worktrees.json")
		if store.StateFilePath != wantPath {
			t.Errorf("StateFilePath after Save = %q, want %q", store.StateFilePath, wantPath)
		}
		if target, err := os.Readlink(legacyDir); err != nil || target != "ghish" {
			t.Errorf("Readlink(%s) = (%q, %v), want (\"ghish\", nil)", legacyDir, target, err)
		}
	}
	loaded, err := store.Load()
	if err != nil || loaded.Projects["migrated"] == nil {
		t.Fatalf("Load after Save = (%+v, %v), want migrated project", loaded, err)
	}
}

// TODO: b/586589869 - Remove legacy pw_ghish split-dir Save() migration test after rollout.
func TestDefaultStateStore_MigratesWorktreesJSONWhenCanonicalDirExists(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	t.Setenv("USERPROFILE", home)
	t.Setenv("XDG_CONFIG_HOME", "")

	// Pre-create canonical ~/.config/ghish/ WITHOUT worktrees.json alongside
	// legacy ~/.config/pw_ghish/worktrees.json.
	canonicalDir := filepath.Join(home, ".config", "ghish")
	legacyDir := filepath.Join(home, ".config", "pw_ghish")
	if err := os.MkdirAll(canonicalDir, 0o755); err != nil {
		t.Fatalf("MkdirAll(canonical): %v", err)
	}
	if err := os.MkdirAll(legacyDir, 0o755); err != nil {
		t.Fatalf("MkdirAll(legacy): %v", err)
	}
	initial := NewEmptyState("/slots", "/projects", "/repo", 4)
	initial.Projects["existing"] = &Project{Name: "existing"}
	if err := NewStateStore(filepath.Join(legacyDir, "worktrees.json")).Save(initial); err != nil {
		t.Fatalf("seed legacy state: %v", err)
	}

	// Read-only DefaultStateStore + Load reads existing legacy state.
	store, err := DefaultStateStore()
	if err != nil {
		t.Fatalf("DefaultStateStore: %v", err)
	}
	st, err := store.Load()
	if err != nil || st.Projects["existing"] == nil {
		t.Fatalf("Load() = (%+v, %v), want existing project from legacy file", st, err)
	}

	// Save() writes to canonical ~/.config/ghish/worktrees.json instead of staying on legacy.
	st.Projects["saved"] = &Project{Name: "saved"}
	if err := store.Save(st); err != nil {
		t.Fatalf("Save: %v", err)
	}
	wantPath := filepath.Join(canonicalDir, "worktrees.json")
	if store.StateFilePath != wantPath {
		t.Errorf("StateFilePath after Save = %q, want %q", store.StateFilePath, wantPath)
	}
	if _, err := os.Stat(wantPath); err != nil {
		t.Fatalf("expected canonical state file at %s: %v", wantPath, err)
	}

	// A fresh DefaultStateStore() now reads from canonical with both projects intact.
	reopened, err := DefaultStateStore()
	if err != nil {
		t.Fatalf("DefaultStateStore(reopened): %v", err)
	}
	if reopened.StateFilePath != wantPath {
		t.Errorf("reopened StateFilePath = %q, want %q", reopened.StateFilePath, wantPath)
	}
	reloaded, err := reopened.Load()
	if err != nil || reloaded.Projects["existing"] == nil || reloaded.Projects["saved"] == nil {
		t.Fatalf("reopened Load() = (%+v, %v), want both existing and saved projects", reloaded, err)
	}
}
