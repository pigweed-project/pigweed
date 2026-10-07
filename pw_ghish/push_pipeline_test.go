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
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"path/filepath"
	"strings"
	"testing"

	"github.com/andygrunwald/go-gerrit"
	"github.com/spf13/cobra"
)

func TestAddAndParseCommonPushFlags(t *testing.T) {
	cmd := &cobra.Command{Use: "test"}
	AddCommonPushFlags(cmd)
	cmd.Flags().Bool("ready", false, "Ready flag")

	args := []string{
		"--reviewer", "alice@google.com",
		"-r", "bob@google.com",
		"--cc", "carol@google.com",
		"--draft",
		"--ready",
		"--auto-submit",
		"--cq", "2",
		"--publish",
		"--push-option", "uploadvalidator~skip",
		"--no-verify",
		"--base", "feature-branch",
		"--stack",
	}
	if err := cmd.ParseFlags(NormalizeCQArgs(args)); err != nil {
		t.Fatalf("ParseFlags failed: %v", err)
	}

	flags := ParseCommonPushFlags(cmd)

	if flags.Base != "feature-branch" {
		t.Errorf("Base = %q, want feature-branch", flags.Base)
	}
	if !flags.Stack {
		t.Errorf("Stack = false, want true")
	}
	if !flags.NoVerify {
		t.Errorf("NoVerify = false, want true")
	}
	if len(flags.PushOptions.Reviewers) != 2 || flags.PushOptions.Reviewers[0] != "alice@google.com" || flags.PushOptions.Reviewers[1] != "bob@google.com" {
		t.Errorf("Reviewers = %v, want [alice@google.com bob@google.com]", flags.PushOptions.Reviewers)
	}
	if len(flags.PushOptions.CC) != 1 || flags.PushOptions.CC[0] != "carol@google.com" {
		t.Errorf("CC = %v, want [carol@google.com]", flags.PushOptions.CC)
	}
	if !flags.PushOptions.Draft {
		t.Errorf("Draft = false, want true")
	}
	if !flags.PushOptions.Ready {
		t.Errorf("Ready = false, want true")
	}
	if !flags.PushOptions.AutoSubmit {
		t.Errorf("AutoSubmit = false, want true")
	}
	if flags.PushOptions.CQ != 2 {
		t.Errorf("CQ = %d, want 2", flags.PushOptions.CQ)
	}
	if !flags.PushOptions.Publish {
		t.Errorf("Publish = false, want true")
	}
	if len(flags.PushOptions.ExtraOptions) != 1 || flags.PushOptions.ExtraOptions[0] != "uploadvalidator~skip" {
		t.Errorf("ExtraOptions = %v, want [uploadvalidator~skip]", flags.PushOptions.ExtraOptions)
	}

	cmdBare := &cobra.Command{Use: "test"}
	AddCommonPushFlags(cmdBare)
	if err := cmdBare.ParseFlags([]string{"--cq"}); err != nil {
		t.Fatalf("ParseFlags failed for bare --cq: %v", err)
	}
	flagsBare := ParseCommonPushFlags(cmdBare)
	if flagsBare.PushOptions.CQ != 1 {
		t.Errorf("bare --cq CQ = %d, want 1", flagsBare.PushOptions.CQ)
	}
}

