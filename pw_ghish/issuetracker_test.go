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
	"io"
	"net/http"
	"net/url"
	"os"
	"runtime"
	"slices"
	"strings"
	"testing"
)

func TestFlexInt64_JSON(t *testing.T) {
	type sample struct {
		Single FlexInt64   `json:"single"`
		List   []FlexInt64 `json:"list"`
	}

	rawJSON := `{"single": "1194524", "list": ["54321", 99999, ""]}`
	var s sample
	if err := json.Unmarshal([]byte(rawJSON), &s); err != nil {
		t.Fatalf("unexpected unmarshal error: %v", err)
	}
	if s.Single != 1194524 {
		t.Errorf("Single = %d, want 1194524", s.Single)
	}
	if len(s.List) != 3 || s.List[0] != 54321 || s.List[1] != 99999 || s.List[2] != 0 {
		t.Errorf("List = %+v, want [54321, 99999, 0]", s.List)
	}

	outBytes, err := json.Marshal(s)
	if err != nil {
		t.Fatalf("unexpected marshal error: %v", err)
	}
	wantOut := `{"single":"1194524","list":["54321","99999","0"]}`
	if string(outBytes) != wantOut {
		t.Errorf("Marshal = %s, want %s", string(outBytes), wantOut)
	}

	// Verify invalid string fails loudly (no silent failure)
	var bad FlexInt64
	if err := json.Unmarshal([]byte(`"not-a-number"`), &bad); err == nil {
		t.Error("expected error unmarshaling invalid string into FlexInt64, got nil")
	}
}

func TestIssueTrackerClient_CreateAndGetIssue(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	client := srv.Client()
	ctx := context.Background()

	created, err := client.CreateIssue(ctx, &CreateIssueRequest{
		IssueState: BuganizerState{
			ComponentID: 1194524,
			Type:        "BUG",
			Status:      "ASSIGNED",
			Priority:    "P1",
			Title:       "pw_rpc: Deadlock on channel close",
			Reporter:    &BuganizerUser{EmailAddress: "author@google.com"},
			Assignee:    &BuganizerUser{EmailAddress: "owner@google.com"},
		},
		IssueComment: &BuganizerComment{
			Comment: "Steps to reproduce:\n1. Open channel\n2. Close concurrently",
		},
	})
	if err != nil {
		t.Fatalf("CreateIssue failed: %v", err)
	}
	if created.IssueID != 300001 {
		t.Errorf("IssueID = %d, want 300001", created.IssueID)
	}
	if created.State.Title != "pw_rpc: Deadlock on channel close" {
		t.Errorf("Title = %q, want %q", created.State.Title, "pw_rpc: Deadlock on channel close")
	}
	if created.Description == nil || !strings.Contains(created.Description.Comment, "Steps to reproduce") {
		t.Errorf("Description = %+v, expected comment text", created.Description)
	}

	fetched, err := client.GetIssue(ctx, int64(created.IssueID))
	if err != nil {
		t.Fatalf("GetIssue failed: %v", err)
	}
	if fetched.IssueID != created.IssueID {
		t.Errorf("GetIssue ID = %d, want %d", fetched.IssueID, created.IssueID)
	}
	if fetched.State.Assignee == nil || fetched.State.Assignee.EmailAddress != "owner@google.com" {
		t.Errorf("Assignee = %+v, want owner@google.com", fetched.State.Assignee)
	}
}

func TestIssueTrackerClient_ListIssues(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	client := srv.Client()
	ctx := context.Background()

	srv.SeedIssue(&BuganizerIssue{
		IssueID: 101,
		State: BuganizerState{
			ComponentID: 1194524,
			Status:      "NEW",
			Priority:    "P2",
			Title:       "Open unassigned issue",
		},
	}, "Description 101")

	srv.SeedIssue(&BuganizerIssue{
		IssueID: 102,
		State: BuganizerState{
			ComponentID: 1194524,
			Status:      "ASSIGNED",
			Priority:    "P1",
			Title:       "Open assigned issue",
			Assignee:    &BuganizerUser{EmailAddress: "alice@google.com"},
		},
	}, "Description 102")

	srv.SeedIssue(&BuganizerIssue{
		IssueID: 103,
		State: BuganizerState{
			ComponentID: 1194524,
			Status:      "FIXED",
			Priority:    "P2",
			Title:       "Closed fixed issue",
		},
	}, "Description 103")

	// List open issues
	openResp, err := client.ListIssues(ctx, "status:open", 30, "")
	if err != nil {
		t.Fatalf("ListIssues(status:open) failed: %v", err)
	}
	if len(openResp.Issues) != 2 {
		t.Fatalf("len(openResp.Issues) = %d, want 2", len(openResp.Issues))
	}
	if openResp.Issues[0].IssueID != 101 || openResp.Issues[1].IssueID != 102 {
		t.Errorf("unexpected issue IDs: %d, %d", openResp.Issues[0].IssueID, openResp.Issues[1].IssueID)
	}

	// List filtered by assignee
	aliceResp, err := client.ListIssues(ctx, "status:open assignee:alice@google.com", 30, "")
	if err != nil {
		t.Fatalf("ListIssues(assignee:alice) failed: %v", err)
	}
	if len(aliceResp.Issues) != 1 || aliceResp.Issues[0].IssueID != 102 {
		t.Errorf("aliceResp = %+v, want issue 102", aliceResp.Issues)
	}

	// List closed issues
	closedResp, err := client.ListIssues(ctx, "status:closed", 30, "")
	if err != nil {
		t.Fatalf("ListIssues(status:closed) failed: %v", err)
	}
	if len(closedResp.Issues) != 1 || closedResp.Issues[0].IssueID != 103 {
		t.Errorf("closedResp = %+v, want issue 103", closedResp.Issues)
	}
}

