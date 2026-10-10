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
	"strconv"
	"strings"

	"pigweed.dev/pw_ghish"
)

// UseWithIssue allocates or resumes a worktree slot linked to a Buganizer issue.
// If projectName is empty, derives a clean slug from issueID and issueTitle.
func (m *Manager) UseWithIssue(projectName, branchName string, issueID int64, issueTitle string, mode LeaseMode, agentID string) (*UseResult, error) {
	if m == nil {
		return nil, fmt.Errorf("worktree manager is not initialized")
	}
	if projectName == "" {
		projectName = pw_ghish.SlugifyBranchName(issueID, issueTitle)
	}

	res, err := m.Use(projectName, branchName, "", mode, agentID)
	if err != nil {
		return nil, err
	}

	if issueID > 0 {
		err = m.Store.WithLock(func() error {
			st, loadErr := m.Store.Load()
			if loadErr != nil {
				return loadErr
			}
			proj, ok := st.Projects[res.Project]
			if !ok {
				return nil
			}
			proj.IssueID = issueID
			if m.IDEDriver != nil {
				if ideErr := m.IDEDriver.SyncProject(proj, res.SymlinkPath); ideErr != nil {
					res.Warnings = append(res.Warnings, DiagnosticWarning{
						Subsystem: "IDE Sync",
						Message:   ideErr.Error(),
					})
				}
			}
			return m.Store.Save(st)
		})
		if err != nil {
			return nil, err
		}
		res.IssueID = issueID
	}

	return res, nil
}

// WorkspaceIntegrationAdapter implements pw_ghish.WorkspaceIntegration backed by Manager.
type WorkspaceIntegrationAdapter struct {
	ManagerFn func() (*Manager, error)
}

func (w *WorkspaceIntegrationAdapter) IsEnabled() bool {
	if w == nil || w.ManagerFn == nil {
		return false
	}
	mgr, err := w.ManagerFn()
	if err != nil || mgr == nil || mgr.Store == nil {
		return false
	}
	_, err = os.Stat(mgr.Store.StateFilePath)
	return err == nil
}

func (w *WorkspaceIntegrationAdapter) ResolveIssueIDForPath(cwd string) (int64, bool) {
	if !w.IsEnabled() || cwd == "" {
		return 0, false
	}
	mgr, err := w.ManagerFn()
	if err != nil || mgr == nil {
		return 0, false
	}
	st, err := mgr.Store.Load()
	if err != nil || st == nil {
		return 0, false
	}

	cleanCWD := filepath.Clean(cwd)
	realCWD, _ := filepath.EvalSymlinks(cleanCWD)

	for _, proj := range st.Projects {
		symlinkPath := filepath.Clean(filepath.Join(st.ProjectsDir, proj.Name))
		var slotPath string
		if proj.Slot != "" && st.Slots[proj.Slot] != nil {
			slotPath = filepath.Clean(st.Slots[proj.Slot].Path)
		}

		matches := cleanCWD == symlinkPath ||
			strings.HasPrefix(cleanCWD, symlinkPath+string(filepath.Separator)) ||
			(slotPath != "" && (cleanCWD == slotPath || strings.HasPrefix(cleanCWD, slotPath+string(filepath.Separator)))) ||
			(realCWD != "" && slotPath != "" && (realCWD == slotPath || strings.HasPrefix(realCWD, slotPath+string(filepath.Separator))))

		if matches {
			if proj.IssueID > 0 {
				return proj.IssueID, true
			}
			if id, ok := pw_ghish.ExtractIssueIDFromBranchName(proj.Branch); ok && id > 0 {
				return id, true
			}
		}
	}
	return 0, false
}

func (w *WorkspaceIntegrationAdapter) FindWorkspaceForIssue(issueID int64) (pw_ghish.WorkspaceIssueStatus, bool) {
	if !w.IsEnabled() || issueID <= 0 {
		return pw_ghish.WorkspaceIssueStatus{}, false
	}
	mgr, err := w.ManagerFn()
	if err != nil || mgr == nil {
		return pw_ghish.WorkspaceIssueStatus{}, false
	}
	st, err := mgr.Store.Load()
	if err != nil || st == nil {
		return pw_ghish.WorkspaceIssueStatus{}, false
	}

	for _, proj := range st.Projects {
		matchedID := proj.IssueID
		if matchedID == 0 {
			if id, ok := pw_ghish.ExtractIssueIDFromBranchName(proj.Branch); ok {
				matchedID = id
			}
		}
		if matchedID == issueID {
			return pw_ghish.WorkspaceIssueStatus{
				ProjectName: proj.Name,
				Residency:   string(proj.Residency),
				Slot:        proj.Slot,
				SymlinkPath: filepath.Join(st.ProjectsDir, proj.Name),
				Branch:      proj.Branch,
			}, true
		}
	}
	return pw_ghish.WorkspaceIssueStatus{}, false
}

