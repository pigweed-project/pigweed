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
	"slices"
	"strings"
	"sync"
	"testing"
	"time"
)

const statusChangeID = "Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e"

func newStatusMockServer(t *testing.T) *MockGerritServer {
	return NewMockGerritServer(t).OnAccountSelf(1000000)
}

// onActiveChange serves the current branch's change without leaking it
// into the owner/reviewer queries.
func onActiveChange(s *MockGerritServer, opts ...func(map[string]any)) *MockGerritServer {
	opts = append([]func(map[string]any){WithChangeID(statusChangeID), WithSubject("Active Feature Subject")}, opts...)
	return s.OnDefaultChange(472267, opts...).OnJSON("GET", "/changes/", http.StatusOK, []any{})
}

func mockGitForFeatureBranch(t *testing.T) *MockGitRunner {
	return NewMockGit(t).
		WithBranch("feature-branch").
		WithCommit("commit msg\n\nChange-Id: " + statusChangeID + "\n")
}

func mockGitForMainBranch(t *testing.T) *MockGitRunner {
	return NewMockGit(t).
		WithBranch("main").
		WithCommit("commit without change-id\n").
		OnCommand("rev-list --count", "0\n")
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

func TestStatus_UnauthenticatedSkipsSelfAccount(t *testing.T) {
	t.Run("main branch makes zero Gerrit requests and reports unauthenticated state", func(t *testing.T) {
		server := NewMockGerritServer(t).UseResolvedAuth()
		mockGitForMainBranch(t)

		out, err := executeCommand(RootCmd, "pr", "status", "--auth-mode=none")
		if err != nil {
			t.Fatalf("pr status failed: %v\nOutput: %s", err, out)
		}
		if reqs := server.Requests(); len(reqs) != 0 {
			t.Errorf("unauthenticated pr status on main made %d requests (%v), want 0", len(reqs), reqs)
		}
		for _, want := range []string{
			"not signed in",
			"Next: gh auth status",
		} {
			if !strings.Contains(out, want) {
				t.Errorf("expected output to contain %q, got:\n%s", want, out)
			}
		}
		if strings.Contains(out, "Created by you") {
			t.Errorf("expected unauthenticated output to omit empty personal sections, got:\n%s", out)
		}

		jsonOut, err := executeCommand(RootCmd, "pr", "status", "--auth-mode=none", "--json", "authenticated")
		if err != nil {
			t.Fatalf("pr status --json authenticated failed: %v\nOutput: %s", err, jsonOut)
		}
		var parsed struct {
			Authenticated *bool `json:"authenticated"`
		}
		if err := json.Unmarshal([]byte(jsonOut), &parsed); err != nil {
			t.Fatalf("failed to decode JSON %q: %v", jsonOut, err)
		}
		if parsed.Authenticated == nil || *parsed.Authenticated {
			t.Errorf("authenticated = %v, want false (raw: %s)", parsed.Authenticated, jsonOut)
		}
	})

	t.Run("active PR branch renders public comments and metadata while skipping drafts", func(t *testing.T) {
		const changeID = "Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e"
		server := NewMockGerritServer(t).UseResolvedAuth()
		server.OnDefaultChange(472267, WithSubject("Unauthed Active Subject"), WithChangeID(changeID))
		server.OnComments(changeID, map[string][]CommentInfo{
			"foo.cc": {
				{
					ID:         "c1",
					Line:       12,
					PatchSet:   1,
					Unresolved: boolPtr(true),
					Message:    "Please add a guard",
				},
			},
		})
		mockGitForFeatureBranch(t)

		out, err := executeCommand(RootCmd, "pr", "status", "--auth-mode=none")
		if err != nil {
			t.Fatalf("pr status --auth-mode=none failed: %v\nOutput: %s", err, out)
		}
		for _, want := range []string{
			"#472267  Unauthed Active Subject",
			"1 unresolved",
			"not signed in",
			"Next: gh auth status",
		} {
			if !strings.Contains(out, want) {
				t.Errorf("expected output to contain %q, got:\n%s", want, out)
			}
		}
		if got := server.CallCount("GET", "/accounts/self"); got != 0 {
			t.Errorf("expected 0 /accounts/self requests, got %d", got)
		}
		if got := server.CallCount("GET", "/changes/"+changeID+"/drafts"); got != 0 {
			t.Errorf("expected 0 /drafts requests when unauthenticated, got %d", got)
		}
		if got := server.CallCount("GET", "/changes/"+changeID+"/comments"); got != 1 {
			t.Errorf("expected 1 /comments request, got %d", got)
		}
	})
}

func TestStatus_CurrentBranch_ActivePR(t *testing.T) {
	server := newStatusMockServer(t)
	onActiveChange(server, func(ch map[string]any) {
		// Ranges mirror the real Pigweed labels: Code-Review and Lint
		// span -2..+2, but Presubmit-Verified tops out at +1.
		ch["labels"] = map[string]LabelInfo{
			"Code-Review":        narrowLabel(-2, 2, 2),
			"Presubmit-Verified": narrowLabel(-1, 1, 1),
			"Lint":               narrowLabel(-2, 2, 1),
		}
	})

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
		"Presubmit-Verified: +1 (Approved)",
		"Lint: +1 (Recommended)",
		"Next: gh pr view <id>  (or gh pr checks)",
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
	NewMockGit(t).WithBranch("my-feature-branch").WithCommit("commit without change-id\n")

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
	var older []ChangeInfo
	for i := 1; i <= 7; i++ {
		older = append(older, ChangeInfo{Number: 10000 + i})
	}

	server := newStatusMockServer(t)
	server.On("GET", "/changes/*", func(w http.ResponseWriter, r *http.Request) {
		qs := r.URL.Query()["q"]
		opts := r.URL.Query()["o"]
		if slices.ContainsFunc(qs, func(q string) bool { return strings.Contains(q, "-age:30d") }) {
			wantOpts := []string{"DETAILED_LABELS", "SUBMITTABLE", "DETAILED_ACCOUNTS", "SUBMIT_REQUIREMENTS"}
			if !slices.Equal(opts, wantOpts) {
				t.Errorf("recent changes batch o=%v, want %v", opts, wantOpts)
			}
		} else if slices.ContainsFunc(qs, func(q string) bool { return strings.Contains(q, "age:30d") }) {
			if len(opts) != 0 {
				t.Errorf("older count-only batch should have no o= params, got %v", opts)
			}
		}
		batches := make([][]ChangeInfo, len(qs))
		for i, query := range qs {
			if strings.Contains(query, "age:30d") && !strings.Contains(query, "-age:30d") {
				batches[i] = older
			} else {
				batches[i] = []ChangeInfo{}
			}
		}
		server.RespondJSON(w, http.StatusOK, batches)
	})

	mockGitForMainBranch(t)

	out, err := executeCommand(RootCmd, "pr", "status")
	if err != nil {
		t.Fatalf("pr status failed: %v\nOutput: %s", err, out)
	}

	hasAgeFilter := slices.ContainsFunc(server.Requests(), func(req MockRequest) bool {
		return slices.ContainsFunc(req.URL.Query()["q"], func(q string) bool {
			return strings.Contains(q, "-age:30d")
		})
	})
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
		q := strings.Join(req.URL.Query()["q"], " ")
		if strings.Contains(q, "-age:30d") || strings.Contains(q, "age:30d") {
			t.Errorf("Expected --all to bypass age filters, but saw query: %s", q)
		}
	}

	if strings.Contains(out, "older changes") {
		t.Errorf("Expected no older changes notice with --all, got:\n%s", out)
	}
}

