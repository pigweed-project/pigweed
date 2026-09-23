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
	"time"

	"pigweed.dev/pw_ghish"
)

// DiagnosticWarning represents a non-fatal warning from an ambient subsystem (e.g., Gerrit offline or IDE sync).
type DiagnosticWarning struct {
	Subsystem   string `json:"subsystem"`
	Message     string `json:"message"`
	Remediation string `json:"remediation,omitempty"`
}

// UseResult contains the resolved paths and metadata returned by `gh wt use`.
type UseResult struct {
	Project     string              `json:"project"`
	Slot        string              `json:"slot"`
	SlotPath    string              `json:"slot_path"`
	SymlinkPath string              `json:"symlink_path"`
	Branch      string              `json:"branch"`
	IssueID     int64               `json:"issue_id,omitempty"`
	ChangeID    string              `json:"change_id,omitempty"`
	Mode        LeaseMode           `json:"mode"`
	ForkedFrom  string              `json:"forked_from,omitempty"`
	SwappedOut  string              `json:"swapped_out,omitempty"`
	Warnings    []DiagnosticWarning `json:"warnings,omitempty"`
}

// ProjectListEntry represents a single row in the `gh wt list` dashboard.
type ProjectListEntry struct {
	Project           string      `json:"project"`
	Residency         Residency   `json:"residency"`
	Slot              string      `json:"slot"` // "pw-01" or "-"
	Branch            string      `json:"branch"`
	IssueID           int64       `json:"issue_id,omitempty"`
	StatusBadge       StatusBadge `json:"status_badge"`
	Details           string      `json:"details"`
	RecommendedAction string      `json:"recommended_action"`
	ActiveLeases      []string    `json:"active_leases,omitempty"`
	IsDirty           bool        `json:"is_dirty"`
}

// DashboardReport contains the full output of `gh wt list`.
type DashboardReport struct {
	TotalSlots      int                 `json:"total_slots"`
	OccupiedSlots   int                 `json:"occupied_slots"`
	AvailableSlots  int                 `json:"available_slots"`
	MountedProjects []ProjectListEntry  `json:"mounted_projects"`
	ParkedProjects  []ProjectListEntry  `json:"parked_projects"`
	Warnings        []DiagnosticWarning `json:"warnings,omitempty"`
}

// Manager coordinates state persistence, Git worktrees, symlinks, Bazel caches, and IDE sync.
type Manager struct {
	Store        *StateStore
	Git          GitRunner
	BuildDriver  BuildEnvDriver
	IDEDriver    IDEIntegrationDriver
	GerritStatus GerritStatusProvider
	Now          func() time.Time
}

// NewManager constructs a Manager with explicit dependencies.
func NewManager(
	store *StateStore,
	git GitRunner,
	buildDriver BuildEnvDriver,
	ideDriver IDEIntegrationDriver,
	gerritStatus GerritStatusProvider,
) *Manager {
	if gerritStatus == nil {
		gerritStatus = NoopGerritStatusProvider{}
	}
	return &Manager{
		Store:        store,
		Git:          git,
		BuildDriver:  buildDriver,
		IDEDriver:    ideDriver,
		GerritStatus: gerritStatus,
		Now:          time.Now,
	}
}

func defaultPaths() (poolRoot, projectsDir, primaryRepo string, err error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return "", "", "", fmt.Errorf("failed to resolve home directory: %w", err)
	}
	poolRoot = filepath.Join(home, "wrk", "slots")
	projectsDir = filepath.Join(home, "wrk", "projects")
	primaryRepo = filepath.Join(home, "wrk", "pigweed")
	if _, statErr := os.Stat(primaryRepo); os.IsNotExist(statErr) {
		// Fall back to current working directory only if it is a valid git repository
		if cwd, cwdErr := os.Getwd(); cwdErr == nil {
			if _, gitStatErr := os.Stat(filepath.Join(cwd, ".git")); gitStatErr == nil {
				primaryRepo = cwd
			}
		}
	}
	return poolRoot, projectsDir, primaryRepo, nil
}

// loadOrInitState loads State from Store or initializes default state in memory.
func (m *Manager) loadOrInitState(defaultSlots int) (*State, error) {
	st, err := m.Store.Load()
	if err == nil {
		if defaultSlots > 0 && st.SlotCount != defaultSlots {
			st.SlotCount = defaultSlots
		}
		if st.SlotCount <= 0 {
			st.SlotCount = 10
		}
		return st, nil
	}
	if !os.IsNotExist(err) {
		return nil, err
	}
	poolRoot, projectsDir, primaryRepo, pathErr := defaultPaths()
	if pathErr != nil {
		return nil, pathErr
	}
	if defaultSlots <= 0 {
		defaultSlots = 10
	}
	return NewEmptyState(poolRoot, projectsDir, primaryRepo, defaultSlots), nil
}

