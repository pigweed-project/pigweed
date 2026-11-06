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
	"bytes"
	"cmp"
	"context"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"

	gocmp "github.com/google/go-cmp/cmp"
)

func TestPreferredCorpGitSchemes(t *testing.T) {
	for _, tc := range []struct {
		name    string
		goos    string
		lsb     string
		hasLSB  bool
		hasRPC  bool
		hasSSO  bool
		ssoPath string
		want    []string
	}{
		{name: "LOAS Linux with rpc+sso", goos: "linux", lsb: "GOOGLE_ROLE=workstation\n", hasLSB: true, hasRPC: true, hasSSO: true, want: []string{"rpc", "sso"}},
		{name: "GOOGLE_ROLE=laptop skips rpc", goos: "linux", lsb: "GOOGLE_ROLE=laptop\n", hasLSB: true, hasRPC: true, hasSSO: true, want: []string{"sso"}},
		{name: "darwin skips rpc", goos: "darwin", hasRPC: true, hasSSO: true, want: []string{"sso"}},
		{name: "darwin git-google fallback path", goos: "darwin", ssoPath: "/usr/local/git/git-google/bin/git-remote-sso", want: []string{"sso"}},
		{name: "LOAS Linux rpc only", goos: "linux", hasRPC: true, want: []string{"rpc"}},
		{name: "no helpers", goos: "linux", want: nil},
	} {
		t.Run(tc.name, func(t *testing.T) {
			withHostRoleForTest(t, tc.goos, tc.lsb, tc.hasLSB)
			WithHermeticAuth(t, AuthResolvers{
				LookPath: func(file string) (string, error) {
					if (tc.hasRPC && file == "git-remote-rpc") || (tc.hasSSO && file == "git-remote-sso") {
						return "/usr/bin/" + file, nil
					}
					if tc.ssoPath != "" && file == tc.ssoPath {
						return tc.ssoPath, nil
					}
					return "", fmt.Errorf("not found: %s", file)
				},
			})
			if diff := gocmp.Diff(tc.want, preferredCorpGitSchemes()); diff != "" {
				t.Errorf("preferredCorpGitSchemes() mismatch (-want +got):\n%s", diff)
			}
		})
	}
}

func TestParseAndResolveGerritGitRemote(t *testing.T) {
	for _, tc := range []struct {
		raw    string
		wantOK bool
		want   gerritGitRemote
	}{
		{raw: "sso://pigweed/pigweed/pigweed", wantOK: true, want: gerritGitRemote{scheme: "sso", tenant: "pigweed", project: "pigweed/pigweed", canonHost: "pigweed-review.googlesource.com"}},
		{raw: "rpc://example-internal/team/repo.git", wantOK: true, want: gerritGitRemote{scheme: "rpc", tenant: "example-internal", project: "team/repo", canonHost: "example-internal-review.googlesource.com"}},
		{raw: "https://pigweed-review.googlesource.com/a/pigweed/pigweed", wantOK: true, want: gerritGitRemote{scheme: "https", tenant: "pigweed", project: "pigweed/pigweed", canonHost: "pigweed-review.googlesource.com"}},
		{raw: "https://github.com/org/repo.git", wantOK: false},
		{raw: "https://googlesource.com", wantOK: false},
		{raw: "https://googlesource.com/pigweed/pigweed", wantOK: false},
	} {
		if got, ok := parseGerritGitRemote(tc.raw); ok != tc.wantOK || (ok && got != tc.want) {
			t.Errorf("parseGerritGitRemote(%q) = (%+v, %v), want (%+v, %v)", tc.raw, got, ok, tc.want, tc.wantOK)
		}
	}

	mock := (&MockGitRunner{}).
		OnCommand("config --get remote.origin.url", "https://pigweed.googlesource.com/pigweed/pigweed\n").
		OnCommand("config --get remote.origin.pushurl", "sso://pigweed/\n")
	gc := NewGitClient(mock)
	if pushInfo, ok := resolveGerritGitRemote(t.Context(), gc, nil, "origin", true); !ok || pushInfo.scheme != "sso" || pushInfo.project != "pigweed/pigweed" {
		t.Fatalf("forPush=true got (%+v, %v)", pushInfo, ok)
	}
	if fetchInfo, ok := resolveGerritGitRemote(t.Context(), gc, nil, "origin", false); !ok || fetchInfo.scheme != "https" || fetchInfo.project != "pigweed/pigweed" {
		t.Fatalf("forPush=false got (%+v, %v)", fetchInfo, ok)
	}
}

