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
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"slices"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

const (
	testLUCIAuthBin     = "/usr/bin/luci-auth"
	wantGooglerScopes   = "https://www.googleapis.com/auth/gerritcodereview https://www.googleapis.com/auth/userinfo.email https://www.googleapis.com/auth/buganizer https://www.googleapis.com/auth/androidbuild.internal"
	wantCommunityScopes = "https://www.googleapis.com/auth/gerritcodereview https://www.googleapis.com/auth/userinfo.email"
)

func luciAuthFoundLookPath(file string) (string, error) {
	if file == "luci-auth" {
		return testLUCIAuthBin, nil
	}
	return "", os.ErrNotExist
}

func luciAuthJSON(tok string, expiry time.Time) []byte {
	return fmt.Appendf(nil, `{"token":%q,"expiry":%d}`, tok, expiry.Unix())
}

func withLUCIAuthTokenResolvers(t *testing.T) {
	t.Helper()
	prevLUCI, prevAB := LUCITokenResolver, AndroidBuildTokenResolver
	LUCITokenResolver, AndroidBuildTokenResolver = DefaultLUCIToken, DefaultAndroidBuildToken
	t.Cleanup(func() { LUCITokenResolver, AndroidBuildTokenResolver = prevLUCI, prevAB })
}

func serviceResolvers() map[string]func(context.Context) (string, string, error) {
	return map[string]func(context.Context) (string, string, error){
		"gerrit":        GerritTokenResolver,
		"luci":          LUCITokenResolver,
		"issue-tracker": IssueTrackerTokenResolver,
		"android-build": func(ctx context.Context) (string, string, error) {
			tok, err := AndroidBuildTokenResolver(ctx)
			return tok, "", err
		},
	}
}

func allServiceTokens(ctx context.Context) map[string]string {
	got := map[string]string{}
	for svc, fn := range serviceResolvers() {
		if tok, _, err := fn(ctx); err == nil {
			got[svc] = tok
		}
	}
	return got
}

func TestServiceTokens_EnvOverrides(t *testing.T) {
	for _, tc := range []struct{ env, service, wantSrc string }{
		{"GERRIT_TOKEN", "gerrit", "GERRIT_TOKEN"},
		{"GHISH_LUCI_TOKEN", "luci", "GHISH_LUCI_TOKEN"},
		{"LUCI_TOKEN", "luci", "LUCI_TOKEN"},
		{"GHISH_ANDROID_BUILD_TOKEN", "android-build", ""},
		{"GHISH_ISSUE_TOKEN", "issue-tracker", "GHISH_ISSUE_TOKEN"},
		{"BUGANIZER_TOKEN", "issue-tracker", "BUGANIZER_TOKEN"},
	} {
		t.Run(tc.env, func(t *testing.T) {
			var runnerCalls atomic.Int32
			WithHermeticAuth(t, AuthResolvers{
				LookPath: luciAuthFoundLookPath,
				CommandRunner: func(context.Context, string, ...string) ([]byte, error) {
					runnerCalls.Add(1)
					return luciAuthJSON("shared", time.Now().Add(time.Hour)), nil
				},
			})
			withLUCIAuthTokenResolvers(t)
			t.Setenv(tc.env, "env-token")
			ctx := t.Context()

			tok, src, err := serviceResolvers()[tc.service](ctx)
			if err != nil || tok != "env-token" || src != tc.wantSrc || runnerCalls.Load() != 0 {
				t.Errorf("%s = (%q, %q, %v), runs=%d; want (\"env-token\", %q, nil), runs=0", tc.service, tok, src, err, runnerCalls.Load(), tc.wantSrc)
			}
			for svc, got := range allServiceTokens(ctx) {
				if svc != tc.service && got != "shared" {
					t.Errorf("%s token = %q, want shared", svc, got)
				}
			}
			if n := runnerCalls.Load(); n != 1 {
				t.Errorf("luci-auth runs for other services = %d, want 1", n)
			}
		})
	}
}

func TestLUCIAuthToken_NeverRunsGcloud(t *testing.T) {
	var runnerCalls atomic.Int32
	WithHermeticAuth(t, AuthResolvers{
		LookPath: func(file string) (string, error) {
			if file == "gcloud" {
				return "/usr/bin/gcloud", nil
			}
			return "", os.ErrNotExist
		},
		CommandRunner: func(context.Context, string, ...string) ([]byte, error) {
			runnerCalls.Add(1)
			return []byte("gcloud-token"), nil
		},
	})
	withLUCIAuthTokenResolvers(t)
	for _, k := range []string{"PW_ENVIRONMENT_ROOT", "BUILD_WORKSPACE_DIRECTORY", "PW_GH_SCRIPT_DIR"} {
		t.Setenv(k, "")
	}
	t.Chdir(t.TempDir())
	for svc, fn := range serviceResolvers() {
		if _, _, err := fn(t.Context()); err == nil || strings.Contains(err.Error(), "gcloud") {
			t.Errorf("%s token error = %v, want non-nil error without gcloud", svc, err)
		}
	}
	if n := runnerCalls.Load(); n != 0 {
		t.Errorf("token command runs = %d, want 0", n)
	}
}

