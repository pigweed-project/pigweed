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
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
	"time"
)

func TestTimestamp_JSONRoundTrip(t *testing.T) {
	raw := `"2026-03-15 14:22:33.123456789"`
	var ts Timestamp
	if err := json.Unmarshal([]byte(raw), &ts); err != nil {
		t.Fatalf("UnmarshalJSON failed: %v", err)
	}
	want := time.Date(2026, 3, 15, 14, 22, 33, 123456789, time.UTC)
	if !ts.Time.Equal(want) {
		t.Errorf("got %v, want %v", ts.Time, want)
	}

	marshaled, err := json.Marshal(ts)
	if err != nil {
		t.Fatalf("MarshalJSON failed: %v", err)
	}
	if string(marshaled) != raw {
		t.Errorf("MarshalJSON = %s, want %s", string(marshaled), raw)
	}

	for _, shortFrac := range []string{`"2026-03-15 14:22:33.123"`, `"2026-03-15 14:22:33.123456"`} {
		var parsed Timestamp
		if err := json.Unmarshal([]byte(shortFrac), &parsed); err != nil || parsed.Time.IsZero() {
			t.Errorf("UnmarshalJSON(%s) = (%v, %v), want non-zero parsed time", shortFrac, parsed.Time, err)
		}
	}

	var empty Timestamp
	if err := json.Unmarshal([]byte(`""`), &empty); err != nil {
		t.Fatalf("UnmarshalJSON empty failed: %v", err)
	}
	if !empty.Time.IsZero() {
		t.Errorf("expected zero time for empty string, got %v", empty.Time)
	}
}

func TestStripXSSIPrefix(t *testing.T) {
	cases := []struct {
		in   string
		want string
	}{
		{in: ")]}'\n{\"ok\":true}", want: "{\"ok\":true}"},
		{in: ")]}'\r\n{\"ok\":true}", want: "{\"ok\":true}"},
		{in: ")]}'{\"ok\":true}", want: "{\"ok\":true}"},
		{in: "{\"ok\":true}", want: "{\"ok\":true}"},
	}
	for _, tc := range cases {
		got := string(stripXSSIPrefix([]byte(tc.in)))
		if got != tc.want {
			t.Errorf("stripXSSIPrefix(%q) = %q, want %q", tc.in, got, tc.want)
		}
	}
}

func TestGerritClient_ErrorAndStatusDetection(t *testing.T) {
	for _, code := range []int{
		http.StatusBadRequest,
		http.StatusUnauthorized,
		http.StatusForbidden,
		http.StatusNotFound,
		http.StatusConflict,
		http.StatusTooManyRequests,
		http.StatusInternalServerError,
		http.StatusServiceUnavailable,
	} {
		server := NewMockGerritServer(t)
		server.OnString("POST", "/changes/12345/submit", code, "text/plain", "status error body\n")
		client := NewGerritClientWithHTTP(server.URL, server.Server.Client(), GerritAuth{})
		_, err := client.SubmitChange(t.Context(), "12345")
		if !IsGerritHTTPStatus(err, code) {
			t.Errorf("IsGerritHTTPStatus(%d) = false for %v", code, err)
		}
		if IsGerritHTTPStatus(err, http.StatusTeapot) {
			t.Errorf("IsGerritHTTPStatus(418) = true for %d", code)
		}
	}
	for _, nonGerritErr := range []error{nil, context.Canceled, context.DeadlineExceeded, io.EOF} {
		if IsGerritHTTPStatus(nonGerritErr, http.StatusNotFound) {
			t.Errorf("IsGerritHTTPStatus(%v, 404) = true, want false", nonGerritErr)
		}
	}
	nilURLResp := &http.Response{StatusCode: http.StatusBadRequest, Status: "400 Bad Request"}
	if apiErr := newGerritAPIError("GET", nil, nilURLResp, []byte("bad")); apiErr.URL != "" || apiErr.Body != "bad" {
		t.Errorf("newGerritAPIError with nil URL = %+v, want empty URL and body \"bad\"", apiErr)
	}
}

func TestGerritClient_ContextCancellation(t *testing.T) {
	cancelCtx, cancel := context.WithCancel(t.Context())
	cancel()
	client := NewGerritClientWithHTTP("https://pigweed-review.googlesource.com/a",
		&http.Client{Transport: &mockTransport{roundTrip: func(r *http.Request) (*http.Response, error) {
			return nil, r.Context().Err()
		}}}, GerritAuth{})
	if _, err := client.GetChange(cancelCtx, "123"); !errors.Is(err, context.Canceled) || IsGerritHTTPStatus(err, http.StatusInternalServerError) {
		t.Errorf("GetChange(canceled) = %v, want context.Canceled", err)
	}

	timeoutCtx, cancelTimeout := context.WithTimeout(t.Context(), time.Nanosecond)
	defer cancelTimeout()
	time.Sleep(time.Millisecond)
	if _, err := client.GetChange(timeoutCtx, "123"); !errors.Is(err, context.DeadlineExceeded) {
		t.Errorf("GetChange(deadline exceeded) = %v, want context.DeadlineExceeded", err)
	}
}