// Init idempotently examines and converges the worktree slot pool, symlinks, hooks, and build caches.
func (m *Manager) Init(slotCount int, checkOnly bool) ([]ChecklistItem, error) {
	var items []ChecklistItem

	err := m.Store.WithLock(func() error {
		st, err := m.loadOrInitState(slotCount)
		if err != nil {
			return err
		}
		if slotCount > 0 {
			st.SlotCount = slotCount
		}

		// 1. Primary Repository check
		if _, err := os.Stat(st.PrimaryRepo); err != nil {
			items = append(items, ChecklistItem{
				Category: "Git Primary Repo",
				Status:   ChecklistError,
				Summary:  fmt.Sprintf("Primary repository not found at %s", st.PrimaryRepo),
			})
			return fmt.Errorf("primary repository not found at %s", st.PrimaryRepo)
		}
		items = append(items, ChecklistItem{
			Category: "Git Primary Repo",
			Status:   ChecklistOK,
			Summary:  fmt.Sprintf("%s", st.PrimaryRepo),
		})

		// 2. Gerrit commit-msg hook check
		if !checkOnly {
			installed, hookErr := m.Git.EnsureCommitMsgHook(st.PrimaryRepo)
			if hookErr != nil {
				items = append(items, ChecklistItem{
					Category: "Gerrit commit-msg Hook",
					Status:   ChecklistWarning,
					Summary:  fmt.Sprintf("Could not verify commit-msg hook: %v", hookErr),
				})
			} else if installed {
				items = append(items, ChecklistItem{
					Category: "Gerrit commit-msg Hook",
					Status:   ChecklistRepaired,
					Summary:  "Installed Change-Id commit-msg hook in primary repository",
				})
			} else {
				items = append(items, ChecklistItem{
					Category: "Gerrit commit-msg Hook",
					Status:   ChecklistOK,
					Summary:  "Verified in primary repository & shared across worktrees",
				})
			}
		} else {
			items = append(items, ChecklistItem{
				Category: "Gerrit commit-msg Hook",
				Status:   ChecklistOK,
				Summary:  "Checked in primary repository",
			})
		}

		// 3. Ensure PoolRoot and ProjectsDir exist
		if !checkOnly {
			if err := os.MkdirAll(st.PoolRoot, 0755); err != nil {
				return fmt.Errorf("failed to create pool root %s: %w", st.PoolRoot, err)
			}
			if err := os.MkdirAll(st.ProjectsDir, 0755); err != nil {
				return fmt.Errorf("failed to create projects symlink dir %s: %w", st.ProjectsDir, err)
			}
		}

		// 4. Converge N physical slots
		createdSlots := 0
		verifiedSlots := 0
		validPaths := map[string]bool{
			st.PrimaryRepo: true,
		}
		for i := 1; i <= st.SlotCount; i++ {
			slotName := fmt.Sprintf("pw-%02d", i)
			slotPath := filepath.Join(st.PoolRoot, slotName)
			validPaths[slotPath] = true

			slot, exists := st.Slots[slotName]
			if !exists {
				slot = &Slot{
					Name: slotName,
					Path: slotPath,
				}
				st.Slots[slotName] = slot
			} else {
				slot.Path = slotPath
			}

			if _, statErr := os.Stat(filepath.Join(slotPath, ".git")); statErr == nil {
				verifiedSlots++
			} else if !checkOnly {
				if err := m.Git.WorktreeAddDetached(st.PrimaryRepo, slotPath); err != nil {
					return fmt.Errorf("failed to initialize worktree slot %s at %s: %w", slotName, slotPath, err)
				}
				createdSlots++
			}
		}

		slotStatus := ChecklistOK
		slotSummary := fmt.Sprintf("%d/%d slots ready in %s", verifiedSlots+createdSlots, st.SlotCount, st.PoolRoot)
		if createdSlots > 0 {
			slotStatus = ChecklistRepaired
			slotSummary = fmt.Sprintf("Created %d new slot(s); %d/%d ready in %s", createdSlots, verifiedSlots+createdSlots, st.SlotCount, st.PoolRoot)
		} else if verifiedSlots < st.SlotCount && checkOnly {
			slotStatus = ChecklistWarning
			slotSummary = fmt.Sprintf("%d/%d slots initialized in %s (run `./gh wt init` to create missing slots)", verifiedSlots, st.SlotCount, st.PoolRoot)
		}
		items = append(items, ChecklistItem{
			Category: "Worktree Slot Pool",
			Status:   slotStatus,
			Summary:  slotSummary,
		})

		// 5. Verify Project Symlinks
		mountedCount := 0
		parkedCount := 0
		repairedSymlinks := 0
		for _, proj := range st.Projects {
			if proj.Residency == ResidencyMounted && proj.Slot != "" {
				mountedCount++
				slot, ok := st.Slots[proj.Slot]
				if ok {
					symlinkPath := filepath.Join(st.ProjectsDir, proj.Name)
					target, err := os.Readlink(symlinkPath)
					if err != nil || target != slot.Path {
						if !checkOnly {
							if err := m.ensureSymlink(slot.Path, symlinkPath); err != nil {
								return err
							}
							repairedSymlinks++
						}
					}
				}
			} else {
				parkedCount++
			}
		}

		symStatus := ChecklistOK
		symSummary := fmt.Sprintf("%s (%d mounted symlinks, %d parked projects tracked)", st.ProjectsDir, mountedCount, parkedCount)
		if repairedSymlinks > 0 {
			symStatus = ChecklistRepaired
			symSummary = fmt.Sprintf("Repaired %d symlink(s) in %s (%d mounted, %d parked)", repairedSymlinks, st.ProjectsDir, mountedCount, parkedCount)
		}
		items = append(items, ChecklistItem{
			Category: "Project Symlinks Dir",
			Status:   symStatus,
			Summary:  symSummary,
		})

		// 6. IDE Integration Health Check
		if m.IDEDriver != nil {
			ideItem, err := m.IDEDriver.CheckHealth()
			if err == nil {
				items = append(items, ideItem)
			}
		}

		// 7. Build Environment & Bazel Cache Driver Check
		if m.BuildDriver != nil {
			buildItems, err := m.BuildDriver.CheckAndConfigure(!checkOnly, validPaths)
			if err != nil {
				return err
			}
			items = append(items, buildItems...)
		}

		if !checkOnly {
			if err := m.Store.Save(st); err != nil {
				return err
			}
		}
		return nil
	})

	return items, err
}

