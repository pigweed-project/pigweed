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
	"fmt"
	"io"
	"net/http"
	"strings"
	"testing"

	"github.com/andygrunwald/go-gerrit"
	"github.com/spf13/cobra"
)

func TestResolveChangeContext_ArgWithPatchset(t *testing.T) {
	server := NewMockGerritServer(t)

	cmd := &cobra.Command{}
	cmd.SetContext(context.Background())
	cmd.Flags().String("host", server.URL, "")

	chCtx, err := ResolveChangeContext(cmd, []string{"12345/3"})
	if err != nil {
		t.Fatalf("ResolveChangeContext failed: %v", err)
	}

	if chCtx.ChangeID != "12345" {
		t.Errorf("got ChangeID %q, want %q", chCtx.ChangeID, "12345")
	}
	if chCtx.Revision != "3" {
		t.Errorf("got Revision %q, want %q", chCtx.Revision, "3")
	}
	if chCtx.TargetID != "12345/3" {
		t.Errorf("got TargetID %q, want %q", chCtx.TargetID, "12345/3")
	}
	if chCtx.Client == nil {
		t.Fatal("expected non-nil Client")
	}
	if chCtx.Cmd != cmd {
		t.Errorf("expected Cmd to be preserved")
	}
}

func TestResolveChangeContext_ArgWithoutPatchset(t *testing.T) {
	server := NewMockGerritServer(t)

	cmd := &cobra.Command{}
	cmd.SetContext(context.Background())
	cmd.Flags().String("host", server.URL, "")

	chCtx, err := ResolveChangeContext(cmd, []string{"472267"})
	if err != nil {
		t.Fatalf("ResolveChangeContext failed: %v", err)
	}

	if chCtx.ChangeID != "472267" {
		t.Errorf("got ChangeID %q, want %q", chCtx.ChangeID, "472267")
	}
	if chCtx.Revision != "current" {
		t.Errorf("got Revision %q, want %q", chCtx.Revision, "current")
	}
	if chCtx.TargetID != "472267" {
		t.Errorf("got TargetID %q, want %q", chCtx.TargetID, "472267")
	}
}

func TestResolveChangeContext_FromGitBranch(t *testing.T) {
	server := NewMockGerritServer(t)

	mockGit := SetMockGit(t, &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) >= 2 && args[0] == "branch" && args[1] == "--show-current" {
				stdout.Write([]byte("my-feature\n"))
				return nil
			}
			if len(args) >= 3 && args[0] == "log" && args[1] == "-1" {
				stdout.Write([]byte("Commit\n\nChange-Id: Iabcdef1234567890abcdef1234567890abcdef12\n"))
				return nil
			}
			return fmt.Errorf("unhandled git command: %v", args)
		},
	})

	cmd := &cobra.Command{}
	cmd.SetContext(context.Background())
	cmd.Flags().String("host", server.URL, "")
	SetConfig(cmd, &Config{Git: mockGit, Host: server.URL})

	chCtx, err := ResolveChangeContext(cmd, nil)
	if err != nil {
		t.Fatalf("ResolveChangeContext failed: %v", err)
	}

	if chCtx.ChangeID != "Iabcdef1234567890abcdef1234567890abcdef12" {
		t.Errorf("got ChangeID %q, want %q", chCtx.ChangeID, "Iabcdef1234567890abcdef1234567890abcdef12")
	}
	if chCtx.Revision != "current" {
		t.Errorf("got Revision %q, want %q", chCtx.Revision, "current")
	}
}

func TestResolveChangeContext_NilCmd(t *testing.T) {
	_, err := ResolveChangeContext(nil, []string{"123"})
	if err == nil {
		t.Fatal("expected error for nil cmd, got nil")
	}
	if !strings.Contains(err.Error(), "cmd is nil") {
		t.Errorf("expected error to mention 'cmd is nil', got: %v", err)
	}
}