func TestGerritClient_QueryChangesMulti_2DAnd1DFallback(t *testing.T) {
	for _, tc := range []struct {
		name    string
		queries []string
		opts    []string
		rawJSON string
		want    []int
	}{
		{
			name:    "2D response",
			queries: []string{"status:open", "owner:self"},
			opts:    []string{"LABELS"},
			rawJSON: ` [[{"_number":101,"subject":"first"}],[{"_number":202,"subject":"second"}]]`,
			want:    []int{101, 202},
		},
		{
			name:    "1D single-query fallback",
			queries: []string{"change:303"},
			rawJSON: `[{"_number":303,"subject":"single"}]`,
			want:    []int{303},
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			server := NewMockGerritServer(t)
			server.OnJSON("GET", "/changes/", http.StatusOK, tc.rawJSON)

			client := NewGerritClientWithHTTP(server.URL, server.Server.Client(), GerritAuth{})
			batches, err := client.QueryChangesMulti(t.Context(), tc.queries, tc.opts...)
			if err != nil {
				t.Fatalf("QueryChangesMulti failed: %v", err)
			}
			if len(batches) != len(tc.want) {
				t.Fatalf("len(batches) = %d, want %d (%+v)", len(batches), len(tc.want), batches)
			}
			for i, wantNum := range tc.want {
				if len(batches[i]) != 1 || batches[i][0].Number != wantNum {
					t.Errorf("batches[%d] = %+v, want single change %d", i, batches[i], wantNum)
				}
			}
		})
	}
}

func TestGerritClient_PreservesEscapedSlashesOnAnonymousPrefixStrip(t *testing.T) {
	ctx := t.Context()

	t.Run("unauthenticated client strips /a/ and preserves %2F in RawPath", func(t *testing.T) {
		var reqs []*http.Request
		client := NewGerritClientWithHTTP("https://pigweed-review.googlesource.com/a",
			&http.Client{Transport: gerritStatusTransport(http.StatusOK, ")]}'\n{\"code_owners\":[]}", &reqs)},
			GerritAuth{AllowAnonymousFallback: true})

		if _, err := client.QueryBranchCodeOwners(ctx, "pigweed/pigweed", "main/dev", "dir/a/file.cc"); err != nil {
			t.Fatalf("QueryBranchCodeOwners failed: %v", err)
		}
		want := "/projects/pigweed%2Fpigweed/branches/main%2Fdev/code_owners/dir%2Fa%2Ffile.cc"
		if len(reqs) != 1 || reqs[0].URL.EscapedPath() != want {
			t.Errorf("EscapedPath = %q, want %q", reqs[0].URL.EscapedPath(), want)
		}
	})

	t.Run("401 anonymous fallback strips /a/ and preserves %2F in RawPath", func(t *testing.T) {
		var gotEscapedPaths []string
		mockBase := &mockTransport{
			roundTrip: func(req *http.Request) (*http.Response, error) {
				gotEscapedPaths = append(gotEscapedPaths, req.URL.EscapedPath())
				if strings.HasPrefix(req.URL.Path, "/a/") {
					return gerritStatusTransport(http.StatusUnauthorized, "Unauthorized", nil).roundTrip(req)
				}
				return gerritStatusTransport(http.StatusOK, ")]}'\n{\"_number\":123}", nil).roundTrip(req)
			},
		}

		var stderrBuf bytes.Buffer
		client := NewGerritClientWithHTTP("https://pigweed-review.googlesource.com/a", &http.Client{Transport: mockBase},
			GerritAuth{StaticToken: "expired-tok", AllowAnonymousFallback: true})
		client.stderr = &stderrBuf

		ch, err := client.GetChange(ctx, "pigweed/pigweed~123")
		if err != nil || ch.Number != 123 {
			t.Fatalf("GetChange = %+v, %v", ch, err)
		}
		wantPaths := []string{"/a/changes/pigweed%2Fpigweed~123", "/changes/pigweed%2Fpigweed~123"}
		if len(gotEscapedPaths) != 2 || gotEscapedPaths[0] != wantPaths[0] || gotEscapedPaths[1] != wantPaths[1] {
			t.Errorf("gotEscapedPaths = %v, want %v", gotEscapedPaths, wantPaths)
		}
		if !strings.Contains(stderrBuf.String(), "Warning: authenticated request") {
			t.Errorf("expected warning in stderr on downgrade, got %q", stderrBuf.String())
		}
	})

	t.Run("AllowAnonymousFallback false does not retry on 401", func(t *testing.T) {
		var reqs []*http.Request
		client := NewGerritClientWithHTTP("https://pigweed-review.googlesource.com/a",
			&http.Client{Transport: gerritStatusTransport(http.StatusUnauthorized, "Unauthorized", &reqs)},
			GerritAuth{StaticToken: "tok"})

		_, err := client.doRaw(ctx, "GET", "changes/123", nil, nil, maxHTTPBodyBytes)
		if !IsGerritHTTPStatus(err, http.StatusUnauthorized) {
			t.Errorf("err = %v, want HTTP 401 (no downgrade)", err)
		}
		if len(reqs) != 1 {
			t.Errorf("expected exactly 1 request (no retry on unauthenticated path), got %d", len(reqs))
		}
	})

	t.Run("400 anonymous fallback on GET", func(t *testing.T) {
		var reqs []*http.Request
		mockBase := &mockTransport{
			roundTrip: func(req *http.Request) (*http.Response, error) {
				reqs = append(reqs, req)
				if strings.HasPrefix(req.URL.Path, "/a/") {
					return gerritStatusTransport(http.StatusBadRequest, "Bad Request", nil).roundTrip(req)
				}
				return gerritStatusTransport(http.StatusOK, ")]}'\n{\"_number\":123}", nil).roundTrip(req)
			},
		}
		client := NewGerritClientWithHTTP("https://pigweed-review.googlesource.com/a", &http.Client{Transport: mockBase},
			GerritAuth{StaticToken: "tok", AllowAnonymousFallback: true})
		client.stderr = io.Discard
		if ch, err := client.GetChange(ctx, "123"); err != nil || ch.Number != 123 || len(reqs) != 2 {
			t.Errorf("GetChange on 400 fallback = (%+v, %v, %d reqs), want (123, nil, 2 reqs)", ch, err, len(reqs))
		}
	})

	t.Run("accounts/self, mutations, and non-Google hosts never fall back to anonymous", func(t *testing.T) {
		for _, tc := range []struct {
			name    string
			baseURL string
			call    func(c *GerritClient) error
		}{
			{
				name:    "accounts/self",
				baseURL: "https://pigweed-review.googlesource.com/a",
				call:    func(c *GerritClient) error { _, err := c.GetSelfAccount(ctx); return err },
			},
			{
				name:    "POST mutation",
				baseURL: "https://pigweed-review.googlesource.com/a",
				call:    func(c *GerritClient) error { _, err := c.SubmitChange(ctx, "123"); return err },
			},
			{
				name:    "non-Google host",
				baseURL: "https://gerrit.example.org/a",
				call:    func(c *GerritClient) error { _, err := c.GetChange(ctx, "123"); return err },
			},
		} {
			t.Run(tc.name, func(t *testing.T) {
				var reqs []*http.Request
				client := NewGerritClientWithHTTP(tc.baseURL,
					&http.Client{Transport: gerritStatusTransport(http.StatusUnauthorized, "Unauthorized", &reqs)},
					GerritAuth{StaticToken: "tok", AllowAnonymousFallback: true})
				if err := tc.call(client); !IsGerritHTTPStatus(err, http.StatusUnauthorized) || len(reqs) != 1 {
					t.Errorf("%s: err=%v, reqs=%d; want 401 with 1 request", tc.name, err, len(reqs))
				}
			})
		}
	})
}