// Use allocates or resumes a project, performing automatic LRU slot swapping if all N slots are full,
// or automatic warm forking if a concurrent writer lease collision occurs.
func (m *Manager) Use(projectName, branchName, clRef string, mode LeaseMode, agentID string) (*UseResult, error) {
	projectName = strings.TrimSpace(projectName)
	if projectName == "" {
		return nil, fmt.Errorf("project name cannot be empty\nRemediation: Provide a project name, e.g., `./gh wt use rpc-fix`")
	}
	if mode == "" {
		mode = LeaseModeWrite
	}
	if branchName == "" {
		branchName = projectName
	}

	var result *UseResult
	err := m.Store.WithLock(func() error {
		st, err := m.loadOrInitState(0)
		if err != nil {
			return err
		}
		now := m.Now()

		// Ensure slot entries exist in state map
		for i := 1; i <= st.SlotCount; i++ {
			sName := fmt.Sprintf("pw-%02d", i)
			if _, exists := st.Slots[sName]; !exists {
				st.Slots[sName] = &Slot{
					Name: sName,
					Path: filepath.Join(st.PoolRoot, sName),
				}
			}
		}

		proj, exists := st.Projects[projectName]
		if exists && proj.Residency == ResidencyMounted && proj.Slot != "" {
			slot, slotOk := st.Slots[proj.Slot]
			if slotOk {
				// Check for concurrent Writer Lease collision!
				activeWriter := slot.ActiveWriteLease(now)
				if mode == LeaseModeWrite && activeWriter != nil && agentID != "" && activeWriter.AgentID != agentID {
					// AUTOMATIC WARM FORK!
					forkName := m.nextForkName(st, projectName)
					forkRes, forkErr := m.allocateAndMountLocked(st, forkName, proj.Branch, clRef, slot.Path, mode, agentID, now)
					if forkErr != nil {
						return forkErr
					}
					forkRes.ForkedFrom = projectName
					result = forkRes
					return m.Store.Save(st)
				}

				// No collision: update lease, verify symlink & IDE sync
				slot.UpsertLease(agentID, mode, now)
				proj.LastUsedAt = now
				symlinkPath := filepath.Join(st.ProjectsDir, proj.Name)
				if err := m.ensureSymlink(slot.Path, symlinkPath); err != nil {
					return err
				}
				var warnings []DiagnosticWarning
				if m.IDEDriver != nil {
					if ideErr := m.IDEDriver.SyncProject(proj, symlinkPath); ideErr != nil {
						warnings = append(warnings, DiagnosticWarning{
							Subsystem: "IDE Sync",
							Message:   ideErr.Error(),
						})
					}
				}
				if sha, revErr := m.Git.RevParse(slot.Path, "HEAD"); revErr == nil {
					proj.LastKnownCommit = sha
				}
				if cid, _, cidErr := m.extractProjectChangeID(slot.Path, "HEAD"); cidErr == nil {
					proj.LastKnownChangeID = cid
				}

				result = &UseResult{
					Project:     proj.Name,
					Slot:        slot.Name,
					SlotPath:    slot.Path,
					SymlinkPath: symlinkPath,
					Branch:      proj.Branch,
					ChangeID:    proj.LastKnownChangeID,
					IssueID:     proj.IssueID,
					Mode:        mode,
					Warnings:    warnings,
				}
				return m.Store.Save(st)
			}
		}

		// Project is either PARKED or Brand New -> allocate a slot (or LRU swap)!
		res, err := m.allocateAndMountLocked(st, projectName, branchName, clRef, "", mode, agentID, now)
		if err != nil {
			return err
		}
		result = res
		return m.Store.Save(st)
	})

	return result, err
}

