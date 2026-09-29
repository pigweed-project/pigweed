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
	"bufio"
	"bytes"
	"context"
	"encoding/base64"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
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

// ResolveAuthMode determines the active AuthMode (googler, community, or none) and a
// human-readable explanation of how it was selected.
//
// Precedence:
//  1. --auth-mode flag (AuthModeFlag)
//  2. GH_ISH_AUTH_MODE environment variable
//  3. git config ghish.authmode
//  4. Auto-detection (gob-curl or sso_client on PATH, @google.com git user.email, or sso:// remote)
func ResolveAuthMode(ctx context.Context, cfg *Config) (AuthMode, string, error) {
	if ctx == nil {
		ctx = context.Background()
	}

	var rawMode, source string
	if v := strings.TrimSpace(AuthModeFlag); v != "" {
		rawMode = v
		source = fmt.Sprintf("--auth-mode=%s", v)
	} else if v := strings.TrimSpace(os.Getenv("GH_ISH_AUTH_MODE")); v != "" {
		rawMode = v
		source = fmt.Sprintf("GH_ISH_AUTH_MODE=%s", v)
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
			return normalized, source, nil
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

	if strings.EqualFold(strings.TrimSpace(os.Getenv("GH_ISH_AUTH_METHOD")), string(AuthMethodNone)) {
		return AuthModeNone, "GH_ISH_AUTH_METHOD=none", nil
	}

	if _, err := LookPathFn("gob-curl"); err == nil {
		return AuthModeGoogler, "auto-detected (gob-curl on PATH)", nil
	}
	if _, err := LookPathFn("sso_client"); err == nil {
		return AuthModeGoogler, "auto-detected (sso_client on PATH)", nil
	}

	gitRunner := DefaultGitRunner
	if cfg != nil && cfg.Git != nil {
		gitRunner = cfg.Git
	}
	if gitRunner != nil {
		gc := NewGitClient(gitRunner)
		if email, err := gc.ConfigGet(ctx, "user.email"); err == nil {
			email = strings.TrimSpace(email)
			if strings.HasSuffix(strings.ToLower(email), "@google.com") {
				return AuthModeGoogler, fmt.Sprintf("auto-detected (git user.email %s)", email), nil
			}
		}
		if origin, err := gc.ConfigGet(ctx, "remote.origin.url"); err == nil {
			origin = strings.TrimSpace(origin)
			if strings.HasPrefix(strings.ToLower(origin), "sso://") {
				return AuthModeGoogler, "auto-detected (sso:// git remote)", nil
			}
		}
	}

	return AuthModeCommunity, "auto-detected (community environment)", nil
}

// AuthMethod describes the authentication strategy.
type AuthMethod string

const (
	AuthMethodAuto    AuthMethod = "auto"
	AuthMethodGobCurl AuthMethod = "gob-curl"
	AuthMethodCookie  AuthMethod = "cookie"
	AuthMethodNetrc   AuthMethod = "netrc"
	AuthMethodToken   AuthMethod = "token"
	AuthMethodNone    AuthMethod = "none"
)

// fallbackTransport wraps an underlying transport and falls back to anonymous /
// when /a/ returns 400 or 401 for GET requests on googlesource.com, unless
// disallowAnonFallback is set (e.g. in googler mode or when explicit auth is configured).
type fallbackTransport struct {
	base                 http.RoundTripper
	disallowAnonFallback bool
	unauthenticated      bool
}

func (t *fallbackTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	if t.unauthenticated && !t.disallowAnonFallback && req.Method == "GET" &&
		strings.Contains(req.URL.Host, ".googlesource.com") && strings.Contains(req.URL.Path, "/a/") {
		newReq := req.Clone(req.Context())
		newReq.URL.Path = strings.Replace(req.URL.Path, "/a/", "/", 1)
		return t.base.RoundTrip(newReq)
	}

	resp, err := t.base.RoundTrip(req)
	if err != nil {
		return resp, err
	}

	if !t.disallowAnonFallback && (resp.StatusCode == 400 || resp.StatusCode == 401) &&
		strings.Contains(req.URL.Host, ".googlesource.com") && strings.Contains(req.URL.Path, "/a/") &&
		!strings.HasSuffix(req.URL.Path, "/accounts/self") {
		if req.Method == "GET" {
			fmt.Fprintf(os.Stderr, "Warning: authenticated request to %s returned %d; falling back to anonymous\n", req.URL.Path, resp.StatusCode)
			newReq := req.Clone(req.Context())
			newReq.URL.Path = strings.Replace(req.URL.Path, "/a/", "/", 1)
			return t.base.RoundTrip(newReq)
		}
	}

	return resp, nil
}

// GobCurlTransport executes /usr/bin/gob-curl to authenticate with Google's Git-on-Borg servers.
type GobCurlTransport struct {
	Path string // path to gob-curl, defaults to "gob-curl"
}

func (t *GobCurlTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	path := t.Path
	if path == "" {
		path = "gob-curl"
	}

	args := []string{"-i", "-s"}
	if req.Method != "" && req.Method != http.MethodGet {
		args = append(args, "-X", req.Method)
	}

	for key, values := range req.Header {
		for _, val := range values {
			args = append(args, "-H", fmt.Sprintf("%s: %s", key, val))
		}
	}

	var stdin io.Reader
	if req.Body != nil {
		args = append(args, "--data-binary", "@-")
		stdin = req.Body
	}

	args = append(args, req.URL.String())

	cmd := exec.CommandContext(req.Context(), path, args...)
	if stdin != nil {
		cmd.Stdin = stdin
	}

	var stderr bytes.Buffer
	cmd.Stderr = &stderr

	stdoutPipe, err := cmd.StdoutPipe()
	if err != nil {
		if req.Body != nil {
			_ = req.Body.Close()
		}
		return nil, fmt.Errorf("gob-curl stdout pipe: %w", err)
	}

	if err := cmd.Start(); err != nil {
		if req.Body != nil {
			_ = req.Body.Close()
		}
		return nil, fmt.Errorf("starting gob-curl: %w", err)
	}

	// curl with -i strips chunk encoding from the body before outputting to stdout,
	// but leaves the original "Transfer-Encoding: chunked" response header in place.
	// We buffer the header section and remove "Transfer-Encoding: chunked" so Go's
	// http.ReadResponse does not attempt to de-chunk an already de-chunked stream.
	pipeReader := bufio.NewReader(stdoutPipe)
	var headerBuf bytes.Buffer

	for {
		line, err := pipeReader.ReadString('\n')
		if err != nil {
			_ = cmd.Wait()
			if req.Body != nil {
				_ = req.Body.Close()
			}
			if ctxErr := req.Context().Err(); ctxErr != nil {
				return nil, ctxErr
			}
			stderrStr := strings.TrimSpace(stderr.String())
			return nil, NewExitCodeError(ExitCodeAuth,
				"reading gob-curl response headers: %w; stderr: %s\n\n"+
					"Cause: gob-curl failed to authenticate with %s (corp SSO / LOAS credentials may be expired).\n\n"+
					"Remediation:\n"+
					"  1. Refresh corp SSO credentials:\n"+
					"     gcert\n"+
					"  2. Verify authentication status:\n"+
					"     gh auth status",
				err, stderrStr, req.URL.Host)
		}
		trimmed := strings.TrimRight(line, "\r\n")
		if trimmed == "" {
			headerBuf.WriteString("\r\n")
			break
		}
		if strings.HasPrefix(strings.ToLower(trimmed), "transfer-encoding:") {
			continue
		}
		headerBuf.WriteString(trimmed)
		headerBuf.WriteString("\r\n")
	}

	combined := io.MultiReader(&headerBuf, pipeReader)
	resp, err := http.ReadResponse(bufio.NewReader(combined), req)
	if err != nil {
		_ = cmd.Wait()
		if req.Body != nil {
			_ = req.Body.Close()
		}
		return nil, fmt.Errorf("parsing gob-curl HTTP response: %w; stderr: %s", err, stderr.String())
	}

	resp.Body = &cmdResponseBody{
		ReadCloser: resp.Body,
		cmd:        cmd,
	}

	return resp, nil
}

type cmdResponseBody struct {
	io.ReadCloser
	cmd *exec.Cmd
}

func (b *cmdResponseBody) Close() error {
	err := b.ReadCloser.Close()
	waitErr := b.cmd.Wait()
	if err != nil {
		return err
	}
	return waitErr
}

// ParseNetscapeCookies parses cookies from an io.Reader in Netscape/curl format.
func ParseNetscapeCookies(r io.Reader, targetURL *url.URL, now time.Time) []*http.Cookie {
	var cookies []*http.Cookie
	scanner := bufio.NewScanner(r)
	targetHost := strings.ToLower(targetURL.Hostname())
	nowUnix := now.Unix()

	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			if strings.HasPrefix(line, "#HttpOnly_") {
				line = strings.TrimPrefix(line, "#HttpOnly_")
			} else {
				continue
			}
		}

		parts := strings.Split(line, "\t")
		if len(parts) < 7 {
			continue
		}

		domain := strings.ToLower(strings.TrimSpace(parts[0]))
		includeSubpath := strings.EqualFold(parts[1], "TRUE")
		path := parts[2]
		secure := strings.EqualFold(parts[3], "TRUE")
		expires, _ := strconv.ParseInt(parts[4], 10, 64)
		name := parts[5]
		value := parts[6]

		// Check expiration (0 means session cookie / doesn't expire)
		if expires > 0 && expires < nowUnix {
			continue
		}

		// Check domain match
		domainMatched := false
		trimmedDomain := strings.TrimPrefix(domain, ".")
		if includeSubpath {
			if targetHost == trimmedDomain || strings.HasSuffix(targetHost, "."+trimmedDomain) {
				domainMatched = true
			}
		} else {
			if targetHost == trimmedDomain {
				domainMatched = true
			}
		}

		if !domainMatched {
			continue
		}

		// Check path match
		if path != "" && path != "/" && !strings.HasPrefix(targetURL.Path, path) {
			continue
		}

		// Check secure match
		if secure && targetURL.Scheme != "https" {
			continue
		}

		cookies = append(cookies, &http.Cookie{
			Name:  name,
			Value: value,
		})
	}

	return cookies
}

