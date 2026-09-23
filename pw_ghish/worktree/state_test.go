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
	"path/filepath"
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
