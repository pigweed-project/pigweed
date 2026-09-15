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
	"io"
	"net/http"
	"strings"
	"testing"

	"github.com/andygrunwald/go-gerrit"
)

var defaultStatusActiveChange = gerrit.ChangeInfo{
	ChangeID: "Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e",
	Number:   472267,
	Subject:  "Active Feature Subject",
	Branch:   "main",
	Status:   "NEW",
}

func newStatusMockServer(t *testing.T) *MockGerritServer {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/accounts/self", http.StatusOK, map[string]any{"_account_id": 1000000})
	return server
}

func mockGitForFeatureBranch(t *testing.T) *MockGitRunner {
	return SetMockGit(t, &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) >= 2 && args[0] == "branch" && args[1] == "--show-current" {
				stdout.Write([]byte("feature-branch\n"))
				return nil
			}
			if len(args) >= 3 && args[0] == "log" && args[1] == "-1" {
				stdout.Write([]byte("commit msg\n\nChange-Id: Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e\n"))
				return nil
			}
			return nil
		},
	})
}

func mockGitForMainBranch(t *testing.T) *MockGitRunner {
	return SetMockGit(t, &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) >= 2 && args[0] == "branch" && args[1] == "--show-current" {
				stdout.Write([]byte("main\n"))
				return nil
			}
			if len(args) >= 2 && args[0] == "rev-list" && args[1] == "--count" {
				stdout.Write([]byte("0\n"))
				return nil
			}
			return nil
		},
	})
}

func TestStatusIntegration(t *testing.T) {
	server := newStatusMockServer(t)
	server.OnJSON("GET", "/changes/*", http.StatusOK, []any{})

	output, err := executeCommand(RootCmd, "pr", "status")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	var hasChangesCall bool
	for _, req := range server.Requests() {
		if strings.HasPrefix(req.Path, "/changes/") {
			hasChangesCall = true
			break
		}
	}
	if !hasChangesCall {
		t.Error("Expected QueryChanges API to be called")
	}
}

func TestStatus_WritesToCmdOut(t *testing.T) {
	server := newStatusMockServer(t)
	server.OnJSON("GET", "/changes/*", http.StatusOK, []map[string]any{
		{"_number": 88888, "subject": "Status Change Subject", "status": "NEW", "owner": map[string]any{"name": "Alice"}},
	})

	var buf bytes.Buffer
	resetAllFlags(RootCmd)
	RootCmd.SetOut(&buf)
	RootCmd.SetErr(&buf)
	RootCmd.SetArgs([]string{"pr", "status"})

	if err := RootCmd.Execute(); err != nil {
		t.Fatalf("RootCmd.Execute() failed: %v", err)
	}

	if !strings.Contains(buf.String(), "88888") || !strings.Contains(buf.String(), "Status Change Subject") {
		t.Errorf("Expected buf to receive rendered output, got:\n%s", buf.String())
	}
}

func TestStatus_ErrorWhenSelfAccountFails(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnStatus(http.StatusUnauthorized)

	_, err := executeCommand(RootCmd, "pr", "status")
	if err == nil {
		t.Fatal("Expected error when GetAccount fails, got nil")
	}
	if !strings.Contains(err.Error(), "failed to get self account") {
		t.Errorf("Expected error mentioning self account, got: %v", err)
	}
}

func TestStatus_CurrentBranch_ActivePR(t *testing.T) {
	activeChange := gerrit.ChangeInfo{
		ChangeID: "Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e",
		Number:   472267,
		Subject:  "Active Feature Subject",
		Status:   "NEW",
		Branch:   "main",
		Labels: map[string]gerrit.LabelInfo{
			"Code-Review": {
				Approved: gerrit.AccountInfo{AccountID: 1000},
			},
			"Presubmit-Verified": {
				Approved: gerrit.AccountInfo{AccountID: 2000},
			},
			"Lint": {
				Recommended: gerrit.AccountInfo{AccountID: 3000},
			},
		},
	}

	server := newStatusMockServer(t)
	server.OnJSON("GET", "/changes/Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e*", http.StatusOK, activeChange)
	server.OnJSON("GET", "/changes/*", http.StatusOK, []any{})

	mockGitForFeatureBranch(t)

	out, err := executeCommand(RootCmd, "pr", "status")
	if err != nil {
		t.Fatalf("pr status failed: %v\nOutput: %s", err, out)
	}

	expectedStrings := []string{
		"Current branch",
		"#472267  Active Feature Subject",
		"Branch:      main",
		"Status:      NEW",
		"Code-Review: +2 (Approved)",
		"Presubmit-Verified: +2 (Approved)",
		"Lint: +1 (Recommended)",
	}
	for _, exp := range expectedStrings {
		if !strings.Contains(out, exp) {
			t.Errorf("Expected output to contain %q, but got:\n%s", exp, out)
		}
	}
}