func TestIssueTrackerClient_ModifyIssue(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	client := srv.Client()
	ctx := context.Background()

	srv.SeedIssue(&BuganizerIssue{
		IssueID: 200,
		State: BuganizerState{
			ComponentID: 1194524,
			Status:      "NEW",
			Priority:    "P2",
			Title:       "Initial title",
		},
	}, "Initial bug report")

	// Modify title, status, assignee, priority + add a comment atomically
	modified, err := client.ModifyIssue(ctx, 200, &ModifyIssueRequest{
		AddMask: "title,status,priority,assignee",
		Add: &BuganizerState{
			Title:    "Updated title",
			Status:   "ASSIGNED",
			Priority: "P1",
			Assignee: &BuganizerUser{EmailAddress: "bob@google.com"},
		},
		IssueComment: &BuganizerComment{
			Comment: "Assigning to Bob and bumping to P1",
		},
	})
	if err != nil {
		t.Fatalf("ModifyIssue failed: %v", err)
	}
	if modified.State.Title != "Updated title" || modified.State.Status != "ASSIGNED" || modified.State.Priority != "P1" {
		t.Errorf("unexpected modified state: %+v", modified.State)
	}
	if modified.State.Assignee == nil || modified.State.Assignee.EmailAddress != "bob@google.com" {
		t.Errorf("Assignee = %+v, want bob@google.com", modified.State.Assignee)
	}

	// Verify comment was recorded
	comments, err := client.ListComments(ctx, 200, 50, "")
	if err != nil {
		t.Fatalf("ListComments failed: %v", err)
	}
	if len(comments.IssueComments) != 2 {
		t.Fatalf("len(comments) = %d, want 2", len(comments.IssueComments))
	}
	if comments.IssueComments[1].Comment != "Assigning to Bob and bumping to P1" {
		t.Errorf("comment[1] = %q", comments.IssueComments[1].Comment)
	}
}

func TestIssueTrackerClient_HotlistEntries(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	client := srv.Client()
	ctx := context.Background()

	srv.SeedIssue(&BuganizerIssue{
		IssueID: 200,
		State: BuganizerState{
			ComponentID: 1194524,
			Status:      "NEW",
			Priority:    "P2",
			Title:       "Hotlist test issue",
		},
	}, "Initial bug report")

	if err := client.CreateHotlistEntry(ctx, 9001, 200); err != nil {
		t.Fatalf("CreateHotlistEntry failed: %v", err)
	}
	if got := srv.Issues[200].State.HotlistIDs; !slices.Equal(got, []FlexInt64{9001}) {
		t.Errorf("HotlistIDs after PUT = %v, want [9001]", got)
	}
	if err := client.CreateHotlistEntry(ctx, 9001, 200); err != nil {
		t.Fatalf("duplicate CreateHotlistEntry failed: %v", err)
	}
	if got := srv.Issues[200].State.HotlistIDs; !slices.Equal(got, []FlexInt64{9001}) {
		t.Errorf("HotlistIDs after duplicate PUT = %v, want [9001]", got)
	}

	if err := client.DeleteHotlistEntry(ctx, 9001, 200); err != nil {
		t.Fatalf("DeleteHotlistEntry failed: %v", err)
	}
	if got := srv.Issues[200].State.HotlistIDs; len(got) != 0 {
		t.Errorf("HotlistIDs after DELETE = %v, want empty", got)
	}
	if !slices.Contains(srv.Calls, "PUT /v1/hotlists/9001/entries") || !slices.Contains(srv.Calls, "DELETE /v1/hotlists/9001/entries/200") {
		t.Errorf("hotlist calls = %v, want PUT and DELETE on /v1/hotlists/9001/entries", srv.Calls)
	}

	if err := client.CreateHotlistEntry(ctx, 0, 200); err == nil {
		t.Error("expected error for invalid hotlist ID 0, got nil")
	}
	if err := client.CreateHotlistEntry(ctx, 9001, 999); err == nil {
		t.Error("expected error for nonexistent issue 999 on CreateHotlistEntry, got nil")
	}
	if err := client.DeleteHotlistEntry(ctx, 9001, 999); err == nil {
		t.Error("expected error for nonexistent issue 999 on DeleteHotlistEntry, got nil")
	}
}

func TestIssueTrackerClient_Comments(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	client := srv.Client()
	ctx := context.Background()

	srv.SeedIssue(&BuganizerIssue{
		IssueID: 300,
		State:   BuganizerState{Title: "Comment test issue", Status: "NEW"},
	}, "First comment (description)")

	added, err := client.CreateComment(ctx, 300, "Second comment from CLI")
	if err != nil {
		t.Fatalf("CreateComment failed: %v", err)
	}
	if added.CommentNumber != 2 || added.Comment != "Second comment from CLI" {
		t.Errorf("CreateComment returned %+v", added)
	}

	list, err := client.ListComments(ctx, 300, 50, "")
	if err != nil {
		t.Fatalf("ListComments failed: %v", err)
	}
	if len(list.IssueComments) != 2 {
		t.Fatalf("len(list) = %d, want 2", len(list.IssueComments))
	}
}

func TestIssueTrackerClient_ActionableErrors(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	client := srv.Client()
	ctx := context.Background()

	// 1. 404 Not Found -> 4-pillar actionable error
	_, err := client.GetIssue(ctx, 999999)
	if err == nil {
		t.Fatal("expected error for non-existent issue, got nil")
	}
	if !strings.Contains(err.Error(), "HTTP 404") || !strings.Contains(err.Error(), "gh issue list") {
		t.Errorf("404 error missing actionable remediation, got:\n%v", err)
	}

	// 2. 401 Unauthorized -> 4-pillar actionable error with auth commands
	srv.ForceStatus = http.StatusUnauthorized
	_, err = client.GetIssue(ctx, 300001)
	if err == nil {
		t.Fatal("expected error for HTTP 401, got nil")
	}
	if !strings.Contains(err.Error(), "HTTP 401") || !strings.Contains(err.Error(), "luci-auth login -scopes") || strings.Contains(err.Error(), "gcloud") {
		t.Errorf("401 error missing actionable auth remediation, got:\n%v", err)
	}
	srv.ForceStatus = 0

	// 3. TokenProvider failure -> propagates actionable error immediately
	client.TokenProvider = func(context.Context) (string, error) {
		return "", errors.New("no OAuth token found")
	}
	_, err = client.GetIssue(ctx, 300001)
	if err == nil {
		t.Fatal("expected error when TokenProvider fails, got nil")
	}
	if !strings.Contains(err.Error(), "no OAuth token found") {
		t.Errorf("unexpected error when TokenProvider fails: %v", err)
	}
}

