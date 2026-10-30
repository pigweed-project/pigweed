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
	"fmt"
	"io"
	"net/http"
	"path/filepath"
	"slices"
	"strings"
	"testing"

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
	id1 := "I1111111111111111111111111111111111111111"
	id2 := "I2222222222222222222222222222222222222222"

	tests := []struct {
		name       string
		count      string
		stackLog   string
		allowStack bool
		cmdName    string
		wantErrs   []string
	}{
		{
			name:    "single commit passes",
			count:   "1\n",
			cmdName: "create",
		},
		{
			name:     "zero commits errors in create",
			count:    "0\n",
			cmdName:  "create",
			wantErrs: []string{"HEAD has 0 commits ahead", "git commit"},
		},
		{
			name:     "multiple commits without stack errors in create",
			count:    "3\n",
			cmdName:  "create",
			wantErrs: []string{"gh pr create --base", "--stack"},
		},
		{
			name:     "multiple commits without stack errors in push",
			count:    "3\n",
			cmdName:  "push",
			wantErrs: []string{"gh pr push --base", "--stack"},
		},
		{
			name:       "multiple commits with stack passes",
			count:      "3\n",
			allowStack: true,
			cmdName:    "create",
		},
		{
			name:       "rejects unsquashed fixup commit in stack",
			count:      "1\n",
			stackLog:   fmt.Sprintf("aaa1111\x00fixup! pw_ghish: Base\x00fixup! pw_ghish: Base\n\nChange-Id: %s\n\x1e", id1),
			allowStack: true,
			cmdName:    "push",
			wantErrs:   []string{"unsquashed fixup/squash commit"},
		},
		{
			name:  "rejects duplicate Change-Id in stack",
			count: "2\n",
			stackLog: fmt.Sprintf(
				"aaa1111\x00pw_ghish: First\x00pw_ghish: First\n\nChange-Id: %s\n\x1e"+
					"bbb2222\x00pw_ghish: Second\x00pw_ghish: Second\n\nChange-Id: %s\n\x1e",
				id1, id1,
			),
			allowStack: true,
			cmdName:    "push",
			wantErrs:   []string{"share the same Change-Id"},
		},
		{
			name:  "rejects commit with multiple Change-Id footers in stack",
			count: "1\n",
			stackLog: fmt.Sprintf(
				"aaa1111\x00pw_ghish: Squashed\x00pw_ghish: Squashed\n\nChange-Id: %s\nChange-Id: %s\n\x1e",
				id1, id2,
			),
			allowStack: true,
			cmdName:    "push",
			wantErrs:   []string{"multiple Change-Id footers"},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			mockGit := &MockGitRunner{
				RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
					if len(args) >= 2 && args[0] == "rev-list" && args[1] == "--count" {
						stdout.Write([]byte(tt.count))
					}
					if len(args) >= 2 && args[0] == "log" && args[1] == "--reverse" && tt.stackLog != "" {
						stdout.Write([]byte(tt.stackLog))
					}
					return nil
				},
			}
			err := ValidateCommitStack(ctx, NewGitClient(mockGit), "main", tt.allowStack, tt.cmdName)
			if len(tt.wantErrs) == 0 {
				if err != nil {
					t.Errorf("unexpected error: %v", err)
				}
				return
			}
			if err == nil {
				t.Fatalf("expected error containing %v, got nil", tt.wantErrs)
			}
			for _, want := range tt.wantErrs {
				if !strings.Contains(err.Error(), want) {
					t.Errorf("expected error to contain %q, got: %v", want, err)
				}
			}
		})
	}

	t.Run("canceled context fails immediately", func(t *testing.T) {
		cancCtx, cancel := context.WithCancel(ctx)
		cancel()
		client := NewGitClient(&MockGitRunner{})
		if err := ValidateCommitStack(cancCtx, client, "main", false, "create"); err == nil {
			t.Fatal("expected error for canceled context, got nil")
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
	if strings.Contains(err.Error(), "curl") {
		t.Errorf("hint still mentions curl: %v", err)
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

const noNewChangesChangeID = "I1234567890123456789012345678901234567890"

func runNoNewChangesPush(t *testing.T, pushOpts PushOptions, setup func(*MockGerritServer)) (*MockGerritServer, string, error) {
	t.Helper()
	ctx := context.Background()
	server := NewMockGerritServer(t)
	if setup != nil {
		setup(server)
	}
	mockGit := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) > 0 && args[0] == "push" {
				stderr.Write([]byte("To sso://pigweed/pigweed\n ! [remote rejected] HEAD -> refs/for/main (no new changes)\nerror: failed to push some refs\n"))
				return fmt.Errorf("exit status 1")
			}
			if len(args) > 0 && args[0] == "log" {
				stdout.Write([]byte("Commit subject\n\nChange-Id: " + noNewChangesChangeID + "\n"))
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
	var outBuf bytes.Buffer
	cmd.SetOut(&outBuf)
	cmd.SetErr(&outBuf)
	cmd.SetContext(ctx)
	SetConfig(cmd, cfg)

	err := executePush(ctx, cmd, cfg, "main", pushOpts, false)
	return server, outBuf.String(), err
}

func TestExecutePush_NoNewChanges_FallbackREST(t *testing.T) {
	server, out, err := runNoNewChangesPush(t, PushOptions{CQ: 1}, func(s *MockGerritServer) {
		s.OnReview(noNewChangesChangeID)
	})
	if err != nil {
		t.Fatalf("unexpected error, should have fallen back to REST: %v", err)
	}
	if !strings.Contains(out, "applied metadata updates to Change") {
		t.Errorf("expected output to mention applied metadata updates, got: %s", out)
	}
	if !strings.Contains(out, "Commit-Queue+1 set successfully") {
		t.Errorf("expected output to mention Commit-Queue+1, got: %s", out)
	}
	if server.CallCount("POST", "/changes/"+noNewChangesChangeID+"/revisions/current/review") != 1 {
		t.Errorf("expected review API to be called on Gerrit server, got calls: %v", server.Requests())
	}
}

// --auto with nothing to push still has to land on the label this host names,
// which means asking the change rather than replaying the profile default.
func TestExecutePush_NoNewChanges_FallbackREST_AutoSubmit(t *testing.T) {
	server, out, err := runNoNewChangesPush(t, PushOptions{AutoSubmit: true}, func(s *MockGerritServer) {
		s.OnDefaultChange(noNewChangesChangeID, WithLabels("Code-Review", "Auto-Submit"))
		s.OnReview(noNewChangesChangeID)
	})
	if err != nil {
		t.Fatalf("unexpected error, should have fallen back to REST: %v", err)
	}

	capturedInput := DecodeRequest[ReviewInput](t, server.LastRequest())
	if capturedInput.Labels["Auto-Submit"] != 1 {
		t.Errorf("Labels got %v, want Auto-Submit=1", capturedInput.Labels)
	}
	if _, voted := capturedInput.Labels["Pigweed-Auto-Submit"]; voted {
		t.Errorf("Labels got %v, want no vote on the profile's label", capturedInput.Labels)
	}
	if !strings.Contains(out, "Auto-submit enabled (Auto-Submit+1)") {
		t.Errorf("expected output to name the label voted, got: %s", out)
	}
}

// The metadata-only path applies what it can and then says --auto could not be
// honored, rather than reporting auto-submit that is not going to happen.
func TestExecutePush_NoNewChanges_FallbackREST_AutoSubmitUnsupported(t *testing.T) {
	server, out, err := runNoNewChangesPush(t, PushOptions{AutoSubmit: true}, func(s *MockGerritServer) {
		s.OnDefaultChange(noNewChangesChangeID, WithLabels("Code-Review", "Commit-Queue"))
		s.OnReview(noNewChangesChangeID)
	})
	if err == nil {
		t.Fatal("expected an error for a host with no auto-submit label")
	}
	if !strings.Contains(err.Error(), "has no auto-submit label") {
		t.Errorf("expected the error to name the problem, got: %v", err)
	}

	var capturedInput ReviewInput
	for _, req := range server.Requests() {
		if req.Method == "POST" && strings.HasSuffix(req.Path, "/review") {
			capturedInput = DecodeRequest[ReviewInput](t, &req)
		}
	}
	if capturedInput.Labels["Commit-Queue"] != 1 {
		t.Errorf("Labels got %v, want the Commit-Queue dry run", capturedInput.Labels)
	}
	if strings.Contains(out, "Auto-submit enabled") {
		t.Errorf("output claims auto-submit was enabled when it was not: %s", out)
	}
}

func TestExecutePush_NoNewChanges_NoOptions(t *testing.T) {
	_, _, err := runNoNewChangesPush(t, PushOptions{}, nil)
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

func TestExecutePush_NoNewChanges_Nightly_FallbackREST(t *testing.T) {
	server, out, err := runNoNewChangesPush(t, PushOptions{Nightly: true, ChangeNumber: 123456}, func(s *MockGerritServer) {
		s.OnDefaultChange(123456, WithChangeID(noNewChangesChangeID), WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit"))
		s.OnJSON("POST", "/changes/123456/flows", http.StatusCreated, map[string]any{})
	})
	if err != nil {
		t.Fatalf("unexpected error, should have fallen back to REST: %v", err)
	}
	if server.CallCount("POST", "/changes/123456/flows") != 1 {
		t.Fatalf("expected POST /changes/123456/flows to be called once, got %d", server.CallCount("POST", "/changes/123456/flows"))
	}
	if !strings.Contains(out, "Created submit flow for Change 123456 (scheduled for tomorrow at 02:36 in America/Los_Angeles).") {
		t.Errorf("unexpected output: %s", out)
	}
}

func TestExecutePush_NoNewChanges_Publish_FallbackREST(t *testing.T) {
	server, out, err := runNoNewChangesPush(t, PushOptions{Publish: true}, func(s *MockGerritServer) {
		s.OnReview(noNewChangesChangeID)
	})
	if err != nil {
		t.Fatalf("unexpected error, should have fallen back to REST: %v", err)
	}
	if !strings.Contains(out, "applied metadata updates to Change") {
		t.Errorf("expected output to mention applied metadata updates, got: %s", out)
	}
	if !strings.Contains(out, "Draft comments published successfully") {
		t.Errorf("expected output to mention Draft comments published, got: %s", out)
	}
	if server.CallCount("POST", "/changes/"+noNewChangesChangeID+"/revisions/current/review") != 1 {
		t.Fatalf("expected review API to be called on Gerrit server, got calls: %v", server.Requests())
	}
	capturedInput := DecodeRequest[ReviewInput](t, server.LastRequest())
	if capturedInput.Drafts != "PUBLISH_ALL_REVISIONS" {
		t.Errorf("expected Drafts=%q, got %q", "PUBLISH_ALL_REVISIONS", capturedInput.Drafts)
	}
}

func TestExecutePush_NoNewChanges_RESTFailure(t *testing.T) {
	_, _, err := runNoNewChangesPush(t, PushOptions{Publish: true}, func(s *MockGerritServer) {
		s.OnStatus(http.StatusInternalServerError)
	})
	if err == nil {
		t.Fatal("expected error on REST failure, got nil")
	}
	if !strings.Contains(err.Error(), "failed to apply metadata updates via Gerrit API") {
		t.Errorf("expected error message mentioning failed to apply metadata updates, got: %v", err)
	}
}

func TestExecutePush_NoNewChanges_CC_Failure(t *testing.T) {
	_, _, err := runNoNewChangesPush(t, PushOptions{CC: []string{"bad@domain.invalid"}}, func(s *MockGerritServer) {
		s.OnStatus(http.StatusBadRequest)
	})
	if err == nil {
		t.Fatal("expected error on CC REST failure, got nil")
	}
	if !strings.Contains(err.Error(), "failed to apply metadata updates via Gerrit API") {
		t.Errorf("expected error message mentioning failed to apply metadata updates, got: %v", err)
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
		{"AutoSubmit", PushOptions{AutoSubmit: true}},
		{"Nightly", PushOptions{Nightly: true}},
		{"SubmitAt", PushOptions{SubmitAt: "20:30"}},
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
		change := &ChangeInfo{
			SubmitRequirements: []SubmitRequirementResultInfo{
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
		server.OnJSON("GET", "/changes/", http.StatusOK, []ChangeInfo{
			{
				Number:   42,
				ChangeID: changeID,
				SubmitRequirements: []SubmitRequirementResultInfo{
					{Name: "No-Submodule-Changes", Status: "UNSATISFIED"},
				},
			},
		})

		mock := &MockGitRunner{}
		mock.OnCommand("log -1 --format=%B", "feat: Update submodule\n\nChange-Id: "+changeID+"\n").
			OnCommand("diff-tree --no-commit-id -r HEAD", diffTreeWithSubmodule)
		cfg := &Config{Git: mock, Host: server.URL}
		cmd := &cobra.Command{}
		cmd.SetOut(io.Discard)
		cmd.SetErr(io.Discard)
		cmd.SetContext(ctx)

		state, err := VerifyHeadForPush(ctx, cmd, cfg, true, false)
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
		var gotEnv []string
		mockGit := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				call := strings.Join(args, " ")
				if call == "config --get remote.origin.url" {
					stdout.Write([]byte("sso://pigweed/pigweed/pigweed\n"))
					return nil
				}
				if strings.HasPrefix(call, "push ") || strings.Contains(call, " push ") {
					pushCalls = append(pushCalls, call)
					if len(pushCalls) == 1 {
						stderr.Write([]byte("remote_helper.go:932: sso: credentials expired. Try running gcert first\nfatal: remote helper 'sso' aborted session\n"))
						return fmt.Errorf("exit status 128")
					}
					if call == "push origin HEAD:refs/for/main" {
						gotEnv = gitEnvFromContext(ctx)
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
		if mockGit.HasCall("mac-luci-bearer-tok") || mockGit.HasCall("Authorization") || mockGit.HasCall("extraHeader") {
			t.Errorf("bearer token or auth header leaked into git command args: %v", mockGit.Calls)
		}
		wantEnv := []string{
			"GIT_CONFIG_COUNT=3",
			"GIT_CONFIG_KEY_0=color.ui",
			"GIT_CONFIG_VALUE_0=false",
			"GIT_CONFIG_KEY_1=url.https://pigweed.googlesource.com/.insteadOf",
			"GIT_CONFIG_VALUE_1=sso://pigweed/",
			"GIT_CONFIG_KEY_2=http.https://pigweed.googlesource.com/.extraHeader",
			"GIT_CONFIG_VALUE_2=Authorization: Bearer mac-luci-bearer-tok",
		}
		if !slices.Equal(gotEnv, wantEnv) {
			t.Errorf("git env = %v, want %v", gotEnv, wantEnv)
		}
		t.Setenv("GIT_CONFIG_COUNT", "1")
		if got := gitEnvFromContext(withGitConfigEnv(t.Context(), "k", "v")); !slices.Equal(got, []string{"GIT_CONFIG_COUNT=2", "GIT_CONFIG_KEY_1=k", "GIT_CONFIG_VALUE_1=v"}) {
			t.Errorf("withGitConfigEnv with existing count = %v", got)
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

// In the no-new-commits path, the other updates are applied and reported
// before scheduling, and a flow failure is not reported as a failure to apply
// them.
func TestExecutePush_NoNewChanges_Nightly_FlowFailure(t *testing.T) {
	_, out, err := runNoNewChangesPush(t, PushOptions{CQ: 1, Nightly: true, ChangeNumber: 123456}, func(s *MockGerritServer) {
		s.OnDefaultChange(123456, WithChangeID(noNewChangesChangeID), WithLabels("Code-Review", "Commit-Queue", "Pigweed-Auto-Submit"))
		s.OnReview(noNewChangesChangeID)
		s.OnString("POST", "/changes/123456/flows", http.StatusInternalServerError, "text/plain", "flows disabled")
	})
	if err == nil {
		t.Fatal("expected an error when flow creation fails, got nil")
	}
	if !strings.Contains(err.Error(), "the change is up to date on Gerrit, but scheduling the submit flow failed") {
		t.Errorf("expected a schedule failure, got: %v", err)
	}
	if strings.Contains(err.Error(), "failed to apply metadata updates") {
		t.Errorf("error must not claim the metadata updates failed, got: %v", err)
	}
	if !strings.Contains(out, "Commit-Queue+1 set successfully") {
		t.Errorf("expected the applied CQ vote to be reported before the failure, got: %s", out)
	}
}

func TestValidateCommitMsgSingleCommit(t *testing.T) {
	ctx := t.Context()
	r := newTestGitRepo(t)
	cfg := &Config{Git: r.Client}

	if ahead, hasRemote, err := validateCommitMsgSingleCommit(ctx, cfg, "main", "create"); err != nil || ahead != 0 || !hasRemote {
		t.Fatalf("at origin/main tip: got (%d, %v, %v), want (0, true, nil)", ahead, hasRemote, err)
	}

	r.WriteFile("base.txt", "commit 1\n")
	if _, err := r.Client.CommitNewAll(ctx, "pw_foo: First local commit\n", false, io.Discard, io.Discard); err != nil {
		t.Fatal(err)
	}
	if ahead, hasRemote, err := validateCommitMsgSingleCommit(ctx, cfg, "main", "push"); err != nil || ahead != 1 || !hasRemote {
		t.Fatalf("single local commit: got (%d, %v, %v), want (1, true, nil)", ahead, hasRemote, err)
	}
	if ahead, hasRemote, err := validateCommitMsgSingleCommit(ctx, cfg, "nonexistent-upstream", "push"); err != nil || ahead != 0 || hasRemote {
		t.Fatalf("missing upstream ref: got (%d, %v, %v), want (0, false, nil)", ahead, hasRemote, err)
	}

	r.WriteFile("base.txt", "commit 2\n")
	if _, err := r.Client.CommitNewAll(ctx, "pw_foo: Second local commit\n", false, io.Discard, io.Discard); err != nil {
		t.Fatal(err)
	}
	ahead, hasRemote, err := validateCommitMsgSingleCommit(ctx, cfg, "main", "push")
	if err == nil || ahead != 2 || !hasRemote || !strings.Contains(err.Error(), "git rebase -i origin/main") {
		t.Fatalf("expected multi-commit error (2, true), got (%d, %v, %v)", ahead, hasRemote, err)
	}
}

func TestQueryExistingChangeByID(t *testing.T) {
	ctx := t.Context()
	const changeID = "I1234567890123456789012345678901234567890"

	if ch, ok := queryExistingChangeByID(ctx, nil, changeID); ch != nil || ok {
		t.Errorf("nil client = (%v, %v), want (nil, false)", ch, ok)
	}

	t.Run("HTTP 404 returns (nil, true)", func(t *testing.T) {
		server := NewMockGerritServer(t).OnStatus(http.StatusNotFound)
		ch, ok := queryExistingChangeByID(ctx, server.Client(), changeID)
		if ch != nil || !ok {
			t.Errorf("HTTP 404 = (%v, %v), want (nil, true)", ch, ok)
		}
	})

	t.Run("HTTP 500 returns (nil, false)", func(t *testing.T) {
		server := NewMockGerritServer(t).OnStatus(http.StatusInternalServerError)
		ch, ok := queryExistingChangeByID(ctx, server.Client(), changeID)
		if ch != nil || ok {
			t.Errorf("HTTP 500 = (%v, %v), want (nil, false)", ch, ok)
		}
	})

	t.Run("HTTP 200 empty slice returns (nil, true)", func(t *testing.T) {
		server := NewMockGerritServer(t)
		server.OnJSON("GET", "/changes/", http.StatusOK, []ChangeInfo{})
		ch, ok := queryExistingChangeByID(ctx, server.Client(), changeID)
		if ch != nil || !ok {
			t.Errorf("HTTP 200 [] = (%v, %v), want (nil, true)", ch, ok)
		}
	})

	t.Run("HTTP 200 null returns (nil, true)", func(t *testing.T) {
		server := NewMockGerritServer(t)
		server.OnString("GET", "/changes/", http.StatusOK, "application/json", ")]}'\nnull\n")
		ch, ok := queryExistingChangeByID(ctx, server.Client(), changeID)
		if ch != nil || !ok {
			t.Errorf("HTTP 200 null = (%v, %v), want (nil, true)", ch, ok)
		}
	})

	t.Run("HTTP 200 match returns (&change, true)", func(t *testing.T) {
		server := NewMockGerritServer(t)
		server.OnJSON("GET", "/changes/", http.StatusOK, []ChangeInfo{{Number: 42, ChangeID: changeID}})
		ch, ok := queryExistingChangeByID(ctx, server.Client(), changeID)
		if ch == nil || ch.Number != 42 || !ok {
			t.Errorf("HTTP 200 match = (%v, %v), want (#42, true)", ch, ok)
		}
	})
}