func TestStatus_CurrentBranch_NoActivePR(t *testing.T) {
	server := newStatusMockServer(t)
	server.OnJSON("GET", "/changes/*", http.StatusOK, []any{})

	mockGitForMainBranch(t)

	out, err := executeCommand(RootCmd, "pr", "status")
	if err != nil {
		t.Fatalf("pr status failed: %v\nOutput: %s", err, out)
	}

	if !strings.Contains(out, "There is no pull request associated with the current branch") {
		t.Errorf("Expected output to say no PR associated with current branch, got:\n%s", out)
	}
	if !strings.Contains(out, "gh pr list") {
		t.Errorf("Expected guidance to run 'gh pr list', got:\n%s", out)
	}
	if !strings.Contains(out, "gh pr checkout <id>") {
		t.Errorf("Expected guidance to run 'gh pr checkout <id>', got:\n%s", out)
	}
	if !strings.Contains(out, "gh pr create") {
		t.Errorf("Expected guidance to run 'gh pr create', got:\n%s", out)
	}
}

func TestStatus_CurrentBranch_NamedBranchNoActivePR(t *testing.T) {
	server := newStatusMockServer(t)
	server.OnJSON("GET", "/changes/*", http.StatusOK, []any{})

	SetMockGit(t, &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) >= 2 && args[0] == "branch" && args[1] == "--show-current" {
				stdout.Write([]byte("my-feature-branch\n"))
				return nil
			}
			if len(args) >= 3 && args[0] == "log" && args[1] == "-1" {
				stdout.Write([]byte("commit without change-id\n"))
				return nil
			}
			return nil
		},
	})

	out, err := executeCommand(RootCmd, "pr", "status")
	if err != nil {
		t.Fatalf("pr status failed: %v\nOutput: %s", err, out)
	}

	if !strings.Contains(out, `There is no pull request associated with the current branch "my-feature-branch"`) {
		t.Errorf("Expected output to mention branch name in message, got:\n%s", out)
	}
	if !strings.Contains(out, "gh pr create") {
		t.Errorf("Expected guidance to run 'gh pr create', got:\n%s", out)
	}
}

func TestStatus_30DayFilter_And_MoreOlderChanges(t *testing.T) {
	var older []gerrit.ChangeInfo
	for i := 1; i <= 7; i++ {
		older = append(older, gerrit.ChangeInfo{Number: 10000 + i})
	}

	server := newStatusMockServer(t)
	server.On("GET", "/changes/*", func(w http.ResponseWriter, r *http.Request) {
		q := r.URL.Query().Get("q")
		if strings.Contains(q, "age:30d") && !strings.Contains(q, "-age:30d") {
			server.RespondJSON(w, http.StatusOK, older)
			return
		}
		server.RespondJSON(w, http.StatusOK, []any{})
	})

	mockGitForMainBranch(t)

	out, err := executeCommand(RootCmd, "pr", "status")
	if err != nil {
		t.Fatalf("pr status failed: %v\nOutput: %s", err, out)
	}

	hasAgeFilter := false
	for _, req := range server.Requests() {
		if strings.Contains(req.URL.Query().Get("q"), "-age:30d") {
			hasAgeFilter = true
			break
		}
	}
	if !hasAgeFilter {
		t.Errorf("Expected query to contain -age:30d")
	}

	if !strings.Contains(out, "... and 7 older changes (use -A / --all to show all)") {
		t.Errorf("Expected notice of 7 older changes, got:\n%s", out)
	}
}

