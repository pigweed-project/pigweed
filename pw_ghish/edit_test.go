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

func getSentCommitMessage(server *MockGerritServer) string {
	for _, req := range server.Requests() {
		if strings.Contains(req.Path, "message") {
			var payload struct {
				Message string `json:"message"`
			}
			_ = json.Unmarshal(req.Body, &payload)
			return payload.Message
		}
	}
	return ""
}

func TestEditIntegration(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/commit", http.StatusOK, map[string]any{
		"subject": "Old Title",
		"message": "Old Title\n\nExisting body line.\n\nChange-Id: I1234567890123456789012345678901234567890\n",
	})
	server.OnJSON("", "/changes/12345/message", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "edit", "12345", "--title", "New Title")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("GET", "/changes/12345/revisions/current/commit") != 1 {
		t.Error("Expected GetCommit API to be called to fetch existing message")
	}
	if server.CallCount("", "/changes/12345/message") != 1 {
		t.Error("Expected SetCommitMessage API to be called")
	}

	if !strings.Contains(output, "Commit message updated successfully") {
		t.Errorf("Unexpected output: %s", output)
	}

	sentMessage := getSentCommitMessage(server)
	if !strings.HasPrefix(sentMessage, "New Title") {
		t.Errorf("Expected sentMessage to start with 'New Title', got: %q", sentMessage)
	}
	if !strings.Contains(sentMessage, "Existing body line.") {
		t.Errorf("DATA LOSS: sentMessage lost existing body! got: %q", sentMessage)
	}
	if !strings.Contains(sentMessage, "Change-Id: I1234567890123456789012345678901234567890") {
		t.Errorf("DATA LOSS: sentMessage lost Change-Id! got: %q", sentMessage)
	}
}

func TestEdit_PreservesTitleAndChangeIDWhenUpdatingBody(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/commit", http.StatusOK, map[string]any{
		"subject": "Original Title",
		"message": "Original Title\n\nOld body.\n\nChange-Id: Iabcdef1234567890abcdef1234567890abcdef12\n",
	})
	server.OnJSON("", "/changes/12345/message", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "edit", "12345", "--body", "Brand new body details.")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	sentMessage := getSentCommitMessage(server)
	if !strings.HasPrefix(sentMessage, "Original Title") {
		t.Errorf("DATA LOSS: sentMessage lost title! got: %q", sentMessage)
	}
	if !strings.Contains(sentMessage, "Brand new body details.") {
		t.Errorf("Expected sentMessage to contain new body! got: %q", sentMessage)
	}
	if !strings.Contains(sentMessage, "Change-Id: Iabcdef1234567890abcdef1234567890abcdef12") {
		t.Errorf("DATA LOSS: sentMessage lost Change-Id! got: %q", sentMessage)
	}
}

func TestEdit_ErrorWhenNoFlagsProvided(t *testing.T) {
	_, err := executeCommand(RootCmd, "pr", "edit", "12345")
	if err == nil {
		t.Fatal("Expected error when no edit flags provided, got nil")
	}
	if !strings.Contains(err.Error(), "at least one of") {
		t.Errorf("Expected error message about missing flags, got: %v", err)
	}
	if !strings.Contains(err.Error(), "Commit-Queue=1") {
		t.Errorf("Expected error message to include examples, got: %v", err)
	}
}

func TestEdit_ErrorInvalidLabelFormat(t *testing.T) {
	_, err := executeCommand(RootCmd, "pr", "edit", "12345", "--add-label", "InvalidFormat")
	if err == nil {
		t.Fatal("Expected error on invalid label format, got nil")
	}
	if !strings.Contains(err.Error(), "must be in the format 'Name+Score' or 'Name=Score'") {
		t.Errorf("Expected label format explanation, got: %v", err)
	}
	if !strings.Contains(err.Error(), "Commit-Queue=1") {
		t.Errorf("Expected example in error, got: %v", err)
	}
}

func TestEdit_ErrorInvalidLabelScore(t *testing.T) {
	_, err := executeCommand(RootCmd, "pr", "edit", "12345", "--add-label", "Commit-Queue=ready")
	if err == nil {
		t.Fatal("Expected error on non-integer score, got nil")
	}
	if !strings.Contains(err.Error(), "score must be an integer") {
		t.Errorf("Expected integer score requirement explanation, got: %v", err)
	}
	if !strings.Contains(err.Error(), "Commit-Queue=1") {
		t.Errorf("Expected example in error, got: %v", err)
	}
}

