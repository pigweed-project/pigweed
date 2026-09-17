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
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/spf13/cobra"
)

// FlexInt64 unmarshals from either a JSON string ("1194524") or JSON number (1194524)
// and marshals as a JSON string per OnePlatform JSONPB int64 convention.
type FlexInt64 int64

func (f FlexInt64) MarshalJSON() ([]byte, error) {
	return json.Marshal(strconv.FormatInt(int64(f), 10))
}

func (f *FlexInt64) UnmarshalJSON(data []byte) error {
	var s string
	if err := json.Unmarshal(data, &s); err == nil {
		s = strings.TrimSpace(s)
		if s == "" {
			*f = 0
			return nil
		}
		v, err := strconv.ParseInt(s, 10, 64)
		if err != nil {
			return fmt.Errorf("invalid FlexInt64 string %q: %w", s, err)
		}
		*f = FlexInt64(v)
		return nil
	}
	var v int64
	if err := json.Unmarshal(data, &v); err != nil {
		return fmt.Errorf("invalid FlexInt64 value %s: %w", string(data), err)
	}
	*f = FlexInt64(v)
	return nil
}

// BuganizerIssue represents an issue resource in Google Issue Tracker v1.
type BuganizerIssue struct {
	IssueID           FlexInt64         `json:"issueId"`
	CreatedTime       time.Time         `json:"createdTime"`
	ModifiedTime      time.Time         `json:"modifiedTime"`
	ResolvedTime      *time.Time        `json:"resolvedTime,omitempty"`
	State             BuganizerState    `json:"issueState"`
	Description       *BuganizerComment `json:"description,omitempty"`
	LegacyDescription *BuganizerComment `json:"issueComment,omitempty"`
}

// EffectiveDescription returns the issue description from either the `description` or `issueComment` field.
func (i *BuganizerIssue) EffectiveDescription() *BuganizerComment {
	if i == nil {
		return nil
	}
	if i.Description != nil {
		return i.Description
	}
	return i.LegacyDescription
}

// BuganizerState represents the mutable metadata state of a Buganizer issue.
type BuganizerState struct {
	ComponentID       FlexInt64       `json:"componentId,omitempty"`
	Type              string          `json:"type,omitempty"`     // BUG, FEATURE_REQUEST, TASK, etc.
	Status            string          `json:"status,omitempty"`   // NEW, ASSIGNED, ACCEPTED, FIXED, etc.
	Priority          string          `json:"priority,omitempty"` // P0..P4
	Severity          string          `json:"severity,omitempty"` // S0..S4
	Title             string          `json:"title,omitempty"`
	Reporter          *BuganizerUser  `json:"reporter,omitempty"`
	Assignee          *BuganizerUser  `json:"assignee,omitempty"`
	CCs               []BuganizerUser `json:"ccs,omitempty"`
	HotlistIDs        []FlexInt64     `json:"hotlistIds,omitempty"`
	CanonicalIssueID  FlexInt64       `json:"canonicalIssueId,omitempty"`
	BlockedByIssueIDs []FlexInt64     `json:"blockedByIssueIds,omitempty"`
	BlockingIssueIDs  []FlexInt64     `json:"blockingIssueIds,omitempty"`
}

// BuganizerUser represents a user account in Google Issue Tracker.
type BuganizerUser struct {
	EmailAddress string `json:"emailAddress"`
}

// BuganizerComment represents a comment entry on a Buganizer issue.
type BuganizerComment struct {
	CommentNumber  int            `json:"commentNumber,omitempty"`
	Comment        string         `json:"comment"`
	Author         *BuganizerUser `json:"author,omitempty"`
	OriginalAuthor *BuganizerUser `json:"originalAuthor,omitempty"`
	LastEditor     *BuganizerUser `json:"lastEditor,omitempty"`
	CreatedTime    time.Time      `json:"createdTime,omitempty"`
}

// EffectiveAuthorEmail returns the comment author's email from author, originalAuthor, or lastEditor.
func (c *BuganizerComment) EffectiveAuthorEmail() string {
	if c == nil {
		return "unknown"
	}
	if c.Author != nil && c.Author.EmailAddress != "" {
		return c.Author.EmailAddress
	}
	if c.OriginalAuthor != nil && c.OriginalAuthor.EmailAddress != "" {
		return c.OriginalAuthor.EmailAddress
	}
	if c.LastEditor != nil && c.LastEditor.EmailAddress != "" {
		return c.LastEditor.EmailAddress
	}
	return "unknown"
}

// CreateIssueRequest is the payload for POST /v1/issues.
type CreateIssueRequest struct {
	IssueState   BuganizerState    `json:"issueState"`
	IssueComment *BuganizerComment `json:"issueComment,omitempty"`
}

// ModifyIssueRequest is the payload for POST /v1/issues/{issueId}:modify.
type ModifyIssueRequest struct {
	AddMask      string            `json:"addMask,omitempty"`
	Add          *BuganizerState   `json:"add,omitempty"`
	RemoveMask   string            `json:"removeMask,omitempty"`
	Remove       *BuganizerState   `json:"remove,omitempty"`
	IssueComment *BuganizerComment `json:"issueComment,omitempty"`
}

// ListIssuesResponse is the payload returned by GET /v1/issues.
type ListIssuesResponse struct {
	Issues        []*BuganizerIssue `json:"issues"`
	NextPageToken string            `json:"nextPageToken,omitempty"`
}

// ListIssueCommentsResponse is the payload returned by GET /v1/issues/{issueId}/comments.
type ListIssueCommentsResponse struct {
	IssueComments []BuganizerComment `json:"issueComments"`
	NextPageToken string             `json:"nextPageToken,omitempty"`
}

// IssueTrackerClient communicates with the Google Issue Tracker v1 REST API.
type IssueTrackerClient struct {
	Endpoint             string
	HTTPClient           *http.Client
	TokenProvider        func(ctx context.Context) (string, error)
	QuotaProjectProvider func(ctx context.Context, token string) string
}

