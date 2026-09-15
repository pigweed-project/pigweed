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
	"strings"
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
