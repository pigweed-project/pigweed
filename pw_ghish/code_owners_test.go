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
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestIsOwnersToken(t *testing.T) {
	for _, token := range []string{"@owners", "owners", "@OWNERS"} {
		if !IsOwnersToken(token) {
			t.Errorf("IsOwnersToken(%q) = false, want true", token)
		}
	}
	for _, nonToken := range []string{"", "keir@google.com", "@me", "me", "owners@google.com", "@auto", "auto"} {
		if IsOwnersToken(nonToken) {
			t.Errorf("IsOwnersToken(%q) = true, want false", nonToken)
		}
	}
}

func TestReady_WithReviewerOwners_GerritAPI(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345", http.StatusOK, map[string]any{
		"_number": 12345,
		"owner":   map[string]any{"email": "author@google.com"},
	})
	server.OnJSON("GET", "/changes/12345/revisions/current/files*", http.StatusOK, map[string]any{
		"/COMMIT_MSG":        map[string]any{},
		"pw_cli/color.py":    map[string]any{"lines_inserted": 5},
		"pw_cli/envparse.py": map[string]any{"lines_inserted": 2},
	})
	server.OnJSON("GET", "/changes/12345/revisions/current/code_owners/*", http.StatusOK, map[string]any{
		"code_owners": []map[string]any{
			{
				"account":  map[string]any{"email": "author@google.com"},
				"scorings": map[string]any{"DISTANCE": 1},
			},
			{
				"account":  map[string]any{"email": "hepler@google.com"},
				"scorings": map[string]any{"DISTANCE": 1},
			},
			{
				"account":  map[string]any{"email": "gwsq-pigweed@pigweed.google.com.iam.gserviceaccount.com"},
				"scorings": map[string]any{"DISTANCE": 2},
			},
		},
	})
	server.OnJSON("POST", "/changes/12345/ready", http.StatusOK, map[string]any{})

	var addedReviewers []string
	server.On("POST", "/changes/12345/reviewers", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			Reviewer string `json:"reviewer"`
		}
		_ = json.NewDecoder(r.Body).Decode(&body)
		addedReviewers = append(addedReviewers, body.Reviewer)
		server.RespondJSON(w, http.StatusOK, map[string]any{
			"reviewers": []map[string]any{{"email": body.Reviewer}},
		})
	})

	output, err := executeCommand(RootCmd, "pr", "ready", "12345", "-r", "@owners")
	if err != nil {
		t.Fatalf("pr ready -r @owners failed: %v\nOutput: %s", err, output)
	}

	if len(addedReviewers) != 1 || addedReviewers[0] != "hepler@google.com" {
		t.Errorf("addedReviewers = %v, want [hepler@google.com]", addedReviewers)
	}
	if !strings.Contains(output, "Resolved @owners: hepler@google.com") {
		t.Errorf("expected output to mention resolved owner, got:\n%s", output)
	}
	if !strings.Contains(output, "Reviewer added successfully: hepler@google.com") {
		t.Errorf("expected reviewer summary in output, got:\n%s", output)
	}
	if !strings.Contains(output, "Change marked ready for review successfully.") {
		t.Errorf("expected ready confirmation in output, got:\n%s", output)
	}
}

func TestReady_WithReviewer_AlreadyReady409Succeeds(t *testing.T) {
	server := NewMockGerritServer(t)
	server.On("POST", "/changes/12345/ready", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusConflict)
		_, _ = w.Write([]byte("change is not work in progress"))
	})

	var addedReviewer string
	server.On("POST", "/changes/12345/reviewers", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			Reviewer string `json:"reviewer"`
		}
		_ = json.NewDecoder(r.Body).Decode(&body)
		addedReviewer = body.Reviewer
		server.RespondJSON(w, http.StatusOK, map[string]any{})
	})

	output, err := executeCommand(RootCmd, "pr", "ready", "12345", "-r", "keir@google.com")
	if err != nil {
		t.Fatalf("expected pr ready -r on non-WIP change to succeed and add reviewer, got error: %v\nOutput: %s", err, output)
	}
	if addedReviewer != "keir@google.com" {
		t.Errorf("addedReviewer = %q, want keir@google.com", addedReviewer)
	}
}

func TestReady_ConflictUndoAndReviewer(t *testing.T) {
	_, err := executeCommand(RootCmd, "pr", "ready", "12345", "--undo", "-r", "keir@google.com")
	if err == nil {
		t.Fatal("expected error when combining --undo and --reviewer, got nil")
	}
	if !strings.Contains(err.Error(), "cannot specify --reviewer (-r) or --owner with --undo (-u)") {
		t.Errorf("unexpected error: %v", err)
	}

	_, err = executeCommand(RootCmd, "pr", "ready", "12345", "--undo", "--owner")
	if err == nil {
		t.Fatal("expected error when combining --undo and --owner, got nil")
	}
}