func TestIssueTrackerClient_RefusesTokenOverPlainHTTP(t *testing.T) {
	sent := 0
	transport := testRoundTripper(func(*http.Request) (*http.Response, error) {
		sent++
		return nil, errors.New("unexpected request")
	})
	client := NewIssueTrackerClient("http://issuetracker.example.com/v1", &http.Client{Transport: transport})
	client.TokenProvider = func(context.Context) (string, error) {
		return "secret-token", nil
	}
	_, err := client.GetIssue(t.Context(), 1)
	if err == nil || !strings.Contains(err.Error(), "refusing to send Issue Tracker credentials") {
		t.Fatalf("GetIssue over plain HTTP error = %v, want refusal", err)
	}
	if sent != 0 {
		t.Errorf("sent %d requests over plain HTTP, want 0", sent)
	}
}

func TestIssueTrackerClient_ListAllComments_Pagination(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	srv.CommentPageSize = 2
	client := srv.Client()
	ctx := context.Background()

	srv.SeedIssue(&BuganizerIssue{
		IssueID: 401,
		State:   BuganizerState{Title: "Paginated comments", Status: "NEW"},
	}, "Comment 1")
	_, _ = client.CreateComment(ctx, 401, "Comment 2")
	_, _ = client.CreateComment(ctx, 401, "Comment 3")
	_, _ = client.CreateComment(ctx, 401, "Comment 4")
	_, _ = client.CreateComment(ctx, 401, "Comment 5")

	all, err := client.ListAllComments(ctx, 401)
	if err != nil {
		t.Fatalf("ListAllComments failed: %v", err)
	}
	if len(all) != 5 {
		t.Fatalf("len(all) = %d, want 5", len(all))
	}
	if all[4].Comment != "Comment 5" {
		t.Errorf("all[4].Comment = %q, want Comment 5", all[4].Comment)
	}
	for i := 1; i < len(all); i++ {
		if all[i].FormattingMode != DefaultCommentFormattingMode {
			t.Errorf("all[%d].FormattingMode = %q, want %q", i, all[i].FormattingMode, DefaultCommentFormattingMode)
		}
	}
}

func TestIssueTrackerClient_CloseAndReopenIssue(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	client := srv.Client()
	ctx := context.Background()

	srv.SeedIssue(&BuganizerIssue{
		IssueID: 501,
		State: BuganizerState{
			Title:    "Issue to close",
			Status:   "ASSIGNED",
			Assignee: &BuganizerUser{EmailAddress: "owner@google.com"},
		},
	}, "Description")

	// 1. Close as FIXED
	closed, err := client.CloseIssue(ctx, 501, "FIXED", 0, "Fixed in CL 123")
	if err != nil {
		t.Fatalf("CloseIssue failed: %v", err)
	}
	if closed.State.Status != "FIXED" {
		t.Errorf("Status = %q, want FIXED", closed.State.Status)
	}

	// 2. Reopen -> should restore ASSIGNED because Assignee is present
	reopened, err := client.ReopenIssue(ctx, 501, "Regression found")
	if err != nil {
		t.Fatalf("ReopenIssue failed: %v", err)
	}
	if reopened.State.Status != "ASSIGNED" {
		t.Errorf("Status after reopen = %q, want ASSIGNED", reopened.State.Status)
	}

	// Verify close and reopen comments use MARKDOWN formatting mode
	comments, err := client.ListAllComments(ctx, 501)
	if err != nil {
		t.Fatalf("ListAllComments failed: %v", err)
	}
	if len(comments) < 3 {
		t.Fatalf("expected at least 3 comments on 501, got %d", len(comments))
	}
	for i := 1; i < len(comments); i++ {
		if comments[i].FormattingMode != DefaultCommentFormattingMode {
			t.Errorf("comments[%d].FormattingMode = %q, want %q", i, comments[i].FormattingMode, DefaultCommentFormattingMode)
		}
	}

	// 3. Reopening an already open issue must fail fast
	if _, err := client.ReopenIssue(ctx, 501, "Again"); err == nil {
		t.Fatal("expected error when reopening an already open issue, got nil")
	}

	// 4. Close as DUPLICATE without canonical ID must fail fast
	if _, err := client.CloseIssue(ctx, 501, "DUPLICATE", 0, ""); err == nil {
		t.Fatal("expected error when closing as DUPLICATE without canonical ID, got nil")
	}

	// 5. Close as DUPLICATE with canonical ID
	dup, err := client.CloseIssue(ctx, 501, "DUPLICATE", 999999, "Dup of 999999")
	if err != nil {
		t.Fatalf("CloseIssue DUPLICATE failed: %v", err)
	}
	if dup.State.Status != "DUPLICATE" || dup.State.CanonicalIssueID != 999999 {
		t.Errorf("unexpected duplicate state: status=%q, canonical=%d", dup.State.Status, dup.State.CanonicalIssueID)
	}
}

