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
	"context"
	"encoding/base64"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func TestParseNetscapeCookies(t *testing.T) {
	now := time.Unix(1700000000, 0)
	future := now.Unix() + 3600
	past := now.Unix() - 3600

	cookieContent := fmt.Sprintf(`# Netscape HTTP Cookie File
# http://curl.haxx.se/rfc/cookie_spec.html

.googlesource.com	TRUE	/	TRUE	%d	o	valid-subdomain-cookie
pigweed-review.googlesource.com	FALSE	/	TRUE	%d	auth	valid-host-cookie
.googlesource.com	TRUE	/	TRUE	%d	expired	expired-cookie
#HttpOnly_.googlesource.com	TRUE	/	TRUE	%d	httponly	valid-httponly-cookie
other.com	TRUE	/	TRUE	%d	other	wrong-domain
.googlesource.com	TRUE	/restricted	TRUE	%d	restricted	path-mismatch
`, future, future, past, future, future, future)

	targetURL, _ := url.Parse("https://pigweed-review.googlesource.com/a/changes/123")
	cookies := ParseNetscapeCookies(strings.NewReader(cookieContent), targetURL, now)

	cookieMap := make(map[string]string)
	for _, c := range cookies {
		cookieMap[c.Name] = c.Value
	}

	if cookieMap["o"] != "valid-subdomain-cookie" {
		t.Errorf("cookie 'o' = %q, want 'valid-subdomain-cookie'", cookieMap["o"])
	}
	if cookieMap["auth"] != "valid-host-cookie" {
		t.Errorf("cookie 'auth' = %q, want 'valid-host-cookie'", cookieMap["auth"])
	}
	if cookieMap["httponly"] != "valid-httponly-cookie" {
		t.Errorf("cookie 'httponly' = %q, want 'valid-httponly-cookie'", cookieMap["httponly"])
	}
	if _, exists := cookieMap["expired"]; exists {
		t.Errorf("expired cookie was unexpectedly parsed")
	}
	if _, exists := cookieMap["other"]; exists {
		t.Errorf("other.com cookie was unexpectedly parsed for googlesource.com")
	}
	if _, exists := cookieMap["restricted"]; exists {
		t.Errorf("restricted path cookie was unexpectedly parsed for /a/changes/123")
	}
}

func TestParseNetrc(t *testing.T) {
	netrcContent := `# Sample netrc
machine pigweed-review.googlesource.com login keir password secret123
machine fuchsia-review.googlesource.com login alice password secret456
machine other.com login bob password secret789
`
	creds := ParseNetrc(strings.NewReader(netrcContent), "pigweed-review.googlesource.com")
	if creds == nil {
		t.Fatal("ParseNetrc returned nil for pigweed-review.googlesource.com")
	}
	if creds.Login != "keir" || creds.Password != "secret123" {
		t.Errorf("got login=%q password=%q, want keir:secret123", creds.Login, creds.Password)
	}

	credsFuchsia := ParseNetrc(strings.NewReader(netrcContent), "fuchsia-review.googlesource.com")
	if credsFuchsia == nil || credsFuchsia.Login != "alice" {
		t.Errorf("ParseNetrc failed for fuchsia-review: %+v", credsFuchsia)
	}

	credsMissing := ParseNetrc(strings.NewReader(netrcContent), "nonexistent.com")
	if credsMissing != nil {
		t.Errorf("ParseNetrc for nonexistent host returned %+v, want nil", credsMissing)
	}
}

type mockTransportRecorder struct {
	lastReq *http.Request
}

func (m *mockTransportRecorder) RoundTrip(req *http.Request) (*http.Response, error) {
	m.lastReq = req
	return &http.Response{StatusCode: 200}, nil
}

func TestBasicAuthTransport(t *testing.T) {
	rec := &mockTransportRecorder{}
	transport := &BasicAuthTransport{
		Base:     rec,
		Username: "myuser",
		Password: "mypassword",
	}

	req, _ := http.NewRequest("GET", "https://example.com/api", nil)
	_, err := transport.RoundTrip(req)
	if err != nil {
		t.Fatalf("RoundTrip failed: %v", err)
	}

	gotAuth := rec.lastReq.Header.Get("Authorization")
	wantAuth := "Basic " + base64.StdEncoding.EncodeToString([]byte("myuser:mypassword"))
	if gotAuth != wantAuth {
		t.Errorf("Authorization header = %q, want %q", gotAuth, wantAuth)
	}
}

