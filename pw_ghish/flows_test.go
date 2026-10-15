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
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"slices"
	"strings"
	"testing"
	"time"
)

func TestComputeScheduleTime(t *testing.T) {
	tests := []struct {
		name       string
		changeNum  int
		customTime string
		want       string
		wantErr    bool
	}{
		{
			name:      "default jitter 123456",
			changeNum: 123456, // 123456 % 360 = 336 -> 5h 36m after 21:00 -> 02:36
			want:      "02:36",
		},
		{
			name:      "default jitter 0",
			changeNum: 0,
			want:      "21:00",
		},
		{
			name:      "default jitter negative clamped to 0",
			changeNum: -42,
			want:      "21:00",
		},
		{
			name:      "default jitter 179 before midnight",
			changeNum: 179,
			want:      "23:59",
		},
		{
			name:      "default jitter 180 midnight",
			changeNum: 180,
			want:      "00:00",
		},
		{
			name:      "default jitter 359 max window",
			changeNum: 359,
			want:      "02:59",
		},
		{
			name:      "default jitter 360 wraps around",
			changeNum: 360,
			want:      "21:00",
		},
		{
			name:       "custom time 20:30",
			changeNum:  123456,
			customTime: "20:30",
			want:       "20:30",
		},
		{
			name:       "custom time with whitespace",
			changeNum:  123456,
			customTime: "  09:05 ",
			want:       "09:05",
		},
		{
			name:       "custom time single digit hour is zero-padded",
			changeNum:  123456,
			customTime: "9:05",
			want:       "09:05",
		},
		{
			name:       "custom time midnight",
			changeNum:  123456,
			customTime: "00:00",
			want:       "00:00",
		},
		{
			name:       "custom time end of day",
			changeNum:  123456,
			customTime: "23:59",
			want:       "23:59",
		},
		{
			name:       "invalid whitespace only",
			changeNum:  123456,
			customTime: "   ",
			wantErr:    true,
		},
		{
			name:       "invalid 12-hour format",
			changeNum:  123456,
			customTime: "8pm",
			wantErr:    true,
		},
		{
			name:       "invalid hour 25:00",
			changeNum:  123456,
			customTime: "25:00",
			wantErr:    true,
		},
		{
			name:       "invalid minute 19:60",
			changeNum:  123456,
			customTime: "19:60",
			wantErr:    true,
		},
		{
			name:       "invalid phrase today at 20:30",
			changeNum:  123456,
			customTime: "today at 20:30",
			wantErr:    true,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got, err := ComputeScheduleTime(tc.changeNum, tc.customTime)
			if tc.wantErr {
				if err == nil {
					t.Fatalf("ComputeScheduleTime(%d, %q) succeeded with %q, want error", tc.changeNum, tc.customTime, got)
				}
				if !strings.Contains(err.Error(), "expected 24-hour format \"HH:MM\"") {
					t.Errorf("unexpected error message: %v", err)
				}
				return
			}
			if err != nil {
				t.Fatalf("ComputeScheduleTime(%d, %q) failed: %v", tc.changeNum, tc.customTime, err)
			}
			if got != tc.want {
				t.Errorf("ComputeScheduleTime(%d, %q) = %q, want %q", tc.changeNum, tc.customTime, got, tc.want)
			}
		})
	}
}

func TestValidateScheduleTime_EmptyRejected(t *testing.T) {
	if _, err := ValidateScheduleTime(""); err == nil {
		t.Fatal("expected ValidateScheduleTime(\"\") to return an error, got nil")
	}
}

func TestAddScheduleMinutes(t *testing.T) {
	tests := []struct {
		hhmm string
		d    time.Duration
		want string
	}{
		{"21:00", 0, "21:00"},
		{"21:00", 30 * time.Minute, "21:30"},
		{"23:45", 30 * time.Minute, "00:15"},
		// Callers pass ValidateScheduleTime's zero-padded output, but a
		// single-digit hour still parses.
		{"9:00", 30 * time.Minute, "09:30"},
	}
	for _, tc := range tests {
		got, err := addScheduleMinutes(tc.hhmm, tc.d)
		if err != nil {
			t.Errorf("addScheduleMinutes(%q, %v) failed: %v", tc.hhmm, tc.d, err)
			continue
		}
		if got != tc.want {
			t.Errorf("addScheduleMinutes(%q, %v) = %q, want %q", tc.hhmm, tc.d, got, tc.want)
		}
	}
}

