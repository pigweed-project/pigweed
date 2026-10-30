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
	"fmt"
	"maps"
	"net/http"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"sync"
	"testing"
)

func TestQueryBuildbucket_Success(t *testing.T) {
	ctx := context.Background()

	server := NewMockGerritServer(t)
	server.OnSearchBuilds(
		FakeBuild("8680709829694997521", "static-checks-pigweed", "SUCCESS",
			WithBucket("pigweed.try"),
			WithTimes("2026-05-26T20:23:35.545752392Z", "2026-05-26T20:23:51.610863977Z"),
			WithSummary("All checks passed")),
		FakeBuild("8680709829694997522", "pigweed-linux", "FAILURE",
			WithBucket("pigweed.try"),
			WithTimes("2026-05-26T20:23:35.000000000Z", "2026-05-26T20:24:35.000000000Z"),
			WithSummary("Build compilation failed")),
	)

	builds, err := queryBuildbucket(ctx, server.URL, "pigweed-review.googlesource.com", "pigweed/pigweed", 413992, 1, server.Server.Client())
	if err != nil {
		t.Fatalf("queryBuildbucket failed: %v", err)
	}

	if len(builds) != 2 {
		t.Fatalf("Expected 2 builds, got %d", len(builds))
	}

	if builds[0].Builder.Builder != "static-checks-pigweed" || builds[0].Status != "SUCCESS" {
		t.Errorf("Unexpected build 0: %+v", builds[0])
	}
	if builds[1].Builder.Builder != "pigweed-linux" || builds[1].Status != "FAILURE" {
		t.Errorf("Unexpected build 1: %+v", builds[1])
	}

	lastReq := server.LastRequest()
	if lastReq == nil {
		t.Fatal("No request captured")
	}
	var capturedReq bbSearchBuildsRequest
	if err := json.Unmarshal(lastReq.Body, &capturedReq); err != nil {
		t.Fatalf("failed to unmarshal captured body: %v", err)
	}

	if len(capturedReq.Predicate.GerritChanges) != 1 {
		t.Fatalf("Expected 1 gerrit change predicate, got %d", len(capturedReq.Predicate.GerritChanges))
	}
	change := capturedReq.Predicate.GerritChanges[0]
	if change.Host != "pigweed-review.googlesource.com" || change.Project != "pigweed/pigweed" || change.Change != 413992 || change.Patchset != 1 {
		t.Errorf("Unexpected change predicate: %+v", change)
	}
}

func TestChecksIntegration(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Test Commit"), WithCurrentPatchset(2))
	server.OnSearchBuilds(
		FakeBuild("123456789", "pigweed-linux-clang", "SUCCESS",
			WithBucket("pigweed.try"),
			WithTimes("2026-05-26T20:00:00Z", "2026-05-26T20:01:30Z")),
	)

	output, err := executeCommand(RootCmd, "pr", "checks", "12345")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "Checks for Change 12345 (Patchset 2)") {
		t.Errorf("Expected output to contain header, got: %s", output)
	}
	if !strings.Contains(output, "pigweed-linux-clang") {
		t.Errorf("Expected output to contain builder name, got: %s", output)
	}
	if !strings.Contains(output, "https://ci.chromium.org/b/123456789") {
		t.Errorf("Expected output to contain CI link, got: %s", output)
	}
}

func TestChecksIntegration_NoSummaryBreakage(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Test Commit"), WithCurrentPatchset(2))
	server.OnSearchBuilds(
		FakeBuild("123456789", "docs-builder", "SUCCESS",
			WithBucket("pigweed.try"),
			WithTimes("2026-05-26T20:00:00Z", "2026-05-26T20:01:30Z"),
			WithSummary("Docs:\n* [permalink](https://example.com/docs)\n**Experiments**:\n* foo")),
	)

	output, err := executeCommand(RootCmd, "pr", "checks", "12345")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if strings.Contains(output, "Docs:\n* [permalink]") || strings.Contains(output, "**Experiments**") {
		t.Errorf("Checks table must not dump raw multiline markdown summaries, got:\n%s", output)
	}
	if !strings.Contains(output, "docs-builder") {
		t.Errorf("Expected docs-builder in output, got:\n%s", output)
	}
}

func TestChecksIntegration_JSON(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345)
	server.OnSearchBuilds(
		FakeBuild("123456789", "pigweed-linux-clang", "SUCCESS", WithBucket("pigweed.try")),
	)

	output, err := executeCommand(RootCmd, "pr", "checks", "12345", "--json", "number,checks")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	var data map[string]any
	if err := json.Unmarshal([]byte(output), &data); err != nil {
		t.Fatalf("Failed to parse JSON output: %v\nOutput: %s", err, output)
	}

	if num, ok := data["number"].(float64); !ok || int(num) != 12345 {
		t.Errorf("Unexpected number in JSON: %v", data["number"])
	}
}

func TestChecks_ErrorWhenInvalidPatchsetNumber(t *testing.T) {
	NewMockGerritServer(t)
	_, err := executeCommand(RootCmd, "pr", "checks", "12345/notanumber")
	if err == nil {
		t.Fatal("Expected error on invalid patchset number, got nil")
	}
	if !strings.Contains(err.Error(), "invalid patchset number") {
		t.Errorf("Expected error to mention invalid patchset number, got: %v", err)
	}
}

func TestChecks_ErrorWhenGerritReturns404(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnStatus(http.StatusNotFound)

	_, err := executeCommand(RootCmd, "pr", "checks", "99999")
	if err == nil {
		t.Fatal("Expected error when change is not found, got nil")
	}
}

func TestChecks_DefaultActivePR(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(472267, WithChangeID("Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e"), WithCurrentPatchset(13))
	server.OnSearchBuilds(
		FakeBuild("8680709829694997521", "static-checks-pigweed", "SUCCESS",
			WithBucket("pigweed.try"),
			WithTimes("2026-05-26T20:23:35.000Z", "2026-05-26T20:24:01.000Z")),
	)

	NewMockGit(t).WithBranch("ghish").WithCommit("commit msg\n\nChange-Id: Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e\n")

	out, err := executeCommand(RootCmd, "pr", "checks")
	if err != nil {
		t.Fatalf("pr checks without args failed: %v\nOutput: %s", err, out)
	}

	if !strings.Contains(out, "Checks for Change 472267 (Patchset 13)") || !strings.Contains(out, "static-checks-pigweed") {
		t.Errorf("Expected output to mention Change 472267 and static-checks-pigweed, got:\n%s", out)
	}
}

func TestChecks_ErrorContextWrapped(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/99999*", http.StatusNotFound, map[string]any{
		"error": "Change not found",
	})

	_, err := executeCommand(RootCmd, "pr", "checks", "99999")
	if err == nil {
		t.Fatal("Expected error for non-existent change")
	}
	if !strings.Contains(err.Error(), `failed to load checks for "99999"`) {
		t.Errorf("Expected wrapped error message mentioning 'failed to load checks for \"99999\"', got: %v", err)
	}
}

func TestChecks_DefaultFiltersOutExperimentalBuilders(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345)
	server.OnSearchBuilds(
		FakeBuild("1", "prod-linux", "SUCCESS", WithBucket("pigweed.try")),
		FakeBuild("2", "prod-mac", "STARTED", WithBucket("pigweed.try")),
		FakeBuild("3", "exp-v9", "FAILURE", WithBucket("pigweed.try"), WithCritical("NO")),
		FakeBuild("4", "exp-vscode", "FAILURE", WithBucket("pigweed.try"), WithCQExperimental(true)),
	)

	output, err := executeCommand(RootCmd, "pr", "checks", "12345")
	// prod-mac is still STARTED, so the gh exit-code contract requires 8
	// (pending). The two failing builders here are both experimental, and
	// experimental builders must never escalate this to 1 (failed).
	if got := ExitCodeFor(err); got != ExitCodePending {
		t.Fatalf("ExitCodeFor(%v) = %d, want %d (pending)\nOutput: %s", err, got, ExitCodePending, output)
	}

	if !strings.Contains(output, "prod-linux") {
		t.Errorf("Expected prod-linux in output, got:\n%s", output)
	}
	if !strings.Contains(output, "prod-mac") {
		t.Errorf("Expected prod-mac in output, got:\n%s", output)
	}
	if strings.Contains(output, "exp-v9") {
		t.Errorf("Expected exp-v9 to be filtered out, got:\n%s", output)
	}
	if strings.Contains(output, "exp-vscode") {
		t.Errorf("Expected exp-vscode to be filtered out, got:\n%s", output)
	}
	expectedNotice := "(2 non-blocking experimental builders omitted; add --experimental to see them)"
	if !strings.Contains(output, expectedNotice) {
		t.Errorf("Expected omitted notice %q in output, got:\n%s", expectedNotice, output)
	}
}

func TestChecks_ExperimentalFlagIncludesAll(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345)
	server.OnSearchBuilds(
		FakeBuild("1", "prod-linux", "SUCCESS", WithBucket("pigweed.try")),
		FakeBuild("2", "exp-v9", "FAILURE", WithBucket("pigweed.try"), WithCritical("NO")),
	)

	// Test with full flag --experimental
	output, err := executeCommand(RootCmd, "pr", "checks", "12345", "--experimental")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "prod-linux") {
		t.Errorf("Expected prod-linux in output, got:\n%s", output)
	}
	if !strings.Contains(output, "exp-v9") {
		t.Errorf("Expected exp-v9 with --experimental, got:\n%s", output)
	}
	if strings.Contains(output, "omitted") {
		t.Errorf("Expected no omitted notice with --experimental, got:\n%s", output)
	}

	// Test with shorthand -e
	outputShort, err := executeCommand(RootCmd, "pr", "checks", "12345", "-e")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, outputShort)
	}
	if !strings.Contains(outputShort, "exp-v9") {
		t.Errorf("Expected exp-v9 with -e, got:\n%s", outputShort)
	}
	if strings.Contains(outputShort, "omitted") {
		t.Errorf("Expected no omitted notice with -e, got:\n%s", outputShort)
	}
}

