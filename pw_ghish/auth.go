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
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"slices"
	"strings"
	"sync/atomic"
	"time"
)

// HTTP authentication overview:
//
//   ResolveAuthMode (auto | googler | community | none)
//          |
//          +---> resolveGerritStrategy ----> HTTPAuthStrategy (*gerritBearerStrategy)
//          |     (GERRIT_TOKEN | luci-auth)         |
//          |                                        v
//          +---> NewDynamicBearerStrategy -> HTTPAuthStrategy (*BearerTokenStrategy)
//                (LUCI, Busytown,                  |
//                 Issue Tracker)                   v
//                                         executeWithAuthStrategy
//                                           1. Prepare(req)   -> Bearer header, /a/ prefix
//                                           2. ensureSafeHTTPClient (HTTPS guard + cross-host strip)
//                                           3. HandleResponse -> token refresh -> anon GET fallback

// AuthMode describes the authentication policy mode (auto, googler, community, none).
type AuthMode string

const (
	AuthModeAuto      AuthMode = "auto"
	AuthModeGoogler   AuthMode = "googler"
	AuthModeCommunity AuthMode = "community"
	AuthModeNone      AuthMode = "none"
)

// AuthModeFlag is the global --auth-mode flag value.
var AuthModeFlag string

type authModeCacheKey struct {
	flag    string
	envMode string
	cfg     *Config
}

type cachedAuthMode struct {
	Mode AuthMode
	Why  string
}

func (cachedAuthMode) CachePolicy() CachePolicy {
	return CachePolicy{
		Tier: CacheInMemory,
		Name: "auth_mode",
	}
}

var authModeCache = NewCache[authModeCacheKey, cachedAuthMode]()

// ResolveAuthMode determines the active AuthMode (googler, community, or none) and a
// human-readable explanation of how it was selected.
//
// Precedence:
//  1. --auth-mode flag (AuthModeFlag)
//  2. GH_ISH_AUTH_MODE environment variable
//  3. git config ghish.authmode (from cfg, or ctx when cfg is nil)
//  4. Auto-detection (gosso on PATH, /usr/bin/sso-cred-helper present,
//     @google.com git user.email, or sso:// or rpc:// remote)
func ResolveAuthMode(ctx context.Context, cfg *Config) (AuthMode, string, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	if err := checkRemovedAuthMethodEnv(); err != nil {
		return "", "", err
	}
	if cfg == nil {
		cfg, _ = ctx.Value(configKey).(*Config)
	}

	cacheKey := authModeCacheKey{
		flag:    strings.TrimSpace(AuthModeFlag),
		envMode: strings.TrimSpace(os.Getenv("GH_ISH_AUTH_MODE")),
		cfg:     cfg,
	}
	if cfg != nil {
		if cached, ok := authModeCache.Get(cacheKey); ok {
			return cached.Mode, cached.Why, nil
		}
	}

	cacheAndReturn := func(m AuthMode, reason string) (AuthMode, string, error) {
		if cfg != nil {
			authModeCache.Put(cacheKey, cachedAuthMode{Mode: m, Why: reason})
		}
		return m, reason, nil
	}

	var rawMode, source string
	if cacheKey.flag != "" {
		rawMode = cacheKey.flag
		source = fmt.Sprintf("--auth-mode=%s", cacheKey.flag)
	} else if cacheKey.envMode != "" {
		rawMode = cacheKey.envMode
		source = fmt.Sprintf("GH_ISH_AUTH_MODE=%s", cacheKey.envMode)
	} else if cfg != nil && cfg.Git != nil {
		if v, err := cfg.GitClient().ConfigGet(ctx, "ghish.authmode"); err == nil && strings.TrimSpace(v) != "" {
			rawMode = strings.TrimSpace(v)
			source = fmt.Sprintf("git config ghish.authmode=%s", rawMode)
		}
	}

	if rawMode != "" {
		normalized := AuthMode(strings.ToLower(rawMode))
		switch normalized {
		case AuthModeGoogler, AuthModeCommunity, AuthModeNone:
			return cacheAndReturn(normalized, source)
		case AuthModeAuto:
			// Proceed to auto-detection below.
		default:
			return "", "", NewExitCodeError(ExitCodeAuth,
				"invalid authentication mode %q (from %s).\n\n"+
					"Valid modes are: 'auto', 'googler', 'community', 'none'.\n"+
					"  - googler:   Require authentication for Gerrit and LUCI (including internal builders)\n"+
					"  - community: Authenticate when configured; allow anonymous access to public Gerrit and LUCI\n"+
					"  - auto:      Detect 'googler' vs 'community' from the local environment\n"+
					"  - none:      Disable authentication",
				rawMode, source)
		}
	}

	if findGossoBinary() != "" {
		return cacheAndReturn(AuthModeGoogler, "auto-detected (gosso on PATH)")
	}

	if hasSSOCredHelper() {
		return cacheAndReturn(AuthModeGoogler, fmt.Sprintf("auto-detected (%s present)", ssoCredHelperPath))
	}

	var gc GitClient
	if cfg != nil && cfg.Git != nil {
		// Share the Config's client so these lookups hit its git config
		// prefetch instead of spawning `git config --get` per key.
		gc = cfg.GitClient()
	} else if DefaultGitRunner != nil {
		gc = NewGitClient(DefaultGitRunner)
	}
	if gc != nil {
		if email, err := gc.ConfigGet(ctx, "user.email"); err == nil {
			email = strings.TrimSpace(email)
			if strings.HasSuffix(strings.ToLower(email), "@google.com") {
				return cacheAndReturn(AuthModeGoogler, fmt.Sprintf("auto-detected (git user.email %s)", email))
			}
		}
		if origin, err := gc.ConfigGet(ctx, "remote.origin.url"); err == nil {
			if scheme, _, ok := parseInternalGitURL(origin); ok {
				return cacheAndReturn(AuthModeGoogler, fmt.Sprintf("auto-detected (%s:// git remote)", scheme))
			}
		}
	}

	return cacheAndReturn(AuthModeCommunity, "auto-detected (community environment)")
}

// checkRemovedAuthMethodEnv fails if the removed GH_ISH_AUTH_METHOD variable is
// still set to a non-empty value (empty or whitespace-only values are treated
// as unset), so a stale `none` can't silently turn into authenticated requests.
func checkRemovedAuthMethodEnv() error {
	v := strings.TrimSpace(os.Getenv("GH_ISH_AUTH_METHOD"))
	if v == "" {
		return nil
	}
	steps := append([]string{"Unset it: unset GH_ISH_AUTH_METHOD"}, gerritCredentialSteps("", AuthModeAuto)...)
	steps = append(steps, "Check auth status: gh auth status")
	return NewExitCodeError(ExitCodeAuth,
		"GH_ISH_AUTH_METHOD=%s is set, but GH_ISH_AUTH_METHOD has been removed.\n\n"+
			"gh-ish now picks Gerrit credentials automatically: GERRIT_TOKEN when set, otherwise luci-auth (for *.googlesource.com).\n"+
			"To disable authentication, use --auth-mode=none or GH_ISH_AUTH_MODE=none.\n\n"+
			"Remediation:\n%s",
		v, formatGerritRemediationSteps(steps...))
}

// AuthMethod describes a credential mechanism used to authenticate HTTP requests.
type AuthMethod string

const (
	AuthMethodToken AuthMethod = "token"
	AuthMethodNone  AuthMethod = "none"
)

func hostnameOf(raw string) string {
	if raw != "" && !strings.Contains(raw, "://") {
		raw = "https://" + raw
	}
	if u, err := url.Parse(raw); err == nil && u.Hostname() != "" {
		return u.Hostname()
	}
	return CleanGerritHost(raw)
}

func isLoopbackHost(host string) bool {
	switch strings.ToLower(hostnameOf(host)) {
	case "localhost", "127.0.0.1", "::1":
		return true
	}
	return false
}

func isHTTPSOrLoopbackURL(u *url.URL) bool {
	if u == nil {
		return false
	}
	if strings.EqualFold(u.Scheme, "https") {
		return true
	}
	return strings.EqualFold(u.Scheme, "http") && isLoopbackHost(u.Hostname())
}

func stripAuthOnCrossHostRedirect(req *http.Request, via []*http.Request) error {
	if len(via) >= 10 {
		return fmt.Errorf("stopped after 10 redirects")
	}
	if len(via) > 0 && req != nil && req.URL != nil && via[0].URL != nil {
		if !isHTTPSOrLoopbackURL(req.URL) || !strings.EqualFold(req.URL.Hostname(), via[0].URL.Hostname()) {
			req.Header.Del("Authorization")
			req.Header.Del("Cookie")
		}
	}
	return nil
}

// urlHostIn reports whether u is HTTPS (or loopback HTTP) and its hostname
// equals one of hosts, case-insensitively. Empty hosts never match.
func urlHostIn(u *url.URL, hosts ...string) bool {
	if !isHTTPSOrLoopbackURL(u) {
		return false
	}
	return slices.ContainsFunc(hosts, func(h string) bool {
		return h != "" && strings.EqualFold(u.Hostname(), h)
	})
}

