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

func TestReopenIntegration(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/12345/restore", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "reopen", "12345")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("POST", "/changes/12345/restore") != 1 {
		t.Error("Expected RestoreChange API to be called")
	}

	if !strings.Contains(output, "Change reopened (restored) successfully") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestReopen_ErrorWhenNotFound(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnStatus(http.StatusNotFound)

	_, err := executeCommand(RootCmd, "pr", "reopen", "99999")
	if err == nil {
		t.Fatal("Expected error when change not found, got nil")
	}
	if !strings.Contains(err.Error(), "error reopening change") {
		t.Errorf("Expected error to mention 'error reopening change', got: %v", err)
	}
}