const (
	// DefaultIssueTrackerEndpoint is the public Google Issue Tracker v1 REST API
	// base URL used by external contributors and standard OAuth2 clients.
	DefaultIssueTrackerEndpoint = "https://issuetracker.googleapis.com/v1"

	// DefaultCorpIssueTrackerEndpoint is the first-party (1P / internal) Issue
	// Tracker v1 REST API base URL.
	//
	// 1P Internal Architecture Note:
	// While external contributors access Buganizer via issuetracker.googleapis.com
	// using standard OAuth2 bearer tokens, internal developer workstations
	// have access to both public components and internal/partner-restricted
	// components. Accessing internal components over REST requires traversing
	// Google's UberProxy via `sso_client` ticket exchange against the
	// `issuetracker.corp.googleapis.com` endpoint.
	DefaultCorpIssueTrackerEndpoint = "https://issuetracker.corp.googleapis.com/v1"
)

// NewIssueTrackerClient creates a client for the Google Issue Tracker v1 REST API.
//
// Endpoint & Transport Selection:
//   - If `sso_client` is present on PATH (1P workstation environment) and no
//     custom external endpoint was forced, the client automatically targets
//     DefaultCorpIssueTrackerEndpoint and wraps HTTP transport in
//     ssoClientRoundTripper to handle UberProxy authentication transparently.
//   - Otherwise (external contributors, CI environments without sso_client),
//     the client targets DefaultIssueTrackerEndpoint over standard HTTPS.
func NewIssueTrackerClient(endpoint string, httpClient *http.Client) *IssueTrackerClient {
	endpoint = strings.TrimRight(strings.TrimSpace(endpoint), "/")
	hasSSOClient := false
	if path, err := LookPathFn("sso_client"); err == nil && path != "" {
		hasSSOClient = true
	}
	if endpoint == "" || (endpoint == DefaultIssueTrackerEndpoint && hasSSOClient) {
		if hasSSOClient {
			endpoint = DefaultCorpIssueTrackerEndpoint
		} else {
			endpoint = DefaultIssueTrackerEndpoint
		}
	}
	if httpClient == nil {
		if strings.Contains(endpoint, ".corp.googleapis.com") && hasSSOClient {
			httpClient = &http.Client{
				Transport: &ssoClientRoundTripper{},
				Timeout:   45 * time.Second,
			}
		} else {
			httpClient = &http.Client{Timeout: 30 * time.Second}
		}
	}
	return &IssueTrackerClient{
		Endpoint:             endpoint,
		HTTPClient:           httpClient,
		TokenProvider:        DefaultIssueTrackerToken,
		QuotaProjectProvider: DefaultIssueTrackerQuotaProject,
	}
}

// ssoClientRoundTripper implements http.RoundTripper for 1P internal requests
// targeting `*.corp.googleapis.com`. It invokes the `sso_client` binary to
// attach UberProxy security tickets alongside the user's OAuth2 bearer token,
// parsing the raw HTTP response back into an *http.Response.
type ssoClientRoundTripper struct {
	fallback http.RoundTripper
}

func (rt *ssoClientRoundTripper) RoundTrip(req *http.Request) (*http.Response, error) {
	if !strings.Contains(req.URL.Host, ".corp.googleapis.com") {
		fb := rt.fallback
		if fb == nil {
			fb = http.DefaultTransport
		}
		return fb.RoundTrip(req)
	}

	const headerSep = "|||"
	args := []string{
		"--dump_header",
		"--location",
		"--header_sep=" + headerSep,
		"--method=" + req.Method,
	}

	var hdrs []string
	for k, vals := range req.Header {
		for _, v := range vals {
			hdrs = append(hdrs, fmt.Sprintf("%s: %s", k, v))
		}
	}
	if len(hdrs) > 0 {
		args = append(args, "--headers="+strings.Join(hdrs, headerSep))
	}

	if req.Body != nil {
		bodyBytes, err := io.ReadAll(req.Body)
		if err != nil {
			return nil, fmt.Errorf("failed to read request body for sso_client: %w", err)
		}
		if len(bodyBytes) > 0 {
			tmpFile, err := os.CreateTemp("", "ghish-sso-body-*.json")
			if err != nil {
				return nil, fmt.Errorf("failed to create temp file for sso_client payload: %w", err)
			}
			tmpPath := tmpFile.Name()
			defer os.Remove(tmpPath)
			if _, err := tmpFile.Write(bodyBytes); err != nil {
				tmpFile.Close()
				return nil, fmt.Errorf("failed to write sso_client payload: %w", err)
			}
			tmpFile.Close()
			args = append(args, "--data_file="+tmpPath)
		}
	}

	args = append(args, "--url="+req.URL.String())
	cmd := exec.CommandContext(req.Context(), "sso_client", args...)
	out, err := cmd.CombinedOutput()
	if err != nil && !bytes.Contains(out, []byte("HTTP/")) {
		return nil, fmt.Errorf("sso_client execution failed for %s %s: %w (output: %s)",
			req.Method, req.URL.String(), err, strings.TrimSpace(string(out)))
	}
	return parseSSOClientResponse(out, req)
}

func parseSSOClientResponse(out []byte, req *http.Request) (*http.Response, error) {
	idx := bytes.Index(out, []byte("HTTP/"))
	if idx < 0 {
		return nil, fmt.Errorf("sso_client returned non-HTTP output: %s", strings.TrimSpace(string(out)))
	}
	raw := out[idx:]
	if bytes.HasPrefix(raw, []byte("HTTP/2 ")) {
		raw = append([]byte("HTTP/1.1 "), raw[len("HTTP/2 "):]...)
	}
	resp, err := http.ReadResponse(bufio.NewReader(bytes.NewReader(raw)), req)
	if err != nil {
		return nil, fmt.Errorf("failed to parse sso_client HTTP response: %w", err)
	}
	return resp, nil
}

var (
	quotaProjectMu     sync.Mutex
	cachedQuotaProject string
)

// DefaultPigweedQuotaProject is the shared Pigweed GCP project configured to
// host One Platform quota buckets for Buganizer API calls (zero billing cost;
// used strictly for One Platform QPS rate-limit accounting).
//
// 1P Internal Infrastructure Note:
// Google's One Platform API Gateway (`*.googleapis.com`) requires every REST
// request to identify both:
//  1. A User Identity (`Authorization: Bearer <token>` + `sso_client` ticket),
//     used by Buganizer to enforce component ACLs and attribute comment authors.
//  2. A Client/Quota Identity (`X-Goog-User-Project`), used by One Platform
//     solely for per-project QPS rate-limit accounting because CLI tools
//     (`luci-auth` and `gcloud`) share generic public OAuth client IDs.
//
// To provide a zero-setup experience for 1P developers, `pigweed-gce` can be
// configured in Gong (`project.pigweed-gce/apis.yaml` and `iam_policy.yaml`)
// with `issuetracker.corp.googleapis.com` and `issuetracker.googleapis.com`
// enabled and `roles/serviceusage.serviceUsageConsumer` granted to internal
// developers (`domain:google.com` or team groups). When enabled, `probeQuotaProject`
// succeeds in ~40ms and caches `pigweed-gce` in `git config ghish.quotaproject`.
// Notice that `serviceUsageConsumer` grants zero access to GCP compute/storage
// resources and does not alter Buganizer user permissions.
const DefaultPigweedQuotaProject = "pigweed-gce"

