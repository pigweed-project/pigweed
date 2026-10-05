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
	"regexp"
	"strings"
	"time"

	"github.com/andygrunwald/go-gerrit"
)

const (
	// FetchArtifactBinPath is the local corp workstation path for Android's fetch_artifact tool.
	FetchArtifactBinPath = "/google/data/ro/projects/android/fetch_artifact"
	// AntsCLIBinPath is the local corp workstation path for Android's AnTS test results CLI.
	AntsCLIBinPath = "/google/data/ro/projects/android/ants_cli"
)

var (
	// BusytownAndroidBuildAPIBase is the base URL for the Android Build Internal v3 REST API.
	BusytownAndroidBuildAPIBase = "https://androidbuildinternal.googleapis.com"
	// BusytownAndroidCIBase is the base URL for ci.android.com artifact and build links.
	BusytownAndroidCIBase = "https://ci.android.com"

	// BusytownFileExistsFn checks whether a local accelerator binary exists on disk.
	// Overridden in NewMockGerritServer so unit tests remain 100% hermetic on corp workstations.
	BusytownFileExistsFn = func(path string) bool {
		st, err := os.Stat(path)
		return err == nil && !st.IsDir()
	}

	// BusytownExecCommandFn executes local accelerator binaries (fetch_artifact, ants_cli).
	BusytownExecCommandFn = func(ctx context.Context, name string, args ...string) ([]byte, error) {
		return exec.CommandContext(ctx, name, args...).CombinedOutput()
	}

	// getBusytownHTTPClient returns an authenticated HTTP client for Android Build / ci.android.com.
	getBusytownHTTPClient = func(ctx context.Context) *http.Client {
		return &http.Client{
			Transport: &AndroidBuildAuthTransport{Base: http.DefaultTransport},
		}
	}
)

var (
	busytownBuildIDPattern       = regexp.MustCompile(`^(?:(?i:[PLI]\d+)|\d{7,10})$`)
	busytownBuildTargetPattern   = regexp.MustCompile(`^((?i:P?\d+))/([A-Za-z0-9_.-]+)$`)
	digitsOnlyPattern            = regexp.MustCompile(`^\d+$`)
	treeHuggerWorkplanRegex      = regexp.MustCompile(`(?i)Workplan\s+ID:\s*(L\d+)`)
	treeHuggerAndroidCIURLRegex  = regexp.MustCompile(`https://ci\.android\.com/builds/(?:pending|submitted)/(P?\d+)/([^\s/)]+)(?:/latest)?`)
	treeHuggerStandaloneBIDRegex = regexp.MustCompile(`\b(P\d{6,})\b`)
	treeHuggerPatchsetRegex      = regexp.MustCompile(`^Patch Set (\d+):`)
)

// IsBusytownBuildID reports whether id matches an Android Busytown / TreeHugger build or workplan identifier
// (such as "P99164207", "L87654321", "12345678", or "P99164207/target_name").
func IsBusytownBuildID(id string) bool {
	trimmed := strings.TrimSpace(id)
	if trimmed == "" {
		return false
	}
	return busytownBuildIDPattern.MatchString(trimmed) || busytownBuildTargetPattern.MatchString(trimmed)
}

// ParseBusytownBuildTarget parses a direct Busytown build specifier (either "<bid>/<target>"
// or "<bid>" paired with an explicit "--target <target>" flag).
func ParseBusytownBuildTarget(raw string, explicitTarget string) (buildID string, target string, ok bool) {
	trimmed := strings.TrimSpace(raw)
	explicitTarget = strings.TrimSpace(explicitTarget)
	if m := busytownBuildTargetPattern.FindStringSubmatch(trimmed); len(m) == 3 {
		return m[1], m[2], true
	}
	if busytownBuildIDPattern.MatchString(trimmed) {
		return trimmed, explicitTarget, true
	}
	if explicitTarget != "" && isDigitsOnly(trimmed) {
		return trimmed, explicitTarget, true
	}
	return "", "", false
}

func isDigitsOnly(s string) bool {
	return digitsOnlyPattern.MatchString(s)
}

func formatGerritTimestamp(ts gerrit.Timestamp) string {
	if ts.Time.IsZero() {
		return ""
	}
	return ts.Time.UTC().Format(time.RFC3339)
}