// CookieTransport injects cookies into requests matching target host.
type CookieTransport struct {
	Base       http.RoundTripper
	CookieFile string
}

func (t *CookieTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	cookiePath := t.CookieFile
	if cookiePath == "" {
		cookiePath = findGitCookieFile(req.Context())
	}

	if cookiePath != "" {
		if f, err := os.Open(cookiePath); err != nil {
			fmt.Fprintf(os.Stderr, "Warning: failed to open git cookiefile %s: %v\n", cookiePath, err)
		} else {
			defer f.Close()
			matched := ParseNetscapeCookies(f, req.URL, time.Now())
			for _, c := range matched {
				req.AddCookie(c)
			}
		}
	}

	base := t.Base
	if base == nil {
		base = http.DefaultTransport
	}
	return base.RoundTrip(req)
}

func findGitCookieFile(ctx context.Context, runner ...GitRunner) string {
	if ctx == nil {
		ctx = context.Background()
	}
	var r GitRunner = DefaultGitRunner
	if len(runner) > 0 && runner[0] != nil {
		r = runner[0]
	}
	if p, err := NewGitClient(r).ConfigGet(ctx, "http.cookiefile"); err == nil && p != "" {
		if strings.HasPrefix(p, "~/") {
			if home, err := os.UserHomeDir(); err == nil {
				p = filepath.Join(home, p[2:])
			}
		}
		return p
	}

	if home, err := os.UserHomeDir(); err == nil {
		defaultPath := filepath.Join(home, ".gitcookies")
		if _, err := os.Stat(defaultPath); err == nil {
			return defaultPath
		}
	}

	return ""
}

