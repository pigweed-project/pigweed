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
//  3. git config ghish.authmode
//  4. Auto-detection (sso_client on PATH, /usr/bin/sso-cred-helper present,
//     @google.com git user.email, or sso:// or rpc:// remote)
func ResolveAuthMode(ctx context.Context, cfg *Config) (AuthMode, string, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	if err := checkRemovedAuthMethodEnv(); err != nil {
		return "", "", err
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

// gerritTokenFromEnv reads and validates GERRIT_TOKEN, returning an empty
// string when unset and an ExitCodeAuth error if it contains a colon
// (username:password HTTP Basic format).
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

// gerritLoginCommand returns the `luci-auth login` invocation for Gerrit in mode.
func gerritLoginCommand(_ AuthMode) string {
	return fmt.Sprintf("luci-auth login -scopes %q", DefaultGerritScopes)
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
		login := fmt.Sprintf("Run '%s'", gerritLoginCommand(mode))
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

// resolveGerritClientAuth translates ResolveAuthMode + resolveGerritAuth into
// the GerritAuth configuration consumed by NewGerritClient.
func resolveGerritClientAuth(ctx context.Context, gerritURL string) (GerritAuth, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	cfg, _ := ctx.Value(configKey).(*Config)
	mode, modeReason, err := ResolveAuthMode(ctx, cfg)
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

// DefaultBuganizerScopes are the OAuth2 scopes required for Google Issue Tracker and GCP quota checks.
const DefaultBuganizerScopes = "https://www.googleapis.com/auth/buganizer https://www.googleapis.com/auth/cloud-platform"

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

// OAuthTokenCommandRunner executes external CLI token helpers (luci-auth, gcloud).
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

func resolveOAuthTokenWithExpiry(ctx context.Context, luciScopes []string) (token string, source string, expiry time.Time, err error) {
	if ctx == nil {
		ctx = context.Background()
	}
	if len(luciScopes) == 0 {
		luciScopes = []string{""}
	}

	if luciAuthBin := findLuciAuthBinary(ctx); luciAuthBin != "" {
		for _, sc := range luciScopes {
			var args []string
			if sc == "" {
				args = []string{"token", "-json-output", "-"}
			} else {
				args = []string{"token", "-scopes", sc, "-json-output", "-"}
			}
			if out, err := OAuthTokenCommandRunner(ctx, luciAuthBin, args...); err == nil {
				tok, exp := parseOAuthTokenOutput(out, time.Now())
				if tok != "" {
					return tok, "luci-auth", exp, nil
				}
			}
		}
	}

	if _, err := LookPathFn("gcloud"); err == nil {
		for _, args := range [][]string{
			{"auth", "application-default", "print-access-token"},
			{"auth", "print-access-token"},
		} {
			if out, err := OAuthTokenCommandRunner(ctx, "gcloud", args...); err == nil {
				if tok := strings.TrimSpace(string(out)); tok != "" {
					exp := time.Now().Add(defaultOAuthTokenTTL)
					return tok, "gcloud", exp, nil
				}
			}
		}
	}

	return "", "", time.Time{}, fmt.Errorf("no active OAuth2 token found from luci-auth or gcloud")
}

var (
	luciTokenMu           sync.Mutex
	cachedLUCIToken       string
	cachedLUCITokenSource string
	cachedLUCITokenExpiry time.Time

	androidBuildTokenMu           sync.Mutex
	cachedAndroidBuildToken       string
	cachedAndroidBuildTokenSource string
	cachedAndroidBuildTokenExpiry time.Time

	gerritTokenMu           sync.Mutex
	cachedGerritToken       string
	cachedGerritTokenSource string
	cachedGerritTokenExpiry time.Time
	gerritTokenProbeErr     error
)

// ScopeAndroidBuildInternal is the OAuth2 scope for Android Build Internal v3 REST APIs.
const ScopeAndroidBuildInternal = "https://www.googleapis.com/auth/androidbuild.internal"

// ScopeGerritCodeReview is the OAuth2 scope for Gerrit Code Review REST APIs.
const ScopeGerritCodeReview = "https://www.googleapis.com/auth/gerritcodereview"

// ScopeUserInfoEmail is the OAuth2 scope for reading the user's email address.
const ScopeUserInfoEmail = "https://www.googleapis.com/auth/userinfo.email"

// DefaultGerritScopes are the OAuth2 scopes requested when authenticating with Gerrit via luci-auth.
const DefaultGerritScopes = ScopeGerritCodeReview + " " + ScopeUserInfoEmail

// DefaultAndroidBuildScopes are the OAuth2 scopes requested when authenticating with Android Build / Busytown.
var DefaultAndroidBuildScopes = []string{
	ScopeAndroidBuildInternal,
	ScopeGerritCodeReview,
}

// ResetAuthTokenCaches clears in-memory OAuth token caches for Gerrit, LUCI, Android Build, and Issue Tracker.
func ResetAuthTokenCaches() {
	authModeCacheMu.Lock()
	authModeCacheValid = false
	cachedAuthModeKey = authModeCacheKey{}
	authModeCacheMu.Unlock()

	luciTokenMu.Lock()
	cachedLUCIToken = ""
	cachedLUCITokenSource = ""
	cachedLUCITokenExpiry = time.Time{}
	luciTokenMu.Unlock()

	androidBuildTokenMu.Lock()
	cachedAndroidBuildToken = ""
	cachedAndroidBuildTokenSource = ""
	cachedAndroidBuildTokenExpiry = time.Time{}
	androidBuildTokenMu.Unlock()

	gerritTokenMu.Lock()
	cachedGerritToken = ""
	cachedGerritTokenSource = ""
	cachedGerritTokenExpiry = time.Time{}
	gerritTokenProbeErr = nil
	gerritTokenMu.Unlock()

	issueTokenMu.Lock()
	cachedIssueToken = ""
	cachedIssueTokenSource = ""
	cachedIssueTokenExpiry = time.Time{}
	issueTokenMu.Unlock()

	quotaProjectCache.reset()
	resetSelfAccountMemCache()
}

func invalidateCachedGerritToken(rejected string) {
	gerritTokenMu.Lock()
	if cachedGerritToken == rejected {
		cachedGerritToken = ""
		cachedGerritTokenExpiry = time.Time{}
	}
	gerritTokenProbeErr = nil
	gerritTokenMu.Unlock()
}

// DefaultGerritToken resolves a Gerrit OAuth2 access token from GERRIT_TOKEN or luci-auth
// (requesting gerritcodereview scope). Unlike DefaultLUCIToken, it does not fall back to
// unscoped 'gcloud auth print-access-token', because standard gcloud tokens lack the
// gerritcodereview scope required by Gerrit REST APIs and git-over-HTTPS.
func DefaultGerritToken(ctx context.Context) (string, string, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	tok, err := gerritTokenFromEnv()
	if err != nil {
		return "", "", err
	}
	if tok != "" {
		return tok, "GERRIT_TOKEN", nil
	}

	gerritTokenMu.Lock()
	defer gerritTokenMu.Unlock()
	now := time.Now()
	if cachedGerritToken != "" && now.Before(cachedGerritTokenExpiry) {
		return cachedGerritToken, cachedGerritTokenSource, nil
	}
	if gerritTokenProbeErr != nil {
		return "", "", gerritTokenProbeErr
	}

	luciAuthBin, searched := findLuciAuthBinaryWithSearched(ctx)
	if luciAuthBin == "" {
		gerritTokenProbeErr = fmt.Errorf("luci-auth not found (searched %s)", strings.Join(searched, ", "))
		return "", "", gerritTokenProbeErr
	}

	var lastErr error
	for _, sc := range []string{DefaultGerritScopes, ScopeGerritCodeReview} {
		out, err := OAuthTokenCommandRunner(ctx, luciAuthBin, "token", "-scopes", sc, "-json-output", "-")
		if err != nil {
			lastErr = err
			continue
		}
		tok, expiry := parseOAuthTokenOutput(out, time.Now())
		if tok != "" {
			cachedGerritToken = tok
			cachedGerritTokenSource = "luci-auth"
			cachedGerritTokenExpiry = expiry
			gerritTokenProbeErr = nil
			return tok, "luci-auth", nil
		}
	}

	gerritTokenProbeErr = fmt.Errorf("luci-auth (%s) returned no OAuth2 token: %s", luciAuthBin, commandErrorDetail(lastErr))
	return "", "", gerritTokenProbeErr
}

// GerritTokenResolver resolves OAuth2 tokens with gerritcodereview scope for Gerrit REST and Git push fallbacks.
// It can be overridden in unit tests.
var GerritTokenResolver = DefaultGerritToken

// DefaultAndroidBuildToken resolves an OAuth2 access token for Android Build Internal v3 and ci.android.com.
func DefaultAndroidBuildToken(ctx context.Context) (string, error) {
	if tok := strings.TrimSpace(os.Getenv("GHISH_ANDROID_BUILD_TOKEN")); tok != "" {
		return tok, nil
	}

	androidBuildTokenMu.Lock()
	if cachedAndroidBuildToken != "" && time.Now().Before(cachedAndroidBuildTokenExpiry) {
		tok := cachedAndroidBuildToken
		androidBuildTokenMu.Unlock()
		return tok, nil
	}
	androidBuildTokenMu.Unlock()

	scopeJoined := strings.Join(DefaultAndroidBuildScopes, " ")
	tok, src, exp, err := resolveOAuthTokenWithExpiry(ctx, []string{
		scopeJoined,
		ScopeAndroidBuildInternal,
		"",
	})
	if err != nil {
		return "", err
	}

	androidBuildTokenMu.Lock()
	cachedAndroidBuildToken = tok
	cachedAndroidBuildTokenSource = src
	cachedAndroidBuildTokenExpiry = exp
	androidBuildTokenMu.Unlock()
	return tok, nil
}

// AndroidBuildTokenResolver is the function used to resolve Android Build OAuth2 tokens.
// It can be overridden in tests.
var AndroidBuildTokenResolver = DefaultAndroidBuildToken

// AndroidBuildAuthTransport attaches OAuth2 Bearer tokens to Android Build Internal and ci.android.com requests.
type AndroidBuildAuthTransport struct {
	Base http.RoundTripper
}

func (t *AndroidBuildAuthTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	ctx := req.Context()
	base := t.Base
	if base == nil {
		base = http.DefaultTransport
	}

	if tok, err := AndroidBuildTokenResolver(ctx); err == nil && tok != "" {
		authedReq := req.Clone(ctx)
		authedReq.Header.Set("Authorization", "Bearer "+tok)
		return base.RoundTrip(authedReq)
	}
	return base.RoundTrip(req)
}

// DefaultLUCIToken resolves an OAuth2 access token for LUCI Buildbucket and LogDog.
func DefaultLUCIToken(ctx context.Context) (string, string, error) {
	if tok := strings.TrimSpace(os.Getenv("GHISH_LUCI_TOKEN")); tok != "" {
		return tok, "GHISH_LUCI_TOKEN", nil
	}
	if tok := strings.TrimSpace(os.Getenv("LUCI_TOKEN")); tok != "" {
		return tok, "LUCI_TOKEN", nil
	}

	luciTokenMu.Lock()
	defer luciTokenMu.Unlock()
	if cachedLUCIToken != "" && time.Now().Before(cachedLUCITokenExpiry) {
		return cachedLUCIToken, cachedLUCITokenSource, nil
	}

	tok, src, exp, err := resolveOAuthTokenWithExpiry(ctx, []string{"", DefaultBuganizerScopes})
	if err != nil {
		return "", "", NewExitCodeError(ExitCodeAuth,
			"failed to obtain LUCI OAuth2 token: %w.\n\n"+
				"Remediation:\n"+
				"  1. Log in with LUCI Auth (recommended):\n"+
				"     luci-auth login\n"+
				"  2. Or log in with Google Cloud SDK:\n"+
				"     gcloud auth application-default login\n"+
				"  3. Or provide a token explicitly:\n"+
				"     export GHISH_LUCI_TOKEN=\"<token>\"\n"+
				"  4. Check authentication status:\n"+
				"     gh auth status",
			err)
	}

	cachedLUCIToken = tok
	cachedLUCITokenSource = src
	cachedLUCITokenExpiry = exp
	return tok, src, nil
}

// LUCITokenResolver is the function used to resolve LUCI OAuth2 tokens.
// It can be overridden in tests.
var LUCITokenResolver = DefaultLUCIToken

// LUCIAuthTransport attaches OAuth2 Bearer tokens to LUCI Buildbucket and LogDog requests.
// In googler mode, it returns ExitCodeAuth if no OAuth token is available.
// In community mode, it attaches a token when available and falls back to unauthenticated requests otherwise.
type LUCIAuthTransport struct {
	Base http.RoundTripper
	Cfg  *Config

	mu               sync.Mutex
	cachedMode       AuthMode
	cachedModeReason string
	cachedModeKey    authModeCacheKey
	modeResolved     bool
}

func (t *LUCIAuthTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	ctx := req.Context()
	cfg := t.Cfg
	if cfg == nil {
		if c, ok := ctx.Value(configKey).(*Config); ok {
			cfg = c
		}
	}

	base := t.Base
	if base == nil {
		base = http.DefaultTransport
	}

	key := authModeCacheKey{
		flag:    strings.TrimSpace(AuthModeFlag),
		envMode: strings.TrimSpace(os.Getenv("GH_ISH_AUTH_MODE")),
		cfg:     cfg,
	}
	t.mu.Lock()
	mode, modeReason, ok := t.cachedMode, t.cachedModeReason, t.modeResolved && t.cachedModeKey == key
	t.mu.Unlock()
	if !ok {
		var err error
		mode, modeReason, err = ResolveAuthMode(ctx, cfg)
		if err != nil {
			return nil, err
		}
		t.mu.Lock()
		t.cachedMode = mode
		t.cachedModeReason = modeReason
		t.cachedModeKey = key
		t.modeResolved = true
		t.mu.Unlock()
	}
	if mode == AuthModeNone {
		return base.RoundTrip(req)
	}

	tok, _, tokErr := LUCITokenResolver(ctx)
	if tokErr == nil && tok != "" {
		authedReq := req.Clone(ctx)
		authedReq.Header.Set("Authorization", "Bearer "+tok)
		return base.RoundTrip(authedReq)
	}

	if mode == AuthModeGoogler {
		return nil, NewExitCodeError(ExitCodeAuth,
			"LUCI authentication required in googler mode (%s): no active OAuth2 token found for %s.\n\n"+
				"Cause: Internal Google tryjobs and logs on LUCI Buildbucket/LogDog are not returned to unauthenticated requests.\n\n"+
				"Remediation:\n"+
				"  1. Authenticate with LUCI Auth (recommended):\n"+
				"     luci-auth login\n"+
				"  2. Or authenticate with Google Cloud SDK:\n"+
				"     gcloud auth application-default login\n"+
				"  3. Check authentication status across all services:\n"+
				"     gh auth status\n"+
				"  4. If working on a public project without internal credentials, set community mode:\n"+
				"     export GH_ISH_AUTH_MODE=community",
			modeReason, req.URL.Host)
	}

	// In community mode, proceed unauthenticated so contributors can query public builders.
	return base.RoundTrip(req)
}
