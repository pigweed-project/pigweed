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
	"net/http"
	"strings"
	"testing"

	"github.com/spf13/cobra"
)

func TestAuthStatus_AllHealthyGoogler(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/accounts/self", http.StatusOK, map[string]any{
		"_account_id": 1001,
		"name":        "Keir Mierle",
		"email":       "keir@google.com",
	})

	WithHermeticAuth(t, AuthResolvers{
		IssueTrackerToken: func(ctx context.Context) (string, string, error) {
			return "it-tok", "luci-auth token", nil
		},
		IssueTrackerQuotaProject: func(ctx context.Context, _ string) string { return "pigweed-infra" },
	})

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
		"Verdict: authenticated (googler mode; all required services healthy)",
		"Next:    gh pr status",
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

	WithHermeticAuth(t, AuthResolvers{
		IssueTrackerToken: func(ctx context.Context) (string, string, error) {
			return "it-tok", "luci-auth token", nil
		},
	})

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
	if err == nil || !strings.Contains(err.Error(), "Unknown JSON field") {
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

	WithHermeticAuth(t, AuthResolvers{
		LUCIToken: func(ctx context.Context) (string, string, error) {
			return "", "", fmt.Errorf("interactive login required")
		},
		IssueTrackerToken: func(ctx context.Context) (string, string, error) {
			return "it-tok", "luci-auth token", nil
		},
	})

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

// TestAuthStatus_AccountIDOnly checks that an account with only an ID is
// labeled once, not as "account #N (account #N)".
func TestAuthStatus_AccountIDOnly(t *testing.T) {
	NewMockGerritServer(t).OnAccountSelf(1001)

	WithHermeticAuth(t, AuthResolvers{
		LookPath: func(file string) (string, error) {
			if file == "sso_client" {
				return "/usr/bin/sso_client", nil
			}
			return "", fmt.Errorf("not found")
		},
		IssueTrackerToken: func(ctx context.Context) (string, string, error) {
			return "it-tok", "luci-auth token", nil
		},
	})

	output, err := executeCommand(RootCmd, "auth", "status", "--auth-mode", "googler")
	if err != nil {
		t.Fatalf("expected auth status to succeed, got error: %v\nOutput:\n%s", err, output)
	}
	if !strings.Contains(output, "as account #1001") {
		t.Errorf("expected output to contain %q, got:\n%s", "as account #1001", output)
	}
	if strings.Contains(output, "account #1001 (account #1001)") {
		t.Errorf("expected the account ID only once, got:\n%s", output)
	}
}

func TestAuthStatus_CommunityModeHealthyWithoutLUCIToken(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/accounts/self", http.StatusOK, map[string]any{
		"_account_id": 2002,
		"name":        "External Contributor",
		"email":       "contributor@example.com",
	})

	WithHermeticAuth(t, AuthResolvers{
		LUCIToken: func(ctx context.Context) (string, string, error) {
			return "", "", fmt.Errorf("luci-auth not found")
		},
		IssueTrackerToken: func(ctx context.Context) (string, string, error) {
			return "", "", fmt.Errorf("luci-auth not found")
		},
	})

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

	WithHermeticAuth(t, AuthResolvers{
		LUCIToken: func(ctx context.Context) (string, string, error) {
			return "", "", fmt.Errorf("luci-auth not found")
		},
		IssueTrackerToken: func(ctx context.Context) (string, string, error) {
			return "", "", fmt.Errorf("luci-auth not found")
		},
	})

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
	if !strings.Contains(output, "Verdict: unauthenticated (community mode; login required)") || !strings.Contains(output, "Next:") {
		t.Errorf("expected unauthenticated Verdict and Next in output, got:\n%s", output)
	}

	t.Run("HTTP 400 surfaces first-time Gerrit account and CLA instructions", func(t *testing.T) {
		for _, mode := range []string{"community", "googler"} {
			t.Run(mode, func(t *testing.T) {
				server := NewMockGerritServer(t)
				server.OnJSON("GET", "/accounts/self", http.StatusBadRequest, map[string]any{
					"message": "Account does not exist",
				})
				WithHermeticAuth(t, AuthResolvers{
					GerritToken: func(ctx context.Context) (string, string, error) {
						return "valid-oauth-tok", "luci-auth", nil
					},
					LUCIToken: func(ctx context.Context) (string, string, error) {
						return "valid-luci-tok", "luci-auth", nil
					},
					IssueTrackerToken: func(ctx context.Context) (string, string, error) {
						return "valid-bug-tok", "luci-auth", nil
					},
				})

				out400, err400 := executeCommand(RootCmd, "auth", "status", "--auth-mode", mode)
				if err400 == nil {
					t.Fatalf("expected error on HTTP 400, got nil\nOutput:\n%s", out400)
				}
				if got := ExitCodeFor(err400); got != ExitCodeAuth {
					t.Errorf("ExitCodeFor(err400) = %d, want %d", got, ExitCodeAuth)
				}
				wantRemediation := fmt.Sprintf("Visit https://%s in a browser and click 'Sign in' once to create your Gerrit account, and sign the CLA at https://cla.developers.google.com/", CleanGerritHost(server.URL))
				for _, want := range []string{
					"No Gerrit account found",
					wantRemediation,
				} {
					if !strings.Contains(out400, want) {
						t.Errorf("expected output to contain %q, got:\n%s", want, out400)
					}
				}
				if strings.Contains(out400, "Run 'gcert'") || strings.Contains(out400, "luci-auth login") {
					t.Errorf("HTTP 400 account remediation was overwritten by generic login hint:\n%s", out400)
				}
			})
		}
	})
}

