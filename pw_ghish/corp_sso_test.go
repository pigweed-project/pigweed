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
	"log/slog"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
)

const corpLoginExpiredURL = "https://" + corpLoginHost + "/request?d=expired"

func setTestAuthMode(t *testing.T, mode AuthMode) {
	t.Helper()
	prev := AuthModeFlag
	AuthModeFlag = string(mode)
	ResetAuthTokenCaches()
	t.Cleanup(func() {
		AuthModeFlag = prev
		ResetAuthTokenCaches()
	})
}

func gossoLookPath(file string) (string, error) {
	if file == "gosso" {
		return gossoBinPath, nil
	}
	return "", fmt.Errorf("not found: %s", file)
}

func mockGossoJarJSON(corpHost, ticket, upCookie, gsCookie string) string {
	return fmt.Sprintf(`[
		{"URL":"https://up.%s:443/uberproxy/loginz","Cookies":[{"Name":"__Secure-GSSO_UberProxy","Value":%q,"Domain":"google.com","Path":"/"}]},
		{"URL":"https://%s","Cookies":[{"Name":"GerritSession","Value":%q,"Path":"/"},{"Name":"SSO","Value":%q,"Path":"/"}]}
	]`, corpCookieDomain, upCookie, corpHost, gsCookie, ticket)
}

func closedLoopbackAddr(t *testing.T) string {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("net.Listen: %v", err)
	}
	addr := ln.Addr().String()
	_ = ln.Close()
	return addr
}

func captureSlogWarnings(t *testing.T) *bytes.Buffer {
	t.Helper()
	var buf bytes.Buffer
	prev := slog.Default()
	slog.SetDefault(slog.New(slog.NewTextHandler(&buf, &slog.HandlerOptions{Level: slog.LevelWarn})))
	t.Cleanup(func() { slog.SetDefault(prev) })
	return &buf
}

type corpSSORigOpts struct {
	mintErr      error
	dialFail     bool
	luciErr      error
	proxyHandler http.HandlerFunc
	directRound  func(*http.Request) (*http.Response, error)
}

type corpSSOTestRig struct {
	ctx         context.Context
	canonHost   string
	corpHost    string
	cacheDir    string
	runtimeDir  string
	proxyHits   atomic.Int32
	ssoMints    atomic.Int32
	luciCalls   atomic.Int32
	directCalls atomic.Int32
	warns       *bytes.Buffer
	directRT    *mockTransport
}

func newCorpSSOTestRig(t *testing.T, opts corpSSORigOpts) *corpSSOTestRig {
	t.Helper()
	const canonHost = "example-internal-review.googlesource.com"
	prefix := strings.TrimSuffix(canonHost, ".googlesource.com")
	corpHost := prefix + gobCorpDomainSuffix
	cacheDir := t.TempDir()
	runtimeDir := t.TempDir()
	WithTestCaches(t, TestCacheConfig{RuntimeDir: runtimeDir, PersistentDir: cacheDir})
	setTestAuthMode(t, AuthModeGoogler)

	rig := &corpSSOTestRig{
		ctx:        t.Context(),
		canonHost:  canonHost,
		corpHost:   corpHost,
		cacheDir:   cacheDir,
		runtimeDir: runtimeDir,
		warns:      captureSlogWarnings(t),
	}

	var proxyAddr string
	if opts.dialFail {
		proxyAddr = closedLoopbackAddr(t)
	} else {
		proxySrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			rig.proxyHits.Add(1)
			if opts.proxyHandler != nil {
				opts.proxyHandler(w, r)
				return
			}
			w.Header().Set("Content-Type", "application/json")
			_, _ = io.WriteString(w, ")]}'\n{\"_account_id\":42,\"username\":\"googler\"}")
		}))
		t.Cleanup(proxySrv.Close)
		proxyAddr = proxySrv.Listener.Addr().String()
	}

	WithHermeticAuth(t, AuthResolvers{
		CorpUplinkAddr: proxyAddr,
		LookPath:       gossoLookPath,
		GerritToken: func(context.Context) (string, string, error) {
			rig.luciCalls.Add(1)
			if opts.luciErr != nil {
				return "", "", opts.luciErr
			}
			return "direct-oauth-tok", "luci-auth", nil
		},
		GossoCookieJarRunner: func(_ context.Context, _ string, _ ...string) ([]byte, error) {
			n := rig.ssoMints.Add(1)
			if opts.mintErr != nil {
				return nil, opts.mintErr
			}
			return []byte(mockGossoJarJSON(corpHost, fmt.Sprintf("ticket-%d", n), "up", "gs")), nil
		},
	})

	rig.directRT = &mockTransport{
		roundTrip: func(req *http.Request) (*http.Response, error) {
			rig.directCalls.Add(1)
			if opts.directRound != nil {
				return opts.directRound(req)
			}
			return okGerritResponse(req, `{"_account_id":84,"username":"direct"}`), nil
		},
	}
	return rig
}

