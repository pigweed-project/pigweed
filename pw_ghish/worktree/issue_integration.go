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
	if branchName == "" {
		branchName = projectName
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