func TestChecks_SingularOmittedMessage(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345)
	server.OnSearchBuilds(
		FakeBuild("1", "prod-linux", "SUCCESS", WithBucket("pigweed.try")),
		FakeBuild("2", "exp-v9", "FAILURE", WithBucket("pigweed.try"), WithCritical("NO")),
	)

	output, err := executeCommand(RootCmd, "pr", "checks", "12345")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	expectedNotice := "(1 non-blocking experimental builder omitted; add --experimental to see them)"
	if !strings.Contains(output, expectedNotice) {
		t.Errorf("Expected singular omitted notice %q in output, got:\n%s", expectedNotice, output)
	}
}

func TestChecks_NoExperimentalOmittedWhenNone(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345)
	server.OnSearchBuilds(
		FakeBuild("1", "prod-linux", "SUCCESS", WithBucket("pigweed.try")),
	)

	output, err := executeCommand(RootCmd, "pr", "checks", "12345")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if strings.Contains(output, "omitted") {
		t.Errorf("Expected no omitted notice, got:\n%s", output)
	}
}

func TestChecks_JSON_FiltersUnlessExperimental(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345)
	server.OnSearchBuilds(
		FakeBuild("1", "prod-linux", "SUCCESS", WithBucket("pigweed.try")),
		FakeBuild("2", "exp-v9", "FAILURE", WithBucket("pigweed.try"), WithCritical("NO")),
	)

	// Default: only prod checks
	outDefault, err := executeCommand(RootCmd, "pr", "checks", "12345", "--json", "checks")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, outDefault)
	}
	var dataDefault map[string][]map[string]any
	if err := json.Unmarshal([]byte(outDefault), &dataDefault); err != nil {
		t.Fatalf("Failed to unmarshal JSON: %v\nOutput: %s", err, outDefault)
	}
	checksDefault := dataDefault["checks"]
	if len(checksDefault) != 1 || checksDefault[0]["name"] != "prod-linux" {
		t.Errorf("Expected 1 prod check in default JSON, got: %+v", checksDefault)
	}

	// With --experimental: both checks returned
	outExp, err := executeCommand(RootCmd, "pr", "checks", "12345", "--experimental", "--json", "checks")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, outExp)
	}
	var dataExp map[string][]map[string]any
	if err := json.Unmarshal([]byte(outExp), &dataExp); err != nil {
		t.Fatalf("Failed to unmarshal JSON: %v\nOutput: %s", err, outExp)
	}
	checksExp := dataExp["checks"]
	if len(checksExp) != 2 {
		t.Fatalf("Expected 2 checks in experimental JSON, got: %+v", checksExp)
	}
	if checksExp[0]["experimental"] != false || checksExp[1]["experimental"] != true {
		t.Errorf("Expected experimental field booleans, got: %+v", checksExp)
	}
}

func TestChecks_Watch_Success(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345)
	server.OnSearchBuildsSequence(
		[]bbBuild{FakeBuild("100", "pigweed-linux", "STARTED", WithTimes("2026-05-26T20:00:00Z", ""))},
		[]bbBuild{FakeBuild("100", "pigweed-linux", "SUCCESS", WithTimes("2026-05-26T20:00:00Z", ""))},
	)

	output, err := executeCommand(RootCmd, "pr", "checks", "12345", "--watch", "--interval", "5ms", "--buildbucket-host", server.URL)
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "pigweed-linux") {
		t.Errorf("Expected output to contain pigweed-linux, got:\n%s", output)
	}
	if !strings.Contains(output, "✓  pigweed-linux") {
		t.Errorf("Expected output to contain success symbol ✓, got:\n%s", output)
	}
	if got := server.CallCount("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds"); got < 2 {
		t.Errorf("Expected at least 2 polls, got %d", got)
	}
}

func TestChecks_Watch_FailFast_IgnoresExperimental(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345)
	server.OnSearchBuildsSequence(
		[]bbBuild{
			FakeBuild("100", "pigweed-linux", "STARTED"),
			FakeBuild("200", "exp-builder", "FAILURE", WithCritical("NO")),
		},
		[]bbBuild{
			FakeBuild("100", "pigweed-linux", "SUCCESS"),
			FakeBuild("200", "exp-builder", "FAILURE", WithCritical("NO")),
		},
	)

	output, err := executeCommand(RootCmd, "pr", "checks", "12345", "--watch", "--fail-fast", "--interval", "5ms", "--buildbucket-host", server.URL)
	if err != nil {
		t.Fatalf("Expected command to succeed because only experimental failed, got error: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "✓  pigweed-linux") {
		t.Errorf("Expected pigweed-linux success in output, got:\n%s", output)
	}
	if strings.Contains(output, "exp-builder") {
		t.Errorf("Expected exp-builder to be omitted from default view, got:\n%s", output)
	}
}

func TestChecks_Watch_LogFailed_Disabled(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345)
	server.OnSearchBuilds(FakeBuild("100", "pigweed-linux", "FAILURE"))

	output, err := executeCommand(RootCmd, "pr", "checks", "12345", "--watch", "--fail-fast", "--log-failed=false", "--interval", "5ms", "--buildbucket-host", server.URL)
	if err == nil {
		t.Fatalf("Expected error when check failed, got nil")
	}

	if !strings.Contains(output, "pigweed-linux") {
		t.Errorf("Expected output to contain check table, got:\n%s", output)
	}
	if strings.Contains(output, "FAILURE: pigweed-linux (Build 100)") {
		t.Errorf("Expected failure diagnostics to be omitted when --log-failed=false, got:\n%s", output)
	}
}

func TestChecks_Watch_AllCompleted_WithFailures(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345)
	server.OnSearchBuildsSequence(
		[]bbBuild{
			FakeBuild("100", "prod-1", "STARTED"),
			FakeBuild("200", "prod-2", "STARTED"),
		},
		[]bbBuild{
			FakeBuild("100", "prod-1", "SUCCESS"),
			FakeBuild("200", "prod-2", "FAILURE"),
		},
	)
	server.OnGetBuild(FakeBuildDetails("200", "prod-2", "FAILURE", WithBuildSummary("compiler error")))

	// Run without --fail-fast: watches until both finish
	output, err := executeCommand(RootCmd, "pr", "checks", "12345", "--watch", "--interval", "5ms", "--buildbucket-host", server.URL)
	if err == nil {
		t.Fatalf("Expected error when check failed, got nil. Output:\n%s", output)
	}

	if !strings.Contains(output, "✓  prod-1") {
		t.Errorf("Expected prod-1 to be listed as success, got:\n%s", output)
	}
	if !strings.Contains(output, "✗  prod-2") {
		t.Errorf("Expected prod-2 to be listed as failure, got:\n%s", output)
	}
	if !strings.Contains(output, "FAILURE: prod-2 (Build 200)") {
		t.Errorf("Expected failure report for prod-2, got:\n%s", output)
	}
}

func TestChecks_Watch_RerunSupersedesOldFailure(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345)
	server.OnSearchBuildsSequence(
		[]bbBuild{
			FakeBuild("200", "pigweed-linux", "STARTED"),
			FakeBuild("100", "pigweed-linux", "FAILURE"),
		},
		[]bbBuild{
			FakeBuild("200", "pigweed-linux", "SUCCESS"),
			FakeBuild("100", "pigweed-linux", "FAILURE"),
		},
	)

	output, err := executeCommand(RootCmd, "pr", "checks", "12345", "--watch", "--fail-fast", "--interval", "5ms", "--buildbucket-host", server.URL)
	if err != nil {
		t.Fatalf("Expected watch to succeed because retry succeeded, got error: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "✓  pigweed-linux") {
		t.Errorf("Expected latest build success in output, got:\n%s", output)
	}
	if strings.Contains(output, "✗  pigweed-linux") {
		t.Errorf("Expected old failure to be superseded by retry, got:\n%s", output)
	}
}

func TestChecks_Watch_CompletionBreadcrumbs(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345)
	server.OnSearchBuildsSequence(
		[]bbBuild{
			FakeBuild("1", "builder-one", "STARTED", WithTimes("2026-05-26T20:00:00Z", "2026-05-26T20:01:00Z")),
			FakeBuild("2", "builder-two", "STARTED", WithTimes("2026-05-26T20:00:00Z", "2026-05-26T20:02:00Z")),
		},
		[]bbBuild{
			FakeBuild("1", "builder-one", "SUCCESS", WithTimes("2026-05-26T20:00:00Z", "2026-05-26T20:01:00Z")),
			FakeBuild("2", "builder-two", "STARTED", WithTimes("2026-05-26T20:00:00Z", "2026-05-26T20:02:00Z")),
		},
		[]bbBuild{
			FakeBuild("1", "builder-one", "SUCCESS", WithTimes("2026-05-26T20:00:00Z", "2026-05-26T20:01:00Z")),
			FakeBuild("2", "builder-two", "SUCCESS", WithTimes("2026-05-26T20:00:00Z", "2026-05-26T20:02:00Z")),
		},
	)

	output, err := executeCommand(RootCmd, "pr", "checks", "12345", "--watch", "--interval", "5ms", "--buildbucket-host", server.URL)
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "Watching 2 checks for Change 12345 (Patchset 1) [0 passed, 2 running]...") {
		t.Errorf("Expected watch header, got:\n%s", output)
	}
	if !strings.Contains(output, "✓  builder-one passed (1m0s) [1/2]") {
		t.Errorf("Expected completion breadcrumb for builder-one, got:\n%s", output)
	}
	if !strings.Contains(output, "✓  builder-two passed (2m0s) [2/2]") {
		t.Errorf("Expected completion breadcrumb for builder-two, got:\n%s", output)
	}
}