func TestGerritClient_DynamicAuthTokenRefreshOn401Or403(t *testing.T) {
	ctx := t.Context()

	t.Run("refreshes token and retries once on 401", func(t *testing.T) {
		tokens := []string{"stale-tok", "fresh-tok"}
		idx := 0
		WithHermeticAuth(t, AuthResolvers{
			GerritToken: func(context.Context) (string, string, error) {
				tok := tokens[idx]
				if idx < len(tokens)-1 {
					idx++
				}
				return tok, "luci-auth", nil
			},
		})
		var gotAuth []string
		tr := &mockTransport{
			roundTrip: func(req *http.Request) (*http.Response, error) {
				gotAuth = append(gotAuth, req.Header.Get("Authorization"))
				if len(gotAuth) == 1 {
					return gerritStatusTransport(http.StatusUnauthorized, "invalid_token", nil).roundTrip(req)
				}
				return gerritStatusTransport(http.StatusOK, ")]}'\n{\"_number\":123}", nil).roundTrip(req)
			},
		}
		client := NewGerritClientWithHTTP("https://pigweed-review.googlesource.com/a", &http.Client{Transport: tr}, GerritAuth{Dynamic: true})
		ch, err := client.GetChange(ctx, "123")
		if err != nil || ch.Number != 123 {
			t.Fatalf("GetChange = (%+v, %v), want 123", ch, err)
		}
		if len(gotAuth) != 2 || gotAuth[0] != "Bearer stale-tok" || gotAuth[1] != "Bearer fresh-tok" {
			t.Errorf("Authorization headers = %v, want [Bearer stale-tok, Bearer fresh-tok]", gotAuth)
		}
	})

	t.Run("does not retry when refreshed token is identical", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{
			GerritToken: func(context.Context) (string, string, error) {
				return "same-tok", "luci-auth", nil
			},
		})
		var reqs []*http.Request
		client := NewGerritClientWithHTTP("https://pigweed-review.googlesource.com/a",
			&http.Client{Transport: gerritStatusTransport(http.StatusUnauthorized, "Unauthorized", &reqs)},
			GerritAuth{Dynamic: true})
		if _, err := client.GetChange(ctx, "123"); !IsGerritHTTPStatus(err, http.StatusUnauthorized) || len(reqs) != 1 {
			t.Errorf("expected 1 request and 401 when token unchanged, got reqs=%d err=%v", len(reqs), err)
		}
	})

	t.Run("fails fast when token refresh returns error in googler mode", func(t *testing.T) {
		calls := 0
		WithHermeticAuth(t, AuthResolvers{
			GerritToken: func(context.Context) (string, string, error) {
				calls++
				if calls == 1 {
					return "stale-tok", "luci-auth", nil
				}
				return "", "", fmt.Errorf("refresh failed")
			},
		})
		var reqs []*http.Request
		client := NewGerritClientWithHTTP("https://pigweed-review.googlesource.com/a",
			&http.Client{Transport: gerritStatusTransport(http.StatusUnauthorized, "Unauthorized", &reqs)},
			GerritAuth{Dynamic: true, AllowAnonymousFallback: false, modeReason: "flag"})
		if _, err := client.GetChange(ctx, "123"); err == nil || !strings.Contains(err.Error(), "refresh failed") || len(reqs) != 1 {
			t.Errorf("expected refresh failure error after 1 request, got reqs=%d err=%v", len(reqs), err)
		}
	})
}

