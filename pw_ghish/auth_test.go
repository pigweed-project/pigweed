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
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"
	"sync"
	"testing"
	"time"
)

// okGerritResponse is a canned 200 reply for recorder transports.
func okGerritResponse(req *http.Request, body string) *http.Response {
	return &http.Response{
		StatusCode: http.StatusOK,
		Body:       io.NopCloser(strings.NewReader(")]}'\n" + body)),
		Header:     make(http.Header),
		Request:    req,
	}
}

func TestGerritClient_PerRequestAuth(t *testing.T) {
	for _, tc := range []struct {
		name     string
		baseURL  string
		auth     GerritAuth
		wantAuth string
	}{
		{
			name:     "static token to HTTPS base host",
			baseURL:  "https://example.com",
			auth:     GerritAuth{StaticToken: "sample-token-12345"},
			wantAuth: "Bearer sample-token-12345",
		},
		{
			name:     "static token is trimmed",
			baseURL:  "https://pigweed-review.googlesource.com/a",
			auth:     GerritAuth{StaticToken: "  pinned-token \n"},
			wantAuth: "Bearer pinned-token",
		},
		{
			name:     "loopback HTTP attaches token",
			baseURL:  "http://127.0.0.1:1234",
			auth:     GerritAuth{StaticToken: "pinned-token"},
			wantAuth: "Bearer pinned-token",
		},
		{
			name:     "plaintext HTTP omits token",
			baseURL:  "http://pigweed-review.googlesource.com/a",
			auth:     GerritAuth{StaticToken: "pinned-token"},
			wantAuth: "",
		},
		{
			name:     "unauthenticated sends no token",
			baseURL:  "https://pigweed-review.googlesource.com/a",
			wantAuth: "",
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			var lastReq *http.Request
			tr := &mockTransport{
				roundTrip: func(req *http.Request) (*http.Response, error) {
					lastReq = req
					return okGerritResponse(req, "{}"), nil
				},
			}
			client := NewGerritClientWithHTTP(tc.baseURL, &http.Client{Transport: tr}, tc.auth)
			if _, err := client.doRaw(t.Context(), "GET", "changes/1", nil, nil, maxHTTPBodyBytes); err != nil {
				t.Fatalf("doRaw failed: %v", err)
			}
			if got := lastReq.Header.Get("Authorization"); got != tc.wantAuth {
				t.Errorf("Authorization = %q, want %q", got, tc.wantAuth)
			}
		})
	}

	t.Run("cross-host redirect strips Authorization", func(t *testing.T) {
		var redirectTargetAuth string
		gcsSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			redirectTargetAuth = r.Header.Get("Authorization")
			w.WriteHeader(http.StatusOK)
			_, _ = w.Write([]byte(")]}'\n{}"))
		}))
		defer gcsSrv.Close()

		var originAuth string
		originSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			originAuth = r.Header.Get("Authorization")
			gcsURL := strings.Replace(gcsSrv.URL, "127.0.0.1", "localhost", 1)
			http.Redirect(w, r, gcsURL+"/signed-blob", http.StatusFound)
		}))
		defer originSrv.Close()

		redirectClient := NewGerritClientWithHTTP(originSrv.URL+"/a", originSrv.Client(), GerritAuth{StaticToken: "pinned-token"})
		if _, err := redirectClient.doRaw(t.Context(), "GET", "changes/1", nil, nil, maxHTTPBodyBytes); err != nil {
			t.Fatalf("redirectClient.doRaw failed: %v", err)
		}
		if originAuth != "Bearer pinned-token" {
			t.Errorf("origin Authorization = %q, want 'Bearer pinned-token'", originAuth)
		}
		if redirectTargetAuth != "" {
			t.Errorf("redirect target Authorization = %q, want empty", redirectTargetAuth)
		}
	})
}

func TestEnsureSafeHTTPClient(t *testing.T) {
	mustAuthedReq := func(rawURL string) *http.Request {
		u, err := url.Parse(rawURL)
		if err != nil {
			t.Fatal(err)
		}
		h := make(http.Header)
		h.Set("Authorization", "Bearer secret")
		h.Set("Cookie", "SID=secret")
		return &http.Request{URL: u, Header: h}
	}
	origin := []*http.Request{mustAuthedReq("https://pigweed-review.googlesource.com/a/changes/1")}

	t.Run("nil client creates safe client with no timeout", func(t *testing.T) {
		got := ensureSafeHTTPClient(nil)
		if got == nil || got.CheckRedirect == nil || got.Timeout != 0 {
			t.Fatalf("ensureSafeHTTPClient(nil) = %+v, want non-nil with CheckRedirect and Timeout=0", got)
		}
		if _, ok := got.Transport.(*safeHTTPTransport); !ok {
			t.Fatalf("got.Transport = %T, want *safeHTTPTransport", got.Transport)
		}
		req := mustAuthedReq("https://evil.example.com/blob")
		if err := got.CheckRedirect(req, origin); err != nil || req.Header.Get("Authorization") != "" || req.Header.Get("Cookie") != "" {
			t.Errorf("cross-host redirect: err=%v, hdr=%v; want nil and stripped", err, req.Header)
		}
	})

	t.Run("client without CheckRedirect is shallow-copied without mutating input", func(t *testing.T) {
		tr := &mockTransport{roundTrip: func(req *http.Request) (*http.Response, error) {
			return okGerritResponse(req, "{}"), nil
		}}
		in := &http.Client{Transport: tr, Timeout: 15 * time.Second}
		got := ensureSafeHTTPClient(in)
		if got == in {
			t.Fatal("expected shallow copy of input client, got same pointer")
		}
		if in.CheckRedirect != nil || in.Transport != tr {
			t.Error("input client was mutated")
		}
		safeRT, ok := got.Transport.(*safeHTTPTransport)
		if !ok || safeRT.direct != tr || got.Timeout != 15*time.Second || got.CheckRedirect == nil {
			t.Fatalf("copied client = %+v, want safeHTTPTransport(direct=tr), Timeout=15s, and CheckRedirect set", got)
		}
		req := mustAuthedReq("https://evil.example.com/blob")
		if err := got.CheckRedirect(req, origin); err != nil || req.Header.Get("Authorization") != "" || req.Header.Get("Cookie") != "" {
			t.Errorf("cross-host redirect: err=%v, hdr=%v; want nil and stripped", err, req.Header)
		}
		if resp, err := got.Transport.RoundTrip(mustAuthedReq("http://evil.example.com/blob")); err == nil {
			resp.Body.Close()
			t.Fatal("expected cleartext non-loopback authenticated request to be refused")
		}
		resp, err := got.Transport.RoundTrip(mustAuthedReq("http://127.0.0.1:8080/a/changes/1"))
		if err != nil {
			t.Fatalf("loopback authenticated request failed: %v", err)
		}
		resp.Body.Close()
	})

	t.Run("client with custom CheckRedirect composes auth stripping and is idempotent", func(t *testing.T) {
		var customCalled bool
		sentinelErr := errors.New("stop redirect")
		in := &http.Client{
			CheckRedirect: func(req *http.Request, via []*http.Request) error {
				customCalled = true
				return sentinelErr
			},
		}
		got := ensureSafeHTTPClient(in)
		if got == in {
			t.Fatal("expected shallow copy of input client so custom CheckRedirect is not mutated in place")
		}
		if ensureSafeHTTPClient(got) != got {
			t.Error("ensureSafeHTTPClient on an already-safe client should return the same pointer")
		}
		req := mustAuthedReq("https://evil.example.com/blob")
		if err := got.CheckRedirect(req, origin); !errors.Is(err, sentinelErr) || !customCalled {
			t.Errorf("custom CheckRedirect: err=%v, customCalled=%v; want %v and true", err, customCalled, sentinelErr)
		}
		if req.Header.Get("Authorization") != "" || req.Header.Get("Cookie") != "" {
			t.Errorf("cross-host redirect with custom CheckRedirect kept auth headers=%v, want stripped", req.Header)
		}
	})
}