func TestTokenTransport(t *testing.T) {
	t.Run("Bearer token", func(t *testing.T) {
		rec := &mockTransportRecorder{}
		transport := &TokenTransport{
			Base:  rec,
			Token: "sample-token-12345",
		}

		req, _ := http.NewRequest("GET", "https://example.com/api", nil)
		_, err := transport.RoundTrip(req)
		if err != nil {
			t.Fatalf("RoundTrip failed: %v", err)
		}

		if got := rec.lastReq.Header.Get("Authorization"); got != "Bearer sample-token-12345" {
			t.Errorf("Authorization = %q, want Bearer token", got)
		}
	})

	t.Run("User:Password token", func(t *testing.T) {
		rec := &mockTransportRecorder{}
		transport := &TokenTransport{
			Base:  rec,
			Token: "user:tokenpassword",
		}

		req, _ := http.NewRequest("GET", "https://example.com/api", nil)
		_, err := transport.RoundTrip(req)
		if err != nil {
			t.Fatalf("RoundTrip failed: %v", err)
		}

		want := "Basic " + base64.StdEncoding.EncodeToString([]byte("user:tokenpassword"))
		if got := rec.lastReq.Header.Get("Authorization"); got != want {
			t.Errorf("Authorization = %q, want %q", got, want)
		}
	})

	t.Run("Host pinning", func(t *testing.T) {
		rec := &mockTransportRecorder{}
		transport := &TokenTransport{
			Base:  rec,
			Token: "pinned-token",
			Host:  "pigweed-review.googlesource.com",
		}

		sameHost, _ := http.NewRequest("GET", "https://pigweed-review.googlesource.com/a/changes/1", nil)
		if _, err := transport.RoundTrip(sameHost); err != nil {
			t.Fatalf("RoundTrip(sameHost) failed: %v", err)
		}
		if got := rec.lastReq.Header.Get("Authorization"); got != "Bearer pinned-token" {
			t.Errorf("same-host Authorization = %q, want 'Bearer pinned-token'", got)
		}

		crossHost, _ := http.NewRequest("GET", "https://storage.googleapis.com/bucket/obj", nil)
		if _, err := transport.RoundTrip(crossHost); err != nil {
			t.Fatalf("RoundTrip(crossHost) failed: %v", err)
		}
		if got := rec.lastReq.Header.Get("Authorization"); got != "" {
			t.Errorf("cross-host Authorization = %q, want empty", got)
		}
	})

	t.Run("Dynamic re-mint and retry bounds", func(t *testing.T) {
		origResolver := GerritTokenResolver
		t.Cleanup(func() { GerritTokenResolver = origResolver })

		var mints, reqs int
		var gotBodies []string
		base := &mockTransport{
			roundTrip: func(req *http.Request) (*http.Response, error) {
				reqs++
				if req.Body != nil {
					b, _ := io.ReadAll(req.Body)
					gotBodies = append(gotBodies, string(b))
				}
				status := http.StatusUnauthorized
				if req.Header.Get("Authorization") == "Bearer fresh-token" {
					status = http.StatusOK
				}
				return &http.Response{
					StatusCode: status,
					Body:       io.NopCloser(strings.NewReader("ok")),
					Header:     make(http.Header),
					Request:    req,
				}, nil
			},
		}

		for _, tc := range []struct {
			name, initTok, body      string
			mintFn                   func(int) string
			wantStatus, mints, nReqs int
		}{
			{"re-mint and replay", "stale-token", `{"message":"hi"}`, func(int) string { return "fresh-token" }, http.StatusOK, 1, 2},
			{"same token", "same-token", "", func(int) string { return "same-token" }, http.StatusUnauthorized, 1, 1},
			{"retry also 401", "bad-0", "", func(m int) string { return fmt.Sprintf("still-bad-%d", m) }, http.StatusUnauthorized, 1, 2},
		} {
			mints, reqs, gotBodies = 0, 0, nil
			GerritTokenResolver = func(ctx context.Context) (string, string, error) {
				mints++
				return tc.mintFn(mints), "luci-auth", nil
			}
			var body io.Reader
			if tc.body != "" {
				body = strings.NewReader(tc.body)
			}
			req, _ := http.NewRequest("POST", "https://pigweed-review.googlesource.com/a/changes/1/review", body)
			resp, err := (&TokenTransport{Base: base, Token: tc.initTok, Dynamic: true}).RoundTrip(req)
			if err != nil || resp.StatusCode != tc.wantStatus {
				t.Fatalf("%s: RoundTrip = (%v, %v), want %d", tc.name, resp, err, tc.wantStatus)
			}
			_ = resp.Body.Close()
			if mints != tc.mints || reqs != tc.nReqs {
				t.Errorf("%s: mints=%d reqs=%d, want %d and %d", tc.name, mints, reqs, tc.mints, tc.nReqs)
			}
			if tc.body != "" && (len(gotBodies) != 2 || gotBodies[0] != tc.body || gotBodies[1] != tc.body) {
				t.Errorf("%s: bodies = %v, want payload replayed on retry", tc.name, gotBodies)
			}
		}
	})

	t.Run("Floor fallback", func(t *testing.T) {
		ResetAuthTokenCaches()
		t.Cleanup(ResetAuthTokenCaches)
		t.Setenv("GH_ISH_AUTH_METHOD", "auto")
		t.Setenv("GERRIT_TOKEN", "")

		origLookPath := LookPathFn
		LookPathFn = func(file string) (string, error) {
			if file == "gob-curl" {
				return "/usr/bin/gob-curl", nil
			}
			return "", os.ErrNotExist
		}
		t.Cleanup(func() { LookPathFn = origLookPath })

		origResolver := GerritTokenResolver
		GerritTokenResolver = func(ctx context.Context) (string, string, error) {
			return "rejected-token", "luci-auth", nil
		}
		t.Cleanup(func() { GerritTokenResolver = origResolver })

		tr, err := NewAuthTransportContext(t.Context(), "https://pigweed-review.googlesource.com")
		if err != nil {
			t.Fatalf("NewAuthTransportContext failed: %v", err)
		}
		tokTr, ok := tr.(*fallbackTransport).base.(*TokenTransport)
		if !ok || !tokTr.Dynamic || tokTr.Host != "pigweed-review.googlesource.com" || tokTr.Floor == nil {
			t.Fatalf("expected dynamic *TokenTransport with Host and gob-curl Floor, got %#v", tr.(*fallbackTransport).base)
		}

		var baseReqs, floorReqs int
		baseStatus := http.StatusForbidden
		baseBody := "submit not permitted"
		tokTr.Base = &mockTransport{
			roundTrip: func(req *http.Request) (*http.Response, error) {
				baseReqs++
				return &http.Response{
					StatusCode: baseStatus,
					Body:       io.NopCloser(strings.NewReader(baseBody)),
					Header:     make(http.Header),
					Request:    req,
				}, nil
			},
		}
		tokTr.Floor = &mockTransport{
			roundTrip: func(req *http.Request) (*http.Response, error) {
				floorReqs++
				if got := req.Header.Get("Authorization"); got != "" {
					t.Errorf("Floor saw leaked Authorization header %q", got)
				}
				return &http.Response{
					StatusCode: http.StatusOK,
					Body:       io.NopCloser(strings.NewReader("floor-ok")),
					Header:     make(http.Header),
					Request:    req,
				}, nil
			},
		}

		aclReq, _ := http.NewRequest("POST", "https://pigweed-review.googlesource.com/a/changes/1/submit", nil)
		aclResp, err := tokTr.RoundTrip(aclReq)
		if err != nil || aclResp.StatusCode != http.StatusForbidden {
			t.Fatalf("ACL 403 RoundTrip = (%v, %v), want 403 Forbidden", aclResp, err)
		}
		aclBytes, _ := io.ReadAll(aclResp.Body)
		_ = aclResp.Body.Close()
		if string(aclBytes) != "submit not permitted" || floorReqs != 0 {
			t.Fatalf("ACL 403 body=%q floorReqs=%d, want 'submit not permitted' and 0 floor calls", string(aclBytes), floorReqs)
		}

		baseReqs = 0
		baseStatus = http.StatusUnauthorized
		baseBody = "unauthorized"
		for i := range 2 {
			req, _ := http.NewRequest("GET", "https://pigweed-review.googlesource.com/a/accounts/self", nil)
			resp, err := tokTr.RoundTrip(req)
			if err != nil || resp.StatusCode != http.StatusOK {
				t.Fatalf("RoundTrip(%d) = (%v, %v), want 200 OK via Floor", i, resp, err)
			}
			_ = resp.Body.Close()
			if fbMeth, fbDesc := getLastGobCurlFallback(); fbMeth != AuthMethodGobCurl || !strings.Contains(fbDesc, "token rejected") {
				t.Errorf("RoundTrip(%d) getLastGobCurlFallback() = (%q, %q), want (gob-curl, token rejected)", i, fbMeth, fbDesc)
			}
			clearLastGobCurlFallback()
		}
		if baseReqs != 1 || floorReqs != 2 {
			t.Errorf("baseReqs=%d floorReqs=%d, want 1 (before latching Floor) and 2", baseReqs, floorReqs)
		}
	})
}

func TestCookieTransport(t *testing.T) {
	tmpDir := t.TempDir()
	cookieFile := filepath.Join(tmpDir, "cookies.txt")
	future := time.Now().Unix() + 3600

	content := fmt.Sprintf("example.com\tFALSE\t/\tTRUE\t%d\tgerrit_cookie\tsecret_value\n", future)
	if err := os.WriteFile(cookieFile, []byte(content), 0600); err != nil {
		t.Fatalf("WriteFile failed: %v", err)
	}

	rec := &mockTransportRecorder{}
	transport := &CookieTransport{
		Base:       rec,
		CookieFile: cookieFile,
	}

	req, _ := http.NewRequest("GET", "https://example.com/a/accounts/self", nil)
	_, err := transport.RoundTrip(req)
	if err != nil {
		t.Fatalf("RoundTrip failed: %v", err)
	}

	gotCookie := rec.lastReq.Header.Get("Cookie")
	if !strings.Contains(gotCookie, "gerrit_cookie=secret_value") {
		t.Errorf("Cookie header = %q, want gerrit_cookie=secret_value", gotCookie)
	}
}

func TestGobCurlTransport_MockExecution(t *testing.T) {
	tmpDir := t.TempDir()
	mockScript := filepath.Join(tmpDir, "mock-gob-curl.sh")

	scriptContent := `#!/bin/sh
printf "HTTP/1.1 200 OK\r\n"
printf "Content-Type: application/json\r\n"
printf "\r\n"
printf ")]}'\n{\"_account_id\":1000}\n"
`
	if err := os.WriteFile(mockScript, []byte(scriptContent), 0755); err != nil {
		t.Fatalf("WriteFile mock script failed: %v", err)
	}

	transport := &GobCurlTransport{Path: mockScript}
	req, _ := http.NewRequest("GET", "https://pigweed-review.googlesource.com/a/accounts/self", nil)

	resp, err := transport.RoundTrip(req)
	if err != nil {
		t.Fatalf("RoundTrip failed: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		t.Errorf("StatusCode = %d, want 200", resp.StatusCode)
	}
	if ct := resp.Header.Get("Content-Type"); ct != "application/json" {
		t.Errorf("Content-Type = %q, want application/json", ct)
	}

	body, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatalf("ReadAll body failed: %v", err)
	}
	bodyStr := string(body)
	if !strings.Contains(bodyStr, `"_account_id":1000`) {
		t.Errorf("body = %q, want account id 1000", bodyStr)
	}

	if cmd := newGobCurlCommand(t.Context(), mockScript); cmd.Cancel == nil || cmd.WaitDelay <= 0 {
		t.Errorf("newGobCurlCommand() = (Cancel: %v, WaitDelay: %v), want non-nil Cancel and WaitDelay > 0", cmd.Cancel != nil, cmd.WaitDelay)
	}
}