// NetrcCredentials holds login/password parsed from .netrc.
type NetrcCredentials struct {
	Login    string
	Password string
}

// ParseNetrc reads .netrc format and finds credentials for a given host.
func ParseNetrc(r io.Reader, targetHost string) *NetrcCredentials {
	scanner := bufio.NewScanner(r)
	targetHost = strings.ToLower(targetHost)

	var inTargetMachine bool
	var creds NetrcCredentials

	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}

		tokens := strings.Fields(line)
		for i := 0; i < len(tokens); i++ {
			tok := tokens[i]
			switch tok {
			case "machine":
				if i+1 < len(tokens) {
					machine := strings.ToLower(tokens[i+1])
					inTargetMachine = (machine == targetHost)
					i++
				}
			case "login":
				if inTargetMachine && i+1 < len(tokens) {
					creds.Login = tokens[i+1]
					i++
				}
			case "password", "account":
				if inTargetMachine && i+1 < len(tokens) {
					creds.Password = tokens[i+1]
					i++
				}
			}
		}

		if inTargetMachine && creds.Login != "" && creds.Password != "" {
			return &creds
		}
	}

	if inTargetMachine && (creds.Login != "" || creds.Password != "") {
		return &creds
	}
	return nil
}

// FindNetrcCredentials looks for machine credentials in ~/.netrc.
func FindNetrcCredentials(host string) *NetrcCredentials {
	home, err := os.UserHomeDir()
	if err != nil {
		return nil
	}
	for _, name := range []string{".netrc", "_netrc"} {
		path := filepath.Join(home, name)
		if f, err := os.Open(path); err == nil {
			defer f.Close()
			if creds := ParseNetrc(f, host); creds != nil {
				return creds
			}
		}
	}
	return nil
}

