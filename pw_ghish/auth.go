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
	"time"
)

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
// when /a/ returns 400 or 401 for GET requests on googlesource.com.
type fallbackTransport struct {
	base http.RoundTripper
}

func (t *fallbackTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	resp, err := t.base.RoundTrip(req)
	if err != nil {
		return resp, err
	}

	if (resp.StatusCode == 400 || resp.StatusCode == 401) && strings.Contains(req.URL.Host, ".googlesource.com") && strings.Contains(req.URL.Path, "/a/") {
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
			return nil, fmt.Errorf("reading gob-curl response headers: %w; stderr: %s", err, stderr.String())
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

// NewAuthTransportContext returns an authenticated http.RoundTripper appropriate for the environment,
// passing ctx through for git config lookups and timeouts.
func NewAuthTransportContext(ctx context.Context, gerritHost string) (http.RoundTripper, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	method := AuthMethod(os.Getenv("GH_ISH_AUTH_METHOD"))
	if method == "" {
		method = AuthMethodAuto
	}

	var base http.RoundTripper

	switch method {
	case AuthMethodToken:
		token := os.Getenv("GERRIT_TOKEN")
		if token == "" {
			return nil, fmt.Errorf("GH_ISH_AUTH_METHOD=token specified but GERRIT_TOKEN is empty.\n\nTo configure a token, run:\n  export GERRIT_TOKEN=\"<token>\"")
		}
		base = &TokenTransport{Token: token}

	case AuthMethodGobCurl:
		if _, err := LookPathFn("gob-curl"); err != nil {
			return nil, fmt.Errorf("GH_ISH_AUTH_METHOD=gob-curl specified but gob-curl executable not found in PATH: %w.\n\nEnsure corp development tools are installed and in PATH, or choose a different auth method (e.g. export GH_ISH_AUTH_METHOD=cookie or =token).", err)
		}
		base = &GobCurlTransport{}

	case AuthMethodCookie:
		cookieFile := findGitCookieFile(ctx)
		if cookieFile == "" {
			return nil, fmt.Errorf("GH_ISH_AUTH_METHOD=cookie specified but no cookiefile found (checked git config http.cookiefile and ~/.gitcookies).\n\nTo configure Git cookies:\n  Visit https://<host>/new-password to obtain credentials and configure ~/.gitcookies.")
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
			return nil, fmt.Errorf("GH_ISH_AUTH_METHOD=netrc specified but no credentials found for %s in ~/.netrc", hostname)
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

		// Priority 5: Fallback to anonymous
		base = http.DefaultTransport

	default:
		return nil, fmt.Errorf("unrecognized auth method: %q", method)
	}

	return &fallbackTransport{base: base}, nil
}

// NewAuthTransport returns an authenticated http.RoundTripper appropriate for the environment.
func NewAuthTransport(gerritHost string) (http.RoundTripper, error) {
	return NewAuthTransportContext(context.Background(), gerritHost)
}
