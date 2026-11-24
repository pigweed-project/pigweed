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
	"cmp"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"slices"
	"strings"
	"sync"
)

// LUCILog represents a single log stream inside a LUCI build step.
type LUCILog struct {
	Name    string `json:"name"`
	ViewURL string `json:"viewUrl"`
	URL     string `json:"url,omitempty"`
}

// LUCIStep represents an execution step within a LUCI build.
type LUCIStep struct {
	Name            string    `json:"name"`
	Status          string    `json:"status"` // SUCCESS, FAILURE, INFRA_FAILURE, etc.
	SummaryMarkdown string    `json:"summaryMarkdown,omitempty"`
	Logs            []LUCILog `json:"logs,omitempty"`
}

// LUCIStatusDetails contains specific failure or cancellation reasons from Buildbucket.
type LUCIStatusDetails struct {
	ResourceExhaustion *struct{} `json:"resourceExhaustion,omitempty"`
	Timeout            *struct{} `json:"timeout,omitempty"`
}

// LUCIGitilesCommit describes the Gitiles commit trigger on a Buildbucket build.
type LUCIGitilesCommit struct {
	Host    string `json:"host,omitempty"`
	Project string `json:"project,omitempty"`
	ID      string `json:"id,omitempty"`
	Ref     string `json:"ref,omitempty"`
}

// LUCIGerritChange describes a Gerrit patchset trigger on a Buildbucket build.
// Buildbucket's ProtoJSON wire format encodes int64 change and patchset numbers
// as JSON strings ("467905"), while outgoing SearchBuilds requests and CLI
// --json output represent them as integers.
type LUCIGerritChange struct {
	Host     string `json:"host"`
	Project  string `json:"project"`
	Change   int    `json:"change"`
	Patchset int    `json:"patchset"`
}

// UnmarshalJSON decodes both ProtoJSON string int64s and numeric JSON values.
func (g *LUCIGerritChange) UnmarshalJSON(data []byte) error {
	var raw struct {
		Host     string    `json:"host"`
		Project  string    `json:"project"`
		Change   FlexInt64 `json:"change"`
		Patchset FlexInt64 `json:"patchset"`
	}
	if err := json.Unmarshal(data, &raw); err != nil {
		return err
	}
	g.Host = raw.Host
	g.Project = raw.Project
	g.Change = int(raw.Change)
	g.Patchset = int(raw.Patchset)
	return nil
}

type bbGerritChange = LUCIGerritChange
type bbInput = LUCIBuildInput

// LUCIBuildInput holds trigger and experiment inputs on a Buildbucket build.
type LUCIBuildInput struct {
	GitilesCommit *LUCIGitilesCommit `json:"gitilesCommit,omitempty"`
	GerritChanges []LUCIGerritChange `json:"gerritChanges,omitempty"`
	Experiments   []string           `json:"experiments,omitempty"`
}

// LUCIBuildDetails holds detailed build information including steps and logs.
type LUCIBuildDetails struct {
	ID                   string             `json:"id"`
	Number               int                `json:"number,omitempty"`
	Builder              bbBuilder          `json:"builder"`
	Status               string             `json:"status"`
	CreateTime           string             `json:"createTime,omitempty"`
	StartTime            string             `json:"startTime,omitempty"`
	EndTime              string             `json:"endTime,omitempty"`
	SummaryMarkdown      string             `json:"summaryMarkdown,omitempty"`
	CancellationMarkdown string             `json:"cancellationMarkdown,omitempty"`
	StatusDetails        *LUCIStatusDetails `json:"statusDetails,omitempty"`
	Input                *LUCIBuildInput    `json:"input,omitempty"`
	Steps                []LUCIStep         `json:"steps,omitempty"`
	Error                string             `json:"error,omitempty"`
	Provider             string             `json:"provider,omitempty"`
	ViewURL              string             `json:"viewUrl,omitempty"`
	Target               string             `json:"target,omitempty"`
	InvocationID         string             `json:"invocationId,omitempty"`
	WorkplanID           string             `json:"workplanId,omitempty"`
}

// FailureReport encapsulates diagnostic information for a failed check.
type FailureReport struct {
	BuildID      string `json:"buildId"`
	Builder      string `json:"builder"`
	Status       string `json:"status"`
	BuildURL     string `json:"buildUrl"`
	FailedStep   string `json:"failedStep"`
	BuildSummary string `json:"buildSummary,omitempty"`
	StepSummary  string `json:"stepSummary,omitempty"`
	LogName      string `json:"logName,omitempty"`
	LogSnippet   string `json:"logSnippet,omitempty"`
	FullLogURL   string `json:"fullLogUrl,omitempty"`
	// LogTotalLines is the line count of the full log LogSnippet was tailed
	// from, or 0 if unknown. It lets the text report state how many lines
	// were really omitted rather than counting only within the snippet.
	// Providers that do not set it (currently Busytown) fall back to
	// counting omitted lines within the snippet.
	LogTotalLines int `json:"-"`
	// Patchset is set only when the build ran on an earlier code-equivalent
	// patchset rather than the one requested.
	Patchset int `json:"patchset,omitempty"`
}

type bbBuilder struct {
	Project string `json:"project"`
	Bucket  string `json:"bucket"`
	Builder string `json:"builder"`
}

type bbTag struct {
	Key   string `json:"key"`
	Value string `json:"value"`
}

type bbBuild struct {
	ID              string    `json:"id"`
	Number          int       `json:"number,omitempty"`
	Builder         bbBuilder `json:"builder"`
	Status          string    `json:"status"`
	SummaryMarkdown string    `json:"summaryMarkdown"`
	Critical        string    `json:"critical,omitempty"`
	Input           *bbInput  `json:"input,omitempty"`
	Tags            []bbTag   `json:"tags,omitempty"`
	CreateTime      string    `json:"createTime"`
	StartTime       string    `json:"startTime"`
	EndTime         string    `json:"endTime"`
	// Patchset is the Gerrit patchset the build ran against. It is not part
	// of the Buildbucket response; SearchBuilds fills it in from the query,
	// so callers can tell builds reused from an earlier code-equivalent
	// patchset apart from builds on the requested one.
	Patchset     int    `json:"-"`
	Provider     string `json:"provider,omitempty"`
	ViewURL      string `json:"viewUrl,omitempty"`
	Target       string `json:"target,omitempty"`
	InvocationID string `json:"invocationId,omitempty"`
	WorkplanID   string `json:"workplanId,omitempty"`
}

// IsBusytown reports whether the build originated from the Android Busytown / TreeHugger provider.
func (b *bbBuild) IsBusytown() bool {
	if b == nil {
		return false
	}
	if strings.EqualFold(b.Provider, "buildbucket") {
		return false
	}
	if b.Builder.Project != "" && !strings.EqualFold(b.Builder.Project, "android-build") {
		return false
	}
	return strings.EqualFold(b.Provider, "busytown") ||
		strings.EqualFold(b.Builder.Project, "android-build") ||
		IsBusytownBuildID(b.ID)
}

