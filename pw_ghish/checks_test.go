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
	"sort"
	"strings"
	"testing"
)

func TestQueryBuildbucket_Success(t *testing.T) {
	ctx := context.Background()

	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "8680709829694997521",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "pigweed.try",
					"builder": "static-checks-pigweed",
				},
				"status":          "SUCCESS",
				"startTime":       "2026-05-26T20:23:35.545752392Z",
				"endTime":         "2026-05-26T20:23:51.610863977Z",
				"summaryMarkdown": "All checks passed",
			},
			{
				"id": "8680709829694997522",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "pigweed.try",
					"builder": "pigweed-linux",
				},
				"status":          "FAILURE",
				"startTime":       "2026-05-26T20:23:35.000000000Z",
				"endTime":         "2026-05-26T20:24:35.000000000Z",
				"summaryMarkdown": "Build compilation failed",
			},
		},
	})

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
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"id":               "pigweed~main~I12345",
		"project":          "pigweed/pigweed",
		"branch":           "main",
		"_number":          12345,
		"subject":          "Test Commit",
		"status":           "NEW",
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{
				"_number": 2,
			},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "123456789",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "pigweed.try",
					"builder": "pigweed-linux-clang",
				},
				"status":    "SUCCESS",
				"startTime": "2026-05-26T20:00:00Z",
				"endTime":   "2026-05-26T20:01:30Z",
			},
		},
	})

	output, err := executeCommand(RootCmd, "pr", "checks", "12345", "--buildbucket-host", server.URL)
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
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"id":               "pigweed~main~I12345",
		"project":          "pigweed/pigweed",
		"branch":           "main",
		"_number":          12345,
		"subject":          "Test Commit",
		"status":           "NEW",
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{"_number": 2},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "123456789",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "pigweed.try",
					"builder": "docs-builder",
				},
				"status":          "SUCCESS",
				"startTime":       "2026-05-26T20:00:00Z",
				"endTime":         "2026-05-26T20:01:30Z",
				"summaryMarkdown": "Docs:\n* [permalink](https://example.com/docs)\n**Experiments**:\n* foo",
			},
		},
	})

	output, err := executeCommand(RootCmd, "pr", "checks", "12345", "--buildbucket-host", server.URL)
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
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"project":          "pigweed/pigweed",
		"_number":          12345,
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{
				"_number": 1,
			},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "123456789",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "pigweed.try",
					"builder": "pigweed-linux-clang",
				},
				"status": "SUCCESS",
			},
		},
	})

	output, err := executeCommand(RootCmd, "pr", "checks", "12345", "--buildbucket-host", server.URL, "--json", "number,checks")
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
	server.OnJSON("GET", "/changes/*", http.StatusOK, map[string]any{
		"project":          "pigweed/pigweed",
		"_number":          472267,
		"current_revision": "rev13",
		"revisions": map[string]any{
			"rev13": map[string]any{
				"_number": 13,
			},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "8680709829694997521",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "pigweed.try",
					"builder": "static-checks-pigweed",
				},
				"status":    "SUCCESS",
				"startTime": "2026-05-26T20:23:35.000Z",
				"endTime":   "2026-05-26T20:24:01.000Z",
			},
		},
	})

	mockGit := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) >= 2 && args[0] == "branch" && args[1] == "--show-current" {
				stdout.Write([]byte("ghish\n"))
				return nil
			}
			if len(args) >= 3 && args[0] == "log" && args[1] == "-1" {
				stdout.Write([]byte("commit msg\n\nChange-Id: Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e\n"))
				return nil
			}
			return nil
		},
	}
	SetMockGit(t, mockGit)

	out, err := executeCommand(RootCmd, "pr", "checks", "--buildbucket-host", server.URL)
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

	_, err := executeCommand(RootCmd, "pr", "checks", "99999", "--buildbucket-host", server.URL)
	if err == nil {
		t.Fatal("Expected error for non-existent change")
	}
	if !strings.Contains(err.Error(), `failed to load checks for "99999"`) {
		t.Errorf("Expected wrapped error message mentioning 'failed to load checks for \"99999\"', got: %v", err)
	}
}