func TestIssueModifier_StateTransitionsAndMasks(t *testing.T) {
	// 1. Assigning a NEW issue automatically transitions status to ASSIGNED
	newIssue := &BuganizerIssue{
		IssueID: 601,
		State:   BuganizerState{Status: "NEW", Title: "Initial"},
	}
	mod := NewIssueModifier(newIssue)
	if mod.HasChanges() {
		t.Fatal("expected HasChanges() == false initially")
	}
	if err := mod.SetTitle("   "); err == nil {
		t.Fatal("expected error for empty title, got nil")
	}
	if err := mod.SetTitle("Updated Title"); err != nil {
		t.Fatalf("SetTitle failed: %v", err)
	}
	mod.SetAssignee("alice@google.com")
	if err := mod.AddLabel("P1"); err != nil {
		t.Fatalf("AddLabel failed: %v", err)
	}
	req := mod.BuildRequest()
	if req.Add.Status != "ASSIGNED" {
		t.Errorf("expected NEW issue to transition to ASSIGNED on SetAssignee, got status=%q", req.Add.Status)
	}
	if !strings.Contains(req.AddMask, "status") || !strings.Contains(req.AddMask, "assignee") || !strings.Contains(req.AddMask, "priority") {
		t.Errorf("AddMask = %q, expected status,assignee,priority", req.AddMask)
	}

	// 2. Removing assignee from an ASSIGNED issue automatically transitions status to NEW
	assignedIssue := &BuganizerIssue{
		IssueID: 602,
		State: BuganizerState{
			Status:   "ASSIGNED",
			Assignee: &BuganizerUser{EmailAddress: "alice@google.com"},
		},
	}
	mod2 := NewIssueModifier(assignedIssue)
	mod2.RemoveAssignee()
	req2 := mod2.BuildRequest()
	if req2.Add.Status != "NEW" {
		t.Errorf("expected ASSIGNED issue to transition to NEW on RemoveAssignee, got status=%q", req2.Add.Status)
	}
	if !strings.Contains(req2.RemoveMask, "assignee") {
		t.Errorf("RemoveMask = %q, expected assignee", req2.RemoveMask)
	}

	// 3. Hotlist labels are tracked separately from ModifyIssueRequest masks
	mod3 := NewIssueModifier(newIssue)
	if err := mod3.AddLabel("hotlist:9001"); err != nil {
		t.Fatalf("AddLabel(hotlist:9001) failed: %v", err)
	}
	if err := mod3.RemoveLabel("hotlist:9002"); err != nil {
		t.Fatalf("RemoveLabel(hotlist:9002) failed: %v", err)
	}
	if !mod3.HasChanges() {
		t.Error("expected HasChanges() == true when hotlist labels are queued")
	}
	if mod3.HasMetadataChanges() {
		t.Error("expected HasMetadataChanges() == false when only hotlist labels are queued")
	}
	req3 := mod3.BuildRequest()
	if strings.Contains(req3.AddMask, "hotlistIds") || strings.Contains(req3.RemoveMask, "hotlistIds") {
		t.Errorf("BuildRequest() must not include hotlistIds in masks; got AddMask=%q RemoveMask=%q", req3.AddMask, req3.RemoveMask)
	}
	if len(mod3.AddedHotlistIDs()) != 1 || mod3.AddedHotlistIDs()[0] != 9001 {
		t.Errorf("AddedHotlistIDs() = %v, want [9001]", mod3.AddedHotlistIDs())
	}
	if len(mod3.RemovedHotlistIDs()) != 1 || mod3.RemovedHotlistIDs()[0] != 9002 {
		t.Errorf("RemovedHotlistIDs() = %v, want [9002]", mod3.RemovedHotlistIDs())
	}
}

func TestLabelToQueryToken(t *testing.T) {
	tests := []struct {
		label string
		want  string
	}{
		{"P0", "priority:P0"},
		{"priority:p2", "priority:P2"},
		{"priority: P1", "priority:P1"},
		{"S1", "severity:S1"},
		{"severity: S2", "severity:S2"},
		{"bug", "type:BUG"},
		{"type: vulnerability", "type:VULNERABILITY"},
		{"feature", "type:FEATURE_REQUEST"},
		{"component:1194524", "componentid:1194524"},
		{"hotlist:5555", "hotlistid:5555"},
	}
	for _, tc := range tests {
		got, err := LabelToQueryToken(tc.label)
		if err != nil {
			t.Errorf("LabelToQueryToken(%q) unexpected error: %v", tc.label, err)
			continue
		}
		if got != tc.want {
			t.Errorf("LabelToQueryToken(%q) = %q, want %q", tc.label, got, tc.want)
		}
	}
	if _, err := LabelToQueryToken("bogus-label"); err == nil {
		t.Error("expected error for bogus-label, got nil")
	}
}

func TestSanitizeUntrustedText(t *testing.T) {
	tests := []struct {
		name  string
		input string
		want  string
	}{
		{
			name:  "strips ANSI colors and styling",
			input: "\x1b[31mRed alert\x1b[0m and \x1b[1;32mBold Green\x1b[m",
			want:  "Red alert and Bold Green",
		},
		{
			name:  "strips cursor movement and screen clear escape sequences",
			input: "Hello\x1b[2J\x1b[HWorld\x1b[10A!",
			want:  "HelloWorld!",
		},
		{
			name:  "strips non-printable control chars but preserves newline, tab, carriage return",
			input: "Line1\x00\x07\x08\n\tIndented\r\nLine2\x1f",
			want:  "Line1\n\tIndented\r\nLine2",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got := SanitizeUntrustedText(tc.input)
			if got != tc.want {
				t.Errorf("SanitizeUntrustedText(%q) = %q, want %q", tc.input, got, tc.want)
			}
		})
	}
}