// BuildURL returns the web UI URL for a build across CI providers (LUCI Buildbucket or Android Busytown).
func BuildURL(b bbBuild) string {
	if b.ViewURL != "" {
		return b.ViewURL
	}
	if b.IsBusytown() {
		target := b.Target
		if target == "" {
			target = b.TagValue("target")
		}
		if target == "" {
			target = b.Builder.Builder
		}
		return BusytownBuildURL(b.ID, target)
	}
	return fmt.Sprintf("https://ci.chromium.org/b/%s", b.ID)
}

// BuildDetailsURL returns the web UI URL for a LUCIBuildDetails struct across CI providers.
func BuildDetailsURL(b *LUCIBuildDetails) string {
	if b == nil {
		return ""
	}
	if b.ViewURL != "" {
		return b.ViewURL
	}
	if strings.EqualFold(b.Provider, "busytown") || strings.EqualFold(b.Builder.Project, "android-build") {
		target := b.Target
		if target == "" {
			target = b.Builder.Builder
		}
		return BusytownBuildURL(b.ID, target)
	}
	return fmt.Sprintf("https://ci.chromium.org/b/%s", b.ID)
}

// TagValue returns the first tag value matching key (case-insensitive), or "".
func (b *bbBuild) TagValue(key string) string {
	for _, t := range b.Tags {
		if strings.EqualFold(t.Key, key) {
			return t.Value
		}
	}
	return ""
}

// HasTag reports whether the build has a tag matching key and value.
func (b *bbBuild) HasTag(key, value string) bool {
	for _, t := range b.Tags {
		if strings.EqualFold(t.Key, key) && (value == "" || t.Value == value) {
			return true
		}
	}
	return false
}

// MatchesTagFilters reports whether the build matches any "key:value" filter in filters.
func (b *bbBuild) MatchesTagFilters(filters []string) bool {
	for _, f := range filters {
		f = strings.TrimSpace(f)
		if f == "" {
			continue
		}
		k, v, found := strings.Cut(f, ":")
		if !found {
			if b.HasTag(k, "") {
				return true
			}
			continue
		}
		if b.HasTag(strings.TrimSpace(k), strings.TrimSpace(v)) {
			return true
		}
	}
	return false
}

// FilterBuildsByTags partitions builds into visible builds and the count of builds
// hidden by hideFilters.
func FilterBuildsByTags(builds []bbBuild, hideFilters []string) (visible []bbBuild, hiddenCount int) {
	if len(hideFilters) == 0 {
		return builds, 0
	}
	visible = make([]bbBuild, 0, len(builds))
	for _, b := range builds {
		if b.MatchesTagFilters(hideFilters) {
			hiddenCount++
			continue
		}
		visible = append(visible, b)
	}
	return visible, hiddenCount
}

// IsExperimental reports whether the build is non-blocking / experimental.
func (b *bbBuild) IsExperimental() bool {
	if strings.EqualFold(b.Critical, "NO") {
		return true
	}
	for _, tag := range b.Tags {
		if strings.EqualFold(tag.Key, "cq_experimental") {
			if strings.EqualFold(tag.Value, "true") || tag.Value == "1" {
				return true
			}
		}
	}
	if b.Input != nil {
		for _, exp := range b.Input.Experiments {
			if strings.HasSuffix(exp, ".non_production") || exp == "luci.non_production" || exp == "cq_experimental" {
				return true
			}
		}
	}
	if strings.Contains(b.SummaryMarkdown, "non_production") || strings.Contains(b.SummaryMarkdown, "cq_experimental") {
		return true
	}
	return false
}

type bbSearchBuildsRequest struct {
	Predicate bbPredicate `json:"predicate"`
	Mask      bbBuildMask `json:"mask,omitempty"`
	PageSize  int         `json:"pageSize,omitempty"`
	PageToken string      `json:"pageToken,omitempty"`
}

type bbPredicate struct {
	GerritChanges []bbGerritChange `json:"gerritChanges,omitempty"`
	Builder       *bbBuilder       `json:"builder,omitempty"`
	Status        string           `json:"status,omitempty"`
}

type bbSearchBuildsResponse struct {
	Builds        []bbBuild `json:"builds"`
	NextPageToken string    `json:"nextPageToken,omitempty"`
}

type bbBuildMask struct {
	Fields string `json:"fields"`
}

type bbGetBuildRequest struct {
	ID   string      `json:"id"`
	Mask bbBuildMask `json:"mask"`
}

// LUCIClient provides methods to interact with LUCI Buildbucket and LogDog APIs.
type LUCIClient struct {
	Host          string
	HTTPClient    *http.Client
	TokenProvider func(context.Context) (string, error)
}

// NewLUCIClient creates a new LUCIClient targeting the given Buildbucket host.
// If httpClient is nil, a client that strips Authorization on cross-host
// redirects is used.
func NewLUCIClient(host string, httpClient *http.Client) *LUCIClient {
	if httpClient == nil {
		httpClient = EnsureSafeHTTPClient(nil)
	}
	return &LUCIClient{
		Host:       host,
		HTTPClient: httpClient,
	}
}

// CallPRPC makes a pRPC POST request to the specified service and method on the LUCI client's host.
// It handles JSON marshaling, setting headers, stripping the ')]}\'\n' security prefix,
// and unmarshaling the JSON response into resp.
func (c *LUCIClient) CallPRPC(ctx context.Context, service, method string, req, resp any) error {
	return c.callPRPCWithMaxBytes(ctx, service, method, req, resp, maxHTTPBodyBytes)
}

func (c *LUCIClient) callPRPCWithMaxBytes(ctx context.Context, service, method string, req, resp any, maxBytes int64) error {
	if c == nil {
		return fmt.Errorf("LUCIClient is nil")
	}
	if c.Host == "" {
		return fmt.Errorf("LUCIClient host cannot be empty")
	}
	if service == "" || method == "" {
		return fmt.Errorf("service and method cannot be empty")
	}
	if req == nil {
		return fmt.Errorf("request payload cannot be nil")
	}
	if resp == nil {
		return fmt.Errorf("response pointer cannot be nil")
	}
	if maxBytes <= 0 {
		maxBytes = maxHTTPBodyBytes
	}

	jsonBytes, err := json.Marshal(req)
	if err != nil {
		return fmt.Errorf("failed to marshal %s/%s request: %w", service, method, err)
	}

	endpoint := fmt.Sprintf("https://%s/prpc/%s/%s", c.Host, service, method)
	lowerHost := strings.ToLower(c.Host)
	if strings.HasPrefix(lowerHost, "http://") || strings.HasPrefix(lowerHost, "https://") {
		endpoint = fmt.Sprintf("%s/prpc/%s/%s", c.Host, service, method)
	}

	if VerboseFlag {
		fmt.Fprintf(os.Stderr, "[debug] pRPC request: %s %s\n", endpoint, string(jsonBytes))
	}

	httpResp, err := ExecuteWithAuthStrategy(ctx, c.HTTPClient, newLUCIAuth(c.Host, c.TokenProvider), http.MethodPost, endpoint, jsonBytes)
	if err != nil {
		if _, ok := errors.AsType[*ExitCodeError](err); ok {
			return err
		}
		return fmt.Errorf("%s/%s request failed: %w", service, method, err)
	}
	defer httpResp.Body.Close()

	bodyBytes, err := readBoundedBody(httpResp.Body, maxBytes)
	if err != nil {
		return fmt.Errorf("failed to read %s/%s response: %w", service, method, err)
	}

	if httpResp.StatusCode != http.StatusOK {
		if httpResp.StatusCode == http.StatusUnauthorized || httpResp.StatusCode == http.StatusForbidden {
			return newLUCIHTTPAuthError(ctx, fmt.Sprintf("Buildbucket %s/%s", service, method), httpResp.StatusCode, bodyBytes)
		}
		return fmt.Errorf("Buildbucket %s/%s returned HTTP %d: %s", service, method, httpResp.StatusCode, string(bodyBytes))
	}

	bodyBytes = stripXSSIPrefix(bodyBytes)
	if VerboseFlag {
		fmt.Fprintf(os.Stderr, "[debug] pRPC response: %s\n", string(bodyBytes))
	}

	if err := json.Unmarshal(bodyBytes, resp); err != nil {
		return fmt.Errorf("failed to parse %s/%s response: %w", service, method, err)
	}

	return nil
}

