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
	"maps"
	"net/http"
	"slices"
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
			return nil, &UnknownJSONFieldsError{Unknown: []string{f}, Available: slices.Sorted(maps.Keys(allowedAuthStatusJSONFields))}
		}
	}
	return fields, nil
}

func luciStatusRemediation(mode AuthMode) string {
	if hasUsableSSOCredHelper() {
		return fmt.Sprintf("Run 'gcert' to refresh corp SSO credentials, or '%s'", luciAuthLoginCommand(mode))
	}
	return fmt.Sprintf("Run '%s'", luciAuthLoginCommand(mode))
}

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

	// Token resolvers read the auth mode, and so the luci-auth scope set,
	// from the context's Config; give them the one this report used.
	cfgCtx := context.WithValue(ctx, configKey, cfg)

	// 1. Gerrit status
	res, _ := resolveGerritAuth(cfgCtx, gerritHost, mode)
	gerritStatus := ServiceAuthStatus{
		Service: "gerrit",
		Host:    gerritHost,
		Method:  res.desc,
	}

	if mode == AuthModeNone {
		gerritStatus.Method = "none"
		gerritStatus.Message = "Authentication disabled (--auth-mode=none)"
	} else {
		client, clientErr := NewGerritClient(ctx, cmd)
		if clientErr != nil {
			gerritStatus.Authenticated = false
			gerritStatus.Message = firstLine(clientErr.Error())
		} else {
			acc, accErr := client.GetSelfAccount(ctx)
			if accErr == nil && acc != nil && (acc.AccountID != 0 || acc.Email != "" || acc.Username != "" || acc.Name != "") {
				gerritStatus.Authenticated = true
				acctLabel := FormatAccount(*acc)
				if acc.Email != "" && acc.Email != acctLabel {
					acctLabel = fmt.Sprintf("%s <%s>", acctLabel, acc.Email)
				}
				// FormatAccount already falls back to "account #N" when only
				// the ID is known; don't repeat it.
				if idLabel := fmt.Sprintf("account #%d", acc.AccountID); acc.AccountID != 0 && acctLabel != idLabel {
					acctLabel = fmt.Sprintf("%s (%s)", acctLabel, idLabel)
				}
				gerritStatus.Account = acctLabel
				gerritStatus.Message = fmt.Sprintf("Logged in to %s as %s", gerritHost, acctLabel)
			} else {
				gerritStatus.Authenticated = false
				if res.method == AuthMethodNone && mode == AuthModeCommunity {
					if res.cause != nil {
						gerritStatus.Message = fmt.Sprintf("No Gerrit credentials configured for %s (%s; public changes readable, login required to push or review)", gerritHost, firstLine(res.cause.Error()))
					} else {
						gerritStatus.Message = fmt.Sprintf("No Gerrit credentials configured for %s (public changes readable; login required to push or review)", gerritHost)
					}
				} else if IsGerritHTTPStatus(accErr, http.StatusBadRequest) {
					gerritStatus.Message = fmt.Sprintf("No Gerrit account found on %s for current credentials (%s)", gerritHost, firstLine(accErr.Error()))
					gerritStatus.Remediation = fmt.Sprintf("Visit https://%s in a browser and click 'Sign in' once to create your Gerrit account, and sign the CLA at https://cla.developers.google.com/", gerritHost)
				} else if accErr != nil {
					gerritStatus.Message = fmt.Sprintf("Failed to authenticate with %s (%s)", gerritHost, firstLine(accErr.Error()))
				} else {
					gerritStatus.Message = fmt.Sprintf("Not authenticated with %s", gerritHost)
				}
			}
		}
		if !gerritStatus.Authenticated && gerritStatus.Remediation == "" {
			gerritStatus.Remediation = gerritStatusRemediation(gerritHost, mode)
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
		tok, src, tokErr := LUCITokenResolver(cfgCtx)
		if tokErr == nil && tok != "" {
			luciStatus.Authenticated = true
			luciStatus.Method = src
			luciStatus.Message = fmt.Sprintf("Authenticated with %s via %s (internal & public builders visible)", bbHost, src)
		} else {
			luciStatus.Authenticated = false
			luciStatus.Method = "none"
			if mode == AuthModeGoogler {
				luciStatus.Message = fmt.Sprintf("No active OAuth2 token for %s (required in googler mode to view internal tryjobs)", bbHost)
				luciStatus.Remediation = luciStatusRemediation(mode)
			} else {
				luciStatus.Message = fmt.Sprintf("Unauthenticated on %s (public builders visible; internal builders hidden)", bbHost)
				luciStatus.Remediation = fmt.Sprintf("Optional (for internal builders): run '%s'", luciAuthLoginCommand(mode))
			}
		}
	}

	// 3. Buganizer (Google Issue Tracker) status
	issueEndpoint := DefaultIssueTrackerEndpoint
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
	} else if mode == AuthModeCommunity && !issueTrackerTokenEnvSet() {
		webURL := issueTrackerCommunityWebURL(profile)
		buganizerStatus.Authenticated = false
		buganizerStatus.Method = "none"
		buganizerStatus.Message = fmt.Sprintf("Google Issue Tracker CLI requires Google Corp access (%s)", issueHost)
		buganizerStatus.Remediation = fmt.Sprintf("View or file issues on the web at %s. Googlers misdetected as community: export GH_ISH_AUTH_MODE=googler", webURL)
	} else {
		tok, src, tokErr := IssueTrackerTokenResolver(cfgCtx)
		gossoPath := findGossoBinary()
		if tokErr == nil && tok != "" && gossoPath == "" {
			buganizerStatus.Authenticated = false
			buganizerStatus.Method = "none"
			buganizerStatus.Message = fmt.Sprintf("gosso not found in PATH for %s", issueHost)
			buganizerStatus.Remediation = "Run 'sudo apt install gosso' (Linux) or 'sudo mule install gosso' (macOS), then 'gcert'"
		} else if tokErr == nil && tok != "" {
			buganizerStatus.Authenticated = true
			methodDesc := fmt.Sprintf("gosso + %s", src)
			buganizerStatus.Method = methodDesc
			buganizerStatus.QuotaProject = ConfiguredIssueTrackerQuotaProject(cfgCtx)
			if buganizerStatus.QuotaProject != "" {
				buganizerStatus.Message = fmt.Sprintf("Authenticated with %s via %s (quota project: %s)", issueHost, methodDesc, buganizerStatus.QuotaProject)
			} else {
				buganizerStatus.Message = fmt.Sprintf("Authenticated with %s via %s", issueHost, methodDesc)
			}
		} else {
			buganizerStatus.Authenticated = false
			buganizerStatus.Method = "none"
			buganizerStatus.Message = fmt.Sprintf("No active Buganizer OAuth2 token for %s", issueHost)
			buganizerStatus.Remediation = luciStatusRemediation(mode)
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
		sym := GlyphPass
		if !s.Authenticated {
			if required {
				sym = GlyphFail
			} else {
				sym = GlyphWarn
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
	sb.WriteString("\n")

	if r.Healthy {
		fmt.Fprintf(&sb, "Verdict: authenticated (%s mode; all required services healthy)\n", r.Mode)
		sb.WriteString("Next:    gh pr status\n")
	} else {
		fmt.Fprintf(&sb, "Verdict: unauthenticated (%s mode; login required)\n", r.Mode)
		nextCmd := "gh auth login"
		if !r.Gerrit.Authenticated && r.Gerrit.Remediation != "" {
			nextCmd = r.Gerrit.Remediation
		} else if !r.LUCI.Authenticated && r.LUCI.Remediation != "" {
			nextCmd = r.LUCI.Remediation
		}
		fmt.Fprintf(&sb, "Next:    %s\n", nextCmd)
	}

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
	Annotations:  map[string]string{KindAnnotation: KindView},
	Short:        "Inspect authentication state across Gerrit, LUCI, and Buganizer",
	SilenceUsage: true,
	Args:         cobra.NoArgs,
	RunE:         runAuthStatus,
}

var authStatusCmd = &cobra.Command{
	Use:          "status",
	Annotations:  map[string]string{KindAnnotation: KindView},
	Short:        "View authentication status across Gerrit, LUCI Buildbucket/LogDog, and Buganizer",
	SilenceUsage: true,
	Args:         cobra.NoArgs,
	RunE:         runAuthStatus,
}

func init() {
	AuthCmd.PersistentFlags().StringVar(&authStatusJSON, "json", "", "Output JSON (optionally specify comma-separated fields: mode,modeReason,authenticated,healthy,gerrit,luci,buganizer)")
	AuthCmd.PersistentFlags().Lookup("json").NoOptDefVal = "*"

	AuthCmd.AddCommand(authStatusCmd)
	RootCmd.AddCommand(AuthCmd)
}