// safeHTTPTransport wraps an HTTP RoundTripper to refuse sending credentials
// over cleartext non-loopback HTTP.
type safeHTTPTransport struct {
	direct http.RoundTripper
}

func (rt *safeHTTPTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	if !isHTTPSOrLoopbackURL(req.URL) &&
		(req.Header.Get("Authorization") != "" || req.Header.Get("Cookie") != "") {
		return nil, fmt.Errorf("refusing to send authenticated request over cleartext HTTP to %q", req.URL.Redacted())
	}
	return cmp.Or[http.RoundTripper](rt.direct, http.DefaultTransport).RoundTrip(req)
}

// ensureSafeHTTPClient returns c with CheckRedirect enforcing
// stripAuthOnCrossHostRedirect and Transport wrapped in safeHTTPTransport
// (composing with any existing CheckRedirect on c without mutating c in place).
// A nil c gives a client with no Timeout: requests are bounded by their
// context, so Ctrl-C cancels them and long log downloads are not cut off.
func ensureSafeHTTPClient(c *http.Client) *http.Client {
	if c == nil {
		return &http.Client{
			CheckRedirect: stripAuthOnCrossHostRedirect,
			Transport:     &safeHTTPTransport{},
		}
	}
	_, hasSafeRT := c.Transport.(*safeHTTPTransport)
	if hasSafeRT && c.CheckRedirect != nil {
		return c
	}
	cp := *c
	if !hasSafeRT {
		cp.Transport = &safeHTTPTransport{direct: c.Transport}
	}
	if c.CheckRedirect == nil {
		cp.CheckRedirect = stripAuthOnCrossHostRedirect
		return &cp
	}
	prev := c.CheckRedirect
	cp.CheckRedirect = func(req *http.Request, via []*http.Request) error {
		if err := stripAuthOnCrossHostRedirect(req, via); err != nil {
			return err
		}
		return prev(req, via)
	}
	return &cp
}

// HTTPAuthStrategy attaches credentials to outbound HTTP requests and handles
// authentication-related response recovery (such as dynamic token invalidation
// and re-minting or Gerrit anonymous fallback). Implementations must be safe
// for concurrent use across goroutines.
type HTTPAuthStrategy interface {
	// Describe returns the active AuthMethod and a human-readable description
	// of the credential source.
	Describe() (AuthMethod, string)
	// HasCredentials reports whether this strategy is configured with credentials.
	HasCredentials() bool
	// Prepare mutates req before transmission.
	Prepare(ctx context.Context, req *http.Request) error
	// HandleResponse inspects an HTTP response and reports whether the caller
	// should rebuild and retry the request.
	HandleResponse(ctx context.Context, req *http.Request, resp *http.Response) (retry bool, err error)
}

type authAttemptKeyType struct{}

var authAttemptKey authAttemptKeyType

// authAttemptState holds per-call retry and fallback state inside the context
// created by executeWithAuthStrategy so a shared HTTPAuthStrategy is never
// mutated across concurrent requests.
type authAttemptState struct {
	// replacementToken holds a fresh token minted during HandleResponse after a
	// cached token was rejected, so the retry's Prepare consumes it without
	// invoking the token resolver a second time.
	replacementToken    string
	retriedTokenRefresh bool
	anonymousFallback   bool
}

// authAttemptFrom returns the per-call retry state attached by
// executeWithAuthStrategy. When Prepare is called directly in a single-shot
// test without executeWithAuthStrategy, returning a fresh zero-valued struct
// (heap-allocated by Go escape analysis) represents a first attempt with no
// prior retry state.
func authAttemptFrom(ctx context.Context) *authAttemptState {
	if st, ok := ctx.Value(authAttemptKey).(*authAttemptState); ok && st != nil {
		return st
	}
	return &authAttemptState{}
}

// maxAuthStrategyRetries bounds executeWithAuthStrategy across multi-step
// authentication recovery (for example: dynamic token refresh followed by
// anonymous fallback).
const maxAuthStrategyRetries = 3

func drainAndCloseResponseBody(resp *http.Response) {
	if resp.Body == nil {
		return
	}
	_, _ = io.Copy(io.Discard, io.LimitReader(resp.Body, 4096))
	_ = resp.Body.Close()
}

// executeWithAuthStrategy executes an HTTP request using strategy, rebuilding
// and re-sending the request when strategy.HandleResponse requests a retry.
func executeWithAuthStrategy(
	ctx context.Context,
	hc *http.Client,
	strategy HTTPAuthStrategy,
	buildReq func(context.Context) (*http.Request, error),
) (*http.Response, error) {
	client := ensureSafeHTTPClient(hc)
	attemptCtx := context.WithValue(ctx, authAttemptKey, &authAttemptState{})
	var resp *http.Response
	for attempt := 0; attempt <= maxAuthStrategyRetries; attempt++ {
		if resp != nil {
			drainAndCloseResponseBody(resp)
			resp = nil
		}
		req, err := buildReq(attemptCtx)
		if err != nil {
			return nil, err
		}
		if err := strategy.Prepare(attemptCtx, req); err != nil {
			return nil, err
		}
		resp, err = client.Do(req)
		if err != nil {
			resp = nil
			if exitErr, ok := errors.AsType[*ExitCodeError](err); ok {
				return nil, exitErr
			}
			return nil, err
		}
		// Custom RoundTrippers (tests, subprocess transports) may omit
		// Response.Request on non-redirect responses; normalize it once here.
		if resp.Request == nil {
			resp.Request = req
		}
		retry, err := strategy.HandleResponse(attemptCtx, req, resp)
		if err != nil {
			drainAndCloseResponseBody(resp)
			return nil, err
		}
		if !retry {
			return resp, nil
		}
	}
	return resp, nil
}

// BearerTokenStrategy implements HTTPAuthStrategy for OAuth2 Bearer tokens.
// Construct via NewStaticBearerStrategy (explicit non-refreshable tokens such
// as GERRIT_TOKEN), NewDynamicBearerStrategy (refreshable tokens minted via
// luci-auth), or NewUnauthenticatedBearerStrategy. Instances are immutable once
// constructed and safe for concurrent use across goroutines.
type BearerTokenStrategy struct {
	description string
	trusted     func(u *url.URL) bool
	token       func(ctx context.Context, reqURL *url.URL) (string, error)
	invalidate  func(rejected string) // non-nil only for refreshable dynamic tokens
}

// NewUnauthenticatedBearerStrategy constructs a BearerTokenStrategy with no
// credentials, reporting desc (or "none") from Describe.
func NewUnauthenticatedBearerStrategy(desc string) *BearerTokenStrategy {
	return &BearerTokenStrategy{description: cmp.Or(desc, "none")}
}

// NewStaticBearerStrategy constructs a BearerTokenStrategy for an explicit,
// non-refreshable Bearer token (such as an environment variable override). If
// token is empty after trimming whitespace, it returns an unauthenticated
// strategy.
func NewStaticBearerStrategy(desc, token string, trusted func(*url.URL) bool) *BearerTokenStrategy {
	tok := strings.TrimSpace(token)
	if tok == "" {
		return NewUnauthenticatedBearerStrategy(desc)
	}
	return &BearerTokenStrategy{
		description: cmp.Or(desc, "token"),
		trusted:     trusted,
		token: func(context.Context, *url.URL) (string, error) {
			return tok, nil
		},
	}
}

// NewDynamicBearerStrategy constructs a BearerTokenStrategy for dynamically
// minted Bearer tokens that can be invalidated and re-minted once on rejection.
// Both token and invalidate must be non-nil.
func NewDynamicBearerStrategy(
	desc string,
	trusted func(*url.URL) bool,
	token func(context.Context, *url.URL) (string, error),
	invalidate func(string),
) *BearerTokenStrategy {
	if token == nil || invalidate == nil {
		panic("NewDynamicBearerStrategy requires non-nil token and invalidate functions")
	}
	return &BearerTokenStrategy{
		description: cmp.Or(desc, "token"),
		trusted:     trusted,
		token:       token,
		invalidate:  invalidate,
	}
}

// Describe returns the active AuthMethod and credential source description.
func (s *BearerTokenStrategy) Describe() (AuthMethod, string) {
	if !s.HasCredentials() {
		return AuthMethodNone, cmp.Or(s.description, "none")
	}
	return AuthMethodToken, cmp.Or(s.description, "token")
}

// HasCredentials reports whether a static or dynamic token source is configured.
func (s *BearerTokenStrategy) HasCredentials() bool {
	return s.token != nil
}

func (s *BearerTokenStrategy) isTrusted(u *url.URL) bool {
	return s.trusted != nil && s.trusted(u)
}