func TestNewAuthTransport_Methods(t *testing.T) {
	t.Run("Explicit token method", func(t *testing.T) {
		t.Setenv("GH_ISH_AUTH_METHOD", "token")
		t.Setenv("GERRIT_TOKEN", "tok123")
		tr, err := NewAuthTransport("https://pigweed-review.googlesource.com")
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		fb, ok := tr.(*fallbackTransport)
		if !ok {
			t.Fatalf("expected *fallbackTransport, got %T", tr)
		}
		if _, isToken := fb.base.(*TokenTransport); !isToken {
			t.Errorf("expected inner transport *TokenTransport, got %T", fb.base)
		}
	})

	t.Run("Explicit token method missing GERRIT_TOKEN", func(t *testing.T) {
		t.Setenv("GH_ISH_AUTH_METHOD", "token")
		t.Setenv("GERRIT_TOKEN", "")
		_, err := NewAuthTransport("https://pigweed-review.googlesource.com")
		if err == nil {
			t.Fatal("expected error when GERRIT_TOKEN is empty, got nil")
		}
		if !strings.Contains(err.Error(), "GERRIT_TOKEN is empty") {
			t.Errorf("unexpected error message: %v", err)
		}
	})

	t.Run("Explicit gob-curl missing from PATH", func(t *testing.T) {
		t.Setenv("GH_ISH_AUTH_METHOD", "gob-curl")
		origLookPath := LookPathFn
		LookPathFn = func(file string) (string, error) {
			return "", os.ErrNotExist
		}
		defer func() { LookPathFn = origLookPath }()

		_, err := NewAuthTransport("https://pigweed-review.googlesource.com")
		if err == nil {
			t.Fatal("expected error when gob-curl is missing from PATH, got nil")
		}
		if !strings.Contains(err.Error(), "gob-curl executable not found in PATH") {
			t.Errorf("unexpected error message: %v", err)
		}
	})

	t.Run("Explicit netrc method missing credentials", func(t *testing.T) {
		t.Setenv("GH_ISH_AUTH_METHOD", "netrc")
		t.Setenv("HOME", t.TempDir()) // empty home, no .netrc
		_, err := NewAuthTransport("https://nonexistent-host.googlesource.com")
		if err == nil {
			t.Fatal("expected error when netrc credentials not found, got nil")
		}
		if !strings.Contains(err.Error(), "no credentials found") {
			t.Errorf("unexpected error message: %v", err)
		}
	})

	t.Run("Explicit cookie method missing cookiefile", func(t *testing.T) {
		t.Setenv("GH_ISH_AUTH_METHOD", "cookie")
		t.Setenv("HOME", t.TempDir()) // empty home, no .gitcookies
		_, err := NewAuthTransport("https://pigweed-review.googlesource.com")
		if err == nil {
			t.Fatal("expected error when cookiefile not found, got nil")
		}
		if !strings.Contains(err.Error(), "no cookiefile found") {
			t.Errorf("unexpected error message: %v", err)
		}
	})

	t.Run("Explicit none method", func(t *testing.T) {
		t.Setenv("GH_ISH_AUTH_METHOD", "none")
		tr, err := NewAuthTransport("https://pigweed-review.googlesource.com")
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		fb := tr.(*fallbackTransport)
		if fb.base != http.DefaultTransport {
			t.Errorf("expected http.DefaultTransport, got %v", fb.base)
		}
	})

	t.Run("Unrecognized method", func(t *testing.T) {
		t.Setenv("GH_ISH_AUTH_METHOD", "bogus_method")
		_, err := NewAuthTransport("https://pigweed-review.googlesource.com")
		if err == nil {
			t.Fatal("expected error on unrecognized auth method, got nil")
		}
		if !strings.Contains(err.Error(), "unrecognized auth method") {
			t.Errorf("unexpected error message: %v", err)
		}
	})

	t.Run("Auto method with mock LookPath for gob-curl", func(t *testing.T) {
		t.Setenv("GH_ISH_AUTH_METHOD", "auto")
		t.Setenv("GERRIT_TOKEN", "")

		origLookPath := LookPathFn
		LookPathFn = func(file string) (string, error) {
			if file == "gob-curl" {
				return "/usr/bin/gob-curl", nil
			}
			return "", os.ErrNotExist
		}
		defer func() { LookPathFn = origLookPath }()

		tr, err := NewAuthTransport("https://pigweed-review.googlesource.com")
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		fb := tr.(*fallbackTransport)
		if _, isGob := fb.base.(*GobCurlTransport); !isGob {
			t.Errorf("expected inner transport *GobCurlTransport, got %T", fb.base)
		}
	})
}

func TestFallbackTransport_WarningOnDowngrade(t *testing.T) {
	mockBase := &mockTransport{
		roundTrip: func(req *http.Request) (*http.Response, error) {
			if strings.Contains(req.URL.Path, "/a/") {
				return &http.Response{
					StatusCode: http.StatusUnauthorized,
					Body:       io.NopCloser(strings.NewReader("Unauthorized")),
					Header:     make(http.Header),
					Request:    req,
				}, nil
			}
			return &http.Response{
				StatusCode: http.StatusOK,
				Body:       io.NopCloser(strings.NewReader("OK")),
				Header:     make(http.Header),
				Request:    req,
			}, nil
		},
	}

	fb := &fallbackTransport{base: mockBase}

	r, w, _ := os.Pipe()
	oldStderr := os.Stderr
	os.Stderr = w

	var buf bytes.Buffer
	readDone := make(chan struct{})
	go func() {
		_, _ = buf.ReadFrom(r)
		close(readDone)
	}()

	req, _ := http.NewRequest("GET", "https://pigweed-review.googlesource.com/a/changes/123", nil)
	resp, err := fb.RoundTrip(req)

	_ = w.Close()
	os.Stderr = oldStderr
	<-readDone
	_ = r.Close()

	if err != nil {
		t.Fatalf("unexpected RoundTrip error: %v", err)
	}
	if resp.StatusCode != http.StatusOK {
		t.Errorf("expected status 200 after downgrade, got %d", resp.StatusCode)
	}
	if !strings.Contains(buf.String(), "Warning: authenticated request") {
		t.Errorf("expected warning in stderr on downgrade, got %q", buf.String())
	}
}

func TestCookieTransport_WarningOnUnopenableFile(t *testing.T) {
	mockBase := &mockTransport{
		roundTrip: func(req *http.Request) (*http.Response, error) {
			return &http.Response{
				StatusCode: http.StatusOK,
				Body:       io.NopCloser(strings.NewReader("OK")),
				Header:     make(http.Header),
				Request:    req,
			}, nil
		},
	}

	ct := &CookieTransport{
		Base:       mockBase,
		CookieFile: "/path/to/nonexistent/cookiefile.txt",
	}

	r, w, _ := os.Pipe()
	oldStderr := os.Stderr
	os.Stderr = w

	var buf2 bytes.Buffer
	readDone2 := make(chan struct{})
	go func() {
		_, _ = buf2.ReadFrom(r)
		close(readDone2)
	}()

	req, _ := http.NewRequest("GET", "https://pigweed-review.googlesource.com/changes/123", nil)
	_, err := ct.RoundTrip(req)

	_ = w.Close()
	os.Stderr = oldStderr
	<-readDone2
	_ = r.Close()

	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if !strings.Contains(buf2.String(), "Warning: failed to open git cookiefile") {
		t.Errorf("expected warning about failed cookiefile open, got %q", buf2.String())
	}
}

