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
	"io"
	"strings"
	"testing"
)

func TestIssueView_ExplicitAndSmartBranchInference(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	srv.Install(t)

	srv.SeedIssue(&BuganizerIssue{
		IssueID: 345678,
		State: BuganizerState{
			ComponentID: 1194524,
			Type:        "BUG",
			Status:      "ASSIGNED",
			Priority:    "P1",
			Severity:    "S2",
			Title:       "pw_rpc: Fix channel deadlock",
			Reporter:    &BuganizerUser{EmailAddress: "author@google.com"},
			Assignee:    &BuganizerUser{EmailAddress: "owner@google.com"},
		},
	}, "Detailed bug report body.")
	_, _ = srv.Client().CreateComment(context.Background(), 345678, "Investigating root cause.")

	gitRunner := &MockGitRunner{}
	SetupMockConfig(t, gitRunner)

	// 1. Explicit ID view
	out, err := executeCommand(RootCmd, "issue", "view", "345678")
	if err != nil {
		t.Fatalf("issue view 345678 failed: %v\nOutput: %s", err, out)
	}
	for _, want := range []string{
		"pw_rpc: Fix channel deadlock #345678",
		"ASSIGNED",
		"P1",
		"owner@google.com",
		"--- BEGIN UNTRUSTED ISSUE DESCRIPTION (b/345678) ---",
		"Detailed bug report body.",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("issue view output missing %q; got:\n%s", want, out)
		}
	}

	// 2. View with --comments
	outComments, err := executeCommand(RootCmd, "issue", "view", "b/345678", "--comments")
	if err != nil {
		t.Fatalf("issue view --comments failed: %v", err)
	}
	if !strings.Contains(outComments, "Investigating root cause.") {
		t.Errorf("issue view --comments missing comment #2; got:\n%s", outComments)
	}

	// 3. Smart branch inference from HEAD commit message trailer
	gitRunner.OnCommand("log -1 --format=%B HEAD", "pw_rpc: Fix deadlock\n\nBug: b/345678\nChange-Id: I1234567890123456789012345678901234567890\n")
	outInferred, err := executeCommand(RootCmd, "issue", "view")
	if err != nil {
		t.Fatalf("issue view (inferred) failed: %v\nOutput: %s", err, outInferred)
	}
	if !strings.Contains(outInferred, "pw_rpc: Fix channel deadlock #345678") {
		t.Errorf("inferred issue view output missing title; got:\n%s", outInferred)
	}

	// 4. Smart branch inference error when HEAD has no Bug: trailer
	gitRunner.OnCommand("log -1 --format=%B HEAD", "pw_rpc: Docs update\n\nChange-Id: I1234567890123456789012345678901234567890\n")
	_, errNoBug := executeCommand(RootCmd, "issue", "view")
	if errNoBug == nil {
		t.Fatal("expected error when no issue ID provided and HEAD has no Bug: trailer")
	}
	if !strings.Contains(errNoBug.Error(), "no Bug: or Fixed: trailer") {
		t.Errorf("unexpected error message for missing trailer: %v", errNoBug)
	}

	// 5. Smart branch inference error when HEAD has multiple bugs
	gitRunner.OnCommand("log -1 --format=%B HEAD", "pw_rpc: Multi fix\n\nBug: b/345678, b/999999\n")
	_, errMultiBug := executeCommand(RootCmd, "issue", "view")
	if errMultiBug == nil {
		t.Fatal("expected error when HEAD has multiple bug trailers")
	}
	if !strings.Contains(errMultiBug.Error(), "multiple bug IDs") {
		t.Errorf("unexpected error for multiple bugs: %v", errMultiBug)
	}

	// 6. --json output and schema validation
	outJSON, err := executeCommand(RootCmd, "issue", "view", "345678", "--json", "number,title,state,priority,labels,url")
	if err != nil {
		t.Fatalf("issue view --json failed: %v", err)
	}
	var parsed map[string]any
	if err := json.Unmarshal([]byte(outJSON), &parsed); err != nil {
		t.Fatalf("failed to parse JSON output: %v\nOutput: %s", err, outJSON)
	}
	if parsed["number"] != float64(345678) || parsed["state"] != "OPEN" || parsed["priority"] != "P1" {
		t.Errorf("unexpected JSON fields: %+v", parsed)
	}

	// Unknown JSON field must fail loudly
	_, errBadJSON := executeCommand(RootCmd, "issue", "view", "345678", "--json", "number,unknownField")
	if errBadJSON == nil || !strings.Contains(errBadJSON.Error(), "unknown JSON field") {
		t.Errorf("expected unknown JSON field error, got: %v", errBadJSON)
	}
}

