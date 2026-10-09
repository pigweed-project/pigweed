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

	// Prefer the current working directory's repository if it contains a .ghish.toml.
	if cwd, cwdErr := os.Getwd(); cwdErr == nil && pw_ghish.HasProjectConfigFileOnDisk(cwd) {
		topLevel := pw_ghish.FindGitTopLevelOnDisk(cwd)
		if _, gitErr := os.Stat(filepath.Join(topLevel, ".git")); gitErr == nil {
			return poolRoot, projectsDir, topLevel, nil
		}
	}

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

// applyRepoProjectConfig populates SlotPrefix, WarmupDriver, and ShortlinkPrefix on State
// from the primary repository's .ghish.toml / git config if not already set, and syncs
// ProjectPrefix onto JetskiIDEDriver.
func (m *Manager) applyRepoProjectConfig(st *State) {
	if st == nil || st.PrimaryRepo == "" {
		return
	}
	if pcfg, err := pw_ghish.LoadProjectConfig(context.Background(), nil, st.PrimaryRepo); err == nil && pcfg != nil {
		if st.SlotPrefix == "" && pcfg.Worktree.SlotPrefix != "" {
			st.SlotPrefix = pcfg.Worktree.SlotPrefix
		}
		if st.WarmupDriver == "" && pcfg.Worktree.WarmupDriver != "" {
			st.WarmupDriver = pcfg.Worktree.WarmupDriver
		}
		if st.ShortlinkPrefix == "" && len(pcfg.Gerrit.Shortlinks) > 0 {
			var keys []string
			for k := range pcfg.Gerrit.Shortlinks {
				keys = append(keys, k)
			}
			sort.Strings(keys)
			st.ShortlinkPrefix = keys[0]
		}
	}
	if ide, ok := m.IDEDriver.(*JetskiIDEDriver); ok && ide != nil && ide.ProjectPrefix == "" && st.SlotPrefix != "" {
		ide.ProjectPrefix = st.EffectiveSlotPrefix()
	}
}