func TestLUCIAuthToken_CacheLifecycle(t *testing.T) {
	t.Run("does not cache cancellation", func(t *testing.T) {
		var cancelMidFlight context.CancelFunc
		var cancelAfterSuccess bool
		var runnerCalls int
		WithHermeticAuth(t, AuthResolvers{
			LookPath: luciAuthFoundLookPath,
			CommandRunner: func(ctx context.Context, _ string, _ ...string) ([]byte, error) {
				runnerCalls++
				if cancelMidFlight != nil {
					cancel := cancelMidFlight
					cancelMidFlight = nil
					cancel()
					if cancelAfterSuccess {
						cancelAfterSuccess = false
						return luciAuthJSON("stale-cancelled-tok", time.Now().Add(time.Hour)), nil
					}
					return nil, errors.New("signal: killed")
				}
				return luciAuthJSON("tok", time.Now().Add(time.Hour)), nil
			},
		})
		withLUCIAuthTokenResolvers(t)
		t.Setenv("GH_ISH_AUTH_MODE", "googler")

		// 1. Pre-cancelled context returns context.Canceled (never ExitCodeAuth).
		preCancelled, preCancel := context.WithCancel(t.Context())
		preCancel()
		if _, err := luciAuthToken(preCancelled); !errors.Is(err, context.Canceled) {
			t.Errorf("pre-cancelled luciAuthToken error = %v, want context.Canceled", err)
		}
		if _, _, err := DefaultLUCIToken(preCancelled); !errors.Is(err, context.Canceled) || ExitCodeFor(err) == ExitCodeAuth {
			t.Errorf("pre-cancelled DefaultLUCIToken error = %v (code %d), want context.Canceled without ExitCodeAuth", err, ExitCodeFor(err))
		}
		if _, _, err := ResolveIssueTrackerTokenWithSource(preCancelled); !errors.Is(err, context.Canceled) || ExitCodeFor(err) == ExitCodeAuth {
			t.Errorf("pre-cancelled ResolveIssueTrackerTokenWithSource error = %v (code %d), want context.Canceled without ExitCodeAuth", err, ExitCodeFor(err))
		}
		if runnerCalls != 0 {
			t.Errorf("runnerCalls on pre-cancelled context = %d, want 0", runnerCalls)
		}

		// 2. Mid-flight cancellation inside CommandRunner exercises mintLUCIAuthToken's
		// ctx.Err() branch and preserves context.Canceled across service resolvers.
		for _, fn := range []struct {
			name string
			call func(context.Context) error
		}{
			{"luciAuthToken", func(ctx context.Context) error { _, err := luciAuthToken(ctx); return err }},
			{"DefaultLUCIToken", func(ctx context.Context) error { _, _, err := DefaultLUCIToken(ctx); return err }},
			{"ResolveIssueTrackerTokenWithSource", func(ctx context.Context) error { _, _, err := ResolveIssueTrackerTokenWithSource(ctx); return err }},
		} {
			midCtx, cancel := context.WithCancel(t.Context())
			cancelMidFlight = cancel
			err := fn.call(midCtx)
			if !errors.Is(err, context.Canceled) || ExitCodeFor(err) == ExitCodeAuth {
				t.Errorf("mid-flight cancelled %s error = %v (code %d), want context.Canceled without ExitCodeAuth", fn.name, err, ExitCodeFor(err))
			}
		}

		// 3. Mid-flight cancellation coinciding with token output exercises
		// luciAuthTokenWithScope's `if ctx.Err() == nil` cache guard.
		raceCtx, cancelRace := context.WithCancel(t.Context())
		cancelMidFlight = cancelRace
		cancelAfterSuccess = true
		_, _ = luciAuthToken(raceCtx)
		if runnerCalls != 4 {
			t.Errorf("runnerCalls after mid-flight cancellations = %d, want 4", runnerCalls)
		}

		// 4. Subsequent call with a fresh context re-mints and returns "tok".
		if tok, err := luciAuthToken(t.Context()); err != nil || tok != "tok" {
			t.Errorf("second call = (%q, %v), want (\"tok\", nil)", tok, err)
		}
		if runnerCalls != 5 {
			t.Errorf("runnerCalls after fresh call = %d, want 5", runnerCalls)
		}
	})

	t.Run("remembers binary not found", func(t *testing.T) {
		var lookups atomic.Int32
		WithHermeticAuth(t, AuthResolvers{
			LookPath: func(string) (string, error) { lookups.Add(1); return "", os.ErrNotExist },
		})
		t.Setenv("GH_ISH_AUTH_MODE", "googler")
		_, err1 := luciAuthToken(t.Context())
		after1 := lookups.Load()
		_, err2 := luciAuthToken(t.Context())
		if !errors.Is(err1, errLUCIAuthNotFound) || !errors.Is(err2, errLUCIAuthNotFound) || after1 == 0 || lookups.Load() != after1 {
			t.Errorf("errors=(%v, %v), lookups=(%d, %d)", err1, err2, after1, lookups.Load())
		}
	})

	t.Run("remints after expiry, transient failure, and invalidation", func(t *testing.T) {
		var calls int
		WithHermeticAuth(t, AuthResolvers{
			LookPath: luciAuthFoundLookPath,
			CommandRunner: func(context.Context, string, ...string) ([]byte, error) {
				calls++
				switch calls {
				case 1:
					return luciAuthJSON("old", time.Now().Add(30*time.Second)), nil
				case 2:
					return nil, errors.New("refresh failed")
				default:
					return luciAuthJSON(fmt.Sprintf("tok%d", calls), time.Now().Add(time.Hour)), nil
				}
			},
		})
		if tok, err := luciAuthToken(t.Context()); err != nil || tok != "old" {
			t.Fatalf("call 1 = (%q, %v), want (\"old\", nil)", tok, err)
		}
		if _, err := luciAuthToken(t.Context()); err == nil {
			t.Fatal("call 2: got nil error, want refresh failure")
		}
		for _, step := range []struct{ invalidate, want string }{{"", "tok3"}, {"other", "tok3"}, {"tok3", "tok4"}, {"", "tok4"}} {
			invalidateLUCIAuthToken(step.invalidate)
			if tok, err := luciAuthToken(t.Context()); err != nil || tok != step.want {
				t.Fatalf("after invalidate(%q) = (%q, %v), want %q", step.invalidate, tok, err, step.want)
			}
		}
	})

	t.Run("mode switch remints and invalid mode never mints", func(t *testing.T) {
		var scopes []string
		WithHermeticAuth(t, AuthResolvers{
			LookPath: luciAuthFoundLookPath,
			CommandRunner: func(_ context.Context, _ string, args ...string) ([]byte, error) {
				scopes = append(scopes, args[2])
				return luciAuthJSON("tok-"+strconv.Itoa(len(scopes)), time.Now().Add(time.Hour)), nil
			},
		})
		for _, step := range []struct {
			mode    AuthMode
			wantTok string
		}{{AuthModeGoogler, "tok-1"}, {AuthModeGoogler, "tok-1"}, {AuthModeCommunity, "tok-2"}, {AuthModeGoogler, "tok-3"}} {
			t.Setenv("GH_ISH_AUTH_MODE", string(step.mode))
			if tok, err := luciAuthToken(t.Context()); err != nil || tok != step.wantTok {
				t.Errorf("luciAuthToken() in %s mode = (%q, %v), want %q", step.mode, tok, err, step.wantTok)
			}
		}
		if want := []string{wantGooglerScopes, wantCommunityScopes, wantGooglerScopes}; !slices.Equal(scopes, want) {
			t.Errorf("luci-auth -scopes = %q, want %q", scopes, want)
		}
		t.Setenv("GH_ISH_AUTH_MODE", "bogus")
		if _, err := luciAuthToken(t.Context()); ExitCodeFor(err) != ExitCodeAuth || len(scopes) != 3 {
			t.Errorf("bogus mode err = %v (runs=%d), want ExitCodeAuth and 3 runs", err, len(scopes))
		}
		t.Setenv("GH_ISH_AUTH_MODE", string(AuthModeGoogler))
		if _, err := luciAuthTokenWithScope(t.Context(), "https://www.googleapis.com/auth/unknown"); err == nil ||
			!strings.Contains(err.Error(), "does not include the https://www.googleapis.com/auth/unknown scope") ||
			strings.Contains(err.Error(), "export GH_ISH_AUTH_MODE=googler") {
			t.Errorf("unrecognized scope in googler mode err = %v, want scope error without googler hint", err)
		}
	})
}

