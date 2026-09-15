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
	"text/template"

	"github.com/andygrunwald/go-gerrit"
	"github.com/google/go-cmp/cmp"
)

func TestDefaultViewTemplate(t *testing.T) {
	data := map[string]any{
		"number":    12345,
		"patchset":  3,
		"title":     "Test Subject",
		"state":     "NEW",
		"author":    "Test User",
		"assignees": "Test User (attention), helper (cc)",
		"reviewers": "Alice (attention), Bob",
		"branch":    "main",
		"project":   "test-project",
		"topic":     "test-topic",
		"labels": map[string]gerrit.LabelInfo{
			"Code-Review": {
				Approved: gerrit.AccountInfo{AccountID: 1},
			},
		},
	}

	tmpl, err := template.New("test").Funcs(template.FuncMap{
		"getLabelSummary": getLabelSummary,
		"hasScore":        hasScore,
		"hasAnyScore":     hasAnyScore,
		"printComments":   func(map[string][]gerrit.CommentInfo) string { return "" },
	}).Parse(defaultViewTemplate)
	if err != nil {
		t.Fatalf("failed to parse defaultViewTemplate: %v", err)
	}

	var buf bytes.Buffer
	if err := tmpl.Execute(&buf, data); err != nil {
		t.Fatalf("failed to execute template: %v", err)
	}

	got := buf.String()
	want := `Change 12345
Subject: Test Subject
Status:  NEW
Owner:   Test User
Patchset: 3
Assignees: Test User (attention), helper (cc)
Reviewers: Alice (attention), Bob
Branch:  main
Project: test-project
Topic:   test-topic

Labels:
  Code-Review: +2 (Approved)`

	// Normalize whitespace for easier comparison if needed, or check exact.
	// Let's check exact but ignore leading/trailing newlines for robustness.
	got = strings.TrimSpace(got)
	want = strings.TrimSpace(want)

	if got != want {
		t.Errorf("template output mismatch\ngot:\n%s\nwant:\n%s", got, want)
	}
}