func TestValidateCommitStack(t *testing.T) {
	ctx := context.Background()

	t.Run("single commit passes", func(t *testing.T) {
		mockGit := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				if len(args) >= 2 && args[0] == "rev-list" && args[1] == "--count" {
					stdout.Write([]byte("1\n"))
				}
				return nil
			},
		}
		client := NewGitClient(mockGit)
		err := ValidateCommitStack(ctx, client, "main", false, "create")
		if err != nil {
			t.Errorf("unexpected error: %v", err)
		}
	})

	t.Run("zero commits errors in create", func(t *testing.T) {
		mockGit := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				if len(args) >= 2 && args[0] == "rev-list" && args[1] == "--count" {
					stdout.Write([]byte("0\n"))
				}
				return nil
			},
		}
		client := NewGitClient(mockGit)
		err := ValidateCommitStack(ctx, client, "main", false, "create")
		if err == nil {
			t.Fatal("expected error when 0 commits ahead in create, got nil")
		}
		if !strings.Contains(err.Error(), "HEAD has 0 commits ahead") || !strings.Contains(err.Error(), "git commit") {
			t.Errorf("unexpected error message: %v", err)
		}
	})

	t.Run("multiple commits without stack errors in create", func(t *testing.T) {
		mockGit := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				if len(args) >= 2 && args[0] == "rev-list" && args[1] == "--count" {
					stdout.Write([]byte("3\n"))
				}
				return nil
			},
		}
		client := NewGitClient(mockGit)
		err := ValidateCommitStack(ctx, client, "main", false, "create")
		if err == nil {
			t.Fatal("expected error, got nil")
		}
		if !strings.Contains(err.Error(), "gh pr create --base") || !strings.Contains(err.Error(), "--stack") {
			t.Errorf("unexpected error message: %v", err)
		}
	})

	t.Run("multiple commits without stack errors in push", func(t *testing.T) {
		mockGit := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				if len(args) >= 2 && args[0] == "rev-list" && args[1] == "--count" {
					stdout.Write([]byte("3\n"))
				}
				return nil
			},
		}
		client := NewGitClient(mockGit)
		err := ValidateCommitStack(ctx, client, "main", false, "push")
		if err == nil {
			t.Fatal("expected error, got nil")
		}
		if !strings.Contains(err.Error(), "gh pr push --base") || !strings.Contains(err.Error(), "--stack") {
			t.Errorf("unexpected error message: %v", err)
		}
	})

	t.Run("multiple commits with stack passes", func(t *testing.T) {
		mockGit := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				if len(args) >= 2 && args[0] == "rev-list" && args[1] == "--count" {
					stdout.Write([]byte("3\n"))
				}
				return nil
			},
		}
		client := NewGitClient(mockGit)
		err := ValidateCommitStack(ctx, client, "main", true, "create")
		if err != nil {
			t.Errorf("unexpected error with stack=true: %v", err)
		}
	})

	t.Run("canceled context fails immediately", func(t *testing.T) {
		cancCtx, cancel := context.WithCancel(ctx)
		cancel()
		client := NewGitClient(&MockGitRunner{})
		err := ValidateCommitStack(cancCtx, client, "main", false, "create")
		if err == nil {
			t.Fatal("expected error for canceled context, got nil")
		}
	})

	t.Run("rejects unsquashed fixup commit in stack", func(t *testing.T) {
		id1 := "I1111111111111111111111111111111111111111"
		stackLog := fmt.Sprintf("aaa1111\x00fixup! pw_ghish: Base\x00fixup! pw_ghish: Base\n\nChange-Id: %s\n\x1e", id1)
		mockGit := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				if len(args) >= 2 && args[0] == "rev-list" && args[1] == "--count" {
					stdout.Write([]byte("1\n"))
				}
				if len(args) >= 2 && args[0] == "log" && args[1] == "--reverse" {
					stdout.Write([]byte(stackLog))
				}
				return nil
			},
		}
		client := NewGitClient(mockGit)
		err := ValidateCommitStack(ctx, client, "main", true, "push")
		if err == nil || !strings.Contains(err.Error(), "unsquashed fixup/squash commit") {
			t.Fatalf("expected unsquashed fixup error, got: %v", err)
		}
	})

	t.Run("rejects duplicate Change-Id in stack", func(t *testing.T) {
		id1 := "I1111111111111111111111111111111111111111"
		stackLog := fmt.Sprintf(
			"aaa1111\x00pw_ghish: First\x00pw_ghish: First\n\nChange-Id: %s\n\x1e"+
				"bbb2222\x00pw_ghish: Second\x00pw_ghish: Second\n\nChange-Id: %s\n\x1e",
			id1, id1,
		)
		mockGit := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				if len(args) >= 2 && args[0] == "rev-list" && args[1] == "--count" {
					stdout.Write([]byte("2\n"))
				}
				if len(args) >= 2 && args[0] == "log" && args[1] == "--reverse" {
					stdout.Write([]byte(stackLog))
				}
				return nil
			},
		}
		client := NewGitClient(mockGit)
		err := ValidateCommitStack(ctx, client, "main", true, "push")
		if err == nil || !strings.Contains(err.Error(), "share the same Change-Id") {
			t.Fatalf("expected duplicate Change-Id error, got: %v", err)
		}
	})

	t.Run("rejects commit with multiple Change-Id footers in stack", func(t *testing.T) {
		id1 := "I1111111111111111111111111111111111111111"
		id2 := "I2222222222222222222222222222222222222222"
		stackLog := fmt.Sprintf(
			"aaa1111\x00pw_ghish: Squashed\x00pw_ghish: Squashed\n\nChange-Id: %s\nChange-Id: %s\n\x1e",
			id1, id2,
		)
		mockGit := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				if len(args) >= 2 && args[0] == "rev-list" && args[1] == "--count" {
					stdout.Write([]byte("1\n"))
				}
				if len(args) >= 2 && args[0] == "log" && args[1] == "--reverse" {
					stdout.Write([]byte(stackLog))
				}
				return nil
			},
		}
		client := NewGitClient(mockGit)
		err := ValidateCommitStack(ctx, client, "main", true, "push")
		if err == nil || !strings.Contains(err.Error(), "multiple Change-Id footers") {
			t.Fatalf("expected multiple Change-Id footers error, got: %v", err)
		}
	})
}

func TestCheckCommitMessageWarnings(t *testing.T) {
	cleanMsg := "pw_ghish: Short subject\n\nWrapped body line within 72 characters.\n\n  Indented code line that is intentionally much longer than seventy-two characters for an example.\n\nBug: https://issues.pigweed.dev/issues/123456789012345678901234567890\nChange-Id: I1111111111111111111111111111111111111111\n"
	if warnings := CheckCommitMessageWarnings(cleanMsg, "HEAD commit"); len(warnings) != 0 {
		t.Errorf("expected 0 warnings for clean message, got: %v", warnings)
	}

	longMsg := "pw_ghish: This subject line is intentionally much longer than seventy-two characters in length\n\nThis prose body line is also intentionally much longer than seventy-two characters without wrapping.\n\nChange-Id: I1111111111111111111111111111111111111111\n"
	warnings := CheckCommitMessageWarnings(longMsg, "HEAD commit")
	if len(warnings) != 2 {
		t.Fatalf("expected 2 warnings (subject + body), got %d: %v", len(warnings), warnings)
	}
	if !strings.Contains(warnings[0], "preserving Change-Id:") {
		t.Errorf("expected subject warning to mention preserving Change-Id:, got: %q", warnings[0])
	}
	if !strings.Contains(warnings[1], "preserving Change-Id:") {
		t.Errorf("expected body warning to mention preserving Change-Id:, got: %q", warnings[1])
	}
}

func TestExecutePush(t *testing.T) {
	ctx := context.Background()
	var calls []string
	mockGit := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			calls = append(calls, strings.Join(args, " "))
			return nil
		},
	}
	cfg := &Config{
		Git:     mockGit,
		Profile: profiles["pigweed"],
	}

	cmd := &cobra.Command{}
	var outBuf, errBuf bytes.Buffer
	cmd.SetOut(&outBuf)
	cmd.SetErr(&errBuf)
	SetConfig(cmd, cfg)

	pushOpts := PushOptions{
		Reviewers: []string{"rev@google.com"},
		// The label is resolved from the host by the caller; executePush
		// votes what it is given and invents nothing.
		AutoSubmit:      true,
		AutoSubmitLabel: LabelVote{Name: "Pigweed-Auto-Submit", Value: 1},
	}

	err := executePush(ctx, cmd, cfg, "main", pushOpts, true)
	if err != nil {
		t.Fatalf("executePush failed: %v", err)
	}

	if len(calls) != 1 {
		t.Fatalf("expected 1 call, got %d: %v", len(calls), calls)
	}
	expected := "push --no-verify origin HEAD:refs/for/main%r=rev@google.com,l=Pigweed-Auto-Submit+1"
	if calls[0] != expected {
		t.Errorf("executePush call = %q, want %q", calls[0], expected)
	}
}