func TestGerritClient_DynamicReMint(t *testing.T) {
	newReMintTransport := func() (tr *mockTransport, reqs *int, bodies *[]string) {
		var nReqs int
		var gotBodies []string
		return &mockTransport{
			roundTrip: func(req *http.Request) (*http.Response, error) {
				nReqs++
				if req.Body != nil {
					b, _ := io.ReadAll(req.Body)
					gotBodies = append(gotBodies, string(b))
				}
				if req.Header.Get("Authorization") == "Bearer fresh-token" {
					return okGerritResponse(req, "{}"), nil
				}
				return &http.Response{
					StatusCode: http.StatusUnauthorized,
					Body:       io.NopCloser(strings.NewReader("Unauthorized")),
					Header:     make(http.Header),
					Request:    req,
				}, nil
			},
		}, &nReqs, &gotBodies
	}

	for _, tc := range []struct {
		name                            string
		auth                            GerritAuth
		body                            any
		mintFn                          func(int) string
		wantStatus, wantMints, wantReqs int
		wantBodies                      []string
	}{
		{
			name: "re-mint and replay",
			auth: GerritAuth{Dynamic: true},
			body: &ReviewInput{Message: "hi"},
			mintFn: func(m int) string {
				if m == 1 {
					return "stale-token"
				}
				return "fresh-token"
			},
			wantStatus: http.StatusOK, wantMints: 2, wantReqs: 2,
			wantBodies: []string{`{"message":"hi"}`, `{"message":"hi"}`},
		},
		{
			name:       "same token is not retried",
			auth:       GerritAuth{Dynamic: true},
			mintFn:     func(int) string { return "same-token" },
			wantStatus: http.StatusUnauthorized, wantMints: 2, wantReqs: 1,
		},
		{
			name:       "retry also 401",
			auth:       GerritAuth{Dynamic: true},
			mintFn:     func(m int) string { return fmt.Sprintf("still-bad-%d", m) },
			wantStatus: http.StatusUnauthorized, wantMints: 2, wantReqs: 2,
		},
		{
			name:       "static token is never re-minted",
			auth:       GerritAuth{StaticToken: "static-token"},
			mintFn:     func(int) string { return "fresh-token" },
			wantStatus: http.StatusUnauthorized, wantMints: 0, wantReqs: 1,
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			var mints int
			WithHermeticAuth(t, AuthResolvers{
				GerritToken: func(ctx context.Context) (string, string, error) {
					mints++
					return tc.mintFn(mints), "luci-auth", nil
				},
			})
			base, reqs, gotBodies := newReMintTransport()
			client := NewGerritClientWithHTTP("https://pigweed-review.googlesource.com/a", &http.Client{Transport: base}, tc.auth)
			_, err := client.doRaw(t.Context(), "POST", "changes/1/review", nil, tc.body, maxHTTPBodyBytes)
			if tc.wantStatus == http.StatusOK && err != nil {
				t.Fatalf("doRaw err = %v, want success", err)
			}
			if tc.wantStatus != http.StatusOK && !IsGerritHTTPStatus(err, tc.wantStatus) {
				t.Fatalf("doRaw err = %v, want HTTP %d", err, tc.wantStatus)
			}
			if mints != tc.wantMints || *reqs != tc.wantReqs {
				t.Errorf("mints=%d reqs=%d, want %d and %d", mints, *reqs, tc.wantMints, tc.wantReqs)
			}
			if !slices.Equal(*gotBodies, tc.wantBodies) {
				t.Errorf("bodies = %v, want %v", *gotBodies, tc.wantBodies)
			}
		})
	}

	t.Run("rejected luci-auth token is evicted from the cache", func(t *testing.T) {
		var cliCalls int
		WithHermeticAuth(t, AuthResolvers{
			LookPath: onPath("luci-auth", "/usr/bin/luci-auth"),
			CommandRunner: func(ctx context.Context, name string, args ...string) ([]byte, error) {
				cliCalls++
				if cliCalls == 1 {
					return []byte("stale-token\n"), nil
				}
				return []byte("fresh-token\n"), nil
			},
		})
		base, reqs, _ := newReMintTransport()
		client := NewGerritClientWithHTTP("https://pigweed-review.googlesource.com/a", &http.Client{Transport: base}, GerritAuth{Dynamic: true})
		if _, err := client.doRaw(t.Context(), "GET", "changes/1", nil, nil, maxHTTPBodyBytes); err != nil {
			t.Fatalf("doRaw after cache eviction = %v, want 200 OK", err)
		}
		if cliCalls != 2 || *reqs != 2 {
			t.Errorf("cliCalls=%d reqs=%d, want 2 and 2", cliCalls, *reqs)
		}
		if tok, _, err := DefaultGerritToken(t.Context()); err != nil || tok != "fresh-token" {
			t.Errorf("cached token after re-mint = (%q, %v), want fresh-token", tok, err)
		}
	})

	t.Run("failed re-mint without anonymous fallback is an auth error", func(t *testing.T) {
		var mints int
		WithHermeticAuth(t, AuthResolvers{
			GerritToken: func(ctx context.Context) (string, string, error) {
				mints++
				if mints == 1 {
					return "stale-token", "luci-auth", nil
				}
				return "", "", errors.New("luci-auth: not logged in")
			},
		})
		base, _, _ := newReMintTransport()
		client := NewGerritClientWithHTTP("https://pigweed-review.googlesource.com/a", &http.Client{Transport: base}, GerritAuth{Dynamic: true})
		_, err := client.doRaw(t.Context(), "GET", "changes/1", nil, nil, maxHTTPBodyBytes)
		if ExitCodeFor(err) != ExitCodeAuth || !strings.Contains(err.Error(), "not logged in") {
			t.Errorf("doRaw err = %v, want ExitCodeAuth naming the mint failure", err)
		}
		if err != nil && !strings.Contains(err.Error(), "luci-auth login -scopes") {
			t.Errorf("doRaw err = %v, want login remediation", err)
		}
	})

	t.Run("dynamic client never sends a minted token to a non-Google host", func(t *testing.T) {
		var mints int
		WithHermeticAuth(t, AuthResolvers{
			GerritToken: func(ctx context.Context) (string, string, error) {
				mints++
				return "minted", "luci-auth", nil
			},
		})
		var gotAuth []string
		rec := &mockTransport{roundTrip: func(req *http.Request) (*http.Response, error) {
			gotAuth = append(gotAuth, req.Header.Get("Authorization"))
			return okGerritResponse(req, "{}"), nil
		}}
		for _, u := range []string{"https://gerrit.example.com", "https://x.googlesource.com.evil.com"} {
			gotAuth = nil
			client := NewGerritClientWithHTTP(u, &http.Client{Transport: rec}, GerritAuth{Dynamic: true, AllowAnonymousFallback: true})
			if _, err := client.doRaw(t.Context(), "GET", "changes/1", nil, nil, maxHTTPBodyBytes); err != nil {
				t.Fatalf("%s: doRaw err = %v", u, err)
			}
			if len(gotAuth) != 1 || gotAuth[0] != "" {
				t.Errorf("%s: Authorization = %q, want none", u, gotAuth)
			}
		}
		if mints != 0 {
			t.Errorf("resolver calls = %d, want 0", mints)
		}
	})

	t.Run("two-step dynamic refresh then anonymous fallback", func(t *testing.T) {
		var mints int
		WithHermeticAuth(t, AuthResolvers{
			GerritToken: func(context.Context) (string, string, error) {
				mints++
				return fmt.Sprintf("rejected-tok-%d", mints), "luci-auth", nil
			},
		})
		var seen []string
		tr := &mockTransport{roundTrip: func(req *http.Request) (*http.Response, error) {
			seen = append(seen, req.Header.Get("Authorization")+"|"+req.URL.Path)
			if strings.HasPrefix(req.URL.Path, "/a/") {
				return gerritStatusTransport(http.StatusUnauthorized, "invalid_token", nil).roundTrip(req)
			}
			return okGerritResponse(req, `{"_number":42}`), nil
		}}
		var stderrBuf bytes.Buffer
		client := NewGerritClientWithHTTP("https://pigweed-review.googlesource.com/a",
			&http.Client{Transport: tr}, GerritAuth{Dynamic: true, AllowAnonymousFallback: true})
		client.stderr = &stderrBuf

		ch, err := client.GetChange(t.Context(), "42")
		if err != nil || ch.Number != 42 {
			t.Fatalf("GetChange = (%+v, %v), want change 42", ch, err)
		}
		wantSeen := []string{
			"Bearer rejected-tok-1|/a/changes/42",
			"Bearer rejected-tok-2|/a/changes/42",
			"|/changes/42",
		}
		if mints != 2 || !slices.Equal(seen, wantSeen) {
			t.Errorf("mints=%d seen=%q, want 2 and %q", mints, seen, wantSeen)
		}
		if !strings.Contains(stderrBuf.String(), "Warning: authenticated request to /a/changes/42 returned 401; falling back to anonymous") {
			t.Errorf("stderr = %q, want anonymous fallback warning", stderrBuf.String())
		}
	})
}