func (m *Manager) nextForkName(st *State, baseName string) string {
	for i := 1; i <= 100; i++ {
		candidate := fmt.Sprintf("%s-fork-%d", baseName, i)
		if _, exists := st.Projects[candidate]; !exists {
			return candidate
		}
	}
	return fmt.Sprintf("%s-fork-%d", baseName, m.Now().Unix())
}

// allocateAndMountLocked finds an AVAILABLE slot or LRU-swaps out a clean MOUNTED slot,
// checks out branchName, creates the POSIX symlink, and syncs IDE state.
func (m *Manager) allocateAndMountLocked(
	st *State,
	projectName string,
	branchName string,
	clRef string,
	sourceWorktreeForFork string,
	mode LeaseMode,
	agentID string,
	now time.Time,
) (*UseResult, error) {
	var targetSlot *Slot
	var swappedOutProject string

	// 1. Look for an AVAILABLE slot (ordered pw-01 .. pw-N)
	for i := 1; i <= st.SlotCount; i++ {
		sName := fmt.Sprintf("pw-%02d", i)
		s := st.Slots[sName]
		if s.Project == "" {
			targetSlot = s
			break
		}
	}

	// 2. If all N slots are occupied, run Automatic LRU Swap-Out!
	if targetSlot == nil {
		victimSlot, victimProj, err := m.selectVictimSlotForSwap(st, now)
		if err != nil {
			return nil, err
		}
		swappedOutProject = victimProj.Name
		if err := m.unmountToParkedLocked(st, victimProj, victimSlot, false); err != nil {
			return nil, fmt.Errorf("failed to swap out project %s from slot %s: %w", victimProj.Name, victimSlot.Name, err)
		}
		targetSlot = victimSlot
	}

	// 3. Ensure physical slot directory exists
	if err := os.MkdirAll(st.PoolRoot, 0755); err != nil {
		return nil, fmt.Errorf("failed to create pool directory %s: %w", st.PoolRoot, err)
	}
	if err := m.Git.WorktreeAddDetached(st.PrimaryRepo, targetSlot.Path); err != nil {
		return nil, fmt.Errorf("failed to initialize physical slot %s at %s: %w", targetSlot.Name, targetSlot.Path, err)
	}

	// 4. Checkout branch (or Gerrit CL ref) in physical slot
	proj, exists := st.Projects[projectName]
	if exists && proj.Branch != "" {
		branchName = proj.Branch
	}
	var resolvedChangeID string
	createFrom := "origin/main"
	if clRef != "" {
		fetchRef, cid, err := m.GerritStatus.ResolveCLFetchRef(context.Background(), clRef)
		if err != nil {
			return nil, fmt.Errorf("failed to resolve Gerrit CL %q: %w", clRef, err)
		}
		if err := m.Git.FetchRef(targetSlot.Path, "origin", fetchRef); err != nil {
			return nil, fmt.Errorf("failed to fetch Gerrit ref %s into slot %s: %w", fetchRef, targetSlot.Name, err)
		}
		createFrom = "FETCH_HEAD"
		resolvedChangeID = cid
	} else if sourceWorktreeForFork != "" {
		if sha, err := m.Git.RevParse(sourceWorktreeForFork, "HEAD"); err == nil && sha != "" {
			createFrom = sha
		}
	}
	if err := m.Git.SwitchBranch(targetSlot.Path, branchName, createFrom); err != nil {
		return nil, fmt.Errorf("failed to switch slot %s to branch %s: %w", targetSlot.Name, branchName, err)
	}

	// 5. Create/update POSIX symlink ~/wrk/projects/<projectName> -> targetSlot.Path
	if err := os.MkdirAll(st.ProjectsDir, 0755); err != nil {
		return nil, fmt.Errorf("failed to create projects directory %s: %w", st.ProjectsDir, err)
	}
	symlinkPath := filepath.Join(st.ProjectsDir, projectName)
	if err := m.ensureSymlink(targetSlot.Path, symlinkPath); err != nil {
		return nil, err
	}

	// 6. Update Project and Slot records
	if !exists {
		proj = &Project{
			Name:      projectName,
			CreatedAt: now,
		}
		st.Projects[projectName] = proj
	}
	proj.Residency = ResidencyMounted
	proj.Slot = targetSlot.Name
	proj.Branch = branchName
	proj.LastUsedAt = now
	if sha, err := m.Git.RevParse(targetSlot.Path, "HEAD"); err == nil {
		proj.LastKnownCommit = sha
	}
	if resolvedChangeID != "" {
		proj.LastKnownChangeID = resolvedChangeID
	} else if cid, _, cidErr := m.extractProjectChangeID(targetSlot.Path, "HEAD"); cidErr == nil {
		proj.LastKnownChangeID = cid
	}

	targetSlot.Project = projectName
	targetSlot.UpsertLease(agentID, mode, now)

	// 7. Sync IDE Project (unarchives & sets TURBO permissions in Antigravity/Jetski)
	var warnings []DiagnosticWarning
	if m.IDEDriver != nil {
		if ideErr := m.IDEDriver.SyncProject(proj, symlinkPath); ideErr != nil {
			warnings = append(warnings, DiagnosticWarning{
				Subsystem: "IDE Sync",
				Message:   ideErr.Error(),
			})
		}
	}

	return &UseResult{
		Project:     projectName,
		Slot:        targetSlot.Name,
		SlotPath:    targetSlot.Path,
		SymlinkPath: symlinkPath,
		Branch:      branchName,
		ChangeID:    proj.LastKnownChangeID,
		IssueID:     proj.IssueID,
		Mode:        mode,
		SwappedOut:  swappedOutProject,
		Warnings:    warnings,
	}, nil
}