func (w *WorkspaceIntegrationAdapter) DevelopIssueInWorktree(ctx context.Context, issueID int64, title string, customBranch string) (string, string, error) {
	if w == nil || w.ManagerFn == nil {
		return "", "", fmt.Errorf("worktree integration is not initialized")
	}
	mgr, err := w.ManagerFn()
	if err != nil {
		return "", "", err
	}
	res, err := mgr.UseWithIssue(customBranch, customBranch, issueID, title, LeaseModeWrite, os.Getenv("CONVERSATION_ID"))
	if err != nil {
		return "", "", err
	}
	return res.SymlinkPath, res.Slot, nil
}

func (w *WorkspaceIntegrationAdapter) SenseWorktrees(
	ctx context.Context,
	cwd string,
	targetIssueID int64,
	targetCLNumber int,
	targetChangeID string,
	includeFleet bool,
) (*pw_ghish.WorkspaceSenseResult, error) {
	res := &pw_ghish.WorkspaceSenseResult{
		ReadyToLand:    []pw_ghish.WorkspaceProjectMatch{},
		NeedsAttention: []pw_ghish.WorkspaceProjectMatch{},
		MergedProjects: []pw_ghish.WorkspaceProjectMatch{},
	}
	if !w.IsEnabled() {
		return res, nil
	}
	mgr, err := w.ManagerFn()
	if err != nil || mgr == nil {
		return res, err
	}
	st, err := mgr.Store.Load()
	if err != nil || st == nil {
		return res, err
	}

	res.Enabled = true
	res.PoolRoot = st.PoolRoot
	res.ProjectsDir = st.ProjectsDir
	res.PrimaryRepo = st.PrimaryRepo
	res.TotalSlots = st.SlotCount

	cleanCWD := ""
	realCWD := ""
	if cwd != "" {
		cleanCWD = filepath.Clean(cwd)
		realCWD, _ = filepath.EvalSymlinks(cleanCWD)
	}

	occupied := 0
	for _, s := range st.Slots {
		if s != nil && s.Project != "" {
			occupied++
		}
	}
	res.AvailableSlots = st.SlotCount - occupied
	if res.AvailableSlots < 0 {
		res.AvailableSlots = 0
	}

	for _, proj := range st.Projects {
		symlinkPath := filepath.Clean(filepath.Join(st.ProjectsDir, proj.Name))
		var slotPath string
		if proj.Slot != "" && st.Slots[proj.Slot] != nil {
			slotPath = filepath.Clean(st.Slots[proj.Slot].Path)
		}

		if cleanCWD != "" {
			matches := cleanCWD == symlinkPath ||
				strings.HasPrefix(cleanCWD, symlinkPath+string(filepath.Separator)) ||
				(slotPath != "" && (cleanCWD == slotPath || strings.HasPrefix(cleanCWD, slotPath+string(filepath.Separator)))) ||
				(realCWD != "" && slotPath != "" && (realCWD == slotPath || strings.HasPrefix(realCWD, slotPath+string(filepath.Separator))))
			if matches {
				res.CurrentProject = proj.Name
				res.CurrentSlot = proj.Slot
				res.CurrentSymlinkPath = symlinkPath
				res.CurrentIssueID = proj.IssueID
				if res.CurrentIssueID == 0 {
					if id, ok := pw_ghish.ExtractIssueIDFromBranchName(proj.Branch); ok {
						res.CurrentIssueID = id
					}
				}
			}
		}

		if res.TargetMatch == nil && matchProjectTarget(proj, targetIssueID, targetCLNumber, targetChangeID, "") {
			uuid := proj.JetskiProjectUUID
			if uuid == "" {
				uuid = DeterministicProjectUUID(proj.Name)
			}
			issueID := proj.IssueID
			if issueID == 0 {
				if id, ok := pw_ghish.ExtractIssueIDFromBranchName(proj.Branch); ok {
					issueID = id
				}
			}
			res.TargetMatch = &pw_ghish.WorkspaceProjectMatch{
				Project:     proj.Name,
				Residency:   string(proj.Residency),
				Slot:        proj.Slot,
				Branch:      proj.Branch,
				IssueID:     issueID,
				SymlinkPath: symlinkPath,
				ChangeID:    proj.LastKnownChangeID,
				ProjectUUID: uuid,
			}
		}
	}

	if includeFleet {
		if report, listErr := mgr.List(ctx); listErr == nil && report != nil {
			res.TotalSlots = report.TotalSlots
			res.AvailableSlots = report.AvailableSlots
			allEntries := append(append([]ProjectListEntry{}, report.MountedProjects...), report.ParkedProjects...)
			for _, entry := range allEntries {
				if entry.Project == res.CurrentProject {
					res.CurrentStatusBadge = string(entry.StatusBadge)
					res.CurrentDetails = entry.Details
					res.CurrentRecommendedAction = entry.RecommendedAction
				}
				symlinkPath := filepath.Clean(filepath.Join(st.ProjectsDir, entry.Project))
				uuid := DeterministicProjectUUID(entry.Project)
				if p := st.Projects[entry.Project]; p != nil && p.JetskiProjectUUID != "" {
					uuid = p.JetskiProjectUUID
				}
				pm := pw_ghish.WorkspaceProjectMatch{
					Project:     entry.Project,
					Residency:   string(entry.Residency),
					Slot:        entry.Slot,
					Branch:      entry.Branch,
					IssueID:     entry.IssueID,
					SymlinkPath: symlinkPath,
					StatusBadge: string(entry.StatusBadge),
					Details:     entry.Details,
					ProjectUUID: uuid,
				}
				if p := st.Projects[entry.Project]; p != nil {
					pm.ChangeID = p.LastKnownChangeID
					if res.TargetMatch == nil && matchProjectTarget(p, targetIssueID, targetCLNumber, targetChangeID, entry.Details) {
						pmCopy := pm
						res.TargetMatch = &pmCopy
					}
				}
				res.AllProjects = append(res.AllProjects, pm)
				switch entry.StatusBadge {
				case BadgeReadyToLand:
					res.ReadyToLand = append(res.ReadyToLand, pm)
				case BadgeNeedsAttention:
					res.NeedsAttention = append(res.NeedsAttention, pm)
				case BadgeCLMerged:
					res.MergedProjects = append(res.MergedProjects, pm)
				}
			}
		}
	}

	return res, nil
}