// TestResolveGerritAuth_LoopbackNotMintedInProduction checks that, without
// the test-only hook, a loopback Gerrit host is treated as non-Google: no
// luci-auth token is minted for it, while a static GERRIT_TOKEN still is.
func TestResolveGerritAuth_LoopbackNotMintedInProduction(t *testing.T) {
	prev := allowLoopbackGerritAuthForTests
	allowLoopbackGerritAuthForTests = false
	t.Cleanup(func() { allowLoopbackGerritAuthForTests = prev })

	var mints int
	WithHermeticAuth(t, AuthResolvers{
		GerritToken: func(ctx context.Context) (string, string, error) {
			mints++
			return "minted", "luci-auth", nil
		},
	})
	setAuthModeFlag(t, "googler")
	for _, host := range []string{"http://127.0.0.1:8080", "http://localhost:9", "http://[::1]:1"} {
		for _, mode := range []AuthMode{AuthModeGoogler, AuthModeCommunity} {
			res, err := resolveGerritAuth(t.Context(), host, mode)
			if err != nil || !res.unauthed || !errors.Is(res.cause, errNonGoogleGerritHost) {
				t.Errorf("resolveGerritAuth(%q, %s) = %+v, %v; want unauthed non-Google", host, mode, res, err)
			}
		}
		u, _ := url.Parse(host + "/a/changes/1")
		if gerritTrustedURL(hostnameOf(host), false)(u) {
			t.Errorf("Dynamic Gerrit strategy for %q trusts %s, want false", host, u)
		}

		_, err := resolveGerritStrategy(t.Context(), host)
		if ExitCodeFor(err) != ExitCodeAuth || !strings.Contains(err.Error(), "not a Google-hosted Gerrit") {
			t.Errorf("resolveGerritStrategy(%q) in googler mode = %v, want ExitCodeAuth for non-Google host", host, err)
		}
	}
	if mints != 0 {
		t.Errorf("resolver calls = %d, want 0", mints)
	}

	t.Setenv("GERRIT_TOKEN", "static")
	res, err := resolveGerritAuth(t.Context(), "http://127.0.0.1:8080", AuthModeGoogler)
	if err != nil || res.token != "static" || res.dynamic {
		t.Errorf("static GERRIT_TOKEN to loopback = %+v, %v; want static token", res, err)
	}
}

// gerritPolicyHosts are the Gerrit URLs TestResolveGerritClientAuth_Policy runs against.
var gerritPolicyHosts = map[string]string{
	"google":     "https://pigweed-review.googlesource.com/a",
	"non-google": "https://gerrit.example.com",
	"spoof":      "https://x.googlesource.com.evil.com",
	"sso-url":    "sso://evil/x",
	// Loopback only receives minted tokens under the test hook that
	// TestMain sets; see TestResolveGerritAuth_LoopbackNotMintedInProduction.
	"loopback": "http://127.0.0.1:1",
}

type gerritPolicyCase struct {
	name        string
	mode        string
	envToken    string
	host        string // key of gerritPolicyHosts
	resolverErr bool
	want        GerritAuth
	wantDesc    string
	wantMints   int
	wantErr     string // substring; "" means success
}

var gerritPolicyCases = []gerritPolicyCase{
	{name: "googler/token/google", mode: "googler", envToken: "  tok  ", host: "google", want: GerritAuth{StaticToken: "tok"}, wantDesc: "token (GERRIT_TOKEN)"},
	{name: "googler/token/non-google", mode: "googler", envToken: "tok", host: "non-google", want: GerritAuth{StaticToken: "tok"}, wantDesc: "token (GERRIT_TOKEN)"},
	{name: "googler/luci/google", mode: "googler", host: "google", want: GerritAuth{Dynamic: true, modeReason: "--auth-mode=googler"}, wantDesc: "token (luci-auth)", wantMints: 1},
	{name: "googler/luci-fails/google", mode: "googler", host: "google", resolverErr: true, wantDesc: "none", wantMints: 1, wantErr: "authentication required in googler mode"},
	{name: "googler/no-token/non-google", mode: "googler", host: "non-google", wantDesc: "none", wantErr: "not a Google-hosted Gerrit"},
	{name: "community/token/google", mode: "community", envToken: "tok", host: "google", want: GerritAuth{StaticToken: "tok", AllowAnonymousFallback: true}, wantDesc: "token (GERRIT_TOKEN)"},
	{name: "community/token/non-google", mode: "community", envToken: "tok", host: "non-google", want: GerritAuth{StaticToken: "tok", AllowAnonymousFallback: true}, wantDesc: "token (GERRIT_TOKEN)"},
	{name: "community/luci/google", mode: "community", host: "google", want: GerritAuth{Dynamic: true, AllowAnonymousFallback: true, modeReason: "--auth-mode=community"}, wantDesc: "token (luci-auth)", wantMints: 1},
	{name: "community/luci-fails/google", mode: "community", host: "google", resolverErr: true, want: GerritAuth{AllowAnonymousFallback: true}, wantDesc: "none", wantMints: 1},
	{name: "community/no-token/non-google", mode: "community", host: "non-google", want: GerritAuth{AllowAnonymousFallback: true}, wantDesc: "none"},
	{name: "none/token/google", mode: "none", envToken: "tok", host: "google", want: GerritAuth{AllowAnonymousFallback: true}, wantDesc: "disabled (--auth-mode=none)"},
	{name: "none/token/non-google", mode: "none", envToken: "tok", host: "non-google", want: GerritAuth{AllowAnonymousFallback: true}, wantDesc: "disabled (--auth-mode=none)"},
	{name: "none/no-token/google", mode: "none", host: "google", want: GerritAuth{AllowAnonymousFallback: true}, wantDesc: "disabled (--auth-mode=none)"},
	{name: "none/no-token/non-google", mode: "none", host: "non-google", want: GerritAuth{AllowAnonymousFallback: true}, wantDesc: "disabled (--auth-mode=none)"},
	{name: "googler/basic-auth/google", mode: "googler", envToken: "user:pass", host: "google", wantDesc: "token (GERRIT_TOKEN)", wantErr: "username:password"},
	{name: "community/basic-auth/google", mode: "community", envToken: "user:pass", host: "google", wantDesc: "token (GERRIT_TOKEN)", wantErr: "username:password"},
	{name: "googler/no-token/spoof", mode: "googler", host: "spoof", wantDesc: "none", wantErr: "not a Google-hosted Gerrit"},
	{name: "community/no-token/spoof", mode: "community", host: "spoof", want: GerritAuth{AllowAnonymousFallback: true}, wantDesc: "none"},
	{name: "community/no-token/sso-url", mode: "community", host: "sso-url", want: GerritAuth{AllowAnonymousFallback: true}, wantDesc: "none"},
	{name: "googler/luci/loopback", mode: "googler", host: "loopback", want: GerritAuth{Dynamic: true, modeReason: "--auth-mode=googler"}, wantDesc: "token (luci-auth)", wantMints: 1},
	{name: "community/luci/loopback", mode: "community", host: "loopback", want: GerritAuth{Dynamic: true, AllowAnonymousFallback: true, modeReason: "--auth-mode=community"}, wantDesc: "token (luci-auth)", wantMints: 1},
}

func withHostRoleForTest(t *testing.T, goos, lsbRelease string, hasLSBFile bool) {
	t.Helper()
	prevGOOS, prevPath := runtimeGOOS, lsbReleasePath
	t.Cleanup(func() {
		runtimeGOOS = prevGOOS
		lsbReleasePath = prevPath
		laptopRoleCache.ResetMemory()
	})
	runtimeGOOS = goos
	if !hasLSBFile {
		lsbReleasePath = filepath.Join(t.TempDir(), "missing-lsb-release")
	} else {
		p := filepath.Join(t.TempDir(), "lsb-release")
		if err := os.WriteFile(p, []byte(lsbRelease), 0o600); err != nil {
			t.Fatalf("os.WriteFile(%q): %v", p, err)
		}
		lsbReleasePath = p
	}
	laptopRoleCache.ResetMemory()
}

func setupGerritPolicyCase(t *testing.T, tc gerritPolicyCase) *int {
	t.Helper()
	mints := new(int)
	WithHermeticAuth(t, AuthResolvers{
		GerritToken: func(ctx context.Context) (string, string, error) {
			*mints++
			if tc.resolverErr {
				return "", "", errors.New("luci-auth not found (searched PATH, /x/luci-auth)")
			}
			return "minted", "luci-auth", nil
		},
	})
	setAuthModeFlag(t, tc.mode)
	t.Setenv("GERRIT_TOKEN", tc.envToken)
	return mints
}