func TestCreateScheduledSubmitFlow_NumericChangeID(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(123456, WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit"))
	server.OnJSON("POST", "/changes/123456/flows", http.StatusCreated, map[string]any{})

	chCtx := &ChangeContext{
		Context:  context.Background(),
		Config:   &Config{Host: "https://pigweed-review.googlesource.com/a"},
		Client:   server.Client(),
		ChangeID: "123456",
	}

	scheduled, err := chCtx.CreateScheduledSubmitFlow(0, "", DefaultScheduleTimezone)
	if err != nil {
		t.Fatalf("CreateScheduledSubmitFlow failed: %v", err)
	}
	if scheduled.ChangeNum != 123456 {
		t.Errorf("ChangeNum = %d, want 123456", scheduled.ChangeNum)
	}
	if scheduled.When != "tomorrow at 02:36" {
		t.Errorf("When = %q, want \"tomorrow at 02:36\"", scheduled.When)
	}
	if scheduled.Replaced != 0 {
		t.Errorf("Replaced = %d, want 0", scheduled.Replaced)
	}

	if server.CallCount("POST", "/changes/123456/flows") != 1 {
		t.Fatalf("expected 1 POST to /changes/123456/flows, got %d", server.CallCount("POST", "/changes/123456/flows"))
	}

	var postReqs []MockRequest
	for _, r := range server.Requests() {
		if r.Method == "POST" && r.Path == "/changes/123456/flows" {
			postReqs = append(postReqs, r)
		}
	}
	if len(postReqs) != 1 {
		t.Fatalf("len(postReqs) = %d, want 1", len(postReqs))
	}

	wantFlows := []struct {
		cond string
		vote string
	}{
		{
			cond: "https://pigweed-review.googlesource.com/c/123456 is is:open, wait until tomorrow at 02:36 in America/Los_Angeles",
			vote: "Commit-Queue+1",
		},
	}

	for i, want := range wantFlows {
		var input FlowInput
		if err := json.Unmarshal(postReqs[i].Body, &input); err != nil {
			t.Fatalf("request %d: failed to unmarshal FlowInput: %v", i, err)
		}
		if len(input.StageExpressions) != 2 {
			t.Fatalf("request %d: len(StageExpressions) = %d, want 2", i, len(input.StageExpressions))
		}
		wantVotes := []string{want.vote, "Pigweed-Auto-Submit+1"}
		for sIdx, wantVote := range wantVotes {
			stage := input.StageExpressions[sIdx]
			if stage.Condition != want.cond {
				t.Errorf("request %d stage %d: Condition = %q, want %q", i, sIdx, stage.Condition, want.cond)
			}
			if stage.Action.Name != "vote" {
				t.Errorf("request %d stage %d: Action.Name = %q, want \"vote\"", i, sIdx, stage.Action.Name)
			}
			if len(stage.Action.Parameters) != 1 || stage.Action.Parameters[0] != wantVote {
				t.Errorf("request %d stage %d: Action.Parameters = %v, want [%q]", i, sIdx, stage.Action.Parameters, wantVote)
			}
		}
	}
}

func TestCreateScheduledSubmitFlow_ResolvesChangeID(t *testing.T) {
	changeID := "I1234567890123456789012345678901234567890"
	server := NewMockGerritServer(t)
	server.OnDefaultChange(123456, WithChangeID(changeID), WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit"))
	server.OnJSON("POST", "/changes/123456/flows", http.StatusCreated, map[string]any{})

	cleanHost := CleanGerritHost(server.URL)
	chCtx := &ChangeContext{
		Context:  context.Background(),
		Config:   &Config{Host: server.URL},
		Client:   server.Client(),
		ChangeID: changeID,
	}

	scheduled, err := chCtx.CreateScheduledSubmitFlow(0, "21:15", DefaultScheduleTimezone)
	if err != nil {
		t.Fatalf("CreateScheduledSubmitFlow failed: %v", err)
	}
	if scheduled.ChangeNum != 123456 {
		t.Errorf("ChangeNum = %d, want 123456", scheduled.ChangeNum)
	}
	if scheduled.When != "today at 21:15" {
		t.Errorf("When = %q, want \"today at 21:15\"", scheduled.When)
	}

	if server.CallCount("POST", "/changes/123456/flows") != 1 {
		t.Fatalf("expected 1 POST to /changes/123456/flows, got %d", server.CallCount("POST", "/changes/123456/flows"))
	}
	var postReqs []MockRequest
	for _, r := range server.Requests() {
		if r.Method == "POST" && r.Path == "/changes/123456/flows" {
			postReqs = append(postReqs, r)
		}
	}
	if len(postReqs) != 1 {
		t.Fatalf("len(postReqs) = %d, want 1", len(postReqs))
	}

	wantFlows := []struct {
		cond string
		vote string
	}{
		{
			cond: fmt.Sprintf("https://%s/c/123456 is is:open, wait until today at 21:15 in America/Los_Angeles", cleanHost),
			vote: "Commit-Queue+1",
		},
	}

	for i, want := range wantFlows {
		var input FlowInput
		if err := json.Unmarshal(postReqs[i].Body, &input); err != nil {
			t.Fatalf("post %d: failed to unmarshal FlowInput: %v", i, err)
		}
		if len(input.StageExpressions) != 2 {
			t.Fatalf("post %d: len(StageExpressions) = %d, want 2", i, len(input.StageExpressions))
		}
		wantVotes := []string{want.vote, "Pigweed-Auto-Submit+1"}
		for sIdx, wantVote := range wantVotes {
			if input.StageExpressions[sIdx].Condition != want.cond {
				t.Errorf("post %d stage %d: Condition = %q, want %q", i, sIdx, input.StageExpressions[sIdx].Condition, want.cond)
			}
			if len(input.StageExpressions[sIdx].Action.Parameters) != 1 || input.StageExpressions[sIdx].Action.Parameters[0] != wantVote {
				t.Errorf("post %d stage %d: Action = %+v, want vote %q", i, sIdx, input.StageExpressions[sIdx].Action, wantVote)
			}
		}
	}
}

func TestCreateScheduledSubmitFlow_APIError(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(123456, WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit"))
	server.OnString("POST", "/changes/123456/flows", http.StatusInternalServerError, "text/plain", "flows disabled")

	chCtx := &ChangeContext{
		Context:  context.Background(),
		Config:   &Config{Host: "https://pigweed-review.googlesource.com/a"},
		Client:   server.Client(),
		ChangeID: "123456",
	}

	_, err := chCtx.CreateScheduledSubmitFlow(123456, "", DefaultScheduleTimezone)
	if err == nil {
		t.Fatal("expected error when POST /flows fails, got nil")
	}
	if !strings.Contains(err.Error(), "creating submit flow on change 123456") {
		t.Errorf("unexpected error: %v", err)
	}
}

// The flows vote whichever auto-submit label the change reports, not a name
// derived from the host.
func TestCreateScheduledSubmitFlow_UsesReportedAutoSubmitLabel(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(123456, WithLabels("Code-Review", "Commit-Queue", "Auto-Submit"))
	server.OnJSON("POST", "/changes/123456/flows", http.StatusCreated, map[string]any{})

	chCtx := &ChangeContext{
		Context:  context.Background(),
		Config:   &Config{Host: "https://pigweed-review.googlesource.com/a"},
		Client:   server.Client(),
		ChangeID: "123456",
	}

	if _, err := chCtx.CreateScheduledSubmitFlow(123456, "", DefaultScheduleTimezone); err != nil {
		t.Fatalf("CreateScheduledSubmitFlow failed: %v", err)
	}
	for _, r := range server.Requests() {
		if r.Method != "POST" || r.Path != "/changes/123456/flows" {
			continue
		}
		var input FlowInput
		if err := json.Unmarshal(r.Body, &input); err != nil {
			t.Fatalf("failed to unmarshal FlowInput: %v", err)
		}
		if got := input.StageExpressions[1].Action.Parameters; len(got) != 1 || got[0] != "Auto-Submit+1" {
			t.Errorf("auto-submit stage votes %v, want [Auto-Submit+1]", got)
		}
	}
}

// A host with no auto-submit label is refused before any flow is created,
// rather than scheduling a vote on a label that will be rejected hours later.
func TestCreateScheduledSubmitFlow_NoAutoSubmitLabel(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(123456, WithLabels("Code-Review", "Commit-Queue"))
	server.OnJSON("POST", "/changes/123456/flows", http.StatusCreated, map[string]any{})

	chCtx := &ChangeContext{
		Context:  context.Background(),
		Config:   &Config{Host: "https://pigweed-review.googlesource.com/a"},
		Client:   server.Client(),
		ChangeID: "123456",
	}

	_, err := chCtx.CreateScheduledSubmitFlow(123456, "", DefaultScheduleTimezone)
	if err == nil {
		t.Fatal("expected an error for a change with no auto-submit label, got nil")
	}
	for _, want := range []string{"has no auto-submit label", "Code-Review, Commit-Queue", "pr merge 123456 --cq"} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("error missing %q, got: %v", want, err)
		}
	}
	if n := server.CallCount("POST", "/changes/123456/flows"); n != 0 {
		t.Errorf("expected no flows to be created, got %d POSTs", n)
	}
}

// A host that triggers presubmits with Presubmit-Ready rather than a Commit
// Queue gets Presubmit-Ready+1 as the dry-run vote, and a pending flow cast
// with those votes is recognized and replaced.
func TestCreateScheduledSubmitFlow_PresubmitReadyHost(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(123456, WithLabels("Code-Review", "Presubmit-Ready", "Autosubmit"))
	server.OnFlows(123456, scheduledFlowFixture("old", "Presubmit-Ready+1", "Autosubmit+1", "PENDING", listParams))
	server.OnJSON("DELETE", "/changes/123456/flows/old", http.StatusNoContent, map[string]any{})
	server.OnJSON("POST", "/changes/123456/flows", http.StatusCreated, map[string]any{})

	chCtx := &ChangeContext{
		Context:  context.Background(),
		Config:   &Config{Host: "https://pigweed-review.googlesource.com/a"},
		Client:   server.Client(),
		ChangeID: "123456",
	}

	scheduled, err := chCtx.CreateScheduledSubmitFlow(123456, "", DefaultScheduleTimezone)
	if err != nil {
		t.Fatalf("CreateScheduledSubmitFlow failed: %v", err)
	}
	if scheduled.Replaced != 1 {
		t.Errorf("Replaced = %d, want 1", scheduled.Replaced)
	}
	var input FlowInput
	found := false
	for _, r := range server.Requests() {
		if r.Method == "POST" && r.Path == "/changes/123456/flows" {
			if err := json.Unmarshal(r.Body, &input); err != nil {
				t.Fatalf("failed to unmarshal FlowInput: %v", err)
			}
			found = true
		}
	}
	if !found {
		t.Fatal("expected POST /changes/123456/flows")
	}
	var got []string
	for _, stage := range input.StageExpressions {
		got = append(got, stage.Action.Parameters...)
	}
	if want := []string{"Presubmit-Ready+1", "Autosubmit+1"}; !slices.Equal(got, want) {
		t.Errorf("flow votes = %v, want %v", got, want)
	}
}

// A host with an auto-submit label but nothing to start presubmits with is
// refused before any flow is created.
func TestCreateScheduledSubmitFlow_NoDryRunLabel(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(123456, WithLabels("Code-Review", "Autosubmit"))
	server.OnJSON("POST", "/changes/123456/flows", http.StatusCreated, map[string]any{})

	chCtx := &ChangeContext{
		Context:  context.Background(),
		Config:   &Config{Host: "https://pigweed-review.googlesource.com/a"},
		Client:   server.Client(),
		ChangeID: "123456",
	}

	_, err := chCtx.CreateScheduledSubmitFlow(123456, "", DefaultScheduleTimezone)
	if err == nil {
		t.Fatal("expected an error for a change with no dry-run label, got nil")
	}
	for _, want := range []string{"no Commit-Queue or Presubmit-Ready label", "Autosubmit, Code-Review", "pr merge 123456 --auto"} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("error missing %q, got: %v", want, err)
		}
	}
	if n := server.CallCount("GET", "/changes/123456/flows"); n != 0 {
		t.Errorf("expected existing flows to be left alone, got %d GETs", n)
	}
	if n := server.CallCount("POST", "/changes/123456/flows"); n != 0 {
		t.Errorf("expected no flows to be created, got %d POSTs", n)
	}
}