// BusytownBuildURL formats the canonical ci.android.com web URL for a Busytown build ID and target.
func BusytownBuildURL(buildID, target string) string {
	buildID = strings.TrimSpace(buildID)
	target = strings.TrimSpace(target)
	if buildID == "" || strings.HasPrefix(buildID, "treehugger-") || strings.HasPrefix(buildID, "busytown-") {
		return "https://ci.android.com"
	}
	if strings.HasPrefix(strings.ToUpper(buildID), "L") {
		return fmt.Sprintf("https://ci.android.com/builds/workplans/%s", buildID)
	}
	kind := "submitted"
	if strings.HasPrefix(strings.ToUpper(buildID), "P") {
		kind = "pending"
	}
	if target == "" {
		return fmt.Sprintf("https://ci.android.com/builds/%s/%s", kind, buildID)
	}
	return fmt.Sprintf("https://ci.android.com/builds/%s/%s/%s/latest", kind, buildID, target)
}

// BusytownProvider implements CIProvider for Android Busytown / TreeHugger (`treetop~` + `ci.android.com`).
type BusytownProvider struct {
	GerritClient *GerritClient
	Cfg          *Config
}

func (p *BusytownProvider) Name() string {
	return "busytown"
}

type busytownTaskJSON struct {
	Name         string `json:"name"`
	Target       string `json:"target"`
	Status       string `json:"status"`
	State        string `json:"state"`
	BuildID      string `json:"build_id"`
	BID          string `json:"bid"`
	InvocationID string `json:"invocation_id"`
	WorkplanID   string `json:"workplan_id"`
	Category     string `json:"category"`
	URL          string `json:"url"`
	Summary      string `json:"summary"`
	Message      string `json:"message"`
	StartTime    string `json:"start_time"`
	EndTime      string `json:"end_time"`
}

func mapBusytownStatus(raw string) string {
	switch strings.ToUpper(strings.TrimSpace(raw)) {
	case "PASS", "PASSED", "SUCCESS", "SUCCEEDED", "COMPLETE", "COMPLETED", "VERIFIED":
		return "SUCCESS"
	case "FAIL", "FAILED", "FAILURE", "ERROR", "INFRA_FAILURE", "TIMED_OUT", "TIMEOUT", "ABORTED", "BROKEN":
		return "FAILURE"
	case "RUNNING", "IN_PROGRESS", "STARTED", "VERIFYING", "BUILDING", "TESTING":
		return "STARTED"
	case "PENDING", "QUEUED", "SCHEDULED", "WAITING", "NEW", "CREATED":
		return "SCHEDULED"
	case "SKIPPED", "CANCELED", "CANCELLED", "NOT_RUN":
		return "CANCELED"
	default:
		return "SCHEDULED"
	}
}

func parseTreetopTasksPayload(rawBytes []byte) ([]busytownTaskJSON, error) {
	trimmed := bytes.TrimSpace(rawBytes)
	trimmed = bytes.TrimPrefix(trimmed, []byte(")]}'"))
	trimmed = bytes.TrimSpace(trimmed)
	if len(trimmed) == 0 {
		return nil, nil
	}

	if trimmed[0] == '[' {
		inner := bytes.TrimSpace(trimmed[1:])
		if len(inner) > 0 && inner[0] == '[' {
			// Treetop JSPB (JavaScript ProtobufLite) positional array response ("[[...]]").
			// Extract any embedded ci.android.com build URLs if present; otherwise return
			// nil, nil so SearchBuilds cleanly falls back to Gerrit TreeHugger messages.
			urlMatches := treeHuggerAndroidCIURLRegex.FindAllStringSubmatch(string(trimmed), -1)
			if len(urlMatches) == 0 {
				return nil, nil
			}
			var tasks []busytownTaskJSON
			seen := make(map[string]bool)
			for _, m := range urlMatches {
				bid, target := m[1], m[2]
				key := bid + "/" + target
				if seen[key] {
					continue
				}
				seen[key] = true
				tasks = append(tasks, busytownTaskJSON{
					Name:    target,
					Target:  target,
					BuildID: bid,
					URL:     BusytownBuildURL(bid, target),
				})
			}
			return tasks, nil
		}
		var list []busytownTaskJSON
		if err := json.Unmarshal(trimmed, &list); err != nil {
			return nil, err
		}
		return list, nil
	}

	var wrapper struct {
		Tasks          []busytownTaskJSON `json:"tasks"`
		PresubmitTasks []busytownTaskJSON `json:"presubmit_tasks"`
		Results        []busytownTaskJSON `json:"results"`
	}
	if err := json.Unmarshal(trimmed, &wrapper); err != nil {
		return nil, err
	}
	if len(wrapper.Tasks) > 0 {
		return wrapper.Tasks, nil
	}
	if len(wrapper.PresubmitTasks) > 0 {
		return wrapper.PresubmitTasks, nil
	}
	return wrapper.Results, nil
}