func TestClassifyGitTransportAuthError(t *testing.T) {
	for _, tc := range []struct {
		stderr     string
		hint       string
		wantScheme string
		wantAuth   bool
	}{
		{"sso: credentials expired. Try running gcert first", "", "sso", true},
		{"git-remote-rpc: LOAS credentials expired", "", "rpc", true},
		{"fatal: remote helper 'rpc' aborted session", "", "rpc", true},
		{"Try running gcert", "rpc", "rpc", true},
		{"Try running gcert", "", "sso", true},
		{"fatal: could not read Username for 'https://pigweed.googlesource.com': terminal prompts disabled", "", "https", true},
		{"remote: HTTP Basic: Access denied", "", "https", true},
		{"The requested URL returned error: 401", "", "https", true},
		{"PERMISSION_DENIED_BY_IP_RESTRICTIONS", "", "https", true},
		{"Access must be through example-internal" + gobCorpDomainSuffix, "", "https", true},
		{" ! [remote rejected] HEAD -> refs/for/main (prohibited by Gerrit)", "", "", false},
		{"remote: ERROR: missing Change-Id in message footer", "", "", false},
		{"fatal: couldn't find remote ref refs/changes/99/999/1", "", "", false},
	} {
		if gotScheme, gotAuth := classifyGitTransportAuthError(tc.stderr, tc.hint); gotScheme != tc.wantScheme || gotAuth != tc.wantAuth {
			t.Errorf("classifyGitTransportAuthError(%q, %q) = (%q, %v), want (%q, %v)", tc.stderr, tc.hint, gotScheme, gotAuth, tc.wantScheme, tc.wantAuth)
		}
	}
}

func TestRemoteSchemeOverrideGitConfig_BeatsGlobalInsteadOf(t *testing.T) {
	ctx := t.Context()
	tmp := t.TempDir()
	repoDir := filepath.Join(tmp, "repo")
	if err := os.MkdirAll(repoDir, 0o755); err != nil {
		t.Fatal(err)
	}
	globalCfg := filepath.Join(tmp, "global.gitconfig")
	globalContent := "[url \"sso://pigweed/\"]\n\tinsteadOf = https://pigweed.googlesource.com/\n\tinsteadOf = rpc://pigweed/\n\tpushInsteadOf = https://pigweed.googlesource.com/\n\tpushInsteadOf = https://pigweed-review.googlesource.com/\n\tpushInsteadOf = rpc://pigweed/\n"
	if err := os.WriteFile(globalCfg, []byte(globalContent), 0o600); err != nil {
		t.Fatal(err)
	}
	runGit := func(extraEnv []string, args ...string) string {
		t.Helper()
		cmd := ExecCommandContext(ctx, "git", args...)
		cmd.Dir = repoDir
		cmd.Env = slices.Concat(ScrubRepoLocalGitEnv(os.Environ()), []string{"GIT_CONFIG_GLOBAL=" + globalCfg, "GIT_CONFIG_NOSYSTEM=1"}, extraEnv)
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("git %v: %v\n%s", args, err, out)
		}
		return strings.TrimSpace(string(out))
	}
	runGit(nil, "init", "-q")
	runGit(nil, "remote", "add", "origin", "https://pigweed.googlesource.com/pigweed/pigweed")
	runGit(nil, "remote", "set-url", "--add", "--push", "origin", "sso://pigweed/pigweed/pigweed")

	repoCfgPath := filepath.Join(repoDir, ".git", "config")
	beforeCfg, err := os.ReadFile(repoCfgPath)
	if err != nil {
		t.Fatal(err)
	}
	for _, tc := range []struct{ scheme, wantURL string }{
		{"rpc", "rpc://pigweed/pigweed/pigweed"},
		{"sso", "sso://pigweed/pigweed/pigweed"},
		{"https", "https://pigweed.googlesource.com/pigweed/pigweed"},
	} {
		env := gitEnvFromContext(withGitConfigEnv(ctx, remoteSchemeOverrideGitConfig("origin", tc.scheme, "pigweed", "pigweed/pigweed")...))
		if got := runGit(env, "remote", "get-url", "origin"); got != tc.wantURL {
			t.Errorf("%s get-url = %q, want %q", tc.scheme, got, tc.wantURL)
		}
		if got := runGit(env, "remote", "get-url", "--push", "origin"); got != tc.wantURL {
			t.Errorf("%s get-url --push = %q, want %q", tc.scheme, got, tc.wantURL)
		}
	}
	if afterCfg, err := os.ReadFile(repoCfgPath); err != nil || !bytes.Equal(beforeCfg, afterCfg) {
		t.Errorf(".git/config mutated: err=%v", err)
	}
}