// SearchBuilds queries Buildbucket for builds matching a Gerrit change and patchset.
func (c *LUCIClient) SearchBuilds(ctx context.Context, gerritHost, project string, changeNum, patchsetNum int) ([]bbBuild, error) {
	if c == nil {
		return nil, fmt.Errorf("LUCIClient is nil")
	}
	if gerritHost == "" {
		return nil, fmt.Errorf("gerritHost cannot be empty")
	}
	if project == "" {
		return nil, fmt.Errorf("project cannot be empty")
	}
	if changeNum <= 0 {
		return nil, fmt.Errorf("changeNum must be greater than 0")
	}
	if patchsetNum <= 0 {
		return nil, fmt.Errorf("patchsetNum must be greater than 0")
	}

	var allBuilds []bbBuild
	pageToken := ""
	for {
		reqPayload := bbSearchBuildsRequest{
			Predicate: bbPredicate{
				GerritChanges: []bbGerritChange{
					{
						Host:     CanonicalGerritHost(gerritHost),
						Project:  project,
						Change:   changeNum,
						Patchset: patchsetNum,
					},
				},
			},
			Mask: bbBuildMask{
				Fields: searchBuildsFieldsMask,
			},
			PageSize:  1000,
			PageToken: pageToken,
		}

		var searchResp bbSearchBuildsResponse
		if err := c.CallPRPC(ctx, "buildbucket.v2.Builds", "SearchBuilds", reqPayload, &searchResp); err != nil {
			return nil, err
		}
		if searchResp.Builds != nil && allBuilds == nil {
			allBuilds = []bbBuild{}
		}
		allBuilds = append(allBuilds, searchResp.Builds...)
		if searchResp.NextPageToken == "" {
			break
		}
		pageToken = searchResp.NextPageToken
	}

	for i := range allBuilds {
		allBuilds[i].Patchset = patchsetNum
	}
	return allBuilds, nil
}

// maxConcurrentBuildSearches caps the number of SearchBuilds requests in
// flight at once when querying several equivalent patchsets.
const maxConcurrentBuildSearches = 8

// SearchBuildsByPatchset queries Buildbucket for the builds on each of the
// given patchsets. The queries run concurrently. The result is aligned with
// patchsets: result[i] holds the builds for patchsets[i]. If any query fails,
// the error for the earliest failing patchset in the list is returned.
func (c *LUCIClient) SearchBuildsByPatchset(ctx context.Context, gerritHost, project string, changeNum int, patchsets []int) ([][]bbBuild, error) {
	if c == nil {
		return nil, fmt.Errorf("LUCIClient is nil")
	}
	if len(patchsets) == 0 {
		return nil, fmt.Errorf("patchsets cannot be empty")
	}

	ctx, cancel := context.WithCancel(ctx)
	defer cancel()

	results := make([][]bbBuild, len(patchsets))
	errs := make([]error, len(patchsets))
	sem := make(chan struct{}, maxConcurrentBuildSearches)
	var wg sync.WaitGroup
	for i, ps := range patchsets {
		wg.Add(1)
		go func(i, ps int) {
			defer wg.Done()
			sem <- struct{}{}
			defer func() { <-sem }()
			results[i], errs[i] = c.SearchBuilds(ctx, gerritHost, project, changeNum, ps)
			if errs[i] != nil {
				// No point waiting on the rest; the whole search fails.
				cancel()
			}
		}(i, ps)
	}
	wg.Wait()

	// Canceling the siblings makes them fail with context errors, so report
	// the first error that isn't one of those, falling back to the first.
	// If multiple searches fail with distinct non-cancellation errors, only the
	// error from the earliest patchset in the input slice is reported, which
	// keeps the failure message deterministic without dumping redundant errors.
	failed := firstNonCanceledErrorIndex(errs)
	if failed < 0 {
		return results, nil
	}
	if len(patchsets) == 1 {
		return nil, errs[failed]
	}
	return nil, fmt.Errorf("patchset %d: %w", patchsets[failed], errs[failed])
}

const getBuildDetailsFieldsMask = "id,number,builder,status,create_time,start_time,end_time,summary_markdown,cancellation_markdown,status_details,input.gitiles_commit,input.gerrit_changes,steps"

// GetBuildDetails fetches step-level details for a build from Buildbucket.
func (c *LUCIClient) GetBuildDetails(ctx context.Context, buildID string) (*LUCIBuildDetails, error) {
	if c == nil {
		return nil, fmt.Errorf("LUCIClient is nil")
	}
	if buildID == "" {
		return nil, fmt.Errorf("buildID cannot be empty")
	}

	reqPayload := bbGetBuildRequest{
		ID: buildID,
		Mask: bbBuildMask{
			Fields: getBuildDetailsFieldsMask,
		},
	}

	var details LUCIBuildDetails
	if err := c.callPRPCWithMaxBytes(ctx, "buildbucket.v2.Builds", "GetBuild", reqPayload, &details, maxBatchHTTPBodyBytes); err != nil {
		return nil, err
	}
	details.ID = cmp.Or(details.ID, buildID)
	details.Provider = cmp.Or(details.Provider, "buildbucket")

	return &details, nil
}

