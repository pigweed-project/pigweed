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
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"sync"
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
	selfAccountCacheTTLSeconds = 86400
	maxAccountCacheBytes       = 64 * 1024
)

type cachedSelfAccountEntry struct {
	Account   AccountInfo `json:"account"`
	SavedUnix int64       `json:"saved_unix"`
}

type selfAccountCacheFile struct {
	Accounts map[string]cachedSelfAccountEntry `json:"accounts"`
}

var (
	selfAccountCacheMu  sync.Mutex
	memSelfAccountCache = make(map[string]cachedSelfAccountEntry)
)

func activeAccountCacheDir() string {
	if strings.TrimSpace(os.Getenv("GH_ISH_ACCOUNT_CACHE")) == "0" {
		return ""
	}
	if dir := strings.TrimSpace(os.Getenv("GH_ISH_ACCOUNT_CACHE_DIR")); dir != "" {
		return dir
	}
	if os.Getenv("TEST_TMPDIR") != "" || strings.HasSuffix(os.Args[0], ".test") {
		return ""
	}
	if xdg := strings.TrimSpace(os.Getenv("XDG_CACHE_HOME")); xdg != "" {
		return filepath.Join(xdg, "pw_ghish")
	}
	if home, err := os.UserHomeDir(); err == nil && strings.TrimSpace(home) != "" {
		return filepath.Join(home, ".cache", "pw_ghish")
	}
	return ""
}

func readSelfAccountCacheLocked(dir string) selfAccountCacheFile {
	var cache selfAccountCacheFile
	info, err := os.Lstat(dir)
	if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		if err != nil && !os.IsNotExist(err) {
			// OK: best-effort disk cache
			slog.Debug("failed to stat account cache dir", "dir", dir, "err", err)
		}
		return selfAccountCacheFile{Accounts: make(map[string]cachedSelfAccountEntry)}
	}
	path := filepath.Join(dir, "accounts.json")
	if pInfo, err := os.Lstat(path); err != nil || !pInfo.Mode().IsRegular() {
		if err != nil && !os.IsNotExist(err) {
			// OK: best-effort disk cache
			slog.Debug("failed to stat account cache file", "path", path, "err", err)
		}
		return selfAccountCacheFile{Accounts: make(map[string]cachedSelfAccountEntry)}
	}
	f, err := os.Open(path)
	if err != nil {
		if !os.IsNotExist(err) {
			// OK: best-effort disk cache
			slog.Debug("failed to open account cache file", "path", path, "err", err)
		}
		return selfAccountCacheFile{Accounts: make(map[string]cachedSelfAccountEntry)}
	}
	defer f.Close()
	data, err := io.ReadAll(io.LimitReader(f, maxAccountCacheBytes))
	if err != nil {
		// OK: best-effort disk cache
		slog.Debug("failed to read account cache file", "path", path, "err", err)
		return selfAccountCacheFile{Accounts: make(map[string]cachedSelfAccountEntry)}
	}
	if err := json.Unmarshal(data, &cache); err != nil {
		// OK: best-effort disk cache
		slog.Debug("failed to unmarshal account cache file", "path", path, "err", err)
		return selfAccountCacheFile{Accounts: make(map[string]cachedSelfAccountEntry)}
	}
	if cache.Accounts == nil {
		return selfAccountCacheFile{Accounts: make(map[string]cachedSelfAccountEntry)}
	}
	return cache
}

func writeSelfAccountCacheLocked(dir string, cache selfAccountCacheFile) {
	if err := os.MkdirAll(dir, 0700); err != nil {
		// OK: best-effort disk cache
		slog.Debug("failed to create account cache dir", "dir", dir, "err", err)
		return
	}
	if err := os.Chmod(dir, 0700); err != nil {
		// OK: best-effort disk cache
		slog.Debug("failed to chmod account cache dir", "dir", dir, "err", err)
		return
	}
	info, err := os.Lstat(dir)
	if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		// OK: best-effort disk cache
		slog.Debug("account cache dir failed safety check", "dir", dir, "err", err)
		return
	}
	data, err := json.Marshal(cache)
	if err != nil {
		// OK: best-effort disk cache
		slog.Debug("failed to marshal account cache", "err", err)
		return
	}
	tmp, err := os.CreateTemp(dir, "accounts-*.tmp")
	if err != nil {
		// OK: best-effort disk cache
		slog.Debug("failed to create temp account cache file", "dir", dir, "err", err)
		return
	}
	tmpName := tmp.Name()
	if err := os.Chmod(tmpName, 0600); err != nil {
		// OK: best-effort disk cache
		slog.Debug("failed to chmod temp account cache file", "path", tmpName, "err", err)
		_ = tmp.Close()
		_ = os.Remove(tmpName)
		return
	}
	if _, err := tmp.Write(data); err != nil {
		// OK: best-effort disk cache
		slog.Debug("failed to write temp account cache file", "path", tmpName, "err", err)
		_ = tmp.Close()
		_ = os.Remove(tmpName)
		return
	}
	if err := tmp.Close(); err != nil {
		// OK: best-effort disk cache
		slog.Debug("failed to close temp account cache file", "path", tmpName, "err", err)
		_ = os.Remove(tmpName)
		return
	}
	targetPath := filepath.Join(dir, "accounts.json")
	if err := os.Rename(tmpName, targetPath); err != nil {
		// OK: best-effort disk cache
		slog.Debug("failed to rename temp account cache file", "from", tmpName, "to", targetPath, "err", err)
		_ = os.Remove(tmpName)
	}
}