func TestChecks_Watch_Heartbeat(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345)
	running := []bbBuild{FakeBuild("1", "long-builder", "STARTED", WithTimes("2026-05-26T20:00:00Z", ""))}
	passed := []bbBuild{FakeBuild("1", "long-builder", "SUCCESS", WithTimes("2026-05-26T20:00:00Z", ""))}
	server.OnSearchBuildsSequence(running, running, running, running, running, passed)

	output, err := executeCommand(RootCmd, "pr", "checks", "12345", "--watch", "--interval", "5ms", "--buildbucket-host", server.URL)
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "0 passed, 1 running, 0 failed (1 total)") {
		t.Errorf("Expected heartbeat status line in output, got:\n%s", output)
	}
}

func TestChecks_Watch_FailFast_BreadcrumbAndDiagnostics(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345)
	server.OnSearchBuildsSequence(
		[]bbBuild{
			FakeBuild("1", "failing-builder", "STARTED", WithTimes("2026-05-26T20:00:00Z", "2026-05-26T20:01:30Z")),
			FakeBuild("2", "running-builder", "STARTED", WithTimes("2026-05-26T20:00:00Z", "")),
		},
		[]bbBuild{
			FakeBuild("1", "failing-builder", "FAILURE", WithTimes("2026-05-26T20:00:00Z", "2026-05-26T20:01:30Z")),
			FakeBuild("2", "running-builder", "STARTED", WithTimes("2026-05-26T20:00:00Z", "")),
		},
	)
	server.OnGetBuild(FakeBuildDetails("1", "failing-builder", "FAILURE", WithBuildSummary("test failed: connection refused")))

	output, err := executeCommand(RootCmd, "pr", "checks", "12345", "--watch", "--fail-fast", "--interval", "5ms", "--buildbucket-host", server.URL)
	if err == nil {
		t.Fatalf("Expected error when fail-fast triggered, got nil. Output:\n%s", output)
	}
	if pollCount := server.CallCount("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds"); pollCount != 2 {
		t.Errorf("pollCount = %d, want 2 (--fail-fast should stop while running-builder is still STARTED)", pollCount)
	}

	if !strings.Contains(output, "✗  failing-builder failed (1m30s) [1/2]") {
		t.Errorf("Expected failure breadcrumb, got:\n%s", output)
	}
	if !strings.Contains(output, "--fail-fast triggered: stopping watch.") {
		t.Errorf("Expected fail-fast notification, got:\n%s", output)
	}
	if !strings.Contains(output, "FAILURE: failing-builder (Build 1)") {
		t.Errorf("Expected failure diagnostics, got:\n%s", output)
	}
	if !strings.Contains(output, "test failed: connection refused") {
		t.Errorf("Expected failure summary in output, got:\n%s", output)
	}
}

func TestChecks_Watch_InitialWaitingToScheduled(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345)
	server.OnSearchBuildsSequence(
		nil,
		[]bbBuild{FakeBuild("1", "builder-one", "STARTED")},
		[]bbBuild{FakeBuild("1", "builder-one", "SUCCESS")},
	)

	output, err := executeCommand(RootCmd, "pr", "checks", "12345", "--watch", "--interval", "5ms", "--buildbucket-host", server.URL)
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "Waiting for checks to be scheduled...") {
		t.Errorf("Expected 'Waiting for checks to be scheduled...', got:\n%s", output)
	}
	if !strings.Contains(output, "Watching 1 check for Change 12345 (Patchset 1) [0 passed, 1 running]...") {
		t.Errorf("Expected discovery header after waiting, got:\n%s", output)
	}
}

func TestChecks_Watch_InitialStatusBreakdown(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345)
	server.OnSearchBuildsSequence(
		[]bbBuild{
			FakeBuild("1", "builder-one", "SUCCESS", WithTimes("2026-09-08T10:00:00Z", "2026-09-08T10:01:00Z")),
			FakeBuild("2", "builder-two", "STARTED", WithTimes("2026-09-08T10:00:00Z", "")),
			FakeBuild("3", "builder-three", "STARTED", WithTimes("2026-09-08T10:00:00Z", "")),
		},
		[]bbBuild{
			FakeBuild("1", "builder-one", "SUCCESS", WithTimes("2026-09-08T10:00:00Z", "2026-09-08T10:01:00Z")),
			FakeBuild("2", "builder-two", "SUCCESS", WithTimes("2026-09-08T10:00:00Z", "2026-09-08T10:02:00Z")),
			FakeBuild("3", "builder-three", "SUCCESS", WithTimes("2026-09-08T10:00:00Z", "2026-09-08T10:03:00Z")),
		},
	)

	output, err := executeCommand(RootCmd, "pr", "checks", "12345", "--watch", "--interval", "5ms", "--buildbucket-host", server.URL)
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	// Verify the initial status breakdown reported that 1 was already passed and 2 running
	if !strings.Contains(output, "Watching 3 checks for Change 12345 (Patchset 1) [1 passed, 2 running]...") {
		t.Errorf("Expected initial status breakdown [1 passed, 2 running], got:\n%s", output)
	}

	// Verify subsequent breadcrumbs count correctly towards total (completed was initialized with the 1 passed check)
	if !strings.Contains(output, "✓  builder-two passed (2m0s) [2/3]") {
		t.Errorf("Expected completion breadcrumb for builder-two [2/3], got:\n%s", output)
	}
	if !strings.Contains(output, "✓  builder-three passed (3m0s) [3/3]") {
		t.Errorf("Expected completion breadcrumb for builder-three [3/3], got:\n%s", output)
	}
}

func TestChecks_Watch_InitialStatusBreakdown_WithFailure(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345)
	server.OnSearchBuilds(
		FakeBuild("1", "builder-fail", "FAILURE", WithTimes("2026-09-08T10:00:00Z", "2026-09-08T10:01:00Z")),
		FakeBuild("2", "builder-pass", "SUCCESS", WithTimes("2026-09-08T10:00:00Z", "2026-09-08T10:02:00Z")),
	)

	output, err := executeCommand(RootCmd, "pr", "checks", "12345", "--watch", "--interval", "5ms", "--buildbucket-host", server.URL)
	if err == nil {
		t.Fatalf("Expected command to fail due to check failure, got success:\n%s", output)
	}

	// Verify the initial status breakdown reported failure
	if !strings.Contains(output, "Watching 2 checks for Change 12345 (Patchset 1) [1 passed, 0 running, 1 failed]...") {
		t.Errorf("Expected initial status breakdown [1 passed, 0 running, 1 failed], got:\n%s", output)
	}
}

func TestChecks_Web(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/files*", http.StatusOK, map[string]any{})
	server.OnDefaultChange(12345, WithCurrentPatchset(2))
	server.OnSearchBuilds()

	var openedURL string
	origOpen := OpenBrowserFn
	defer func() { OpenBrowserFn = origOpen }()
	OpenBrowserFn = func(url string) error {
		openedURL = url
		return nil
	}

	out, err := executeCommand(RootCmd, "pr", "checks", "12345", "--web", "--buildbucket-host", server.URL)
	if err != nil {
		t.Fatalf("pr checks --web failed: %v\nOutput: %s", err, out)
	}
	if !strings.Contains(out, "Opening https://") {
		t.Errorf("Expected opening message, got:\n%s", out)
	}
	if !strings.Contains(openedURL, "/c/pigweed/pigweed/+/12345/2") {
		t.Errorf("Expected opened URL to contain /c/pigweed/pigweed/+/12345/2, got %q", openedURL)
	}
}

// ---------------------------------------------------------------------------
// Exit code contract
//
// `gh pr checks` is used as a merge gate: `gh pr checks && gh pr merge --cq`.
// If it exits 0 while checks are red or still running, that command line
// merges broken code. These tests pin the contract:
//
//	0 - every blocking check passed
//	8 - nothing failed, but something is still running
//	1 - something failed, or nothing blocking was reported at all
// ---------------------------------------------------------------------------

func TestClassifyCheckStatus(t *testing.T) {
	tests := []struct {
		status string
		want   checkOutcome
	}{
		{"SUCCESS", checkPassed},
		{"SCHEDULED", checkPending},
		{"STARTED", checkPending},
		{"FAILURE", checkFailed},
		{"INFRA_FAILURE", checkFailed},
		// Buildbucket reports cancelled builds as CANCELED. A cancelled check
		// did not pass, so it must not let the gate report success -- but it
		// also did not fail, and reporting it as a failure makes the tool lie
		// about the state of the change. The overwhelmingly common cause is a
		// newer patchset superseding the run.
		{"CANCELED", checkCanceled},
		// Fail closed on anything unrecognized, including a status
		// Buildbucket might add later, and on an empty status from a
		// malformed response.
		{"SOME_FUTURE_STATUS", checkFailed},
		{"", checkFailed},
	}

	for _, tt := range tests {
		t.Run(tt.status, func(t *testing.T) {
			if got := classifyCheckStatus(tt.status); got != tt.want {
				t.Errorf("classifyCheckStatus(%q) = %v, want %v", tt.status, got, tt.want)
			}
		})
	}
}

