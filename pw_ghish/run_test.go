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
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func TestRunList(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Test Run List"))
	server.OnSearchBuilds(
		FakeBuild("111", "builder-pass", "SUCCESS", WithTimes("2026-09-08T10:00:00Z", "2026-09-08T10:02:00Z")),
		FakeBuild("222", "builder-fail", "FAILURE", WithTimes("2026-09-08T10:00:00Z", "2026-09-08T10:01:00Z")),
	)

	output, err := executeCommand(RootCmd, "run", "list", "12345")
	if err != nil {
		t.Fatalf("run list failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "builder-pass") || !strings.Contains(output, "builder-fail") {
		t.Errorf("Expected both builders in run list output, got:\n%s", output)
	}
	if !strings.Contains(output, "111") || !strings.Contains(output, "222") {
		t.Errorf("Expected build IDs in output, got:\n%s", output)
	}
	if !strings.Contains(output, "Showing 2 checks for Change 12345 (Patchset 1) • Test Run List") {
		t.Errorf("Expected context header in run list output, got:\n%s", output)
	}
}

func TestRunView_DefaultStructuredSummary(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Test Run View Summary"))
	server.OnSearchBuilds(
		FakeBuild("111", "builder-pass", "SUCCESS", WithTimes("2026-09-08T10:00:00Z", "2026-09-08T10:02:00Z")),
		FakeBuild("222", "builder-fail", "FAILURE", WithTimes("2026-09-08T10:00:00Z", "2026-09-08T10:01:00Z")),
	)

	output, err := executeCommand(RootCmd, "run", "view", "12345")
	if err != nil {
		t.Fatalf("run view failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "Change 12345 (Patchset 1)") {
		t.Errorf("Expected Change 12345 (Patchset 1) in run view output, got:\n%s", output)
	}
	if !strings.Contains(output, "Test Run View Summary") {
		t.Errorf("Expected subject in run view output, got:\n%s", output)
	}
	if !strings.Contains(output, "JOBS") {
		t.Errorf("Expected JOBS section header, got:\n%s", output)
	}
	if !strings.Contains(output, "builder-pass") || !strings.Contains(output, "builder-fail") {
		t.Errorf("Expected builders in JOBS list, got:\n%s", output)
	}
	if !strings.Contains(output, "To view failed logs:   gh run view 12345 --log-failed") {
		t.Errorf("Expected helpful footer hints, got:\n%s", output)
	}
}

func TestRunView_Default_JSON(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Test Run View Summary JSON"))
	server.OnSearchBuilds(
		FakeBuild("111", "builder-pass", "SUCCESS"),
	)

	output, err := executeCommand(RootCmd, "run", "view", "12345", "--json")
	if err != nil {
		t.Fatalf("run view --json failed: %v\nOutput: %s", err, output)
	}

	var parsed map[string]any
	if err := json.Unmarshal([]byte(output), &parsed); err != nil {
		t.Fatalf("Failed to parse JSON: %v\nOutput: %s", err, output)
	}
	if parsed["subject"] != "Test Run View Summary JSON" {
		t.Errorf("Expected subject in JSON, got %v", parsed["subject"])
	}
	if parsed["status"] != "SUCCESS" {
		t.Errorf("Expected status SUCCESS in JSON, got %v", parsed["status"])
	}
	jobs, ok := parsed["jobs"].([]any)
	if !ok || len(jobs) != 1 {
		t.Errorf("Expected 1 job in JSON, got %v", parsed["jobs"])
	}
}

func TestRunView_LogFailed(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Test Run View Log Failed"))
	server.OnSearchBuilds(
		FakeBuild("222", "pigweed-lint", "FAILURE"),
	)
	server.OnGetBuild(FakeBuildDetails("222", "pigweed-lint", "FAILURE",
		WithBuildSummary("flake in linter"),
		WithSteps(FakeStep("lint_check", "FAILURE")),
	))

	output, err := executeCommand(RootCmd, "run", "view", "12345", "--log-failed")
	if err != nil {
		t.Fatalf("run view --log-failed failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "FAILURE: pigweed-lint (Build 222)") {
		t.Errorf("Expected failure report for pigweed-lint, got:\n%s", output)
	}
	if !strings.Contains(output, "flake in linter") {
		t.Errorf("Expected failure summary markdown in output, got:\n%s", output)
	}
}

func TestRunView_LogFailed_NoFailedChecks(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("All Passing Change"))
	server.OnSearchBuilds(
		FakeBuild("111", "pigweed-build", "SUCCESS"),
	)

	output, err := executeCommand(RootCmd, "run", "view", "12345", "--log-failed")
	if err != nil {
		t.Fatalf("run view --log-failed failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "No failed checks found on this change.") {
		t.Errorf("Expected 'No failed checks found on this change.', got:\n%s", output)
	}
}

func TestRunView_VerboseSteps(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Steps Change"))
	server.OnSearchBuilds(
		FakeBuild("111", "pigweed-compile", "STARTED"),
	)
	server.OnGetBuild(FakeBuildDetails("111", "pigweed-compile", "STARTED",
		WithSteps(
			FakeStep("setup", "SUCCESS"),
			FakeStep("compile", "STARTED"),
		),
	))

	output, err := executeCommand(RootCmd, "run", "view", "12345", "pigweed-compile", "-v")
	if err != nil {
		t.Fatalf("run view -v failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "Steps for pigweed-compile (Build 111)") {
		t.Errorf("Expected steps header, got:\n%s", output)
	}
	if !strings.Contains(output, "✓  setup") || !strings.Contains(output, "●  compile") {
		t.Errorf("Expected step items, got:\n%s", output)
	}

	runOut, err := executeCommand(RootCmd, "run", "view", "12345", "-j", "pigweed-compile", "--log-failed")
	if err != nil || !strings.Contains(runOut, "gh run view 12345 -j pigweed-compile -v") {
		t.Errorf("run view -j --log-failed on running build = (%q, %v), want 'gh run view 12345 -j pigweed-compile -v'", runOut, err)
	}
}

func TestRunView_JobFlag(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Job Flag Change"))
	server.OnSearchBuilds(
		FakeBuild("111", "pigweed-compile", "FAILURE"),
	)
	server.OnGetBuild(FakeBuildDetails("111", "pigweed-compile", "FAILURE",
		WithBuildSummary("compiler error"),
	))

	output, err := executeCommand(RootCmd, "run", "view", "12345", "-j", "pigweed-compile", "--log-failed")
	if err != nil {
		t.Fatalf("run view -j failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "FAILURE: pigweed-compile (Build 111)") {
		t.Errorf("Expected failure report for pigweed-compile, got:\n%s", output)
	}
}

func TestRunView_JobFlag_ShowsStepsByDefault(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Job Flag Steps Default"))
	server.OnSearchBuilds(
		FakeBuild("111", "pigweed-compile", "SUCCESS"),
	)
	server.OnGetBuild(FakeBuildDetails("111", "pigweed-compile", "SUCCESS",
		WithSteps(
			FakeStep("setup", "SUCCESS"),
			FakeStep("build", "SUCCESS"),
		),
	))

	output, err := executeCommand(RootCmd, "run", "view", "12345", "-j", "pigweed-compile")
	if err != nil {
		t.Fatalf("run view -j failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "Steps for pigweed-compile (Build 111)") {
		t.Errorf("Expected steps header by default when targeting builder, got:\n%s", output)
	}
	if !strings.Contains(output, "✓  setup") || !strings.Contains(output, "✓  build") {
		t.Errorf("Expected steps in output, got:\n%s", output)
	}
}

func TestRunView_Web(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Web Run View"))
	server.OnSearchBuilds(
		FakeBuild("8671017385591991697", "static-checks-pigweed", "SUCCESS"),
	)

	var openedURL string
	oldOpenBrowser := OpenBrowserFn
	defer func() { OpenBrowserFn = oldOpenBrowser }()
	OpenBrowserFn = func(urlStr string) error {
		openedURL = urlStr
		return nil
	}

	output, err := executeCommand(RootCmd, "run", "view", "12345", "static-checks-pigweed", "-w")
	if err != nil {
		t.Fatalf("run view -w failed: %v\nOutput: %s", err, output)
	}

	if openedURL != "https://ci.chromium.org/b/8671017385591991697" {
		t.Errorf("Expected browser to open Buildbucket URL https://ci.chromium.org/b/8671017385591991697, got %q", openedURL)
	}
	if !strings.Contains(output, "Opening https://ci.chromium.org/b/8671017385591991697 in your browser.") {
		t.Errorf("Expected 'Opening ... in your browser.' in output, got:\n%s", output)
	}
}

func TestRunRerun_Failed(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Rerun Change"))
	server.OnSearchBuilds(
		FakeBuild("111", "pigweed-linux", "FAILURE", WithBucket("pigweed.try")),
	)

	output, err := executeCommand(RootCmd, "run", "rerun", "12345", "--failed", "--dry-run")
	if err != nil {
		t.Fatalf("run rerun --failed failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "[dry-run]") || !strings.Contains(output, "pigweed-linux") {
		t.Errorf("Expected dry-run rerun for pigweed-linux, got:\n%s", output)
	}
}

func TestRunRerun_Job(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Rerun Job Change"))
	server.OnSearchBuilds(
		FakeBuild("111", "pigweed-linux", "FAILURE", WithBucket("pigweed.try")),
	)

	output, err := executeCommand(RootCmd, "run", "rerun", "12345", "-j", "pigweed-linux", "--dry-run")
	if err != nil {
		t.Fatalf("run rerun -j failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "[dry-run]") || !strings.Contains(output, "pigweed-linux") {
		t.Errorf("Expected dry-run rerun for pigweed-linux, got:\n%s", output)
	}
}

func TestRunRerun_MissingBuilderError(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Rerun Missing Error Change"))
	server.OnSearchBuilds(
		FakeBuild("111", "pigweed-linux", "FAILURE", WithBucket("pigweed.try")),
	)

	_, err := executeCommand(RootCmd, "run", "rerun", "12345")
	if err == nil {
		t.Fatal("Expected error when no builder specified and --failed not passed, got nil")
	}
	errStr := err.Error()
	if !strings.Contains(errStr, "specify a builder name or pass --failed") {
		t.Errorf("Expected guidance to specify builder name or --failed, got: %v", err)
	}
	if !strings.Contains(errStr, "pigweed-linux") {
		t.Errorf("Expected failed builder to be listed, got: %v", err)
	}
	if !strings.Contains(errStr, "gh run rerun 12345 --failed") {
		t.Errorf("Expected change-scoped usage example, got: %v", err)
	}
}

func TestRunView_DirectBuildID(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnGetBuild(FakeBuildDetails("8671182706745774001", "pigweed-lint", "FAILURE",
		WithBuildSummary("lint check failed"),
	))

	output, err := executeCommand(RootCmd, "run", "view", "8671182706745774001", "-v")
	if err != nil {
		t.Fatalf("run view direct build ID failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "Steps for pigweed-lint (Build 8671182706745774001)") {
		t.Errorf("Expected steps tree for direct build ID, got:\n%s", output)
	}
}

func TestRunView_DirectBuildID_Web(t *testing.T) {
	var openedURL string
	origOpen := OpenBrowserFn
	defer func() { OpenBrowserFn = origOpen }()
	OpenBrowserFn = func(url string) error {
		openedURL = url
		return nil
	}

	output, err := executeCommand(RootCmd, "run", "view", "8671182706745774001", "-w")
	if err != nil {
		t.Fatalf("run view -w failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "Opening https://ci.chromium.org/b/8671182706745774001 in your browser.") {
		t.Errorf("Expected opening message, got:\n%s", output)
	}
	if openedURL != "https://ci.chromium.org/b/8671182706745774001" {
		t.Errorf("Expected URL https://ci.chromium.org/b/8671182706745774001, got %q", openedURL)
	}
}

func TestRunView_BuilderNotFound_ShowsAvailable(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Builder Not Found Change"))
	server.OnSearchBuilds(
		FakeBuild("111", "pigweed-linux", "SUCCESS"),
	)

	_, err := executeCommand(RootCmd, "run", "view", "12345", "-v", "-j", "nonexistent")
	if err == nil {
		t.Fatal("Expected error for nonexistent builder, got nil")
	}
	errStr := err.Error()
	if !strings.Contains(errStr, "check \"nonexistent\" not found") {
		t.Errorf("Expected not found message, got: %v", err)
	}
	if !strings.Contains(errStr, "pigweed-linux") {
		t.Errorf("Expected available check pigweed-linux to be listed, got: %v", err)
	}
	if _, err := executeCommand(RootCmd, "run", "view", "12345", "--log-failed", "-j", "nonexistent"); err == nil || !strings.Contains(err.Error(), "Run 'gh run list 12345' to view all checks") {
		t.Errorf("Expected 'gh run list 12345' hint in --log-failed error, got: %v", err)
	}
	emptyServer := NewMockGerritServer(t)
	emptyServer.OnDefaultChange(12345, WithSubject("Builder Not Found Change"))
	emptyServer.OnSearchBuilds()
	if _, err := executeCommand(RootCmd, "run", "view", "12345", "--log-failed", "-j", "nonexistent"); err == nil || !strings.Contains(err.Error(), "Run 'gh run list 12345' to view check status") {
		t.Errorf("Expected 'gh run list 12345' no-checks hint in --log-failed error, got: %v", err)
	}
}

func TestRunRerun_BuilderNotFound_ShowsAvailable(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Rerun Not Found Change"))
	server.OnSearchBuilds(
		FakeBuild("111", "pigweed-linux", "SUCCESS"),
	)

	_, err := executeCommand(RootCmd, "run", "rerun", "12345", "-j", "nonexistent")
	if err == nil {
		t.Fatal("Expected error for nonexistent builder, got nil")
	}
	errStr := err.Error()
	if !strings.Contains(errStr, "builder \"nonexistent\" not found") {
		t.Errorf("Expected not found message, got: %v", err)
	}
	if !strings.Contains(errStr, "pigweed-linux") {
		t.Errorf("Expected available check pigweed-linux to be listed, got: %v", err)
	}
}

func TestRunList_JSON(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Run List JSON"))
	server.OnSearchBuilds(
		FakeBuild("111", "pigweed-linux", "SUCCESS"),
	)

	output, err := executeCommand(RootCmd, "run", "list", "12345", "--json")
	if err != nil {
		t.Fatalf("run list --json failed: %v\nOutput: %s", err, output)
	}

	var parsed []map[string]any
	if err := json.Unmarshal([]byte(output), &parsed); err != nil {
		t.Fatalf("Failed to parse JSON output: %v\nOutput: %s", err, output)
	}
	if len(parsed) != 1 {
		t.Fatalf("Expected 1 build in JSON output, got %d", len(parsed))
	}
	if parsed[0]["name"] != "pigweed-linux" {
		t.Errorf("Expected builder pigweed-linux, got %v", parsed[0]["name"])
	}
}

func TestParseRunTargetArgs(t *testing.T) {
	ctx := context.Background()
	mockGit := NewMockGit(t).WithCommit("Subject\n\nChange-Id: I9999999999999999999999999999999999999999\n")
	cmd := mockCmdWithGit(ctx, mockGit)

	t.Run("zero args uses active change and jobFlag", func(t *testing.T) {
		rawID, builder, directID, err := parseRunTargetArgs(cmd, nil, "pigweed-linux", true)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if rawID != "I9999999999999999999999999999999999999999" || builder != "pigweed-linux" || directID != "" {
			t.Errorf("got (%q, %q, %q)", rawID, builder, directID)
		}
	})

	t.Run("one arg Buildbucket ID when allowBuildID is true", func(t *testing.T) {
		rawID, builder, directID, err := parseRunTargetArgs(cmd, []string{"8680709829694997521"}, "", true)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if rawID != "" || builder != "" || directID != "8680709829694997521" {
			t.Errorf("got (%q, %q, %q)", rawID, builder, directID)
		}
	})

	t.Run("one arg numeric change number", func(t *testing.T) {
		rawID, builder, directID, err := parseRunTargetArgs(cmd, []string{"12345"}, "my-job", false)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if rawID != "12345" || builder != "my-job" || directID != "" {
			t.Errorf("got (%q, %q, %q)", rawID, builder, directID)
		}
	})

	t.Run("one arg builder name resolves active change", func(t *testing.T) {
		rawID, builder, directID, err := parseRunTargetArgs(cmd, []string{"pigweed-windows"}, "", false)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if rawID != "I9999999999999999999999999999999999999999" || builder != "pigweed-windows" || directID != "" {
			t.Errorf("got (%q, %q, %q)", rawID, builder, directID)
		}
	})

	t.Run("two args change and builder", func(t *testing.T) {
		rawID, builder, directID, err := parseRunTargetArgs(cmd, []string{"12345", "pos-builder"}, "flag-builder", true)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if rawID != "12345" || builder != "flag-builder" || directID != "" {
			t.Errorf("got (%q, %q, %q)", rawID, builder, directID)
		}
	})
}

func TestCollectFailedBuilders(t *testing.T) {
	builds := []bbBuild{
		FakeBuild("1", "builder-pass", "SUCCESS"),
		FakeBuild("2", "builder-fail", "FAILURE"),
		FakeBuild("3", "builder-infra", "INFRA_FAILURE"),
		FakeBuild("4", "builder-exp-fail", "FAILURE", WithExperiments("luci.non_production")),
		FakeBuild("5", "builder-fail", "FAILURE"), // duplicate builder name
	}

	gotNoExp := collectFailedBuilders(builds, false)
	if len(gotNoExp) != 2 || gotNoExp[0] != "builder-fail" || gotNoExp[1] != "builder-infra" {
		t.Errorf("collectFailedBuilders(includeExperimental=false) = %v, want [builder-fail builder-infra]", gotNoExp)
	}

	gotExp := collectFailedBuilders(builds, true)
	if len(gotExp) != 3 || gotExp[2] != "builder-exp-fail" {
		t.Errorf("collectFailedBuilders(includeExperimental=true) = %v, want 3 items including builder-exp-fail", gotExp)
	}
}

func TestRunView_JobFlag_LogFailed_IgnoresSupersededFailure(t *testing.T) {
	// Patchset 9 is a commit-message-only change of patchset 8, so builds from
	// both are valid for patchset 9.
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345", http.StatusOK, map[string]any{
		"id":               "pigweed%2Fpigweed~main~I12345",
		"project":          "pigweed/pigweed",
		"branch":           "main",
		"change_id":        "I12345",
		"subject":          "Superseded failure",
		"status":           "NEW",
		"_number":          12345,
		"current_revision": "rev9",
		"revisions": map[string]any{
			"rev8": map[string]any{"_number": 8, "kind": "REWORK"},
			"rev9": map[string]any{"_number": 9, "kind": "NO_CODE_CHANGE"},
		},
	})
	server.On("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", func(w http.ResponseWriter, r *http.Request) {
		var req bbSearchBuildsRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil || len(req.Predicate.GerritChanges) != 1 {
			t.Errorf("unexpected SearchBuilds request (err = %v): %+v", err, req)
			server.RespondJSON(w, http.StatusBadRequest, map[string]any{})
			return
		}
		var builds []bbBuild
		switch ps := req.Predicate.GerritChanges[0].Patchset; ps {
		case 9:
			builds = []bbBuild{FakeBuild("901", "static-checks-pigweed", "SUCCESS")}
		case 8:
			builds = []bbBuild{FakeBuild("802", "static-checks-pigweed", "FAILURE")}
		default:
			t.Errorf("unexpected SearchBuilds call for patchset %d", ps)
		}
		server.RespondJSON(w, http.StatusOK, map[string]any{"builds": builds})
	})
	server.OnGetBuild(FakeBuildDetails("901", "static-checks-pigweed", "SUCCESS"))

	output, err := executeCommand(RootCmd, "run", "view", "12345", "-j", "static-checks-pigweed", "--log-failed")
	if err != nil {
		t.Fatalf("run view -j --log-failed failed: %v\nOutput: %s", err, output)
	}

	if containsBuildID(output, "802") {
		t.Errorf("reported superseded patchset 8 failure (build 802), got:\n%s", output)
	}
	if !containsBuildID(output, "901") {
		t.Errorf("expected newest build 901 from patchset 9, got:\n%s", output)
	}
}

// newReusedBuildServer serves Change 12345 where patchset 9 is a
// commit-message-only change of patchset 8. static-checks-pigweed ran on
// patchset 9 (build 901, SUCCESS) and pigweed-linux only ran on patchset 8
// (build 801, FAILURE), so build 801 is reused for patchset 9.
func newReusedBuildServer(t *testing.T) *MockGerritServer {
	t.Helper()
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345", http.StatusOK, map[string]any{
		"id":               "pigweed%2Fpigweed~main~I12345",
		"project":          "pigweed/pigweed",
		"branch":           "main",
		"change_id":        "I12345",
		"subject":          "Reused build",
		"status":           "NEW",
		"_number":          12345,
		"current_revision": "rev9",
		"revisions": map[string]any{
			"rev8": map[string]any{"_number": 8, "kind": "REWORK"},
			"rev9": map[string]any{"_number": 9, "kind": "NO_CODE_CHANGE"},
		},
	})
	server.On("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", func(w http.ResponseWriter, r *http.Request) {
		var req bbSearchBuildsRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil || len(req.Predicate.GerritChanges) != 1 {
			t.Errorf("unexpected SearchBuilds request (err = %v): %+v", err, req)
			server.RespondJSON(w, http.StatusBadRequest, map[string]any{})
			return
		}
		var builds []bbBuild
		switch ps := req.Predicate.GerritChanges[0].Patchset; ps {
		case 9:
			builds = []bbBuild{FakeBuild("901", "static-checks-pigweed", "SUCCESS")}
		case 8:
			builds = []bbBuild{FakeBuild("801", "pigweed-linux", "FAILURE")}
		default:
			t.Errorf("unexpected SearchBuilds call for patchset %d", ps)
		}
		server.RespondJSON(w, http.StatusOK, map[string]any{"builds": builds})
	})
	return server
}

// containsBuildID reports whether s mentions the build ID id as a whole
// number. A plain substring match is flaky because mock-server URLs embed
// random ports (e.g. 127.0.0.1:41801 contains "801").
func containsBuildID(s, id string) bool {
	return regexp.MustCompile(`\b` + regexp.QuoteMeta(id) + `\b`).MatchString(s)
}

// checkReusedBuildNotes verifies that the line mentioning build 801 is labeled
// with patchset 8 and the line mentioning build 901 carries no label.
func checkReusedBuildNotes(t *testing.T, output string) {
	t.Helper()
	var saw801, saw901 bool
	for _, line := range strings.Split(output, "\n") {
		if containsBuildID(line, "801") {
			saw801 = true
			if !strings.Contains(line, "(from patchset 8)") {
				t.Errorf("expected build 801 line to contain %q, got: %q", "(from patchset 8)", line)
			}
		}
		if containsBuildID(line, "901") {
			saw901 = true
			if strings.Contains(line, "from patchset") {
				t.Errorf("build 901 ran on the current patchset and should not be labeled, got: %q", line)
			}
		}
	}
	if !saw801 || !saw901 {
		t.Errorf("expected both builds 801 and 901 in output, got:\n%s", output)
	}
}

func TestRunList_LabelsReusedBuilds(t *testing.T) {
	newReusedBuildServer(t)
	output, err := executeCommand(RootCmd, "run", "list", "12345")
	if err != nil {
		t.Fatalf("run list failed: %v\nOutput: %s", err, output)
	}
	checkReusedBuildNotes(t, output)
}

func TestRunView_Summary_LabelsReusedBuilds(t *testing.T) {
	newReusedBuildServer(t)
	output, err := executeCommand(RootCmd, "run", "view", "12345")
	if err != nil {
		t.Fatalf("run view failed: %v\nOutput: %s", err, output)
	}
	checkReusedBuildNotes(t, output)
}

func TestRunView_LogFailed_LabelsReusedBuild(t *testing.T) {
	server := newReusedBuildServer(t)
	server.OnGetBuild(FakeBuildDetails("801", "pigweed-linux", "FAILURE"))

	output, err := executeCommand(RootCmd, "run", "view", "12345", "--log-failed")
	if err != nil {
		t.Fatalf("run view --log-failed failed: %v\nOutput: %s", err, output)
	}
	if want := "FAILURE: pigweed-linux (Build 801, from patchset 8)"; !strings.Contains(output, want) {
		t.Errorf("expected failure header %q, got:\n%s", want, output)
	}
}

// newCanceledOverSuccessServer serves Change 12345 where patchset 10 is a
// trivial rebase of patchset 9:
//   - pigweed-linux passed on patchset 9 (901) and was canceled on 10 (1001).
//   - static-checks-pigweed failed on patchset 9 (902) and passed on 10 (1002).
//
// pr checks reports builds 901 and 1002. It returns a pointer to the list of
// build IDs requested through GetBuild.
func newCanceledOverSuccessServer(t *testing.T) (*MockGerritServer, *[]string) {
	t.Helper()
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345", http.StatusOK, map[string]any{
		"id":               "pigweed%2Fpigweed~main~I12345",
		"project":          "pigweed/pigweed",
		"branch":           "main",
		"change_id":        "I12345",
		"subject":          "Canceled over success",
		"status":           "NEW",
		"_number":          12345,
		"current_revision": "rev10",
		"revisions": map[string]any{
			"rev9":  map[string]any{"_number": 9, "kind": "REWORK"},
			"rev10": map[string]any{"_number": 10, "kind": "TRIVIAL_REBASE"},
		},
	})
	server.On("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", func(w http.ResponseWriter, r *http.Request) {
		var req bbSearchBuildsRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil || len(req.Predicate.GerritChanges) != 1 {
			t.Errorf("unexpected SearchBuilds request (err = %v): %+v", err, req)
			server.RespondJSON(w, http.StatusBadRequest, map[string]any{})
			return
		}
		var builds []bbBuild
		switch ps := req.Predicate.GerritChanges[0].Patchset; ps {
		case 10:
			builds = []bbBuild{
				FakeBuild("1001", "pigweed-linux", "CANCELED"),
				FakeBuild("1002", "static-checks-pigweed", "SUCCESS"),
			}
		case 9:
			builds = []bbBuild{
				FakeBuild("901", "pigweed-linux", "SUCCESS"),
				FakeBuild("902", "static-checks-pigweed", "FAILURE"),
			}
		default:
			t.Errorf("unexpected SearchBuilds call for patchset %d", ps)
		}
		server.RespondJSON(w, http.StatusOK, map[string]any{"builds": builds})
	})
	var fetched []string
	server.On("POST", "/prpc/buildbucket.v2.Builds/GetBuild", func(w http.ResponseWriter, r *http.Request) {
		var req bbGetBuildRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			t.Errorf("failed to decode GetBuild request: %v", err)
			server.RespondJSON(w, http.StatusBadRequest, map[string]any{})
			return
		}
		fetched = append(fetched, req.ID)
		server.RespondJSON(w, http.StatusOK, FakeBuildDetails(req.ID, "pigweed-linux", "SUCCESS"))
	})
	return server, &fetched
}

func TestRunView_JobFlag_StepTree_PrefersOlderSuccessOverCancel(t *testing.T) {
	_, fetched := newCanceledOverSuccessServer(t)
	output, err := executeCommand(RootCmd, "run", "view", "12345", "-j", "pigweed-linux")
	if err != nil {
		t.Fatalf("run view -j failed: %v\nOutput: %s", err, output)
	}
	if fmt.Sprint(*fetched) != "[901]" {
		t.Errorf("fetched builds %v, want [901] (the success pr checks reports, not canceled 1001)", *fetched)
	}
}

func TestRunView_Web_JobFlag_PrefersOlderSuccessOverCancel(t *testing.T) {
	newCanceledOverSuccessServer(t)
	var opened string
	origOpen := OpenBrowserFn
	defer func() { OpenBrowserFn = origOpen }()
	OpenBrowserFn = func(urlStr string) error {
		opened = urlStr
		return nil
	}

	output, err := executeCommand(RootCmd, "run", "view", "12345", "-j", "pigweed-linux", "-w")
	if err != nil {
		t.Fatalf("run view -j -w failed: %v\nOutput: %s", err, output)
	}
	if want := "https://ci.chromium.org/b/901"; opened != want {
		t.Errorf("opened %q, want %q", opened, want)
	}
}

func TestRunView_Verbose_IgnoresSupersededFailure(t *testing.T) {
	_, fetched := newCanceledOverSuccessServer(t)
	output, err := executeCommand(RootCmd, "run", "view", "12345", "-v")
	if err != nil {
		t.Fatalf("run view -v failed: %v\nOutput: %s", err, output)
	}
	if len(*fetched) != 1 || (*fetched)[0] == "902" || (*fetched)[0] == "1001" {
		t.Errorf("fetched builds %v; want one build that pr checks reports (901 or 1002), not superseded 902 or canceled 1001", *fetched)
	}
}

func TestRunRerun_UsesActualBuildProjectAndBucket(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345,
		WithSubject("Non-Profile Host Rerun"),
		WithProjectName("bar/baz"),
	)
	server.OnSearchBuilds(
		FakeBuild("777", "bar-bazel-test", "FAILURE",
			WithBuildProject("foo-ci"),
			WithBucket("try"),
		),
	)

	// Rerun --failed on a host that resolves to DefaultProfile (which has empty Project/Bucket).
	outFailed, err := executeCommand(RootCmd, "run", "rerun", "https://acme-review.googlesource.com/c/bar/baz/+/12345/1", "--failed", "--dry-run")
	if err != nil {
		t.Fatalf("run rerun --failed failed: %v\nOutput: %s", err, outFailed)
	}
	wantCmd := "bb add -cl https://acme-review.googlesource.com/c/bar/baz/+/12345/1 foo-ci/try/bar-bazel-test"
	if !strings.Contains(outFailed, wantCmd) {
		t.Errorf("Expected dry-run output to contain %q, got:\n%s", wantCmd, outFailed)
	}

	// Rerun with fully qualified -j <project>/<bucket>/<builder>.
	outQual, err := executeCommand(RootCmd, "run", "rerun", "https://acme-review.googlesource.com/c/bar/baz/+/12345/1", "-j", "foo-ci/try/bar-bazel-test", "--dry-run")
	if err != nil {
		t.Fatalf("run rerun -j foo-ci/try/bar-bazel-test failed: %v\nOutput: %s", err, outQual)
	}
	if !strings.Contains(outQual, wantCmd) {
		t.Errorf("Expected dry-run output to contain %q, got:\n%s", wantCmd, outQual)
	}

	// Rerun with two-part -j <bucket>/<builder>, matching against existing builds to resolve the project.
	outBucketBuilder, err := executeCommand(RootCmd, "run", "rerun", "https://acme-review.googlesource.com/c/bar/baz/+/12345/1", "-j", "try/bar-bazel-test", "--dry-run")
	if err != nil {
		t.Fatalf("run rerun -j try/bar-bazel-test failed: %v\nOutput: %s", err, outBucketBuilder)
	}
	if !strings.Contains(outBucketBuilder, wantCmd) {
		t.Errorf("Expected dry-run output to contain %q, got:\n%s", wantCmd, outBucketBuilder)
	}
}

func TestRunList_HideTagFiltersFromProjectConfig(t *testing.T) {
	tmpDir := t.TempDir()
	toml := `
[ci]
hide_tag_filters = ["hide-in-gerrit:subbuild"]
`
	if err := os.WriteFile(filepath.Join(tmpDir, ".ghish.toml"), []byte(toml), 0644); err != nil {
		t.Fatalf("failed to write .ghish.toml: %v", err)
	}

	server := NewMockGerritServer(t)
	MockCWD = tmpDir
	server.OnDefaultChange(12345, WithSubject("Run List Hide Subbuilds"))
	server.OnSearchBuilds(
		FakeBuild("100", "fuchsia-x64-orchestrator", "SUCCESS"),
		FakeBuild("101", "core.x64-subbuild", "SUCCESS", WithTags(bbTag{Key: "hide-in-gerrit", Value: "subbuild"})),
	)

	output, err := executeCommand(RootCmd, "run", "list", "12345")
	if err != nil {
		t.Fatalf("run list failed: %v\nOutput: %s", err, output)
	}
	if !strings.Contains(output, "fuchsia-x64-orchestrator") {
		t.Errorf("Expected orchestrator in output, got:\n%s", output)
	}
	if strings.Contains(output, "core.x64-subbuild") {
		t.Errorf("Expected subbuild to be hidden by hide_tag_filters, got:\n%s", output)
	}
	if !strings.Contains(output, "1") || !strings.Contains(output, "hidden") {
		t.Errorf("Expected hidden count note in output, got:\n%s", output)
	}
}

func TestRunRerun_Failed_SkipsSubbuildsWithSkipRetryTag(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Rerun Skip Subbuild"))
	server.OnSearchBuilds(
		FakeBuild("100", "fuchsia-x64-orchestrator", "FAILURE", WithBuildProject("fuchsia"), WithBucket("try")),
		FakeBuild("101", "core.x64-subbuild", "FAILURE",
			WithBuildProject("fuchsia"),
			WithBucket("ci"),
			WithTags(bbTag{Key: "skip-retry-in-gerrit", Value: "subbuild"}),
		),
	)

	output, err := executeCommand(RootCmd, "run", "rerun", "12345", "--failed", "--dry-run")
	if err != nil {
		t.Fatalf("run rerun --failed failed: %v\nOutput: %s", err, output)
	}
	if !strings.Contains(output, "fuchsia-x64-orchestrator") {
		t.Errorf("Expected parent orchestrator to be scheduled for rerun, got:\n%s", output)
	}
	if strings.Contains(output, "core.x64-subbuild") {
		t.Errorf("Expected child subbuild with skip-retry-in-gerrit:subbuild tag to be skipped, got:\n%s", output)
	}
}

func TestRunView_LogFailed_ParallelAndDeterministicOrder(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Parallel Failure Reports"))
	server.OnSearchBuilds(
		FakeBuild("101", "builder-alpha", "FAILURE"),
		FakeBuild("102", "builder-beta", "FAILURE"),
		FakeBuild("103", "builder-gamma", "INFRA_FAILURE"),
		FakeBuild("104", "builder-delta", "FAILURE"),
	)

	var inFlight atomic.Int32
	allArrived := make(chan struct{})
	var allOnce sync.Once

	detailsByID := map[string]*LUCIBuildDetails{
		"101": FakeBuildDetails("101", "builder-alpha", "FAILURE", WithBuildSummary("alpha failure")),
		"102": FakeBuildDetails("102", "builder-beta", "FAILURE", WithBuildSummary("beta failure")),
		"103": FakeBuildDetails("103", "builder-gamma", "INFRA_FAILURE", WithBuildSummary("gamma failure")),
		"104": FakeBuildDetails("104", "builder-delta", "FAILURE", WithBuildSummary("delta failure")),
	}

	server.On("POST", "/prpc/buildbucket.v2.Builds/GetBuild", func(w http.ResponseWriter, r *http.Request) {
		bodyBytes, _ := io.ReadAll(r.Body)
		var req struct {
			ID string `json:"id"`
		}
		_ = json.Unmarshal(bodyBytes, &req)

		if inFlight.Add(1) == 4 {
			allOnce.Do(func() { close(allArrived) })
		}
		timeout := time.NewTimer(2 * time.Second)
		defer timeout.Stop()
		select {
		case <-allArrived:
		case <-timeout.C:
			t.Errorf("expected all 4 GetBuild RPCs to run in parallel, only %d arrived concurrently", inFlight.Load())
		}
		server.RespondJSON(w, http.StatusOK, detailsByID[req.ID])
	})

	output, err := executeCommand(RootCmd, "run", "view", "12345", "--log-failed", "--json")
	if err != nil {
		t.Fatalf("run view --log-failed --json failed: %v\nOutput: %s", err, output)
	}

	var reports []FailureReport
	if err := json.Unmarshal([]byte(output), &reports); err != nil {
		t.Fatalf("failed to parse JSON reports: %v\nOutput: %s", err, output)
	}
	if len(reports) != 4 {
		t.Fatalf("expected 4 failure reports, got %d", len(reports))
	}
	wantBuilders := []string{"builder-alpha", "builder-beta", "builder-gamma", "builder-delta"}
	for i, want := range wantBuilders {
		if reports[i].Builder != want {
			t.Errorf("reports[%d].Builder = %q, want %q (deterministic order violated)", i, reports[i].Builder, want)
		}
	}
}

func TestRunView_Summary_CollapsesPassingChecksAndExpandsWithAll(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Run View Collapse Passing"))
	server.OnSearchBuilds(fakeTenPassOneFailBuilds()...)

	output, err := executeCommand(RootCmd, "run", "view", "12345")
	if err != nil {
		t.Fatalf("run view failed: %v\nOutput: %s", err, output)
	}
	if !strings.Contains(output, "fail-builder") {
		t.Errorf("Expected fail-builder in output, got:\n%s", output)
	}
	wantOmitted := "10 passing checks omitted (use --all to list)"
	if !strings.Contains(output, wantOmitted) {
		t.Errorf("Expected %q in output, got:\n%s", wantOmitted, output)
	}
	if strings.Contains(output, "pass-builder-01") {
		t.Errorf("Expected individual passing checks to be collapsed in default run view, got:\n%s", output)
	}

	allOutput, err := executeCommand(RootCmd, "run", "view", "12345", "--all")
	if err != nil {
		t.Fatalf("run view --all failed: %v\nOutput: %s", err, allOutput)
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

func TestRunView_Summary_AllGreenOmitsLogFailedFooter(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("All Green Run View"))
	server.OnSearchBuilds(
		FakeBuild("101", "builder-pass-1", "SUCCESS"),
		FakeBuild("102", "builder-pass-2", "SUCCESS"),
	)

	output, err := executeCommand(RootCmd, "run", "view", "12345")
	if err != nil {
		t.Fatalf("run view failed: %v\nOutput: %s", err, output)
	}
	if strings.Contains(output, "To view failed logs:") {
		t.Errorf("Expected 'To view failed logs:' footer to be omitted when all checks pass, got:\n%s", output)
	}
	if !strings.Contains(output, "To view step tree:     gh run view 12345 -j <builder>") {
		t.Errorf("Expected step tree footer in output, got:\n%s", output)
	}
}

func TestRunRerun_CompletionReceipt(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Rerun Receipt"), WithCurrentPatchset(4))
	server.OnSearchBuilds(
		FakeBuild("111", "pigweed-linux", "FAILURE", WithBucket("pigweed.try")),
	)

	restore := SetRerunExecForTesting(
		func(name string) (string, error) { return "/usr/bin/bb", nil },
		func(ctx context.Context, name string, args []string, stdout, stderr io.Writer) error { return nil },
	)
	defer restore()

	output, err := executeCommand(RootCmd, "run", "rerun", "12345", "--failed")
	if err != nil {
		t.Fatalf("run rerun --failed failed: %v\nOutput: %s", err, output)
	}
	wantReceipt := "✓ Triggered rerun for Change 12345 (Patchset 4)"
	if !strings.Contains(output, wantReceipt) {
		t.Errorf("Expected completion receipt %q in output, got:\n%s", wantReceipt, output)
	}
	if !strings.Contains(output, "Builders: 1 triggered") {
		t.Errorf("Expected 'Builders: 1 triggered' in rerun receipt, got:\n%s", output)
	}
	if !strings.Contains(output, "Next:     gh run watch 12345") {
		t.Errorf("Expected 'Next:     gh run watch 12345' in rerun receipt, got:\n%s", output)
	}

	dryOutput, err := executeCommand(RootCmd, "run", "rerun", "12345", "--failed", "--dry-run")
	if err != nil {
		t.Fatalf("run rerun --failed --dry-run failed: %v\nOutput: %s", err, dryOutput)
	}
	if strings.Contains(dryOutput, wantReceipt) {
		t.Errorf("Did not expect completion receipt on --dry-run, got:\n%s", dryOutput)
	}
}

func TestRunList_CollapsesPassingChecksAndPrintsVerdictAndNext(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Run List Collapse Passing"))
	server.OnSearchBuilds(fakeTenPassOneFailBuilds()...)

	output, err := executeCommand(RootCmd, "run", "list", "12345")
	if err != nil {
		t.Fatalf("run list failed: %v\nOutput: %s", err, output)
	}
	if !strings.Contains(output, "fail-builder") {
		t.Errorf("Expected fail-builder in run list output, got:\n%s", output)
	}
	wantOmitted := "✓  10 passing checks omitted (use --all to list)"
	if !strings.Contains(output, wantOmitted) {
		t.Errorf("Expected %q in run list output, got:\n%s", wantOmitted, output)
	}
	if strings.Contains(output, "pass-builder-01") {
		t.Errorf("Expected individual passing checks to be collapsed in default run list, got:\n%s", output)
	}
	if !strings.Contains(output, "Verdict: FAILED (1 of 11 checks failed)") {
		t.Errorf("Expected Verdict line in run list output, got:\n%s", output)
	}
	if !strings.Contains(output, "Next:    gh run view 12345 --log-failed  (or gh run rerun 12345 --failed)") {
		t.Errorf("Expected Next line in run list output, got:\n%s", output)
	}

	verboseOutput, err := executeCommand(RootCmd, "run", "list", "12345", "-v")
	if err != nil {
		t.Fatalf("run list -v failed: %v\nOutput: %s", err, verboseOutput)
	}
	if !strings.Contains(verboseOutput, "pass-builder-01") || !strings.Contains(verboseOutput, "pass-builder-10") {
		t.Errorf("Expected all passing checks in run list -v output, got:\n%s", verboseOutput)
	}
}

func TestWriteCIVerdictFooter(t *testing.T) {
	tests := []struct {
		name        string
		v           ciVerdict
		wantVerdict string
		wantNext    string
	}{
		{
			name:        "failed",
			v:           ciVerdict{total: 5, failed: 2, passed: 3},
			wantVerdict: "Verdict: FAILED (2 of 5 checks failed)",
			wantNext:    "Next:    gh run view 7 --log-failed  (or gh run rerun 7 --failed)",
		},
		{
			name:        "failed gates only",
			v:           ciVerdict{total: 2, passed: 2, failedGates: []string{"Lint (-1)", "Verified"}},
			wantVerdict: "Verdict: FAILED (Gerrit gates failed: Lint (-1), Verified)",
			wantNext:    "Next:    gh pr view 7 --comments",
		},
		{
			name:        "canceled",
			v:           ciVerdict{total: 4, canceled: 2, passed: 2},
			wantVerdict: "Verdict: CANCELED (2 of 4 checks canceled, likely superseded by a newer patchset)",
			wantNext:    "Next:    gh pr checks 7  (or gh pr edit 7 --cq)",
		},
		{
			name:        "running uses gh run watch by default",
			v:           ciVerdict{total: 3, running: 1, passed: 2},
			wantVerdict: "Verdict: PENDING (1 running, 2 passed)",
			wantNext:    "Next:    gh run watch 7",
		},
		{
			name:        "running honors pendingNext",
			v:           ciVerdict{total: 1, running: 1, pendingNext: "gh pr checks 7 --watch --fail-fast"},
			wantVerdict: "Verdict: PENDING (1 running, 0 passed)",
			wantNext:    "Next:    gh pr checks 7 --watch --fail-fast",
		},
		{
			name:        "only experimental builders",
			v:           ciVerdict{experimental: 2},
			wantVerdict: "Verdict: NO BLOCKING CHECKS (2 non-blocking experimental builders ran)",
			wantNext:    "Next:    gh pr checks 7 --experimental",
		},
		{
			name:        "no checks",
			v:           ciVerdict{},
			wantVerdict: "Verdict: NO CHECKS (CI not scheduled yet, or no tryjobs configured)",
			wantNext:    "Next:    gh pr edit 7 --cq",
		},
		{
			name:        "passed singular",
			v:           ciVerdict{total: 1, passed: 1},
			wantVerdict: "Verdict: PASSED (1 check passed)",
			wantNext:    "Next:    gh pr view 7",
		},
		{
			name:        "passed plural",
			v:           ciVerdict{total: 3, passed: 3},
			wantVerdict: "Verdict: PASSED (3 checks passed)",
			wantNext:    "Next:    gh pr view 7",
		},
		{
			name:        "failed beats gates, canceled, and running",
			v:           ciVerdict{total: 4, failed: 1, canceled: 1, running: 1, passed: 1, failedGates: []string{"Lint"}},
			wantVerdict: "Verdict: FAILED (1 of 4 checks failed)",
			wantNext:    "Next:    gh run view 7 --log-failed",
		},
		{
			name:        "gates beat canceled and running",
			v:           ciVerdict{total: 2, canceled: 1, running: 1, failedGates: []string{"Lint"}},
			wantVerdict: "Verdict: FAILED (Gerrit gates failed: Lint)",
		},
		{
			name:        "canceled beats running",
			v:           ciVerdict{total: 2, canceled: 1, running: 1},
			wantVerdict: "Verdict: CANCELED (1 of 2 checks canceled",
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			var sb strings.Builder
			writeCIVerdictFooter(&sb, 7, tt.v)
			out := sb.String()
			if !strings.HasPrefix(out, "\n") {
				t.Errorf("footer must start with a blank line, got %q", out)
			}
			if !strings.Contains(out, tt.wantVerdict) {
				t.Errorf("missing %q in:\n%s", tt.wantVerdict, out)
			}
			if tt.wantNext != "" && !strings.Contains(out, tt.wantNext) {
				t.Errorf("missing %q in:\n%s", tt.wantNext, out)
			}
			if n := strings.Count(out, "Verdict:"); n != 1 {
				t.Errorf("want exactly one Verdict: line, got %d:\n%s", n, out)
			}
			if n := strings.Count(out, "Next:"); n != 1 {
				t.Errorf("want exactly one Next: line, got %d:\n%s", n, out)
			}
		})
	}
}

func TestTallyCIVerdict_FailsClosedAndSkipsExperimental(t *testing.T) {
	builds := []bbBuild{
		FakeBuild("1", "ok", "SUCCESS"),
		FakeBuild("2", "weird", "WHO_KNOWS"),
		FakeBuild("3", "stopped", "CANCELED"),
		FakeBuild("4", "busy", "SCHEDULED"),
		FakeBuild("5", "exp", "FAILURE", WithCritical("NO")),
	}
	got := tallyCIVerdict(builds)
	want := ciVerdict{total: 4, failed: 1, canceled: 1, running: 1, passed: 1, experimental: 1}
	if got.total != want.total || got.failed != want.failed || got.canceled != want.canceled ||
		got.running != want.running || got.passed != want.passed || got.experimental != want.experimental {
		t.Errorf("tallyCIVerdict() = %+v, want %+v", got, want)
	}
}

func TestRunListAndView_AllPassingCollapsedAndExpandedWithAll(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("All Green Change"))
	var builds []bbBuild
	for i := 1; i <= 6; i++ {
		builds = append(builds, FakeBuild(fmt.Sprintf("%d", i), fmt.Sprintf("pass-builder-%d", i), "SUCCESS"))
	}
	server.OnSearchBuilds(builds...)

	listOut, err := executeCommand(RootCmd, "run", "list", "12345")
	if err != nil {
		t.Fatalf("run list failed: %v\nOutput: %s", err, listOut)
	}
	if !strings.Contains(listOut, "All 6 checks passed (use --all to list)") || strings.Contains(listOut, "STATUS") || strings.Contains(listOut, "pass-builder-1") {
		t.Errorf("run list collapsed = %q, want summary line without STATUS header or builder rows", listOut)
	}

	listAllOut, err := executeCommand(RootCmd, "run", "list", "12345", "--all")
	if err != nil {
		t.Fatalf("run list --all failed: %v\nOutput: %s", err, listAllOut)
	}
	if !strings.Contains(listAllOut, "pass-builder-1") || !strings.Contains(listAllOut, "pass-builder-6") {
		t.Errorf("run list --all = %q, want all 6 passing builders", listAllOut)
	}

	viewOut, err := executeCommand(RootCmd, "run", "view", "12345")
	if err != nil {
		t.Fatalf("run view failed: %v\nOutput: %s", err, viewOut)
	}
	if !strings.Contains(viewOut, "All 6 checks passed (use --all to list)") || strings.Contains(viewOut, "pass-builder-1") {
		t.Errorf("run view collapsed = %q, want collapsed all-passing summary without builder rows", viewOut)
	}
}