func matchProjectTarget(proj *Project, targetIssueID int64, targetCLNumber int, targetChangeID string, details string) bool {
	if proj == nil {
		return false
	}
	if targetIssueID > 0 {
		issueID := proj.IssueID
		if issueID == 0 {
			if id, ok := pw_ghish.ExtractIssueIDFromBranchName(proj.Branch); ok {
				issueID = id
			}
		}
		if issueID == targetIssueID {
			return true
		}
	}
	if targetChangeID != "" && proj.LastKnownChangeID != "" && strings.EqualFold(proj.LastKnownChangeID, targetChangeID) {
		return true
	}
	if targetCLNumber > 0 {
		clStr := strconv.Itoa(targetCLNumber)
		for _, candidate := range []string{proj.Name, proj.Branch, details} {
			if containsExactCLNumber(candidate, clStr) {
				return true
			}
		}
	}
	return false
}

// containsExactCLNumber reports whether candidate equals clStr or contains a
// CL reference such as "cl-<clStr>", "pwrev-<clStr>", "pwrev/<clStr>",
// "fxrev/<clStr>", or ends with "-<clStr>", where <clStr> is not immediately
// followed by another digit (avoiding false positives like "cl-123" matching "cl-12345").
func containsExactCLNumber(candidate, clStr string) bool {
	if candidate == "" || clStr == "" {
		return false
	}
	if candidate == clStr {
		return true
	}
	if strings.HasSuffix(candidate, "-"+clStr) {
		prefixLen := len(candidate) - len(clStr) - 1
		if prefixLen == 0 || (candidate[prefixLen-1] < '0' || candidate[prefixLen-1] > '9') {
			return true
		}
	}
	for _, pfx := range []string{"cl-", "pwrev-", "pwrev/", "fxrev-", "fxrev/"} {
		token := pfx + clStr
		searchFrom := 0
		for searchFrom < len(candidate) {
			idx := strings.Index(candidate[searchFrom:], token)
			if idx < 0 {
				break
			}
			absIdx := searchFrom + idx
			endIdx := absIdx + len(token)
			// Ensure preceding character (if any) is not alphanumeric
			prevOK := absIdx == 0 || !isASCIIAlphaNum(candidate[absIdx-1])
			// Ensure following character (if any) is not a digit
			nextOK := endIdx == len(candidate) || (candidate[endIdx] < '0' || candidate[endIdx] > '9')
			if prevOK && nextOK {
				return true
			}
			searchFrom = absIdx + len(pfx)
		}
	}
	return false
}

func isASCIIAlphaNum(b byte) bool {
	return (b >= '0' && b <= '9') || (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z')
}
