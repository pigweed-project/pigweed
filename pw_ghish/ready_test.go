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
)

func TestReadyIntegration(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/12345/ready", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "ready", "12345")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("POST", "/changes/12345/ready") != 1 {
		t.Error("Expected SetReadyForReview API to be called")
	}

	if !strings.Contains(output, "Change marked ready for review successfully") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestReady_ErrorWhenNotFound(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnStatus(http.StatusNotFound)

	_, err := executeCommand(RootCmd, "pr", "ready", "99999")
	if err == nil {
		t.Fatal("Expected error when change not found, got nil")
	}
	if !strings.Contains(err.Error(), "error marking change") {
		t.Errorf("Expected error to mention 'error marking change', got: %v", err)
	}
}

func TestReady_WithMessage(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/12345/ready", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "ready", "12345", "-m", "Ready for review!")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("POST", "/changes/12345/ready") != 1 {
		t.Error("Expected SetReadyForReview API to be called")
	}

	var captured map[string]any
	if req := server.LastRequest(); req != nil {
		json.Unmarshal(req.Body, &captured)
	}
	if captured["message"] != "Ready for review!" {
		t.Errorf("expected message %q, got %v", "Ready for review!", captured["message"])
	}

	if !strings.Contains(output, "Change marked ready for review successfully") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestReady_Undo(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/12345/wip", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "ready", "12345", "--undo")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("POST", "/changes/12345/wip") != 1 {
		t.Error("Expected WIP API to be called")
	}

	if !strings.Contains(output, "Change marked as work in progress successfully") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestReady_Undo_WithMessage(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/12345/wip", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "ready", "12345", "-u", "-m", "WIP: need more work")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("POST", "/changes/12345/wip") != 1 {
		t.Error("Expected WIP API to be called")
	}

	var captured map[string]any
	if req := server.LastRequest(); req != nil {
		json.Unmarshal(req.Body, &captured)
	}
	if captured["message"] != "WIP: need more work" {
		t.Errorf("expected message %q, got %v", "WIP: need more work", captured["message"])
	}

	if !strings.Contains(output, "Change marked as work in progress successfully") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestReady_Undo_ErrorWhenNotFound(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnStatus(http.StatusNotFound)

	_, err := executeCommand(RootCmd, "pr", "ready", "99999", "--undo")
	if err == nil {
		t.Fatal("Expected error when change not found, got nil")
	}
	if !strings.Contains(err.Error(), "marking change as work in progress") {
		t.Errorf("Expected error to mention 'marking change as work in progress', got: %v", err)
	}
}