// DefaultIssueTrackerQuotaProject resolves a GCP project ID for X-Goog-User-Project.
//
// Resolution Cascade (strictly READ-ONLY; never mutates GCP services or IAM):
//  1. Explicit overrides: `GHISH_QUOTA_PROJECT` or `GOOGLE_CLOUD_QUOTA_PROJECT` env vars.
//  2. Cached repository/global setting: `git config ghish.quotaproject`.
//  3. Explicit gcloud billing quota setting: `gcloud config get-value billing/quota_project`.
//  4. Shared 1P team default: `DefaultPigweedQuotaProject` ("pigweed-gce"),
//     verified via a fast read-only Service Usage state check.
//  5. Active gcloud default project: `gcloud config get-value project`.
//  6. Read-only scan of existing active GCP projects (`GET /v1/projects`) where
//     the Issue Tracker API is already enabled.
func DefaultIssueTrackerQuotaProject(ctx context.Context, token string) string {
	for _, envVar := range []string{"GHISH_QUOTA_PROJECT", "GOOGLE_CLOUD_QUOTA_PROJECT"} {
		if v := strings.TrimSpace(os.Getenv(envVar)); v != "" {
			return v
		}
	}

	quotaProjectMu.Lock()
	if cachedQuotaProject != "" {
		v := cachedQuotaProject
		quotaProjectMu.Unlock()
		return v
	}
	quotaProjectMu.Unlock()

	if out, err := exec.CommandContext(ctx, "git", "config", "--get", "ghish.quotaproject").Output(); err == nil {
		if v := strings.TrimSpace(string(out)); v != "" {
			quotaProjectMu.Lock()
			cachedQuotaProject = v
			quotaProjectMu.Unlock()
			return v
		}
	}

	if token == "" {
		return ""
	}

	cacheAndReturn := func(projID string) string {
		quotaProjectMu.Lock()
		cachedQuotaProject = projID
		quotaProjectMu.Unlock()
		_ = exec.CommandContext(ctx, "git", "config", "ghish.quotaproject", projID).Run()
		return projID
	}

	// Priority 1: Check explicit gcloud billing/quota_project configuration.
	if _, err := LookPathFn("gcloud"); err == nil {
		if out, err := exec.CommandContext(ctx, "gcloud", "config", "get-value", "billing/quota_project").Output(); err == nil {
			if v := strings.TrimSpace(string(out)); v != "" && v != "(unset)" {
				if probeQuotaProject(ctx, token, v) {
					return cacheAndReturn(v)
				}
			}
		}
	}

	// Priority 2: Probe the shared Pigweed team quota project (pigweed-gce).
	if probeQuotaProject(ctx, token, DefaultPigweedQuotaProject) {
		return cacheAndReturn(DefaultPigweedQuotaProject)
	}

	// Priority 3: Check active gcloud default project.
	if _, err := LookPathFn("gcloud"); err == nil {
		if out, err := exec.CommandContext(ctx, "gcloud", "config", "get-value", "project").Output(); err == nil {
			if v := strings.TrimSpace(string(out)); v != "" && v != "(unset)" && v != DefaultPigweedQuotaProject {
				if probeQuotaProject(ctx, token, v) {
					return cacheAndReturn(v)
				}
			}
		}
	}

	// Priority 4: Read-only discovery of existing active GCP projects where
	// the user already has Issue Tracker API enabled and Service Usage Consumer access.
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, "https://cloudresourcemanager.googleapis.com/v1/projects?pageSize=20", nil)
	if err != nil {
		return ""
	}
	req.Header.Set("Authorization", "Bearer "+token)
	client := &http.Client{Timeout: 5 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		return ""
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return ""
	}

	var listResp struct {
		Projects []struct {
			ProjectID      string `json:"projectId"`
			LifecycleState string `json:"lifecycleState"`
		} `json:"projects"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&listResp); err != nil {
		return ""
	}

	user := strings.ToLower(strings.TrimSpace(os.Getenv("USER")))
	var preferred, others []string
	for _, p := range listResp.Projects {
		if p.LifecycleState != "ACTIVE" || p.ProjectID == "" || p.ProjectID == DefaultPigweedQuotaProject {
			continue
		}
		if strings.HasPrefix(p.ProjectID, "google.com:") || strings.HasPrefix(p.ProjectID, "loas-") {
			others = append(others, p.ProjectID)
			continue
		}
		if user != "" && strings.Contains(strings.ToLower(p.ProjectID), user) {
			preferred = append(preferred, p.ProjectID)
		} else {
			others = append(others, p.ProjectID)
		}
	}
	for _, projID := range append(preferred, others...) {
		if probeQuotaProject(ctx, token, projID) {
			return cacheAndReturn(projID)
		}
	}

	return ""
}

func probeQuotaProject(ctx context.Context, token, projID string) bool {
	if projID == "" || token == "" {
		return false
	}
	serviceName := "issuetracker.googleapis.com"
	if _, err := LookPathFn("sso_client"); err == nil {
		serviceName = "issuetracker.corp.googleapis.com"
	}
	svcURL := fmt.Sprintf("https://serviceusage.googleapis.com/v1/projects/%s/services/%s", projID, serviceName)
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, svcURL, nil)
	if err != nil {
		return false
	}
	req.Header.Set("Authorization", "Bearer "+token)
	client := &http.Client{Timeout: 3 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		return false
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return false
	}
	var svc struct {
		State string `json:"state"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&svc); err != nil {
		return false
	}
	return svc.State == "ENABLED"
}

