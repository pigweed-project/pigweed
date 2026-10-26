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
	t.Run("defaults to draft for inline comment and prints batch publish hint", func(t *testing.T) {
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
		server.OnJSON("GET", "/changes/123/drafts", http.StatusOK, map[string]any{})
		server.OnJSON("PUT", "/changes/123/revisions/current/drafts", http.StatusOK, map[string]any{})

		out, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "test.txt", "--line", "10", "--message", "reply message")
		if err != nil {
			t.Fatalf("Command failed: %v", err)
		}
		if !strings.Contains(out, "✓ Draft comment saved successfully on test.txt:10 (reply).") {
			t.Errorf("Expected draft saved message, got: %q", out)
		}
		if !strings.Contains(out, "gh pr review --publish") {
			t.Errorf("Expected batch publish hint in output, got: %q", out)
		}

		req := server.LastRequest()
		if req == nil {
			t.Fatal("No request captured")
		}
		if req.Method != http.MethodPut || req.Path != "/changes/123/revisions/current/drafts" {
			t.Fatalf("Expected PUT /changes/123/revisions/current/drafts, got %s %s", req.Method, req.Path)
		}

		var commentInput gerrit.CommentInput
		if err := json.Unmarshal(req.Body, &commentInput); err != nil {
			t.Fatalf("Failed to unmarshal request body: %v", err)
		}
		if commentInput.InReplyTo != "parent_id" {
			t.Errorf("Expected InReplyTo 'parent_id', got %q", commentInput.InReplyTo)
		}
	})

	t.Run("publishes immediately when --publish is specified", func(t *testing.T) {
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

		_, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "test.txt", "--line", "10", "--message", "reply message", "--publish")
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
	})
}

func TestCommentCmd_Resolved(t *testing.T) {
	t.Run("defaults to draft resolution", func(t *testing.T) {
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
		server.OnJSON("GET", "/changes/123/drafts", http.StatusOK, map[string]any{})
		server.OnJSON("PUT", "/changes/123/revisions/current/drafts", http.StatusOK, map[string]any{})

		_, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "test.txt", "--line", "10", "--message", "reply message", "--resolved")
		if err != nil {
			t.Fatalf("Command failed: %v", err)
		}

		req := server.LastRequest()
		if req == nil {
			t.Fatal("No request captured")
		}
		if req.Method != http.MethodPut || req.Path != "/changes/123/revisions/current/drafts" {
			t.Fatalf("Expected PUT /changes/123/revisions/current/drafts, got %s %s", req.Method, req.Path)
		}

		var commentInput gerrit.CommentInput
		if err := json.Unmarshal(req.Body, &commentInput); err != nil {
			t.Fatalf("Failed to unmarshal request body: %v", err)
		}
		if commentInput.Unresolved == nil || *commentInput.Unresolved != false {
			t.Errorf("Expected Unresolved to be false, got %v", commentInput.Unresolved)
		}
	})

	t.Run("publishes resolution immediately with --publish", func(t *testing.T) {
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

		_, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "test.txt", "--line", "10", "--message", "reply message", "--resolved", "--publish")
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
	})
}

func TestCommentCmd_Patchset(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/123/comments", http.StatusOK, map[string]any{})
	server.OnJSON("POST", "/changes/123/revisions/2/review", http.StatusOK, map[string]any{})

	_, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "test.txt", "--line", "10", "--message", "reply message", "--patchset", "2", "--publish")
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
	t.Run("stages draft on earlier patchset by default", func(t *testing.T) {
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
		server.OnJSON("GET", "/changes/123/drafts", http.StatusOK, map[string]any{})
		server.OnJSON("PUT", "/changes/123/revisions/1/drafts", http.StatusOK, map[string]any{})

		_, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "test.txt", "--line", "42", "--message", "Fixed in PS2", "--resolved")
		if err != nil {
			t.Fatalf("Command failed: %v", err)
		}

		req := server.LastRequest()
		if req == nil {
			t.Fatal("No request captured")
		}
		expectedPath := "/changes/123/revisions/1/drafts"
		if req.Path != expectedPath {
			t.Errorf("Expected path %s, got %s", expectedPath, req.Path)
		}

		var commentInput gerrit.CommentInput
		if err := json.Unmarshal(req.Body, &commentInput); err != nil {
			t.Fatalf("Failed to parse body: %v", err)
		}
		if commentInput.InReplyTo != "ps1_comment_id" {
			t.Errorf("Expected InReplyTo 'ps1_comment_id', got %q", commentInput.InReplyTo)
		}
		if commentInput.Unresolved == nil || *commentInput.Unresolved != false {
			t.Errorf("Expected Unresolved to be false, got %v", commentInput.Unresolved)
		}
	})

	t.Run("publishes on earlier patchset when --publish is passed", func(t *testing.T) {
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

		_, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "test.txt", "--line", "42", "--message", "Fixed in PS2", "--resolved", "--publish")
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
	})
}