func (p *BusytownProvider) SearchBuilds(ctx context.Context, host string, changeNum int, patchsetNum int, change *GerritChangeInfo) ([]bbBuild, error) {
	if p.GerritClient != nil && changeNum > 0 && patchsetNum > 0 {
		endpoint := fmt.Sprintf("changes/%d/revisions/%d/treetop~presubmittasks", changeNum, patchsetNum)
		req, err := p.GerritClient.NewRequest(ctx, "GET", endpoint, nil)
		if err == nil {
			var buf bytes.Buffer
			resp, doErr := p.GerritClient.Do(req, &buf)
			if doErr == nil && resp != nil && resp.StatusCode >= 200 && resp.StatusCode < 300 {
				if tasks, parseErr := parseTreetopTasksPayload(buf.Bytes()); parseErr == nil && len(tasks) > 0 {
					builds := convertTreetopTasksToBuilds(tasks)
					for i := range builds {
						builds[i].Patchset = patchsetNum
					}
					return builds, nil
				}
			}
		}
	}

	// Fallback: parse Gerrit change messages and Presubmit-Verified labels from TreeHugger.
	if change != nil && len(change.Messages) == 0 && p.GerritClient != nil && changeNum > 0 {
		if detailed, _, err := p.GerritClient.Changes.GetChangeDetail(ctx, fmt.Sprintf("%d", changeNum), &gerrit.ChangeOptions{
			AdditionalFields: []string{"MESSAGES", "DETAILED_LABELS", "CURRENT_REVISION"},
		}); err == nil && detailed != nil {
			change.Messages = detailed.Messages
			if len(change.Labels) == 0 {
				change.Labels = detailed.Labels
			}
			change = detailed
		}
	}

	builds := parseTreeHuggerMessagesFallback(change, patchsetNum)
	for i := range builds {
		builds[i].Patchset = patchsetNum
	}
	return builds, nil
}

func convertTreetopTasksToBuilds(tasks []busytownTaskJSON) []bbBuild {
	builds := make([]bbBuild, 0, len(tasks))
	for i, t := range tasks {
		target := strings.TrimSpace(t.Target)
		name := strings.TrimSpace(t.Name)
		if name == "" {
			name = target
		}
		if target == "" {
			target = name
		}
		if name == "" {
			name = fmt.Sprintf("presubmit-task-%d", i+1)
		}

		bid := strings.TrimSpace(t.BuildID)
		if bid == "" {
			bid = strings.TrimSpace(t.BID)
		}
		if bid == "" && t.WorkplanID != "" {
			bid = strings.TrimSpace(t.WorkplanID)
		}
		if bid == "" {
			bid = fmt.Sprintf("busytown-%d", i+1)
		}

		rawStatus := t.Status
		if rawStatus == "" {
			rawStatus = t.State
		}
		status := mapBusytownStatus(rawStatus)

		bucket := strings.TrimSpace(t.Category)
		if bucket == "" {
			bucket = "presubmit"
		}

		viewURL := strings.TrimSpace(t.URL)
		if viewURL == "" {
			viewURL = BusytownBuildURL(bid, target)
		}

		summary := strings.TrimSpace(t.Summary)
		if summary == "" {
			summary = strings.TrimSpace(t.Message)
		}

		var tags []bbTag
		if bid != "" {
			tags = append(tags, bbTag{Key: "build_id", Value: bid})
		}
		if target != "" {
			tags = append(tags, bbTag{Key: "target", Value: target})
		}
		if t.InvocationID != "" {
			tags = append(tags, bbTag{Key: "invocation_id", Value: t.InvocationID})
		}
		if t.WorkplanID != "" {
			tags = append(tags, bbTag{Key: "workplan_id", Value: t.WorkplanID})
		}

		builds = append(builds, bbBuild{
			ID:              bid,
			Provider:        "busytown",
			Status:          status,
			SummaryMarkdown: summary,
			ViewURL:         viewURL,
			Target:          target,
			InvocationID:    strings.TrimSpace(t.InvocationID),
			WorkplanID:      strings.TrimSpace(t.WorkplanID),
			StartTime:       t.StartTime,
			EndTime:         t.EndTime,
			Builder: bbBuilder{
				Project: "android-build",
				Bucket:  bucket,
				Builder: name,
			},
			Tags: tags,
		})
	}
	return builds
}