func findLuciAuthBinary(ctx context.Context) string {
	if path, err := LookPathFn("luci-auth"); err == nil && path != "" {
		return path
	}
	var candidates []string
	if envRoot := strings.TrimSpace(os.Getenv("PW_ENVIRONMENT_ROOT")); envRoot != "" {
		candidates = append(candidates,
			filepath.Join(envRoot, "cipd", "packages", "luci", "luci-auth"),
			filepath.Join(envRoot, "cipd", "packages", "pigweed", "bin", "luci-auth"),
		)
	}
	if cwd, err := os.Getwd(); err == nil {
		candidates = append(candidates,
			filepath.Join(cwd, "environment", "cipd", "packages", "luci", "luci-auth"),
			filepath.Join(cwd, ".environment", "cipd", "packages", "luci", "luci-auth"),
		)
	}
	if out, err := exec.CommandContext(ctx, "git", "rev-parse", "--git-common-dir").Output(); err == nil {
		commonGitDir := strings.TrimSpace(string(out))
		if commonGitDir != "" {
			if abs, err := filepath.Abs(commonGitDir); err == nil {
				repoRoot := filepath.Dir(abs)
				candidates = append(candidates,
					filepath.Join(repoRoot, "environment", "cipd", "packages", "luci", "luci-auth"),
					filepath.Join(repoRoot, ".environment", "cipd", "packages", "luci", "luci-auth"),
				)
			}
		}
	}
	for _, cand := range candidates {
		if path, err := LookPathFn(cand); err == nil && path != "" {
			return path
		}
	}
	return ""
}

var (
	issueTokenMu           sync.Mutex
	cachedIssueToken       string
	cachedIssueTokenExpiry time.Time
)

// DefaultIssueTrackerToken resolves an OAuth2 access token with Buganizer scope.
func DefaultIssueTrackerToken(ctx context.Context) (string, error) {
	if tok := strings.TrimSpace(os.Getenv("GHISH_ISSUE_TOKEN")); tok != "" {
		return tok, nil
	}
	if tok := strings.TrimSpace(os.Getenv("BUGANIZER_TOKEN")); tok != "" {
		return tok, nil
	}

	issueTokenMu.Lock()
	if cachedIssueToken != "" && time.Now().Before(cachedIssueTokenExpiry) {
		tok := cachedIssueToken
		issueTokenMu.Unlock()
		return tok, nil
	}
	issueTokenMu.Unlock()

	cacheToken := func(tok string) (string, error) {
		issueTokenMu.Lock()
		cachedIssueToken = tok
		cachedIssueTokenExpiry = time.Now().Add(5 * time.Minute)
		issueTokenMu.Unlock()
		return tok, nil
	}

	// Priority 1: luci-auth with buganizer scope (including Pigweed CIPD & git worktree discovery)
	if luciAuthBin := findLuciAuthBinary(ctx); luciAuthBin != "" {
		cmd := exec.CommandContext(ctx, luciAuthBin, "token", "-scopes", "https://www.googleapis.com/auth/buganizer https://www.googleapis.com/auth/cloud-platform")
		if out, err := cmd.Output(); err == nil {
			if tok := strings.TrimSpace(string(out)); tok != "" {
				return cacheToken(tok)
			}
		}
	}

	// Priority 2: gcloud application-default or active account token
	if _, err := LookPathFn("gcloud"); err == nil {
		for _, args := range [][]string{
			{"auth", "application-default", "print-access-token"},
			{"auth", "print-access-token"},
		} {
			cmd := exec.CommandContext(ctx, "gcloud", args...)
			if out, err := cmd.Output(); err == nil {
				if tok := strings.TrimSpace(string(out)); tok != "" {
					return cacheToken(tok)
				}
			}
		}
	}

	return "", fmt.Errorf("failed to authenticate with Google Issue Tracker: no OAuth2 token found.\n\n" +
		"Cause: Neither GHISH_ISSUE_TOKEN, luci-auth, nor gcloud returned an active access token.\n\n" +
		"Remediation:\n" +
		"  1. Authenticate via LUCI Auth (recommended for Pigweed/Fuchsia developers):\n" +
		"     luci-auth login -scopes \"https://www.googleapis.com/auth/buganizer https://www.googleapis.com/auth/cloud-platform\"\n" +
		"  2. Or authenticate via Google Cloud SDK:\n" +
		"     gcloud auth application-default login --scopes=\"https://www.googleapis.com/auth/buganizer,https://www.googleapis.com/auth/cloud-platform\"\n" +
		"  3. Or provide a token explicitly:\n" +
		"     export GHISH_ISSUE_TOKEN=\"<token>\"")
}

// NewIssueTrackerClientForCommand creates an IssueTrackerClient for a CLI command using the active profile.
var NewIssueTrackerClientForCommand = func(ctx context.Context, cmd *cobra.Command) (*IssueTrackerClient, error) {
	cfg := GetConfig(cmd)
	if cfg == nil {
		cfg = &Config{
			Host: HostFlag,
			Git:  DefaultGitRunner,
		}
	}
	profile := cfg.GetProfile(ctx)
	return NewIssueTrackerClient(profile.IssueTrackerAPIEndpoint(), nil), nil
}