type trackCloseReader struct {
	io.Reader
	closed bool
}

func (r *trackCloseReader) Close() error {
	r.closed = true
	return nil
}

func TestGobCurlTransport_ClosesRequestBodyOnError(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel() // pre-cancel context to force CommandContext/Start to fail

	body := &trackCloseReader{Reader: strings.NewReader("sample payload")}
	req, err := http.NewRequestWithContext(ctx, "POST", "https://pigweed-review.googlesource.com/a/changes/123", body)
	if err != nil {
		t.Fatalf("failed to create request: %v", err)
	}

	tr := &GobCurlTransport{}
	_, err = tr.RoundTrip(req)
	if err == nil {
		t.Fatal("expected RoundTrip to fail with canceled context")
	}

	if !body.closed {
		t.Errorf("expected req.Body to be closed on RoundTrip error, but it was not")
	}
}

func TestFindGitCookieFile_WithContext(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel() // canceled context

	mockRunner := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if ctx.Err() != nil {
				return ctx.Err()
			}
			if len(args) >= 3 && args[0] == "config" && args[1] == "--get" && args[2] == "http.cookiefile" {
				stdout.Write([]byte("/tmp/custom_cookiefile\n"))
				return nil
			}
			return fmt.Errorf("unexpected args: %v", args)
		},
	}

	// When context is canceled, ConfigGet should fail with context error and not hang
	_ = findGitCookieFile(ctx, mockRunner)

	// With active context:
	activeCtx := context.Background()
	pathActive := findGitCookieFile(activeCtx, mockRunner)
	if pathActive != "/tmp/custom_cookiefile" {
		t.Errorf("got %q, want %q", pathActive, "/tmp/custom_cookiefile")
	}
}

func TestResolveAuthMode(t *testing.T) {
	ctx := context.Background()

	origFlag := AuthModeFlag
	defer func() { AuthModeFlag = origFlag }()
	origLookPath := LookPathFn
	defer func() { LookPathFn = origLookPath }()

	t.Run("explicit flag googler", func(t *testing.T) {
		AuthModeFlag = "googler"
		mode, reason, err := ResolveAuthMode(ctx, nil)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if mode != AuthModeGoogler || !strings.Contains(reason, "--auth-mode=googler") {
			t.Errorf("got (%q, %q), want (googler, --auth-mode=googler)", mode, reason)
		}
	})

	t.Run("explicit flag invalid", func(t *testing.T) {
		AuthModeFlag = "bogus"
		_, _, err := ResolveAuthMode(ctx, nil)
		if err == nil {
			t.Fatal("expected error for invalid auth mode")
		}
		if ExitCodeFor(err) != ExitCodeAuth {
			t.Errorf("exit code = %d, want %d", ExitCodeFor(err), ExitCodeAuth)
		}
	})

	t.Run("env var community", func(t *testing.T) {
		AuthModeFlag = ""
		t.Setenv("GH_ISH_AUTH_MODE", "community")
		mode, reason, err := ResolveAuthMode(ctx, nil)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if mode != AuthModeCommunity || !strings.Contains(reason, "GH_ISH_AUTH_MODE=community") {
			t.Errorf("got (%q, %q), want (community, GH_ISH_AUTH_MODE=community)", mode, reason)
		}
	})

	t.Run("auto detects googler via gob-curl on PATH", func(t *testing.T) {
		AuthModeFlag = ""
		t.Setenv("GH_ISH_AUTH_MODE", "")
		LookPathFn = func(file string) (string, error) {
			if file == "gob-curl" {
				return "/usr/bin/gob-curl", nil
			}
			return "", os.ErrNotExist
		}
		mode, reason, err := ResolveAuthMode(ctx, nil)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if mode != AuthModeGoogler || !strings.Contains(reason, "gob-curl") {
			t.Errorf("got (%q, %q), want (googler, gob-curl)", mode, reason)
		}
	})

	t.Run("auto detects googler via @google.com git email", func(t *testing.T) {
		AuthModeFlag = ""
		t.Setenv("GH_ISH_AUTH_MODE", "")
		LookPathFn = func(file string) (string, error) {
			return "", os.ErrNotExist
		}
		mockGit := &MockGitRunner{}
		mockGit.OnCommand("config --get user.email", "keir@google.com\n")
		cfg := &Config{Git: mockGit}

		mode, reason, err := ResolveAuthMode(ctx, cfg)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if mode != AuthModeGoogler || !strings.Contains(reason, "keir@google.com") {
			t.Errorf("got (%q, %q), want (googler, keir@google.com)", mode, reason)
		}
	})

	for _, tc := range []struct {
		remote     string
		wantReason string
	}{
		{"sso://pigweed/pigweed/pigweed\n", "sso:// git remote"},
		{"rpc://pigweed/pigweed/pigweed\n", "rpc:// git remote"},
	} {
		t.Run("auto detects googler via "+tc.wantReason, func(t *testing.T) {
			AuthModeFlag = ""
			t.Setenv("GH_ISH_AUTH_MODE", "")
			LookPathFn = func(file string) (string, error) {
				return "", os.ErrNotExist
			}
			mockGit := &MockGitRunner{}
			mockGit.OnCommand("config --get user.email", "contributor@example.com\n").
				OnCommand("config --get remote.origin.url", tc.remote)
			cfg := &Config{Git: mockGit}

			mode, reason, err := ResolveAuthMode(ctx, cfg)
			if err != nil {
				t.Fatalf("unexpected error: %v", err)
			}
			if mode != AuthModeGoogler || !strings.Contains(reason, tc.wantReason) {
				t.Errorf("got (%q, %q), want (googler, %s)", mode, reason, tc.wantReason)
			}
		})
	}

	t.Run("auto detects community when no corp signals", func(t *testing.T) {
		AuthModeFlag = ""
		t.Setenv("GH_ISH_AUTH_MODE", "")
		LookPathFn = func(file string) (string, error) {
			return "", os.ErrNotExist
		}
		mockGit := &MockGitRunner{}
		mockGit.OnCommand("config --get user.email", "contributor@example.com\n")
		cfg := &Config{Git: mockGit}

		mode, _, err := ResolveAuthMode(ctx, cfg)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if mode != AuthModeCommunity {
			t.Errorf("got %q, want community", mode)
		}
	})
}

func TestNewAuthTransportContext_GooglerModeRefusesSilentAnonFallback(t *testing.T) {
	ctx := context.Background()
	origFlag := AuthModeFlag
	AuthModeFlag = "googler"
	defer func() { AuthModeFlag = origFlag }()

	origLookPath := LookPathFn
	LookPathFn = func(file string) (string, error) {
		return "", os.ErrNotExist
	}
	defer func() { LookPathFn = origLookPath }()

	t.Setenv("GH_ISH_AUTH_METHOD", "auto")
	t.Setenv("GERRIT_TOKEN", "")
	t.Setenv("HOME", t.TempDir())

	mockGit := &MockGitRunner{}
	SetMockGit(t, mockGit)

	_, err := NewAuthTransportContext(ctx, "https://pigweed-review.googlesource.com")
	if err == nil {
		t.Fatal("expected error in googler mode when no credentials exist, got nil")
	}
	if ExitCodeFor(err) != ExitCodeAuth {
		t.Errorf("exit code = %d, want %d (%v)", ExitCodeFor(err), ExitCodeAuth, err)
	}
	if !strings.Contains(err.Error(), "gcert") || !strings.Contains(err.Error(), "gh auth status") {
		t.Errorf("expected error to contain gcert and gh auth status remediation, got:\n%v", err)
	}
}