func (s *BearerTokenStrategy) tokenForAttempt(ctx context.Context, reqURL *url.URL) (string, error) {
	st := authAttemptFrom(ctx)
	// When HandleResponse invalidated a rejected cached token and minted a
	// fresh replacement to confirm a retry is worthwhile, consume that
	// pre-minted token here instead of invoking s.token a second time.
	if st.replacementToken != "" {
		tok := st.replacementToken
		st.replacementToken = ""
		return tok, nil
	}
	tok, err := s.token(ctx, reqURL)
	if err != nil {
		if cerr := ctx.Err(); cerr != nil {
			return "", cerr
		}
		return "", err
	}
	return strings.TrimSpace(tok), nil
}

// Prepare clears any existing Authorization header and attaches a Bearer token
// when s has credentials and req.URL is trusted.
func (s *BearerTokenStrategy) Prepare(ctx context.Context, req *http.Request) error {
	req.Header.Del("Authorization")
	if !s.HasCredentials() || !s.isTrusted(req.URL) {
		return nil
	}
	tok, err := s.tokenForAttempt(ctx, req.URL)
	if err != nil {
		return err
	}
	if tok != "" {
		req.Header.Set("Authorization", "Bearer "+tok)
	}
	return nil
}

func (s *BearerTokenStrategy) refreshRejectedToken(ctx context.Context, req *http.Request, resp *http.Response) (bool, error) {
	if s.invalidate == nil || !s.isTrusted(resp.Request.URL) {
		return false, nil
	}
	sentTok, ok := strings.CutPrefix(req.Header.Get("Authorization"), "Bearer ")
	if !ok || sentTok == "" {
		return false, nil
	}
	st := authAttemptFrom(ctx)
	if st.retriedTokenRefresh {
		return false, nil
	}
	st.retriedTokenRefresh = true
	s.invalidate(sentTok)
	newTok, err := s.tokenForAttempt(ctx, req.URL)
	if err != nil {
		return false, err
	}
	if newTok == "" || newTok == sentTok {
		return false, nil
	}
	st.replacementToken = newTok
	return true, nil
}

// HandleResponse invalidates and re-mints a dynamic Bearer token once on
// HTTP 401 Unauthorized when a fresh token differs from the rejected one.
func (s *BearerTokenStrategy) HandleResponse(ctx context.Context, req *http.Request, resp *http.Response) (bool, error) {
	if resp.StatusCode != http.StatusUnauthorized {
		return false, nil
	}
	return s.refreshRejectedToken(ctx, req, resp)
}

// modeAwareBearerToken adapts a service token resolver to a dynamic
// BearerTokenStrategy token callback under the active AuthMode: skipping
// minting in AuthModeNone, failing with ExitCodeAuth when requiredInGoogler is
// set in AuthModeGoogler, and proceeding unauthenticated in community mode or
// optional-auth backends.
func modeAwareBearerToken(
	service string,
	cfg *Config,
	requiredInGoogler bool,
	mint func(context.Context) (string, error),
) func(context.Context, *url.URL) (string, error) {
	return func(ctx context.Context, reqURL *url.URL) (string, error) {
		if cfg != nil && ctx.Value(configKey) == nil {
			ctx = context.WithValue(ctx, configKey, cfg)
		}
		mode, modeReason, err := ResolveAuthMode(ctx, nil)
		if err != nil {
			return "", err
		}
		if mode == AuthModeNone {
			return "", nil
		}
		tok, tokErr := mint(ctx)
		tok = strings.TrimSpace(tok)
		if tokErr == nil && tok != "" {
			return tok, nil
		}
		if tokErr == nil {
			tokErr = errors.New("empty token")
		}
		if cerr := ctx.Err(); cerr != nil {
			return "", cerr
		}
		if requiredInGoogler && mode == AuthModeGoogler {
			// Resolver errors such as DefaultLUCIToken's carry their own
			// remediation; keep only their first line so it is not repeated.
			return "", NewExitCodeError(ExitCodeAuth,
				"%s authentication required in googler mode (%s): no OAuth2 token for %s: %v\n\n"+
					"Remediation:\n"+
					"%s\n"+
					"  2. Check authentication status: gh auth status\n"+
					"  3. For public projects without internal credentials: export GH_ISH_AUTH_MODE=community",
				service, modeReason, reqURL.Host, firstLine(tokErr.Error()),
				luciAuthTokenRemediationStep1(luciAuthLoginCommand(mode)))
		}
		// OK: community mode and optional-auth backends proceed unauthenticated when no token is available.
		slog.Debug("proceeding without OAuth2 token", "service", service, "host", reqURL.Host, "err", tokErr)
		return "", nil
	}
}

// isPreSignedGCSURL reports whether u is a Google Cloud Storage URL or
// carries an X-Goog-Signature query parameter, where an Authorization header
// must not be attached.
func isPreSignedGCSURL(u *url.URL) bool {
	if u == nil {
		return false
	}
	host := strings.ToLower(u.Hostname())
	if host == "storage.googleapis.com" || strings.HasSuffix(host, ".storage.googleapis.com") {
		return true
	}
	return strings.Contains(u.RawQuery, "X-Goog-Signature")
}

// isTrustedLUCIURL reports whether u may receive the LUCI token: HTTPS to
// cr-buildbucket.appspot.com, logs.chromium.org, or the exact configured
// Buildbucket host, and never a pre-signed download URL.
func isTrustedLUCIURL(bbHost string, u *url.URL) bool {
	if isPreSignedGCSURL(u) {
		return false
	}
	configured := ""
	if bbHost != "" {
		configured = hostnameOf(bbHost)
	}
	return urlHostIn(u, "cr-buildbucket.appspot.com", "logs.chromium.org", configured)
}

// newLUCIAuth returns the HTTPAuthStrategy for LUCI Buildbucket and LogDog.
// Internal tryjobs and logs are not returned to unauthenticated requests, so a
// missing token is an error in googler mode.
func newLUCIAuth(bbHost string, tokenProvider func(context.Context) (string, error)) HTTPAuthStrategy {
	if tokenProvider == nil {
		tokenProvider = func(ctx context.Context) (string, error) {
			tok, _, err := LUCITokenResolver(ctx)
			return tok, err
		}
	}
	return NewDynamicBearerStrategy(
		"token (LUCI)",
		func(u *url.URL) bool {
			return isTrustedLUCIURL(bbHost, u)
		},
		modeAwareBearerToken("LUCI", nil, true, tokenProvider),
		invalidateLUCIAuthToken,
	)
}

func newLUCIHTTPAuthError(ctx context.Context, target string, statusCode int, body []byte) error {
	return NewExitCodeError(ExitCodeAuth,
		"%s returned HTTP %d: %s\n\n"+
			"To authenticate with LUCI:\n"+
			"%s\n"+
			"  2. Or set an OAuth token: export GHISH_LUCI_TOKEN=\"<token>\"\n"+
			"  3. Check auth status: gh auth status",
		target, statusCode, strings.TrimSpace(string(body)),
		luciAuthTokenRemediationStep1(luciAuthLoginCommandForScope(ctx, "")))
}

// isTrustedAndroidBuildURL reports whether u may receive the Android Build
// token: HTTPS (or loopback HTTP) to the exact configured Android Build API or
// CI host, and never a pre-signed GCS download URL.
func isTrustedAndroidBuildURL(apiBase, ciBase string, u *url.URL) bool {
	if isPreSignedGCSURL(u) {
		return false
	}
	return urlHostIn(u, hostnameOf(apiBase), hostnameOf(ciBase))
}

// newAndroidBuildAuth returns the HTTPAuthStrategy for Android Build API and
// CI hosts. Requests proceed without a token when none is available.
func newAndroidBuildAuth(apiBase, ciBase string, tokenProvider func(context.Context) (string, error), cfg *Config) HTTPAuthStrategy {
	if tokenProvider == nil {
		tokenProvider = func(ctx context.Context) (string, error) {
			return AndroidBuildTokenResolver(ctx)
		}
	}
	return NewDynamicBearerStrategy(
		"token (Android Build)",
		func(u *url.URL) bool {
			return isTrustedAndroidBuildURL(apiBase, ciBase, u)
		},
		modeAwareBearerToken("Android Build", cfg, false, tokenProvider),
		invalidateLUCIAuthToken,
	)
}

// ScopeBuganizer is the OAuth2 scope for Google Issue Tracker (Buganizer).
const ScopeBuganizer = "https://www.googleapis.com/auth/buganizer"

// isCorpIssueTrackerURL reports whether u is an HTTPS URL on
// issuetracker.corp.googleapis.com or another *.corp.googleapis.com host.
func isCorpIssueTrackerURL(u *url.URL) bool {
	if u == nil || !strings.EqualFold(u.Scheme, "https") {
		return false
	}
	h := strings.ToLower(strings.TrimSuffix(u.Hostname(), "."))
	return len(h) > len(".corp.googleapis.com") && strings.HasSuffix(h, ".corp.googleapis.com")
}