// selectVictimSlotForSwap picks the best clean, unleased MOUNTED slot to transition to PARKED.
func (m *Manager) selectVictimSlotForSwap(st *State, now time.Time) (*Slot, *Project, error) {
	type candidate struct {
		slot *Slot
		proj *Project
	}
	var candidates []candidate

	for _, slot := range st.Slots {
		if slot.Project == "" {
			continue
		}
		// Rule 1: Never swap out a slot with an active write lease
		if slot.ActiveWriteLease(now) != nil {
			continue
		}
		// Rule 2: Never swap out a slot with uncommitted working tree edits (DIRTY) or status errors (FAIL-CLOSED)
		status, err := m.Git.StatusPorcelain(slot.Path)
		if err != nil || strings.TrimSpace(status) != "" {
			continue
		}
		proj, ok := st.Projects[slot.Project]
		if !ok {
			continue
		}
		candidates = append(candidates, candidate{slot: slot, proj: proj})
	}

	if len(candidates) == 0 {
		return nil, nil, fmt.Errorf(
			"cannot allocate worktree slot: all %d slots are currently busy (either DIRTY with uncommitted edits, actively leased by agents, or status check failed)\n"+
				"Precondition: gh wt never auto-evicts worktrees with uncommitted changes or active agent leases.\n"+
				"Remediation:\n"+
				"  1. Inspect active slots with `./gh wt list`\n"+
				"  2. Commit or stash changes in a dirty slot and run `./gh wt park <project>`\n"+
				"  3. Or expand your slot pool capacity with `./gh wt init --slots %d`",
			st.SlotCount, st.SlotCount+2,
		)
	}

	// Sort by LastUsedAt ascending (Least Recently Used first)
	sort.Slice(candidates, func(i, j int) bool {
		return candidates[i].proj.LastUsedAt.Before(candidates[j].proj.LastUsedAt)
	})

	return candidates[0].slot, candidates[0].proj, nil
}

// unmountToParkedLocked transitions a MOUNTED project to PARKED and frees its physical slot.
// If any git detach or symlink removal fails, state mutation is aborted so slots are never corrupted.
func (m *Manager) unmountToParkedLocked(st *State, proj *Project, slot *Slot, force bool) error {
	if sha, err := m.Git.RevParse(slot.Path, "HEAD"); err == nil && sha != "" {
		proj.LastKnownCommit = sha
	}
	if cid, _, cidErr := m.extractProjectChangeID(slot.Path, "HEAD"); cidErr == nil {
		proj.LastKnownChangeID = cid
	}

	// Switch physical slot to detached HEAD so local branch ref is not locked to this worktree
	var detachErr error
	if force {
		detachErr = m.Git.SwitchDetachForce(slot.Path, "origin/main")
	} else {
		detachErr = m.Git.SwitchDetach(slot.Path, "origin/main")
	}
	if detachErr != nil {
		return fmt.Errorf("failed to detach slot %s (%s): %w", slot.Name, slot.Path, detachErr)
	}

	// Remove symlink ~/wrk/projects/<projectName>
	symlinkPath := filepath.Join(st.ProjectsDir, proj.Name)
	if err := m.removeSymlink(symlinkPath); err != nil {
		return err
	}

	// Archive in Antigravity/Jetski IDE
	if m.IDEDriver != nil {
		_ = m.IDEDriver.ArchiveProject(proj)
	}

	proj.Residency = ResidencyParked
	proj.Slot = ""
	slot.Project = ""
	slot.Leases = nil
	return nil
}