// A failed label lookup is an error, not a cue to guess the label name.
func TestCreateScheduledSubmitFlow_LabelLookupFails(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnString("GET", "/changes/123456*", http.StatusInternalServerError, "text/plain", "backend unavailable")
	server.OnJSON("POST", "/changes/123456/flows", http.StatusCreated, map[string]any{})

	chCtx := &ChangeContext{
		Context:  context.Background(),
		Config:   &Config{Host: "https://pigweed-review.googlesource.com/a"},
		Client:   server.Client(),
		ChangeID: "123456",
	}

	if _, err := chCtx.CreateScheduledSubmitFlow(123456, "", DefaultScheduleTimezone); err == nil {
		t.Fatal("expected an error when the change lookup fails, got nil")
	}
	if n := server.CallCount("POST", "/changes/123456/flows"); n != 0 {
		t.Errorf("expected no flows to be created, got %d POSTs", n)
	}
}

// scheduledFlowFixture returns a flow shaped like the ones
// CreateScheduledSubmitFlow creates, with both stages in state. params formats
// a vote as the flow service would return it.
func scheduledFlowFixture(uuid, cqVote, autoSubmitVote, state string, params func(string) json.RawMessage) FlowInfo {
	cond := "https://pigweed-review.googlesource.com/c/123456 is is:open, wait until today at 21:00 in America/Los_Angeles"
	return FlowInfo{
		UUID: uuid,
		Stages: []FlowStageInfo{
			{Expression: FlowExpressionInfo{Condition: cond, Action: FlowActionInfo{Name: "vote", Parameters: params(cqVote)}}, State: state},
			{Expression: FlowExpressionInfo{Condition: cond, Action: FlowActionInfo{Name: "vote", Parameters: params(autoSubmitVote)}}, State: state},
		},
	}
}