func TestChecks_DefaultFiltersOutExperimentalBuilders(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"project":          "pigweed/pigweed",
		"_number":          12345,
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{
				"_number": 1,
			},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "1",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "pigweed.try",
					"builder": "prod-linux",
				},
				"status": "SUCCESS",
			},
			{
				"id": "2",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "pigweed.try",
					"builder": "prod-mac",
				},
				"status": "STARTED",
			},
			{
				"id": "3",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "pigweed.try",
					"builder": "exp-v9",
				},
				"status":   "FAILURE",
				"critical": "NO",
			},
			{
				"id": "4",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "pigweed.try",
					"builder": "exp-vscode",
				},
				"status": "FAILURE",
				"tags": []map[string]any{
					{"key": "cq_experimental", "value": "true"},
				},
			},
		},
	})

	output, err := executeCommand(RootCmd, "pr", "checks", "12345", "--buildbucket-host", server.URL)
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
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"project":          "pigweed/pigweed",
		"_number":          12345,
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{
				"_number": 1,
			},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "1",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "pigweed.try",
					"builder": "prod-linux",
				},
				"status": "SUCCESS",
			},
			{
				"id": "2",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "pigweed.try",
					"builder": "exp-v9",
				},
				"status":   "FAILURE",
				"critical": "NO",
			},
		},
	})

	// Test with full flag --experimental
	output, err := executeCommand(RootCmd, "pr", "checks", "12345", "--experimental", "--buildbucket-host", server.URL)
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
	outputShort, err := executeCommand(RootCmd, "pr", "checks", "12345", "-e", "--buildbucket-host", server.URL)
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
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"project":          "pigweed/pigweed",
		"_number":          12345,
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{
				"_number": 1,
			},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "1",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "pigweed.try",
					"builder": "prod-linux",
				},
				"status": "SUCCESS",
			},
			{
				"id": "2",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "pigweed.try",
					"builder": "exp-v9",
				},
				"status":   "FAILURE",
				"critical": "NO",
			},
		},
	})

	output, err := executeCommand(RootCmd, "pr", "checks", "12345", "--buildbucket-host", server.URL)
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
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"project":          "pigweed/pigweed",
		"_number":          12345,
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{
				"_number": 1,
			},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "1",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "pigweed.try",
					"builder": "prod-linux",
				},
				"status": "SUCCESS",
			},
		},
	})

	output, err := executeCommand(RootCmd, "pr", "checks", "12345", "--buildbucket-host", server.URL)
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if strings.Contains(output, "omitted") {
		t.Errorf("Expected no omitted notice, got:\n%s", output)
	}
}

func TestChecks_JSON_FiltersUnlessExperimental(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"project":          "pigweed/pigweed",
		"_number":          12345,
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{
				"_number": 1,
			},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "1",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "pigweed.try",
					"builder": "prod-linux",
				},
				"status": "SUCCESS",
			},
			{
				"id": "2",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "pigweed.try",
					"builder": "exp-v9",
				},
				"status":   "FAILURE",
				"critical": "NO",
			},
		},
	})

	// Default: only prod checks
	outDefault, err := executeCommand(RootCmd, "pr", "checks", "12345", "--json", "checks", "--buildbucket-host", server.URL)
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
	outExp, err := executeCommand(RootCmd, "pr", "checks", "12345", "--experimental", "--json", "checks", "--buildbucket-host", server.URL)
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
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"project":          "pigweed/pigweed",
		"_number":          12345,
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{
				"_number": 1,
			},
		},
	})

	pollCount := 0
	server.On("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", func(w http.ResponseWriter, r *http.Request) {
		pollCount++
		status := "STARTED"
		if pollCount >= 2 {
			status = "SUCCESS"
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]any{
			"builds": []map[string]any{
				{
					"id": "100",
					"builder": map[string]any{
						"project": "pigweed",
						"bucket":  "pigweed.try",
						"builder": "pigweed-linux",
					},
					"status":    status,
					"startTime": "2026-05-26T20:00:00Z",
				},
			},
		})
	})

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
	if pollCount < 2 {
		t.Errorf("Expected at least 2 polls, got %d", pollCount)
	}
}

