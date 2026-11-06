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
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"net/url"
	"os"
	"strings"
	"time"
)

// Corp SSO Gerrit authentication via the local BeyondCorp Uplink proxy:
//
//	AdaptiveGerritStrategy
//	  |-- CorpSSOStrategy ---> ssoSessions ---> mintViaGosso (/dev/fd/3 jar)
//	  |          |
//	  |          +---> corpUplinkHTTPTransport (127.0.0.1:999 -> corp domain)
//	  |
//	  +-- gerritBearerStrategy (lazy OAuth2 fallback on open hosts; pivots to
//	                            Corp SSO & records hosts.json on IP restriction)

const (
	maxSSOSessionLifetime     = 4 * time.Hour // matches git-remote-sso's 4h SSO ticket cap
	maxIPRestrictionBodyBytes = 4096

	hostCacheEnvVar = "GHISH_HOST_CACHE"
	corpSSOEnvVar   = "GHISH_CORP_SSO"
)

// AdaptiveGerritStrategy routes googler-mode requests for a Google-hosted
// Gerrit instance through CorpSSOStrategy (the local BeyondCorp Uplink proxy)
// whenever local SSO helpers exist, falling back to direct OAuth2 Bearer once
// per host per process on open hosts when Corp SSO fails, and pivoting direct
// to CorpSSOStrategy on cold UberProxy IP-restriction 403/401 responses.
type AdaptiveGerritStrategy struct {
	corpSSO *CorpSSOStrategy
	direct  *gerritBearerStrategy
}

func (s *AdaptiveGerritStrategy) corpSSOActive() bool {
	return corpSSOWouldBeActive(s.corpSSO.Host)
}

func (s *AdaptiveGerritStrategy) active() HTTPAuthStrategy {
	if s.corpSSOActive() {
		return s.corpSSO
	}
	return s.direct
}

// Describe returns the active AuthMethod and credential source description.
func (s *AdaptiveGerritStrategy) Describe() (AuthMethod, string) {
	return s.active().Describe()
}

// HasCredentials reports whether the currently active sub-strategy carries credentials.
func (s *AdaptiveGerritStrategy) HasCredentials() bool {
	return s.active().HasCredentials()
}

func (s *AdaptiveGerritStrategy) stageFallback(st *authAttemptState, err error) bool {
	pathErr, ok := errors.AsType[*corpSSOPathError](err)
	if !ok || pathErr == nil || pathErr.Reason == "" || defaultHostCache.IsCorpOnly(s.corpSSO.Host) {
		return false
	}
	st.corpSSOFallback = pathErr
	return true
}

func (s *AdaptiveGerritStrategy) responseFallbackError(req *http.Request, resp *http.Response) error {
	// Only GET/HEAD 403s trigger fallback: Gerrit returns 403 on mutations for
	// ordinary label/ACL restrictions even when Corp SSO succeeded, which must
	// not downgrade the host to OAuth2.
	if resp.StatusCode == http.StatusForbidden && (req.Method == http.MethodGet || req.Method == http.MethodHead) {
		return corpSSOError(s.corpSSO.Host, "the host returned HTTP 403 over Corp SSO (hosts that grant access through Google Groups are only authorized via OAuth2)", nil)
	}
	return nil
}

// Prepare delegates to CorpSSOStrategy when Corp SSO is active for the host
// (falling back to direct OAuth2 when Corp SSO fails on an open host), or to
// direct when OAuth2 is active.
func (s *AdaptiveGerritStrategy) Prepare(ctx context.Context, req *http.Request) error {
	st := authAttemptFrom(ctx)
	if st.corpSSOFallback == nil && s.corpSSOActive() {
		err := s.corpSSO.Prepare(ctx, req)
		if err == nil {
			return nil
		}
		if !s.stageFallback(st, err) {
			return err
		}
	}
	if err := s.direct.Prepare(ctx, req); err != nil {
		if st.corpSSOFallback != nil {
			return st.corpSSOFallback.Err
		}
		return err
	}
	return nil
}

