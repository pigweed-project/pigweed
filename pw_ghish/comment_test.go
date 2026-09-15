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
	"strings"
	"testing"

	"github.com/andygrunwald/go-gerrit"
)

func TestCommentCmd_InReplyTo(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/123/comments", http.StatusOK, map[string]any{
		"test.txt": []map[string]any{
			{
				"id":      "parent_id",
				"path":    "test.txt",
				"line":    10,
				"message": "existing comment",
				"updated": "2026-04-08 10:00:00.000000000",
			},
		},
	})
	server.OnJSON("POST", "/changes/123/revisions/current/review", http.StatusOK, map[string]any{})

	_, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "test.txt", "--line", "10", "--message", "reply message")
	if err != nil {
		t.Fatalf("Command failed: %v", err)
	}

	req := server.LastRequest()
	if req == nil {
		t.Fatal("No request captured")
	}
	expectedPath := "/changes/123/revisions/current/review"
	if req.Path != expectedPath {
		t.Errorf("Expected path %s, got %s", expectedPath, req.Path)
	}

	var reviewInput gerrit.ReviewInput
	if err := json.Unmarshal(req.Body, &reviewInput); err != nil {
		t.Fatalf("Failed to unmarshal request body: %v", err)
	}

	comments, ok := reviewInput.Comments["test.txt"]
	if !ok || len(comments) == 0 {
		t.Fatal("No comments found for test.txt")
	}

	if comments[0].InReplyTo != "parent_id" {
		t.Errorf("Expected InReplyTo 'parent_id', got %q", comments[0].InReplyTo)
	}
}

func TestCommentCmd_Resolved(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/123/comments", http.StatusOK, map[string]any{
		"test.txt": []map[string]any{
			{
				"id":      "parent_id",
				"path":    "test.txt",
				"line":    10,
				"message": "existing comment",
				"updated": "2026-04-08 10:00:00.000000000",
			},
		},
	})
	server.OnJSON("POST", "/changes/123/revisions/current/review", http.StatusOK, map[string]any{})

	_, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "test.txt", "--line", "10", "--message", "reply message", "--resolved")
	if err != nil {
		t.Fatalf("Command failed: %v", err)
	}

	req := server.LastRequest()
	if req == nil {
		t.Fatal("No request captured")
	}

	var reviewInput gerrit.ReviewInput
	if err := json.Unmarshal(req.Body, &reviewInput); err != nil {
		t.Fatalf("Failed to unmarshal request body: %v", err)
	}

	comments, ok := reviewInput.Comments["test.txt"]
	if !ok || len(comments) == 0 {
		t.Fatal("No comments found for test.txt")
	}

	if comments[0].Unresolved == nil {
		t.Fatal("Expected Unresolved to be set, but got nil")
	}

	if *comments[0].Unresolved != false {
		t.Errorf("Expected Unresolved to be false, got %v", *comments[0].Unresolved)
	}
}

func TestCommentCmd_Patchset(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/123/comments", http.StatusOK, map[string]any{})
	server.OnJSON("POST", "/changes/123/revisions/2/review", http.StatusOK, map[string]any{})

	_, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "test.txt", "--line", "10", "--message", "reply message", "--patchset", "2")
	if err != nil {
		t.Fatalf("Command failed: %v", err)
	}

	req := server.LastRequest()
	if req == nil {
		t.Fatal("No request captured")
	}
	expectedPath := "/changes/123/revisions/2/review"
	if req.Path != expectedPath {
		t.Errorf("Expected path %s, got %s", expectedPath, req.Path)
	}
}

func TestCommentCmd_InReplyTo_EarlierPatchset(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/123/comments", http.StatusOK, map[string]any{
		"test.txt": []map[string]any{
			{
				"id":        "ps1_comment_id",
				"path":      "test.txt",
				"line":      42,
				"message":   "comment on ps1",
				"patch_set": 1,
				"updated":   "2026-04-08 10:00:00.000000000",
			},
		},
	})
	server.OnJSON("POST", "/changes/123/revisions/1/review", http.StatusOK, map[string]any{})

	_, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "test.txt", "--line", "42", "--message", "Fixed in PS2", "--resolved")
	if err != nil {
		t.Fatalf("Command failed: %v", err)
	}

	req := server.LastRequest()
	if req == nil {
		t.Fatal("No request captured")
	}
	expectedPath := "/changes/123/revisions/1/review"
	if req.Path != expectedPath {
		t.Errorf("Expected path %s, got %s", expectedPath, req.Path)
	}

	var reviewInput gerrit.ReviewInput
	if err := json.Unmarshal(req.Body, &reviewInput); err != nil {
		t.Fatalf("Failed to parse body: %v", err)
	}
	comments := reviewInput.Comments["test.txt"]
	if len(comments) != 1 {
		t.Fatalf("Expected 1 comment, got %d", len(comments))
	}
	if comments[0].InReplyTo != "ps1_comment_id" {
		t.Errorf("Expected InReplyTo 'ps1_comment_id', got %q", comments[0].InReplyTo)
	}
	if comments[0].Unresolved == nil || *comments[0].Unresolved != false {
		t.Errorf("Expected Unresolved to be false, got %v", comments[0].Unresolved)
	}
}