func TestAuthStatus_GerritUnauthenticatedMatrix(t *testing.T) {
	const (
		googleHost    = "pigweed-review.googlesource.com"
		nonGoogleHost = "gerrit.example.org"
		luciCause     = "luci-auth not found (searched PATH, /x/luci-auth)"
	)
	googlerLoginStep := fmt.Sprintf("Run '%s'", luciAuthLoginCommand(AuthModeGoogler))
	communityLoginStep := fmt.Sprintf("Run '%s'", luciAuthLoginCommand(AuthModeCommunity))
	const (
		gcertStep = "Run 'gcert' (with sso-cred-helper)"
		tokenStep = `Set an OAuth2 token: export GERRIT_TOKEN="<token>"`
	)

	for _, tc := range []struct {
		name             string
		mode             string
		host             string
		hasSSOCredHelper bool
		wantMessage      string
		wantRemediation  string
	}{
		{
			name:            "googler/google without sso-cred-helper",
			mode:            "googler",
			host:            googleHost,
			wantMessage:     fmt.Sprintf("Gerrit authentication required in googler mode (--auth-mode=googler): no Gerrit credentials found for %s (%s).", googleHost, luciCause),
			wantRemediation: googlerLoginStep + "; " + tokenStep,
		},
		{
			name:             "googler/google with sso-cred-helper",
			mode:             "googler",
			host:             googleHost,
			hasSSOCredHelper: true,
			wantMessage:      fmt.Sprintf("Gerrit authentication required in googler mode (--auth-mode=googler): no Gerrit credentials found for %s (%s).", googleHost, luciCause),
			wantRemediation:  gcertStep + "; " + googlerLoginStep + "; " + tokenStep,
		},
		{
			name:            "community/google includes cause in message",
			mode:            "community",
			host:            googleHost,
			wantMessage:     fmt.Sprintf("No Gerrit credentials configured for %s (%s; public changes readable, login required to push or review)", googleHost, luciCause),
			wantRemediation: communityLoginStep + "; " + tokenStep,
		},
		{
			name:             "googler/non-google offers only GERRIT_TOKEN",
			mode:             "googler",
			host:             nonGoogleHost,
			hasSSOCredHelper: true,
			wantMessage:      fmt.Sprintf("Gerrit host %s is not a Google-hosted Gerrit (*.googlesource.com); gh-ish only sends luci-auth tokens to Google hosts.", nonGoogleHost),
			wantRemediation:  tokenStep,
		},
		{
			name:             "community/non-google includes cause and offers only GERRIT_TOKEN",
			mode:             "community",
			host:             nonGoogleHost,
			hasSSOCredHelper: true,
			wantMessage:      fmt.Sprintf("No Gerrit credentials configured for %s (%s; public changes readable, login required to push or review)", nonGoogleHost, errNonGoogleGerritHost),
			wantRemediation:  tokenStep,
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			resolvers := AuthResolvers{
				GerritToken: func(ctx context.Context) (string, string, error) {
					return "", "", errors.New(luciCause)
				},
			}
			if tc.hasSSOCredHelper {
				resolvers.LookPath = onPath(ssoCredHelperPath, ssoCredHelperPath)
			}
			WithHermeticAuth(t, resolvers)
			SetupMockConfig(t, &MockGitRunner{})
			SetMockGerritClient(t, func(ctx context.Context, cmd *cobra.Command) (*GerritClient, error) {
				cfg := GetConfig(cmd)
				if ctx.Value(configKey) == nil {
					ctx = context.WithValue(ctx, configKey, cfg)
				}
				gerritURL, err := cfg.GerritURL(ctx)
				if err != nil {
					return nil, err
				}
				return newGerritClientForURL(ctx, gerritURL, &http.Client{
					Transport: gerritStatusTransport(http.StatusForbidden, "Authentication required\n", nil),
				})
			})

			var stdout, stderr bytes.Buffer
			err := executeCommandIO(RootCmd, nil, &stdout, &stderr, "auth", "status", "--json", "--auth-mode", tc.mode, "--host", tc.host)
			if ExitCodeFor(err) != ExitCodeAuth {
				t.Fatalf("ExitCodeFor(err) = %d, want %d (err: %v)\nStdout:\n%s\nStderr:\n%s", ExitCodeFor(err), ExitCodeAuth, err, stdout.String(), stderr.String())
			}
			var report AuthStatusReport
			if uErr := json.Unmarshal(stdout.Bytes(), &report); uErr != nil {
				t.Fatalf("json.Unmarshal: %v\nStdout:\n%s\nStderr:\n%s", uErr, stdout.String(), stderr.String())
			}
			if report.Gerrit.Authenticated || report.Gerrit.Method != "none" {
				t.Errorf("gerrit = (authenticated=%v, method=%q), want (false, \"none\")", report.Gerrit.Authenticated, report.Gerrit.Method)
			}
			if report.Gerrit.Message != tc.wantMessage {
				t.Errorf("gerrit.message = %q, want %q", report.Gerrit.Message, tc.wantMessage)
			}
			if report.Gerrit.Remediation != tc.wantRemediation {
				t.Errorf("gerrit.remediation = %q, want %q", report.Gerrit.Remediation, tc.wantRemediation)
			}
		})
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
	NewMockGerritServer(t).OnDefaultChange(12345)

	WithHermeticAuth(t, AuthResolvers{
		LUCIToken: func(ctx context.Context) (string, string, error) {
			return "", "", NewExitCodeError(ExitCodeAuth, "no token")
		},
	})

	output, err := executeCommand(RootCmd, "pr", "view", "12345", "--auth-mode", "googler")
	if err != nil {
		t.Fatalf("unexpected error from pr view: %v\nOutput:\n%s", err, output)
	}
	if !strings.Contains(output, "LUCI authentication required") {
		t.Errorf("expected pr view to surface LUCI authentication required in Checks line, got:\n%s", output)
	}
}