func TestLUCIAuthToken_ConcurrentCallersMintOnce(t *testing.T) {
	release := make(chan struct{})
	started := make(chan struct{}, 1)
	var runnerCalls atomic.Int32
	WithHermeticAuth(t, AuthResolvers{
		LookPath: luciAuthFoundLookPath,
		CommandRunner: func(ctx context.Context, _ string, _ ...string) ([]byte, error) {
			runnerCalls.Add(1)
			select {
			case started <- struct{}{}:
			default:
			}
			select {
			case <-release:
				return luciAuthJSON("tok", time.Now().Add(time.Hour)), nil
			case <-ctx.Done():
				return nil, ctx.Err()
			}
		},
	})

	cancelledCtx, cancel := context.WithCancel(t.Context())
	cancel()
	if _, err := luciAuthToken(cancelledCtx); !errors.Is(err, context.Canceled) {
		t.Errorf("cancelled caller error = %v, want context.Canceled", err)
	}

	const waiters = 8
	var wg sync.WaitGroup
	toks := make([]string, waiters)
	errs := make([]error, waiters)
	for i := range waiters {
		wg.Add(1)
		go func() { defer wg.Done(); toks[i], errs[i] = luciAuthToken(t.Context()) }()
	}
	<-started
	close(release)
	wg.Wait()
	for i := range waiters {
		if errs[i] != nil || toks[i] != "tok" {
			t.Errorf("waiter %d = (%q, %v), want (\"tok\", nil)", i, toks[i], errs[i])
		}
	}
	if n := runnerCalls.Load(); n != 1 {
		t.Errorf("luci-auth runs = %d, want 1", n)
	}
}