func TestResolveChangeContext_TargetResolutionError(t *testing.T) {
	mockGit := SetMockGit(t, &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			return fmt.Errorf("git failure")
		},
	})

	cmd := &cobra.Command{}
	cmd.SetContext(context.Background())
	SetConfig(cmd, &Config{Git: mockGit})

	_, err := ResolveChangeContext(cmd, nil)
	if err == nil {
		t.Fatal("expected error when change target cannot be resolved, got nil")
	}
}

func TestResolveChangeContext_ClientCreationError(t *testing.T) {
	origNewClient := NewGerritClient
	NewGerritClient = func(ctx context.Context, cmd *cobra.Command) (*gerrit.Client, error) {
		return nil, fmt.Errorf("simulated auth failure")
	}
	t.Cleanup(func() { NewGerritClient = origNewClient })

	cmd := &cobra.Command{}
	cmd.SetContext(context.Background())

	_, err := ResolveChangeContext(cmd, []string{"12345"})
	if err == nil {
		t.Fatal("expected error when client creation fails, got nil")
	}
	if !strings.Contains(err.Error(), "error creating Gerrit client") {
		t.Errorf("expected error to mention 'error creating Gerrit client', got: %v", err)
	}
}

func TestSetReviewSafe_Success(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/12345/revisions/current/review", http.StatusOK, map[string]any{
		"labels": map[string]int{"Code-Review": 2},
	})

	client, err := gerrit.NewClient(context.Background(), server.URL, nil)
	if err != nil {
		t.Fatalf("gerrit.NewClient failed: %v", err)
	}

	input := &gerrit.ReviewInput{
		Message: "LGTM",
		Labels:  map[string]int{"Code-Review": 2},
	}

	if err := SetReviewSafe(context.Background(), client, "12345", "current", input); err != nil {
		t.Fatalf("SetReviewSafe failed: %v", err)
	}

	if server.CallCount("POST", "/changes/12345/revisions/current/review") != 1 {
		t.Errorf("expected 1 call to SetReview, got %d", server.CallCount("POST", "/changes/12345/revisions/current/review"))
	}
}

func TestSetReviewSafe_LabelsUnmarshalErrorIgnored(t *testing.T) {
	// Simulate Gerrit returning a JSON shape for labels that triggers go-gerrit's UnmarshalTypeError.
	server := NewMockGerritServer(t)
	// Return labels as a string instead of map[string]int, triggering json.UnmarshalTypeError on "labels"
	server.OnJSON("POST", "/changes/12345/revisions/current/review", http.StatusOK, `{
		"labels": "malformed_labels_string"
	}`)

	client, err := gerrit.NewClient(context.Background(), server.URL, nil)
	if err != nil {
		t.Fatalf("gerrit.NewClient failed: %v", err)
	}

	input := &gerrit.ReviewInput{
		Message: "LGTM",
	}

	// SetReviewSafe should safely swallow this known go-gerrit bug
	if err := SetReviewSafe(context.Background(), client, "12345", "current", input); err != nil {
		t.Fatalf("expected SetReviewSafe to ignore labels unmarshal error, got: %v", err)
	}
}

func TestSetReviewSafe_OtherUnmarshalErrorPropagated(t *testing.T) {
	server := NewMockGerritServer(t)
	// Return ready as a string instead of boolean or something that triggers UnmarshalTypeError on non-labels
	server.OnJSON("POST", "/changes/12345/revisions/current/review", http.StatusOK, `{
		"ready": "not_a_bool"
	}`)

	client, err := gerrit.NewClient(context.Background(), server.URL, nil)
	if err != nil {
		t.Fatalf("gerrit.NewClient failed: %v", err)
	}

	input := &gerrit.ReviewInput{Message: "LGTM"}

	err = SetReviewSafe(context.Background(), client, "12345", "current", input)
	if err == nil {
		t.Fatal("expected unmarshal error on non-labels field to be returned, got nil")
	}
}