// ResolveIssueTrackerTokenWithSource resolves an OAuth2 access token for
// Google Issue Tracker from GHISH_ISSUE_TOKEN, BUGANIZER_TOKEN, or the shared
// luci-auth token, and returns its source.
func ResolveIssueTrackerTokenWithSource(ctx context.Context) (string, string, error) {
	for _, envVar := range []string{"GHISH_ISSUE_TOKEN", "BUGANIZER_TOKEN"} {
		if tok := strings.TrimSpace(os.Getenv(envVar)); tok != "" {
			return tok, envVar, nil
		}
	}
	tok, err := luciAuthTokenWithScope(ctx, ScopeBuganizer)
	if err == nil {
		return tok, "luci-auth", nil
	}
	if ctx != nil && ctx.Err() != nil {
		return "", "", ctx.Err()
	}
	return "", "", NewExitCodeError(ExitCodeAuth,
		"failed to authenticate with Google Issue Tracker: no OAuth2 token found.\n\n"+
			"Cause: Neither GHISH_ISSUE_TOKEN/BUGANIZER_TOKEN is set nor did luci-auth return a token: %v\n\n"+
			"Remediation:\n"+
			"%s\n"+
			"  2. Or provide a token explicitly:\n"+
			"     export GHISH_ISSUE_TOKEN=\"<token>\"\n"+
			"  3. Check authentication status:\n"+
			"     gh auth status",
		err, luciAuthTokenRemediationStep1(luciAuthLoginCommandForScope(ctx, ScopeBuganizer)))
}

// IssueTrackerTokenResolver resolves an Issue Tracker OAuth2 token and its source.
// It can be overridden in tests.
var IssueTrackerTokenResolver = ResolveIssueTrackerTokenWithSource

// issueTrackerTokenEnvSet reports whether GHISH_ISSUE_TOKEN or BUGANIZER_TOKEN
// supplies an explicit Issue Tracker token.
func issueTrackerTokenEnvSet() bool {
	return strings.TrimSpace(os.Getenv("GHISH_ISSUE_TOKEN")) != "" ||
		strings.TrimSpace(os.Getenv("BUGANIZER_TOKEN")) != ""
}

func issueTrackerCommunityWebURL(profile ProjectProfile) string {
	return strings.TrimSuffix(profile.IssueWebURL(0), "/issues/0")
}

// DefaultIssueTrackerToken resolves an OAuth2 access token with Buganizer scope.
func DefaultIssueTrackerToken(ctx context.Context) (string, error) {
	tok, _, err := IssueTrackerTokenResolver(ctx)
	return tok, err
}

func resolveIssueTrackerCommandAuth(ctx context.Context, cfg *Config, profile ProjectProfile) (func(context.Context) (string, error), error) {
	mode, modeReason, err := ResolveAuthMode(ctx, cfg)
	if err != nil {
		return nil, err
	}
	if mode == AuthModeNone {
		return nil, NewExitCodeError(ExitCodeAuth,
			"Google Issue Tracker requires authentication, which is disabled (%s).\n\n"+
				"Remediation:\n"+
				"  1. Re-enable authentication by removing --auth-mode=none / GH_ISH_AUTH_MODE=none.\n"+
				"  2. Check authentication status: gh auth status",
			modeReason)
	}
	// Community mode has no luci-auth token with the buganizer scope, so point
	// non-Googlers at the web tracker unless they supplied an explicit token.
	if mode == AuthModeCommunity && !issueTrackerTokenEnvSet() {
		authErr := NewExitCodeError(ExitCodeAuth,
			"Google Issue Tracker CLI commands (gh issue) require Google Corp access (%s).\n\n"+
				"Remediation:\n"+
				"  1. View or file issues on the web at %s\n"+
				"  2. If you are a Google employee on a Corp workstation, pass --auth-mode=googler (or set GH_ISH_AUTH_MODE=googler).",
			modeReason, issueTrackerCommunityWebURL(profile))
		return func(context.Context) (string, error) {
			return "", authErr
		}, nil
	}
	return nil, nil
}

type issueTrackerAuthStrategy struct {
	*BearerTokenStrategy
	endpointHost string
}

func (s *issueTrackerAuthStrategy) Prepare(ctx context.Context, req *http.Request) error {
	if !urlHostIn(req.URL, s.endpointHost) {
		return fmt.Errorf("refusing to send Issue Tracker credentials to %s: endpoint must be HTTPS", req.URL)
	}
	return s.BearerTokenStrategy.Prepare(ctx, req)
}

// newIssueTrackerAuth returns the HTTPAuthStrategy for the configured Issue
// Tracker endpoint, failing closed before RoundTrip if a request targets plain
// non-loopback HTTP or a different host.
func newIssueTrackerAuth(endpoint string, tokenProvider func(context.Context) (string, error)) HTTPAuthStrategy {
	if tokenProvider == nil {
		tokenProvider = DefaultIssueTrackerToken
	}
	endpointHost := hostnameOf(endpoint)
	return &issueTrackerAuthStrategy{
		endpointHost: endpointHost,
		BearerTokenStrategy: NewDynamicBearerStrategy(
			"token (Issue Tracker)",
			func(u *url.URL) bool {
				return urlHostIn(u, endpointHost)
			},
			func(ctx context.Context, _ *url.URL) (string, error) {
				tok, err := tokenProvider(ctx)
				if err == nil && strings.TrimSpace(tok) == "" {
					return "", errors.New("empty token")
				}
				return tok, err
			},
			invalidateLUCIAuthToken,
		),
	}
}

func isIssueTrackerQuotaProjectError(rawErr string) bool {
	return strings.Contains(rawErr, "USER_PROJECT_DENIED") ||
		strings.Contains(rawErr, "SERVICE_DISABLED") ||
		strings.Contains(rawErr, "has not been used in project") ||
		strings.Contains(rawErr, "cannot be identified with a client project")
}

func newIssueTrackerHTTPAuthError(ctx context.Context, endpoint, method, path, quotaProject string, statusCode int, respBytes []byte) error {
	rawErr := strings.TrimSpace(string(respBytes))
	if isIssueTrackerQuotaProjectError(rawErr) {
		svcName := "issuetracker.googleapis.com"
		if u, err := url.Parse(endpoint); err == nil && isCorpIssueTrackerURL(u) {
			svcName = "issuetracker.corp.googleapis.com"
		}
		if quotaProject != "" {
			return fmt.Errorf("Google Issue Tracker quota project error (HTTP %d) for %s %s.\n\n"+
				"Cause: The configured quota project %q (X-Goog-User-Project) does not have %s enabled,\n"+
				"or you lack the serviceusage.services.use permission on it.\n\n"+
				"Remediation:\n"+
				"  1. Enable %s on project %q.\n"+
				"  2. Or remove the override (GHISH_QUOTA_PROJECT / GOOGLE_CLOUD_QUOTA_PROJECT,\n"+
				"     issue.quota_project in .ghish.toml, or git config ghish.issue.quotaProject);\n"+
				"     gh-ish's luci-auth credentials do not need a quota project.\n\n"+
				"Server Details: %s", statusCode, method, path, quotaProject, svcName, svcName, quotaProject, rawErr)
		}
		return fmt.Errorf("Google Issue Tracker quota project error (HTTP %d) for %s %s.\n\n"+
			"Cause: The OAuth client that minted this token is not enabled for %s.\n"+
			"gh-ish's luci-auth token does not need a quota project; this usually means\n"+
			"GHISH_ISSUE_TOKEN/BUGANIZER_TOKEN came from another tool.\n\n"+
			"Remediation:\n"+
			"  1. Unset GHISH_ISSUE_TOKEN/BUGANIZER_TOKEN and re-authenticate with luci-auth:\n"+
			"     %s\n"+
			"  2. Or set a quota project that has %s enabled:\n"+
			"     git config --global ghish.issue.quotaProject \"<your-gcp-project-id>\"\n"+
			"     # or: export GHISH_QUOTA_PROJECT=\"<your-gcp-project-id>\"\n\n"+
			"Server Details: %s", statusCode, method, path, svcName, luciAuthLoginCommandForScope(ctx, ScopeBuganizer), svcName, rawErr)
	}
	return fmt.Errorf("Google Issue Tracker authentication failed (HTTP %d) for %s %s.\n\n"+
		"Cause: Your current OAuth token was rejected or lacks the 'https://www.googleapis.com/auth/buganizer' scope.\n\n"+
		"Remediation:\n"+
		"%s\n"+
		"  2. Or check if this issue belongs to a restricted internal component.\n\n"+
		"Server Details: %s",
		statusCode, method, path,
		luciAuthTokenRemediationStep1(luciAuthLoginCommandForScope(ctx, ScopeBuganizer)),
		rawErr)
}

var gerritTokenAuthRejectionMarkers = []string{
	"insufficient_scope",
	"invalid_token",
	"authentication required",
	"access_token_scope_insufficient",
}

// prefixReadCloser replays an already-read byte prefix before reading the
// remainder of closer, preserving closer.Close().
type prefixReadCloser struct {
	reader io.Reader
	closer io.Closer
}

func (r *prefixReadCloser) Read(p []byte) (int, error) { return r.reader.Read(p) }
func (r *prefixReadCloser) Close() error               { return r.closer.Close() }