func TestBearerTokenStrategy_Prepare(t *testing.T) {
	origFlag := AuthModeFlag
	t.Cleanup(func() { AuthModeFlag = origFlag })

	for _, tc := range []struct {
		name, mode, url, tok string
		tokErr               error
		required, cancelled  bool
		wantHeader           string
		wantCalls, wantCode  int // wantCode: 0=nil, -1=any error
		wantInErr            []string
	}{
		{name: "untrusted URL", mode: "googler", url: "https://evil.example/x", tok: "tok", required: true},
		{name: "mode none", mode: "none", url: "https://trusted.example/x", tok: "tok", required: true},
		{name: "token attached and trimmed", mode: "googler", url: "https://trusted.example/x", tok: " tok \n", wantHeader: "Bearer tok", wantCalls: 1},
		{name: "token error required googler", mode: "googler", url: "https://trusted.example/x", tokErr: errors.New("not logged in"), required: true, wantCalls: 1, wantCode: ExitCodeAuth, wantInErr: []string{"Test service", "not logged in", "luci-auth login -scopes"}},
		{name: "token error required community", mode: "community", url: "https://trusted.example/x", tokErr: errors.New("not logged in"), required: true, wantCalls: 1},
		{name: "token error not required googler", mode: "googler", url: "https://trusted.example/x", tokErr: errors.New("not logged in"), wantCalls: 1},
		{name: "empty token required googler", mode: "googler", url: "https://trusted.example/x", required: true, wantCalls: 1, wantCode: ExitCodeAuth, wantInErr: []string{"empty token"}},
		{name: "invalid auth mode", mode: "bogus", url: "https://trusted.example/x", tok: "tok", wantCode: -1},
		{name: "cancelled context required googler", mode: "googler", url: "https://trusted.example/x", tokErr: context.Canceled, required: true, cancelled: true, wantCalls: 1, wantCode: -1},
	} {
		t.Run(tc.name, func(t *testing.T) {
			WithHermeticAuth(t, AuthResolvers{})
			AuthModeFlag = tc.mode
			var calls int
			s := NewDynamicBearerStrategy(
				"token",
				func(u *url.URL) bool { return urlHostIn(u, "trusted.example") },
				modeAwareBearerToken("Test service", nil, tc.required, func(context.Context) (string, error) {
					calls++
					return tc.tok, tc.tokErr
				}),
				func(string) {},
			)
			ctx := t.Context()
			if tc.cancelled {
				var cancel context.CancelFunc
				ctx, cancel = context.WithCancel(ctx)
				cancel()
			}
			req, _ := http.NewRequestWithContext(ctx, http.MethodGet, tc.url, nil)
			err := s.Prepare(ctx, req)
			if tc.cancelled && (!errors.Is(err, context.Canceled) || ExitCodeFor(err) == ExitCodeAuth) {
				t.Errorf("Prepare() error = %v, want context.Canceled without ExitCodeAuth", err)
			}
			if (tc.wantCode == 0 && err != nil) || (tc.wantCode != 0 && err == nil) || (tc.wantCode > 0 && ExitCodeFor(err) != tc.wantCode) {
				t.Errorf("Prepare() err = %v (code %d), want code %d", err, ExitCodeFor(err), tc.wantCode)
			}
			for _, want := range tc.wantInErr {
				if err == nil || !strings.Contains(err.Error(), want) {
					t.Errorf("Prepare() error = %v, want substring %q", err, want)
				}
			}
			if got := req.Header.Get("Authorization"); got != tc.wantHeader || calls != tc.wantCalls {
				t.Errorf("Authorization = %q (calls=%d), want %q (calls=%d)", got, calls, tc.wantHeader, tc.wantCalls)
			}
		})
	}

	t.Run("fallback Config propagation", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{})
		SetMockGit(t, nil)
		AuthModeFlag = ""
		t.Setenv("GH_ISH_AUTH_MODE", "")

		var gotCfg *Config
		var gotMode AuthMode
		var gotReason string
		var failToken bool
		wantCfg := &Config{
			Host: "pigweed-review.googlesource.com",
			Git:  (&MockGitRunner{}).OnCommand("config --get-regexp ^ghish\\.", "ghish.authmode googler\n"),
		}
		s := NewDynamicBearerStrategy(
			"token",
			func(u *url.URL) bool { return urlHostIn(u, "trusted.example") },
			modeAwareBearerToken("Test service", wantCfg, true, func(ctx context.Context) (string, error) {
				gotCfg, _ = ctx.Value(configKey).(*Config)
				gotMode, gotReason, _ = ResolveAuthMode(ctx, nil)
				if failToken {
					return "", errors.New("no token")
				}
				return "tok", nil
			}),
			func(string) {},
		)
		req := httptest.NewRequestWithContext(t.Context(), http.MethodGet, "https://trusted.example/x", nil)
		if err := s.Prepare(t.Context(), req); err != nil {
			t.Fatalf("Prepare() error = %v, want nil", err)
		}
		if got := req.Header.Get("Authorization"); got != "Bearer tok" {
			t.Errorf("Authorization = %q, want %q", got, "Bearer tok")
		}
		if gotCfg != wantCfg {
			t.Errorf("token ctx config = %v, want %v", gotCfg, wantCfg)
		}
		if gotMode != AuthModeGoogler || gotReason != "git config ghish.authmode=googler" {
			t.Errorf("token ResolveAuthMode(ctx) = (%q, %q), want (googler, git config ghish.authmode=googler)", gotMode, gotReason)
		}

		// Verify modeAwareBearerToken observes cfg via ResolveAuthMode when token fails.
		failToken = true
		reqErr := httptest.NewRequestWithContext(t.Context(), http.MethodGet, "https://trusted.example/x", nil)
		err := s.Prepare(t.Context(), reqErr)
		if ExitCodeFor(err) != ExitCodeAuth || !strings.Contains(err.Error(), "git config ghish.authmode=googler") {
			t.Errorf("Prepare() with failing token under cfg googler mode = %v, want ExitCodeAuth mentioning git config ghish.authmode=googler", err)
		}

		// Verify cfg with ghish.authmode=none disables auth without calling token().
		ResetAuthTokenCaches()
		var tokenCalls int
		noneStrategy := NewDynamicBearerStrategy(
			"token",
			func(u *url.URL) bool { return urlHostIn(u, "trusted.example") },
			modeAwareBearerToken("Test service", &Config{
				Git: (&MockGitRunner{}).OnCommand("config --get-regexp ^ghish\\.", "ghish.authmode none\n"),
			}, false, func(context.Context) (string, error) {
				tokenCalls++
				return "tok", nil
			}),
			func(string) {},
		)
		reqNone := httptest.NewRequestWithContext(t.Context(), http.MethodGet, "https://trusted.example/x", nil)
		if err := noneStrategy.Prepare(t.Context(), reqNone); err != nil || tokenCalls != 0 || reqNone.Header.Get("Authorization") != "" {
			t.Errorf("Prepare() under cfg none mode = %v (calls=%d, Auth=%q), want nil, 0, empty", err, tokenCalls, reqNone.Header.Get("Authorization"))
		}
	})
}