func TestCommentCmd_DraftInline(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/123/comments", http.StatusOK, map[string]any{})
	server.OnJSON("GET", "/changes/123/drafts", http.StatusOK, map[string]any{})
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
	if commentInput.Unresolved == nil || !*commentInput.Unresolved {
		t.Errorf("Expected new root inline draft to default to Unresolved=true, got %v", commentInput.Unresolved)
	}
}

func TestCommentCmd_DraftPatchsetLevel(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/123/drafts", http.StatusOK, map[string]any{})
	server.OnJSON("PUT", "/changes/123/revisions/current/drafts", http.StatusOK, map[string]any{})

	out, err := executeCommand(RootCmd, "pr", "comment", "123", "--message", "draft top comment", "--draft")
	if err != nil {
		t.Fatalf("Command failed: %v", err)
	}
	if !strings.Contains(out, "gh pr review --publish") {
		t.Errorf("Expected batch publish hint in output, got: %q", out)
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
	if !strings.Contains(err.Error(), "must specify either --message") {
		t.Errorf("Expected error message about specifying message or body file, got: %v", err)
	}
}

func TestComment_ErrorWhenBothMessageAndBody(t *testing.T) {
	_, err := executeCommand(RootCmd, "pr", "comment", "123", "-m", "msg", "-b", "body")
	if err == nil {
		t.Fatal("Expected error when both --message and --body provided, got nil")
	}
	if !strings.Contains(err.Error(), "cannot specify both --message (-m) and --body (-b)") {
		t.Errorf("Expected error message about conflicting -m and -b flags, got: %v", err)
	}
}

func TestCommentCmd_BodyFlagAndDraftsKeep(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/123/revisions/current/review", http.StatusOK, map[string]any{})

	_, err := executeCommand(RootCmd, "pr", "comment", "123", "-b", "comment via body flag")
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
	if reviewInput.Message != "comment via body flag" {
		t.Errorf("Expected Message 'comment via body flag', got %q", reviewInput.Message)
	}
	if reviewInput.Drafts != "KEEP" {
		t.Errorf("Expected Drafts 'KEEP' so unrelated private drafts are not leaked, got %q", reviewInput.Drafts)
	}
}

func TestCommentCmd_FileLevelPublicComment(t *testing.T) {
	t.Run("defaults to draft for file-level comment", func(t *testing.T) {
		server := NewMockGerritServer(t)
		server.OnJSON("GET", "/changes/123/comments", http.StatusOK, map[string]any{})
		server.OnJSON("GET", "/changes/123/drafts", http.StatusOK, map[string]any{})
		server.OnJSON("PUT", "/changes/123/revisions/current/drafts", http.StatusOK, map[string]any{})

		out, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "test.txt", "-m", "File-level note")
		if err != nil {
			t.Fatalf("Command failed: %v", err)
		}
		if !strings.Contains(out, "Draft comment saved successfully on test.txt (new thread, unresolved).") {
			t.Errorf("Expected file-level draft saved message, got: %q", out)
		}
		if !strings.Contains(out, "gh pr review --publish") {
			t.Errorf("Expected batch publish hint, got: %q", out)
		}

		req := server.LastRequest()
		if req == nil || req.Method != http.MethodPut || req.Path != "/changes/123/revisions/current/drafts" {
			t.Fatalf("Expected PUT /changes/123/revisions/current/drafts, got %+v", req)
		}
		var commentInput gerrit.CommentInput
		if err := json.Unmarshal(req.Body, &commentInput); err != nil {
			t.Fatalf("Failed to unmarshal request body: %v", err)
		}
		if commentInput.Path != "test.txt" || commentInput.Line != 0 {
			t.Errorf("Expected Path 'test.txt' and Line 0, got %+v", commentInput)
		}
		if commentInput.Unresolved == nil || !*commentInput.Unresolved {
			t.Errorf("Expected Unresolved=true, got %v", commentInput.Unresolved)
		}
	})

	t.Run("publishes file-level comment immediately when --publish is passed", func(t *testing.T) {
		server := NewMockGerritServer(t)
		server.OnJSON("GET", "/changes/123/comments", http.StatusOK, map[string]any{})
		server.OnJSON("POST", "/changes/123/revisions/current/review", http.StatusOK, map[string]any{})

		_, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "test.txt", "-m", "File-level note", "--publish")
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
		if reviewInput.Message != "" {
			t.Errorf("Expected empty top-level ReviewInput.Message for file-level comment, got %q", reviewInput.Message)
		}
		fileComments := reviewInput.Comments["test.txt"]
		if len(fileComments) != 1 {
			t.Fatalf("Expected 1 file-level comment on test.txt, got %+v", reviewInput.Comments)
		}
		if fileComments[0].Line != 0 {
			t.Errorf("Expected Line 0 for file-level comment, got %d", fileComments[0].Line)
		}
		if fileComments[0].Message != "File-level note" {
			t.Errorf("Expected Message 'File-level note', got %q", fileComments[0].Message)
		}
		if fileComments[0].Unresolved == nil || !*fileComments[0].Unresolved {
			t.Errorf("Expected new root file-level comment to default to Unresolved=true, got %v", fileComments[0].Unresolved)
		}
	})
}

