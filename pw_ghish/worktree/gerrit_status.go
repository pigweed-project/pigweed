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
)

// StatusBadge classifies the Git & Gerrit review state of a project.
type StatusBadge string

const (
	BadgeNeedsAttention StatusBadge = "🔥 NEEDS_ATTENTION"
	BadgeReadyToLand    StatusBadge = "🚀 READY_TO_LAND"
	BadgeInReview       StatusBadge = "⏳ IN_REVIEW"
	BadgeGerritOffline  StatusBadge = "⏳ GERRIT_OFFLINE"
	BadgeCLMerged       StatusBadge = "🎉 CL_MERGED"
	BadgeLocalWIP       StatusBadge = "✎ LOCAL_WIP"
	BadgeCleanSynced    StatusBadge = "✨ CLEAN_SYNCED"
)

// ChangeStatus captures Gerrit review and CI health for a single Change-Id.
type ChangeStatus struct {
	ChangeID          string `json:"change_id"`
	Number            int    `json:"number"`
	Status            string `json:"status"` // "NEW", "MERGED", "ABANDONED"
	CodeReviewScore   int    `json:"code_review_score"`
	CommitQueueScore  int    `json:"commit_queue_score"`
	UnresolvedThreads int    `json:"unresolved_threads"`
	ChecksFailing     int    `json:"checks_failing"`
	Subject           string `json:"subject,omitempty"`
}

// GerritStatusProvider queries Gerrit for live review/merge statuses by Change-Id
// and resolves CL fetch refs for `--cl`.
type GerritStatusProvider interface {
	QueryChangesByID(ctx context.Context, changeIDs []string) (map[string]ChangeStatus, error)
	ResolveCLFetchRef(ctx context.Context, clRef string) (fetchRef string, changeID string, err error)
}

// NoopGerritStatusProvider returns an empty map when offline or unconfigured.
type NoopGerritStatusProvider struct{}

func (NoopGerritStatusProvider) QueryChangesByID(ctx context.Context, changeIDs []string) (map[string]ChangeStatus, error) {
	return make(map[string]ChangeStatus), nil
}

func (NoopGerritStatusProvider) ResolveCLFetchRef(ctx context.Context, clRef string) (string, string, error) {
	return "", "", fmt.Errorf("Gerrit client not configured")
}

// ComputeStatusBadge determines the StatusBadge and human-readable details/recommendation
// for a project given its local dirty/commit state and optional Gerrit ChangeStatus.
func ComputeStatusBadge(isDirty bool, localCommitsAhead int, changeID string, cs *ChangeStatus) (badge StatusBadge, details string, action string) {
	return ComputeStatusBadgeWithGerritState(isDirty, localCommitsAhead, changeID, cs, false)
}

// ComputeStatusBadgeWithGerritState determines the StatusBadge, taking into account whether
// the Gerrit query failed (gerritOffline == true).
func ComputeStatusBadgeWithGerritState(isDirty bool, localCommitsAhead int, changeID string, cs *ChangeStatus, gerritOffline bool) (badge StatusBadge, details string, action string) {
	if cs != nil && cs.Number > 0 {
		switch cs.Status {
		case "MERGED":
			details = fmt.Sprintf("pwrev/%d (Merged)", cs.Number)
			if isDirty {
				details += " + dirty tree"
			}
			return BadgeCLMerged, details, "Run `./gh wt next` for next CL, or `park`/`close`"
		case "NEW":
			if cs.UnresolvedThreads > 0 || cs.CodeReviewScore < 0 || cs.ChecksFailing > 0 {
				details = fmt.Sprintf("pwrev/%d (CR:%+d, %d threads, %d failing checks)",
					cs.Number, cs.CodeReviewScore, cs.UnresolvedThreads, cs.ChecksFailing)
				return BadgeNeedsAttention, details, "Inspect via `./gh pr view --comments` or `./gh pr checks`"
			}
			if cs.CodeReviewScore >= 2 {
				details = fmt.Sprintf("pwrev/%d (CR+%d, CQ ready)", cs.Number, cs.CodeReviewScore)
				return BadgeReadyToLand, details, "Approved! Ready to land (`./gh pr merge --cq`)"
			}
			details = fmt.Sprintf("pwrev/%d (CR:%+d, waiting review/CI)", cs.Number, cs.CodeReviewScore)
			return BadgeInReview, details, "Waiting on reviewers — safe candidate to `./gh wt park`"
		case "ABANDONED":
			details = fmt.Sprintf("pwrev/%d (Abandoned)", cs.Number)
			return BadgeCleanSynced, details, "CL abandoned -> Run `./gh wt close` or start new CL"
		}
	}

	if isDirty {
		return BadgeLocalWIP, "Uncommitted working tree changes (DIRTY)", "Protected from auto-swap; commit or stash changes"
	}
	if gerritOffline && changeID != "" {
		return BadgeGerritOffline, fmt.Sprintf("%s (Gerrit unreachable)", changeID), "Check network/auth or retry `./gh wt list`"
	}
	if localCommitsAhead > 0 || changeID != "" {
		return BadgeLocalWIP, "Local commits (no CL uploaded yet)", "Upload CL via `./gh pr create`"
	}
	return BadgeCleanSynced, "Synced with origin/main", "Ready for hacking"
}