func TestIssueList_FiltersAndJSON(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	srv.Install(t)

	srv.SeedIssue(&BuganizerIssue{
		IssueID: 101,
		State: BuganizerState{
			ComponentID: 1194524,
			Status:      "NEW",
			Priority:    "P2",
			Title:       "First open issue",
		},
	}, "Desc 101")
	srv.SeedIssue(&BuganizerIssue{
		IssueID: 102,
		State: BuganizerState{
			ComponentID: 1194524,
			Status:      "ASSIGNED",
			Priority:    "P1",
			Title:       "Second open issue assigned",
			Assignee:    &BuganizerUser{EmailAddress: "alice@google.com"},
		},
	}, "Desc 102")
	srv.SeedIssue(&BuganizerIssue{
		IssueID: 103,
		State: BuganizerState{
			ComponentID: 1194524,
			Status:      "FIXED",
			Priority:    "P2",
			Title:       "Third closed issue",
		},
	}, "Desc 103")

	gitRunner := &MockGitRunner{}
	SetupMockConfig(t, gitRunner)

	// Default list (open)
	out, err := executeCommand(RootCmd, "issue", "list")
	if err != nil {
		t.Fatalf("issue list failed: %v", err)
	}
	if !strings.Contains(out, "First open issue") || !strings.Contains(out, "Second open issue assigned") {
		t.Errorf("issue list missing open issues:\n%s", out)
	}
	if strings.Contains(out, "Third closed issue") {
		t.Errorf("issue list should not include closed issue by default:\n%s", out)
	}

	// Filtered by --state closed
	outClosed, err := executeCommand(RootCmd, "issue", "list", "--state", "closed")
	if err != nil {
		t.Fatalf("issue list --state closed failed: %v", err)
	}
	if !strings.Contains(outClosed, "Third closed issue") || strings.Contains(outClosed, "First open issue") {
		t.Errorf("unexpected --state closed output:\n%s", outClosed)
	}

	// Filtered by assignee and label P1
	outAlice, err := executeCommand(RootCmd, "issue", "list", "--assignee", "alice@google.com", "-l", "P1")
	if err != nil {
		t.Fatalf("issue list --assignee failed: %v", err)
	}
	if !strings.Contains(outAlice, "Second open issue assigned") || strings.Contains(outAlice, "First open issue") {
		t.Errorf("unexpected filtered list output:\n%s", outAlice)
	}

	// Invalid --state value
	_, errBadState := executeCommand(RootCmd, "issue", "list", "--state", "bogus")
	if errBadState == nil || !strings.Contains(errBadState.Error(), "invalid --state") {
		t.Errorf("expected error for invalid --state, got: %v", errBadState)
	}
}

func TestIssueStatus(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	srv.Install(t)

	srv.SeedIssue(&BuganizerIssue{
		IssueID: 201,
		State: BuganizerState{
			ComponentID: 1194524,
			Status:      "ASSIGNED",
			Priority:    "P1",
			Title:       "Assigned to me issue",
			Assignee:    &BuganizerUser{EmailAddress: "dev@google.com"},
		},
	}, "Body")
	srv.SeedIssue(&BuganizerIssue{
		IssueID: 202,
		State: BuganizerState{
			ComponentID: 1194524,
			Status:      "NEW",
			Priority:    "P2",
			Title:       "Reported by me issue",
			Reporter:    &BuganizerUser{EmailAddress: "dev@google.com"},
		},
	}, "Body")

	gitRunner := &MockGitRunner{}
	gitRunner.OnCommand("config --get user.email", "dev@google.com")
	SetupMockConfig(t, gitRunner)

	out, err := executeCommand(RootCmd, "issue", "status")
	if err != nil {
		t.Fatalf("issue status failed: %v\nOutput: %s", err, out)
	}
	if !strings.Contains(out, "Assigned to me issue") || !strings.Contains(out, "Reported by me issue") {
		t.Errorf("issue status missing expected issues:\n%s", out)
	}
}