// peekResponseBody reads up to limit bytes from resp.Body for error
// classification and re-buffers those bytes onto resp.Body. Go's
// http.Response.Body is a forward-only io.ReadCloser stream with no native peek
// or seek, so without re-buffering, downstream callers (such as
// newGerritAPIError) would read an empty or truncated body when HandleResponse
// decides not to retry.
func peekResponseBody(resp *http.Response, limit int64) string {
	if resp.Body == nil {
		return ""
	}
	buf, err := io.ReadAll(io.LimitReader(resp.Body, limit))
	if err != nil {
		// OK: best-effort body inspection preserves whatever prefix was read.
		slog.Debug("failed to read HTTP response body prefix", "err", err)
	}
	resp.Body = &prefixReadCloser{
		reader: io.MultiReader(bytes.NewReader(buf), resp.Body),
		closer: resp.Body,
	}
	return string(buf)
}

func isGerritTokenAuthRejection(resp *http.Response) bool {
	if resp == nil {
		return false
	}
	if resp.StatusCode == http.StatusUnauthorized {
		return true
	}
	if resp.StatusCode != http.StatusForbidden {
		return false
	}
	combined := strings.ToLower(resp.Header.Get("WWW-Authenticate") + "\n" + peekResponseBody(resp, 4096))
	return slices.ContainsFunc(gerritTokenAuthRejectionMarkers, func(m string) bool {
		return strings.Contains(combined, m)
	})
}

// LookPathFn allows overriding exec.LookPath in tests.
var LookPathFn = exec.LookPath

// lookPathFirst returns the first candidate found by LookPathFn, or "" if none exist.
func lookPathFirst(candidates ...string) string {
	for _, cand := range candidates {
		if path, err := LookPathFn(cand); err == nil && path != "" {
			return path
		}
	}
	return ""
}

func missingGossoAuthError(service, host, statusCmd string) error {
	remediation := formatGerritRemediationSteps(
		"Install gosso on a managed corp machine (Cloudtop, Corp Linux, or gMac):\n"+
			"     - Corp Linux:   sudo apt install gosso\n"+
			"     - macOS (gMac): sudo mule install gosso",
		"Ensure you have valid Corp SSO credentials:\n     gcert",
		"Check authentication status:\n     "+statusCmd,
	)
	return NewExitCodeError(ExitCodeAuth,
		"%s requires Corp SSO access to %s, but 'gosso' was not found on PATH.\n\nRemediation:\n%s",
		service, host, remediation)
}

type resolvedGerritAuth struct {
	method   AuthMethod
	desc     string
	token    string
	dynamic  bool
	unauthed bool
	cause    error // why no credential was found when unauthed
}

// errNonGoogleGerritHost is the resolvedGerritAuth cause when luci-auth is
// skipped because the Gerrit host is not Google-hosted.
var errNonGoogleGerritHost = errors.New("luci-auth tokens are only sent to *.googlesource.com")

// allowLoopbackGerritAuthForTests lets loopback Gerrit hosts (httptest
// servers) receive auto-minted luci-auth tokens. Only the unit-test TestMain
// sets it; in production a loopback host is treated like any other
// non-Google host, so a local process cannot harvest the luci-auth token.
var allowLoopbackGerritAuthForTests = false

// isGerritAutoAuthHost reports whether hostname h may receive an auto-minted
// luci-auth token: a Google-hosted Gerrit, or loopback under the test hook.
func isGerritAutoAuthHost(h string) bool {
	return isGoogleGerritHost(h) || (allowLoopbackGerritAuthForTests && isLoopbackHost(h))
}

// resolveGerritAuth picks the Gerrit credential for gerritHost (a hostname or
// URL) in mode: none when authentication is disabled, else GERRIT_TOKEN, else
// a luci-auth token. Auto-minted luci-auth tokens are only used for
// Google-hosted Gerrit (isGerritAutoAuthHost); an explicit GERRIT_TOKEN goes
// to any configured host. When no credential is found, the result is unauthed
// and cause says why.
func resolveGerritAuth(ctx context.Context, gerritHost string, mode AuthMode) (resolvedGerritAuth, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	if mode == AuthModeNone {
		return resolvedGerritAuth{
			method:   AuthMethodNone,
			desc:     "disabled (--auth-mode=none)",
			unauthed: true,
		}, nil
	}
	tok, err := gerritTokenFromEnv()
	if err != nil {
		return resolvedGerritAuth{
			method: AuthMethodToken,
			desc:   "token (GERRIT_TOKEN)",
		}, err
	}
	if tok != "" {
		return resolvedGerritAuth{
			method: AuthMethodToken,
			desc:   "token (GERRIT_TOKEN)",
			token:  tok,
		}, nil
	}
	if !isGerritAutoAuthHost(hostnameOf(gerritHost)) {
		return resolvedGerritAuth{
			method:   AuthMethodNone,
			desc:     "none",
			unauthed: true,
			cause:    errNonGoogleGerritHost,
		}, nil
	}
	tok, src, err := GerritTokenResolver(ctx)
	if err == nil && tok != "" {
		return resolvedGerritAuth{
			method:  AuthMethodToken,
			desc:    fmt.Sprintf("token (%s)", cmp.Or(src, "luci-auth")),
			token:   tok,
			dynamic: true,
		}, nil
	}
	if err == nil {
		err = errors.New("luci-auth returned no Gerrit token")
	}
	return resolvedGerritAuth{
		method:   AuthMethodNone,
		desc:     "none",
		unauthed: true,
		cause:    err,
	}, nil
}

// gerritCredentialSteps returns the host- and OS-aware credential setup steps
// for gerritHost (an empty string means the host is unknown).
func gerritCredentialSteps(gerritHost string, mode AuthMode) []string {
	raw := strings.TrimSpace(gerritHost)
	var steps []string
	if raw == "" || isGerritAutoAuthHost(hostnameOf(raw)) {
		if hasUsableSSOCredHelper() {
			steps = append(steps, "Run 'gcert' (with sso-cred-helper)")
		}
		login := fmt.Sprintf("Run '%s'", luciAuthLoginCommand(mode))
		if raw == "" {
			login += " (for *.googlesource.com)"
		}
		steps = append(steps, login)
	}
	steps = append(steps, `Set an OAuth2 token: export GERRIT_TOKEN="<token>"`)
	return steps
}

func formatGerritRemediationSteps(steps ...string) string {
	var b strings.Builder
	for i, step := range steps {
		if i > 0 {
			b.WriteByte('\n')
		}
		fmt.Fprintf(&b, "  %d. %s", i+1, step)
	}
	return b.String()
}

// gerritStatusRemediation returns the single-line Gerrit remediation shown by
// `gh auth status`.
func gerritStatusRemediation(gerritHost string, mode AuthMode) string {
	return strings.Join(gerritCredentialSteps(gerritHost, mode), "; ")
}

// gerritGooglerAuthError explains why googler mode refuses an unauthenticated
// Gerrit client for gerritHost.
func gerritGooglerAuthError(modeReason, gerritHost string, cause error) error {
	cleanHost := cmp.Or(CleanGerritHost(gerritHost), "<host>")
	if errors.Is(cause, errNonGoogleGerritHost) {
		steps := append(gerritCredentialSteps(gerritHost, AuthModeGoogler),
			"Use --auth-mode=community for anonymous reads",
			"Check auth status: gh auth status",
		)
		return NewExitCodeError(ExitCodeAuth,
			"Gerrit host %s is not a Google-hosted Gerrit (*.googlesource.com); gh-ish only sends "+
				"luci-auth tokens to Google hosts.\n\n"+
				"Remediation:\n%s",
			cleanHost, formatGerritRemediationSteps(steps...))
	}
	steps := append(gerritCredentialSteps(gerritHost, AuthModeGoogler),
		"Switch to community mode: export GH_ISH_AUTH_MODE=community",
		"Check auth status: gh auth status",
	)
	return NewExitCodeError(ExitCodeAuth,
		"Gerrit authentication required in googler mode (%s): no Gerrit credentials found for %s (%v).\n\n"+
			"Cause: In googler mode, gh-ish does not fall back to unauthenticated Gerrit requests.\n\n"+
			"Remediation:\n%s",
		modeReason, cleanHost, cause, formatGerritRemediationSteps(steps...))
}

type gerritStderrKey struct{}

func withGerritStderr(ctx context.Context, w io.Writer) context.Context {
	return context.WithValue(ctx, gerritStderrKey{}, w)
}

func gerritStderrFrom(ctx context.Context) io.Writer {
	if w, ok := ctx.Value(gerritStderrKey{}).(io.Writer); ok && w != nil {
		return w
	}
	return os.Stderr
}

// gerritBearerStrategy wraps BearerTokenStrategy with Gerrit REST API rules:
//   - strips Gerrit's "/a/" authenticated path prefix when sending an
//     unauthenticated request,
//   - treats Gerrit's HTTP 403 token/scope rejections (in addition to 401) as
//     eligible for one dynamic token refresh, and
//   - in community mode (allowAnonymousFallback), retries failed (400/401)
//     authenticated GETs on *.googlesource.com once anonymously without "/a/"
//     (excluding "/accounts/self", which has no anonymous equivalent).
type gerritBearerStrategy struct {
	bearer                 *BearerTokenStrategy
	allowAnonymousFallback bool
}

