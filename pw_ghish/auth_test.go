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
		return &http.Request{URL: u, Header: h}
	}
	origin := []*http.Request{mustAuthedReq("https://pigweed-review.googlesource.com/a/changes/1")}

	t.Run("nil client creates safe client with no timeout", func(t *testing.T) {
		got := ensureSafeHTTPClient(nil)
		if got == nil || got.CheckRedirect == nil || got.Timeout != 0 {
			t.Fatalf("ensureSafeHTTPClient(nil) = %+v, want non-nil with CheckRedirect and Timeout=0", got)
		}
		req := mustAuthedReq("https://evil.example.com/blob")
		if err := got.CheckRedirect(req, origin); err != nil || req.Header.Get("Authorization") != "" {
			t.Errorf("cross-host redirect: err=%v, Authorization=%q; want nil and stripped", err, req.Header.Get("Authorization"))
		}
	})

	t.Run("client without CheckRedirect is shallow-copied without mutating input", func(t *testing.T) {
		tr := &mockTransport{}
		in := &http.Client{Transport: tr, Timeout: 15 * time.Second}
		got := ensureSafeHTTPClient(in)
		if got == in {
			t.Fatal("expected shallow copy of input client, got same pointer")
		}
		if in.CheckRedirect != nil {
			t.Error("input client CheckRedirect was mutated")
		}
		if got.Transport != tr || got.Timeout != 15*time.Second || got.CheckRedirect == nil {
			t.Fatalf("copied client = %+v, want Transport/Timeout preserved and CheckRedirect set", got)
		}
		req := mustAuthedReq("https://evil.example.com/blob")
		if err := got.CheckRedirect(req, origin); err != nil || req.Header.Get("Authorization") != "" {
			t.Errorf("cross-host redirect: err=%v, Authorization=%q; want nil and stripped", err, req.Header.Get("Authorization"))
		}
	})

	t.Run("client with custom CheckRedirect is preserved", func(t *testing.T) {
		var customCalled bool
		sentinel := errors.New("stop redirect")
		in := &http.Client{
			CheckRedirect: func(req *http.Request, via []*http.Request) error {
				customCalled = true
				return sentinel
			},
		}
		got := ensureSafeHTTPClient(in)
		if got != in {
			t.Fatalf("ensureSafeHTTPClient(custom) = %p, want %p", got, in)
		}
		req := mustAuthedReq("https://evil.example.com/blob")
		if err := got.CheckRedirect(req, origin); !errors.Is(err, sentinel) || !customCalled {
			t.Errorf("custom CheckRedirect: err=%v, customCalled=%v; want %v and true", err, customCalled, sentinel)
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
		client := NewGerritClientWithHTTP(host, nil, GerritAuth{Dynamic: true})
		u, _ := url.Parse(host + "/a/changes/1")
		if client.trusted(u) {
			t.Errorf("Dynamic client for %q trusts %s, want false", host, u)
		}

		_, err := resolveGerritClientAuth(t.Context(), host)
		if ExitCodeFor(err) != ExitCodeAuth || !strings.Contains(err.Error(), "not a Google-hosted Gerrit") {
			t.Errorf("resolveGerritClientAuth(%q) in googler mode = %v, want ExitCodeAuth for non-Google host", host, err)
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
			got, err := resolveGerritClientAuth(t.Context(), hostURL)
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
			if err != nil || got != tc.want {
				t.Fatalf("resolveGerritClientAuth = (%+v, %v), want (%+v, nil)", got, err, tc.want)
			}

			for _, firstStatus := range []int{http.StatusOK, http.StatusUnauthorized} {
				setupGerritPolicyCase(t, tc)
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
		authModeCacheMu.Lock()
		cached := authModeCacheValid
		authModeCacheMu.Unlock()
		if cached {
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

	t.Run("auto detects googler via sso_client on PATH", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{LookPath: onPath("sso_client", "/usr/bin/sso_client")})
		mode, reason, err := ResolveAuthMode(ctx, nil)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if mode != AuthModeGoogler || !strings.Contains(reason, "sso_client") {
			t.Errorf("got (%q, %q), want (googler, sso_client)", mode, reason)
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

func TestDefaultGerritToken_CachesJSONExpiry(t *testing.T) {
	wantExpiry := time.Now().Add(45 * time.Minute).Unix()
	var runnerCalls int
	var gotArgs []string
	WithHermeticAuth(t, AuthResolvers{
		LookPath: onPath("luci-auth", "/usr/bin/luci-auth"),
		CommandRunner: func(ctx context.Context, name string, args ...string) ([]byte, error) {
			runnerCalls++
			gotArgs = args
			return fmt.Appendf(nil, `{"token":"gerrit-json-token","expiry":%d}`, wantExpiry), nil
		},
	})

	for range 2 {
		tok, src, err := DefaultGerritToken(t.Context())
		if err != nil {
			t.Fatalf("DefaultGerritToken() error = %v", err)
		}
		if tok != "gerrit-json-token" || src != "luci-auth" {
			t.Errorf("DefaultGerritToken() = (%q, %q), want (gerrit-json-token, luci-auth)", tok, src)
		}
	}
	if runnerCalls != 1 {
		t.Errorf("OAuthTokenCommandRunner calls = %d, want 1", runnerCalls)
	}
	if want := []string{"token", "-scopes", DefaultGerritScopes, "-json-output", "-"}; !slices.Equal(gotArgs, want) {
		t.Errorf("luci-auth args = %q, want %q", gotArgs, want)
	}
	gerritTokenMu.Lock()
	gotExpiry := cachedGerritTokenExpiry
	gerritTokenMu.Unlock()
	if want := time.Unix(wantExpiry, 0).Add(-60 * time.Second); !gotExpiry.Equal(want) {
		t.Errorf("cachedGerritTokenExpiry = %v, want %v", gotExpiry, want)
	}
}

func TestDefaultGerritToken_ScopeFallback(t *testing.T) {
	wantExpiry := time.Now().Add(30 * time.Minute).Unix()
	var gotScopes []string
	WithHermeticAuth(t, AuthResolvers{
		LookPath: onPath("luci-auth", "/usr/bin/luci-auth"),
		CommandRunner: func(ctx context.Context, name string, args ...string) ([]byte, error) {
			if idx := slices.Index(args, "-scopes"); idx >= 0 && idx+1 < len(args) {
				sc := args[idx+1]
				gotScopes = append(gotScopes, sc)
				if sc == DefaultGerritScopes {
					return nil, &exec.ExitError{Stderr: []byte("Not logged in for userinfo.email\n")}
				}
			}
			return fmt.Appendf(nil, `{"token":"narrow-scope-token","expiry":%d}`, wantExpiry), nil
		},
	})

	tok, src, err := DefaultGerritToken(t.Context())
	if err != nil {
		t.Fatalf("DefaultGerritToken() error = %v", err)
	}
	if tok != "narrow-scope-token" || src != "luci-auth" {
		t.Errorf("DefaultGerritToken() = (%q, %q), want (narrow-scope-token, luci-auth)", tok, src)
	}
	if want := []string{DefaultGerritScopes, ScopeGerritCodeReview}; !slices.Equal(gotScopes, want) {
		t.Errorf("probed scopes = %q, want %q", gotScopes, want)
	}
}

func TestLUCIAuthTransport(t *testing.T) {
	newRecordingTransport := func() (*mockTransport, **http.Request) {
		var lastReq *http.Request
		return &mockTransport{
			roundTrip: func(req *http.Request) (*http.Response, error) {
				lastReq = req
				return okGerritResponse(req, "{}"), nil
			},
		}, &lastReq
	}

	t.Run("attaches Bearer token when available", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{
			LUCIToken: func(ctx context.Context) (string, string, error) {
				return "luci-secret-token", "luci-auth token", nil
			},
		})
		setAuthModeFlag(t, "googler")
		rec, lastReq := newRecordingTransport()
		tr := &LUCIAuthTransport{Base: rec}

		req, _ := http.NewRequest("POST", "https://cr-buildbucket.appspot.com/prpc/buildbucket.v2.Builds/SearchBuilds", nil)
		resp, err := tr.RoundTrip(req)
		if err != nil {
			t.Fatalf("RoundTrip failed: %v", err)
		}
		_ = resp.Body.Close()
		if *lastReq == nil {
			t.Fatal("expected HTTP request to be sent, got nil")
		}
		if got := (*lastReq).Header.Get("Authorization"); got != "Bearer luci-secret-token" {
			t.Errorf("Authorization = %q, want 'Bearer luci-secret-token'", got)
		}
	})

	t.Run("fails with ExitCodeAuth in googler mode when token missing", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{
			LUCIToken: func(ctx context.Context) (string, string, error) {
				return "", "", fmt.Errorf("interactive login required")
			},
		})
		setAuthModeFlag(t, "googler")
		rec, lastReq := newRecordingTransport()
		tr := &LUCIAuthTransport{Base: rec}

		req, _ := http.NewRequest("POST", "https://cr-buildbucket.appspot.com/prpc/buildbucket.v2.Builds/SearchBuilds", nil)
		_, err := tr.RoundTrip(req)
		if err == nil {
			t.Fatal("expected error in googler mode when LUCI token is missing")
		}
		if ExitCodeFor(err) != ExitCodeAuth {
			t.Errorf("ExitCodeFor(err) = %d, want %d", ExitCodeFor(err), ExitCodeAuth)
		}
		if *lastReq != nil {
			t.Errorf("expected no HTTP request to be sent when googler LUCI check fails")
		}
	})

	t.Run("proceeds unauthenticated in community mode when token missing", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{
			LUCIToken: func(ctx context.Context) (string, string, error) {
				return "", "", fmt.Errorf("luci-auth not installed")
			},
		})
		setAuthModeFlag(t, "community")
		rec, lastReq := newRecordingTransport()
		tr := &LUCIAuthTransport{Base: rec}

		req, _ := http.NewRequest("POST", "https://cr-buildbucket.appspot.com/prpc/buildbucket.v2.Builds/SearchBuilds", nil)
		resp, err := tr.RoundTrip(req)
		if err != nil {
			t.Fatalf("expected community mode to proceed unauthenticated, got error: %v", err)
		}
		_ = resp.Body.Close()
		if *lastReq == nil {
			t.Fatal("expected HTTP request to be sent, got nil")
		}
		if got := (*lastReq).Header.Get("Authorization"); got != "" {
			t.Errorf("expected empty Authorization header in community mode without token, got %q", got)
		}
	})
}

func TestLUCIAuthTransport_MemoizesAuthModeAcrossRequests(t *testing.T) {
	setAuthModeFlag(t, "")
	WithHermeticAuth(t, AuthResolvers{
		LUCIToken: func(ctx context.Context) (string, string, error) {
			return "luci-tok", "luci-auth", nil
		},
	})

	var gitConfigCalls int
	mockGit := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			gitConfigCalls++
			if len(args) >= 3 && args[0] == "config" && args[1] == "--get" && args[2] == "user.email" {
				_, _ = stdout.Write([]byte("dev@google.com\n"))
				return nil
			}
			return fmt.Errorf("not set")
		},
	}
	cfg := &Config{Git: mockGit}
	rec := &mockTransport{
		roundTrip: func(req *http.Request) (*http.Response, error) {
			return okGerritResponse(req, "{}"), nil
		},
	}
	tr := &LUCIAuthTransport{Base: rec, Cfg: cfg}

	for i := range 3 {
		req, _ := http.NewRequest("POST", "https://cr-buildbucket.appspot.com/prpc/buildbucket.v2.Builds/GetBuild", nil)
		resp, err := tr.RoundTrip(req)
		if err != nil {
			t.Fatalf("RoundTrip(%d) failed: %v", i, err)
		}
		_ = resp.Body.Close()
	}
	// Only the first request should query git config (ghish.authmode + user.email = 2 calls).
	if gitConfigCalls > 2 {
		t.Errorf("gitConfigCalls across 3 LUCI requests = %d, want <= 2", gitConfigCalls)
	}
}