func TestIssueCreate_AndAmend(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	srv.Install(t)

	var amendedMsg string
	gitRunner := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			cmdStr := strings.Join(args, " ")
			switch {
			case cmdStr == "log -1 --format=%B HEAD":
				stdout.Write([]byte("pw_allocator: Refactor block split\n\nChange-Id: I1234567890123456789012345678901234567890\n"))
			case cmdStr == "branch -r --points-at HEAD":
				// Not a remote tracking branch tip
				return nil
			case strings.HasPrefix(cmdStr, "commit --amend -m "):
				amendedMsg = args[3]
				return nil
			}
			return nil
		},
	}
	SetupMockConfig(t, gitRunner)

	// 0. Creating on generic profile without -C or git config fails with 4-pillar actionable error
	_, errNoComp := executeCommand(RootCmd, "issue", "create", "-t", "Missing component test")
	if errNoComp == nil || !strings.Contains(errNoComp.Error(), "no Buganizer component ID configured") {
		t.Fatalf("expected missing component ID error on generic profile, got: %v", errNoComp)
	}

	// Set active profile to pigweed (default component 1194524)
	SetTestProfile(t, "pigweed")

	// 1. Create with --assignee (status must automatically be ASSIGNED) and --amend
	out, err := executeCommand(RootCmd, "issue", "create",
		"-t", "pw_allocator: Fix split block alignment",
		"-b", "Blocks were misaligned on 64-bit targets.",
		"-P", "P1",
		"-a", "alice@google.com",
		"--amend",
	)
	if err != nil {
		t.Fatalf("issue create --amend failed: %v\nOutput: %s", err, out)
	}

	createdIssue, err := srv.Client().GetIssue(context.Background(), 300001)
	if err != nil {
		t.Fatalf("failed to fetch created issue 300001: %v", err)
	}
	if createdIssue.State.Status != "ASSIGNED" {
		t.Errorf("created issue status = %q, want ASSIGNED when assignee is provided", createdIssue.State.Status)
	}
	if createdIssue.State.Priority != "P1" {
		t.Errorf("created issue priority = %q, want P1", createdIssue.State.Priority)
	}

	// Verify commit message was amended with Bug: b/300001 while preserving Change-Id
	if !strings.Contains(amendedMsg, "Bug: b/300001") {
		t.Errorf("amended commit message missing 'Bug: b/300001':\n%s", amendedMsg)
	}
	if !strings.Contains(amendedMsg, "Change-Id: I1234567890123456789012345678901234567890") {
		t.Errorf("amended commit message lost Change-Id trailer:\n%s", amendedMsg)
	}
}

func TestIssueEdit_Close_Reopen_Comment(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	srv.Install(t)

	srv.SeedIssue(&BuganizerIssue{
		IssueID: 400,
		State: BuganizerState{
			ComponentID: 1194524,
			Status:      "NEW",
			Priority:    "P2",
			Title:       "Original bug title",
		},
	}, "Initial description")

	gitRunner := &MockGitRunner{}
	SetupMockConfig(t, gitRunner)

	// 1. Edit title, label (P0), and add assignee -> transitions NEW -> ASSIGNED
	_, err := executeCommand(RootCmd, "issue", "edit", "400", "-t", "Edited bug title", "-l", "P0", "--add-assignee", "bob@google.com")
	if err != nil {
		t.Fatalf("issue edit failed: %v", err)
	}
	iss, _ := srv.Client().GetIssue(context.Background(), 400)
	if iss.State.Title != "Edited bug title" || iss.State.Priority != "P0" || iss.State.Status != "ASSIGNED" {
		t.Errorf("unexpected state after edit: %+v", iss.State)
	}

	// 2. Comment
	_, err = executeCommand(RootCmd, "issue", "comment", "400", "-b", "Progress update comment")
	if err != nil {
		t.Fatalf("issue comment failed: %v", err)
	}
	comments, _ := srv.Client().ListComments(context.Background(), 400, 10, "")
	if len(comments.IssueComments) != 2 || comments.IssueComments[1].Comment != "Progress update comment" {
		t.Errorf("unexpected comments after issue comment: %+v", comments.IssueComments)
	}

	// 3. Close with reason "not planned"
	_, err = executeCommand(RootCmd, "issue", "close", "400", "-r", "not planned", "-c", "Closing as obsolete")
	if err != nil {
		t.Fatalf("issue close failed: %v", err)
	}
	iss, _ = srv.Client().GetIssue(context.Background(), 400)
	if iss.State.Status != "OBSOLETE" {
		t.Errorf("status after close -r 'not planned' = %q, want OBSOLETE", iss.State.Status)
	}

	// 4. Reopen
	_, err = executeCommand(RootCmd, "issue", "reopen", "400", "-c", "Reopening issue")
	if err != nil {
		t.Fatalf("issue reopen failed: %v", err)
	}
	iss, _ = srv.Client().GetIssue(context.Background(), 400)
	if iss.State.Status != "ASSIGNED" {
		t.Errorf("status after reopen (with assignee) = %q, want ASSIGNED", iss.State.Status)
	}

	// 5. Close as duplicate
	_, err = executeCommand(RootCmd, "issue", "close", "400", "--duplicate-of", "111111")
	if err != nil {
		t.Fatalf("issue close --duplicate-of failed: %v", err)
	}
	iss, _ = srv.Client().GetIssue(context.Background(), 400)
	if iss.State.Status != "DUPLICATE" || iss.State.CanonicalIssueID != 111111 {
		t.Errorf("unexpected state after duplicate close: status=%q, canonical=%d", iss.State.Status, iss.State.CanonicalIssueID)
	}
}