func TestTrustedURLsAndLogStreamAuth(t *testing.T) {
	const appspot = "cr-buildbucket.appspot.com"
	for _, tc := range []struct {
		host, url string
		want      bool
	}{
		{appspot, "https://cr-buildbucket.appspot.com/prpc/x", true},
		{appspot, "https://logs.chromium.org/x", true},
		{"https://bb.example.test", "https://bb.example.test/x", true},
		{appspot, "https://CR-BUILDBUCKET.appspot.com/prpc/x", true},
		{appspot, "https://evil.appspot.com", false},
		{appspot, "https://x.chromium.org", false},
		{appspot, "https://chromium.org", false},
		{appspot, "https://cr-buildbucket.appspot.com.evil.com", false},
		{appspot, "https://foo.luci.app", false},
		{appspot, "https://x.cr.dev", false},
		{appspot, "http://cr-buildbucket.appspot.com", false},
		{appspot, "http://logs.chromium.org", false},
		{appspot, "https://storage.googleapis.com/b/o", false},
		{appspot, "https://logs.chromium.org/x?X-Goog-Signature=abc", false},
		{"", "https://evil.example", false},
		{appspot, "http://127.0.0.1:1234", false},
		{"http://127.0.0.1:1234", "http://127.0.0.1:1234/prpc/x", true},
	} {
		u, _ := url.Parse(tc.url)
		if got := isTrustedLUCIURL(tc.host, u); got != tc.want {
			t.Errorf("isTrustedLUCIURL(%q, %q) = %v, want %v", tc.host, tc.url, got, tc.want)
		}
	}

	for _, tc := range []struct {
		url  string
		want bool
	}{
		{"https://androidbuildinternal.googleapis.com/android/internal/build/v3/x", true},
		{"http://127.0.0.1:8080/builds/x", true},
		{"https://androidbuildinternal.googleapis.com.evil.com/x", false},
		{"http://androidbuildinternal.googleapis.com/x", false},
		{"http://localhost:8080/builds/x", false},
		{"https://ci.android.com/builds/x", false},
		{"https://storage.googleapis.com/b/o", false},
		{"https://androidbuildinternal.googleapis.com/x?X-Goog-Signature=abc", false},
	} {
		u, _ := url.Parse(tc.url)
		if got := isTrustedAndroidBuildURL("https://androidbuildinternal.googleapis.com", "http://127.0.0.1:8080", u); got != tc.want {
			t.Errorf("isTrustedAndroidBuildURL(%q) = %v, want %v", tc.url, got, tc.want)
		}
	}

	WithHermeticAuth(t, AuthResolvers{})
	auths := map[string]string{}
	rawQueries := map[string]string{}
	client := NewLUCIClient(appspot, &http.Client{Transport: testRoundTripper(func(req *http.Request) (*http.Response, error) {
		auths[req.URL.Host] = req.Header.Get("Authorization")
		rawQueries[req.URL.Host] = req.URL.RawQuery
		return &http.Response{
			StatusCode: http.StatusOK,
			Body:       io.NopCloser(strings.NewReader("log line 1\n")),
			Header:     http.Header{},
			Request:    req,
		}, nil
	})})
	client.TokenProvider = func(context.Context) (string, error) { return "tok", nil }
	_, _, _ = client.fetchLogStreamTail(t.Context(), "https://evil.appspot.com/logs/x", 0)
	_, _, _ = client.fetchLogStreamTail(t.Context(), "https://logs.chromium.org/logs/x", 0)
	if auths["evil.appspot.com"] != "" || auths["logs.chromium.org"] != "Bearer tok" {
		t.Errorf("auths = %v, want empty for evil.appspot.com and Bearer tok for logs.chromium.org", auths)
	}
	if rawQueries["logs.chromium.org"] != "format=raw" {
		t.Errorf("logs.chromium.org query = %q, want %q", rawQueries["logs.chromium.org"], "format=raw")
	}

	// Pre-signed GCS URLs must preserve the exact raw query string without
	// appending format=raw or reordering parameters, and must never carry Authorization.
	const wantSignedQuery = "z=1&X-Goog-Algorithm=GOOG4-RSA-SHA256&X-Goog-Signature=deadbeef"
	signedURL := "https://storage.googleapis.com/bucket/stdout.txt?" + wantSignedQuery
	gotText, gotLines, err := client.fetchLogStreamTail(t.Context(), signedURL, 0)
	if err != nil {
		t.Fatalf("fetchLogStreamTail(pre-signed GCS URL) failed: %v", err)
	}
	if gotText != "log line 1\n" || gotLines != 1 {
		t.Errorf("fetchLogStreamTail(pre-signed GCS URL) = (%q, %d), want (\"log line 1\\n\", 1)", gotText, gotLines)
	}
	if rawQueries["storage.googleapis.com"] != wantSignedQuery {
		t.Errorf("pre-signed GCS RawQuery = %q, want exact %q", rawQueries["storage.googleapis.com"], wantSignedQuery)
	}
	if auths["storage.googleapis.com"] != "" {
		t.Errorf("pre-signed GCS Authorization = %q, want empty", auths["storage.googleapis.com"])
	}

	// Verify fetchLogStreamTail wraps googler-mode missing-token errors with
	// call-site context while preserving ExitCodeAuth.
	t.Setenv("GH_ISH_AUTH_MODE", "googler")
	client.TokenProvider = func(context.Context) (string, error) { return "", errors.New("not logged in") }
	_, _, authApplyErr := client.fetchLogStreamTail(t.Context(), "https://logs.chromium.org/logs/x", 0)
	if ExitCodeFor(authApplyErr) != ExitCodeAuth || !strings.Contains(authApplyErr.Error(), "failed to authenticate log request:") || !strings.Contains(authApplyErr.Error(), "LUCI authentication required in googler mode") {
		t.Errorf("fetchLogStreamTail(missing googler token) = %v (code %d), want ExitCodeAuth with log auth context", authApplyErr, ExitCodeFor(authApplyErr))
	}

	// Verify newLUCIHTTPAuthError omits gcert on external Linux without sso-cred-helper,
	// and includes Corp Linux gcert remediation when sso-cred-helper is usable.
	withHostRoleForTest(t, "linux", "", false)
	if err3P := newLUCIHTTPAuthError(t.Context(), "LogDog", http.StatusForbidden, []byte("denied")); strings.Contains(err3P.Error(), "gcert") {
		t.Errorf("newLUCIHTTPAuthError without sso-cred-helper unexpectedly mentioned gcert: %v", err3P)
	}
	WithHermeticAuth(t, AuthResolvers{
		LookPath: func(file string) (string, error) {
			if file == ssoCredHelperPath {
				return ssoCredHelperPath, nil
			}
			return "", os.ErrNotExist
		},
	})
	httpAuthErr := newLUCIHTTPAuthError(t.Context(), "LogDog", http.StatusForbidden, []byte("denied"))
	if ExitCodeFor(httpAuthErr) != ExitCodeAuth || !strings.Contains(httpAuthErr.Error(), "gcert") {
		t.Errorf("newLUCIHTTPAuthError = %v, want ExitCodeAuth mentioning gcert", httpAuthErr)
	}
}