// BasicAuthTransport attaches HTTP Basic Auth credentials.
type BasicAuthTransport struct {
	Base     http.RoundTripper
	Username string
	Password string
}

func (t *BasicAuthTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	if t.Username != "" || t.Password != "" {
		req = req.Clone(req.Context())
		auth := base64.StdEncoding.EncodeToString([]byte(t.Username + ":" + t.Password))
		req.Header.Set("Authorization", "Basic "+auth)
	}
	base := t.Base
	if base == nil {
		base = http.DefaultTransport
	}
	return base.RoundTrip(req)
}

// TokenTransport attaches an Authorization Bearer token.
type TokenTransport struct {
	Base  http.RoundTripper
	Token string
}

func (t *TokenTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	if t.Token != "" {
		req = req.Clone(req.Context())
		if strings.Contains(t.Token, ":") {
			auth := base64.StdEncoding.EncodeToString([]byte(t.Token))
			req.Header.Set("Authorization", "Basic "+auth)
		} else {
			req.Header.Set("Authorization", "Bearer "+t.Token)
		}
	}
	base := t.Base
	if base == nil {
		base = http.DefaultTransport
	}
	return base.RoundTrip(req)
}

// LookPathFn allows overriding exec.LookPath in tests.
var LookPathFn = exec.LookPath