func TestIssueDevelop(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	srv.Install(t)

	srv.SeedIssue(&BuganizerIssue{
		IssueID: 505,
		State: BuganizerState{
			ComponentID: 1194524,
			Status:      "NEW",
			Title:       "pw_bluetooth: Add LE Gatt client support!",
		},
	}, "Description")

	var checkedOutBranch string
	gitRunner := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) >= 3 && args[0] == "checkout" && args[1] == "-b" {
				checkedOutBranch = args[2]
			}
			return nil
		},
	}
	SetupMockConfig(t, gitRunner)

	out, err := executeCommand(RootCmd, "issue", "develop", "505")
	if err != nil {
		t.Fatalf("issue develop failed: %v\nOutput: %s", err, out)
	}
	wantBranch := "b-505-pw-bluetooth-add-le-gatt-client-support"
	if checkedOutBranch != wantBranch {
		t.Errorf("checked out branch = %q, want %q", checkedOutBranch, wantBranch)
	}
}

func TestIssue_FailFastOnInvalidConfig(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	srv.Install(t)

	gitRunner := &MockGitRunner{}
	SetupMockConfig(t, gitRunner)
	SetTestProfile(t, "pigweed")

	// 1. Invalid git config ghish.componentid must fail fast (no silent fallback to pigweed default)
	gitRunner.OnCommand("config --get ghish.componentid", "invalid-id")
	_, errBadComp := executeCommand(RootCmd, "issue", "list")
	if errBadComp == nil || !strings.Contains(errBadComp.Error(), "invalid git config 'ghish.componentid'") {
		t.Errorf("expected invalid ghish.componentid error, got: %v", errBadComp)
	}
	gitRunner.OnCommand("config --get ghish.componentid", "")

	// 2. --assignee me when user.email is unconfigured must fail fast
	_, errNoEmail := executeCommand(RootCmd, "issue", "list", "--assignee", "me")
	if errNoEmail == nil || !strings.Contains(errNoEmail.Error(), "git config 'user.email' is not configured") {
		t.Errorf("expected unconfigured user.email error for --assignee me, got: %v", errNoEmail)
	}

	// 3. Unsupported label format must fail fast
	_, errBadLabel := executeCommand(RootCmd, "issue", "list", "-l", "arbitrary-label")
	if errBadLabel == nil || !strings.Contains(errBadLabel.Error(), "unsupported Buganizer label format") {
		t.Errorf("expected unsupported Buganizer label format error, got: %v", errBadLabel)
	}
}