func (m *Manager) effectiveBuildDriver(st *State) BuildEnvDriver {
	if st != nil && (st.WarmupDriver == "none" || st.WarmupDriver == "gn") {
		if m.BuildDriver == nil {
			return NoopBuildDriver{Mode: st.WarmupDriver}
		}
		if _, isBazel := m.BuildDriver.(*BazelDriver); isBazel {
			return NoopBuildDriver{Mode: st.WarmupDriver}
		}
	}
	return m.BuildDriver
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
		m.applyRepoProjectConfig(st)
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
	st = NewEmptyState(poolRoot, projectsDir, primaryRepo, defaultSlots)
	m.applyRepoProjectConfig(st)
	return st, nil
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
			slotName := st.SlotName(i)
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

		// 5b. Inspect unmanaged Git worktrees holding branches
		if unmanagedWTs := m.findUnmanagedBranchWorktreesLocked(st); len(unmanagedWTs) > 0 {
			var unmanagedDesc []string
			for _, uwt := range unmanagedWTs {
				unmanagedDesc = append(unmanagedDesc, fmt.Sprintf("%s [%s]", filepath.Base(uwt.Path), uwt.Branch))
			}
			items = append(items, ChecklistItem{
				Category: "Unmanaged Git Worktrees",
				Status:   ChecklistOK,
				Summary:  fmt.Sprintf("%d unmanaged worktree(s) hold branches (%s); gh wt use auto-suffixes on collision", len(unmanagedWTs), strings.Join(unmanagedDesc, ", ")),
			})
		}

		// 6. IDE Integration Health Check
		if m.IDEDriver != nil {
			ideItem, err := m.IDEDriver.CheckHealth()
			if err == nil {
				items = append(items, ideItem)
			}
		}

		// 7. Build Environment & Bazel Cache Driver Check
		if buildDriver := m.effectiveBuildDriver(st); buildDriver != nil {
			buildItems, err := buildDriver.CheckAndConfigure(!checkOnly, validPaths)
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
	explicitBranch := strings.TrimSpace(branchName) != ""
	branchName = strings.TrimSpace(branchName)
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
			sName := st.SlotName(i)
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
					// AUTOMATIC WARM FORK! Pass forkName as the new branch name so it never
					// collides with proj.Branch already checked out in slot.Path.
					forkName := m.nextForkName(st, projectName)
					forkRes, forkErr := m.allocateAndMountLocked(st, forkName, forkName, false, clRef, slot.Path, mode, agentID, now)
					if forkErr != nil {
						return forkErr
					}
					forkRes.ForkedFrom = projectName
					result = forkRes
					return m.Store.Save(st)
				}

				// Ensure physical slot directory still exists on disk
				if err := os.MkdirAll(st.PoolRoot, 0755); err != nil {
					return fmt.Errorf("failed to create pool directory %s: %w", st.PoolRoot, err)
				}
				if err := m.Git.WorktreeAddDetached(st.PrimaryRepo, slot.Path); err != nil {
					return fmt.Errorf("failed to verify physical slot %s at %s: %w", slot.Name, slot.Path, err)
				}

				var warnings []DiagnosticWarning
				var resolvedChangeID string

				// If caller explicitly requested --cl or a different --branch on an already-mounted project,
				// or if the slot is detached/on a different branch than proj.Branch, converge the branch safely.
				currBranch, currErr := m.Git.CurrentBranch(slot.Path)
				needsBranchSwitch := clRef != "" || (explicitBranch && branchName != proj.Branch) || (currErr == nil && proj.Branch != "" && currBranch != proj.Branch)
				if needsBranchSwitch {
					status, statusErr := m.Git.StatusPorcelain(slot.Path)
					if statusErr != nil {
						return fmt.Errorf("failed to inspect working tree status of slot %s (%s): %w", slot.Name, slot.Path, statusErr)
					}
					if strings.TrimSpace(status) != "" {
						if clRef != "" || (explicitBranch && branchName != proj.Branch) {
							return fmt.Errorf(
								"refusing to switch branch/CL in mounted project %q: slot %s has uncommitted working tree changes\n"+
									"Precondition: Switching branches or checking out a CL requires a clean working tree.\n"+
									"Remediation: Commit, stash, or discard changes in %s first",
								proj.Name, slot.Name, filepath.Join(st.ProjectsDir, proj.Name),
							)
						}
					} else {
						desired := proj.Branch
						if explicitBranch || desired == "" {
							desired = branchName
						}
						createFrom := "origin/main"
						forceReset := false
						if clRef != "" {
							fetchRef, cid, err := m.GerritStatus.ResolveCLFetchRef(context.Background(), clRef)
							if err != nil {
								return fmt.Errorf("failed to resolve Gerrit CL %q: %w", clRef, err)
							}
							if err := m.Git.FetchRef(slot.Path, "origin", fetchRef); err != nil {
								return fmt.Errorf("failed to fetch Gerrit ref %s into slot %s: %w", fetchRef, slot.Name, err)
							}
							createFrom = "FETCH_HEAD"
							resolvedChangeID = cid
							forceReset = true
						}
						actualBranch, branchWarnings, switchErr := m.checkoutSlotBranchLocked(
							st,
							slot,
							proj.Name,
							desired,
							true,
							explicitBranch,
							createFrom,
							forceReset,
						)
						if switchErr != nil {
							return fmt.Errorf("failed to switch slot %s to branch %s: %w", slot.Name, desired, switchErr)
						}
						proj.Branch = actualBranch
						warnings = append(warnings, branchWarnings...)
					}
				}

				// Update lease, verify symlink & IDE sync
				slot.UpsertLease(agentID, mode, now)
				proj.LastUsedAt = now
				symlinkPath := filepath.Join(st.ProjectsDir, proj.Name)
				if err := m.ensureSymlink(slot.Path, symlinkPath); err != nil {
					return err
				}
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
				if resolvedChangeID != "" {
					proj.LastKnownChangeID = resolvedChangeID
				} else if cid, _, cidErr := m.extractProjectChangeID(slot.Path, "HEAD"); cidErr == nil {
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
		res, err := m.allocateAndMountLocked(st, projectName, branchName, explicitBranch, clRef, "", mode, agentID, now)
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
// checks out branchName (handling any branch collisions with unmanaged/other worktrees),
// creates the POSIX symlink, and syncs IDE state.
func (m *Manager) allocateAndMountLocked(
	st *State,
	projectName string,
	branchName string,
	explicitBranch bool,
	clRef string,
	sourceWorktreeForFork string,
	mode LeaseMode,
	agentID string,
	now time.Time,
) (*UseResult, error) {
	var targetSlot *Slot
	var swappedOutProject string

	// 1. Look for an AVAILABLE slot (ordered <prefix>-01 .. <prefix>-N)
	for i := 1; i <= st.SlotCount; i++ {
		sName := st.SlotName(i)
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

	// 4. Checkout branch (or Gerrit CL ref) in physical slot with collision & stale-branch protection
	proj, exists := st.Projects[projectName]
	desiredBranch := branchName
	if exists && !explicitBranch && proj.Branch != "" {
		desiredBranch = proj.Branch
	}
	var resolvedChangeID string
	createFrom := "origin/main"
	forceResetToCreateFrom := !exists && !explicitBranch
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
		forceResetToCreateFrom = true
	} else if sourceWorktreeForFork != "" {
		if sha, err := m.Git.RevParse(sourceWorktreeForFork, "HEAD"); err == nil && sha != "" {
			createFrom = sha
		}
		forceResetToCreateFrom = true
	}

	actualBranch, warnings, err := m.checkoutSlotBranchLocked(
		st,
		targetSlot,
		projectName,
		desiredBranch,
		exists,
		explicitBranch,
		createFrom,
		forceResetToCreateFrom,
	)
	if err != nil {
		return nil, fmt.Errorf("failed to switch slot %s to branch %s: %w", targetSlot.Name, desiredBranch, err)
	}
	branchName = actualBranch

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

// sameFilesystemPath compares two paths after cleaning and resolving symlinks when possible.
func sameFilesystemPath(a, b string) bool {
	if a == "" || b == "" {
		return false
	}
	cleanA := filepath.Clean(a)
	cleanB := filepath.Clean(b)
	if cleanA == cleanB {
		return true
	}
	realA, errA := filepath.EvalSymlinks(cleanA)
	realB, errB := filepath.EvalSymlinks(cleanB)
	if errA == nil && errB == nil && realA == realB {
		return true
	}
	return false
}

// pathIsWithin reports whether childPath is equal to or a subdirectory of parentPath.
func pathIsWithin(childPath, parentPath string) bool {
	if childPath == "" || parentPath == "" {
		return false
	}
	cleanChild := filepath.Clean(childPath)
	cleanParent := filepath.Clean(parentPath)
	if cleanChild == cleanParent || strings.HasPrefix(cleanChild, cleanParent+string(filepath.Separator)) {
		return true
	}
	realChild, errC := filepath.EvalSymlinks(cleanChild)
	realParent, errP := filepath.EvalSymlinks(cleanParent)
	if errC == nil && errP == nil {
		if realChild == realParent || strings.HasPrefix(realChild, realParent+string(filepath.Separator)) {
			return true
		}
	}
	return false
}

// findUnmanagedBranchWorktreesLocked returns all Git worktrees outside the gh wt slot pool
// that currently have a branch checked out (non-detached).
func (m *Manager) findUnmanagedBranchWorktreesLocked(st *State) []WorktreeInfo {
	worktrees, err := m.Git.ListWorktrees(st.PrimaryRepo)
	if err != nil || len(worktrees) == 0 {
		return nil
	}
	var unmanaged []WorktreeInfo
	for _, wt := range worktrees {
		if wt.Detached || wt.Branch == "" {
			continue
		}
		isSlot := false
		for _, s := range st.Slots {
			if sameFilesystemPath(wt.Path, s.Path) {
				isSlot = true
				break
			}
		}
		if !isSlot && pathIsWithin(wt.Path, st.PoolRoot) {
			isSlot = true
		}
		if !isSlot {
			unmanaged = append(unmanaged, wt)
		}
	}
	return unmanaged
}

// branchesInUseByOtherWorktreesLocked returns a map of branchName -> owner description for all
// branches checked out in any worktree other than targetSlot, or owned by another project in State.
func (m *Manager) branchesInUseByOtherWorktreesLocked(st *State, targetSlot *Slot, projectName string) map[string]string {
	inUse := make(map[string]string)
	worktrees, err := m.Git.ListWorktrees(targetSlot.Path)
	if err != nil || len(worktrees) == 0 {
		worktrees, _ = m.Git.ListWorktrees(st.PrimaryRepo)
	}
	for _, wt := range worktrees {
		if wt.Detached || wt.Branch == "" {
			continue
		}
		if sameFilesystemPath(wt.Path, targetSlot.Path) {
			continue
		}
		inUse[wt.Branch] = wt.Path
	}
	for _, otherProj := range st.Projects {
		if otherProj.Name == projectName || otherProj.Branch == "" {
			continue
		}
		if _, exists := inUse[otherProj.Branch]; !exists {
			if otherProj.Residency == ResidencyMounted && otherProj.Slot != "" && otherProj.Slot != targetSlot.Name {
				inUse[otherProj.Branch] = fmt.Sprintf("slot %s (project %s)", otherProj.Slot, otherProj.Name)
			} else if otherProj.Residency == ResidencyParked {
				inUse[otherProj.Branch] = fmt.Sprintf("parked project %s", otherProj.Name)
			}
		}
	}
	return inUse
}

// nextAvailableBranchNameLocked finds a collision-free branch name (`<base>-wt`, `<base>-wt-2`, ...)
// that is not checked out in any other worktree, not owned by another project, and has no unmerged commits.
func (m *Manager) nextAvailableBranchNameLocked(
	st *State,
	worktreePath string,
	projectName string,
	baseBranch string,
	checkedOutByOther map[string]string,
) string {
	for i := 1; i <= 100; i++ {
		candidate := fmt.Sprintf("%s-wt", baseBranch)
		if i > 1 {
			candidate = fmt.Sprintf("%s-wt-%d", baseBranch, i)
		}
		if checkedOutByOther[candidate] != "" {
			continue
		}
		exists, err := m.Git.BranchExists(worktreePath, candidate)
		if err == nil && exists {
			// Only reuse an existing -wt branch if it has 0 unmerged commits ahead of origin/main
			ahead, aheadErr := m.Git.CommitsAhead(worktreePath, "origin/main", "refs/heads/"+candidate)
			if aheadErr != nil || ahead > 0 {
				continue
			}
		}
		return candidate
	}
	return fmt.Sprintf("%s-wt-%d", baseBranch, m.Now().Unix())
}

// checkoutSlotBranchLocked safely checks out desiredBranch in targetSlot, automatically resolving:
//  1. Collisions with branches already checked out in unmanaged worktrees (e.g. ~/wrk/pw-*) or other slots
//  2. Stale local branches from previously closed projects (resetting if 0 commits ahead of origin/main,
//     or suffixing to <branch>-wt if the existing branch has unmerged commits).
func (m *Manager) checkoutSlotBranchLocked(
	st *State,
	targetSlot *Slot,
	projectName string,
	desiredBranch string,
	existsInState bool,
	explicitBranch bool,
	createFrom string,
	forceResetToCreateFrom bool,
) (string, []DiagnosticWarning, error) {
	var warnings []DiagnosticWarning
	checkedOutByOther := m.branchesInUseByOtherWorktreesLocked(st, targetSlot, projectName)

	switchOrResetCandidate := func(branch, baseRef string) error {
		existsInGit, err := m.Git.BranchExists(targetSlot.Path, branch)
		if err != nil {
			return err
		}
		if existsInGit {
			return m.Git.ResetBranch(targetSlot.Path, branch, baseRef)
		}
		return m.Git.SwitchBranch(targetSlot.Path, branch, baseRef)
	}

	// Case 1: desiredBranch is currently checked out in another worktree (e.g., ~/wrk/pw-ghish) or owned by another project.
	if ownerDesc, inUse := checkedOutByOther[desiredBranch]; inUse {
		actualBranch := m.nextAvailableBranchNameLocked(st, targetSlot.Path, projectName, desiredBranch, checkedOutByOther)
		baseRef := createFrom
		if (existsInState || explicitBranch) && !forceResetToCreateFrom {
			if existsInGit, err := m.Git.BranchExists(targetSlot.Path, desiredBranch); err == nil && existsInGit {
				baseRef = "refs/heads/" + desiredBranch
			}
		}
		if err := switchOrResetCandidate(actualBranch, baseRef); err != nil {
			return "", nil, err
		}
		warnings = append(warnings, DiagnosticWarning{
			Subsystem:   "Git Branch Collision",
			Message:     fmt.Sprintf("Branch %q is already in use by %s; mounted on branch %q (from %s) instead", desiredBranch, ownerDesc, actualBranch, baseRef),
			Remediation: fmt.Sprintf("To use branch name %q directly in the future, detach %s (`git -C %s switch --detach`)", desiredBranch, ownerDesc, ownerDesc),
		})
		return actualBranch, warnings, nil
	}

	// Case 2: desiredBranch is not checked out in any other worktree.
	existsInGit, err := m.Git.BranchExists(targetSlot.Path, desiredBranch)
	if err != nil {
		return "", nil, err
	}
	if !existsInGit {
		if switchErr := m.Git.SwitchBranch(targetSlot.Path, desiredBranch, createFrom); switchErr != nil {
			// Defense-in-depth fallback if git reports a worktree collision race
			if strings.Contains(switchErr.Error(), "already used by worktree") {
				checkedOutByOther[desiredBranch] = "another worktree"
				actualBranch := m.nextAvailableBranchNameLocked(st, targetSlot.Path, projectName, desiredBranch, checkedOutByOther)
				if retryErr := switchOrResetCandidate(actualBranch, createFrom); retryErr == nil {
					warnings = append(warnings, DiagnosticWarning{
						Subsystem: "Git Branch Collision",
						Message:   fmt.Sprintf("Branch %q is locked by another worktree; mounted on branch %q instead", desiredBranch, actualBranch),
					})
					return actualBranch, warnings, nil
				}
			}
			return "", nil, switchErr
		}
		return desiredBranch, warnings, nil
	}

	// desiredBranch already exists in local Git (`refs/heads/<desiredBranch>`).
	if !forceResetToCreateFrom {
		// Resuming a PARKED project or checking out an explicit --branch without --cl: preserve existing commits on the branch.
		if switchErr := m.Git.SwitchBranch(targetSlot.Path, desiredBranch, ""); switchErr != nil {
			if strings.Contains(switchErr.Error(), "already used by worktree") {
				checkedOutByOther[desiredBranch] = "another worktree"
				actualBranch := m.nextAvailableBranchNameLocked(st, targetSlot.Path, projectName, desiredBranch, checkedOutByOther)
				if retryErr := switchOrResetCandidate(actualBranch, "refs/heads/"+desiredBranch); retryErr == nil {
					warnings = append(warnings, DiagnosticWarning{
						Subsystem: "Git Branch Collision",
						Message:   fmt.Sprintf("Branch %q is locked by another worktree; mounted on branch %q (from %s) instead", desiredBranch, actualBranch, desiredBranch),
					})
					return actualBranch, warnings, nil
				}
			}
			return "", nil, switchErr
		}
		return desiredBranch, warnings, nil
	}

	// Brand-new project (not in State) or --cl / warm-fork: check if existing local branch has unmerged commits.
	ahead, aheadErr := m.Git.CommitsAhead(targetSlot.Path, "origin/main", "refs/heads/"+desiredBranch)
	if aheadErr == nil && ahead == 0 {
		// 0 unmerged commits ahead of origin/main -> safe to reset the stale ref to createFrom (origin/main or FETCH_HEAD)!
		if resetErr := m.Git.ResetBranch(targetSlot.Path, desiredBranch, createFrom); resetErr != nil {
			if strings.Contains(resetErr.Error(), "already used by worktree") {
				checkedOutByOther[desiredBranch] = "another worktree"
				actualBranch := m.nextAvailableBranchNameLocked(st, targetSlot.Path, projectName, desiredBranch, checkedOutByOther)
				if retryErr := switchOrResetCandidate(actualBranch, createFrom); retryErr == nil {
					warnings = append(warnings, DiagnosticWarning{
						Subsystem: "Git Branch Collision",
						Message:   fmt.Sprintf("Branch %q is locked by another worktree; mounted on branch %q (from %s) instead", desiredBranch, actualBranch, createFrom),
					})
					return actualBranch, warnings, nil
				}
			}
			return "", nil, resetErr
		}
		return desiredBranch, warnings, nil
	}

	// Existing local branch has unmerged commits ahead of origin/main!
	// Never clobber unmerged work: allocate <desiredBranch>-wt at createFrom and warn.
	checkedOutByOther[desiredBranch] = "existing local branch with unmerged commits"
	actualBranch := m.nextAvailableBranchNameLocked(st, targetSlot.Path, projectName, desiredBranch, checkedOutByOther)
	if err := switchOrResetCandidate(actualBranch, createFrom); err != nil {
		return "", nil, err
	}
	warnings = append(warnings, DiagnosticWarning{
		Subsystem:   "Git Branch Collision",
		Message:     fmt.Sprintf("Local branch %q already has %d unmerged commit(s) ahead of origin/main; created fresh branch %q (from %s) to avoid overwriting unmerged work", desiredBranch, ahead, actualBranch, createFrom),
		Remediation: fmt.Sprintf("Pass `--branch %s` (`./gh wt use %s --branch %s`) if you want to check out the existing branch with its %d commit(s)", desiredBranch, projectName, desiredBranch, ahead),
	})
	return actualBranch, warnings, nil
}

// resolveProjectLocked resolves a project by explicit name, slot name (e.g. "pw-02"), branch name,
// or (when nameOrEmpty is "") from the caller's working directory ($BUILD_WORKING_DIRECTORY, $PWD, or os.Getwd()).
func (m *Manager) resolveProjectLocked(st *State, nameOrEmpty string) (*Project, error) {
	nameOrEmpty = strings.TrimSpace(nameOrEmpty)
	if nameOrEmpty != "" {
		if proj, ok := st.Projects[nameOrEmpty]; ok {
			return proj, nil
		}
		if slot, ok := st.Slots[nameOrEmpty]; ok && slot.Project != "" {
			if proj, ok := st.Projects[slot.Project]; ok {
				return proj, nil
			}
		}
		for _, proj := range st.Projects {
			if proj.Branch == nameOrEmpty {
				return proj, nil
			}
		}
		return nil, fmt.Errorf(
			"project %q not found\nRemediation: Run `./gh wt list` to see active and parked projects",
			nameOrEmpty,
		)
	}

	var candidateDirs []string
	if bwd := strings.TrimSpace(os.Getenv("BUILD_WORKING_DIRECTORY")); bwd != "" {
		candidateDirs = append(candidateDirs, bwd)
	}
	if pwd := strings.TrimSpace(os.Getenv("PWD")); pwd != "" {
		candidateDirs = append(candidateDirs, pwd)
	}
	if cwd, err := os.Getwd(); err == nil && cwd != "" {
		candidateDirs = append(candidateDirs, cwd)
	}

	for _, dir := range candidateDirs {
		for _, proj := range st.Projects {
			symPath := filepath.Join(st.ProjectsDir, proj.Name)
			if pathIsWithin(dir, symPath) {
				return proj, nil
			}
		}
		for _, s := range st.Slots {
			if s.Project != "" && pathIsWithin(dir, s.Path) {
				if proj, ok := st.Projects[s.Project]; ok {
					return proj, nil
				}
			}
		}
	}

	displayDir := ""
	if len(candidateDirs) > 0 {
		displayDir = candidateDirs[0]
	}
	return nil, fmt.Errorf(
		"could not infer active gh wt project from current directory %q\n"+
			"Precondition: Running without a <project> argument requires being inside %s/<project> or %s/pw-XX (not an unmanaged worktree).\n"+
			"Remediation:\n"+
			"  1. Pass the project name explicitly, e.g., `./gh wt next <project>` or `./gh wt park <project>`\n"+
			"  2. Or run `./gh wt list` to see mounted projects",
		displayDir, st.ProjectsDir, st.PoolRoot,
	)
}

// ResolveProjectName resolves a project identifier (name, slot, branch, or empty for CWD) to its canonical project name.
func (m *Manager) ResolveProjectName(nameOrEmpty string) (string, error) {
	st, err := m.loadOrInitState(0)
	if err != nil {
		return "", err
	}
	proj, err := m.resolveProjectLocked(st, nameOrEmpty)
	if err != nil {
		return "", err
	}
	return proj.Name, nil
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
		proj, err := m.resolveProjectLocked(st, projectName)
		if err != nil {
			return err
		}
		projectName = proj.Name
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
		proj, err := m.resolveProjectLocked(st, projectName)
		if err != nil {
			return err
		}
		projectName = proj.Name
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
		proj, err := m.resolveProjectLocked(st, projectName)
		if err != nil {
			return err
		}
		projectName = proj.Name

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
			badge, details, action := ComputeStatusBadgeWithShortlink(isDirty, ahead, proj.LastKnownChangeID, cs, gerritOffline, st.ShortlinkPrefix)

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
		buildDriver := m.effectiveBuildDriver(st)
		if buildDriver == nil {
			return nil
		}
		rep, gcErr := buildDriver.GarbageCollect(dryRun, validPaths)
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
