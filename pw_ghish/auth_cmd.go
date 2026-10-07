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
	"strings"

	"github.com/spf13/cobra"
)

var authStatusJSON string

// ServiceAuthStatus represents the live authentication status of a single backend service.
type ServiceAuthStatus struct {
	Service       string `json:"service"`
	Host          string `json:"host,omitempty"`
	Authenticated bool   `json:"authenticated"`
	Method        string `json:"method,omitempty"`
	Account       string `json:"account,omitempty"`
	QuotaProject  string `json:"quotaProject,omitempty"`
	Message       string `json:"message,omitempty"`
	Remediation   string `json:"remediation,omitempty"`
}

// AuthStatusReport encapsulates the live authentication status across Gerrit, LUCI, and Buganizer.
type AuthStatusReport struct {
	Mode          AuthMode          `json:"mode"`
	ModeReason    string            `json:"modeReason"`
	Authenticated bool              `json:"authenticated"`
	Healthy       bool              `json:"healthy"`
	Gerrit        ServiceAuthStatus `json:"gerrit"`
	LUCI          ServiceAuthStatus `json:"luci"`
	Buganizer     ServiceAuthStatus `json:"buganizer"`
}

var allowedAuthStatusJSONFields = map[string]bool{
	"mode":          true,
	"modeReason":    true,
	"authenticated": true,
	"healthy":       true,
	"gerrit":        true,
	"luci":          true,
	"buganizer":     true,
}

// ValidateAuthStatusJSONFields validates a comma-separated list of --json fields for 'gh auth status'.
func ValidateAuthStatusJSONFields(fieldsStr string) ([]string, error) {
	trimmed := strings.TrimSpace(fieldsStr)
	if trimmed == "" || trimmed == "*" {
		return nil, nil
	}
	fields := SplitJSONFields(trimmed)
	for _, f := range fields {
		if !allowedAuthStatusJSONFields[f] {
			return nil, fmt.Errorf("unknown JSON field: %q\n\n"+
				"Available fields for gh auth status --json:\n"+
				"  mode, modeReason, authenticated, healthy, gerrit, luci, buganizer", f)
		}
	}
	return fields, nil
}

// IssueTrackerQuotaProjectResolver resolves the GCP quota project for Buganizer status reporting.
// Defaults to ConfiguredIssueTrackerQuotaProject so 'gh auth status' remains fast and side-effect-free.
var IssueTrackerQuotaProjectResolver = ConfiguredIssueTrackerQuotaProject