func TestStatus_AllFlag_BypassesFilter(t *testing.T) {
	mockGitForMainBranch(t)
	server := newStatusMockServer(t)
	server.OnJSON("GET", "/changes/*", http.StatusOK, []any{})

	out, err := executeCommand(RootCmd, "pr", "status", "--all")
	if err != nil {
		t.Fatalf("pr status --all failed: %v\nOutput: %s", err, out)
	}

	for _, req := range server.Requests() {
		q := req.URL.Query().Get("q")
		if strings.Contains(q, "-age:30d") || strings.Contains(q, "age:30d") {
			t.Errorf("Expected --all to bypass age filters, but saw query: %s", q)
		}
	}

	if strings.Contains(out, "older changes") {
		t.Errorf("Expected no older changes notice with --all, got:\n%s", out)
	}
}

func TestStatus_CurrentBranch_WithComments_Unresolved(t *testing.T) {
	mockGitForFeatureBranch(t)
	server := newStatusMockServer(t)
	server.OnJSON("GET", "/changes/*/comments", http.StatusOK, map[string][]gerrit.CommentInfo{
		"pw_ghish/status.go": {
			{
				ID:         "c1",
				Line:       142,
				PatchSet:   14,
				Unresolved: boolPtr(true),
				Author:     gerrit.AccountInfo{Email: "reviewer@google.com"},
				Message:    "Consider sorting these keys so test output is deterministic.",
			},
		},
	})
	server.OnJSON("GET", "/changes/*/drafts", http.StatusOK, map[string]any{})
	server.OnJSON("GET", "/changes/Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e*", http.StatusOK, defaultStatusActiveChange)
	server.OnJSON("GET", "/changes/*", http.StatusOK, []any{})

	out, err := executeCommand(RootCmd, "pr", "status")
	if err != nil {
		t.Fatalf("pr status failed: %v\nOutput: %s", err, out)
	}

	expectedStrings := []string{
		"Comments:",
		"⚠ 1 unresolved thread:",
		"• pw_ghish/status.go:142 [PS14] by reviewer@google.com:",
		"\"Consider sorting these keys so test output is deterministic.\"",
	}
	for _, exp := range expectedStrings {
		if !strings.Contains(out, exp) {
			t.Errorf("Expected output to contain %q, but got:\n%s", exp, out)
		}
	}
}

func TestStatus_CurrentBranch_WithComments_Hysteresis(t *testing.T) {
	mockGitForFeatureBranch(t)
	server := newStatusMockServer(t)
	server.OnJSON("GET", "/changes/*/comments", http.StatusOK, map[string][]gerrit.CommentInfo{
		"file1.go": {
			{ID: "c1", Line: 10, PatchSet: 1, Unresolved: boolPtr(true), Author: gerrit.AccountInfo{Name: "Rev1"}, Message: "First comment"},
		},
		"file2.go": {
			{ID: "c2", Line: 20, PatchSet: 1, Unresolved: boolPtr(true), Author: gerrit.AccountInfo{Name: "Rev2"}, Message: "Second comment"},
		},
		"file3.go": {
			{ID: "c3", Line: 30, PatchSet: 1, Unresolved: boolPtr(true), Author: gerrit.AccountInfo{Name: "Rev3"}, Message: "Third comment"},
		},
	})
	server.OnJSON("GET", "/changes/*/drafts", http.StatusOK, map[string]any{})
	server.OnJSON("GET", "/changes/Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e*", http.StatusOK, defaultStatusActiveChange)
	server.OnJSON("GET", "/changes/*", http.StatusOK, []any{})

	out, err := executeCommand(RootCmd, "pr", "status")
	if err != nil {
		t.Fatalf("pr status failed: %v\nOutput: %s", err, out)
	}

	expectedStrings := []string{
		"Comments:",
		"⚠ 3 unresolved threads (use 'gh pr view --comments' to view all):",
		"file1.go:10",
		"file2.go:20",
		"... and 1 more unresolved thread",
	}
	for _, exp := range expectedStrings {
		if !strings.Contains(out, exp) {
			t.Errorf("Expected output to contain %q, but got:\n%s", exp, out)
		}
	}
	if strings.Contains(out, "file3.go:30") {
		t.Errorf("Expected file3.go to be truncated by hysteresis, but found it in output:\n%s", out)
	}
}