func (c *IssueTrackerClient) doJSON(ctx context.Context, method, path string, reqBody, respBody any) error {
	token, err := c.TokenProvider(ctx)
	if err != nil {
		return err
	}

	var bodyReader io.Reader
	if reqBody != nil {
		jsonBytes, err := json.Marshal(reqBody)
		if err != nil {
			return fmt.Errorf("failed to marshal request payload for %s %s: %w", method, path, err)
		}
		bodyReader = bytes.NewReader(jsonBytes)
	}

	fullURL := c.Endpoint + path
	req, err := http.NewRequestWithContext(ctx, method, fullURL, bodyReader)
	if err != nil {
		return fmt.Errorf("failed to create HTTP request for %s %s: %w", method, fullURL, err)
	}
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Accept", "application/json")
	if c.QuotaProjectProvider != nil {
		if qp := strings.TrimSpace(c.QuotaProjectProvider(ctx, token)); qp != "" {
			req.Header.Set("X-Goog-User-Project", qp)
		}
	}
	if reqBody != nil {
		req.Header.Set("Content-Type", "application/json")
	}

	resp, err := c.HTTPClient.Do(req)
	if err != nil {
		return fmt.Errorf("HTTP request failed for %s %s: %w", method, fullURL, err)
	}
	defer resp.Body.Close()

	respBytes, err := io.ReadAll(resp.Body)
	if err != nil {
		return fmt.Errorf("failed to read HTTP response from %s %s: %w", method, fullURL, err)
	}

	if resp.StatusCode == http.StatusUnauthorized || resp.StatusCode == http.StatusForbidden {
		rawErr := strings.TrimSpace(string(respBytes))
		if strings.Contains(rawErr, "USER_PROJECT_DENIED") ||
			strings.Contains(rawErr, "SERVICE_DISABLED") ||
			strings.Contains(rawErr, "has not been used in project") ||
			strings.Contains(rawErr, "cannot be identified with a client project") {
			quotaProjectMu.Lock()
			cachedQuotaProject = ""
			quotaProjectMu.Unlock()
			if strings.Contains(c.Endpoint, "googleapis.com") {
				_ = exec.CommandContext(ctx, "git", "config", "--unset", "ghish.quotaproject").Run()
			}
			svcName := "issuetracker.googleapis.com"
			if strings.Contains(c.Endpoint, ".corp.googleapis.com") {
				svcName = "issuetracker.corp.googleapis.com"
			}
			return fmt.Errorf("Google Issue Tracker quota project error (HTTP %d) for %s %s.\n\n"+
				"Cause: Google's API gateway (OnePlatform) requires a GCP consumer project ID (X-Goog-User-Project)\n"+
				"for rate-limit accounting when using CLI OAuth tokens (Buganizer API itself is $0 / free).\n\n"+
				"Remediation:\n"+
				"  1. Enable the Issue Tracker API on any GCP project you own or have access to:\n"+
				"     gcloud services enable %s --project=\"<your-gcp-project-id>\"\n\n"+
				"  2. Configure gh-ish to use that project for rate-limit quota:\n"+
				"     git config --global ghish.quotaproject \"<your-gcp-project-id>\"\n"+
				"     # or: export GHISH_QUOTA_PROJECT=\"<your-gcp-project-id>\"\n\n"+
				"Server Details: %s", resp.StatusCode, method, path, svcName, rawErr)
		}
		return fmt.Errorf("Google Issue Tracker authentication failed (HTTP %d) for %s %s.\n\n"+
			"Cause: Your current OAuth token was rejected or lacks the 'https://www.googleapis.com/auth/buganizer' scope.\n\n"+
			"Remediation:\n"+
			"  1. Re-authenticate with LUCI Auth including the buganizer scope:\n"+
			"     luci-auth login -scopes \"https://www.googleapis.com/auth/buganizer https://www.googleapis.com/auth/cloud-platform\"\n"+
			"  2. Or re-authenticate with Google Cloud SDK:\n"+
			"     gcloud auth application-default login --scopes=\"https://www.googleapis.com/auth/buganizer,https://www.googleapis.com/auth/cloud-platform\"\n"+
			"  3. Or check if this issue belongs to a restricted internal component.\n\n"+
			"Server Details: %s", resp.StatusCode, method, path, rawErr)
	}

	if resp.StatusCode == http.StatusNotFound {
		return fmt.Errorf("Buganizer issue not found (HTTP 404) at %s.\n\n"+
			"Cause: The requested issue ID does not exist, or it is restricted to an internal component not visible to your account.\n\n"+
			"Remediation:\n"+
			"  - Verify the issue number is correct.\n"+
			"  - List accessible open issues using:\n"+
			"    gh issue list", path)
	}

	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return fmt.Errorf("Google Issue Tracker API returned HTTP %d for %s %s: %s",
			resp.StatusCode, method, path, strings.TrimSpace(string(respBytes)))
	}

	if respBody != nil && len(respBytes) > 0 {
		if err := json.Unmarshal(respBytes, respBody); err != nil {
			return fmt.Errorf("failed to parse JSON response from %s %s: %w", method, path, err)
		}
	}

	return nil
}

// GetIssue retrieves a single Buganizer issue by ID, including its initial description comment.
func (c *IssueTrackerClient) GetIssue(ctx context.Context, issueID int64) (*BuganizerIssue, error) {
	path := fmt.Sprintf("/issues/%d?view=FULL", issueID)
	var issue BuganizerIssue
	if err := c.doJSON(ctx, http.MethodGet, path, nil, &issue); err != nil {
		return nil, err
	}
	return &issue, nil
}

// ListIssues searches for issues matching a Buganizer query string.
func (c *IssueTrackerClient) ListIssues(ctx context.Context, query string, pageSize int, pageToken string) (*ListIssuesResponse, error) {
	params := url.Values{}
	if query != "" {
		params.Set("query", query)
	}
	if pageSize > 0 {
		params.Set("pageSize", strconv.Itoa(pageSize))
	}
	if pageToken != "" {
		params.Set("pageToken", pageToken)
	}

	path := "/issues?" + params.Encode()
	var resp ListIssuesResponse
	if err := c.doJSON(ctx, http.MethodGet, path, nil, &resp); err != nil {
		return nil, err
	}
	return &resp, nil
}

// CreateIssue creates a new Buganizer issue with the provided state and initial comment.
func (c *IssueTrackerClient) CreateIssue(ctx context.Context, req *CreateIssueRequest) (*BuganizerIssue, error) {
	var created BuganizerIssue
	if err := c.doJSON(ctx, http.MethodPost, "/issues", req, &created); err != nil {
		return nil, err
	}
	return &created, nil
}

// ModifyIssue atomically updates fields on an existing Buganizer issue and optionally posts a comment.
func (c *IssueTrackerClient) ModifyIssue(ctx context.Context, issueID int64, req *ModifyIssueRequest) (*BuganizerIssue, error) {
	path := fmt.Sprintf("/issues/%d:modify", issueID)
	var updated BuganizerIssue
	if err := c.doJSON(ctx, http.MethodPost, path, req, &updated); err != nil {
		return nil, err
	}
	return &updated, nil
}

// ListComments retrieves the comments for an issue.
func (c *IssueTrackerClient) ListComments(ctx context.Context, issueID int64, pageSize int, pageToken string) (*ListIssueCommentsResponse, error) {
	params := url.Values{}
	if pageSize > 0 {
		params.Set("pageSize", strconv.Itoa(pageSize))
	}
	if pageToken != "" {
		params.Set("pageToken", pageToken)
	}

	path := fmt.Sprintf("/issues/%d/comments", issueID)
	if encoded := params.Encode(); encoded != "" {
		path += "?" + encoded
	}
	var resp ListIssueCommentsResponse
	if err := c.doJSON(ctx, http.MethodGet, path, nil, &resp); err != nil {
		return nil, err
	}
	return &resp, nil
}