// DescribeGerritAuthMethod returns the active Gerrit AuthMethod and a human-readable
// description of the credential source without executing a network request.
func DescribeGerritAuthMethod(ctx context.Context, gerritHost string, mode AuthMode) (AuthMethod, string) {
	if ctx == nil {
		ctx = context.Background()
	}
	method := AuthMethod(strings.TrimSpace(os.Getenv("GH_ISH_AUTH_METHOD")))
	if method == "" {
		if mode == AuthModeNone {
			return AuthMethodNone, "disabled (--auth-mode=none)"
		}
		method = AuthMethodAuto
	}

	switch method {
	case AuthMethodToken:
		return AuthMethodToken, "token (GH_ISH_AUTH_METHOD=token)"
	case AuthMethodGobCurl:
		return AuthMethodGobCurl, "gob-curl (GH_ISH_AUTH_METHOD=gob-curl)"
	case AuthMethodCookie:
		if cf := findGitCookieFile(ctx); cf != "" {
			return AuthMethodCookie, fmt.Sprintf("cookie (%s)", cf)
		}
		return AuthMethodCookie, "cookie (GH_ISH_AUTH_METHOD=cookie)"
	case AuthMethodNetrc:
		return AuthMethodNetrc, "netrc (~/.netrc)"
	case AuthMethodNone:
		return AuthMethodNone, "none (unauthenticated)"
	case AuthMethodAuto:
		if os.Getenv("GERRIT_TOKEN") != "" {
			return AuthMethodToken, "token (GERRIT_TOKEN)"
		}
		if _, err := LookPathFn("gob-curl"); err == nil {
			return AuthMethodGobCurl, "gob-curl"
		}
		if cookieFile := findGitCookieFile(ctx); cookieFile != "" {
			return AuthMethodCookie, fmt.Sprintf("cookie (%s)", cookieFile)
		}
		u, _ := url.Parse(gerritHost)
		hostname := gerritHost
		if u != nil && u.Hostname() != "" {
			hostname = u.Hostname()
		}
		if creds := FindNetrcCredentials(hostname); creds != nil {
			return AuthMethodNetrc, "netrc (~/.netrc)"
		}
		return AuthMethodNone, "none"
	default:
		return method, string(method)
	}
}

// NewAuthTransportContext returns an authenticated http.RoundTripper appropriate for the environment,
// passing ctx through for git config lookups and timeouts.
func NewAuthTransportContext(ctx context.Context, gerritHost string) (http.RoundTripper, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	var cfg *Config
	if c, ok := ctx.Value(configKey).(*Config); ok {
		cfg = c
	}
	mode, modeReason, err := ResolveAuthMode(ctx, cfg)
	if err != nil {
		return nil, err
	}

	method := AuthMethod(os.Getenv("GH_ISH_AUTH_METHOD"))
	if method == "" {
		if mode == AuthModeNone {
			method = AuthMethodNone
		} else {
			method = AuthMethodAuto
		}
	}

	var base http.RoundTripper

	switch method {
	case AuthMethodToken:
		token := os.Getenv("GERRIT_TOKEN")
		if token == "" {
			return nil, NewExitCodeError(ExitCodeAuth, "GH_ISH_AUTH_METHOD=token specified but GERRIT_TOKEN is empty.\n\nTo configure a token, run:\n  export GERRIT_TOKEN=\"<token>\"")
		}
		base = &TokenTransport{Token: token}

	case AuthMethodGobCurl:
		if _, err := LookPathFn("gob-curl"); err != nil {
			return nil, NewExitCodeError(ExitCodeAuth, "GH_ISH_AUTH_METHOD=gob-curl specified but gob-curl executable not found in PATH: %w.\n\nEnsure corp development tools are installed and in PATH, or choose a different auth method (e.g. export GH_ISH_AUTH_METHOD=cookie or =token).", err)
		}
		base = &GobCurlTransport{}

	case AuthMethodCookie:
		cookieFile := findGitCookieFile(ctx)
		if cookieFile == "" {
			return nil, NewExitCodeError(ExitCodeAuth, "GH_ISH_AUTH_METHOD=cookie specified but no cookiefile found (checked git config http.cookiefile and ~/.gitcookies).\n\nTo configure Git cookies:\n  Visit https://<host>/new-password to obtain credentials and configure ~/.gitcookies.")
		}
		base = &CookieTransport{CookieFile: cookieFile}

	case AuthMethodNetrc:
		u, _ := url.Parse(gerritHost)
		hostname := gerritHost
		if u != nil && u.Hostname() != "" {
			hostname = u.Hostname()
		}
		creds := FindNetrcCredentials(hostname)
		if creds == nil {
			return nil, NewExitCodeError(ExitCodeAuth, "GH_ISH_AUTH_METHOD=netrc specified but no credentials found for %s in ~/.netrc", hostname)
		}
		base = &BasicAuthTransport{Username: creds.Login, Password: creds.Password}

	case AuthMethodNone:
		base = http.DefaultTransport

	case AuthMethodAuto:
		// Priority 1: GERRIT_TOKEN environment variable
		if token := os.Getenv("GERRIT_TOKEN"); token != "" {
			base = &TokenTransport{Token: token}
			break
		}

		// Priority 2: gob-curl on PATH (seamless Google internal auth)
		if _, err := LookPathFn("gob-curl"); err == nil {
			base = &GobCurlTransport{}
			break
		}

		// Priority 3: Git cookies (.gitcookies or git config http.cookiefile)
		if cookieFile := findGitCookieFile(ctx); cookieFile != "" {
			base = &CookieTransport{CookieFile: cookieFile}
			break
		}

		// Priority 4: .netrc credentials
		u, _ := url.Parse(gerritHost)
		hostname := gerritHost
		if u != nil && u.Hostname() != "" {
			hostname = u.Hostname()
		}
		if creds := FindNetrcCredentials(hostname); creds != nil {
			base = &BasicAuthTransport{Username: creds.Login, Password: creds.Password}
			break
		}

		// Priority 5: In googler mode, do not silently fall back to anonymous Gerrit.
		if mode == AuthModeGoogler {
			cleanHost := CleanGerritHost(gerritHost)
			if cleanHost == "" {
				cleanHost = "<host>"
			}
			return nil, NewExitCodeError(ExitCodeAuth,
				"Gerrit authentication required in googler mode (%s): no Gerrit credentials found for %s.\n\n"+
					"Cause: In googler mode, gh-ish does not fall back to unauthenticated Gerrit requests.\n\n"+
					"Remediation:\n"+
					"  1. (Corp users) Ensure 'gob-curl' is in PATH and run 'gcert'.\n"+
					"  2. Or configure Git cookies at: https://%s/new-password\n"+
					"  3. Or set a token: export GERRIT_TOKEN=\"<token>\"\n"+
					"  4. Or switch to community mode: export GH_ISH_AUTH_MODE=community\n"+
					"  5. Inspect auth status: gh auth status",
				modeReason, cleanHost, cleanHost)
		}

		// In community mode, fall back to anonymous transport.
		base = http.DefaultTransport

	default:
		return nil, NewExitCodeError(ExitCodeAuth, "unrecognized auth method: %q", method)
	}

	disallowAnonFallback := mode == AuthModeGoogler || (method != AuthMethodAuto && method != AuthMethodNone)
	return &fallbackTransport{
		base:                 base,
		disallowAnonFallback: disallowAnonFallback,
		unauthenticated:      base == http.DefaultTransport,
	}, nil
}