func TestEdit_ErrorWhenBothMessageAndTitleProvided(t *testing.T) {
	_, err := executeCommand(RootCmd, "pr", "edit", "12345", "--message", "msg", "--title", "title")
	if err == nil {
		t.Fatal("Expected error when both --message and --title provided, got nil")
	}
	if !strings.Contains(err.Error(), "cannot specify both") {
		t.Errorf("Expected error message about conflicting flags, got: %v", err)
	}
}

func TestEdit_ErrorWhenInvalidLabel(t *testing.T) {
	_, err := executeCommand(RootCmd, "pr", "edit", "12345", "--add-label", "NotAValidLabel")
	if err == nil {
		t.Fatal("Expected error on invalid label syntax, got nil")
	}
	if !strings.Contains(err.Error(), "must be in the format") {
		t.Errorf("Expected error message about label format, got: %v", err)
	}
}

func TestEditAddLabel(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("", "/changes/12345/revisions/current/review", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "edit", "12345", "--add-label", "Commit-Queue=+1", "--add-label", "Fuchsia-Auto-Submit=1")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("", "/changes/12345/revisions/current/review") != 1 {
		t.Error("Expected SetReview API to be called for labels")
	}

	if !strings.Contains(output, "Labels added successfully") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestEditAddMultipleReviewers(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("", "/changes/12345/reviewers", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "edit", "12345", "--add-reviewer", "adamperry@google.com", "--add-reviewer", "jamesr@google.com")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("", "/changes/12345/reviewers") != 2 {
		t.Errorf("Expected 2 calls to AddReviewer API, got %d", server.CallCount("", "/changes/12345/reviewers"))
	}

	if !strings.Contains(output, "Reviewer added successfully") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestEditRemoveMultipleReviewers(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("DELETE", "/changes/12345/reviewers*", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "edit", "12345", "--remove-reviewer", "adamperry@google.com", "--remove-reviewer", "jamesr@google.com")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("DELETE", "") != 2 {
		t.Errorf("Expected 2 calls to DeleteReviewer API, got %d", server.CallCount("DELETE", ""))
	}

	if !strings.Contains(output, "Reviewer removed successfully") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestEditAddAssignee(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/12345/reviewers", http.StatusOK, map[string]any{})
	server.OnJSON("POST", "/changes/12345/revisions/current/review", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "edit", "12345", "--add-assignee", "helper@google.com")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("POST", "/changes/12345/reviewers") != 1 {
		t.Errorf("AddReviewer API calls: got %d, want 1", server.CallCount("POST", "/changes/12345/reviewers"))
	}
	if server.CallCount("POST", "/changes/12345/revisions/current/review") != 1 {
		t.Errorf("SetReview (Attention Set) API calls: got %d, want 1", server.CallCount("POST", "/changes/12345/revisions/current/review"))
	}

	if !strings.Contains(output, "Assignee added successfully") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestEdit_PreservesAllTrailersWhenUpdatingBody(t *testing.T) {
	origCommitMsg := "pw_foo: Original Subject\n\nOriginal body line.\n\nBug: b/12345\nChange-Id: I0123456789abcdef0123456789abcdef01234567\nReviewed-on: https://pigweed-review.googlesource.com/12345\n"

	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/commit", http.StatusOK, map[string]any{
		"subject": "pw_foo: Original Subject",
		"message": origCommitMsg,
	})
	server.OnJSON("", "/changes/12345/message", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "edit", "12345", "--body", "Updated body text without trailers.")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	sentMessage := getSentCommitMessage(server)
	if !strings.HasPrefix(sentMessage, "pw_foo: Original Subject") {
		t.Errorf("DATA LOSS: Title was not preserved! got: %q", sentMessage)
	}
	if !strings.Contains(sentMessage, "Updated body text without trailers.") {
		t.Errorf("Expected new body to be present, got: %q", sentMessage)
	}
	if !strings.Contains(sentMessage, "Bug: b/12345") {
		t.Errorf("DATA LOSS: Bug trailer was lost! got: %q", sentMessage)
	}
	if !strings.Contains(sentMessage, "Change-Id: I0123456789abcdef0123456789abcdef01234567") {
		t.Errorf("DATA LOSS: Change-Id was lost! got: %q", sentMessage)
	}
	if !strings.Contains(sentMessage, "Reviewed-on: https://pigweed-review.googlesource.com/12345") {
		t.Errorf("DATA LOSS: Reviewed-on trailer was lost! got: %q", sentMessage)
	}
}

func TestEdit_OverridingSpecificTrailerPreservesOthers(t *testing.T) {
	origCommitMsg := "pw_foo: Original Subject\n\nOriginal body line.\n\nBug: b/12345\nChange-Id: I0123456789abcdef0123456789abcdef01234567\nReviewed-on: https://pigweed-review.googlesource.com/12345\n"

	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/commit", http.StatusOK, map[string]any{
		"subject": "pw_foo: Original Subject",
		"message": origCommitMsg,
	})
	server.OnJSON("", "/changes/12345/message", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "edit", "12345", "--body", "Updated body text.\n\nBug: b/99999")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	sentMessage := getSentCommitMessage(server)
	if strings.Contains(sentMessage, "Bug: b/12345") {
		t.Errorf("Expected old Bug: b/12345 to be replaced by new bug, but found in: %q", sentMessage)
	}
	if !strings.Contains(sentMessage, "Bug: b/99999") {
		t.Errorf("Expected new Bug: b/99999 to be present, got: %q", sentMessage)
	}
	if !strings.Contains(sentMessage, "Change-Id: I0123456789abcdef0123456789abcdef01234567") {
		t.Errorf("DATA LOSS: Change-Id was lost! got: %q", sentMessage)
	}
	if !strings.Contains(sentMessage, "Reviewed-on: https://pigweed-review.googlesource.com/12345") {
		t.Errorf("DATA LOSS: Reviewed-on trailer was lost! got: %q", sentMessage)
	}
}