func TestChecks_Watch_FailFast(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"project":          "pigweed/pigweed",
		"_number":          12345,
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{
				"_number": 1,
			},
		},
	})

	pollCount := 0
	server.On("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", func(w http.ResponseWriter, r *http.Request) {
		pollCount++
		failStatus := "STARTED"
		if pollCount >= 2 {
			failStatus = "FAILURE"
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]any{
			"builds": []map[string]any{
				{
					"id": "100",
					"builder": map[string]any{
						"project": "pigweed",
						"bucket":  "pigweed.try",
						"builder": "pigweed-linux",
					},
					"status": "STARTED",
				},
				{
					"id": "200",
					"builder": map[string]any{
						"project": "pigweed",
						"bucket":  "pigweed.try",
						"builder": "static-checks",
					},
					"status": failStatus,
				},
			},
		})
	})

	server.On("POST", "/prpc/buildbucket.v2.Builds/GetBuild", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]any{
			"id": "200",
			"builder": map[string]any{
				"project": "pigweed",
				"bucket":  "pigweed.try",
				"builder": "static-checks",
			},
			"status":          "FAILURE",
			"summaryMarkdown": "python formatting check failed",
			"steps":           []any{},
		})
	})

	output, err := executeCommand(RootCmd, "pr", "checks", "12345", "--watch", "--fail-fast", "--interval", "5ms", "--buildbucket-host", server.URL)
	if err == nil {
		t.Fatalf("Expected command to fail on check failure, but succeeded. Output:\n%s", output)
	}

	if !strings.Contains(output, "static-checks") {
		t.Errorf("Expected output to contain static-checks, got:\n%s", output)
	}
	if !strings.Contains(output, "FAILURE: static-checks (Build 200)") {
		t.Errorf("Expected auto failure report in output, got:\n%s", output)
	}
	if !strings.Contains(output, "python formatting check failed") {
		t.Errorf("Expected failure summary in output, got:\n%s", output)
	}
	if pollCount != 2 {
		t.Errorf("Expected fail-fast to stop at poll 2, got pollCount=%d", pollCount)
	}
}

func TestChecks_Watch_FailFast_IgnoresExperimental(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"project":          "pigweed/pigweed",
		"_number":          12345,
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{
				"_number": 1,
			},
		},
	})

	pollCount := 0
	server.On("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", func(w http.ResponseWriter, r *http.Request) {
		pollCount++
		prodStatus := "STARTED"
		if pollCount >= 2 {
			prodStatus = "SUCCESS"
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]any{
			"builds": []map[string]any{
				{
					"id": "100",
					"builder": map[string]any{
						"project": "pigweed",
						"bucket":  "pigweed.try",
						"builder": "pigweed-linux",
					},
					"status": prodStatus,
				},
				{
					"id": "200",
					"builder": map[string]any{
						"project": "pigweed",
						"bucket":  "pigweed.try",
						"builder": "exp-builder",
					},
					"status":   "FAILURE",
					"critical": "NO",
				},
			},
		})
	})

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
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"project":          "pigweed/pigweed",
		"_number":          12345,
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{
				"_number": 1,
			},
		},
	})

	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "100",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "pigweed.try",
					"builder": "pigweed-linux",
				},
				"status": "FAILURE",
			},
		},
	})

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
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"project":          "pigweed/pigweed",
		"_number":          12345,
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{
				"_number": 1,
			},
		},
	})

	pollCount := 0
	server.On("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", func(w http.ResponseWriter, r *http.Request) {
		pollCount++
		s1 := "STARTED"
		s2 := "STARTED"
		if pollCount >= 2 {
			s1 = "SUCCESS"
			s2 = "FAILURE"
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]any{
			"builds": []map[string]any{
				{
					"id": "100",
					"builder": map[string]any{
						"project": "pigweed",
						"bucket":  "pigweed.try",
						"builder": "prod-1",
					},
					"status": s1,
				},
				{
					"id": "200",
					"builder": map[string]any{
						"project": "pigweed",
						"bucket":  "pigweed.try",
						"builder": "prod-2",
					},
					"status": s2,
				},
			},
		})
	})

	server.On("POST", "/prpc/buildbucket.v2.Builds/GetBuild", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]any{
			"id": "200",
			"builder": map[string]any{
				"project": "pigweed",
				"bucket":  "pigweed.try",
				"builder": "prod-2",
			},
			"status":          "FAILURE",
			"summaryMarkdown": "compiler error",
			"steps":           []any{},
		})
	})

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
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"project":          "pigweed/pigweed",
		"_number":          12345,
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{
				"_number": 1,
			},
		},
	})

	pollCount := 0
	server.On("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", func(w http.ResponseWriter, r *http.Request) {
		pollCount++
		retryStatus := "STARTED"
		if pollCount >= 2 {
			retryStatus = "SUCCESS"
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]any{
			"builds": []map[string]any{
				{
					"id": "200", // New build (retry)
					"builder": map[string]any{
						"project": "pigweed",
						"bucket":  "pigweed.try",
						"builder": "pigweed-linux",
					},
					"status": retryStatus,
				},
				{
					"id": "100", // Old build (initial failure)
					"builder": map[string]any{
						"project": "pigweed",
						"bucket":  "pigweed.try",
						"builder": "pigweed-linux",
					},
					"status": "FAILURE",
				},
			},
		})
	})

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
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"project":          "pigweed/pigweed",
		"_number":          12345,
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{
				"_number": 1,
			},
		},
	})

	pollCount := 0
	server.On("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", func(w http.ResponseWriter, r *http.Request) {
		pollCount++
		s1 := "STARTED"
		s2 := "STARTED"
		if pollCount >= 2 {
			s1 = "SUCCESS"
		}
		if pollCount >= 3 {
			s2 = "SUCCESS"
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]any{
			"builds": []map[string]any{
				{
					"id": "1",
					"builder": map[string]any{
						"project": "pigweed",
						"bucket":  "pigweed.try",
						"builder": "builder-one",
					},
					"status":    s1,
					"startTime": "2026-05-26T20:00:00Z",
					"endTime":   "2026-05-26T20:01:00Z",
				},
				{
					"id": "2",
					"builder": map[string]any{
						"project": "pigweed",
						"bucket":  "pigweed.try",
						"builder": "builder-two",
					},
					"status":    s2,
					"startTime": "2026-05-26T20:00:00Z",
					"endTime":   "2026-05-26T20:02:00Z",
				},
			},
		})
	})

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
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"project":          "pigweed/pigweed",
		"_number":          12345,
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{
				"_number": 1,
			},
		},
	})

	pollCount := 0
	server.On("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", func(w http.ResponseWriter, r *http.Request) {
		pollCount++
		status := "STARTED"
		if pollCount >= 6 {
			status = "SUCCESS"
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]any{
			"builds": []map[string]any{
				{
					"id": "1",
					"builder": map[string]any{
						"project": "pigweed",
						"bucket":  "pigweed.try",
						"builder": "long-builder",
					},
					"status":    status,
					"startTime": "2026-05-26T20:00:00Z",
				},
			},
		})
	})

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
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"project":          "pigweed/pigweed",
		"_number":          12345,
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{
				"_number": 1,
			},
		},
	})

	pollCount := 0
	server.On("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", func(w http.ResponseWriter, r *http.Request) {
		pollCount++
		status := "STARTED"
		if pollCount >= 2 {
			status = "FAILURE"
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]any{
			"builds": []map[string]any{
				{
					"id": "1",
					"builder": map[string]any{
						"project": "pigweed",
						"bucket":  "pigweed.try",
						"builder": "failing-builder",
					},
					"status":    status,
					"startTime": "2026-05-26T20:00:00Z",
					"endTime":   "2026-05-26T20:01:30Z",
				},
			},
		})
	})

	server.On("POST", "/prpc/buildbucket.v2.Builds/GetBuild", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]any{
			"id": "1",
			"builder": map[string]any{
				"project": "pigweed",
				"bucket":  "pigweed.try",
				"builder": "failing-builder",
			},
			"status":          "FAILURE",
			"summaryMarkdown": "test failed: connection refused",
			"steps":           []any{},
		})
	})

	output, err := executeCommand(RootCmd, "pr", "checks", "12345", "--watch", "--fail-fast", "--interval", "5ms", "--buildbucket-host", server.URL)
	if err == nil {
		t.Fatalf("Expected error when fail-fast triggered, got nil. Output:\n%s", output)
	}

	if !strings.Contains(output, "✗  failing-builder failed (1m30s) [1/1]") {
		t.Errorf("Expected failure breadcrumb, got:\n%s", output)
	}
	if !strings.Contains(output, "--fail-fast triggered: stopping watch.") {
		t.Errorf("Expected fail-fast notification, got:\n%s", output)
	}
	if !strings.Contains(output, "FAILURE: failing-builder (Build 1)") {
		t.Errorf("Expected failure diagnostics, got:\n%s", output)
	}
}