func TestEdit_AddReviewerOwners(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345", http.StatusOK, map[string]any{
		"_number": 12345,
		"owner":   map[string]any{"email": "hoangmle@google.com"},
	})
	server.OnJSON("GET", "/changes/12345/revisions/current/files*", http.StatusOK, map[string]any{
		"pw_ghish/ready.go": map[string]any{"lines_inserted": 10},
	})
	server.OnJSON("GET", "/changes/12345/revisions/current/code_owners/*", http.StatusOK, map[string]any{
		"code_owners": []map[string]any{
			{
				"account":  map[string]any{"email": "keir@google.com"},
				"scorings": map[string]any{"DISTANCE": 1},
			},
		},
	})

	var addedReviewer string
	server.On("POST", "/changes/12345/reviewers", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			Reviewer string `json:"reviewer"`
		}
		_ = json.NewDecoder(r.Body).Decode(&body)
		addedReviewer = body.Reviewer
		server.RespondJSON(w, http.StatusOK, map[string]any{})
	})

	output, err := executeCommand(RootCmd, "pr", "edit", "12345", "--add-owner")
	if err != nil {
		t.Fatalf("pr edit --add-owner failed: %v\nOutput: %s", err, output)
	}
	if addedReviewer != "keir@google.com" {
		t.Errorf("addedReviewer = %q, want keir@google.com", addedReviewer)
	}
	if !strings.Contains(output, "Reviewer added successfully: keir@google.com") {
		t.Errorf("expected reviewer summary in output, got:\n%s", output)
	}
}