func TestStatus_CurrentBranch_WithDrafts(t *testing.T) {
	mockGitForFeatureBranch(t)
	server := newStatusMockServer(t)
	server.OnJSON("GET", "/changes/*/comments", http.StatusOK, map[string][]gerrit.CommentInfo{
		"pw_ghish/status.go": {
			{
				ID:         "c1",
				Line:       142,
				PatchSet:   14,
				Unresolved: boolPtr(true),
				Author:     gerrit.AccountInfo{Email: "reviewer@google.com"},
				Message:    "Please fix this.",
			},
		},
	})
	server.OnJSON("GET", "/changes/*/drafts", http.StatusOK, map[string][]gerrit.CommentInfo{
		"pw_ghish/status.go": {
			{
				ID:        "d1",
				InReplyTo: "c1",
				Message:   "I am working on it right now.",
			},
		},
	})
	server.OnJSON("GET", "/changes/Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e*", http.StatusOK, defaultStatusActiveChange)
	server.OnJSON("GET", "/changes/*", http.StatusOK, []any{})

	out, err := executeCommand(RootCmd, "pr", "status")
	if err != nil {
		t.Fatalf("pr status failed: %v\nOutput: %s", err, out)
	}

	expectedStrings := []string{
		"⚠ 1 unresolved thread (1 unpublished draft):",
		"(has unpublished draft reply)",
	}
	for _, exp := range expectedStrings {
		if !strings.Contains(out, exp) {
			t.Errorf("Expected output to contain %q, but got:\n%s", exp, out)
		}
	}
}

func TestStatus_CurrentBranch_CommentsJSON(t *testing.T) {
	mockGitForFeatureBranch(t)
	server := newStatusMockServer(t)
	server.OnJSON("GET", "/changes/*/comments", http.StatusOK, map[string][]gerrit.CommentInfo{
		"pw_ghish/status.go": {
			{
				ID:         "c1",
				Line:       142,
				PatchSet:   14,
				Unresolved: boolPtr(true),
				Author:     gerrit.AccountInfo{Email: "reviewer@google.com"},
				Message:    "Please check this.",
			},
		},
	})
	server.OnJSON("GET", "/changes/*/drafts", http.StatusOK, map[string]any{})
	server.OnJSON("GET", "/changes/Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e*", http.StatusOK, defaultStatusActiveChange)
	server.OnJSON("GET", "/changes/*", http.StatusOK, []any{})

	out, err := executeCommand(RootCmd, "pr", "status", "--json", "current_branch")
	if err != nil {
		t.Fatalf("pr status --json failed: %v\nOutput: %s", err, out)
	}

	var res struct {
		CurrentBranch struct {
			Comments CommentsSummary `json:"comments"`
		} `json:"current_branch"`
	}
	if err := json.Unmarshal([]byte(out), &res); err != nil {
		t.Fatalf("Failed to parse JSON output: %v\nOutput: %s", err, out)
	}

	if res.CurrentBranch.Comments.TotalThreads != 1 || res.CurrentBranch.Comments.UnresolvedThreads != 1 {
		t.Errorf("Unexpected comments summary in JSON: %+v", res.CurrentBranch.Comments)
	}
	if len(res.CurrentBranch.Comments.Unresolved) != 1 {
		t.Fatalf("Expected 1 unresolved comment in JSON, got %d", len(res.CurrentBranch.Comments.Unresolved))
	}
	if res.CurrentBranch.Comments.Unresolved[0].File != "pw_ghish/status.go" || res.CurrentBranch.Comments.Unresolved[0].Line != 142 {
		t.Errorf("Unexpected unresolved comment item in JSON: %+v", res.CurrentBranch.Comments.Unresolved[0])
	}
}