// TestEdit_PreservesTrailersWhenMessageEndsInProse pins the bug that motivated
// rewriting ExtractTrailers. The old implementation looked only at the final
// paragraph, so a commit message ending in prose -- an extremely common shape,
// e.g. "Note: reviewers, ..." -- lost its real trailer block. A hardcoded
// allowlist then rescued a handful of well-known keys, which is why this was
// not caught earlier: Change-Id and Bug survived, and everything else
// (Co-authored-by, Cq-Include-Trybots, the cherry-pick provenance footer) was
// silently deleted. Verified RED against the previous implementation.
func TestEdit_PreservesTrailersWhenMessageEndsInProse(t *testing.T) {
	origCommitMsg := strings.Join([]string{
		"pw_foo: Original Subject",
		"",
		"Original body line.",
		"",
		"Bug: b/12345",
		"Change-Id: I0123456789abcdef0123456789abcdef01234567",
		"Co-authored-by: Someone <someone@example.com>",
		"(cherry picked from commit deadbeefdeadbeefdeadbeefdeadbeefdeadbeef)",
		"",
		"Note: reviewers please look at the retry logic.",
		"It changed subtly in the last patchset.",
		"",
	}, "\n")

	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/commit", http.StatusOK, map[string]any{
		"subject": "pw_foo: Original Subject",
		"message": origCommitMsg,
	})
	server.OnJSON("", "/changes/12345/message", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "edit", "12345", "--body", "Updated body text.")
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	sentMessage := getSentCommitMessage(server)
	for _, want := range []string{
		"Bug: b/12345",
		"Change-Id: I0123456789abcdef0123456789abcdef01234567",
		"Co-authored-by: Someone <someone@example.com>",
		"(cherry picked from commit deadbeefdeadbeefdeadbeefdeadbeefdeadbeef)",
	} {
		if !strings.Contains(sentMessage, want) {
			t.Errorf("DATA LOSS: %q missing from rewritten message:\n%s", want, sentMessage)
		}
	}
	if !strings.Contains(sentMessage, "Updated body text.") {
		t.Errorf("Expected new body to be present, got:\n%s", sentMessage)
	}
}

// TestEdit_DoesNotPromoteProseIntoTrailers is the other half of the contract:
// widening the trailer scan to every paragraph must not turn ordinary prose
// that happens to contain a colon into a trailer that survives a body rewrite.
func TestEdit_DoesNotPromoteProseIntoTrailers(t *testing.T) {
	origCommitMsg := strings.Join([]string{
		"pw_foo: Original Subject",
		"",
		"Warning: this paragraph is prose, not a trailer block.",
		"It spans two lines, which is what proves it is prose.",
		"",
		"Change-Id: I0123456789abcdef0123456789abcdef01234567",
		"",
	}, "\n")

	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/commit", http.StatusOK, map[string]any{
		"subject": "pw_foo: Original Subject",
		"message": origCommitMsg,
	})
	server.OnJSON("", "/changes/12345/message", http.StatusOK, map[string]any{})

	if _, err := executeCommand(RootCmd, "pr", "edit", "12345", "--body", "Updated body text."); err != nil {
		t.Fatalf("Command failed: %v", err)
	}

	sentMessage := getSentCommitMessage(server)
	if strings.Contains(sentMessage, "Warning: this paragraph is prose") {
		t.Errorf("Prose was promoted into a trailer and re-appended:\n%s", sentMessage)
	}
	if !strings.Contains(sentMessage, "Change-Id: I0123456789abcdef0123456789abcdef01234567") {
		t.Errorf("DATA LOSS: Change-Id was lost!\n%s", sentMessage)
	}
}