func TestFallbackTransport_DisallowAnonFallbackInGooglerMode(t *testing.T) {
	requests := 0
	mockBase := &mockTransport{
		roundTrip: func(req *http.Request) (*http.Response, error) {
			requests++
			return &http.Response{
				StatusCode: http.StatusUnauthorized,
				Body:       io.NopCloser(strings.NewReader("Unauthorized")),
				Header:     make(http.Header),
				Request:    req,
			}, nil
		},
	}

	fb := &fallbackTransport{
		base:                 mockBase,
		disallowAnonFallback: true,
	}

	req, _ := http.NewRequest("GET", "https://pigweed-review.googlesource.com/a/changes/123", nil)
	resp, err := fb.RoundTrip(req)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if resp.StatusCode != http.StatusUnauthorized {
		t.Errorf("StatusCode = %d, want 401 (no downgrade)", resp.StatusCode)
	}
	if requests != 1 {
		t.Errorf("expected exactly 1 request (no retry on unauthenticated path), got %d", requests)
	}
}

func TestGobCurlTransport_FailureReturnsExitCodeAuth(t *testing.T) {
	tmpDir := t.TempDir()
	mockScript := filepath.Join(tmpDir, "mock-gob-curl-fail.sh")
	var resolverCalled, fallbackMarked bool
	origGerritResolver := GerritTokenResolver
	t.Cleanup(func() { GerritTokenResolver = origGerritResolver })
	GerritTokenResolver = func(ctx context.Context) (string, string, error) {
		resolverCalled = true
		return "should-not-be-used", "luci-auth", nil
	}

	req, _ := http.NewRequest("GET", "https://pigweed-review.googlesource.com/a/accounts/self", nil)
	for _, tc := range []struct {
		name, stderr           string
		autoFallback, wantAuth bool
		wantSubstr             string
	}{
		{"LOAS expired returns ExitCodeAuth", "LOAS certificate expired", false, true, "gcert"},
		{"scheme error is non-auth and skips fallback", "Scheme must be http(s)", true, false, "Scheme must be http(s)"},
		{"TLS cert error is non-auth and skips fallback", "SSL certificate problem: unable to get local issuer certificate", true, false, "SSL certificate problem"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if err := os.WriteFile(mockScript, []byte("#!/bin/sh\nprintf '"+tc.stderr+"\\n' >&2\nexit 1\n"), 0755); err != nil {
				t.Fatalf("write mock script: %v", err)
			}
			tr := &GobCurlTransport{Path: mockScript, AutoFallback: tc.autoFallback, Base: &mockTransportRecorder{}}
			_, roundTripErr := tr.RoundTrip(req)
			if roundTripErr == nil {
				t.Fatal("RoundTrip succeeded, want error")
			}
			if gotAuth := (ExitCodeFor(roundTripErr) == ExitCodeAuth); gotAuth != tc.wantAuth {
				t.Errorf("ExitCodeFor(err) == ExitCodeAuth is %v, want %v (err: %v)", gotAuth, tc.wantAuth, roundTripErr)
			}
			if !strings.Contains(roundTripErr.Error(), tc.wantSubstr) {
				t.Errorf("expected %q in error, got: %v", tc.wantSubstr, roundTripErr)
			}
			fallbackMarked = fallbackMarked || tr.gobCurlFailed
		})
	}
	if resolverCalled {
		t.Error("unexpected GerritTokenResolver fallback call")
	}
	if fallbackMarked {
		t.Error("unexpected gobCurlFailed latch on non-auth failure")
	}
}

func TestFindLuciAuthBinary_WorktreeCommonGitDir(t *testing.T) {
	origLookPath := LookPathFn
	defer func() { LookPathFn = origLookPath }()

	t.Setenv("PW_ENVIRONMENT_ROOT", "")
	t.Setenv("BUILD_WORKSPACE_DIRECTORY", "")
	t.Setenv(scriptDirEnvVar, "")

	mainRepoRoot := filepath.Join(t.TempDir(), "pigweed")
	expectedLuciAuth := filepath.Join(mainRepoRoot, "environment", "cipd", "packages", "luci", "luci-auth")

	LookPathFn = func(file string) (string, error) {
		if file == expectedLuciAuth {
			return expectedLuciAuth, nil
		}
		return "", os.ErrNotExist
	}

	mockGit := &MockGitRunner{}
	mockGit.OnCommand("rev-parse --git-common-dir", filepath.Join(mainRepoRoot, ".git")+"\n")
	ctx := context.WithValue(context.Background(), configKey, &Config{Git: mockGit})

	got := findLuciAuthBinary(ctx)
	if got != expectedLuciAuth {
		t.Errorf("findLuciAuthBinary() = %q, want %q", got, expectedLuciAuth)
	}
}

func TestFindLuciAuthBinary_ScriptDirFallback(t *testing.T) {
	origLookPath := LookPathFn
	defer func() { LookPathFn = origLookPath }()

	otherRepoDir := filepath.Join(t.TempDir(), "other-repo")
	pwScriptDir := filepath.Join(t.TempDir(), "pigweed-checkout")
	expectedLuciAuth := filepath.Join(pwScriptDir, "environment", "cipd", "packages", "luci", "luci-auth")

	t.Setenv("PW_ENVIRONMENT_ROOT", "")
	t.Setenv("BUILD_WORKSPACE_DIRECTORY", otherRepoDir)
	t.Setenv(scriptDirEnvVar, pwScriptDir)

	LookPathFn = func(file string) (string, error) {
		if file == expectedLuciAuth {
			return expectedLuciAuth, nil
		}
		return "", os.ErrNotExist
	}

	mockGit := &MockGitRunner{}
	ctx := context.WithValue(context.Background(), configKey, &Config{Git: mockGit})

	got := findLuciAuthBinary(ctx)
	if got != expectedLuciAuth {
		t.Errorf("findLuciAuthBinary() = %q, want %q", got, expectedLuciAuth)
	}
}

func TestLUCIAuthTransport(t *testing.T) {
	origResolver := LUCITokenResolver
	defer func() { LUCITokenResolver = origResolver }()
	origFlag := AuthModeFlag
	defer func() { AuthModeFlag = origFlag }()

	t.Run("attaches Bearer token when available", func(t *testing.T) {
		AuthModeFlag = "googler"
		LUCITokenResolver = func(ctx context.Context) (string, string, error) {
			return "luci-secret-token", "luci-auth token", nil
		}
		rec := &mockTransportRecorder{}
		tr := &LUCIAuthTransport{Base: rec}

		req, _ := http.NewRequest("POST", "https://cr-buildbucket.appspot.com/prpc/buildbucket.v2.Builds/SearchBuilds", nil)
		_, err := tr.RoundTrip(req)
		if err != nil {
			t.Fatalf("RoundTrip failed: %v", err)
		}
		if got := rec.lastReq.Header.Get("Authorization"); got != "Bearer luci-secret-token" {
			t.Errorf("Authorization = %q, want 'Bearer luci-secret-token'", got)
		}
	})

	t.Run("fails with ExitCodeAuth in googler mode when token missing", func(t *testing.T) {
		AuthModeFlag = "googler"
		LUCITokenResolver = func(ctx context.Context) (string, string, error) {
			return "", "", fmt.Errorf("interactive login required")
		}
		rec := &mockTransportRecorder{}
		tr := &LUCIAuthTransport{Base: rec}

		req, _ := http.NewRequest("POST", "https://cr-buildbucket.appspot.com/prpc/buildbucket.v2.Builds/SearchBuilds", nil)
		_, err := tr.RoundTrip(req)
		if err == nil {
			t.Fatal("expected error in googler mode when LUCI token is missing")
		}
		if ExitCodeFor(err) != ExitCodeAuth {
			t.Errorf("ExitCodeFor(err) = %d, want %d", ExitCodeFor(err), ExitCodeAuth)
		}
		if rec.lastReq != nil {
			t.Errorf("expected no HTTP request to be sent when googler LUCI check fails")
		}
	})

	t.Run("proceeds unauthenticated in community mode when token missing", func(t *testing.T) {
		AuthModeFlag = "community"
		LUCITokenResolver = func(ctx context.Context) (string, string, error) {
			return "", "", fmt.Errorf("luci-auth not installed")
		}
		rec := &mockTransportRecorder{}
		tr := &LUCIAuthTransport{Base: rec}

		req, _ := http.NewRequest("POST", "https://cr-buildbucket.appspot.com/prpc/buildbucket.v2.Builds/SearchBuilds", nil)
		_, err := tr.RoundTrip(req)
		if err != nil {
			t.Fatalf("expected community mode to proceed unauthenticated, got error: %v", err)
		}
		if got := rec.lastReq.Header.Get("Authorization"); got != "" {
			t.Errorf("expected empty Authorization header in community mode without token, got %q", got)
		}
	})
}