// Park explicitly transitions a MOUNTED project to PARKED, freeing its physical slot.
func (m *Manager) Park(projectName string, force bool) error {
	projectName = strings.TrimSpace(projectName)
	return m.Store.WithLock(func() error {
		st, err := m.loadOrInitState(0)
		if err != nil {
			return err
		}
		proj, exists := st.Projects[projectName]
		if !exists {
			return fmt.Errorf(
				"project %q not found\nRemediation: Run `./gh wt list` to see active and parked projects",
				projectName,
			)
		}
		if proj.Residency == ResidencyParked || proj.Slot == "" {
			return nil // Already parked
		}
		slot, ok := st.Slots[proj.Slot]
		if !ok {
			proj.Residency = ResidencyParked
			proj.Slot = ""
			return m.Store.Save(st)
		}

		// Check DIRTY guard (fail-closed if StatusPorcelain returns an error)
		if !force {
			status, err := m.Git.StatusPorcelain(slot.Path)
			if err != nil {
				return fmt.Errorf("failed to inspect working tree status of slot %s (%s): %w", slot.Name, slot.Path, err)
			}
			if strings.TrimSpace(status) != "" {
				return fmt.Errorf(
					"refusing to park project %q: slot %s has uncommitted working tree changes\n"+
						"Precondition: Uncommitted edits would be detached when freeing the worktree slot.\n"+
						"Remediation:\n"+
						"  1. Commit your changes in %s (or push with `./gh pr push`), then re-run `./gh wt park %s`\n"+
						"  2. Or pass `--force` to discard uncommitted edits",
					projectName, slot.Name, filepath.Join(st.ProjectsDir, projectName), projectName,
				)
			}
		}

		if err := m.unmountToParkedLocked(st, proj, slot, force); err != nil {
			return err
		}
		return m.Store.Save(st)
	})
}

// Next rebases a MOUNTED project onto origin/main in-place to begin the next CL in the workstream.
func (m *Manager) Next(projectName string) error {
	projectName = strings.TrimSpace(projectName)
	return m.Store.WithLock(func() error {
		st, err := m.loadOrInitState(0)
		if err != nil {
			return err
		}
		if projectName == "" {
			// Attempt to resolve from current working directory against both ProjectsDir and Slots
			if cwd, cwdErr := os.Getwd(); cwdErr == nil {
				for _, s := range st.Slots {
					if s.Project != "" && (cwd == s.Path || strings.HasPrefix(cwd, s.Path+string(filepath.Separator))) {
						projectName = s.Project
						break
					}
				}
			}
		}
		proj, exists := st.Projects[projectName]
		if !exists {
			return fmt.Errorf("project %q not found\nRemediation: Run `./gh wt list` to view projects", projectName)
		}
		if proj.Residency != ResidencyMounted || proj.Slot == "" {
			return fmt.Errorf(
				"project %q is currently PARKED (not mounted in a slot)\nRemediation: Run `./gh wt use %s` first to mount it into a warm slot",
				projectName, projectName,
			)
		}
		slot := st.Slots[proj.Slot]
		status, err := m.Git.StatusPorcelain(slot.Path)
		if err != nil {
			return fmt.Errorf("failed to inspect working tree status in %s: %w", slot.Path, err)
		}
		if strings.TrimSpace(status) != "" {
			return fmt.Errorf(
				"cannot start next CL in project %q: working tree has uncommitted changes\nRemediation: Commit, stash, or discard changes in %s before running `./gh wt next`",
				projectName, filepath.Join(st.ProjectsDir, projectName),
			)
		}

		if err := m.Git.FetchOrigin(slot.Path); err != nil {
			return fmt.Errorf("failed to fetch origin in %s: %w", slot.Path, err)
		}
		if err := m.Git.RebaseOriginMain(slot.Path); err != nil {
			return fmt.Errorf("failed to rebase onto origin/main in %s: %w", slot.Path, err)
		}

		if sha, err := m.Git.RevParse(slot.Path, "HEAD"); err == nil {
			proj.LastKnownCommit = sha
		}
		proj.LastKnownChangeID = "" // Reset Change-Id for the new CL
		proj.LastUsedAt = m.Now()
		return m.Store.Save(st)
	})
}