// fetchLogStreamTail downloads raw text logs from LogDog (?format=raw),
// optionally returning the last maxLines along with the number of lines in the
// full log before tailing.
func (c *LUCIClient) fetchLogStreamTail(ctx context.Context, viewURL string, maxLines int) (string, int, error) {
	if c == nil {
		return "", 0, fmt.Errorf("LUCIClient is nil")
	}
	if viewURL == "" {
		return "", 0, fmt.Errorf("viewURL cannot be empty")
	}

	parsed, err := url.Parse(viewURL)
	if err != nil {
		return "", 0, fmt.Errorf("invalid log URL %q: %w", viewURL, err)
	}

	preSigned := isPreSignedGCSURL(parsed)
	if !preSigned {
		q := parsed.Query()
		q.Set("format", "raw")
		parsed.RawQuery = q.Encode()
	}

	targetURL := parsed.String()
	resp, err := ExecuteWithAuthStrategy(ctx, c.HTTPClient, newLUCIAuth(c.Host, c.TokenProvider), http.MethodGet, targetURL, nil)
	if err != nil {
		if ExitCodeFor(err) == ExitCodeAuth {
			return "", 0, fmt.Errorf("failed to authenticate log request: %w", err)
		}
		return "", 0, fmt.Errorf("failed to fetch log: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, maxHTTPBodyBytes))
		if resp.StatusCode == http.StatusUnauthorized || resp.StatusCode == http.StatusForbidden {
			return "", 0, newLUCIHTTPAuthError(ctx, "LogDog", resp.StatusCode, body)
		}
		return "", 0, fmt.Errorf("LogDog returned HTTP %d: %s", resp.StatusCode, string(body))
	}

	rawBytes, err := readBoundedLog(resp.Body, maxHTTPBodyBytes)
	if err != nil {
		return "", 0, fmt.Errorf("failed to read log body: %w", err)
	}

	text := string(rawBytes)
	trimmed := strings.TrimRight(text, "\r\n")
	totalLines := 0
	if trimmed != "" {
		totalLines = strings.Count(trimmed, "\n") + 1
	}
	if maxLines <= 0 || totalLines <= maxLines {
		return text, totalLines, nil
	}

	lines := strings.Split(trimmed, "\n")
	return strings.Join(lines[len(lines)-maxLines:], "\n"), totalLines, nil
}

var defaultPreferredLogs = []string{"failure summary", "failure_summary", "stdout", "stderr", "full contents"}

func normalizeLogStreamName(s string) string {
	s = strings.ToLower(strings.TrimSpace(s))
	return strings.ReplaceAll(s, "_", " ")
}

// SelectPreferredLog selects the highest-priority log stream from logs.
// When preferred is empty, it defaults to ["failure summary", "failure_summary", "stdout", "stderr", "full contents"].
// If no preferred log name matches, it falls back to the first non-"$" log with a non-empty ViewURL.
func SelectPreferredLog(logs []LUCILog, preferred ...string) *LUCILog {
	log, _ := selectPreferredLogWithRank(logs, preferred...)
	return log
}

func selectPreferredLogWithRank(logs []LUCILog, preferred ...string) (*LUCILog, int) {
	if len(preferred) == 0 {
		preferred = defaultPreferredLogs
	}
	for rank, pref := range preferred {
		normPref := normalizeLogStreamName(pref)
		for i := range logs {
			if cmp.Or(logs[i].ViewURL, logs[i].URL) == "" {
				continue
			}
			if normalizeLogStreamName(logs[i].Name) == normPref {
				return &logs[i], rank
			}
		}
	}
	for i := range logs {
		if cmp.Or(logs[i].ViewURL, logs[i].URL) != "" && !strings.HasPrefix(logs[i].Name, "$") {
			return &logs[i], len(preferred)
		}
	}
	return nil, len(preferred) + 1
}

func isRecipePlumbingChild(childSuffix string) bool {
	return childSuffix == "logs" || strings.HasPrefix(childSuffix, "logs|") || strings.HasPrefix(childSuffix, "easy rerun cmd")
}

func isRecipePlumbingStep(stepName string) bool {
	for rest := stepName; ; {
		_, after, ok := strings.Cut(rest, "|")
		if !ok {
			return false
		}
		if isRecipePlumbingChild(after) {
			return true
		}
		rest = after
	}
}

func isFailingStepStatus(status string) bool {
	return status != "" && status != "SUCCESS" && status != "SCHEDULED" && status != "STARTED"
}

// selectLeafFailingSteps returns leaf failing steps from steps, preferring active
// failures (FAILURE/INFRA_FAILURE) over CANCELED steps, filtering out recipe
// plumbing containers (|logs, |easy rerun cmd), and dropping parent containers
// that have deeper failing descendants.
func selectLeafFailingSteps(steps []LUCIStep) []*LUCIStep {
	var candidates []*LUCIStep
	for i := range steps {
		if steps[i].Name != "" && isFailingStepStatus(steps[i].Status) {
			candidates = append(candidates, &steps[i])
		}
	}
	if len(candidates) == 0 {
		return nil
	}
	if slices.ContainsFunc(candidates, func(s *LUCIStep) bool { return s.Status != "CANCELED" }) {
		candidates = slices.DeleteFunc(candidates, func(s *LUCIStep) bool { return s.Status == "CANCELED" })
	}
	if slices.ContainsFunc(candidates, func(s *LUCIStep) bool { return !isRecipePlumbingStep(s.Name) }) {
		candidates = slices.DeleteFunc(candidates, func(s *LUCIStep) bool { return isRecipePlumbingStep(s.Name) })
	}
	var leaves []*LUCIStep
	for _, s := range candidates {
		prefix := s.Name + "|"
		if !slices.ContainsFunc(candidates, func(other *LUCIStep) bool { return strings.HasPrefix(other.Name, prefix) }) {
			leaves = append(leaves, s)
		}
	}
	if len(leaves) == 0 {
		return candidates
	}
	return leaves
}

// selectPrimaryFailedStep picks the leaf failing step with the highest-priority
// preferred log stream, falling back to the earliest leaf failing step.
func selectPrimaryFailedStep(steps []LUCIStep, preferredLogs []string) (*LUCIStep, *LUCILog) {
	leafSteps := selectLeafFailingSteps(steps)
	if len(leafSteps) == 0 {
		return nil, nil
	}
	bestRank := len(defaultPreferredLogs) + 2
	if len(preferredLogs) > 0 {
		bestRank = len(preferredLogs) + 2
	}
	failedStep := leafSteps[0]
	var targetLog *LUCILog
	for _, step := range leafSteps {
		candLog, rank := selectPreferredLogWithRank(step.Logs, preferredLogs...)
		if candLog != nil && rank <= bestRank {
			failedStep = step
			targetLog = candLog
			bestRank = rank
		}
	}
	return failedStep, targetLog
}

// ExtractFailureReportWithOptions analyzes steps in a failing build using custom preferred log
// names and build SummaryMarkdown inclusion settings.
func (c *LUCIClient) ExtractFailureReportWithOptions(ctx context.Context, b *LUCIBuildDetails, maxLogLines int, preferredLogs []string, includeSummaryMarkdown bool) *FailureReport {
	if c == nil {
		c = NewLUCIClient("", nil)
	}
	if b == nil {
		return nil
	}
	if b.Status != "FAILURE" && b.Status != "INFRA_FAILURE" {
		return nil
	}

	report := &FailureReport{
		BuildID:  b.ID,
		Builder:  b.Builder.Builder,
		Status:   b.Status,
		BuildURL: BuildDetailsURL(b),
	}

	failedStep, targetLog := selectPrimaryFailedStep(b.Steps, preferredLogs)

	if failedStep == nil {
		if b.SummaryMarkdown != "" && includeSummaryMarkdown {
			report.StepSummary = b.SummaryMarkdown
		} else if b.CancellationMarkdown != "" {
			report.StepSummary = b.CancellationMarkdown
		} else if b.StatusDetails != nil && b.StatusDetails.ResourceExhaustion != nil {
			report.StepSummary = "Task did not start: resource exhaustion (no available bots in pool)"
		} else if b.StatusDetails != nil && b.StatusDetails.Timeout != nil {
			report.StepSummary = "Task timed out before completion"
		} else {
			report.StepSummary = fmt.Sprintf("Build ended with status %s, but no step details were reported.", b.Status)
		}
		return report
	}

	report.FailedStep = failedStep.Name
	report.StepSummary = failedStep.SummaryMarkdown
	if report.StepSummary == "" {
		if b.SummaryMarkdown != "" && includeSummaryMarkdown {
			report.StepSummary = b.SummaryMarkdown
		} else if b.CancellationMarkdown != "" {
			report.StepSummary = b.CancellationMarkdown
		}
	} else if b.SummaryMarkdown != "" && includeSummaryMarkdown {
		cleanedBuildSum := cleanBuildSummary(b.SummaryMarkdown)
		if cleanedBuildSum != "" && !strings.Contains(report.StepSummary, cleanedBuildSum) {
			report.BuildSummary = cleanedBuildSum
		}
	}

	if targetLog != nil {
		if logURL := cmp.Or(targetLog.ViewURL, targetLog.URL); logURL != "" {
			report.LogName = targetLog.Name
			report.FullLogURL = logURL
			snippet, totalLines, err := c.fetchLogStreamTail(ctx, logURL, maxLogLines)
			if err != nil {
				report.LogSnippet = fmt.Sprintf("[Error fetching log stream from %s: %v]", logURL, err)
			} else {
				report.LogSnippet = strings.TrimSpace(snippet)
				report.LogTotalLines = totalLines
			}
		}
	}

	return report
}

// FormatBuildSteps formats the step-by-step progress and status of a LUCI build.
func FormatBuildSteps(b *LUCIBuildDetails) string {
	return FormatBuildStepsVerbose(b, false)
}

// FormatBuildStepsVerbose formats the step-by-step progress of a LUCI build.
// When verbose is false, internal recipe plumbing sub-steps are collapsed into top-level steps,
// with failing sub-steps explicitly highlighted with tree branches.
func FormatBuildStepsVerbose(b *LUCIBuildDetails, verbose bool) string {
	if b == nil {
		return ""
	}
	var sb strings.Builder
	fmt.Fprintf(&sb, "Steps for %s (Build %s)\n", b.Builder.Builder, b.ID)
	fmt.Fprintf(&sb, "Status: %s %s | URL: %s\n", b.Status, getStatusSymbol(b.Status), BuildDetailsURL(b))

	cleanSummary := cleanBuildSummary(b.SummaryMarkdown)
	if cleanSummary != "" {
		fmt.Fprintf(&sb, "Summary: %s\n", cleanSummary)
	} else if b.CancellationMarkdown != "" {
		fmt.Fprintf(&sb, "Summary: %s\n", b.CancellationMarkdown)
	}
	fmt.Fprintln(&sb)

	if len(b.Steps) == 0 {
		fmt.Fprintln(&sb, "  (no steps recorded for this build)")
		return sb.String()
	}

	if verbose {
		for _, s := range b.Steps {
			symbol := getStatusSymbol(s.Status)
			depth := strings.Count(s.Name, "|")
			indent := strings.Repeat("  ", depth)
			displayName := s.Name
			if depth > 0 {
				displayName = s.Name[strings.LastIndex(s.Name, "|")+1:]
			}
			cleanSum := cleanStepSummary(s.SummaryMarkdown)
			if cleanSum != "" {
				fmt.Fprintf(&sb, "  %s%s  %-40s  %s\n", indent, symbol, displayName, cleanSum)
			} else {
				fmt.Fprintf(&sb, "  %s%s  %s\n", indent, symbol, displayName)
			}
		}
		return sb.String()
	}

	const maxExpandedPassingSteps = 8

	// Map existing step names to determine true top-level steps
	stepNames := make(map[string]bool, len(b.Steps))
	for _, s := range b.Steps {
		stepNames[s.Name] = true
	}

	isTopLevelStep := func(s LUCIStep) bool {
		if !strings.Contains(s.Name, "|") {
			return true
		}
		parent := strings.Split(s.Name, "|")[0]
		return !stepNames[parent]
	}

	hasNonPassingChild := func(parentName string) bool {
		prefix := parentName + "|"
		for _, child := range b.Steps {
			if strings.HasPrefix(child.Name, prefix) && child.Status != "SUCCESS" {
				return true
			}
		}
		return false
	}

	passingTopLevelCount := 0
	for _, s := range b.Steps {
		if isTopLevelStep(s) && s.Status == "SUCCESS" && !hasNonPassingChild(s.Name) {
			passingTopLevelCount++
		}
	}
	leafFailingSteps := selectLeafFailingSteps(b.Steps)
	collapsePassingSteps := passingTopLevelCount > maxExpandedPassingSteps

	for _, s := range b.Steps {
		if !isTopLevelStep(s) {
			continue
		}
		if collapsePassingSteps && s.Status == "SUCCESS" && !hasNonPassingChild(s.Name) {
			continue
		}

		symbol := getStatusSymbol(s.Status)
		cleanSum := cleanStepSummary(s.SummaryMarkdown)
		if cleanSum != "" {
			fmt.Fprintf(&sb, "  %s  %-40s  %s\n", symbol, s.Name, cleanSum)
		} else {
			fmt.Fprintf(&sb, "  %s  %s\n", symbol, s.Name)
		}

		// If this top-level step failed, highlight failing leaf child steps
		if s.Status == "FAILURE" || s.Status == "INFRA_FAILURE" {
			prefix := s.Name + "|"
			var failingChildren []*LUCIStep
			for _, leaf := range leafFailingSteps {
				if strings.HasPrefix(leaf.Name, prefix) {
					failingChildren = append(failingChildren, leaf)
				}
			}
			for _, child := range failingChildren {
				childName := child.Name[len(prefix):]
				// Skip exact duplicate child name if more specific failing children exist
				if childName == s.Name && len(failingChildren) > 1 {
					continue
				}
				childSym := getStatusSymbol(child.Status)
				childDiag := extractStepDiagnostic(child.SummaryMarkdown)

				if childName == "failure summary" {
					if childDiag != "" {
						fmt.Fprintf(&sb, "     └── %s  failure summary: %s\n", childSym, childDiag)
					} else {
						fmt.Fprintf(&sb, "     └── %s  failure summary (run 'gh run view -j %s --log-failed' to inspect)\n", childSym, b.Builder.Builder)
					}
				} else {
					if childDiag != "" {
						fmt.Fprintf(&sb, "     └── %s  %-34s  %s\n", childSym, childName, childDiag)
					} else {
						fmt.Fprintf(&sb, "     └── %s  %s\n", childSym, childName)
					}
				}
			}
		}
	}
	if collapsePassingSteps {
		fmt.Fprintf(&sb, "  ✓  %d passing steps omitted (use -v to list)\n", passingTopLevelCount)
	}
	return sb.String()
}

// maxExpandedPassingChecks is the maximum number of passing (SUCCESS) checks
// rendered individually in default human-readable `pr checks` and `run view`
// output before collapsing into a single summary line (unless --all is set).
const maxExpandedPassingChecks = 5

func nonPassingStatusRank(status string) int {
	switch status {
	case "FAILURE", "INFRA_FAILURE":
		return 0
	case "STARTED", "SCHEDULED":
		return 1
	default:
		return 2
	}
}

// PartitionAndOrderBuilds separates builds into non-passing and passing (SUCCESS)
// slices. Non-passing builds are ordered with failures (FAILURE, INFRA_FAILURE)
// first, then running/scheduled (STARTED, SCHEDULED), then canceled/other,
// preserving relative order within each group.
func PartitionAndOrderBuilds(builds []bbBuild) (nonPassing, passing []bbBuild) {
	var failed, running, other []bbBuild
	for _, b := range builds {
		if b.Status == "SUCCESS" {
			passing = append(passing, b)
			continue
		}
		switch nonPassingStatusRank(b.Status) {
		case 0:
			failed = append(failed, b)
		case 1:
			running = append(running, b)
		default:
			other = append(other, b)
		}
	}
	nonPassing = make([]bbBuild, 0, len(failed)+len(running)+len(other))
	nonPassing = append(nonPassing, failed...)
	nonPassing = append(nonPassing, running...)
	nonPassing = append(nonPassing, other...)
	return nonPassing, passing
}

// selectVisibleBuilds partitions builds with failures first and collapses
// passing builds beyond maxExpandedPassingChecks unless showAllPassing is set.
func selectVisibleBuilds(builds []bbBuild, showAllPassing bool) (visible []bbBuild, omittedPassingLine string) {
	nonPassingBuilds, passingBuilds := PartitionAndOrderBuilds(builds)
	if !showAllPassing && len(passingBuilds) > maxExpandedPassingChecks {
		if len(nonPassingBuilds) == 0 {
			return nonPassingBuilds, fmt.Sprintf("All %d checks passed (use --all to list)", len(passingBuilds))
		}
		return nonPassingBuilds, fmt.Sprintf("%d passing checks omitted (use --all to list)", len(passingBuilds))
	}
	return slices.Concat(nonPassingBuilds, passingBuilds), ""
}

// formatCheckSummary returns a 1-line summary of checks status, highlighting failures.
func formatCheckSummary(builds []bbBuild, changeNum int) string {
	if len(builds) == 0 {
		return fmt.Sprintf("No checks scheduled (run 'gh pr review %d --cq' to trigger dry run)", changeNum)
	}

	var (
		passed         int
		running        int
		failedBuilders []string
	)

	for _, b := range deduplicateLatestBuilds(builds) {
		if b.IsExperimental() {
			continue
		}
		switch b.Status {
		case "SUCCESS":
			passed++
		case "FAILURE", "INFRA_FAILURE":
			if b.Builder.Builder != "" {
				failedBuilders = append(failedBuilders, b.Builder.Builder)
			}
		case "STARTED", "SCHEDULED":
			running++
		}
	}

	total := passed + len(failedBuilders) + running
	if total == 0 {
		return fmt.Sprintf("No checks scheduled (run 'gh pr review %d --cq' to trigger dry run)", changeNum)
	}

	if len(failedBuilders) > 0 {
		var failList string
		if len(failedBuilders) == 1 {
			failList = failedBuilders[0]
		} else if len(failedBuilders) <= 3 {
			failList = strings.Join(failedBuilders, ", ")
		} else {
			failList = fmt.Sprintf("%s, %s, %s, and %d more",
				failedBuilders[0], failedBuilders[1], failedBuilders[2], len(failedBuilders)-3)
		}

		suffix := fmt.Sprintf("(run 'gh run view %d --log-failed' to view errors)", changeNum)
		if running > 0 {
			suffix = fmt.Sprintf("(%d running; run 'gh run view %d --log-failed' to view errors)", running, changeNum)
		}

		if len(failedBuilders) == 1 {
			return fmt.Sprintf(GlyphFail+" 1 failed: %s %s", failList, suffix)
		}
		return fmt.Sprintf(GlyphFail+" %d failed: %s %s", len(failedBuilders), failList, suffix)
	}

	if running > 0 {
		if passed > 0 {
			return fmt.Sprintf(GlyphRunning+" %d passed, %d running (use 'gh pr checks --watch' to monitor)", passed, running)
		}
		return fmt.Sprintf(GlyphRunning+" %d running (use 'gh pr checks --watch' to monitor)", running)
	}

	return fmt.Sprintf(GlyphPass+" %d passing", passed)
}

const (
	// maxBuildbucketBatchSize caps the number of SearchBuilds sub-requests per
	// Buildbucket Builds/Batch pRPC call.
	maxBuildbucketBatchSize = 50
	// maxGetBuildBatchSize caps GetBuild sub-requests per Builds/Batch call so
	// step-heavy build responses fit comfortably within maxBatchHTTPBodyBytes.
	maxGetBuildBatchSize = 8
	// maxBatchHTTPBodyBytes bounds Builds/Batch HTTP responses (64 MiB).
	maxBatchHTTPBodyBytes = 64 * 1024 * 1024
	// defaultBuilderRunLimit is the default per-builder page size for
	// `gh run list -w` when -L/--limit is not specified.
	defaultBuilderRunLimit = 10
	// maxBuilderRunLimit is Buildbucket's maximum pageSize per SearchBuilds call.
	maxBuilderRunLimit = 1000
	// searchBuildsFieldsMask requests the Buildbucket fields needed by change-scoped
	// `SearchBuilds` queries.
	searchBuildsFieldsMask = "id,builder,status,create_time,start_time,end_time,summary_markdown,critical,input.experiments,tags"
	// searchBuilderBuildsMask adds `number` for builder-scoped `CheckItem` output.
	searchBuilderBuildsMask = "number," + searchBuildsFieldsMask
)

func canonicalAllowedBuildbucketHost(raw string) (string, bool) {
	trimmed := strings.TrimSpace(raw)
	if trimmed == "" {
		return "", false
	}
	isAllowedHost := func(h string) bool {
		return h == "cr-buildbucket.appspot.com" || h == "cr-buildbucket-dev.appspot.com"
	}
	if strings.Contains(trimmed, "://") {
		u, err := url.Parse(trimmed)
		if err != nil || !strings.EqualFold(u.Scheme, "https") || u.User != nil || u.Port() != "" || (u.Path != "" && u.Path != "/") || u.RawQuery != "" || u.Fragment != "" {
			return "", false
		}
		h := strings.ToLower(u.Hostname())
		if !isAllowedHost(h) {
			return "", false
		}
		return "https://" + h, true
	}
	if strings.ContainsAny(trimmed, "/?#@:") {
		return "", false
	}
	h := strings.ToLower(trimmed)
	if !isAllowedHost(h) {
		return "", false
	}
	return h, true
}

// ResolveEffectiveBuildbucketHost resolves the Buildbucket host from explicit
// CLI flags/overrides first, then validates and falls back to `configuredHost`
// from `.ghish.toml` (`ci.buildbucket_host`), failing closed on untrusted hosts.
func ResolveEffectiveBuildbucketHost(explicitHost string, flagChanged bool, configuredHost string) (string, error) {
	if flagChanged {
		return explicitHost, nil
	}
	if cfgHost := strings.TrimSpace(configuredHost); cfgHost != "" {
		canonical, ok := canonicalAllowedBuildbucketHost(cfgHost)
		if !ok {
			return "", fmt.Errorf("untrusted ci.buildbucket_host %q in .ghish.toml: must be 'cr-buildbucket.appspot.com' or 'cr-buildbucket-dev.appspot.com' over HTTPS", cfgHost)
		}
		if explicitHost != "" && explicitHost != "cr-buildbucket.appspot.com" {
			return explicitHost, nil
		}
		return canonical, nil
	}
	return explicitHost, nil
}

// BuilderSearchResult holds the recent builds (or per-builder RPC error) for one
// queried LUCI builder in `SearchBuilderBuildsBatch`.
type BuilderSearchResult struct {
	Project string
	Bucket  string
	Builder string
	Builds  []bbBuild
	Error   string
}

type bbBatchSubRequest struct {
	SearchBuilds *bbSearchBuildsRequest `json:"searchBuilds,omitempty"`
	GetBuild     *bbGetBuildRequest     `json:"getBuild,omitempty"`
}

type bbBatchRequest struct {
	Requests []bbBatchSubRequest `json:"requests"`
}

type bbRPCStatus struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
}

