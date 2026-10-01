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
	"strings"
	"testing"
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
	if !strings.Contains(output, "To view failed logs:") {
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
	if !strings.Contains(output, "✓  setup") || !strings.Contains(output, "*  compile") {
		t.Errorf("Expected step items, got:\n%s", output)
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

// TestRunView_JobFlag_LogFailed_IgnoresSupersededFailure verifies that
// `run view -j <builder> --log-failed` reports the newest build of the
// builder -- the one `pr checks` reports -- rather than a failure on an older
// code-equivalent patchset that a later build already superseded.
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

	if strings.Contains(output, "802") {
		t.Errorf("reported superseded patchset 8 failure (build 802), got:\n%s", output)
	}
	if !strings.Contains(output, "901") {
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

// checkReusedBuildNotes verifies that the line mentioning build 801 is labeled
// with patchset 8 and the line mentioning build 901 carries no label.
func checkReusedBuildNotes(t *testing.T, output string) {
	t.Helper()
	var saw801, saw901 bool
	for _, line := range strings.Split(output, "\n") {
		if strings.Contains(line, "801") {
			saw801 = true
			if !strings.Contains(line, "(from patchset 8)") {
				t.Errorf("expected build 801 line to contain %q, got: %q", "(from patchset 8)", line)
			}
		}
		if strings.Contains(line, "901") {
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
