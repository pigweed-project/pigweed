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
	"net/http"
	"strings"
	"testing"

	"github.com/google/go-cmp/cmp"
)

func TestReviewCmd_FlagsAndLabels(t *testing.T) {
	tests := []struct {
		name         string
		changeID     int
		changeLabels []string
		args         []string
		wantMessage  string
		wantLabels   map[string]int
		wantDrafts   string
		wantOut      string
	}{
		{
			name:        "approve with message",
			changeID:    123,
			args:        []string{"123", "--approve", "-m", "Looks good"},
			wantMessage: "Looks good",
			wantLabels:  map[string]int{"Code-Review": 2},
			wantDrafts:  "KEEP",
		},
		{
			name:        "request changes with message",
			changeID:    456,
			args:        []string{"456", "--request-changes", "-m", "Needs fixes"},
			wantMessage: "Needs fixes",
			wantLabels:  map[string]int{"Code-Review": -1},
			wantDrafts:  "KEEP",
		},
		{
			name:        "comment flag with message",
			changeID:    123,
			args:        []string{"123", "--comment", "-m", "Just a question"},
			wantMessage: "Just a question",
			wantDrafts:  "KEEP",
		},
		{
			name:         "cq default",
			changeID:     123,
			changeLabels: []string{"Code-Review", "Commit-Queue"},
			args:         NormalizeCQArgs([]string{"123", "--cq"}),
			wantLabels:   map[string]int{"Commit-Queue": 1},
			wantDrafts:   "KEEP",
		},
		{
			name:         "approve and cq",
			changeID:     123,
			changeLabels: []string{"Code-Review", "Commit-Queue"},
			args:         NormalizeCQArgs([]string{"123", "--approve", "--cq", "-m", "LGTM"}),
			wantMessage:  "LGTM",
			wantLabels:   map[string]int{"Code-Review": 2, "Commit-Queue": 1},
			wantDrafts:   "KEEP",
		},
		{
			name:         "cq explicit",
			changeID:     123,
			changeLabels: []string{"Code-Review", "Commit-Queue"},
			args:         NormalizeCQArgs([]string{"123", "--cq", "2"}),
			wantLabels:   map[string]int{"Commit-Queue": 2},
			wantDrafts:   "KEEP",
		},
		{
			name:         "cq falls back to Commit-Queue when change lookup 404s",
			changeID:     123,
			changeLabels: nil,
			args:         NormalizeCQArgs([]string{"123", "--cq"}),
			wantLabels:   map[string]int{"Commit-Queue": 1},
			wantDrafts:   "KEEP",
		},
		{
			name:         "cq uses Presubmit-Ready when no Commit-Queue",
			changeID:     123,
			changeLabels: []string{"Code-Review", "Presubmit-Ready"},
			args:         NormalizeCQArgs([]string{"123", "--cq"}),
			wantLabels:   map[string]int{"Presubmit-Ready": 1},
			wantDrafts:   "KEEP",
		},
		{
			name:       "publish drafts",
			changeID:   123,
			args:       []string{"123", "--publish"},
			wantDrafts: "PUBLISH_ALL_REVISIONS",
			wantOut:    "✓ Review submitted successfully (published pending drafts).",
		},
		{
			name:        "default keeps drafts and supports upstream shorthand flags",
			changeID:    123,
			args:        []string{"123", "-r", "-b", "Please add a unit test"},
			wantMessage: "Please add a unit test",
			wantLabels:  map[string]int{"Code-Review": -1},
			wantDrafts:  "KEEP",
		},
		{
			name:         "trigger",
			changeID:     123,
			changeLabels: []string{"Code-Review", "Presubmit-Ready"},
			args:         NormalizeCQArgs([]string{"123", "--trigger"}),
			wantLabels:   map[string]int{"Presubmit-Ready": 1},
			wantDrafts:   "KEEP",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			server := NewMockGerritServer(t)
			if len(tt.changeLabels) > 0 {
				server.OnDefaultChange(tt.changeID, WithLabels(tt.changeLabels...))
			}
			server.OnReview(tt.changeID)

			var out strings.Builder
			cmd := newReviewCmd()
			cmd.SetContext(t.Context())
			cmd.SetOut(&out)
			cmd.SetArgs(tt.args)
			if err := cmd.Execute(); err != nil {
				t.Fatalf("cmd.Execute failed: %v", err)
			}
			if tt.wantOut != "" && !strings.Contains(out.String(), tt.wantOut) {
				t.Errorf("output = %q, want substring %q", out.String(), tt.wantOut)
			}

			capturedInput := DecodeRequest[ReviewInput](t, server.LastRequest())
			if capturedInput.Message != tt.wantMessage {
				t.Errorf("got message %q, want %q", capturedInput.Message, tt.wantMessage)
			}
			if diff := cmp.Diff(tt.wantLabels, capturedInput.Labels); diff != "" {
				t.Errorf("labels mismatch (-want +got):\n%s", diff)
			}
			if capturedInput.Drafts != tt.wantDrafts {
				t.Errorf("got Drafts = %q, want %q", capturedInput.Drafts, tt.wantDrafts)
			}
		})
	}
}

