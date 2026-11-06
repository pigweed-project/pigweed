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
	"io"
	"net/http"
	"net/url"
	"os"
	"slices"
	"strings"
	"testing"
)

func TestParseGossoResponse(t *testing.T) {
	req, err := http.NewRequest(http.MethodGet, "https://issuetracker.corp.googleapis.com/v1/issues/123", nil)
	if err != nil {
		t.Fatalf("http.NewRequest: %v", err)
	}

	t.Run("standard HTTP/1.1 response with banner prefix", func(t *testing.T) {
		raw := "gosso: connecting to uberproxy...\r\n" +
			"HTTP/1.1 200 OK\r\n" +
			"Content-Type: application/json\r\n" +
			"\r\n" +
			`{"issueId":"123"}`
		resp, err := parseGossoResponse([]byte(raw), req)
		if err != nil {
			t.Fatalf("parseGossoResponse unexpected error: %v", err)
		}
		defer resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			t.Errorf("StatusCode = %d, want 200", resp.StatusCode)
		}
		body, _ := io.ReadAll(resp.Body)
		if string(body) != `{"issueId":"123"}` {
			t.Errorf("body = %q, want %q", string(body), `{"issueId":"123"}`)
		}
	})

	t.Run("HTTP/2 normalized to HTTP/1.1", func(t *testing.T) {
		raw := "HTTP/2 404 Not Found\r\n" +
			"Content-Type: application/json\r\n" +
			"\r\n" +
			`{"error":{"code":404}}`
		resp, err := parseGossoResponse([]byte(raw), req)
		if err != nil {
			t.Fatalf("parseGossoResponse unexpected error: %v", err)
		}
		defer resp.Body.Close()
		if resp.StatusCode != http.StatusNotFound {
			t.Errorf("StatusCode = %d, want 404", resp.StatusCode)
		}
	})

	t.Run("non-HTTP output names gosso", func(t *testing.T) {
		_, err := parseGossoResponse([]byte("gosso: handshake error"), req)
		if err == nil || !strings.Contains(err.Error(), "gosso returned non-HTTP output") {
			t.Errorf("parseGossoResponse error = %v, want 'gosso returned non-HTTP output'", err)
		}
	})
}

