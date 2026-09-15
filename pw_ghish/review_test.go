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
	"strings"
	"testing"

	"github.com/andygrunwald/go-gerrit"
)

func TestReviewCmd_Approve(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/123/revisions/current/review", http.StatusOK, map[string]any{})

	cmd := newReviewCmd()
	cmd.SetContext(context.Background())
	cmd.SetArgs([]string{"123", "--approve", "-m", "Looks good"})
	if err := cmd.Execute(); err != nil {
		t.Fatalf("cmd.Execute failed: %v", err)
	}

	var capturedInput gerrit.ReviewInput
	if req := server.LastRequest(); req != nil {
		json.Unmarshal(req.Body, &capturedInput)
	}
	if capturedInput.Message != "Looks good" {
		t.Errorf("got message %q, want %q", capturedInput.Message, "Looks good")
	}
	if capturedInput.Labels["Code-Review"] != 2 {
		t.Errorf("got Code-Review %d, want 2", capturedInput.Labels["Code-Review"])
	}
}

func TestReviewCmd_RequestChanges(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/456/revisions/current/review", http.StatusOK, map[string]any{})

	cmd := newReviewCmd()
	cmd.SetContext(context.Background())
	cmd.SetArgs([]string{"456", "--request-changes", "-m", "Needs fixes"})
	if err := cmd.Execute(); err != nil {
		t.Fatalf("cmd.Execute failed: %v", err)
	}

	var capturedInput gerrit.ReviewInput
	if req := server.LastRequest(); req != nil {
		json.Unmarshal(req.Body, &capturedInput)
	}
	if capturedInput.Message != "Needs fixes" {
		t.Errorf("got message %q, want %q", capturedInput.Message, "Needs fixes")
	}
	if capturedInput.Labels["Code-Review"] != -1 {
		t.Errorf("got Code-Review %d, want -1", capturedInput.Labels["Code-Review"])
	}
}

func TestReviewCmd_ErrorWhenBothApproveAndRequestChanges(t *testing.T) {
	cmd := newReviewCmd()
	cmd.SetContext(context.Background())
	cmd.SetArgs([]string{"123", "--approve", "--request-changes"})
	err := cmd.Execute()
	if err == nil {
		t.Fatal("Expected error when both --approve and --request-changes are specified, got nil")
	}
}

func TestReviewCmd_DefaultActivePR(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/I1234567890abcdef1234567890abcdef12345678/revisions/current/review", http.StatusOK, map[string]any{})

	mockGit := SetMockGit(t, &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) >= 2 && args[0] == "branch" && args[1] == "--show-current" {
				stdout.Write([]byte("my-feature\n"))
				return nil
			}
			if len(args) >= 3 && args[0] == "log" && args[1] == "-1" {
				stdout.Write([]byte("Commit on feature branch\n\nChange-Id: I1234567890abcdef1234567890abcdef12345678\n"))
				return nil
			}
			return fmt.Errorf("not handled")
		},
	})

	cmd := newReviewCmd()
	cmd.SetContext(context.Background())
	SetConfig(cmd, &Config{Git: mockGit})
	cmd.SetArgs([]string{"--approve", "-m", "LGTM from active branch"})

	if err := cmd.Execute(); err != nil {
		t.Fatalf("cmd.Execute failed: %v", err)
	}

	req := server.LastRequest()
	if req == nil || req.Path != "/changes/I1234567890abcdef1234567890abcdef12345678/revisions/current/review" {
		t.Errorf("got path %v, want /changes/I1234567890abcdef1234567890abcdef12345678/revisions/current/review", req)
	}
	var capturedInput gerrit.ReviewInput
	if req != nil {
		json.Unmarshal(req.Body, &capturedInput)
	}
	if capturedInput.Message != "LGTM from active branch" {
		t.Errorf("got message %q, want %q", capturedInput.Message, "LGTM from active branch")
	}
	if capturedInput.Labels["Code-Review"] != 2 {
		t.Errorf("got Code-Review %d, want 2", capturedInput.Labels["Code-Review"])
	}
}

func TestReviewCmd_SpecificPatchset(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/472267/revisions/3/review", http.StatusOK, map[string]any{})

	cmd := newReviewCmd()
	cmd.SetContext(context.Background())
	cmd.SetArgs([]string{"472267/3", "--approve"})

	if err := cmd.Execute(); err != nil {
		t.Fatalf("cmd.Execute failed: %v", err)
	}

	if server.CallCount("POST", "/changes/472267/revisions/3/review") != 1 {
		t.Errorf("expected 1 call to /changes/472267/revisions/3/review, got %d", server.CallCount("POST", "/changes/472267/revisions/3/review"))
	}
}

func TestReviewCmd_URL(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/472267/revisions/4/review", http.StatusOK, map[string]any{})

	cmd := newReviewCmd()
	cmd.SetContext(context.Background())
	cmd.SetArgs([]string{"https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/472267/4", "--approve"})

	if err := cmd.Execute(); err != nil {
		t.Fatalf("cmd.Execute failed: %v", err)
	}

	if server.CallCount("POST", "/changes/472267/revisions/4/review") != 1 {
		t.Errorf("expected 1 call to /changes/472267/revisions/4/review, got %d", server.CallCount("POST", "/changes/472267/revisions/4/review"))
	}
}

