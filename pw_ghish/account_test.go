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
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"reflect"
	"testing"
	"time"

	"github.com/andygrunwald/go-gerrit"
)

func TestFormatAccount(t *testing.T) {
	tests := []struct {
		name     string
		acc      gerrit.AccountInfo
		expected string
	}{
		{
			name: "username takes highest priority",
			acc: gerrit.AccountInfo{
				AccountID: 100,
				Username:  "jdoe",
				Name:      "John Doe",
				Email:     "jdoe@example.com",
			},
			expected: "jdoe",
		},
		{
			name: "name takes priority when username is empty",
			acc: gerrit.AccountInfo{
				AccountID: 101,
				Name:      "Jane Smith",
				Email:     "jsmith@example.com",
			},
			expected: "Jane Smith",
		},
		{
			name: "email used when username and name are empty",
			acc: gerrit.AccountInfo{
				AccountID: 102,
				Email:     "bot@example.com",
			},
			expected: "bot@example.com",
		},
		{
			name: "account ID used when string fields are empty",
			acc: gerrit.AccountInfo{
				AccountID: 103,
			},
			expected: "account #103",
		},
		{
			name:     "empty account info returns empty string",
			acc:      gerrit.AccountInfo{},
			expected: "",
		},
		{
			name: "whitespace is trimmed",
			acc: gerrit.AccountInfo{
				Username: "  trimmed_user  ",
				Name:     "  Trimmed Name  ",
			},
			expected: "trimmed_user",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got := FormatAccount(tc.acc)
			if got != tc.expected {
				t.Errorf("FormatAccount() = %q, want %q", got, tc.expected)
			}
		})
	}
}

func TestBuildAttentionMap(t *testing.T) {
	t.Run("nil or empty map", func(t *testing.T) {
		m := BuildAttentionMap(nil)
		if m == nil {
			t.Fatal("expected non-nil map")
		}
		if len(m) != 0 {
			t.Errorf("expected empty map, got %v", m)
		}
		if m[123] {
			t.Error("expected false for arbitrary lookup")
		}
	})

	t.Run("populates valid account IDs and ignores 0", func(t *testing.T) {
		attnSet := map[string]gerrit.AttentionSetInfo{
			"10": {Account: gerrit.AccountInfo{AccountID: 10}},
			"20": {Account: gerrit.AccountInfo{AccountID: 20}},
			"0":  {Account: gerrit.AccountInfo{AccountID: 0}},
		}
		m := BuildAttentionMap(attnSet)
		if !m[10] {
			t.Errorf("expected account 10 in attention set")
		}
		if !m[20] {
			t.Errorf("expected account 20 in attention set")
		}
		if m[0] {
			t.Errorf("did not expect account 0 in attention set")
		}
		if m[30] {
			t.Errorf("did not expect account 30 in attention set")
		}
	})
}

func TestFormatReviewer(t *testing.T) {
	acc := gerrit.AccountInfo{Name: "Alice"}

	tests := []struct {
		name        string
		inAttention bool
		isCC        bool
		expected    string
	}{
		{
			name:        "regular reviewer not in attention",
			inAttention: false,
			isCC:        false,
			expected:    "Alice",
		},
		{
			name:        "regular reviewer in attention",
			inAttention: true,
			isCC:        false,
			expected:    "Alice (attention)",
		},
		{
			name:        "cc not in attention",
			inAttention: false,
			isCC:        true,
			expected:    "Alice (cc)",
		},
		{
			name:        "cc in attention",
			inAttention: true,
			isCC:        true,
			expected:    "Alice (cc, attention)",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got := FormatReviewer(acc, tc.inAttention, tc.isCC)
			if got != tc.expected {
				t.Errorf("FormatReviewer() = %q, want %q", got, tc.expected)
			}
		})
	}
}