func TestGerritClient_SetReview(t *testing.T) {
	ctx := t.Context()

	for _, tc := range []struct {
		name       string
		status     int
		respBody   any
		wantErr    bool
		checkInput bool
	}{
		{
			name:       "defaults Drafts to KEEP without mutating caller input",
			status:     http.StatusOK,
			respBody:   map[string]any{"labels": map[string]int{"Code-Review": 2}},
			checkInput: true,
		},
		{
			name:     "ignores labels unmarshal error",
			status:   http.StatusOK,
			respBody: `{"labels": "malformed_labels_string"}`,
		},
		{
			name:     "propagates non-labels unmarshal error",
			status:   http.StatusOK,
			respBody: `{"ready": "not_a_bool"}`,
			wantErr:  true,
		},
		{
			name:     "propagates HTTP status error",
			status:   http.StatusForbidden,
			respBody: map[string]string{"error": "forbidden"},
			wantErr:  true,
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			server := NewMockGerritServer(t)
			server.OnJSON("POST", "/changes/12345/revisions/current/review", tc.status, tc.respBody)

			client := NewGerritClientWithHTTP(server.URL, server.Server.Client(), GerritAuth{})
			input := &ReviewInput{Message: "LGTM"}
			_, err := client.SetReview(ctx, "12345", "current", input)
			if (err != nil) != tc.wantErr {
				t.Fatalf("SetReview() err = %v, wantErr %v", err, tc.wantErr)
			}
			if tc.checkInput {
				got := DecodeRequest[ReviewInput](t, server.LastRequest())
				if got.Drafts != "KEEP" {
					t.Errorf("wire ReviewInput.Drafts = %q, want %q", got.Drafts, "KEEP")
				}
				if input.Drafts != "" {
					t.Errorf("caller input.Drafts mutated to %q, want empty", input.Drafts)
				}
			}
		})
	}

	validClient := NewGerritClientWithHTTP("http://localhost", nil, GerritAuth{})
	for _, tc := range []struct {
		name    string
		client  *GerritClient
		input   *ReviewInput
		wantErr string
	}{
		{"nil client", nil, &ReviewInput{}, "client is nil"},
		{"nil review input", validClient, nil, "review input is nil"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			_, err := tc.client.SetReview(ctx, "12345", "current", tc.input)
			if err == nil || !strings.Contains(err.Error(), tc.wantErr) {
				t.Errorf("SetReview() error = %v, want %q", err, tc.wantErr)
			}
		})
	}
}

// gerritStatusTransport replies to every request with status and body.
func gerritStatusTransport(status int, body string, reqs *[]*http.Request) *mockTransport {
	return &mockTransport{
		roundTrip: func(req *http.Request) (*http.Response, error) {
			if reqs != nil {
				*reqs = append(*reqs, req)
			}
			return &http.Response{
				StatusCode: status,
				Status:     http.StatusText(status),
				Body:       io.NopCloser(strings.NewReader(body)),
				Header:     make(http.Header),
				Request:    req,
			}, nil
		},
	}
}

func TestGerritAPIError_TruncatesBody(t *testing.T) {
	body := strings.Repeat("x", 10<<10)
	client := NewGerritClientWithHTTP("https://pigweed-review.googlesource.com/a",
		&http.Client{Transport: gerritStatusTransport(http.StatusInternalServerError, body, nil)}, GerritAuth{StaticToken: "tok"})
	_, err := client.GetChange(t.Context(), "1")
	if !IsGerritHTTPStatus(err, http.StatusInternalServerError) {
		t.Fatalf("err = %v, want HTTP 500", err)
	}
	msg := err.Error()
	if len(msg) >= 2200 {
		t.Errorf("len(Error()) = %d, want < 2200", len(msg))
	}
	if !strings.Contains(msg, "… (truncated)") {
		t.Errorf("Error() = %q, want a truncation marker", msg)
	}
}

func TestGerritAPIError_ReportsSentURL(t *testing.T) {
	var reqs []*http.Request
	client := NewGerritClientWithHTTP("https://pigweed-review.googlesource.com/a",
		&http.Client{Transport: gerritStatusTransport(http.StatusUnauthorized, "denied", &reqs)},
		GerritAuth{StaticToken: "tok", AllowAnonymousFallback: true})
	client.stderr = io.Discard
	_, err := client.GetChange(t.Context(), "1")
	apiErr, ok := errors.AsType[*GerritAPIError](err)
	if !ok {
		t.Fatalf("err = %v, want *GerritAPIError", err)
	}
	if len(reqs) != 2 {
		t.Fatalf("requests = %d, want 2 (anonymous fallback)", len(reqs))
	}
	if want := "https://pigweed-review.googlesource.com/changes/1"; apiErr.URL != want {
		t.Errorf("GerritAPIError.URL = %q, want %q", apiErr.URL, want)
	}
}