func (r *corpSSOTestRig) newCorpSSOClient() *GerritClient {
	return NewGerritClientWithStrategy("https://"+r.canonHost, &http.Client{Transport: r.directRT}, NewCorpSSOStrategy(r.canonHost))
}

func (r *corpSSOTestRig) newClient(t *testing.T) *GerritClient {
	t.Helper()
	client, err := newGerritClientForURL(r.ctx, "https://"+r.canonHost+"/a", &http.Client{Transport: r.directRT})
	if err != nil {
		t.Fatalf("newGerritClientForURL: %v", err)
	}
	return client
}

func TestCorpSSOStrategy(t *testing.T) {
	t.Run("normalizes sso:// remote to canonical and corp hosts", func(t *testing.T) {
		wantCorpHost := "example-internal-review" + gobCorpDomainSuffix
		strat := NewCorpSSOStrategy("sso://example-internal/repo")
		if strat.Host != "example-internal-review.googlesource.com" || strat.CorpHost != wantCorpHost {
			t.Errorf("NewCorpSSOStrategy(sso://...) = (%q, %q), want (example-internal-review.googlesource.com, %q)",
				strat.Host, strat.CorpHost, wantCorpHost)
		}
	})
	t.Run("POST mutation body and /a/ prefix preserved across stale-302 refresh", func(t *testing.T) {
		var bodies []string
		rig := newCorpSSOTestRig(t, corpSSORigOpts{
			proxyHandler: func(w http.ResponseWriter, r *http.Request) {
				b, _ := io.ReadAll(r.Body)
				bodies = append(bodies, string(b))
				if r.URL.EscapedPath() != "/a/changes/pigweed%2Fpigweed~123/revisions/current/review" {
					t.Errorf("escaped path = %q, want /a/ prefix with preserved %%2F", r.URL.EscapedPath())
				}
				if r.Header.Get("Authorization") == formatCorpSSOBasicAuth("ticket-1") {
					w.Header().Set("Location", corpLoginExpiredURL)
					w.WriteHeader(http.StatusFound)
					return
				}
				w.Header().Set("Content-Type", "application/json")
				_, _ = io.WriteString(w, ")]}'\n{\"labels\":{\"Code-Review\":1}}")
			},
		})
		client := rig.newCorpSSOClient()
		if _, err := client.SetReview(rig.ctx, "pigweed/pigweed~123", "current", &ReviewInput{Message: "lgtm"}); err != nil {
			t.Fatalf("SetReview: %v", err)
		}
		if len(bodies) != 2 || bodies[0] != bodies[1] || !strings.Contains(bodies[1], `"message":"lgtm"`) {
			t.Errorf("bodies = %q, want identical non-empty JSON across refresh", bodies)
		}
		if rig.ssoMints.Load() != 2 || rig.directCalls.Load() != 0 {
			t.Errorf("ssoMints=%d directCalls=%d, want 2 and 0", rig.ssoMints.Load(), rig.directCalls.Load())
		}
	})

	t.Run("repeated 302 after refresh is not followed and fails with ExitCodeAuth", func(t *testing.T) {
		rig := newCorpSSOTestRig(t, corpSSORigOpts{
			proxyHandler: func(w http.ResponseWriter, _ *http.Request) {
				w.Header().Set("Location", "https://evil.example.com/harvest")
				w.WriteHeader(http.StatusFound)
			},
		})
		_, err := rig.newCorpSSOClient().GetSelfAccount(rig.ctx)
		if ExitCodeFor(err) != ExitCodeAuth || rig.ssoMints.Load() != 2 || rig.proxyHits.Load() != 2 {
			t.Errorf("err=%v (code=%d) ssoMints=%d proxyHits=%d, want ExitCodeAuth and 2/2", err, ExitCodeFor(err), rig.ssoMints.Load(), rig.proxyHits.Load())
		}
	})

	t.Run("Uplink dial failure returns actionable ExitCodeAuth", func(t *testing.T) {
		rig := newCorpSSOTestRig(t, corpSSORigOpts{dialFail: true})
		_, err := rig.newCorpSSOClient().GetSelfAccount(rig.ctx)
		if ExitCodeFor(err) != ExitCodeAuth || !strings.Contains(err.Error(), "127.0.0.1:999") {
			t.Errorf("err = %v (code=%d), want ExitCodeAuth mentioning 127.0.0.1:999", err, ExitCodeFor(err))
		}
	})
}