func TestPrViewIntegration(t *testing.T) {
	// Mock Gerrit response
	mockChange := map[string]any{
		"_number": 12345,
		"subject": "Test Subject",
		"status":  "NEW",
		"owner": map[string]any{
			"_account_id": 1,
			"name":        "Test User",
			"email":       "test@google.com",
		},
		"reviewers": map[string]any{
			"REVIEWER": []any{
				map[string]any{"_account_id": 2, "name": "Alice"},
			},
			"CC": []any{
				map[string]any{"_account_id": 3, "name": "helper"},
			},
		},
		"attention_set": map[string]any{
			"2": map[string]any{"account": map[string]any{"_account_id": 2, "name": "Alice"}},
		},
		"branch":           "main",
		"project":          "test-project",
		"topic":            "test-topic",
		"hashtags":         []any{"feature-x", "infra"},
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{
				"_number": 3,
				"commit": map[string]any{
					"subject": "Test Subject",
					"message": "Test Subject\n\nThis is the detailed commit body.\n",
				},
			},
		},
		"labels": map[string]any{
			"Code-Review": map[string]any{
				"approved": map[string]any{
					"account_id": 1,
				},
			},
			"Lint": map[string]any{},
		},
	}

	mockAbandonedChange := map[string]any{
		"_number": 67890,
		"subject": "Abandoned Subject",
		"status":  "ABANDONED",
		"owner": map[string]any{
			"_account_id": 1,
			"name":        "Test User",
			"email":       "test@google.com",
		},
		"current_revision": "deadbeef1234567890abcdef1234567890abcdef",
		"revisions": map[string]any{
			"deadbeef1234567890abcdef1234567890abcdef": map[string]any{
				"_number": 1,
			},
		},
		"branch":  "main",
		"project": "test-project",
	}

	server := NewMockGerritServer(t)
	server.On("GET", "/changes/12345", func(w http.ResponseWriter, r *http.Request) {
		wantOpts := []string{"DETAILED_LABELS", "CURRENT_REVISION", "CURRENT_COMMIT", "DETAILED_ACCOUNTS"}
		if diff := cmp.Diff(wantOpts, r.URL.Query()["o"]); diff != "" {
			t.Errorf("options mismatch (-want +got):\n%s", diff)
		}
		server.RespondJSON(w, http.StatusOK, mockChange)
	})
	server.On("GET", "/changes/67890", func(w http.ResponseWriter, r *http.Request) {
		wantOpts := []string{"DETAILED_LABELS", "CURRENT_REVISION", "CURRENT_COMMIT", "DETAILED_ACCOUNTS"}
		if diff := cmp.Diff(wantOpts, r.URL.Query()["o"]); diff != "" {
			t.Errorf("options mismatch (-want +got):\n%s", diff)
		}
		server.RespondJSON(w, http.StatusOK, mockAbandonedChange)
	})
	server.OnJSON("GET", "/changes/12345/revisions/current/files*", http.StatusOK, map[string]any{})
	server.OnJSON("GET", "/changes/67890/revisions/current/files*", http.StatusOK, map[string]any{})

	oldJSON := jsonOutputFields
	defer func() { jsonOutputFields = oldJSON }()

	tests := []struct {
		name     string
		args     []string
		wantCont []string
		dontCont []string
		wantJSON map[string]any
	}{
		{
			name: "standard output",
			args: []string{"pr", "view", "12345"},
			wantCont: []string{
				"Change 12345",
				"Patchset: 3",
				"Subject: Test Subject",
				"Status:  NEW",
				"Owner:   Test User",
				"Assignees: Test User, helper (cc)",
				"Reviewers: Alice (attention)",
				"Topic:   test-topic",
				"Hashtags: feature-x, infra",
				"This is the detailed commit body.",
			},
			dontCont: []string{
				"Lint: No score",
			},
		},
		{
			name:     "json output",
			args:     []string{"pr", "view", "12345", "--json", "number,title,patchset"},
			wantJSON: map[string]any{"number": 12345.0, "title": "Test Subject", "patchset": 3.0},
		},
		{
			name:     "json output with body",
			args:     []string{"pr", "view", "12345", "--json", "number,body,title"},
			wantJSON: map[string]any{"number": 12345.0, "title": "Test Subject", "body": "This is the detailed commit body."},
		},
		{
			name: "abandoned change",
			args: []string{"pr", "view", "67890"},
			wantCont: []string{
				"Change 67890",
				"Patchset: 1",
				"Subject: Abandoned Subject",
				"Status:  ABANDONED",
				"Owner:   Test User",
			},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			jsonOutputFields = ""
			output, err := executeCommand(RootCmd, tt.args...)
			if err != nil {
				t.Fatalf("Command failed: %v\nOutput: %s", err, output)
			}

			if tt.wantJSON != nil {
				var got map[string]any
				if err := json.Unmarshal([]byte(output), &got); err != nil {
					t.Fatalf("failed to unmarshal JSON output: %v\nOutput: %s", err, output)
				}
				if !cmp.Equal(got, tt.wantJSON) {
					t.Errorf("JSON output mismatch (-got +want):\n%s", cmp.Diff(got, tt.wantJSON))
				}
			} else {
				for _, want := range tt.wantCont {
					if !strings.Contains(output, want) {
						t.Errorf("Output missing %q. Got:\n%s", want, output)
					}
				}
				for _, dont := range tt.dontCont { // NOTYPO
					if strings.Contains(output, dont) {
						t.Errorf("Output unexpectedly contains %q. Got:\n%s", dont, output)
					}
				}
			}
		})
	}
}

func TestView_WritesToCmdOut(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/files*", http.StatusOK, map[string]any{})
	server.OnJSON("GET", "/changes/12345", http.StatusOK, map[string]any{
		"project": "test-project",
		"_number": 12345,
		"subject": "View Subject",
		"status":  "NEW",
		"owner": map[string]any{
			"name": "Test User",
		},
	})

	var buf bytes.Buffer
	resetAllFlags(RootCmd)
	RootCmd.SetOut(&buf)
	RootCmd.SetErr(&buf)
	RootCmd.SetArgs([]string{"pr", "view", "12345"})

	if err := RootCmd.Execute(); err != nil {
		t.Fatalf("RootCmd.Execute() failed: %v", err)
	}

	if !strings.Contains(buf.String(), "Change 12345") || !strings.Contains(buf.String(), "View Subject") {
		t.Errorf("Expected buf to receive rendered output, got:\n%s", buf.String())
	}
}

func TestView_ErrorWhenNotFound(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnStatus(http.StatusNotFound)

	_, err := executeCommand(RootCmd, "pr", "view", "99999")
	if err == nil {
		t.Fatal("Expected error when change not found, got nil")
	}
	if !strings.Contains(err.Error(), "failed to get change 99999") {
		t.Errorf("Expected error mentioning change 99999, got: %v", err)
	}
}