func TestCommentCmd_DraftInline(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/123/comments", http.StatusOK, map[string]any{})
	server.OnJSON("PUT", "/changes/123/revisions/current/drafts", http.StatusOK, map[string]any{})

	_, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "test.txt", "--line", "15", "--message", "draft inline comment", "--draft")
	if err != nil {
		t.Fatalf("Command failed: %v", err)
	}

	req := server.LastRequest()
	if req == nil {
		t.Fatal("No request captured")
	}
	if req.Method != http.MethodPut {
		t.Errorf("Expected PUT request, got %s", req.Method)
	}
	expectedPath := "/changes/123/revisions/current/drafts"
	if req.Path != expectedPath {
		t.Errorf("Expected path %s, got %s", expectedPath, req.Path)
	}

	var commentInput gerrit.CommentInput
	if err := json.Unmarshal(req.Body, &commentInput); err != nil {
		t.Fatalf("Failed to unmarshal request body: %v", err)
	}

	if commentInput.Path != "test.txt" {
		t.Errorf("Expected Path 'test.txt', got %q", commentInput.Path)
	}
	if commentInput.Line != 15 {
		t.Errorf("Expected Line 15, got %d", commentInput.Line)
	}
	if commentInput.Message != "draft inline comment" {
		t.Errorf("Expected Message 'draft inline comment', got %q", commentInput.Message)
	}
}

func TestCommentCmd_DraftPatchsetLevel(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("PUT", "/changes/123/revisions/current/drafts", http.StatusOK, map[string]any{})

	_, err := executeCommand(RootCmd, "pr", "comment", "123", "--message", "draft top comment", "--draft")
	if err != nil {
		t.Fatalf("Command failed: %v", err)
	}

	req := server.LastRequest()
	if req == nil {
		t.Fatal("No request captured")
	}
	expectedPath := "/changes/123/revisions/current/drafts"
	if req.Path != expectedPath {
		t.Errorf("Expected path %s, got %s", expectedPath, req.Path)
	}

	var commentInput gerrit.CommentInput
	if err := json.Unmarshal(req.Body, &commentInput); err != nil {
		t.Fatalf("Failed to unmarshal request body: %v", err)
	}

	if commentInput.Path != "/PATCHSET_LEVEL" {
		t.Errorf("Expected Path '/PATCHSET_LEVEL', got %q", commentInput.Path)
	}
	if commentInput.Message != "draft top comment" {
		t.Errorf("Expected Message 'draft top comment', got %q", commentInput.Message)
	}
}

func TestComment_ErrorWhenNeitherMessageNorBodyFile(t *testing.T) {
	_, err := executeCommand(RootCmd, "pr", "comment", "123")
	if err == nil {
		t.Fatal("Expected error when neither message nor body file provided, got nil")
	}
	if !strings.Contains(err.Error(), "must specify either --message or --body-file") {
		t.Errorf("Expected error message about specifying message or body file, got: %v", err)
	}
}

func TestComment_ErrorWhenBothMessageAndBodyFile(t *testing.T) {
	_, err := executeCommand(RootCmd, "pr", "comment", "123", "-m", "msg", "-F", "file")
	if err == nil {
		t.Fatal("Expected error when both message and body file provided, got nil")
	}
	if !strings.Contains(err.Error(), "cannot specify both --message and --body-file") {
		t.Errorf("Expected error message about conflicting flags, got: %v", err)
	}
}

func TestComment_ErrorWhenResolvedWithoutPathOrLine(t *testing.T) {
	_, err := executeCommand(RootCmd, "pr", "comment", "123", "-m", "fixed", "--resolved")
	if err == nil {
		t.Fatal("Expected error when --resolved is used without --path and --line, got nil")
	}
	if !strings.Contains(err.Error(), "--resolved requires both --path and --line") {
		t.Errorf("Expected error about requiring --path and --line, got: %v", err)
	}
}

func TestComment_ErrorWhenListCommentsFailsWithResolved(t *testing.T) {
	server := NewMockGerritServer(t)
	server.On("GET", "/changes/123/comments", func(w http.ResponseWriter, r *http.Request) {
		http.Error(w, "Gerrit database error", http.StatusInternalServerError)
	})

	_, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "main.go", "--line", "10", "-m", "fixed", "--resolved")
	if err == nil {
		t.Fatal("Expected error when ListChangeComments fails with --resolved, got nil")
	}
	if !strings.Contains(err.Error(), "failed to list change comments to resolve thread") {
		t.Errorf("Expected error about failing to list change comments to resolve thread, got: %v", err)
	}
}

func TestComment_WarnWhenNoThreadFoundWithResolved(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/123/comments", http.StatusOK, map[string]any{})
	server.OnJSON("POST", "/changes/123/revisions/current/review", http.StatusOK, map[string]any{})

	out, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "main.go", "--line", "10", "-m", "fixed", "--resolved")
	if err != nil {
		t.Fatalf("Unexpected command error: %v", err)
	}
	if !strings.Contains(out, "Warning: no existing comment thread found") {
		t.Errorf("Expected warning in stderr about no existing comment thread, got: %q", out)
	}
	if !strings.Contains(out, "gh pr view --comments") {
		t.Errorf("Expected suggestion to run 'gh pr view --comments', got: %q", out)
	}
}