// TestBuilderList pins the truncation of long builder lists. CL 472267/43 has
// 29 canceled builders, and naming every one of them produced a single
// unreadable 900-character line.
func TestBuilderList(t *testing.T) {
	build := func(names ...string) []bbBuild {
		out := make([]bbBuild, 0, len(names))
		for _, n := range names {
			out = append(out, exitStatusBuild(n, "FAILURE"))
		}
		return out
	}

	tests := []struct {
		name   string
		builds []bbBuild
		want   string
	}{
		{
			name:   "single builder",
			builds: build("alpha"),
			want:   "alpha",
		},
		{
			name:   "sorted regardless of input order",
			builds: build("zeta", "alpha", "mu"),
			want:   "alpha, mu, zeta",
		},
		{
			name:   "exactly at the cap is not truncated",
			builds: build("a", "b", "c", "d", "e"),
			want:   "a, b, c, d, e",
		},
		{
			name:   "over the cap is truncated with a count",
			builds: build("a", "b", "c", "d", "e", "f"),
			want:   "a, b, c, d, e and 1 more",
		},
		{
			name:   "the real case",
			builds: build("i", "h", "g", "f", "e", "d", "c", "b", "a"),
			want:   "a, b, c, d, e and 4 more",
		},
		{
			name: "builds from an earlier patchset are annotated",
			builds: func() []bbBuild {
				bs := build("beta", "alpha", "gamma")
				bs[0].Patchset = 8
				bs[1].Patchset = 10
				return bs
			}(),
			want: "alpha, beta (from patchset 8), gamma",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := builderList(tt.builds, 10); got != tt.want {
				t.Errorf("builderList() = %q, want %q", got, tt.want)
			}
		})
	}
}

// build is a terse constructor for exit-status test fixtures.
func exitStatusBuild(name, status string) bbBuild {
	var b bbBuild
	b.Builder.Builder = name
	b.Status = status
	return b
}

func TestChecksExitStatus(t *testing.T) {
	tests := []struct {
		name         string
		tally        checkTally
		wantCode     int
		wantContains []string
		wantOmits    []string
	}{
		{
			name:     "all blocking checks passed",
			tally:    checkTally{blocking: 3},
			wantCode: ExitCodeOK,
		},
		{
			name: "single failure names the builder and points at the logs",
			tally: checkTally{
				blocking: 2,
				failed:   []bbBuild{exitStatusBuild("pigweed-linux", "FAILURE")},
			},
			wantCode: ExitCodeFailure,
			wantContains: []string{
				`check "pigweed-linux" failed on Change 12345 (Patchset 2)`,
				"gh run view 12345 --log-failed",
				"gh run rerun 12345 --failed",
			},
		},
		{
			name: "multiple failures are listed in sorted order",
			tally: checkTally{
				blocking: 5,
				failed: []bbBuild{
					exitStatusBuild("zeta", "FAILURE"),
					exitStatusBuild("alpha", "INFRA_FAILURE"),
				},
			},
			wantCode: ExitCodeFailure,
			wantContains: []string{
				"2 of 5 checks failed on Change 12345 (Patchset 2): alpha, zeta",
			},
		},
		{
			name: "pending checks map to exit 8",
			tally: checkTally{
				blocking: 4,
				pending: []bbBuild{
					exitStatusBuild("pigweed-mac", "STARTED"),
					exitStatusBuild("pigweed-win", "SCHEDULED"),
				},
			},
			wantCode: ExitCodePending,
			wantContains: []string{
				"2 of 4 checks are still running on Change 12345 (Patchset 2): pigweed-mac, pigweed-win",
				"gh pr checks 12345 --watch",
			},
		},
		{
			// A red check is actionable now; a pending one is not. Reporting
			// pending here would tell a polling agent to keep waiting for a
			// result that is already decided.
			name: "failure takes precedence over pending",
			tally: checkTally{
				blocking: 3,
				failed:   []bbBuild{exitStatusBuild("pigweed-linux", "FAILURE")},
				pending:  []bbBuild{exitStatusBuild("pigweed-mac", "STARTED")},
			},
			wantCode: ExitCodeFailure,
		},
		{
			// Regression test for a real bug: CL 472267/43 has 46 SUCCESS and
			// 29 CANCELED builds and zero failures, and the tool reported
			// "29 of 75 checks failed". Canceled is not failed, and saying so
			// sends the reader hunting for a broken build that does not exist.
			name: "canceled checks are reported as canceled, never as failed",
			tally: checkTally{
				blocking: 75,
				canceled: []bbBuild{
					exitStatusBuild("pigweed-mac-arm-gn-stm", "CANCELED"),
					exitStatusBuild("pigweed-linux-gn-san", "CANCELED"),
				},
			},
			wantCode: ExitCodeFailure,
			wantContains: []string{
				"2 of 75 checks were canceled on Change 12345 (Patchset 2): pigweed-linux-gn-san, pigweed-mac-arm-gn-stm",
				"superseded",
			},
			wantOmits: []string{"failed", "failure"},
		},
		{
			name: "a single canceled check is phrased in the singular",
			tally: checkTally{
				blocking: 3,
				canceled: []bbBuild{exitStatusBuild("pigweed-linux", "CANCELED")},
			},
			wantCode: ExitCodeFailure,
			wantContains: []string{
				`check "pigweed-linux" was canceled on Change 12345 (Patchset 2)`,
			},
			wantOmits: []string{"failed"},
		},
		{
			// An actual failure is the more actionable signal, so it wins.
			name: "failure takes precedence over canceled",
			tally: checkTally{
				blocking: 4,
				failed:   []bbBuild{exitStatusBuild("pigweed-linux", "FAILURE")},
				canceled: []bbBuild{exitStatusBuild("pigweed-mac", "CANCELED")},
			},
			wantCode: ExitCodeFailure,
			wantContains: []string{
				`check "pigweed-linux" failed`,
			},
		},
		{
			// Waiting cannot resolve a canceled build, so canceled must not be
			// reported as pending: exit 8 would send `--watch` into a loop that
			// can never reach a verdict.
			name: "canceled takes precedence over pending",
			tally: checkTally{
				blocking: 4,
				canceled: []bbBuild{exitStatusBuild("pigweed-mac", "CANCELED")},
				pending:  []bbBuild{exitStatusBuild("pigweed-win", "STARTED")},
			},
			wantCode: ExitCodeFailure,
			wantContains: []string{
				"was canceled",
			},
		},
		{
			name:     "no checks at all is a failure, not a pass",
			tally:    checkTally{blocking: 0},
			wantCode: ExitCodeFailure,
			wantContains: []string{
				"no checks reported on Change 12345 (Patchset 2)",
				"gh pr edit 12345 --cq",
			},
		},
		{
			name:     "only experimental builders ran is also a failure",
			tally:    checkTally{blocking: 0, experimental: 2},
			wantCode: ExitCodeFailure,
			wantContains: []string{
				"no blocking checks reported on Change 12345 (Patchset 2)",
				"2 non-blocking experimental builder(s) ran",
				"gh pr checks 12345 --experimental",
			},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := checksExitStatus(12345, 2, tt.tally)

			if got := ExitCodeFor(err); got != tt.wantCode {
				t.Fatalf("exit code = %d, want %d (err = %v)", got, tt.wantCode, err)
			}
			if tt.wantCode == ExitCodeOK {
				if err != nil {
					t.Fatalf("expected nil error for a passing state, got %v", err)
				}
				return
			}
			if err == nil {
				t.Fatal("expected a non-nil error")
			}
			for _, want := range tt.wantContains {
				if !strings.Contains(err.Error(), want) {
					t.Errorf("error message missing %q.\nGot:\n%s", want, err.Error())
				}
			}
			for _, omit := range tt.wantOmits {
				if strings.Contains(strings.ToLower(err.Error()), omit) {
					t.Errorf("error message must not contain %q.\nGot:\n%s", omit, err.Error())
				}
			}
		})
	}
}

// exitCodeChecksServer builds a mock Gerrit + Buildbucket server whose
// SearchBuilds response contains one builder per entry in statuses.
func exitCodeChecksServer(t *testing.T, statuses map[string]string, experimental map[string]bool, changeOpts ...func(map[string]any)) *MockGerritServer {
	t.Helper()

	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, changeOpts...)

	builds := make([]bbBuild, 0, len(statuses))
	for i, name := range slices.Sorted(maps.Keys(statuses)) {
		var opts []func(*bbBuild)
		if experimental[name] {
			opts = append(opts, WithCritical("NO"))
		}
		builds = append(builds, FakeBuild(fmt.Sprintf("%d", i+1), name, statuses[name], opts...))
	}

	server.OnSearchBuilds(builds...)
	return server
}