type bbBatchSubResponse struct {
	SearchBuilds *bbSearchBuildsResponse `json:"searchBuilds,omitempty"`
	GetBuild     *LUCIBuildDetails       `json:"getBuild,omitempty"`
	Error        *bbRPCStatus            `json:"error,omitempty"`
}

type bbBatchResponse struct {
	Responses []bbBatchSubResponse `json:"responses"`
}

func splitLUCIBucket(rawBucket, defaultProject string) (project, shortBucket string, ok bool) {
	b := strings.TrimSpace(rawBucket)
	if b == "" {
		return "", "", false
	}
	if strings.HasPrefix(b, "luci.") {
		if defaultProject != "" {
			if after, cut := strings.CutPrefix(b, "luci."+defaultProject+"."); cut && after != "" {
				return defaultProject, after, true
			}
		}
		proj, short, cut := strings.Cut(strings.TrimPrefix(b, "luci."), ".")
		if !cut || proj == "" || short == "" {
			return "", "", false
		}
		return proj, short, true
	}
	if defaultProject == "" {
		return "", b, false
	}
	return defaultProject, b, true
}

// ParseBuilderSelector parses a LUCI builder identifier in the form
// "<project>/<bucket>/<builder>", "buildbucket/luci.<project>.<bucket>/<builder>",
// "luci.<project>.<bucket>/<builder>", or "<bucket>/<builder>" (when defaultProject
// is non-empty).
func ParseBuilderSelector(raw, defaultProject string) (bbBuilder, error) {
	clean := strings.TrimSpace(raw)
	clean = strings.TrimPrefix(clean, "buildbucket/")
	parts := strings.Split(clean, "/")
	switch len(parts) {
	case 2:
		first, builder := strings.TrimSpace(parts[0]), strings.TrimSpace(parts[1])
		if builder != "" {
			if proj, bucket, ok := splitLUCIBucket(first, defaultProject); ok {
				return bbBuilder{Project: proj, Bucket: bucket, Builder: builder}, nil
			}
		}
	case 3:
		proj := strings.TrimSpace(parts[0])
		bucket := strings.TrimSpace(parts[1])
		builder := strings.TrimSpace(parts[2])
		if proj != "" && bucket != "" && builder != "" {
			if strings.HasPrefix(bucket, "luci.") {
				p, short, ok := splitLUCIBucket(bucket, proj)
				if !ok || p != proj {
					break
				}
				bucket = short
			}
			return bbBuilder{Project: proj, Bucket: bucket, Builder: builder}, nil
		}
	}
	return bbBuilder{}, fmt.Errorf("invalid LUCI builder selector %q: expected '<project>/<bucket>/<builder>' (for example, 'my-project/ci/linux-build').\n\n"+
		"Usage examples:\n"+
		"  gh run list -w <project>/<bucket>/<builder> -L 5\n"+
		"  gh run list -w <project>/<bucket>/<builder> --status completed\n\n"+
		"To inspect checks on a Gerrit change instead, run:\n"+
		"  gh run list [<change-id>]", raw)
}