func TestEdit_RawMessagePreservesChangeID(t *testing.T) {
	origCommitMsg := "pw_foo: Original Subject\n\nOriginal body line.\n\nChange-Id: I0123456789abcdef0123456789abcdef01234567\n"

	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/commit", http.StatusOK, map[string]any{
		"subject": "pw_foo: Original Subject",
		"message": origCommitMsg,
	})
	server.OnJSON("", "/changes/12345/message", http.StatusOK, map[string]any{})

	_, err := executeCommand(RootCmd, "pr", "edit", "12345", "--message", "pw_foo: Complete new message\n\nNew description without Change-Id.")
	if err != nil {
		t.Fatalf("Command failed: %v", err)
	}

	sentMessage := getSentCommitMessage(server)
	if !strings.Contains(sentMessage, "Change-Id: I0123456789abcdef0123456789abcdef01234567") {
		t.Errorf("DATA LOSS: Raw message wiped Change-Id! got: %q", sentMessage)
	}
}

// TestEdit_RawMessageRefusesToDropTrailers covers the trailers `-m` cannot
// silently discard. `-m` replaces the whole message, so re-appending author
// trailers behind the user's back would override a deliberate deletion.
// Deleting them by accident is far more common than deleting them on purpose,
// so the tool refuses and says exactly what it would have destroyed.
func TestEdit_RawMessageRefusesToDropTrailers(t *testing.T) {
	origCommitMsg := strings.Join([]string{
		"pw_foo: Original Subject",
		"",
		"Original body line.",
		"",
		"Bug: b/12345",
		"Co-authored-by: Someone <someone@example.com>",
		"Change-Id: I0123456789abcdef0123456789abcdef01234567",
		"",
	}, "\n")

	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/commit", http.StatusOK, map[string]any{
		"subject": "pw_foo: Original Subject",
		"message": origCommitMsg,
	})
	server.OnJSON("", "/changes/12345/message", http.StatusOK, map[string]any{})

	_, err := executeCommand(RootCmd, "pr", "edit", "12345", "--message", "pw_foo: Rewritten\n\nNew description.")
	if err == nil {
		t.Fatal("Expected an error when --message would drop trailers, got nil")
	}

	// Pillar 1: say what would have been destroyed, by name.
	for _, want := range []string{"Bug: b/12345", "Co-authored-by: Someone <someone@example.com>"} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("Error does not name the dropped trailer %q:\n%v", want, err)
		}
	}
	// Pillar 3: an escape hatch the user can actually copy-paste.
	if !strings.Contains(err.Error(), "--drop-trailers") {
		t.Errorf("Error does not mention the --drop-trailers escape hatch:\n%v", err)
	}
	// Change-Id is the change's identity, not author content; it is always
	// rescued and so must never be listed as a casualty.
	if strings.Contains(err.Error(), "Change-Id:") {
		t.Errorf("Change-Id is auto-preserved and must not be reported as dropped:\n%v", err)
	}
	// Nothing may be written when the command refuses.
	if n := server.CallCount("", "/changes/12345/message"); n != 0 {
		t.Errorf("Commit message was written despite the refusal (%d calls)", n)
	}
}

func TestEdit_RawMessageDropTrailersFlagAllowsTheDrop(t *testing.T) {
	origCommitMsg := "pw_foo: Original Subject\n\nBody.\n\nBug: b/12345\nChange-Id: I0123456789abcdef0123456789abcdef01234567\n"

	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/commit", http.StatusOK, map[string]any{
		"subject": "pw_foo: Original Subject",
		"message": origCommitMsg,
	})
	server.OnJSON("", "/changes/12345/message", http.StatusOK, map[string]any{})

	if _, err := executeCommand(RootCmd, "pr", "edit", "12345", "--drop-trailers",
		"--message", "pw_foo: Rewritten\n\nNew description."); err != nil {
		t.Fatalf("Command failed with --drop-trailers: %v", err)
	}

	sentMessage := getSentCommitMessage(server)
	if strings.Contains(sentMessage, "Bug: b/12345") {
		t.Errorf("--drop-trailers was requested but Bug: was re-appended:\n%s", sentMessage)
	}
	if !strings.Contains(sentMessage, "Change-Id: I0123456789abcdef0123456789abcdef01234567") {
		t.Errorf("Change-Id must survive even --drop-trailers:\n%s", sentMessage)
	}
}