func TestSlugifyBranchName(t *testing.T) {
	tests := []struct {
		name    string
		issueID int64
		title   string
		want    string
	}{
		{
			name:    "normal title",
			issueID: 12345,
			title:   "pw_rpc: Fix channel deadlock!",
			want:    "b-12345-pw-rpc-fix-channel-deadlock",
		},
		{
			name:    "symbols only results in empty slug fallback",
			issueID: 999,
			title:   "!!! @@@ ### $$$ %%% ^^^ &&& ***",
			want:    "b-999",
		},
		{
			name:    "exceeds 45 character limit and trims trailing hyphens",
			issueID: 42,
			title:   "pw_bluetooth_sapphire: Implement comprehensive1 LE GATT client caching and service discovery state machine",
			want:    "b-42-pw-bluetooth-sapphire-implement-comprehensive",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got := SlugifyBranchName(tc.issueID, tc.title)
			if got != tc.want {
				t.Errorf("SlugifyBranchName(%d, %q) = %q, want %q", tc.issueID, tc.title, got, tc.want)
			}
			parts := strings.SplitN(got, "-", 3)
			if len(parts) == 3 && len(parts[2]) > 45 {
				t.Errorf("slug portion %q length %d exceeds 45 chars", parts[2], len(parts[2]))
			}
		})
	}
}