func TestGerritClient_GetPatchLimit(t *testing.T) {
	prev := maxGerritPatchBytes
	maxGerritPatchBytes = 16
	t.Cleanup(func() { maxGerritPatchBytes = prev })

	client := NewGerritClientWithHTTP("https://pigweed-review.googlesource.com/a",
		&http.Client{Transport: gerritStatusTransport(http.StatusOK, strings.Repeat("A", 64), nil)}, GerritAuth{})
	_, err := client.GetPatch(t.Context(), "1", "current")
	if err == nil || !strings.Contains(err.Error(), "exceeds 16 bytes limit") || !strings.Contains(err.Error(), "gh pr checkout") {
		t.Errorf("GetPatch err = %v, want limit error with remediation", err)
	}

	maxGerritPatchBytes = 64
	if patch, err := client.GetPatch(t.Context(), "1", "current"); err != nil || len(patch) != 64 {
		t.Errorf("GetPatch at limit = (%d bytes, %v), want 64 bytes", len(patch), err)
	}
}

func TestGerritClient_DefaultHTTPClient(t *testing.T) {
	client := NewGerritClientWithHTTP("pigweed-review.googlesource.com/a/", nil, GerritAuth{})
	if client.hc == nil || client.hc.CheckRedirect == nil || client.hc.Timeout != 0 {
		t.Errorf("default client = %+v, want CheckRedirect set and no Timeout", client.hc)
	}
	if got, want := client.BaseURL(), "https://pigweed-review.googlesource.com/a"; got != want {
		t.Errorf("BaseURL() = %q, want %q", got, want)
	}
	if client.Authenticated() {
		t.Error("Authenticated() = true for zero GerritAuth, want false")
	}

	hc := &http.Client{}
	client = NewGerritClientWithHTTP("https://example.com", hc, GerritAuth{StaticToken: "tok"})
	if client.hc == hc || client.hc.CheckRedirect == nil {
		t.Error("caller client without CheckRedirect was not copied with it set")
	}
	if hc.CheckRedirect != nil {
		t.Error("caller's http.Client was mutated")
	}
}

func TestIsGoogleGerritHost(t *testing.T) {
	for _, tc := range []struct {
		host string
		want bool
	}{
		{"pigweed-review.googlesource.com", true},
		{"PIGWEED.googlesource.com.", true},
		{"foo" + gobCorpDomainSuffix, true},
		{"x.googlesource.com.evil.com", false},
		{"googlesource.com", false},
		{".googlesource.com", false},
		{"pigweed-review.googlesource.com:443", false},
		{"evil.com", false},
		{"localhost", false},
		{"", false},
		{"sso://evil/x", false},
		{"https://pigweed-review.googlesource.com/", false},
		{"pigweed-review.googlesource.com/a", false},
	} {
		if got := isGoogleGerritHost(tc.host); got != tc.want {
			t.Errorf("isGoogleGerritHost(%q) = %v, want %v", tc.host, got, tc.want)
		}
	}
}

func TestStripAuthOnCrossHostRedirect(t *testing.T) {
	mustReq := func(rawURL string) *http.Request {
		u, err := url.Parse(rawURL)
		if err != nil {
			t.Fatal(err)
		}
		h := make(http.Header)
		h.Set("Authorization", "Bearer secret")
		return &http.Request{URL: u, Header: h}
	}
	origin := []*http.Request{mustReq("https://pigweed-review.googlesource.com:443/a/changes/1")}

	sameHost := mustReq("https://pigweed-review.googlesource.com/a/changes/1/")
	if err := stripAuthOnCrossHostRedirect(sameHost, origin); err != nil || sameHost.Header.Get("Authorization") == "" {
		t.Errorf("same-host default-port redirect stripped Authorization or erred: hdr=%q err=%v", sameHost.Header.Get("Authorization"), err)
	}

	crossHost := mustReq("https://evil.example.com/a/changes/1")
	if err := stripAuthOnCrossHostRedirect(crossHost, origin); err != nil || crossHost.Header.Get("Authorization") != "" {
		t.Errorf("cross-host redirect retained Authorization: hdr=%q err=%v", crossHost.Header.Get("Authorization"), err)
	}

	plainHTTP := mustReq("http://pigweed-review.googlesource.com/a/changes/1")
	if err := stripAuthOnCrossHostRedirect(plainHTTP, origin); err != nil || plainHTTP.Header.Get("Authorization") != "" {
		t.Errorf("plain HTTP redirect retained Authorization: hdr=%q err=%v", plainHTTP.Header.Get("Authorization"), err)
	}

	if err := stripAuthOnCrossHostRedirect(sameHost, make([]*http.Request, 10)); err == nil {
		t.Errorf("expected error when len(via) == 10, got nil")
	}
}

