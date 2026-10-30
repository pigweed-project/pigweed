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
	"fmt"
	"net/http"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/andygrunwald/go-gerrit"
	"github.com/google/go-cmp/cmp"
)

// narrowLabel builds a LabelInfo for a label whose range is [low, high],
// voted at value. Gerrit sets Approved/Rejected when a vote hits the label's
// maximum/minimum, whatever those happen to be.
func narrowLabel(low, high, value int) gerrit.LabelInfo {
	info := gerrit.LabelInfo{Values: map[string]string{}}
	for v := low; v <= high; v++ {
		info.Values[fmt.Sprintf("%+d", v)] = "desc"
	}
	if value != 0 {
		info.All = []gerrit.ApprovalInfo{
			{AccountInfo: gerrit.AccountInfo{AccountID: 1}, Value: value},
		}
	}
	switch value {
	case high:
		info.Approved = gerrit.AccountInfo{AccountID: 1}
	case low:
		info.Rejected = gerrit.AccountInfo{AccountID: 1}
	}
	return info
}

func TestGetLabelSummary(t *testing.T) {
	for _, tt := range []struct {
		name string
		info gerrit.LabelInfo
		want string
	}{
		// Labels that top out at +1 must never be reported as +2: that is
		// not a value they can hold.
		{"Docs-Not-Needed approved", narrowLabel(0, 1, 1), "+1 (Approved)"},
		{"Pigweed-Auto-Submit approved", narrowLabel(0, 1, 1), "+1 (Approved)"},
		{"Presubmit-Verified approved", narrowLabel(-1, 1, 1), "+1 (Approved)"},
		{"Presubmit-Verified rejected", narrowLabel(-1, 1, -1), "-1 (Rejected)"},

		// A full -2..+2 label keeps its familiar rendering.
		{"Code-Review approved", narrowLabel(-2, 2, 2), "+2 (Approved)"},
		{"Code-Review recommended", narrowLabel(-2, 2, 1), "+1 (Recommended)"},
		{"Code-Review disliked", narrowLabel(-2, 2, -1), "-1 (Disliked)"},
		{"Code-Review rejected", narrowLabel(-2, 2, -2), "-2 (Rejected)"},

		{"no score", narrowLabel(-2, 2, 0), "No score"},
		{"empty label", gerrit.LabelInfo{}, "No score"},

		// A negative vote outranks a positive one, matching Gerrit.
		{
			"negative outranks positive",
			gerrit.LabelInfo{
				Values: map[string]string{"-2": "d", "-1": "d", "+0": "d", "+1": "d", "+2": "d"},
				All: []gerrit.ApprovalInfo{
					{AccountInfo: gerrit.AccountInfo{AccountID: 1}, Value: 2},
					{AccountInfo: gerrit.AccountInfo{AccountID: 2}, Value: -1},
				},
			},
			"-1 (Disliked)",
		},

		// Without DETAILED_LABELS there is no All[] and no range, so report
		// the state without inventing a number.
		{
			"approved without detail",
			gerrit.LabelInfo{Approved: gerrit.AccountInfo{AccountID: 1}},
			"Approved",
		},
	} {
		t.Run(tt.name, func(t *testing.T) {
			if got := getLabelSummary(tt.info); got != tt.want {
				t.Errorf("getLabelSummary() = %q, want %q", got, tt.want)
			}
		})
	}
}