func TestCommentCmd_DraftUpdatesExistingDraftAndPreservesRangeAndSide(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/123/comments", http.StatusOK, map[string]any{
		"test.txt": []map[string]any{
			{
				"id":        "parent_comment_id",
				"path":      "test.txt",
				"line":      307,
				"patch_set": 15,
				"message":   "reviewer comment",
				"updated":   "2026-09-30 06:00:00.000000000",
			},
		},
	})
	server.OnJSON("GET", "/changes/123/drafts", http.StatusOK, map[string]any{
		"test.txt": []map[string]any{
			{
				"id":          "existing_draft_id",
				"path":        "test.txt",
				"line":        307,
				"side":        "PARENT",
				"patch_set":   15,
				"in_reply_to": "parent_comment_id",
				"message":     "FOR GEMINI: Please revert this file.",
				"range": map[string]any{
					"start_line":      305,
					"start_character": 2,
					"end_line":        307,
					"end_character":   18,
				},
			},
		},
	})
	server.OnJSON("PUT", "/changes/123/revisions/15/drafts/existing_draft_id", http.StatusOK, map[string]any{})

	out, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "test.txt", "--line", "307", "--message", "Reverted.", "--resolved", "--draft")
	if err != nil {
		t.Fatalf("Command failed: %v", err)
	}
	if !strings.Contains(out, "Draft comment updated successfully on test.txt:307 [PS15] (reply, resolved).") {
		t.Errorf("Expected detailed updated message, got: %q", out)
	}
	if !strings.Contains(out, "gh pr review --publish") {
		t.Errorf("Expected batch publish hint in output, got: %q", out)
	}

	req := server.LastRequest()
	if req == nil {
		t.Fatal("No request captured")
	}
	expectedPath := "/changes/123/revisions/15/drafts/existing_draft_id"
	if req.Path != expectedPath {
		t.Errorf("Expected path %s, got %s", expectedPath, req.Path)
	}

	var commentInput gerrit.CommentInput
	if err := json.Unmarshal(req.Body, &commentInput); err != nil {
		t.Fatalf("Failed to unmarshal request body: %v", err)
	}
	if commentInput.Message != "Reverted." {
		t.Errorf("Expected Message 'Reverted.', got %q", commentInput.Message)
	}
	if commentInput.Unresolved == nil || *commentInput.Unresolved != false {
		t.Errorf("Expected Unresolved=false, got %v", commentInput.Unresolved)
	}
	if commentInput.Side != "PARENT" {
		t.Errorf("Expected Side 'PARENT' to be preserved, got %q", commentInput.Side)
	}
	if commentInput.Range == nil || commentInput.Range.StartLine != 305 || commentInput.Range.EndLine != 307 {
		t.Errorf("Expected Range [305..307] to be preserved, got %+v", commentInput.Range)
	}
}

