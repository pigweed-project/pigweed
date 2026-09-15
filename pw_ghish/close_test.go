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
	"net/http"
	"strings"
	"testing"
)

func TestCloseIntegration(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/12345/abandon", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "close", "12345")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("POST", "/changes/12345/abandon") != 1 {
		t.Error("Expected AbandonChange API to be called")
	}

	if !strings.Contains(output, "closed (abandoned) successfully") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestClose_ErrorWhenNotFound(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnStatus(http.StatusNotFound)

	_, err := executeCommand(RootCmd, "pr", "close", "99999")
	if err == nil {
		t.Fatal("Expected error when change not found, got nil")
	}
	if !strings.Contains(err.Error(), "error closing change") {
		t.Errorf("Expected error to mention 'error closing change', got: %v", err)
	}
}

func TestClose_ErrorWhenAlreadyMerged(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/12345/abandon", http.StatusConflict, map[string]any{
		"message": "change is merged",
	})

	_, err := executeCommand(RootCmd, "pr", "close", "12345", "--host", server.URL)
	if err == nil {
		t.Fatal("Expected error when change is already merged, got nil")
	}
	if !strings.Contains(err.Error(), "already merged") {
		t.Errorf("Expected error to mention 'already merged', got: %v", err)
	}
}