func messagePatchsetNum(msg *gerrit.ChangeMessageInfo) int {
	if msg == nil {
		return 0
	}
	if msg.RevisionNumber > 0 {
		return msg.RevisionNumber
	}
	if m := treeHuggerPatchsetRegex.FindStringSubmatch(strings.TrimSpace(msg.Message)); len(m) > 1 {
		var n int
		if _, err := fmt.Sscanf(m[1], "%d", &n); err == nil && n > 0 {
			return n
		}
	}
	return 0
}

func parseTreeHuggerMessagesFallback(change *GerritChangeInfo, patchsetNum int) []bbBuild {
	if change == nil {
		return nil
	}

	var startMsg *gerrit.ChangeMessageInfo
	var latestMsg *gerrit.ChangeMessageInfo
	var workplanID string
	for i := range change.Messages {
		msg := &change.Messages[i]
		msgPS := messagePatchsetNum(msg)
		if patchsetNum > 0 && msgPS != 0 && msgPS != patchsetNum {
			continue
		}
		isTH := strings.HasPrefix(msg.Tag, "autogenerated:TreeHugger") ||
			strings.Contains(msg.Message, "TreeHugger") ||
			strings.Contains(msg.Message, "ci.android.com/builds/")
		if !isTH {
			continue
		}
		if startMsg == nil || strings.Contains(strings.ToLower(msg.Tag), "verify-start") {
			startMsg = msg
		}
		latestMsg = msg
		if m := treeHuggerWorkplanRegex.FindStringSubmatch(msg.Message); len(m) > 1 {
			workplanID = m[1]
		}
	}

	if latestMsg == nil {
		return nil
	}

	status := "STARTED"
	tagLower := strings.ToLower(latestMsg.Tag)
	msgLower := strings.ToLower(latestMsg.Message)
	switch {
	case strings.Contains(tagLower, "verify-fail") || strings.Contains(msgLower, "presubmit-verified-1"):
		status = "FAILURE"
	case strings.Contains(tagLower, "verify-pass") || strings.Contains(msgLower, "presubmit-verified+1") || strings.Contains(msgLower, "presubmit-verified+2"):
		status = "SUCCESS"
	case strings.Contains(tagLower, "verify-start"):
		status = "STARTED"
	default:
		if lbl, ok := change.Labels["Presubmit-Verified"]; ok {
			for _, app := range lbl.All {
				if app.Value < 0 {
					status = "FAILURE"
					break
				} else if app.Value > 0 {
					status = "SUCCESS"
				}
			}
		}
	}

	startTime := formatGerritTimestamp(latestMsg.Date)
	if startMsg != nil && !startMsg.Date.Time.IsZero() {
		startTime = formatGerritTimestamp(startMsg.Date)
	}
	endTime := ""
	if status == "SUCCESS" || status == "FAILURE" || status == "CANCELED" {
		endTime = formatGerritTimestamp(latestMsg.Date)
	}

	urlMatches := treeHuggerAndroidCIURLRegex.FindAllStringSubmatch(latestMsg.Message, -1)
	if len(urlMatches) > 0 {
		var builds []bbBuild
		seen := make(map[string]bool)
		for _, m := range urlMatches {
			bid := m[1]
			target := m[2]
			key := bid + "/" + target
			if seen[key] {
				continue
			}
			seen[key] = true
			viewURL := BusytownBuildURL(bid, target)
			builds = append(builds, bbBuild{
				ID:              bid,
				Provider:        "busytown",
				Status:          status,
				SummaryMarkdown: strings.TrimSpace(latestMsg.Message),
				ViewURL:         viewURL,
				Target:          target,
				WorkplanID:      workplanID,
				StartTime:       startTime,
				EndTime:         endTime,
				Builder: bbBuilder{
					Project: "android-build",
					Bucket:  "presubmit",
					Builder: target,
				},
				Tags: []bbTag{
					{Key: "build_id", Value: bid},
					{Key: "target", Value: target},
					{Key: "workplan_id", Value: workplanID},
				},
			})
		}
		return builds
	}

	bid := workplanID
	if m := treeHuggerStandaloneBIDRegex.FindStringSubmatch(latestMsg.Message); len(m) > 1 {
		bid = m[1]
	}
	if bid == "" {
		bid = fmt.Sprintf("treehugger-ps%d", patchsetNum)
	}
	return []bbBuild{
		{
			ID:              bid,
			Provider:        "busytown",
			Status:          status,
			SummaryMarkdown: strings.TrimSpace(latestMsg.Message),
			ViewURL:         BusytownBuildURL(bid, ""),
			WorkplanID:      workplanID,
			StartTime:       startTime,
			EndTime:         endTime,
			Builder: bbBuilder{
				Project: "android-build",
				Bucket:  "presubmit",
				Builder: "TreeHugger Presubmit",
			},
			Tags: []bbTag{
				{Key: "workplan_id", Value: workplanID},
			},
		},
	}
}