func TestCorpSSO_TransportSecurityInvariants(t *testing.T) {
	rt := &safeHTTPTransport{
		direct: &mockTransport{
			roundTrip: func(r *http.Request) (*http.Response, error) {
				t.Errorf("unexpected direct transport call: %s", r.URL)
				return nil, errors.New("unexpected direct call")
			},
		},
	}

	corpHost := "example-internal-review" + gobCorpDomainSuffix
	for _, tc := range []struct {
		url, hdr, val string
	}{
		{"http://" + corpHost + "/a/accounts/self", "", ""},
		{"http://example.com/path", "Authorization", "Bearer leaked"},
		{"http://example.com/path", "Cookie", "SID=leaked"},
	} {
		req, _ := http.NewRequestWithContext(t.Context(), http.MethodGet, tc.url, nil)
		if tc.hdr != "" {
			req.Header.Set(tc.hdr, tc.val)
		}
		resp, err := rt.RoundTrip(req)
		if resp != nil {
			resp.Body.Close()
		}
		if err == nil || !strings.Contains(err.Error(), "refusing to send authenticated or corp request over cleartext HTTP") {
			t.Errorf("RoundTrip(%s) = %v, want cleartext firewall error", tc.url, err)
		}
	}
}

func corpSSOFallbackReason(host string) string {
	fb, _ := corpSSOFallbacks.Get(normalizeCorpHostKey(host))
	return string(fb)
}