func TestExecutePush_MissingChangeIdInStderr(t *testing.T) {
	ctx := context.Background()
	mockGit := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) > 0 && args[0] == "push" {
				stderr.Write([]byte("remote: ERROR: commit 1234567: missing Change-Id in commit message footer\n"))
				return fmt.Errorf("exit status 1")
			}
			return nil
		},
	}
	cfg := &Config{
		Git:     mockGit,
		Host:    "pigweed-review.googlesource.com",
		Profile: profiles["pigweed"],
	}

	cmd := &cobra.Command{}
	var outBuf, errBuf bytes.Buffer
	cmd.SetOut(&outBuf)
	cmd.SetErr(&errBuf)
	SetConfig(cmd, cfg)

	err := executePush(ctx, cmd, cfg, "main", PushOptions{}, false)
	if err == nil {
		t.Fatal("expected error on git push rejection, got nil")
	}
	if !strings.Contains(err.Error(), "missing a Change-Id in its footer") {
		t.Errorf("expected Change-Id explanation in error, got: %v", err)
	}
	if !strings.Contains(err.Error(), "commit-msg") {
		t.Errorf("expected commit-msg hook instructions in error, got: %v", err)
	}
	if !strings.Contains(err.Error(), "git commit --amend --no-edit") {
		t.Errorf("expected amend instructions in error, got: %v", err)
	}
}

func TestExecutePush_WithTopicAndHashtags(t *testing.T) {
	ctx := context.Background()
	var calls []string
	mockGit := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			calls = append(calls, strings.Join(args, " "))
			return nil
		},
	}
	cfg := &Config{
		Git:     mockGit,
		Profile: profiles["pigweed"],
	}

	cmd := &cobra.Command{}
	var outBuf, errBuf bytes.Buffer
	cmd.SetOut(&outBuf)
	cmd.SetErr(&errBuf)
	SetConfig(cmd, cfg)

	pushOpts := PushOptions{
		Topic:    "kernel-sync",
		Hashtags: []string{"kernel", "arm64"},
	}

	err := executePush(ctx, cmd, cfg, "main", pushOpts, false)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if len(calls) != 1 {
		t.Fatalf("expected 1 call, got %d", len(calls))
	}

	expected := "push origin HEAD:refs/for/main%topic=kernel-sync,t=kernel,t=arm64"
	if calls[0] != expected {
		t.Errorf("executePush call = %q, want %q", calls[0], expected)
	}
}

func TestExecutePush_NoNewChanges_FallbackREST(t *testing.T) {
	ctx := context.Background()
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/I1234567890123456789012345678901234567890/revisions/current/review", http.StatusOK, map[string]any{})
	SetMockGerritClient(t, func(ctx context.Context, cmd *cobra.Command) (*gerrit.Client, error) {
		return gerrit.NewClient(ctx, server.URL, http.DefaultClient)
	})

	mockGit := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) > 0 && args[0] == "push" {
				stderr.Write([]byte("To sso://pigweed/pigweed\n ! [remote rejected] HEAD -> refs/for/main%l=Commit-Queue+1 (no new changes)\nerror: failed to push some refs\n"))
				return fmt.Errorf("exit status 1")
			}
			if len(args) > 0 && args[0] == "log" {
				stdout.Write([]byte("Commit subject\n\nChange-Id: I1234567890123456789012345678901234567890\n"))
				return nil
			}
			return nil
		},
	}
	cfg := &Config{
		Git:     mockGit,
		Host:    server.URL,
		Profile: profiles["pigweed"],
	}

	cmd := &cobra.Command{}
	var outBuf, errBuf bytes.Buffer
	cmd.SetOut(&outBuf)
	cmd.SetErr(&errBuf)
	cmd.SetContext(ctx)
	SetConfig(cmd, cfg)

	pushOpts := PushOptions{
		CQ: 1,
	}

	err := executePush(ctx, cmd, cfg, "main", pushOpts, false)
	if err != nil {
		t.Fatalf("unexpected error, should have fallen back to REST: %v", err)
	}

	if !strings.Contains(outBuf.String(), "applied metadata updates to Change") {
		t.Errorf("expected output to mention applied metadata updates, got: %s", outBuf.String())
	}
	if !strings.Contains(outBuf.String(), "Commit-Queue+1 set successfully") {
		t.Errorf("expected output to mention Commit-Queue+1, got: %s", outBuf.String())
	}

	if server.CallCount("POST", "/changes/I1234567890123456789012345678901234567890/revisions/current/review") != 1 {
		t.Errorf("expected review API to be called on Gerrit server, got calls: %v", server.Requests())
	}
}