// TestEdit_RawMessageCarryingTrailersForwardSucceeds is the intended workflow:
// the user includes the trailers in the new message, so nothing is lost and no
// refusal is warranted. A rewritten value counts as carried forward -- the
// check is per key, not per line, or editing a bug number would be impossible.
func TestEdit_RawMessageCarryingTrailersForwardSucceeds(t *testing.T) {
	origCommitMsg := "pw_foo: Original Subject\n\nBody.\n\nBug: b/12345\nChange-Id: I0123456789abcdef0123456789abcdef01234567\n"

	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/commit", http.StatusOK, map[string]any{
		"subject": "pw_foo: Original Subject",
		"message": origCommitMsg,
	})
	server.OnJSON("", "/changes/12345/message", http.StatusOK, map[string]any{})

	newMsg := "pw_foo: Rewritten\n\nNew description.\n\nBug: b/99999\nChange-Id: I0123456789abcdef0123456789abcdef01234567\n"
	if _, err := executeCommand(RootCmd, "pr", "edit", "12345", "--message", newMsg); err != nil {
		t.Fatalf("Command failed: %v", err)
	}

	sentMessage := getSentCommitMessage(server)
	if !strings.Contains(sentMessage, "Bug: b/99999") {
		t.Errorf("Expected the rewritten Bug value, got:\n%s", sentMessage)
	}
}

// TestEdit_RawMessageRefusesToDropCherryPickProvenance covers the keyless
// footer line, which has no key to compare on and so needs whole-line
// matching. It records where the commit came from; losing it is unrecoverable.
func TestEdit_RawMessageRefusesToDropCherryPickProvenance(t *testing.T) {
	const provenance = "(cherry picked from commit deadbeefdeadbeefdeadbeefdeadbeefdeadbeef)"
	origCommitMsg := "pw_foo: Original Subject\n\nBody.\n\nChange-Id: I0123456789abcdef0123456789abcdef01234567\n" + provenance + "\n"

	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/commit", http.StatusOK, map[string]any{
		"subject": "pw_foo: Original Subject",
		"message": origCommitMsg,
	})
	server.OnJSON("", "/changes/12345/message", http.StatusOK, map[string]any{})

	_, err := executeCommand(RootCmd, "pr", "edit", "12345", "--message", "pw_foo: Rewritten\n\nNew description.")
	if err == nil {
		t.Fatal("Expected an error when --message would drop the cherry-pick footer, got nil")
	}
	if !strings.Contains(err.Error(), provenance) {
		t.Errorf("Error does not name the dropped provenance footer:\n%v", err)
	}
}

// TestEdit_DropTrailersWithoutMessageIsRejected keeps the flag honest.
// --drop-trailers only has meaning for --message, which replaces the whole
// commit message; --body preserves trailers unconditionally. Accepting the
// flag silently would let a user believe they had disabled a safety net they
// never actually touched.
func TestEdit_DropTrailersWithoutMessageIsRejected(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/commit", http.StatusOK, map[string]any{
		"subject": "pw_foo: Original Subject",
		"message": "pw_foo: Original Subject\n\nBody.\n\nChange-Id: I0123456789abcdef0123456789abcdef01234567\n",
	})
	server.OnJSON("", "/changes/12345/message", http.StatusOK, map[string]any{})

	_, err := executeCommand(RootCmd, "pr", "edit", "12345", "--drop-trailers", "--body", "New body.")
	if err == nil {
		t.Fatal("Expected --drop-trailers without --message to be rejected, got nil")
	}
	if !strings.Contains(err.Error(), "--drop-trailers") || !strings.Contains(err.Error(), "--message") {
		t.Errorf("Error should explain that --drop-trailers only applies to --message, got: %v", err)
	}
	if n := server.CallCount("", "/changes/12345/message"); n != 0 {
		t.Errorf("Commit message was written despite the rejection (%d calls)", n)
	}
}