func TestFormatChangeReviewersAndAssignees(t *testing.T) {
	t.Run("nil change returns nil", func(t *testing.T) {
		assignees, reviewers := FormatChangeReviewersAndAssignees(nil)
		if assignees != nil || reviewers != nil {
			t.Errorf("expected nil slices for nil change, got %v, %v", assignees, reviewers)
		}
	})

	t.Run("comprehensive change formatting", func(t *testing.T) {
		change := &gerrit.ChangeInfo{
			Owner: gerrit.AccountInfo{
				AccountID: 1,
				Name:      "Owner Alice",
			},
			Reviewers: map[string][]gerrit.AccountInfo{
				"REVIEWER": {
					{AccountID: 2, Name: "Bob"},
					{AccountID: 3, Name: "Charlie"},
				},
				"CC": {
					{AccountID: 4, Name: "Dave"},
					{AccountID: 5, Name: "Eve"},
				},
			},
			AttentionSet: map[string]gerrit.AttentionSetInfo{
				"1": {Account: gerrit.AccountInfo{AccountID: 1, Name: "Owner Alice"}},
				"2": {Account: gerrit.AccountInfo{AccountID: 2, Name: "Bob"}},
				"5": {Account: gerrit.AccountInfo{AccountID: 5, Name: "Eve"}},
				"9": {Account: gerrit.AccountInfo{AccountID: 9, Name: "Frank"}}, // remaining attention set
			},
		}

		assignees, reviewers := FormatChangeReviewersAndAssignees(change)

		wantAssignees := []string{
			"Owner Alice (attention)",
			"Dave (cc)",
			"Eve (cc, attention)",
			"Frank (attention)",
		}
		wantReviewers := []string{
			"Bob (attention)",
			"Charlie",
		}

		if !reflect.DeepEqual(assignees, wantAssignees) {
			t.Errorf("assignees mismatch\ngot:  %v\nwant: %v", assignees, wantAssignees)
		}
		if !reflect.DeepEqual(reviewers, wantReviewers) {
			t.Errorf("reviewers mismatch\ngot:  %v\nwant: %v", reviewers, wantReviewers)
		}
	})
}