func (p *BusytownProvider) resolveBuildAndTarget(buildID string, b *bbBuild, explicitTarget string) (bid, target, invocationID, workplanID string) {
	if parsedBID, parsedTarget, ok := ParseBusytownBuildTarget(buildID, explicitTarget); ok {
		bid = parsedBID
		target = parsedTarget
	} else {
		bid = strings.TrimSpace(buildID)
	}

	if b != nil {
		if bid == "" {
			bid = b.ID
		}
		if target == "" {
			target = b.Target
		}
		if target == "" {
			target = b.TagValue("target")
		}
		if target == "" && b.Builder.Builder != "" && b.Builder.Builder != "TreeHugger Presubmit" {
			target = b.Builder.Builder
		}
		invocationID = b.InvocationID
		if invocationID == "" {
			invocationID = b.TagValue("invocation_id")
		}
		workplanID = b.WorkplanID
		if workplanID == "" {
			workplanID = b.TagValue("workplan_id")
		}
	}
	if explicitTarget != "" && target == "" {
		target = explicitTarget
	}
	return bid, target, invocationID, workplanID
}

func (p *BusytownProvider) fetchBusytownLogs(ctx context.Context, bid, target, invocationID string) (buildLog string, antsOutput string, logURL string) {
	logURL = BusytownBuildURL(bid, target)

	// Tier A: Local corp workstation accelerators (`fetch_artifact` and `ants_cli`).
	if bid != "" && target != "" && BusytownFileExistsFn(FetchArtifactBinPath) {
		if out, err := BusytownExecCommandFn(ctx, FetchArtifactBinPath, "--bid", bid, "--target", target, "build.log", "-"); err == nil && len(out) > 0 {
			buildLog = string(out)
		}
	}
	if invocationID != "" && BusytownFileExistsFn(AntsCLIBinPath) {
		if out, err := BusytownExecCommandFn(ctx, AntsCLIBinPath, "test_results", "--invocation_id="+invocationID, "--status=fail"); err == nil && len(out) > 0 {
			antsOutput = strings.TrimSpace(string(out))
		}
	}

	// Tier B: Direct HTTP fallback via androidbuildinternal v3 REST API and ci.android.com.
	if buildLog == "" && bid != "" && target != "" {
		client := getBusytownHTTPClient(ctx)
		if client == nil {
			client = http.DefaultClient
		}
		apiBase := strings.TrimRight(BusytownAndroidBuildAPIBase, "/")
		urlEndpoint := fmt.Sprintf("%s/android/internal/build/v3/builds/%s/%s/attempts/latest/artifacts/build.log/url",
			apiBase, url.PathEscape(bid), url.PathEscape(target))

		if req, err := http.NewRequestWithContext(ctx, "GET", urlEndpoint, nil); err == nil {
			if resp, err := client.Do(req); err == nil && resp != nil {
				body, _ := io.ReadAll(resp.Body)
				resp.Body.Close()
				if resp.StatusCode >= 200 && resp.StatusCode < 300 && len(body) > 0 {
					var signed struct {
						SignedURL string `json:"signedUrl"`
						URL       string `json:"url"`
					}
					trimmed := bytes.TrimSpace(bytes.TrimPrefix(body, []byte(")]}'")))
					if json.Unmarshal(trimmed, &signed) == nil && (signed.SignedURL != "" || signed.URL != "") {
						dlURL := signed.SignedURL
						if dlURL == "" {
							dlURL = signed.URL
						}
						if dlReq, err := http.NewRequestWithContext(ctx, "GET", dlURL, nil); err == nil {
							if dlResp, err := client.Do(dlReq); err == nil && dlResp != nil {
								dlBytes, _ := io.ReadAll(dlResp.Body)
								dlResp.Body.Close()
								if dlResp.StatusCode >= 200 && dlResp.StatusCode < 300 {
									buildLog = string(dlBytes)
									logURL = dlURL
								}
							}
						}
					} else {
						buildLog = string(body)
						logURL = urlEndpoint
					}
				}
			}
		}

		if buildLog == "" {
			ciBase := strings.TrimRight(BusytownAndroidCIBase, "/")
			kind := "submitted"
			if strings.HasPrefix(strings.ToUpper(bid), "P") {
				kind = "pending"
			}
			rawEndpoint := fmt.Sprintf("%s/builds/%s/%s/%s/latest/raw/build.log",
				ciBase, kind, url.PathEscape(bid), url.PathEscape(target))
			if req, err := http.NewRequestWithContext(ctx, "GET", rawEndpoint, nil); err == nil {
				if resp, err := client.Do(req); err == nil && resp != nil {
					body, _ := io.ReadAll(resp.Body)
					resp.Body.Close()
					if resp.StatusCode >= 200 && resp.StatusCode < 300 && len(body) > 0 {
						buildLog = string(body)
						logURL = rawEndpoint
					}
				}
			}
		}
	}

	return buildLog, antsOutput, logURL
}