// CheckAuthStatus probes Gerrit, LUCI Buildbucket/LogDog, and Buganizer authentication
// and returns a structured AuthStatusReport.
func CheckAuthStatus(ctx context.Context, cmd *cobra.Command) (*AuthStatusReport, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	cfg := GetConfig(cmd)
	if cfg == nil {
		cfg = &Config{
			Host: HostFlag,
			Git:  DefaultGitRunner,
		}
	}

	mode, modeReason, err := ResolveAuthMode(ctx, cfg)
	if err != nil {
		return nil, err
	}

	profile := cfg.GetProfile(ctx)
	gerritHost := cfg.GerritHost(ctx)
	if gerritHost == "" && profile != nil {
		gerritHost = CleanGerritHost(profile.DefaultGerritHost())
	}
	if gerritHost == "" {
		gerritHost = "pigweed-review.googlesource.com"
	}
	if cfg.Host == "" && cfg.GerritHost(ctx) == "" {
		cfgCopy := *cfg
		cfgCopy.Host = gerritHost
		cfg = &cfgCopy
		if cmd != nil {
			SetConfig(cmd, cfg)
		}
	}

	// 1. Gerrit status
	gerritMethod, gerritMethodDesc := DescribeGerritAuthMethod(ctx, gerritHost, mode)
	gerritStatus := ServiceAuthStatus{
		Service: "gerrit",
		Host:    gerritHost,
		Method:  gerritMethodDesc,
	}

	if mode == AuthModeNone {
		gerritStatus.Method = "none"
		gerritStatus.Message = "Authentication disabled (--auth-mode=none)"
	} else {
		clearLastGobCurlFallback()
		client, clientErr := NewGerritClient(ctx, cmd)
		if clientErr != nil {
			gerritStatus.Authenticated = false
			gerritStatus.Message = firstLine(clientErr.Error())
		} else {
			acc, _, accErr := client.Accounts.GetAccount(ctx, "self")
			if fbMethod, fbDesc := getLastGobCurlFallback(); fbDesc != "" {
				gerritMethod = fbMethod
				gerritStatus.Method = fbDesc
			}
			if accErr == nil && acc != nil && (acc.AccountID != 0 || acc.Email != "" || acc.Username != "" || acc.Name != "") {
				gerritStatus.Authenticated = true
				acctLabel := FormatAccount(*acc)
				if acc.Email != "" && acc.Email != acctLabel {
					acctLabel = fmt.Sprintf("%s <%s>", acctLabel, acc.Email)
				}
				if acc.AccountID != 0 {
					acctLabel = fmt.Sprintf("%s (account #%d)", acctLabel, acc.AccountID)
				}
				gerritStatus.Account = acctLabel
				gerritStatus.Message = fmt.Sprintf("Logged in to %s as %s", gerritHost, acctLabel)
			} else {
				gerritStatus.Authenticated = false
				if gerritMethod == AuthMethodNone && mode == AuthModeCommunity {
					gerritStatus.Message = fmt.Sprintf("No Gerrit credentials configured for %s (public changes readable; login required to push or review)", gerritHost)
				} else if accErr != nil {
					gerritStatus.Message = fmt.Sprintf("Failed to authenticate with %s (%s)", gerritHost, firstLine(accErr.Error()))
				} else {
					gerritStatus.Message = fmt.Sprintf("Not authenticated with %s", gerritHost)
				}
			}
		}
		if !gerritStatus.Authenticated {
			if mode == AuthModeGoogler {
				gerritStatus.Remediation = fmt.Sprintf("Run 'gcert' to refresh corp SSO credentials, or 'luci-auth login -scopes %q', or configure cookies at https://%s/new-password", DefaultGerritScopes, gerritHost)
			} else {
				gerritStatus.Remediation = fmt.Sprintf("Configure Git cookies at https://%s/new-password or set export GERRIT_TOKEN=\"<token>\"", gerritHost)
			}
		}
	}

	// 2. LUCI Buildbucket / LogDog status
	bbHost := buildbucketHost
	if bbHost == "" {
		bbHost = "cr-buildbucket.appspot.com"
	}
	luciStatus := ServiceAuthStatus{
		Service: "luci",
		Host:    bbHost,
	}
	if mode == AuthModeNone {
		luciStatus.Method = "none"
		luciStatus.Message = "Authentication disabled (--auth-mode=none)"
	} else {
		tok, src, tokErr := LUCITokenResolver(ctx)
		if tokErr == nil && tok != "" {
			luciStatus.Authenticated = true
			luciStatus.Method = src
			luciStatus.Message = fmt.Sprintf("Authenticated with %s via %s (internal & public builders visible)", bbHost, src)
		} else {
			luciStatus.Authenticated = false
			luciStatus.Method = "none"
			if mode == AuthModeGoogler {
				luciStatus.Message = fmt.Sprintf("No active OAuth2 token for %s (required in googler mode to view internal tryjobs)", bbHost)
				luciStatus.Remediation = "Run 'luci-auth login' (or 'gcloud auth application-default login')"
			} else {
				luciStatus.Message = fmt.Sprintf("Unauthenticated on %s (public builders visible; internal builders hidden)", bbHost)
				luciStatus.Remediation = "Optional (for internal builders): run 'luci-auth login'"
			}
		}
	}

	// 3. Buganizer (Google Issue Tracker) status
	issueEndpoint := "https://issuetracker.googleapis.com/v1"
	if profile != nil && profile.IssueTrackerAPIEndpoint() != "" {
		issueEndpoint = profile.IssueTrackerAPIEndpoint()
	}
	issueHost := CleanGerritHost(issueEndpoint)
	buganizerStatus := ServiceAuthStatus{
		Service: "buganizer",
		Host:    issueHost,
	}
	if mode == AuthModeNone {
		buganizerStatus.Method = "none"
		buganizerStatus.Message = "Authentication disabled (--auth-mode=none)"
	} else {
		tok, src, tokErr := IssueTrackerTokenResolver(ctx)
		if tokErr == nil && tok != "" {
			buganizerStatus.Authenticated = true
			methodDesc := src
			if strings.Contains(issueEndpoint, ".corp.googleapis.com") {
				methodDesc = fmt.Sprintf("sso_client + %s", src)
			}
			buganizerStatus.Method = methodDesc
			if IssueTrackerQuotaProjectResolver != nil {
				buganizerStatus.QuotaProject = IssueTrackerQuotaProjectResolver(ctx, tok)
			}
			if buganizerStatus.QuotaProject != "" {
				buganizerStatus.Message = fmt.Sprintf("Authenticated with %s via %s (quota project: %s)", issueHost, methodDesc, buganizerStatus.QuotaProject)
			} else {
				buganizerStatus.Message = fmt.Sprintf("Authenticated with %s via %s", issueHost, methodDesc)
			}
		} else {
			buganizerStatus.Authenticated = false
			buganizerStatus.Method = "none"
			buganizerStatus.Message = fmt.Sprintf("No active Buganizer OAuth2 token for %s", issueHost)
			buganizerStatus.Remediation = "Run 'luci-auth login -scopes \"https://www.googleapis.com/auth/buganizer https://www.googleapis.com/auth/cloud-platform\"'"
		}
	}

	var overallAuth, healthy bool
	switch mode {
	case AuthModeNone:
		overallAuth = false
		healthy = true
	case AuthModeGoogler:
		overallAuth = gerritStatus.Authenticated && luciStatus.Authenticated
		healthy = overallAuth
	default:
		overallAuth = gerritStatus.Authenticated
		healthy = overallAuth
	}

	return &AuthStatusReport{
		Mode:          mode,
		ModeReason:    modeReason,
		Authenticated: overallAuth,
		Healthy:       healthy,
		Gerrit:        gerritStatus,
		LUCI:          luciStatus,
		Buganizer:     buganizerStatus,
	}, nil
}

