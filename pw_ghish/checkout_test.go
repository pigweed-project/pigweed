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

func TestCheckoutIntegration(t *testing.T) {
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

	output, err := executeCommand(RootCmd, "pr", "checkout", "12345")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if len(mockGit.Calls) < 2 {
		t.Fatalf("Expected at least 2 git calls. Got: %v", mockGit.Calls)
	}

	if !strings.Contains(mockGit.Calls[0], "fetch origin refs/changes/45/12345/1") {
		t.Errorf("Expected fetch call. Got: %s", mockGit.Calls[0])
	}

	if !strings.Contains(mockGit.Calls[1], "checkout FETCH_HEAD") {
		t.Errorf("Expected checkout call. Got: %s", mockGit.Calls[1])
	}
}

func TestCheckout_ErrorWhenChangeNotFound(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnStatus(http.StatusNotFound)

	_, err := executeCommand(RootCmd, "pr", "checkout", "99999")
	if err == nil {
		t.Fatal("Expected error when change is not found, got nil")
	}
	if !strings.Contains(err.Error(), "error getting change") {
		t.Errorf("Expected error message to mention 'error getting change', got: %v", err)
	}
}

func TestCheckout_SpecificPatchset(t *testing.T) {
	mockGit := SetMockGit(t, nil)

	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345", http.StatusOK, `{
		"_number": 12345,
		"current_revision": "rev2",
		"revisions": {
			"rev1": {
				"_number": 1,
				"fetch": {
					"http": {
						"ref": "refs/changes/45/12345/1"
					}
				}
			},
			"rev2": {
				"_number": 2,
				"fetch": {
					"http": {
						"ref": "refs/changes/45/12345/2"
					}
				}
			}
		}
	}`)

	output, err := executeCommand(RootCmd, "pr", "checkout", "12345/1")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if len(mockGit.Calls) < 2 {
		t.Fatalf("Expected at least 2 git calls. Got: %v", mockGit.Calls)
	}

	if !strings.Contains(mockGit.Calls[0], "fetch origin refs/changes/45/12345/1") {
		t.Errorf("Expected fetch call for patchset 1. Got: %s", mockGit.Calls[0])
	}
}

func TestCheckout_URL(t *testing.T) {
	mockGit := SetMockGit(t, nil)

	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345", http.StatusOK, `{
		"_number": 12345,
		"current_revision": "rev2",
		"revisions": {
			"rev2": {
				"_number": 2,
				"fetch": {
					"http": {
						"ref": "refs/changes/45/12345/2"
					}
				}
			}
		}
	}`)

	clURL := "https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/12345/2"
	output, err := executeCommand(RootCmd, "pr", "checkout", clURL, "--host", server.URL)
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if len(mockGit.Calls) < 2 {
		t.Fatalf("Expected at least 2 git calls. Got: %v", mockGit.Calls)
	}

	if !strings.Contains(mockGit.Calls[0], "fetch origin refs/changes/45/12345/2") {
		t.Errorf("Expected fetch call for patchset 2. Got: %s", mockGit.Calls[0])
	}
}

func TestCheckout_DefaultActivePR(t *testing.T) {
	mockGit := SetMockGit(t, &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) >= 2 && args[0] == "branch" && args[1] == "--show-current" {
				stdout.Write([]byte("my-feature\n"))
				return nil
			}
			if len(args) >= 3 && args[0] == "log" && args[1] == "-1" {
				stdout.Write([]byte("Commit\n\nChange-Id: I1234567890abcdef1234567890abcdef12345678\n"))
				return nil
			}
			return nil
		},
	})

	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/I1234567890abcdef1234567890abcdef12345678", http.StatusOK, `{
		"_number": 12345,
		"current_revision": "rev1",
		"revisions": {
			"rev1": {
				"_number": 1,
				"fetch": {
					"http": {
						"ref": "refs/changes/45/12345/1"
					}
				}
			}
		}
	}`)

	output, err := executeCommand(RootCmd, "pr", "checkout")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	fetchFound := false
	for _, call := range mockGit.Calls {
		if strings.Contains(call, "fetch origin refs/changes/45/12345/1") {
			fetchFound = true
			break
		}
	}
	if !fetchFound {
		t.Errorf("Expected fetch call for active PR in calls: %v", mockGit.Calls)
	}
}

func TestCheckout_DetachedHeadWarning(t *testing.T) {
	SetMockGit(t, nil)

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

	output, err := executeCommand(RootCmd, "pr", "checkout", "12345")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "detached HEAD") {
		t.Errorf("Expected output to mention 'detached HEAD', got:\n%s", output)
	}
	if !strings.Contains(output, "git checkout -b <branch-name> FETCH_HEAD") {
		t.Errorf("Expected output to mention branch creation command, got:\n%s", output)
	}
}

func TestCheckout_WithBranchFlag(t *testing.T) {
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

	output, err := executeCommand(RootCmd, "pr", "checkout", "12345", "-b", "my-feature")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	checkoutBranchFound := false
	for _, call := range mockGit.Calls {
		if strings.Contains(call, "checkout -b my-feature FETCH_HEAD") {
			checkoutBranchFound = true
			break
		}
	}
	if !checkoutBranchFound {
		t.Errorf("Expected 'checkout -b my-feature FETCH_HEAD' in calls: %v", mockGit.Calls)
	}
	if !strings.Contains(output, `onto new branch "my-feature"`) {
		t.Errorf("Expected output to mention checked out onto new branch, got:\n%s", output)
	}
}

func TestCheckout_ErrorWithStashHint(t *testing.T) {
	SetMockGit(t, &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) > 0 && args[0] == "checkout" {
				return fmt.Errorf("error: Your local changes would be overwritten by checkout")
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

	_, err := executeCommand(RootCmd, "pr", "checkout", "12345")
	if err == nil {
		t.Fatal("Expected error on checkout failure, got nil")
	}
	if !strings.Contains(err.Error(), "Hint: If you have uncommitted changes") {
		t.Errorf("Expected error to contain stash hint, got:\n%v", err)
	}
	if !strings.Contains(err.Error(), "git stash") {
		t.Errorf("Expected error to mention 'git stash', got:\n%v", err)
	}
}
