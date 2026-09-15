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

func TestMergeIntegration_ImmediateSubmit(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/12345/submit", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "merge", "12345")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("POST", "/changes/12345/submit") != 1 {
		t.Error("Expected SubmitChange API to be called")
	}

	if !strings.Contains(output, "merged (submitted) successfully") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestMergeIntegration_AutoSubmit_Pigweed(t *testing.T) {
	SetTestProfile(t, "pigweed")
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/12345/revisions/current/review", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "merge", "12345", "--auto", "--message", "Auto-submitting")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("POST", "/changes/12345/revisions/current/review") != 1 {
		t.Fatal("Expected SetReview API to be called for auto-submit")
	}

	var capturedInput gerrit.ReviewInput
	if req := server.LastRequest(); req != nil {
		json.Unmarshal(req.Body, &capturedInput)
	}

	if capturedInput.Labels["Pigweed-Auto-Submit"] != 1 {
		t.Errorf("Labels got %v, want Pigweed-Auto-Submit=1", capturedInput.Labels)
	}
	if capturedInput.Message != "Auto-submitting" {
		t.Errorf("Message got %q, want %q", capturedInput.Message, "Auto-submitting")
	}

	if !strings.Contains(output, "Auto-submit enabled for change 12345 (Pigweed-Auto-Submit+1)") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestMergeIntegration_AutoSubmit_Fuchsia(t *testing.T) {
	SetTestProfile(t, "fuchsia")
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/54321/revisions/current/review", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "merge", "54321", "--auto")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("POST", "/changes/54321/revisions/current/review") != 1 {
		t.Fatal("Expected SetReview API to be called for Fuchsia auto-submit")
	}

	var capturedInput gerrit.ReviewInput
	if req := server.LastRequest(); req != nil {
		json.Unmarshal(req.Body, &capturedInput)
	}

	if capturedInput.Labels["Commit-Queue"] != 2 {
		t.Errorf("Labels got %v, want Commit-Queue=2", capturedInput.Labels)
	}

	if !strings.Contains(output, "Auto-submit enabled for change 54321 (Commit-Queue+2)") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestMergeIntegration_AutoSubmit_Generic(t *testing.T) {
	SetTestProfile(t, "generic")
	server := NewMockGerritServer(t)
	server.OnJSON("", "", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "merge", "99999", "--auto")
	if err == nil {
		t.Fatal("Expected error for generic profile auto-submit, got nil")
	}

	if !strings.Contains(output, "does not support auto-submit") {
		t.Errorf("Expected output to mention unsupported auto-submit, got: %s", output)
	}
}

func TestMergeIntegration_AutoSubmit_Alias(t *testing.T) {
	SetTestProfile(t, "pigweed")
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/11111/revisions/current/review", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "merge", "11111", "--auto-submit")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("POST", "/changes/11111/revisions/current/review") != 1 {
		t.Fatal("Expected SetReview API to be called using --auto-submit alias")
	}
}

func TestMergeIntegration_ErrorOnExplicitPatchset(t *testing.T) {
	_, err := executeCommand(RootCmd, "pr", "merge", "12345/3")
	if err == nil {
		t.Fatal("Expected error when merging explicit patchset, got nil")
	}
	if !strings.Contains(err.Error(), "cannot merge specific patchset") {
		t.Errorf("Expected error about specific patchset, got: %v", err)
	}
}

func TestMergeIntegration_CQ_Pigweed(t *testing.T) {
	SetTestProfile(t, "pigweed")
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/12345/revisions/current/review", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "merge", "12345", "--cq")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("POST", "/changes/12345/revisions/current/review") != 1 {
		t.Fatal("Expected SetReview API to be called for --cq")
	}

	var capturedInput gerrit.ReviewInput
	if req := server.LastRequest(); req != nil {
		json.Unmarshal(req.Body, &capturedInput)
	}

	if capturedInput.Labels["Commit-Queue"] != 2 {
		t.Errorf("Labels got %v, want Commit-Queue=2", capturedInput.Labels)
	}

	if !strings.Contains(output, "Commit-Queue enabled for change 12345 (Commit-Queue+2)") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestMergeIntegration_ErrorWhenBothAutoAndCQ(t *testing.T) {
	_, err := executeCommand(RootCmd, "pr", "merge", "12345", "--auto", "--cq")
	if err == nil {
		t.Fatal("Expected error when passing both --auto and --cq, got nil")
	}
	if !strings.Contains(err.Error(), "cannot specify both --auto and --cq") {
		t.Errorf("Unexpected error: %v", err)
	}
}

func TestMergeIntegration_SubmitConflictOfframp(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnString("POST", "/changes/12345/submit", http.StatusConflict, "text/plain", "change 12345: submit requirements not met: Presubmit-Verified is missing")

	_, err := executeCommand(RootCmd, "pr", "merge", "12345")
	if err == nil {
		t.Fatal("Expected error on 409 conflict, got nil")
	}

	if !strings.Contains(err.Error(), "submit requirements") {
		t.Errorf("Expected error to mention submit requirements, got: %v", err)
	}
	expectedAuto := RootCmd.CommandPath() + " pr merge 12345 --auto"
	if !strings.Contains(err.Error(), expectedAuto) {
		t.Errorf("Expected hint mentioning %q, got: %v", expectedAuto, err)
	}
	expectedCQ := RootCmd.CommandPath() + " pr merge 12345 --cq"
	if !strings.Contains(err.Error(), expectedCQ) {
		t.Errorf("Expected hint mentioning %q, got: %v", expectedCQ, err)
	}
}