func listParams(v string) json.RawMessage { return json.RawMessage(fmt.Sprintf("[%q]", v)) }
func mapParams(v string) json.RawMessage  { return json.RawMessage(fmt.Sprintf(`{"vote":%q}`, v)) }

// Re-running --nightly/--submit-at replaces the pending schedule rather than
// adding another flow that would also fire. Finished flows, flows that do
// something else, and flows voting a different label are left alone.
func TestCreateScheduledSubmitFlow_ReplacesPendingFlows(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(123456, WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit"))
	server.OnFlows(123456,
		scheduledFlowFixture("pending-list", "Commit-Queue+1", "Pigweed-Auto-Submit+1", "PENDING", listParams),
		scheduledFlowFixture("pending-map", "Commit-Queue+1", "Pigweed-Auto-Submit+1", "PENDING", mapParams),
		scheduledFlowFixture("done", "Commit-Queue+1", "Pigweed-Auto-Submit+1", "DONE", listParams),
		scheduledFlowFixture("cq2", "Commit-Queue+2", "Pigweed-Auto-Submit+1", "PENDING", listParams),
		scheduledFlowFixture("other-label", "Commit-Queue+1", "Auto-Submit+1", "PENDING", listParams),
		FlowInfo{UUID: "unrelated", Stages: []FlowStageInfo{{
			Expression: FlowExpressionInfo{
				Condition: "com.google.gerrit[change:123456 label:Verified+1]",
				Action:    FlowActionInfo{Name: "AddReviewer", Parameters: json.RawMessage(`{"user":"foo@example.com"}`)},
			},
			State: "PENDING",
		}}},
	)
	server.OnStatus(http.StatusNoContent) // DELETE /changes/123456/flows/<uuid>
	server.OnJSON("POST", "/changes/123456/flows", http.StatusCreated, map[string]any{})

	chCtx := &ChangeContext{
		Context:  context.Background(),
		Config:   &Config{Host: "https://pigweed-review.googlesource.com/a"},
		Client:   server.Client(),
		ChangeID: "123456",
	}

	scheduled, err := chCtx.CreateScheduledSubmitFlow(123456, "", DefaultScheduleTimezone)
	if err != nil {
		t.Fatalf("CreateScheduledSubmitFlow failed: %v", err)
	}
	if scheduled.Replaced != 2 {
		t.Errorf("Replaced = %d, want 2", scheduled.Replaced)
	}
	if !strings.Contains(scheduled.Summary(), "Replaced 2 previously scheduled submit flow(s).") {
		t.Errorf("Summary() = %q, want it to mention the 2 replaced flows", scheduled.Summary())
	}

	var deleted []string
	sawPost := false
	for _, r := range server.Requests() {
		switch {
		case r.Method == "POST" && r.Path == "/changes/123456/flows":
			sawPost = true
		case r.Method == "DELETE":
			if sawPost {
				t.Errorf("DELETE %s came after a POST; old flows must be removed first", r.Path)
			}
			deleted = append(deleted, strings.TrimPrefix(r.Path, "/a"))
		}
	}
	want := []string{"/changes/123456/flows/pending-list", "/changes/123456/flows/pending-map"}
	if !slices.Equal(deleted, want) {
		t.Errorf("deleted %v, want %v", deleted, want)
	}
	if n := server.CallCount("POST", "/changes/123456/flows"); n != 1 {
		t.Errorf("expected 1 POST, got %d", n)
	}
}

// If an old flow cannot be deleted, no new flow is created, so a failure
// never leaves more flows than before.
func TestCreateScheduledSubmitFlow_DeleteFails(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(123456, WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit"))
	server.OnFlows(123456, scheduledFlowFixture("pending", "Commit-Queue+1", "Pigweed-Auto-Submit+1", "PENDING", listParams))
	server.OnString("DELETE", "/changes/123456/flows/pending", http.StatusInternalServerError, "text/plain", "backend unavailable")
	server.OnJSON("POST", "/changes/123456/flows", http.StatusCreated, map[string]any{})

	chCtx := &ChangeContext{
		Context:  context.Background(),
		Config:   &Config{Host: "https://pigweed-review.googlesource.com/a"},
		Client:   server.Client(),
		ChangeID: "123456",
	}

	_, err := chCtx.CreateScheduledSubmitFlow(123456, "", DefaultScheduleTimezone)
	if err == nil {
		t.Fatal("expected an error when deleting the old flow fails, got nil")
	}
	if !strings.Contains(err.Error(), "deleting previously scheduled submit flow") {
		t.Errorf("unexpected error: %v", err)
	}
	if n := server.CallCount("POST", "/changes/123456/flows"); n != 0 {
		t.Errorf("expected no flows to be created, got %d POSTs", n)
	}
}

// If the existing flows cannot be listed, no new flows are created: there is
// no way to tell whether they would duplicate a schedule already in place.
func TestCreateScheduledSubmitFlow_ListFails(t *testing.T) {
	server := NewMockGerritServer(t)
	// Registered first so it wins over OnDefaultChange's empty flow list.
	server.OnString("GET", "/changes/123456/flows", http.StatusInternalServerError, "text/plain", "backend unavailable")
	server.OnDefaultChange(123456, WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit"))
	server.OnJSON("POST", "/changes/123456/flows", http.StatusCreated, map[string]any{})

	chCtx := &ChangeContext{
		Context:  context.Background(),
		Config:   &Config{Host: "https://pigweed-review.googlesource.com/a"},
		Client:   server.Client(),
		ChangeID: "123456",
	}

	_, err := chCtx.CreateScheduledSubmitFlow(123456, "", DefaultScheduleTimezone)
	if err == nil {
		t.Fatal("expected an error when listing flows fails, got nil")
	}
	if !strings.Contains(err.Error(), "listing existing flows") {
		t.Errorf("unexpected error: %v", err)
	}
	if n := server.CallCount("POST", "/changes/123456/flows"); n != 0 {
		t.Errorf("expected no flows to be created, got %d POSTs", n)
	}
}

// A work-in-progress change is marked ready for review before its submit is
// scheduled: a scheduled submit says the change is done.
func TestCreateScheduledSubmitFlow_MarksWorkInProgressReady(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(123456,
		WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit"),
		func(m map[string]any) { m["work_in_progress"] = true },
	)
	server.OnJSON("POST", "/changes/123456/ready", http.StatusOK, map[string]any{})
	server.OnJSON("POST", "/changes/123456/flows", http.StatusCreated, map[string]any{})

	chCtx := &ChangeContext{
		Context:  context.Background(),
		Config:   &Config{Host: "https://pigweed-review.googlesource.com/a"},
		Client:   server.Client(),
		ChangeID: "123456",
	}

	scheduled, err := chCtx.CreateScheduledSubmitFlow(123456, "", DefaultScheduleTimezone)
	if err != nil {
		t.Fatalf("CreateScheduledSubmitFlow failed: %v", err)
	}
	if !scheduled.MarkedReady {
		t.Error("MarkedReady = false, want true")
	}
	if !strings.Contains(scheduled.Summary(), "Marked it ready for review.") {
		t.Errorf("Summary() = %q, want it to mention marking ready", scheduled.Summary())
	}

	readyIdx, flowIdx := -1, -1
	for i, r := range server.Requests() {
		if r.Method != "POST" {
			continue
		}
		switch r.Path {
		case "/changes/123456/ready":
			readyIdx = i
		case "/changes/123456/flows":
			flowIdx = i
		}
	}
	if readyIdx < 0 {
		t.Fatal("expected POST /changes/123456/ready")
	}
	if flowIdx < 0 {
		t.Fatal("expected POST /changes/123456/flows")
	}
	if readyIdx > flowIdx {
		t.Error("expected the change to be marked ready before the flow was created")
	}
}

// A change that is already ready for review is left alone.
func TestCreateScheduledSubmitFlow_ReadyChangeNotMarked(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(123456, WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit"))
	server.OnJSON("POST", "/changes/123456/ready", http.StatusOK, map[string]any{})
	server.OnJSON("POST", "/changes/123456/flows", http.StatusCreated, map[string]any{})

	chCtx := &ChangeContext{
		Context:  context.Background(),
		Config:   &Config{Host: "https://pigweed-review.googlesource.com/a"},
		Client:   server.Client(),
		ChangeID: "123456",
	}

	scheduled, err := chCtx.CreateScheduledSubmitFlow(123456, "", DefaultScheduleTimezone)
	if err != nil {
		t.Fatalf("CreateScheduledSubmitFlow failed: %v", err)
	}
	if scheduled.MarkedReady {
		t.Error("MarkedReady = true, want false")
	}
	if strings.Contains(scheduled.Summary(), "ready for review") {
		t.Errorf("Summary() = %q, should not mention marking ready", scheduled.Summary())
	}
	if n := server.CallCount("POST", "/changes/123456/ready"); n != 0 {
		t.Errorf("expected no POST to /ready, got %d", n)
	}
}

// If the change cannot be marked ready, no flow is created or deleted.
func TestCreateScheduledSubmitFlow_MarkReadyFails(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(123456,
		WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit"),
		func(m map[string]any) { m["work_in_progress"] = true },
	)
	server.OnString("POST", "/changes/123456/ready", http.StatusForbidden, "text/plain", "not permitted")
	server.OnJSON("POST", "/changes/123456/flows", http.StatusCreated, map[string]any{})

	chCtx := &ChangeContext{
		Context:  context.Background(),
		Config:   &Config{Host: "https://pigweed-review.googlesource.com/a"},
		Client:   server.Client(),
		ChangeID: "123456",
	}

	_, err := chCtx.CreateScheduledSubmitFlow(123456, "", DefaultScheduleTimezone)
	if err == nil {
		t.Fatal("expected an error when marking ready fails, got nil")
	}
	if !strings.Contains(err.Error(), "marking ready for review") {
		t.Errorf("unexpected error: %v", err)
	}
	if n := server.CallCount("GET", "/changes/123456/flows"); n != 0 {
		t.Errorf("expected existing flows to be left alone, got %d GETs", n)
	}
	if n := server.CallCount("POST", "/changes/123456/flows"); n != 0 {
		t.Errorf("expected no flows to be created, got %d POSTs", n)
	}
}

// If the host does not have Gerrit Flows enabled, scheduling fails with an
// actionable error before anything on the change is modified: the change is
// not marked ready, and no flow is listed, deleted, or created.
func TestCreateScheduledSubmitFlow_FlowsNotEnabled(t *testing.T) {
	tests := []struct {
		name    string
		status  int
		enabled bool
		want    string
	}{
		{"reported disabled", http.StatusOK, false, "not enabled for this change"},
		{"no flow service", http.StatusMethodNotAllowed, false, "no flow service"},
		{"endpoint missing", http.StatusNotFound, false, "Gerrit 3.13"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			server := NewMockGerritServer(t)
			server.OnDefaultChange(123456,
				WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit"),
				func(m map[string]any) { m["work_in_progress"] = true },
			)
			server.OnFlowsEnabled(123456, tc.status, tc.enabled)
			server.OnJSON("POST", "/changes/123456/ready", http.StatusOK, map[string]any{})
			server.OnJSON("POST", "/changes/123456/flows", http.StatusCreated, map[string]any{})

			chCtx := &ChangeContext{
				Context:  context.Background(),
				Config:   &Config{Host: "https://pigweed-review.googlesource.com/a"},
				Client:   server.Client(),
				ChangeID: "123456",
			}

			_, err := chCtx.CreateScheduledSubmitFlow(123456, "", DefaultScheduleTimezone)
			var notEnabled *FlowsNotEnabledError
			if !errors.As(err, &notEnabled) {
				t.Fatalf("expected a *FlowsNotEnabledError, got %T: %v", err, err)
			}
			for _, want := range []string{
				"Gerrit Flows are not enabled",
				"pigweed-review.googlesource.com",
				tc.want,
				"pr merge 123456 --auto",
			} {
				if !strings.Contains(err.Error(), want) {
					t.Errorf("error missing %q, got: %v", want, err)
				}
			}
			for _, call := range []struct{ method, path string }{
				{"POST", "/changes/123456/ready"},
				{"GET", "/changes/123456/flows"},
				{"POST", "/changes/123456/flows"},
				{"DELETE", ""},
			} {
				if n := server.CallCount(call.method, call.path); n != 0 {
					t.Errorf("expected no %s %s, got %d", call.method, call.path, n)
				}
			}
		})
	}
}

// Any other failure to check is reported as such, not guessed to mean that
// flows are disabled, and still stops before anything is modified.
func TestCreateScheduledSubmitFlow_FlowsEnabledCheckFails(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(123456,
		WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit"),
		func(m map[string]any) { m["work_in_progress"] = true },
	)
	server.OnFlowsEnabled(123456, http.StatusInternalServerError, false)
	server.OnJSON("POST", "/changes/123456/ready", http.StatusOK, map[string]any{})
	server.OnJSON("POST", "/changes/123456/flows", http.StatusCreated, map[string]any{})

	chCtx := &ChangeContext{
		Context:  context.Background(),
		Config:   &Config{Host: "https://pigweed-review.googlesource.com/a"},
		Client:   server.Client(),
		ChangeID: "123456",
	}

	_, err := chCtx.CreateScheduledSubmitFlow(123456, "", DefaultScheduleTimezone)
	if err == nil {
		t.Fatal("expected an error when the flows check fails, got nil")
	}
	var notEnabled *FlowsNotEnabledError
	if errors.As(err, &notEnabled) {
		t.Errorf("a failed check must not be reported as flows being disabled, got: %v", err)
	}
	if !strings.Contains(err.Error(), "checking whether Gerrit Flows are enabled") {
		t.Errorf("unexpected error: %v", err)
	}
	if n := server.CallCount("POST", "/changes/123456/ready"); n != 0 {
		t.Errorf("expected the change not to be marked ready, got %d POSTs", n)
	}
	if n := server.CallCount("POST", "/changes/123456/flows"); n != 0 {
		t.Errorf("expected no flows to be created, got %d POSTs", n)
	}
}

func TestFlowActionInfoParameterValues(t *testing.T) {
	tests := []struct {
		name string
		raw  string
		want []string
	}{
		{"empty", ``, nil},
		{"list", `["Commit-Queue+1"]`, []string{"Commit-Queue+1"}},
		{"object in key order", `{"b":"second","a":"first","c":"third"}`, []string{"first", "second", "third"}},
		{"neither", `42`, nil},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			a := FlowActionInfo{Parameters: json.RawMessage(tc.raw)}
			// Repeat so a map-order dependency would show up.
			for i := 0; i < 20; i++ {
				if got := a.parameterValues(); !slices.Equal(got, tc.want) {
					t.Fatalf("parameterValues() = %q, want %q", got, tc.want)
				}
			}
		})
	}
}