func TestStatus_CurrentBranch_WithCommentsAndDrafts(t *testing.T) {
	singleUnresolved := map[string][]CommentInfo{
		"pw_ghish/status.go": {
			{
				ID:         "c1",
				Line:       142,
				PatchSet:   14,
				Unresolved: new(true),
				Author:     AccountInfo{Email: "reviewer@google.com"},
				Message:    "Consider sorting these keys so test output is deterministic.",
			},
		},
	}

	t.Run("unresolved without drafts", func(t *testing.T) {
		mockGitForFeatureBranch(t)
		server := newStatusMockServer(t)
		onActiveChange(server).
			OnComments("*", singleUnresolved).
			OnDrafts("*", nil)

		out, err := executeCommand(RootCmd, "pr", "status")
		if err != nil {
			t.Fatalf("pr status failed: %v\nOutput: %s", err, out)
		}
		for _, exp := range []string{
			"Comments:",
			"⚠ 1 unresolved thread:",
			"• pw_ghish/status.go:142 [PS14] by reviewer@google.com:",
			"\"Consider sorting these keys so test output is deterministic.\"",
		} {
			if !strings.Contains(out, exp) {
				t.Errorf("Expected output to contain %q, but got:\n%s", exp, out)
			}
		}
		if strings.Contains(out, "draft") {
			t.Errorf("Expected no draft mention in output, got:\n%s", out)
		}
	})

	t.Run("unresolved with unpublished draft reply", func(t *testing.T) {
		mockGitForFeatureBranch(t)
		server := newStatusMockServer(t)
		onActiveChange(server).
			OnComments("*", singleUnresolved).
			OnDrafts("*", map[string][]CommentInfo{
				"pw_ghish/status.go": {
					{
						ID:        "d1",
						InReplyTo: "c1",
						Message:   "I am working on it right now.",
					},
				},
			})

		out, err := executeCommand(RootCmd, "pr", "status")
		if err != nil {
			t.Fatalf("pr status failed: %v\nOutput: %s", err, out)
		}
		for _, exp := range []string{
			"Comments:",
			"⚠ 1 unresolved thread (1 unpublished draft):",
			"• pw_ghish/status.go:142 [PS14] by reviewer@google.com (has unpublished draft reply):",
			"\"Consider sorting these keys so test output is deterministic.\"",
		} {
			if !strings.Contains(out, exp) {
				t.Errorf("Expected output to contain %q, but got:\n%s", exp, out)
			}
		}
	})

	t.Run("truncates after two unresolved threads", func(t *testing.T) {
		mockGitForFeatureBranch(t)
		server := newStatusMockServer(t)
		onActiveChange(server).
			OnComments("*", map[string][]CommentInfo{
				"pw_ghish/status.go": {
					{ID: "c1", Line: 10, PatchSet: 14, Unresolved: new(true), Author: AccountInfo{Email: "r@google.com"}, Message: "First thread."},
					{ID: "c2", Line: 20, PatchSet: 14, Unresolved: new(true), Author: AccountInfo{Email: "r@google.com"}, Message: "Second thread."},
					{ID: "c3", Line: 30, PatchSet: 14, Unresolved: new(true), Author: AccountInfo{Email: "r@google.com"}, Message: "Third thread."},
				},
			}).
			OnDrafts("*", map[string][]CommentInfo{})

		out, err := executeCommand(RootCmd, "pr", "status")
		if err != nil {
			t.Fatalf("pr status failed: %v\nOutput: %s", err, out)
		}
		for _, exp := range []string{
			"Comments:",
			"⚠ 3 unresolved threads (use 'gh pr view --comments' to view all):",
			"pw_ghish/status.go:10",
			"pw_ghish/status.go:20",
			"... and 1 more unresolved thread",
		} {
			if !strings.Contains(out, exp) {
				t.Errorf("Expected output to contain %q, but got:\n%s", exp, out)
			}
		}
		if strings.Contains(out, "pw_ghish/status.go:30") || strings.Contains(out, "Third thread.") {
			t.Errorf("Expected third thread to be truncated from output, got:\n%s", out)
		}
	})
}

