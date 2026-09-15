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
	"net/http"
	"strings"
	"testing"
)

func TestDiffIntegration(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/patch", http.StatusOK, "\"SGVsbG8gRGlmZg==\"")

	output, err := executeCommand(RootCmd, "pr", "diff", "12345")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("GET", "/changes/12345/revisions/current/patch") != 1 {
		t.Error("Expected GetPatch API to be called")
	}

	if !strings.Contains(output, "Hello Diff") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestDiffIntegration_SpecificPatchset(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/2/patch", http.StatusOK, "\"UGF0Y2hzZXQgMiBEaWZm\"")

	output, err := executeCommand(RootCmd, "pr", "diff", "12345/2")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("GET", "/changes/12345/revisions/2/patch") != 1 {
		t.Error("Expected GetPatch API to be called for patchset 2")
	}

	if !strings.Contains(output, "Patchset 2 Diff") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestDiff_ErrorWhenNotFound(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnStatus(http.StatusNotFound)

	_, err := executeCommand(RootCmd, "pr", "diff", "99999")
	if err == nil {
		t.Fatal("Expected error when change is not found, got nil")
	}
	if !strings.Contains(err.Error(), "error getting patch") {
		t.Errorf("Expected error to mention 'error getting patch', got: %v", err)
	}
}

func TestDiff_ErrorOnMalformedPatch(t *testing.T) {
	server := NewMockGerritServer(t)
	// Not valid base64 and not a diff header
	server.OnJSON("GET", "/changes/12345/revisions/current/patch", http.StatusOK, "\"???not-base64-and-not-diff-header\"")

	_, err := executeCommand(RootCmd, "pr", "diff", "12345")
	if err == nil {
		t.Fatal("Expected error on malformed patch payload, got nil")
	}
	if !strings.Contains(err.Error(), "failed to decode patch diff") {
		t.Errorf("Expected error about failing to decode patch diff, got: %v", err)
	}
}

func TestDiff_RawDiffPassThrough(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/patch", http.StatusOK, "\"diff --git a/main.go b/main.go\\n+hello\"")

	out, err := executeCommand(RootCmd, "pr", "diff", "12345")
	if err != nil {
		t.Fatalf("Unexpected error for raw diff: %v", err)
	}
	if !strings.Contains(out, "diff --git a/main.go") {
		t.Errorf("Expected raw diff output, got: %q", out)
	}
}

func TestDiff_ErrorWhenPatchsetNotFound_SuggestsView(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnStatus(http.StatusNotFound)

	_, err := executeCommand(RootCmd, "pr", "diff", "12345/99")
	if err == nil {
		t.Fatal("Expected error when patchset not found, got nil")
	}
	if !strings.Contains(err.Error(), "patchset not found") {
		t.Errorf("Expected patchset not found explanation, got: %v", err)
	}
	if !strings.Contains(err.Error(), "gh pr view 12345") {
		t.Errorf("Expected suggestion to run 'gh pr view 12345', got: %v", err)
	}
}

func TestDiff_NameOnly(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/files*", http.StatusOK, map[string]any{
		"/COMMIT_MSG":      map[string]any{},
		"pw_ghish/diff.go": map[string]any{"lines_inserted": 10, "lines_deleted": 2},
		"pw_ghish/view.go": map[string]any{"lines_inserted": 5, "lines_deleted": 1},
	})

	output, err := executeCommand(RootCmd, "pr", "diff", "12345", "--name-only")
	if err != nil {
		t.Fatalf("pr diff --name-only failed: %v\nOutput: %s", err, output)
	}

	lines := strings.Split(strings.TrimSpace(output), "\n")
	if len(lines) != 2 {
		t.Fatalf("Expected 2 lines for 2 modified files, got %d:\n%s", len(lines), output)
	}
	if lines[0] != "pw_ghish/diff.go" || lines[1] != "pw_ghish/view.go" {
		t.Errorf("Expected sorted file paths, got: %v", lines)
	}
	if strings.Contains(output, "/COMMIT_MSG") {
		t.Errorf("Expected /COMMIT_MSG to be excluded from --name-only")
	}
}

func TestDiff_Stat(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/files*", http.StatusOK, map[string]any{
		"/COMMIT_MSG":      map[string]any{},
		"pw_ghish/diff.go": map[string]any{"lines_inserted": 10, "lines_deleted": 5},
	})

	output, err := executeCommand(RootCmd, "pr", "diff", "12345", "--stat")
	if err != nil {
		t.Fatalf("pr diff --stat failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "pw_ghish/diff.go") {
		t.Errorf("Expected diffstat to mention file, got:\n%s", output)
	}
	if !strings.Contains(output, "1 file changed, 10 insertions(+), 5 deletions(-)") {
		t.Errorf("Expected diffstat summary line, got:\n%s", output)
	}
}
