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
	"net"
	"net/http"
	"net/url"
	"strings"
	"time"
)

// Corp SSO Gerrit authentication via the local BeyondCorp Uplink proxy:
//
//	CorpSSOStrategy ---> ssoSessions ---> mintViaGosso (/dev/fd/3 cookie jar)
//	       |
//	       +---> corpUplinkHTTPTransport (127.0.0.1:999 -> <tenant>-review corp domain)

const maxSSOSessionLifetime = 4 * time.Hour // matches git-remote-sso's 4h SSO ticket cap

// CorpSSOStrategy implements HTTPAuthStrategy for Google-hosted Gerrit hosts
// accessed over HTTP through the local BeyondCorp Uplink proxy (:999) using
// UberProxy cookies and a Google-CorpSSO Basic auth ticket.
type CorpSSOStrategy struct {
	Host     string
	CorpHost string
}

// NewCorpSSOStrategy constructs a CorpSSOStrategy for a Google-hosted Gerrit
// host (canonicalized to <tenant>-review.googlesource.com).
func NewCorpSSOStrategy(host string) *CorpSSOStrategy {
	canon := normalizeCorpHostKey(host)
	prefix, ok := strings.CutSuffix(canon, ".googlesource.com")
	if !ok {
		panic(fmt.Sprintf("NewCorpSSOStrategy requires a *.googlesource.com host, got %q", host))
	}
	return &CorpSSOStrategy{Host: canon, CorpHost: prefix + gobCorpDomainSuffix}
}

// Describe returns AuthMethodCorpSSO and "corp-sso".
func (s *CorpSSOStrategy) Describe() (AuthMethod, string) {
	return AuthMethodCorpSSO, string(AuthMethodCorpSSO)
}

// HasCredentials implements HTTPAuthStrategy; CorpSSOStrategy always
// authenticates via gosso.
func (s *CorpSSOStrategy) HasCredentials() bool {
	return true
}

func (s *CorpSSOStrategy) matchesRequest(req *http.Request) bool {
	return CanonicalGerritHost(req.URL.Hostname()) == s.Host
}

func (s *CorpSSOStrategy) mintCredentials(ctx context.Context) (ssoSession, time.Time, error) {
	gossoBin := findGossoBinary()
	if gossoBin == "" {
		return ssoSession{}, time.Time{}, missingGossoAuthError("Gerrit", s.Host, "gh auth status --host "+s.Host)
	}
	sess, err := mintViaGosso(ctx, gossoBin, s.CorpHost)
	if err != nil {
		if ctxErr := ctx.Err(); ctxErr != nil {
			return ssoSession{}, time.Time{}, ctxErr
		}
		return ssoSession{}, time.Time{}, corpSSOError(s.Host, "no Corp SSO credentials could be minted", err)
	}
	return sess, time.Time{}, nil
}

// Prepare routes req through the local BeyondCorp Uplink proxy, which accepts
// plain HTTP on loopback for the internal corp host and upgrades to mTLS upstream.
func (s *CorpSSOStrategy) Prepare(ctx context.Context, req *http.Request) error {
	if !s.matchesRequest(req) {
		return nil
	}
	sess, err := ssoSessions.GetOrLoad(ctx, s.Host, s.mintCredentials)
	if err != nil {
		return err
	}
	markCorpSSORequest(req)
	req.URL.Scheme = "http"
	req.URL.Host = s.CorpHost
	req.Host = s.CorpHost
	ensureAuthenticatedPrefix(req.URL)
	req.Header.Set("Authorization", sess.BasicAuth)
	req.Header.Set("Cookie", sess.CookieHeader)
	return nil
}

// HandleResponse refreshes Corp SSO credentials once on an expired-session
// redirect or HTTP 401.
func (s *CorpSSOStrategy) HandleResponse(ctx context.Context, req *http.Request, resp *http.Response) (retry bool, err error) {
	if !s.matchesRequest(req) || !isCorpSSOSessionRejected(resp.StatusCode) {
		return false, nil
	}
	rejected := ssoSession{
		BasicAuth:    req.Header.Get("Authorization"),
		CookieHeader: req.Header.Get("Cookie"),
	}
	// Evict the rejected session (unless a concurrent caller already replaced it)
	// and ask executeWithAuthStrategy to retry once with freshly minted credentials.
	ssoSessions.DeleteIf(s.Host, func(cur ssoSession) bool { return cur == rejected })
	if st := authAttemptFrom(ctx); !st.retriedCorpSSORefresh {
		st.retriedCorpSSORefresh = true
		return true, nil
	}
	return false, corpSSOError(s.Host, fmt.Sprintf("session rejected with HTTP %d after a refresh", resp.StatusCode), nil)
}

