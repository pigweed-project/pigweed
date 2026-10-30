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
	"encoding/base64"
	"fmt"
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

func TestDiff_EmptyPatchPrintsNotice(t *testing.T) {
	for _, tc := range []struct {
		name  string
		setup func(*MockGerritServer)
	}{
		{
			name: "empty raw body",
			setup: func(s *MockGerritServer) {
				s.OnString("GET", "/changes/12345/revisions/current/patch", http.StatusOK, "text/plain", "")
			},
		},
		{
			name: "empty JSON string",
			setup: func(s *MockGerritServer) {
				s.OnJSON("GET", "/changes/12345/revisions/current/patch", http.StatusOK, "")
			},
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			server := NewMockGerritServer(t)
			tc.setup(server)

			out, err := executeCommand(RootCmd, "pr", "diff", "12345")
			if err != nil {
				t.Fatalf("unexpected error for empty patch: %v", err)
			}
			if strings.TrimSpace(out) != "No patch returned." {
				t.Errorf("output = %q, want %q", strings.TrimSpace(out), "No patch returned.")
			}
		})
	}
}

func TestDiff_ErrorWhenPatchsetNotFound_SuggestsView(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnStatus(http.StatusNotFound)

	for _, args := range [][]string{
		{"pr", "diff", "12345/99"},
		{"pr", "diff", "12345/99", "--name-only"},
		{"pr", "diff", "12345/99", "--stat"},
	} {
		t.Run(strings.Join(args, " "), func(t *testing.T) {
			_, err := executeCommand(RootCmd, args...)
			if err == nil {
				t.Fatalf("Expected error when patchset not found for %v, got nil", args)
			}
			if !strings.Contains(err.Error(), "patchset not found") {
				t.Errorf("Expected patchset not found explanation, got: %v", err)
			}
			if !strings.Contains(err.Error(), "gh pr view 12345") {
				t.Errorf("Expected suggestion to run 'gh pr view 12345', got: %v", err)
			}
		})
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

func TestDiff_FoldsLongPatchAndHonorsAll(t *testing.T) {
	preamble := "From 0123456789abcdef Mon Sep 17 00:00:00 2001\nSubject: [PATCH] pw_ghish: Budget diff output\n\nChange-Id: I1234567890abcdef\n---"
	var sections []string
	sections = append(sections, preamble)

	// 1. A generated lockfile section (50 lines)
	var lockLines []string
	lockLines = append(lockLines, "diff --git a/MODULE.bazel.lock b/MODULE.bazel.lock", "--- a/MODULE.bazel.lock", "+++ b/MODULE.bazel.lock")
	for i := 0; i < 47; i++ {
		lockLines = append(lockLines, fmt.Sprintf("+lock_line_%d", i))
	}
	sections = append(sections, strings.Join(lockLines, "\n"))

	// 2. First regular file (250 lines) -> fits within 400-line budget
	var file1Lines []string
	file1Lines = append(file1Lines, "diff --git a/pw_ghish/first.go b/pw_ghish/first.go", "--- a/pw_ghish/first.go", "+++ b/pw_ghish/first.go")
	for i := 0; i < 247; i++ {
		file1Lines = append(file1Lines, fmt.Sprintf("+first_file_line_%d", i))
	}
	sections = append(sections, strings.Join(file1Lines, "\n"))

	// 3. Second regular file (250 lines) -> pushes total over 400 lines, should be omitted by default
	var file2Lines []string
	file2Lines = append(file2Lines, "diff --git a/pw_ghish/second.go b/pw_ghish/second.go", "--- a/pw_ghish/second.go", "+++ b/pw_ghish/second.go")
	for i := 0; i < 247; i++ {
		file2Lines = append(file2Lines, fmt.Sprintf("+second_file_line_%d", i))
	}
	sections = append(sections, strings.Join(file2Lines, "\n"))

	fullPatch := strings.Join(sections, "\n")
	encodedPatch := base64.StdEncoding.EncodeToString([]byte(fullPatch))

	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/patch", http.StatusOK, fmt.Sprintf("%q", encodedPatch))

	t.Run("default diff preserves preamble, folds lockfile, and truncates at 400-line budget", func(t *testing.T) {
		output, err := executeCommand(RootCmd, "pr", "diff", "12345")
		if err != nil {
			t.Fatalf("pr diff failed: %v\nOutput: %s", err, output)
		}
		if !strings.Contains(output, "Subject: [PATCH] pw_ghish: Budget diff output") {
			t.Errorf("expected commit preamble to be preserved before Files changed header, got:\n%s", output)
		}
		if !strings.Contains(output, "Files changed: 3 (555 diff lines)") {
			t.Errorf("expected Files changed header, got:\n%s", output)
		}
		if !strings.Contains(output, "# [folded generated/lockfile MODULE.bazel.lock (50 lines) — use --all or --path MODULE.bazel.lock to view]") {
			t.Errorf("expected folded lockfile notice, got:\n%s", output)
		}
		if !strings.Contains(output, "+first_file_line_0") {
			t.Errorf("expected first.go to be included in full, got:\n%s", output)
		}
		if strings.Contains(output, "+second_file_line_0") {
			t.Errorf("expected second.go to be omitted due to 400-line budget, got:\n%s", output)
		}
		wantFooter := "… 1 more file(s) (250 lines omitted) · run 'gh pr diff 12345 --all' or 'gh pr diff 12345 --path <file>' to view full patch (or 'gh pr checkout 12345' / 'gh pr cherry-pick 12345')"
		if !strings.Contains(output, wantFooter) {
			t.Errorf("expected unfold footer %q, got:\n%s", wantFooter, output)
		}
	})

	t.Run("--all outputs full untruncated diff", func(t *testing.T) {
		output, err := executeCommand(RootCmd, "pr", "diff", "12345", "--all")
		if err != nil {
			t.Fatalf("pr diff --all failed: %v\nOutput: %s", err, output)
		}
		for _, want := range []string{"+lock_line_0", "+first_file_line_0", "+second_file_line_0"} {
			if !strings.Contains(output, want) {
				t.Errorf("expected %q in --all output, got:\n%s", want, output)
			}
		}
	})
}
