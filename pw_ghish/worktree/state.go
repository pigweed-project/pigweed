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
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"

	"pigweed.dev/pw_ghish"
)

// Residency describes whether a project currently occupies a physical slot on disk
// or is shelved purely in Git branches + Gerrit.
type Residency string

const (
	ResidencyMounted Residency = "MOUNTED"
	ResidencyParked  Residency = "PARKED"
)

// LeaseMode indicates whether an agent/session is mutating or reading a slot.
type LeaseMode string

const (
	LeaseModeWrite LeaseMode = "write"
	LeaseModeRead  LeaseMode = "read"
)

// LeaseTimeout is the duration after which an un-refreshed agent lease is considered stale.
const LeaseTimeout = 30 * time.Minute

// Lease represents an advisory lock held by an agent conversation or user session.
type Lease struct {
	AgentID       string    `json:"agent_id"`
	Mode          LeaseMode `json:"mode"`
	AcquiredAt    time.Time `json:"acquired_at"`
	LastHeartbeat time.Time `json:"last_heartbeat"`
}

// IsActive reports whether the lease has not expired relative to now.
func (l Lease) IsActive(now time.Time) bool {
	return now.Sub(l.LastHeartbeat) <= LeaseTimeout
}

// Project tracks a persistent workstream managed by gh wt.
type Project struct {
	Name              string    `json:"name"`
	Residency         Residency `json:"residency"`
	Slot              string    `json:"slot"` // e.g., "pw-01" when MOUNTED, "" when PARKED
	Branch            string    `json:"branch"`
	IssueID           int64     `json:"issue_id,omitempty"`
	LastKnownChangeID string    `json:"last_known_change_id,omitempty"`
	LastKnownCommit   string    `json:"last_known_commit,omitempty"`
	JetskiProjectUUID string    `json:"jetski_project_uuid,omitempty"`
	CreatedAt         time.Time `json:"created_at"`
	LastUsedAt        time.Time `json:"last_used_at"`
}

// Slot represents one physical Git worktree directory in the fixed pool.
type Slot struct {
	Name    string  `json:"name"`    // e.g., "pw-01"
	Path    string  `json:"path"`    // e.g., "/usr/local/google/home/keir/wrk/slots/pw-01"
	Project string  `json:"project"` // Project name currently MOUNTED here, or "" if AVAILABLE
	Leases  []Lease `json:"leases,omitempty"`
}

// ActiveWriteLease returns the active write lease on this slot, if any.
func (s *Slot) ActiveWriteLease(now time.Time) *Lease {
	for i := range s.Leases {
		if s.Leases[i].Mode == LeaseModeWrite && s.Leases[i].IsActive(now) {
			return &s.Leases[i]
		}
	}
	return nil
}

// UpsertLease adds or refreshes a lease for agentID.
func (s *Slot) UpsertLease(agentID string, mode LeaseMode, now time.Time) {
	if agentID == "" {
		return
	}
	var active []Lease
	found := false
	for _, l := range s.Leases {
		if l.AgentID == agentID {
			l.Mode = mode
			l.LastHeartbeat = now
			active = append(active, l)
			found = true
		} else if l.IsActive(now) {
			active = append(active, l)
		}
	}
	if !found {
		active = append(active, Lease{
			AgentID:       agentID,
			Mode:          mode,
			AcquiredAt:    now,
			LastHeartbeat: now,
		})
	}
	s.Leases = active
}

// State represents the persistent JSON registry stored at ~/.config/ghish/worktrees.json.
type State struct {
	Version         int                 `json:"version"`
	PoolRoot        string              `json:"pool_root"`
	ProjectsDir     string              `json:"projects_dir"`
	PrimaryRepo     string              `json:"primary_repo"`
	SlotCount       int                 `json:"slot_count"`
	SlotPrefix      string              `json:"slot_prefix,omitempty"`
	WarmupDriver    string              `json:"warmup_driver,omitempty"`
	ShortlinkPrefix string              `json:"shortlink_prefix,omitempty"`
	Projects        map[string]*Project `json:"projects"`
	Slots           map[string]*Slot    `json:"slots"`
}

// EffectiveSlotPrefix returns the normalized slot prefix (without a trailing '-')
// used when naming physical worktree slots (defaulting to "pw").
func (st *State) EffectiveSlotPrefix() string {
	if st == nil {
		return "pw"
	}
	pfx := strings.TrimSuffix(strings.TrimSpace(st.SlotPrefix), "-")
	if pfx == "" {
		return "pw"
	}
	return pfx
}

// SlotName formats the physical slot directory name for a 1-based slot index (e.g., "pw-01").
func (st *State) SlotName(index int) string {
	return fmt.Sprintf("%s-%02d", st.EffectiveSlotPrefix(), index)
}

// NewEmptyState initializes an empty State struct with default maps.
func NewEmptyState(poolRoot, projectsDir, primaryRepo string, slotCount int) *State {
	return &State{
		Version:     1,
		PoolRoot:    poolRoot,
		ProjectsDir: projectsDir,
		PrimaryRepo: primaryRepo,
		SlotCount:   slotCount,
		Projects:    make(map[string]*Project),
		Slots:       make(map[string]*Slot),
	}
}