func TestOAuthTokenCommand_AutoConfiguresSSOCredHelper(t *testing.T) {
	tests := []struct {
		name           string
		hasHelper      bool
		lookPathErr    error
		envSet         bool
		envVal         string
		bin            string
		args           []string
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
			name:           "injects for resolved absolute luci-auth path",
			hasHelper:      true,
			bin:            "/opt/cipd/packages/luci/luci-auth",
			args:           []string{"token"},
			wantInjectedTo: ssoCredHelperPath,
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
			"  2. Run 'luci-auth login -scopes %q' (for *.googlesource.com)\n"+
			"  3. Set an OAuth2 token: export GERRIT_TOKEN=\"<token>\"\n"+
			"  4. Check auth status: gh auth status",
		DefaultGerritScopes)

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
			fmt.Sprintf("luci-auth login -scopes %q", DefaultGerritScopes),
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

func TestInvalidateCachedGerritToken_ClearsProbeError(t *testing.T) {
	const bin = "/x/luci-auth"
	var calls int
	shouldFail := true
	WithHermeticAuth(t, AuthResolvers{
		LookPath: onPath("luci-auth", bin),
		CommandRunner: func(context.Context, string, ...string) ([]byte, error) {
			calls++
			if shouldFail {
				return nil, &exec.ExitError{Stderr: []byte("Not logged in.\n")}
			}
			return []byte("recovered-token\n"), nil
		},
	})

	if _, _, err := DefaultGerritToken(t.Context()); err == nil {
		t.Fatal("initial DefaultGerritToken() succeeded, want probe failure")
	}
	afterFirst := calls
	if _, _, err := DefaultGerritToken(t.Context()); err == nil || calls != afterFirst {
		t.Fatalf("second DefaultGerritToken() err=%v, calls=%d (want cached error with %d calls)", err, calls, afterFirst)
	}

	shouldFail = false
	invalidateCachedGerritToken("stale")

	tok, src, err := DefaultGerritToken(t.Context())
	if err != nil || tok != "recovered-token" || src != "luci-auth" {
		t.Fatalf("DefaultGerritToken() after invalidation = (%q, %q, %v), want (recovered-token, luci-auth, nil)", tok, src, err)
	}
	if calls != afterFirst+1 {
		t.Errorf("OAuthTokenCommandRunner calls after recovery = %d, want %d", calls, afterFirst+1)
	}
}