func TestDefaultGerritToken_CachesJSONExpiry(t *testing.T) {
	t.Setenv("GERRIT_TOKEN", "")
	ResetAuthTokenCaches()
	t.Cleanup(ResetAuthTokenCaches)

	origLookPath := LookPathFn
	LookPathFn = func(file string) (string, error) {
		if file == "luci-auth" {
			return "/usr/bin/luci-auth", nil
		}
		return "", os.ErrNotExist
	}
	t.Cleanup(func() { LookPathFn = origLookPath })

	wantExpiry := time.Now().Add(45 * time.Minute).Unix()
	var runnerCalls int
	origRunner := OAuthTokenCommandRunner
	OAuthTokenCommandRunner = func(ctx context.Context, name string, args ...string) ([]byte, error) {
		runnerCalls++
		return fmt.Appendf(nil, `{"token":"gerrit-json-token","expiry":%d}`, wantExpiry), nil
	}
	t.Cleanup(func() { OAuthTokenCommandRunner = origRunner })

	for range 2 {
		tok, src, err := DefaultGerritToken(t.Context())
		if err != nil {
			t.Fatalf("DefaultGerritToken() error = %v", err)
		}
		if tok != "gerrit-json-token" || src != "luci-auth" {
			t.Errorf("DefaultGerritToken() = (%q, %q), want (gerrit-json-token, luci-auth)", tok, src)
		}
	}
	if runnerCalls != 1 {
		t.Errorf("OAuthTokenCommandRunner calls = %d, want 1", runnerCalls)
	}
	gerritTokenMu.Lock()
	gotExpiry := cachedGerritTokenExpiry
	gerritTokenMu.Unlock()
	if want := time.Unix(wantExpiry, 0).Add(-60 * time.Second); !gotExpiry.Equal(want) {
		t.Errorf("cachedGerritTokenExpiry = %v, want %v", gotExpiry, want)
	}
}

func TestGobCurlTransport_AutoFallbackToLuciAuth(t *testing.T) {
	tmpDir := t.TempDir()
	counterFile := filepath.Join(tmpDir, "gob-curl-count")
	mockScript := filepath.Join(tmpDir, "mock-gob-curl-expired.sh")
	scriptContent := fmt.Sprintf("#!/bin/sh\necho x >> %q\nprintf 'sso: credentials expired. Try running gcert first\\n' >&2\nexit 1\n", counterFile)
	if err := os.WriteFile(mockScript, []byte(scriptContent), 0755); err != nil {
		t.Fatalf("WriteFile failed: %v", err)
	}

	origGerritResolver := GerritTokenResolver
	GerritTokenResolver = func(ctx context.Context) (string, string, error) {
		return "luci-gerrit-oauth-token", "luci-auth", nil
	}
	defer func() { GerritTokenResolver = origGerritResolver }()

	var gotAuthHeader string
	var gotBody string
	mockBase := &mockTransport{
		roundTrip: func(req *http.Request) (*http.Response, error) {
			gotAuthHeader = req.Header.Get("Authorization")
			if req.Body != nil {
				b, _ := io.ReadAll(req.Body)
				gotBody = string(b)
			}
			return &http.Response{
				StatusCode: http.StatusOK,
				Body:       io.NopCloser(strings.NewReader(")]}'\n{\"_account_id\":1001}\n")),
				Header:     make(http.Header),
				Request:    req,
			}, nil
		},
	}

	tr := &GobCurlTransport{
		Path:         mockScript,
		AutoFallback: true,
		Base:         mockBase,
	}

	// 1. First request (POST with body) should fail gob-curl, preserve body, and succeed via luci-auth
	req1, _ := http.NewRequest("POST", "https://pigweed-review.googlesource.com/a/changes/123/revisions/current/review", strings.NewReader(`{"labels":{"Code-Review":1}}`))
	resp1, err := tr.RoundTrip(req1)
	if err != nil {
		t.Fatalf("expected AutoFallback to succeed via luci-auth, got error: %v", err)
	}
	_ = resp1.Body.Close()

	if gotAuthHeader != "Bearer luci-gerrit-oauth-token" {
		t.Errorf("Authorization = %q, want 'Bearer luci-gerrit-oauth-token'", gotAuthHeader)
	}
	if gotBody != `{"labels":{"Code-Review":1}}` {
		t.Errorf("POST body after fallback = %q, want JSON payload", gotBody)
	}

	method, desc := tr.ActiveMethod()
	if method != AuthMethodToken || !strings.Contains(desc, "luci-auth") || !strings.Contains(desc, "gob-curl") {
		t.Errorf("ActiveMethod() = (%q, %q), want (token, luci-auth fallback description)", method, desc)
	}

	// 2. Second request in the same process should use cached fallback without spawning gob-curl again
	req2, _ := http.NewRequest("GET", "https://pigweed-review.googlesource.com/a/accounts/self", nil)
	resp2, err := tr.RoundTrip(req2)
	if err != nil {
		t.Fatalf("second request failed: %v", err)
	}
	_ = resp2.Body.Close()

	countBytes, err := os.ReadFile(counterFile)
	if err != nil {
		t.Fatalf("failed to read counter file: %v", err)
	}
	if lines := strings.Split(strings.TrimSpace(string(countBytes)), "\n"); len(lines) != 1 {
		t.Errorf("expected gob-curl to be invoked only once before caching fallback, got %d invocations", len(lines))
	}
}

func TestGobCurlTransport_AutoFallbackToCookieWhenLuciAuthUnavailable(t *testing.T) {
	tmpDir := t.TempDir()
	mockScript := filepath.Join(tmpDir, "mock-gob-curl-expired.sh")
	if err := os.WriteFile(mockScript, []byte("#!/bin/sh\nprintf 'sso: credentials expired\\n' >&2\nexit 1\n"), 0755); err != nil {
		t.Fatalf("WriteFile failed: %v", err)
	}

	cookieFile := filepath.Join(tmpDir, ".gitcookies")
	future := time.Now().Unix() + 3600
	cookieData := fmt.Sprintf(".googlesource.com\tTRUE\t/\tTRUE\t%d\to\tgitcookie-secret-val\n", future)
	if err := os.WriteFile(cookieFile, []byte(cookieData), 0600); err != nil {
		t.Fatalf("WriteFile cookie failed: %v", err)
	}
	t.Setenv("HOME", tmpDir)

	origGerritResolver := GerritTokenResolver
	GerritTokenResolver = func(ctx context.Context) (string, string, error) {
		return "", "", fmt.Errorf("no luci-auth token")
	}
	defer func() { GerritTokenResolver = origGerritResolver }()

	var gotCookie string
	mockBase := &mockTransport{
		roundTrip: func(req *http.Request) (*http.Response, error) {
			gotCookie = req.Header.Get("Cookie")
			return &http.Response{
				StatusCode: http.StatusOK,
				Body:       io.NopCloser(strings.NewReader(")]}'\n{}\n")),
				Header:     make(http.Header),
				Request:    req,
			}, nil
		},
	}

	tr := &GobCurlTransport{
		Path:         mockScript,
		AutoFallback: true,
		Base:         mockBase,
	}

	req, _ := http.NewRequest("GET", "https://pigweed-review.googlesource.com/a/accounts/self", nil)
	resp, err := tr.RoundTrip(req)
	if err != nil {
		t.Fatalf("expected AutoFallback to cookie to succeed, got error: %v", err)
	}
	_ = resp.Body.Close()

	if !strings.Contains(gotCookie, "o=gitcookie-secret-val") {
		t.Errorf("Cookie header = %q, want o=gitcookie-secret-val", gotCookie)
	}
	method, desc := tr.ActiveMethod()
	if method != AuthMethodCookie || !strings.Contains(desc, "cookie") {
		t.Errorf("ActiveMethod() = (%q, %q), want cookie fallback", method, desc)
	}
}