func TestSetReviewSafe_HTTPErrorPropagated(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnStatus(http.StatusForbidden)

	client, err := gerrit.NewClient(context.Background(), server.URL, nil)
	if err != nil {
		t.Fatalf("gerrit.NewClient failed: %v", err)
	}

	input := &gerrit.ReviewInput{Message: "LGTM"}

	err = SetReviewSafe(context.Background(), client, "12345", "current", input)
	if err == nil {
		t.Fatal("expected HTTP 403 error to be returned, got nil")
	}
}

func TestSetReviewSafe_InvariantChecks(t *testing.T) {
	ctx := context.Background()

	t.Run("nil client returns error", func(t *testing.T) {
		err := SetReviewSafe(ctx, nil, "12345", "current", &gerrit.ReviewInput{})
		if err == nil {
			t.Fatal("expected error for nil client, got nil")
		}
		if !strings.Contains(err.Error(), "client is nil") {
			t.Errorf("expected error to mention 'client is nil', got: %v", err)
		}
	})

	t.Run("empty changeID returns error", func(t *testing.T) {
		client, _ := gerrit.NewClient(ctx, "http://localhost", nil)
		err := SetReviewSafe(ctx, client, "", "current", &gerrit.ReviewInput{})
		if err == nil {
			t.Fatal("expected error for empty changeID, got nil")
		}
		if !strings.Contains(err.Error(), "changeID is empty") {
			t.Errorf("expected error to mention 'changeID is empty', got: %v", err)
		}
	})
}

func TestExtractFetchRef_ProtocolOrder(t *testing.T) {
	change := &gerrit.ChangeInfo{Number: 12345}

	t.Run("prefers http protocol", func(t *testing.T) {
		rev := gerrit.RevisionInfo{
			Fetch: map[string]gerrit.FetchInfo{
				"anonymous http": {Ref: "refs/changes/45/12345/anon"},
				"http":           {Ref: "refs/changes/45/12345/http"},
				"ssh":            {Ref: "refs/changes/45/12345/ssh"},
			},
		}
		ref, err := ExtractFetchRef(change, rev)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if ref != "refs/changes/45/12345/http" {
			t.Errorf("got ref %q, want %q", ref, "refs/changes/45/12345/http")
		}
	})

	t.Run("falls back to anonymous http", func(t *testing.T) {
		rev := gerrit.RevisionInfo{
			Fetch: map[string]gerrit.FetchInfo{
				"anonymous http": {Ref: "refs/changes/45/12345/anon"},
			},
		}
		ref, err := ExtractFetchRef(change, rev)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if ref != "refs/changes/45/12345/anon" {
			t.Errorf("got ref %q, want %q", ref, "refs/changes/45/12345/anon")
		}
	})

	t.Run("falls back to other protocol", func(t *testing.T) {
		rev := gerrit.RevisionInfo{
			Fetch: map[string]gerrit.FetchInfo{
				"ssh": {Ref: "refs/changes/45/12345/ssh"},
			},
		}
		ref, err := ExtractFetchRef(change, rev)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if ref != "refs/changes/45/12345/ssh" {
			t.Errorf("got ref %q, want %q", ref, "refs/changes/45/12345/ssh")
		}
	})

	t.Run("falls back to standard gerrit ref format", func(t *testing.T) {
		rev := gerrit.RevisionInfo{
			Number: 4,
			Fetch:  nil,
		}
		ref, err := ExtractFetchRef(change, rev)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if ref != "refs/changes/45/12345/4" {
			t.Errorf("got ref %q, want %q", ref, "refs/changes/45/12345/4")
		}
	})

	t.Run("error when no ref available", func(t *testing.T) {
		emptyChange := &gerrit.ChangeInfo{Number: 0}
		rev := gerrit.RevisionInfo{Number: 0}
		_, err := ExtractFetchRef(emptyChange, rev)
		if err == nil {
			t.Fatal("expected error when no ref could be determined, got nil")
		}
		if !strings.Contains(err.Error(), "fetch ref not found") {
			t.Errorf("expected error to mention 'fetch ref not found', got: %v", err)
		}
	})

	t.Run("nil change returns error", func(t *testing.T) {
		_, err := ExtractFetchRef(nil, gerrit.RevisionInfo{})
		if err == nil {
			t.Fatal("expected error for nil change, got nil")
		}
	})
}