// ParseRunStatusFilter validates a `--status` flag value on `gh run list` and
// maps it to the corresponding Buildbucket status or status mask.
func ParseRunStatusFilter(raw string) (string, error) {
	clean := strings.TrimSpace(raw)
	if clean == "" {
		return "", nil
	}
	switch strings.ToLower(clean) {
	case "completed", "ended", "ended_mask":
		return "ENDED_MASK", nil
	case "in_progress", "started", "running":
		return "STARTED", nil
	case "queued", "scheduled", "pending":
		return "SCHEDULED", nil
	case "success", "passed":
		return "SUCCESS", nil
	case "failure", "failed":
		return "FAILURE", nil
	case "infra_failure", "infra":
		return "INFRA_FAILURE", nil
	case "canceled", "cancelled":
		return "CANCELED", nil
	default:
		return "", fmt.Errorf("invalid --status value %q: must be one of completed, in_progress, queued, success, failure, infra_failure, or canceled", raw)
	}
}

func formatBatchSubError(errStatus *bbRPCStatus) string {
	if errStatus == nil {
		return ""
	}
	if msg := strings.TrimSpace(errStatus.Message); msg != "" {
		return msg
	}
	return fmt.Sprintf("RPC code %d", errStatus.Code)
}

func (c *LUCIClient) executeBuildsBatch(
	ctx context.Context,
	n, chunkSize int,
	makeSubReq func(idx int) bbBatchSubRequest,
	handleResp func(idx int, sub bbBatchSubResponse),
) error {
	numChunks := (n + chunkSize - 1) / chunkSize
	chunkErrs := make([]error, numChunks)
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()

	sem := make(chan struct{}, maxConcurrentBuildSearches)
	var wg sync.WaitGroup
	for chunkIdx := range numChunks {
		start := chunkIdx * chunkSize
		end := min(start+chunkSize, n)
		wg.Add(1)
		go func(cIdx, lo, hi int) {
			defer wg.Done()
			select {
			case sem <- struct{}{}:
				defer func() { <-sem }()
			case <-ctx.Done():
				chunkErrs[cIdx] = ctx.Err()
				return
			}
			if err := ctx.Err(); err != nil {
				chunkErrs[cIdx] = err
				return
			}

			reqs := make([]bbBatchSubRequest, hi-lo)
			for i := range hi - lo {
				reqs[i] = makeSubReq(lo + i)
			}
			var batchResp bbBatchResponse
			if err := c.callPRPCWithMaxBytes(ctx, "buildbucket.v2.Builds", "Batch", bbBatchRequest{Requests: reqs}, &batchResp, maxBatchHTTPBodyBytes); err != nil {
				chunkErrs[cIdx] = err
				cancel()
				return
			}
			if len(batchResp.Responses) != len(reqs) {
				chunkErrs[cIdx] = fmt.Errorf("Buildbucket Builds/Batch returned %d responses for %d requests at offset %d",
					len(batchResp.Responses), len(reqs), lo)
				cancel()
				return
			}
			for i, sub := range batchResp.Responses {
				handleResp(lo+i, sub)
			}
		}(chunkIdx, start, end)
	}
	wg.Wait()
	return firstNonCanceledError(chunkErrs)
}

