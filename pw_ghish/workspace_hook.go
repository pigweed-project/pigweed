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

package pw_ghish

import (
	"context"
	"regexp"
	"strconv"
	"strings"
)

// WorkspaceIssueStatus describes where an issue lives in the local worktree manager.
type WorkspaceIssueStatus struct {
	ProjectName string `json:"project_name"`
	Residency   string `json:"residency"` // "MOUNTED" or "PARKED"
	Slot        string `json:"slot,omitempty"`
	SymlinkPath string `json:"symlink_path,omitempty"`
	Branch      string `json:"branch"`
}

// WorkspaceIntegration is an optional hook implemented by pw_ghish/worktree.
// If nil or disabled, all gh issue/pr commands use standard single-repo Git behavior.
type WorkspaceIntegration interface {
	// IsEnabled returns true only if the user has initialized gh wt on this machine.
	IsEnabled() bool
	// ResolveIssueIDForPath returns the linked Buganizer Issue ID for a working directory.
	ResolveIssueIDForPath(cwd string) (int64, bool)
	// FindWorkspaceForIssue returns local workspace residency metadata for a Buganizer Issue ID.
	FindWorkspaceForIssue(issueID int64) (WorkspaceIssueStatus, bool)
	// DevelopIssueInWorktree allocates/mounts a warm worktree slot for the given issue.
	DevelopIssueInWorktree(ctx context.Context, issueID int64, title string, customBranch string) (symlinkPath string, slotName string, err error)
}

// RegisteredWorkspaceIntegration holds the optional worktree integration registered at startup.
var RegisteredWorkspaceIntegration WorkspaceIntegration

var (
	prefixedIssueBranchRegex = regexp.MustCompile(`^(?:b[-/]|issue[-/])(\d+)(?:$|[-/])`)
	numericIssueBranchRegex  = regexp.MustCompile(`^(\d{6,})(?:$|[-/])`)
)

// ExtractIssueIDFromBranchName extracts a Buganizer issue ID from a branch name
// (e.g., "b-315378787-fix-rpc", "315378787-fix-rpc", "issue-315378787", or "b/315378787").
func ExtractIssueIDFromBranchName(branch string) (int64, bool) {
	branch = strings.TrimSpace(branch)
	if branch == "" {
		return 0, false
	}
	var numStr string
	if m := prefixedIssueBranchRegex.FindStringSubmatch(branch); len(m) > 1 {
		numStr = m[1]
	} else if m := numericIssueBranchRegex.FindStringSubmatch(branch); len(m) > 1 {
		numStr = m[1]
	}
	if numStr == "" {
		return 0, false
	}
	id, err := strconv.ParseInt(numStr, 10, 64)
	if err != nil || id <= 0 {
		return 0, false
	}
	return id, true
}
