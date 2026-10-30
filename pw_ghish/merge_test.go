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
	server.OnReview(12345)

	output, err := executeCommand(RootCmd, "pr", "merge", "12345", "--auto", "--message", "Auto-submitting")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("POST", "/changes/12345/revisions/current/review") != 1 {
		t.Fatal("Expected SetReview API to be called for auto-submit")
	}
	server.AssertOptions(t, "GET", "/changes/12345", "DETAILED_LABELS")

	capturedInput := DecodeRequest[ReviewInput](t, server.LastRequest())

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

func TestMergeIntegration_AutoSubmit_NoLabelDryRunsAndFails(t *testing.T) {
	SetTestProfile(t, "fuchsia")
	server := NewMockGerritServer(t)
	server.OnDefaultChange(54321, WithLabels("Code-Review", "Commit-Queue"))
	server.OnReview(54321)

	output, err := executeCommand(RootCmd, "pr", "merge", "54321", "--auto")
	if err == nil {
		t.Fatalf("Expected an error for a host with no auto-submit label, got nil.\nOutput: %s", output)
	}

	if server.CallCount("POST", "/changes/54321/revisions/current/review") != 1 {
		t.Fatal("Expected SetReview API to be called for --auto fallback dry run")
	}
	server.AssertOptions(t, "GET", "/changes/54321", "DETAILED_LABELS")

	capturedInput := DecodeRequest[ReviewInput](t, server.LastRequest())
	if capturedInput.Labels["Commit-Queue"] != 1 {
		t.Errorf("Labels got %v, want Commit-Queue=1", capturedInput.Labels)
	}
	if !strings.Contains(output, "Commit-Queue+1 set for change 54321.") || strings.Contains(output, "Auto-submit enabled") {
		t.Errorf("Unexpected output: %s", output)
	}
	for _, want := range []string{"has no auto-submit label", "--cq"} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("Expected error to contain %q, got: %v", want, err)
		}
	}
}

func TestMergeIntegration_ErrorOnExplicitPatchset(t *testing.T) {
	NewMockGerritServer(t)
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
	server.OnReview(12345)

	output, err := executeCommand(RootCmd, "pr", "merge", "12345", "--cq")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("POST", "/changes/12345/revisions/current/review") != 1 {
		t.Fatal("Expected SetReview API to be called for --cq")
	}
	server.AssertOptions(t, "GET", "/changes/12345", "DETAILED_LABELS")

	capturedInput := DecodeRequest[ReviewInput](t, server.LastRequest())

	if capturedInput.Labels["Commit-Queue"] != 2 {
		t.Errorf("Labels got %v, want Commit-Queue=2", capturedInput.Labels)
	}

	if !strings.Contains(output, "Commit-Queue enabled for change 12345 (Commit-Queue+2)") {
		t.Errorf("Unexpected output: %s", output)
	}
}

// TestMergeIntegration_LabelSelection checks which label pr merge votes on
// for a change's labels under each profile, and how it fails when the change
// has no usable label.
func TestMergeIntegration_LabelSelection(t *testing.T) {
	tests := []struct {
		name    string
		profile string
		labels  []string
		args    []string
		// wantErr lists substrings of the returned error; empty means success.
		wantErr     []string
		wantOut     []string
		wantVotes   map[string]int
		wantNoVotes []string
		// wantNoReview asserts that no vote was attempted.
		wantNoReview bool
	}{
		{
			name:      "auto-submit alias",
			profile:   "pigweed",
			labels:    []string{"Code-Review", "Pigweed-Auto-Submit"},
			args:      []string{"--auto-submit"},
			wantVotes: map[string]int{"Pigweed-Auto-Submit": 1},
		},
		{
			name:         "change with no labels",
			profile:      "pigweed",
			args:         []string{"--auto"},
			wantErr:      []string{"reports no labels"},
			wantNoReview: true,
		},
		{
			name:    "error lists available labels",
			profile: "generic",
			labels:  []string{"Code-Review", "Verified"},
			args:    []string{"--auto"},
			wantErr: []string{"has no auto-submit label", "Code-Review, Verified", "--add-label"},
		},
		{
			name:        "fuchsia prefers the change's label",
			profile:     "fuchsia",
			labels:      []string{"Code-Review", "Commit-Queue", "Fuchsia-Auto-Submit"},
			args:        []string{"--auto"},
			wantVotes:   map[string]int{"Fuchsia-Auto-Submit": 1},
			wantNoVotes: []string{"Commit-Queue"},
		},
		{
			name:         "no auto-submit label and no Commit-Queue",
			profile:      "generic",
			labels:       []string{"Code-Review"},
			args:         []string{"--auto"},
			wantErr:      []string{"has no auto-submit label"},
			wantNoReview: true,
		},
		{
			name:        "uses the change's label over the profile's",
			profile:     "pigweed",
			labels:      []string{"Code-Review", "Commit-Queue", "Auto-Submit"},
			args:        []string{"--auto"},
			wantOut:     []string{"Auto-submit enabled for change 12345 (Auto-Submit+1)"},
			wantVotes:   map[string]int{"Auto-Submit": 1},
			wantNoVotes: []string{"Pigweed-Auto-Submit"},
		},
		{
			name:      "generic --cq with Autosubmit and Presubmit-Ready",
			profile:   "generic",
			labels:    []string{"Code-Review", "Autosubmit", "Presubmit-Ready"},
			args:      []string{"--cq"},
			wantOut:   []string{"Autosubmit+1", "Presubmit-Ready+1"},
			wantVotes: map[string]int{"Autosubmit": 1, "Presubmit-Ready": 1},
		},
		{
			name:      "generic --cq with Commit-Queue",
			profile:   "generic",
			labels:    []string{"Code-Review", "Commit-Queue"},
			args:      []string{"--cq"},
			wantOut:   []string{"Commit-Queue enabled for change 12345 (Commit-Queue+2)"},
			wantVotes: map[string]int{"Commit-Queue": 2},
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			SetTestProfile(t, tc.profile)
			server := NewMockGerritServer(t)
			var opts []func(map[string]any)
			if len(tc.labels) > 0 {
				opts = append(opts, WithLabels(tc.labels...))
			}
			server.OnDefaultChange(12345, opts...)
			server.OnReview(12345)

			output, err := executeCommand(RootCmd, append([]string{"pr", "merge", "12345"}, tc.args...)...)
			if len(tc.wantErr) == 0 && err != nil {
				t.Fatalf("Command failed: %v\nOutput: %s", err, output)
			}
			if len(tc.wantErr) > 0 {
				if err == nil {
					t.Fatalf("Expected an error, got nil.\nOutput: %s", output)
				}
				for _, want := range tc.wantErr {
					if !strings.Contains(err.Error(), want) {
						t.Errorf("Expected error to contain %q, got: %v", want, err)
					}
				}
			}
			for _, want := range tc.wantOut {
				if !strings.Contains(output, want) {
					t.Errorf("Expected output to contain %q, got: %s", want, output)
				}
			}
			server.AssertOptions(t, "GET", "/changes/12345", "DETAILED_LABELS")
			if tc.wantNoReview && server.CallCount("POST", "/changes/12345/revisions/current/review") != 0 {
				t.Error("Expected no vote to be attempted")
			}
			if len(tc.wantVotes) == 0 && len(tc.wantNoVotes) == 0 {
				return
			}
			input := DecodeRequest[ReviewInput](t, server.LastRequest())
			for label, want := range tc.wantVotes {
				if got := input.Labels[label]; got != want {
					t.Errorf("Labels got %v, want %s=%d", input.Labels, label, want)
				}
			}
			for _, label := range tc.wantNoVotes {
				if _, voted := input.Labels[label]; voted {
					t.Errorf("Labels got %v, want no %s vote", input.Labels, label)
				}
			}
		})
	}
}

