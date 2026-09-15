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
	"encoding/json"
	"net/http"
	"strings"
	"testing"
)

func TestRunList(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"id":               "pigweed~main~I12345",
		"project":          "pigweed/pigweed",
		"branch":           "main",
		"_number":          12345,
		"subject":          "Test Run List",
		"status":           "NEW",
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{"_number": 1},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "111",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "try",
					"builder": "builder-pass",
				},
				"status":    "SUCCESS",
				"startTime": "2026-09-08T10:00:00Z",
				"endTime":   "2026-09-08T10:02:00Z",
			},
			{
				"id": "222",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "try",
					"builder": "builder-fail",
				},
				"status":    "FAILURE",
				"startTime": "2026-09-08T10:00:00Z",
				"endTime":   "2026-09-08T10:01:00Z",
			},
		},
	})

	output, err := executeCommand(RootCmd, "run", "list", "12345", "--buildbucket-host", server.URL)
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
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"id":               "pigweed~main~I12345",
		"project":          "pigweed/pigweed",
		"branch":           "main",
		"_number":          12345,
		"subject":          "Test Run View Summary",
		"status":           "NEW",
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{"_number": 1},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "111",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "try",
					"builder": "builder-pass",
				},
				"status":    "SUCCESS",
				"startTime": "2026-09-08T10:00:00Z",
				"endTime":   "2026-09-08T10:02:00Z",
			},
			{
				"id": "222",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "try",
					"builder": "builder-fail",
				},
				"status":    "FAILURE",
				"startTime": "2026-09-08T10:00:00Z",
				"endTime":   "2026-09-08T10:01:00Z",
			},
		},
	})

	output, err := executeCommand(RootCmd, "run", "view", "12345", "--buildbucket-host", server.URL)
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
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"id":               "pigweed~main~I12345",
		"project":          "pigweed/pigweed",
		"branch":           "main",
		"_number":          12345,
		"subject":          "Test Run View Summary JSON",
		"status":           "NEW",
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{"_number": 1},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "111",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "try",
					"builder": "builder-pass",
				},
				"status": "SUCCESS",
			},
		},
	})

	output, err := executeCommand(RootCmd, "run", "view", "12345", "--json", "--buildbucket-host", server.URL)
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
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"id":               "pigweed~main~I12345",
		"project":          "pigweed/pigweed",
		"branch":           "main",
		"_number":          12345,
		"subject":          "Test Run View Log Failed",
		"status":           "NEW",
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{"_number": 1},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "222",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "try",
					"builder": "pigweed-lint",
				},
				"status": "FAILURE",
			},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/GetBuild", http.StatusOK, map[string]any{
		"id": "222",
		"builder": map[string]any{
			"project": "pigweed",
			"bucket":  "try",
			"builder": "pigweed-lint",
		},
		"status":          "FAILURE",
		"summaryMarkdown": "flake in linter",
		"steps": []map[string]any{
			{"name": "lint_check", "status": "FAILURE"},
		},
	})

	output, err := executeCommand(RootCmd, "run", "view", "12345", "--log-failed", "--buildbucket-host", server.URL)
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
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"id":               "pigweed~main~I12345",
		"project":          "pigweed/pigweed",
		"branch":           "main",
		"_number":          12345,
		"subject":          "All Passing Change",
		"status":           "NEW",
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{"_number": 1},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "111",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "try",
					"builder": "pigweed-build",
				},
				"status": "SUCCESS",
			},
		},
	})

	output, err := executeCommand(RootCmd, "run", "view", "12345", "--log-failed", "--buildbucket-host", server.URL)
	if err != nil {
		t.Fatalf("run view --log-failed failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "No failed checks found on this change.") {
		t.Errorf("Expected 'No failed checks found on this change.', got:\n%s", output)
	}
}

func TestRunView_VerboseSteps(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"id":               "pigweed~main~I12345",
		"project":          "pigweed/pigweed",
		"branch":           "main",
		"_number":          12345,
		"subject":          "Steps Change",
		"status":           "NEW",
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{"_number": 1},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "111",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "try",
					"builder": "pigweed-compile",
				},
				"status": "STARTED",
			},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/GetBuild", http.StatusOK, map[string]any{
		"id": "111",
		"builder": map[string]any{
			"project": "pigweed",
			"bucket":  "try",
			"builder": "pigweed-compile",
		},
		"status": "STARTED",
		"steps": []map[string]any{
			{"name": "setup", "status": "SUCCESS"},
			{"name": "compile", "status": "STARTED"},
		},
	})

	output, err := executeCommand(RootCmd, "run", "view", "12345", "pigweed-compile", "-v", "--buildbucket-host", server.URL)
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
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"id":               "pigweed~main~I12345",
		"project":          "pigweed/pigweed",
		"branch":           "main",
		"_number":          12345,
		"subject":          "Job Flag Change",
		"status":           "NEW",
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{"_number": 1},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "111",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "try",
					"builder": "pigweed-compile",
				},
				"status": "FAILURE",
			},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/GetBuild", http.StatusOK, map[string]any{
		"id": "111",
		"builder": map[string]any{
			"project": "pigweed",
			"bucket":  "try",
			"builder": "pigweed-compile",
		},
		"status":          "FAILURE",
		"summaryMarkdown": "compiler error",
		"steps":           []any{},
	})

	output, err := executeCommand(RootCmd, "run", "view", "12345", "-j", "pigweed-compile", "--log-failed", "--buildbucket-host", server.URL)
	if err != nil {
		t.Fatalf("run view -j failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "FAILURE: pigweed-compile (Build 111)") {
		t.Errorf("Expected failure report for pigweed-compile, got:\n%s", output)
	}
}