func TestView_DefaultActivePR(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e/revisions/current/files*", http.StatusOK, map[string]any{})
	server.OnJSON("GET", "/changes/Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e", http.StatusOK, gerrit.ChangeInfo{
		ChangeID: "Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e",
		Number:   472267,
		Subject:  "Default Active PR Subject",
		Status:   "NEW",
		Branch:   "main",
		Project:  "test-project",
	})

	mockGit := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) >= 2 && args[0] == "branch" && args[1] == "--show-current" {
				stdout.Write([]byte("my-feature\n"))
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

	cfg := &Config{
		Git:  mockGit,
		Host: server.URL,
	}
	SetConfig(RootCmd, cfg)
	defer SetConfig(RootCmd, nil)

	out, err := executeCommand(RootCmd, "pr", "view")
	if err != nil {
		t.Fatalf("pr view without args failed: %v\nOutput: %s", err, out)
	}

	if !strings.Contains(out, "Change 472267") || !strings.Contains(out, "Default Active PR Subject") {
		t.Errorf("Expected output to contain Change 472267 and subject, got:\n%s", out)
	}
}

func TestView_NoActivePR_Error(t *testing.T) {
	mockGit := &MockGitRunner{
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
	}
	SetMockGit(t, mockGit)

	cfg := &Config{
		Git: mockGit,
	}
	SetConfig(RootCmd, cfg)
	defer SetConfig(RootCmd, nil)

	_, err := executeCommand(RootCmd, "pr", "view")
	if err == nil {
		t.Fatal("Expected error when on main with no active PR, got nil")
	}
	if !strings.Contains(err.Error(), "no change ID specified") {
		t.Errorf("Expected error to mention no change ID specified, got: %v", err)
	}
}

func TestFormatCheckSummary(t *testing.T) {
	tests := []struct {
		name   string
		builds []bbBuild
		want   string
	}{
		{
			name:   "empty builds",
			builds: []bbBuild{},
			want:   "No checks scheduled (run 'gh pr review --cq' to trigger dry run)",
		},
		{
			name: "all passing",
			builds: []bbBuild{
				{Status: "SUCCESS", Builder: bbBuilder{Builder: "builder-1"}},
				{Status: "SUCCESS", Builder: bbBuilder{Builder: "builder-2"}},
			},
			want: "✓ 2 passing",
		},
		{
			name: "single failure detected",
			builds: []bbBuild{
				{Status: "SUCCESS", Builder: bbBuilder{Builder: "builder-1"}},
				{Status: "FAILURE", Builder: bbBuilder{Builder: "pigweed-lint"}},
			},
			want: "✖ 1 failed: pigweed-lint (run 'gh run view --log-failed' to view errors)",
		},
		{
			name: "multiple failures detected",
			builds: []bbBuild{
				{Status: "FAILURE", Builder: bbBuilder{Builder: "builder-a"}},
				{Status: "INFRA_FAILURE", Builder: bbBuilder{Builder: "builder-b"}},
			},
			want: "✖ 2 failed: builder-a, builder-b (run 'gh run view --log-failed' to view errors)",
		},
		{
			name: "running and passed",
			builds: []bbBuild{
				{Status: "SUCCESS", Builder: bbBuilder{Builder: "builder-1"}},
				{Status: "STARTED", Builder: bbBuilder{Builder: "builder-2"}},
			},
			want: "● 1 passed, 1 running (use 'gh pr checks --watch' to monitor)",
		},
		{
			name: "only running",
			builds: []bbBuild{
				{Status: "SCHEDULED", Builder: bbBuilder{Builder: "builder-1"}},
			},
			want: "● 1 running (use 'gh pr checks --watch' to monitor)",
		},
		{
			name: "single failure with running",
			builds: []bbBuild{
				{Status: "FAILURE", Builder: bbBuilder{Builder: "pigweed-lint"}},
				{Status: "STARTED", Builder: bbBuilder{Builder: "builder-running"}},
			},
			want: "✖ 1 failed: pigweed-lint (1 running; run 'gh run view --log-failed' to view errors)",
		},
		{
			name: "more than 3 failures",
			builds: []bbBuild{
				{Status: "FAILURE", Builder: bbBuilder{Builder: "b1"}},
				{Status: "FAILURE", Builder: bbBuilder{Builder: "b2"}},
				{Status: "FAILURE", Builder: bbBuilder{Builder: "b3"}},
				{Status: "FAILURE", Builder: bbBuilder{Builder: "b4"}},
			},
			want: "✖ 4 failed: b1, b2, b3, and 1 more (run 'gh run view --log-failed' to view errors)",
		},
		{
			name: "experimental failure ignored",
			builds: []bbBuild{
				{Status: "SUCCESS", Builder: bbBuilder{Builder: "b1"}},
				{Status: "FAILURE", Critical: "NO", Builder: bbBuilder{Builder: "b-exp"}},
			},
			want: "✓ 1 passing",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := formatCheckSummary(tt.builds)
			if got != tt.want {
				t.Errorf("formatCheckSummary() = %q, want %q", got, tt.want)
			}
		})
	}
}

