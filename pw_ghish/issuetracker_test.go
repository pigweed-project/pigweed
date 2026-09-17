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
	if !strings.Contains(err.Error(), "HTTP 401") || !strings.Contains(err.Error(), "luci-auth login") || !strings.Contains(err.Error(), "gcloud auth") {
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
}

func TestLabelToQueryToken(t *testing.T) {
	tests := []struct {
		label string
		want  string
	}{
		{"P0", "priority:P0"},
		{"priority:p2", "priority:P2"},
		{"S1", "severity:S1"},
		{"bug", "type:BUG"},
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

func TestParseSSOClientResponse(t *testing.T) {
	req, err := http.NewRequest(http.MethodGet, "https://issuetracker.corp.googleapis.com/v1/issues/123", nil)
	if err != nil {
		t.Fatalf("http.NewRequest: %v", err)
	}

	t.Run("standard HTTP/1.1 response with banner prefix", func(t *testing.T) {
		raw := "sso_client: connecting to uberproxy...\r\n" +
			"HTTP/1.1 200 OK\r\n" +
			"Content-Type: application/json\r\n" +
			"\r\n" +
			`{"issueId":"123"}`
		resp, err := parseSSOClientResponse([]byte(raw), req)
		if err != nil {
			t.Fatalf("parseSSOClientResponse unexpected error: %v", err)
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
		resp, err := parseSSOClientResponse([]byte(raw), req)
		if err != nil {
			t.Fatalf("parseSSOClientResponse unexpected error: %v", err)
		}
		defer resp.Body.Close()
		if resp.StatusCode != http.StatusNotFound {
			t.Errorf("StatusCode = %d, want 404", resp.StatusCode)
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
		Endpoint: "http://example.invalid/v1",
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
		QuotaProjectProvider: func(ctx context.Context, token string) string {
			return "broken-project"
		},
	}

	_, err := client.GetIssue(context.Background(), 12345)
	if err == nil {
		t.Fatal("expected error from HTTP 403 USER_PROJECT_DENIED, got nil")
	}
	if !strings.Contains(err.Error(), "quota project error") || !strings.Contains(err.Error(), "ghish.quotaproject") {
		t.Errorf("expected actionable quota project remediation in error, got:\n%v", err)
	}
}