func TestIssueViewWeb_OpensBrowser(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	srv.Install(t)

	gitRunner := &MockGitRunner{}
	SetupMockConfig(t, gitRunner)
	SetTestProfile(t, "pigweed")

	var openedURL string
	origOpen := OpenBrowserFn
	OpenBrowserFn = func(url string) error {
		openedURL = url
		return nil
	}
	t.Cleanup(func() {
		OpenBrowserFn = origOpen
	})

	out, err := executeCommand(RootCmd, "issue", "view", "345678", "--web")
	if err != nil {
		t.Fatalf("issue view --web failed: %v", err)
	}
	wantURL := "https://issues.pigweed.dev/issues/345678"
	if openedURL != wantURL {
		t.Errorf("openedURL = %q, want %q", openedURL, wantURL)
	}
	if !strings.Contains(out, wantURL) {
		t.Errorf("output %q missing %q", out, wantURL)
	}
}

func TestIssueViewComments_Pagination(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	srv.Install(t)
	srv.CommentPageSize = 2

	srv.SeedIssue(&BuganizerIssue{
		IssueID: 777,
		State: BuganizerState{
			ComponentID: 1194524,
			Status:      "ASSIGNED",
			Title:       "Multi-page comment issue",
		},
	}, "Comment 1 (Description)")
	_, _ = srv.Client().CreateComment(context.Background(), 777, "Comment 2 on page 1")
	_, _ = srv.Client().CreateComment(context.Background(), 777, "Comment 3 on page 2")
	_, _ = srv.Client().CreateComment(context.Background(), 777, "Comment 4 on page 2")
	_, _ = srv.Client().CreateComment(context.Background(), 777, "Comment 5 on page 3")

	gitRunner := &MockGitRunner{}
	SetupMockConfig(t, gitRunner)

	out, err := executeCommand(RootCmd, "issue", "view", "777", "--comments")
	if err != nil {
		t.Fatalf("issue view --comments failed: %v", err)
	}
	for _, want := range []string{
		"Comment 1 (Description)",
		"Comment 2 on page 1",
		"Comment 3 on page 2",
		"Comment 4 on page 2",
		"Comment 5 on page 3",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("paginated comments output missing %q; got:\n%s", want, out)
		}
	}
}

func TestIssueEdit_AddAssigneeMe(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	srv.Install(t)

	srv.SeedIssue(&BuganizerIssue{
		IssueID: 888,
		State: BuganizerState{
			ComponentID: 1194524,
			Status:      "NEW",
			Title:       "Unassigned issue",
		},
	}, "Description")

	gitRunner := &MockGitRunner{}
	gitRunner.OnCommand("config --get user.email", "me-developer@google.com")
	SetupMockConfig(t, gitRunner)

	out, err := executeCommand(RootCmd, "issue", "edit", "888", "--add-assignee", "me")
	if err != nil {
		t.Fatalf("issue edit --add-assignee me failed: %v\nOutput: %s", err, out)
	}

	iss, err := srv.Client().GetIssue(context.Background(), 888)
	if err != nil {
		t.Fatalf("GetIssue failed: %v", err)
	}
	if iss.State.Assignee == nil || iss.State.Assignee.EmailAddress != "me-developer@google.com" {
		t.Errorf("Assignee = %+v, want me-developer@google.com", iss.State.Assignee)
	}
	if iss.State.Status != "ASSIGNED" {
		t.Errorf("Status = %q, want ASSIGNED", iss.State.Status)
	}
}

func TestIssueCreate_AmendDeduplicatesExistingBug(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	srv.Install(t)
	// NextIssueID starts at 300001 in MockIssueTrackerServer
	var amendedMsg string
	gitRunner := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			for i := 0; i < len(args)-1; i++ {
				if args[i] == "-m" {
					amendedMsg = args[i+1]
				}
			}
			return nil
		},
	}
	gitRunner.OnCommand("config user.email", "dev@google.com")
	// Commit already has b/300001 in Bug: trailer
	gitRunner.OnCommand("log -1 --format=%B HEAD", "pw_foo: Fix bug\n\nBug: b/111111, b/300001\nChange-Id: I1234567890123456789012345678901234567890\n")
	SetupMockConfig(t, gitRunner)
	SetTestProfile(t, "pigweed")

	_, err := executeCommand(RootCmd, "issue", "create", "-t", "New bug", "-b", "Body", "--amend")
	if err != nil {
		t.Fatalf("issue create --amend failed: %v", err)
	}
	if strings.Count(amendedMsg, "300001") != 1 {
		t.Errorf("expected b/300001 to appear exactly once in amended message, got:\n%s", amendedMsg)
	}
}