func TestChecks_Watch_InitialWaitingToScheduled(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"project":          "pigweed/pigweed",
		"_number":          12345,
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{
				"_number": 1,
			},
		},
	})

	pollCount := 0
	server.On("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", func(w http.ResponseWriter, r *http.Request) {
		pollCount++
		var builds []map[string]any
		if pollCount == 2 {
			builds = []map[string]any{
				{
					"id": "1",
					"builder": map[string]any{
						"project": "pigweed",
						"bucket":  "pigweed.try",
						"builder": "builder-one",
					},
					"status": "STARTED",
				},
			}
		} else if pollCount >= 3 {
			builds = []map[string]any{
				{
					"id": "1",
					"builder": map[string]any{
						"project": "pigweed",
						"bucket":  "pigweed.try",
						"builder": "builder-one",
					},
					"status": "SUCCESS",
				},
			}
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]any{
			"builds": builds,
		})
	})

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
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"project":          "pigweed/pigweed",
		"_number":          12345,
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{
				"_number": 1,
			},
		},
	})

	pollCount := 0
	server.On("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", func(w http.ResponseWriter, r *http.Request) {
		pollCount++
		var builds []map[string]any

		if pollCount == 1 {
			// Initial state: builder-one is already passed, builder-two and builder-three are running
			builds = []map[string]any{
				{
					"id": "1",
					"builder": map[string]any{
						"project": "pigweed",
						"bucket":  "try",
						"builder": "builder-one",
					},
					"status":    "SUCCESS",
					"startTime": "2026-09-08T10:00:00Z",
					"endTime":   "2026-09-08T10:01:00Z",
				},
				{
					"id": "2",
					"builder": map[string]any{
						"project": "pigweed",
						"bucket":  "try",
						"builder": "builder-two",
					},
					"status":    "STARTED",
					"startTime": "2026-09-08T10:00:00Z",
				},
				{
					"id": "3",
					"builder": map[string]any{
						"project": "pigweed",
						"bucket":  "try",
						"builder": "builder-three",
					},
					"status":    "STARTED",
					"startTime": "2026-09-08T10:00:00Z",
				},
			}
		} else {
			// Subsequent state: all passed
			builds = []map[string]any{
				{
					"id": "1",
					"builder": map[string]any{
						"project": "pigweed",
						"bucket":  "try",
						"builder": "builder-one",
					},
					"status":    "SUCCESS",
					"startTime": "2026-09-08T10:00:00Z",
					"endTime":   "2026-09-08T10:01:00Z",
				},
				{
					"id": "2",
					"builder": map[string]any{
						"project": "pigweed",
						"bucket":  "try",
						"builder": "builder-two",
					},
					"status":    "SUCCESS",
					"startTime": "2026-09-08T10:00:00Z",
					"endTime":   "2026-09-08T10:02:00Z",
				},
				{
					"id": "3",
					"builder": map[string]any{
						"project": "pigweed",
						"bucket":  "try",
						"builder": "builder-three",
					},
					"status":    "SUCCESS",
					"startTime": "2026-09-08T10:00:00Z",
					"endTime":   "2026-09-08T10:03:00Z",
				},
			}
		}

		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]any{
			"builds": builds,
		})
	})

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
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"project":          "pigweed/pigweed",
		"_number":          12345,
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{
				"_number": 1,
			},
		},
	})

	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "1",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "try",
					"builder": "builder-fail",
				},
				"status":    "FAILURE",
				"startTime": "2026-09-08T10:00:00Z",
				"endTime":   "2026-09-08T10:01:00Z",
			},
			{
				"id": "2",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "try",
					"builder": "builder-pass",
				},
				"status":    "SUCCESS",
				"startTime": "2026-09-08T10:00:00Z",
				"endTime":   "2026-09-08T10:02:00Z",
			},
		},
	})

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
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"project":          "pigweed/pigweed",
		"_number":          12345,
		"subject":          "Check Web Test",
		"status":           "NEW",
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{
				"_number": 2,
			},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{},
	})

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
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := builderList(tt.builds); got != tt.want {
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
func exitCodeChecksServer(t *testing.T, statuses map[string]string, experimental map[string]bool) *MockGerritServer {
	t.Helper()

	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"project":          "pigweed/pigweed",
		"_number":          12345,
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{"_number": 1},
		},
	})

	builds := []map[string]any{}
	id := 0
	// Sort for a deterministic response order.
	names := make([]string, 0, len(statuses))
	for name := range statuses {
		names = append(names, name)
	}
	sort.Strings(names)
	for _, name := range names {
		id++
		build := map[string]any{
			"id": fmt.Sprintf("%d", id),
			"builder": map[string]any{
				"project": "pigweed",
				"bucket":  "pigweed.try",
				"builder": name,
			},
			"status": statuses[name],
		}
		if experimental[name] {
			build["critical"] = "NO"
		}
		builds = append(builds, build)
	}

	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": builds,
	})
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
		wantCode     int
		wantMessage  string
		wantNotInMsg string
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
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			server := exitCodeChecksServer(t, tt.statuses, tt.experimental)

			args := append([]string{"pr", "checks", "12345", "--buildbucket-host", server.URL}, tt.extraArgs...)
			output, err := executeCommand(RootCmd, args...)

			if got := ExitCodeFor(err); got != tt.wantCode {
				t.Fatalf("exit code = %d, want %d\nerr = %v\nOutput:\n%s", got, tt.wantCode, err, output)
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
		{"json", []string{"--json", "number,checks"}},
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

	// Default client:
	client := getLUCIHTTPClient(ctx, "cr-buildbucket.appspot.com")
	if client == nil {
		t.Fatal("expected non-nil client")
	}
	if client != http.DefaultClient {
		t.Errorf("expected http.DefaultClient, got %v", client)
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
