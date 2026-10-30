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
	SetMockGerritClient(t, func(ctx context.Context, cmd *cobra.Command) (*GerritClient, error) {
		return nil, fmt.Errorf("simulated auth failure")
	})

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
	change := &ChangeInfo{Number: 12345}

	t.Run("prefers http protocol", func(t *testing.T) {
		rev := RevisionInfo{
			Fetch: map[string]FetchInfo{
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
		rev := RevisionInfo{
			Fetch: map[string]FetchInfo{
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
		rev := RevisionInfo{
			Fetch: map[string]FetchInfo{
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
		rev := RevisionInfo{
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
		emptyChange := &ChangeInfo{Number: 0}
		rev := RevisionInfo{Number: 0}
		_, err := ExtractFetchRef(emptyChange, rev)
		if err == nil {
			t.Fatal("expected error when no ref could be determined, got nil")
		}
		if !strings.Contains(err.Error(), "fetch ref not found") {
			t.Errorf("expected error to mention 'fetch ref not found', got: %v", err)
		}
	})

	t.Run("nil change returns error", func(t *testing.T) {
		_, err := ExtractFetchRef(nil, RevisionInfo{})
		if err == nil {
			t.Fatal("expected error for nil change, got nil")
		}
	})
}

func TestExtractRevision(t *testing.T) {
	change := &ChangeInfo{
		ChangeID:        "I123",
		CurrentRevision: "rev2",
		Revisions: map[string]RevisionInfo{
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
		brokenChange := &ChangeInfo{
			CurrentRevision: "nonexistent",
			Revisions:       map[string]RevisionInfo{},
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

	client := server.Client()

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
		change, err := chCtx.GetChange()
		if err != nil {
			t.Fatalf("GetChange failed: %v", err)
		}
		if change.Number != 12345 {
			t.Errorf("got change number %d, want 12345", change.Number)
		}
	})

	t.Run("SetReview success", func(t *testing.T) {
		err := chCtx.SetReview(&ReviewInput{Message: "test"})
		if err != nil {
			t.Fatalf("SetReview failed: %v", err)
		}
	})

	t.Run("ExtractRevision and ExtractFetchRef", func(t *testing.T) {
		change, err := chCtx.GetChange()
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
		_, err := nilCtx.GetChange()
		if err == nil || !strings.Contains(err.Error(), "ChangeContext is nil") {
			t.Errorf("expected 'ChangeContext is nil', got: %v", err)
		}
	})

	t.Run("SetReview nil receiver", func(t *testing.T) {
		err := nilCtx.SetReview(&ReviewInput{})
		if err == nil || !strings.Contains(err.Error(), "ChangeContext is nil") {
			t.Errorf("expected 'ChangeContext is nil', got: %v", err)
		}
	})

	t.Run("SetReviewRevision nil receiver", func(t *testing.T) {
		err := nilCtx.SetReviewRevision("current", &ReviewInput{})
		if err == nil || !strings.Contains(err.Error(), "ChangeContext is nil") {
			t.Errorf("expected 'ChangeContext is nil', got: %v", err)
		}
	})

	t.Run("ExtractRevision nil receiver", func(t *testing.T) {
		_, err := nilCtx.ExtractRevision(&ChangeInfo{})
		if err == nil || !strings.Contains(err.Error(), "ChangeContext is nil") {
			t.Errorf("expected 'ChangeContext is nil', got: %v", err)
		}
	})

	t.Run("ExtractFetchRef nil receiver", func(t *testing.T) {
		_, err := nilCtx.ExtractFetchRef(&ChangeInfo{}, RevisionInfo{})
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
	change := &ChangeInfo{
		Number:   12345,
		ChangeID: "I12345",
		Revisions: map[string]RevisionInfo{
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
	change := &ChangeInfo{Number: 12345}

	t.Run("falls back to rev.Ref if fetch map empty", func(t *testing.T) {
		rev := RevisionInfo{
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
		rev := RevisionInfo{
			Fetch: map[string]FetchInfo{
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

func TestFormatGerritError(t *testing.T) {
	const host = "https://pigweed-review.googlesource.com"
	for _, tc := range []struct {
		name         string
		err          error
		action       string
		changeID     string
		wantExitCode int
		wantContains []string
		wantAbsent   []string
	}{
		{
			name:         "HTTP 401 Unauthorized",
			err:          &GerritAPIError{StatusCode: http.StatusUnauthorized, Body: "Unauthorized"},
			action:       "submitting review for",
			changeID:     "12345",
			wantExitCode: ExitCodeAuth,
			wantContains: []string{"authentication required", "https://pigweed-review.googlesource.com/new-password", "GERRIT_TOKEN", "gcert"},
		},
		{
			name:         "HTTP 403 Forbidden",
			err:          &GerritAPIError{StatusCode: http.StatusForbidden, Body: "Forbidden"},
			action:       "submitting review for",
			changeID:     "12345",
			wantExitCode: ExitCodeAuth,
			wantContains: []string{"authentication required", "https://pigweed-review.googlesource.com/new-password", "GERRIT_TOKEN", "gcert"},
		},
		{
			name:         "HTTP 404 Not Found",
			err:          &GerritAPIError{StatusCode: http.StatusNotFound, Body: "Not found: 99999"},
			action:       "getting",
			changeID:     "99999",
			wantExitCode: ExitCodeFailure,
			wantContains: []string{"change not found", "gh pr list"},
		},
		{
			name:         "wrapped GerritAPIError 404",
			err:          fmt.Errorf("outer: %w", &GerritAPIError{StatusCode: http.StatusNotFound, Body: "Not found"}),
			action:       "getting",
			changeID:     "99999",
			wantExitCode: ExitCodeFailure,
			wantContains: []string{"change not found", "gh pr list", "outer:"},
		},
		{
			name:         "HTTP 409 Conflict merged",
			err:          &GerritAPIError{StatusCode: http.StatusConflict, Body: "change is merged"},
			action:       "closing",
			changeID:     "12345",
			wantExitCode: ExitCodeFailure,
			wantContains: []string{"already merged"},
		},
		{
			name:         "HTTP 409 Conflict abandoned",
			err:          &GerritAPIError{StatusCode: http.StatusConflict, Body: "change is abandoned"},
			action:       "closing",
			changeID:     "12345",
			wantExitCode: ExitCodeFailure,
			wantContains: []string{"already closed (abandoned)"},
		},
		{
			name:         "HTTP 409 Conflict ready",
			err:          &GerritAPIError{StatusCode: http.StatusConflict, Body: "change is not work in progress"},
			action:       "marking as ready",
			changeID:     "12345",
			wantExitCode: ExitCodeFailure,
			wantContains: []string{"already marked as ready for review"},
		},
		{
			name:         "HTTP 409 Conflict open",
			err:          &GerritAPIError{StatusCode: http.StatusConflict, Body: "change is new"},
			action:       "restoring",
			changeID:     "12345",
			wantExitCode: ExitCodeFailure,
			wantContains: []string{"already open"},
		},
		{
			name:         "plain non-API error mentioning 404 passes through",
			err:          errors.New("404 Not Found"),
			action:       "getting",
			changeID:     "12345",
			wantExitCode: ExitCodeFailure,
			wantContains: []string{"error getting change 12345: 404 Not Found"},
			wantAbsent:   []string{"change not found", "gh pr list"},
		},
		{
			name:         "plain non-API error mentioning 401 passes through without ExitCodeAuth",
			err:          errors.New("401 Unauthorized"),
			action:       "getting",
			changeID:     "12345",
			wantExitCode: ExitCodeFailure,
			wantContains: []string{"error getting change 12345: 401 Unauthorized"},
			wantAbsent:   []string{"authentication required", "GERRIT_TOKEN"},
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			formatted := FormatGerritError(tc.err, tc.action, tc.changeID, host)
			if formatted == nil {
				t.Fatal("expected formatted error, got nil")
			}
			if got := ExitCodeFor(formatted); got != tc.wantExitCode {
				t.Errorf("ExitCodeFor() = %d, want %d (err: %v)", got, tc.wantExitCode, formatted)
			}
			msg := formatted.Error()
			for _, want := range tc.wantContains {
				if !strings.Contains(msg, want) {
					t.Errorf("expected %q in error, got:\n%s", want, msg)
				}
			}
			for _, absent := range tc.wantAbsent {
				if strings.Contains(msg, absent) {
					t.Errorf("expected %q to be absent from error, got:\n%s", absent, msg)
				}
			}
		})
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

	rErr := chCtx.SetReview(&ReviewInput{Message: "test"})
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

func TestChangeContext_ResolveFetchRef(t *testing.T) {
	for _, tc := range []struct {
		name     string
		revision string
		wantOpt  string
		wantRef  string
	}{
		{
			name:     "empty revision requests CURRENT_REVISION",
			revision: "",
			wantOpt:  "CURRENT_REVISION",
			wantRef:  "refs/changes/45/12345/2",
		},
		{
			name:     "current revision requests CURRENT_REVISION",
			revision: "current",
			wantOpt:  "CURRENT_REVISION",
			wantRef:  "refs/changes/45/12345/2",
		},
		{
			name:     "specific patchset requests ALL_REVISIONS",
			revision: "1",
			wantOpt:  "ALL_REVISIONS",
			wantRef:  "refs/changes/45/12345/1",
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			server := NewMockGerritServer(t)
			server.OnDefaultChange(12345, func(ch map[string]any) {
				ch["current_revision"] = "rev2"
				ch["revisions"] = map[string]any{
					"rev1": map[string]any{"_number": 1, "ref": "refs/changes/45/12345/1"},
					"rev2": map[string]any{"_number": 2, "ref": "refs/changes/45/12345/2"},
				}
			})
			chCtx := &ChangeContext{
				Context:  t.Context(),
				Client:   server.Client(),
				ChangeID: "12345",
				Revision: tc.revision,
			}
			_, ref, err := chCtx.ResolveFetchRef()
			if err != nil {
				t.Fatalf("ResolveFetchRef() failed: %v", err)
			}
			if ref != tc.wantRef {
				t.Errorf("ResolveFetchRef() ref = %q, want %q", ref, tc.wantRef)
			}
			server.AssertOptions(t, "GET", "/changes/12345", tc.wantOpt)
		})
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
	client := NewGerritClientWithHTTP("https://pigweed-review.googlesource.com", nil, GerritAuth{})
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

	server.OnJSON("POST", "/changes/12345/wip", http.StatusOK, map[string]any{})
	server.OnJSON("POST", "/changes/12345/reviewers", http.StatusOK, map[string]any{})
	server.OnJSON("PUT", "/changes/12345/topic", http.StatusOK, `"my-topic"`)
	server.OnJSON("POST", "/changes/12345/abandon", http.StatusOK, map[string]any{"status": "ABANDONED"})

	cmd := &cobra.Command{}
	cmd.SetContext(context.Background())
	SetConfig(cmd, &Config{Host: server.URL})

	chCtx, err := ResolveChangeContext(cmd, []string{"12345"})
	if err != nil {
		t.Fatalf("ResolveChangeContext failed: %v", err)
	}

	if err := chCtx.SetWorkInProgress("moving to draft"); err != nil {
		t.Fatalf("SetWorkInProgress failed: %v", err)
	}
	if wip := DecodeRequest[messageInput](t, server.LastRequest()); wip.Message != "moving to draft" {
		t.Errorf("SetWorkInProgress body = %+v, want message=\"moving to draft\"", wip)
	}

	if err := chCtx.AddCC("cc@google.com"); err != nil {
		t.Fatalf("AddCC failed: %v", err)
	}
	if rev := DecodeRequest[reviewerInput](t, server.LastRequest()); rev.Reviewer != "cc@google.com" || rev.State != "CC" {
		t.Errorf("AddCC body = %+v, want reviewer=\"cc@google.com\" state=\"CC\"", rev)
	}

	if err := chCtx.Client.SetTopic(chCtx.Context, chCtx.ChangeID, "my-topic"); err != nil {
		t.Fatalf("SetTopic failed: %v", err)
	}
	if top := DecodeRequest[topicInput](t, server.LastRequest()); top.Topic != "my-topic" {
		t.Errorf("SetTopic body = %+v, want topic=\"my-topic\"", top)
	}

	if _, err := chCtx.Client.AbandonChange(chCtx.Context, chCtx.ChangeID, "no longer needed"); err != nil {
		t.Fatalf("AbandonChange failed: %v", err)
	}
	if ab := DecodeRequest[messageInput](t, server.LastRequest()); ab.Message != "no longer needed" {
		t.Errorf("AbandonChange body = %+v, want message=\"no longer needed\"", ab)
	}
}

func TestIsEquivalentRevisionKind(t *testing.T) {
	tests := []struct {
		kind string
		want bool
	}{
		{"REWORK", false},
		{"", false},
		{"UNKNOWN", false},
		{"TRIVIAL_REBASE", true},
		{"TRIVIAL_REBASE_WITH_MESSAGE_UPDATE", true},
		{"NO_CODE_CHANGE", true},
		{"NO_CHANGE", true},
		{"MERGE_FIRST_PARENT_UPDATE", true},
		{"  trivial_rebase  ", true},
	}
	for _, tc := range tests {
		if got := IsEquivalentRevisionKind(tc.kind); got != tc.want {
			t.Errorf("IsEquivalentRevisionKind(%q) = %v, want %v", tc.kind, got, tc.want)
		}
	}
}

func TestEquivalentPatchsets(t *testing.T) {
	// Patchset 10 is a trivial rebase, patchset 9 is a commit-message-only change (NO_CODE_CHANGE),
	// patchset 8 is a rework, and patchset 7 is a rework.
	change := &ChangeInfo{
		CurrentRevision: "rev10",
		Revisions: map[string]RevisionInfo{
			"rev7":  {Number: 7, Kind: "REWORK"},
			"rev8":  {Number: 8, Kind: "REWORK"},
			"rev9":  {Number: 9, Kind: "NO_CODE_CHANGE"},
			"rev10": {Number: 10, Kind: "TRIVIAL_REBASE"},
		},
	}

	assertIntsEqual := func(t *testing.T, got, want []int) {
		t.Helper()
		if len(got) != len(want) {
			t.Fatalf("got %v, want %v", got, want)
		}
		for i := range got {
			if got[i] != want[i] {
				t.Fatalf("got %v, want %v", got, want)
			}
		}
	}

	// Patchset 10 includes 10, 9 (NO_CODE_CHANGE), and 8 (REWORK), stopping before 7.
	assertIntsEqual(t, EquivalentPatchsets(change, 10), []int{10, 9, 8})

	// Patchset 9 includes 9 and 8, stopping before 7.
	assertIntsEqual(t, EquivalentPatchsets(change, 9), []int{9, 8})

	// Patchset 8 is a REWORK, so only patchset 8 is valid.
	assertIntsEqual(t, EquivalentPatchsets(change, 8), []int{8})

	// All non-rework kinds chain back to the preceding REWORK.
	allKindsChange := &ChangeInfo{
		Revisions: map[string]RevisionInfo{
			"r1": {Number: 1, Kind: "REWORK"},
			"r2": {Number: 2, Kind: "NO_CHANGE"},
			"r3": {Number: 3, Kind: "NO_CODE_CHANGE"},
			"r4": {Number: 4, Kind: "MERGE_FIRST_PARENT_UPDATE"},
			"r5": {Number: 5, Kind: "TRIVIAL_REBASE_WITH_MESSAGE_UPDATE"},
			"r6": {Number: 6, Kind: "TRIVIAL_REBASE"},
		},
	}
	assertIntsEqual(t, EquivalentPatchsets(allKindsChange, 6), []int{6, 5, 4, 3, 2, 1})

	// Edge cases
	if got := EquivalentPatchsets(change, 0); got != nil {
		t.Errorf("expected nil for targetPatchset 0, got %v", got)
	}
	assertIntsEqual(t, EquivalentPatchsets(nil, 5), []int{5})
	assertIntsEqual(t, EquivalentPatchsets(change, 99), []int{99})
}

func TestIsLoopbackHost(t *testing.T) {
	tests := []struct {
		host string
		want bool
	}{
		{"localhost", true},
		{"127.0.0.1", true},
		{"::1", true},
		{"[::1]", true},
		{"localhost:8080", true},
		{"127.0.0.1:8080", true},
		{"http://localhost", true},
		{"http://127.0.0.1:8080", true},
		{"https://[::1]:9090", true},
		{"", false},
		{"localhost.example.com", false},
		{"127.0.0.10", false},
		{"pigweed-review.googlesource.com", false},
	}
	for _, tc := range tests {
		if got := isLoopbackHost(tc.host); got != tc.want {
			t.Errorf("isLoopbackHost(%q) = %v, want %v", tc.host, got, tc.want)
		}
	}
}

func TestResolveChangeContext_PreservesTargetHost(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithProjectName("fuchsia"))

	// Even when cfg.Host is already populated from the local checkout, an explicit
	// URL or shortlink argument should override it when --host was not passed.
	cmd := &cobra.Command{}
	cmd.SetContext(context.Background())
	SetConfig(cmd, &Config{Host: "pigweed-review.googlesource.com"})

	chCtx, err := ResolveChangeContext(cmd, []string{"https://fuchsia-review.googlesource.com/c/fuchsia/+/12345/2"})
	if err != nil {
		t.Fatalf("ResolveChangeContext failed: %v", err)
	}
	if chCtx.Host != "fuchsia-review.googlesource.com" {
		t.Errorf("chCtx.Host = %q, want fuchsia-review.googlesource.com", chCtx.Host)
	}
	if chCtx.Project != "fuchsia" {
		t.Errorf("chCtx.Project = %q, want fuchsia", chCtx.Project)
	}
	if gotHost := chCtx.Config.GerritHost(cmd.Context()); gotHost != "fuchsia-review.googlesource.com" {
		t.Errorf("chCtx.Config.GerritHost() = %q, want fuchsia-review.googlesource.com", gotHost)
	}
}

func TestResolveCIContext_CanonicalHostAndEquivalentPatchsets(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345", http.StatusOK, map[string]any{
		"id":               "foo~main~I12345",
		"project":          "foo",
		"branch":           "main",
		"change_id":        "I12345",
		"subject":          "Merged CL With Trivial Rebase",
		"status":           "MERGED",
		"_number":          12345,
		"current_revision": "rev4",
		"revisions": map[string]any{
			"rev3": map[string]any{"_number": 3, "kind": "REWORK"},
			"rev4": map[string]any{"_number": 4, "kind": "TRIVIAL_REBASE"},
		},
	})

	server.On("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		var req struct {
			Predicate struct {
				GerritChanges []struct {
					Host     string `json:"host"`
					Change   int    `json:"change"`
					Patchset int    `json:"patchset"`
				} `json:"gerritChanges"`
			} `json:"predicate"`
		}
		_ = json.Unmarshal(body, &req)
		ps := 0
		if len(req.Predicate.GerritChanges) > 0 {
			if gotHost := req.Predicate.GerritChanges[0].Host; gotHost != "foo-internal-review.googlesource.com" {
				t.Errorf("SearchBuilds queried non-canonical host %q, want foo-internal-review.googlesource.com", gotHost)
			}
			ps = req.Predicate.GerritChanges[0].Patchset
		}

		w.Header().Set("Content-Type", "application/json")
		if ps == 4 {
			w.Write([]byte(")]}'\n{}\n"))
			return
		}
		if ps == 3 {
			w.Write([]byte(")]}'\n" + `{"builds":[{"id":"9001","builder":{"project":"pigweed","bucket":"pigweed.try","builder":"pw-presubmit"},"status":"SUCCESS"}]}`))
			return
		}
		w.Write([]byte(")]}'\n{}\n"))
	})

	cmd := &cobra.Command{}
	cmd.SetContext(context.Background())
	SetConfig(cmd, &Config{Host: "foo-internal-review" + gobCorpDomainSuffix})

	oldBB := buildbucketHost
	buildbucketHost = server.URL
	defer func() { buildbucketHost = oldBB }()

	ciCtx, err := ResolveCIContext(cmd, "12345")
	if err != nil {
		t.Fatalf("ResolveCIContext failed: %v", err)
	}
	if ciCtx.GerritHost != "foo-internal-review.googlesource.com" {
		t.Errorf("ciCtx.GerritHost = %q, want foo-internal-review.googlesource.com", ciCtx.GerritHost)
	}
	if len(ciCtx.EquivalentPatchsets) != 2 || ciCtx.EquivalentPatchsets[0] != 4 || ciCtx.EquivalentPatchsets[1] != 3 {
		t.Errorf("ciCtx.EquivalentPatchsets = %v, want [4 3]", ciCtx.EquivalentPatchsets)
	}
	if len(ciCtx.Builds) != 1 || ciCtx.Builds[0].ID != "9001" || ciCtx.Builds[0].Patchset != 3 {
		t.Errorf("expected 1 build from patchset 3, got %+v", ciCtx.Builds)
	}
}