func TestAdaptiveGerritStrategy_RoutingAndFallback(t *testing.T) {
	const fallbackWarning = "using OAuth2 instead of Corp SSO"

	t.Run("prefers Corp SSO without invoking luci-auth or writing hosts.json", func(t *testing.T) {
		rig := newCorpSSOTestRig(t, corpSSORigOpts{luciErr: errors.New("luci-auth not logged in")})
		client := rig.newClient(t)
		if m, desc := client.DescribeAuth(); m != AuthMethodCorpSSO || desc != "corp-sso" {
			t.Errorf("DescribeAuth = (%q, %q), want (corp-sso, corp-sso)", m, desc)
		}
		acc, err := client.GetSelfAccount(rig.ctx)
		if err != nil || acc.AccountID != 42 {
			t.Fatalf("GetSelfAccount = (%+v, %v), want AccountID=42", acc, err)
		}
		if rig.proxyHits.Load() != 1 || rig.directCalls.Load() != 0 || rig.luciCalls.Load() != 0 {
			t.Errorf("proxy=%d direct=%d luci=%d, want 1/0/0", rig.proxyHits.Load(), rig.directCalls.Load(), rig.luciCalls.Load())
		}
		if _, statErr := os.Stat(filepath.Join(rig.cacheDir, "hosts.json")); !errors.Is(statErr, os.ErrNotExist) {
			t.Errorf("hosts.json stat = %v, want not-exist", statErr)
		}
	})

	t.Run("dual Corp SSO and luci-auth failure surfaces primary Corp SSO error", func(t *testing.T) {
		for _, tc := range []struct {
			name string
			opts corpSSORigOpts
			want string
		}{
			{"mint failure", corpSSORigOpts{mintErr: errors.New("gcert expired"), luciErr: errors.New("luci-auth expired")}, "gcert"},
			{"dial failure", corpSSORigOpts{dialFail: true, luciErr: errors.New("luci-auth expired")}, "127.0.0.1:"},
		} {
			t.Run(tc.name, func(t *testing.T) {
				rig := newCorpSSOTestRig(t, tc.opts)
				_, err := rig.newClient(t).GetSelfAccount(rig.ctx)
				if ExitCodeFor(err) != ExitCodeAuth || !strings.Contains(err.Error(), tc.want) {
					t.Errorf("err = %v, want ExitCodeAuth containing %q", err, tc.want)
				}
				if fb := corpSSOFallbackReason(rig.canonHost); fb != "" || rig.warns.Len() != 0 {
					t.Errorf("recorded bogus fallback (fb=%q, warns=%q)", fb, rig.warns.String())
				}
			})
		}
	})

	t.Run("open host falls back to OAuth2 once per process and warns once", func(t *testing.T) {
		for _, tc := range []struct {
			name          string
			opts          corpSSORigOpts
			postReview    bool
			wantProxyHits int32
		}{
			{
				name:       "Prepare credential mint failure",
				opts:       corpSSORigOpts{mintErr: errors.New("gcert required")},
				postReview: true,
			},
			{
				name: "HandleTransportError Uplink dial failure",
				opts: corpSSORigOpts{dialFail: true},
			},
			{
				name: "HandleResponse GET 403",
				opts: corpSSORigOpts{proxyHandler: func(w http.ResponseWriter, _ *http.Request) {
					http.Error(w, "not permitted", http.StatusForbidden)
				}},
				wantProxyHits: 1,
			},
		} {
			t.Run(tc.name, func(t *testing.T) {
				var directBodies []string
				opts := tc.opts
				opts.directRound = func(req *http.Request) (*http.Response, error) {
					if req.Header.Get("Authorization") != "Bearer direct-oauth-tok" || req.Header.Get("Cookie") != "" {
						t.Errorf("direct headers = Auth:%q Cookie:%q", req.Header.Get("Authorization"), req.Header.Get("Cookie"))
					}
					if req.Body != nil {
						b, _ := io.ReadAll(req.Body)
						directBodies = append(directBodies, string(b))
					}
					return okGerritResponse(req, `{"_number":77}`), nil
				}
				rig := newCorpSSOTestRig(t, opts)
				client := rig.newClient(t)
				if tc.postReview {
					if _, err := client.SetReview(rig.ctx, "77", "current", &ReviewInput{Message: "Ship it"}); err != nil {
						t.Fatalf("SetReview fallback: %v", err)
					}
					if len(directBodies) != 1 || !strings.Contains(directBodies[0], "Ship it") {
						t.Errorf("directBodies = %q, want replayed POST body", directBodies)
					}
				} else if _, err := client.GetChange(rig.ctx, "77"); err != nil {
					t.Fatalf("GetChange fallback: %v", err)
				}
				if rig.directCalls.Load() != 1 || rig.ssoMints.Load() != 1 || rig.proxyHits.Load() != tc.wantProxyHits {
					t.Errorf("direct=%d ssoMints=%d proxyHits=%d, want 1/1/%d",
						rig.directCalls.Load(), rig.ssoMints.Load(), rig.proxyHits.Load(), tc.wantProxyHits)
				}
				if m, desc := client.DescribeAuth(); m != AuthMethodToken || desc != "token (luci-auth)" {
					t.Errorf("DescribeAuth = (%q, %q), want (token, token (luci-auth))", m, desc)
				}
				if corpSSOFallbackReason(rig.canonHost) == "" {
					t.Error("corpSSOFallbackReason = empty, want recorded reason")
				}
				// Sticky fallback: second call goes straight to Direct without retrying SSO or re-warning.
				if _, err := client.GetChange(rig.ctx, "77"); err != nil {
					t.Fatalf("second GetChange: %v", err)
				}
				if rig.ssoMints.Load() != 1 || strings.Count(rig.warns.String(), fallbackWarning) != 1 {
					t.Errorf("ssoMints=%d warnCount=%d, want 1 and 1:\n%s",
						rig.ssoMints.Load(), strings.Count(rig.warns.String(), fallbackWarning), rig.warns.String())
				}
				ResetAuthTokenCaches()
				if corpSSOFallbackReason(rig.canonHost) != "" {
					t.Error("fallback survived ResetAuthTokenCaches")
				}
			})
		}
	})

	t.Run("mutation 403 over Corp SSO returns Gerrit error without falling back", func(t *testing.T) {
		rig := newCorpSSOTestRig(t, corpSSORigOpts{
			proxyHandler: func(w http.ResponseWriter, _ *http.Request) {
				http.Error(w, "Applying label is restricted", http.StatusForbidden)
			},
		})
		if _, err := rig.newClient(t).SetReview(rig.ctx, "77", "current", &ReviewInput{Message: "nope"}); !IsGerritHTTPStatus(err, http.StatusForbidden) {
			t.Errorf("mutation 403 err = %v, want HTTP 403 GerritAPIError", err)
		}
		if rig.directCalls.Load() != 0 || rig.warns.Len() != 0 {
			t.Errorf("mutation 403 fell back: direct=%d warns=%q", rig.directCalls.Load(), rig.warns.String())
		}
	})

	t.Run("corp-only host never falls back to OAuth2", func(t *testing.T) {
		rig := newCorpSSOTestRig(t, corpSSORigOpts{
			proxyHandler: func(w http.ResponseWriter, _ *http.Request) {
				http.Error(w, "Forbidden", http.StatusForbidden)
			},
		})
		defaultHostCache.MarkCorpOnly(rig.canonHost)
		if _, err := rig.newClient(t).GetChange(rig.ctx, "77"); !IsGerritHTTPStatus(err, http.StatusForbidden) || rig.directCalls.Load() != 0 {
			t.Errorf("warm corp-only 403 err=%v directCalls=%d, want 403 and 0", err, rig.directCalls.Load())
		}

		rigCold := newCorpSSOTestRig(t, corpSSORigOpts{
			mintErr: errors.New("gcert expired"),
			directRound: func(req *http.Request) (*http.Response, error) {
				return statusResponse(req, http.StatusForbidden, "PERMISSION_DENIED_BY_IP_RESTRICTIONS"), nil
			},
		})
		if _, err := rigCold.newClient(t).GetChange(rigCold.ctx, "77"); ExitCodeFor(err) != ExitCodeAuth ||
			rigCold.ssoMints.Load() != 1 || rigCold.warns.Len() != 0 || !defaultHostCache.IsCorpOnly(rigCold.canonHost) {
			t.Errorf("cold corp-only mint fail: err=%v ssoMints=%d warns=%q corpOnly=%v",
				err, rigCold.ssoMints.Load(), rigCold.warns.String(), defaultHostCache.IsCorpOnly(rigCold.canonHost))
		}
	})

	t.Run("GERRIT_TOKEN bypasses Corp SSO even when corp_only is cached", func(t *testing.T) {
		rig := newCorpSSOTestRig(t, corpSSORigOpts{})
		defaultHostCache.MarkCorpOnly(rig.canonHost)
		t.Setenv("GERRIT_TOKEN", "explicit-tok")
		if _, err := rig.newClient(t).GetSelfAccount(rig.ctx); err != nil || rig.directCalls.Load() != 1 || rig.proxyHits.Load() != 0 {
			t.Errorf("err=%v directCalls=%d proxyHits=%d, want nil, 1, and 0", err, rig.directCalls.Load(), rig.proxyHits.Load())
		}
	})

	t.Run("community mode bypasses Corp SSO even when corp_only is cached", func(t *testing.T) {
		rig := newCorpSSOTestRig(t, corpSSORigOpts{})
		setTestAuthMode(t, AuthModeCommunity)
		defaultHostCache.MarkCorpOnly(rig.canonHost)
		if _, err := rig.newClient(t).GetSelfAccount(rig.ctx); err != nil || rig.directCalls.Load() != 1 || rig.proxyHits.Load() != 0 {
			t.Errorf("err=%v directCalls=%d proxyHits=%d, want nil, 1, and 0", err, rig.directCalls.Load(), rig.proxyHits.Load())
		}
	})
}

