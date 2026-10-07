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
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"strings"
	"testing"

	"github.com/andygrunwald/go-gerrit"
	"github.com/spf13/cobra"
)

func TestAuthStatus_AllHealthyGoogler(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/accounts/self", http.StatusOK, map[string]any{
		"_account_id": 1001,
		"name":        "Keir Mierle",
		"email":       "keir@google.com",
	})

	origLUCI := LUCITokenResolver
	LUCITokenResolver = func(ctx context.Context) (string, string, error) {
		return "luci-tok", "luci-auth token", nil
	}
	defer func() { LUCITokenResolver = origLUCI }()

	origIT := IssueTrackerTokenResolver
	IssueTrackerTokenResolver = func(ctx context.Context) (string, string, error) {
		return "it-tok", "luci-auth token", nil
	}
	defer func() { IssueTrackerTokenResolver = origIT }()

	origQuota := IssueTrackerQuotaProjectResolver
	IssueTrackerQuotaProjectResolver = func(ctx context.Context, token string) string { return "pigweed-infra" }
	defer func() { IssueTrackerQuotaProjectResolver = origQuota }()

	output, err := executeCommand(RootCmd, "auth", "status", "--auth-mode", "googler")
	if err != nil {
		t.Fatalf("expected auth status to succeed, got error: %v\nOutput:\n%s", err, output)
	}

	for _, want := range []string{
		"Authentication Mode: googler",
		"Keir Mierle <keir@google.com> (account #1001)",
		"(LUCI Buildbucket & LogDog)",
		"issuetracker.googleapis.com (Google Issue Tracker)",
		"quota project: pigweed-infra",
	} {
		if !strings.Contains(output, want) {
			t.Errorf("output missing %q:\n%s", want, output)
		}
	}
}

func TestAuthStatus_JSON(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/accounts/self", http.StatusOK, map[string]any{
		"_account_id": 1001,
		"name":        "Keir Mierle",
		"email":       "keir@google.com",
	})

	origLUCI := LUCITokenResolver
	LUCITokenResolver = func(ctx context.Context) (string, string, error) {
		return "luci-tok", "luci-auth token", nil
	}
	defer func() { LUCITokenResolver = origLUCI }()

	origIT := IssueTrackerTokenResolver
	IssueTrackerTokenResolver = func(ctx context.Context) (string, string, error) {
		return "it-tok", "luci-auth token", nil
	}
	defer func() { IssueTrackerTokenResolver = origIT }()

	output, err := executeCommand(RootCmd, "auth", "status", "--auth-mode", "googler", "--json", "mode,healthy,gerrit")
	if err != nil {
		t.Fatalf("unexpected error: %v\nOutput:\n%s", err, output)
	}

	var parsed map[string]any
	if err := json.Unmarshal([]byte(output), &parsed); err != nil {
		t.Fatalf("failed to parse JSON output: %v\nOutput:\n%s", err, output)
	}
	if parsed["mode"] != "googler" || parsed["healthy"] != true {
		t.Errorf("unexpected JSON payload: %+v", parsed)
	}

	// Unknown JSON field should fail fast
	_, err = executeCommand(RootCmd, "auth", "status", "--auth-mode", "googler", "--json", "unknownField")
	if err == nil || !strings.Contains(err.Error(), "unknown JSON field") {
		t.Errorf("expected unknown JSON field error, got: %v", err)
	}
}

func TestAuthStatus_GooglerFailsWithExitCode4WhenLUCIUnauthenticated(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/accounts/self", http.StatusOK, map[string]any{
		"_account_id": 1001,
		"name":        "Keir Mierle",
		"email":       "keir@google.com",
	})

	origLUCI := LUCITokenResolver
	LUCITokenResolver = func(ctx context.Context) (string, string, error) {
		return "", "", fmt.Errorf("interactive login required")
	}
	defer func() { LUCITokenResolver = origLUCI }()

	origIT := IssueTrackerTokenResolver
	IssueTrackerTokenResolver = func(ctx context.Context) (string, string, error) {
		return "it-tok", "luci-auth token", nil
	}
	defer func() { IssueTrackerTokenResolver = origIT }()

	output, err := executeCommand(RootCmd, "auth", "status", "--auth-mode", "googler")
	if err == nil {
		t.Fatalf("expected error when LUCI token is missing in googler mode, got nil\nOutput:\n%s", output)
	}
	if ExitCodeFor(err) != ExitCodeAuth {
		t.Errorf("ExitCodeFor(err) = %d, want %d", ExitCodeFor(err), ExitCodeAuth)
	}
	if !strings.Contains(err.Error(), "luci-auth login") {
		t.Errorf("expected luci-auth login remediation in error, got: %v", err)
	}
}