func TestCommentCmd_StandaloneDraftResolveDoesNotWarn(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/123/comments", http.StatusOK, map[string]any{})
	server.OnJSON("GET", "/changes/123/drafts", http.StatusOK, map[string]any{
		"ring_buffer.cc": []map[string]any{
			{
				"id":         "standalone_draft_id",
				"path":       "ring_buffer.cc",
				"line":       84,
				"patch_set":  1,
				"unresolved": true,
				"message":    "FOR GEMINI: Use pw::Result here.",
			},
		},
	})
	server.OnJSON("PUT", "/changes/123/revisions/1/drafts/standalone_draft_id", http.StatusOK, map[string]any{})

	out, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "ring_buffer.cc", "--line", "84", "-m", "Switched to pw::Result.", "--resolved", "--draft")
	if err != nil {
		t.Fatalf("Command failed: %v", err)
	}
	if strings.Contains(out, "Warning: no existing comment thread found") {
		t.Errorf("Did not expect missing thread warning when resolving a standalone draft, got: %q", out)
	}
	if !strings.Contains(out, "Draft comment updated successfully on ring_buffer.cc:84 [PS1] (new thread, resolved).") {
		t.Errorf("Expected detailed 'Draft comment updated successfully' message, got: %q", out)
	}
}

func TestCommentCmd_DeleteDraft(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/123/drafts", http.StatusOK, map[string]any{
		"ring_buffer.cc": []map[string]any{
			{
				"id":        "draft_to_delete",
				"path":      "ring_buffer.cc",
				"line":      84,
				"patch_set": 2,
				"message":   "FOR GEMINI: Remove debug print.",
			},
		},
	})
	server.OnJSON("DELETE", "/changes/123/revisions/2/drafts/draft_to_delete", http.StatusNoContent, nil)

	out, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "ring_buffer.cc", "--line", "84", "--delete-draft")
	if err != nil {
		t.Fatalf("Command failed: %v", err)
	}
	if !strings.Contains(out, "✓ Draft comment deleted successfully (ring_buffer.cc:84 [PS2]).") {
		t.Errorf("Expected '✓ Draft comment deleted successfully (ring_buffer.cc:84 [PS2]).', got: %q", out)
	}

	req := server.LastRequest()
	if req == nil || req.Method != http.MethodDelete || req.Path != "/changes/123/revisions/2/drafts/draft_to_delete" {
		t.Errorf("Expected DELETE /changes/123/revisions/2/drafts/draft_to_delete, got %+v", req)
	}
}