func TestScopesAndLoginCommandByMode(t *testing.T) {
	for _, tc := range []struct {
		mode                  AuthMode
		wantScopes, wantLogin string
		wantServices          []string
	}{
		{AuthModeGoogler, wantGooglerScopes, `luci-auth login -scopes "` + wantGooglerScopes + `"`, []string{"gerrit", "luci", "android-build", "issue-tracker"}},
		{AuthModeCommunity, wantCommunityScopes, `luci-auth login -scopes "` + wantCommunityScopes + `"`, []string{"gerrit", "luci"}},
		{AuthModeNone, "", "", nil},
	} {
		t.Run(string(tc.mode), func(t *testing.T) {
			var calls [][]string
			fail := false
			WithHermeticAuth(t, AuthResolvers{
				LookPath: luciAuthFoundLookPath,
				CommandRunner: func(_ context.Context, _ string, args ...string) ([]byte, error) {
					calls = append(calls, args)
					if fail {
						return nil, errors.New("not logged in")
					}
					return luciAuthJSON("shared", time.Now().Add(time.Hour)), nil
				},
			})
			withLUCIAuthTokenResolvers(t)
			t.Setenv("GH_ISH_AUTH_MODE", string(tc.mode))

			got := allServiceTokens(t.Context())
			var gotServices []string
			for _, svc := range []string{"gerrit", "luci", "android-build", "issue-tracker"} {
				if got[svc] == "shared" {
					gotServices = append(gotServices, svc)
				}
			}
			if !slices.Equal(gotServices, tc.wantServices) {
				t.Errorf("services with luci-auth token = %q, want %q", gotServices, tc.wantServices)
			}
			if tc.wantScopes != "" {
				if len(calls) != 1 || !slices.Equal(calls[0], []string{"token", "-scopes", tc.wantScopes, "-json-output", "-"}) {
					t.Fatalf("luci-auth calls = %q, want 1 call with scopes %q", calls, tc.wantScopes)
				}
				if got := luciAuthLoginCommand(tc.mode); got != tc.wantLogin {
					t.Errorf("luciAuthLoginCommand(%s) = %q, want %q", tc.mode, got, tc.wantLogin)
				}
				ResetAuthTokenCaches()
				fail = true
				if _, _, err := DefaultLUCIToken(t.Context()); err == nil || !strings.Contains(err.Error(), tc.wantLogin) {
					t.Errorf("DefaultLUCIToken() error = %v, want substring %q", err, tc.wantLogin)
				}
				if tc.mode == AuthModeCommunity {
					_, _, issueErr := ResolveIssueTrackerTokenWithSource(t.Context())
					_, abErr := DefaultAndroidBuildToken(t.Context())
					for _, args := range calls {
						if joined := strings.Join(args, " "); strings.Contains(joined, ScopeBuganizer) || strings.Contains(joined, ScopeAndroidBuildInternal) {
							t.Errorf("community mode ran luci-auth %q, want no buganizer or androidbuild.internal scope", args)
						}
					}
					if abErr == nil || !strings.Contains(abErr.Error(), "GH_ISH_AUTH_MODE=googler") {
						t.Errorf("DefaultAndroidBuildToken() error = %v, want GH_ISH_AUTH_MODE=googler hint", abErr)
					}
					wantHint := "(Googlers) export GH_ISH_AUTH_MODE=googler; " + luciAuthLoginCommand(AuthModeGoogler)
					if issueErr == nil || !strings.Contains(issueErr.Error(), wantHint) {
						t.Errorf("ResolveIssueTrackerTokenWithSource() error = %v, want substring %q", issueErr, wantHint)
					}
				}
			} else if _, err := luciAuthToken(t.Context()); !errors.Is(err, errLUCIAuthDisabled) || len(calls) != 0 {
				t.Errorf("luciAuthToken() in none mode = %v (calls=%d), want errLUCIAuthDisabled", err, len(calls))
			}

			server := NewMockGerritServer(t)
			server.OnJSON("GET", "/accounts/self", http.StatusUnauthorized, map[string]any{"message": "auth required"})
			noToken := errors.New("luci-auth not found")
			WithHermeticAuth(t, AuthResolvers{
				LookPath:          luciAuthFoundLookPath,
				GerritToken:       func(context.Context) (string, string, error) { return "", "", noToken },
				LUCIToken:         func(context.Context) (string, string, error) { return "", "", noToken },
				IssueTrackerToken: func(context.Context) (string, string, error) { return "", "", noToken },
			})

			text, _ := executeCommand(RootCmd, "auth", "status", "--auth-mode", string(tc.mode))
			if !strings.Contains(text, fmt.Sprintf("Authentication Mode: %s (--auth-mode=%s)\n", tc.mode, tc.mode)) {
				t.Errorf("auth status output missing mode and reason:\n%s", text)
			}
			if strings.Contains(text, "luci-auth Login:") {
				t.Errorf("auth status output unexpectedly contained top-level luci-auth Login header:\n%s", text)
			}

			out, _ := executeCommand(RootCmd, "auth", "status", "--auth-mode", string(tc.mode), "--json")
			var report AuthStatusReport
			if err := json.NewDecoder(strings.NewReader(out)).Decode(&report); err != nil {
				t.Fatalf("failed to parse JSON: %v\nRaw:\n%s", err, out)
			}
			if tc.mode == AuthModeNone {
				return
			}
			for _, st := range []ServiceAuthStatus{report.Gerrit, report.LUCI, report.Buganizer} {
				want := tc.wantLogin
				if st.Service == "buganizer" && tc.mode == AuthModeCommunity {
					want = "export GH_ISH_AUTH_MODE=googler"
				}
				if !strings.Contains(st.Remediation, want) || strings.Contains(st.Remediation, "gcloud") {
					t.Errorf("%s remediation = %q, want substring %q and no gcloud", st.Service, st.Remediation, want)
				}
			}
		})
	}
}