func TestResolveGerritClientAuth(t *testing.T) {
	ctx := t.Context()
	origMode := AuthModeFlag
	t.Cleanup(func() { AuthModeFlag = origMode })

	WithHermeticAuth(t, AuthResolvers{
		GerritToken: func(context.Context) (string, string, error) {
			return "", "", fmt.Errorf("no token")
		},
	})
	AuthModeFlag = "googler"
	if _, err := resolveGerritClientAuth(ctx, "https://pigweed-review.googlesource.com"); ExitCodeFor(err) != ExitCodeAuth {
		t.Errorf("googler mode without token: err = %v, want ExitCodeAuth", err)
	}

	AuthModeFlag = "community"
	if auth, err := resolveGerritClientAuth(ctx, "https://pigweed-review.googlesource.com"); err != nil || !auth.AllowAnonymousFallback {
		t.Errorf("community mode without token = (%+v, %v), want AllowAnonymousFallback=true", auth, err)
	}

	t.Setenv("GERRIT_TOKEN", "static-env-tok")
	ResetAuthTokenCaches()
	if auth, err := resolveGerritClientAuth(ctx, "https://pigweed-review.googlesource.com"); err != nil || auth.StaticToken != "static-env-tok" {
		t.Errorf("community mode with GERRIT_TOKEN = (%+v, %v), want StaticToken=static-env-tok", auth, err)
	}

	t.Setenv("GERRIT_TOKEN", "")
	WithHermeticAuth(t, AuthResolvers{
		GerritToken: func(context.Context) (string, string, error) {
			return "dyn-tok", "luci-auth", nil
		},
	})
	if auth, err := resolveGerritClientAuth(ctx, "https://pigweed-review.googlesource.com"); err != nil || !auth.Dynamic {
		t.Errorf("dynamic luci-auth = (%+v, %v), want Dynamic=true", auth, err)
	}
}

