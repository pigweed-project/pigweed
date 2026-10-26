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
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"
	"sync"
	"testing"

	"pigweed.dev/pw_ghish"
)

// Deterministic `wt list` subprocess budget (pw_ghish/AGENTS.md invariant
// #16). Budgets are counts, never wall-clock: raise them only with a CL
// description justifying the extra work.
const (
	// wtListBaseSubprocesses is the fixed cost of `wt list` independent of
	// how many slots are mounted.
	wtListBaseSubprocesses = 0
	// wtListPerSlotSubprocesses is the git cost per mounted slot. Any
	// per-slot fan-out (e.g. separate rev-list + log + status calls per
	// slot) shows up here as a multiplier. Measured: `git log -n 1
	// origin/main..HEAD` + `git status --porcelain` per slot.
	wtListPerSlotSubprocesses = 2
)

// countWTSubprocesses runs fn and returns the commands constructed
// (≈ subprocesses spawned) through pw_ghish.ExecCommand / ExecCommandContext
// while it ran.
func countWTSubprocesses(t *testing.T, fn func()) (int, []string) {
	t.Helper()
	var mu sync.Mutex
	var calls []string
	restore := pw_ghish.RecordSubprocessesForTesting(func(call string) {
		mu.Lock()
		calls = append(calls, call)
		mu.Unlock()
	})
	defer restore()
	fn()
	mu.Lock()
	defer mu.Unlock()
	slices.Sort(calls)
	return len(calls), calls
}

// newRealSlotPool creates a real git primary repo and a pool of slotCount
// real git worktree slots, each with a mounted project.
func newRealSlotPool(t *testing.T, slotCount int) *Manager {
	t.Helper()
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git binary not found in PATH")
	}
	tmpDir := t.TempDir()
	// Isolate git from the developer's system/global config and HOME so the
	// counts are identical on every machine.
	home := filepath.Join(tmpDir, "home")
	if err := os.MkdirAll(home, 0755); err != nil {
		t.Fatal(err)
	}
	globalCfg := filepath.Join(home, "gitconfig")
	if err := os.WriteFile(globalCfg, nil, 0644); err != nil {
		t.Fatal(err)
	}
	t.Setenv("HOME", home)
	t.Setenv("XDG_CONFIG_HOME", home)
	t.Setenv("GIT_CONFIG_NOSYSTEM", "1")
	t.Setenv("GIT_CONFIG_GLOBAL", globalCfg)
	// Pin the files ref backend: under reftable, HEAD is a stub that current
	// branch detection reads as ".invalid", which changes the counted calls.
	t.Setenv("GIT_DEFAULT_REF_FORMAT", "files")
	primaryRepo := filepath.Join(tmpDir, "primary-repo")
	if err := os.MkdirAll(primaryRepo, 0755); err != nil {
		t.Fatal(err)
	}
	runGit := func(args ...string) {
		t.Helper()
		var out bytes.Buffer
		fullArgs := append([]string{"-c", "commit.gpgsign=false"}, args...)
		if err := (&pw_ghish.RealGitRunner{Dir: primaryRepo}).Run(t.Context(), &out, &out, fullArgs...); err != nil {
			t.Fatalf("git %s failed: %v\n%s", strings.Join(args, " "), err, out.String())
		}
	}
	runGit("init", "-b", "main")
	runGit("config", "user.email", "test@pigweed.dev")
	runGit("config", "user.name", "Pigweed Test")
	if err := os.WriteFile(filepath.Join(primaryRepo, "README.md"), []byte("# Test\n"), 0644); err != nil {
		t.Fatal(err)
	}
	runGit("add", "README.md")
	runGit("commit", "-m", "Initial commit\n\nChange-Id: I1234567890123456789012345678901234567890")
	runGit("remote", "add", "origin", primaryRepo)
	runGit("fetch", "origin")

	store := NewStateStore(filepath.Join(tmpDir, "state", "worktrees.json"))
	if err := store.Save(NewEmptyState(filepath.Join(tmpDir, "slots"), filepath.Join(tmpDir, "projects"), primaryRepo, slotCount)); err != nil {
		t.Fatal(err)
	}
	mgr := NewManager(store, NewExecGitRunner(), nil, NoopIDEDriver{}, &MockGerritStatus{Statuses: map[string]ChangeStatus{}})
	if _, err := mgr.Init(slotCount, false); err != nil {
		t.Fatalf("Init(%d) failed: %v", slotCount, err)
	}
	for i := range slotCount {
		name := fmt.Sprintf("proj-%02d", i)
		if _, err := mgr.Use(name, "", "", LeaseModeWrite, "conv-"+name); err != nil {
			t.Fatalf("Use(%q) failed: %v", name, err)
		}
	}
	return mgr
}

// TestPerfBudget_WTListPerSlotCostIsConstant measures `wt list` against 3 and
// 6 real slots and asserts the subprocess cost is base + perSlot×slots, which
// catches a reintroduced per-slot fan-out of git queries.
func TestPerfBudget_WTListPerSlotCostIsConstant(t *testing.T) {
	measure := func(slots int) (int, []string) {
		mgr := newRealSlotPool(t, slots)
		var out string
		var err error
		n, calls := countWTSubprocesses(t, func() {
			out, err = execWTCommand(t, mgr, "list")
		})
		if err != nil {
			t.Fatalf("wt list (%d slots) failed: %v\n%s", slots, err, out)
		}
		return n, calls
	}
	n3, calls3 := measure(3)
	n6, calls6 := measure(6)

	for _, tc := range []struct {
		slots int
		got   int
		calls []string
	}{{3, n3, calls3}, {6, n6, calls6}} {
		want := wtListBaseSubprocesses + wtListPerSlotSubprocesses*tc.slots
		switch {
		case tc.got > want:
			t.Errorf("wt list with %d slots constructed %d commands (≈ subprocesses), budget %d (base %d + %d/slot).\nRecorded calls:\n  %s\n\n"+
				"Perf budgets are exact deterministic counts, never wall-clock (pw_ghish/AGENTS.md invariant #16). "+
				"Batch per-slot git queries instead of fanning out; if the increase is intended, raise the "+
				"number here and justify it in the CL description.",
				tc.slots, tc.got, want, wtListBaseSubprocesses, wtListPerSlotSubprocesses, strings.Join(tc.calls, "\n  "))
		case tc.got < want:
			t.Errorf("wt list with %d slots: budget is stale, lower it to %d (pinned %d).\nRecorded calls:\n  %s",
				tc.slots, tc.got, want, strings.Join(tc.calls, "\n  "))
		}
	}
	if n6-n3 != 3*wtListPerSlotSubprocesses {
		t.Errorf("wt list per-slot cost = (%d-%d)/3, want exactly %d per slot", n6, n3, wtListPerSlotSubprocesses)
	}
}