func TestParseGossoResponse(t *testing.T) {
	req, err := http.NewRequest(http.MethodGet, "https://issuetracker.corp.googleapis.com/v1/issues/123", nil)
	if err != nil {
		t.Fatalf("http.NewRequest: %v", err)
	}

	t.Run("standard HTTP/1.1 response with banner prefix", func(t *testing.T) {
		raw := "gosso: connecting to uberproxy...\r\n" +
			"HTTP/1.1 200 OK\r\n" +
			"Content-Type: application/json\r\n" +
			"\r\n" +
			`{"issueId":"123"}`
		resp, err := parseGossoResponse([]byte(raw), req)
		if err != nil {
			t.Fatalf("parseGossoResponse unexpected error: %v", err)
		}
		defer resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			t.Errorf("StatusCode = %d, want 200", resp.StatusCode)
		}
		body, _ := io.ReadAll(resp.Body)
		if string(body) != `{"issueId":"123"}` {
			t.Errorf("body = %q, want %q", string(body), `{"issueId":"123"}`)
		}
	})

	t.Run("HTTP/2 normalized to HTTP/1.1", func(t *testing.T) {
		raw := "HTTP/2 404 Not Found\r\n" +
			"Content-Type: application/json\r\n" +
			"\r\n" +
			`{"error":{"code":404}}`
		resp, err := parseGossoResponse([]byte(raw), req)
		if err != nil {
			t.Fatalf("parseGossoResponse unexpected error: %v", err)
		}
		defer resp.Body.Close()
		if resp.StatusCode != http.StatusNotFound {
			t.Errorf("StatusCode = %d, want 404", resp.StatusCode)
		}
	})

	t.Run("non-HTTP output names gosso", func(t *testing.T) {
		_, err := parseGossoResponse([]byte("gosso: handshake error"), req)
		if err == nil || !strings.Contains(err.Error(), "gosso returned non-HTTP output") {
			t.Errorf("parseGossoResponse error = %v, want 'gosso returned non-HTTP output'", err)
		}
	})

	t.Run("RoundTrip sanitizes CRLF headers", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{})

		var gotArgs []string
		rt := &gossoRoundTripper{
			exec: func(_ context.Context, args ...string) ([]byte, error) {
				gotArgs = args
				return []byte("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{}"), nil
			},
		}

		rtReq := req.Clone(t.Context())
		rtReq.Header.Set("Authorization", "Bearer gosso-token")
		rtReq.Header.Set("X-Goog-User-Project", "proj\r\nX-CRLF: bad")

		resp, err := rt.RoundTrip(rtReq)
		if err != nil {
			t.Fatalf("RoundTrip: %v", err)
		}
		resp.Body.Close()

		if got := slices.Contains(gotArgs, "-loas"); got != (runtime.GOOS == "linux") {
			t.Errorf("slices.Contains(gotArgs, \"-loas\") = %v, want %v (args=%q)", got, runtime.GOOS == "linux", gotArgs)
		}
		if !slices.Contains(gotArgs, "X-Goog-User-Project: projX-CRLF: bad") {
			t.Errorf("expected CRLF-sanitized header in gosso args: %q", gotArgs)
		}
	})

	t.Run("RoundTrip fails on mid-stream gosso error after 200 OK headers", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{})
		rt := &gossoRoundTripper{
			exec: func(_ context.Context, _ ...string) ([]byte, error) {
				return []byte("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"issueId\":"), errors.New("exit status 1")
			},
		}
		if _, err := rt.RoundTrip(req.Clone(t.Context())); err == nil || !strings.Contains(err.Error(), "gosso execution failed") {
			t.Errorf("RoundTrip mid-stream error = %v, want 'gosso execution failed'", err)
		}
	})

	t.Run("RoundTrip invokes gosso with expected flags and parses non-2xx on exit status 1", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{
			LookPath: func(file string) (string, error) {
				if file == "gosso" || file == "/usr/local/bin/gosso" {
					return "/usr/local/bin/gosso", nil
				}
				return "", errors.New("not found")
			},
		})
		var gotArgs []string
		var dataPath, dataBody string
		rt := &gossoRoundTripper{
			exec: func(_ context.Context, args ...string) ([]byte, error) {
				gotArgs = args
				for i, a := range args {
					if a == "-data_file" && i+1 < len(args) {
						dataPath = args[i+1]
						b, err := os.ReadFile(dataPath)
						if err != nil {
							return nil, err
						}
						dataBody = string(b)
					}
				}
				raw := "HTTP/1.1 403 Forbidden\r\n" +
					"Content-Type: application/json\r\n" +
					"\r\n" +
					`{"error":{"code":403,"message":"SERVICE_DISABLED"}}`
				return []byte(raw), errors.New("exit status 1")
			},
		}

		postReq, err := http.NewRequestWithContext(
			t.Context(),
			http.MethodPost,
			"https://issuetracker.corp.googleapis.com/v1/issues",
			strings.NewReader(`{"issueState":{"title":"test"}}`),
		)
		if err != nil {
			t.Fatalf("http.NewRequestWithContext: %v", err)
		}
		postReq.Header.Set("Authorization", "Bearer gosso-token")
		postReq.Header.Set("Accept", "application/json")

		resp, err := rt.RoundTrip(postReq)
		if err != nil {
			t.Fatalf("RoundTrip with gosso exit status 1 unexpected error: %v", err)
		}
		defer resp.Body.Close()
		if resp.StatusCode != http.StatusForbidden {
			t.Errorf("StatusCode = %d, want 403", resp.StatusCode)
		}
		bodyBytes, _ := io.ReadAll(resp.Body)
		if !strings.Contains(string(bodyBytes), "SERVICE_DISABLED") {
			t.Errorf("body = %q, want SERVICE_DISABLED payload", string(bodyBytes))
		}
		if dataBody != `{"issueState":{"title":"test"}}` {
			t.Errorf("data_file content = %q, want request body", dataBody)
		}
		if _, err := os.Stat(dataPath); !errors.Is(err, os.ErrNotExist) {
			t.Errorf("expected temp data_file %q to be removed after RoundTrip, stat err = %v", dataPath, err)
		}

		var wantArgs []string
		if runtime.GOOS == "linux" {
			wantArgs = append(wantArgs, "-loas")
		}
		wantArgs = append(wantArgs,
			"-dump_headers", "-method", "POST",
			"-header", "Accept: application/json",
			"-header", "Authorization: Bearer gosso-token",
			"-data_file", dataPath,
			"-url", "https://issuetracker.corp.googleapis.com/v1/issues",
		)
		if !slices.Equal(gotArgs, wantArgs) {
			t.Errorf("gosso args = %q, want %q", gotArgs, wantArgs)
		}
	})

	t.Run("RoundTrip fails fast with actionable error when gosso is not installed", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{})

		rt := &gossoRoundTripper{}
		_, err := rt.RoundTrip(req.Clone(t.Context()))
		if err == nil {
			t.Fatal("expected error when gosso is not installed, got nil")
		}
		if ExitCodeFor(err) != ExitCodeAuth {
			t.Errorf("ExitCodeFor(err) = %d, want %d (%v)", ExitCodeFor(err), ExitCodeAuth, err)
		}
		if !strings.Contains(err.Error(), "sudo apt install gosso") || !strings.Contains(err.Error(), "sudo mule install gosso") || !strings.Contains(err.Error(), "gcert") {
			t.Errorf("expected actionable gosso install error message, got:\n%v", err)
		}
	})

	t.Run("default corp endpoint uses gossoRoundTripper", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{
			LookPath: func(file string) (string, error) {
				if file == "/google/bin/releases/gosso/gosso" {
					return file, nil
				}
				return "", errors.New("not found")
			},
		})

		c := NewIssueTrackerClient("", nil)
		if c.Endpoint != DefaultIssueTrackerEndpoint {
			t.Errorf("c.Endpoint = %q, want %q", c.Endpoint, DefaultIssueTrackerEndpoint)
		}
		if c.HTTPClient.CheckRedirect == nil {
			t.Errorf("c.HTTPClient.CheckRedirect = nil, want stripAuthOnCrossHostRedirect")
		}
		rt, ok := c.HTTPClient.Transport.(*gossoRoundTripper)
		if !ok {
			t.Fatalf("Transport = %#v, want *gossoRoundTripper", c.HTTPClient.Transport)
		}
		var gotArgs []string
		rt.exec = func(_ context.Context, args ...string) ([]byte, error) {
			gotArgs = args
			return []byte("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}"), nil
		}
		if _, err := rt.RoundTrip(req.Clone(t.Context())); err != nil {
			t.Fatalf("RoundTrip: %v", err)
		}
		if !slices.Contains(gotArgs, "-dump_headers") {
			t.Errorf("gosso args = %q, want gosso -dump_headers flag", gotArgs)
		}
	})

	t.Run("RoundTrip delegates non-corp URLs to fallback transport", func(t *testing.T) {
		var fallbackCalled bool
		rt := &gossoRoundTripper{
			fallback: testRoundTripper(func(r *http.Request) (*http.Response, error) {
				fallbackCalled = true
				return &http.Response{
					StatusCode: http.StatusOK,
					Body:       io.NopCloser(strings.NewReader("{}")),
					Header:     make(http.Header),
				}, nil
			}),
			exec: func(_ context.Context, _ ...string) ([]byte, error) {
				t.Fatal("unexpected gosso exec for non-corp URL")
				return nil, nil
			},
		}
		nonCorpReq, err := http.NewRequestWithContext(t.Context(), http.MethodGet, "https://issuetracker.googleapis.com/v1/issues/123", nil)
		if err != nil {
			t.Fatalf("http.NewRequestWithContext: %v", err)
		}
		resp, err := rt.RoundTrip(nonCorpReq)
		if err != nil {
			t.Fatalf("RoundTrip non-corp URL error: %v", err)
		}
		resp.Body.Close()
		if !fallbackCalled {
			t.Error("expected non-corp URL to delegate to fallback transport")
		}
	})

	t.Run("doJSON unwraps ExitCodeError from RoundTripper without prefixing HTTP request failed", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{})
		c := NewIssueTrackerClient("", nil)
		c.TokenProvider = func(context.Context) (string, error) { return "tok", nil }
		c.QuotaProjectProvider = func(context.Context) string { return "" }

		_, err := c.GetIssue(t.Context(), 123)
		if err == nil {
			t.Fatal("expected error when gosso is not installed, got nil")
		}
		if ExitCodeFor(err) != ExitCodeAuth {
			t.Errorf("ExitCodeFor(err) = %d, want %d", ExitCodeFor(err), ExitCodeAuth)
		}
		if strings.HasPrefix(err.Error(), "HTTP request failed for") {
			t.Errorf("expected ExitCodeError to be unwrapped directly, got:\n%v", err)
		}
	})

	t.Run("isCorpIssueTrackerURL rejects spoofed and plaintext URLs", func(t *testing.T) {
		for _, tc := range []struct {
			rawURL string
			want   bool
		}{
			{"https://issuetracker.corp.googleapis.com/v1", true},
			{"HTTPS://IssueTracker.CORP.GoogleAPIs.com:443/v1", true},
			{"https://autopush-issuetracker.corp.googleapis.com/v1", true},
			{"http://issuetracker.corp.googleapis.com/v1", false},
			{"https://issuetracker.corp.googleapis.com.evil.com/v1", false},
			{"https://evil.com/v1?host=issuetracker.corp.googleapis.com", false},
			{"https://user.corp.googleapis.com@evil.com/v1", false},
			{"https://.corp.googleapis.com/v1", false},
			{"https://corp.googleapis.com/v1", false},
		} {
			u, err := url.Parse(tc.rawURL)
			if err != nil {
				t.Fatalf("url.Parse(%q): %v", tc.rawURL, err)
			}
			if got := isCorpIssueTrackerURL(u); got != tc.want {
				t.Errorf("isCorpIssueTrackerURL(%q) = %v, want %v", tc.rawURL, got, tc.want)
			}
			c := NewIssueTrackerClient(tc.rawURL, nil)
			_, gotGosso := c.HTTPClient.Transport.(*gossoRoundTripper)
			if gotGosso != tc.want {
				t.Errorf("NewIssueTrackerClient(%q) uses gossoRoundTripper = %v, want %v", tc.rawURL, gotGosso, tc.want)
			}
		}
	})
}

