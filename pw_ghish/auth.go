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
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"
	"sync"
	"time"
)

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

var (
	authModeCacheMu    sync.Mutex
	cachedAuthModeKey  authModeCacheKey
	cachedAuthModeVal  AuthMode
	cachedAuthModeWhy  string
	authModeCacheValid bool
)

// ResolveAuthMode determines the active AuthMode (googler, community, or none) and a
// human-readable explanation of how it was selected.
//
// Precedence:
//  1. --auth-mode flag (AuthModeFlag)
//  2. GH_ISH_AUTH_MODE environment variable
//  3. git config ghish.authmode (from cfg, or ctx when cfg is nil)
//  4. Auto-detection (sso_client on PATH, /usr/bin/sso-cred-helper present,
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
		authModeCacheMu.Lock()
		if authModeCacheValid && cachedAuthModeKey == cacheKey {
			m, r := cachedAuthModeVal, cachedAuthModeWhy
			authModeCacheMu.Unlock()
			return m, r, nil
		}
		authModeCacheMu.Unlock()
	}

	cacheAndReturn := func(m AuthMode, reason string) (AuthMode, string, error) {
		if cfg != nil {
			authModeCacheMu.Lock()
			cachedAuthModeKey = cacheKey
			cachedAuthModeVal = m
			cachedAuthModeWhy = reason
			authModeCacheValid = true
			authModeCacheMu.Unlock()
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

	if _, err := LookPathFn("sso_client"); err == nil {
		return cacheAndReturn(AuthModeGoogler, "auto-detected (sso_client on PATH)")
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

// AuthMethod describes the Gerrit credential source.
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

// ensureSafeHTTPClient returns c with CheckRedirect set to
// stripAuthOnCrossHostRedirect (copying c if it has no CheckRedirect). A nil c
// gives a client with no Timeout: requests are bounded by their context, so
// Ctrl-C cancels them and long log downloads are not cut off.
func ensureSafeHTTPClient(c *http.Client) *http.Client {
	if c == nil {
		return &http.Client{CheckRedirect: stripAuthOnCrossHostRedirect}
	}
	if c.CheckRedirect == nil {
		cp := *c
		cp.CheckRedirect = stripAuthOnCrossHostRedirect
		return &cp
	}
	return c
}

// bearerAuth attaches a gh-ish OAuth2 token to requests for one backend.
type bearerAuth struct {
	// service names the backend in errors ("LUCI", "Android Build").
	service string
	// trusted reports whether a URL may receive the token. It must match
	// exact hosts (see urlHostIn).
	trusted func(u *url.URL) bool
	// token resolves the backend's OAuth2 token.
	token func(ctx context.Context) (string, error)
	// requiredInGoogler makes a missing token an ExitCodeAuth error in googler
	// mode instead of sending the request unauthenticated.
	requiredInGoogler bool
	// cfg is used when the request context carries no *Config.
	cfg *Config
}

// apply sets "Authorization: Bearer <token>" on req when req.URL is trusted
// and authentication is not disabled. Without a token it leaves req
// unauthenticated, unless requiredInGoogler is set and the mode is googler.
func (a bearerAuth) apply(req *http.Request) error {
	if req == nil || req.URL == nil || a.trusted == nil || a.token == nil || !a.trusted(req.URL) {
		return nil
	}
	ctx := req.Context()
	if a.cfg != nil && ctx.Value(configKey) == nil {
		ctx = context.WithValue(ctx, configKey, a.cfg)
	}
	mode, modeReason, err := ResolveAuthMode(ctx, nil)
	if err != nil {
		return err
	}
	if mode == AuthModeNone {
		return nil
	}
	tok, tokErr := a.token(ctx)
	tok = strings.TrimSpace(tok)
	if tokErr == nil && tok != "" {
		if req.Header == nil {
			req.Header = make(http.Header)
		}
		req.Header.Set("Authorization", "Bearer "+tok)
		return nil
	}
	if tokErr == nil {
		tokErr = errors.New("empty token")
	}
	// A cancelled or expired request (Ctrl-C, --watch teardown) is not an
	// authentication failure; report the context error instead.
	if cerr := ctx.Err(); cerr != nil {
		return cerr
	}
	if a.requiredInGoogler && mode == AuthModeGoogler {
		// Resolver errors such as DefaultLUCIToken's carry their own
		// remediation; keep only their first line so it is not repeated.
		return NewExitCodeError(ExitCodeAuth,
			"%s authentication required in googler mode (%s): no OAuth2 token for %s: %v\n\n"+
				"Remediation:\n"+
				"  1. Corp Linux: run 'gcert'. Otherwise run '%s'\n"+
				"  2. Check authentication status: gh auth status\n"+
				"  3. For public projects without internal credentials: export GH_ISH_AUTH_MODE=community",
			a.service, modeReason, req.URL.Host, firstLine(tokErr.Error()), luciAuthLoginCommand(mode))
	}
	return nil
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

// newLUCIAuth returns the bearerAuth policy for LUCI Buildbucket and LogDog.
// Internal tryjobs and logs are not returned to unauthenticated requests, so a
// missing token is an error in googler mode.
func newLUCIAuth(bbHost string, tokenProvider func(context.Context) (string, error)) bearerAuth {
	if tokenProvider == nil {
		tokenProvider = func(ctx context.Context) (string, error) {
			tok, _, err := LUCITokenResolver(ctx)
			return tok, err
		}
	}
	return bearerAuth{
		service: "LUCI",
		trusted: func(u *url.URL) bool {
			return isTrustedLUCIURL(bbHost, u)
		},
		token:             tokenProvider,
		requiredInGoogler: true,
	}
}

func newLUCIHTTPAuthError(ctx context.Context, target string, statusCode int, body []byte) error {
	return NewExitCodeError(ExitCodeAuth,
		"%s returned HTTP %d: %s\n\n"+
			"To authenticate with LUCI:\n"+
			"  1. Corp Linux: run 'gcert' (sso-cred-helper lets luci-auth mint tokens). Otherwise:\n"+
			"     %s\n"+
			"  2. Or set an OAuth token: export GHISH_LUCI_TOKEN=\"<token>\"\n"+
			"  3. Check auth status: gh auth status",
		target, statusCode, strings.TrimSpace(string(body)), luciAuthLoginCommandForScope(ctx, ""))
}

// newAndroidBuildAuth returns the bearerAuth policy for Android Build API and
// CI hosts. Requests proceed without a token when none is available.
func newAndroidBuildAuth(apiBase, ciBase string, tokenProvider func(context.Context) (string, error), cfg *Config) bearerAuth {
	if tokenProvider == nil {
		tokenProvider = func(ctx context.Context) (string, error) {
			return AndroidBuildTokenResolver(ctx)
		}
	}
	return bearerAuth{
		service: "Android Build",
		trusted: func(u *url.URL) bool {
			return urlHostIn(u, hostnameOf(apiBase), hostnameOf(ciBase))
		},
		token: tokenProvider,
		cfg:   cfg,
	}
}

var gerritTokenAuthRejectionMarkers = []string{
	"insufficient_scope",
	"invalid_token",
	"authentication required",
	"access_token_scope_insufficient",
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
	var bodyPreview string
	if resp.Body != nil {
		buf, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		orig := resp.Body
		resp.Body = struct {
			io.Reader
			io.Closer
		}{
			Reader: io.MultiReader(bytes.NewReader(buf), orig),
			Closer: orig,
		}
		bodyPreview = string(buf)
	}
	combined := strings.ToLower(resp.Header.Get("WWW-Authenticate") + "\n" + bodyPreview)
	return slices.ContainsFunc(gerritTokenAuthRejectionMarkers, func(m string) bool {
		return strings.Contains(combined, m)
	})
}

// LookPathFn allows overriding exec.LookPath in tests.
var LookPathFn = exec.LookPath

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
		if hasSSOCredHelper() {
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

// resolveGerritClientAuth is the Gerrit auth policy for a client of
// gerritURL:
//   - --auth-mode=none: unauthenticated; GERRIT_TOKEN is ignored.
//   - GERRIT_TOKEN set: a static token for the configured host.
//   - Otherwise a luci-auth token, only for Google-hosted Gerrit (or
//     loopback under the allowLoopbackGerritAuthForTests unit-test hook).
//     It is minted once here so Authenticated() is known up front.
//
// Googler mode never falls back to anonymous requests: without a credential
// it returns an ExitCodeAuth error. Community mode reads anonymously.
func resolveGerritClientAuth(ctx context.Context, gerritURL string) (GerritAuth, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	mode, modeReason, err := ResolveAuthMode(ctx, nil)
	if err != nil {
		return GerritAuth{}, err
	}
	res, err := resolveGerritAuth(ctx, gerritURL, mode)
	if err != nil {
		return GerritAuth{}, err
	}
	if res.unauthed && mode == AuthModeGoogler {
		return GerritAuth{}, gerritGooglerAuthError(modeReason, gerritURL, res.cause)
	}
	auth := GerritAuth{AllowAnonymousFallback: mode != AuthModeGoogler}
	switch {
	case res.unauthed:
	case res.dynamic:
		auth.Dynamic = true
		auth.modeReason = modeReason
	default:
		auth.StaticToken = res.token
	}
	return auth, nil
}

// ssoCredHelperPath is the Corp Linux path for the SSO credential helper
// used by luci-auth v1.6+ to mint OAuth2 tokens directly from 7-day gcert
// LOAS2 credentials. Non-login shells (spawned by IDEs, coding agents, or
// scripts) often do not source /etc/profile.d/sso-cred-helper.sh, leaving
// LUCI_AUTH_CREDENTIAL_HELPER unset.
const ssoCredHelperPath = "/usr/bin/sso-cred-helper"

// hasSSOCredHelper reports whether the Corp Linux SSO credential helper is
// installed at ssoCredHelperPath.
func hasSSOCredHelper() bool {
	path, err := LookPathFn(ssoCredHelperPath)
	return err == nil && path != ""
}

// newOAuthTokenCommand constructs an exec.Cmd for an external OAuth CLI helper
// and auto-injects LUCI_AUTH_CREDENTIAL_HELPER when invoking luci-auth on Corp
// Linux if LUCI_AUTH_CREDENTIAL_HELPER is unset in the environment.
func newOAuthTokenCommand(ctx context.Context, name string, args ...string) *exec.Cmd {
	cmd := ExecCommandContext(ctx, name, args...)
	if filepath.Base(name) == "luci-auth" {
		if _, set := os.LookupEnv("LUCI_AUTH_CREDENTIAL_HELPER"); !set && hasSSOCredHelper() {
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
			if maxExpiry := now.Add(maxOAuthTokenTTL); expiry.After(maxExpiry) {
				expiry = maxExpiry
			}
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
	tok, expiry := parseOAuthTokenOutput(out, time.Now())
	if tok == "" {
		return "", time.Time{}, fmt.Errorf("luci-auth (%s) returned no OAuth2 token: %s", bin, commandErrorDetail(nil))
	}
	return tok, expiry, nil
}

// luciAuthCache holds the process-wide luci-auth token.
var luciAuthCache struct {
	mu          sync.Mutex
	token       string
	expiry      time.Time
	scopes      string
	notFoundErr error
}

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
	c := &luciAuthCache
	c.mu.Lock()
	defer c.mu.Unlock()
	if err := ctx.Err(); err != nil {
		return "", err
	}
	if c.token != "" && c.scopes == scopes && time.Now().Before(c.expiry) {
		return c.token, nil
	}
	if c.notFoundErr != nil {
		return "", c.notFoundErr
	}
	tok, expiry, err := mintLUCIAuthToken(ctx, scopes)
	if ctx.Err() == nil {
		switch {
		case err == nil:
			c.token, c.expiry, c.scopes = tok, expiry, scopes
		case errors.Is(err, errLUCIAuthNotFound):
			c.notFoundErr = err
		}
	}
	return tok, err
}

// invalidateLUCIAuthToken drops the cached token if it equals rejected, so the
// next luciAuthToken re-mints. An empty rejected value is a no-op, so a caller
// that sent no token can never wipe the shared cache.
func invalidateLUCIAuthToken(rejected string) {
	if rejected == "" {
		return
	}
	c := &luciAuthCache
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.token == rejected {
		c.token = ""
		c.expiry = time.Time{}
	}
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

// ResetAuthTokenCaches clears in-memory OAuth token caches for Gerrit, LUCI, Android Build, and Issue Tracker.
func ResetAuthTokenCaches() {
	authModeCacheMu.Lock()
	authModeCacheValid = false
	cachedAuthModeKey = authModeCacheKey{}
	authModeCacheMu.Unlock()

	luciAuthCache.mu.Lock()
	luciAuthCache.token = ""
	luciAuthCache.expiry = time.Time{}
	luciAuthCache.scopes = ""
	luciAuthCache.notFoundErr = nil
	luciAuthCache.mu.Unlock()

	resetSelfAccountMemCache()
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
			"  1. Corp Linux: run 'gcert' (sso-cred-helper lets luci-auth mint tokens). Otherwise:\n"+
			"     %s\n"+
			"  2. Or provide a token explicitly:\n"+
			"     export GHISH_LUCI_TOKEN=\"<token>\"\n"+
			"  3. Check authentication status:\n"+
			"     gh auth status",
		err, luciAuthLoginCommandForScope(ctx, ""))
}

// LUCITokenResolver is the function used to resolve LUCI OAuth2 tokens.
// It can be overridden in tests.
var LUCITokenResolver = DefaultLUCIToken