// --auto with nothing to push still has to land on the label this host names,
// which means asking the change rather than replaying the profile default.
func TestExecutePush_NoNewChanges_FallbackREST_AutoSubmit(t *testing.T) {
	ctx := context.Background()
	changeID := "I1234567890123456789012345678901234567890"
	server := NewMockGerritServer(t)
	server.OnDefaultChange(changeID, WithLabels("Code-Review", "Auto-Submit"))
	server.OnJSON("POST", "/changes/"+changeID+"/revisions/current/review", http.StatusOK, map[string]any{})
	SetMockGerritClient(t, func(ctx context.Context, cmd *cobra.Command) (*gerrit.Client, error) {
		return gerrit.NewClient(ctx, server.URL, http.DefaultClient)
	})

	mockGit := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) > 0 && args[0] == "push" {
				stderr.Write([]byte("To sso://pigweed/pigweed\n ! [remote rejected] HEAD -> refs/for/main (no new changes)\nerror: failed to push some refs\n"))
				return fmt.Errorf("exit status 1")
			}
			if len(args) > 0 && args[0] == "log" {
				stdout.Write([]byte("Commit subject\n\nChange-Id: " + changeID + "\n"))
				return nil
			}
			return nil
		},
	}
	cfg := &Config{
		Git:     mockGit,
		Host:    server.URL,
		Profile: profiles["pigweed"],
	}

	cmd := &cobra.Command{}
	var outBuf, errBuf bytes.Buffer
	cmd.SetOut(&outBuf)
	cmd.SetErr(&errBuf)
	cmd.SetContext(ctx)
	SetConfig(cmd, cfg)

	if err := executePush(ctx, cmd, cfg, "main", PushOptions{AutoSubmit: true}, false); err != nil {
		t.Fatalf("unexpected error, should have fallen back to REST: %v", err)
	}

	var capturedInput gerrit.ReviewInput
	for _, req := range server.Requests() {
		if req.Method == "POST" && strings.HasSuffix(req.Path, "/review") {
			json.Unmarshal(req.Body, &capturedInput)
		}
	}
	if capturedInput.Labels["Auto-Submit"] != 1 {
		t.Errorf("Labels got %v, want Auto-Submit=1", capturedInput.Labels)
	}
	if _, voted := capturedInput.Labels["Pigweed-Auto-Submit"]; voted {
		t.Errorf("Labels got %v, want no vote on the profile's label", capturedInput.Labels)
	}
	if !strings.Contains(outBuf.String(), "Auto-submit enabled (Auto-Submit+1)") {
		t.Errorf("expected output to name the label voted, got: %s", outBuf.String())
	}
}

// The metadata-only path applies what it can and then says --auto could not be
// honored, rather than reporting auto-submit that is not going to happen.
func TestExecutePush_NoNewChanges_FallbackREST_AutoSubmitUnsupported(t *testing.T) {
	ctx := context.Background()
	changeID := "I1234567890123456789012345678901234567890"
	server := NewMockGerritServer(t)
	server.OnDefaultChange(changeID, WithLabels("Code-Review", "Commit-Queue"))
	server.OnJSON("POST", "/changes/"+changeID+"/revisions/current/review", http.StatusOK, map[string]any{})
	SetMockGerritClient(t, func(ctx context.Context, cmd *cobra.Command) (*gerrit.Client, error) {
		return gerrit.NewClient(ctx, server.URL, http.DefaultClient)
	})

	mockGit := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) > 0 && args[0] == "push" {
				stderr.Write([]byte("To sso://pigweed/pigweed\n ! [remote rejected] HEAD -> refs/for/main (no new changes)\nerror: failed to push some refs\n"))
				return fmt.Errorf("exit status 1")
			}
			if len(args) > 0 && args[0] == "log" {
				stdout.Write([]byte("Commit subject\n\nChange-Id: " + changeID + "\n"))
				return nil
			}
			return nil
		},
	}
	cfg := &Config{
		Git:     mockGit,
		Host:    server.URL,
		Profile: profiles["pigweed"],
	}

	cmd := &cobra.Command{}
	var outBuf, errBuf bytes.Buffer
	cmd.SetOut(&outBuf)
	cmd.SetErr(&errBuf)
	cmd.SetContext(ctx)
	SetConfig(cmd, cfg)

	err := executePush(ctx, cmd, cfg, "main", PushOptions{AutoSubmit: true}, false)
	if err == nil {
		t.Fatal("expected an error for a host with no auto-submit label")
	}
	if !strings.Contains(err.Error(), "has no auto-submit label") {
		t.Errorf("expected the error to name the problem, got: %v", err)
	}

	var capturedInput gerrit.ReviewInput
	for _, req := range server.Requests() {
		if req.Method == "POST" && strings.HasSuffix(req.Path, "/review") {
			json.Unmarshal(req.Body, &capturedInput)
		}
	}
	if capturedInput.Labels["Commit-Queue"] != 1 {
		t.Errorf("Labels got %v, want the Commit-Queue dry run", capturedInput.Labels)
	}
	if strings.Contains(outBuf.String(), "Auto-submit enabled") {
		t.Errorf("output claims auto-submit was enabled when it was not: %s", outBuf.String())
	}
}

func TestExecutePush_NoNewChanges_NoOptions(t *testing.T) {
	ctx := context.Background()
	mockGit := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) > 0 && args[0] == "push" {
				stderr.Write([]byte("To sso://pigweed/pigweed\n ! [remote rejected] HEAD -> refs/for/main (no new changes)\nerror: failed to push some refs\n"))
				return fmt.Errorf("exit status 1")
			}
			return nil
		},
	}
	cfg := &Config{
		Git:     mockGit,
		Host:    "pigweed-review.googlesource.com",
		Profile: profiles["pigweed"],
	}

	cmd := &cobra.Command{}
	var outBuf, errBuf bytes.Buffer
	cmd.SetOut(&outBuf)
	cmd.SetErr(&errBuf)
	SetConfig(cmd, cfg)

	err := executePush(ctx, cmd, cfg, "main", PushOptions{}, false)
	if err == nil {
		t.Fatal("expected error, got nil")
	}

	if !strings.Contains(err.Error(), "no new changes to push") {
		t.Errorf("expected error to mention 'no new changes to push', got: %v", err)
	}
	if !strings.Contains(err.Error(), "gh pr edit --cq") {
		t.Errorf("expected error to suggest 'gh pr edit --cq', got: %v", err)
	}
}

func TestHasMetadataUpdates(t *testing.T) {
	if hasMetadataUpdates(PushOptions{}) {
		t.Errorf("empty PushOptions should not have metadata updates")
	}

	tests := []struct {
		name string
		opts PushOptions
	}{
		{"CQ", PushOptions{CQ: 1}},
		{"Topic", PushOptions{Topic: "fix"}},
		{"Hashtags", PushOptions{Hashtags: []string{"tag"}}},
		{"Draft", PushOptions{Draft: true}},
		{"Wip", PushOptions{Wip: true}},
		{"Ready", PushOptions{Ready: true}},
		{"Publish", PushOptions{Publish: true}},
		{"Reviewers", PushOptions{Reviewers: []string{"a@b.com"}}},
		{"CC", PushOptions{CC: []string{"c@b.com"}}},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			if !hasMetadataUpdates(tc.opts) {
				t.Errorf("hasMetadataUpdates(%+v) = false, want true", tc.opts)
			}
		})
	}
}