// HandleTransportError implements transportErrorRecoverer: when a Corp SSO
// request failed before any response because Uplink could not be reached on an
// open host, it stages a fallback so Prepare retries via direct OAuth2.
func (s *AdaptiveGerritStrategy) HandleTransportError(ctx context.Context, req *http.Request, err error) bool {
	return isCorpSSORequest(req) && s.stageFallback(authAttemptFrom(ctx), err)
}

// HandleResponse handles Corp SSO session refresh and open-host fallback to
// direct OAuth2, as well as cold UberProxy IP-restriction 403/401 pivots.
func (s *AdaptiveGerritStrategy) HandleResponse(ctx context.Context, req *http.Request, resp *http.Response) (retry bool, err error) {
	st := authAttemptFrom(ctx)
	if isCorpSSORequest(req) {
		retry, err := s.corpSSO.HandleResponse(ctx, req, resp)
		if retry {
			return true, err
		}
		if s.stageFallback(st, cmp.Or(err, s.responseFallbackError(req, resp))) {
			return true, nil
		}
		return false, err
	}
	if isUberProxyIPRestriction(resp) {
		defaultHostCache.MarkCorpOnly(s.corpSSO.Host)
		if st.corpSSOFallback != nil {
			return false, st.corpSSOFallback.Err
		}
		return true, nil
	}
	retry, err = s.direct.HandleResponse(ctx, req, resp)
	if retry || err != nil {
		return retry, err
	}
	if st.corpSSOFallback != nil {
		markCorpSSOFallback(s.corpSSO.Host, st.corpSSOFallback.Reason)
	}
	return false, nil
}

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
		return ssoSession{}, time.Time{}, &corpSSOPathError{
			Reason: "no Corp SSO credentials could be minted (gosso not found on PATH)",
			Err:    missingGossoAuthError("Gerrit", s.Host, "gh auth status --host "+s.Host),
		}
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
// redirect or HTTP 401, and rejects HTTP 502 from the local Uplink proxy.
func (s *CorpSSOStrategy) HandleResponse(ctx context.Context, req *http.Request, resp *http.Response) (retry bool, err error) {
	if !s.matchesRequest(req) {
		return false, nil
	}
	if resp.StatusCode == http.StatusBadGateway {
		return false, corpSSOError(s.Host, "local BeyondCorp Uplink proxy returned HTTP 502 Bad Gateway", nil)
	}
	if !isCorpSSOSessionRejected(resp.StatusCode) {
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

func hasCorpOnlyIPRestrictionText(s string) bool {
	return strings.Contains(s, "PERMISSION_DENIED_BY_IP_RESTRICTIONS") ||
		(strings.Contains(s, "Access must be through ") && strings.Contains(s, gobCorpDomainSuffix))
}

func isUberProxyIPRestriction(resp *http.Response) bool {
	return (resp.StatusCode == http.StatusForbidden || resp.StatusCode == http.StatusUnauthorized) &&
		hasCorpOnlyIPRestrictionText(peekResponseBody(resp, maxIPRestrictionBodyBytes))
}

type hostClassification struct {
	CorpOnly bool `json:"corp_only"`
}

func (hostClassification) CachePolicy() CachePolicy {
	return CachePolicy{
		Tier:       CachePersistent,
		Name:       "hosts",
		MaxTTL:     7 * 24 * time.Hour,
		DisableEnv: hostCacheEnvVar,
	}
}

type hostCache struct {
	*Cache[string, hostClassification]
}

var defaultHostCache = &hostCache{
	Cache: NewCache[string, hostClassification](),
}

// IsCorpOnly reports whether host is recorded in hosts.json (or in-memory) as
// requiring Corp SSO / UberProxy access.
func (hc *hostCache) IsCorpOnly(host string) bool {
	entry, ok := hc.Get(normalizeCorpHostKey(host))
	return ok && entry.CorpOnly
}

// MarkCorpOnly records host as requiring Corp SSO / UberProxy access.
func (hc *hostCache) MarkCorpOnly(host string) {
	hc.PutIfAbsent(normalizeCorpHostKey(host), hostClassification{CorpOnly: true})
}

type corpSSOFallback string

func (corpSSOFallback) CachePolicy() CachePolicy {
	return CachePolicy{Tier: CacheInMemory, Name: "corp_sso_fallbacks"}
}

var corpSSOFallbacks = NewCache[string, corpSSOFallback]()

func markCorpSSOFallback(host, reason string) {
	key := normalizeCorpHostKey(host)
	if corpSSOFallbacks.PutIfAbsent(key, corpSSOFallback(reason)) {
		slog.Warn(fmt.Sprintf("Gerrit requests to %s are using OAuth2 instead of Corp SSO: %s; votes may not count for review enforcement", key, reason))
	}
}

// corpSSOUnavailableReason returns why Corp SSO is not the active Gerrit
// transport for host in googler mode, or "" when Corp SSO would be used (or
// host is not a Google-hosted Gerrit).
func corpSSOUnavailableReason(host string) string {
	key := normalizeCorpHostKey(host)
	if key == "" || defaultHostCache.IsCorpOnly(key) {
		return ""
	}
	if strings.TrimSpace(os.Getenv(corpSSOEnvVar)) == "0" {
		return corpSSOEnvVar + "=0 is set"
	}
	if fb, ok := corpSSOFallbacks.Get(key); ok {
		return string(fb)
	}
	if findGossoBinary() == "" {
		return "gosso was not found on PATH"
	}
	return ""
}

func corpSSOWouldBeActive(host string) bool {
	return normalizeCorpHostKey(host) != "" && corpSSOUnavailableReason(host) == ""
}

// corpSSOStatusWarning returns an auth-status warning when a Googler is
// authenticated to an open Google-hosted Gerrit instance over OAuth2 rather
// than Corp SSO, or "" when Corp SSO is active or not applicable.
func corpSSOStatusWarning(host string, mode AuthMode, method AuthMethod, methodDesc string) string {
	if mode != AuthModeGoogler || method == AuthMethodCorpSSO ||
		normalizeCorpHostKey(host) == "" || defaultHostCache.IsCorpOnly(host) {
		return ""
	}
	reason := cmp.Or(corpSSOUnavailableReason(host), "authenticating with "+methodDesc)
	return fmt.Sprintf("Corp SSO is not in use for %s (%s). Code-Review votes sent over OAuth2 "+
		"lack BeyondCorp device trust and are recorded as 'missing device trust' by Gerrit review enforcement "+
		"on hosts that verify it.", host, reason)
}

func normalizeCorpHostKey(host string) string {
	canon := CanonicalGerritHost(host)
	if !isGoogleGerritHost(canon) {
		return ""
	}
	return canon
}

// corpSSOPathError wraps an ExitCodeAuth error from the Corp SSO transport
// (credential minting, the local Uplink proxy, or a terminal session
// rejection) with a one-line reason.
type corpSSOPathError struct {
	Reason string
	Err    error
}

func (e *corpSSOPathError) Error() string { return e.Err.Error() }

func (e *corpSSOPathError) Unwrap() error { return e.Err }

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

func corpSSOError(host, reason string, cause error, extraSteps ...string) *corpSSOPathError {
	steps := append(
		[]string{"Refresh your corp single sign-on certificate:\n     gcert"},
		extraSteps...,
	)
	steps = append(steps, "Check authentication status:\n     gh auth status --host "+host)
	remediation := formatGerritRemediationSteps(steps...)
	if cause != nil {
		return &corpSSOPathError{
			Reason: reason + " (" + firstLine(cause.Error()) + ")",
			Err: NewExitCodeError(ExitCodeAuth,
				"Corp SSO authentication failed for %s: %s (%w).\n\nRemediation:\n%s",
				host, reason, cause, remediation),
		}
	}
	return &corpSSOPathError{
		Reason: reason,
		Err: NewExitCodeError(ExitCodeAuth,
			"Corp SSO authentication failed for %s: %s.\n\nRemediation:\n%s",
			host, reason, remediation),
	}
}