func TestNewAuthTransportContext_AutoFallsBackToLuciAuthWhenGobCurlAndCookiesMissing(t *testing.T) {
	ctx := context.Background()
	origFlag := AuthModeFlag
	AuthModeFlag = "googler"
	defer func() { AuthModeFlag = origFlag }()

	origLookPath := LookPathFn
	LookPathFn = func(file string) (string, error) {
		return "", os.ErrNotExist
	}
	defer func() { LookPathFn = origLookPath }()

	origGerritResolver := GerritTokenResolver
	GerritTokenResolver = func(ctx context.Context) (string, string, error) {
		return "luci-token-xyz", "luci-auth", nil
	}
	defer func() { GerritTokenResolver = origGerritResolver }()

	t.Setenv("GH_ISH_AUTH_METHOD", "auto")
	t.Setenv("GERRIT_TOKEN", "")
	t.Setenv("HOME", t.TempDir())
	SetMockGit(t, &MockGitRunner{})

	tr, err := NewAuthTransportContext(ctx, "https://pigweed-review.googlesource.com")
	if err != nil {
		t.Fatalf("expected NewAuthTransportContext to succeed via luci-auth fallback, got error: %v", err)
	}
	fb, ok := tr.(*fallbackTransport)
	if !ok {
		t.Fatalf("expected *fallbackTransport, got %T", tr)
	}
	tokTr, ok := fb.base.(*TokenTransport)
	if !ok || tokTr.Token != "luci-token-xyz" {
		t.Errorf("expected *TokenTransport with luci-token-xyz, got %#v", fb.base)
	}

	method, desc := DescribeGerritAuthMethod(ctx, "https://pigweed-review.googlesource.com", AuthModeGoogler)
	if method != AuthMethodToken || !strings.Contains(desc, "luci-auth") {
		t.Errorf("DescribeGerritAuthMethod = (%q, %q), want (token, token (luci-auth))", method, desc)
	}
}

func TestGobCurlTransport_BodySizeLimit(t *testing.T) {
	tr := &GobCurlTransport{Path: "gob-curl"}
	oversized := bytes.NewReader(make([]byte, maxGerritRequestBodyBytes+1))
	req, _ := http.NewRequest("POST", "https://pigweed-review.googlesource.com/a/changes/123/revisions/current/review", oversized)
	_, err := tr.RoundTrip(req)
	if err == nil || !strings.Contains(err.Error(), "exceeds maximum allowed size") {
		t.Fatalf("expected oversized request body error, got: %v", err)
	}
}

func TestGobCurlTransport_ConcurrentFallbackDiscovery(t *testing.T) {
	tmpDir := t.TempDir()
	counterFile := filepath.Join(tmpDir, "gob-curl-count")
	mockScript := filepath.Join(tmpDir, "mock-gob-curl-expired.sh")
	scriptContent := fmt.Sprintf("#!/bin/sh\necho x >> %q\nprintf 'sso: credentials expired\\n' >&2\nexit 1\n", counterFile)
	if err := os.WriteFile(mockScript, []byte(scriptContent), 0755); err != nil {
		t.Fatalf("WriteFile failed: %v", err)
	}

	var resolverCalls atomic.Int32
	origGerritResolver := GerritTokenResolver
	GerritTokenResolver = func(ctx context.Context) (string, string, error) {
		resolverCalls.Add(1)
		return "luci-concurrent-token", "luci-auth", nil
	}
	defer func() { GerritTokenResolver = origGerritResolver }()

	mockBase := &mockTransport{
		roundTrip: func(req *http.Request) (*http.Response, error) {
			return &http.Response{
				StatusCode: http.StatusOK,
				Body:       io.NopCloser(strings.NewReader(")]}'\n{}\n")),
				Header:     make(http.Header),
				Request:    req,
			}, nil
		},
	}

	tr := &GobCurlTransport{
		Path:         mockScript,
		AutoFallback: true,
		Base:         mockBase,
	}

	start := make(chan struct{})
	var wg sync.WaitGroup
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			<-start
			req, _ := http.NewRequest("GET", "https://pigweed-review.googlesource.com/a/accounts/self", nil)
			resp, err := tr.RoundTrip(req)
			if err != nil {
				t.Errorf("concurrent RoundTrip failed: %v", err)
				return
			}
			_ = resp.Body.Close()
		}()
	}
	close(start)
	wg.Wait()

	if got := resolverCalls.Load(); got != 1 {
		t.Errorf("expected GerritTokenResolver to be called exactly once during concurrent fallback discovery, got %d", got)
	}
	countBytes, err := os.ReadFile(counterFile)
	if err != nil {
		t.Fatalf("failed to read counter file: %v", err)
	}
	if lines := strings.Split(strings.TrimSpace(string(countBytes)), "\n"); len(lines) != 1 {
		t.Errorf("expected gob-curl to be invoked only once during concurrent fallback discovery, got %d invocations", len(lines))
	}
}

func TestGobCurlTransport_FallbackReturns400WithoutContinuing(t *testing.T) {
	tmpDir := t.TempDir()
	mockScript := filepath.Join(tmpDir, "mock-gob-curl-expired.sh")
	if err := os.WriteFile(mockScript, []byte("#!/bin/sh\nprintf 'sso: credentials expired\\n' >&2\nexit 1\n"), 0755); err != nil {
		t.Fatalf("WriteFile failed: %v", err)
	}

	origGerritResolver := GerritTokenResolver
	GerritTokenResolver = func(ctx context.Context) (string, string, error) {
		return "luci-gerrit-token", "luci-auth", nil
	}
	defer func() { GerritTokenResolver = origGerritResolver }()

	mockBase := &mockTransport{
		roundTrip: func(req *http.Request) (*http.Response, error) {
			return &http.Response{
				StatusCode: http.StatusBadRequest,
				Body:       io.NopCloser(strings.NewReader("invalid topic name\n")),
				Header:     make(http.Header),
				Request:    req,
			}, nil
		},
	}

	tr := &GobCurlTransport{
		Path:         mockScript,
		AutoFallback: true,
		Base:         mockBase,
	}

	req, _ := http.NewRequest("PUT", "https://pigweed-review.googlesource.com/a/changes/123/topic", strings.NewReader(`{"topic":"bad"}`))
	resp, err := tr.RoundTrip(req)
	if err != nil {
		t.Fatalf("expected HTTP 400 response to be returned rather than gobCurlErr, got err: %v", err)
	}
	defer func() { _ = resp.Body.Close() }()
	if resp.StatusCode != http.StatusBadRequest {
		t.Errorf("resp.StatusCode = %d, want %d", resp.StatusCode, http.StatusBadRequest)
	}
}