func TestStatus_CurrentBranch_CommentsFailureGraceful(t *testing.T) {
	mockGitForFeatureBranch(t)
	server := newStatusMockServer(t)
	server.On("GET", "/changes/*/comments", func(w http.ResponseWriter, r *http.Request) {
		http.Error(w, "internal server error", http.StatusInternalServerError)
	})
	server.On("GET", "/changes/*/drafts", func(w http.ResponseWriter, r *http.Request) {
		http.Error(w, "forbidden", http.StatusForbidden)
	})
	server.OnJSON("GET", "/changes/Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e*", http.StatusOK, defaultStatusActiveChange)
	server.OnJSON("GET", "/changes/*", http.StatusOK, []any{})

	out, err := executeCommand(RootCmd, "pr", "status")
	if err != nil {
		t.Fatalf("pr status failed even though comments error was expected to degrade gracefully: %v\nOutput: %s", err, out)
	}

	if !strings.Contains(out, "Active Feature Subject") {
		t.Errorf("Expected active feature subject in output despite comments failure, got:\n%s", out)
	}
}

func TestStatus_CurrentBranch_ChecksSummary_Passing(t *testing.T) {
	activeChange := gerrit.ChangeInfo{
		ChangeID:        "Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e",
		Number:          472267,
		Project:         "pigweed/pigweed",
		Subject:         "Active Feature Subject",
		Status:          "NEW",
		Branch:          "main",
		CurrentRevision: "rev1",
		Revisions: map[string]gerrit.RevisionInfo{
			"rev1": {Number: 1},
		},
	}

	server := newStatusMockServer(t)
	server.OnJSON("GET", "/changes/Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e*", http.StatusOK, activeChange)
	server.OnJSON("GET", "/changes/*", http.StatusOK, []any{})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "1",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "try",
					"builder": "builder-1",
				},
				"status": "SUCCESS",
			},
			{
				"id": "2",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "try",
					"builder": "builder-2",
				},
				"status": "SUCCESS",
			},
		},
	})

	mockGitForFeatureBranch(t)

	out, err := executeCommand(RootCmd, "pr", "status", "--buildbucket-host", server.URL)
	if err != nil {
		t.Fatalf("pr status failed: %v\nOutput: %s", err, out)
	}

	if !strings.Contains(out, "Checks:      ✓ 2 passing") {
		t.Errorf("Expected 'Checks:      ✓ 2 passing' in status output, got:\n%s", out)
	}
	if strings.Contains(out, "https://ci.chromium.org/b/") {
		t.Errorf("Expected no verbose URL table in status output, got:\n%s", out)
	}
}

func TestStatus_CurrentBranch_ChecksSummary_Failing(t *testing.T) {
	activeChange := gerrit.ChangeInfo{
		ChangeID:        "Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e",
		Number:          472267,
		Project:         "pigweed/pigweed",
		Subject:         "Active Feature Subject",
		Status:          "NEW",
		Branch:          "main",
		CurrentRevision: "rev1",
		Revisions: map[string]gerrit.RevisionInfo{
			"rev1": {Number: 1},
		},
	}

	server := newStatusMockServer(t)
	server.OnJSON("GET", "/changes/Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e*", http.StatusOK, activeChange)
	server.OnJSON("GET", "/changes/*", http.StatusOK, []any{})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "1",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "try",
					"builder": "pigweed-lint",
				},
				"status": "FAILURE",
			},
			{
				"id": "2",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "try",
					"builder": "builder-2",
				},
				"status": "SUCCESS",
			},
		},
	})

	mockGitForFeatureBranch(t)

	out, err := executeCommand(RootCmd, "pr", "status", "--buildbucket-host", server.URL)
	if err != nil {
		t.Fatalf("pr status failed: %v\nOutput: %s", err, out)
	}

	if !strings.Contains(out, "Checks:      ✖ 1 failed: pigweed-lint (run 'gh run view --log-failed' to view errors)") {
		t.Errorf("Expected failure summary in status output, got:\n%s", out)
	}
}

