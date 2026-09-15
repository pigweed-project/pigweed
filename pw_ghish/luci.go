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
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"regexp"
	"strings"
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

// LUCIBuildDetails holds detailed build information including steps and logs.
type LUCIBuildDetails struct {
	ID                   string             `json:"id"`
	Builder              bbBuilder          `json:"builder"`
	Status               string             `json:"status"`
	SummaryMarkdown      string             `json:"summaryMarkdown,omitempty"`
	CancellationMarkdown string             `json:"cancellationMarkdown,omitempty"`
	StatusDetails        *LUCIStatusDetails `json:"statusDetails,omitempty"`
	Steps                []LUCIStep         `json:"steps,omitempty"`
}

// FailureReport encapsulates diagnostic information for a failed check.
type FailureReport struct {
	BuildID     string `json:"buildId"`
	Builder     string `json:"builder"`
	Status      string `json:"status"`
	BuildURL    string `json:"buildUrl"`
	FailedStep  string `json:"failedStep"`
	StepSummary string `json:"stepSummary,omitempty"`
	LogName     string `json:"logName,omitempty"`
	LogSnippet  string `json:"logSnippet,omitempty"`
	FullLogURL  string `json:"fullLogUrl,omitempty"`
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

type bbInput struct {
	Experiments []string `json:"experiments"`
}

type bbBuild struct {
	ID              string    `json:"id"`
	Builder         bbBuilder `json:"builder"`
	Status          string    `json:"status"`
	SummaryMarkdown string    `json:"summaryMarkdown"`
	Critical        string    `json:"critical,omitempty"`
	Input           *bbInput  `json:"input,omitempty"`
	Tags            []bbTag   `json:"tags,omitempty"`
	CreateTime      string    `json:"createTime"`
	StartTime       string    `json:"startTime"`
	EndTime         string    `json:"endTime"`
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
}

type bbPredicate struct {
	GerritChanges []bbGerritChange `json:"gerritChanges"`
}

type bbGerritChange struct {
	Host     string `json:"host"`
	Project  string `json:"project"`
	Change   int    `json:"change"`
	Patchset int    `json:"patchset"`
}

type bbSearchBuildsResponse struct {
	Builds []bbBuild `json:"builds"`
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
	Host       string
	HTTPClient *http.Client
}

// NewLUCIClient creates a new LUCIClient targeting the given Buildbucket host.
// If httpClient is nil, http.DefaultClient is used.
func NewLUCIClient(host string, httpClient *http.Client) *LUCIClient {
	if httpClient == nil {
		httpClient = http.DefaultClient
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
	httpClient := c.HTTPClient
	if httpClient == nil {
		httpClient = http.DefaultClient
	}

	jsonBytes, err := json.Marshal(req)
	if err != nil {
		return fmt.Errorf("failed to marshal %s/%s request: %w", service, method, err)
	}

	endpoint := fmt.Sprintf("https://%s/prpc/%s/%s", c.Host, service, method)
	if strings.HasPrefix(c.Host, "http://") || strings.HasPrefix(c.Host, "https://") {
		endpoint = fmt.Sprintf("%s/prpc/%s/%s", c.Host, service, method)
	}

	if VerboseFlag {
		fmt.Fprintf(os.Stderr, "[debug] pRPC request: %s %s\n", endpoint, string(jsonBytes))
	}

	httpReq, err := http.NewRequestWithContext(ctx, "POST", endpoint, bytes.NewReader(jsonBytes))
	if err != nil {
		return fmt.Errorf("failed to create %s/%s request: %w", service, method, err)
	}
	httpReq.Header.Set("Content-Type", "application/json")
	httpReq.Header.Set("Accept", "application/json")

	httpResp, err := httpClient.Do(httpReq)
	if err != nil {
		return fmt.Errorf("%s/%s request failed: %w", service, method, err)
	}
	defer httpResp.Body.Close()

	bodyBytes, err := io.ReadAll(httpResp.Body)
	if err != nil {
		return fmt.Errorf("failed to read %s/%s response: %w", service, method, err)
	}

	if httpResp.StatusCode != http.StatusOK {
		return fmt.Errorf("Buildbucket %s/%s returned HTTP %d: %s", service, method, httpResp.StatusCode, string(bodyBytes))
	}

	bodyBytes = bytes.TrimPrefix(bodyBytes, []byte(")]}'\n"))
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

	reqPayload := bbSearchBuildsRequest{
		Predicate: bbPredicate{
			GerritChanges: []bbGerritChange{
				{
					Host:     gerritHost,
					Project:  project,
					Change:   changeNum,
					Patchset: patchsetNum,
				},
			},
		},
		Mask: bbBuildMask{
			Fields: "id,builder,status,create_time,start_time,end_time,summary_markdown,critical,input.experiments,tags",
		},
	}

	var searchResp bbSearchBuildsResponse
	if err := c.CallPRPC(ctx, "buildbucket.v2.Builds", "SearchBuilds", reqPayload, &searchResp); err != nil {
		return nil, err
	}

	return searchResp.Builds, nil
}

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
			Fields: "id,builder,status,summary_markdown,cancellation_markdown,status_details,steps",
		},
	}

	var details LUCIBuildDetails
	if err := c.CallPRPC(ctx, "buildbucket.v2.Builds", "GetBuild", reqPayload, &details); err != nil {
		return nil, err
	}

	return &details, nil
}

