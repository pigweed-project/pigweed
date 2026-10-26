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

	if !strings.Contains(output, "✓ Change merged (submitted) successfully.") {
		t.Errorf("Unexpected output: %s", output)
	}
	if !strings.Contains(output, "Next:   gh pr view 12345") {
		t.Errorf("Expected Next hint in output: %s", output)
	}
}

func TestMergeIntegration_AutoSubmit_Pigweed(t *testing.T) {
	SetTestProfile(t, "pigweed")
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit"))
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

	if !strings.Contains(output, "✓ Auto-submit enabled for change 12345 (Pigweed-Auto-Submit+1).") {
		t.Errorf("Unexpected output: %s", output)
	}
	if !strings.Contains(output, "Next:   gh pr checks 12345 --watch") {
		t.Errorf("Expected Next hint in output: %s", output)
	}
}

// The label name belongs to the host, not to the profile: a chrome-internal or
// Chromium change calls it Auto-Submit, and voting Pigweed-Auto-Submit there is
// rejected by Gerrit as an unknown label.
func TestMergeIntegration_AutoSubmit_UsesLabelFromChange(t *testing.T) {
	SetTestProfile(t, "pigweed")
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithLabels("Code-Review", "Commit-Queue", "Auto-Submit"))
	server.OnJSON("POST", "/changes/12345/revisions/current/review", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "merge", "12345", "--auto")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	var capturedInput gerrit.ReviewInput
	if req := server.LastRequest(); req != nil {
		json.Unmarshal(req.Body, &capturedInput)
	}

	if capturedInput.Labels["Auto-Submit"] != 1 {
		t.Errorf("Labels got %v, want Auto-Submit=1", capturedInput.Labels)
	}
	if _, voted := capturedInput.Labels["Pigweed-Auto-Submit"]; voted {
		t.Errorf("Labels got %v, want no vote on the profile's label", capturedInput.Labels)
	}
	if !strings.Contains(output, "Auto-submit enabled for change 12345 (Auto-Submit+1)") {
		t.Errorf("Unexpected output: %s", output)
	}
}

// Fuchsia lands changes through the Commit Queue and has no auto-submit label,
// so --auto cannot be honored there. Voting Commit-Queue+2 and calling it
// auto-submit (as pw_ghish used to) hides that; the dry run runs the checks and
// the error says who has to submit it.
func TestMergeIntegration_AutoSubmit_NoLabelDryRunsAndFails(t *testing.T) {
	SetTestProfile(t, "fuchsia")
	server := NewMockGerritServer(t)
	server.OnDefaultChange(54321, WithLabels("Code-Review", "Commit-Queue"))
	server.OnJSON("POST", "/changes/54321/revisions/current/review", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "merge", "54321", "--auto")
	if err == nil {
		t.Fatalf("Expected an error for a host with no auto-submit label, got nil.\nOutput: %s", output)
	}

	if server.CallCount("POST", "/changes/54321/revisions/current/review") != 1 {
		t.Fatal("Expected the Commit-Queue dry run to still be requested")
	}

	var capturedInput gerrit.ReviewInput
	if req := server.LastRequest(); req != nil {
		json.Unmarshal(req.Body, &capturedInput)
	}
	// +1 is the dry run. +2 would submit the change, which is not something
	// pw_ghish should do because it could not find the label asked for.
	if capturedInput.Labels["Commit-Queue"] != 1 {
		t.Errorf("Labels got %v, want Commit-Queue=1", capturedInput.Labels)
	}

	if strings.Contains(output, "Auto-submit enabled") {
		t.Errorf("Output claims auto-submit was enabled when it was not: %s", output)
	}
	for _, want := range []string{"has no auto-submit label", "--cq"} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("Expected error to contain %q, got: %v", want, err)
		}
	}
}