func TestExecutePush_NoNewChanges_Publish_FallbackREST(t *testing.T) {
	ctx := context.Background()
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/I1234567890123456789012345678901234567890/revisions/current/review", http.StatusOK, map[string]any{})
	SetMockGerritClient(t, func(ctx context.Context, cmd *cobra.Command) (*gerrit.Client, error) {
		return gerrit.NewClient(ctx, server.URL, http.DefaultClient)
	})

	mockGit := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) > 0 && args[0] == "push" {
				stderr.Write([]byte("To sso://pigweed/pigweed\n ! [remote rejected] HEAD -> refs/for/main%publish-comments (no new changes)\nerror: failed to push some refs\n"))
				return fmt.Errorf("exit status 1")
			}
			if len(args) > 0 && args[0] == "log" {
				stdout.Write([]byte("Commit subject\n\nChange-Id: I1234567890123456789012345678901234567890\n"))
				return nil
			}
			return nil
		},
	}
	cfg := &Config{
		Git:     mockGit,
		Host:    server.URL,
		Profile: profiles["pigweed"],
	}

	cmd := &cobra.Command{}
	var outBuf, errBuf bytes.Buffer
	cmd.SetOut(&outBuf)
	cmd.SetErr(&errBuf)
	cmd.SetContext(ctx)
	SetConfig(cmd, cfg)

	pushOpts := PushOptions{
		Publish: true,
	}

	err := executePush(ctx, cmd, cfg, "main", pushOpts, false)
	if err != nil {
		t.Fatalf("unexpected error, should have fallen back to REST: %v", err)
	}

	if !strings.Contains(outBuf.String(), "applied metadata updates to Change") {
		t.Errorf("expected output to mention applied metadata updates, got: %s", outBuf.String())
	}
	if !strings.Contains(outBuf.String(), "Draft comments published successfully") {
		t.Errorf("expected output to mention Draft comments published, got: %s", outBuf.String())
	}

	if server.CallCount("POST", "/changes/I1234567890123456789012345678901234567890/revisions/current/review") != 1 {
		t.Fatalf("expected review API to be called on Gerrit server, got calls: %v", server.Requests())
	}

	var capturedInput gerrit.ReviewInput
	if req := server.LastRequest(); req != nil {
		json.Unmarshal(req.Body, &capturedInput)
	}
	if capturedInput.Drafts != "PUBLISH_ALL_REVISIONS" {
		t.Errorf("expected Drafts=%q, got %q", "PUBLISH_ALL_REVISIONS", capturedInput.Drafts)
	}
}

func TestExecutePush_NoNewChanges_RESTFailure(t *testing.T) {
	ctx := context.Background()
	server := NewMockGerritServer(t)
	server.OnStatus(http.StatusInternalServerError)
	SetMockGerritClient(t, func(ctx context.Context, cmd *cobra.Command) (*gerrit.Client, error) {
		return gerrit.NewClient(ctx, server.URL, http.DefaultClient)
	})

	mockGit := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) > 0 && args[0] == "push" {
				stderr.Write([]byte("To sso://pigweed/pigweed\n ! [remote rejected] HEAD -> refs/for/main%publish-comments (no new changes)\nerror: failed to push some refs\n"))
				return fmt.Errorf("exit status 1")
			}
			if len(args) > 0 && args[0] == "log" {
				stdout.Write([]byte("Commit subject\n\nChange-Id: I1234567890123456789012345678901234567890\n"))
				return nil
			}
			return nil
		},
	}
	cfg := &Config{
		Git:     mockGit,
		Host:    server.URL,
		Profile: profiles["pigweed"],
	}

	cmd := &cobra.Command{}
	var outBuf, errBuf bytes.Buffer
	cmd.SetOut(&outBuf)
	cmd.SetErr(&errBuf)
	cmd.SetContext(ctx)
	SetConfig(cmd, cfg)

	pushOpts := PushOptions{
		Publish: true,
	}

	err := executePush(ctx, cmd, cfg, "main", pushOpts, false)
	if err == nil {
		t.Fatal("expected error on REST failure, got nil")
	}
	if !strings.Contains(err.Error(), "failed to apply metadata updates via Gerrit API") {
		t.Errorf("expected error message mentioning failed to apply metadata updates, got: %v", err)
	}
}

func TestExecutePush_NoNewChanges_CC_Failure(t *testing.T) {
	ctx := context.Background()
	server := NewMockGerritServer(t)
	server.OnStatus(http.StatusBadRequest)
	SetMockGerritClient(t, func(ctx context.Context, cmd *cobra.Command) (*gerrit.Client, error) {
		return gerrit.NewClient(ctx, server.URL, http.DefaultClient)
	})

	mockGit := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) > 0 && args[0] == "push" {
				stderr.Write([]byte("To sso://pigweed/pigweed\n ! [remote rejected] HEAD -> refs/for/main%cc=bad (no new changes)\nerror: failed to push some refs\n"))
				return fmt.Errorf("exit status 1")
			}
			if len(args) > 0 && args[0] == "log" {
				stdout.Write([]byte("Commit subject\n\nChange-Id: I1234567890123456789012345678901234567890\n"))
				return nil
			}
			return nil
		},
	}
	cfg := &Config{
		Git:     mockGit,
		Host:    server.URL,
		Profile: profiles["pigweed"],
	}

	cmd := &cobra.Command{}
	var outBuf, errBuf bytes.Buffer
	cmd.SetOut(&outBuf)
	cmd.SetErr(&errBuf)
	cmd.SetContext(ctx)
	SetConfig(cmd, cfg)

	pushOpts := PushOptions{
		CC: []string{"bad@domain.invalid"},
	}

	err := executePush(ctx, cmd, cfg, "main", pushOpts, false)
	if err == nil {
		t.Fatal("expected error on CC REST failure, got nil")
	}
	if !strings.Contains(err.Error(), "failed to apply metadata updates via Gerrit API") {
		t.Errorf("expected error message mentioning failed to apply metadata updates, got: %v", err)
	}
}