func TestStatus_CurrentBranch_CommentsJSON(t *testing.T) {
	mockGitForFeatureBranch(t)
	server := newStatusMockServer(t)
	onActiveChange(server).
		OnComments("*", map[string][]CommentInfo{
			"pw_ghish/status.go": {
				{
					ID:         "c1",
					Line:       142,
					PatchSet:   14,
					Unresolved: new(true),
					Author:     AccountInfo{Email: "reviewer@google.com"},
					Message:    "Please check this.",
				},
			},
		}).
		OnDrafts("*", nil)

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
	onActiveChange(server)

	out, err := executeCommand(RootCmd, "pr", "status")
	if err != nil {
		t.Fatalf("pr status failed even though comments error was expected to degrade gracefully: %v\nOutput: %s", err, out)
	}

	if !strings.Contains(out, "Active Feature Subject") {
		t.Errorf("Expected active feature subject in output despite comments failure, got:\n%s", out)
	}
	if !strings.Contains(out, "✗ Failed to load comments") {
		t.Errorf("Expected status output to surface '✗ Failed to load comments' instead of silently claiming None, got:\n%s", out)
	}
}

func TestStatus_CurrentBranch_ChecksSummary(t *testing.T) {
	for _, tc := range []struct {
		name           string
		builds         []bbBuild
		args           []string
		wantContains   string
		wantAbsent     string
		wantJSON       string
		wantCheckCount int
	}{
		{
			name: "passing",
			builds: []bbBuild{
				FakeBuild("1", "builder-1", "SUCCESS"),
				FakeBuild("2", "builder-2", "SUCCESS"),
			},
			args:         []string{"pr", "status"},
			wantContains: "Checks:      ✓ 2 passing",
			wantAbsent:   "https://ci.chromium.org/b/",
		},
		{
			name: "failing",
			builds: []bbBuild{
				FakeBuild("1", "pigweed-lint", "FAILURE"),
				FakeBuild("2", "builder-2", "SUCCESS"),
			},
			args:         []string{"pr", "status"},
			wantContains: "Checks:      ✗ 1 failed: pigweed-lint (run 'gh run view 472267 --log-failed' to view errors)",
		},
		{
			name: "json",
			builds: []bbBuild{
				FakeBuild("1", "builder-1", "SUCCESS"),
			},
			args:           []string{"pr", "status", "--json", "current_branch"},
			wantJSON:       "✓ 1 passing",
			wantCheckCount: 1,
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			mockGitForFeatureBranch(t)
			server := newStatusMockServer(t)
			onActiveChange(server, WithProjectName("pigweed/pigweed")).
				OnComments("*", nil).
				OnDrafts("*", nil).
				OnSearchBuilds(tc.builds...)

			out, err := executeCommand(RootCmd, tc.args...)
			if err != nil {
				t.Fatalf("pr status failed: %v\nOutput: %s", err, out)
			}
			if tc.wantContains != "" && !strings.Contains(out, tc.wantContains) {
				t.Errorf("Expected %q in status output, got:\n%s", tc.wantContains, out)
			}
			if tc.wantAbsent != "" && strings.Contains(out, tc.wantAbsent) {
				t.Errorf("Expected no %q in status output, got:\n%s", tc.wantAbsent, out)
			}
			if tc.wantJSON != "" {
				var res struct {
					CurrentBranch struct {
						ChecksSummary string      `json:"checks_summary"`
						Checks        []CheckItem `json:"checks"`
					} `json:"current_branch"`
				}
				if err := json.Unmarshal([]byte(out), &res); err != nil {
					t.Fatalf("Failed to parse JSON: %v\nOutput: %s", err, out)
				}
				if res.CurrentBranch.ChecksSummary != tc.wantJSON {
					t.Errorf("checks_summary = %q, want %q", res.CurrentBranch.ChecksSummary, tc.wantJSON)
				}
				if len(res.CurrentBranch.Checks) != tc.wantCheckCount {
					t.Errorf("len(checks) = %d, want %d", len(res.CurrentBranch.Checks), tc.wantCheckCount)
				}
			}
		})
	}
}