// A Fuchsia host that does define an auto-submit label is auto-submitted
// through it, with no Commit-Queue consolation prize.
func TestMergeIntegration_AutoSubmit_FuchsiaPrefersChangeLabel(t *testing.T) {
	SetTestProfile(t, "fuchsia")
	server := NewMockGerritServer(t)
	server.OnDefaultChange(54321, WithLabels("Code-Review", "Commit-Queue", "Fuchsia-Auto-Submit"))
	server.OnJSON("POST", "/changes/54321/revisions/current/review", http.StatusOK, map[string]any{})

	if _, err := executeCommand(RootCmd, "pr", "merge", "54321", "--auto"); err != nil {
		t.Fatalf("Command failed: %v", err)
	}

	var capturedInput gerrit.ReviewInput
	if req := server.LastRequest(); req != nil {
		json.Unmarshal(req.Body, &capturedInput)
	}

	if capturedInput.Labels["Fuchsia-Auto-Submit"] != 1 {
		t.Errorf("Labels got %v, want Fuchsia-Auto-Submit=1", capturedInput.Labels)
	}
	if _, voted := capturedInput.Labels["Commit-Queue"]; voted {
		t.Errorf("Labels got %v, want no Commit-Queue vote alongside the real label", capturedInput.Labels)
	}
}

// Nothing to vote means nothing to do, so this fails before touching the
// change rather than doing half of what was asked.
func TestMergeIntegration_AutoSubmit_NoLabelAndNoCommitQueue(t *testing.T) {
	SetTestProfile(t, "generic")
	server := NewMockGerritServer(t)
	server.OnDefaultChange(99999, WithLabels("Code-Review"))

	output, err := executeCommand(RootCmd, "pr", "merge", "99999", "--auto")
	if err == nil {
		t.Fatal("Expected error for a host with no auto-submit label, got nil")
	}

	if !strings.Contains(output, "has no auto-submit label") {
		t.Errorf("Expected output to mention the missing auto-submit label, got: %s", output)
	}
	if server.CallCount("POST", "/changes/99999/revisions/current/review") != 0 {
		t.Error("Expected no vote to be attempted when there is nothing worth voting")
	}
}

// A change that reports no labels at all says nothing about what this host
// supports, and the profile is no longer allowed to fill in the blank.
func TestMergeIntegration_AutoSubmit_ChangeWithNoLabels(t *testing.T) {
	SetTestProfile(t, "pigweed")
	server := NewMockGerritServer(t)
	server.OnDefaultChange(99999)

	_, err := executeCommand(RootCmd, "pr", "merge", "99999", "--auto")
	if err == nil {
		t.Fatal("Expected error when the change reports no labels, got nil")
	}
	if !strings.Contains(err.Error(), "reports no labels") {
		t.Errorf("Expected the error to say the labels are unknown, got: %v", err)
	}
	if server.CallCount("POST", "/changes/99999/revisions/current/review") != 0 {
		t.Error("Expected no vote to be attempted when the labels are unknown")
	}
}

// A host that defines labels but no auto-submit label should say so, and name
// the labels it does define, rather than failing on a Gerrit 400.
func TestMergeIntegration_AutoSubmit_ErrorListsAvailableLabels(t *testing.T) {
	SetTestProfile(t, "generic")
	server := NewMockGerritServer(t)
	server.OnDefaultChange(99999, WithLabels("Code-Review", "Verified"))

	_, err := executeCommand(RootCmd, "pr", "merge", "99999", "--auto")
	if err == nil {
		t.Fatal("Expected error when no auto-submit label exists, got nil")
	}
	for _, want := range []string{"has no auto-submit label", "Code-Review, Verified", "--add-label"} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("Expected error to contain %q, got: %v", want, err)
		}
	}
}