func TestAuthStatus_CommunityModeHealthyWithoutLUCIToken(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/accounts/self", http.StatusOK, map[string]any{
		"_account_id": 2002,
		"name":        "External Contributor",
		"email":       "contributor@example.com",
	})

	origLookPath := LookPathFn
	LookPathFn = func(file string) (string, error) {
		return "", fmt.Errorf("not found: %s", file)
	}
	defer func() { LookPathFn = origLookPath }()

	origLUCI := LUCITokenResolver
	LUCITokenResolver = func(ctx context.Context) (string, string, error) {
		return "", "", fmt.Errorf("luci-auth not found")
	}
	defer func() { LUCITokenResolver = origLUCI }()

	origIT := IssueTrackerTokenResolver
	IssueTrackerTokenResolver = func(ctx context.Context) (string, string, error) {
		return "", "", fmt.Errorf("gcloud not found")
	}
	defer func() { IssueTrackerTokenResolver = origIT }()

	output, err := executeCommand(RootCmd, "auth", "status", "--auth-mode", "community")
	if err != nil {
		t.Fatalf("expected community auth status to succeed when Gerrit is authenticated, got: %v\nOutput:\n%s", err, output)
	}
	if !strings.Contains(output, "Authentication Mode: community") || !strings.Contains(output, "External Contributor") {
		t.Errorf("unexpected output:\n%s", output)
	}
}

func TestAuthStatus_CommunityModeUnauthenticatedGerrit(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/accounts/self", http.StatusForbidden, map[string]any{
		"message": "Auth required",
	})

	t.Setenv("HOME", t.TempDir())
	t.Setenv("GERRIT_TOKEN", "")

	origLookPath := LookPathFn
	LookPathFn = func(file string) (string, error) {
		return "", fmt.Errorf("not found: %s", file)
	}
	defer func() { LookPathFn = origLookPath }()

	origLUCI := LUCITokenResolver
	LUCITokenResolver = func(ctx context.Context) (string, string, error) {
		return "", "", fmt.Errorf("luci-auth not found")
	}
	defer func() { LUCITokenResolver = origLUCI }()

	origIT := IssueTrackerTokenResolver
	IssueTrackerTokenResolver = func(ctx context.Context) (string, string, error) {
		return "", "", fmt.Errorf("gcloud not found")
	}
	defer func() { IssueTrackerTokenResolver = origIT }()

	output, err := executeCommand(RootCmd, "auth", "status", "--auth-mode", "community")
	if err == nil {
		t.Fatalf("expected community auth status to return ExitCodeAuth when Gerrit is unauthenticated, got nil\nOutput:\n%s", output)
	}
	if ExitCodeFor(err) != ExitCodeAuth {
		t.Errorf("ExitCodeFor(err) = %d, want %d", ExitCodeFor(err), ExitCodeAuth)
	}
	if !strings.Contains(output, "No Gerrit credentials configured") {
		t.Errorf("expected friendly unauthenticated Gerrit message in community mode, got:\n%s", output)
	}
}

func TestAuthStatus_NoneModeJSON(t *testing.T) {
	output, err := executeCommand(RootCmd, "auth", "status", "--auth-mode", "none", "--json")
	if err != nil {
		t.Fatalf("expected none mode auth status to succeed, got: %v\nOutput:\n%s", err, output)
	}
	var report AuthStatusReport
	if err := json.Unmarshal([]byte(output), &report); err != nil {
		t.Fatalf("failed to parse JSON: %v\nRaw:\n%s", err, output)
	}
	if report.Mode != AuthModeNone {
		t.Errorf("mode = %q, want %q", report.Mode, AuthModeNone)
	}
	if !report.Healthy {
		t.Errorf("expected Healthy=true in none mode, got false")
	}
	if report.Authenticated {
		t.Errorf("expected Authenticated=false in none mode, got true")
	}
	if report.Gerrit.Method != "none" || report.LUCI.Method != "none" || report.Buganizer.Method != "none" {
		t.Errorf("expected all service methods to be 'none', got gerrit=%q luci=%q buganizer=%q",
			report.Gerrit.Method, report.LUCI.Method, report.Buganizer.Method)
	}
}