func TestExtractRevision(t *testing.T) {
	change := &gerrit.ChangeInfo{
		ChangeID:        "I123",
		CurrentRevision: "rev2",
		Revisions: map[string]gerrit.RevisionInfo{
			"rev1": {Number: 1, Ref: "refs/changes/1"},
			"rev2": {Number: 2, Ref: "refs/changes/2"},
			"rev3": {Number: 3, Ref: "refs/changes/3"},
		},
	}

	t.Run("extracts current revision when empty", func(t *testing.T) {
		rev, err := ExtractRevision(change, "")
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if rev.Number != 2 {
			t.Errorf("got rev number %d, want 2", rev.Number)
		}
	})

	t.Run("extracts current revision when 'current'", func(t *testing.T) {
		rev, err := ExtractRevision(change, "current")
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if rev.Number != 2 {
			t.Errorf("got rev number %d, want 2", rev.Number)
		}
	})

	t.Run("extracts by patchset number", func(t *testing.T) {
		rev, err := ExtractRevision(change, "1")
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if rev.Number != 1 {
			t.Errorf("got rev number %d, want 1", rev.Number)
		}
	})

	t.Run("extracts by revision hash key", func(t *testing.T) {
		rev, err := ExtractRevision(change, "rev3")
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if rev.Number != 3 {
			t.Errorf("got rev number %d, want 3", rev.Number)
		}
	})

	t.Run("returns error when patchset not found", func(t *testing.T) {
		_, err := ExtractRevision(change, "99")
		if err == nil {
			t.Fatal("expected error when patchset not found, got nil")
		}
		if !strings.Contains(err.Error(), "revision 99 not found") {
			t.Errorf("expected error to mention 'revision 99 not found', got: %v", err)
		}
	})

	t.Run("returns error when current revision missing", func(t *testing.T) {
		brokenChange := &gerrit.ChangeInfo{
			CurrentRevision: "nonexistent",
			Revisions:       map[string]gerrit.RevisionInfo{},
		}
		_, err := ExtractRevision(brokenChange, "current")
		if err == nil {
			t.Fatal("expected error when current revision missing, got nil")
		}
	})

	t.Run("nil change returns error", func(t *testing.T) {
		_, err := ExtractRevision(nil, "current")
		if err == nil {
			t.Fatal("expected error for nil change, got nil")
		}
	})
}