// SearchBuilderBuildsBatch queries Buildbucket for recent builds across one or
// more LUCI builders, batching multi-builder queries via buildbucket.v2.Builds/Batch
// with bounded concurrency.
func (c *LUCIClient) SearchBuilderBuildsBatch(ctx context.Context, builders []bbBuilder, limit int, statusFilter string) ([]BuilderSearchResult, error) {
	if c == nil {
		return nil, fmt.Errorf("LUCIClient is nil")
	}
	if len(builders) == 0 {
		return nil, fmt.Errorf("builders list cannot be empty")
	}
	bbStatus, err := ParseRunStatusFilter(statusFilter)
	if err != nil {
		return nil, err
	}
	pageSize := limit
	if pageSize <= 0 {
		pageSize = defaultBuilderRunLimit
	}
	if pageSize > maxBuilderRunLimit {
		return nil, fmt.Errorf("invalid --limit %d: must be between 1 and %d", limit, maxBuilderRunLimit)
	}

	makeSearchReq := func(b bbBuilder) *bbSearchBuildsRequest {
		bCopy := b
		return &bbSearchBuildsRequest{
			Predicate: bbPredicate{
				Builder: &bCopy,
				Status:  bbStatus,
			},
			Mask:     bbBuildMask{Fields: searchBuilderBuildsMask},
			PageSize: pageSize,
		}
	}

	stampForBuilder := func(b bbBuilder, rawBuilds []bbBuild) []bbBuild {
		out := make([]bbBuild, len(rawBuilds))
		copy(out, rawBuilds)
		stampBuildbucketBuilds(out)
		for i := range out {
			out[i].Builder.Project = cmp.Or(out[i].Builder.Project, b.Project)
			out[i].Builder.Bucket = cmp.Or(out[i].Builder.Bucket, b.Bucket)
			out[i].Builder.Builder = cmp.Or(out[i].Builder.Builder, b.Builder)
		}
		return out
	}

	results := make([]BuilderSearchResult, len(builders))
	for i, b := range builders {
		results[i] = BuilderSearchResult{
			Project: b.Project,
			Bucket:  b.Bucket,
			Builder: b.Builder,
			Builds:  []bbBuild{},
		}
	}

	if len(builders) == 1 {
		b := builders[0]
		var resp bbSearchBuildsResponse
		if err := c.CallPRPC(ctx, "buildbucket.v2.Builds", "SearchBuilds", makeSearchReq(b), &resp); err != nil {
			return nil, err
		}
		results[0].Builds = stampForBuilder(b, resp.Builds)
		return results, nil
	}

	err = c.executeBuildsBatch(
		ctx,
		len(builders),
		maxBuildbucketBatchSize,
		func(idx int) bbBatchSubRequest {
			return bbBatchSubRequest{SearchBuilds: makeSearchReq(builders[idx])}
		},
		func(idx int, sub bbBatchSubResponse) {
			if sub.Error != nil {
				results[idx].Error = formatBatchSubError(sub.Error)
				return
			}
			if sub.SearchBuilds != nil {
				results[idx].Builds = stampForBuilder(builders[idx], sub.SearchBuilds.Builds)
			}
		},
	)
	if err != nil {
		return nil, err
	}

	if !slices.ContainsFunc(results, func(r BuilderSearchResult) bool { return r.Error == "" }) {
		r := results[0]
		return nil, fmt.Errorf("Buildbucket SearchBuilds failed for all %d builders (first: %s/%s/%s: %s)",
			len(results), r.Project, r.Bucket, r.Builder, SanitizeUntrustedText(r.Error))
	}
	return results, nil
}