func TestCommentCmd_AmbiguousDraftsFailsInsteadOfClobbering(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/123/comments", http.StatusOK, map[string]any{})
	server.OnJSON("GET", "/changes/123/drafts", http.StatusOK, map[string]any{
		"ring_buffer.cc": []map[string]any{
			{
				"id":        "draft_ps1",
				"path":      "ring_buffer.cc",
				"line":      84,
				"patch_set": 1,
				"message":   "Draft on PS1",
			},
			{
				"id":        "draft_ps2",
				"path":      "ring_buffer.cc",
				"line":      84,
				"patch_set": 2,
				"message":   "Draft on PS2",
			},
		},
	})

	t.Run("update draft refuses to guess when multiple drafts match", func(t *testing.T) {
		_, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "ring_buffer.cc", "--line", "84", "-m", "Updated", "--draft")
		if err == nil {
			t.Fatal("Expected error when multiple drafts match at target location, got nil")
		}
		for _, want := range []string{
			"multiple (2) unpublished draft comments found at ring_buffer.cc:84",
			"[PS1 (--patchset 1 or 123/1)] \"Draft on PS1\"",
			"[PS2 (--patchset 2 or 123/2)] \"Draft on PS2\"",
			"--patchset <N>",
		} {
			if !strings.Contains(err.Error(), want) {
				t.Errorf("Error missing %q.\nGot: %v", want, err)
			}
		}
	})

	t.Run("delete draft refuses to guess when multiple drafts match", func(t *testing.T) {
		_, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "ring_buffer.cc", "--line", "84", "--delete-draft")
		if err == nil {
			t.Fatal("Expected error when multiple drafts match at target location, got nil")
		}
		if !strings.Contains(err.Error(), "multiple (2) unpublished draft comments found at ring_buffer.cc:84") {
			t.Errorf("Unexpected error: %v", err)
		}
	})

	t.Run("specifying --patchset disambiguates cleanly", func(t *testing.T) {
		server.OnJSON("DELETE", "/changes/123/revisions/1/drafts/draft_ps1", http.StatusNoContent, nil)
		_, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "ring_buffer.cc", "--line", "84", "--patchset", "1", "--delete-draft")
		if err != nil {
			t.Fatalf("Expected --patchset 1 to disambiguate draft deletion, got: %v", err)
		}
	})

	t.Run("specifying pwrev/123/2 disambiguates cleanly", func(t *testing.T) {
		server.OnJSON("DELETE", "/changes/123/revisions/2/drafts/draft_ps2", http.StatusNoContent, nil)
		_, err := executeCommand(RootCmd, "pr", "comment", "pwrev/123/2", "--path", "ring_buffer.cc", "--line", "84", "--delete-draft")
		if err != nil {
			t.Fatalf("Expected pwrev/123/2 to disambiguate draft deletion, got: %v", err)
		}
	})
}

func TestCommentCmd_PwrevWithPatchsetSuffix_CarriedForwardDraftAndThread(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/123/comments", http.StatusOK, map[string]any{
		"ring_buffer.cc": []map[string]any{
			{
				"id":        "parent_ps15",
				"path":      "ring_buffer.cc",
				"line":      84,
				"patch_set": 15,
				"message":   "Comment from PS15",
				"updated":   "2026-09-30 06:00:00.000000000",
			},
		},
	})
	server.OnJSON("GET", "/changes/123/drafts", http.StatusOK, map[string]any{
		"ring_buffer.cc": []map[string]any{
			{
				"id":          "draft_ps15",
				"path":        "ring_buffer.cc",
				"line":        84,
				"patch_set":   15,
				"in_reply_to": "parent_ps15",
				"message":     "Initial draft on PS15",
			},
		},
	})
	server.OnJSON("PUT", "/changes/123/revisions/15/drafts/draft_ps15", http.StatusOK, map[string]any{})

	// Referencing pwrev/123/23 (viewing PS23) while the thread and draft live on PS15
	// must still find and update the PS15 draft on revision 15 rather than creating a duplicate on PS23.
	out, err := executeCommand(RootCmd, "pr", "comment", "pwrev/123/23", "--path", "ring_buffer.cc", "--line", "84", "-m", "Done in PS23", "--resolved", "--draft")
	if err != nil {
		t.Fatalf("Command failed: %v", err)
	}
	if !strings.Contains(out, "Draft comment updated successfully on ring_buffer.cc:84 [PS15] (reply, resolved).") {
		t.Errorf("Expected detailed 'Draft comment updated successfully', got: %q", out)
	}
	req := server.LastRequest()
	if req == nil || req.Path != "/changes/123/revisions/15/drafts/draft_ps15" {
		t.Errorf("Expected PUT /changes/123/revisions/15/drafts/draft_ps15, got %+v", req)
	}
}

func TestCommentCmd_DeleteDraft_NotFoundListsAvailable(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/123/drafts", http.StatusOK, map[string]any{
		"other.cc": []map[string]any{
			{
				"id":        "d1",
				"path":      "other.cc",
				"line":      42,
				"patch_set": 1,
				"message":   "Existing draft",
			},
		},
	})

	_, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "ring_buffer.cc", "--line", "99", "--delete-draft")
	if err == nil {
		t.Fatal("Expected error when no draft exists at target location, got nil")
	}
	for _, want := range []string{
		"no unpublished draft comment found at ring_buffer.cc:99",
		"other.cc:42 [PS1] (--path other.cc --line 42 --delete-draft --patchset 1)",
		"gh pr view 123 --comments",
	} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("Error missing %q.\nGot: %v", want, err)
		}
	}
}