func TestRealBuganizerJSONCompatibility(t *testing.T) {
	rawIssue := `{
		"issueId": "562965055",
		"issueState": {"title": "Live Test Bug", "status": "NEW"},
		"description": {
			"commentNumber": 1,
			"comment": "Original issue description",
			"originalAuthor": {"emailAddress": "developer@google.com"}
		}
	}`
	var issue BuganizerIssue
	if err := json.Unmarshal([]byte(rawIssue), &issue); err != nil {
		t.Fatalf("json.Unmarshal issue failed: %v", err)
	}
	if desc := issue.EffectiveDescription(); desc == nil || desc.Comment != "Original issue description" {
		t.Errorf("EffectiveDescription() = %+v, want 'Original issue description'", desc)
	}
	if email := issue.EffectiveDescription().EffectiveAuthorEmail(); email != "developer@google.com" {
		t.Errorf("EffectiveAuthorEmail() = %q, want 'developer@google.com'", email)
	}
}

type testRoundTripper func(*http.Request) (*http.Response, error)

func (f testRoundTripper) RoundTrip(r *http.Request) (*http.Response, error) {
	return f(r)
}

func TestIssueTrackerClient_QuotaProjectErrorRemediation(t *testing.T) {
	client := &IssueTrackerClient{
		Endpoint: "https://example.invalid/v1",
		HTTPClient: &http.Client{
			Transport: testRoundTripper(func(r *http.Request) (*http.Response, error) {
				body := `{"error":{"code":403,"message":"Caller does not have required permission","details":[{"reason":"USER_PROJECT_DENIED"}]}}`
				return &http.Response{
					StatusCode: http.StatusForbidden,
					Body:       io.NopCloser(strings.NewReader(body)),
					Header:     make(http.Header),
				}, nil
			}),
		},
		TokenProvider: func(ctx context.Context) (string, error) {
			return "test-token", nil
		},
		QuotaProjectProvider: func(ctx context.Context) string {
			return "broken-project"
		},
	}

	_, err := client.GetIssue(context.Background(), 12345)
	if err == nil {
		t.Fatal("expected error from HTTP 403 USER_PROJECT_DENIED, got nil")
	}
	for _, want := range []string{"quota project error", "broken-project", "ghish.issue.quotaProject"} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("expected %q in quota project remediation, got:\n%v", want, err)
		}
	}

	t.Run("no configured quota project: SERVICE_DISABLED is not retried and points at luci-auth", func(t *testing.T) {
		var requests int
		var seenProjects []string
		client := &IssueTrackerClient{
			Endpoint: "https://issuetracker.corp.googleapis.com/v1",
			HTTPClient: &http.Client{
				Transport: testRoundTripper(func(r *http.Request) (*http.Response, error) {
					requests++
					seenProjects = append(seenProjects, r.Header.Get("X-Goog-User-Project"))
					body := `{"error":{"code":403,"message":"Google Issue Tracker API has not been used in project 446450136466 before or it is disabled.","details":[{"reason":"SERVICE_DISABLED"}]}}`
					return &http.Response{
						StatusCode: http.StatusForbidden,
						Body:       io.NopCloser(strings.NewReader(body)),
						Header:     make(http.Header),
					}, nil
				}),
			},
			TokenProvider: func(context.Context) (string, error) {
				return "test-token", nil
			},
			QuotaProjectProvider: func(context.Context) string { return "" },
		}

		_, err := client.GetIssue(t.Context(), 12345)
		if err == nil {
			t.Fatal("expected error from HTTP 403 SERVICE_DISABLED, got nil")
		}
		if requests != 1 {
			t.Errorf("requests = %d, want 1 (no retry)", requests)
		}
		if len(seenProjects) != 1 || seenProjects[0] != "" {
			t.Errorf("X-Goog-User-Project = %q, want a single empty value", seenProjects)
		}
		for _, want := range []string{"quota project error", "luci-auth"} {
			if !strings.Contains(err.Error(), want) {
				t.Errorf("expected %q in error, got:\n%v", want, err)
			}
		}
		for _, unwanted := range []string{"pigweed-buganizer", "pigweed-gce", "gcloud"} {
			if strings.Contains(err.Error(), unwanted) {
				t.Errorf("unexpected %q in error, got:\n%v", unwanted, err)
			}
		}
	})
}

