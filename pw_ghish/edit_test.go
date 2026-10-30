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
	"errors"
	"maps"
	"net/http"
	"strings"
	"testing"

	"github.com/google/go-cmp/cmp"
)

func TestEdit_HelpHidesCommitMessageFlags(t *testing.T) {
	cmd := newEditCmd()
	for _, name := range []string{"title", "body", "body-file", "bug", "fixed"} {
		f := cmd.Flags().Lookup(name)
		if f == nil {
			t.Fatalf("expected hidden unsupported-guard flag --%s to be registered on edit command", name)
		}
		if !f.Hidden {
			t.Errorf("expected flag --%s on edit command to be hidden", name)
		}
	}

	helpOut, err := executeCommand(RootCmd, "pr", "edit", "--help")
	if err != nil {
		t.Fatalf("pr edit --help failed: %v", err)
	}
	for _, hidden := range []string{"--title", "--body", "--body-file", "--bug", "--fixed"} {
		if strings.Contains(helpOut, hidden) {
			t.Errorf("expected pr edit --help to hide %s, got:\n%s", hidden, helpOut)
		}
	}
	for _, visible := range []string{
		"--add-reviewer",
		"--add-owner",
		"--remove-reviewer",
		"--add-assignee",
		"--remove-assignee",
		"--add-attention",
		"--remove-attention",
		"--add-label",
		"--trigger",
		"--cq",
		"--topic",
		"--remove-topic",
		"--add-hashtag",
		"--remove-hashtag",
	} {
		if !strings.Contains(helpOut, visible) {
			t.Errorf("expected pr edit --help to advertise %s, got:\n%s", visible, helpOut)
		}
	}
}

func TestEdit_ErrorWhenNoFlagsProvided(t *testing.T) {
	_, err := executeCommand(RootCmd, "pr", "edit", "12345")
	if err == nil {
		t.Fatal("Expected error when no edit flags provided, got nil")
	}
	if !strings.Contains(err.Error(), "at least one of") {
		t.Errorf("Expected error message about missing flags, got: %v", err)
	}
	if !strings.Contains(err.Error(), "Commit-Queue=1") {
		t.Errorf("Expected error message to include examples, got: %v", err)
	}
	for _, flag := range []string{"--add-attention", "--remove-attention"} {
		if !strings.Contains(err.Error(), flag) {
			t.Errorf("Expected error message to list %s, got: %v", flag, err)
		}
	}
	for _, hidden := range []string{"--title", "--body", "--body-file", "--bug", "--fixed"} {
		if strings.Contains(err.Error(), hidden) {
			t.Errorf("Expected bare pr edit error to omit %s, got:\n%v", hidden, err)
		}
	}
}

func TestEdit_InvalidLabel(t *testing.T) {
	tests := []struct {
		label   string
		wantErr string
	}{
		{"InvalidFormat", "must be in the format 'Name+Score', 'Name-Score', or 'Name=Score'"},
		{"NotAValidLabel", "must be in the format 'Name+Score', 'Name-Score', or 'Name=Score'"},
		{"+1", "must be in the format 'Name+Score', 'Name-Score', or 'Name=Score'"},
		{"-1", "must be in the format 'Name+Score', 'Name-Score', or 'Name=Score'"},
		{"=1", "must be in the format 'Name+Score', 'Name-Score', or 'Name=Score'"},
		{"   =1", "must be in the format 'Name+Score', 'Name-Score', or 'Name=Score'"},
		{"+Commit-Queue+1", "must be in the format 'Name+Score', 'Name-Score', or 'Name=Score'"},
		{"-Commit-Queue+1", "must be in the format 'Name+Score', 'Name-Score', or 'Name=Score'"},
		{"Foo--1", "must be in the format 'Name+Score', 'Name-Score', or 'Name=Score'"},
		{"Commit-Queue=ready", "score must be an integer"},
		{"Commit-Queue= 1", "score must be an integer"},
	}

	for _, tc := range tests {
		t.Run(tc.label, func(t *testing.T) {
			server := NewMockGerritServer(t)
			server.OnJSON("POST", "/changes/12345/reviewers", http.StatusOK, map[string]any{})
			server.OnReview(12345)

			_, err := executeCommand(
				RootCmd,
				"pr", "edit", "12345",
				"--add-reviewer", "reviewer@google.com",
				"--add-label", tc.label,
				"--host", server.URL,
			)
			if err == nil {
				t.Fatalf("expected error for malformed --add-label %q, got nil", tc.label)
			}
			if !strings.Contains(err.Error(), tc.wantErr) {
				t.Errorf("label %q: expected error containing %q, got %v", tc.label, tc.wantErr, err)
			}
			if got := len(server.Requests()); got != 0 {
				t.Errorf("expected zero Gerrit HTTP requests on invalid label %q, got %d (%+v)", tc.label, got, server.Requests())
			}
		})
	}
}