func TestMergeIntegration_AutoSubmit_Alias(t *testing.T) {
	SetTestProfile(t, "pigweed")
	server := NewMockGerritServer(t)
	server.OnDefaultChange(11111, WithLabels("Code-Review", "Pigweed-Auto-Submit"))
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

func TestMergeIntegration_CQ_GenericProfile_WithCommitQueue(t *testing.T) {
	SetTestProfile(t, "generic")
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithLabels("Code-Review", "Commit-Queue"))
	server.OnJSON("POST", "/changes/12345/revisions/current/review", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "merge", "12345", "--cq")
	if err != nil {
		t.Fatalf("Command failed on generic profile with Commit-Queue: %v\nOutput: %s", err, output)
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

func TestMergeIntegration_CQ_GenericProfile_WithAutosubmitAndPresubmitReady(t *testing.T) {
	SetTestProfile(t, "generic")
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithLabels("Code-Review", "Autosubmit", "Presubmit-Ready"))
	server.OnJSON("POST", "/changes/12345/revisions/current/review", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "merge", "12345", "--cq")
	if err != nil {
		t.Fatalf("Command failed on generic profile with Autosubmit+Presubmit-Ready: %v\nOutput: %s", err, output)
	}

	var capturedInput gerrit.ReviewInput
	if req := server.LastRequest(); req != nil {
		json.Unmarshal(req.Body, &capturedInput)
	}
	if capturedInput.Labels["Autosubmit"] != 1 || capturedInput.Labels["Presubmit-Ready"] != 1 {
		t.Errorf("Labels got %v, want Autosubmit=1 and Presubmit-Ready=1", capturedInput.Labels)
	}
	if !strings.Contains(output, "Autosubmit+1") || !strings.Contains(output, "Presubmit-Ready+1") {
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

func TestMergeIntegration_Trigger(t *testing.T) {
	SetTestProfile(t, "generic")
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithLabels("Code-Review", "Autosubmit", "Presubmit-Ready"))
	server.OnJSON("POST", "/changes/12345/revisions/current/review", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "merge", "12345", "--trigger")
	if err != nil {
		t.Fatalf("Command failed with --trigger: %v\nOutput: %s", err, output)
	}

	var capturedInput gerrit.ReviewInput
	if req := server.LastRequest(); req != nil {
		json.Unmarshal(req.Body, &capturedInput)
	}
	if capturedInput.Labels["Autosubmit"] != 1 || capturedInput.Labels["Presubmit-Ready"] != 1 {
		t.Errorf("Labels got %v, want Autosubmit=1 and Presubmit-Ready=1", capturedInput.Labels)
	}
}

func TestMergeIntegration_Nightly(t *testing.T) {
	SetTestProfile(t, "pigweed")
	server := NewMockGerritServer(t)
	server.OnDefaultChange(123456, WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit"))
	server.OnJSON("POST", "/changes/123456/flows", http.StatusCreated, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "merge", "123456", "--nightly")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("POST", "/changes/123456/flows") != 1 {
		t.Fatalf("Expected POST /changes/123456/flows to be called once, got %d", server.CallCount("POST", "/changes/123456/flows"))
	}

	var postReqs []MockRequest
	for _, r := range server.Requests() {
		if r.Method == "POST" && r.Path == "/changes/123456/flows" {
			postReqs = append(postReqs, r)
		}
	}
	if len(postReqs) != 1 {
		t.Fatalf("Expected 1 POST request, got %d", len(postReqs))
	}

	wantFlows := []struct {
		condSubstr string
		cqVote     string
	}{
		{
			condSubstr: "/c/123456 is is:open, wait until tomorrow at 02:36 in America/Los_Angeles",
			cqVote:     "Commit-Queue+1",
		},
	}

	for i, want := range wantFlows {
		var capturedFlow FlowInput
		if err := json.Unmarshal(postReqs[i].Body, &capturedFlow); err != nil {
			t.Fatalf("request %d: failed to unmarshal FlowInput: %v", i, err)
		}
		if len(capturedFlow.StageExpressions) != 2 {
			t.Fatalf("request %d: expected 2 stage expressions, got %d", i, len(capturedFlow.StageExpressions))
		}
		wantVotes := []string{want.cqVote, "Pigweed-Auto-Submit+1"}
		for sIdx, wantVote := range wantVotes {
			stage := capturedFlow.StageExpressions[sIdx]
			if !strings.Contains(stage.Condition, want.condSubstr) {
				t.Errorf("request %d stage %d: unexpected flow condition: %q, want substring %q", i, sIdx, stage.Condition, want.condSubstr)
			}
			if stage.Action.Name != "vote" || len(stage.Action.Parameters) != 1 || stage.Action.Parameters[0] != wantVote {
				t.Errorf("request %d stage %d: unexpected flow action: %+v, want vote %s", i, sIdx, stage.Action, wantVote)
			}
		}
	}

	wantOut := "✓ Created submit flow for Change 123456 (scheduled for tomorrow at 02:36 in America/Los_Angeles)."
	if !strings.Contains(output, wantOut) {
		t.Errorf("Output got %q, want %q", output, wantOut)
	}
}

func TestMergeIntegration_SubmitAt(t *testing.T) {
	SetTestProfile(t, "pigweed")
	server := NewMockGerritServer(t)
	server.OnDefaultChange(123456, WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit"))
	server.OnJSON("POST", "/changes/123456/flows", http.StatusCreated, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "merge", "123456", "--submit-at", "21:00")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("POST", "/changes/123456/flows") != 1 {
		t.Fatalf("Expected POST /changes/123456/flows to be called once, got %d", server.CallCount("POST", "/changes/123456/flows"))
	}

	var capturedFlow FlowInput
	if req := server.LastRequest(); req != nil {
		json.Unmarshal(req.Body, &capturedFlow)
	}
	if len(capturedFlow.StageExpressions) != 2 {
		t.Fatalf("Expected 2 stage expressions, got %d", len(capturedFlow.StageExpressions))
	}
	if !strings.Contains(capturedFlow.StageExpressions[0].Condition, "wait until today at 21:00 in America/Los_Angeles") {
		t.Errorf("Unexpected flow condition: %q", capturedFlow.StageExpressions[0].Condition)
	}

	wantOut := "Created submit flow for Change 123456 (scheduled for today at 21:00 in America/Los_Angeles)."
	if !strings.Contains(output, wantOut) {
		t.Errorf("Output got %q, want %q", output, wantOut)
	}
}

func TestMergeIntegration_ScheduledFlagConflicts(t *testing.T) {
	tests := []struct {
		name    string
		args    []string
		wantErr string
	}{
		{
			name:    "nightly and submit-at",
			args:    []string{"pr", "merge", "12345", "--nightly", "--submit-at", "20:00"},
			wantErr: "cannot specify both --nightly and --submit-at",
		},
		{
			name:    "nightly and auto",
			args:    []string{"pr", "merge", "12345", "--nightly", "--auto"},
			wantErr: "cannot specify both --auto and --nightly/--submit-at",
		},
		{
			name:    "nightly and cq",
			args:    []string{"pr", "merge", "12345", "--nightly", "--cq"},
			wantErr: "cannot specify both --cq/--trigger and --nightly/--submit-at",
		},
		{
			name:    "nightly and trigger",
			args:    []string{"pr", "merge", "12345", "--nightly", "--trigger"},
			wantErr: "cannot specify both --cq/--trigger and --nightly/--submit-at",
		},
		{
			name:    "submit-at and auto",
			args:    []string{"pr", "merge", "12345", "--submit-at", "20:00", "--auto"},
			wantErr: "cannot specify both --auto and --nightly/--submit-at",
		},
		{
			name:    "submit-at and cq",
			args:    []string{"pr", "merge", "12345", "--submit-at", "20:00", "--cq"},
			wantErr: "cannot specify both --cq/--trigger and --nightly/--submit-at",
		},
		{
			name:    "invalid submit-at",
			args:    []string{"pr", "merge", "12345", "--submit-at", "25:00"},
			wantErr: "expected 24-hour format \"HH:MM\"",
		},
		{
			name:    "empty submit-at",
			args:    []string{"pr", "merge", "12345", "--submit-at", ""},
			wantErr: "expected 24-hour format \"HH:MM\"",
		},
		{
			name:    "nightly and message",
			args:    []string{"pr", "merge", "12345", "--nightly", "--message", "Landing tonight."},
			wantErr: "cannot specify --message with --nightly/--submit-at",
		},
		{
			name:    "submit-at and message",
			args:    []string{"pr", "merge", "12345", "--submit-at", "20:00", "--message", "Landing tonight."},
			wantErr: "cannot specify --message with --nightly/--submit-at",
		},
		{
			// An explicitly empty message is still a flag the user passed.
			name:    "nightly and empty message",
			args:    []string{"pr", "merge", "12345", "--nightly", "--message", ""},
			wantErr: "cannot specify --message with --nightly/--submit-at",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			_, err := executeCommand(RootCmd, tc.args...)
			if err == nil {
				t.Fatalf("Expected error for args %v, got nil", tc.args)
			}
			if !strings.Contains(err.Error(), tc.wantErr) {
				t.Errorf("Error got %q, want substring %q", err.Error(), tc.wantErr)
			}
		})
	}
}