func TestReviewCmd_ErrorWhenNoFlagsSpecified(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/123", http.StatusOK, map[string]any{
		"_number": 123,
	})

	cmd := newReviewCmd()
	cmd.SetContext(context.Background())
	cmd.SetArgs([]string{"123"})

	err := cmd.Execute()
	if err == nil {
		t.Fatal("expected error when running review with no flags, got nil")
	}
	if !strings.Contains(err.Error(), "no review action or message specified") {
		t.Errorf("expected error to mention 'no review action or message specified', got: %v", err)
	}
	if !strings.Contains(err.Error(), "--approve") {
		t.Errorf("expected error to provide debug crumbs with '--approve', got: %v", err)
	}
}

func TestReviewCmd_CommentFlagWithMessage(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/123/revisions/current/review", http.StatusOK, map[string]any{})

	cmd := newReviewCmd()
	cmd.SetContext(context.Background())
	cmd.SetArgs([]string{"123", "--comment", "-m", "Just a question"})

	if err := cmd.Execute(); err != nil {
		t.Fatalf("cmd.Execute failed: %v", err)
	}

	var capturedInput gerrit.ReviewInput
	if req := server.LastRequest(); req != nil {
		json.Unmarshal(req.Body, &capturedInput)
	}
	if capturedInput.Message != "Just a question" {
		t.Errorf("got message %q, want %q", capturedInput.Message, "Just a question")
	}
	if len(capturedInput.Labels) != 0 {
		t.Errorf("expected no label votes with --comment, got: %v", capturedInput.Labels)
	}
}

func TestReviewCmd_ErrorWhenCommentWithoutMessage(t *testing.T) {
	cmd := newReviewCmd()
	cmd.SetContext(context.Background())
	cmd.SetArgs([]string{"123", "--comment"})

	err := cmd.Execute()
	if err == nil {
		t.Fatal("expected error when running review with --comment but no message, got nil")
	}
	if !strings.Contains(err.Error(), "--comment requires a message") {
		t.Errorf("expected error to mention '--comment requires a message', got: %v", err)
	}
}

func TestReviewCmd_ErrorWhenMultipleActions(t *testing.T) {
	cmd := newReviewCmd()
	cmd.SetContext(context.Background())
	cmd.SetArgs([]string{"123", "--approve", "--comment", "-m", "hi"})

	err := cmd.Execute()
	if err == nil {
		t.Fatal("expected error when combining --approve and --comment, got nil")
	}
	if !strings.Contains(err.Error(), "cannot specify more than one") {
		t.Errorf("expected error to mention 'cannot specify more than one', got: %v", err)
	}
}

func TestReviewCmd_CQ_Default(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/123/revisions/current/review", http.StatusOK, map[string]any{})

	cmd := newReviewCmd()
	cmd.SetContext(context.Background())
	cmd.SetArgs(NormalizeCQArgs([]string{"123", "--cq"}))
	if err := cmd.Execute(); err != nil {
		t.Fatalf("cmd.Execute failed: %v", err)
	}

	var capturedInput gerrit.ReviewInput
	if req := server.LastRequest(); req != nil {
		json.Unmarshal(req.Body, &capturedInput)
	}
	if capturedInput.Labels["Commit-Queue"] != 1 {
		t.Errorf("got Commit-Queue = %d, want 1", capturedInput.Labels["Commit-Queue"])
	}
}

func TestReviewCmd_ApproveAndCQ(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/123/revisions/current/review", http.StatusOK, map[string]any{})

	cmd := newReviewCmd()
	cmd.SetContext(context.Background())
	cmd.SetArgs(NormalizeCQArgs([]string{"123", "--approve", "--cq", "-m", "LGTM"}))
	if err := cmd.Execute(); err != nil {
		t.Fatalf("cmd.Execute failed: %v", err)
	}

	var capturedInput gerrit.ReviewInput
	if req := server.LastRequest(); req != nil {
		json.Unmarshal(req.Body, &capturedInput)
	}
	if capturedInput.Labels["Code-Review"] != 2 {
		t.Errorf("got Code-Review = %d, want 2", capturedInput.Labels["Code-Review"])
	}
	if capturedInput.Labels["Commit-Queue"] != 1 {
		t.Errorf("got Commit-Queue = %d, want 1", capturedInput.Labels["Commit-Queue"])
	}
}

func TestReviewCmd_CQ_Explicit(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/123/revisions/current/review", http.StatusOK, map[string]any{})

	cmd := newReviewCmd()
	cmd.SetContext(context.Background())
	cmd.SetArgs(NormalizeCQArgs([]string{"123", "--cq", "2"}))
	if err := cmd.Execute(); err != nil {
		t.Fatalf("cmd.Execute failed: %v", err)
	}

	var capturedInput gerrit.ReviewInput
	if req := server.LastRequest(); req != nil {
		json.Unmarshal(req.Body, &capturedInput)
	}
	if capturedInput.Labels["Commit-Queue"] != 2 {
		t.Errorf("got Commit-Queue = %d, want 2", capturedInput.Labels["Commit-Queue"])
	}
}