func TestRunView_JobFlag_ShowsStepsByDefault(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"id":               "pigweed~main~I12345",
		"project":          "pigweed/pigweed",
		"branch":           "main",
		"_number":          12345,
		"subject":          "Job Flag Steps Default",
		"status":           "NEW",
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{"_number": 1},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "111",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "try",
					"builder": "pigweed-compile",
				},
				"status": "SUCCESS",
			},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/GetBuild", http.StatusOK, map[string]any{
		"id": "111",
		"builder": map[string]any{
			"project": "pigweed",
			"bucket":  "try",
			"builder": "pigweed-compile",
		},
		"status": "SUCCESS",
		"steps": []map[string]any{
			{"name": "setup", "status": "SUCCESS"},
			{"name": "build", "status": "SUCCESS"},
		},
	})

	output, err := executeCommand(RootCmd, "run", "view", "12345", "-j", "pigweed-compile", "--buildbucket-host", server.URL)
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
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"id":               "pigweed~main~I12345",
		"project":          "pigweed/pigweed",
		"branch":           "main",
		"_number":          12345,
		"subject":          "Web Run View",
		"status":           "NEW",
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{"_number": 1},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "8671017385591991697",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "try",
					"builder": "static-checks-pigweed",
				},
				"status": "SUCCESS",
			},
		},
	})

	var openedURL string
	oldOpenBrowser := OpenBrowserFn
	defer func() { OpenBrowserFn = oldOpenBrowser }()
	OpenBrowserFn = func(urlStr string) error {
		openedURL = urlStr
		return nil
	}

	output, err := executeCommand(RootCmd, "run", "view", "12345", "static-checks-pigweed", "-w", "--buildbucket-host", server.URL)
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
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"id":               "pigweed~main~I12345",
		"project":          "pigweed/pigweed",
		"branch":           "main",
		"_number":          12345,
		"subject":          "Rerun Change",
		"status":           "NEW",
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{"_number": 1},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "111",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "pigweed.try",
					"builder": "pigweed-linux",
				},
				"status": "FAILURE",
			},
		},
	})

	output, err := executeCommand(RootCmd, "run", "rerun", "12345", "--failed", "--dry-run", "--buildbucket-host", server.URL)
	if err != nil {
		t.Fatalf("run rerun --failed failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "[dry-run]") || !strings.Contains(output, "pigweed-linux") {
		t.Errorf("Expected dry-run rerun for pigweed-linux, got:\n%s", output)
	}
}

func TestRunRerun_Job(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"id":               "pigweed~main~I12345",
		"project":          "pigweed/pigweed",
		"branch":           "main",
		"_number":          12345,
		"subject":          "Rerun Job Change",
		"status":           "NEW",
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{"_number": 1},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "111",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "pigweed.try",
					"builder": "pigweed-linux",
				},
				"status": "FAILURE",
			},
		},
	})

	output, err := executeCommand(RootCmd, "run", "rerun", "12345", "-j", "pigweed-linux", "--dry-run", "--buildbucket-host", server.URL)
	if err != nil {
		t.Fatalf("run rerun -j failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "[dry-run]") || !strings.Contains(output, "pigweed-linux") {
		t.Errorf("Expected dry-run rerun for pigweed-linux, got:\n%s", output)
	}
}

func TestRunRerun_MissingBuilderError(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"id":               "pigweed~main~I12345",
		"project":          "pigweed/pigweed",
		"branch":           "main",
		"_number":          12345,
		"subject":          "Rerun Missing Error Change",
		"status":           "NEW",
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{"_number": 1},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "111",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "pigweed.try",
					"builder": "pigweed-linux",
				},
				"status": "FAILURE",
			},
		},
	})

	_, err := executeCommand(RootCmd, "run", "rerun", "12345", "--buildbucket-host", server.URL)
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
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/GetBuild", http.StatusOK, map[string]any{
		"id": "8671182706745774001",
		"builder": map[string]any{
			"project": "pigweed",
			"bucket":  "try",
			"builder": "pigweed-lint",
		},
		"status":          "FAILURE",
		"summaryMarkdown": "lint check failed",
		"steps":           []any{},
	})

	output, err := executeCommand(RootCmd, "run", "view", "8671182706745774001", "-v", "--buildbucket-host", server.URL)
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
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"id":               "pigweed~main~I12345",
		"project":          "pigweed/pigweed",
		"branch":           "main",
		"_number":          12345,
		"subject":          "Builder Not Found Change",
		"status":           "NEW",
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{"_number": 1},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "111",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "try",
					"builder": "pigweed-linux",
				},
				"status": "SUCCESS",
			},
		},
	})

	_, err := executeCommand(RootCmd, "run", "view", "12345", "-v", "-j", "nonexistent", "--buildbucket-host", server.URL)
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
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"id":               "pigweed~main~I12345",
		"project":          "pigweed/pigweed",
		"branch":           "main",
		"_number":          12345,
		"subject":          "Rerun Not Found Change",
		"status":           "NEW",
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{"_number": 1},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "111",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "try",
					"builder": "pigweed-linux",
				},
				"status": "SUCCESS",
			},
		},
	})

	_, err := executeCommand(RootCmd, "run", "rerun", "12345", "-j", "nonexistent", "--buildbucket-host", server.URL)
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
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"id":               "pigweed~main~I12345",
		"project":          "pigweed/pigweed",
		"branch":           "main",
		"_number":          12345,
		"subject":          "Run List JSON",
		"status":           "NEW",
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{"_number": 1},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "111",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "try",
					"builder": "pigweed-linux",
				},
				"status": "SUCCESS",
			},
		},
	})

	output, err := executeCommand(RootCmd, "run", "list", "12345", "--json", "--buildbucket-host", server.URL)
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