// NewAuthTransport returns an authenticated http.RoundTripper appropriate for the environment.
func NewAuthTransport(gerritHost string) (http.RoundTripper, error) {
	return NewAuthTransportContext(context.Background(), gerritHost)
}

// DefaultBuganizerScopes are the OAuth2 scopes required for Google Issue Tracker and GCP quota checks.
const DefaultBuganizerScopes = "https://www.googleapis.com/auth/buganizer https://www.googleapis.com/auth/cloud-platform"

// OAuthTokenCommandRunner executes external CLI token helpers (luci-auth, gcloud).
// It can be overridden in unit tests to prevent spawning real subprocesses.
var OAuthTokenCommandRunner = func(ctx context.Context, name string, args ...string) ([]byte, error) {
	return exec.CommandContext(ctx, name, args...).Output()
}

// findLuciAuthBinary locates the 'luci-auth' executable.
//
// Why this fallback exists (User Workflow Note):
// In a Bazelisk-only workflow or inside secondary Git worktrees (e.g., created
// via 'gh wt use' or 'git worktree add'), developers often do not source
// 'activate.sh', so CIPD tools are not on system PATH, and 'environment/' may
// only exist in the main Git checkout (resolved via '--git-common-dir') rather
// than the secondary worktree directory.
//
// TODO: https://pwbug.dev/567289969 - Replace Pigweed CIPD directory probing
// once tool discovery is driven by project profile configuration or
// Bazel-managed toolchains.
func findLuciAuthBinary(ctx context.Context) string {
	if path, err := LookPathFn("luci-auth"); err == nil && path != "" {
		return path
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
	if cwd, err := os.Getwd(); err == nil {
		candidates = append(candidates,
			filepath.Join(cwd, "environment", "cipd", "packages", "luci", "luci-auth"),
			filepath.Join(cwd, ".environment", "cipd", "packages", "luci", "luci-auth"),
		)
	}
	var runner GitRunner = DefaultGitRunner
	if ctx != nil {
		if cfg, ok := ctx.Value(configKey).(*Config); ok && cfg != nil && cfg.Git != nil {
			runner = cfg.Git
		}
	}
	if runner != nil {
		var stdout, stderr bytes.Buffer
		if err := runner.Run(ctx, &stdout, &stderr, "rev-parse", "--git-common-dir"); err == nil {
			commonGitDir := strings.TrimSpace(stdout.String())
			if commonGitDir != "" {
				if abs, err := filepath.Abs(commonGitDir); err == nil {
					repoRoot := filepath.Dir(abs)
					candidates = append(candidates,
						filepath.Join(repoRoot, "environment", "cipd", "packages", "luci", "luci-auth"),
						filepath.Join(repoRoot, ".environment", "cipd", "packages", "luci", "luci-auth"),
					)
				}
			}
		}
	}
	for _, cand := range candidates {
		if path, err := LookPathFn(cand); err == nil && path != "" {
			return path
		}
	}
	return ""
}

// resolveOAuthTokenFromCLI attempts to mint an OAuth2 access token from luci-auth
// (trying each scope string in luciScopes; "" means default luci-auth scopes) and
// then falls back to gcloud. It returns the token and a human-readable source label.
func resolveOAuthTokenFromCLI(ctx context.Context, luciScopes []string) (string, string, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	if luciAuthBin := findLuciAuthBinary(ctx); luciAuthBin != "" {
		if len(luciScopes) == 0 {
			luciScopes = []string{""}
		}
		for _, sc := range luciScopes {
			var args []string
			if sc == "" {
				args = []string{"token"}
			} else {
				args = []string{"token", "-scopes", sc}
			}
			if out, err := OAuthTokenCommandRunner(ctx, luciAuthBin, args...); err == nil {
				if tok := strings.TrimSpace(string(out)); tok != "" {
					return tok, "luci-auth", nil
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
					return tok, "gcloud", nil
				}
			}
		}
	}

	return "", "", fmt.Errorf("no active OAuth2 token found from luci-auth or gcloud")
}

var (
	luciTokenMu           sync.Mutex
	cachedLUCIToken       string
	cachedLUCITokenSource string
	cachedLUCITokenExpiry time.Time
)

// ResetAuthTokenCaches clears in-memory OAuth token caches for LUCI and Issue Tracker.
func ResetAuthTokenCaches() {
	luciTokenMu.Lock()
	cachedLUCIToken = ""
	cachedLUCITokenSource = ""
	cachedLUCITokenExpiry = time.Time{}
	luciTokenMu.Unlock()

	issueTokenMu.Lock()
	cachedIssueToken = ""
	cachedIssueTokenSource = ""
	cachedIssueTokenExpiry = time.Time{}
	issueTokenMu.Unlock()
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
	if cachedLUCIToken != "" && time.Now().Before(cachedLUCITokenExpiry) {
		tok, src := cachedLUCIToken, cachedLUCITokenSource
		luciTokenMu.Unlock()
		return tok, src, nil
	}
	luciTokenMu.Unlock()

	tok, src, err := resolveOAuthTokenFromCLI(ctx, []string{"", DefaultBuganizerScopes})
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

	luciTokenMu.Lock()
	cachedLUCIToken = tok
	cachedLUCITokenSource = src
	cachedLUCITokenExpiry = time.Now().Add(5 * time.Minute)
	luciTokenMu.Unlock()
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

	mode, modeReason, err := ResolveAuthMode(ctx, cfg)
	if err != nil {
		return nil, err
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