// TestChecks_ExitCodeContract drives the real command end to end. It is the
// regression test for `gh pr checks` returning 0 with failing CI unless
// --watch was passed.
func TestChecks_ExitCodeContract(t *testing.T) {
	tests := []struct {
		name         string
		statuses     map[string]string
		experimental map[string]bool
		extraArgs    []string
		changeOpts   []func(map[string]any)
		wantCode     int
		wantMessage  string
		wantNotInMsg string
		wantOutput   string
	}{
		{
			name:     "all passed",
			statuses: map[string]string{"a": "SUCCESS", "b": "SUCCESS"},
			wantCode: ExitCodeOK,
		},
		{
			// The regression: this previously exited 0 without --watch.
			name:        "one failure without --watch",
			statuses:    map[string]string{"a": "SUCCESS", "b": "FAILURE"},
			wantCode:    ExitCodeFailure,
			wantMessage: `check "b" failed`,
		},
		{
			name:     "infra failure without --watch",
			statuses: map[string]string{"a": "INFRA_FAILURE"},
			wantCode: ExitCodeFailure,
		},
		{
			// Reproduces CL 472267/43: lots of passes, some canceled builds,
			// nothing actually broken. This must not claim anything failed.
			name: "canceled build is not a pass, but is not a failure either",
			statuses: map[string]string{
				"a": "SUCCESS", "b": "SUCCESS", "c": "CANCELED", "d": "CANCELED",
			},
			wantCode:     ExitCodeFailure,
			wantMessage:  "2 of 4 checks were canceled",
			wantNotInMsg: "failed",
		},
		{
			name:     "unknown status fails closed",
			statuses: map[string]string{"a": "WHO_KNOWS"},
			wantCode: ExitCodeFailure,
		},
		{
			name:     "still running",
			statuses: map[string]string{"a": "SUCCESS", "b": "STARTED"},
			wantCode: ExitCodePending,
		},
		{
			name:     "scheduled but not started",
			statuses: map[string]string{"a": "SCHEDULED"},
			wantCode: ExitCodePending,
		},
		{
			name:     "failure wins over pending",
			statuses: map[string]string{"a": "FAILURE", "b": "STARTED"},
			wantCode: ExitCodeFailure,
		},
		{
			name:     "no checks reported",
			statuses: map[string]string{},
			wantCode: ExitCodeFailure,
		},
		{
			// --experimental is a display flag. A failing non-blocking
			// builder must not change the gate's answer, with or without it.
			name:         "failing experimental builder is hidden and does not gate",
			statuses:     map[string]string{"a": "SUCCESS", "exp": "FAILURE"},
			experimental: map[string]bool{"exp": true},
			wantCode:     ExitCodeOK,
		},
		{
			name:         "failing experimental builder does not gate even when shown",
			statuses:     map[string]string{"a": "SUCCESS", "exp": "FAILURE"},
			experimental: map[string]bool{"exp": true},
			extraArgs:    []string{"--experimental"},
			wantCode:     ExitCodeOK,
		},
		{
			name:         "only experimental builders means nothing blocking ran",
			statuses:     map[string]string{"exp": "SUCCESS"},
			experimental: map[string]bool{"exp": true},
			wantCode:     ExitCodeFailure,
		},
		{
			// The exit code covers CI only, matching gh, but the output must
			// not let a passing run read as "ready to submit".
			name:     "unsatisfied Code-Review is printed but does not gate",
			statuses: map[string]string{"a": "SUCCESS"},
			changeOpts: []func(map[string]any){func(ch map[string]any) {
				ch["submit_requirements"] = []map[string]any{{"name": "Code-Review", "status": "UNSATISFIED"}}
			}},
			wantCode:   ExitCodeOK,
			wantOutput: "Not yet submittable: Code-Review (+2 required)",
		},
		{
			name:     "satisfied submit requirements print no blockers",
			statuses: map[string]string{"a": "SUCCESS"},
			changeOpts: []func(map[string]any){func(ch map[string]any) {
				ch["submit_requirements"] = []map[string]any{{"name": "Code-Review", "status": "SATISFIED"}}
			}},
			wantCode: ExitCodeOK,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			server := exitCodeChecksServer(t, tt.statuses, tt.experimental, tt.changeOpts...)

			args := append([]string{"pr", "checks", "12345", "--buildbucket-host", server.URL}, tt.extraArgs...)
			output, err := executeCommand(RootCmd, args...)

			if got := ExitCodeFor(err); got != tt.wantCode {
				t.Fatalf("exit code = %d, want %d\nerr = %v\nOutput:\n%s", got, tt.wantCode, err, output)
			}
			if tt.wantOutput == "" && strings.Contains(output, "Not yet submittable") {
				t.Errorf("output must not contain a blockers line, got:\n%s", output)
			}
			if !strings.Contains(output, tt.wantOutput) {
				t.Errorf("output missing %q, got:\n%s", tt.wantOutput, output)
			}
			if tt.wantMessage != "" {
				if err == nil || !strings.Contains(err.Error(), tt.wantMessage) {
					t.Errorf("error message missing %q, got: %v", tt.wantMessage, err)
				}
			}
			if tt.wantNotInMsg != "" && err != nil {
				if strings.Contains(strings.ToLower(err.Error()), tt.wantNotInMsg) {
					t.Errorf("error message must not contain %q, got: %v", tt.wantNotInMsg, err)
				}
			}
		})
	}
}

// TestChecks_ExitCodeContractIsIndependentOfOutputFormat guards against the
// exit code being tied to the human-readable rendering path. A script using
// --json to parse results relies on the exit code just as much as an
// interactive user does.
func TestChecks_ExitCodeContractIsIndependentOfOutputFormat(t *testing.T) {
	formats := []struct {
		name string
		args []string
	}{
		{"default", nil},
		{"json", []string{"--json", "number,checks,gerritGates"}},
		{"template", []string{"--template", "{{.number}}"}},
	}

	for _, f := range formats {
		t.Run(f.name+"/failing", func(t *testing.T) {
			server := exitCodeChecksServer(t, map[string]string{"a": "FAILURE"}, nil)
			args := append([]string{"pr", "checks", "12345", "--buildbucket-host", server.URL}, f.args...)
			output, err := executeCommand(RootCmd, args...)
			if got := ExitCodeFor(err); got != ExitCodeFailure {
				t.Errorf("exit code = %d, want %d\nOutput:\n%s", got, ExitCodeFailure, output)
			}
		})

		t.Run(f.name+"/pending", func(t *testing.T) {
			server := exitCodeChecksServer(t, map[string]string{"a": "STARTED"}, nil)
			args := append([]string{"pr", "checks", "12345", "--buildbucket-host", server.URL}, f.args...)
			output, err := executeCommand(RootCmd, args...)
			if got := ExitCodeFor(err); got != ExitCodePending {
				t.Errorf("exit code = %d, want %d\nOutput:\n%s", got, ExitCodePending, output)
			}
		})

		t.Run(f.name+"/passing", func(t *testing.T) {
			server := exitCodeChecksServer(t, map[string]string{"a": "SUCCESS"}, nil)
			args := append([]string{"pr", "checks", "12345", "--buildbucket-host", server.URL}, f.args...)
			output, err := executeCommand(RootCmd, args...)
			if err != nil {
				t.Errorf("expected success, got %v\nOutput:\n%s", err, output)
			}
		})
	}
}

// TestChecks_FailureStillRendersTheTable ensures the non-zero exit does not
// suppress the check table. The user needs to see which builder failed, and
// the error message is written to stderr while the table goes to stdout.
func TestChecks_FailureStillRendersTheTable(t *testing.T) {
	server := exitCodeChecksServer(t, map[string]string{"pigweed-linux": "FAILURE"}, nil)

	output, err := executeCommand(RootCmd, "pr", "checks", "12345", "--buildbucket-host", server.URL)
	if got := ExitCodeFor(err); got != ExitCodeFailure {
		t.Fatalf("exit code = %d, want %d", got, ExitCodeFailure)
	}
	if !strings.Contains(output, "Checks for Change 12345") {
		t.Errorf("expected the check table to still be rendered, got:\n%s", output)
	}
	if !strings.Contains(output, "pigweed-linux") {
		t.Errorf("expected the failing builder in the table, got:\n%s", output)
	}
}

// TestRunWatch_InheritsExitCodeContract covers `gh run watch`, which delegates
// to the checks command and must therefore report the same codes.
func TestRunWatch_InheritsExitCodeContract(t *testing.T) {
	t.Run("failure", func(t *testing.T) {
		server := exitCodeChecksServer(t, map[string]string{"a": "FAILURE"}, nil)
		output, err := executeCommand(RootCmd, "run", "watch", "12345", "--buildbucket-host", server.URL)
		if got := ExitCodeFor(err); got != ExitCodeFailure {
			t.Errorf("exit code = %d, want %d\nOutput:\n%s", got, ExitCodeFailure, output)
		}
	})

	t.Run("success", func(t *testing.T) {
		server := exitCodeChecksServer(t, map[string]string{"a": "SUCCESS"}, nil)
		output, err := executeCommand(RootCmd, "run", "watch", "12345", "--buildbucket-host", server.URL)
		if err != nil {
			t.Errorf("expected success, got %v\nOutput:\n%s", err, output)
		}
	})
}

func TestGetLUCIHTTPClient(t *testing.T) {
	ctx := context.Background()

	// Default client uses LUCIAuthTransport:
	client := getLUCIHTTPClient(ctx, "cr-buildbucket.appspot.com")
	if client == nil {
		t.Fatal("expected non-nil client")
	}
	if _, ok := client.Transport.(*LUCIAuthTransport); !ok {
		t.Errorf("expected *LUCIAuthTransport, got %T", client.Transport)
	}

	// Customizable hook:
	orig := getLUCIHTTPClient
	defer func() { getLUCIHTTPClient = orig }()

	customClient := &http.Client{}
	getLUCIHTTPClient = func(ctx context.Context, host string) *http.Client {
		return customClient
	}

	got := getLUCIHTTPClient(ctx, "cr-buildbucket.appspot.com")
	if got != customClient {
		t.Errorf("got %v, want %v", got, customClient)
	}
}

func TestBuildCheckItems(t *testing.T) {
	builds := []bbBuild{
		FakeBuild("101", "pigweed-linux", "SUCCESS", WithSummary("Passed all tests")),
		FakeBuild("102", "pigweed-exp", "FAILURE", WithExperiments("luci.non_production")),
	}
	items := BuildCheckItems(builds)
	if len(items) != 2 {
		t.Fatalf("expected 2 CheckItems, got %d", len(items))
	}
	if items[0].ID != "101" || items[0].Name != "pigweed-linux" || items[0].StatusSymbol != "✓" || items[0].Experimental {
		t.Errorf("unexpected items[0]: %+v", items[0])
	}
	if items[1].ID != "102" || items[1].Name != "pigweed-exp" || items[1].StatusSymbol != "✗" || !items[1].Experimental {
		t.Errorf("unexpected items[1]: %+v", items[1])
	}
}

