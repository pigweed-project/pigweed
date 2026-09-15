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
