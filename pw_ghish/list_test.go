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
	"net/http"
	"slices"
	"strings"
	"testing"
)

func TestListIntegration(t *testing.T) {
	server := NewMockGerritServer(t)
	server.On("GET", "/changes/", func(w http.ResponseWriter, r *http.Request) {
		opts := r.URL.Query()["o"]
		if !slices.Contains(opts, "DETAILED_ACCOUNTS") {
			t.Errorf("Expected DETAILED_ACCOUNTS in query options, got %v", opts)
		}
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(")]}'\n[{\"_number\": 12345, \"subject\": \"Mock change\", \"status\": \"NEW\", \"owner\": {\"name\": \"John Doe\", \"email\": \"johndoe@google.com\", \"username\": \"johndoe\"}}]"))
	})

	output, err := executeCommand(RootCmd, "pr", "list")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("GET", "/changes/") != 1 {
		t.Error("Expected ListChanges API to be called")
	}

	// Verify that the owner username is present in the output
	if !strings.Contains(output, "johndoe") {
		t.Errorf("Expected output to contain owner 'johndoe', got:\n%s", output)
	}
}

func TestList_WritesToCmdOut(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/", http.StatusOK, `[{"_number": 99999, "subject": "Custom Out Subject", "status": "NEW", "owner": {"username": "alice"}}]`)

	var buf bytes.Buffer
	resetAllFlags(RootCmd)
	RootCmd.SetOut(&buf)
	RootCmd.SetErr(&buf)
	RootCmd.SetArgs([]string{"pr", "list"})

	if err := RootCmd.Execute(); err != nil {
		t.Fatalf("RootCmd.Execute() failed: %v", err)
	}

	if !strings.Contains(buf.String(), "99999") || !strings.Contains(buf.String(), "Custom Out Subject") {
		t.Errorf("Expected buf to receive rendered output, got:\n%s", buf.String())
	}
}

func TestList_ErrorWhenInvalidState(t *testing.T) {
	_, err := executeCommand(RootCmd, "pr", "list", "--state", "bogus")
	if err == nil {
		t.Fatal("Expected error when invalid state provided, got nil")
	}
	if !strings.Contains(err.Error(), "unknown state: bogus") {
		t.Errorf("Expected unknown state error, got: %v", err)
	}
	if !strings.Contains(err.Error(), "Valid states are: 'open', 'closed', 'merged', 'all'") {
		t.Errorf("Expected valid states in error, got: %v", err)
	}
	if !strings.Contains(err.Error(), "gh pr list --state merged") {
		t.Errorf("Expected example in error, got: %v", err)
	}
}

func TestList_AuthorMeMapsToSelf(t *testing.T) {
	server := NewMockGerritServer(t)
	var capturedQuery []string
	server.On("GET", "/changes/", func(w http.ResponseWriter, r *http.Request) {
		capturedQuery = r.URL.Query()["q"]
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(")]}'\n[]"))
	})

	output, err := executeCommand(RootCmd, "pr", "list", "--author", "@me")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	foundOwnerSelf := false
	for _, q := range capturedQuery {
		if strings.Contains(q, "owner:self") {
			foundOwnerSelf = true
			break
		}
	}
	if !foundOwnerSelf {
		t.Errorf("Expected query to contain 'owner:self' when --author @me passed, got queries: %v", capturedQuery)
	}
}