func TestView_DisplaysChecks_Failing(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/files*", http.StatusOK, map[string]any{})
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"project":          "pigweed/pigweed",
		"_number":          12345,
		"subject":          "Check Failing View Test",
		"status":           "NEW",
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
					"builder": "pigweed-lint",
				},
				"status": "FAILURE",
			},
			{
				"id": "2",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "try",
					"builder": "pigweed-build",
				},
				"status": "SUCCESS",
			},
		},
	})

	output, err := executeCommand(RootCmd, "pr", "view", "12345", "--buildbucket-host", server.URL)
	if err != nil {
		t.Fatalf("pr view failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "Checks:  ✖ 1 failed: pigweed-lint (run 'gh run view --log-failed' to view errors)") {
		t.Errorf("Expected failure indicator for pigweed-lint, got:\n%s", output)
	}
}

func TestView_JSON_Checks(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/files*", http.StatusOK, map[string]any{})
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"project":          "pigweed/pigweed",
		"_number":          12345,
		"subject":          "Check JSON View Test",
		"status":           "NEW",
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
					"builder": "pigweed-build",
				},
				"status": "SUCCESS",
			},
		},
	})

	output, err := executeCommand(RootCmd, "pr", "view", "12345", "--buildbucket-host", server.URL, "--json", "number,checks")
	if err != nil {
		t.Fatalf("pr view failed: %v\nOutput: %s", err, output)
	}

	var res struct {
		Number int    `json:"number"`
		Checks string `json:"checks"`
	}
	if err := json.Unmarshal([]byte(output), &res); err != nil {
		t.Fatalf("Failed to parse JSON: %v\nOutput: %s", err, output)
	}
	if res.Checks != "✓ 1 passing" {
		t.Errorf("Expected checks '✓ 1 passing', got %q", res.Checks)
	}
}

func TestView_WebBrowserOpen(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/files*", http.StatusOK, map[string]any{})
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"project":          "pigweed/pigweed",
		"_number":          12345,
		"subject":          "Web View Test",
		"status":           "NEW",
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{"_number": 1},
		},
	})

	var openedURL string
	oldOpenBrowser := OpenBrowserFn
	defer func() { OpenBrowserFn = oldOpenBrowser }()
	OpenBrowserFn = func(urlStr string) error {
		openedURL = urlStr
		return nil
	}

	output, err := executeCommand(RootCmd, "pr", "view", "12345", "--web")
	if err != nil {
		t.Fatalf("pr view --web failed: %v\nOutput: %s", err, output)
	}

	expectedPrefix := server.URL + "/c/pigweed/pigweed/+/12345"
	if !strings.HasPrefix(openedURL, expectedPrefix) {
		t.Errorf("Expected browser to open %q, got %q", expectedPrefix, openedURL)
	}
	if !strings.Contains(output, "Opening") && !strings.Contains(output, server.URL) {
		t.Errorf("Expected output to mention opening URL, got:\n%s", output)
	}
}

// bugViewServer serves change 12345 whose current patchset carries commitMsg.
func bugViewServer(t *testing.T, commitMsg string) *MockGerritServer {
	t.Helper()
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/files*", http.StatusOK, map[string]any{})
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"project":          "pigweed/pigweed",
		"_number":          12345,
		"subject":          "pw_foo: Add bar",
		"status":           "NEW",
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{
				"_number": 1,
				"commit":  map[string]any{"message": commitMsg},
			},
		},
	})
	return server
}