func TestStatus_CurrentBranch_ChecksSummary_JSON(t *testing.T) {
	activeChange := gerrit.ChangeInfo{
		ChangeID:        "Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e",
		Number:          472267,
		Project:         "pigweed/pigweed",
		Subject:         "Active Feature Subject",
		Status:          "NEW",
		Branch:          "main",
		CurrentRevision: "rev1",
		Revisions: map[string]gerrit.RevisionInfo{
			"rev1": {Number: 1},
		},
	}

	server := newStatusMockServer(t)
	server.OnJSON("GET", "/changes/Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e*", http.StatusOK, activeChange)
	server.OnJSON("GET", "/changes/*", http.StatusOK, []any{})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "1",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "try",
					"builder": "builder-1",
				},
				"status": "SUCCESS",
			},
		},
	})

	mockGitForFeatureBranch(t)

	out, err := executeCommand(RootCmd, "pr", "status", "--buildbucket-host", server.URL, "--json", "current_branch")
	if err != nil {
		t.Fatalf("pr status --json failed: %v\nOutput: %s", err, out)
	}

	var res struct {
		CurrentBranch struct {
			ChecksSummary string      `json:"checks_summary"`
			Checks        []CheckItem `json:"checks"`
		} `json:"current_branch"`
	}
	if err := json.Unmarshal([]byte(out), &res); err != nil {
		t.Fatalf("Failed to parse JSON: %v\nOutput: %s", err, out)
	}

	if res.CurrentBranch.ChecksSummary != "✓ 1 passing" {
		t.Errorf("Expected checks_summary '✓ 1 passing', got %q", res.CurrentBranch.ChecksSummary)
	}
	if len(res.CurrentBranch.Checks) != 1 {
		t.Errorf("Expected 1 item in checks array, got %d", len(res.CurrentBranch.Checks))
	}
}

func TestExtractLabelScore(t *testing.T) {
	tests := []struct {
		name      string
		labels    map[string]gerrit.LabelInfo
		labelName string
		want      int
	}{
		{
			name:      "nil map",
			labels:    nil,
			labelName: "Code-Review",
			want:      0,
		},
		{
			name:      "missing label",
			labels:    map[string]gerrit.LabelInfo{},
			labelName: "Code-Review",
			want:      0,
		},
		{
			name: "highest positive score wins",
			labels: map[string]gerrit.LabelInfo{
				"Code-Review": {
					All: []gerrit.ApprovalInfo{
						{Value: 1},
						{Value: 2},
						{Value: 1},
					},
				},
			},
			labelName: "Code-Review",
			want:      2,
		},
		{
			name: "negative score overrides positive scores",
			labels: map[string]gerrit.LabelInfo{
				"Code-Review": {
					All: []gerrit.ApprovalInfo{
						{Value: 2},
						{Value: -1},
						{Value: 1},
					},
				},
			},
			labelName: "Code-Review",
			want:      -1,
		},
		{
			name: "lowest negative score wins",
			labels: map[string]gerrit.LabelInfo{
				"Code-Review": {
					All: []gerrit.ApprovalInfo{
						{Value: -1},
						{Value: -2},
						{Value: 2},
					},
				},
			},
			labelName: "Code-Review",
			want:      -2,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := extractLabelScore(tt.labels, tt.labelName); got != tt.want {
				t.Errorf("extractLabelScore() = %d, want %d", got, tt.want)
			}
		})
	}
}

func TestExtractBlockers(t *testing.T) {
	tests := []struct {
		name   string
		change *gerrit.ChangeInfo
		want   []string
	}{
		{
			name:   "submittable change has no blockers",
			change: &gerrit.ChangeInfo{Submittable: true},
			want:   nil,
		},
		{
			name: "missing Code-Review and Verified approvals",
			change: &gerrit.ChangeInfo{
				Submittable: false,
				Labels: map[string]gerrit.LabelInfo{
					"Code-Review": {},
					"Verified":    {},
				},
			},
			want: []string{"Code-Review (+2 required)", "Verified (+1 required)"},
		},
		{
			name: "rejected label reported",
			change: &gerrit.ChangeInfo{
				Submittable: false,
				Labels: map[string]gerrit.LabelInfo{
					"Code-Review": {Approved: gerrit.AccountInfo{AccountID: 1}},
					"Lint":        {Rejected: gerrit.AccountInfo{AccountID: 2}},
				},
			},
			want: []string{"Lint (Rejected)"},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := extractBlockers(tt.change)
			if len(got) != len(tt.want) {
				t.Fatalf("extractBlockers() = %v, want %v", got, tt.want)
			}
			for i := range got {
				if got[i] != tt.want[i] {
					t.Errorf("extractBlockers()[%d] = %q, want %q", i, got[i], tt.want[i])
				}
			}
		})
	}
}