type countingGitRunner struct{ calls atomic.Int32 }

func (r *countingGitRunner) Run(context.Context, io.Writer, io.Writer, ...string) error {
	r.calls.Add(1)
	return errors.New("countingGitRunner: no git")
}

func TestLUCIAuthToken_ContextModeSkipsAutoDetection(t *testing.T) {
	var detectLookups atomic.Int32
	WithHermeticAuth(t, AuthResolvers{
		LookPath: func(file string) (string, error) {
			if file != "luci-auth" {
				detectLookups.Add(1)
			}
			return luciAuthFoundLookPath(file)
		},
		CommandRunner: func(context.Context, string, ...string) ([]byte, error) {
			return luciAuthJSON("tok", time.Now().Add(time.Hour)), nil
		},
	})
	git := &countingGitRunner{}
	prevGit := DefaultGitRunner
	DefaultGitRunner = git
	t.Cleanup(func() { DefaultGitRunner = prevGit })
	detections := func() int32 { return detectLookups.Load() + git.calls.Load() }

	t.Setenv("GH_ISH_AUTH_MODE", "googler")
	for i := range 2 {
		if tok, err := luciAuthToken(t.Context()); err != nil || tok != "tok" {
			t.Fatalf("luciAuthToken() call %d = (%q, %v), want \"tok\"", i, tok, err)
		}
	}
	if n := detections(); n != 0 {
		t.Errorf("auto-detection lookups with explicit mode = %d, want 0", n)
	}

	s := NewDynamicBearerStrategy(
		"token",
		func(*url.URL) bool { return true },
		modeAwareBearerToken("LUCI", nil, false, luciAuthToken),
		func(string) {},
	)
	req := httptest.NewRequestWithContext(t.Context(), http.MethodGet, "https://cr-buildbucket.appspot.com/", nil)
	if err := s.Prepare(t.Context(), req); err != nil || req.Header.Get("Authorization") != "Bearer tok" {
		t.Errorf("Prepare() = %v, Auth=%q", err, req.Header.Get("Authorization"))
	}

	t.Setenv("GH_ISH_AUTH_MODE", "")
	cfgCtx := context.WithValue(t.Context(), configKey, &Config{Git: git})
	if _, err := luciAuthToken(cfgCtx); err != nil || detections() == 0 {
		t.Errorf("luciAuthToken() in auto mode err=%v, detections=%d (want >0)", err, detections())
	}
	afterFirstAuto := detections()
	if err := s.Prepare(cfgCtx, httptest.NewRequestWithContext(cfgCtx, http.MethodGet, "https://cr-buildbucket.appspot.com/", nil)); err != nil {
		t.Errorf("Prepare() in auto mode = %v", err)
	}
	if got := detections(); got != afterFirstAuto {
		t.Errorf("detections after cached auto-mode Prepare() = %d, want %d", got, afterFirstAuto)
	}
}