func TestExecutePush_NilConfig(t *testing.T) {
	cmd := &cobra.Command{}
	err := executePush(context.Background(), cmd, nil, "main", PushOptions{}, false)
	if err == nil {
		t.Fatal("expected error on nil cfg, got nil")
	}
	if !strings.Contains(err.Error(), "internal error: cfg is uninitialized in executePush") {
		t.Errorf("expected internal error message, got: %v", err)
	}
}

func TestExecutePush_NilCmd(t *testing.T) {
	cfg := &Config{}
	err := executePush(context.Background(), nil, cfg, "main", PushOptions{}, false)
	if err == nil {
		t.Fatal("expected error on nil cmd, got nil")
	}
	if !strings.Contains(err.Error(), "internal error: cmd is uninitialized in executePush") {
		t.Errorf("expected internal error message, got: %v", err)
	}
}

func TestApplyPushOptionsViaREST_NilConfig(t *testing.T) {
	cmd := &cobra.Command{}
	err := applyPushOptionsViaREST(context.Background(), cmd, nil, PushOptions{})
	if err == nil {
		t.Fatal("expected error on nil cfg, got nil")
	}
	if !strings.Contains(err.Error(), "internal error: cfg is uninitialized in applyPushOptionsViaREST") {
		t.Errorf("expected internal error message, got: %v", err)
	}
}

func TestApplyPushOptionsViaREST_NilCmd(t *testing.T) {
	cfg := &Config{}
	err := applyPushOptionsViaREST(context.Background(), nil, cfg, PushOptions{})
	if err == nil {
		t.Fatal("expected error on nil cmd, got nil")
	}
	if !strings.Contains(err.Error(), "internal error: cmd is uninitialized in applyPushOptionsViaREST") {
		t.Errorf("expected internal error message, got: %v", err)
	}
}