func TestResolveGerritClientAuth_Policy(t *testing.T) {
	for _, tc := range gerritPolicyCases {
		t.Run(tc.name, func(t *testing.T) {
			mints := setupGerritPolicyCase(t, tc)
			hostURL := gerritPolicyHosts[tc.host]
			strategy, err := resolveGerritStrategy(t.Context(), hostURL)
			if *mints != tc.wantMints {
				t.Errorf("resolver calls = %d, want %d", *mints, tc.wantMints)
			}
			res, _ := resolveGerritAuth(t.Context(), hostURL, AuthMode(tc.mode))
			wantMethod := AuthMethodNone
			if strings.HasPrefix(tc.wantDesc, "token ") {
				wantMethod = AuthMethodToken
			}
			if res.method != wantMethod || res.desc != tc.wantDesc {
				t.Errorf("resolveGerritAuth = (method=%q, desc=%q), want (%q, %q)", res.method, res.desc, wantMethod, tc.wantDesc)
			}
			if tc.wantErr != "" {
				if err == nil || !strings.Contains(err.Error(), tc.wantErr) {
					t.Fatalf("err = %v, want it to contain %q", err, tc.wantErr)
				}
				if ExitCodeFor(err) != ExitCodeAuth {
					t.Errorf("exit code = %d, want %d", ExitCodeFor(err), ExitCodeAuth)
				}
				if tc.resolverErr {
					if !strings.Contains(firstLine(err.Error()), "luci-auth not found (searched PATH, /x/luci-auth)") {
						t.Errorf("first line of error = %q, want the resolver cause", firstLine(err.Error()))
					}
					if !strings.Contains(err.Error(), "luci-auth login -scopes") || !strings.Contains(err.Error(), "GERRIT_TOKEN") || !strings.Contains(err.Error(), "gh auth status") {
						t.Errorf("err = %v, want luci-auth login, GERRIT_TOKEN, and gh auth status remediation", err)
					}
					if strings.Contains(err.Error(), "gcert") {
						t.Errorf("err = %v, did not expect gcert without sso-cred-helper", err)
					}
				}
				return
			}
			if err != nil {
				t.Fatalf("resolveGerritStrategy = %v, want nil", err)
			}
			gs, ok := strategy.(*gerritBearerStrategy)
			if !ok || gs == nil {
				t.Fatalf("resolveGerritStrategy = %T, want *gerritBearerStrategy", strategy)
			}
			wantCreds := tc.want.StaticToken != "" || tc.want.Dynamic
			if gs.HasCredentials() != wantCreds || (gs.bearer.invalidate != nil) != tc.want.Dynamic || gs.allowAnonymousFallback != tc.want.AllowAnonymousFallback {
				t.Fatalf("strategy = (creds=%v, dynamic=%v, anon=%v), want (%+v)", gs.HasCredentials(), gs.bearer.invalidate != nil, gs.allowAnonymousFallback, tc.want)
			}

			if sMethod, sDesc := strategy.Describe(); sMethod != wantMethod || sDesc != tc.wantDesc {
				t.Errorf("strategy.Describe() = (%q, %q), want (%q, %q)", sMethod, sDesc, wantMethod, tc.wantDesc)
			}
			for _, firstStatus := range []int{http.StatusOK, http.StatusUnauthorized} {
				iterMints := setupGerritPolicyCase(t, tc)
				var reqs []*http.Request
				rec := &mockTransport{roundTrip: func(req *http.Request) (*http.Response, error) {
					reqs = append(reqs, req)
					if len(reqs) == 1 && firstStatus != http.StatusOK {
						return gerritStatusTransport(firstStatus, "Unauthorized", nil).roundTrip(req)
					}
					return okGerritResponse(req, `{"_number":1}`), nil
				}}
				client, err := newGerritClientForURL(t.Context(), hostURL, &http.Client{Transport: rec})
				if err != nil {
					t.Fatalf("newGerritClientForURL: %v", err)
				}
				client.stderr = io.Discard
				_, err = client.GetChange(t.Context(), "1")
				if firstStatus == http.StatusOK && *iterMints != tc.wantMints {
					t.Errorf("first=200 resolver calls across resolveGerritStrategy + GetChange = %d, want %d", *iterMints, tc.wantMints)
				}

				wantAuth := ""
				if tc.want.StaticToken != "" {
					wantAuth = "Bearer " + tc.want.StaticToken
				} else if tc.want.Dynamic {
					wantAuth = "Bearer minted"
				}
				wantA := tc.host == "google" && wantAuth != ""
				if got := reqs[0].Header.Get("Authorization"); got != wantAuth {
					t.Errorf("first=%d Authorization = %q, want %q", firstStatus, got, wantAuth)
				}
				if got := hasAuthenticatedPrefix(reqs[0].URL); got != wantA {
					t.Errorf("first=%d path %q: has /a/ = %v, want %v", firstStatus, reqs[0].URL.Path, got, wantA)
				}
				wantRetry := firstStatus != http.StatusOK && tc.want.AllowAnonymousFallback && wantA
				wantReqs := 1
				if wantRetry {
					wantReqs = 2
				}
				if len(reqs) != wantReqs {
					t.Fatalf("first=%d reqs=%d, want %d (wantRetry=%v)", firstStatus, len(reqs), wantReqs, wantRetry)
				}
				wantSuccess := firstStatus == http.StatusOK || wantRetry
				if (err == nil) != wantSuccess {
					t.Fatalf("first=%d err=%v, wantSuccess=%v", firstStatus, err, wantSuccess)
				}
				if wantRetry && (reqs[1].Header.Get("Authorization") != "" || hasAuthenticatedPrefix(reqs[1].URL)) {
					t.Errorf("anonymous retry: auth=%q path=%q", reqs[1].Header.Get("Authorization"), reqs[1].URL.Path)
				}
				// Verify initial token is consumed once and subsequent requests re-query the resolver.
				if firstStatus == http.StatusOK && tc.mode == "community" && tc.want.Dynamic {
					if _, err := client.GetChange(t.Context(), "1"); err != nil || *iterMints != 2 {
						t.Errorf("second GetChange err=%v, resolver calls=%d, want nil and 2", err, *iterMints)
					}
				}
			}
		})
	}
}

func TestIsGerritTokenAuthRejection(t *testing.T) {
	if isGerritTokenAuthRejection(nil) {
		t.Error("isGerritTokenAuthRejection(nil) = true, want false")
	}
	for _, tc := range []struct {
		name    string
		status  int
		wwwAuth string
		body    string
		want    bool
	}{
		{"200 OK", http.StatusOK, "", "ok", false},
		{"401 Unauthorized", http.StatusUnauthorized, "", "unauthorized", true},
		{"403 ACL denial", http.StatusForbidden, "", "submit not permitted", false},
		{"403 insufficient_scope header", http.StatusForbidden, `Bearer error="insufficient_scope"`, "forbidden", true},
		{"403 invalid_token header", http.StatusForbidden, `Bearer error="invalid_token"`, "forbidden", true},
		{"403 authentication required body", http.StatusForbidden, "", "Authentication required\n", true},
		{"403 access_token_scope_insufficient body", http.StatusForbidden, "", "ACCESS_TOKEN_SCOPE_INSUFFICIENT: insufficient authentication scopes", true},
		{"403 large body preserved on rejection", http.StatusForbidden, "", "Authentication required\n" + strings.Repeat("x", 5000), true},
		{"403 large body preserved on non-rejection", http.StatusForbidden, "", "submit not permitted\n" + strings.Repeat("y", 5000), false},
	} {
		hdr := make(http.Header)
		if tc.wwwAuth != "" {
			hdr.Set("WWW-Authenticate", tc.wwwAuth)
		}
		resp := &http.Response{
			StatusCode: tc.status,
			Header:     hdr,
			Body:       io.NopCloser(strings.NewReader(tc.body)),
		}
		if got := isGerritTokenAuthRejection(resp); got != tc.want {
			t.Errorf("%s: isGerritTokenAuthRejection() = %v, want %v", tc.name, got, tc.want)
		}
		gotBody, err := io.ReadAll(resp.Body)
		_ = resp.Body.Close()
		if err != nil || string(gotBody) != tc.body {
			t.Errorf("%s: body after inspection = %q (err=%v), want %q preserved", tc.name, string(gotBody), err, tc.body)
		}
	}
}