// Close permanently removes a completed project from the active/parked catalog and frees its slot.
func (m *Manager) Close(projectName string, force bool) error {
	projectName = strings.TrimSpace(projectName)
	return m.Store.WithLock(func() error {
		st, err := m.loadOrInitState(0)
		if err != nil {
			return err
		}
		proj, exists := st.Projects[projectName]
		if !exists {
			return fmt.Errorf("project %q not found\nRemediation: Run `./gh wt list` to view projects", projectName)
		}

		if !force {
			repoPath := st.PrimaryRepo
			rev := proj.Branch
			if proj.Residency == ResidencyMounted && proj.Slot != "" {
				if slot, ok := st.Slots[proj.Slot]; ok {
					status, err := m.Git.StatusPorcelain(slot.Path)
					if err != nil {
						return fmt.Errorf("failed to inspect working tree status of slot %s (%s): %w", slot.Name, slot.Path, err)
					}
					if strings.TrimSpace(status) != "" {
						return fmt.Errorf(
							"refusing to close project %q: slot %s has uncommitted working tree changes\n"+
								"Remediation: Commit or discard changes in %s, or pass `--force`",
							projectName, slot.Name, filepath.Join(st.ProjectsDir, projectName),
						)
					}
					repoPath = slot.Path
					rev = "HEAD"
				}
			}
			// Verify unmerged commits guard (DL-07)
			if repoPath != "" && rev != "" {
				ahead, aheadErr := m.Git.CommitsAhead(repoPath, "origin/main", rev)
				if aheadErr != nil {
					return fmt.Errorf("failed to check unmerged commits for project %q: %w", projectName, aheadErr)
				}
				if ahead > 0 {
					isMerged := false
					if proj.LastKnownChangeID != "" {
						if gMap, gErr := m.GerritStatus.QueryChangesByID(context.Background(), []string{proj.LastKnownChangeID}); gErr == nil {
							if cs, ok := gMap[proj.LastKnownChangeID]; ok && cs.Status == "MERGED" {
								isMerged = true
							}
						}
					}
					if !isMerged {
						return fmt.Errorf(
							"refusing to close project %q: branch %q has %d commit(s) ahead of origin/main that are not merged in Gerrit\n"+
								"Precondition: Closing an unmerged project removes its tracking state and workspace.\n"+
								"Remediation:\n"+
								"  1. If you want to shelve this project for later, run `./gh wt park %s`\n"+
								"  2. If the CL is merged, run `git fetch origin` or `./gh wt next %s`\n"+
								"  3. Or pass `--force` to close anyway",
							projectName, proj.Branch, ahead, projectName, projectName,
						)
					}
				}
			}
		}

		if proj.Residency == ResidencyMounted && proj.Slot != "" {
			if slot, ok := st.Slots[proj.Slot]; ok {
				if err := m.unmountToParkedLocked(st, proj, slot, force); err != nil {
					return err
				}
			}
		} else {
			symlinkPath := filepath.Join(st.ProjectsDir, proj.Name)
			if err := m.removeSymlink(symlinkPath); err != nil {
				return err
			}
			if m.IDEDriver != nil {
				_ = m.IDEDriver.ArchiveProject(proj)
			}
		}

		delete(st.Projects, projectName)
		return m.Store.Save(st)
	})
}

// List builds the live DashboardReport combining local Git status and Gerrit review statuses.
func (m *Manager) List(ctx context.Context) (*DashboardReport, error) {
	var report DashboardReport
	err := m.Store.WithLock(func() error {
		st, err := m.loadOrInitState(0)
		if err != nil {
			return err
		}
		now := m.Now()
		report.TotalSlots = st.SlotCount

		// Collect Change-IDs across all MOUNTED and PARKED projects
		var changeIDs []string
		changeIDSet := make(map[string]bool)
		dirtyMap := make(map[string]bool)
		aheadMap := make(map[string]int)

		for _, proj := range st.Projects {
			if proj.Residency == ResidencyMounted && proj.Slot != "" {
				if slot, ok := st.Slots[proj.Slot]; ok {
					if status, err := m.Git.StatusPorcelain(slot.Path); err == nil && strings.TrimSpace(status) != "" {
						dirtyMap[proj.Name] = true
					}
					cid, ahead, cidErr := m.extractProjectChangeID(slot.Path, "HEAD")
					aheadMap[proj.Name] = ahead
					if cidErr == nil {
						proj.LastKnownChangeID = cid
					}
				}
			} else if proj.Residency == ResidencyParked && proj.Branch != "" && st.PrimaryRepo != "" {
				cid, ahead, cidErr := m.extractProjectChangeID(st.PrimaryRepo, proj.Branch)
				aheadMap[proj.Name] = ahead
				if cidErr == nil && cid != "" {
					proj.LastKnownChangeID = cid
				}
			}
			if proj.LastKnownChangeID != "" && !changeIDSet[proj.LastKnownChangeID] {
				changeIDSet[proj.LastKnownChangeID] = true
				changeIDs = append(changeIDs, proj.LastKnownChangeID)
			}
		}

		// Batch query Gerrit
		gerritMap, gerritErr := m.GerritStatus.QueryChangesByID(ctx, changeIDs)
		gerritOffline := gerritErr != nil
		if gerritOffline {
			report.Warnings = append(report.Warnings, DiagnosticWarning{
				Subsystem:   "Gerrit",
				Message:     fmt.Sprintf("Gerrit query failed (%v); displaying local status only", gerritErr),
				Remediation: "Check network/VPN connection or Gerrit authentication",
			})
		}

		// Classify projects
		for _, proj := range st.Projects {
			var cs *ChangeStatus
			if proj.LastKnownChangeID != "" && gerritMap != nil {
				if found, ok := gerritMap[proj.LastKnownChangeID]; ok {
					csCopy := found
					cs = &csCopy
				}
			}
			isDirty := dirtyMap[proj.Name]
			ahead := aheadMap[proj.Name]
			badge, details, action := ComputeStatusBadgeWithGerritState(isDirty, ahead, proj.LastKnownChangeID, cs, gerritOffline)

			issueID := proj.IssueID
			if issueID == 0 {
				if id, ok := pw_ghish.ExtractIssueIDFromBranchName(proj.Branch); ok {
					issueID = id
				}
			}
			if issueID > 0 {
				if details == "" || details == "-" {
					details = fmt.Sprintf("b/%d", issueID)
				} else {
					details = fmt.Sprintf("b/%d • %s", issueID, details)
				}
			}

			var activeLeases []string
			slotDisplay := "-"
			if proj.Residency == ResidencyMounted && proj.Slot != "" {
				slotDisplay = proj.Slot
				if slot, ok := st.Slots[proj.Slot]; ok {
					for _, l := range slot.Leases {
						if l.IsActive(now) {
							activeLeases = append(activeLeases, fmt.Sprintf("%s (%s)", l.AgentID, l.Mode))
						}
					}
				}
			} else if badge == BadgeNeedsAttention {
				action = fmt.Sprintf("Reviewer replied! Run `./gh wt use %s` to swap in", proj.Name)
			}

			entry := ProjectListEntry{
				Project:           proj.Name,
				Residency:         proj.Residency,
				Slot:              slotDisplay,
				Branch:            proj.Branch,
				IssueID:           issueID,
				StatusBadge:       badge,
				Details:           details,
				RecommendedAction: action,
				ActiveLeases:      activeLeases,
				IsDirty:           isDirty,
			}

			if proj.Residency == ResidencyMounted {
				report.MountedProjects = append(report.MountedProjects, entry)
				report.OccupiedSlots++
			} else {
				report.ParkedProjects = append(report.ParkedProjects, entry)
			}
		}

		report.AvailableSlots = report.TotalSlots - report.OccupiedSlots
		if report.AvailableSlots < 0 {
			report.AvailableSlots = 0
		}

		sort.Slice(report.MountedProjects, func(i, j int) bool {
			return report.MountedProjects[i].Slot < report.MountedProjects[j].Slot
		})
		sort.Slice(report.ParkedProjects, func(i, j int) bool {
			return report.ParkedProjects[i].Project < report.ParkedProjects[j].Project
		})

		return m.Store.Save(st)
	})

	return &report, err
}

