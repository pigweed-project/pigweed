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
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"
	"sync"
	"testing"

	"github.com/google/go-cmp/cmp"
)

// prefetchCall is the recorded shared git config prefetch invocation.
const prefetchCall = "git config -z --get-regexp " + prefetchedGitConfigRegexp

// isolateGitEnv strips ambient git configuration and repo-local env vars so
// tests see only the repository they create.
func isolateGitEnv(t *testing.T) {
	t.Helper()
	t.Setenv("GIT_CONFIG_GLOBAL", os.DevNull)
	t.Setenv("GIT_CONFIG_NOSYSTEM", "1")
	for _, kv := range os.Environ() {
		k, _, _ := strings.Cut(kv, "=")
		if isRepoLocalGitEnvKey(k) || k == "GIT_CEILING_DIRECTORIES" {
			t.Setenv(k, "")
			_ = os.Unsetenv(k)
		}
	}
}

func runTestGit(t *testing.T, dir string, args ...string) string {
	t.Helper()
	var out, stderr strings.Builder
	r := &RealGitRunner{Dir: dir}
	if err := r.Run(t.Context(), &out, &stderr, args...); err != nil {
		t.Fatalf("git %s in %s failed: %v (%s)", strings.Join(args, " "), dir, err, stderr.String())
	}
	return strings.TrimSpace(out.String())
}

// initTestRepo initializes a 1-commit repository with a main branch.
func initTestRepo(t *testing.T) string {
	t.Helper()
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git binary not found in PATH")
	}
	dir := t.TempDir()
	var stderr strings.Builder
	r := &RealGitRunner{Dir: dir}
	if err := r.Run(t.Context(), io.Discard, &stderr, "init", "-q", "--initial-branch=main"); err != nil {
		t.Fatalf("git init failed: %v (%s)", err, stderr.String())
	}
	runTestGit(t, dir, "config", "user.name", "Test")
	runTestGit(t, dir, "config", "user.email", "test@example.com")
	runTestGit(t, dir, "commit", "-q", "--allow-empty", "-m", "pw_foo: Initial\n\nChange-Id: I0000000000000000000000000000000000000001")
	return dir
}

// recordGitCalls runs fn and returns every subprocess it constructed.
func recordGitCalls(t *testing.T, fn func()) []string {
	t.Helper()
	var mu sync.Mutex
	var calls []string
	restore := RecordSubprocessesForTesting(func(call string) {
		mu.Lock()
		calls = append(calls, call)
		mu.Unlock()
	})
	defer restore()
	fn()
	mu.Lock()
	defer mu.Unlock()
	return slices.Clone(calls)
}

// inclusive-language: disable
func TestPickDefaultBranchCandidate(t *testing.T) {
	cands := defaultBranchRefCandidates("goog")
	for _, tc := range []struct {
		name   string
		out    string
		want   string
		wantOK bool
	}{
		{name: "local main wins", out: "refs/heads/master\nrefs/heads/main\n", want: "main", wantOK: true},
		{name: "remote main before remote master", out: "refs/remotes/goog/master\nrefs/remotes/goog/main\n", want: "main", wantOK: true},
		{name: "remote master before local master", out: "refs/heads/master\nrefs/remotes/goog/master\n", want: "master", wantOK: true},
		{name: "nested refs below a pattern do not match", out: "refs/heads/main/x\nrefs/remotes/goog/main/y\n", wantOK: false},
		{name: "other remote ignored", out: "refs/remotes/origin/main\n", wantOK: false},
		{name: "empty", out: "", wantOK: false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			got, ok := pickDefaultBranchCandidate(cands, tc.out)
			if got != tc.want || ok != tc.wantOK {
				t.Errorf("pickDefaultBranchCandidate(%q) = (%q, %v), want (%q, %v)", tc.out, got, ok, tc.want, tc.wantOK)
			}
		})
	}
}

// inclusive-language: enable