func TestComment_ErrorWhenBothMessageAndBodyFile(t *testing.T) {
	_, err := executeCommand(RootCmd, "pr", "comment", "123", "-m", "msg", "-F", "file")
	if err == nil {
		t.Fatal("Expected error when both message and body file provided, got nil")
	}
	if !strings.Contains(err.Error(), "cannot specify both --message/--body and --body-file") {
		t.Errorf("Expected error message about conflicting flags, got: %v", err)
	}
}

func TestComment_ErrorWhenResolvedWithoutPath(t *testing.T) {
	_, err := executeCommand(RootCmd, "pr", "comment", "123", "-m", "fixed", "--resolved")
	if err == nil {
		t.Fatal("Expected error when --resolved is used without --path, got nil")
	}
	if !strings.Contains(err.Error(), "--resolved requires --path") {
		t.Errorf("Expected error about requiring --path, got: %v", err)
	}
}

func TestCommentCmd_FileLevelThreadResolve(t *testing.T) {
	t.Run("stages draft reply by default", func(t *testing.T) {
		server := NewMockGerritServer(t)
		server.OnJSON("GET", "/changes/123/comments", http.StatusOK, map[string]any{
			"test.txt": []map[string]any{
				{
					"id":         "file_thread_id",
					"path":       "test.txt",
					"line":       0,
					"patch_set":  2,
					"unresolved": true,
					"message":    "Please add a copyright header to this file.",
					"updated":    "2026-09-30 06:00:00.000000000",
				},
			},
		})
		server.OnJSON("GET", "/changes/123/drafts", http.StatusOK, map[string]any{})
		server.OnJSON("PUT", "/changes/123/revisions/2/drafts", http.StatusOK, map[string]any{})

		out, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "test.txt", "-m", "Added copyright header.", "--resolved")
		if err != nil {
			t.Fatalf("Command failed: %v", err)
		}
		if !strings.Contains(out, "Draft comment saved successfully on test.txt [PS2] (reply, resolved).") {
			t.Errorf("Expected draft saved confirmation for file-level thread reply, got: %q", out)
		}

		req := server.LastRequest()
		if req == nil || req.Path != "/changes/123/revisions/2/drafts" {
			t.Fatalf("Expected PUT /changes/123/revisions/2/drafts, got %+v", req)
		}
		var commentInput gerrit.CommentInput
		if err := json.Unmarshal(req.Body, &commentInput); err != nil {
			t.Fatalf("Failed to parse body: %v", err)
		}
		if commentInput.InReplyTo != "file_thread_id" || commentInput.Unresolved == nil || *commentInput.Unresolved {
			t.Errorf("Expected resolved reply to file_thread_id, got %+v", commentInput)
		}
	})

	t.Run("publishes immediately with --publish", func(t *testing.T) {
		server := NewMockGerritServer(t)
		server.OnJSON("GET", "/changes/123/comments", http.StatusOK, map[string]any{
			"test.txt": []map[string]any{
				{
					"id":         "file_thread_id",
					"path":       "test.txt",
					"line":       0,
					"patch_set":  2,
					"unresolved": true,
					"message":    "Please add a copyright header to this file.",
					"updated":    "2026-09-30 06:00:00.000000000",
				},
			},
		})
		server.OnJSON("POST", "/changes/123/revisions/2/review", http.StatusOK, map[string]any{})

		out, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "test.txt", "-m", "Added copyright header.", "--resolved", "--publish")
		if err != nil {
			t.Fatalf("Command failed: %v", err)
		}
		if !strings.Contains(out, "✓ Comment submitted successfully on test.txt [PS2] (reply, resolved).") {
			t.Errorf("Expected confirmation for file-level thread reply, got: %q", out)
		}

		req := server.LastRequest()
		if req == nil || req.Path != "/changes/123/revisions/2/review" {
			t.Fatalf("Expected POST /changes/123/revisions/2/review, got %+v", req)
		}
		var reviewInput gerrit.ReviewInput
		if err := json.Unmarshal(req.Body, &reviewInput); err != nil {
			t.Fatalf("Failed to parse body: %v", err)
		}
		comments := reviewInput.Comments["test.txt"]
		if len(comments) != 1 || comments[0].InReplyTo != "file_thread_id" || comments[0].Unresolved == nil || *comments[0].Unresolved {
			t.Errorf("Expected resolved reply to file_thread_id, got %+v", comments)
		}
	})
}