func TestCreate_WithReviewerOwners_LocalOWNERSFallback(t *testing.T) {
	tmpDir := t.TempDir()
	if err := os.WriteFile(filepath.Join(tmpDir, "OWNERS"), []byte("keir@google.com #{LAST_RESORT_SUGGESTION}\n"), 0644); err != nil {
		t.Fatal(err)
	}
	modDir := filepath.Join(tmpDir, "pw_ghish")
	if err := os.MkdirAll(modDir, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(modDir, "OWNERS"), []byte("hoangmle@google.com\nkeir@google.com\n"), 0644); err != nil {
		t.Fatal(err)
	}

	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/*", http.StatusOK, []map[string]any{})

	mockGit := NewMockGit(t).WithBranch("main").
		OnCommand("log -1 --format=%ae HEAD", "hoangmle@google.com\n").
		OnCommand("diff-tree --no-commit-id --name-only -r HEAD", "pw_ghish/ready.go\n").
		OnCommand("rev-parse --show-toplevel", tmpDir+"\n")

	output, err := executeCommand(RootCmd, "pr", "create", "-r", "@owners")
	if err != nil {
		t.Fatalf("pr create -r @owners failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "Resolved @owners: keir@google.com") {
		t.Errorf("expected Resolved @owners output, got:\n%s", output)
	}

	var pushCall string
	for _, call := range mockGit.Calls {
		if strings.HasPrefix(call, "push") {
			pushCall = call
			break
		}
	}
	if !strings.Contains(pushCall, "r=keir@google.com") {
		t.Errorf("expected git push to contain r=keir@google.com, got %q", pushCall)
	}
}

func TestReady_WithReviewerOwners_NoEligibleOwnersErrors(t *testing.T) {
	tmpDir := t.TempDir()
	// Root OWNERS only contains the author themselves and a service account.
	if err := os.WriteFile(filepath.Join(tmpDir, "OWNERS"), []byte("author@google.com {LAST_RESORT_SUGGESTION}\ngwsq-pigweed@pigweed.google.com.iam.gserviceaccount.com\n"), 0644); err != nil {
		t.Fatal(err)
	}

	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345", http.StatusOK, map[string]any{
		"_number": 12345,
		"owner":   map[string]any{"email": "author@google.com"},
	})
	server.OnJSON("GET", "/changes/12345/revisions/current/files*", http.StatusOK, map[string]any{
		"COMMIT_MSG": map[string]any{},
		"foo/bar.cc": map[string]any{"lines_inserted": 1},
	})

	_ = NewMockGit(t).OnCommand("rev-parse --show-toplevel", tmpDir+"\n")

	_, err := executeCommand(RootCmd, "pr", "ready", "12345", "-r", "@owners")
	if err == nil {
		t.Fatal("expected error when no eligible non-author code owners exist, got nil")
	}
	if !strings.Contains(err.Error(), "cannot resolve @owners") {
		t.Errorf("expected error to mention 'cannot resolve @owners', got: %v", err)
	}
}

func TestResolveCodeOwners_MultipleDirectoriesAndAlreadyCoveredBy(t *testing.T) {
	t.Run("disjoint directory owners adds one owner per directory", func(t *testing.T) {
		server := NewMockGerritServer(t)
		server.OnJSON("GET", "/changes/12345", http.StatusOK, map[string]any{
			"_number": 12345,
			"owner":   map[string]any{"email": "author@google.com"},
		})
		server.OnJSON("GET", "/changes/12345/revisions/current/files*", http.StatusOK, map[string]any{
			"pw_cli/foo.py":   map[string]any{"lines_inserted": 3},
			"pw_build/bar.py": map[string]any{"lines_inserted": 4},
		})
		server.OnJSON("GET", "/changes/12345/revisions/current/code_owners/pw_build*", http.StatusOK, map[string]any{
			"code_owners": []map[string]any{
				{
					"account":  map[string]any{"email": "tpudlik@google.com"},
					"scorings": map[string]any{"DISTANCE": 1},
				},
			},
		})
		server.OnJSON("GET", "/changes/12345/revisions/current/code_owners/pw_cli*", http.StatusOK, map[string]any{
			"code_owners": []map[string]any{
				{
					"account":  map[string]any{"email": "hepler@google.com"},
					"scorings": map[string]any{"DISTANCE": 1},
				},
			},
		})
		server.OnJSON("POST", "/changes/12345/ready", http.StatusOK, map[string]any{})

		var added []string
		server.On("POST", "/changes/12345/reviewers", func(w http.ResponseWriter, r *http.Request) {
			var body struct {
				Reviewer string `json:"reviewer"`
			}
			_ = json.NewDecoder(r.Body).Decode(&body)
			added = append(added, body.Reviewer)
			server.RespondJSON(w, http.StatusOK, map[string]any{})
		})

		output, err := executeCommand(RootCmd, "pr", "ready", "12345", "--owner")
		if err != nil {
			t.Fatalf("pr ready --owner failed: %v\nOutput: %s", err, output)
		}
		if len(added) != 2 || added[0] != "tpudlik@google.com" || added[1] != "hepler@google.com" {
			t.Errorf("added = %v, want [tpudlik@google.com hepler@google.com]", added)
		}
		if !strings.Contains(output, "Reviewer added successfully: tpudlik@google.com, hepler@google.com") {
			t.Errorf("expected single summary output with added reviewers, got:\n%s", output)
		}
	})

	t.Run("shared owner across directories is reused via alreadyCoveredBy", func(t *testing.T) {
		server := NewMockGerritServer(t)
		server.OnJSON("GET", "/changes/12345", http.StatusOK, map[string]any{
			"_number": 12345,
			"owner":   map[string]any{"email": "author@google.com"},
		})
		server.OnJSON("GET", "/changes/12345/revisions/current/files*", http.StatusOK, map[string]any{
			"pw_cli/foo.py":   map[string]any{"lines_inserted": 3},
			"pw_build/bar.py": map[string]any{"lines_inserted": 4},
		})
		// pw_build is evaluated first (alphabetical dir order) and selects hepler@google.com.
		server.OnJSON("GET", "/changes/12345/revisions/current/code_owners/pw_build*", http.StatusOK, map[string]any{
			"code_owners": []map[string]any{
				{
					"account":  map[string]any{"email": "hepler@google.com"},
					"scorings": map[string]any{"DISTANCE": 1},
				},
			},
		})
		// pw_cli lists keir@google.com first and hepler@google.com second; alreadyCoveredBy
		// sees hepler@google.com is already selected and skips adding a second reviewer.
		server.OnJSON("GET", "/changes/12345/revisions/current/code_owners/pw_cli*", http.StatusOK, map[string]any{
			"code_owners": []map[string]any{
				{
					"account":  map[string]any{"email": "keir@google.com"},
					"scorings": map[string]any{"DISTANCE": 1},
				},
				{
					"account":  map[string]any{"email": "hepler@google.com"},
					"scorings": map[string]any{"DISTANCE": 1},
				},
			},
		})

		var added []string
		server.On("POST", "/changes/12345/reviewers", func(w http.ResponseWriter, r *http.Request) {
			var body struct {
				Reviewer string `json:"reviewer"`
			}
			_ = json.NewDecoder(r.Body).Decode(&body)
			added = append(added, body.Reviewer)
			server.RespondJSON(w, http.StatusOK, map[string]any{})
		})

		output, err := executeCommand(RootCmd, "pr", "edit", "12345", "--add-owner")
		if err != nil {
			t.Fatalf("pr edit --add-owner failed: %v\nOutput: %s", err, output)
		}
		if len(added) != 1 || added[0] != "hepler@google.com" {
			t.Errorf("added = %v, want [hepler@google.com] (alreadyCoveredBy should reuse hepler)", added)
		}
		if !strings.Contains(output, "Reviewer added successfully: hepler@google.com") {
			t.Errorf("expected summary output with added reviewer, got:\n%s", output)
		}
	})
}