func TestFilterExperimentalBuilds(t *testing.T) {
	builds := []bbBuild{
		FakeBuild("1", "blocking-1", "SUCCESS"),
		FakeBuild("2", "exp-1", "FAILURE", WithExperiments("luci.non_production")),
		FakeBuild("3", "exp-2", "SUCCESS", WithCritical("NO")),
	}

	relNoExp, omittedNoExp := FilterExperimentalBuilds(builds, false)
	if len(relNoExp) != 1 || omittedNoExp != 2 {
		t.Errorf("FilterExperimentalBuilds(false) = (%d builds, %d omitted), want (1, 2)", len(relNoExp), omittedNoExp)
	}

	relWithExp, omittedWithExp := FilterExperimentalBuilds(builds, true)
	if len(relWithExp) != 3 || omittedWithExp != 0 {
		t.Errorf("FilterExperimentalBuilds(true) = (%d builds, %d omitted), want (3, 0)", len(relWithExp), omittedWithExp)
	}
}

func TestFormatOmittedExperimentalNotice(t *testing.T) {
	if got := FormatOmittedExperimentalNotice(0); got != "" {
		t.Errorf("expected empty string for 0, got %q", got)
	}
	if got := FormatOmittedExperimentalNotice(1); !strings.Contains(got, "1 non-blocking experimental builder omitted") {
		t.Errorf("unexpected singular notice: %q", got)
	}
	if got := FormatOmittedExperimentalNotice(3); !strings.Contains(got, "3 non-blocking experimental builders omitted") {
		t.Errorf("unexpected plural notice: %q", got)
	}
}

func TestChecks_EquivalentPatchsets(t *testing.T) {
	// Patchset 10 is TRIVIAL_REBASE (no builds yet),
	// Patchset 9 is NO_CODE_CHANGE (reran static-checks-pigweed and passed),
	// Patchset 8 is REWORK (ran pigweed-linux=SUCCESS and static-checks-pigweed=FAILURE),
	// Patchset 7 is REWORK (had a failing build that must NOT be included).
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345,
		WithSubject("pw_ghish: Test equivalent patchsets"),
		WithPatchsetKind(7, "REWORK"),
		WithPatchsetKind(8, "REWORK"),
		WithPatchsetKind(9, "NO_CODE_CHANGE"),
		WithPatchsetKind(10, "TRIVIAL_REBASE"),
	)
	server.OnSearchBuildsByPatchset(map[int][]bbBuild{
		10: nil,
		9: {
			FakeBuild("901", "static-checks-pigweed", "SUCCESS"),
		},
		8: {
			FakeBuild("801", "pigweed-linux", "SUCCESS"),
			FakeBuild("802", "static-checks-pigweed", "FAILURE"),
		},
	})

	out, err := executeCommand(RootCmd, "pr", "checks", "12345")
	if err != nil {
		t.Fatalf("expected pr checks 12345 to pass across equivalent patchsets [10, 9, 8], got error: %v\nOutput:\n%s", err, out)
	}

	var queried []int
	for _, r := range server.Requests() {
		if r.Path == "/prpc/buildbucket.v2.Builds/SearchBuilds" {
			req := DecodeRequest[bbSearchBuildsRequest](t, &r)
			queried = append(queried, req.Predicate.GerritChanges[0].Patchset)
		}
	}
	slices.SortFunc(queried, func(a, b int) int { return cmp.Compare(b, a) })
	if want := "[10 9 8]"; fmt.Sprint(queried) != want {
		t.Errorf("queried patchsets = %v, want %s", queried, want)
	}

	if !strings.Contains(out, "Checks for Change 12345 (Patchset 10)") {
		t.Errorf("expected header for Patchset 10, got:\n%s", out)
	}
	if !strings.Contains(out, "static-checks-pigweed") || !strings.Contains(out, "https://ci.chromium.org/b/901") {
		t.Errorf("expected static-checks-pigweed build 901 from patchset 9, got:\n%s", out)
	}
	if !strings.Contains(out, "pigweed-linux") || !strings.Contains(out, "https://ci.chromium.org/b/801") {
		t.Errorf("expected pigweed-linux build 801 from patchset 8, got:\n%s", out)
	}

	// Each reused build is labeled with the patchset it actually ran on.
	wantNotes := map[string]string{
		"https://ci.chromium.org/b/901": "(from patchset 9)",
		"https://ci.chromium.org/b/801": "(from patchset 8)",
	}
	for _, line := range strings.Split(out, "\n") {
		for url, note := range wantNotes {
			if strings.Contains(line, url) && !strings.Contains(line, note) {
				t.Errorf("expected row for %s to contain %q, got: %q", url, note, line)
			}
		}
	}

	jsonOut, err := executeCommand(RootCmd, "pr", "checks", "12345", "--json", "checks,equivalentPatchsets")
	if err != nil {
		t.Fatalf("pr checks --json failed: %v\nOutput:\n%s", err, jsonOut)
	}
	var got struct {
		Checks []struct {
			ID       string `json:"id"`
			Patchset int    `json:"patchset"`
		} `json:"checks"`
		EquivalentPatchsets []int `json:"equivalentPatchsets"`
	}
	if err := json.Unmarshal([]byte(jsonOut), &got); err != nil {
		t.Fatalf("failed to parse --json output: %v\nOutput:\n%s", err, jsonOut)
	}
	wantPS := map[string]int{"901": 9, "801": 8}
	if len(got.Checks) != len(wantPS) {
		t.Errorf("got %d checks in JSON, want %d:\n%s", len(got.Checks), len(wantPS), jsonOut)
	}
	for _, c := range got.Checks {
		if want, ok := wantPS[c.ID]; !ok || c.Patchset != want {
			t.Errorf("check %s: patchset = %d, want %d", c.ID, c.Patchset, want)
		}
	}
	if fmt.Sprint(got.EquivalentPatchsets) != "[10 9 8]" {
		t.Errorf("equivalentPatchsets = %v, want [10 9 8]", got.EquivalentPatchsets)
	}
}

func TestDeduplicateLatestBuilds(t *testing.T) {
	build := func(id, builder, status string) bbBuild {
		return FakeBuild(id, builder, status)
	}
	tests := []struct {
		name   string
		builds []bbBuild
		want   []string // IDs, in output order
	}{
		{
			name:   "newest build wins",
			builds: []bbBuild{build("3", "a", "FAILURE"), build("2", "a", "SUCCESS"), build("1", "b", "SUCCESS")},
			want:   []string{"3", "1"},
		},
		{
			name:   "older success replaces newer cancellation",
			builds: []bbBuild{build("3", "a", "CANCELED"), build("2", "b", "SUCCESS"), build("1", "a", "SUCCESS")},
			want:   []string{"1", "2"},
		},
		{
			name:   "older failure does not replace newer cancellation",
			builds: []bbBuild{build("2", "a", "CANCELED"), build("1", "a", "FAILURE")},
			want:   []string{"2"},
		},
		{
			name:   "newest success is kept over older successes",
			builds: []bbBuild{build("3", "a", "CANCELED"), build("2", "a", "SUCCESS"), build("1", "a", "SUCCESS")},
			want:   []string{"2"},
		},
		{
			name:   "running build is not replaced by older success",
			builds: []bbBuild{build("2", "a", "STARTED"), build("1", "a", "SUCCESS")},
			want:   []string{"2"},
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			var got []string
			for _, b := range deduplicateLatestBuilds(tt.builds) {
				got = append(got, b.ID)
			}
			if fmt.Sprint(got) != fmt.Sprint(tt.want) {
				t.Errorf("deduplicateLatestBuilds() IDs = %v, want %v", got, tt.want)
			}
		})
	}
}

// TestChecks_EquivalentPatchsets_SuccessBeatsNewerCancel covers a builder that
// passed on patchset 9 and was then canceled on the code-equivalent patchset
// 10. LUCI CV counts the patchset 9 success, so pr checks must too.
func TestChecks_EquivalentPatchsets_SuccessBeatsNewerCancel(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345,
		WithSubject("pw_ghish: Test canceled on equivalent patchset"),
		WithPatchsetKind(9, "REWORK"),
		WithPatchsetKind(10, "TRIVIAL_REBASE"),
	)
	server.OnSearchBuildsByPatchset(map[int][]bbBuild{
		10: {FakeBuild("1001", "pigweed-linux", "CANCELED")},
		9:  {FakeBuild("901", "pigweed-linux", "SUCCESS")},
	})

	out, err := executeCommand(RootCmd, "pr", "checks", "12345")
	if err != nil {
		t.Fatalf("expected pr checks to pass using the patchset 9 success, got error: %v\nOutput:\n%s", err, out)
	}
	if !strings.Contains(out, "https://ci.chromium.org/b/901") {
		t.Errorf("expected successful build 901 from patchset 9, got:\n%s", out)
	}
	if containsBuildID(out, "1001") {
		t.Errorf("canceled build 1001 should be superseded by the older success, got:\n%s", out)
	}
}