func TestAdaptiveGerritStrategy_ColdIPRestrictionPivotAndWarmCache(t *testing.T) {
	t.Setenv(corpSSOEnvVar, "0") // Force OAuth2 first to exercise the cold IP-restriction pivot.
	var proxyBodies []string
	rig := newCorpSSOTestRig(t, corpSSORigOpts{
		directRound: func(req *http.Request) (*http.Response, error) {
			return statusResponse(req, http.StatusForbidden, "PERMISSION_DENIED_BY_IP_RESTRICTIONS: Access must be through "+"example-internal-review"+gobCorpDomainSuffix), nil
		},
		proxyHandler: func(w http.ResponseWriter, r *http.Request) {
			if b, _ := io.ReadAll(r.Body); len(b) > 0 {
				proxyBodies = append(proxyBodies, string(b))
			}
			w.Header().Set("Content-Type", "application/json")
			_, _ = io.WriteString(w, ")]}'\n{\"_number\":12345}")
		},
	})

	client1 := rig.newClient(t)
	if _, err := client1.SetReview(rig.ctx, "12345", "current", &ReviewInput{Message: "cold-pivot"}); err != nil {
		t.Fatalf("SetReview (cold pivot): %v", err)
	}
	if rig.directCalls.Load() != 1 || rig.proxyHits.Load() != 1 || rig.ssoMints.Load() != 1 || rig.luciCalls.Load() != 1 {
		t.Errorf("cold counts: direct=%d proxy=%d sso=%d luci=%d, want 1/1/1/1",
			rig.directCalls.Load(), rig.proxyHits.Load(), rig.ssoMints.Load(), rig.luciCalls.Load())
	}
	if len(proxyBodies) != 1 || !strings.Contains(proxyBodies[0], "cold-pivot") {
		t.Errorf("proxyBodies = %q, want preserved POST body", proxyBodies)
	}
	if m, _ := client1.DescribeAuth(); m != AuthMethodCorpSSO {
		t.Errorf("DescribeAuth after pivot = %q, want corp-sso", m)
	}

	entries := readDiskCacheFile[hostClassification](t, filepath.Join(rig.cacheDir, "hosts.json"))
	if !entries[rig.canonHost].Value.CorpOnly {
		t.Errorf("hosts.json[%s] = %+v, want corp_only=true", rig.canonHost, entries[rig.canonHost])
	}

	// Warm reload from hosts.json ignores GHISH_CORP_SSO=0, makes 0 direct calls, and needs no luci-auth.
	defaultHostCache.ResetMemory()
	rig.directCalls.Store(0)
	rig.luciCalls.Store(0)
	SetAuthResolvers(AuthResolvers{
		GerritToken: func(context.Context) (string, string, error) {
			rig.luciCalls.Add(1)
			return "", "", errors.New("unexpected luci-auth on warm corp-only host")
		},
	})
	client2 := rig.newClient(t)
	if m, _ := client2.DescribeAuth(); m != AuthMethodCorpSSO {
		t.Errorf("warm DescribeAuth = %q, want corp-sso", m)
	}
	if _, err := client2.GetChange(rig.ctx, "12345"); err != nil {
		t.Fatalf("GetChange (warm): %v", err)
	}
	if rig.directCalls.Load() != 0 || rig.luciCalls.Load() != 0 {
		t.Errorf("warm direct=%d luci=%d, want 0/0", rig.directCalls.Load(), rig.luciCalls.Load())
	}
}