func TestGerritClient_Endpoints(t *testing.T) {
	ctx := t.Context()
	var lastReq *http.Request
	var lastBody []byte
	var nextResp string
	tr := &mockTransport{
		roundTrip: func(req *http.Request) (*http.Response, error) {
			lastReq = req
			lastBody = nil
			if req.Body != nil {
				lastBody, _ = io.ReadAll(req.Body)
				_ = req.Body.Close()
			}
			return gerritStatusTransport(http.StatusOK, ")]}'\n"+nextResp, nil).roundTrip(req)
		},
	}
	client := NewGerritClientWithHTTP("https://pigweed-review.googlesource.com/a", &http.Client{Transport: tr}, GerritAuth{StaticToken: "tok"})

	for _, tc := range []struct {
		name       string
		resp       string
		call       func() error
		wantMethod string
		wantPath   string
		wantQuery  string
		wantBody   string
	}{
		{
			name:       "GetSelfAccount",
			resp:       `{"_account_id":42}`,
			call:       func() error { _, err := client.GetSelfAccount(ctx); return err },
			wantMethod: "GET",
			wantPath:   "/a/accounts/self",
		},
		{
			name:       "GetChangeDetail",
			resp:       `{"_number":123}`,
			call:       func() error { _, err := client.GetChangeDetail(ctx, "123", "LABELS", "MESSAGES"); return err },
			wantMethod: "GET",
			wantPath:   "/a/changes/123/detail",
			wantQuery:  "o=LABELS&o=MESSAGES",
		},
		{
			name:       "QueryChanges",
			resp:       `[{"_number":123}]`,
			call:       func() error { _, err := client.QueryChanges(ctx, "status:open", 5, "LABELS"); return err },
			wantMethod: "GET",
			wantPath:   "/a/changes/",
			wantQuery:  "n=5&o=LABELS&q=status%3Aopen",
		},
		{
			name:       "AbandonChange",
			resp:       `{"_number":123,"status":"ABANDONED"}`,
			call:       func() error { _, err := client.AbandonChange(ctx, "123", "closing"); return err },
			wantMethod: "POST",
			wantPath:   "/a/changes/123/abandon",
			wantBody:   `"message":"closing"`,
		},
		{
			name:       "RestoreChange",
			resp:       `{"_number":123,"status":"NEW"}`,
			call:       func() error { _, err := client.RestoreChange(ctx, "123", "reopening"); return err },
			wantMethod: "POST",
			wantPath:   "/a/changes/123/restore",
			wantBody:   `"message":"reopening"`,
		},
		{
			name:       "SetReadyForReview",
			resp:       `{}`,
			call:       func() error { return client.SetReadyForReview(ctx, "123", "ready") },
			wantMethod: "POST",
			wantPath:   "/a/changes/123/ready",
		},
		{
			name:       "SetWorkInProgress",
			resp:       `{}`,
			call:       func() error { return client.SetWorkInProgress(ctx, "123", "wip") },
			wantMethod: "POST",
			wantPath:   "/a/changes/123/wip",
		},
		{
			name:       "AddReviewer",
			resp:       `{}`,
			call:       func() error { return client.AddReviewer(ctx, "123", "a@b.com", "REVIEWER") },
			wantMethod: "POST",
			wantPath:   "/a/changes/123/reviewers",
			wantBody:   `"reviewer":"a@b.com"`,
		},
		{
			name:       "DeleteReviewer",
			resp:       ``,
			call:       func() error { return client.DeleteReviewer(ctx, "123", "a@b.com") },
			wantMethod: "DELETE",
			wantPath:   "/a/changes/123/reviewers/a@b.com",
		},
		{
			name:       "AddToAttentionSet",
			resp:       `{}`,
			call:       func() error { return client.AddToAttentionSet(ctx, "123", "42", "ping") },
			wantMethod: "POST",
			wantPath:   "/a/changes/123/attention",
			wantBody:   `"user":"42"`,
		},
		{
			name:       "RemoveFromAttentionSet",
			resp:       ``,
			call:       func() error { return client.RemoveFromAttentionSet(ctx, "123", "42", "done") },
			wantMethod: "DELETE",
			wantPath:   "/a/changes/123/attention/42",
			wantBody:   `"reason":"done"`,
		},
		{
			name:       "SetTopic",
			resp:       `"my-topic"`,
			call:       func() error { return client.SetTopic(ctx, "123", "my-topic") },
			wantMethod: "PUT",
			wantPath:   "/a/changes/123/topic",
			wantBody:   `"topic":"my-topic"`,
		},
		{
			name:       "DeleteTopic",
			resp:       ``,
			call:       func() error { return client.DeleteTopic(ctx, "123") },
			wantMethod: "DELETE",
			wantPath:   "/a/changes/123/topic",
		},
		{
			name:       "SetHashtags",
			resp:       `["tag1"]`,
			call:       func() error { return client.SetHashtags(ctx, "123", []string{"tag1"}, []string{"tag2"}) },
			wantMethod: "POST",
			wantPath:   "/a/changes/123/hashtags",
			wantBody:   `"add":["tag1"]`,
		},
		{
			name:       "ListChangeComments",
			resp:       `{"foo.go":[{"id":"c1"}]}`,
			call:       func() error { _, err := client.ListChangeComments(ctx, "123"); return err },
			wantMethod: "GET",
			wantPath:   "/a/changes/123/comments",
		},
		{
			name:       "ListChangeDrafts",
			resp:       `{"foo.go":[{"id":"d1"}]}`,
			call:       func() error { _, err := client.ListChangeDrafts(ctx, "123"); return err },
			wantMethod: "GET",
			wantPath:   "/a/changes/123/drafts",
		},
		{
			name:       "CreateDraft",
			resp:       `{"id":"d1"}`,
			call:       func() error { _, err := client.CreateDraft(ctx, "123", "2", &CommentInput{Message: "nit"}); return err },
			wantMethod: "PUT",
			wantPath:   "/a/changes/123/revisions/2/drafts",
			wantBody:   `"message":"nit"`,
		},
		{
			name: "UpdateDraft",
			resp: `{"id":"d1"}`,
			call: func() error {
				_, err := client.UpdateDraft(ctx, "123", "current", "d1", &CommentInput{Message: "updated"})
				return err
			},
			wantMethod: "PUT",
			wantPath:   "/a/changes/123/revisions/current/drafts/d1",
			wantBody:   `"message":"updated"`,
		},
		{
			name:       "DeleteDraft",
			resp:       ``,
			call:       func() error { return client.DeleteDraft(ctx, "123", "current", "d1") },
			wantMethod: "DELETE",
			wantPath:   "/a/changes/123/revisions/current/drafts/d1",
		},
		{
			name:       "ListFiles",
			resp:       `{"foo.go":{"lines_inserted":5}}`,
			call:       func() error { _, err := client.ListFiles(ctx, "123", "current"); return err },
			wantMethod: "GET",
			wantPath:   "/a/changes/123/revisions/current/files",
		},
		{
			name:       "ListProjectLabels",
			resp:       `[{"name":"Code-Review"}]`,
			call:       func() error { _, err := client.ListProjectLabels(ctx, "pigweed/pigweed"); return err },
			wantMethod: "GET",
			wantPath:   "/a/projects/pigweed%2Fpigweed/labels/",
		},
		{
			name: "QueryChangeCodeOwners",
			resp: `{"code_owners":[]}`,
			call: func() error {
				_, err := client.QueryChangeCodeOwners(ctx, "123", "current", "pw_ghish/gerrit.go")
				return err
			},
			wantMethod: "GET",
			wantPath:   "/a/changes/123/revisions/current/code_owners/pw_ghish%2Fgerrit.go",
			wantQuery:  "limit=10&o=DETAILS",
		},
		{
			name:       "IsFlowsEnabled",
			resp:       `{"enabled":true}`,
			call:       func() error { _, err := client.IsFlowsEnabled(ctx, 123); return err },
			wantMethod: "GET",
			wantPath:   "/a/changes/123/is-flows-enabled",
		},
		{
			name:       "ListFlows",
			resp:       `[{"uuid":"f1"}]`,
			call:       func() error { _, err := client.ListFlows(ctx, 123); return err },
			wantMethod: "GET",
			wantPath:   "/a/changes/123/flows",
		},
		{
			name:       "CreateFlow",
			resp:       `{"uuid":"f1"}`,
			call:       func() error { return client.CreateFlow(ctx, 123, FlowInput{}) },
			wantMethod: "POST",
			wantPath:   "/a/changes/123/flows",
		},
		{
			name:       "DeleteFlow",
			resp:       ``,
			call:       func() error { return client.DeleteFlow(ctx, 123, "uuid-1") },
			wantMethod: "DELETE",
			wantPath:   "/a/changes/123/flows/uuid-1",
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			nextResp = tc.resp
			if err := tc.call(); err != nil {
				t.Fatalf("%s() error: %v", tc.name, err)
			}
			if lastReq.Method != tc.wantMethod || lastReq.URL.EscapedPath() != tc.wantPath {
				t.Errorf("%s() = %s %s, want %s %s", tc.name, lastReq.Method, lastReq.URL.EscapedPath(), tc.wantMethod, tc.wantPath)
			}
			if tc.wantQuery != "" && lastReq.URL.RawQuery != tc.wantQuery {
				t.Errorf("%s() query = %q, want %q", tc.name, lastReq.URL.RawQuery, tc.wantQuery)
			}
			if tc.wantBody != "" && !strings.Contains(string(lastBody), tc.wantBody) {
				t.Errorf("%s() body = %q, want substring %q", tc.name, lastBody, tc.wantBody)
			}
		})
	}

	for _, emptyCall := range []func() error{
		func() error { _, err := client.GetChange(ctx, ""); return err },
		func() error { _, err := client.SetReview(ctx, "", "current", &ReviewInput{}); return err },
		func() error { _, err := client.GetPatch(ctx, "", "current"); return err },
		func() error { _, err := client.ListFiles(ctx, "", "current"); return err },
		func() error { _, err := client.CreateDraft(ctx, "", "current", &CommentInput{}); return err },
	} {
		if err := emptyCall(); err == nil || !strings.Contains(err.Error(), "changeID is required") {
			t.Errorf("expected empty changeID error, got %v", err)
		}
	}
}