func TestGetSelfAccountCached(t *testing.T) {
	cacheDir := filepath.Join(t.TempDir(), "pw_ghish_accounts")
	t.Setenv("GH_ISH_ACCOUNT_CACHE_DIR", cacheDir)

	var selfCalls int
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/accounts/self" {
			selfCalls++
			w.Header().Set("Content-Type", "application/json")
			_, _ = w.Write([]byte(")]}'\n{\"_account_id\":100042,\"username\":\"alice\",\"name\":\"Alice Dev\",\"email\":\"alice@example.com\"}\n"))
			return
		}
		http.NotFound(w, r)
	}))
	defer srv.Close()

	client, err := gerrit.NewClient(t.Context(), srv.URL, srv.Client())
	if err != nil {
		t.Fatalf("gerrit.NewClient failed: %v", err)
	}

	// 0. PeekSelfAccountCache returns nil on cold cache miss without making RPCs.
	if got := PeekSelfAccountCache("pigweed-review.googlesource.com"); got != nil {
		t.Fatalf("PeekSelfAccountCache on cold cache = %+v, want nil", got)
	}
	if selfCalls != 0 {
		t.Fatalf("selfCalls after cold PeekSelfAccountCache = %d, want 0", selfCalls)
	}

	// 1. First call fetches from Gerrit and writes accounts.json to disk.
	acc1, err := GetSelfAccountCached(t.Context(), client, "pigweed-review.googlesource.com")
	if err != nil {
		t.Fatalf("GetSelfAccountCached first call failed: %v", err)
	}
	if acc1 == nil || acc1.AccountID != 100042 || acc1.Username != "alice" {
		t.Fatalf("acc1 = %+v, want AccountID=100042 Username=alice", acc1)
	}
	if selfCalls != 1 {
		t.Fatalf("selfCalls = %d, want 1", selfCalls)
	}

	dirInfo, err := os.Stat(cacheDir)
	if err != nil {
		t.Fatalf("os.Stat(cacheDir) failed: %v", err)
	}
	if perm := dirInfo.Mode().Perm(); perm != 0700 {
		t.Errorf("cacheDir perm = %o, want 0700", perm)
	}
	fileInfo, err := os.Stat(filepath.Join(cacheDir, "accounts.json"))
	if err != nil {
		t.Fatalf("os.Stat(accounts.json) failed: %v", err)
	}
	if perm := fileInfo.Mode().Perm(); perm != 0600 {
		t.Errorf("accounts.json perm = %o, want 0600", perm)
	}

	// 2. Clear in-memory cache to force reading from accounts.json on disk,
	// then verify PeekSelfAccountCache and GetSelfAccountCached hit disk with zero RPCs.
	resetSelfAccountMemCache()
	if peeked := PeekSelfAccountCache("https://pigweed-review.googlesource.com"); peeked == nil || peeked.AccountID != 100042 {
		t.Fatalf("PeekSelfAccountCache on warm disk cache = %+v, want AccountID=100042", peeked)
	}
	acc2, err := GetSelfAccountCached(t.Context(), client, "https://pigweed-review.googlesource.com")
	if err != nil {
		t.Fatalf("GetSelfAccountCached second call failed: %v", err)
	}
	if acc2 == nil || acc2.AccountID != 100042 || acc2.Email != "alice@example.com" {
		t.Fatalf("acc2 = %+v, want AccountID=100042 Email=alice@example.com", acc2)
	}
	if selfCalls != 1 {
		t.Errorf("selfCalls after disk cache hit = %d, want 1", selfCalls)
	}

	// 3. Loopback host bypasses disk cache.
	if peeked := PeekSelfAccountCache(srv.URL); peeked != nil {
		t.Errorf("PeekSelfAccountCache(loopback) = %+v, want nil", peeked)
	}
	if _, err := GetSelfAccountCached(t.Context(), client, srv.URL); err != nil {
		t.Fatalf("GetSelfAccountCached loopback call failed: %v", err)
	}
	if selfCalls != 2 {
		t.Errorf("selfCalls after loopback call = %d, want 2", selfCalls)
	}
}