func isBusytownFailureMarker(line string) bool {
	l := strings.TrimSpace(line)
	return strings.HasPrefix(l, "FAILED:") ||
		strings.Contains(l, ": error:") ||
		strings.Contains(l, ": fatal error:") ||
		strings.HasPrefix(l, "ninja: build stopped")
}

func extractBusytownFailureSnippet(rawLog string, maxLines int) string {
	if maxLines <= 0 {
		maxLines = 40
	}
	rawLog = strings.TrimSpace(rawLog)
	if rawLog == "" {
		return ""
	}

	scanner := bufio.NewScanner(strings.NewReader(rawLog))
	scanner.Buffer(make([]byte, 0, 64*1024), 1024*1024)

	tail := make([]string, 0, maxLines)
	for scanner.Scan() {
		line := scanner.Text()
		if isBusytownFailureMarker(line) {
			snippet := make([]string, 0, maxLines)
			contextCount := 2
			if len(tail) < contextCount {
				contextCount = len(tail)
			}
			if contextCount > 0 {
				snippet = append(snippet, tail[len(tail)-contextCount:]...)
			}
			if len(snippet) < maxLines {
				snippet = append(snippet, line)
			}
			for len(snippet) < maxLines && scanner.Scan() {
				snippet = append(snippet, scanner.Text())
			}
			return strings.TrimSpace(strings.Join(snippet, "\n"))
		}
		if len(tail) == maxLines {
			copy(tail, tail[1:])
			tail[maxLines-1] = line
		} else {
			tail = append(tail, line)
		}
	}

	return strings.TrimSpace(strings.Join(tail, "\n"))
}

func (p *BusytownProvider) fetchBusytownFailureSnippet(ctx context.Context, bid, target, invocationID string, maxLines int) (snippet string, logURL string) {
	buildLog, antsOut, logURL := p.fetchBusytownLogs(ctx, bid, target, invocationID)
	snippet = extractBusytownFailureSnippet(buildLog, maxLines)
	if antsOut != "" {
		if snippet != "" {
			snippet = snippet + "\n\nAnTS Failed Tests:\n" + antsOut
		} else {
			snippet = antsOut
		}
	}
	return snippet, logURL
}