func TestComment_PreconditionValidationErrors(t *testing.T) {
	t.Run("draft and delete-draft together", func(t *testing.T) {
		_, err := executeCommand(RootCmd, "pr", "comment", "123", "--draft", "--delete-draft")
		if err == nil || !strings.Contains(err.Error(), "cannot specify both --draft and --delete-draft") {
			t.Errorf("Expected conflicting --draft and --delete-draft error, got: %v", err)
		}
	})

	t.Run("publish and delete-draft together", func(t *testing.T) {
		_, err := executeCommand(RootCmd, "pr", "comment", "123", "--publish", "--delete-draft")
		if err == nil || !strings.Contains(err.Error(), "cannot specify both --publish and --delete-draft") {
			t.Errorf("Expected conflicting --publish and --delete-draft error, got: %v", err)
		}
	})

	t.Run("draft and publish together", func(t *testing.T) {
		_, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "a.cc", "--line", "10", "-m", "hi", "--draft", "--publish")
		if err == nil || !strings.Contains(err.Error(), "cannot specify both --draft and --publish") {
			t.Errorf("Expected conflicting --draft and --publish error, got: %v", err)
		}
	})

	t.Run("line without path", func(t *testing.T) {
		_, err := executeCommand(RootCmd, "pr", "comment", "123", "--line", "10", "-m", "hi")
		if err == nil || !strings.Contains(err.Error(), "--line requires --path") {
			t.Errorf("Expected --line requires --path error, got: %v", err)
		}
	})

	t.Run("negative line", func(t *testing.T) {
		_, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "a.cc", "--line", "-1", "-m", "hi")
		if err == nil || !strings.Contains(err.Error(), "--line must be a positive line number") {
			t.Errorf("Expected negative line error, got: %v", err)
		}
	})
}

func TestComment_ErrorWhenListCommentsOrDraftsFails(t *testing.T) {
	t.Run("fails when ListChangeComments returns error even without --resolved", func(t *testing.T) {
		server := NewMockGerritServer(t)
		server.On("GET", "/changes/123/comments", func(w http.ResponseWriter, r *http.Request) {
			http.Error(w, "Gerrit database error", http.StatusInternalServerError)
		})

		_, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "main.go", "--line", "10", "-m", "reply")
		if err == nil {
			t.Fatal("Expected error when ListChangeComments fails, got nil")
		}
		if !strings.Contains(err.Error(), "error listing comments for change 123") {
			t.Errorf("Expected error about listing comments for change 123, got: %v", err)
		}
	})

	t.Run("fails when ListChangeDrafts returns error during --draft", func(t *testing.T) {
		server := NewMockGerritServer(t)
		server.OnJSON("GET", "/changes/123/comments", http.StatusOK, map[string]any{})
		server.On("GET", "/changes/123/drafts", func(w http.ResponseWriter, r *http.Request) {
			http.Error(w, "Gerrit draft service unavailable", http.StatusInternalServerError)
		})

		_, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "main.go", "--line", "10", "-m", "draft reply", "--draft")
		if err == nil {
			t.Fatal("Expected error when ListChangeDrafts fails, got nil")
		}
		if !strings.Contains(err.Error(), "error listing draft comments for change 123") {
			t.Errorf("Expected error about listing draft comments for change 123, got: %v", err)
		}
	})
}