func TestChangeContext_Methods(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345", http.StatusOK, `{
		"_number": 12345,
		"change_id": "I12345",
		"current_revision": "rev1",
		"revisions": {
			"rev1": {
				"_number": 1,
				"fetch": {
					"http": {"ref": "refs/changes/45/12345/1"}
				}
			}
		}
	}`)
	server.OnJSON("POST", "/changes/12345/revisions/rev1/review", http.StatusOK, map[string]any{})

	client, err := gerrit.NewClient(context.Background(), server.URL, nil)
	if err != nil {
		t.Fatalf("gerrit.NewClient failed: %v", err)
	}

	mockGit := &MockGitRunner{}
	cfg := &Config{Git: mockGit}

	cmd := &cobra.Command{}
	cmd.SetContext(context.Background())

	chCtx := &ChangeContext{
		Context:  context.Background(),
		Cmd:      cmd,
		Config:   cfg,
		Client:   client,
		TargetID: "12345",
		ChangeID: "12345",
		Revision: "rev1",
	}

	t.Run("GitClient success", func(t *testing.T) {
		git, err := chCtx.GitClient()
		if err != nil {
			t.Fatalf("GitClient failed: %v", err)
		}
		if git == nil {
			t.Fatal("expected non-nil GitClient")
		}
	})

	t.Run("GitClient error when uninitialized", func(t *testing.T) {
		emptyCtx := &ChangeContext{}
		_, err := emptyCtx.GitClient()
		if err == nil {
			t.Fatal("expected error when git uninitialized, got nil")
		}
	})

	t.Run("GetChange success", func(t *testing.T) {
		change, err := chCtx.GetChange(nil)
		if err != nil {
			t.Fatalf("GetChange failed: %v", err)
		}
		if change.Number != 12345 {
			t.Errorf("got change number %d, want 12345", change.Number)
		}
	})

	t.Run("SetReview success", func(t *testing.T) {
		err := chCtx.SetReview(&gerrit.ReviewInput{Message: "test"})
		if err != nil {
			t.Fatalf("SetReview failed: %v", err)
		}
	})

	t.Run("ExtractRevision and ExtractFetchRef", func(t *testing.T) {
		change, err := chCtx.GetChange(nil)
		if err != nil {
			t.Fatalf("GetChange failed: %v", err)
		}
		rev, err := chCtx.ExtractRevision(change)
		if err != nil {
			t.Fatalf("ExtractRevision failed: %v", err)
		}
		ref, err := chCtx.ExtractFetchRef(change, rev)
		if err != nil {
			t.Fatalf("ExtractFetchRef failed: %v", err)
		}
		if ref != "refs/changes/45/12345/1" {
			t.Errorf("got ref %q, want refs/changes/45/12345/1", ref)
		}
	})
}

func TestChangeContext_NilReceiverInvariants(t *testing.T) {
	var nilCtx *ChangeContext

	t.Run("GitClient nil receiver", func(t *testing.T) {
		_, err := nilCtx.GitClient()
		if err == nil || !strings.Contains(err.Error(), "ChangeContext is nil") {
			t.Errorf("expected 'ChangeContext is nil', got: %v", err)
		}
	})

	t.Run("GetChange nil receiver", func(t *testing.T) {
		_, err := nilCtx.GetChange(nil)
		if err == nil || !strings.Contains(err.Error(), "ChangeContext is nil") {
			t.Errorf("expected 'ChangeContext is nil', got: %v", err)
		}
	})

	t.Run("SetReview nil receiver", func(t *testing.T) {
		err := nilCtx.SetReview(&gerrit.ReviewInput{})
		if err == nil || !strings.Contains(err.Error(), "ChangeContext is nil") {
			t.Errorf("expected 'ChangeContext is nil', got: %v", err)
		}
	})

	t.Run("SetReviewRevision nil receiver", func(t *testing.T) {
		err := nilCtx.SetReviewRevision("current", &gerrit.ReviewInput{})
		if err == nil || !strings.Contains(err.Error(), "ChangeContext is nil") {
			t.Errorf("expected 'ChangeContext is nil', got: %v", err)
		}
	})

	t.Run("ExtractRevision nil receiver", func(t *testing.T) {
		_, err := nilCtx.ExtractRevision(&gerrit.ChangeInfo{})
		if err == nil || !strings.Contains(err.Error(), "ChangeContext is nil") {
			t.Errorf("expected 'ChangeContext is nil', got: %v", err)
		}
	})

	t.Run("ExtractFetchRef nil receiver", func(t *testing.T) {
		_, err := nilCtx.ExtractFetchRef(&gerrit.ChangeInfo{}, gerrit.RevisionInfo{})
		if err == nil || !strings.Contains(err.Error(), "ChangeContext is nil") {
			t.Errorf("expected 'ChangeContext is nil', got: %v", err)
		}
	})
}

func TestSetReviewSafe_AdditionalInvariants(t *testing.T) {
	client, _ := gerrit.NewClient(context.Background(), "http://localhost", nil)

	t.Run("nil ctx returns error", func(t *testing.T) {
		err := SetReviewSafe(nil, client, "123", "current", &gerrit.ReviewInput{})
		if err == nil || !strings.Contains(err.Error(), "context is nil") {
			t.Errorf("expected 'context is nil', got: %v", err)
		}
	})

	t.Run("nil review input returns error", func(t *testing.T) {
		err := SetReviewSafe(context.Background(), client, "123", "current", nil)
		if err == nil || !strings.Contains(err.Error(), "review input is nil") {
			t.Errorf("expected 'review input is nil', got: %v", err)
		}
	})
}

func TestExtractRevision_ShortSHAAndActionableError(t *testing.T) {
	change := &gerrit.ChangeInfo{
		Number:   12345,
		ChangeID: "I12345",
		Revisions: map[string]gerrit.RevisionInfo{
			"abcdef1234567890abcdef1234567890abcdef12": {Number: 1},
			"fedcba0987654321fedcba0987654321fedcba09": {Number: 2},
		},
	}

	t.Run("matches by 7-char short SHA prefix", func(t *testing.T) {
		rev, err := ExtractRevision(change, "abcdef1")
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if rev.Number != 1 {
			t.Errorf("got rev number %d, want 1", rev.Number)
		}
	})

	t.Run("actionable error message lists available patchsets", func(t *testing.T) {
		_, err := ExtractRevision(change, "99")
		if err == nil {
			t.Fatal("expected error, got nil")
		}
		if !strings.Contains(err.Error(), "available patchsets: 1, 2") {
			t.Errorf("expected error to list available patchsets, got: %v", err)
		}
	})
}

func TestExtractFetchRef_RevRefFallbackAndDeterminism(t *testing.T) {
	change := &gerrit.ChangeInfo{Number: 12345}

	t.Run("falls back to rev.Ref if fetch map empty", func(t *testing.T) {
		rev := gerrit.RevisionInfo{
			Ref: "refs/changes/45/12345/special",
		}
		ref, err := ExtractFetchRef(change, rev)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if ref != "refs/changes/45/12345/special" {
			t.Errorf("got %q, want %q", ref, "refs/changes/45/12345/special")
		}
	})

	t.Run("deterministic key order when non-standard protocols", func(t *testing.T) {
		rev := gerrit.RevisionInfo{
			Fetch: map[string]gerrit.FetchInfo{
				"zebra": {Ref: "refs/changes/zebra"},
				"alpha": {Ref: "refs/changes/alpha"},
			},
		}
		ref, err := ExtractFetchRef(change, rev)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if ref != "refs/changes/alpha" {
			t.Errorf("got %q, want %q (alphabetical first)", ref, "refs/changes/alpha")
		}
	})
}

func TestResolveChangeContext_CanceledContext(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	cmd := &cobra.Command{}
	cmd.SetContext(ctx)

	_, err := ResolveChangeContext(cmd, []string{"123"})
	if err == nil {
		t.Fatal("expected error for canceled context, got nil")
	}
	if !strings.Contains(err.Error(), "context canceled") {
		t.Errorf("expected error to mention 'context canceled', got: %v", err)
	}
}

func TestFormatGerritError_Auth401(t *testing.T) {
	rawErr := fmt.Errorf("API call to http://example.com failed: 401 Unauthorized")
	formatted := FormatGerritError(rawErr, "submitting review for", "12345", "https://pigweed-review.googlesource.com")
	if formatted == nil {
		t.Fatal("expected formatted error, got nil")
	}
	msg := formatted.Error()
	if !strings.Contains(msg, "authentication required") {
		t.Errorf("expected 'authentication required', got: %s", msg)
	}
	if !strings.Contains(msg, "https://pigweed-review.googlesource.com/new-password") {
		t.Errorf("expected new-password URL, got: %s", msg)
	}
	if !strings.Contains(msg, "GERRIT_TOKEN") {
		t.Errorf("expected GERRIT_TOKEN hint, got: %s", msg)
	}
	if !strings.Contains(msg, "gcert") {
		t.Errorf("expected gcert hint, got: %s", msg)
	}
}

func TestFormatGerritError_NotFound404(t *testing.T) {
	rawErr := fmt.Errorf("API call failed: 404 Not Found")
	formatted := FormatGerritError(rawErr, "getting", "99999", "https://pigweed-review.googlesource.com")
	if formatted == nil {
		t.Fatal("expected formatted error, got nil")
	}
	msg := formatted.Error()
	if !strings.Contains(msg, "not found") {
		t.Errorf("expected 'not found', got: %s", msg)
	}
	if !strings.Contains(msg, "gh pr list") {
		t.Errorf("expected 'gh pr list', got: %s", msg)
	}
}

func TestChangeContext_SetReview_AuthErrorFormatted(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/123/revisions/current/review", http.StatusUnauthorized, map[string]any{
		"message": "Authentication required",
	})

	cmd := &cobra.Command{}
	cmd.SetContext(context.Background())
	cmd.Flags().String("host", server.URL, "")

	chCtx, err := ResolveChangeContext(cmd, []string{"123"})
	if err != nil {
		t.Fatalf("ResolveChangeContext failed: %v", err)
	}

	rErr := chCtx.SetReview(&gerrit.ReviewInput{Message: "test"})
	if rErr == nil {
		t.Fatal("expected error on 401 Unauthorized, got nil")
	}
	if !strings.Contains(rErr.Error(), "authentication required") {
		t.Errorf("expected 'authentication required' in error, got: %v", rErr)
	}
	if !strings.Contains(rErr.Error(), "new-password") {
		t.Errorf("expected 'new-password' in error, got: %v", rErr)
	}
}

func TestFormatGerritError_Conflict409_Merged(t *testing.T) {
	rawErr := fmt.Errorf("API call failed: 409 Conflict: change is merged")
	formatted := FormatGerritError(rawErr, "closing", "12345", "https://pigweed-review.googlesource.com")
	if formatted == nil {
		t.Fatal("expected formatted error, got nil")
	}
	msg := formatted.Error()
	if !strings.Contains(msg, "already merged") {
		t.Errorf("expected 'already merged' in error, got: %s", msg)
	}
}

func TestFormatGerritError_Conflict409_Abandoned(t *testing.T) {
	rawErr := fmt.Errorf("API call failed: 409 Conflict: change is abandoned")
	formatted := FormatGerritError(rawErr, "closing", "12345", "https://pigweed-review.googlesource.com")
	if formatted == nil {
		t.Fatal("expected formatted error, got nil")
	}
	msg := formatted.Error()
	if !strings.Contains(msg, "already closed (abandoned)") {
		t.Errorf("expected 'already closed (abandoned)' in error, got: %s", msg)
	}
}

func TestFormatGerritError_Conflict409_Ready(t *testing.T) {
	rawErr := fmt.Errorf("API call failed: 409 Conflict: change is not work in progress")
	formatted := FormatGerritError(rawErr, "marking as ready", "12345", "https://pigweed-review.googlesource.com")
	if formatted == nil {
		t.Fatal("expected formatted error, got nil")
	}
	msg := formatted.Error()
	if !strings.Contains(msg, "already marked as ready for review") {
		t.Errorf("expected 'already marked as ready for review' in error, got: %s", msg)
	}
}

func TestFormatGerritError_Conflict409_Open(t *testing.T) {
	rawErr := fmt.Errorf("API call failed: 409 Conflict: change is new")
	formatted := FormatGerritError(rawErr, "restoring", "12345", "https://pigweed-review.googlesource.com")
	if formatted == nil {
		t.Fatal("expected formatted error, got nil")
	}
	msg := formatted.Error()
	if !strings.Contains(msg, "already open") {
		t.Errorf("expected 'already open' in error, got: %s", msg)
	}
}

func TestResolveProfile(t *testing.T) {
	ctx := context.Background()

	// 1. Detect from gerrit host
	p, err := ResolveProfile(ctx, nil, "pigweed-review.googlesource.com", "")
	if err != nil {
		t.Fatalf("ResolveProfile failed: %v", err)
	}
	if p.Name() != "pigweed" {
		t.Errorf("ResolveProfile by host = %q, want pigweed", p.Name())
	}

	// 2. Fallback to change project name
	p, err = ResolveProfile(ctx, nil, "custom-gerrit.example.com", "fuchsia/src")
	if err != nil {
		t.Fatalf("ResolveProfile failed: %v", err)
	}
	if p.Name() != "fuchsia" {
		t.Errorf("ResolveProfile by project = %q, want fuchsia", p.Name())
	}

	// 3. Fallback to generic
	p, err = ResolveProfile(ctx, nil, "custom-gerrit.example.com", "other/project")
	if err != nil {
		t.Fatalf("ResolveProfile failed: %v", err)
	}
	if p.Name() != "generic" {
		t.Errorf("ResolveProfile generic fallback = %q, want generic", p.Name())
	}

	// 4. ChangeContext.ResolveProfile with populated context
	client, _ := gerrit.NewClient(ctx, "https://pigweed-review.googlesource.com", nil)
	chCtx := &ChangeContext{
		Context:  ctx,
		Client:   client,
		ChangeID: "123",
	}
	p, err = chCtx.ResolveProfile("pigweed/pigweed")
	if err != nil {
		t.Fatalf("chCtx.ResolveProfile failed: %v", err)
	}
	if p.Name() != "pigweed" {
		t.Errorf("chCtx.ResolveProfile = %q, want pigweed", p.Name())
	}

	// 5. Nil ChangeContext and nil Client safety
	var nilCtx *ChangeContext
	p, err = nilCtx.ResolveProfile()
	if err != nil || p == nil {
		t.Fatalf("nil ChangeContext.ResolveProfile() failed: %v, p=%v", err, p)
	}

	emptyCtx := &ChangeContext{Context: ctx}
	p, err = emptyCtx.ResolveProfile("pigweed/pigweed")
	if err != nil || p == nil || p.Name() != "pigweed" {
		t.Fatalf("empty ChangeContext.ResolveProfile() = %v, %v; want pigweed", p, err)
	}
}

func TestResolveCIContext(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("CI Context Change"))
	server.OnSearchBuilds(
		FakeBuild("901", "pigweed-linux", "SUCCESS"),
	)

	cmd := &cobra.Command{}
	cmd.SetContext(context.Background())
	SetConfig(cmd, &Config{Host: server.URL})

	oldBB := buildbucketHost
	buildbucketHost = server.URL
	defer func() { buildbucketHost = oldBB }()

	ciCtx, err := ResolveCIContext(cmd, "12345")
	if err != nil {
		t.Fatalf("ResolveCIContext failed: %v", err)
	}
	if ciCtx.Change.Number != 12345 || ciCtx.PatchsetNum != 1 {
		t.Errorf("got change %d patchset %d, want 12345 / 1", ciCtx.Change.Number, ciCtx.PatchsetNum)
	}
	if len(ciCtx.Builds) != 1 || ciCtx.Builds[0].ID != "901" {
		t.Errorf("unexpected builds: %+v", ciCtx.Builds)
	}

	// Explicit revision in ChangeID (e.g. "12345/2")
	ciCtxRev, err := ResolveCIContext(cmd, "12345/2")
	if err != nil {
		t.Fatalf("ResolveCIContext(12345/2) failed: %v", err)
	}
	if ciCtxRev.PatchsetNum != 2 {
		t.Errorf("ResolveCIContext(12345/2) PatchsetNum = %d, want 2", ciCtxRev.PatchsetNum)
	}
}

func TestChangeContext_Mutations(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345)

	server.OnJSON("POST", "/a/changes/12345/wip", http.StatusOK, map[string]any{})
	server.OnJSON("POST", "/a/changes/12345/reviewers", http.StatusOK, map[string]any{})

	cmd := &cobra.Command{}
	cmd.SetContext(context.Background())
	SetConfig(cmd, &Config{Host: server.URL})

	chCtx, err := ResolveChangeContext(cmd, []string{"12345"})
	if err != nil {
		t.Fatalf("ResolveChangeContext failed: %v", err)
	}

	if err := chCtx.SetWorkInProgress("moving to draft"); err != nil {
		t.Errorf("SetWorkInProgress failed: %v", err)
	}
	if err := chCtx.AddCC("cc@google.com"); err != nil {
		t.Errorf("AddCC failed: %v", err)
	}
}