func TestConfiguredIssueTrackerQuotaProject_ProjectConfigAndGitConfig(t *testing.T) {
	t.Setenv("GHISH_QUOTA_PROJECT", "")
	t.Setenv("GOOGLE_CLOUD_QUOTA_PROJECT", "")

	if got := ConfiguredIssueTrackerQuotaProject(nil); got != "" {
		t.Errorf("ConfiguredIssueTrackerQuotaProject(nil) = %q, want empty", got)
	}
	if got := ConfiguredIssueTrackerQuotaProject(context.Background()); got != "" {
		t.Errorf("ConfiguredIssueTrackerQuotaProject(context.Background()) = %q, want empty", got)
	}

	t.Run("uses ProjectConfig Issue.QuotaProject and yields to env vars", func(t *testing.T) {
		pcfg := DefaultProjectConfig()
		pcfg.Issue.QuotaProject = "acme-quota-project"
		cfg := &Config{
			Git:           &MockGitRunner{},
			ProjectConfig: pcfg,
		}
		ctx := context.WithValue(context.Background(), configKey, cfg)
		if got := ConfiguredIssueTrackerQuotaProject(ctx); got != "acme-quota-project" {
			t.Errorf("ConfiguredIssueTrackerQuotaProject() = %q, want %q", got, "acme-quota-project")
		}

		t.Setenv("GOOGLE_CLOUD_QUOTA_PROJECT", "gcp-env-project")
		if got := ConfiguredIssueTrackerQuotaProject(ctx); got != "gcp-env-project" {
			t.Errorf("ConfiguredIssueTrackerQuotaProject() with GOOGLE_CLOUD_QUOTA_PROJECT = %q, want %q", got, "gcp-env-project")
		}

		t.Setenv("GHISH_QUOTA_PROJECT", "env-override-project")
		if got := ConfiguredIssueTrackerQuotaProject(ctx); got != "env-override-project" {
			t.Errorf("ConfiguredIssueTrackerQuotaProject() with env = %q, want %q", got, "env-override-project")
		}
	})

	t.Run("uses git config ghish.issue.quotaproject and ignores legacy ghish.quotaproject", func(t *testing.T) {
		mockGit := &MockGitRunner{}
		mockGit.OnCommand("config --get-regexp ^ghish\\.", "ghish.issue.quotaproject git-issue-quota-proj\nghish.quotaproject legacy-quota-proj\n")
		cfg := &Config{Git: mockGit}
		ctx := context.WithValue(context.Background(), configKey, cfg)
		if got := ConfiguredIssueTrackerQuotaProject(ctx); got != "git-issue-quota-proj" {
			t.Errorf("ConfiguredIssueTrackerQuotaProject() = %q, want %q", got, "git-issue-quota-proj")
		}

		mockGitLegacy := &MockGitRunner{}
		mockGitLegacy.OnCommand("config --get-regexp ^ghish\\.", "ghish.quotaproject legacy-only-proj\n")
		ctxLegacy := context.WithValue(t.Context(), configKey, &Config{Git: mockGitLegacy})
		if got := ConfiguredIssueTrackerQuotaProject(ctxLegacy); got != "" {
			t.Errorf("ConfiguredIssueTrackerQuotaProject() legacy-only = %q, want empty", got)
		}
	})

	t.Run("memoizes loaded ProjectConfig and skips duplicate git config calls", func(t *testing.T) {
		var gitCalls int
		mockGit := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				gitCalls++
				return errors.New("not configured")
			},
		}
		cfg := &Config{
			Git:           mockGit,
			ProjectConfig: DefaultProjectConfig(),
		}
		ctx := context.WithValue(context.Background(), configKey, cfg)
		for i := range 3 {
			if got := ConfiguredIssueTrackerQuotaProject(ctx); got != "" {
				t.Errorf("call %d: ConfiguredIssueTrackerQuotaProject() = %q, want empty", i, got)
			}
		}
		if gitCalls != 0 {
			t.Errorf("gitCalls = %d, want 0 when ProjectConfig is already loaded", gitCalls)
		}
	})
}

func TestNewIssueTrackerClientForCommand_CommunityHonorsTokenEnv(t *testing.T) {
	prevMode, prevProfile := AuthModeFlag, ProfileFlag
	AuthModeFlag, ProfileFlag = string(AuthModeCommunity), "pigweed"
	t.Cleanup(func() { AuthModeFlag, ProfileFlag = prevMode, prevProfile })

	for _, envVar := range []string{"GHISH_ISSUE_TOKEN", "BUGANIZER_TOKEN"} {
		t.Run(envVar, func(t *testing.T) {
			WithHermeticAuth(t, AuthResolvers{IssueTrackerToken: ResolveIssueTrackerTokenWithSource})
			t.Setenv(envVar, "env-token")
			client, err := NewIssueTrackerClientForCommand(t.Context(), nil)
			if err != nil {
				t.Fatalf("NewIssueTrackerClientForCommand: %v", err)
			}
			if tok, err := client.TokenProvider(t.Context()); err != nil || tok != "env-token" {
				t.Errorf("TokenProvider() = (%q, %v), want (\"env-token\", nil)", tok, err)
			}
		})
	}

	t.Run("no token env", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{IssueTrackerToken: ResolveIssueTrackerTokenWithSource})
		client, err := NewIssueTrackerClientForCommand(t.Context(), nil)
		if err != nil {
			t.Fatalf("NewIssueTrackerClientForCommand: %v", err)
		}
		_, err = client.TokenProvider(t.Context())
		if ExitCodeFor(err) != ExitCodeAuth || !strings.Contains(err.Error(), "https://issues.pigweed.dev") {
			t.Errorf("TokenProvider() error = %v, want ExitCodeAuth pointing at the web tracker", err)
		}
		for _, tool := range []string{"sso_client", "gosso", "mule", "gcert", "luci-auth", "gcloud"} {
			if strings.Contains(err.Error(), tool) {
				t.Errorf("community error leaked internal tool %q:\n%v", tool, err)
			}
		}
	})
}