func TestAuthEnvVars_TrimSpace(t *testing.T) {
	ctx := context.Background()
	t.Setenv("GH_ISH_AUTH_METHOD", "  token  ")
	t.Setenv("GERRIT_TOKEN", "  my-trimmed-token  ")

	method, _ := DescribeGerritAuthMethod(ctx, "https://pigweed-review.googlesource.com", AuthModeGoogler)
	if method != AuthMethodToken {
		t.Errorf("DescribeGerritAuthMethod method = %q, want %q", method, AuthMethodToken)
	}

	tr, err := NewAuthTransportContext(ctx, "https://pigweed-review.googlesource.com")
	if err != nil {
		t.Fatalf("NewAuthTransportContext failed: %v", err)
	}
	fb, ok := tr.(*fallbackTransport)
	if !ok {
		t.Fatalf("expected *fallbackTransport, got %T", tr)
	}
	tokTr, ok := fb.base.(*TokenTransport)
	if !ok || tokTr.Token != "my-trimmed-token" {
		t.Errorf("TokenTransport.Token = %q, want 'my-trimmed-token'", tokTr.Token)
	}
}

func TestFindLuciAuthBinary_SkipsGitRevParseWhenFoundInScriptDir(t *testing.T) {
	origLookPath := LookPathFn
	defer func() { LookPathFn = origLookPath }()

	pwScriptDir := filepath.Join(t.TempDir(), "pigweed-checkout")
	expectedLuciAuth := filepath.Join(pwScriptDir, "environment", "cipd", "packages", "luci", "luci-auth")

	t.Setenv("PW_ENVIRONMENT_ROOT", "")
	t.Setenv("BUILD_WORKSPACE_DIRECTORY", "")
	t.Setenv(scriptDirEnvVar, pwScriptDir)

	LookPathFn = func(file string) (string, error) {
		if file == expectedLuciAuth {
			return expectedLuciAuth, nil
		}
		return "", os.ErrNotExist
	}

	var gitCalled bool
	mockGit := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			gitCalled = true
			return fmt.Errorf("unexpected git call: %v", args)
		},
	}
	ctx := context.WithValue(t.Context(), configKey, &Config{Git: mockGit})

	got := findLuciAuthBinary(ctx)
	if got != expectedLuciAuth {
		t.Errorf("findLuciAuthBinary() = %q, want %q", got, expectedLuciAuth)
	}
	if gitCalled {
		t.Errorf("expected findLuciAuthBinary to return early without running git rev-parse --git-common-dir")
	}
}

func TestLUCIAuthTransport_MemoizesAuthModeAcrossRequests(t *testing.T) {
	ResetAuthTokenCaches()
	t.Cleanup(ResetAuthTokenCaches)

	origFlag := AuthModeFlag
	AuthModeFlag = ""
	defer func() { AuthModeFlag = origFlag }()
	t.Setenv("GH_ISH_AUTH_MODE", "")
	t.Setenv("GH_ISH_AUTH_METHOD", "")

	origLookPath := LookPathFn
	LookPathFn = func(file string) (string, error) {
		return "", os.ErrNotExist
	}
	defer func() { LookPathFn = origLookPath }()

	origResolver := LUCITokenResolver
	LUCITokenResolver = func(ctx context.Context) (string, string, error) {
		return "luci-tok", "luci-auth", nil
	}
	defer func() { LUCITokenResolver = origResolver }()

	var gitConfigCalls int
	mockGit := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			gitConfigCalls++
			if len(args) >= 3 && args[0] == "config" && args[1] == "--get" && args[2] == "user.email" {
				_, _ = stdout.Write([]byte("dev@google.com\n"))
				return nil
			}
			return fmt.Errorf("not set")
		},
	}
	cfg := &Config{Git: mockGit}
	rec := &mockTransportRecorder{}
	tr := &LUCIAuthTransport{Base: rec, Cfg: cfg}

	for i := range 3 {
		req, _ := http.NewRequest("POST", "https://cr-buildbucket.appspot.com/prpc/buildbucket.v2.Builds/GetBuild", nil)
		if _, err := tr.RoundTrip(req); err != nil {
			t.Fatalf("RoundTrip(%d) failed: %v", i, err)
		}
	}
	// Only the first request should query git config (ghish.authmode + user.email = 2 calls).
	if gitConfigCalls > 2 {
		t.Errorf("gitConfigCalls across 3 LUCI requests = %d, want <= 2", gitConfigCalls)
	}
}

func TestOAuthTokenCommand_AutoConfiguresSSOCredHelper(t *testing.T) {
	origLookPath := LookPathFn
	t.Cleanup(func() { LookPathFn = origLookPath })

	tests := []struct {
		name           string
		hasHelper      bool
		lookPathErr    error
		envSet         bool
		envVal         string
		bin            string
		args           []string
		wantInjectedTo string
	}{
		{
			name:           "injects LUCI_AUTH_CREDENTIAL_HELPER when unset and helper exists",
			hasHelper:      true,
			bin:            "luci-auth",
			args:           []string{"token"},
			wantInjectedTo: ssoCredHelperPath,
		},
		{
			name:           "injects for resolved absolute luci-auth path",
			hasHelper:      true,
			bin:            "/opt/cipd/packages/luci/luci-auth",
			args:           []string{"token"},
			wantInjectedTo: ssoCredHelperPath,
		},
		{
			name:      "preserves existing LUCI_AUTH_CREDENTIAL_HELPER when already set",
			hasHelper: true,
			envSet:    true,
			envVal:    "/custom/cred-helper",
			bin:       "luci-auth",
			args:      []string{"token"},
		},
		{
			name:      "respects explicit empty LUCI_AUTH_CREDENTIAL_HELPER opt-out",
			hasHelper: true,
			envSet:    true,
			envVal:    "",
			bin:       "luci-auth",
			args:      []string{"token"},
		},
		{
			name:      "does not inject when helper binary is missing on disk",
			hasHelper: false,
			bin:       "luci-auth",
			args:      []string{"token"},
		},
		{
			name:        "does not inject when LookPath returns permission error",
			lookPathErr: os.ErrPermission,
			bin:         "luci-auth",
			args:        []string{"token"},
		},
		{
			name:      "does not inject for non-luci-auth commands",
			hasHelper: true,
			bin:       "gcloud",
			args:      []string{"auth", "print-access-token"},
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			LookPathFn = func(file string) (string, error) {
				if tc.lookPathErr != nil {
					return "", tc.lookPathErr
				}
				if tc.hasHelper && file == ssoCredHelperPath {
					return ssoCredHelperPath, nil
				}
				return "", os.ErrNotExist
			}
			t.Setenv("GHISH_PARENT_ENV_CANARY", "preserved")
			t.Setenv("LUCI_AUTH_CREDENTIAL_HELPER", tc.envVal)
			if !tc.envSet {
				_ = os.Unsetenv("LUCI_AUTH_CREDENTIAL_HELPER")
			}

			cmd := newOAuthTokenCommand(t.Context(), tc.bin, tc.args...)
			if tc.wantInjectedTo == "" {
				if cmd.Env != nil {
					t.Errorf("cmd.Env = %v, want nil", cmd.Env)
				}
				if tc.envSet && !slices.Contains(cmd.Environ(), "LUCI_AUTH_CREDENTIAL_HELPER="+tc.envVal) {
					t.Errorf("cmd.Environ() missing preserved LUCI_AUTH_CREDENTIAL_HELPER=%q", tc.envVal)
				}
				return
			}
			wantEntry := "LUCI_AUTH_CREDENTIAL_HELPER=" + tc.wantInjectedTo
			if !slices.Contains(cmd.Env, wantEntry) {
				t.Errorf("cmd.Env missing %q: %v", wantEntry, cmd.Env)
			}
			if !slices.Contains(cmd.Env, "GHISH_PARENT_ENV_CANARY=preserved") {
				t.Errorf("cmd.Env lost parent environment variables: %v", cmd.Env)
			}
		})
	}
}