func TestReviewCmd_ValidationErrors(t *testing.T) {
	tests := []struct {
		name     string
		args     []string
		wantErrs []string
	}{
		{
			name:     "both approve and request-changes",
			args:     []string{"123", "--approve", "--request-changes"},
			wantErrs: []string{"cannot specify more than one"},
		},
		{
			name:     "no flags specified",
			args:     []string{"123"},
			wantErrs: []string{"no review action or message specified", "--approve"},
		},
		{
			name:     "comment without message",
			args:     []string{"123", "--comment"},
			wantErrs: []string{"--comment requires a message"},
		},
		{
			name:     "multiple actions",
			args:     []string{"123", "--approve", "--comment", "-m", "hi"},
			wantErrs: []string{"cannot specify more than one"},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			server := NewMockGerritServer(t)
			server.OnDefaultChange(123)

			cmd := newReviewCmd()
			cmd.SetContext(t.Context())
			cmd.SetArgs(tt.args)
			err := cmd.Execute()
			if err == nil {
				t.Fatalf("expected error for args %v, got nil", tt.args)
			}
			for _, want := range tt.wantErrs {
				if !strings.Contains(err.Error(), want) {
					t.Errorf("expected error to contain %q, got: %v", want, err)
				}
			}
		})
	}
}

func TestReviewCmd_DefaultActivePR(t *testing.T) {
	const changeID = "I1234567890abcdef1234567890abcdef12345678"
	server := NewMockGerritServer(t)
	server.OnReview(changeID)

	mockGit := NewMockGit(t).
		WithBranch("my-feature").
		WithCommit("Commit on feature branch\n\nChange-Id: " + changeID + "\n")

	cmd := newReviewCmd()
	cmd.SetContext(context.Background())
	SetConfig(cmd, &Config{Git: mockGit})
	cmd.SetArgs([]string{"--approve", "-m", "LGTM from active branch"})

	if err := cmd.Execute(); err != nil {
		t.Fatalf("cmd.Execute failed: %v", err)
	}

	req := server.LastRequest()
	if req == nil || req.Path != "/changes/"+changeID+"/revisions/current/review" {
		t.Fatalf("got path %v, want /changes/%s/revisions/current/review", req, changeID)
	}
	capturedInput := DecodeRequest[ReviewInput](t, req)
	if capturedInput.Message != "LGTM from active branch" {
		t.Errorf("got message %q, want %q", capturedInput.Message, "LGTM from active branch")
	}
	if capturedInput.Labels["Code-Review"] != 2 {
		t.Errorf("got Code-Review %d, want 2", capturedInput.Labels["Code-Review"])
	}
}