func firstNonCanceledErrorIndex(errs []error) int {
	failed := -1
	for i, err := range errs {
		if err == nil {
			continue
		}
		if failed < 0 || (errors.Is(errs[failed], context.Canceled) && !errors.Is(err, context.Canceled)) {
			failed = i
		}
	}
	return failed
}

func firstNonCanceledError(errs []error) error {
	if idx := firstNonCanceledErrorIndex(errs); idx >= 0 {
		return errs[idx]
	}
	return nil
}

// GetBuildDetailsBatch fetches step-level details for one or more Buildbucket
// build IDs, batching multi-ID requests via buildbucket.v2.Builds/Batch.
func (c *LUCIClient) GetBuildDetailsBatch(ctx context.Context, buildIDs []string) ([]*LUCIBuildDetails, error) {
	if c == nil {
		return nil, fmt.Errorf("LUCIClient is nil")
	}
	if len(buildIDs) == 0 {
		return nil, fmt.Errorf("buildIDs cannot be empty")
	}
	if len(buildIDs) == 1 {
		det, err := c.GetBuildDetails(ctx, buildIDs[0])
		if err != nil {
			return nil, err
		}
		return []*LUCIBuildDetails{det}, nil
	}

	results := make([]*LUCIBuildDetails, len(buildIDs))
	err := c.executeBuildsBatch(
		ctx,
		len(buildIDs),
		maxGetBuildBatchSize,
		func(idx int) bbBatchSubRequest {
			return bbBatchSubRequest{
				GetBuild: &bbGetBuildRequest{
					ID:   buildIDs[idx],
					Mask: bbBuildMask{Fields: getBuildDetailsFieldsMask},
				},
			}
		},
		func(idx int, sub bbBatchSubResponse) {
			id := buildIDs[idx]
			if sub.Error != nil {
				results[idx] = &LUCIBuildDetails{ID: id, Error: formatBatchSubError(sub.Error)}
				return
			}
			if sub.GetBuild == nil {
				results[idx] = &LUCIBuildDetails{ID: id, Error: "missing getBuild payload in BatchResponse"}
				return
			}
			det := sub.GetBuild
			det.ID = cmp.Or(det.ID, id)
			det.Provider = cmp.Or(det.Provider, "buildbucket")
			results[idx] = det
		},
	)
	if err != nil {
		return nil, err
	}

	if !slices.ContainsFunc(results, func(d *LUCIBuildDetails) bool { return d != nil && d.Error == "" }) {
		return nil, fmt.Errorf("Buildbucket GetBuild failed for all %d builds (first: %s: %s)",
			len(results), results[0].ID, SanitizeUntrustedText(results[0].Error))
	}
	return results, nil
}