func isValidSelfAccountCacheEntry(entry cachedSelfAccountEntry, nowUnix int64) bool {
	return entry.Account.AccountID > 0 &&
		entry.SavedUnix > 0 &&
		nowUnix >= entry.SavedUnix &&
		nowUnix-entry.SavedUnix < selfAccountCacheTTLSeconds
}

func tryReadSelfAccountCache(dir, hostKey string, now time.Time) (AccountInfo, bool) {
	selfAccountCacheMu.Lock()
	defer selfAccountCacheMu.Unlock()
	nowUnix := now.Unix()
	memKey := dir + "\x00" + hostKey
	if entry, ok := memSelfAccountCache[memKey]; ok {
		if isValidSelfAccountCacheEntry(entry, nowUnix) {
			return entry.Account, true
		}
		delete(memSelfAccountCache, memKey)
	}
	cache := readSelfAccountCacheLocked(dir)
	entry, ok := cache.Accounts[hostKey]
	if !ok || !isValidSelfAccountCacheEntry(entry, nowUnix) {
		return AccountInfo{}, false
	}
	memSelfAccountCache[memKey] = entry
	return entry.Account, true
}

func tryWriteSelfAccountCache(dir, hostKey string, acc AccountInfo, now time.Time) {
	if acc.AccountID <= 0 {
		return
	}
	selfAccountCacheMu.Lock()
	defer selfAccountCacheMu.Unlock()
	cache := readSelfAccountCacheLocked(dir)
	nowUnix := now.Unix()
	for k, v := range cache.Accounts {
		if !isValidSelfAccountCacheEntry(v, nowUnix) {
			delete(cache.Accounts, k)
			delete(memSelfAccountCache, dir+"\x00"+k)
		}
	}
	entry := cachedSelfAccountEntry{
		Account:   acc,
		SavedUnix: nowUnix,
	}
	cache.Accounts[hostKey] = entry
	memSelfAccountCache[dir+"\x00"+hostKey] = entry
	writeSelfAccountCacheLocked(dir, cache)
}

func resetSelfAccountMemCache() {
	selfAccountCacheMu.Lock()
	clear(memSelfAccountCache)
	selfAccountCacheMu.Unlock()
}

// PeekSelfAccountCache checks the in-memory and on-disk accounts.json cache for
// gerritHost and returns the cached *AccountInfo on hit, or nil on miss
// without making any network RPC.
func PeekSelfAccountCache(gerritHost string) *AccountInfo {
	hostKey := strings.ToLower(strings.TrimSpace(CleanGerritHost(gerritHost)))
	if hostKey == "" || isLoopbackHost(gerritHost) {
		return nil
	}
	cacheDir := activeAccountCacheDir()
	if cacheDir == "" {
		return nil
	}
	if acc, ok := tryReadSelfAccountCache(cacheDir, hostKey, time.Now()); ok {
		return &acc
	}
	return nil
}

// CachedSelfAccountRef returns the cached numeric Gerrit account ID for cmd's
// target Gerrit host when client is authenticated and a warm accounts.json
// entry exists, or ("self", nil) otherwise without making any network RPC or
// spawning git subprocesses when unauthenticated.
func CachedSelfAccountRef(ctx context.Context, cmd *cobra.Command, client *GerritClient) (string, *AccountInfo) {
	if client == nil || !client.Authenticated() || activeAccountCacheDir() == "" {
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
	hostKey := strings.ToLower(strings.TrimSpace(CleanGerritHost(gerritHost)))
	if hostKey != "" && !isLoopbackHost(gerritHost) && acc != nil && acc.AccountID > 0 {
		if cacheDir := activeAccountCacheDir(); cacheDir != "" {
			tryWriteSelfAccountCache(cacheDir, hostKey, *acc, time.Now())
		}
	}
	return acc, nil
}