func (g *gerritBearerStrategy) Describe() (AuthMethod, string) {
	return g.bearer.Describe()
}

func (g *gerritBearerStrategy) HasCredentials() bool {
	return g.bearer.HasCredentials()
}

func (g *gerritBearerStrategy) canFallBackToAnonymous(req *http.Request) bool {
	return g.allowAnonymousFallback && req.Method == http.MethodGet &&
		isGoogleGerritHost(req.URL.Hostname()) && hasAuthenticatedPrefix(req.URL) &&
		!strings.HasSuffix(req.URL.Path, "/accounts/self")
}

func (g *gerritBearerStrategy) Prepare(ctx context.Context, req *http.Request) error {
	if authAttemptFrom(ctx).anonymousFallback {
		req.Header.Del("Authorization")
		stripAuthenticatedPrefix(req.URL)
		return nil
	}
	if err := g.bearer.Prepare(ctx, req); err != nil {
		return err
	}
	if req.Header.Get("Authorization") == "" && g.canFallBackToAnonymous(req) {
		stripAuthenticatedPrefix(req.URL)
	}
	return nil
}

func (g *gerritBearerStrategy) HandleResponse(ctx context.Context, req *http.Request, resp *http.Response) (bool, error) {
	if isGerritTokenAuthRejection(resp) {
		retry, err := g.bearer.refreshRejectedToken(ctx, req, resp)
		if retry || err != nil {
			return retry, err
		}
	}
	st := authAttemptFrom(ctx)
	if !st.anonymousFallback &&
		(resp.StatusCode == http.StatusBadRequest || resp.StatusCode == http.StatusUnauthorized) &&
		g.canFallBackToAnonymous(req) {
		st.anonymousFallback = true
		fmt.Fprintf(gerritStderrFrom(ctx),
			"Warning: authenticated request to %s returned %d; falling back to anonymous\n",
			req.URL.Path, resp.StatusCode)
		return true, nil
	}
	return false, nil
}

func gerritTrustedURL(host string, hasStaticToken bool) func(*url.URL) bool {
	return func(u *url.URL) bool {
		if host == "" || !urlHostIn(u, host) {
			return false
		}
		return hasStaticToken || isGerritAutoAuthHost(host)
	}
}

func newGerritBearerStrategy(host string, auth GerritAuth, desc, initialToken string) *gerritBearerStrategy {
	staticTok := strings.TrimSpace(auth.StaticToken)
	trusted := gerritTrustedURL(host, staticTok != "")
	var bearer *BearerTokenStrategy
	switch {
	case staticTok != "":
		bearer = NewStaticBearerStrategy(cmp.Or(desc, "token (GERRIT_TOKEN)"), staticTok, trusted)
	case auth.Dynamic:
		// resolveGerritStrategy already called GerritTokenResolver once while
		// constructing the client; consume that initial token on the first
		// request so single-request commands do not invoke the resolver twice.
		var prefetched atomic.Pointer[string]
		if initialToken != "" {
			prefetched.Store(&initialToken)
		}
		bearer = NewDynamicBearerStrategy(
			cmp.Or(desc, "token (luci-auth)"),
			trusted,
			func(ctx context.Context, reqURL *url.URL) (string, error) {
				if cerr := ctx.Err(); cerr != nil {
					return "", cerr
				}
				if p := prefetched.Swap(nil); p != nil {
					return *p, nil
				}
				tok, _, err := GerritTokenResolver(ctx)
				if err == nil && tok != "" {
					return tok, nil
				}
				if err == nil {
					err = errors.New("luci-auth returned no Gerrit token")
				}
				if cerr := ctx.Err(); cerr != nil {
					return "", cerr
				}
				if !auth.AllowAnonymousFallback {
					return "", gerritGooglerAuthError(auth.modeReason, reqURL.Host,
						fmt.Errorf("luci-auth token refresh failed: %w", err))
				}
				// OK: community mode falls back to anonymous Gerrit requests when token refresh fails.
				slog.Debug("proceeding with anonymous Gerrit request after token refresh failure", "host", reqURL.Host, "err", err)
				return "", nil
			},
			invalidateLUCIAuthToken,
		)
	default:
		bearer = NewUnauthenticatedBearerStrategy(cmp.Or(desc, "none"))
	}
	return &gerritBearerStrategy{
		bearer:                 bearer,
		allowAnonymousFallback: auth.AllowAnonymousFallback,
	}
}

// resolveGerritStrategy resolves the HTTPAuthStrategy for a Gerrit client
// targeting gerritURL under the active AuthMode:
//   - --auth-mode=none: unauthenticated; GERRIT_TOKEN is ignored.
//   - GERRIT_TOKEN set: a static token for the configured host.
//   - Otherwise a luci-auth token, only for Google-hosted Gerrit (or
//     loopback under the allowLoopbackGerritAuthForTests unit-test hook).
//     It is minted once here so Authenticated() is known up front.
//
// Googler mode never falls back to anonymous requests: without a credential
// it returns an ExitCodeAuth error. Community mode reads anonymously.
func resolveGerritStrategy(ctx context.Context, gerritURL string) (HTTPAuthStrategy, error) {
	mode, modeReason, err := ResolveAuthMode(ctx, nil)
	if err != nil {
		return nil, err
	}
	res, err := resolveGerritAuth(ctx, gerritURL, mode)
	if err != nil {
		return nil, err
	}
	if res.unauthed && mode == AuthModeGoogler {
		return nil, gerritGooglerAuthError(modeReason, gerritURL, res.cause)
	}
	auth := GerritAuth{AllowAnonymousFallback: mode != AuthModeGoogler}
	var initialToken string
	switch {
	case res.unauthed:
	case res.dynamic:
		auth.Dynamic = true
		auth.modeReason = modeReason
		initialToken = res.token
	default:
		auth.StaticToken = res.token
	}
	_, host := normalizeGerritBaseURL(gerritURL)
	return newGerritBearerStrategy(host, auth, res.desc, initialToken), nil
}

// ssoCredHelperPath is the Corp Linux path for the SSO credential helper
// used by luci-auth v1.6+ to mint OAuth2 tokens directly from 7-day gcert
// LOAS2 credentials. Non-login shells (spawned by IDEs, coding agents, or
// scripts) often do not source /etc/profile.d/sso-cred-helper.sh, leaving
// LUCI_AUTH_CREDENTIAL_HELPER unset.
const ssoCredHelperPath = "/usr/bin/sso-cred-helper"

type cachedLaptopRole bool

func (cachedLaptopRole) CachePolicy() CachePolicy {
	return CachePolicy{Tier: CacheInMemory, Name: "laptop_role"}
}

var (
	lsbReleasePath  = "/etc/lsb-release"
	runtimeGOOS     = runtime.GOOS
	laptopRoleCache = NewCache[string, cachedLaptopRole]()
)

// isLaptopRole reports whether the local Linux host has GOOGLE_ROLE=laptop in
// /etc/lsb-release (a gLinux laptop, where local LOAS credential helpers are
// unavailable even when a gcert certificate is present). The result is
// memoized in memory for the current lsbReleasePath and cleared by
// ResetAuthTokenCaches.
func isLaptopRole() bool {
	if runtimeGOOS != "linux" {
		return false
	}
	if val, ok := laptopRoleCache.Get(lsbReleasePath); ok {
		return bool(val)
	}
	val := readLaptopRole(lsbReleasePath)
	laptopRoleCache.Put(lsbReleasePath, cachedLaptopRole(val))
	return val
}

func readLaptopRole(path string) bool {
	data, err := os.ReadFile(path)
	if err != nil {
		// OK: /etc/lsb-release is absent on many non-gLinux distributions.
		slog.Debug("unable to read lsb-release for laptop role check", "path", path, "err", err)
		return false
	}
	for line := range strings.SplitSeq(string(data), "\n") {
		if val, ok := strings.CutPrefix(strings.TrimSpace(line), "GOOGLE_ROLE="); ok {
			if strings.Trim(strings.TrimSpace(val), `"'`) == "laptop" {
				return true
			}
		}
	}
	return false
}

// isLOASLinux reports whether the local host is a non-laptop Linux workstation
// where direct LOAS RPC helpers (sso-cred-helper, gosso -loas, git-remote-rpc)
// can be used.
func isLOASLinux() bool {
	return runtimeGOOS == "linux" && !isLaptopRole()
}

// hasSSOCredHelper reports whether the Corp Linux SSO credential helper is
// installed at ssoCredHelperPath.
func hasSSOCredHelper() bool {
	if runtimeGOOS != "linux" {
		return false
	}
	path, err := LookPathFn(ssoCredHelperPath)
	return err == nil && path != ""
}

