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
	"cmp"
	"context"
	"fmt"
	"slices"
	"strconv"
	"strings"
	"time"

	"github.com/spf13/cobra"
)

// FormatAccount formats a Gerrit AccountInfo consistently across all commands.
// It prioritizes Username (LDAP username, matching GitHub CLI login behavior),
// falls back to display Name, and finally falls back to Email.
func FormatAccount(acc AccountInfo) string {
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
func BuildAttentionMap(attnSet map[string]AttentionSetInfo) map[int]bool {
	m := make(map[int]bool)
	for _, info := range attnSet {
		if info.Account.AccountID != 0 {
			m[info.Account.AccountID] = true
		}
	}
	return m
}

// FormatReviewer formats a Gerrit account with its role (CC or Reviewer) and attention set status.
func FormatReviewer(acc AccountInfo, inAttention bool, isCC bool) string {
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
func FormatChangeReviewersAndAssignees(change *ChangeInfo) (assignees []string, reviewers []string) {
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
		var remaining []AttentionSetInfo
		for _, info := range change.AttentionSet {
			if info.Account.AccountID != 0 && !seenAccs[info.Account.AccountID] {
				remaining = append(remaining, info)
			}
		}
		slices.SortFunc(remaining, func(a, b AttentionSetInfo) int {
			return cmp.Compare(a.Account.AccountID, b.Account.AccountID)
		})
		for _, info := range remaining {
			assignees = append(assignees, FormatReviewer(info.Account, true, false))
			seenAccs[info.Account.AccountID] = true
		}
	}

	return assignees, reviewers
}

const (
	selfAccountCacheTTL    = 24 * time.Hour
	selfAccountCacheEnvVar = "GHISH_ACCOUNT_CACHE"
)

type cachedSelfAccount struct {
	Account AccountInfo `json:"account"`
}

func (cachedSelfAccount) CachePolicy() CachePolicy {
	return CachePolicy{
		Tier:       CachePersistent,
		Name:       "accounts",
		MaxTTL:     selfAccountCacheTTL,
		DisableEnv: selfAccountCacheEnvVar,
	}
}

var defaultAccountCache = NewCache[string, cachedSelfAccount]()

// accountCacheKey normalizes gerritHost into its accounts.json lookup key, or
// returns "" (which Cache treats as a no-op) when no remote host is configured
// or gerritHost is an ephemeral loopback test server.
func accountCacheKey(gerritHost string) string {
	if isLoopbackHost(gerritHost) {
		return ""
	}
	return CanonicalGerritHost(strings.ToLower(gerritHost))
}

func tryWriteSelfAccountCache(gerritHost string, acc AccountInfo) {
	// Gerrit _account_id values are strictly positive; ignore unpopulated (0)
	// or malformed responses so CachedSelfAccountRef never substitutes "0" for "self".
	if acc.AccountID <= 0 {
		return
	}
	defaultAccountCache.Put(accountCacheKey(gerritHost), cachedSelfAccount{Account: acc})
}

// PeekSelfAccountCache checks the in-memory and on-disk accounts.json cache for
// gerritHost and returns the cached *AccountInfo on hit, or nil on miss
// without making any network RPC.
func PeekSelfAccountCache(gerritHost string) *AccountInfo {
	entry, ok := defaultAccountCache.Get(accountCacheKey(gerritHost))
	// Gerrit _account_id values are strictly positive; treat an unpopulated (0)
	// or corrupt on-disk entry as a miss so callers fall back to "self" or re-fetch.
	if !ok || entry.Account.AccountID <= 0 {
		return nil
	}
	acc := entry.Account
	return &acc
}

// CachedSelfAccountRef returns the cached numeric Gerrit account ID for cmd's
// target Gerrit host when client is authenticated and a warm accounts.json
// entry exists, or ("self", nil) otherwise without making any network RPC or
// spawning git subprocesses when unauthenticated.
func CachedSelfAccountRef(ctx context.Context, cmd *cobra.Command, client *GerritClient) (string, *AccountInfo) {
	if client == nil || !client.Authenticated() {
		return "self", nil
	}
	var gerritHost string
	if cmd != nil && isHostFlagChanged(cmd) && HostFlag != "" {
		gerritHost = CanonicalGerritHost(HostFlag)
	} else {
		cfg := GetConfig(cmd)
		if cfg == nil && cmd != nil {
			cfg = &Config{Host: HostFlag, Git: DefaultGitRunner}
		}
		if cfg != nil {
			gerritHost = cfg.GerritHost(ctx)
		}
	}
	if acc := PeekSelfAccountCache(gerritHost); acc != nil && acc.AccountID > 0 {
		return strconv.Itoa(acc.AccountID), acc
	}
	return "self", nil
}

// GetSelfAccountCached returns the authenticated user's Gerrit AccountInfo for gerritHost,
// consulting a 24-hour disk cache (accounts.json) before querying GET /a/accounts/self.
func GetSelfAccountCached(ctx context.Context, client *GerritClient, gerritHost string) (*AccountInfo, error) {
	if client == nil || !client.Authenticated() {
		return nil, fmt.Errorf("cannot resolve self account: Gerrit client is unauthenticated")
	}
	if cached := PeekSelfAccountCache(gerritHost); cached != nil {
		return cached, nil
	}

	acc, err := client.GetSelfAccount(ctx)
	if err != nil {
		return nil, err
	}
	if acc != nil {
		tryWriteSelfAccountCache(gerritHost, *acc)
	}
	return acc, nil
}