// TestResolveRemote_RealRepo checks remote resolution against real
// repositories: results must match the documented priority order, and config
// queries are served by the shared config prefetch (no per-key config or
// `@{u}` subprocesses).
func TestResolveRemote_RealRepo(t *testing.T) {
	for _, tc := range []struct {
		name       string
		setup      func(t *testing.T, dir string)
		want       string
		extraCalls []string
	}{
		{
			name:       "no remotes falls back to origin",
			want:       "origin",
			extraCalls: []string{"git branch --show-current", "git remote"},
		},
		{
			name:       "single non-origin remote",
			setup:      func(t *testing.T, dir string) { runTestGit(t, dir, "remote", "add", "goog", "https://example.com/r") },
			want:       "goog",
			extraCalls: []string{"git branch --show-current"},
		},
		{
			name: "origin preferred among multiple remotes",
			setup: func(t *testing.T, dir string) {
				runTestGit(t, dir, "remote", "add", "zeta", "https://example.com/z")
				runTestGit(t, dir, "remote", "add", "origin", "https://example.com/o")
			},
			want:       "origin",
			extraCalls: []string{"git branch --show-current"},
		},
		{
			name: "branch tracking remote wins over origin",
			setup: func(t *testing.T, dir string) {
				runTestGit(t, dir, "remote", "add", "origin", "https://example.com/o")
				runTestGit(t, dir, "remote", "add", "goog", "https://example.com/g")
				runTestGit(t, dir, "config", "branch.main.remote", "goog")
				runTestGit(t, dir, "config", "branch.main.merge", "refs/heads/main")
			},
			want:       "goog",
			extraCalls: []string{"git branch --show-current"},
		},
		{
			name: "ghish.gerrit.remote override wins",
			setup: func(t *testing.T, dir string) {
				runTestGit(t, dir, "remote", "add", "origin", "https://example.com/o")
				runTestGit(t, dir, "config", "branch.main.remote", "origin")
				runTestGit(t, dir, "config", "ghish.gerrit.remote", "aosp")
			},
			want: "aosp",
		},
		{
			name: "detached HEAD uses remote list",
			setup: func(t *testing.T, dir string) {
				runTestGit(t, dir, "remote", "add", "partner", "https://example.com/p")
				runTestGit(t, dir, "checkout", "-q", "--detach")
			},
			want:       "partner",
			extraCalls: []string{"git branch --show-current"},
		},
		{
			name: "legacy remotes file still consults git remote",
			setup: func(t *testing.T, dir string) {
				gitDir := filepath.Join(dir, ".git", "remotes")
				if err := os.MkdirAll(gitDir, 0o755); err != nil {
					t.Fatal(err)
				}
				if err := os.WriteFile(filepath.Join(gitDir, "legacy"), []byte("URL: https://example.com/l\n"), 0o644); err != nil {
					t.Fatal(err)
				}
			},
			// Whatever `git remote` reports: newer git builds may no
			// longer read legacy remotes files at all.
			want:       "<git remote>",
			extraCalls: []string{"git branch --show-current", "git remote"},
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			isolateGitEnv(t)
			dir := initTestRepo(t)
			if tc.setup != nil {
				tc.setup(t, dir)
			}
			want := tc.want
			if want == "<git remote>" {
				want = "origin"
				if r := runTestGit(t, dir, "remote"); r != "" {
					want = strings.Fields(r)[0]
				}
			}
			client := NewGitClientInDir(dir)
			var got string
			calls := recordGitCalls(t, func() { got = client.ResolveRemote(t.Context()) })
			if got != want {
				t.Errorf("ResolveRemote() = %q, want %q (calls: %q)", got, want, calls)
			}
			wantCalls := append([]string{prefetchCall}, tc.extraCalls...)
			slices.Sort(wantCalls)
			slices.Sort(calls)
			if diff := cmp.Diff(wantCalls, calls); diff != "" {
				t.Errorf("ResolveRemote() subprocesses mismatch (-want +got):\n%s", diff)
			}
		})
	}
}

