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
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
)

const corpLoginExpiredURL = "https://" + corpLoginHost + "/request?d=expired"

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

type corpSSORigOpts struct {
	dialFail     bool
	proxyHandler http.HandlerFunc
}

type corpSSOTestRig struct {
	ctx         context.Context
	canonHost   string
	corpHost    string
	proxyHits   atomic.Int32
	ssoMints    atomic.Int32
	directCalls atomic.Int32
	directRT    *mockTransport
}

func newCorpSSOTestRig(t *testing.T, opts corpSSORigOpts) *corpSSOTestRig {
	t.Helper()
	const canonHost = "example-internal-review.googlesource.com"
	prefix := strings.TrimSuffix(canonHost, ".googlesource.com")
	corpHost := prefix + gobCorpDomainSuffix

	rig := &corpSSOTestRig{
		ctx:       t.Context(),
		canonHost: canonHost,
		corpHost:  corpHost,
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
		GossoCookieJarRunner: func(_ context.Context, _ string, _ ...string) ([]byte, error) {
			n := rig.ssoMints.Add(1)
			return []byte(mockGossoJarJSON(corpHost, fmt.Sprintf("ticket-%d", n), "up", "gs")), nil
		},
	})

	rig.directRT = &mockTransport{
		roundTrip: func(req *http.Request) (*http.Response, error) {
			rig.directCalls.Add(1)
			return okGerritResponse(req, `{"_account_id":84,"username":"direct"}`), nil
		},
	}
	return rig
}

func (r *corpSSOTestRig) newCorpSSOClient() *GerritClient {
	return NewGerritClientWithStrategy("https://"+r.canonHost, &http.Client{Transport: r.directRT}, NewCorpSSOStrategy(r.canonHost))
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