// hasUsableSSOCredHelper reports whether ssoCredHelperPath is installed and can
// mint luci-auth tokens (non-laptop Linux with sso-cred-helper present).
func hasUsableSSOCredHelper() bool {
	return hasSSOCredHelper() && isLOASLinux()
}

// newOAuthTokenCommand constructs an exec.Cmd for an external OAuth CLI helper
// and auto-injects LUCI_AUTH_CREDENTIAL_HELPER when invoking luci-auth on LOAS
// Corp Linux if LUCI_AUTH_CREDENTIAL_HELPER is unset in the environment.
func newOAuthTokenCommand(ctx context.Context, name string, args ...string) *exec.Cmd {
	cmd := ExecCommandContext(ctx, name, args...)
	if filepath.Base(name) == "luci-auth" {
		if _, set := os.LookupEnv("LUCI_AUTH_CREDENTIAL_HELPER"); !set && hasUsableSSOCredHelper() {
			cmd.Env = append(cmd.Environ(), "LUCI_AUTH_CREDENTIAL_HELPER="+ssoCredHelperPath)
		}
	}
	return cmd
}

// OAuthTokenCommandRunner executes external CLI token helpers (luci-auth).
// It can be overridden in unit tests to prevent spawning real subprocesses.
var OAuthTokenCommandRunner = func(ctx context.Context, name string, args ...string) ([]byte, error) {
	return newOAuthTokenCommand(ctx, name, args...).Output()
}

// scriptDirEnvVar names the directory of the `gh` wrapper script, which the
// wrapper exports for the binary. See the wrapper for why it uses the _PW_
// prefix.
const scriptDirEnvVar = "_PW_GH_SCRIPT_DIR"

// findLuciAuthBinary locates the 'luci-auth' executable.
//
// Why this fallback exists (User Workflow Note):
// In a Bazelisk-only workflow or './gh' invocation without sourcing
// 'activate.sh', CIPD tools may not be on system PATH. We fall back only to
// explicit environment roots (PW_ENVIRONMENT_ROOT, BUILD_WORKSPACE_DIRECTORY,
// and _PW_GH_SCRIPT_DIR), never probing ambient cwd or Git repository paths.
//
// TODO: https://pwbug.dev/567289969 - Replace Pigweed CIPD directory probing
// once tool discovery is driven by project profile configuration or
// Bazel-managed toolchains.
func findLuciAuthBinary(ctx context.Context) string {
	p, _ := findLuciAuthBinaryWithSearched(ctx)
	return p
}

// findLuciAuthBinaryWithSearched is findLuciAuthBinary, also returning every
// location it checked, in order ("PATH" first), for error messages.
func findLuciAuthBinaryWithSearched(_ context.Context) (string, []string) {
	if path, err := LookPathFn("luci-auth"); err == nil && path != "" {
		return path, []string{"PATH"}
	}
	var candidates []string
	if envRoot := strings.TrimSpace(os.Getenv("PW_ENVIRONMENT_ROOT")); envRoot != "" {
		candidates = append(candidates,
			filepath.Join(envRoot, "cipd", "packages", "luci", "luci-auth"),
			filepath.Join(envRoot, "cipd", "packages", "pigweed", "bin", "luci-auth"),
		)
	}
	if wsDir := strings.TrimSpace(os.Getenv("BUILD_WORKSPACE_DIRECTORY")); wsDir != "" {
		candidates = append(candidates,
			filepath.Join(wsDir, "environment", "cipd", "packages", "luci", "luci-auth"),
			filepath.Join(wsDir, ".environment", "cipd", "packages", "luci", "luci-auth"),
		)
	}
	if scriptDir := strings.TrimSpace(os.Getenv(scriptDirEnvVar)); scriptDir != "" {
		candidates = append(candidates,
			filepath.Join(scriptDir, "environment", "cipd", "packages", "luci", "luci-auth"),
			filepath.Join(scriptDir, ".environment", "cipd", "packages", "luci", "luci-auth"),
		)
	}
	for i, cand := range candidates {
		if path, err := LookPathFn(cand); err == nil && path != "" {
			return path, append([]string{"PATH"}, candidates[:i+1]...)
		}
	}
	return "", append([]string{"PATH"}, candidates...)
}

// errLUCIAuthNotFound is wrapped by luciAuthToken errors when no luci-auth
// binary exists. It is the only token failure remembered for the process.
var errLUCIAuthNotFound = errors.New("luci-auth not found")

// commandErrorDetail summarizes why a token helper command failed: the first
// line of its stderr when available, else the error itself.
func commandErrorDetail(err error) string {
	if err == nil {
		return "empty token output"
	}
	var exitErr *exec.ExitError
	if errors.As(err, &exitErr) {
		if msg := strings.TrimSpace(string(exitErr.Stderr)); msg != "" {
			return firstLine(msg)
		}
	}
	return err.Error()
}

const (
	defaultOAuthTokenTTL = 30 * time.Minute
	maxOAuthTokenTTL     = 45 * time.Minute
	luciOAuthActiveKey   = "active"
)

func parseOAuthTokenOutput(out []byte, now time.Time) (string, time.Time) {
	tok := strings.TrimSpace(string(out))
	expiry := now.Add(defaultOAuthTokenTTL)
	var parsed struct {
		Token  string `json:"token"`
		Expiry int64  `json:"expiry"`
	}
	if json.Unmarshal(out, &parsed) == nil {
		tok = strings.TrimSpace(parsed.Token)
		if parsed.Expiry > 0 {
			expiry = time.Unix(parsed.Expiry, 0).Add(-60 * time.Second)
		}
	}
	return tok, expiry
}

// mintLUCIAuthToken runs luci-auth once for scopes (a space-separated
// luciAuthScopes set). It never falls back to another tool: luci-auth (with
// sso-cred-helper on Corp Linux, or `luci-auth login` elsewhere) covers every
// scope gh-ish needs.
func mintLUCIAuthToken(ctx context.Context, scopes string) (string, time.Time, error) {
	bin, searched := findLuciAuthBinaryWithSearched(ctx)
	if bin == "" {
		if err := ctx.Err(); err != nil {
			return "", time.Time{}, err
		}
		return "", time.Time{}, fmt.Errorf("%w (searched %s)", errLUCIAuthNotFound, strings.Join(searched, ", "))
	}
	out, err := OAuthTokenCommandRunner(ctx, bin, "token", "-scopes", scopes, "-json-output", "-")
	if err != nil {
		if ctxErr := ctx.Err(); ctxErr != nil {
			return "", time.Time{}, ctxErr
		}
		return "", time.Time{}, fmt.Errorf("luci-auth (%s) returned no OAuth2 token: %s", bin, commandErrorDetail(err))
	}
	tok, expiry := parseOAuthTokenOutput(out, currentCacheEnv().Now())
	if tok == "" {
		return "", time.Time{}, fmt.Errorf("luci-auth (%s) returned no OAuth2 token: %s", bin, commandErrorDetail(nil))
	}
	return tok, expiry, nil
}

type cachedOAuthToken struct {
	Scopes      string
	Token       string
	NotFoundErr error
}

func (cachedOAuthToken) CachePolicy() CachePolicy {
	return CachePolicy{
		Tier:   CacheInMemory,
		Name:   "luci_oauth_tokens",
		MaxTTL: maxOAuthTokenTTL,
	}
}

var luciOAuthCache = NewCache[string, cachedOAuthToken]()

// errLUCIAuthDisabled is returned by luciAuthToken in auth mode none.
var errLUCIAuthDisabled = errors.New("luci-auth token not requested: authentication is disabled (auth mode none)")

// luciAuthToken returns the process-wide luci-auth OAuth2 token for the
// luciAuthScopes of the auth mode resolved from ctx, minting it on first use
// or after expiry. It never runs luci-auth when the mode can't be resolved or
// is none.
func luciAuthToken(ctx context.Context) (string, error) {
	return luciAuthTokenWithScope(ctx, "")
}

