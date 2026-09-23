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
