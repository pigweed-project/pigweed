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
	"fmt"
	"sort"
	"strings"

	"github.com/andygrunwald/go-gerrit"
)

// FormatAccount formats a Gerrit AccountInfo consistently across all commands.
// It prioritizes Username (LDAP username, matching GitHub CLI login behavior),
// falls back to display Name, and finally falls back to Email.
func FormatAccount(acc gerrit.AccountInfo) string {
	if u := strings.TrimSpace(acc.Username); u != "" {
		return u
	}
	if n := strings.TrimSpace(acc.Name); n != "" {
		return n
	}
	if e := strings.TrimSpace(acc.Email); e != "" {
		return e
	}
	if acc.AccountID != 0 {
		return fmt.Sprintf("account #%d", acc.AccountID)
	}
	return ""
}

// BuildAttentionMap creates a fast lookup set of account IDs present in the attention set.
func BuildAttentionMap(attnSet map[string]gerrit.AttentionSetInfo) map[int]bool {
	m := make(map[int]bool)
	for _, info := range attnSet {
		if info.Account.AccountID != 0 {
			m[info.Account.AccountID] = true
		}
	}
	return m
}

// FormatReviewer formats a Gerrit account with its role (CC or Reviewer) and attention set status.
func FormatReviewer(acc gerrit.AccountInfo, inAttention bool, isCC bool) string {
	name := FormatAccount(acc)
	switch {
	case isCC && inAttention:
		return name + " (cc, attention)"
	case isCC:
		return name + " (cc)"
	case inAttention:
		return name + " (attention)"
	default:
		return name
	}
}

// FormatChangeReviewersAndAssignees extracts and formats the assignees and reviewers
// lists for a change, incorporating CC annotations and Attention Set status.
func FormatChangeReviewersAndAssignees(change *gerrit.ChangeInfo) (assignees []string, reviewers []string) {
	if change == nil {
		return nil, nil
	}

	attnMap := BuildAttentionMap(change.AttentionSet)
	seenAccs := make(map[int]bool)

	// 1. Owner is always the primary assignee.
	ownerName := FormatAccount(change.Owner)
	if ownerName != "" {
		inAttn := change.Owner.AccountID != 0 && attnMap[change.Owner.AccountID]
		assignees = append(assignees, FormatReviewer(change.Owner, inAttn, false))
		if change.Owner.AccountID != 0 {
			seenAccs[change.Owner.AccountID] = true
		}
	}

	// 2. CC reviewers are listed in assignees.
	if change.Reviewers != nil {
		for _, acc := range change.Reviewers["CC"] {
			inAttn := acc.AccountID != 0 && attnMap[acc.AccountID]
			assignees = append(assignees, FormatReviewer(acc, inAttn, true))
			if acc.AccountID != 0 {
				seenAccs[acc.AccountID] = true
			}
		}

		// 3. Regular reviewers.
		for _, acc := range change.Reviewers["REVIEWER"] {
			inAttn := acc.AccountID != 0 && attnMap[acc.AccountID]
			reviewers = append(reviewers, FormatReviewer(acc, inAttn, false))
			if acc.AccountID != 0 {
				seenAccs[acc.AccountID] = true
			}
		}
	}

	// 4. Remaining accounts in the attention set that were not in Owner, CC, or REVIEWER.
	// Sort by AccountID for deterministic output.
	if len(change.AttentionSet) > 0 {
		var remaining []gerrit.AttentionSetInfo
		for _, info := range change.AttentionSet {
			if info.Account.AccountID != 0 && !seenAccs[info.Account.AccountID] {
				remaining = append(remaining, info)
			}
		}
		sort.Slice(remaining, func(i, j int) bool {
			return remaining[i].Account.AccountID < remaining[j].Account.AccountID
		})
		for _, info := range remaining {
			assignees = append(assignees, FormatReviewer(info.Account, true, false))
			seenAccs[info.Account.AccountID] = true
		}
	}

	return assignees, reviewers
}