// TestChecks_Watch_SkipsSettledEquivalentPatchsets verifies that --watch only
// re-queries older equivalent patchsets while they still have builds running.
func TestChecks_Watch_SkipsSettledEquivalentPatchsets(t *testing.T) {
	// Patchset 10 (current, TRIVIAL_REBASE): pigweed-linux runs for two polls,
	//   then passes.
	// Patchset 9 (NO_CODE_CHANGE): static-checks-pigweed is still running at
	//   first, then passes on the next query.
	// Patchset 8 (REWORK): docs already passed.
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345,
		WithSubject("pw_ghish: Test watch caching"),
		WithPatchsetKind(8, "REWORK"),
		WithPatchsetKind(9, "NO_CODE_CHANGE"),
		WithPatchsetKind(10, "TRIVIAL_REBASE"),
	)

	var mu sync.Mutex
	queries := make(map[int]int)
	server.On("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", func(w http.ResponseWriter, r *http.Request) {
		var req bbSearchBuildsRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil || len(req.Predicate.GerritChanges) != 1 {
			t.Errorf("unexpected SearchBuilds request (err = %v): %+v", err, req)
			server.RespondJSON(w, http.StatusBadRequest, map[string]any{})
			return
		}
		ps := req.Predicate.GerritChanges[0].Patchset
		mu.Lock()
		queries[ps]++
		n := queries[ps]
		mu.Unlock()

		var builds []bbBuild
		switch ps {
		case 10:
			status := "STARTED"
			if n >= 3 {
				status = "SUCCESS"
			}
			builds = []bbBuild{FakeBuild("1001", "pigweed-linux", status)}
		case 9:
			status := "STARTED"
			if n >= 2 {
				status = "SUCCESS"
			}
			builds = []bbBuild{FakeBuild("901", "static-checks-pigweed", status)}
		case 8:
			builds = []bbBuild{FakeBuild("801", "docs", "SUCCESS")}
		default:
			t.Errorf("unexpected SearchBuilds call for patchset %d", ps)
		}
		server.RespondJSON(w, http.StatusOK, map[string]any{"builds": builds})
	})

	out, err := executeCommand(RootCmd, "pr", "checks", "12345", "--watch", "--interval", "5ms", "--buildbucket-host", server.URL)
	if err != nil {
		t.Fatalf("pr checks --watch failed: %v\nOutput:\n%s", err, out)
	}

	mu.Lock()
	defer mu.Unlock()
	// Patchset 10 is queried on every poll until pigweed-linux passes.
	// Patchset 9 is queried until static-checks-pigweed finishes, and
	// patchset 8 only once, since its builds had already finished.
	want := map[int]int{10: 3, 9: 2, 8: 1}
	for ps, n := range want {
		if queries[ps] != n {
			t.Errorf("patchset %d queried %d times, want %d (all queries: %v)", ps, queries[ps], n, queries)
		}
	}
}

func TestChecks_HideTagFiltersAndLocalPresubmitHintFromProjectConfig(t *testing.T) {
	tmpDir := t.TempDir()
	toml := `
[ci]
hide_tag_filters = ["hide-in-gerrit:subbuild"]
local_presubmit_hint = "fx test //src/..."
`
	if err := os.WriteFile(filepath.Join(tmpDir, ".ghish.toml"), []byte(toml), 0644); err != nil {
		t.Fatalf("failed to write .ghish.toml: %v", err)
	}

	server := NewMockGerritServer(t)
	MockCWD = tmpDir
	server.OnDefaultChange(12345, WithSubject("Orchestrator Subbuild Filtering"))
	server.OnSearchBuilds(
		FakeBuild("100", "fuchsia-x64-orchestrator", "FAILURE"),
		FakeBuild("101", "core.x64-subbuild-1", "FAILURE", WithTags(bbTag{Key: "hide-in-gerrit", Value: "subbuild"})),
		FakeBuild("102", "core.x64-subbuild-2", "SUCCESS", WithTags(bbTag{Key: "hide-in-gerrit", Value: "subbuild"})),
	)

	output, err := executeCommand(RootCmd, "pr", "checks", "12345")
	if got := ExitCodeFor(err); got != ExitCodeFailure {
		t.Fatalf("ExitCodeFor(%v) = %d, want %d\nOutput: %s", err, got, ExitCodeFailure, output)
	}

	combined := output + "\n" + err.Error()
	if !strings.Contains(output, "fuchsia-x64-orchestrator") {
		t.Errorf("Expected orchestrator in output, got:\n%s", output)
	}
	if strings.Contains(output, "core.x64-subbuild-1") || strings.Contains(output, "core.x64-subbuild-2") {
		t.Errorf("Expected subbuilds matching hide_tag_filters to be hidden, got:\n%s", output)
	}
	if !strings.Contains(output, "2") || !strings.Contains(output, "hidden") {
		t.Errorf("Expected note about 2 hidden builds, got:\n%s", output)
	}
	if !strings.Contains(combined, "fx test //src/...") {
		t.Errorf("Expected configured local_presubmit_hint 'fx test //src/...' in output, got:\n%s", combined)
	}

	allOutput, _ := executeCommand(RootCmd, "pr", "checks", "12345", "--all")
	if !strings.Contains(allOutput, "core.x64-subbuild-1") || !strings.Contains(allOutput, "core.x64-subbuild-2") {
		t.Errorf("Expected --all to include subbuilds hidden by hide_tag_filters, got:\n%s", allOutput)
	}
}

func TestChecks_GerritGates_FailedLintLabelEvenWithZeroBuilds(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345,
		WithSubject("Change with Gerrit Gate Failure"),
		func(ch map[string]any) {
			ch["submit_requirements"] = []map[string]any{
				{"name": "Code-Review", "status": "SATISFIED"},
				{"name": "API-Review", "status": "UNSATISFIED"},
				{"name": "DrNo-Review", "status": "UNSATISFIED"},
				{"name": "Lint", "status": "UNSATISFIED"},
				{"name": "Copybara-Verified", "status": "SATISFIED"},
				{"name": "Topics-Not-Supported", "status": "UNSATISFIED"},
			}
			ch["labels"] = map[string]any{
				"Code-Review":       narrowLabel(-2, 2, 2),
				"API-Review":        narrowLabel(-1, 1, 0),
				"DrNo-Review":       narrowLabel(-1, 1, 0),
				"Lint":              narrowLabel(-1, 1, -1),
				"Copybara-Verified": narrowLabel(-1, 1, 1),
			}
		},
	)
	server.OnSearchBuilds()

	output, err := executeCommand(RootCmd, "pr", "checks", "12345")
	if got := ExitCodeFor(err); got != ExitCodeFailure {
		t.Fatalf("ExitCodeFor(%v) = %d, want %d\nOutput: %s", err, got, ExitCodeFailure, output)
	}

	for _, want := range []string{
		"Gerrit Gates / Submit Requirements:",
		"✗  Lint",
		"✓  Copybara-Verified",
		"✗  Topics-Not-Supported",
	} {
		if !strings.Contains(output, want) {
			t.Errorf("Expected output to contain %q, got:\n%s", want, output)
		}
	}
	gates, _, _ := strings.Cut(output, "Not yet submittable")
	for _, notWant := range []string{"API-Review", "DrNo-Review"} {
		if strings.Contains(gates, notWant) {
			t.Errorf("Expected unvoted human review requirement %q to be omitted from CI gates, got:\n%s", notWant, output)
		}
	}
	if !strings.Contains(err.Error(), "Gerrit gate(s) failed") || !strings.Contains(err.Error(), "Lint") {
		t.Errorf("Expected error to mention Gerrit gate failure on Lint, got: %v", err)
	}
	if !strings.Contains(output, "Not yet submittable: API-Review (Unsatisfied), DrNo-Review (Unsatisfied), Lint (Rejected)") {
		t.Errorf("Expected unsatisfied submit requirements in the blockers line, got:\n%s", output)
	}
	if !slices.ContainsFunc(server.Requests(), func(r MockRequest) bool {
		o := r.URL.Query()["o"]
		return slices.Contains(o, "SUBMIT_REQUIREMENTS") && slices.Contains(o, "SUBMITTABLE")
	}) {
		t.Error("pr checks must request o=SUBMIT_REQUIREMENTS and o=SUBMITTABLE for its change fetch")
	}
}

func TestLookupChangeLabel_CaseInsensitive(t *testing.T) {
	change := &ChangeInfo{
		Labels: map[string]LabelInfo{
			"LINT": {
				Recommended: AccountInfo{AccountID: 1},
			},
		},
	}

	gotKey, gotInfo, ok := lookupChangeLabel(change, "Lint")
	if !ok {
		t.Fatalf("lookupChangeLabel(change, %q) returned ok=false, want true", "Lint")
	}
	if gotKey != "LINT" {
		t.Errorf("lookupChangeLabel key = %q, want %q", gotKey, "LINT")
	}
	if gotInfo.Recommended.AccountID != 1 {
		t.Errorf("lookupChangeLabel Recommended.AccountID = %d, want 1", gotInfo.Recommended.AccountID)
	}
}

func fakeTenPassOneFailBuilds() []bbBuild {
	var builds []bbBuild
	for i := 1; i <= 10; i++ {
		builds = append(builds, FakeBuild(
			fmt.Sprintf("10%d", i),
			fmt.Sprintf("pass-builder-%02d", i),
			"SUCCESS",
			WithBucket("pigweed.try"),
		))
	}
	builds = append(builds, FakeBuild("201", "fail-builder", "FAILURE", WithBucket("pigweed.try")))
	return builds
}