func TestView_SurfacesLUCIAuthErrorInGooglerMode(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345", http.StatusOK, map[string]any{
		"_number":          12345,
		"subject":          "Test change",
		"status":           "NEW",
		"project":          "pigweed/pigweed",
		"branch":           "main",
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{"_number": 1},
		},
	})
	server.OnJSON("GET", "/changes/12345/revisions/current/files/", http.StatusOK, map[string]any{})

	origLUCIClient := getLUCIHTTPClient
	getLUCIHTTPClient = func(ctx context.Context, bbHost string) *http.Client {
		return &http.Client{Transport: &LUCIAuthTransport{Base: server.Server.Client().Transport}}
	}
	defer func() { getLUCIHTTPClient = origLUCIClient }()

	origLUCI := LUCITokenResolver
	LUCITokenResolver = func(ctx context.Context) (string, string, error) {
		return "", "", NewExitCodeError(ExitCodeAuth, "no token")
	}
	defer func() { LUCITokenResolver = origLUCI }()

	output, err := executeCommand(RootCmd, "pr", "view", "12345", "--auth-mode", "googler")
	if err != nil {
		t.Fatalf("unexpected error from pr view: %v\nOutput:\n%s", err, output)
	}
	if !strings.Contains(output, "LUCI authentication required") {
		t.Errorf("expected pr view to surface LUCI authentication required in Checks line, got:\n%s", output)
	}
}

func TestAuthStatus_ReportsFallbackMethodWhenGobCurlExpired(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/accounts/self", http.StatusOK, map[string]any{
		"_account_id": 1001,
		"name":        "Keir Mierle",
		"email":       "keir@google.com",
	})

	tmpDir := t.TempDir()
	mockScript := tmpDir + "/mock-gob-curl-expired.sh"
	if err := os.WriteFile(mockScript, []byte("#!/bin/sh\nprintf 'sso: credentials expired\\n' >&2\nexit 1\n"), 0755); err != nil {
		t.Fatalf("WriteFile failed: %v", err)
	}

	origLookPath := LookPathFn
	LookPathFn = func(file string) (string, error) {
		if file == "gob-curl" {
			return mockScript, nil
		}
		return "", fmt.Errorf("not found: %s", file)
	}
	defer func() { LookPathFn = origLookPath }()

	origGerrit := GerritTokenResolver
	GerritTokenResolver = func(ctx context.Context) (string, string, error) {
		return "luci-gerrit-token", "luci-auth", nil
	}
	defer func() { GerritTokenResolver = origGerrit }()

	origLUCI := LUCITokenResolver
	LUCITokenResolver = func(ctx context.Context) (string, string, error) {
		return "luci-tok", "luci-auth token", nil
	}
	defer func() { LUCITokenResolver = origLUCI }()

	origIT := IssueTrackerTokenResolver
	IssueTrackerTokenResolver = func(ctx context.Context) (string, string, error) {
		return "it-tok", "luci-auth token", nil
	}
	defer func() { IssueTrackerTokenResolver = origIT }()

	gobTr := &GobCurlTransport{
		Path:         mockScript,
		AutoFallback: true,
		Base:         server.Server.Client().Transport,
	}
	SetMockGerritClient(t, func(ctx context.Context, cmd *cobra.Command) (*gerrit.Client, error) {
		return gerrit.NewClient(ctx, server.URL, &http.Client{
			Transport: &fallbackTransport{
				base:                 gobTr,
				disallowAnonFallback: true,
			},
		})
	})

	ctx := context.Background()
	report, err := CheckAuthStatus(ctx, RootCmd)
	if err != nil {
		t.Fatalf("CheckAuthStatus failed: %v", err)
	}
	if !report.Healthy || !report.Gerrit.Authenticated {
		t.Fatalf("expected report to be healthy and Gerrit authenticated via fallback, got: %+v", report)
	}
	if !strings.Contains(report.Gerrit.Method, "luci-auth") || !strings.Contains(report.Gerrit.Method, "gob-curl") {
		t.Errorf("Gerrit.Method = %q, want luci-auth fallback description", report.Gerrit.Method)
	}
}