// TestResolveDefaultBranch_RealRepo checks origin/HEAD symref resolution
// and the single for-each-ref candidate probe against real repositories,
// including dangling symrefs (which `git symbolic-ref` still reports),
// and linked worktrees.
func TestResolveDefaultBranch_RealRepo(t *testing.T) {
	addOriginWithHead := func(t *testing.T, dir, target string, createTarget bool) {
		t.Helper()
		runTestGit(t, dir, "remote", "add", "origin", "https://example.com/o")
		if createTarget {
			runTestGit(t, dir, "update-ref", "refs/remotes/origin/"+target, "HEAD")
		}
		runTestGit(t, dir, "symbolic-ref", "refs/remotes/origin/HEAD", "refs/remotes/origin/"+target)
	}
	for _, tc := range []struct {
		name   string
		branch string
		setup  func(t *testing.T, dir string) string
		want   string
	}{
		{
			name: "origin/HEAD symref",
			setup: func(t *testing.T, dir string) string {
				addOriginWithHead(t, dir, "dev", true)
				return dir
			},
			want: "dev",
		},
		{
			name: "dangling origin/HEAD symref matches symbolic-ref",
			setup: func(t *testing.T, dir string) string {
				addOriginWithHead(t, dir, "gone", false)
				return dir
			},
			want: "gone",
		},
		{
			name: "origin/HEAD read from linked worktree common dir",
			setup: func(t *testing.T, dir string) string {
				addOriginWithHead(t, dir, "dev", true)
				wt := filepath.Join(filepath.Dir(dir), "wt")
				runTestGit(t, dir, "worktree", "add", "-q", "-b", "wtb", wt)
				return wt
			},
			want: "dev",
		},
		{
			name: "local main without origin/HEAD",
			setup: func(t *testing.T, dir string) string {
				runTestGit(t, dir, "remote", "add", "origin", "https://example.com/o")
				return dir
			},
			want: "main",
		},
		// inclusive-language: disable
		{
			name: "remote master only",
			setup: func(t *testing.T, dir string) string {
				runTestGit(t, dir, "remote", "add", "origin", "https://example.com/o")
				runTestGit(t, dir, "update-ref", "refs/remotes/origin/master", "HEAD")
				runTestGit(t, dir, "checkout", "-q", "-b", "topic")
				runTestGit(t, dir, "branch", "-D", "main")
				return dir
			},
			want: "master",
		},
		// inclusive-language: enable
		{
			name: "no candidates falls back to main",
			setup: func(t *testing.T, dir string) string {
				runTestGit(t, dir, "checkout", "-q", "-b", "topic")
				runTestGit(t, dir, "branch", "-D", "main")
				return dir
			},
			want: "main",
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			isolateGitEnv(t)
			dir := tc.setup(t, initTestRepo(t))
			client := NewGitClientInDir(dir)
			var got string
			calls := recordGitCalls(t, func() { got = client.ResolveDefaultBranch(t.Context()) })
			if got != tc.want {
				t.Errorf("ResolveDefaultBranch() = %q, want %q (calls: %q)", got, tc.want, calls)
			}
			if n := len(slices.DeleteFunc(slices.Clone(calls), func(c string) bool { return !strings.Contains(c, "rev-parse --verify") })); n != 0 {
				t.Errorf("ResolveDefaultBranch() spawned %d rev-parse --verify probes; want one for-each-ref (calls: %q)", n, calls)
			}
		})
	}
}