func TestCheckSubmodulePolicy(t *testing.T) {
	ctx := context.Background()
	diffTreeWithSubmodule := ":160000 160000 1111111111111111111111111111111111111111 2222222222222222222222222222222222222222 M\tthird_party/foo\n" +
		":100644 100644 3333333333333333333333333333333333333333 4444444444444444444444444444444444444444 M\tpw_foo/bar.cc\n"

	t.Run("forbid-manual-rolls rejects modified gitlink with remediation", func(t *testing.T) {
		mock := &MockGitRunner{}
		mock.OnCommand("diff-tree --no-commit-id -r HEAD", diffTreeWithSubmodule)
		client := NewGitClient(mock)
		pcfg := DefaultProjectConfig()
		pcfg.Gerrit.SubmodulePolicy = "forbid-manual-rolls"

		var errBuf bytes.Buffer
		err := CheckSubmodulePolicy(ctx, client, pcfg, nil, "goog", "main", nil, &errBuf)
		if err == nil {
			t.Fatal("expected error when submodule is modified and policy is forbid-manual-rolls")
		}
		for _, want := range []string{"third_party/foo", "git checkout goog/main -- third_party/foo"} {
			if !strings.Contains(err.Error(), want) {
				t.Errorf("expected error to contain %q, got:\n%v", want, err)
			}
		}
	})

	t.Run("No-Submodule-Changes submit requirement rejects modified gitlink", func(t *testing.T) {
		mock := &MockGitRunner{}
		mock.OnCommand("diff-tree --no-commit-id -r HEAD", diffTreeWithSubmodule)
		client := NewGitClient(mock)
		pcfg := DefaultProjectConfig()
		change := &gerrit.ChangeInfo{
			SubmitRequirements: []gerrit.SubmitRequirementResultInfo{
				{Name: "No-Submodule-Changes", Status: "UNSATISFIED"},
			},
		}

		var errBuf bytes.Buffer
		err := CheckSubmodulePolicy(ctx, client, pcfg, change, "origin", "main", nil, &errBuf)
		if err == nil {
			t.Fatal("expected error when change has No-Submodule-Changes submit requirement")
		}
		for _, want := range []string{"No-Submodule-Changes", "third_party/foo", "git checkout origin/main -- third_party/foo"} {
			if !strings.Contains(err.Error(), want) {
				t.Errorf("expected error to contain %q, got:\n%v", want, err)
			}
		}
	})

	t.Run("warn-unpushed warns when new submodule SHA is not on remote branch", func(t *testing.T) {
		mock := &MockGitRunner{}
		mock.OnCommand("diff-tree --no-commit-id -r HEAD", diffTreeWithSubmodule).
			OnCommand("-C third_party/foo branch -r --contains 2222222222222222222222222222222222222222", "")
		client := NewGitClient(mock)
		pcfg := DefaultProjectConfig()
		pcfg.Gerrit.SubmodulePolicy = "warn-unpushed"

		var errBuf bytes.Buffer
		err := CheckSubmodulePolicy(ctx, client, pcfg, nil, "origin", "main", nil, &errBuf)
		if err != nil {
			t.Fatalf("expected warn-unpushed to warn rather than fail, got: %v", err)
		}
		for _, want := range []string{"Warning:", "third_party/foo", "2222222222222222222222222222222222222222"} {
			if !strings.Contains(errBuf.String(), want) {
				t.Errorf("expected stderr warning to contain %q, got:\n%s", want, errBuf.String())
			}
		}
	})

	t.Run("require-pushed fails when new submodule SHA is not on remote branch", func(t *testing.T) {
		mock := &MockGitRunner{}
		mock.OnCommand("diff-tree --no-commit-id -r HEAD", diffTreeWithSubmodule).
			OnCommand("-C third_party/foo branch -r --contains 2222222222222222222222222222222222222222", "")
		client := NewGitClient(mock)
		pcfg := DefaultProjectConfig()
		pcfg.Gerrit.SubmodulePolicy = "require-pushed"

		var errBuf bytes.Buffer
		err := CheckSubmodulePolicy(ctx, client, pcfg, nil, "origin", "main", nil, &errBuf)
		if err == nil {
			t.Fatal("expected require-pushed to fail when submodule SHA is not on remote branch")
		}
		for _, want := range []string{"third_party/foo", "2222222222222222222222222222222222222222", "git checkout origin/main -- third_party/foo"} {
			if !strings.Contains(err.Error(), want) {
				t.Errorf("expected error to contain %q, got:\n%v", want, err)
			}
		}
	})

	t.Run("require-pushed succeeds when new submodule SHA is on remote branch", func(t *testing.T) {
		mock := &MockGitRunner{}
		mock.OnCommand("diff-tree --no-commit-id -r HEAD", diffTreeWithSubmodule).
			OnCommand("-C third_party/foo branch -r --contains 2222222222222222222222222222222222222222", "  origin/main\n")
		client := NewGitClient(mock)
		pcfg := DefaultProjectConfig()
		pcfg.Gerrit.SubmodulePolicy = "require-pushed"

		var errBuf bytes.Buffer
		if err := CheckSubmodulePolicy(ctx, client, pcfg, nil, "origin", "main", nil, &errBuf); err != nil {
			t.Fatalf("expected require-pushed to succeed when SHA is on origin/main, got: %v", err)
		}
	})

	t.Run("resolves submodule path relative to repository root when invoked from subdirectory", func(t *testing.T) {
		repoRoot := filepath.Join(t.TempDir(), "repo")
		expectedSubPath := filepath.Join(repoRoot, "third_party", "foo")
		mock := &MockGitRunner{}
		mock.OnCommand("diff-tree --no-commit-id -r HEAD", diffTreeWithSubmodule).
			OnCommand("rev-parse --show-toplevel", repoRoot+"\n").
			OnCommand("-C "+expectedSubPath+" branch -r --contains 2222222222222222222222222222222222222222", "  origin/main\n")
		client := NewGitClient(mock)
		pcfg := DefaultProjectConfig()
		pcfg.Gerrit.SubmodulePolicy = "require-pushed"

		var errBuf bytes.Buffer
		if err := CheckSubmodulePolicy(ctx, client, pcfg, nil, "origin", "main", nil, &errBuf); err != nil {
			t.Fatalf("expected require-pushed to succeed using top-level resolved submodule path, got: %v", err)
		}
		if !mock.HasCall("-C " + expectedSubPath + " branch -r --contains 2222222222222222222222222222222222222222") {
			t.Errorf("expected git -C %s to be called, got calls: %v", expectedSubPath, mock.Calls)
		}
	})

	t.Run("VerifyHeadForPush requests SUBMIT_REQUIREMENTS so CheckSubmodulePolicy enforces No-Submodule-Changes", func(t *testing.T) {
		server := NewMockGerritServer(t)
		changeID := "I1234567890123456789012345678901234567890"
		server.OnJSON("GET", "/changes/", http.StatusOK, []gerrit.ChangeInfo{
			{
				Number:   42,
				ChangeID: changeID,
				SubmitRequirements: []gerrit.SubmitRequirementResultInfo{
					{Name: "No-Submodule-Changes", Status: "UNSATISFIED"},
				},
			},
		})
		SetMockGerritClient(t, func(ctx context.Context, cmd *cobra.Command) (*gerrit.Client, error) {
			return gerrit.NewClient(ctx, server.URL, http.DefaultClient)
		})

		mock := &MockGitRunner{}
		mock.OnCommand("log -1 --format=%B", "feat: Update submodule\n\nChange-Id: "+changeID+"\n").
			OnCommand("diff-tree --no-commit-id -r HEAD", diffTreeWithSubmodule)
		cfg := &Config{Git: mock, Host: server.URL}
		cmd := &cobra.Command{}
		cmd.SetOut(io.Discard)
		cmd.SetErr(io.Discard)
		cmd.SetContext(ctx)

		state, err := VerifyHeadForPush(ctx, cmd, cfg, true)
		if err != nil {
			t.Fatalf("VerifyHeadForPush failed: %v", err)
		}
		if req := server.LastRequest(); req == nil || !strings.Contains(req.URL.RawQuery, "SUBMIT_REQUIREMENTS") {
			t.Fatalf("expected VerifyHeadForPush to request SUBMIT_REQUIREMENTS, got request: %+v", req)
		}
		if err := CheckSubmodulePolicy(ctx, cfg.GitClient(), DefaultProjectConfig(), state.ExistingChange, "origin", "main", nil, io.Discard); err == nil {
			t.Fatal("expected CheckSubmodulePolicy to reject modified submodule via SubmitRequirements from VerifyHeadForPush")
		}
	})

	t.Run("propagates real git diff-tree error when policy is active", func(t *testing.T) {
		mock := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				stderr.Write([]byte("fatal: corrupt object\n"))
				return fmt.Errorf("exit status 128")
			},
		}
		client := NewGitClient(mock)
		pcfg := DefaultProjectConfig()
		pcfg.Gerrit.SubmodulePolicy = "forbid-manual-rolls"

		var errBuf bytes.Buffer
		err := CheckSubmodulePolicy(ctx, client, pcfg, nil, "origin", "main", nil, &errBuf)
		if err == nil || !strings.Contains(err.Error(), "failed to inspect commit HEAD for submodule changes") {
			t.Fatalf("expected diff-tree error to propagate, got: %v", err)
		}
	})
}