// editServer stands up a mock Gerrit serving origCommitMsg for change 12345
// and accepting a commit message write.
func editServer(t *testing.T, origCommitMsg string) *MockGerritServer {
	t.Helper()
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/commit", http.StatusOK, map[string]any{
		"subject": strings.SplitN(origCommitMsg, "\n", 2)[0],
		"message": origCommitMsg,
	})
	server.OnJSON("", "/changes/12345/message", http.StatusOK, map[string]any{})
	return server
}

const editOrigMsg = "pw_foo: Original Subject\n\nOriginal body line.\n\n" +
	"Co-authored-by: Someone <someone@example.com>\n" +
	"Change-Id: I0123456789abcdef0123456789abcdef01234567\n"

func TestEdit_BugFlag(t *testing.T) {
	tests := []struct {
		name string
		orig string
		args []string
		want string
	}{
		{
			// A bare number is what an agent has after reading a bug URL, and
			// what a human types. Accepting only `b/123456` would push the
			// normalization work back onto the caller.
			name: "bare number becomes a canonical trailer",
			orig: editOrigMsg,
			args: []string{"--bug", "123456"},
			want: "Bug: b/123456",
		},
		{
			name: "issue URL becomes a canonical trailer",
			orig: editOrigMsg,
			args: []string{"--bug", "https://issues.pigweed.dev/issues/123456"},
			want: "Bug: b/123456",
		},
		{
			// The whole point of (c): gh-ish writes Pigweed's house spelling,
			// not GitHub's.
			name: "--fixed writes Fixed:, not Fixes:",
			orig: editOrigMsg,
			args: []string{"--fixed", "999"},
			want: "Fixed: b/999",
		},
		{
			name: "replaces an existing bug rather than adding a second",
			orig: "pw_foo: S\n\nBody.\n\nBug: b/1\nChange-Id: I0123456789abcdef0123456789abcdef01234567\n",
			args: []string{"--bug", "b/2"},
			want: "Bug: b/2",
		},
		{
			name: "none is a valid answer",
			orig: editOrigMsg,
			args: []string{"--bug", "none"},
			want: "Bug: None",
		},
		{
			name: "multiple bugs are accepted",
			orig: editOrigMsg,
			args: []string{"--bug", "123456, 789012"},
			want: "Bug: b/123456, b/789012",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			server := editServer(t, tt.orig)
			args := append([]string{"pr", "edit", "12345"}, tt.args...)
			if _, err := executeCommand(RootCmd, args...); err != nil {
				t.Fatalf("Command failed: %v", err)
			}
			sent := getSentCommitMessage(server)
			if !strings.Contains(sent, tt.want) {
				t.Errorf("Expected %q in the new message, got:\n%s", tt.want, sent)
			}
			// Setting one trailer must never cost another.
			if !strings.Contains(sent, "Change-Id: I0123456789abcdef0123456789abcdef01234567") {
				t.Errorf("DATA LOSS: Change-Id was lost:\n%s", sent)
			}
		})
	}
}

// TestEdit_BugFlagLeavesEverythingElseAlone checks that a trailer-only edit is
// exactly that: no title change, no body change, no collateral trailer loss.
func TestEdit_BugFlagLeavesEverythingElseAlone(t *testing.T) {
	server := editServer(t, editOrigMsg)
	if _, err := executeCommand(RootCmd, "pr", "edit", "12345", "--bug", "b/42"); err != nil {
		t.Fatalf("Command failed: %v", err)
	}
	sent := getSentCommitMessage(server)
	for _, want := range []string{
		"pw_foo: Original Subject",
		"Original body line.",
		"Co-authored-by: Someone <someone@example.com>",
		"Change-Id: I0123456789abcdef0123456789abcdef01234567",
		"Bug: b/42",
	} {
		if !strings.Contains(sent, want) {
			t.Errorf("Expected %q to survive a --bug-only edit, got:\n%s", want, sent)
		}
	}
}

func TestEdit_BugFlagCombinesWithBody(t *testing.T) {
	server := editServer(t, editOrigMsg)
	if _, err := executeCommand(RootCmd, "pr", "edit", "12345",
		"--body", "A new description.", "--bug", "b/42"); err != nil {
		t.Fatalf("Command failed: %v", err)
	}
	sent := getSentCommitMessage(server)
	for _, want := range []string{"A new description.", "Bug: b/42", "Co-authored-by: Someone"} {
		if !strings.Contains(sent, want) {
			t.Errorf("Expected %q in the new message, got:\n%s", want, sent)
		}
	}
}