func TestMergeIntegration_ErrorWhenBothAutoAndCQ(t *testing.T) {
	NewMockGerritServer(t)
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
	server.OnReview(12345)

	output, err := executeCommand(RootCmd, "pr", "merge", "12345", "--trigger")
	if err != nil {
		t.Fatalf("Command failed with --trigger: %v\nOutput: %s", err, output)
	}
	server.AssertOptions(t, "GET", "/changes/12345", "DETAILED_LABELS")

	capturedInput := DecodeRequest[ReviewInput](t, server.LastRequest())
	if capturedInput.Labels["Autosubmit"] != 1 || capturedInput.Labels["Presubmit-Ready"] != 1 {
		t.Errorf("Labels got %v, want Autosubmit=1 and Presubmit-Ready=1", capturedInput.Labels)
	}
}

func TestMergeIntegration_ScheduledFlow(t *testing.T) {
	tests := []struct {
		name           string
		args           []string
		wantCondSubstr string
		wantOut        string
	}{
		{
			name:           "nightly",
			args:           []string{"pr", "merge", "123456", "--nightly"},
			wantCondSubstr: "/c/123456 is is:open, wait until tomorrow at 02:36 in America/Los_Angeles",
			wantOut:        "✓ Created submit flow for Change 123456 (scheduled for tomorrow at 02:36 in America/Los_Angeles).",
		},
		{
			name:           "submit-at",
			args:           []string{"pr", "merge", "123456", "--submit-at", "21:00"},
			wantCondSubstr: "/c/123456 is is:open, wait until today at 21:00 in America/Los_Angeles",
			wantOut:        "Created submit flow for Change 123456 (scheduled for today at 21:00 in America/Los_Angeles).",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			SetTestProfile(t, "pigweed")
			server := NewMockGerritServer(t)
			server.OnDefaultChange(123456, WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit"))
			server.OnJSON("POST", "/changes/123456/flows", http.StatusCreated, map[string]any{})

			output, err := executeCommand(RootCmd, tt.args...)
			if err != nil {
				t.Fatalf("Command failed: %v\nOutput: %s", err, output)
			}

			if server.CallCount("POST", "/changes/123456/flows") != 1 {
				t.Fatalf("Expected POST /changes/123456/flows to be called once, got %d", server.CallCount("POST", "/changes/123456/flows"))
			}
			server.AssertOptions(t, "GET", "/changes/123456", "DETAILED_LABELS")

			capturedFlow := DecodeRequest[FlowInput](t, server.LastRequest())
			if len(capturedFlow.StageExpressions) != 2 {
				t.Fatalf("Expected 2 stage expressions, got %d", len(capturedFlow.StageExpressions))
			}
			wantVotes := []string{"Commit-Queue+1", "Pigweed-Auto-Submit+1"}
			for sIdx, wantVote := range wantVotes {
				stage := capturedFlow.StageExpressions[sIdx]
				if !strings.Contains(stage.Condition, tt.wantCondSubstr) {
					t.Errorf("stage %d: unexpected flow condition: %q, want substring %q", sIdx, stage.Condition, tt.wantCondSubstr)
				}
				if stage.Action.Name != "vote" || len(stage.Action.Parameters) != 1 || stage.Action.Parameters[0] != wantVote {
					t.Errorf("stage %d: unexpected flow action: %+v, want vote %s", sIdx, stage.Action, wantVote)
				}
			}

			if !strings.Contains(output, tt.wantOut) {
				t.Errorf("Output got %q, want substring %q", output, tt.wantOut)
			}
		})
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
			NewMockGerritServer(t)
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