func TestChecks_CollapsesPassingChecksByDefaultAndExpandsWithAll(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Collapse Passing Checks"), WithCurrentPatchset(3))
	server.OnSearchBuilds(fakeTenPassOneFailBuilds()...)

	// Default: > 5 passing checks are collapsed, failing check is shown first.
	output, err := executeCommand(RootCmd, "pr", "checks", "12345")
	if got := ExitCodeFor(err); got != ExitCodeFailure {
		t.Fatalf("ExitCodeFor(%v) = %d, want %d\nOutput: %s", err, got, ExitCodeFailure, output)
	}
	if !strings.Contains(output, "fail-builder") {
		t.Errorf("Expected fail-builder in output, got:\n%s", output)
	}
	wantOmitted := "10 passing checks omitted (use --all to list)"
	if !strings.Contains(output, wantOmitted) {
		t.Errorf("Expected %q in output, got:\n%s", wantOmitted, output)
	}
	if strings.Contains(output, "pass-builder-01") {
		t.Errorf("Expected individual passing checks to be collapsed in default view, got:\n%s", output)
	}

	// With --all: all 10 passing checks are expanded.
	allOutput, err := executeCommand(RootCmd, "pr", "checks", "12345", "--all")
	if got := ExitCodeFor(err); got != ExitCodeFailure {
		t.Fatalf("ExitCodeFor(%v) = %d, want %d\nOutput: %s", err, got, ExitCodeFailure, allOutput)
	}
	for i := 1; i <= 10; i++ {
		name := fmt.Sprintf("pass-builder-%02d", i)
		if !strings.Contains(allOutput, name) {
			t.Errorf("Expected %q in --all output, got:\n%s", name, allOutput)
		}
	}
	if strings.Contains(allOutput, "passing checks omitted") {
		t.Errorf("Expected no omitted passing line with --all, got:\n%s", allOutput)
	}
}

func TestChecks_InfraFailureAnnotationAndFailureClustering(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Infra Failure & Clustering"))
	server.OnSearchBuilds(
		FakeBuild("1", "pass-builder", "SUCCESS", WithBucket("pigweed.try")),
		FakeBuild("2", "infra-builder-1", "INFRA_FAILURE", WithBucket("pigweed.try"), WithSummary("Task did not start, no resource\nextra details 1")),
		FakeBuild("3", "infra-builder-2", "INFRA_FAILURE", WithBucket("pigweed.try"), WithSummary("Task did not start, no resource\nextra details 2")),
	)

	output, err := executeCommand(RootCmd, "pr", "checks", "12345")
	if got := ExitCodeFor(err); got != ExitCodeFailure {
		t.Fatalf("ExitCodeFor(%v) = %d, want %d\nOutput: %s", err, got, ExitCodeFailure, output)
	}
	if !strings.Contains(output, "[infra failure]") {
		t.Errorf("Expected [infra failure] annotation in output, got:\n%s", output)
	}
	wantCluster := "Cluster: 2 builders failed with the same diagnostic: Task did not start, no resource"
	if !strings.Contains(output, wantCluster) {
		t.Errorf("Expected failure cluster %q in output, got:\n%s", wantCluster, output)
	}
	if strings.Index(output, "infra-builder-1") > strings.Index(output, "pass-builder") {
		t.Errorf("Expected failed builders to be ordered before passing builders, got:\n%s", output)
	}
}

func TestChecks_ExplicitLogFailedFlagWithoutWatch(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Explicit Log Failed"))
	server.OnSearchBuilds(
		FakeBuild("200", "static-checks", "FAILURE", WithBucket("pigweed.try")),
	)
	server.OnGetBuild(FakeBuildDetails("200", "static-checks", "FAILURE", WithBuildSummary("clang-format diff detected")))

	output, err := executeCommand(RootCmd, "pr", "checks", "12345", "--log-failed", "--buildbucket-host", server.URL)
	if got := ExitCodeFor(err); got != ExitCodeFailure {
		t.Fatalf("ExitCodeFor(%v) = %d, want %d\nOutput: %s", err, got, ExitCodeFailure, output)
	}
	if !strings.Contains(output, "FAILURE: static-checks (Build 200)") {
		t.Errorf("Expected failure report header with --log-failed, got:\n%s", output)
	}
	if !strings.Contains(output, "clang-format diff detected") {
		t.Errorf("Expected failure summary with --log-failed, got:\n%s", output)
	}
}

// TestChecksExitStatus_VerdictShown verifies that once the Verdict:/Next:
// footer is printed, the error keeps only its summary (plus the repro hint
// and the pending exit-code note), while the exit code is unchanged.
func TestChecksExitStatus_VerdictShown(t *testing.T) {
	failed := checkTally{
		blocking:           2,
		failed:             []bbBuild{exitStatusBuild("pigweed-linux", "FAILURE")},
		localPresubmitHint: "./pw presubmit --step lint",
	}
	pending := checkTally{blocking: 1, pending: []bbBuild{exitStatusBuild("pigweed-mac", "STARTED")}}
	canceled := checkTally{blocking: 1, canceled: []bbBuild{exitStatusBuild("pigweed-win", "CANCELED")}}
	gates := checkTally{blocking: 1, failedGates: []string{"Lint (-1)"}}
	none := checkTally{}

	tests := []struct {
		name        string
		tally       checkTally
		wantCode    int
		wantSummary string
		// proseOnly appears only when the footer was not shown.
		proseOnly string
		// alwaysKept appears in both modes.
		alwaysKept string
	}{
		{"failed", failed, ExitCodeFailure, `check "pigweed-linux" failed on Change 12345 (Patchset 2).`, "gh run rerun 12345 --failed", "./pw presubmit --step lint"},
		{"pending", pending, ExitCodePending, "1 of 1 checks are still running", "gh pr checks 12345 --watch", `Exit code 8 means "checks pending"`},
		{"canceled", canceled, ExitCodeFailure, `check "pigweed-win" was canceled`, "gh pr edit 12345 --cq", ""},
		{"gates", gates, ExitCodeFailure, "Gerrit gate(s) failed on Change 12345 (Patchset 2): Lint (-1).", "gh pr view 12345 --comments", ""},
		{"no checks", none, ExitCodeFailure, "no checks reported on Change 12345 (Patchset 2).", "gh pr edit 12345 --cq", ""},
	}
	for _, tt := range tests {
		for _, shown := range []bool{false, true} {
			t.Run(fmt.Sprintf("%s/verdictShown=%v", tt.name, shown), func(t *testing.T) {
				tally := tt.tally
				tally.verdictShown = shown
				err := checksExitStatus(12345, 2, tally)
				if got := ExitCodeFor(err); got != tt.wantCode {
					t.Fatalf("exit code = %d, want %d (err = %v)", got, tt.wantCode, err)
				}
				msg := err.Error()
				if !strings.Contains(msg, tt.wantSummary) {
					t.Errorf("missing summary %q in:\n%s", tt.wantSummary, msg)
				}
				if tt.alwaysKept != "" && !strings.Contains(msg, tt.alwaysKept) {
					t.Errorf("missing %q in:\n%s", tt.alwaysKept, msg)
				}
				if hasProse := strings.Contains(msg, tt.proseOnly); hasProse == shown {
					t.Errorf("remediation %q present = %v, want %v:\n%s", tt.proseOnly, hasProse, !shown, msg)
				}
			})
		}
	}
}

// TestChecks_PendingFooterSuggestsWatchFailFast checks the pr checks footer
// uses the documented agent watch command rather than `gh run watch`.
func TestChecks_PendingFooterSuggestsWatchFailFast(t *testing.T) {
	server := exitCodeChecksServer(t, map[string]string{"a": "STARTED", "b": "SUCCESS"}, nil)
	output, err := executeCommand(RootCmd, "pr", "checks", "12345", "--buildbucket-host", server.URL)
	if got := ExitCodeFor(err); got != ExitCodePending {
		t.Fatalf("exit code = %d, want %d\nOutput:\n%s", got, ExitCodePending, output)
	}
	for _, want := range []string{"Verdict: PENDING (1 running, 1 passed)", "Next:    gh pr checks 12345 --watch --fail-fast"} {
		if !strings.Contains(output, want) {
			t.Errorf("missing %q in:\n%s", want, output)
		}
	}
}

func TestChecks_JSON_RejectsWatch(t *testing.T) {
	for _, flag := range []string{"--watch", "--fail-fast"} {
		_, err := executeCommand(RootCmd, "pr", "checks", "12345", "--json", "status", flag)
		if err == nil || !strings.Contains(err.Error(), "cannot use `--watch` with `--json` flag") {
			t.Errorf("pr checks --json %s: err = %v, want cannot use `--watch` with `--json` flag", flag, err)
		}
	}
}

func TestChecks_AllPassingCollapsedAndWatchExpanded(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345)
	var builds []bbBuild
	for i := 1; i <= 6; i++ {
		builds = append(builds, FakeBuild(fmt.Sprintf("%d", i), fmt.Sprintf("pass-builder-%d", i), "SUCCESS"))
	}
	server.OnSearchBuilds(builds...)

	collapsedOut, err := executeCommand(RootCmd, "pr", "checks", "12345")
	if err != nil {
		t.Fatalf("pr checks failed: %v\nOutput: %s", err, collapsedOut)
	}
	if !strings.Contains(collapsedOut, "All 6 checks passed (use --all to list)") || strings.Contains(collapsedOut, "pass-builder-1") {
		t.Errorf("expected collapsed all-passing summary without individual rows, got:\n%s", collapsedOut)
	}

	watchOut, err := executeCommand(RootCmd, "pr", "checks", "12345", "--watch", "-i", "0")
	if err != nil {
		t.Fatalf("pr checks --watch failed: %v\nOutput: %s", err, watchOut)
	}
	if !strings.Contains(watchOut, "pass-builder-1") || !strings.Contains(watchOut, "pass-builder-6") {
		t.Errorf("expected --watch to expand all 6 passing builders, got:\n%s", watchOut)
	}
}