func TestComment_WarnWhenNoThreadFoundWithResolved(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/123/comments", http.StatusOK, map[string]any{})
	server.OnJSON("GET", "/changes/123/drafts", http.StatusOK, map[string]any{})
	server.OnJSON("PUT", "/changes/123/revisions/current/drafts", http.StatusOK, map[string]any{})

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

func TestCommentCmd_BodyFileStdin(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/123/comments", http.StatusOK, map[string]any{})
	server.OnJSON("GET", "/changes/123/drafts", http.StatusOK, map[string]any{})
	server.OnJSON("PUT", "/changes/123/revisions/current/drafts", http.StatusCreated, map[string]any{
		"id":        "draft_stdin",
		"patch_set": 3,
	})
	server.OnJSON("POST", "/changes/123/revisions/current/review", http.StatusOK, map[string]any{})

	t.Run("inline draft comment from stdin via --body-file -", func(t *testing.T) {
		heredocBody := "Multi-line comment with \"quotes\", `backticks`, and $vars\nSecond line."
		out, err := executeCommandWithStdin(
			RootCmd,
			strings.NewReader(heredocBody),
			"pr", "comment", "123", "--path", "foo.cc", "--line", "42", "--body-file", "-", "--draft",
		)
		if err != nil {
			t.Fatalf("pr comment --body-file - failed: %v\nOutput: %s", err, out)
		}
		if !strings.Contains(out, "Draft comment saved successfully on foo.cc:42 [PS3] (new thread, unresolved).") {
			t.Errorf("Unexpected output: %q", out)
		}
		req := server.LastRequest()
		if req == nil || req.Path != "/changes/123/revisions/current/drafts" {
			t.Fatalf("Expected PUT /changes/123/revisions/current/drafts, got %+v", req)
		}
		var draftInput gerrit.CommentInput
		if err := json.Unmarshal(req.Body, &draftInput); err != nil {
			t.Fatalf("Failed to unmarshal draft input: %v", err)
		}
		if draftInput.Message != heredocBody {
			t.Errorf("draftInput.Message = %q, want %q", draftInput.Message, heredocBody)
		}
	})

	t.Run("change-level comment from stdin via -F -", func(t *testing.T) {
		stdinBody := "Change-level note from stdin\n"
		out, err := executeCommandWithStdin(
			RootCmd,
			strings.NewReader(stdinBody),
			"pr", "comment", "123", "-F", "-",
		)
		if err != nil {
			t.Fatalf("pr comment -F - failed: %v\nOutput: %s", err, out)
		}
		req := server.LastRequest()
		if req == nil || req.Path != "/changes/123/revisions/current/review" {
			t.Fatalf("Expected POST /changes/123/revisions/current/review, got %+v", req)
		}
		var reviewInput gerrit.ReviewInput
		if err := json.Unmarshal(req.Body, &reviewInput); err != nil {
			t.Fatalf("Failed to unmarshal review input: %v", err)
		}
		if reviewInput.Message != stdinBody {
			t.Errorf("reviewInput.Message = %q, want %q", reviewInput.Message, stdinBody)
		}
	})

	t.Run("empty stdin returns clear error", func(t *testing.T) {
		_, err := executeCommandWithStdin(
			RootCmd,
			strings.NewReader("   \n\t "),
			"pr", "comment", "123", "--body-file", "-",
		)
		if err == nil {
			t.Fatal("Expected error when stdin is empty/whitespace, got nil")
		}
		if !strings.Contains(err.Error(), "body from stdin is empty; comment message cannot be empty") {
			t.Errorf("Unexpected error for empty stdin: %v", err)
		}
	})

	t.Run("-m - and -b - also read from stdin instead of literal dash", func(t *testing.T) {
		stdinBody := "Comment body via -m - alias."
		out, err := executeCommandWithStdin(
			RootCmd,
			strings.NewReader(stdinBody),
			"pr", "comment", "123", "-m", "-",
		)
		if err != nil {
			t.Fatalf("pr comment -m - failed: %v\nOutput: %s", err, out)
		}
		req := server.LastRequest()
		var reviewInput gerrit.ReviewInput
		if err := json.Unmarshal(req.Body, &reviewInput); err != nil {
			t.Fatalf("Failed to unmarshal review input: %v", err)
		}
		if reviewInput.Message != stdinBody {
			t.Errorf("reviewInput.Message = %q, want %q", reviewInput.Message, stdinBody)
		}
	})
}