func TestRunGitWithAuthFallback(t *testing.T) {
	const (
		ssoExp   = "sso: credentials expired. Try running gcert first\nfatal: remote helper 'sso' aborted session\n"
		rpcExp   = "git-remote-rpc: LOAS credentials expired; run gcert\nfatal: remote helper 'rpc' aborted session\n"
		httpsPrm = "fatal: could not read Username for 'https://pigweed.googlesource.com': terminal prompts disabled\n"
		ipDenied = "fatal: The requested URL returned error: 403 PERMISSION_DENIED_BY_IP_RESTRICTIONS\n"
	)
	schemeFromEnv := func(env []string) string {
		for _, kv := range env {
			switch {
			case strings.Contains(kv, "url.rpc://"):
				return "rpc"
			case strings.Contains(kv, "url.sso://"):
				return "sso"
			case strings.Contains(kv, "Authorization: Bearer "):
				return "https-bearer"
			}
		}
		return "initial"
	}

	for _, tc := range []struct {
		name             string
		authMode         AuthMode
		gerritToken      string
		laptop           bool
		hasRPC, hasSSO   bool
		hasSSOCred       bool
		tokenErr         bool
		preseedCorp      string
		remote           string
		remoteURL        string
		stderrs          []string
		wantSchemes      []string
		wantCorpAfter    string
		wantNote         string
		wantErr          string
		wantForbiddenErr []string
		wantExitCode     int
	}{
		{name: "happy path", hasRPC: true, hasSSO: true, remote: "origin", remoteURL: "sso://pigweed/pigweed/pigweed", stderrs: []string{""}, wantSchemes: []string{"initial"}},
		{name: "terminal push rejection", hasRPC: true, hasSSO: true, remote: "origin", remoteURL: "sso://pigweed/pigweed/pigweed", stderrs: []string{" ! [remote rejected] HEAD -> refs/for/main (prohibited by Gerrit)\n"}, wantSchemes: []string{"initial"}, wantErr: "exit status 128"},
		{name: "empty stderr on fallback stops without auth error", hasRPC: true, hasSSO: true, remote: "origin", remoteURL: "sso://pigweed/pigweed/pigweed", stderrs: []string{ssoExp, "<killed>"}, wantSchemes: []string{"initial", "rpc"}, wantErr: "signal: killed"},
		{name: "LOAS https->rpc->sso->https", hasRPC: true, hasSSO: true, remote: "origin", remoteURL: "https://pigweed.googlesource.com/pigweed/pigweed", stderrs: []string{httpsPrm, rpcExp, ssoExp, ""}, wantSchemes: []string{"initial", "rpc", "sso", "https-bearer"}},
		{name: "laptop skips rpc", laptop: true, hasRPC: true, hasSSO: true, remote: "origin", remoteURL: "https://pigweed.googlesource.com/pigweed/pigweed", stderrs: []string{httpsPrm, ssoExp, ""}, wantSchemes: []string{"initial", "sso", "https-bearer"}},
		{name: "warm corp-only skips https", hasRPC: true, hasSSO: true, preseedCorp: "example-internal-review.googlesource.com", remote: "origin", remoteURL: "sso://example-internal/team/repo", stderrs: []string{ssoExp, rpcExp}, wantSchemes: []string{"initial", "rpc"}, wantErr: "gcert", wantForbiddenErr: []string{"GERRIT_TOKEN", "luci-auth login"}, wantExitCode: ExitCodeAuth},
		{name: "cold IP restriction marks corp-only and uses rpc", hasRPC: true, hasSSO: true, remote: "origin", remoteURL: "https://example-internal.googlesource.com/team/repo", stderrs: []string{ipDenied, ""}, wantSchemes: []string{"initial", "rpc"}, wantCorpAfter: "example-internal-review.googlesource.com", wantNote: "rpc://example-internal/ using LOAS2"},
		{name: "cold IP restriction on https fallback marks corp-only", laptop: true, hasRPC: true, hasSSO: true, remote: "origin", remoteURL: "sso://example-internal/team/repo", stderrs: []string{ssoExp, ipDenied}, wantSchemes: []string{"initial", "https-bearer"}, wantCorpAfter: "example-internal-review.googlesource.com", wantErr: "gcert", wantForbiddenErr: []string{"GERRIT_TOKEN", "luci-auth login"}, wantExitCode: ExitCodeAuth},
		{name: "direct URL remote", hasRPC: true, remote: "sso://pigweed/pigweed/pigweed", stderrs: []string{ssoExp, ""}, wantSchemes: []string{"initial", "rpc"}, wantNote: "rpc://pigweed/"},
		{name: "auth-mode none", authMode: AuthModeNone, hasRPC: true, hasSSO: true, preseedCorp: "example-internal-review.googlesource.com", remote: "origin", remoteURL: "https://example-internal.googlesource.com/team/repo", stderrs: []string{httpsPrm}, wantSchemes: []string{"initial"}, wantErr: "auth mode none", wantForbiddenErr: []string{"gcert"}, wantExitCode: ExitCodeAuth},
		{name: "community IP restriction", authMode: AuthModeCommunity, hasRPC: true, hasSSO: true, remote: "origin", remoteURL: "https://example-internal.googlesource.com/team/repo", stderrs: []string{httpsPrm, ipDenied}, wantSchemes: []string{"initial", "https-bearer"}, wantErr: "restrictions", wantForbiddenErr: []string{"gcert", "which was also rejected"}, wantExitCode: ExitCodeAuth},
		{name: "explicit GERRIT_TOKEN bypasses corp-only", authMode: AuthModeGoogler, gerritToken: "bot-tok", hasRPC: true, hasSSO: true, preseedCorp: "example-internal-review.googlesource.com", remote: "origin", remoteURL: "sso://example-internal/team/repo", stderrs: []string{ssoExp, ""}, wantSchemes: []string{"initial", "https-bearer"}, wantNote: "token (GERRIT_TOKEN)"},
		{name: "explicit GERRIT_TOKEN IP restriction advises unset", authMode: AuthModeGoogler, gerritToken: "bot-tok", hasRPC: true, hasSSO: true, remote: "origin", remoteURL: "https://example-internal.googlesource.com/team/repo", stderrs: []string{ipDenied}, wantSchemes: []string{"initial"}, wantErr: "unset GERRIT_TOKEN", wantExitCode: ExitCodeAuth},
		{name: "LOAS exhaustion prescribes gcert", hasRPC: true, hasSSO: true, hasSSOCred: true, tokenErr: true, remote: "origin", remoteURL: "sso://pigweed/pigweed/pigweed", stderrs: []string{ssoExp, rpcExp}, wantSchemes: []string{"initial", "rpc"}, wantErr: "gcert", wantForbiddenErr: []string{"luci-auth login"}, wantExitCode: ExitCodeAuth},
		{name: "laptop exhaustion prescribes luci-auth", laptop: true, hasSSO: true, tokenErr: true, remote: "origin", remoteURL: "sso://pigweed/pigweed/pigweed", stderrs: []string{ssoExp}, wantSchemes: []string{"initial"}, wantErr: "luci-auth login", wantExitCode: ExitCodeAuth},
	} {
		t.Run(tc.name, func(t *testing.T) {
			WithHermeticAuth(t, AuthResolvers{
				LookPath: func(file string) (string, error) {
					if (tc.hasRPC && file == "git-remote-rpc") || (tc.hasSSO && file == "git-remote-sso") || (tc.hasSSOCred && file == ssoCredHelperPath) {
						return file, nil
					}
					return "", fmt.Errorf("not found: %s", file)
				},
				GerritToken: func(context.Context) (string, string, error) {
					if tc.tokenErr {
						return "", "", fmt.Errorf("luci-auth: not logged in")
					}
					return "luci-tok", "luci-auth", nil
				},
			})
			WithTestCaches(t, TestCacheConfig{PersistentDir: t.TempDir()})
			setTestAuthMode(t, cmp.Or(tc.authMode, AuthModeGoogler))
			if tc.laptop {
				withHostRoleForTest(t, "linux", "GOOGLE_ROLE=laptop\n", true)
			} else {
				withHostRoleForTest(t, "linux", "", false)
			}
			if tc.preseedCorp != "" {
				defaultHostCache.MarkCorpOnly(tc.preseedCorp)
			}
			if tc.gerritToken != "" {
				t.Setenv("GERRIT_TOKEN", tc.gerritToken)
			}

			args := []string{"push", tc.remote, "HEAD:refs/for/main"}
			var gotSchemes []string
			mock := &MockGitRunner{
				RunFn: func(runCtx context.Context, stdout, stderr io.Writer, runArgs ...string) error {
					call := strings.Join(runArgs, " ")
					if strings.HasPrefix(call, "config --get ") && strings.HasSuffix(call, ".url") && tc.remoteURL != "" {
						_, _ = io.WriteString(stdout, tc.remoteURL+"\n")
						return nil
					}
					if len(runArgs) > 0 && runArgs[0] == "push" {
						env := gitEnvFromContext(runCtx)
						if !slices.Contains(env, "GIT_TERMINAL_PROMPT=0") {
							t.Errorf("missing GIT_TERMINAL_PROMPT=0: %v", env)
						}
						gotSchemes = append(gotSchemes, schemeFromEnv(env))
						if idx := len(gotSchemes) - 1; idx < len(tc.stderrs) && tc.stderrs[idx] != "" {
							if tc.stderrs[idx] == "<killed>" {
								return fmt.Errorf("signal: killed")
							}
							_, _ = io.WriteString(stderr, tc.stderrs[idx])
							return fmt.Errorf("exit status 128")
						}
					}
					return nil
				},
			}
			var noteBuf bytes.Buffer
			_, _, err := runGitWithAuthFallback(t.Context(), NewGitClient(mock), &Config{Git: mock, Remote: tc.remote}, tc.remote, args, &noteBuf)

			if tc.wantErr == "" && err != nil {
				t.Fatalf("unexpected error: %v", err)
			}
			if tc.wantErr != "" {
				if err == nil || !strings.Contains(err.Error(), tc.wantErr) {
					t.Fatalf("err = %v, want %q", err, tc.wantErr)
				}
				for _, f := range tc.wantForbiddenErr {
					if strings.Contains(err.Error(), f) {
						t.Errorf("err prescribed %q:\n%v", f, err)
					}
				}
			}
			if tc.wantExitCode != 0 && ExitCodeFor(err) != tc.wantExitCode {
				t.Errorf("ExitCode = %d, want %d", ExitCodeFor(err), tc.wantExitCode)
			} else if tc.wantExitCode == 0 && tc.wantErr != "" && ExitCodeFor(err) == ExitCodeAuth {
				t.Errorf("unexpected ExitCodeAuth for non-auth error: %v", err)
			}
			if diff := gocmp.Diff(tc.wantSchemes, gotSchemes); diff != "" {
				t.Errorf("schemes (-want +got):\n%s", diff)
			}
			if tc.wantCorpAfter != "" && !defaultHostCache.IsCorpOnly(tc.wantCorpAfter) {
				t.Errorf("IsCorpOnly(%q) = false, want true", tc.wantCorpAfter)
			}
			if tc.wantNote != "" && !strings.Contains(noteBuf.String(), tc.wantNote) {
				t.Errorf("note = %q, want %q", noteBuf.String(), tc.wantNote)
			}
		})
	}
}