// TestGitDir_OnDiskMatchesRevParse checks that GitDir agrees with
// `git rev-parse --git-dir` and memoizes the result across repeated calls.
func TestGitDir_OnDiskMatchesRevParse(t *testing.T) {
	isolateGitEnv(t)
	dir := initTestRepo(t)
	sub := filepath.Join(dir, "a", "b")
	if err := os.MkdirAll(sub, 0o755); err != nil {
		t.Fatal(err)
	}
	wt := filepath.Join(filepath.Dir(dir), "wt")
	runTestGit(t, dir, "worktree", "add", "-q", "-b", "wtb", wt)

	for _, start := range []string{dir, sub, wt} {
		want := runTestGit(t, start, "rev-parse", "--git-dir")
		if !filepath.IsAbs(want) {
			want = filepath.Join(start, want)
		}
		client := NewGitClientInDir(start)
		var got string
		var err error
		calls := recordGitCalls(t, func() {
			got, err = client.GitDir(t.Context())
			if err == nil {
				_, _ = client.GitDir(t.Context())
			}
		})
		if err != nil {
			t.Fatalf("GitDir(%s) failed: %v", start, err)
		}
		if got != want {
			t.Errorf("GitDir(%s) = %q, want %q", start, got, want)
		}
		if diff := cmp.Diff([]string{"git rev-parse --git-dir"}, calls); diff != "" {
			t.Errorf("GitDir(%s) subprocesses mismatch (-want +got):\n%s", start, diff)
		}
	}
}

// TestConfigPrefetch_SharedAcrossProfileAndProjectConfig checks that a config
// lookup issued before LoadProjectConfig (as GetProfile does) triggers the
// single shared prefetch, which LoadProjectConfig then reuses.
func TestConfigPrefetch_SharedAcrossProfileAndProjectConfig(t *testing.T) {
	isolateGitEnv(t)
	dir := initTestRepo(t)
	runTestGit(t, dir, "config", "--unset", "user.email")
	runTestGit(t, dir, "remote", "add", "origin", "https://example.com/o")
	runTestGit(t, dir, "config", "ghish.gerrit.defaultbranch", "trunk")
	cfg := &Config{Git: &RealGitRunner{Dir: dir}, CWD: dir}

	var projCfg *ProjectConfig
	var err error
	calls := recordGitCalls(t, func() {
		_ = cfg.GetProfile(t.Context())
		if _, uErr := cfg.GitClient().UserEmail(t.Context()); uErr == nil {
			t.Errorf("UserEmail() unexpectedly succeeded in an isolated repo without user.email")
		}
		projCfg, err = cfg.LoadProjectConfig(t.Context())
	})
	if err != nil {
		t.Fatalf("LoadProjectConfig() failed: %v", err)
	}
	if got := projCfg.Gerrit.DefaultBranch; got != "trunk" {
		t.Errorf("ProjectConfig.Gerrit.DefaultBranch = %q, want %q from the shared prefetch", got, "trunk")
	}
	want := []string{prefetchCall, "git branch --show-current"}
	if diff := cmp.Diff(want, calls); diff != "" {
		t.Errorf("subprocesses mismatch (-want +got):\n%s", diff)
	}
}

func TestParseGitConfigNUL(t *testing.T) {
	raw := "ghish.gerrit.remote\ngoog\x00" +
		"ghish.issue.note\nline one\nghish.gerrit.host evil.example.com\x00" +
		"http.sslverify\x00" +
		"branch.Foo.remote\norigin\x00"
	want := []gitConfigEntry{
		{key: "ghish.gerrit.remote", val: "goog"},
		// A newline inside a value must not inject a fake key.
		{key: "ghish.issue.note", val: "line one\nghish.gerrit.host evil.example.com"},
		// Valueless keys have no newline separator.
		{key: "http.sslverify", val: ""},
		{key: "branch.Foo.remote", val: "origin"},
	}
	if diff := cmp.Diff(want, parseGitConfigNUL(raw), cmp.AllowUnexported(gitConfigEntry{})); diff != "" {
		t.Errorf("parseGitConfigNUL() mismatch (-want +got):\n%s", diff)
	}
	cfg := DefaultProjectConfig()
	if err := applyGitConfigEntries(parseGitConfigNUL(raw), cfg); err != nil {
		t.Fatal(err)
	}
	if cfg.Gerrit.Host != "" || cfg.Gerrit.Remote != "goog" {
		t.Errorf("applyGitConfigEntries: Gerrit.Host=%q Remote=%q, want no injected host and remote goog", cfg.Gerrit.Host, cfg.Gerrit.Remote)
	}
}