func TestResolveAuthMode(t *testing.T) {
	ctx := context.Background()
	WithHermeticAuth(t, AuthResolvers{})
	setAuthModeFlag(t, "")

	t.Run("explicit flag googler", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{})
		setAuthModeFlag(t, "googler")
		mode, reason, err := ResolveAuthMode(ctx, nil)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if mode != AuthModeGoogler || !strings.Contains(reason, "--auth-mode=googler") {
			t.Errorf("got (%q, %q), want (googler, --auth-mode=googler)", mode, reason)
		}
		if _, ok := authModeCache.Get(authModeCacheKey{flag: "googler"}); ok {
			t.Error("expected nil-cfg call not to populate authModeCache")
		}
	})

	t.Run("explicit flag invalid", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{})
		setAuthModeFlag(t, "bogus")
		_, _, err := ResolveAuthMode(ctx, nil)
		if err == nil {
			t.Fatal("expected error for invalid auth mode")
		}
		if ExitCodeFor(err) != ExitCodeAuth {
			t.Errorf("exit code = %d, want %d", ExitCodeFor(err), ExitCodeAuth)
		}
	})

	t.Run("env var community", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{})
		t.Setenv("GH_ISH_AUTH_MODE", "community")
		mode, reason, err := ResolveAuthMode(ctx, nil)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if mode != AuthModeCommunity || !strings.Contains(reason, "GH_ISH_AUTH_MODE=community") {
			t.Errorf("got (%q, %q), want (community, GH_ISH_AUTH_MODE=community)", mode, reason)
		}
	})

	t.Run("nil cfg falls back to Config on ctx and caches it", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{})
		mockGit := &MockGitRunner{}
		mockGit.OnCommand("config --get ghish.authmode", "googler\n")
		cfg := &Config{Git: mockGit}
		ctxWithCfg := context.WithValue(ctx, configKey, cfg)

		mode, reason, err := ResolveAuthMode(ctxWithCfg, nil)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if mode != AuthModeGoogler || reason != "git config ghish.authmode=googler" {
			t.Errorf("got (%q, %q), want (googler, \"git config ghish.authmode=googler\")", mode, reason)
		}
		cached, ok := authModeCache.Get(authModeCacheKey{cfg: cfg})
		if !ok || cached.Mode != AuthModeGoogler {
			t.Errorf("authModeCache.Get(cfg=%p) = (%+v, %v), want (googler, true)", cfg, cached, ok)
		}
	})

	t.Run("auto detects googler via gosso on PATH", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{LookPath: onPath("gosso", "/usr/local/bin/gosso")})
		mode, reason, err := ResolveAuthMode(ctx, nil)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if mode != AuthModeGoogler || reason != "auto-detected (gosso on PATH)" {
			t.Errorf("got (%q, %q), want (googler, auto-detected (gosso on PATH))", mode, reason)
		}
	})

	t.Run("auto detects googler via @google.com git email", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{})
		mockGit := &MockGitRunner{}
		mockGit.OnCommand("config --get user.email", "keir@google.com\n")
		cfg := &Config{Git: mockGit}

		mode, reason, err := ResolveAuthMode(ctx, cfg)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if mode != AuthModeGoogler || !strings.Contains(reason, "keir@google.com") {
			t.Errorf("got (%q, %q), want (googler, keir@google.com)", mode, reason)
		}
	})

	for _, tc := range []struct {
		remote     string
		wantReason string
	}{
		{"sso://pigweed/pigweed/pigweed\n", "sso:// git remote"},
		{"rpc://pigweed/pigweed/pigweed\n", "rpc:// git remote"},
	} {
		t.Run("auto detects googler via "+tc.wantReason, func(t *testing.T) {
			WithHermeticAuth(t, AuthResolvers{})
			mockGit := &MockGitRunner{}
			mockGit.OnCommand("config --get user.email", "contributor@example.com\n").
				OnCommand("config --get remote.origin.url", tc.remote)
			cfg := &Config{Git: mockGit}

			mode, reason, err := ResolveAuthMode(ctx, cfg)
			if err != nil {
				t.Fatalf("unexpected error: %v", err)
			}
			if mode != AuthModeGoogler || !strings.Contains(reason, tc.wantReason) {
				t.Errorf("got (%q, %q), want (googler, %s)", mode, reason, tc.wantReason)
			}
		})
	}

	t.Run("auto detects googler via sso-cred-helper", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{LookPath: onPath(ssoCredHelperPath, ssoCredHelperPath)})
		mockGit := &MockGitRunner{}
		mockGit.OnCommand("config --get user.email", "contributor@example.com\n")
		cfg := &Config{Git: mockGit}

		mode, reason, err := ResolveAuthMode(ctx, cfg)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		wantReason := fmt.Sprintf("auto-detected (%s present)", ssoCredHelperPath)
		if mode != AuthModeGoogler || reason != wantReason {
			t.Errorf("got (%q, %q), want (googler, %q)", mode, reason, wantReason)
		}
	})

	t.Run("auto detects community when no corp signals", func(t *testing.T) {
		// gob-curl on PATH alone is no longer a corp auto-detection signal.
		WithHermeticAuth(t, AuthResolvers{LookPath: onPath("gob-curl", "/usr/bin/gob-curl")})
		mockGit := &MockGitRunner{}
		mockGit.OnCommand("config --get user.email", "contributor@example.com\n")
		cfg := &Config{Git: mockGit}

		mode, reason, err := ResolveAuthMode(ctx, cfg)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if mode != AuthModeCommunity || reason != "auto-detected (community environment)" {
			t.Errorf("got (%q, %q), want (community, \"auto-detected (community environment)\")", mode, reason)
		}
	})
}

func TestFindLuciAuthBinary_IgnoresAmbientCwdAndGitRepo(t *testing.T) {
	repoDir := filepath.Join(t.TempDir(), "untrusted-repo")
	mainRepoRoot := filepath.Join(t.TempDir(), "pigweed-main")
	t.Setenv("PW_ENVIRONMENT_ROOT", "")
	t.Setenv("BUILD_WORKSPACE_DIRECTORY", "")
	t.Setenv(scriptDirEnvVar, "")
	if err := os.MkdirAll(repoDir, 0o755); err != nil {
		t.Fatal(err)
	}
	t.Chdir(repoDir)

	untrustedBins := map[string]bool{
		filepath.Join(repoDir, "environment", "cipd", "packages", "luci", "luci-auth"):      true,
		filepath.Join(repoDir, ".environment", "cipd", "packages", "luci", "luci-auth"):     true,
		filepath.Join(mainRepoRoot, "environment", "cipd", "packages", "luci", "luci-auth"): true,
	}

	WithHermeticAuth(t, AuthResolvers{
		LookPath: func(file string) (string, error) {
			if untrustedBins[file] {
				return file, nil
			}
			return "", os.ErrNotExist
		},
	})

	mockGit := &MockGitRunner{}
	mockGit.OnCommand("rev-parse --git-common-dir", filepath.Join(mainRepoRoot, ".git")+"\n")
	ctx := context.WithValue(t.Context(), configKey, &Config{Git: mockGit})

	if got := findLuciAuthBinary(ctx); got != "" {
		t.Errorf("findLuciAuthBinary() = %q, want empty (ambient cwd and --git-common-dir must not be searched)", got)
	}
	if len(mockGit.Calls) != 0 {
		t.Errorf("expected findLuciAuthBinary not to run git rev-parse --git-common-dir, got: %v", mockGit.Calls)
	}
}

func TestFindLuciAuthBinary_ScriptDirFallback(t *testing.T) {
	otherRepoDir := filepath.Join(t.TempDir(), "other-repo")
	pwScriptDir := filepath.Join(t.TempDir(), "pigweed-checkout")
	expectedLuciAuth := filepath.Join(pwScriptDir, "environment", "cipd", "packages", "luci", "luci-auth")

	t.Setenv("PW_ENVIRONMENT_ROOT", "")
	t.Setenv("BUILD_WORKSPACE_DIRECTORY", otherRepoDir)
	t.Setenv(scriptDirEnvVar, pwScriptDir)

	WithHermeticAuth(t, AuthResolvers{
		LookPath: onPath(expectedLuciAuth, expectedLuciAuth),
	})

	got := findLuciAuthBinary(t.Context())
	if got != expectedLuciAuth {
		t.Errorf("findLuciAuthBinary() = %q, want %q", got, expectedLuciAuth)
	}
}

func TestParseOAuthTokenOutput(t *testing.T) {
	now := time.Date(2026, 10, 29, 6, 0, 0, 0, time.UTC)
	expUnix := now.Add(45 * time.Minute).Unix()
	for _, tc := range []struct {
		name    string
		raw     []byte
		wantTok string
		wantExp time.Time
	}{
		{
			name:    "JSON output subtracts 60s safety margin",
			raw:     fmt.Appendf(nil, `{"token":"json-tok","expiry":%d}`, expUnix),
			wantTok: "json-tok",
			wantExp: time.Unix(expUnix, 0).Add(-60 * time.Second),
		},
		{
			name:    "plain text falls back to default TTL",
			raw:     []byte("  plain-tok\n"),
			wantTok: "plain-tok",
			wantExp: now.Add(defaultOAuthTokenTTL),
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			tok, exp := parseOAuthTokenOutput(tc.raw, now)
			if tok != tc.wantTok || !exp.Equal(tc.wantExp) {
				t.Errorf("parseOAuthTokenOutput() = (%q, %v), want (%q, %v)", tok, exp, tc.wantTok, tc.wantExp)
			}
		})
	}
}