// FetchLogStream downloads raw text logs from LogDog (?format=raw), optionally returning the last maxLines.
func (c *LUCIClient) FetchLogStream(ctx context.Context, viewURL string, maxLines int) (string, error) {
	if c == nil {
		return "", fmt.Errorf("LUCIClient is nil")
	}
	if viewURL == "" {
		return "", fmt.Errorf("viewURL cannot be empty")
	}
	httpClient := c.HTTPClient
	if httpClient == nil {
		httpClient = http.DefaultClient
	}

	parsed, err := url.Parse(viewURL)
	if err != nil {
		return "", fmt.Errorf("invalid log URL %q: %w", viewURL, err)
	}

	q := parsed.Query()
	q.Set("format", "raw")
	parsed.RawQuery = q.Encode()

	req, err := http.NewRequestWithContext(ctx, "GET", parsed.String(), nil)
	if err != nil {
		return "", fmt.Errorf("failed to create log request: %w", err)
	}

	resp, err := httpClient.Do(req)
	if err != nil {
		return "", fmt.Errorf("failed to fetch log: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		return "", fmt.Errorf("LogDog returned HTTP %d: %s", resp.StatusCode, string(body))
	}

	rawBytes, err := io.ReadAll(resp.Body)
	if err != nil {
		return "", fmt.Errorf("failed to read log body: %w", err)
	}

	text := string(rawBytes)
	if maxLines <= 0 {
		return text, nil
	}

	trimmed := strings.TrimRight(text, "\r\n")
	lines := strings.Split(trimmed, "\n")
	if len(lines) <= maxLines {
		return text, nil
	}

	return strings.Join(lines[len(lines)-maxLines:], "\n"), nil
}

// ExtractFailureReport analyzes steps in a failing build and retrieves the primary failure diagnostics.
func (c *LUCIClient) ExtractFailureReport(ctx context.Context, b *LUCIBuildDetails, maxLogLines int) *FailureReport {
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
		BuildURL: fmt.Sprintf("https://ci.chromium.org/b/%s", b.ID),
	}

	// Find the failing step, prioritizing steps that have accessible logs (stdout, stderr, etc.)
	var failedStep *LUCIStep
	var targetLog *LUCILog

	for i := range b.Steps {
		step := &b.Steps[i]
		if step.Status != "FAILURE" && step.Status != "INFRA_FAILURE" {
			continue
		}
		if failedStep == nil {
			failedStep = step
		}

		for j := range step.Logs {
			log := &step.Logs[j]
			if log.ViewURL == "" {
				continue
			}
			if log.Name == "stdout" || log.Name == "stderr" || log.Name == "full contents" {
				failedStep = step
				targetLog = log
				break
			}
			if targetLog == nil && !strings.HasPrefix(log.Name, "$") {
				failedStep = step
				targetLog = log
			}
		}
	}

	if failedStep == nil {
		if b.SummaryMarkdown != "" {
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
		if b.SummaryMarkdown != "" {
			report.StepSummary = b.SummaryMarkdown
		} else if b.CancellationMarkdown != "" {
			report.StepSummary = b.CancellationMarkdown
		}
	}

	if targetLog != nil && targetLog.ViewURL != "" {
		report.LogName = targetLog.Name
		report.FullLogURL = targetLog.ViewURL
		snippet, err := c.FetchLogStream(ctx, targetLog.ViewURL, maxLogLines)
		if err != nil {
			report.LogSnippet = fmt.Sprintf("[Error fetching log stream from %s: %v]", targetLog.ViewURL, err)
		} else {
			report.LogSnippet = strings.TrimSpace(snippet)
		}
	}

	return report
}

// GetBuildDetails fetches step-level details for a build from Buildbucket.
func GetBuildDetails(ctx context.Context, bbHost, buildID string, httpClient *http.Client) (*LUCIBuildDetails, error) {
	return NewLUCIClient(bbHost, httpClient).GetBuildDetails(ctx, buildID)
}

// FetchLogStream downloads raw text logs from LogDog (?format=raw), optionally returning the last maxLines.
func FetchLogStream(ctx context.Context, viewURL string, maxLines int, httpClient *http.Client) (string, error) {
	return NewLUCIClient("", httpClient).FetchLogStream(ctx, viewURL, maxLines)
}

// ExtractFailureReport analyzes steps in a failing build and retrieves the primary failure diagnostics.
func (b *LUCIBuildDetails) ExtractFailureReport(ctx context.Context, maxLogLines int, httpClient *http.Client) *FailureReport {
	return NewLUCIClient("", httpClient).ExtractFailureReport(ctx, b, maxLogLines)
}

// cleanBuildSummary removes internal infra experiment noise from the top-level build summary.
func cleanBuildSummary(summary string) string {
	summary = strings.TrimSpace(summary)
	if summary == "" {
		return ""
	}
	lines := strings.Split(summary, "\n")
	var kept []string
	inExperiments := false
	for _, line := range lines {
		trimmed := strings.TrimSpace(line)
		if strings.HasPrefix(trimmed, "**Experiments**") || strings.HasPrefix(trimmed, "Experiments:") {
			inExperiments = true
			continue
		}
		if inExperiments {
			if strings.HasPrefix(trimmed, "* ") || strings.HasPrefix(trimmed, "- ") || trimmed == "" {
				continue
			}
			inExperiments = false
		}
		kept = append(kept, line)
	}
	return strings.TrimSpace(strings.Join(kept, "\n"))
}

// cleanStepSummary sanitizes recipe step summaries, filtering out multi-line dumps,
// Python repr objects, raw JSON/dicts, and disk usage noise.
func cleanStepSummary(summary string) string {
	summary = strings.TrimSpace(summary)
	if summary == "" {
		return ""
	}
	// Suppress multi-line summaries or raw data dumps
	if strings.Contains(summary, "\n") {
		return ""
	}
	// Suppress Python repr / object dumps
	if strings.HasPrefix(summary, "Change(") || strings.HasPrefix(summary, "applied [") || strings.HasPrefix(summary, "[Change(") {
		return ""
	}
	// Suppress JSON / YAML / dict dumps
	if strings.HasPrefix(summary, "{") || strings.HasPrefix(summary, "[") || strings.HasPrefix(summary, "remote:") || strings.Contains(summary, "{\"") || strings.Contains(summary, "{'") {
		return ""
	}
	// Suppress disk usage noise
	if strings.Contains(summary, "GB used") {
		return ""
	}
	// Suppress markdown links/bullets that don't belong inline
	if strings.HasPrefix(summary, "* ") || strings.HasPrefix(summary, "- ") {
		return ""
	}
	if len(summary) > 120 {
		return summary[:117] + "..."
	}
	return summary
}

var (
	diffFileRegex = regexp.MustCompile(`^\+\+\+\s+(?:[ab]/)?([^\s\t]+)`)
	botPathRegex  = regexp.MustCompile(`\[\.\.\.\]/[^:]+/`)
)

// extractStepDiagnostic extracts a concise single-line failure diagnostic from
// recipe step summaries (such as unified diffs, compiler diagnostics, or failure reasons).
func extractStepDiagnostic(summary string) string {
	summary = strings.TrimSpace(summary)
	summary = strings.Trim(summary, "`")
	summary = strings.TrimSpace(summary)
	if summary == "" {
		return ""
	}

	// 1. Check for unified diff headers (linters and formatters)
	var diffFiles []string
	seenFiles := make(map[string]bool)
	lines := strings.Split(summary, "\n")
	for _, line := range lines {
		line = strings.TrimSpace(line)
		m := diffFileRegex.FindStringSubmatch(line)
		if len(m) > 1 {
			rawPath := m[1]
			cleanPath := rawPath
			if idx := strings.Index(cleanPath, "/co/"); idx != -1 {
				cleanPath = cleanPath[idx+4:]
			} else if idx := strings.Index(cleanPath, "/checkout/"); idx != -1 {
				cleanPath = cleanPath[idx+10:]
			} else if strings.Contains(cleanPath, "/s/w/ir/") {
				parts := strings.Split(cleanPath, "/")
				cleanPath = parts[len(parts)-1]
			}
			cleanPath = strings.TrimSpace(cleanPath)
			if cleanPath != "" && !seenFiles[cleanPath] {
				seenFiles[cleanPath] = true
				diffFiles = append(diffFiles, cleanPath)
			}
		}
	}
	if len(diffFiles) == 1 {
		return fmt.Sprintf("formatting diff in %s", diffFiles[0])
	} else if len(diffFiles) > 1 && len(diffFiles) <= 3 {
		return fmt.Sprintf("formatting diff in %d files: %s", len(diffFiles), strings.Join(diffFiles, ", "))
	} else if len(diffFiles) > 3 {
		return fmt.Sprintf("formatting diff in %d files: %s, %s (+%d more)", len(diffFiles), diffFiles[0], diffFiles[1], len(diffFiles)-2)
	}

	// 2. Check for compiler, linter, or fatal error lines
	var errorLine string
	for i, line := range lines {
		l := strings.TrimSpace(line)
		if l == "" || strings.HasPrefix(l, "``") || strings.HasPrefix(l, "---") || strings.HasPrefix(l, "+++") {
			continue
		}
		if strings.Contains(l, "error:") || strings.Contains(l, "error [") || strings.Contains(l, "fatal error:") {
			cleaned := botPathRegex.ReplaceAllString(l, "")
			// If the line ends with "error:", pull in the next non-empty line
			if (strings.HasSuffix(cleaned, "error:") || strings.HasSuffix(cleaned, "fatal error:")) && i+1 < len(lines) {
				next := strings.TrimSpace(lines[i+1])
				if next != "" && !strings.HasPrefix(next, "``") {
					cleaned = cleaned + " " + next
				}
			}
			errorLine = cleaned
			break
		}
		if strings.HasPrefix(l, "FAILED:") && errorLine == "" {
			errorLine = l
		}
	}
	if errorLine != "" {
		if len(errorLine) > 80 {
			errorLine = errorLine[:77] + "..."
		}
		return errorLine
	}

	// 3. Check for test failure summaries (e.g. "Found 1 error in 1 file", "FAILED (failures=1)")
	for _, line := range lines {
		l := strings.TrimSpace(line)
		if strings.HasPrefix(l, "Found ") && strings.Contains(l, "error") {
			if len(l) > 80 {
				l = l[:77] + "..."
			}
			return l
		}
	}

	// 4. Single-line summary fallback if clean and not raw data
	if !strings.Contains(summary, "\n") {
		clean := cleanStepSummary(summary)
		if clean != "" {
			return clean
		}
	}

	// 5. First non-empty, non-fence, non-header line as fallback
	for _, line := range lines {
		l := strings.TrimSpace(line)
		if l == "" || strings.HasPrefix(l, "``") || strings.HasPrefix(l, "[ACTION") || strings.HasPrefix(l, "python ") {
			continue
		}
		if strings.HasPrefix(l, "Change(") || strings.HasPrefix(l, "{") || strings.HasPrefix(l, "[") {
			continue
		}
		if len(l) > 80 {
			l = l[:77] + "..."
		}
		return l
	}

	return ""
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
	fmt.Fprintf(&sb, "Status: %s %s | URL: https://ci.chromium.org/b/%s\n", b.Status, getStatusSymbol(b.Status), b.ID)

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

	// Map existing step names to determine true top-level steps
	stepNames := make(map[string]bool, len(b.Steps))
	for _, s := range b.Steps {
		stepNames[s.Name] = true
	}

	for _, s := range b.Steps {
		// A step is top-level if it contains no '|', OR if its parent does not exist in b.Steps.
		isTopLevel := true
		if strings.Contains(s.Name, "|") {
			parent := strings.Split(s.Name, "|")[0]
			if stepNames[parent] {
				isTopLevel = false
			}
		}

		if !isTopLevel {
			continue
		}

		symbol := getStatusSymbol(s.Status)
		cleanSum := cleanStepSummary(s.SummaryMarkdown)
		if cleanSum != "" {
			fmt.Fprintf(&sb, "  %s  %-40s  %s\n", symbol, s.Name, cleanSum)
		} else {
			fmt.Fprintf(&sb, "  %s  %s\n", symbol, s.Name)
		}

		// If this top-level step failed, highlight failing child steps
		if s.Status == "FAILURE" || s.Status == "INFRA_FAILURE" {
			prefix := s.Name + "|"
			var failingChildren []LUCIStep
			for _, child := range b.Steps {
				if strings.HasPrefix(child.Name, prefix) && (child.Status == "FAILURE" || child.Status == "INFRA_FAILURE") {
					failingChildren = append(failingChildren, child)
				}
			}
			for _, child := range failingChildren {
				childName := child.Name[len(prefix):]
				// Skip recipe log collection containers and rerun helpers
				if childName == "logs" || strings.HasPrefix(childName, "easy rerun cmd") || strings.HasPrefix(childName, "logs|") {
					continue
				}
				// Skip exact duplicate child name if more specific failing children exist
				if childName == s.Name && len(failingChildren) > 1 {
					continue
				}
				// Skip intermediate containers if there is a deeper failing descendant
				hasFailingChild := false
				for _, other := range failingChildren {
					if other.Name != child.Name && strings.HasPrefix(other.Name, child.Name+"|") {
						hasFailingChild = true
						break
					}
				}
				if hasFailingChild {
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
	return sb.String()
}

// FormatFailureReports formats diagnostic failure reports into a readable string.
func FormatFailureReports(reports []FailureReport) string {
	var sb strings.Builder
	for i, r := range reports {
		if i > 0 {
			sb.WriteString("\n")
		}
		sb.WriteString("================================================================================\n")
		fmt.Fprintf(&sb, "FAILURE: %s (Build %s)\n", r.Builder, r.BuildID)
		fmt.Fprintf(&sb, "Status: %s | URL: %s\n", r.Status, r.BuildURL)
		if r.FailedStep != "" {
			fmt.Fprintf(&sb, "Step: %s\n", r.FailedStep)
		}
		sb.WriteString("================================================================================\n")
		if r.StepSummary != "" {
			sb.WriteString(r.StepSummary)
			sb.WriteString("\n")
		}
		if r.LogSnippet != "" {
			header := "Log Snippet"
			if r.LogName != "" {
				header = fmt.Sprintf("Log Snippet (%s)", r.LogName)
			}
			fmt.Fprintf(&sb, "\n--- %s ---\n", header)
			sb.WriteString(r.LogSnippet)
			sb.WriteString("\n")
		}
		if r.FullLogURL != "" {
			fmt.Fprintf(&sb, "\nFull Log URL: %s\n", r.FullLogURL)
		}
	}
	return sb.String()
}

// formatCheckSummary returns a 1-line summary of checks status, highlighting failures.
func formatCheckSummary(builds []bbBuild) string {
	if len(builds) == 0 {
		return "No checks scheduled (run 'gh pr review --cq' to trigger dry run)"
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
		return "No checks scheduled (run 'gh pr review --cq' to trigger dry run)"
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

		suffix := "(run 'gh run view --log-failed' to view errors)"
		if running > 0 {
			suffix = fmt.Sprintf("(%d running; run 'gh run view --log-failed' to view errors)", running)
		}

		if len(failedBuilders) == 1 {
			return fmt.Sprintf("✖ 1 failed: %s %s", failList, suffix)
		}
		return fmt.Sprintf("✖ %d failed: %s %s", len(failedBuilders), failList, suffix)
	}

	if running > 0 {
		if passed > 0 {
			return fmt.Sprintf("● %d passed, %d running (use 'gh pr checks --watch' to monitor)", passed, running)
		}
		return fmt.Sprintf("● %d running (use 'gh pr checks --watch' to monitor)", running)
	}

	return fmt.Sprintf("✓ %d passing", passed)
}
