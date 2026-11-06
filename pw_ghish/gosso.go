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
	"errors"
	"fmt"
	"io"
	"maps"
	"net/http"
	"net/http/cookiejar"
	"net/url"
	"os"
	"os/exec"
	"slices"
	"strings"
)

const maxGossoCookieJarBytes = 256 * 1024

// findGossoBinary locates the Corp SSO HTTP helper (`gosso`) on PATH or at
// standard gLinux / gMac / BinFS paths.
func findGossoBinary() string {
	return lookPathFirst("gosso", "/usr/bin/gosso", "/usr/local/bin/gosso", "/google/bin/releases/gosso/gosso")
}

func gossoArgs(args ...string) []string {
	if isLOASLinux() {
		return append(args, "-loas")
	}
	return args
}

// gossoRoundTripper implements http.RoundTripper for 1P internal requests
// targeting `*.corp.googleapis.com`. It invokes `gosso` to attach UberProxy
// security tickets alongside the user's OAuth2 bearer token, parsing the raw
// HTTP response back into an *http.Response.
type gossoRoundTripper struct {
	fallback http.RoundTripper
	exec     func(ctx context.Context, args ...string) ([]byte, error)
}

func (rt *gossoRoundTripper) RoundTrip(req *http.Request) (*http.Response, error) {
	if !isCorpIssueTrackerURL(req.URL) {
		return cmp.Or(rt.fallback, http.DefaultTransport).RoundTrip(req)
	}
	if req.Body != nil {
		defer req.Body.Close()
	}

	binPath := findGossoBinary()
	if binPath == "" && rt.exec == nil {
		return nil, missingGossoAuthError("Google Issue Tracker (gh issue)", req.URL.Host, "gh auth status")
	}

	tmpBodyPath, cleanupBody, err := writeGossoBodyFile(req.Body)
	if err != nil {
		return nil, err
	}
	defer cleanupBody()

	args := gossoArgs("-dump_headers", "-method", req.Method)
	// gosso uses pure-Go flag parsing without --flagfile or header-file flags,
	// so request headers must be passed via -header.
	stripCRLF := strings.NewReplacer("\r", "", "\n", "")
	for _, k := range slices.Sorted(maps.Keys(req.Header)) {
		for _, v := range req.Header[k] {
			args = append(args, "-header", fmt.Sprintf("%s: %s", stripCRLF.Replace(k), stripCRLF.Replace(v)))
		}
	}
	if tmpBodyPath != "" {
		args = append(args, "-data_file", tmpBodyPath)
	}
	args = append(args, "-url", req.URL.String())

	var out []byte
	if rt.exec != nil {
		out, err = rt.exec(req.Context(), args...)
	} else {
		cmd := ExecCommandContext(req.Context(), binPath, args...)
		cmd.Env = ScrubRepoLocalGitEnv(os.Environ())
		cmd.Stdin = bytes.NewReader(nil)
		out, err = cmd.Output()
	}
	gossoErr := func() error {
		return fmt.Errorf("gosso execution failed for %s %s: %w (output: %s)",
			req.Method, req.URL.String(), err, cmp.Or(strings.TrimSpace(string(out)), commandErrorDetail(err)))
	}
	if err != nil && !bytes.Contains(out, []byte("HTTP/")) {
		return nil, gossoErr()
	}
	resp, parseErr := parseGossoResponse(out, req)
	if parseErr != nil {
		if err != nil {
			return nil, gossoErr()
		}
		return nil, parseErr
	}
	if err != nil && resp.StatusCode >= 200 && resp.StatusCode < 300 {
		_ = resp.Body.Close()
		return nil, gossoErr()
	}
	return resp, nil
}

func writeGossoBodyFile(body io.Reader) (string, func(), error) {
	noop := func() {}
	if body == nil {
		return "", noop, nil
	}
	bodyBytes, err := readBoundedBody(body, maxHTTPBodyBytes)
	if err != nil {
		return "", noop, fmt.Errorf("failed to read request body for gosso: %w", err)
	}
	if len(bodyBytes) == 0 {
		return "", noop, nil
	}
	f, err := os.CreateTemp("", "ghish-gosso-body-*.json")
	if err != nil {
		return "", noop, fmt.Errorf("failed to create gosso payload file: %w", err)
	}
	path := f.Name()
	cleanup := func() { _ = os.Remove(path) }
	_, writeErr := f.Write(bodyBytes)
	if err := errors.Join(writeErr, f.Close()); err != nil {
		cleanup()
		return "", noop, fmt.Errorf("failed to write gosso payload file: %w", err)
	}
	return path, cleanup, nil
}

func parseGossoResponse(out []byte, req *http.Request) (*http.Response, error) {
	idx := bytes.Index(out, []byte("HTTP/"))
	if idx < 0 {
		return nil, fmt.Errorf("gosso returned non-HTTP output: %s", strings.TrimSpace(string(out)))
	}
	raw := out[idx:]
	if bytes.HasPrefix(raw, []byte("HTTP/2 ")) {
		raw = append([]byte("HTTP/1.1 "), raw[len("HTTP/2 "):]...)
	}
	resp, err := http.ReadResponse(bufio.NewReader(bytes.NewReader(raw)), req)
	if err != nil {
		return nil, fmt.Errorf("failed to parse gosso HTTP response: %w", err)
	}
	return resp, nil
}