func TestEditAddLabel(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnReview(12345)

	output, err := executeCommand(
		RootCmd,
		"pr", "edit", "12345",
		"--add-label", "Code-Review-1",
		"--add-label", "Verified=-1",
		"--add-label", "Commit-Queue=+1",
		"--add-label", "Fuchsia-Auto-Submit=1",
	)
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("POST", "/changes/12345/revisions/current/review") != 1 {
		t.Fatal("Expected SetReview API to be called for labels")
	}

	if !strings.Contains(output, "✓ Labels added successfully.") {
		t.Errorf("Unexpected output: %s", output)
	}

	payload := DecodeRequest[ReviewInput](t, server.LastRequest())
	wantLabels := map[string]int{
		"Code-Review":         -1,
		"Verified":            -1,
		"Commit-Queue":        1,
		"Fuchsia-Auto-Submit": 1,
	}
	if !maps.Equal(payload.Labels, wantLabels) {
		t.Errorf("labels = %v, want %v", payload.Labels, wantLabels)
	}
}

func TestEditAddMultipleReviewers(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("", "/changes/12345/reviewers", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "edit", "12345", "--add-reviewer", "adamperry@google.com", "--add-reviewer", "jamesr@google.com")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("", "/changes/12345/reviewers") != 2 {
		t.Errorf("Expected 2 calls to AddReviewer API, got %d", server.CallCount("", "/changes/12345/reviewers"))
	}

	if !strings.Contains(output, "Reviewer added successfully") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestEditRemoveMultipleReviewers(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("DELETE", "/changes/12345/reviewers*", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "edit", "12345", "--remove-reviewer", "adamperry@google.com", "--remove-reviewer", "jamesr@google.com")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("DELETE", "") != 2 {
		t.Errorf("Expected 2 calls to DeleteReviewer API, got %d", server.CallCount("DELETE", ""))
	}

	if !strings.Contains(output, "Reviewer removed successfully") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestEditAddAssignee(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/12345/reviewers", http.StatusOK, map[string]any{})
	server.OnReview(12345)

	output, err := executeCommand(RootCmd, "pr", "edit", "12345", "--add-assignee", "helper@google.com")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("POST", "/changes/12345/reviewers") != 1 {
		t.Errorf("AddReviewer API calls: got %d, want 1", server.CallCount("POST", "/changes/12345/reviewers"))
	}
	if server.CallCount("POST", "/changes/12345/revisions/current/review") != 1 {
		t.Errorf("SetReview (Attention Set) API calls: got %d, want 1", server.CallCount("POST", "/changes/12345/revisions/current/review"))
	}

	if !strings.Contains(output, "Assignee added successfully") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestEditAddAttention(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/12345/attention", http.StatusOK, map[string]any{"_account_id": 1})

	output, err := executeCommand(RootCmd, "pr", "edit", "12345",
		"--add-attention", "helper@google.com", "--add-attention", "other@google.com")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	var users []string
	for _, req := range server.Requests() {
		if req.Method != "POST" || req.Path != "/changes/12345/attention" {
			continue
		}
		input := DecodeRequest[AttentionSetInput](t, &req)
		if input.Reason == "" {
			t.Errorf("attention set input for %q has no reason; Gerrit requires one", input.User)
		}
		users = append(users, input.User)
	}
	if got, want := strings.Join(users, ","), "helper@google.com,other@google.com"; got != want {
		t.Errorf("users added to attention set = %q, want %q", got, want)
	}
	// The dedicated endpoint must be used, not a review post, so that no
	// change message or automatic attention set rules are triggered.
	if n := server.CallCount("POST", "/changes/12345/revisions/current/review"); n != 0 {
		t.Errorf("SetReview called %d times, want 0", n)
	}
	if !strings.Contains(output, "Added helper@google.com to the attention set") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestEditRemoveAttention(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnString("DELETE", "/changes/12345/attention/*", http.StatusNoContent, "", "")

	output, err := executeCommand(RootCmd, "pr", "edit", "12345", "--remove-attention", "helper@google.com")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}
	if n := server.CallCount("DELETE", "/changes/12345/attention/helper@google.com"); n != 1 {
		t.Errorf("RemoveAttention calls = %d, want 1; requests: %+v", n, server.Requests())
	}
	// Removing someone from the attention set must not remove them as a
	// reviewer, which is what --remove-assignee does.
	if n := server.CallCount("DELETE", "/changes/12345/reviewers/helper@google.com"); n != 0 {
		t.Errorf("DeleteReviewer called %d times, want 0", n)
	}
	if !strings.Contains(output, "Removed helper@google.com from the attention set") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestEditAttentionMeMeansSelf(t *testing.T) {
	for _, alias := range []string{"me", "@me", "self"} {
		t.Run(alias, func(t *testing.T) {
			server := NewMockGerritServer(t)
			server.OnJSON("POST", "/changes/12345/attention", http.StatusOK, map[string]any{"_account_id": 1})
			server.OnString("DELETE", "/changes/12345/attention/*", http.StatusNoContent, "", "")

			if output, err := executeCommand(RootCmd, "pr", "edit", "12345", "--remove-attention", alias); err != nil {
				t.Fatalf("--remove-attention %s failed: %v\nOutput: %s", alias, err, output)
			}
			if n := server.CallCount("DELETE", "/changes/12345/attention/self"); n != 1 {
				t.Errorf("--remove-attention %s: DELETE .../attention/self calls = %d, want 1; requests: %+v", alias, n, server.Requests())
			}

			if output, err := executeCommand(RootCmd, "pr", "edit", "12345", "--add-attention", alias); err != nil {
				t.Fatalf("--add-attention %s failed: %v\nOutput: %s", alias, err, output)
			}
			req := server.LastRequest()
			if req == nil || req.Path != "/changes/12345/attention" {
				t.Fatalf("--add-attention %s: last request = %+v, want POST /changes/12345/attention", alias, req)
			}
			if got := DecodeRequest[AttentionSetInput](t, req).User; got != "self" {
				t.Errorf("--add-attention %s: user = %q, want %q", alias, got, "self")
			}
		})
	}
}

func TestEditAttentionErrorNamesAccount(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnString("POST", "/changes/12345/attention", http.StatusBadRequest, "text/plain", "nobody@google.com is not a reviewer")

	_, err := executeCommand(RootCmd, "pr", "edit", "12345", "--add-attention", "nobody@google.com")
	if err == nil {
		t.Fatal("expected an error when Gerrit rejects the attention set update")
	}
	if !strings.Contains(err.Error(), "nobody@google.com") {
		t.Errorf("error should name the account, got: %v", err)
	}
}

func TestEditAttentionRejectsEmptyAccount(t *testing.T) {
	NewMockGerritServer(t)
	for _, flag := range []string{"--add-attention", "--remove-attention"} {
		_, err := executeCommand(RootCmd, "pr", "edit", "12345", flag, " ")
		if err == nil || !strings.Contains(err.Error(), flag) {
			t.Errorf("%s with an empty account: expected an error naming the flag, got %v", flag, err)
		}
	}
}

func TestEdit_SetTopic(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("PUT", "/changes/12345/topic", http.StatusOK, `"my-feature-topic"`)

	output, err := executeCommand(RootCmd, "pr", "edit", "12345", "--topic", "my-feature-topic", "--host", server.URL)
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("PUT", "/changes/12345/topic") != 1 {
		t.Errorf("Expected SetTopic API to be called, got calls: %v", server.Requests())
	}
	if !strings.Contains(output, "✓ Topic set to \"my-feature-topic\" successfully.") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestEdit_RemoveTopic(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnStatus(http.StatusNoContent)

	output, err := executeCommand(RootCmd, "pr", "edit", "12345", "--remove-topic", "--host", server.URL)
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("DELETE", "/changes/12345/topic") != 1 {
		t.Errorf("Expected DeleteTopic API to be called, got calls: %v", server.Requests())
	}
	if !strings.Contains(output, "✓ Topic removed successfully.") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestEdit_Hashtags(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/12345/hashtags", http.StatusOK, []string{"feature", "triage"})

	output, err := executeCommand(RootCmd, "pr", "edit", "12345", "--add-hashtag", "feature", "--add-hashtag", "triage", "--remove-hashtag", "legacy", "--host", server.URL)
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("POST", "/changes/12345/hashtags") != 1 {
		t.Errorf("Expected SetHashtags API to be called, got calls: %v", server.Requests())
	}
	if !strings.Contains(output, "✓ Hashtags updated successfully.") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestEdit_CQAndTrigger(t *testing.T) {
	tests := []struct {
		name         string
		changeLabels []string
		args         []string
		wantOut      string
		wantLabels   map[string]int
	}{
		{
			name:         "cq default",
			changeLabels: []string{"Code-Review", "Commit-Queue"},
			args:         []string{"--cq"},
			wantOut:      "Commit-Queue+1 set successfully.",
			wantLabels:   map[string]int{"Commit-Queue": 1},
		},
		{
			name:         "cq explicit",
			changeLabels: []string{"Code-Review", "Commit-Queue"},
			args:         []string{"--cq", "2"},
			wantOut:      "Commit-Queue+2 set successfully.",
			wantLabels:   map[string]int{"Commit-Queue": 2},
		},
		{
			name:         "cq remove",
			changeLabels: []string{"Code-Review", "Commit-Queue"},
			args:         []string{"--cq", "0"},
			wantOut:      "Commit-Queue vote removed successfully.",
			wantLabels:   map[string]int{"Commit-Queue": 0},
		},
		{
			name:         "cq falls back to Commit-Queue when change lookup 404s",
			changeLabels: nil,
			args:         []string{"--cq"},
			wantOut:      "Commit-Queue+1 set successfully.",
			wantLabels:   map[string]int{"Commit-Queue": 1},
		},
		{
			name:         "cq uses Presubmit-Ready when no Commit-Queue",
			changeLabels: []string{"Code-Review", "Presubmit-Ready"},
			args:         []string{"--cq"},
			wantOut:      "Presubmit-Ready+1 set successfully.",
			wantLabels:   map[string]int{"Presubmit-Ready": 1},
		},
		{
			name:         "trigger default",
			changeLabels: []string{"Code-Review", "Presubmit-Ready"},
			args:         []string{"--trigger"},
			wantOut:      "Presubmit-Ready+1 set successfully.",
			wantLabels:   map[string]int{"Presubmit-Ready": 1},
		},
		{
			name:         "trigger explicit",
			changeLabels: []string{"Code-Review", "Commit-Queue"},
			args:         []string{"--trigger=2"},
			wantOut:      "Commit-Queue+2 set successfully.",
			wantLabels:   map[string]int{"Commit-Queue": 2},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			server := NewMockGerritServer(t)
			if len(tt.changeLabels) > 0 {
				server.OnDefaultChange(12345, WithLabels(tt.changeLabels...))
			}
			server.OnReview(12345)

			output, err := executeCommand(RootCmd, append([]string{"pr", "edit", "12345"}, tt.args...)...)
			if err != nil {
				t.Fatalf("Command failed: %v\nOutput: %s", err, output)
			}
			if !strings.Contains(output, tt.wantOut) {
				t.Errorf("Unexpected output: %s, want %q", output, tt.wantOut)
			}
			server.AssertOptions(t, "GET", "/changes/12345", "DETAILED_LABELS")
			payload := DecodeRequest[ReviewInput](t, server.LastRequest())
			if diff := cmp.Diff(tt.wantLabels, payload.Labels); diff != "" {
				t.Errorf("labels mismatch (-want +got):\n%s", diff)
			}
		})
	}
}

type failingStdinReader struct {
	readCalled bool
}

func (r *failingStdinReader) Read([]byte) (int, error) {
	r.readCalled = true
	return 0, errors.New("unexpected read from stdin")
}

func TestEdit_CommitMessageFlagsGuard(t *testing.T) {
	tests := []struct {
		name      string
		args      []string
		wantSub   []string
		forbidSub []string
	}{
		{"no target", []string{"pr", "edit", "-t", "pw_foo: Title", "--bug", "b/123456"}, []string{"gh pr push", "gh pr create"}, []string{"gh pr checkout", "COMMIT_EDITMSG_TMP"}},
		{"with target", []string{"pr", "edit", "12345\x1b[31m", "-b", "Updated body", "--fixed", "b/123456"}, []string{"gh pr checkout 12345", "gh pr push", "gh pr create"}, []string{"\x1b"}},
		{"body-file", []string{"pr", "edit", "-F", "nonexistent-body.txt"}, []string{"gh pr push", "gh pr create"}, nil},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			server := NewMockGerritServer(t)
			SetupMockConfig(t, &MockGitRunner{})
			_, err := executeCommand(RootCmd, tt.args...)
			if err == nil {
				t.Fatalf("Expected error for %v, got nil", tt.args)
			}
			if len(server.Requests()) != 0 {
				t.Errorf("Expected zero Gerrit HTTP requests, got %d: %+v", len(server.Requests()), server.Requests())
			}
			for _, want := range tt.wantSub {
				if !strings.Contains(err.Error(), want) {
					t.Errorf("Expected error to contain %q, got:\n%s", want, err.Error())
				}
			}
			for _, forbid := range tt.forbidSub {
				if strings.Contains(err.Error(), forbid) {
					t.Errorf("Expected error not to contain %q, got:\n%s", forbid, err.Error())
				}
			}
		})
	}
}

func TestEdit_BodyFileStdinDoesNotReadStdin(t *testing.T) {
	server := NewMockGerritServer(t)
	SetupMockConfig(t, &MockGitRunner{})
	stdin := &failingStdinReader{}

	_, err := executeCommandWithStdin(RootCmd, stdin, "pr", "edit", "123", "-F", "-")
	if err == nil {
		t.Fatal("Expected disabled-flag guard error for 'pr edit 123 -F -', got nil")
	}
	if stdin.readCalled {
		t.Error("Expected 'pr edit 123 -F -' not to read from stdin")
	}
	if len(server.Requests()) != 0 {
		t.Errorf("Expected zero Gerrit HTTP requests, got %d: %+v", len(server.Requests()), server.Requests())
	}
	for _, want := range []string{"cannot update commit messages", "gh pr checkout 123", "gh pr push"} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("Expected error to contain %q, got:\n%s", want, err.Error())
		}
	}
}

func TestEdit_TopicForbiddenBySubmitRequirement(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, func(ch map[string]any) {
		ch["submit_requirements"] = []map[string]any{
			{"name": "Topics-Not-Supported", "status": "SATISFIED"},
		}
	})

	_, err := executeCommand(RootCmd, "pr", "edit", "12345", "--topic", "my-topic", "--host", server.URL)
	if err == nil {
		t.Fatal("Expected error when --topic is passed on a change with Topics-Not-Supported submit requirement")
	}
	for _, want := range []string{"Topics-Not-Supported", "my-topic", "without --topic"} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("Expected error to contain %q, got: %v", want, err)
		}
	}
	if server.CallCount("PUT", "/changes/12345/topic") != 0 {
		t.Errorf("Expected no SetTopic API call when topic is forbidden")
	}
}