func TestSelfAccountCache_EdgeCases(t *testing.T) {
	now := time.Unix(1700000000, 0)

	t.Run("TTL expiration and future timestamp rejection", func(t *testing.T) {
		dir := filepath.Join(t.TempDir(), "cache")
		acc := gerrit.AccountInfo{AccountID: 77, Username: "bob"}

		// Entry at exact TTL boundary is expired when read at now.
		tryWriteSelfAccountCache(dir, "expired-host.example.com", acc, now.Add(-time.Duration(selfAccountCacheTTLSeconds)*time.Second))
		if _, ok := tryReadSelfAccountCache(dir, "expired-host.example.com", now); ok {
			t.Error("entry at exact TTL boundary should be expired")
		}

		// Entry saved just within TTL is valid when read at now.
		tryWriteSelfAccountCache(dir, "fresh-host.example.com", acc, now.Add(-time.Duration(selfAccountCacheTTLSeconds-1)*time.Second))
		resetSelfAccountMemCache()
		if got, ok := tryReadSelfAccountCache(dir, "fresh-host.example.com", now); !ok || got.AccountID != 77 {
			t.Errorf("entry at TTL-1s: got (%+v, %v), want AccountID=77, true", got, ok)
		}

		// Writing a new entry at now prunes expired entries while preserving valid ones.
		tryWriteSelfAccountCache(dir, "second-fresh.example.com", gerrit.AccountInfo{AccountID: 88}, now)
		disk := readSelfAccountCacheLocked(dir)
		if _, ok := disk.Accounts["expired-host.example.com"]; ok {
			t.Error("expected expired-host.example.com to be pruned on write")
		}
		if _, ok := disk.Accounts["fresh-host.example.com"]; !ok {
			t.Error("expected fresh-host.example.com to be preserved on write")
		}
		if _, ok := disk.Accounts["second-fresh.example.com"]; !ok {
			t.Error("expected second-fresh.example.com to be present on disk")
		}

		// Entry with timestamp in the future (clock skew) is rejected when read at now.
		tryWriteSelfAccountCache(dir, "future-host.example.com", acc, now.Add(10*time.Second))
		if _, ok := tryReadSelfAccountCache(dir, "future-host.example.com", now); ok {
			t.Error("entry with future timestamp should be rejected")
		}

		// Account with AccountID <= 0 is ignored and never written.
		tryWriteSelfAccountCache(dir, "zero-id.example.com", gerrit.AccountInfo{AccountID: 0, Username: "ghost"}, now)
		if _, ok := tryReadSelfAccountCache(dir, "zero-id.example.com", now); ok {
			t.Error("entry with AccountID <= 0 should not be cached")
		}
	})

	t.Run("corrupt JSON on disk recovers cleanly", func(t *testing.T) {
		dir := filepath.Join(t.TempDir(), "corrupt")
		if err := os.MkdirAll(dir, 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(dir, "accounts.json"), []byte("{not-json"), 0600); err != nil {
			t.Fatal(err)
		}
		resetSelfAccountMemCache()
		if _, ok := tryReadSelfAccountCache(dir, "pigweed-review.googlesource.com", now); ok {
			t.Error("expected cache miss on corrupt JSON")
		}
		tryWriteSelfAccountCache(dir, "pigweed-review.googlesource.com", gerrit.AccountInfo{AccountID: 99, Username: "carol"}, now)
		resetSelfAccountMemCache()
		if got, ok := tryReadSelfAccountCache(dir, "pigweed-review.googlesource.com", now); !ok || got.AccountID != 99 {
			t.Errorf("after overwriting corrupt JSON: got (%+v, %v), want AccountID=99, true", got, ok)
		}
	})

	t.Run("refuses symlinked cache directory or symlinked accounts.json", func(t *testing.T) {
		base := t.TempDir()
		realDir := filepath.Join(base, "real")
		if err := os.MkdirAll(realDir, 0700); err != nil {
			t.Fatal(err)
		}
		symlinkDir := filepath.Join(base, "symlink-dir")
		if err := os.Symlink(realDir, symlinkDir); err != nil {
			t.Fatal(err)
		}
		tryWriteSelfAccountCache(symlinkDir, "host.example.com", gerrit.AccountInfo{AccountID: 55}, now)
		if _, err := os.Stat(filepath.Join(realDir, "accounts.json")); !os.IsNotExist(err) {
			t.Errorf("expected write through symlinked dir to be refused, stat err = %v", err)
		}

		// Symlinked accounts.json inside a real directory is refused on read.
		secretFile := filepath.Join(base, "secret.json")
		if err := os.WriteFile(secretFile, []byte(`{"accounts":{"host.example.com":{"account":{"_account_id":55},"saved_unix":1700000000}}}`), 0600); err != nil {
			t.Fatal(err)
		}
		if err := os.Symlink(secretFile, filepath.Join(realDir, "accounts.json")); err != nil {
			t.Fatal(err)
		}
		resetSelfAccountMemCache()
		if _, ok := tryReadSelfAccountCache(realDir, "host.example.com", now); ok {
			t.Error("expected read of symlinked accounts.json to be refused")
		}
	})

	t.Run("GH_ISH_ACCOUNT_CACHE=0 disables cache", func(t *testing.T) {
		dir := filepath.Join(t.TempDir(), "disabled")
		t.Setenv("GH_ISH_ACCOUNT_CACHE_DIR", dir)
		t.Setenv("GH_ISH_ACCOUNT_CACHE", "0")
		if got := activeAccountCacheDir(); got != "" {
			t.Errorf("activeAccountCacheDir() with GH_ISH_ACCOUNT_CACHE=0 = %q, want empty", got)
		}
	})
}
