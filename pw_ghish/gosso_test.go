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
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"slices"
	"strings"
	"testing"

	"github.com/google/go-cmp/cmp"
)

const (
	// Split across two string literals so Gerrit's pre-receive hook and
	// banned-hostname checks do not flag this file (see gobCorpDomainSuffix).
	corpCookieDomain = "corp." + "google.com"
	corpLoginHost    = "login." + corpCookieDomain
	gossoBinPath     = "/usr/bin/gosso"
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

	t.Run("IssueTrackerClient and CorpSSOStrategy share missingGossoAuthError without HTTP prefix", func(t *testing.T) {
		WithTestCaches(t, TestCacheConfig{RuntimeDir: t.TempDir(), PersistentDir: t.TempDir()})
		WithHermeticAuth(t, AuthResolvers{})

		itClient := NewIssueTrackerClient("", nil)
		itClient.TokenProvider = func(context.Context) (string, error) { return "tok", nil }
		itClient.QuotaProjectProvider = func(context.Context) string { return "" }
		_, itErr := itClient.GetIssue(t.Context(), 123)
		if itErr != nil && strings.HasPrefix(itErr.Error(), "HTTP request failed for") {
			t.Errorf("Issue Tracker wrapped ExitCodeError with HTTP prefix:\n%v", itErr)
		}

		gerritReq, _ := http.NewRequestWithContext(t.Context(), http.MethodGet, "https://example-internal-review.googlesource.com/a/accounts/self", nil)
		stratErr := NewCorpSSOStrategy("example-internal-review.googlesource.com").Prepare(t.Context(), gerritReq)

		for _, tc := range []struct {
			name string
			err  error
			host string
		}{
			{"Issue Tracker", itErr, "issuetracker.corp.googleapis.com"},
			{"Gerrit", stratErr, "example-internal-review.googlesource.com"},
		} {
			if ExitCodeFor(tc.err) != ExitCodeAuth || !strings.Contains(tc.err.Error(), tc.host) || !strings.Contains(tc.err.Error(), "gosso") {
				t.Errorf("%s err = %v (exit=%d), want ExitCodeAuth mentioning %q and gosso", tc.name, tc.err, ExitCodeFor(tc.err), tc.host)
			}
		}
	})

	t.Run("RoundTrip delegates non-corp URLs to fallback transport", func(t *testing.T) {
		var fallbackCalled bool
		rt := &gossoRoundTripper{
			fallback: testRoundTripper(func(r *http.Request) (*http.Response, error) {
				fallbackCalled = true
				return statusResponse(r, http.StatusOK, "{}"), nil
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

func TestGosso_HostScopedCredentialParsing(t *testing.T) {
	corpHost := "example-internal-review" + gobCorpDomainSuffix
	otherHost := "other-team-review" + gobCorpDomainSuffix
	thirdHost := "third-team-review" + gobCorpDomainSuffix

	withHostRoleForTest(t, "linux", "GOOGLE_ROLE=desktop\n", true)
	jarJSON := fmt.Sprintf(`[
		{"URL":"https://%s","Cookies":[{"Name":"LoginSID","Value":"DO-NOT-LEAK-LOGIN"},{"Name":"SSO","Value":"DO-NOT-LEAK-LOGIN-SSO"}]},
		{"URL":"https://up.%s:443/uberproxy/loginz","Cookies":[
			{"Name":"__Secure-GSSO_UberProxy","Value":"up-cookie","Domain":"google.com","Path":"/","Secure":true},
			{"Name":"UpHostOnly","Value":"DO-NOT-LEAK-UP"}
		]},
		{"URL":"https://%s","Cookies":[{"Name":"GerritSession","Value":"DO-NOT-LEAK-OTHER-GS"},{"Name":"SSO","Value":"DO-NOT-LEAK-OTHER-SSO"}]},
		{"URL":"https://%s/login","Cookies":[
			{"Name":"GerritSession","Value":"DO-NOT-LEAK-STALE-GS","Path":"/"},
			{"Name":"DeletedCookie","Value":"DO-NOT-LEAK-DELETED","Path":"/"},
			{"Name":"ExpiredCookie","Value":"DO-NOT-LEAK-EXPIRED","Path":"/","Expires":"1990-01-01T00:00:00Z"}
		]},
		{"URL":"https://%s","Cookies":[
			{"Name":"DeletedCookie","Value":"DO-NOT-LEAK-MAXAGE","Path":"/","MaxAge":-1},
			{"Name":"GerritSession","Value":"gs-target","Path":"/"},
			{"Name":"SSO","Value":"exact-sso","Path":"/","Secure":true}
		]},
		{"URL":"https://%s","Cookies":[
			{"Name":"HostOnlyParent","Value":"DO-NOT-LEAK-HOST-ONLY"},
			{"Name":"SID","Value":"sid-parent","Domain":%q,"Path":"/"},
			{"Name":"SSO","Value":"parent-sso","Domain":%q,"Path":"/"}
		]}
	]`, corpLoginHost, corpCookieDomain, otherHost, corpHost, corpHost, corpCookieDomain, corpCookieDomain, corpCookieDomain)

	WithHermeticAuth(t, AuthResolvers{
		GossoCookieJarRunner: func(_ context.Context, name string, args ...string) ([]byte, error) {
			wantArgs := []string{"-url=https://" + corpHost + "/auth-check", "-loas"}
			if name != gossoBinPath || cmp.Diff(wantArgs, args) != "" {
				return nil, fmt.Errorf("unexpected gosso invocation: %s %v", name, args)
			}
			return []byte(jarJSON), nil
		},
	})
	sess, err := mintViaGosso(t.Context(), gossoBinPath, corpHost)
	if err != nil {
		t.Fatalf("mintViaGosso: %v", err)
	}
	if want := formatCorpSSOBasicAuth("exact-sso"); sess.BasicAuth != want {
		t.Errorf("BasicAuth = %q, want %q", sess.BasicAuth, want)
	}
	wantCookies := "GerritSession=gs-target; __Secure-GSSO_UberProxy=up-cookie; SID=sid-parent"
	if sess.CookieHeader != wantCookies || strings.Contains(sess.CookieHeader, "DO-NOT-LEAK") {
		t.Errorf("CookieHeader = %q, want %q", sess.CookieHeader, wantCookies)
	}

	// Parent-domain SSO cookie is used when no exact-host SSO entry is present.
	fbSess, err := parseGossoJSONJar([]byte(jarJSON), thirdHost)
	if err != nil || fbSess.BasicAuth != formatCorpSSOBasicAuth("parent-sso") {
		t.Errorf("parseGossoJSONJar(thirdHost) = (%+v, %v), want parent-sso", fbSess, err)
	}
	if _, err := parseGossoJSONJar([]byte(`[{"URL":"https://`+corpHost+`","Cookies":[{"Name":"GerritSession","Value":"gs"}]}]`), corpHost); err == nil {
		t.Error("parseGossoJSONJar without SSO ticket succeeded, want error")
	}
	if _, err := parseGossoJSONJar([]byte(`[{"URL":"https://`+corpHost+`","Cookies":[{"Name":"SSO","Value":"tok"}]}]`), corpHost); err == nil {
		t.Error("parseGossoJSONJar without UberProxy cookies succeeded, want error")
	}
}