func TestDefaultGerritToken_ReusesMintedToken(t *testing.T) {
	var runnerCalls int
	var gotArgs []string
	WithHermeticAuth(t, AuthResolvers{
		LookPath: onPath("luci-auth", "/usr/bin/luci-auth"),
		CommandRunner: func(ctx context.Context, name string, args ...string) ([]byte, error) {
			runnerCalls++
			gotArgs = args
			return fmt.Appendf(nil, `{"token":"gerrit-json-token-%d","expiry":%d}`, runnerCalls, time.Now().Add(45*time.Minute).Unix()), nil
		},
	})
	setAuthModeFlag(t, "googler")

	for range 2 {
		tok, src, err := DefaultGerritToken(t.Context())
		if err != nil || tok != "gerrit-json-token-1" || src != "luci-auth" {
			t.Fatalf("DefaultGerritToken() = (%q, %q, %v), want (gerrit-json-token-1, luci-auth, nil)", tok, src, err)
		}
	}
	if runnerCalls != 1 {
		t.Errorf("OAuthTokenCommandRunner calls = %d, want 1", runnerCalls)
	}
	if want := []string{"token", "-scopes", strings.Join(luciAuthScopes(AuthModeGoogler), " "), "-json-output", "-"}; !slices.Equal(gotArgs, want) {
		t.Errorf("luci-auth args = %q, want %q", gotArgs, want)
	}
}

func TestDefaultGerritToken_ModeScopes(t *testing.T) {
	wantExpiry := time.Now().Add(30 * time.Minute).Unix()
	var gotScopes []string
	WithHermeticAuth(t, AuthResolvers{
		LookPath: onPath("luci-auth", "/usr/bin/luci-auth"),
		CommandRunner: func(ctx context.Context, name string, args ...string) ([]byte, error) {
			if idx := slices.Index(args, "-scopes"); idx >= 0 && idx+1 < len(args) {
				gotScopes = append(gotScopes, args[idx+1])
			}
			return fmt.Appendf(nil, `{"token":"tok-%d","expiry":%d}`, len(gotScopes), wantExpiry), nil
		},
	})

	t.Setenv("GH_ISH_AUTH_MODE", "community")
	if tok, src, err := DefaultGerritToken(t.Context()); err != nil || tok != "tok-1" || src != "luci-auth" {
		t.Fatalf("community DefaultGerritToken() = (%q, %q, %v), want (tok-1, luci-auth, nil)", tok, src, err)
	}
	t.Setenv("GH_ISH_AUTH_MODE", "googler")
	if tok, src, err := DefaultGerritToken(t.Context()); err != nil || tok != "tok-2" || src != "luci-auth" {
		t.Fatalf("googler DefaultGerritToken() = (%q, %q, %v), want (tok-2, luci-auth, nil)", tok, src, err)
	}
	t.Setenv("GH_ISH_AUTH_MODE", "none")
	if _, _, err := DefaultGerritToken(t.Context()); !errors.Is(err, errLUCIAuthDisabled) {
		t.Fatalf("none DefaultGerritToken() err = %v, want %v", err, errLUCIAuthDisabled)
	}

	wantScopes := []string{
		strings.Join(luciAuthScopes(AuthModeCommunity), " "),
		strings.Join(luciAuthScopes(AuthModeGoogler), " "),
	}
	if !slices.Equal(gotScopes, wantScopes) {
		t.Errorf("probed scopes = %q, want %q", gotScopes, wantScopes)
	}
}

func TestOAuthTokenCommand_AutoConfiguresSSOCredHelper(t *testing.T) {
	tests := []struct {
		name           string
		goos           string
		lsbRelease     string
		hasLSBFile     bool
		hasHelper      bool
		lookPathErr    error
		envSet         bool
		envVal         string
		bin            string
		args           []string
		wantLaptop     bool
		wantInjectedTo string
	}{
		{
			name:           "injects LUCI_AUTH_CREDENTIAL_HELPER when unset and helper exists",
			hasHelper:      true,
			bin:            "luci-auth",
			args:           []string{"token"},
			wantInjectedTo: ssoCredHelperPath,
		},
		{
			name:           "injects for resolved absolute luci-auth path on GOOGLE_ROLE=desktop",
			lsbRelease:     "DISTRIB_ID=gLinux\nGOOGLE_ROLE=desktop\n",
			hasLSBFile:     true,
			hasHelper:      true,
			bin:            "/opt/cipd/packages/luci/luci-auth",
			args:           []string{"token"},
			wantInjectedTo: ssoCredHelperPath,
		},
		{
			name:       "skips injection on GOOGLE_ROLE=laptop",
			lsbRelease: "DISTRIB_ID=gLinux\nGOOGLE_ROLE=\"laptop\"\n",
			hasLSBFile: true,
			hasHelper:  true,
			bin:        "luci-auth",
			args:       []string{"token"},
			wantLaptop: true,
		},
		{
			name:       "darwin ignores /etc/lsb-release and skips Linux sso-cred-helper",
			goos:       "darwin",
			lsbRelease: "GOOGLE_ROLE=laptop\n",
			hasLSBFile: true,
			hasHelper:  true,
			bin:        "luci-auth",
			args:       []string{"token"},
		},
		{
			name:      "preserves existing LUCI_AUTH_CREDENTIAL_HELPER when already set",
			hasHelper: true,
			envSet:    true,
			envVal:    "/custom/cred-helper",
			bin:       "luci-auth",
			args:      []string{"token"},
		},
		{
			name:      "respects explicit empty LUCI_AUTH_CREDENTIAL_HELPER opt-out",
			hasHelper: true,
			envSet:    true,
			envVal:    "",
			bin:       "luci-auth",
			args:      []string{"token"},
		},
		{
			name:      "does not inject when helper binary is missing on disk",
			hasHelper: false,
			bin:       "luci-auth",
			args:      []string{"token"},
		},
		{
			name:        "does not inject when LookPath returns permission error",
			lookPathErr: os.ErrPermission,
			bin:         "luci-auth",
			args:        []string{"token"},
		},
		{
			name:      "does not inject for non-luci-auth commands",
			hasHelper: true,
			bin:       "gcloud",
			args:      []string{"auth", "print-access-token"},
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			goos := tc.goos
			if goos == "" {
				goos = "linux"
			}
			withHostRoleForTest(t, goos, tc.lsbRelease, tc.hasLSBFile)
			WithHermeticAuth(t, AuthResolvers{
				LookPath: func(file string) (string, error) {
					if tc.lookPathErr != nil {
						return "", tc.lookPathErr
					}
					if tc.hasHelper && file == ssoCredHelperPath {
						return ssoCredHelperPath, nil
					}
					return "", os.ErrNotExist
				},
			})
			t.Setenv("GHISH_PARENT_ENV_CANARY", "preserved")
			t.Setenv("LUCI_AUTH_CREDENTIAL_HELPER", tc.envVal)
			if !tc.envSet {
				_ = os.Unsetenv("LUCI_AUTH_CREDENTIAL_HELPER")
			}

			if got := isLaptopRole(); got != tc.wantLaptop {
				t.Errorf("isLaptopRole() = %v, want %v", got, tc.wantLaptop)
			}

			cmd := newOAuthTokenCommand(t.Context(), tc.bin, tc.args...)
			if tc.wantInjectedTo == "" {
				if cmd.Env != nil {
					t.Errorf("cmd.Env = %v, want nil", cmd.Env)
				}
				if tc.envSet && !slices.Contains(cmd.Environ(), "LUCI_AUTH_CREDENTIAL_HELPER="+tc.envVal) {
					t.Errorf("cmd.Environ() missing preserved LUCI_AUTH_CREDENTIAL_HELPER=%q", tc.envVal)
				}
				return
			}
			wantEntry := "LUCI_AUTH_CREDENTIAL_HELPER=" + tc.wantInjectedTo
			if !slices.Contains(cmd.Env, wantEntry) {
				t.Errorf("cmd.Env missing %q: %v", wantEntry, cmd.Env)
			}
			if !slices.Contains(cmd.Env, "GHISH_PARENT_ENV_CANARY=preserved") {
				t.Errorf("cmd.Env lost parent environment variables: %v", cmd.Env)
			}
		})
	}

	t.Run("remediation and auth-mode auto-detection respect host role", func(t *testing.T) {
		for _, tc := range []struct {
			name      string
			goos      string
			lsb       string
			hasHelper bool
			wantMode  AuthMode
			wantHint  bool
		}{
			{"LOAS linux", "linux", "GOOGLE_ROLE=desktop\n", true, AuthModeGoogler, true},
			{"external linux without helper", "linux", "", false, AuthModeCommunity, false},
			{"laptop linux", "linux", "GOOGLE_ROLE='laptop'\n", true, AuthModeGoogler, false},
			{"darwin", "darwin", "GOOGLE_ROLE=laptop\n", true, AuthModeCommunity, false},
		} {
			t.Run(tc.name, func(t *testing.T) {
				withHostRoleForTest(t, tc.goos, tc.lsb, tc.lsb != "")
				WithHermeticAuth(t, AuthResolvers{
					LookPath: func(file string) (string, error) {
						if tc.hasHelper && file == ssoCredHelperPath {
							return ssoCredHelperPath, nil
						}
						return "", os.ErrNotExist
					},
				})
				mockGit := &MockGitRunner{}
				mockGit.OnCommand("config --get user.email", "contributor@example.com\n")
				mode, _, err := ResolveAuthMode(t.Context(), &Config{Git: mockGit})
				if err != nil || mode != tc.wantMode {
					t.Errorf("ResolveAuthMode() = (%q, %v), want %q", mode, err, tc.wantMode)
				}
				step1 := luciAuthTokenRemediationStep1("luci-auth login")
				if got := strings.Contains(step1, "gcert"); got != tc.wantHint {
					t.Errorf("remediation mentions gcert = %v, want %v:\n%s", got, tc.wantHint, step1)
				}
				if strings.Contains(step1, "sso-cred-helper") {
					t.Errorf("user-facing remediation unexpectedly mentions sso-cred-helper:\n%s", step1)
				}
			})
		}
	})
}