func TestReviewCmd_TargetResolution(t *testing.T) {
	tests := []struct {
		name     string
		target   string
		wantPath string
	}{
		{
			name:     "specific patchset shorthand",
			target:   "472267/3",
			wantPath: "/changes/472267/revisions/3/review",
		},
		{
			name:     "full Gerrit URL with patchset",
			target:   "https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/472267/4",
			wantPath: "/changes/472267/revisions/4/review",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			server := NewMockGerritServer(t)
			server.OnJSON("POST", tt.wantPath, http.StatusOK, map[string]any{})

			cmd := newReviewCmd()
			cmd.SetContext(t.Context())
			cmd.SetArgs([]string{tt.target, "--approve"})
			if err := cmd.Execute(); err != nil {
				t.Fatalf("cmd.Execute failed: %v", err)
			}
			if server.CallCount("POST", tt.wantPath) != 1 {
				t.Errorf("expected 1 call to %s, got %d", tt.wantPath, server.CallCount("POST", tt.wantPath))
			}
		})
	}
}

func TestReviewCmd_BodyFileStdin(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnReview(123)

	t.Run("reads review message from stdin via -F -", func(t *testing.T) {
		reviewBody := "Detailed review summary:\n- Checked stack usage\n- Verified BUILD.gn parity"
		cmd := newReviewCmd()
		cmd.SetContext(context.Background())
		cmd.SetIn(strings.NewReader(reviewBody))
		cmd.SetArgs([]string{"123", "--approve", "-F", "-"})
		if err := cmd.Execute(); err != nil {
			t.Fatalf("cmd.Execute failed: %v", err)
		}

		capturedInput := DecodeRequest[ReviewInput](t, server.LastRequest())
		if capturedInput.Message != reviewBody {
			t.Errorf("got Message = %q, want %q", capturedInput.Message, reviewBody)
		}
		if capturedInput.Labels["Code-Review"] != 2 {
			t.Errorf("got Code-Review = %d, want 2", capturedInput.Labels["Code-Review"])
		}
	})

	t.Run("--comment with empty stdin fails with clear error", func(t *testing.T) {
		cmd := newReviewCmd()
		cmd.SetContext(context.Background())
		cmd.SetIn(strings.NewReader("  \n"))
		cmd.SetArgs([]string{"123", "--comment", "--body-file", "-"})
		err := cmd.Execute()
		if err == nil || !strings.Contains(err.Error(), "--comment requires a message") {
			t.Fatalf("expected '--comment requires a message' error for empty stdin, got: %v", err)
		}
	})
}

func TestReviewCmd_Agent(t *testing.T) {
	tests := []struct {
		name       string
		agent      bool
		args       []string
		wantAIText bool // message sent as an is_ai patchset-level comment
	}{
		{name: "human", args: []string{"--approve", "-m", "LGTM"}},
		{name: "agent", agent: true, args: []string{"--approve", "-m", "LGTM"}, wantAIText: true},
		{name: "--agent", args: []string{"-m", "LGTM", "--agent"}, wantAIText: true},
		{name: "agent --agent=false", agent: true, args: []string{"-m", "LGTM", "--agent=false"}},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			for _, name := range agentEnvVars {
				t.Setenv(name, "")
			}
			if tc.agent {
				t.Setenv("ANTIGRAVITY_AGENT", "1")
			}
			server := NewMockGerritServer(t)
			server.OnJSON("POST", "/changes/123/revisions/current/review", http.StatusOK, map[string]any{})

			if _, err := executeCommand(RootCmd, append([]string{"pr", "review", "123"}, tc.args...)...); err != nil {
				t.Fatalf("command failed: %v", err)
			}
			var body struct {
				Message  string                      `json:"message"`
				Labels   map[string]int              `json:"labels"`
				Comments map[string][]map[string]any `json:"comments"`
			}
			if err := json.Unmarshal(server.LastRequest().Body, &body); err != nil {
				t.Fatal(err)
			}
			if !tc.wantAIText {
				if body.Message != "LGTM" || body.Comments != nil {
					t.Errorf("want plain message, got message %q comments %v", body.Message, body.Comments)
				}
				return
			}
			c := body.Comments["/PATCHSET_LEVEL"]
			if body.Message != "" || len(c) != 1 || c[0]["message"] != "LGTM" || c[0]["is_ai"] != true {
				t.Errorf("want is_ai patchset-level comment, got message %q comments %v", body.Message, body.Comments)
			}
			if tc.name == "agent" && body.Labels["Code-Review"] != 2 {
				t.Errorf("approve vote lost: labels %v", body.Labels)
			}
		})
	}
}