// StateStore manages atomic reads, writes, and file locking for worktrees.json.
type StateStore struct {
	StateFilePath string
	LockFilePath  string

	mu      sync.Mutex
	homeDir string // Set by DefaultStateStore so Save() can defer config-dir migration.
}

// NewStateStore creates a StateStore pointing to the given state JSON file.
func NewStateStore(stateFilePath string) *StateStore {
	return &StateStore{
		StateFilePath: stateFilePath,
		LockFilePath:  stateFilePath + ".lock",
	}
}

func resolveDefaultStateFilePath(home string, migrate bool) (string, error) {
	configDir := pw_ghish.ResolveUserConfigDir(home, migrate)
	if configDir == "" {
		return "", fmt.Errorf("failed to resolve user config directory for home %q", home)
	}
	statePath := filepath.Join(configDir, "worktrees.json")
	// On reads (migrate=false), fall back to an existing legacy worktrees.json
	// if canonical ghish/ already exists without one; on Save() (migrate=true),
	// always write to statePath so state migrates to canonical ghish/.
	// TODO: b/586589869 - Remove fallback when both ghish/ and legacy pw_ghish/ exist.
	if !migrate {
		if _, err := os.Stat(statePath); os.IsNotExist(err) {
			var legacyDirs []string
			if base := pw_ghish.BaseConfigDir(home); base != "" && filepath.IsAbs(base) {
				legacyDirs = append(legacyDirs, base)
			}
			if home != "" && filepath.IsAbs(home) {
				legacyDirs = append(legacyDirs, filepath.Join(home, ".config"))
			}
			for _, dir := range legacyDirs {
				// TODO: b/586589869 - Remove legacy pw_ghish state fallback after rollout.
				legacyState := filepath.Join(dir, "pw_ghish", "worktrees.json")
				if _, lerr := os.Stat(legacyState); lerr == nil {
					return legacyState, nil
				}
			}
		}
	}
	return statePath, nil
}

// DefaultStateStore resolves ${XDG_CONFIG_HOME:-~/.config}/ghish/worktrees.json
// without mutating the filesystem (deferred to Save so init() and read-only
// commands stay side-effect-free).
func DefaultStateStore() (*StateStore, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return nil, fmt.Errorf("failed to resolve user home directory: %w", err)
	}
	statePath, err := resolveDefaultStateFilePath(home, false)
	if err != nil {
		return nil, err
	}
	s := NewStateStore(statePath)
	s.homeDir = home
	return s, nil
}

// WithLock acquires an exclusive OS flock on worktrees.lock, executes fn, and releases the lock.
func (s *StateStore) WithLock(fn func() error) error {
	s.mu.Lock()
	lockPath := s.LockFilePath
	s.mu.Unlock()

	if err := os.MkdirAll(filepath.Dir(lockPath), 0755); err != nil {
		return fmt.Errorf("failed to create state directory %s: %w", filepath.Dir(lockPath), err)
	}
	lockFile, err := os.OpenFile(lockPath, os.O_CREATE|os.O_RDWR, 0644)
	if err != nil {
		return fmt.Errorf("failed to open lockfile %s: %w", lockPath, err)
	}
	defer lockFile.Close()

	if err := syscall.Flock(int(lockFile.Fd()), syscall.LOCK_EX); err != nil {
		return fmt.Errorf("failed to acquire exclusive flock on %s: %w", lockPath, err)
	}
	defer func() {
		_ = syscall.Flock(int(lockFile.Fd()), syscall.LOCK_UN)
	}()

	return fn()
}

// Load reads the State from disk, or returns nil, os.ErrNotExist if not initialized.
func (s *StateStore) Load() (*State, error) {
	s.mu.Lock()
	statePath := s.StateFilePath
	s.mu.Unlock()

	data, err := os.ReadFile(statePath)
	if err != nil {
		return nil, err
	}
	var st State
	if err := json.Unmarshal(data, &st); err != nil {
		return nil, fmt.Errorf("failed to parse state file %s: %w\nRemediation: Run `./gh wt init` to repair or recreate state", statePath, err)
	}
	if st.Projects == nil {
		st.Projects = make(map[string]*Project)
	}
	if st.Slots == nil {
		st.Slots = make(map[string]*Slot)
	}
	return &st, nil
}

// Save atomically writes State to disk using a temporary file and rename.
func (s *StateStore) Save(st *State) error {
	if st == nil {
		return fmt.Errorf("internal error: cannot save nil state")
	}
	s.mu.Lock()
	if s.homeDir != "" {
		if migratedPath, err := resolveDefaultStateFilePath(s.homeDir, true); err == nil && migratedPath != "" {
			s.StateFilePath = migratedPath
			s.LockFilePath = migratedPath + ".lock"
		}
	}
	statePath := s.StateFilePath
	s.mu.Unlock()

	return SafeModifyFile(statePath, 0644, func(_ []byte, _ bool) ([]byte, error) {
		return json.MarshalIndent(st, "", "  ")
	})
}