func TestResolveAuthMode_RemovedAuthMethodEnv(t *testing.T) {
	WithHermeticAuth(t, AuthResolvers{})
	// An explicit --auth-mode flag does not bypass the GH_ISH_AUTH_METHOD check.
	setAuthModeFlag(t, "community")

	wantNoneErr := fmt.Sprintf(
		"GH_ISH_AUTH_METHOD=none is set, but GH_ISH_AUTH_METHOD has been removed.\n\n"+
			"gh-ish now picks Gerrit credentials automatically: GERRIT_TOKEN when set, otherwise luci-auth (for *.googlesource.com).\n"+
			"To disable authentication, use --auth-mode=none or GH_ISH_AUTH_MODE=none.\n\n"+
			"Remediation:\n"+
			"  1. Unset it: unset GH_ISH_AUTH_METHOD\n"+
			"  2. Run '%s' (for *.googlesource.com)\n"+
			"  3. Set an OAuth2 token: export GERRIT_TOKEN=\"<token>\"\n"+
			"  4. Check auth status: gh auth status",
		luciAuthLoginCommand(AuthModeCommunity))

	for _, raw := range []string{"gob-curl", "cookie", "token", "basic", "auto", "none", "  none  "} {
		t.Run(raw, func(t *testing.T) {
			t.Setenv("GH_ISH_AUTH_METHOD", raw)
			_, _, err := ResolveAuthMode(t.Context(), nil)
			if ExitCodeFor(err) != ExitCodeAuth || !strings.Contains(err.Error(), "GH_ISH_AUTH_METHOD="+strings.TrimSpace(raw)) || !strings.Contains(err.Error(), "unset GH_ISH_AUTH_METHOD") {
				t.Fatalf("ResolveAuthMode(GH_ISH_AUTH_METHOD=%q) = %v, want ExitCodeAuth mentioning GH_ISH_AUTH_METHOD", raw, err)
			}
			if raw == "none" && err.Error() != wantNoneErr {
				t.Errorf("ResolveAuthMode(GH_ISH_AUTH_METHOD=%q) error =\n%s\nwant:\n%s", raw, err.Error(), wantNoneErr)
			}
		})
	}

	for _, blank := range []string{"", "   "} {
		t.Run(fmt.Sprintf("unset/%q", blank), func(t *testing.T) {
			t.Setenv("GH_ISH_AUTH_METHOD", blank)
			mode, _, err := ResolveAuthMode(t.Context(), nil)
			if err != nil || mode != AuthModeCommunity {
				t.Errorf("ResolveAuthMode(GH_ISH_AUTH_METHOD=%q) = (%q, %v), want (community, nil)", blank, mode, err)
			}
		})
	}

	t.Run("primed cache still checks GH_ISH_AUTH_METHOD", func(t *testing.T) {
		cfg := &Config{Git: &MockGitRunner{}}
		if _, _, err := ResolveAuthMode(t.Context(), cfg); err != nil {
			t.Fatalf("initial ResolveAuthMode error = %v", err)
		}
		t.Setenv("GH_ISH_AUTH_METHOD", "none")
		if _, _, err := ResolveAuthMode(t.Context(), cfg); ExitCodeFor(err) != ExitCodeAuth {
			t.Errorf("ResolveAuthMode with primed cache = %v (exit %d), want ExitCodeAuth", err, ExitCodeFor(err))
		}
	})
}

func TestGerritGooglerAuthError(t *testing.T) {
	t.Run("Google host without sso-cred-helper", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{})
		err := gerritGooglerAuthError("git config ghish.authmode=googler", "https://pigweed-review.googlesource.com",
			errors.New("luci-auth not found (searched PATH, /x/luci-auth)"))
		if ExitCodeFor(err) != ExitCodeAuth {
			t.Errorf("exit code = %d, want %d", ExitCodeFor(err), ExitCodeAuth)
		}
		for _, want := range []string{
			"git config ghish.authmode=googler",
			"pigweed-review.googlesource.com",
			"luci-auth not found (searched PATH, /x/luci-auth)",
			luciAuthLoginCommand(AuthModeGoogler),
			"GERRIT_TOKEN",
			"gh auth status",
		} {
			if !strings.Contains(err.Error(), want) {
				t.Errorf("gerritGooglerAuthError() = %v, want substring %q", err, want)
			}
		}
		if strings.Contains(err.Error(), "gcert") {
			t.Errorf("did not expect gcert without sso-cred-helper, got: %v", err)
		}
	})

	t.Run("Google host with sso-cred-helper", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{LookPath: onPath(ssoCredHelperPath, ssoCredHelperPath)})
		err := gerritGooglerAuthError("git config ghish.authmode=googler", "https://pigweed-review.googlesource.com",
			errors.New("luci-auth: expired"))
		if !strings.Contains(err.Error(), "Run 'gcert' (with sso-cred-helper)") {
			t.Errorf("expected gcert remediation with sso-cred-helper, got: %v", err)
		}
	})

	t.Run("non-Google host offers only GERRIT_TOKEN", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{LookPath: onPath(ssoCredHelperPath, ssoCredHelperPath)})
		err := gerritGooglerAuthError("git config ghish.authmode=googler", "https://gerrit.example.org", errNonGoogleGerritHost)
		if ExitCodeFor(err) != ExitCodeAuth || !strings.Contains(err.Error(), "GERRIT_TOKEN") {
			t.Errorf("gerritGooglerAuthError(non-Google) = %v, want ExitCodeAuth with GERRIT_TOKEN", err)
		}
		if strings.Contains(err.Error(), "gcert") || strings.Contains(err.Error(), "luci-auth login") {
			t.Errorf("did not expect gcert or luci-auth login for non-Google host, got: %v", err)
		}
	})
}