func TestExecutePush_SSOFallbacks(t *testing.T) {
	ctx := context.Background()

	t.Run("falls back from expired sso:// to rpc:// when git-remote-rpc is on PATH", func(t *testing.T) {
		origLookPath := LookPathFn
		LookPathFn = func(file string) (string, error) {
			if file == "git-remote-rpc" {
				return "/usr/bin/git-remote-rpc", nil
			}
			return "", fmt.Errorf("not found: %s", file)
		}
		defer func() { LookPathFn = origLookPath }()

		var pushCalls []string
		mockGit := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				call := strings.Join(args, " ")
				if call == "config --get remote.origin.url" {
					stdout.Write([]byte("sso://pigweed/pigweed/pigweed\n"))
					return nil
				}
				if strings.HasPrefix(call, "push ") || strings.Contains(call, " push ") {
					pushCalls = append(pushCalls, call)
					if call == "push origin HEAD:refs/for/main" {
						stderr.Write([]byte("remote_helper.go:932: sso: credentials expired. Try running gcert first\nfatal: remote helper 'sso' aborted session\n"))
						return fmt.Errorf("exit status 128")
					}
					if call == "-c url.rpc://pigweed/.insteadOf=sso://pigweed/ push origin HEAD:refs/for/main" {
						stderr.Write([]byte("remote: Processing changes: updated: 1, done\n"))
						return nil
					}
				}
				return nil
			},
		}

		var outBuf, errBuf bytes.Buffer
		cmd := &cobra.Command{}
		cmd.SetOut(&outBuf)
		cmd.SetErr(&errBuf)
		cmd.SetContext(ctx)
		cfg := &Config{Git: mockGit, Host: "pigweed-review.googlesource.com", Remote: "origin"}

		if err := executePush(ctx, cmd, cfg, "main", PushOptions{}, false); err != nil {
			t.Fatalf("expected executePush to succeed via rpc:// fallback, got error: %v\nStderr: %s", err, errBuf.String())
		}
		if len(pushCalls) != 2 {
			t.Fatalf("expected 2 push calls (initial sso + rpc fallback), got %d: %v", len(pushCalls), pushCalls)
		}
		if !strings.Contains(errBuf.String(), "rpc://pigweed/") {
			t.Errorf("expected stderr note mentioning rpc://pigweed/ fallback, got:\n%s", errBuf.String())
		}
	})

	t.Run("falls back from expired sso:// to https:// with luci-auth Bearer token on Corp Mac (no git-remote-rpc)", func(t *testing.T) {
		origLookPath := LookPathFn
		LookPathFn = func(file string) (string, error) {
			return "", fmt.Errorf("not found: %s", file)
		}
		defer func() { LookPathFn = origLookPath }()

		origGerrit := GerritTokenResolver
		GerritTokenResolver = func(ctx context.Context) (string, string, error) {
			return "mac-luci-bearer-tok", "luci-auth", nil
		}
		defer func() { GerritTokenResolver = origGerrit }()

		var pushCalls []string
		mockGit := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				call := strings.Join(args, " ")
				if call == "config --get remote.origin.url" {
					stdout.Write([]byte("sso://pigweed/pigweed/pigweed\n"))
					return nil
				}
				if strings.HasPrefix(call, "push ") || strings.Contains(call, " push ") {
					pushCalls = append(pushCalls, call)
					if call == "push origin HEAD:refs/for/main" {
						stderr.Write([]byte("remote_helper.go:932: sso: credentials expired. Try running gcert first\nfatal: remote helper 'sso' aborted session\n"))
						return fmt.Errorf("exit status 128")
					}
					wantHTTPS := "-c url.https://pigweed.googlesource.com/.insteadOf=sso://pigweed/ -c http.https://pigweed.googlesource.com/.extraHeader=Authorization: Bearer mac-luci-bearer-tok push origin HEAD:refs/for/main"
					if call == wantHTTPS {
						stderr.Write([]byte("remote: Processing changes: updated: 1, done\n"))
						return nil
					}
				}
				return nil
			},
		}

		var outBuf, errBuf bytes.Buffer
		cmd := &cobra.Command{}
		cmd.SetOut(&outBuf)
		cmd.SetErr(&errBuf)
		cmd.SetContext(ctx)
		cfg := &Config{Git: mockGit, Host: "pigweed-review.googlesource.com", Remote: "origin"}

		if err := executePush(ctx, cmd, cfg, "main", PushOptions{}, false); err != nil {
			t.Fatalf("expected executePush to succeed via https:// + luci-auth fallback, got error: %v\nCalls: %v", err, pushCalls)
		}
		if len(pushCalls) != 2 {
			t.Fatalf("expected 2 push calls (initial sso + https fallback), got %d: %v", len(pushCalls), pushCalls)
		}
		if !strings.Contains(errBuf.String(), "https://pigweed.googlesource.com/") || !strings.Contains(errBuf.String(), "luci-auth") {
			t.Errorf("expected stderr note mentioning https://pigweed.googlesource.com/ and luci-auth, got:\n%s", errBuf.String())
		}
	})

	t.Run("preserves no-new-changes error from fallback push so metadata updates still apply", func(t *testing.T) {
		origLookPath := LookPathFn
		LookPathFn = func(file string) (string, error) {
			if file == "git-remote-rpc" {
				return "/usr/bin/git-remote-rpc", nil
			}
			return "", fmt.Errorf("not found: %s", file)
		}
		defer func() { LookPathFn = origLookPath }()

		mockGit := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				call := strings.Join(args, " ")
				if call == "config --get remote.origin.url" {
					stdout.Write([]byte("sso://pigweed/pigweed/pigweed\n"))
					return nil
				}
				if call == "push origin HEAD:refs/for/main" {
					stderr.Write([]byte("sso: credentials expired. Try running gcert first\n"))
					return fmt.Errorf("exit status 128")
				}
				if call == "-c url.rpc://pigweed/.insteadOf=sso://pigweed/ push origin HEAD:refs/for/main" {
					stderr.Write([]byte(" ! [remote rejected] HEAD -> refs/for/main (no new changes)\n"))
					return fmt.Errorf("exit status 1")
				}
				return nil
			},
		}

		var outBuf, errBuf bytes.Buffer
		cmd := &cobra.Command{}
		cmd.SetOut(&outBuf)
		cmd.SetErr(&errBuf)
		cmd.SetContext(ctx)
		cfg := &Config{Git: mockGit, Host: "pigweed-review.googlesource.com", Remote: "origin"}

		err := executePush(ctx, cmd, cfg, "main", PushOptions{}, false)
		if err == nil || !strings.Contains(err.Error(), "no new changes to push") {
			t.Fatalf("expected friendly 'no new changes to push' error after rpc:// fallback reached Gerrit, got: %v", err)
		}
	})
}