func TestGossoRoundTripper(t *testing.T) {
	req, err := http.NewRequest(http.MethodGet, "https://issuetracker.corp.googleapis.com/v1/issues/123", nil)
	if err != nil {
		t.Fatalf("http.NewRequest: %v", err)
	}

	t.Run("RoundTrip appends -loas only on LOAS Linux and sanitizes CRLF headers", func(t *testing.T) {
		for _, tc := range []struct {
			name, lsb string
			wantLOAS  bool
		}{
			{"workstation", "GOOGLE_ROLE=desktop\n", true},
			{"laptop", "GOOGLE_ROLE=laptop\n", false},
		} {
			t.Run(tc.name, func(t *testing.T) {
				withHostRoleForTest(t, "linux", tc.lsb, true)
				WithHermeticAuth(t, AuthResolvers{})

				var gotArgs []string
				rt := &gossoRoundTripper{
					exec: func(_ context.Context, args ...string) ([]byte, error) {
						gotArgs = args
						return []byte("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{}"), nil
					},
				}

				rtReq := req.Clone(t.Context())
				rtReq.Header.Set("Authorization", "Bearer gosso-token")
				rtReq.Header.Set("X-Goog-User-Project", "proj\r\nX-CRLF: bad")

				resp, err := rt.RoundTrip(rtReq)
				if err != nil {
					t.Fatalf("RoundTrip: %v", err)
				}
				resp.Body.Close()

				if got := slices.Contains(gotArgs, "-loas"); got != tc.wantLOAS {
					t.Errorf("slices.Contains(gotArgs, \"-loas\") = %v, want %v (args=%q)", got, tc.wantLOAS, gotArgs)
				}
				if !slices.Contains(gotArgs, "X-Goog-User-Project: projX-CRLF: bad") {
					t.Errorf("expected CRLF-sanitized header in gosso args: %q", gotArgs)
				}
			})
		}
	})

	t.Run("RoundTrip fails on mid-stream gosso error after 200 OK headers", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{})
		rt := &gossoRoundTripper{
			exec: func(_ context.Context, _ ...string) ([]byte, error) {
				return []byte("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"issueId\":"), errors.New("exit status 1")
			},
		}
		if _, err := rt.RoundTrip(req.Clone(t.Context())); err == nil || !strings.Contains(err.Error(), "gosso execution failed") {
			t.Errorf("RoundTrip mid-stream error = %v, want 'gosso execution failed'", err)
		}
	})

	t.Run("RoundTrip invokes gosso with expected flags and parses non-2xx on exit status 1", func(t *testing.T) {
		withHostRoleForTest(t, "darwin", "", false)
		WithHermeticAuth(t, AuthResolvers{
			LookPath: func(file string) (string, error) {
				if file == "gosso" || file == "/usr/local/bin/gosso" {
					return "/usr/local/bin/gosso", nil
				}
				return "", errors.New("not found")
			},
		})
		var gotArgs []string
		var dataPath, dataBody string
		rt := &gossoRoundTripper{
			exec: func(_ context.Context, args ...string) ([]byte, error) {
				gotArgs = args
				for i, a := range args {
					if a == "-data_file" && i+1 < len(args) {
						dataPath = args[i+1]
						b, err := os.ReadFile(dataPath)
						if err != nil {
							return nil, err
						}
						dataBody = string(b)
					}
				}
				raw := "HTTP/1.1 403 Forbidden\r\n" +
					"Content-Type: application/json\r\n" +
					"\r\n" +
					`{"error":{"code":403,"message":"SERVICE_DISABLED"}}`
				return []byte(raw), errors.New("exit status 1")
			},
		}

		postReq, err := http.NewRequestWithContext(
			t.Context(),
			http.MethodPost,
			"https://issuetracker.corp.googleapis.com/v1/issues",
			strings.NewReader(`{"issueState":{"title":"test"}}`),
		)
		if err != nil {
			t.Fatalf("http.NewRequestWithContext: %v", err)
		}
		postReq.Header.Set("Authorization", "Bearer gosso-token")
		postReq.Header.Set("Accept", "application/json")

		resp, err := rt.RoundTrip(postReq)
		if err != nil {
			t.Fatalf("RoundTrip with gosso exit status 1 unexpected error: %v", err)
		}
		defer resp.Body.Close()
		if resp.StatusCode != http.StatusForbidden {
			t.Errorf("StatusCode = %d, want 403", resp.StatusCode)
		}
		bodyBytes, _ := io.ReadAll(resp.Body)
		if !strings.Contains(string(bodyBytes), "SERVICE_DISABLED") {
			t.Errorf("body = %q, want SERVICE_DISABLED payload", string(bodyBytes))
		}
		if dataBody != `{"issueState":{"title":"test"}}` {
			t.Errorf("data_file content = %q, want request body", dataBody)
		}
		if _, err := os.Stat(dataPath); !errors.Is(err, os.ErrNotExist) {
			t.Errorf("expected temp data_file %q to be removed after RoundTrip, stat err = %v", dataPath, err)
		}

		wantArgs := []string{
			"-dump_headers", "-method", "POST",
			"-header", "Accept: application/json",
			"-header", "Authorization: Bearer gosso-token",
			"-data_file", dataPath,
			"-url", "https://issuetracker.corp.googleapis.com/v1/issues",
		}
		if !slices.Equal(gotArgs, wantArgs) {
			t.Errorf("gosso args = %q, want %q", gotArgs, wantArgs)
		}
	})

	t.Run("IssueTrackerClient returns missingGossoAuthError without HTTP prefix", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{})

		itClient := NewIssueTrackerClient("", nil)
		itClient.TokenProvider = func(context.Context) (string, error) { return "tok", nil }
		itClient.QuotaProjectProvider = func(context.Context) string { return "" }
		_, itErr := itClient.GetIssue(t.Context(), 123)
		if itErr != nil && strings.HasPrefix(itErr.Error(), "HTTP request failed for") {
			t.Errorf("Issue Tracker wrapped ExitCodeError with HTTP prefix:\n%v", itErr)
		}
		if ExitCodeFor(itErr) != ExitCodeAuth || !strings.Contains(itErr.Error(), "issuetracker.corp.googleapis.com") || !strings.Contains(itErr.Error(), "gosso") {
			t.Errorf("Issue Tracker err = %v (exit=%d), want ExitCodeAuth mentioning issuetracker.corp.googleapis.com and gosso", itErr, ExitCodeFor(itErr))
		}
	})

	t.Run("RoundTrip delegates non-corp URLs to fallback transport", func(t *testing.T) {
		var fallbackCalled bool
		rt := &gossoRoundTripper{
			fallback: testRoundTripper(func(*http.Request) (*http.Response, error) {
				fallbackCalled = true
				return &http.Response{
					StatusCode: http.StatusOK,
					Body:       io.NopCloser(strings.NewReader("{}")),
					Header:     make(http.Header),
				}, nil
			}),
			exec: func(_ context.Context, _ ...string) ([]byte, error) {
				t.Fatal("unexpected gosso exec for non-corp URL")
				return nil, nil
			},
		}
		nonCorpReq, _ := http.NewRequestWithContext(t.Context(), http.MethodGet, "https://issuetracker.googleapis.com/v1/issues/123", nil)
		resp, err := rt.RoundTrip(nonCorpReq)
		if err != nil {
			t.Fatalf("RoundTrip non-corp URL error: %v", err)
		}
		resp.Body.Close()
		if !fallbackCalled {
			t.Error("expected non-corp URL to delegate to fallback transport")
		}
	})

	t.Run("isCorpIssueTrackerURL rejects spoofed and plaintext URLs", func(t *testing.T) {
		for _, tc := range []struct {
			rawURL string
			want   bool
		}{
			{"https://issuetracker.corp.googleapis.com/v1", true},
			{"HTTPS://IssueTracker.CORP.GoogleAPIs.com:443/v1", true},
			{"https://autopush-issuetracker.corp.googleapis.com/v1", true},
			{"http://issuetracker.corp.googleapis.com/v1", false},
			{"https://issuetracker.corp.googleapis.com.evil.com/v1", false},
			{"https://evil.com/v1?host=issuetracker.corp.googleapis.com", false},
			{"https://user.corp.googleapis.com@evil.com/v1", false},
			{"https://.corp.googleapis.com/v1", false},
			{"https://corp.googleapis.com/v1", false},
		} {
			u, err := url.Parse(tc.rawURL)
			if err != nil {
				t.Fatalf("url.Parse(%q): %v", tc.rawURL, err)
			}
			if got := isCorpIssueTrackerURL(u); got != tc.want {
				t.Errorf("isCorpIssueTrackerURL(%q) = %v, want %v", tc.rawURL, got, tc.want)
			}
		}
	})
}