func TestPrViewIntegration(t *testing.T) {
	owner := map[string]any{"_account_id": 1, "name": "Test User", "email": "test@google.com"}
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345,
		WithSubject("Test Subject"),
		WithCurrentPatchset(3),
		WithProjectName("test-project"),
		WithCommitMessage("Test Subject\n\nThis is the detailed commit body.\n"),
		func(ch map[string]any) {
			ch["owner"] = owner
			ch["topic"] = "test-topic"
			ch["hashtags"] = []string{"feature-x", "infra"}
			ch["reviewers"] = map[string]any{
				"REVIEWER": []any{map[string]any{"_account_id": 2, "name": "Alice"}},
				"CC":       []any{map[string]any{"_account_id": 3, "name": "helper"}},
			}
			ch["attention_set"] = map[string]any{
				"2": map[string]any{"account": map[string]any{"_account_id": 2, "name": "Alice"}},
			}
			ch["labels"] = map[string]any{
				"Code-Review": map[string]any{
					"approved": map[string]any{"_account_id": 1},
					"values":   map[string]string{"-2": "Veto", "-1": "Negative", " 0": "No score", "+1": "Positive", "+2": "Approved"},
					"all":      []any{map[string]any{"_account_id": 1, "value": 2}},
				},
				"Lint": map[string]any{},
			}
		},
	)
	server.OnDefaultChange(67890,
		WithSubject("Abandoned Subject"),
		WithStatus("ABANDONED"),
		WithProjectName("test-project"),
		func(ch map[string]any) { ch["owner"] = owner },
	)
	server.OnJSON("GET", "/changes/*/revisions/current/files*", http.StatusOK, map[string]any{})

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
				"Branch:  main",
				"Project: test-project",
				"Topic:   test-topic",
				"Hashtags: feature-x, infra",
				"Labels:\n  Code-Review: +2 (Approved)",
				"This is the detailed commit body.",
			},
			dontCont: []string{"Lint: No score"},
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

	wantOpts := []string{"DETAILED_LABELS", "ALL_REVISIONS", "CURRENT_COMMIT", "DETAILED_ACCOUNTS"}
	if diff := cmp.Diff(wantOpts, server.Requests()[0].URL.Query()["o"]); diff != "" {
		t.Errorf("options mismatch (-want +got):\n%s", diff)
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
	server.OnDefaultChange(472267, WithChangeID("Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e"), WithSubject("Default Active PR Subject"))

	NewMockGit(t).
		WithBranch("my-feature").
		WithCommit("commit msg\n\nChange-Id: Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e\n")

	out, err := executeCommand(RootCmd, "pr", "view")
	if err != nil {
		t.Fatalf("pr view without args failed: %v\nOutput: %s", err, out)
	}

	if !strings.Contains(out, "Change 472267") || !strings.Contains(out, "Default Active PR Subject") {
		t.Errorf("Expected output to contain Change 472267 and subject, got:\n%s", out)
	}
}

func TestView_NoActivePR_Error(t *testing.T) {
	NewMockGit(t).
		WithBranch("main").
		WithCommit("commit without change-id\n").
		OnCommand("rev-list --count", "0\n")

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
			want:   "No checks scheduled (run 'gh pr review 12345 --cq' to trigger dry run)",
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
			want: "✗ 1 failed: pigweed-lint (run 'gh run view 12345 --log-failed' to view errors)",
		},
		{
			name: "multiple failures detected",
			builds: []bbBuild{
				{Status: "FAILURE", Builder: bbBuilder{Builder: "builder-a"}},
				{Status: "INFRA_FAILURE", Builder: bbBuilder{Builder: "builder-b"}},
			},
			want: "✗ 2 failed: builder-a, builder-b (run 'gh run view 12345 --log-failed' to view errors)",
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
			want: "✗ 1 failed: pigweed-lint (1 running; run 'gh run view 12345 --log-failed' to view errors)",
		},
		{
			name: "more than 3 failures",
			builds: []bbBuild{
				{Status: "FAILURE", Builder: bbBuilder{Builder: "b1"}},
				{Status: "FAILURE", Builder: bbBuilder{Builder: "b2"}},
				{Status: "FAILURE", Builder: bbBuilder{Builder: "b3"}},
				{Status: "FAILURE", Builder: bbBuilder{Builder: "b4"}},
			},
			want: "✗ 4 failed: b1, b2, b3, and 1 more (run 'gh run view 12345 --log-failed' to view errors)",
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
			got := formatCheckSummary(tt.builds, 12345)
			if got != tt.want {
				t.Errorf("formatCheckSummary() = %q, want %q", got, tt.want)
			}
		})
	}
}