func TestNewEditCmd_IsolatesOptionsAcrossInstances(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345)
	server.OnJSON("POST", "/changes/12345/hashtags", http.StatusOK, []string{"alpha"})
	server.OnJSON("PUT", "/changes/12345/topic", http.StatusOK, `"first-topic"`)
	server.OnReview(12345)

	cmd1 := newEditCmd()
	cmd1.SetArgs([]string{"12345", "--add-hashtag", "alpha", "--topic", "first-topic"})
	if err := cmd1.Execute(); err != nil {
		t.Fatalf("first edit invocation failed: %v", err)
	}

	cmd2 := newEditCmd()
	cmd2.SetArgs([]string{"12345", "--cq"})
	if err := cmd2.Execute(); err != nil {
		t.Fatalf("second edit invocation failed: %v", err)
	}

	if n := server.CallCount("POST", "/changes/12345/hashtags"); n != 1 {
		t.Errorf("expected hashtags endpoint called only once, got %d", n)
	}
	if n := server.CallCount("PUT", "/changes/12345/topic"); n != 1 {
		t.Errorf("expected topic endpoint called only once, got %d", n)
	}
	if n := server.CallCount("POST", "/changes/12345/revisions/current/review"); n != 1 {
		t.Errorf("expected review endpoint called once on second command, got %d", n)
	}
}