func TestBuildbucketProvider_PropagatesCfgToLUCIAuth(t *testing.T) {
	WithHermeticAuth(t, AuthResolvers{})
	SetMockGit(t, nil)
	origFlag := AuthModeFlag
	AuthModeFlag = ""
	t.Cleanup(func() { AuthModeFlag = origFlag })
	t.Setenv("GH_ISH_AUTH_MODE", "")

	cfg := &Config{
		Host: "pigweed-review.googlesource.com",
		Git:  (&MockGitRunner{}).OnCommand("config --get-regexp ^ghish\\.", "ghish.authmode googler\n"),
	}

	var mu sync.Mutex
	var seenCfgs []*Config
	var seenModes []AuthMode
	var failToken bool

	luciClient := NewLUCIClient("https://bb.example.test", &http.Client{
		Transport: testRoundTripper(func(req *http.Request) (*http.Response, error) {
			if got := req.Header.Get("Authorization"); got != "Bearer bb-tok" {
				t.Errorf("%s Authorization = %q, want 'Bearer bb-tok'", req.URL, got)
			}
			var body string
			switch {
			case strings.HasSuffix(req.URL.Path, "/SearchBuilds"):
				body = ")]}'\n{\"builds\": [{\"id\": \"101\", \"builder\": {\"builder\": \"host\"}, \"status\": \"FAILURE\"}]}"
			case strings.HasSuffix(req.URL.Path, "/GetBuild"):
				body = ")]}'\n{\"id\": \"101\", \"builder\": {\"builder\": \"host\"}, \"status\": \"FAILURE\", \"steps\": [{\"name\": \"ninja\", \"status\": \"FAILURE\", \"logs\": [{\"name\": \"stdout\", \"viewUrl\": \"https://logs.chromium.org/logs/stdout\"}]}]}"
			case req.URL.Host == "logs.chromium.org":
				body = "FAILED: out/foo.o\nerror: compile failed\n"
			default:
				t.Fatalf("unexpected URL: %s", req.URL)
			}
			return &http.Response{
				StatusCode: http.StatusOK,
				Body:       io.NopCloser(strings.NewReader(body)),
				Header:     http.Header{"Content-Type": []string{"application/json"}},
				Request:    req,
			}, nil
		}),
	})
	luciClient.TokenProvider = func(ctx context.Context) (string, error) {
		c, _ := ctx.Value(configKey).(*Config)
		mode, _, _ := ResolveAuthMode(ctx, nil)
		mu.Lock()
		seenCfgs = append(seenCfgs, c)
		seenModes = append(seenModes, mode)
		fail := failToken
		mu.Unlock()
		if fail {
			return "", errors.New("token unavailable")
		}
		return "bb-tok", nil
	}

	prov := &BuildbucketProvider{
		LUCIClient: luciClient,
		Cfg:        cfg,
	}
	change := &ChangeInfo{Project: "pigweed/pigweed"}

	if _, err := prov.SearchBuilds(t.Context(), "pigweed-review.googlesource.com", 123, 1, change); err != nil {
		t.Fatalf("SearchBuilds failed: %v", err)
	}
	if _, err := prov.SearchBuildsByPatchset(t.Context(), "pigweed-review.googlesource.com", 123, []int{2, 1}, change); err != nil {
		t.Fatalf("SearchBuildsByPatchset failed: %v", err)
	}
	if _, err := prov.GetBuildDetails(t.Context(), "101", nil); err != nil {
		t.Fatalf("GetBuildDetails failed: %v", err)
	}
	report, err := prov.FetchFailureReportWithOptions(t.Context(), bbBuild{ID: "101"}, FailureReportOptions{MaxLogLines: 20})
	if err != nil {
		t.Fatalf("FetchFailureReportWithOptions failed: %v", err)
	}
	if report == nil || !strings.Contains(report.LogSnippet, "error: compile failed") {
		t.Errorf("FetchFailureReportWithOptions report = %+v, want compile failed snippet", report)
	}

	// 1 (SearchBuilds) + 2 (SearchBuildsByPatchset) + 1 (GetBuildDetails) + 2 (GetBuild + LogDog in FetchFailureReportWithOptions) = 6
	if len(seenCfgs) != 6 {
		t.Fatalf("token calls = %d, want 6", len(seenCfgs))
	}
	for i := range seenCfgs {
		if seenCfgs[i] != cfg || seenModes[i] != AuthModeGoogler {
			t.Errorf("call %d observed (cfg=%p, mode=%q), want (cfg=%p, googler)", i, seenCfgs[i], seenModes[i], cfg)
		}
	}

	// Because p.Cfg propagates git config ghish.authmode=googler, missing token fails with ExitCodeAuth.
	failToken = true
	_, err = prov.SearchBuilds(t.Context(), "pigweed-review.googlesource.com", 123, 1, change)
	if ExitCodeFor(err) != ExitCodeAuth || !strings.Contains(err.Error(), "git config ghish.authmode=googler") {
		t.Errorf("SearchBuilds with failing token = %v, want ExitCodeAuth mentioning git config ghish.authmode=googler", err)
	}
}

func TestHTTPClients_InvalidateCachedLUCITokenOn401(t *testing.T) {
	for _, tc := range []struct {
		name string
		body string
		call func(*testing.T, *http.Client)
	}{
		{
			name: "LUCIClient",
			body: ")]}'\n{}",
			call: func(t *testing.T, hc *http.Client) {
				var out map[string]any
				if err := NewLUCIClient("cr-buildbucket.appspot.com", hc).CallPRPC(t.Context(), "buildbucket.v2.Builds", "GetBuild", map[string]any{"id": "1"}, &out); err != nil {
					t.Fatalf("CallPRPC: %v", err)
				}
			},
		},
		{
			name: "BusytownProvider",
			body: "build log line",
			call: func(t *testing.T, hc *http.Client) {
				p := NewBusytownProvider()
				p.APIBase = "https://androidbuildinternal.googleapis.com"
				p.HTTPClient = hc
				if logText, _, _, _ := p.fetchBusytownLogs(t.Context(), "123", "target", "inv"); logText != "build log line" {
					t.Fatalf("fetchBusytownLogs = %q, want \"build log line\"", logText)
				}
			},
		},
		{
			name: "IssueTrackerClient",
			body: `{"issueId":"1"}`,
			call: func(t *testing.T, hc *http.Client) {
				if issue, err := NewIssueTrackerClient("https://issuetracker.googleapis.com/v1", hc).GetIssue(t.Context(), 1); err != nil || issue == nil || issue.IssueID != 1 {
					t.Fatalf("GetIssue = (%+v, %v), want issue 1", issue, err)
				}
			},
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			var mints int
			WithHermeticAuth(t, AuthResolvers{
				LookPath: luciAuthFoundLookPath,
				CommandRunner: func(context.Context, string, ...string) ([]byte, error) {
					mints++
					return luciAuthJSON(fmt.Sprintf("shared-tok-%d", mints), time.Now().Add(time.Hour)), nil
				},
			})
			withLUCIAuthTokenResolvers(t)
			t.Setenv("GH_ISH_AUTH_MODE", "googler")
			ResetAuthTokenCaches()

			var auths []string
			tc.call(t, &http.Client{
				Transport: testRoundTripper(func(req *http.Request) (*http.Response, error) {
					auths = append(auths, req.Header.Get("Authorization"))
					if len(auths) == 1 {
						return &http.Response{StatusCode: http.StatusUnauthorized, Body: http.NoBody, Header: make(http.Header), Request: req}, nil
					}
					return &http.Response{StatusCode: http.StatusOK, Body: io.NopCloser(strings.NewReader(tc.body)), Header: make(http.Header), Request: req}, nil
				}),
			})
			if mints != 2 || !slices.Equal(auths, []string{"Bearer shared-tok-1", "Bearer shared-tok-2"}) {
				t.Errorf("mints=%d auths=%q, want 2 and [Bearer shared-tok-1, Bearer shared-tok-2]", mints, auths)
			}
		})
	}
}