func TestView_DisplaysChecks_Failing(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/files*", http.StatusOK, map[string]any{})
	server.OnDefaultChange(12345, WithSubject("Check Failing View Test")).
		OnSearchBuilds(
			FakeBuild("1", "pigweed-lint", "FAILURE"),
			FakeBuild("2", "pigweed-build", "SUCCESS"),
		)

	output, err := executeCommand(RootCmd, "pr", "view", "12345")
	if err != nil {
		t.Fatalf("pr view failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "Checks:  ✗ 1 failed: pigweed-lint (run 'gh run view 12345 --log-failed' to view errors)") {
		t.Errorf("Expected failure indicator for pigweed-lint, got:\n%s", output)
	}
}

func TestView_JSON_Checks(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/files*", http.StatusOK, map[string]any{})
	server.OnDefaultChange(12345, WithSubject("Check JSON View Test")).
		OnSearchBuilds(FakeBuild("1", "pigweed-build", "SUCCESS"))

	output, err := executeCommand(RootCmd, "pr", "view", "12345", "--json", "number,checks")
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
	server.OnDefaultChange(12345, WithSubject("Web View Test"))

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
	server.OnDefaultChange(12345, WithSubject("pw_foo: Add bar"), WithCommitMessage(commitMsg))
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
	server.OnDefaultChange(12345, WithSubject("Revision Error Test"), WithCommitMessage("pw_foo: Add bar\n\nChange-Id: I1234\n"))

	_, err := executeCommand(RootCmd, "pr", "view", "12345/99")
	if err == nil {
		t.Fatal("expected error when non-existent patchset 99 requested, got nil")
	}
	if !strings.Contains(err.Error(), "revision 99 not found") {
		t.Errorf("expected error mentioning 'revision 99 not found', got: %v", err)
	}
}

func TestView_CommentsAndDrafts(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/files*", http.StatusOK, map[string]any{})
	server.OnComments(12345, map[string]any{
		"pw_ring_buffer/ring_buffer.cc": []map[string]any{
			{
				"id":         "pub_1",
				"line":       42,
				"patch_set":  2,
				"unresolved": true,
				"author":     map[string]any{"name": "Ben Lawson"},
				"message":    "Why was this file deleted?",
			},
		},
	})
	server.OnDrafts(12345, map[string]any{
		"pw_ring_buffer/ring_buffer.cc": []map[string]any{
			{
				"id":          "draft_1",
				"in_reply_to": "pub_1",
				"line":        42,
				"patch_set":   2,
				"unresolved":  false,
				"message":     "Reverted.",
			},
		},
		"pw_ring_buffer/ring_buffer.h": []map[string]any{
			{
				"id":         "draft_2",
				"line":       84,
				"patch_set":  2,
				"unresolved": true,
				"message":    "FOR GEMINI: Please use pw::Result here.",
			},
		},
	})
	server.OnDefaultChange(12345, WithSubject("Drafts View Test"), WithCurrentPatchset(2), WithCommitMessage("pw_ring_buffer: Test\n\nChange-Id: I1234\n"))

	t.Run("terminal output renders published comments and drafts", func(t *testing.T) {
		output, err := executeCommand(RootCmd, "pr", "view", "12345", "--comments")
		if err != nil {
			t.Fatalf("pr view --comments failed: %v\nOutput: %s", err, output)
		}
		for _, want := range []string{
			"Comments:",
			"File: pw_ring_buffer/ring_buffer.cc",
			"Line 42 [PS2]: Ben Lawson [unresolved]",
			"Why was this file deleted?",
			"-> [DRAFT, resolved]: Reverted.",
			"File: pw_ring_buffer/ring_buffer.h",
			"Line 84 [PS2]: [DRAFT] [unresolved]",
			"FOR GEMINI: Please use pw::Result here.",
		} {
			if !strings.Contains(output, want) {
				t.Errorf("output missing %q.\nGot:\n%s", want, output)
			}
		}
	})

	t.Run("json output exposes comments and drafts without mutating author", func(t *testing.T) {
		output, err := executeCommand(RootCmd, "pr", "view", "12345", "--json", "comments,drafts")
		if err != nil {
			t.Fatalf("pr view --json comments,drafts failed: %v\nOutput: %s", err, output)
		}
		var got struct {
			Comments map[string][]gerrit.CommentInfo `json:"comments"`
			Drafts   map[string][]gerrit.CommentInfo `json:"drafts"`
		}
		if err := json.Unmarshal([]byte(output), &got); err != nil {
			t.Fatalf("failed to parse JSON: %v\nOutput: %s", err, output)
		}
		if len(got.Comments["pw_ring_buffer/ring_buffer.cc"]) != 1 {
			t.Fatalf("expected 1 published comment on ring_buffer.cc, got %+v", got.Comments)
		}
		if len(got.Drafts["pw_ring_buffer/ring_buffer.cc"]) != 1 {
			t.Fatalf("expected 1 draft on ring_buffer.cc, got %+v", got.Drafts)
		}
		if got.Drafts["pw_ring_buffer/ring_buffer.cc"][0].Author.Name != "" {
			t.Errorf("expected draft Author.Name to remain unmodified empty string in JSON, got %q",
				got.Drafts["pw_ring_buffer/ring_buffer.cc"][0].Author.Name)
		}
		if len(got.Drafts["pw_ring_buffer/ring_buffer.h"]) != 1 {
			t.Fatalf("expected 1 standalone draft on ring_buffer.h, got %+v", got.Drafts)
		}
	})
}

func TestView_EmptyCommentsShowsNone(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/files*", http.StatusOK, map[string]any{})
	server.OnComments(12345, nil).OnDrafts(12345, nil).OnDefaultChange(12345, WithSubject("No Comments Test"))

	output, err := executeCommand(RootCmd, "pr", "view", "12345", "--comments")
	if err != nil {
		t.Fatalf("pr view --comments failed: %v\nOutput: %s", err, output)
	}
	if !strings.Contains(output, "Comments: None") {
		t.Errorf("expected 'Comments: None' when --comments is passed and there are no comments or drafts, got:\n%s", output)
	}
}

func TestView_OnlyDrafts_RendersCommentsSection(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/files*", http.StatusOK, map[string]any{})
	server.OnComments(12345, nil).OnDrafts(12345, map[string]any{
		"foo.cc": []map[string]any{
			{
				"id":         "draft_only",
				"line":       10,
				"patch_set":  1,
				"unresolved": true,
				"message":    "Private steering note",
			},
		},
	}).OnDefaultChange(12345, WithSubject("Only Drafts Test"))

	output, err := executeCommand(RootCmd, "pr", "view", "12345", "--comments")
	if err != nil {
		t.Fatalf("pr view --comments failed: %v\nOutput: %s", err, output)
	}
	if !strings.Contains(output, "Comments:") || !strings.Contains(output, "Private steering note") {
		t.Errorf("expected Comments section with standalone draft even when published comments are empty, got:\n%s", output)
	}
}

func TestView_ErrorWhenDraftsOrFilesFail(t *testing.T) {
	t.Run("fails when ListChangeDrafts fails during --comments", func(t *testing.T) {
		server := NewMockGerritServer(t)
		server.OnJSON("GET", "/changes/12345/revisions/current/files*", http.StatusOK, map[string]any{})
		server.OnComments(12345, nil)
		server.On("GET", "/changes/12345/drafts", func(w http.ResponseWriter, r *http.Request) {
			http.Error(w, "Drafts DB error", http.StatusInternalServerError)
		})
		server.OnDefaultChange(12345, WithSubject("Drafts Error Test"))

		_, err := executeCommand(RootCmd, "pr", "view", "12345", "--comments")
		if err == nil {
			t.Fatal("expected error when ListChangeDrafts fails during --comments, got nil")
		}
		if !strings.Contains(err.Error(), "error listing draft comments for change 12345") {
			t.Errorf("expected error mentioning draft comments failure, got: %v", err)
		}
	})

	t.Run("fails when ListFiles fails during --json files", func(t *testing.T) {
		server := NewMockGerritServer(t)
		server.On("GET", "/changes/12345/revisions/current/files*", func(w http.ResponseWriter, r *http.Request) {
			http.Error(w, "Files DB error", http.StatusInternalServerError)
		})
		server.OnDefaultChange(12345, WithSubject("Files Error Test"))

		_, err := executeCommand(RootCmd, "pr", "view", "12345", "--json", "files")
		if err == nil {
			t.Fatal("expected error when ListFiles fails during --json files, got nil")
		}
		if !strings.Contains(err.Error(), "error listing files for change 12345") {
			t.Errorf("expected error mentioning list files failure, got: %v", err)
		}
	})
}

func TestView_Wave1ParallelPrefetch_CommentsAndDrafts(t *testing.T) {
	server := NewMockGerritServer(t)

	commentsArrived := make(chan struct{})
	draftsArrived := make(chan struct{})
	var commentsOnce, draftsOnce sync.Once

	server.OnJSON("GET", "/changes/12345/revisions/current/files*", http.StatusOK, map[string]any{})
	server.On("GET", "/changes/12345/comments", func(w http.ResponseWriter, r *http.Request) {
		commentsOnce.Do(func() { close(commentsArrived) })
		server.RespondJSON(w, http.StatusOK, map[string]any{
			"pw_ghish/view.go": []map[string]any{
				{
					"id":         "c1",
					"line":       10,
					"patch_set":  1,
					"unresolved": true,
					"author":     map[string]any{"name": "Reviewer"},
					"message":    "Wave 1 comment",
				},
			},
		})
	})
	server.On("GET", "/changes/12345/drafts", func(w http.ResponseWriter, r *http.Request) {
		draftsOnce.Do(func() { close(draftsArrived) })
		server.RespondJSON(w, http.StatusOK, map[string]any{
			"pw_ghish/view.go": []map[string]any{
				{
					"id":         "d1",
					"line":       20,
					"patch_set":  1,
					"unresolved": true,
					"message":    "Wave 1 draft",
				},
			},
		})
	})
	server.On("GET", "/changes/12345", func(w http.ResponseWriter, r *http.Request) {
		wantOpts := []string{"DETAILED_LABELS", "ALL_REVISIONS", "CURRENT_COMMIT", "DETAILED_ACCOUNTS"}
		if diff := cmp.Diff(wantOpts, r.URL.Query()["o"]); diff != "" {
			t.Errorf("options mismatch (-want +got):\n%s", diff)
		}
		timeout := time.NewTimer(2 * time.Second)
		defer timeout.Stop()
		select {
		case <-commentsArrived:
		case <-timeout.C:
			t.Errorf("ListChangeComments did not arrive concurrently with GetChange in Wave 1")
		}
		select {
		case <-draftsArrived:
		case <-timeout.C:
			t.Errorf("ListChangeDrafts did not arrive concurrently with GetChange in Wave 1")
		}
		server.RespondJSON(w, http.StatusOK, map[string]any{
			"project":          "pigweed/pigweed",
			"_number":          12345,
			"subject":          "Wave 1 Prefetch Subject",
			"status":           "NEW",
			"current_revision": "rev1",
			"revisions": map[string]any{
				"rev1": map[string]any{"_number": 1},
			},
		})
	})

	output, err := executeCommand(RootCmd, "pr", "view", "12345", "--comments")
	if err != nil {
		t.Fatalf("pr view --comments failed: %v\nOutput: %s", err, output)
	}
	if !strings.Contains(output, "Wave 1 comment") || !strings.Contains(output, "Wave 1 draft") {
		t.Errorf("expected Wave 1 comment and draft in output, got:\n%s", output)
	}
}

func TestView_Comments_SuppressesResolvedThreadsAndAllFlag(t *testing.T) {
	registerMockChange := func(srv *MockGerritServer, changeID int, subject string, ps int, comments map[string]any) {
		srv.OnJSON("GET", fmt.Sprintf("/changes/%d/revisions/current/files*", changeID), http.StatusOK, map[string]any{})
		srv.OnComments(changeID, comments)
		srv.OnDrafts(changeID, map[string]any{})
		srv.OnDefaultChange(changeID, WithSubject(subject), WithCurrentPatchset(ps))
	}

	server := NewMockGerritServer(t)
	registerMockChange(server, 12345, "Comment Suppression Subject", 2, map[string]any{
		"a.cc": []map[string]any{
			{
				"id":         "res_1",
				"line":       10,
				"patch_set":  1,
				"unresolved": false,
				"author":     map[string]any{"name": "Reviewer"},
				"message":    "Resolved nit 1",
			},
			{
				"id":         "res_2",
				"line":       20,
				"patch_set":  1,
				"unresolved": false,
				"author":     map[string]any{"name": "Reviewer"},
				"message":    "Resolved nit 2",
			},
			{
				"id":         "unres_1",
				"line":       30,
				"patch_set":  2,
				"unresolved": true,
				"author":     map[string]any{"name": "Reviewer"},
				"message":    "Actionable issue 1",
			},
		},
	})

	t.Run("default --comments omits resolved threads when unresolved exist", func(t *testing.T) {
		output, err := executeCommand(RootCmd, "pr", "view", "12345", "--comments")
		if err != nil {
			t.Fatalf("pr view --comments failed: %v\nOutput: %s", err, output)
		}
		if !strings.Contains(output, "Actionable issue 1") {
			t.Errorf("expected unresolved thread in output, got:\n%s", output)
		}
		if strings.Contains(output, "Resolved nit 1") || strings.Contains(output, "Resolved nit 2") {
			t.Errorf("expected resolved threads to be omitted without --all, got:\n%s", output)
		}
		if !strings.Contains(output, "(✓ 2 resolved thread(s) omitted; use --all to view all)") {
			t.Errorf("expected omitted summary footer in output, got:\n%s", output)
		}
	})

	t.Run("--comments --all expands all resolved threads", func(t *testing.T) {
		output, err := executeCommand(RootCmd, "pr", "view", "12345", "--comments", "--all")
		if err != nil {
			t.Fatalf("pr view --comments --all failed: %v\nOutput: %s", err, output)
		}
		for _, want := range []string{"Resolved nit 1", "Resolved nit 2", "Actionable issue 1"} {
			if !strings.Contains(output, want) {
				t.Errorf("expected %q in --all output, got:\n%s", want, output)
			}
		}
		if strings.Contains(output, "resolved thread(s) omitted") {
			t.Errorf("did not expect omitted notice when --all is passed, got:\n%s", output)
		}
	})

	t.Run("folds unresolved threads beyond 5 when --all is not set", func(t *testing.T) {
		var manyComments []map[string]any
		for i := 1; i <= 7; i++ {
			manyComments = append(manyComments, map[string]any{
				"id":         fmt.Sprintf("u_%d", i),
				"line":       i * 10,
				"patch_set":  1,
				"unresolved": true,
				"author":     map[string]any{"name": "Alice"},
				"message":    fmt.Sprintf("Unresolved comment body %d", i),
			})
		}
		server2 := NewMockGerritServer(t)
		registerMockChange(server2, 22222, "Many Unresolved Subject", 1, map[string]any{
			"many.cc": manyComments,
		})

		output, err := executeCommand(RootCmd, "pr", "view", "22222", "--comments")
		if err != nil {
			t.Fatalf("pr view --comments failed: %v\nOutput: %s", err, output)
		}
		if !strings.Contains(output, "• many.cc:60 [PS1] Alice: Unresolved comment body 6") {
			t.Errorf("expected folded 6th thread summary, got:\n%s", output)
		}
		if !strings.Contains(output, "(use --all to expand all 7 unresolved threads)") {
			t.Errorf("expected expand notice for 7 unresolved threads, got:\n%s", output)
		}
	})
}

func TestView_VerdictAndNextSteps(t *testing.T) {
	tests := []struct {
		name        string
		change      *gerrit.ChangeInfo
		checks      string
		comments    map[string][]gerrit.CommentInfo
		fetched     bool
		wantVerdict string
		wantNext    string
	}{
		{
			name:        "failing checks",
			change:      &gerrit.ChangeInfo{Number: 1002},
			checks:      "CI failed: pw-presubmit",
			wantVerdict: "BLOCKED (CI failed)",
			wantNext:    "gh run view 1002 --log-failed",
		},
		{
			name:        "unresolved comments",
			change:      &gerrit.ChangeInfo{Number: 1004},
			comments:    map[string][]gerrit.CommentInfo{"a.cc": {{Unresolved: boolPtr(true)}}},
			fetched:     true,
			wantVerdict: "IN REVIEW (waiting on Code-Review +2)",
			wantNext:    "gh pr view 1004 --comments",
		},
		{
			name:        "self drafts",
			change:      &gerrit.ChangeInfo{Number: 1003, WorkInProgress: true},
			wantVerdict: "WIP",
			wantNext:    "gh pr ready 1003",
		},
		{
			name:        "submittable",
			change:      &gerrit.ChangeInfo{Number: 1001, Submittable: true},
			wantVerdict: "READY (submittable)",
			wantNext:    "gh pr merge 1001 --auto",
		},
		{
			name:        "in-review fallback to UnresolvedCommentCount",
			change:      &gerrit.ChangeInfo{Number: 1005, UnresolvedCommentCount: 2},
			wantVerdict: "IN REVIEW (waiting on Code-Review +2)",
			wantNext:    "gh pr view 1005 --comments",
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			gotVerdict, gotNext := computeChangeVerdict(tt.change, tt.checks, tt.comments, tt.fetched)
			if gotVerdict != tt.wantVerdict {
				t.Errorf("computeChangeVerdict() verdict = %q, want %q", gotVerdict, tt.wantVerdict)
			}
			if gotNext != tt.wantNext {
				t.Errorf("computeChangeVerdict() next = %q, want %q", gotNext, tt.wantNext)
			}
		})
	}
}

func TestView_Wave1CancelsCommentsAndDraftsOnGetChange404(t *testing.T) {
	server := NewMockGerritServer(t)
	commentsStarted := make(chan struct{})
	commentsCanceled := make(chan struct{})
	var startOnce sync.Once

	server.On("GET", "/changes/99999/comments", func(w http.ResponseWriter, r *http.Request) {
		startOnce.Do(func() { close(commentsStarted) })
		<-r.Context().Done()
		close(commentsCanceled)
	})
	server.On("GET", "/changes/99999/drafts", func(w http.ResponseWriter, r *http.Request) {
		<-r.Context().Done()
	})
	server.On("GET", "/changes/99999*", func(w http.ResponseWriter, r *http.Request) {
		<-commentsStarted
		http.Error(w, "Not found", http.StatusNotFound)
	})

	_, err := executeCommand(RootCmd, "pr", "view", "99999", "--comments")
	if err == nil {
		t.Fatal("expected error when GetChange returns 404")
	}
	select {
	case <-commentsCanceled:
	case <-time.After(2 * time.Second):
		t.Fatal("expected Wave 1 ListChangeComments context to be canceled when GetChange fails")
	}
}