// extractProjectChangeID returns the Change-Id and ahead-count of rev ONLY if rev has commits
// ahead of origin/main. If rev has 0 commits ahead of origin/main, any Change-Id on rev
// belongs to upstream origin/main and NOT to this project.
func (m *Manager) extractProjectChangeID(repoOrWorktreePath, rev string) (string, int, error) {
	ahead, err := m.Git.CommitsAhead(repoOrWorktreePath, "origin/main", rev)
	if err != nil {
		return "", 0, err
	}
	if ahead == 0 {
		return "", 0, nil
	}
	cid, err := m.Git.ExtractChangeID(repoOrWorktreePath, rev)
	if err != nil {
		return "", ahead, err
	}
	return cid, ahead, nil
}

// GarbageCollect sweeps orphaned Bazel output bases outside the managed pool.
func (m *Manager) GarbageCollect(dryRun bool) (GCReport, error) {
	var report GCReport
	err := m.Store.WithLock(func() error {
		st, err := m.loadOrInitState(0)
		if err != nil {
			return err
		}
		validPaths := map[string]bool{
			st.PrimaryRepo: true,
		}
		for _, slot := range st.Slots {
			validPaths[slot.Path] = true
		}
		if m.BuildDriver == nil {
			return nil
		}
		rep, gcErr := m.BuildDriver.GarbageCollect(dryRun, validPaths)
		report = rep
		return gcErr
	})
	return report, err
}

func (m *Manager) ensureSymlink(targetPath, symlinkPath string) error {
	fi, err := os.Lstat(symlinkPath)
	if err == nil {
		if fi.Mode()&os.ModeSymlink != 0 {
			if existingTarget, readErr := os.Readlink(symlinkPath); readErr == nil && existingTarget == targetPath {
				return nil
			}
		} else if fi.IsDir() {
			return fmt.Errorf("refusing to overwrite physical directory %s with a symlink", symlinkPath)
		}
		if rmErr := os.Remove(symlinkPath); rmErr != nil {
			return fmt.Errorf("failed to remove existing entry at %s: %w", symlinkPath, rmErr)
		}
	} else if !os.IsNotExist(err) {
		return fmt.Errorf("failed to inspect %s: %w", symlinkPath, err)
	}
	if err := os.Symlink(targetPath, symlinkPath); err != nil {
		return fmt.Errorf("failed to create symlink %s -> %s: %w", symlinkPath, targetPath, err)
	}
	return nil
}

func (m *Manager) removeSymlink(symlinkPath string) error {
	fi, err := os.Lstat(symlinkPath)
	if err != nil {
		if os.IsNotExist(err) {
			return nil
		}
		return fmt.Errorf("failed to inspect symlink %s: %w", symlinkPath, err)
	}
	if fi.Mode()&os.ModeSymlink == 0 && fi.IsDir() {
		return fmt.Errorf("refusing to remove physical directory %s when unmounting symlink", symlinkPath)
	}
	if err := os.Remove(symlinkPath); err != nil && !os.IsNotExist(err) {
		return fmt.Errorf("failed to remove symlink %s: %w", symlinkPath, err)
	}
	return nil
}