// CreateComment posts a new comment to an existing issue thread.
func (c *IssueTrackerClient) CreateComment(ctx context.Context, issueID int64, commentText string) (*BuganizerComment, error) {
	path := fmt.Sprintf("/issues/%d/comments", issueID)
	req := BuganizerComment{Comment: commentText}
	var created BuganizerComment
	if err := c.doJSON(ctx, http.MethodPost, path, &req, &created); err != nil {
		return nil, err
	}
	return &created, nil
}

// ListAllComments fetches all comment pages for an issue, following NextPageToken until exhausted,
// and returns them in chronological order (comment #1 first).
func (c *IssueTrackerClient) ListAllComments(ctx context.Context, issueID int64) ([]BuganizerComment, error) {
	var all []BuganizerComment
	pageToken := ""
	for {
		resp, err := c.ListComments(ctx, issueID, 100, pageToken)
		if err != nil {
			return nil, err
		}
		all = append(all, resp.IssueComments...)
		if resp.NextPageToken == "" {
			break
		}
		pageToken = resp.NextPageToken
	}
	sort.SliceStable(all, func(i, j int) bool {
		if all[i].CommentNumber != all[j].CommentNumber {
			return all[i].CommentNumber < all[j].CommentNumber
		}
		return all[i].CreatedTime.Before(all[j].CreatedTime)
	})
	return all, nil
}

// CloseIssue closes an issue with the specified status (FIXED, OBSOLETE, DUPLICATE, etc.) and optional comment.
func (c *IssueTrackerClient) CloseIssue(ctx context.Context, issueID int64, status string, canonicalIssueID int64, commentText string) (*BuganizerIssue, error) {
	upper := strings.ToUpper(strings.TrimSpace(status))
	if upper == "" {
		upper = "FIXED"
	}
	if upper == "DUPLICATE" && canonicalIssueID <= 0 {
		return nil, fmt.Errorf("closing an issue as DUPLICATE requires a positive canonical issue ID")
	}

	addState := &BuganizerState{Status: upper}
	addMasks := []string{"status"}
	if canonicalIssueID > 0 {
		addState.CanonicalIssueID = FlexInt64(canonicalIssueID)
		addMasks = append(addMasks, "canonicalIssueId")
	}

	req := &ModifyIssueRequest{
		AddMask: strings.Join(addMasks, ","),
		Add:     addState,
	}
	if strings.TrimSpace(commentText) != "" {
		req.IssueComment = &BuganizerComment{Comment: strings.TrimSpace(commentText)}
	}
	return c.ModifyIssue(ctx, issueID, req)
}

// ReopenIssue reopens a closed Buganizer issue, restoring ASSIGNED if an assignee is present or ACCEPTED otherwise.
func (c *IssueTrackerClient) ReopenIssue(ctx context.Context, issueID int64, commentText string) (*BuganizerIssue, error) {
	current, err := c.GetIssue(ctx, issueID)
	if err != nil {
		return nil, err
	}
	if IsIssueOpen(current.State.Status) {
		return nil, fmt.Errorf("issue b/%d is already open (status: %s)", issueID, current.State.Status)
	}

	targetStatus := "ACCEPTED"
	if current.State.Assignee != nil && current.State.Assignee.EmailAddress != "" {
		targetStatus = "ASSIGNED"
	}

	req := &ModifyIssueRequest{
		AddMask: "status",
		Add:     &BuganizerState{Status: targetStatus},
	}
	if strings.TrimSpace(commentText) != "" {
		req.IssueComment = &BuganizerComment{Comment: strings.TrimSpace(commentText)}
	}
	return c.ModifyIssue(ctx, issueID, req)
}

// IssueModifier encapsulates Buganizer field masks and state machine transitions (such as NEW <-> ASSIGNED)
// when building a ModifyIssueRequest.
type IssueModifier struct {
	current     *BuganizerIssue
	addState    *BuganizerState
	removeState *BuganizerState
	addMasks    []string
	removeMasks []string
	maskSet     map[string]bool
	remSet      map[string]bool
}

// NewIssueModifier creates a modifier initialized against the current state of an issue.
func NewIssueModifier(current *BuganizerIssue) *IssueModifier {
	return &IssueModifier{
		current:     current,
		addState:    &BuganizerState{},
		removeState: &BuganizerState{},
		maskSet:     make(map[string]bool),
		remSet:      make(map[string]bool),
	}
}

func (m *IssueModifier) addMask(f string) {
	if !m.maskSet[f] {
		m.maskSet[f] = true
		m.addMasks = append(m.addMasks, f)
	}
}

func (m *IssueModifier) remMask(f string) {
	if !m.remSet[f] {
		m.remSet[f] = true
		m.removeMasks = append(m.removeMasks, f)
	}
}

// SetTitle sets a new issue title, rejecting empty strings.
func (m *IssueModifier) SetTitle(title string) error {
	trimmed := strings.TrimSpace(title)
	if trimmed == "" {
		return fmt.Errorf("--title cannot be empty")
	}
	m.addState.Title = trimmed
	m.addMask("title")
	return nil
}

// SetPriority sets the issue priority (e.g., P0-P4).
func (m *IssueModifier) SetPriority(priority string) error {
	if _, err := ApplyLabelToState(m.addState, priority); err != nil {
		return err
	}
	m.addMask("priority")
	return nil
}

// SetType sets the issue type (e.g., BUG, FEATURE_REQUEST, TASK).
func (m *IssueModifier) SetType(issueType string) error {
	if _, err := ApplyLabelToState(m.addState, issueType); err != nil {
		return err
	}
	m.addMask("type")
	return nil
}

// SetComponent sets the issue component ID.
func (m *IssueModifier) SetComponent(componentID int64) {
	m.addState.ComponentID = FlexInt64(componentID)
	m.addMask("componentId")
}

// AddLabel parses a GitHub-style label and adds the corresponding Buganizer field.
func (m *IssueModifier) AddLabel(label string) error {
	field, err := ApplyLabelToState(m.addState, label)
	if err != nil {
		return err
	}
	m.addMask(field)
	return nil
}