// TestEdit_BugFlagConflictingWithBodyIsRejected covers the ambiguity. If the
// flag says one bug and the hand-written text says another, guessing which the
// user meant would silently discard one of them.
func TestEdit_BugFlagConflictingWithBodyIsRejected(t *testing.T) {
	server := editServer(t, editOrigMsg)
	_, err := executeCommand(RootCmd, "pr", "edit", "12345",
		"--body", "A new description.\n\nBug: b/7", "--bug", "b/42")
	if err == nil {
		t.Fatal("Expected an error when --bug and --body both set a Bug: trailer, got nil")
	}
	if !strings.Contains(err.Error(), "--bug") || !strings.Contains(err.Error(), "Bug:") {
		t.Errorf("Error should name both the flag and the trailer, got: %v", err)
	}
	if n := server.CallCount("", "/changes/12345/message"); n != 0 {
		t.Errorf("Commit message was written despite the conflict (%d calls)", n)
	}
}

func TestEdit_BugFlagEmptyValueIsRejected(t *testing.T) {
	editServer(t, editOrigMsg)
	_, err := executeCommand(RootCmd, "pr", "edit", "12345", "--bug", "")
	if err == nil {
		t.Fatal("Expected an error for an empty --bug value, got nil")
	}
	if !strings.Contains(err.Error(), "--bug") {
		t.Errorf("Error should name the flag, got: %v", err)
	}
}

// TestEdit_RejectsGitHubIssueSyntax is the reason this feature exists: an
// agent carrying `gh` habits writes "Fixes #456", which Gerrit does not parse.
// Accepting it would leave the bug silently unlinked.
func TestEdit_RejectsGitHubIssueSyntax(t *testing.T) {
	tests := []struct {
		name string
		args []string
	}{
		{"in --body", []string{"--body", "Rework the retry loop.\n\nFixes #456"}},
		{"in --message", []string{"--message", "pw_foo: Rework\n\nCloses #456"}},
		{"in --title", []string{"--title", "pw_foo: Rework, resolves #456"}},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			server := editServer(t, editOrigMsg)
			args := append([]string{"pr", "edit", "12345"}, tt.args...)
			_, err := executeCommand(RootCmd, args...)
			if err == nil {
				t.Fatal("Expected GitHub issue syntax to be rejected, got nil")
			}
			if !strings.Contains(err.Error(), "#456") {
				t.Errorf("Error should quote the offending reference, got: %v", err)
			}
			if !strings.Contains(err.Error(), "--fixed") {
				t.Errorf("Error should point at the --fixed flag, got: %v", err)
			}
			if n := server.CallCount("", "/changes/12345/message"); n != 0 {
				t.Errorf("Commit message was written despite the rejection (%d calls)", n)
			}
		})
	}
}

// TestEdit_AcceptsGerritBugTrailers is the false-positive guard: the correct
// spelling must sail straight through.
func TestEdit_AcceptsGerritBugTrailers(t *testing.T) {
	server := editServer(t, editOrigMsg)
	if _, err := executeCommand(RootCmd, "pr", "edit", "12345",
		"--body", "Rework the retry loop.\n\nFixed: b/456"); err != nil {
		t.Fatalf("A correct Gerrit trailer was rejected: %v", err)
	}
	if !strings.Contains(getSentCommitMessage(server), "Fixed: b/456") {
		t.Errorf("Expected Fixed: b/456 to be written, got:\n%s", getSentCommitMessage(server))
	}
}

func TestEdit_ErrorWhenGetCommitFailsOnRawMessage(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnStatus(http.StatusInternalServerError)

	_, err := executeCommand(RootCmd, "pr", "edit", "12345", "--message", "pw_foo: Complete new message")
	if err == nil {
		t.Fatal("Expected error when GetCommit fails, got nil (error was silently swallowed)")
	}
	if !strings.Contains(err.Error(), "error fetching current commit message") {
		t.Errorf("Expected error to mention fetching current commit message, got: %v", err)
	}
}

func TestEdit_ErrorWhenMismatchedChangeIDProvided(t *testing.T) {
	origCommitMsg := "pw_foo: Original Subject\n\nOriginal body.\n\nChange-Id: I0123456789abcdef0123456789abcdef01234567\n"

	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/commit", http.StatusOK, map[string]any{
		"subject": "pw_foo: Original Subject",
		"message": origCommitMsg,
	})

	_, err := executeCommand(RootCmd, "pr", "edit", "12345", "--message", "pw_foo: Subject\n\nBody\n\nChange-Id: I9999999999999999999999999999999999999999")
	if err == nil {
		t.Fatal("Expected error when mismatched Change-Id provided, got nil")
	}
	if !strings.Contains(err.Error(), "cannot change Gerrit Change-Id") {
		t.Errorf("Expected error about mismatched Change-Id, got: %v", err)
	}
}