func TestSSOSessionCache_CrossProcessReuse(t *testing.T) {
	rig := newCorpSSOTestRig(t, corpSSORigOpts{})
	cachePath := filepath.Join(rig.runtimeDir, "sso_sessions.json")

	if _, err := rig.newClient(t).GetSelfAccount(rig.ctx); err != nil || rig.ssoMints.Load() != 1 {
		t.Fatalf("GetSelfAccount (cold) err=%v ssoMints=%d, want nil and 1", err, rig.ssoMints.Load())
	}
	entry, ok := readDiskCacheFile[ssoSession](t, cachePath)[rig.canonHost]
	if !ok || entry.Value.BasicAuth != formatCorpSSOBasicAuth("ticket-1") {
		t.Errorf("cached entry = %+v (ok=%v), want ticket-1", entry, ok)
	}

	// Simulated second process (in-memory cleared, runtime disk retained) avoids re-minting.
	ResetAuthTokenCaches()
	if _, err := rig.newClient(t).GetSelfAccount(rig.ctx); err != nil {
		t.Fatalf("GetSelfAccount (warm): %v", err)
	}
	if rig.ssoMints.Load() != 1 || rig.proxyHits.Load() != 2 {
		t.Errorf("warm ssoMints=%d proxyHits=%d, want 1 and 2", rig.ssoMints.Load(), rig.proxyHits.Load())
	}
}