// RemoveLabel parses a GitHub-style label and removes the corresponding Buganizer field.
func (m *IssueModifier) RemoveLabel(label string) error {
	field, err := ApplyLabelToState(m.removeState, label)
	if err != nil {
		return err
	}
	m.remMask(field)
	return nil
}

// SetAssignee assigns the issue and automatically transitions NEW issues to ASSIGNED.
func (m *IssueModifier) SetAssignee(email string) {
	m.addState.Assignee = &BuganizerUser{EmailAddress: email}
	m.addMask("assignee")
	if m.current != nil && m.current.State.Status == "NEW" {
		m.addState.Status = "ASSIGNED"
		m.addMask("status")
	}
}

// RemoveAssignee unassigns the issue and automatically transitions ASSIGNED issues back to NEW.
func (m *IssueModifier) RemoveAssignee() {
	m.remMask("assignee")
	if m.current != nil && m.current.State.Status == "ASSIGNED" {
		m.addState.Status = "NEW"
		m.addMask("status")
	}
}

// HasChanges returns true if at least one field mask has been queued for addition or removal.
func (m *IssueModifier) HasChanges() bool {
	return len(m.addMasks) > 0 || len(m.removeMasks) > 0
}

// BuildRequest constructs the ModifyIssueRequest payload.
func (m *IssueModifier) BuildRequest() *ModifyIssueRequest {
	return &ModifyIssueRequest{
		AddMask:    strings.Join(m.addMasks, ","),
		Add:        m.addState,
		RemoveMask: strings.Join(m.removeMasks, ","),
		Remove:     m.removeState,
	}
}

// --- Buganizer Domain & Schema Translation ---

// ApplyLabelToState translates a GitHub-style label (e.g. "P1", "bug", "hotlist:123")
// into its corresponding BuganizerState field and returns the REST field mask name.
func ApplyLabelToState(state *BuganizerState, label string) (string, error) {
	clean := strings.TrimSpace(label)
	upper := strings.ToUpper(clean)
	lower := strings.ToLower(clean)

	switch {
	case upper == "P0" || upper == "P1" || upper == "P2" || upper == "P3" || upper == "P4":
		state.Priority = upper
		return "priority", nil
	case strings.HasPrefix(lower, "priority:"):
		p := strings.ToUpper(strings.TrimPrefix(lower, "priority:"))
		if p == "P0" || p == "P1" || p == "P2" || p == "P3" || p == "P4" {
			state.Priority = p
			return "priority", nil
		}
	case upper == "S0" || upper == "S1" || upper == "S2" || upper == "S3" || upper == "S4":
		state.Severity = upper
		return "severity", nil
	case strings.HasPrefix(lower, "severity:"):
		s := strings.ToUpper(strings.TrimPrefix(lower, "severity:"))
		if s == "S0" || s == "S1" || s == "S2" || s == "S3" || s == "S4" {
			state.Severity = s
			return "severity", nil
		}
	case lower == "bug" || lower == "type:bug":
		state.Type = "BUG"
		return "type", nil
	case lower == "feature" || lower == "enhancement" || lower == "type:feature":
		state.Type = "FEATURE_REQUEST"
		return "type", nil
	case lower == "task" || lower == "type:task":
		state.Type = "TASK"
		return "type", nil
	case lower == "cleanup" || lower == "type:cleanup":
		state.Type = "INTERNAL_CLEANUP"
		return "type", nil
	case lower == "process" || lower == "type:process":
		state.Type = "PROCESS"
		return "type", nil
	case strings.HasPrefix(lower, "component:"):
		idStr := strings.TrimPrefix(lower, "component:")
		id, err := strconv.ParseInt(idStr, 10, 64)
		if err != nil || id <= 0 {
			return "", fmt.Errorf("invalid component ID in label %q", label)
		}
		state.ComponentID = FlexInt64(id)
		return "componentId", nil
	case strings.HasPrefix(lower, "hotlist:"):
		idStr := strings.TrimPrefix(lower, "hotlist:")
		id, err := strconv.ParseInt(idStr, 10, 64)
		if err != nil || id <= 0 {
			return "", fmt.Errorf("invalid hotlist ID in label %q", label)
		}
		state.HotlistIDs = append(state.HotlistIDs, FlexInt64(id))
		return "hotlistIds", nil
	}

	return "", fmt.Errorf("unsupported Buganizer label format %q.\n\n"+
		"Cause: Google Issue Tracker uses structured fields instead of arbitrary text labels.\n\n"+
		"Supported label formats:\n"+
		"  - Priority: P0, P1, P2, P3, P4 (or priority:P1)\n"+
		"  - Severity: S0, S1, S2, S3, S4 (or severity:S2)\n"+
		"  - Type:     bug, feature, task, cleanup, process\n"+
		"  - Hotlist:  hotlist:<id>\n"+
		"  - Component: component:<id>", label)
}

// LabelToQueryToken converts a GitHub-style label into a Buganizer search query predicate.
func LabelToQueryToken(label string) (string, error) {
	var parsedState BuganizerState
	field, err := ApplyLabelToState(&parsedState, label)
	if err != nil {
		return "", err
	}
	switch field {
	case "priority":
		return fmt.Sprintf("priority:%s", parsedState.Priority), nil
	case "severity":
		return fmt.Sprintf("severity:%s", parsedState.Severity), nil
	case "type":
		return fmt.Sprintf("type:%s", parsedState.Type), nil
	case "componentId":
		return fmt.Sprintf("componentid:%d", parsedState.ComponentID), nil
	case "hotlistIds":
		if len(parsedState.HotlistIDs) > 0 {
			return fmt.Sprintf("hotlistid:%d", parsedState.HotlistIDs[0]), nil
		}
	}
	return "", fmt.Errorf("unsupported query label %q", label)
}

// IsIssueOpen returns true if a Buganizer status corresponds to an open state.
func IsIssueOpen(status string) bool {
	return status == "NEW" || status == "ASSIGNED" || status == "ACCEPTED"
}

// SynthesizeLabels generates GitHub CLI compatible label objects from Buganizer structured state.
func SynthesizeLabels(st BuganizerState) []map[string]string {
	var labels []map[string]string
	if st.Priority != "" {
		labels = append(labels, map[string]string{"name": st.Priority})
	}
	if st.Severity != "" {
		labels = append(labels, map[string]string{"name": st.Severity})
	}
	if st.Type != "" {
		labels = append(labels, map[string]string{"name": "Type: " + st.Type})
	}
	if st.ComponentID > 0 {
		labels = append(labels, map[string]string{"name": fmt.Sprintf("Component: %d", st.ComponentID)})
	}
	for _, h := range st.HotlistIDs {
		labels = append(labels, map[string]string{"name": fmt.Sprintf("Hotlist: %d", h)})
	}
	if labels == nil {
		labels = []map[string]string{}
	}
	return labels
}