func TestEdit_ErrorWhenEmptyTitle(t *testing.T) {
	_, err := executeCommand(RootCmd, "pr", "edit", "12345", "--title", "   ")
	if err == nil {
		t.Fatal("Expected error when empty title provided, got nil")
	}
	if !strings.Contains(err.Error(), "cannot set an empty commit title") {
		t.Errorf("Expected error about empty title, got: %v", err)
	}
}

func TestEdit_ErrorWhenEmptyMessage(t *testing.T) {
	_, err := executeCommand(RootCmd, "pr", "edit", "12345", "--message", "   ")
	if err == nil {
		t.Fatal("Expected error when empty message provided, got nil")
	}
	if !strings.Contains(err.Error(), "cannot set an empty commit message") {
		t.Errorf("Expected error about empty message, got: %v", err)
	}
}

func TestEdit_SetTopic(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("PUT", "/changes/12345/topic", http.StatusOK, `"my-feature-topic"`)

	output, err := executeCommand(RootCmd, "pr", "edit", "12345", "--topic", "my-feature-topic", "--host", server.URL)
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("PUT", "/changes/12345/topic") != 1 {
		t.Errorf("Expected SetTopic API to be called, got calls: %v", server.Requests())
	}
	if !strings.Contains(output, "Topic set to \"my-feature-topic\" successfully.") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestEdit_RemoveTopic(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnStatus(http.StatusNoContent)

	output, err := executeCommand(RootCmd, "pr", "edit", "12345", "--remove-topic", "--host", server.URL)
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("DELETE", "/changes/12345/topic") != 1 {
		t.Errorf("Expected DeleteTopic API to be called, got calls: %v", server.Requests())
	}
	if !strings.Contains(output, "Topic removed successfully.") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestEdit_Hashtags(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/12345/hashtags", http.StatusOK, []string{"feature", "triage"})

	output, err := executeCommand(RootCmd, "pr", "edit", "12345", "--add-hashtag", "feature", "--add-hashtag", "triage", "--remove-hashtag", "legacy", "--host", server.URL)
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("POST", "/changes/12345/hashtags") != 1 {
		t.Errorf("Expected SetHashtags API to be called, got calls: %v", server.Requests())
	}
	if !strings.Contains(output, "Hashtags updated successfully.") {
		t.Errorf("Unexpected output: %s", output)
	}
}

func TestEdit_CQ_Default(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/12345/revisions/current/review", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "edit", "12345", "--cq", "--host", server.URL)
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if server.CallCount("POST", "/changes/12345/revisions/current/review") != 1 {
		t.Errorf("Expected review API to be called, got calls: %v", server.Requests())
	}
	if !strings.Contains(output, "Commit-Queue+1 set successfully.") {
		t.Errorf("Unexpected output: %s", output)
	}

	var payload gerrit.ReviewInput
	if req := server.LastRequest(); req != nil {
		json.Unmarshal(req.Body, &payload)
	}
	if payload.Labels["Commit-Queue"] != 1 {
		t.Errorf("got Commit-Queue = %d, want 1", payload.Labels["Commit-Queue"])
	}
}

func TestEdit_CQ_Explicit(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/12345/revisions/current/review", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "edit", "12345", "--cq", "2", "--host", server.URL)
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "Commit-Queue+2 set successfully.") {
		t.Errorf("Unexpected output: %s", output)
	}

	var payload gerrit.ReviewInput
	if req := server.LastRequest(); req != nil {
		json.Unmarshal(req.Body, &payload)
	}
	if payload.Labels["Commit-Queue"] != 2 {
		t.Errorf("got Commit-Queue = %d, want 2", payload.Labels["Commit-Queue"])
	}
}

func TestEdit_CQ_Remove(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/12345/revisions/current/review", http.StatusOK, map[string]any{})

	output, err := executeCommand(RootCmd, "pr", "edit", "12345", "--cq", "0", "--host", server.URL)
	if err != nil {
		t.Fatalf("Command failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "Commit-Queue vote removed successfully.") {
		t.Errorf("Unexpected output: %s", output)
	}

	var payload gerrit.ReviewInput
	if req := server.LastRequest(); req != nil {
		json.Unmarshal(req.Body, &payload)
	}
	if payload.Labels["Commit-Queue"] != 0 {
		t.Errorf("got Commit-Queue = %d, want 0", payload.Labels["Commit-Queue"])
	}
}