func (r *corpSSOTestRig) newChangeContext(t *testing.T, changeID string) *ChangeContext {
	t.Helper()
	return &ChangeContext{
		Context:  r.ctx,
		Config:   &Config{Host: r.canonHost},
		Client:   r.newClient(t),
		ChangeID: changeID,
		Revision: "current",
		Host:     r.canonHost,
	}
}

func TestSetPositiveCodeReview_VoteGating(t *testing.T) {
	t.Run("enforced change refuses positive vote when Corp SSO fails", func(t *testing.T) {
		var directPosts atomic.Int32
		rig := newCorpSSOTestRig(t, corpSSORigOpts{
			dialFail: true,
			directRound: func(req *http.Request) (*http.Response, error) {
				if req.Method == http.MethodPost {
					directPosts.Add(1)
				}
				return okGerritResponse(req, `{"_number":77,"submit_requirements":[{"name":"Review-Enforcement","status":"UNSATISFIED"}]}`), nil
			},
		})
		chCtx := rig.newChangeContext(t, "77")
		err := chCtx.SetReview(&ReviewInput{Message: "LGTM", Labels: map[string]int{"Code-Review": 2}})
		if ExitCodeFor(err) != ExitCodeAuth || !strings.Contains(err.Error(), "missing device trust") {
			t.Fatalf("SetReview(CR+2) err = %v, want ExitCodeAuth mentioning missing device trust", err)
		}
		if directPosts.Load() != 0 {
			t.Errorf("directPosts = %d, want 0", directPosts.Load())
		}
	})

	t.Run("non-enforced change falls back to direct on cold Corp SSO failure", func(t *testing.T) {
		var directPosts atomic.Int32
		rig := newCorpSSOTestRig(t, corpSSORigOpts{
			mintErr: errors.New("gcert expired"),
			directRound: func(req *http.Request) (*http.Response, error) {
				if req.Method == http.MethodPost {
					directPosts.Add(1)
				}
				return okGerritResponse(req, `{"_number":77,"submit_requirements":[]}`), nil
			},
		})
		if err := rig.newChangeContext(t, "77").SetReview(&ReviewInput{Message: "LGTM", Labels: map[string]int{"Code-Review": 2}}); err != nil {
			t.Fatalf("SetReview on non-enforced change: %v", err)
		}
		if directPosts.Load() != 1 || rig.directCalls.Load() != 2 || corpSSOFallbackReason(rig.canonHost) == "" {
			t.Errorf("directPosts=%d directCalls=%d fb=%q, want 1, 2, and non-empty",
				directPosts.Load(), rig.directCalls.Load(), corpSSOFallbackReason(rig.canonHost))
		}
	})

	t.Run("GHISH_CORP_SSO=0 on cold corp-only host pivots probe to Corp SSO and allows enforced vote", func(t *testing.T) {
		t.Setenv(corpSSOEnvVar, "0")
		var proxyPosts atomic.Int32
		rig := newCorpSSOTestRig(t, corpSSORigOpts{
			proxyHandler: func(w http.ResponseWriter, r *http.Request) {
				if r.Method == http.MethodPost {
					proxyPosts.Add(1)
				}
				w.Header().Set("Content-Type", "application/json")
				_, _ = io.WriteString(w, ")]}'\n"+`{"_number":77,"submit_requirements":[{"name":"Review-Enforcement","status":"UNSATISFIED"}]}`)
			},
			directRound: func(req *http.Request) (*http.Response, error) {
				return statusResponse(req, http.StatusForbidden, "PERMISSION_DENIED_BY_IP_RESTRICTIONS"), nil
			},
		})
		if err := rig.newChangeContext(t, "77").SetReview(&ReviewInput{Message: "LGTM", Labels: map[string]int{"Code-Review": 2}}); err != nil {
			t.Fatalf("SetReview on cold corp-only host: %v", err)
		}
		if proxyPosts.Load() != 1 || !defaultHostCache.IsCorpOnly(rig.canonHost) {
			t.Errorf("proxyPosts=%d corpOnly=%v, want 1 and true", proxyPosts.Load(), defaultHostCache.IsCorpOnly(rig.canonHost))
		}
	})
}