var allowedIssueJSONFields = map[string]bool{
	"number":      true,
	"title":       true,
	"state":       true,
	"stateReason": true,
	"body":        true,
	"author":      true,
	"assignees":   true,
	"labels":      true,
	"comments":    true,
	"createdAt":   true,
	"updatedAt":   true,
	"closedAt":    true,
	"url":         true,
	"priority":    true,
	"severity":    true,
	"type":        true,
	"componentId": true,
}

// ValidateIssueJSONFields validates a comma-separated list of --json fields against the supported schema.
func ValidateIssueJSONFields(fieldsStr string) ([]string, error) {
	if strings.TrimSpace(fieldsStr) == "" {
		return nil, nil
	}
	var fields []string
	for _, f := range strings.Split(fieldsStr, ",") {
		f = strings.TrimSpace(f)
		if f == "" {
			continue
		}
		if !allowedIssueJSONFields[f] {
			return nil, fmt.Errorf("unknown JSON field: %q\n\n"+
				"Available fields for gh issue --json:\n"+
				"  number, title, state, stateReason, body, author, assignees, labels,\n"+
				"  comments, createdAt, updatedAt, closedAt, url, priority, severity, type, componentId", f)
		}
		fields = append(fields, f)
	}
	return fields, nil
}

// IssueToJSONMap projects a BuganizerIssue and its comments into a map matching the requested --json fields.
func IssueToJSONMap(issue *BuganizerIssue, comments []BuganizerComment, profile ProjectProfile, fields []string) map[string]any {
	stateStr := "CLOSED"
	stateReason := "COMPLETED"
	if IsIssueOpen(issue.State.Status) {
		stateStr = "OPEN"
		stateReason = ""
	} else if issue.State.Status == "OBSOLETE" || issue.State.Status == "INFEASIBLE" || issue.State.Status == "INTENDED_BEHAVIOR" {
		stateReason = "NOT_PLANNED"
	}

	body := ""
	if desc := issue.EffectiveDescription(); desc != nil {
		body = desc.Comment
	} else if len(comments) > 0 {
		body = comments[0].Comment
	}

	authorLogin := ""
	if issue.State.Reporter != nil {
		authorLogin = issue.State.Reporter.EmailAddress
	}

	var assignees []map[string]string
	if issue.State.Assignee != nil && issue.State.Assignee.EmailAddress != "" {
		assignees = append(assignees, map[string]string{"login": issue.State.Assignee.EmailAddress})
	} else {
		assignees = []map[string]string{}
	}

	var jsonComments []map[string]any
	for i, c := range comments {
		if c.CommentNumber == 1 || (c.CommentNumber == 0 && i == 0) {
			continue // Comment #1 is the description body
		}
		cAuthor := c.EffectiveAuthorEmail()
		if cAuthor == "unknown" {
			cAuthor = ""
		}
		jsonComments = append(jsonComments, map[string]any{
			"id":        strconv.Itoa(c.CommentNumber),
			"author":    map[string]string{"login": cAuthor},
			"body":      c.Comment,
			"createdAt": c.CreatedTime.Format(time.RFC3339),
		})
	}
	if jsonComments == nil {
		jsonComments = []map[string]any{}
	}

	closedAt := ""
	if issue.ResolvedTime != nil {
		closedAt = issue.ResolvedTime.Format(time.RFC3339)
	}

	full := map[string]any{
		"number":      int64(issue.IssueID),
		"title":       issue.State.Title,
		"state":       stateStr,
		"stateReason": stateReason,
		"body":        body,
		"author":      map[string]string{"login": authorLogin},
		"assignees":   assignees,
		"labels":      SynthesizeLabels(issue.State),
		"comments":    jsonComments,
		"createdAt":   issue.CreatedTime.Format(time.RFC3339),
		"updatedAt":   issue.ModifiedTime.Format(time.RFC3339),
		"closedAt":    closedAt,
		"url":         profile.IssueWebURL(int64(issue.IssueID)),
		"priority":    issue.State.Priority,
		"severity":    issue.State.Severity,
		"type":        issue.State.Type,
		"componentId": int64(issue.State.ComponentID),
	}

	if len(fields) == 0 {
		return full
	}

	filtered := make(map[string]any, len(fields))
	for _, f := range fields {
		filtered[f] = full[f]
	}
	return filtered
}

var (
	ansiEscapeRegex  = regexp.MustCompile(`\x1b\[[0-9;]*[a-zA-Z]`)
	nonAlphaNumRegex = regexp.MustCompile(`[^a-z0-9]+`)
)

// SanitizeUntrustedText strips ANSI escape sequences and non-printable control characters
// (preserving \n, \t, and \r) from untrusted issue titles, descriptions, and comments.
func SanitizeUntrustedText(s string) string {
	s = ansiEscapeRegex.ReplaceAllString(s, "")
	return strings.Map(func(r rune) rune {
		if r == '\n' || r == '\t' || r == '\r' || r >= 32 {
			return r
		}
		return -1
	}, s)
}

// FormatUntrustedBlock wraps untrusted issue text in explicit boundary delimiters after sanitizing.
func FormatUntrustedBlock(header, footer, text string) string {
	clean := strings.TrimSpace(SanitizeUntrustedText(text))
	if clean == "" {
		clean = "(empty)"
	}
	return fmt.Sprintf("%s\n%s\n%s", header, clean, footer)
}

// SlugifyBranchName generates a deterministic branch name ("b-<id>-<slug>") capped at 45 slug characters.
func SlugifyBranchName(issueID int64, title string) string {
	lower := strings.ToLower(title)
	slug := nonAlphaNumRegex.ReplaceAllString(lower, "-")
	slug = strings.Trim(slug, "-")
	if len(slug) > 45 {
		slug = strings.TrimRight(slug[:45], "-")
	}
	if slug == "" {
		return fmt.Sprintf("b-%d", issueID)
	}
	return fmt.Sprintf("b-%d-%s", issueID, slug)
}
