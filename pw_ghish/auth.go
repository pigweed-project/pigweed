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
	"cmp"
	"context"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
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
			lowerOrigin := strings.ToLower(origin)
			if strings.HasPrefix(lowerOrigin, "sso://") || strings.HasPrefix(lowerOrigin, "rpc://") {
				scheme, _, _ := strings.Cut(lowerOrigin, "://")
				return AuthModeGoogler, fmt.Sprintf("auto-detected (%s:// git remote)", scheme), nil
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
// When AutoFallback is true, if gob-curl fails (for example, when the 20-hour corp/normal
// SSO ticket in ssh-agent expires while luci-auth OAuth or .gitcookies are still valid),
// it automatically falls back to luci-auth OAuth token, .gitcookies, or .netrc.
// maxGerritRequestBodyBytes bounds in-memory request body buffering for Gerrit
// REST API calls (10 MiB) to support fallback retries without risking OOM.
const maxGerritRequestBodyBytes = maxHTTPBodyBytes

// gobCurlAuthMarkers are lowercase substrings of gob-curl's stderr that point
// at a credential problem. gob-curl exits 1 for every failure (including
// "Scheme must be http(s)"), so its stderr is the only available signal.
var gobCurlAuthMarkers = []string{
	"gcert",
	"credential",
	"loas",
	"sso:",
	"uberproxy",
	"ticket",
	"unauthenticated",
	"unauthorized",
	"expired",
}

// isGobCurlNonAuthFailure reports whether gob-curl's stderr describes a
// failure that is clearly not about credentials. Empty stderr is treated as a
// possible auth failure.
func isGobCurlNonAuthFailure(stderr string) bool {
	lower := strings.ToLower(stderr)
	return lower != "" && !slices.ContainsFunc(gobCurlAuthMarkers, func(m string) bool {
		return strings.Contains(lower, m)
	})
}

type GobCurlTransport struct {
	Path         string // path to gob-curl, defaults to "gob-curl"
	AutoFallback bool   // when true, fall back to luci-auth / cookie / netrc on gob-curl auth failure
	Base         http.RoundTripper

	SkipTokenFallback bool // skip GerritTokenResolver during AutoFallback (already tried upstream)

	mu               sync.Mutex
	fallbackMu       sync.Mutex
	gobCurlVerified  bool
	gobCurlFailed    bool
	gobCurlErr       error
	activeFallback   http.RoundTripper
	activeMethod     AuthMethod
	activeMethodDesc string
}

// ActiveMethod returns the active authentication method and description used by GobCurlTransport.
func (t *GobCurlTransport) ActiveMethod() (AuthMethod, string) {
	t.mu.Lock()
	defer t.mu.Unlock()
	if t.activeMethod != "" {
		return t.activeMethod, t.activeMethodDesc
	}
	return AuthMethodGobCurl, "gob-curl"
}

func (t *GobCurlTransport) checkState(req *http.Request, bodyBytes []byte, hasBody bool) (settled bool, run func() (*http.Response, error)) {
	t.mu.Lock()
	verified, failed := t.gobCurlVerified, t.gobCurlFailed
	fb, cachedErr := t.activeFallback, t.gobCurlErr
	method, desc := t.activeMethod, t.activeMethodDesc
	t.mu.Unlock()
	if failed && fb != nil {
		return true, func() (*http.Response, error) {
			setLastGobCurlFallback(method, desc)
			cloned := req.Clone(req.Context())
			if hasBody {
				cloned.Body = io.NopCloser(bytes.NewReader(bodyBytes))
			}
			return fb.RoundTrip(cloned)
		}
	}
	if failed {
		return true, func() (*http.Response, error) { return nil, cachedErr }
	}
	return verified, nil
}

func (t *GobCurlTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	bodyBytes, hasBody, err := readBoundedRequestBody(req)
	if err != nil {
		return nil, err
	}
	if ctxErr := req.Context().Err(); ctxErr != nil {
		return nil, ctxErr
	}

	var probeLocked bool
	if t.AutoFallback {
		settled, run := t.checkState(req, bodyBytes, hasBody)
		if !settled {
			t.fallbackMu.Lock()
			probeLocked = true
			defer func() {
				if probeLocked {
					t.fallbackMu.Unlock()
				}
			}()
			if settled, run = t.checkState(req, bodyBytes, hasBody); settled {
				t.fallbackMu.Unlock()
				probeLocked = false
			}
		}
		if run != nil {
			return run()
		}
	}

	resp, err := t.execGobCurl(req, bodyBytes, hasBody)
	if err == nil {
		if t.AutoFallback {
			t.mu.Lock()
			t.gobCurlVerified = true
			t.mu.Unlock()
		}
		return resp, nil
	}
	if ctxErr := req.Context().Err(); ctxErr != nil {
		return nil, ctxErr
	}
	if !t.AutoFallback || ExitCodeFor(err) != ExitCodeAuth {
		return nil, err
	}
	if !probeLocked {
		t.fallbackMu.Lock()
		defer t.fallbackMu.Unlock()
	}
	return t.roundTripWithFallback(req, bodyBytes, hasBody, err)
}

func newGobCurlCommand(ctx context.Context, path string, args ...string) *exec.Cmd {
	cmd := exec.CommandContext(ctx, path, args...)
	cmd.Cancel = func() error {
		return cmd.Process.Signal(os.Interrupt)
	}
	cmd.WaitDelay = 2 * time.Second
	return cmd
}

func (t *GobCurlTransport) execGobCurl(req *http.Request, bodyBytes []byte, hasBody bool) (*http.Response, error) {
	path := t.Path
	if path == "" {
		if resolved, err := LookPathFn("gob-curl"); err == nil && resolved != "" {
			path = resolved
		} else {
			path = "gob-curl"
		}
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
	if hasBody {
		args = append(args, "--data-binary", "@-")
		stdin = bytes.NewReader(bodyBytes)
	}

	args = append(args, req.URL.String())

	cmd := newGobCurlCommand(req.Context(), path, args...)
	if stdin != nil {
		cmd.Stdin = stdin
	}

	var stderr bytes.Buffer
	cmd.Stderr = &stderr

	stdoutPipe, err := cmd.StdoutPipe()
	if err != nil {
		return nil, fmt.Errorf("gob-curl stdout pipe: %w", err)
	}

	if err := cmd.Start(); err != nil {
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
			if ctxErr := req.Context().Err(); ctxErr != nil {
				return nil, ctxErr
			}
			stderrStr := strings.TrimSpace(stderr.String())
			if isGobCurlNonAuthFailure(stderrStr) {
				return nil, fmt.Errorf("gob-curl request to %s failed: %s\n\n"+
					"Cause: gob-curl rejected the request for a reason other than credentials.\n\n"+
					"Remediation:\n"+
					"  1. Check the remote URL the Gerrit host is detected from:\n"+
					"     git remote -v\n"+
					"  2. If detection is wrong, pass the host explicitly:\n"+
					"     gh <command> --host <project>-review.googlesource.com",
					req.URL.Host, stderrStr)
			}
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
		return nil, fmt.Errorf("parsing gob-curl HTTP response: %w; stderr: %s", err, stderr.String())
	}

	resp.Body = &cmdResponseBody{
		ReadCloser: resp.Body,
		cmd:        cmd,
	}

	return resp, nil
}

func (t *GobCurlTransport) roundTripWithFallback(req *http.Request, bodyBytes []byte, hasBody bool, gobCurlErr error) (*http.Response, error) {
	if _, run := t.checkState(req, bodyBytes, hasBody); run != nil {
		return run()
	}

	ctx := req.Context()
	type candidate struct {
		tr     http.RoundTripper
		method AuthMethod
		desc   string
	}
	var candidates []candidate

	var tok, src string
	if !t.SkipTokenFallback {
		tok, src, _ = GerritTokenResolver(ctx)
	}
	if tok != "" {
		if src == "" {
			src = "luci-auth"
		}
		candidates = append(candidates, candidate{
			tr:     &TokenTransport{Base: t.Base, Token: tok, Host: req.URL.Hostname()},
			method: AuthMethodToken,
			desc:   fmt.Sprintf("%s (fallback: gob-curl SSO expired)", src),
		})
	}

	if cookieFile := findGitCookieFile(ctx); cookieFile != "" {
		if f, err := os.Open(cookieFile); err == nil {
			matched := ParseNetscapeCookies(f, req.URL, time.Now())
			_ = f.Close()
			if len(matched) > 0 {
				candidates = append(candidates, candidate{
					tr:     &CookieTransport{Base: t.Base, CookieFile: cookieFile},
					method: AuthMethodCookie,
					desc:   fmt.Sprintf("cookie (%s; fallback: gob-curl SSO expired)", cookieFile),
				})
			}
		}
	}

	if hostname := req.URL.Hostname(); hostname != "" {
		if creds := FindNetrcCredentials(hostname); creds != nil {
			candidates = append(candidates, candidate{
				tr:     &BasicAuthTransport{Base: t.Base, Username: creds.Login, Password: creds.Password},
				method: AuthMethodNetrc,
				desc:   "netrc (~/.netrc; fallback: gob-curl SSO expired)",
			})
		}
	}

	for _, cand := range candidates {
		cloned := req.Clone(ctx)
		if hasBody {
			cloned.Body = io.NopCloser(bytes.NewReader(bodyBytes))
		}
		resp, err := cand.tr.RoundTrip(cloned)
		if err != nil {
			continue
		}
		if resp.StatusCode == http.StatusUnauthorized || resp.StatusCode == http.StatusForbidden {
			_ = resp.Body.Close()
			continue
		}
		t.mu.Lock()
		t.gobCurlFailed = true
		t.activeFallback = cand.tr
		t.activeMethod = cand.method
		t.activeMethodDesc = cand.desc
		t.mu.Unlock()
		setLastGobCurlFallback(cand.method, cand.desc)
		return resp, nil
	}

	t.mu.Lock()
	t.gobCurlFailed = true
	t.gobCurlErr = gobCurlErr
	t.mu.Unlock()
	return nil, gobCurlErr
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

	Host     string
	Dynamic  bool
	Floor    http.RoundTripper
	mu       sync.Mutex
	useFloor bool
}

func hostnameOf(raw string) string {
	if raw != "" && !strings.Contains(raw, "://") {
		raw = "https://" + raw
	}
	if u, err := url.Parse(raw); err == nil && u.Hostname() != "" {
		return u.Hostname()
	}
	return CleanGerritHost(raw)
}

func setTokenAuthHeader(req *http.Request, tok string) {
	if strings.Contains(tok, ":") {
		req.Header.Set("Authorization", "Basic "+base64.StdEncoding.EncodeToString([]byte(tok)))
		return
	}
	req.Header.Set("Authorization", "Bearer "+tok)
}

func readBoundedRequestBody(req *http.Request) ([]byte, bool, error) {
	if req.Body == nil || req.Body == http.NoBody {
		return nil, false, nil
	}
	bodyBytes, err := io.ReadAll(io.LimitReader(req.Body, maxGerritRequestBodyBytes+1))
	_ = req.Body.Close()
	if err != nil {
		return nil, false, fmt.Errorf("reading request body: %w", err)
	}
	if len(bodyBytes) > maxGerritRequestBodyBytes {
		return nil, false, fmt.Errorf("request body exceeds maximum allowed size of %d bytes", maxGerritRequestBodyBytes)
	}
	return bodyBytes, true, nil
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

func (t *TokenTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	base := cmp.Or[http.RoundTripper](t.Base, http.DefaultTransport)
	if t.Host != "" && !strings.EqualFold(req.URL.Hostname(), t.Host) {
		return base.RoundTrip(req)
	}
	if t.Dynamic || t.Floor != nil {
		return t.roundTripDynamic(req, base)
	}
	t.mu.Lock()
	tok := t.Token
	t.mu.Unlock()
	if tok != "" {
		req = req.Clone(req.Context())
		setTokenAuthHeader(req, tok)
	}
	return base.RoundTrip(req)
}

func (t *TokenTransport) roundTripDynamic(req *http.Request, base http.RoundTripper) (*http.Response, error) {
	bodyBytes, hasBody, err := readBoundedRequestBody(req)
	if err != nil {
		return nil, err
	}
	cloneReq := func(tok string) *http.Request {
		cloned := req.Clone(req.Context())
		if hasBody {
			cloned.Body = io.NopCloser(bytes.NewReader(bodyBytes))
		}
		if tok != "" {
			setTokenAuthHeader(cloned, tok)
		}
		return cloned
	}

	t.mu.Lock()
	useFloor := t.useFloor
	tok := t.Token
	t.mu.Unlock()
	if useFloor && t.Floor != nil {
		setLastGobCurlFallback(AuthMethodGobCurl, "gob-curl (fallback: token rejected)")
		return t.Floor.RoundTrip(cloneReq(""))
	}

	ctx := req.Context()
	resp, err := base.RoundTrip(cloneReq(tok))
	if err != nil {
		return nil, err
	}
	if rejected := isGerritTokenAuthRejection(resp); rejected {
		if t.Dynamic {
			invalidateCachedGerritToken(tok)
			if newTok, _, mErr := GerritTokenResolver(ctx); mErr == nil && newTok != "" && newTok != tok {
				_ = resp.Body.Close()
				t.mu.Lock()
				t.Token = newTok
				t.mu.Unlock()
				resp, err = base.RoundTrip(cloneReq(newTok))
				if err != nil {
					return nil, err
				}
				rejected = isGerritTokenAuthRejection(resp)
			}
		}
		if rejected && t.Floor != nil {
			_ = resp.Body.Close()
			t.mu.Lock()
			t.useFloor = true
			t.mu.Unlock()
			setLastGobCurlFallback(AuthMethodGobCurl, "gob-curl (fallback: token rejected)")
			return t.Floor.RoundTrip(cloneReq(""))
		}
	}
	return resp, nil
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
		// Priority 1: GERRIT_TOKEN environment variable
		if strings.TrimSpace(os.Getenv("GERRIT_TOKEN")) != "" {
			return AuthMethodToken, "token (GERRIT_TOKEN)"
		}
		// Priority 1b: luci-auth Gerrit OAuth2 token
		if tok, src, err := GerritTokenResolver(ctx); err == nil && tok != "" {
			return AuthMethodToken, fmt.Sprintf("token (%s)", cmp.Or(src, "luci-auth"))
		}
		// Priority 2: gob-curl on PATH
		if _, err := LookPathFn("gob-curl"); err == nil {
			return AuthMethodGobCurl, "gob-curl"
		}
		// Priority 3: Git cookies (.gitcookies or git config http.cookiefile)
		if cookieFile := findGitCookieFile(ctx); cookieFile != "" {
			return AuthMethodCookie, fmt.Sprintf("cookie (%s)", cookieFile)
		}
		// Priority 4: .netrc credentials
		if creds := FindNetrcCredentials(hostnameOf(gerritHost)); creds != nil {
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

	method := AuthMethod(strings.TrimSpace(os.Getenv("GH_ISH_AUTH_METHOD")))
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
		token := strings.TrimSpace(os.Getenv("GERRIT_TOKEN"))
		if token == "" {
			return nil, NewExitCodeError(ExitCodeAuth, "GH_ISH_AUTH_METHOD=token specified but GERRIT_TOKEN is empty.\n\nTo configure a token, run:\n  export GERRIT_TOKEN=\"<token>\"")
		}
		base = &TokenTransport{Token: token, Host: hostnameOf(gerritHost)}

	case AuthMethodGobCurl:
		gobCurlPath, err := LookPathFn("gob-curl")
		if err != nil {
			return nil, NewExitCodeError(ExitCodeAuth, "GH_ISH_AUTH_METHOD=gob-curl specified but gob-curl executable not found in PATH: %w.\n\nEnsure corp development tools are installed and in PATH, or choose a different auth method (e.g. export GH_ISH_AUTH_METHOD=cookie or =token).", err)
		}
		base = &GobCurlTransport{Path: gobCurlPath}

	case AuthMethodCookie:
		cookieFile := findGitCookieFile(ctx)
		if cookieFile == "" {
			return nil, NewExitCodeError(ExitCodeAuth, "GH_ISH_AUTH_METHOD=cookie specified but no cookiefile found (checked git config http.cookiefile and ~/.gitcookies).\n\nTo configure Git cookies:\n  Visit https://<host>/new-password to obtain credentials and configure ~/.gitcookies.")
		}
		base = &CookieTransport{CookieFile: cookieFile}

	case AuthMethodNetrc:
		hostname := hostnameOf(gerritHost)
		creds := FindNetrcCredentials(hostname)
		if creds == nil {
			return nil, NewExitCodeError(ExitCodeAuth, "GH_ISH_AUTH_METHOD=netrc specified but no credentials found for %s in ~/.netrc", hostname)
		}
		base = &BasicAuthTransport{Username: creds.Login, Password: creds.Password}

	case AuthMethodNone:
		base = http.DefaultTransport

	case AuthMethodAuto:
		// Priority 1: GERRIT_TOKEN environment variable
		if token := strings.TrimSpace(os.Getenv("GERRIT_TOKEN")); token != "" {
			base = &TokenTransport{Token: token, Host: hostnameOf(gerritHost)}
			break
		}

		// Priority 1b: luci-auth Gerrit OAuth2 token (works across Linux, macOS, Windows)
		if tok, _, err := GerritTokenResolver(ctx); err == nil && tok != "" {
			var floor http.RoundTripper
			if gobCurlPath, err := LookPathFn("gob-curl"); err == nil {
				floor = &GobCurlTransport{Path: gobCurlPath, AutoFallback: true, SkipTokenFallback: true}
			}
			base = &TokenTransport{Token: tok, Host: hostnameOf(gerritHost), Dynamic: true, Floor: floor}
			break
		}

		// Priority 2: gob-curl on PATH (seamless Google internal auth, with automatic
		// runtime fallback to luci-auth / .gitcookies / .netrc if the 20h SSO ticket is expired)
		if gobCurlPath, err := LookPathFn("gob-curl"); err == nil {
			base = &GobCurlTransport{Path: gobCurlPath, AutoFallback: true}
			break
		}

		// Priority 3: Git cookies (.gitcookies or git config http.cookiefile)
		if cookieFile := findGitCookieFile(ctx); cookieFile != "" {
			base = &CookieTransport{CookieFile: cookieFile}
			break
		}

		// Priority 4: .netrc credentials
		if creds := FindNetrcCredentials(hostnameOf(gerritHost)); creds != nil {
			base = &BasicAuthTransport{Username: creds.Login, Password: creds.Password}
			break
		}

		// Priority 6: In googler mode, do not silently fall back to anonymous Gerrit.
		if mode == AuthModeGoogler {
			cleanHost := CleanGerritHost(gerritHost)
			if cleanHost == "" {
				cleanHost = "<host>"
			}
			return nil, NewExitCodeError(ExitCodeAuth,
				"Gerrit authentication required in googler mode (%s): no Gerrit credentials found for %s.\n\n"+
					"Cause: In googler mode, gh-ish does not fall back to unauthenticated Gerrit requests.\n\n"+
					"Remediation:\n"+
					"  1. (Corp users) Ensure 'gob-curl' is in PATH and run 'gcert', or run 'luci-auth login'.\n"+
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
	if scriptDir := strings.TrimSpace(os.Getenv("PW_GH_SCRIPT_DIR")); scriptDir != "" {
		candidates = append(candidates,
			filepath.Join(scriptDir, "environment", "cipd", "packages", "luci", "luci-auth"),
			filepath.Join(scriptDir, ".environment", "cipd", "packages", "luci", "luci-auth"),
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
func resolveOAuthTokenFromCLI(ctx context.Context, luciScopes []string) (token string, source string, err error) {
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

const oauthTokenCacheTTL = 5 * time.Minute

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
	gerritTokenProbeFailed  bool
	lastGobCurlFallbackMeth AuthMethod
	lastGobCurlFallbackDesc string
)

func setLastGobCurlFallback(method AuthMethod, desc string) {
	gerritTokenMu.Lock()
	lastGobCurlFallbackMeth = method
	lastGobCurlFallbackDesc = desc
	gerritTokenMu.Unlock()
}

func getLastGobCurlFallback() (AuthMethod, string) {
	gerritTokenMu.Lock()
	defer gerritTokenMu.Unlock()
	return lastGobCurlFallbackMeth, lastGobCurlFallbackDesc
}

func clearLastGobCurlFallback() {
	gerritTokenMu.Lock()
	lastGobCurlFallbackMeth = ""
	lastGobCurlFallbackDesc = ""
	gerritTokenMu.Unlock()
}

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
	rpcClientMu.Lock()
	cachedRPCClients = nil
	rpcClientMu.Unlock()

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
	gerritTokenProbeFailed = false
	lastGobCurlFallbackMeth = ""
	lastGobCurlFallbackDesc = ""
	gerritTokenMu.Unlock()

	issueTokenMu.Lock()
	cachedIssueToken = ""
	cachedIssueTokenSource = ""
	cachedIssueTokenExpiry = time.Time{}
	issueTokenMu.Unlock()
}

func invalidateCachedGerritToken(rejected string) {
	gerritTokenMu.Lock()
	if cachedGerritToken == rejected {
		cachedGerritToken = ""
		cachedGerritTokenExpiry = time.Time{}
	}
	gerritTokenProbeFailed = false
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
	if tok := strings.TrimSpace(os.Getenv("GERRIT_TOKEN")); tok != "" {
		return tok, "GERRIT_TOKEN", nil
	}

	gerritTokenMu.Lock()
	defer gerritTokenMu.Unlock()
	if cachedGerritToken != "" && time.Now().Before(cachedGerritTokenExpiry) {
		return cachedGerritToken, cachedGerritTokenSource, nil
	}
	if gerritTokenProbeFailed {
		return "", "", fmt.Errorf("no active Gerrit OAuth2 token found from luci-auth")
	}

	luciAuthBin := findLuciAuthBinary(ctx)
	if luciAuthBin == "" {
		gerritTokenProbeFailed = true
		return "", "", fmt.Errorf("luci-auth binary not found")
	}

	for _, sc := range []string{DefaultGerritScopes, ScopeGerritCodeReview} {
		out, err := OAuthTokenCommandRunner(ctx, luciAuthBin, "token", "-scopes", sc, "-json-output", "-")
		if err == nil {
			tok := strings.TrimSpace(string(out))
			expiry := time.Now().Add(oauthTokenCacheTTL)
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
			if tok != "" {
				cachedGerritToken = tok
				cachedGerritTokenSource = "luci-auth"
				cachedGerritTokenExpiry = expiry
				gerritTokenProbeFailed = false
				return tok, "luci-auth", nil
			}
		}
	}

	gerritTokenProbeFailed = true
	return "", "", fmt.Errorf("no active Gerrit OAuth2 token found from luci-auth")
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
	tok, src, err := resolveOAuthTokenFromCLI(ctx, []string{
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
	cachedAndroidBuildTokenExpiry = time.Now().Add(oauthTokenCacheTTL)
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

	cachedLUCIToken = tok
	cachedLUCITokenSource = src
	cachedLUCITokenExpiry = time.Now().Add(oauthTokenCacheTTL)
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