// gossoCookieJarRunner executes gosso with an in-memory -cookie-jar pipe and
// returns the serialized JSON jar. Tests override it via AuthResolvers.
var gossoCookieJarRunner = defaultGossoCookieJarRunner

func defaultGossoCookieJarRunner(ctx context.Context, gossoBin string, args ...string) ([]byte, error) {
	rPipe, wPipe, err := os.Pipe()
	if err != nil {
		return nil, err
	}
	var stderr bytes.Buffer
	// gosso writes the HTTP body to stdout and requires a file path for
	// -cookie-jar. In Go's os/exec, ExtraFiles[0] is inherited by the child as
	// fd 3 (3+i), so /dev/fd/3 streams the jar over an anonymous pipe without
	// writing live SSO cookies to /tmp.
	cmd := ExecCommandContext(ctx, gossoBin, slices.Concat(args, []string{"-cookie-jar=/dev/fd/3"})...)
	cmd.Stderr = &stderr
	cmd.ExtraFiles = []*os.File{wPipe}
	if err := cmd.Start(); err != nil {
		_ = wPipe.Close()
		_ = rPipe.Close()
		return nil, err
	}
	_ = wPipe.Close()

	jarData, readErr := readBoundedBody(rPipe, maxGossoCookieJarBytes)
	_ = rPipe.Close()
	waitErr := cmd.Wait()
	if readErr != nil {
		return nil, readErr
	}
	if waitErr != nil {
		if exitErr, ok := errors.AsType[*exec.ExitError](waitErr); ok {
			exitErr.Stderr = stderr.Bytes()
		}
		return nil, waitErr
	}
	return jarData, nil
}

func mintViaGosso(ctx context.Context, gossoBin, corpHost string) (ssoSession, error) {
	authURL := (&url.URL{Scheme: "https", Host: corpHost, Path: "/auth-check"}).String()
	args := gossoArgs("-url=" + authURL)
	cmdDesc := "gosso"
	if slices.Contains(args, "-loas") {
		cmdDesc += " -loas"
	}
	jarData, err := gossoCookieJarRunner(ctx, gossoBin, args...)
	if ctxErr := ctx.Err(); ctxErr != nil {
		return ssoSession{}, ctxErr
	}
	if err != nil {
		return ssoSession{}, fmt.Errorf("%s failed (%s): %w", cmdDesc, commandErrorDetail(err), err)
	}
	return parseGossoJSONJar(jarData, corpHost)
}

func formatCorpSSOBasicAuth(ssoTicket string) string {
	return "Basic " + base64.StdEncoding.EncodeToString([]byte("Google-CorpSSO:"+ssoTicket))
}

type gossoJarEntry struct {
	URL     string         `json:"URL"`
	Cookies []*http.Cookie `json:"Cookies"`
}

func parseGossoJSONJar(data []byte, corpHost string) (ssoSession, error) {
	var entries []gossoJarEntry
	if err := json.Unmarshal(data, &entries); err != nil {
		return ssoSession{}, fmt.Errorf("parsing gosso cookie jar JSON: %w", err)
	}
	jar, err := cookiejar.New(nil)
	if err != nil {
		return ssoSession{}, err
	}
	// Insert exact-host entries before parent-domain entries: cookiejar keys
	// host-only and domain cookies separately, and jar.Cookies orders equal-path
	// matches by earliest insertion (RFC 6265 §5.4) while stripping Domain.
	for _, exact := range []bool{true, false} {
		for _, entry := range entries {
			u, err := url.Parse(entry.URL)
			if err != nil || strings.EqualFold(u.Hostname(), corpHost) != exact {
				continue
			}
			jar.SetCookies(u, entry.Cookies)
		}
	}

	// Query with https:// and Gerrit's authenticated /a/ prefix so Secure
	// UberProxy cookies match (even though local Uplink speaks http:// on
	// loopback) alongside any root- or /a/-scoped host cookies.
	target := &url.URL{Scheme: "https", Host: corpHost, Path: "/a/"}
	// UberProxy authenticates session cookies from the Cookie header, whereas
	// Gerrit's /a/ REST API authenticates non-browser callers via HTTP Basic
	// auth ("Google-CorpSSO:<SSO>") rather than the SSO cookie.
	cookieFormatter := &http.Request{Header: make(http.Header)}
	var ssoTicket string
	for _, c := range jar.Cookies(target) {
		if c.Name == "SSO" {
			if ssoTicket == "" {
				ssoTicket = c.Value
			}
			continue
		}
		cookieFormatter.AddCookie(c)
	}
	if ssoTicket == "" {
		return ssoSession{}, errors.New("gosso cookie jar contained no SSO ticket")
	}
	cookieHeader := cookieFormatter.Header.Get("Cookie")
	if cookieHeader == "" {
		return ssoSession{}, errors.New("gosso cookie jar contained no session cookies")
	}
	return ssoSession{
		BasicAuth:    formatCorpSSOBasicAuth(ssoTicket),
		CookieHeader: cookieHeader,
	}, nil
}