func TestGerritClient_PathSegmentEscapingAndTraversalSafety(t *testing.T) {
	ctx := t.Context()
	var gotMethod, gotEscapedPath string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotMethod = r.Method
		gotEscapedPath = r.URL.EscapedPath()
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte(")]}'\n{}\n"))
	}))
	t.Cleanup(srv.Close)

	client := NewGerritClientWithHTTP(srv.URL, srv.Client(), GerritAuth{})

	for _, tc := range []struct {
		name            string
		call            func() error
		wantMethod      string
		wantEscapedPath string
	}{
		{
			name:            "unencoded project-qualified change ID",
			call:            func() error { _, err := client.GetChangeDetail(ctx, "pigweed/pigweed~123"); return err },
			wantMethod:      http.MethodGet,
			wantEscapedPath: "/changes/pigweed%2Fpigweed~123/detail",
		},
		{
			name:            "pre-encoded project-qualified change ID is not double-encoded",
			call:            func() error { _, err := client.GetChangeDetail(ctx, "pigweed%2Fpigweed~123"); return err },
			wantMethod:      http.MethodGet,
			wantEscapedPath: "/changes/pigweed%2Fpigweed~123/detail",
		},
		{
			name:            "traversal and fragment in change ID",
			call:            func() error { _, err := client.GetChangeDetail(ctx, "../abandon#"); return err },
			wantMethod:      http.MethodGet,
			wantEscapedPath: "/changes/..%2Fabandon%23/detail",
		},
		{
			name:            "traversal in revision ID",
			call:            func() error { _, err := client.ListFiles(ctx, "123", "../../abandon"); return err },
			wantMethod:      http.MethodGet,
			wantEscapedPath: "/changes/123/revisions/..%2F..%2Fabandon/files",
		},
		{
			name: "traversal in UpdateDraft draft ID",
			call: func() error {
				_, err := client.UpdateDraft(ctx, "123", "current", "../x", &CommentInput{Message: "m"})
				return err
			},
			wantMethod:      http.MethodPut,
			wantEscapedPath: "/changes/123/revisions/current/drafts/..%2Fx",
		},
		{
			name:            "traversal in DeleteDraft draft ID",
			call:            func() error { return client.DeleteDraft(ctx, "123", "current", "../x") },
			wantMethod:      http.MethodDelete,
			wantEscapedPath: "/changes/123/revisions/current/drafts/..%2Fx",
		},
		{
			name:            "malformed percent-escape is escaped raw",
			call:            func() error { _, err := client.GetChangeDetail(ctx, "bad%zz"); return err },
			wantMethod:      http.MethodGet,
			wantEscapedPath: "/changes/bad%25zz/detail",
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			gotMethod, gotEscapedPath = "", ""
			if err := tc.call(); err != nil {
				t.Fatalf("%s error: %v", tc.name, err)
			}
			if gotMethod != tc.wantMethod || gotEscapedPath != tc.wantEscapedPath {
				t.Errorf("%s sent %s %s, want %s %s", tc.name, gotMethod, gotEscapedPath, tc.wantMethod, tc.wantEscapedPath)
			}
			if strings.Contains(gotEscapedPath, "%252F") {
				t.Errorf("%s double-encoded slash in %q", tc.name, gotEscapedPath)
			}
		})
	}
}