func TestCanonicalGitConfigKey(t *testing.T) {
	for in, want := range map[string]string{
		"User.Email":               "user.email",
		"branch.Foo.Remote":        "branch.Foo.remote",
		"BRANCH.foo.REMOTE":        "branch.foo.remote",
		"remote.My.Remote.URL":     "remote.My.Remote.url",
		"init.defaultBranch":       "init.defaultbranch",
		"ghish.gerrit.remote":      "ghish.gerrit.remote",
		"extensions.refStorage":    "extensions.refstorage",
		"url.https://X/.insteadOf": "url.https://X/.insteadof",
	} {
		if got := canonicalGitConfigKey(in); got != want {
			t.Errorf("canonicalGitConfigKey(%q) = %q, want %q", in, got, want)
		}
	}
}

// TestConfigPrefetch_SubsectionsAreCaseSensitive checks that prefetched
// lookups follow git's rules: section/variable names are case-insensitive,
// subsections (branch names) are not.
func TestConfigPrefetch_SubsectionsAreCaseSensitive(t *testing.T) {
	isolateGitEnv(t)
	dir := initTestRepo(t)
	runTestGit(t, dir, "config", "branch.Foo.remote", "upper")
	runTestGit(t, dir, "config", "branch.foo.remote", "lower")
	client := NewGitClientInDir(dir)
	for key, want := range map[string]string{
		"branch.Foo.remote": "upper",
		"branch.foo.remote": "lower",
		"BRANCH.Foo.REMOTE": "upper",
	} {
		got, err := client.ConfigGet(t.Context(), key)
		if err != nil || got != want {
			t.Errorf("ConfigGet(%q) = (%q, %v), want %q", key, got, err, want)
		}
		if gitGot := runTestGit(t, dir, "config", "--get", key); gitGot != want {
			t.Errorf("git config --get %s = %q, want %q (test premise)", key, gitGot, want)
		}
	}
	if got, err := client.ConfigGet(t.Context(), "branch.FOO.remote"); err == nil {
		t.Errorf("ConfigGet(branch.FOO.remote) = %q, want not found (subsection is case-sensitive)", got)
	}
}

// TestLoadProjectConfig_RetriesAfterFailedLazyPrefetch checks that a failed
// lazy prefetch does not make LoadProjectConfig drop ghish.* overrides.
func TestLoadProjectConfig_RetriesAfterFailedLazyPrefetch(t *testing.T) {
	isolateGitEnv(t)
	dir := initTestRepo(t)
	runTestGit(t, dir, "config", "ghish.gerrit.defaultbranch", "trunk")
	dc := NewGitClientInDir(dir).(*defaultGitClient)
	// Simulate an earlier lazy prefetch that failed.
	dc.mu.Lock()
	dc.prefetchAttempted = true
	dc.mu.Unlock()

	projCfg, err := LoadProjectConfig(t.Context(), dc, dir)
	if err != nil {
		t.Fatalf("LoadProjectConfig() failed: %v", err)
	}
	if got := projCfg.Gerrit.DefaultBranch; got != "trunk" {
		t.Errorf("Gerrit.DefaultBranch = %q, want %q after retrying the prefetch", got, "trunk")
	}
	if got, err := dc.ConfigGet(t.Context(), "ghish.gerrit.defaultbranch"); err != nil || got != "trunk" {
		t.Errorf("ConfigGet after retry = (%q, %v), want trunk from the seeded cache", got, err)
	}
}