// luciAuthTokenWithScope is luciAuthToken for a service that needs scope ("" for
// none beyond the base set). When the mode's scope set lacks scope (Buganizer
// or Android Build in community mode), it fails without running luci-auth
// rather than send a token the service would reject.
func luciAuthTokenWithScope(ctx context.Context, scope string) (string, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	if err := ctx.Err(); err != nil {
		return "", err
	}
	mode, _, err := ResolveAuthMode(ctx, nil)
	if err != nil {
		return "", err
	}
	scopeSet := luciAuthScopes(mode)
	if len(scopeSet) == 0 {
		return "", errLUCIAuthDisabled
	}
	if scope != "" && !slices.Contains(scopeSet, scope) {
		if mode != AuthModeGoogler {
			return "", fmt.Errorf("the %s-mode luci-auth token does not include the %s scope "+
				"(Googlers: export GH_ISH_AUTH_MODE=googler)", mode, scope)
		}
		return "", fmt.Errorf("the %s-mode luci-auth token does not include the %s scope", mode, scope)
	}
	scopes := strings.Join(scopeSet, " ")
	loadToken := func(ctx context.Context) (cachedOAuthToken, time.Time, error) {
		tok, expiry, err := mintLUCIAuthToken(ctx, scopes)
		if errors.Is(err, errLUCIAuthNotFound) {
			// Negative-cache missing luci-auth so commands in community mode do
			// not re-probe CIPD paths and git rev-parse on every HTTP request.
			return cachedOAuthToken{NotFoundErr: err}, time.Time{}, nil
		}
		if err != nil {
			return cachedOAuthToken{}, time.Time{}, err
		}
		return cachedOAuthToken{Scopes: scopes, Token: tok}, expiry, nil
	}
	entry, err := luciOAuthCache.GetOrLoad(ctx, luciOAuthActiveKey, loadToken)
	if err == nil && entry.NotFoundErr == nil && entry.Scopes != scopes {
		luciOAuthCache.DeleteIf(luciOAuthActiveKey, func(cur cachedOAuthToken) bool {
			return cur.NotFoundErr == nil && cur.Scopes != scopes
		})
		entry, err = luciOAuthCache.GetOrLoad(ctx, luciOAuthActiveKey, loadToken)
	}
	if err != nil {
		return "", err
	}
	if entry.NotFoundErr != nil {
		return "", entry.NotFoundErr
	}
	return entry.Token, nil
}

// invalidateLUCIAuthToken drops the cached token if it equals rejected, so the
// next luciAuthToken re-mints. An empty rejected value is a no-op, so a caller
// that sent no token can never wipe the shared cache.
func invalidateLUCIAuthToken(rejected string) {
	if rejected == "" {
		return
	}
	luciOAuthCache.DeleteIf(luciOAuthActiveKey, func(cur cachedOAuthToken) bool {
		return cur.Token != "" && cur.Token == rejected
	})
}

// ScopeAndroidBuildInternal is the OAuth2 scope for Android Build Internal v3 REST APIs.
const ScopeAndroidBuildInternal = "https://www.googleapis.com/auth/androidbuild.internal"

// ScopeGerritCodeReview is the OAuth2 scope for Gerrit Code Review REST APIs.
const ScopeGerritCodeReview = "https://www.googleapis.com/auth/gerritcodereview"

// ScopeUserInfoEmail is the OAuth2 scope for reading the user's email address.
const ScopeUserInfoEmail = "https://www.googleapis.com/auth/userinfo.email"

// luciAuthScopes returns the one scope set gh-ish requests from luci-auth in
// mode. luci-auth only mints tokens for the exact scope set the user logged in
// with, so each mode uses a single set and needs one `luci-auth login`:
//   - googler: Gerrit, userinfo.email, Buganizer, and Android Build, for every
//     Googler regardless of repository.
//   - community: Gerrit and userinfo.email only.
//   - none: no scopes; gh-ish never runs luci-auth.
func luciAuthScopes(mode AuthMode) []string {
	switch mode {
	case AuthModeGoogler:
		return []string{ScopeGerritCodeReview, ScopeUserInfoEmail, ScopeBuganizer, ScopeAndroidBuildInternal}
	case AuthModeCommunity:
		return []string{ScopeGerritCodeReview, ScopeUserInfoEmail}
	default:
		return nil
	}
}

// luciAuthLoginCommand returns the login command for mode to print in
// remediation text. Auth mode none never runs luci-auth; it shows the
// community command, the minimum needed once authentication is re-enabled.
func luciAuthLoginCommand(mode AuthMode) string {
	if mode != AuthModeGoogler {
		mode = AuthModeCommunity
	}
	return fmt.Sprintf("luci-auth login -scopes %q", strings.Join(luciAuthScopes(mode), " "))
}

// luciAuthLoginCommandForScope is luciAuthLoginCommand for the auth mode
// resolved from ctx when a service needs scope. When the mode's scope set
// lacks scope (Buganizer or Android Build in community mode), only googler
// mode can help, so it shows how a misdetected Googler switches to it.
func luciAuthLoginCommandForScope(ctx context.Context, scope string) string {
	mode, _, err := ResolveAuthMode(ctx, nil)
	if err != nil {
		mode = AuthModeGoogler
	}
	if mode == AuthModeCommunity && scope != "" && !slices.Contains(luciAuthScopes(mode), scope) {
		return "(Googlers) export GH_ISH_AUTH_MODE=googler; " + luciAuthLoginCommand(AuthModeGoogler)
	}
	return luciAuthLoginCommand(mode)
}

// ResetAuthTokenCaches clears all in-process auth and account caches.
func ResetAuthTokenCaches() {
	resetAllCaches()
}

// gerritTokenFromEnv returns the trimmed GERRIT_TOKEN. A value containing ':'
// is a username:password pair for HTTP Basic auth, which only non-Google
// Gerrit servers use and gh-ish does not support yet; sending it as a Bearer
// token would fail with a confusing 401, so it is rejected up front.
func gerritTokenFromEnv() (string, error) {
	tok := strings.TrimSpace(os.Getenv("GERRIT_TOKEN"))
	if strings.Contains(tok, ":") {
		return "", NewExitCodeError(ExitCodeAuth,
			"GERRIT_TOKEN looks like a username:password pair, but HTTP Basic auth "+
				"(used by non-Google Gerrit servers) is not supported yet.\n\n"+
				"Set GERRIT_TOKEN to an OAuth2 access token for a *.googlesource.com "+
				"Gerrit host, or unset it to use luci-auth:\n"+
				"  unset GERRIT_TOKEN")
	}
	return tok, nil
}

// DefaultGerritToken resolves a Gerrit OAuth2 access token from GERRIT_TOKEN
// or the shared luci-auth token (see luciAuthToken).
func DefaultGerritToken(ctx context.Context) (string, string, error) {
	tok, err := gerritTokenFromEnv()
	if err != nil {
		return "", "", err
	}
	if tok != "" {
		return tok, "GERRIT_TOKEN", nil
	}
	tok, err = luciAuthToken(ctx)
	if err != nil {
		return "", "", err
	}
	return tok, "luci-auth", nil
}

// GerritTokenResolver resolves OAuth2 tokens with gerritcodereview scope for Gerrit REST and Git push fallbacks.
// It can be overridden in unit tests.
var GerritTokenResolver = DefaultGerritToken

// DefaultAndroidBuildToken resolves an OAuth2 access token for Android Build
// Internal v3 and ci.android.com from GHISH_ANDROID_BUILD_TOKEN or the shared
// luci-auth token.
func DefaultAndroidBuildToken(ctx context.Context) (string, error) {
	if tok := strings.TrimSpace(os.Getenv("GHISH_ANDROID_BUILD_TOKEN")); tok != "" {
		return tok, nil
	}
	return luciAuthTokenWithScope(ctx, ScopeAndroidBuildInternal)
}

// AndroidBuildTokenResolver is the function used to resolve Android Build OAuth2 tokens.
// It can be overridden in tests.
var AndroidBuildTokenResolver = DefaultAndroidBuildToken

// luciAuthTokenRemediationStep1 formats the primary remediation step for
// minting a luci-auth token with loginCmd, including Corp Linux gcert advice
// only when sso-cred-helper is installed and usable (skipping non-Linux,
// external Linux/Crostini without sso-cred-helper, and GOOGLE_ROLE=laptop
// hosts).
func luciAuthTokenRemediationStep1(loginCmd string) string {
	if !hasUsableSSOCredHelper() {
		return fmt.Sprintf("  1. Authenticate with luci-auth:\n     %s", loginCmd)
	}
	return fmt.Sprintf("  1. Corp Linux: run 'gcert'. Otherwise:\n     %s", loginCmd)
}

// DefaultLUCIToken resolves an OAuth2 access token for LUCI Buildbucket and
// LogDog from GHISH_LUCI_TOKEN, LUCI_TOKEN, or the shared luci-auth token.
func DefaultLUCIToken(ctx context.Context) (string, string, error) {
	if tok := strings.TrimSpace(os.Getenv("GHISH_LUCI_TOKEN")); tok != "" {
		return tok, "GHISH_LUCI_TOKEN", nil
	}
	if tok := strings.TrimSpace(os.Getenv("LUCI_TOKEN")); tok != "" {
		return tok, "LUCI_TOKEN", nil
	}
	tok, err := luciAuthToken(ctx)
	if err == nil {
		return tok, "luci-auth", nil
	}
	if ctx != nil && ctx.Err() != nil {
		return "", "", ctx.Err()
	}
	return "", "", NewExitCodeError(ExitCodeAuth,
		"failed to obtain LUCI OAuth2 token: %w.\n\n"+
			"Remediation:\n"+
			"%s\n"+
			"  2. Or provide a token explicitly:\n"+
			"     export GHISH_LUCI_TOKEN=\"<token>\"\n"+
			"  3. Check authentication status:\n"+
			"     gh auth status",
		err, luciAuthTokenRemediationStep1(luciAuthLoginCommandForScope(ctx, "")))
}

// LUCITokenResolver is the function used to resolve LUCI OAuth2 tokens.
// It can be overridden in tests.
var LUCITokenResolver = DefaultLUCIToken