func isCorpSSOSessionRejected(statusCode int) bool {
	switch statusCode {
	case http.StatusMovedPermanently, http.StatusFound, http.StatusSeeOther,
		http.StatusTemporaryRedirect, http.StatusPermanentRedirect, http.StatusUnauthorized:
		return true
	default:
		return false
	}
}

func normalizeCorpHostKey(host string) string {
	canon := CanonicalGerritHost(host)
	if !isGoogleGerritHost(canon) {
		return ""
	}
	return canon
}

type corpSSORequestMark struct{}

// markCorpSSORequest tags req so safeHTTPTransport routes it through the local
// Uplink proxy (instead of rejecting cleartext HTTP) and CheckRedirect returns
// 3xx login redirects to HandleResponse without following them.
func markCorpSSORequest(req *http.Request) {
	*req = *req.WithContext(context.WithValue(req.Context(), corpSSORequestMark{}, corpSSORequestMark{}))
}

func isCorpSSORequest(req *http.Request) bool {
	_, ok := req.Context().Value(corpSSORequestMark{}).(corpSSORequestMark)
	return ok
}

const defaultCorpUplinkAddr = "127.0.0.1:999"

var (
	// corpUplinkAddr is the host:port of the local BeyondCorp Uplink HTTP proxy
	// (overridden in hermetic tests via AuthResolvers.CorpUplinkAddr).
	corpUplinkAddr          = defaultCorpUplinkAddr
	corpUplinkHTTPTransport = func() *http.Transport {
		t := http.DefaultTransport.(*http.Transport).Clone()
		t.Proxy = func(*http.Request) (*url.URL, error) {
			if corpUplinkAddr == "disabled" {
				return nil, errors.New("hermetic test: unexpected corp uplink proxy connection")
			}
			return &url.URL{Scheme: "http", Host: corpUplinkAddr}, nil
		}
		return t
	}()
)

func roundTripCorpSSO(req *http.Request) (*http.Response, error) {
	resp, err := corpUplinkHTTPTransport.RoundTrip(req)
	if err != nil && req.Context().Err() == nil {
		// http.Transport wraps proxy dial failures in Op "proxyconnect".
		if opErr, ok := errors.AsType[*net.OpError](err); ok && opErr.Op == "proxyconnect" {
			return nil, corpSSOError(
				CanonicalGerritHost(req.URL.Hostname()),
				fmt.Sprintf("cannot connect to local BeyondCorp Uplink proxy (%s)", corpUplinkAddr),
				err,
				"Run from a managed corp machine (Cloudtop, Corp Linux, or gMac) where BeyondCorp Uplink ("+defaultCorpUplinkAddr+") is running.",
			)
		}
	}
	return resp, err
}

type ssoSession struct {
	BasicAuth    string `json:"basic_auth"`
	CookieHeader string `json:"cookie_header"`
}

func (ssoSession) CachePolicy() CachePolicy {
	return CachePolicy{
		Tier:   CacheInMemory,
		Name:   "sso_sessions",
		MaxTTL: maxSSOSessionLifetime,
	}
}

var ssoSessions = NewCache[string, ssoSession]()

func corpSSOError(host, reason string, cause error, extraSteps ...string) error {
	steps := append(
		[]string{"Refresh your corp single sign-on certificate:\n     gcert"},
		extraSteps...,
	)
	steps = append(steps, "Check authentication status:\n     gh auth status --host "+host)
	remediation := formatGerritRemediationSteps(steps...)
	if cause != nil {
		return NewExitCodeError(ExitCodeAuth,
			"Corp SSO authentication failed for %s: %s (%w).\n\nRemediation:\n%s",
			host, reason, cause, remediation)
	}
	return NewExitCodeError(ExitCodeAuth,
		"Corp SSO authentication failed for %s: %s.\n\nRemediation:\n%s",
		host, reason, remediation)
}