func (p *BusytownProvider) GetBuildDetails(ctx context.Context, buildID string, b *bbBuild) (*bbBuildDetails, error) {
	bid, target, invocationID, workplanID := p.resolveBuildAndTarget(buildID, b, "")
	builderName := target
	if builderName == "" && b != nil {
		builderName = b.Builder.Builder
	}
	if builderName == "" {
		builderName = bid
	}

	status := "FAILURE"
	summary := ""
	if b != nil {
		if b.Status != "" {
			status = b.Status
		}
		summary = b.SummaryMarkdown
	}

	viewURL := BusytownBuildURL(bid, target)
	if b != nil && b.ViewURL != "" {
		viewURL = b.ViewURL
	}

	details := &LUCIBuildDetails{
		ID:              bid,
		Provider:        "busytown",
		Status:          status,
		SummaryMarkdown: summary,
		ViewURL:         viewURL,
		Target:          target,
		InvocationID:    invocationID,
		WorkplanID:      workplanID,
		Builder: bbBuilder{
			Project: "android-build",
			Bucket:  "presubmit",
			Builder: builderName,
		},
	}

	if status == "FAILURE" || status == "INFRA_FAILURE" {
		snippet, logURL := p.fetchBusytownFailureSnippet(ctx, bid, target, invocationID, 40)
		step := LUCIStep{
			Name:            builderName,
			Status:          "FAILURE",
			SummaryMarkdown: snippet,
			Logs: []LUCILog{
				{
					Name:    "build.log",
					ViewURL: logURL,
				},
			},
		}
		details.Steps = append(details.Steps, step)
	} else {
		details.Steps = append(details.Steps, LUCIStep{
			Name:   builderName,
			Status: status,
		})
	}

	return details, nil
}

func (p *BusytownProvider) FetchFailureReportWithOptions(ctx context.Context, b bbBuild, opts FailureReportOptions) (*FailureReport, error) {
	bid, target, invocationID, _ := p.resolveBuildAndTarget(b.ID, &b, opts.Target)
	builderName := b.Builder.Builder
	if builderName == "" {
		builderName = target
	}
	if builderName == "" {
		builderName = bid
	}

	snippet, logURL := p.fetchBusytownFailureSnippet(ctx, bid, target, invocationID, opts.MaxLogLines)

	viewURL := b.ViewURL
	if viewURL == "" {
		viewURL = BusytownBuildURL(bid, target)
	}

	report := &FailureReport{
		BuildID:      bid,
		Builder:      builderName,
		Status:       "FAILURE",
		BuildURL:     viewURL,
		FailedStep:   builderName,
		BuildSummary: b.SummaryMarkdown,
		LogName:      "build.log",
		LogSnippet:   snippet,
		FullLogURL:   logURL,
	}
	if snippet == "" && b.SummaryMarkdown != "" {
		report.StepSummary = b.SummaryMarkdown
	}
	return report, nil
}

func (p *BusytownProvider) RerunBuilds(ctx context.Context, ch ChangeRef, builds []bbBuild, dryRun bool, out io.Writer) error {
	if out == nil {
		out = os.Stdout
	}
	endpoint := fmt.Sprintf("changes/%d/revisions/%d/treetop~runaction?service-id=presubmit&action-id=run", ch.ChangeID, ch.Patchset)
	if dryRun {
		fmt.Fprintf(out, "[dry-run] gerrit POST /%s (fallback: Presubmit-Ready+1)\n", endpoint)
		return nil
	}

	if p.GerritClient == nil {
		return fmt.Errorf("cannot rerun Busytown presubmit: Gerrit client is nil")
	}

	fmt.Fprintf(out, "Triggering Busytown / TreeHugger presubmit rerun on Change %d (Patchset %d)...\n", ch.ChangeID, ch.Patchset)
	req, err := p.GerritClient.NewRequest(ctx, "POST", endpoint, map[string]any{})
	if err == nil {
		resp, doErr := p.GerritClient.Do(req, nil)
		if doErr == nil && resp != nil && resp.StatusCode >= 200 && resp.StatusCode < 300 {
			return nil
		}
	}

	// Fallback: vote Presubmit-Ready+1 via SetReview
	reviewIn := &gerrit.ReviewInput{
		Labels: map[string]int{
			"Presubmit-Ready": 1,
		},
	}
	if err := SetReviewSafe(ctx, p.GerritClient, fmt.Sprintf("%d", ch.ChangeID), fmt.Sprintf("%d", ch.Patchset), reviewIn); err != nil {
		return fmt.Errorf("failed to trigger Busytown presubmit rerun via treetop~runaction or Presubmit-Ready+1 fallback: %w", err)
	}
	return nil
}