func firstLine(s string) string {
	s = strings.TrimSpace(s)
	if idx := strings.IndexByte(s, '\n'); idx != -1 {
		return strings.TrimSpace(s[:idx])
	}
	return s
}

func formatAuthStatusReport(r *AuthStatusReport) string {
	var sb strings.Builder
	fmt.Fprintf(&sb, "Authentication Mode: %s (%s)\n\n", r.Mode, r.ModeReason)

	writeService := func(title string, s ServiceAuthStatus, required bool) {
		sym := "✓"
		if !s.Authenticated {
			if required {
				sym = "✗"
			} else {
				sym = "!"
			}
		}
		fmt.Fprintf(&sb, "%s\n", title)
		fmt.Fprintf(&sb, "  %s %s\n", sym, s.Message)
		if s.Method != "" {
			fmt.Fprintf(&sb, "  - Method: %s\n", s.Method)
		}
		if s.QuotaProject != "" {
			fmt.Fprintf(&sb, "  - Quota project: %s\n", s.QuotaProject)
		}
		if s.Remediation != "" {
			fmt.Fprintf(&sb, "  - Remediation: %s\n", s.Remediation)
		}
	}

	writeService(r.Gerrit.Host+" (Gerrit)", r.Gerrit, r.Mode != AuthModeNone)
	sb.WriteString("\n")
	writeService(r.LUCI.Host+" (LUCI Buildbucket & LogDog)", r.LUCI, r.Mode == AuthModeGoogler)
	sb.WriteString("\n")
	writeService(r.Buganizer.Host+" (Google Issue Tracker)", r.Buganizer, false)

	return sb.String()
}

func authReportToJSONMap(r *AuthStatusReport, fields []string) any {
	full := map[string]any{
		"mode":          r.Mode,
		"modeReason":    r.ModeReason,
		"authenticated": r.Authenticated,
		"healthy":       r.Healthy,
		"gerrit":        r.Gerrit,
		"luci":          r.LUCI,
		"buganizer":     r.Buganizer,
	}
	if len(fields) == 0 {
		return full
	}
	filtered, err := filterData(full, fields)
	if err != nil {
		return full
	}
	return filtered
}

func runAuthStatus(cmd *cobra.Command, args []string) error {
	fields, err := ValidateAuthStatusJSONFields(authStatusJSON)
	if err != nil {
		return err
	}

	report, err := CheckAuthStatus(cmd.Context(), cmd)
	if err != nil {
		return err
	}

	if strings.TrimSpace(authStatusJSON) != "" {
		projected := authReportToJSONMap(report, fields)
		data, err := json.MarshalIndent(projected, "", "  ")
		if err != nil {
			return fmt.Errorf("failed to marshal auth status JSON: %w", err)
		}
		fmt.Fprintln(cmd.OutOrStdout(), string(data))
	} else {
		fmt.Fprint(cmd.OutOrStdout(), formatAuthStatusReport(report))
	}

	if !report.Healthy {
		if report.Mode == AuthModeGoogler {
			var missing []string
			if !report.Gerrit.Authenticated {
				missing = append(missing, fmt.Sprintf("Gerrit (%s): %s", report.Gerrit.Host, report.Gerrit.Remediation))
			}
			if !report.LUCI.Authenticated {
				missing = append(missing, fmt.Sprintf("LUCI (%s): %s", report.LUCI.Host, report.LUCI.Remediation))
			}
			return NewExitCodeError(ExitCodeAuth, "googler authentication check failed:\n  - %s", strings.Join(missing, "\n  - "))
		}
		return NewExitCodeError(ExitCodeAuth, "authentication check failed: not logged in to %s (%s)", report.Gerrit.Host, report.Gerrit.Remediation)
	}
	return nil
}

// AuthCmd is the top-level 'gh auth' command.
var AuthCmd = &cobra.Command{
	Use:          "auth",
	Short:        "Inspect authentication state across Gerrit, LUCI, and Buganizer",
	SilenceUsage: true,
	Args:         cobra.NoArgs,
	RunE:         runAuthStatus,
}

var authStatusCmd = &cobra.Command{
	Use:          "status",
	Short:        "View authentication status across Gerrit, LUCI Buildbucket/LogDog, and Buganizer",
	SilenceUsage: true,
	Args:         cobra.NoArgs,
	RunE:         runAuthStatus,
}

func init() {
	AuthCmd.PersistentFlags().StringVar(&authStatusJSON, "json", "", "Output JSON (optionally specify comma-separated fields: mode,modeReason,authenticated,gerrit,luci,buganizer)")
	AuthCmd.PersistentFlags().Lookup("json").NoOptDefVal = "*"

	AuthCmd.AddCommand(authStatusCmd)
	RootCmd.AddCommand(AuthCmd)
}