func TestDefaultGerritToken_Errors(t *testing.T) {
	const bin = "/x/luci-auth"
	envRoot := filepath.Join(t.TempDir(), "pw-env")
	wsDir := filepath.Join(t.TempDir(), "ws")
	scriptDir := filepath.Join(t.TempDir(), "script")
	t.Chdir(t.TempDir())

	for _, tc := range []struct {
		name      string
		envRoot   string
		wsDir     string
		scriptDir string
		resolvers AuthResolvers
		want      string
	}{
		{
			name: "not found without env roots lists PATH",
			want: "luci-auth not found (searched PATH)",
		},
		{
			name:      "not found with explicit env roots lists search path",
			envRoot:   envRoot,
			wsDir:     wsDir,
			scriptDir: scriptDir,
			want: "luci-auth not found (searched PATH, " +
				filepath.Join(envRoot, "cipd", "packages", "luci", "luci-auth") + ", " +
				filepath.Join(envRoot, "cipd", "packages", "pigweed", "bin", "luci-auth") + ", " +
				filepath.Join(wsDir, "environment", "cipd", "packages", "luci", "luci-auth") + ", " +
				filepath.Join(wsDir, ".environment", "cipd", "packages", "luci", "luci-auth") + ", " +
				filepath.Join(scriptDir, "environment", "cipd", "packages", "luci", "luci-auth") + ", " +
				filepath.Join(scriptDir, ".environment", "cipd", "packages", "luci", "luci-auth") + ")",
		},
		{
			name: "not logged in",
			resolvers: AuthResolvers{
				LookPath: onPath("luci-auth", bin),
				CommandRunner: func(context.Context, string, ...string) ([]byte, error) {
					return nil, &exec.ExitError{Stderr: []byte("Not logged in.\n")}
				},
			},
			want: "luci-auth (" + bin + ") returned no OAuth2 token: Not logged in.",
		},
		{
			name: "wrapped ExitError uses first line of stderr",
			resolvers: AuthResolvers{
				LookPath: onPath("luci-auth", bin),
				CommandRunner: func(context.Context, string, ...string) ([]byte, error) {
					return nil, fmt.Errorf("running luci-auth: %w", &exec.ExitError{Stderr: []byte("reauth required\nrun luci-auth login\n")})
				},
			},
			want: "luci-auth (" + bin + ") returned no OAuth2 token: reauth required",
		},
		{
			name: "plain error falls back to err.Error()",
			resolvers: AuthResolvers{
				LookPath: onPath("luci-auth", bin),
				CommandRunner: func(context.Context, string, ...string) ([]byte, error) {
					return nil, errors.New("fork/exec /x/luci-auth: permission denied")
				},
			},
			want: "luci-auth (" + bin + ") returned no OAuth2 token: fork/exec /x/luci-auth: permission denied",
		},
		{
			name: "empty token output",
			resolvers: AuthResolvers{
				LookPath: onPath("luci-auth", bin),
				CommandRunner: func(context.Context, string, ...string) ([]byte, error) {
					return []byte(""), nil
				},
			},
			want: "luci-auth (" + bin + ") returned no OAuth2 token: empty token output",
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			t.Setenv("PW_ENVIRONMENT_ROOT", tc.envRoot)
			t.Setenv("BUILD_WORKSPACE_DIRECTORY", tc.wsDir)
			t.Setenv(scriptDirEnvVar, tc.scriptDir)
			WithHermeticAuth(t, tc.resolvers)
			ctx := t.Context()
			for i := range 2 {
				tok, _, err := DefaultGerritToken(ctx)
				if err == nil {
					t.Fatalf("call %d: DefaultGerritToken() = %q, nil; want error", i, tok)
				}
				if err.Error() != tc.want {
					t.Errorf("call %d: DefaultGerritToken() = (%q, %v), want error %q", i, tok, err, tc.want)
				}
			}
		})
	}
}

func TestDefaultGerritToken_RecoversAfterFailureAndInvalidation(t *testing.T) {
	const bin = "/x/luci-auth"
	var calls int
	binaryPresent := false
	shouldFail := true
	WithHermeticAuth(t, AuthResolvers{
		LookPath: func(file string) (string, error) {
			if binaryPresent && file == "luci-auth" {
				return bin, nil
			}
			return "", os.ErrNotExist
		},
		CommandRunner: func(context.Context, string, ...string) ([]byte, error) {
			calls++
			if shouldFail {
				return nil, &exec.ExitError{Stderr: []byte("Not logged in.\n")}
			}
			return fmt.Appendf(nil, "recovered-token-%d\n", calls), nil
		},
	})

	// Binary missing is cached in luciOAuthCache until ResetAuthTokenCaches().
	if _, _, err := DefaultGerritToken(t.Context()); !errors.Is(err, errLUCIAuthNotFound) {
		t.Fatalf("missing binary err = %v, want %v", err, errLUCIAuthNotFound)
	}
	binaryPresent = true
	if _, _, err := DefaultGerritToken(t.Context()); !errors.Is(err, errLUCIAuthNotFound) {
		t.Fatalf("cached missing binary err = %v, want %v", err, errLUCIAuthNotFound)
	}
	ResetAuthTokenCaches()

	// Transient login failure is not sticky; subsequent call recovers once logged in.
	if _, _, err := DefaultGerritToken(t.Context()); err == nil {
		t.Fatal("DefaultGerritToken() succeeded while not logged in, want error")
	}
	shouldFail = false
	tok, src, err := DefaultGerritToken(t.Context())
	if err != nil || tok != "recovered-token-2" || src != "luci-auth" {
		t.Fatalf("DefaultGerritToken() after transient failure = (%q, %q, %v), want (recovered-token-2, luci-auth, nil)", tok, src, err)
	}

	// Invalidating the rejected token triggers a fresh mint on the next call.
	invalidateLUCIAuthToken("recovered-token-2")
	tok2, _, err := DefaultGerritToken(t.Context())
	if err != nil || tok2 != "recovered-token-3" {
		t.Fatalf("DefaultGerritToken() after invalidation = (%q, %v), want (recovered-token-3, nil)", tok2, err)
	}
}

func TestBearerTokenStrategy_ConcurrentRequests(t *testing.T) {
	type workerKey struct{}
	var mu sync.Mutex
	mints := map[int]int{}
	seen := map[int][]string{}

	shared := &gerritBearerStrategy{
		bearer: NewDynamicBearerStrategy(
			"",
			func(u *url.URL) bool { return urlHostIn(u, "pigweed-review.googlesource.com") },
			func(ctx context.Context, _ *url.URL) (string, error) {
				id := ctx.Value(workerKey{}).(int)
				mu.Lock()
				defer mu.Unlock()
				mints[id]++
				return fmt.Sprintf("w%d-tok%d", id, mints[id]), nil
			},
			func(string) {},
		),
		allowAnonymousFallback: true,
	}
	hc := &http.Client{
		Transport: testRoundTripper(func(req *http.Request) (*http.Response, error) {
			id := req.Context().Value(workerKey{}).(int)
			mu.Lock()
			seen[id] = append(seen[id], req.Header.Get("Authorization")+"|"+req.URL.Path)
			step := len(seen[id])
			mu.Unlock()
			if (id%3 == 1 && step == 1) || (id%3 == 2 && strings.HasPrefix(req.URL.Path, "/a/")) {
				return gerritStatusTransport(http.StatusUnauthorized, "invalid_token", nil).roundTrip(req)
			}
			return okGerritResponse(req, "{}"), nil
		}),
	}

	var wg sync.WaitGroup
	for i := range 24 {
		wg.Go(func() {
			wCtx := withGerritStderr(context.WithValue(t.Context(), workerKey{}, i), io.Discard)
			u := fmt.Sprintf("https://pigweed-review.googlesource.com/a/changes/%d", i)
			resp, err := executeWithAuthStrategy(wCtx, hc, shared, func(reqCtx context.Context) (*http.Request, error) {
				return http.NewRequestWithContext(reqCtx, http.MethodGet, u, nil)
			})
			if err != nil {
				t.Errorf("worker %d err = %v", i, err)
				return
			}
			resp.Body.Close()
			want := [][]string{
				{fmt.Sprintf("Bearer w%d-tok1|/a/changes/%d", i, i)},
				{fmt.Sprintf("Bearer w%d-tok1|/a/changes/%d", i, i), fmt.Sprintf("Bearer w%d-tok2|/a/changes/%d", i, i)},
				{fmt.Sprintf("Bearer w%d-tok1|/a/changes/%d", i, i), fmt.Sprintf("Bearer w%d-tok2|/a/changes/%d", i, i), fmt.Sprintf("|/changes/%d", i)},
			}[i%3]
			mu.Lock()
			got := slices.Clone(seen[i])
			mu.Unlock()
			if !slices.Equal(got, want) {
				t.Errorf("worker %d seen = %q, want %q", i, got, want)
			}
		})
	}
	wg.Wait()
}

func TestBearerTokenStrategy_ClearsPreExistingAuthHeader(t *testing.T) {
	trustedOnly := NewStaticBearerStrategy("token", "trusted-tok", func(u *url.URL) bool {
		return urlHostIn(u, "trusted.example")
	})
	for _, tc := range []struct {
		name     string
		strategy *BearerTokenStrategy
		rawURL   string
		wantAuth string
	}{
		{
			name:     "unauthenticated strategy clears stale header",
			strategy: NewUnauthenticatedBearerStrategy("none"),
			rawURL:   "https://trusted.example/x",
		},
		{
			name:     "untrusted URL clears stale header",
			strategy: trustedOnly,
			rawURL:   "https://untrusted.example/x",
		},
		{
			name:     "trusted URL replaces stale header",
			strategy: trustedOnly,
			rawURL:   "https://trusted.example/x",
			wantAuth: "Bearer trusted-tok",
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			req, err := http.NewRequestWithContext(t.Context(), http.MethodGet, tc.rawURL, nil)
			if err != nil {
				t.Fatalf("NewRequestWithContext: %v", err)
			}
			req.Header.Set("Authorization", "Bearer stale-header")
			if err := tc.strategy.Prepare(t.Context(), req); err != nil {
				t.Fatalf("Prepare: %v", err)
			}
			if got := req.Header.Get("Authorization"); got != tc.wantAuth {
				t.Errorf("Authorization = %q, want %q", got, tc.wantAuth)
			}
		})
	}
}
