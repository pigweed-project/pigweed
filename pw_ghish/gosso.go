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
	"errors"
	"fmt"
	"io"
	"maps"
	"net/http"
	"os"
	"slices"
	"strings"
)

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
