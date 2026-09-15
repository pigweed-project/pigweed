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
	"fmt"
	"io"
	"net/http"
	"strings"
	"testing"
)

func TestCherryPickIntegration(t *testing.T) {
	mockGit := SetMockGit(t, nil)

	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345", http.StatusOK, `{
		"_number": 12345,
		"current_revision": "rev1",
		"revisions": {
			"rev1": {
				"fetch": {
					"http": {
						"ref": "refs/changes/45/12345/1"
					}
				}
			}
		}
	}`)

	output, err := executeCommand(RootCmd, "pr", "cherry-pick", "12345")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if len(mockGit.Calls) < 2 {
		t.Fatalf("Expected at least 2 git calls. Got: %v", mockGit.Calls)
	}

	if !strings.Contains(mockGit.Calls[0], "fetch origin refs/changes/45/12345/1") {
		t.Errorf("Expected fetch call. Got: %s", mockGit.Calls[0])
	}

	if !strings.Contains(mockGit.Calls[1], "cherry-pick FETCH_HEAD") {
		t.Errorf("Expected cherry-pick call. Got: %s", mockGit.Calls[1])
	}
}

func TestCherryPick_ErrorWhenChangeNotFound(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnStatus(http.StatusNotFound)

	_, err := executeCommand(RootCmd, "pr", "cherry-pick", "99999")
	if err == nil {
		t.Fatal("Expected error when change is not found, got nil")
	}
	if !strings.Contains(err.Error(), "error getting change") {
		t.Errorf("Expected error to mention 'error getting change', got: %v", err)
	}
}

func TestCherryPick_ErrorOnConflictWithActionableHelp(t *testing.T) {
	SetMockGit(t, &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) > 0 && args[0] == "cherry-pick" {
				return fmt.Errorf("error: could not apply rev1... Some commit: merge conflict in foo.cc")
			}
			return nil
		},
	})

	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345", http.StatusOK, `{
		"_number": 12345,
		"current_revision": "rev1",
		"revisions": {
			"rev1": {
				"fetch": {
					"http": {
						"ref": "refs/changes/45/12345/1"
					}
				}
			}
		}
	}`)

	_, err := executeCommand(RootCmd, "pr", "cherry-pick", "12345")
	if err == nil {
		t.Fatal("Expected error on cherry-pick conflict, got nil")
	}
	if !strings.Contains(err.Error(), "To resolve merge conflicts:") {
		t.Errorf("Expected conflict resolution instructions, got:\n%v", err)
	}
	if !strings.Contains(err.Error(), "git cherry-pick --continue") {
		t.Errorf("Expected mention of 'git cherry-pick --continue', got:\n%v", err)
	}
	if !strings.Contains(err.Error(), "git cherry-pick --abort") {
		t.Errorf("Expected mention of 'git cherry-pick --abort', got:\n%v", err)
	}
}