func TestView_JSONBugFields(t *testing.T) {
	tests := []struct {
		name      string
		commitMsg string
		wantBug   string
		wantBugs  []BugLink
	}{
		{
			name:      "no bug trailer",
			commitMsg: "pw_foo: Add bar\n\nBody.\n\nChange-Id: I1234\n",
			wantBug:   "",
			wantBugs:  []BugLink{},
		},
		{
			name:      "bug trailer",
			commitMsg: "pw_foo: Add bar\n\nBody.\n\nBug: b/123456\nChange-Id: I1234\n",
			wantBug:   "b/123456",
			wantBugs:  []BugLink{{ID: "b/123456", Closes: false}},
		},
		{
			name:      "fixed trailer closes",
			commitMsg: "pw_foo: Add bar\n\nBody.\n\nFixed: 123456\nChange-Id: I1234\n",
			wantBug:   "b/123456",
			wantBugs:  []BugLink{{ID: "b/123456", Closes: true}},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			bugViewServer(t, tt.commitMsg)

			output, err := executeCommand(RootCmd, "pr", "view", "12345", "--json", "bug,bugs")
			if err != nil {
				t.Fatalf("pr view --json bug,bugs failed: %v\nOutput: %s", err, output)
			}

			var got struct {
				Bug  string    `json:"bug"`
				Bugs []BugLink `json:"bugs"`
			}
			if err := json.Unmarshal([]byte(output), &got); err != nil {
				t.Fatalf("failed to parse JSON: %v\nOutput: %s", err, output)
			}
			if got.Bug != tt.wantBug {
				t.Errorf("bug = %q, want %q", got.Bug, tt.wantBug)
			}
			if diff := cmp.Diff(tt.wantBugs, got.Bugs); diff != "" {
				t.Errorf("bugs mismatch (-want +got):\n%s", diff)
			}
			if strings.Contains(output, "null") {
				t.Errorf("bugs should serialize as [] rather than null, got:\n%s", output)
			}
		})
	}
}

func TestView_DefaultOutputShowsBug(t *testing.T) {
	bugViewServer(t, "pw_foo: Add bar\n\nBody.\n\nFixed: b/123456\nChange-Id: I1234\n")

	output, err := executeCommand(RootCmd, "pr", "view", "12345")
	if err != nil {
		t.Fatalf("pr view failed: %v\nOutput: %s", err, output)
	}
	if !strings.Contains(output, "Bug:     b/123456") {
		t.Errorf("Expected a Bug line in the default output, got:\n%s", output)
	}
}

func TestView_DefaultOutputOmitsBugLineWhenUnlinked(t *testing.T) {
	bugViewServer(t, "pw_foo: Add bar\n\nBody.\n\nChange-Id: I1234\n")

	output, err := executeCommand(RootCmd, "pr", "view", "12345")
	if err != nil {
		t.Fatalf("pr view failed: %v\nOutput: %s", err, output)
	}
	if strings.Contains(output, "Bug:") {
		t.Errorf("Expected no Bug line when nothing is linked, got:\n%s", output)
	}
}

// TestView_BugFieldRefusedWithoutCommitMessage is the whole reason this field
// can be trusted. Reporting an empty bug when the commit message never arrived
// would tell an agent "nothing is linked", which is how a duplicate trailer
// gets written over a perfectly good one.
func TestView_BugFieldRefusedWithoutCommitMessage(t *testing.T) {
	bugViewServer(t, "")

	output, err := executeCommand(RootCmd, "pr", "view", "12345", "--json", "number,bug")
	if err == nil {
		t.Fatalf("Expected an error when the commit message is unavailable, got output:\n%s", output)
	}

	for _, want := range []string{
		"12345", // which change
		"commit message",
		"Bug:",          // what the field is derived from
		"commitMessage", // the discovery fallback
	} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("Error missing %q. Got:\n%v", want, err)
		}
	}
}

// A view that does not ask for the bug field must still work without a commit
// message: refusing there would break `pr view` for every change whose
// revision detail is unavailable.
func TestView_WithoutCommitMessageStillViewsOtherFields(t *testing.T) {
	bugViewServer(t, "")

	output, err := executeCommand(RootCmd, "pr", "view", "12345", "--json", "number,title")
	if err != nil {
		t.Fatalf("pr view --json number,title failed: %v\nOutput: %s", err, output)
	}
	if !strings.Contains(output, "pw_foo: Add bar") {
		t.Errorf("Expected the title, got:\n%s", output)
	}
}

func TestView_NonexistentRevisionError(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/files*", http.StatusOK, map[string]any{})
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"project":          "pigweed/pigweed",
		"_number":          12345,
		"subject":          "Revision Error Test",
		"status":           "NEW",
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{
				"_number": 1,
				"commit":  map[string]any{"message": "pw_foo: Add bar\n\nChange-Id: I1234\n"},
			},
		},
	})

	_, err := executeCommand(RootCmd, "pr", "view", "12345/99")
	if err == nil {
		t.Fatal("expected error when non-existent patchset 99 requested, got nil")
	}
	if !strings.Contains(err.Error(), "revision 99 not found") {
		t.Errorf("expected error mentioning 'revision 99 not found', got: %v", err)
	}
}