func TestExtractLabelScore(t *testing.T) {
	tests := []struct {
		name      string
		labels    map[string]LabelInfo
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
			labels:    map[string]LabelInfo{},
			labelName: "Code-Review",
			want:      0,
		},
		{
			name: "highest positive score wins",
			labels: map[string]LabelInfo{
				"Code-Review": {
					All: []ApprovalInfo{
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
			labels: map[string]LabelInfo{
				"Code-Review": {
					All: []ApprovalInfo{
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
			labels: map[string]LabelInfo{
				"Code-Review": {
					All: []ApprovalInfo{
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
			if got := ExtractLabelScore(tt.labels, tt.labelName); got != tt.want {
				t.Errorf("ExtractLabelScore() = %d, want %d", got, tt.want)
			}
		})
	}
}

func TestExtractBlockers(t *testing.T) {
	tests := []struct {
		name   string
		change *ChangeInfo
		want   []string
	}{
		{
			name:   "submittable change has no blockers",
			change: &ChangeInfo{Submittable: true},
			want:   nil,
		},
		{
			name: "missing Code-Review and Verified approvals",
			change: &ChangeInfo{
				Submittable: false,
				Labels: map[string]LabelInfo{
					"Code-Review": {},
					"Verified":    {},
				},
			},
			want: []string{"Code-Review (+2 required)", "Verified (+1 required)"},
		},
		{
			name: "rejected label reported",
			change: &ChangeInfo{
				Submittable: false,
				Labels: map[string]LabelInfo{
					"Code-Review": {Approved: AccountInfo{AccountID: 1}},
					"Lint":        {Rejected: AccountInfo{AccountID: 2}},
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

func TestExtractBlockers_DynamicSubmitRequirements(t *testing.T) {
	change := &ChangeInfo{
		Submittable: false,
		SubmitRequirements: []SubmitRequirementResultInfo{
			{Name: "Code-Review", Status: "UNSATISFIED"},
			{Name: "Lint", Status: "SATISFIED"},
			{Name: "Copybara-Verified", Status: "UNSATISFIED"},
			{Name: "Topics-Not-Supported", Status: "UNSATISFIED"},
		},
		Labels: map[string]LabelInfo{
			"Code-Review":       {},
			"Lint":              {Approved: AccountInfo{AccountID: 1}},
			"Copybara-Verified": {},
		},
	}

	got := extractBlockers(change)
	want := []string{
		"Code-Review (+2 required)",
		"Copybara-Verified (Unsatisfied)",
		"Topics-Not-Supported (Unsatisfied)",
	}
	if len(got) != len(want) {
		t.Fatalf("extractBlockers() = %v, want %v", got, want)
	}
	for i := range got {
		if got[i] != want[i] {
			t.Errorf("extractBlockers()[%d] = %q, want %q", i, got[i], want[i])
		}
	}
}

func TestStatus_Pipeline1ParallelPrefetch_CommentsAndDrafts(t *testing.T) {
	mockGitForFeatureBranch(t)
	server := newStatusMockServer(t)

	commentsArrived := make(chan struct{})
	draftsArrived := make(chan struct{})
	var commentsOnce, draftsOnce sync.Once

	server.On("GET", "/changes/*/comments", func(w http.ResponseWriter, r *http.Request) {
		commentsOnce.Do(func() { close(commentsArrived) })
		timeout := time.NewTimer(2 * time.Second)
		defer timeout.Stop()
		select {
		case <-draftsArrived:
		case <-timeout.C:
			t.Errorf("ListChangeDrafts did not arrive concurrently with ListChangeComments in pr status Pipeline 1")
		}
		server.RespondJSON(w, http.StatusOK, map[string][]CommentInfo{
			"pw_ghish/status.go": {
				{
					ID:         "c1",
					Line:       42,
					PatchSet:   1,
					Unresolved: boolPtr(true),
					Author:     AccountInfo{Email: "reviewer@google.com"},
					Message:    "Concurrent status comment.",
				},
			},
		})
	})
	server.On("GET", "/changes/*/drafts", func(w http.ResponseWriter, r *http.Request) {
		draftsOnce.Do(func() { close(draftsArrived) })
		timeout := time.NewTimer(2 * time.Second)
		defer timeout.Stop()
		select {
		case <-commentsArrived:
		case <-timeout.C:
			t.Errorf("ListChangeComments did not arrive concurrently with ListChangeDrafts in pr status Pipeline 1")
		}
		server.RespondJSON(w, http.StatusOK, map[string]any{})
	})
	onActiveChange(server)

	out, err := executeCommand(RootCmd, "pr", "status")
	if err != nil {
		t.Fatalf("pr status failed: %v\nOutput: %s", err, out)
	}
	found := false
	var paths []string
	for _, req := range server.Requests() {
		paths = append(paths, req.Path)
		if req.Path == "/changes/"+statusChangeID {
			found = true
			wantOpts := []string{"DETAILED_LABELS", "ALL_REVISIONS", "DETAILED_ACCOUNTS", "SUBMITTABLE", "SUBMIT_REQUIREMENTS"}
			if !slices.Equal(req.URL.Query()["o"], wantOpts) {
				t.Errorf("GetChange o=%v, want %v", req.URL.Query()["o"], wantOpts)
			}
		}
	}
	if !found {
		t.Fatalf("no GetChange request for /changes/%s; requests: %v", statusChangeID, paths)
	}
	if !strings.Contains(out, "Concurrent status comment.") {
		t.Errorf("expected concurrent status comment in output, got:\n%s", out)
	}
}

func TestStatus_UsesCachedSelfAccountAndNumericQueries(t *testing.T) {
	cacheDir := t.TempDir()
	t.Setenv("GH_ISH_ACCOUNT_CACHE_DIR", cacheDir)
	tryWriteSelfAccountCache(cacheDir, "pigweed-review.googlesource.com", AccountInfo{
		AccountID: 1132682,
		Name:      "Cached Dev",
	}, time.Now())

	mockGit := mockGitForMainBranch(t)
	mockGit.OnCommand("config --get remote.origin.url", "https://pigweed.googlesource.com/pigweed/pigweed\n")

	server := NewMockGerritServer(t)
	server.On("GET", "/changes/*", func(w http.ResponseWriter, r *http.Request) {
		qs := r.URL.Query()["q"]
		batches := make([][]ChangeInfo, len(qs))
		server.RespondJSON(w, http.StatusOK, batches)
	})

	outStr, err := executeCommand(RootCmd, "pr", "status", "--host", "pigweed-review.googlesource.com")
	if err != nil {
		t.Fatalf("pr status failed: %v\nOutput: %s", err, outStr)
	}

	if got := server.CallCount("GET", "/accounts/self"); got != 0 {
		t.Errorf("expected 0 /accounts/self requests on cache hit, got %d", got)
	}
	var capturedQueries []string
	for _, req := range server.Requests() {
		capturedQueries = append(capturedQueries, req.URL.Query()["q"]...)
	}
	allQ := strings.Join(capturedQueries, " ")
	if !strings.Contains(allQ, "owner:1132682") || !strings.Contains(allQ, "reviewer:1132682") {
		t.Errorf("expected queries to rewrite self to 1132682, got %v", capturedQueries)
	}
	if strings.Contains(allQ, "owner:self") || strings.Contains(allQ, "reviewer:self") {
		t.Errorf("expected queries to omit :self when cached AccountID is available, got %v", capturedQueries)
	}
}

func TestStatusVerdict(t *testing.T) {
	change := func(attention bool) map[string]any { return map[string]any{"attention": attention} }
	tests := []struct {
		name               string
		created, reviewing []map[string]any
		want               string
	}{
		{"empty", nil, nil, "nothing awaits you (0 changes open, 0 review requests)"},
		{"nothing pending singular", []map[string]any{change(false)}, []map[string]any{change(false)},
			"nothing awaits you (1 change open, 1 review request)"},
		{"singular", []map[string]any{change(true)}, []map[string]any{change(true), change(false)},
			"1 of your 1 change needs attention, 1 review request awaits you"},
		{"plural", []map[string]any{change(true), change(true), change(false)}, []map[string]any{change(true), change(true)},
			"2 of your 3 changes need attention, 2 review requests await you"},
		{"only reviews", []map[string]any{change(false), change(false)}, []map[string]any{change(true)},
			"0 of your 2 changes need attention, 1 review request awaits you"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := statusVerdict(tt.created, tt.reviewing); got != tt.want {
				t.Errorf("statusVerdict() = %q, want %q", got, tt.want)
			}
		})
	}
}
