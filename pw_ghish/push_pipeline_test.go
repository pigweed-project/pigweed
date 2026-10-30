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
			if len(args) > 0 && args[0] == "push" {
				calls = append(calls, strings.Join(args, " "))
			}
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
	for _, tc := range []struct {
		name     string
		stderr   string
		wantErrs []string
	}{
		{
			name:     "missing Change-Id",
			stderr:   "remote: ERROR: commit 1234567: missing Change-Id in commit message footer\n",
			wantErrs: []string{"missing a Change-Id in its footer", "commit-msg", "git commit --amend --no-edit"},
		},
		{
			name:     "missing CLA",
			stderr:   "remote: ERROR: One or more of your commits has an author without a Contributor License Agreement (see https://cla.developers.google.com/)\n",
			wantErrs: []string{"https://cla.developers.google.com/", "git commit --amend --reset-author --no-edit", "./gh pr push"},
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			mockGit := &MockGitRunner{
				RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
					if len(args) > 0 && args[0] == "push" {
						stderr.Write([]byte(tc.stderr))
						return fmt.Errorf("exit status 1")
					}
					return nil
				},
			}
			cfg := &Config{Git: mockGit, Host: "pigweed-review.googlesource.com", Profile: profiles["pigweed"]}
			cmd := &cobra.Command{}
			cmd.SetOut(io.Discard)
			cmd.SetErr(io.Discard)
			SetConfig(cmd, cfg)

			err := executePush(ctx, cmd, cfg, "main", PushOptions{}, false)
			if err == nil {
				t.Fatal("expected error on git push rejection, got nil")
			}
			for _, want := range tc.wantErrs {
				if !strings.Contains(err.Error(), want) {
					t.Errorf("expected error to contain %q, got: %v", want, err)
				}
			}
			if strings.Contains(err.Error(), "curl") {
				t.Errorf("hint still mentions curl: %v", err)
			}
		})
	}
}

func TestExecutePush_HTTPSAuthRetry(t *testing.T) {
	const authFailed = "fatal: Authentication failed for 'https://pigweed.googlesource.com/pigweed/pigweed/'\n"
	const promptDisabled = "fatal: could not read Username for 'https://pigweed.googlesource.com': terminal prompts disabled\n"
	const prohibited = "remote: prohibited by Gerrit: not permitted: create change on refs/heads/main\n"
	const claRejected = "remote: error: You must sign the Contributor License Agreement at https://cla.developers.google.com/\n"
	wantFirstEnv := []string{
		"GIT_CONFIG_COUNT=1",
		"GIT_CONFIG_KEY_0=color.ui",
		"GIT_CONFIG_VALUE_0=false",
		"GIT_TERMINAL_PROMPT=0",
	}
	wantRetryEnv := []string{
		"GIT_CONFIG_COUNT=2",
		"GIT_CONFIG_KEY_0=color.ui",
		"GIT_CONFIG_VALUE_0=false",
		"GIT_TERMINAL_PROMPT=0",
		"GIT_CONFIG_KEY_1=http.https://pigweed.googlesource.com/.extraHeader",
		"GIT_CONFIG_VALUE_1=Authorization: Bearer luci-tok",
	}

	tests := []struct {
		name string
		// setup runs after WithHermeticAuth.
		setup func(t *testing.T)
		// remote overrides cfg.Remote ("origin" by default).
		remote string
		// pushURL configures remote.origin.pushurl (with remote.origin.url set to sso://).
		pushURL string
		// pushStderr is the stderr of each push attempt, in order; "" succeeds.
		pushStderr      []string
		wantPushes      int
		wantResolver    int
		wantRetryEnv    bool
		wantErr         bool
		wantErrContains string
		wantErrExtra    string
		wantExitCode    int
		// wantStderr, if set, must appear in the command's stderr.
		wantStderr string
		// resolverErr makes the Gerrit token resolver fail.
		resolverErr bool
	}{
		{
			name:       "success uses own credentials",
			pushStderr: []string{""},
			wantPushes: 1,
		},
		{
			name:         "auth failure retries with token",
			pushStderr:   []string{authFailed, ""},
			wantPushes:   2,
			wantResolver: 1,
			wantRetryEnv: true,
		},
		{
			name:         "prompt disabled retries with token",
			pushStderr:   []string{promptDisabled, ""},
			wantPushes:   2,
			wantResolver: 1,
			wantRetryEnv: true,
		},
		{
			name:         "direct https remote URL retries with token",
			remote:       "https://pigweed.googlesource.com/pigweed/pigweed",
			pushStderr:   []string{authFailed, ""},
			wantPushes:   2,
			wantResolver: 1,
			wantRetryEnv: true,
		},
		{
			name:         "remote.origin.pushurl https retries with token",
			pushURL:      "https://pigweed.googlesource.com/pigweed/pigweed",
			pushStderr:   []string{authFailed, ""},
			wantPushes:   2,
			wantResolver: 1,
			wantRetryEnv: true,
		},
		{
			name: "auth-mode none attaches no token",
			setup: func(t *testing.T) {
				prev := AuthModeFlag
				AuthModeFlag = "none"
				t.Cleanup(func() { AuthModeFlag = prev })
			},
			pushStderr:      []string{authFailed},
			wantPushes:      1,
			wantErr:         true,
			wantErrContains: "Gerrit authentication is disabled",
			wantExitCode:    ExitCodeAuth,
			wantStderr:      authFailed,
		},
		{
			name:            "retry also rejected",
			pushStderr:      []string{authFailed, authFailed},
			wantPushes:      2,
			wantResolver:    1,
			wantRetryEnv:    true,
			wantErr:         true,
			wantErrContains: "which was also rejected",
			wantExitCode:    ExitCodeAuth,
		},
		{
			name:            "retry fails for a non-auth reason",
			pushStderr:      []string{authFailed, prohibited},
			wantPushes:      2,
			wantResolver:    1,
			wantRetryEnv:    true,
			wantErr:         true,
			wantErrContains: "exit status 128 (push 2)",
			wantStderr:      prohibited,
		},
		{
			name:            "retry fails with terminal CLA rejection",
			pushStderr:      []string{authFailed, claRejected},
			wantPushes:      2,
			wantResolver:    1,
			wantRetryEnv:    true,
			wantErr:         true,
			wantErrContains: "Contributor License Agreement",
		},
		{
			name:            "token unavailable: no retry",
			resolverErr:     true,
			pushStderr:      []string{promptDisabled},
			wantPushes:      1,
			wantResolver:    1,
			wantErr:         true,
			wantErrContains: "luci-auth login -scopes " + fmt.Sprintf("%q", DefaultGerritScopes),
			wantErrExtra:    "Gerrit OAuth2 token probe: luci-auth: not logged in",
			wantExitCode:    ExitCodeAuth,
			wantStderr:      promptDisabled,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			var resolverCalls int
			WithHermeticAuth(t, AuthResolvers{
				GerritToken: func(ctx context.Context) (string, string, error) {
					resolverCalls++
					if tc.resolverErr {
						return "", "", fmt.Errorf("luci-auth: not logged in")
					}
					return "luci-tok", "luci-auth", nil
				},
			})
			if tc.setup != nil {
				tc.setup(t)
			}

			var pushEnvs [][]string
			git := &MockGitRunner{
				RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
					if len(args) == 0 || args[0] != "push" {
						return nil
					}
					pushEnvs = append(pushEnvs, gitEnvFromContext(ctx))
					i := len(pushEnvs) - 1
					if i < len(tc.pushStderr) && tc.pushStderr[i] != "" {
						_, _ = io.WriteString(stderr, tc.pushStderr[i])
						return fmt.Errorf("exit status 128 (push %d)", i+1)
					}
					return nil
				},
			}
			if tc.pushURL != "" {
				git.OnCommand("config --get remote.origin.pushurl", tc.pushURL+"\n")
				git.OnCommand("config --get remote.origin.url", "sso://pigweed/pigweed/pigweed\n")
			} else if tc.remote == "" {
				git.OnCommand("config --get remote.origin.url", "https://pigweed.googlesource.com/pigweed/pigweed\n")
			}

			remote := tc.remote
			if remote == "" {
				remote = "origin"
			}
			cfg := &Config{Git: git, Host: "pigweed-review.googlesource.com", Remote: remote, Profile: profiles["pigweed"]}
			cmd := &cobra.Command{}
			var stderr bytes.Buffer
			cmd.SetOut(io.Discard)
			cmd.SetErr(&stderr)
			SetConfig(cmd, cfg)

			err := executePush(t.Context(), cmd, cfg, "main", PushOptions{}, false)
			if (err != nil) != tc.wantErr {
				t.Fatalf("executePush error = %v, wantErr %v", err, tc.wantErr)
			}
			if !strings.Contains(stderr.String(), tc.wantStderr) {
				t.Errorf("stderr = %q, want it to contain %q", stderr.String(), tc.wantStderr)
			}
			if tc.wantErrContains != "" && !strings.Contains(err.Error(), tc.wantErrContains) {
				t.Errorf("executePush error = %q, want it to contain %q", err, tc.wantErrContains)
			}
			if tc.wantErrExtra != "" && !strings.Contains(err.Error(), tc.wantErrExtra) {
				t.Errorf("executePush error = %q, want it to contain %q", err, tc.wantErrExtra)
			}
			if tc.wantExitCode != 0 {
				if got := ExitCodeFor(err); got != tc.wantExitCode {
					t.Errorf("ExitCodeFor(err) = %d, want %d", got, tc.wantExitCode)
				}
			}
			if len(pushEnvs) != tc.wantPushes {
				t.Fatalf("push attempts = %d, want %d", len(pushEnvs), tc.wantPushes)
			}
			if resolverCalls != tc.wantResolver {
				t.Errorf("Gerrit token resolver calls = %d, want %d", resolverCalls, tc.wantResolver)
			}
			if !slices.Equal(pushEnvs[0], wantFirstEnv) {
				t.Errorf("first push git env = %v, want %v", pushEnvs[0], wantFirstEnv)
			}
			if tc.wantRetryEnv && !slices.Equal(pushEnvs[1], wantRetryEnv) {
				t.Errorf("retry push git env = %v, want %v", pushEnvs[1], wantRetryEnv)
			}
		})
	}

	t.Run("sso pushurl is not an HTTPS base", func(t *testing.T) {
		git := &MockGitRunner{}
		git.OnCommand("config --get remote.origin.url", "https://pigweed.googlesource.com/pigweed/pigweed\n")
		git.OnCommand("config --get remote.origin.pushurl", "sso://pigweed/pigweed/pigweed\n")
		cfg := &Config{Git: git, Remote: "origin"}
		if got := resolveHTTPSGoogleSourceBase(t.Context(), cfg, "origin"); got != "" {
			t.Errorf("resolveHTTPSGoogleSourceBase with sso:// pushurl = %q, want empty", got)
		}
	})

	t.Run("non-googlesource and bare googlesource.com HTTPS remotes return empty", func(t *testing.T) {
		for _, remoteURL := range []string{
			"https://github.com/google/pigweed.git",
			"https://googlesource.com",
			"https://googlesource.com/pigweed/pigweed",
		} {
			if got := resolveHTTPSGoogleSourceBase(t.Context(), nil, remoteURL); got != "" {
				t.Errorf("resolveHTTPSGoogleSourceBase(%q) = %q, want empty", remoteURL, got)
			}
		}
	})

	t.Run("isGitPushHTTPSAuthError markers", func(t *testing.T) {
		cases := []struct {
			stderr string
			want   bool
		}{
			{"fatal: could not read Username for 'https://pigweed.googlesource.com': No such device or address", true},
			{"fatal: Authentication failed for 'https://pigweed.googlesource.com/pigweed/'", true},
			{"fatal: terminal prompts disabled", true},
			{"remote: HTTP Basic: Access denied. The provided password or token is incorrect", true},
			{"fatal: unable to access 'https://pigweed.googlesource.com/': The requested URL returned error: 401", true},
			{"fatal: unable to access 'https://pigweed.googlesource.com/': The requested URL returned error: 403", true},
			{"fatal: repository 'https://pigweed.googlesource.com/missing/' not found (The requested URL returned error: 404)", false},
			{"fatal: unable to access 'https://pigweed.googlesource.com/': Could not resolve host: pigweed.googlesource.com", false},
		}
		for _, tc := range cases {
			if got := isGitPushHTTPSAuthError(tc.stderr); got != tc.want {
				t.Errorf("isGitPushHTTPSAuthError(%q) = %v, want %v", tc.stderr, got, tc.want)
			}
		}
	})

	t.Run("nil cmd and nil cfg return explicit errors", func(t *testing.T) {
		ctx := t.Context()
		cfg := &Config{Git: &MockGitRunner{}}
		cmd := &cobra.Command{}
		if err := executePush(ctx, nil, cfg, "main", PushOptions{}, false); err == nil || !strings.Contains(err.Error(), "cmd is uninitialized") {
			t.Errorf("executePush(nil cmd) = %v, want cmd uninitialized error", err)
		}
		if err := executePush(ctx, cmd, nil, "main", PushOptions{}, false); err == nil || !strings.Contains(err.Error(), "cfg is uninitialized") {
			t.Errorf("executePush(nil cfg) = %v, want cfg uninitialized error", err)
		}
		if err := applyPushOptionsViaREST(ctx, nil, cfg, PushOptions{}); err == nil || !strings.Contains(err.Error(), "cmd is uninitialized") {
			t.Errorf("applyPushOptionsViaREST(nil cmd) = %v, want cmd uninitialized error", err)
		}
		if err := applyPushOptionsViaREST(ctx, cmd, nil, PushOptions{}); err == nil || !strings.Contains(err.Error(), "cfg is uninitialized") {
			t.Errorf("applyPushOptionsViaREST(nil cfg) = %v, want cfg uninitialized error", err)
		}
	})
}

func TestExecutePush_WithTopicAndHashtags(t *testing.T) {
	ctx := context.Background()
	var calls []string
	mockGit := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) > 0 && args[0] == "push" {
				calls = append(calls, strings.Join(args, " "))
			}
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
		WithHermeticAuth(t, AuthResolvers{
			LookPath: func(file string) (string, error) {
				if file == "git-remote-rpc" {
					return "/usr/bin/git-remote-rpc", nil
				}
				return "", fmt.Errorf("not found: %s", file)
			},
		})

		var pushCalls []string
		var gotEnv []string
		mockGit := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				call := strings.Join(args, " ")
				if call == "config --get remote.origin.url" {
					stdout.Write([]byte("sso://pigweed/pigweed/pigweed\n"))
					return nil
				}
				if call == "push origin HEAD:refs/for/main" {
					pushCalls = append(pushCalls, call)
					if len(pushCalls) == 1 {
						stderr.Write([]byte("remote_helper.go:932: sso: credentials expired. Try running gcert first\nfatal: remote helper 'sso' aborted session\n"))
						return fmt.Errorf("exit status 128")
					}
					gotEnv = gitEnvFromContext(ctx)
					stderr.Write([]byte("remote: Processing changes: updated: 1, done\n"))
					return nil
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
		if !slices.Contains(gotEnv, "GIT_CONFIG_KEY_1=url.rpc://pigweed/.insteadOf") {
			t.Errorf("expected rpc:// rewrite in git env, got: %v", gotEnv)
		}
		if !strings.Contains(errBuf.String(), "rpc://pigweed/") {
			t.Errorf("expected stderr note mentioning rpc://pigweed/ fallback, got:\n%s", errBuf.String())
		}
	})

	t.Run("falls back from expired sso:// to https:// with luci-auth Bearer token on Corp Mac (no git-remote-rpc)", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{
			GerritToken: func(ctx context.Context) (string, string, error) {
				return "mac-luci-bearer-tok", "luci-auth", nil
			},
		})

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
			"GIT_CONFIG_COUNT=6",
			"GIT_CONFIG_KEY_0=color.ui",
			"GIT_CONFIG_VALUE_0=false",
			"GIT_CONFIG_KEY_1=url.https://pigweed.googlesource.com/.insteadOf",
			"GIT_CONFIG_VALUE_1=sso://pigweed/",
			"GIT_CONFIG_KEY_2=url.https://pigweed.googlesource.com/.pushInsteadOf",
			"GIT_CONFIG_VALUE_2=sso://pigweed/",
			"GIT_CONFIG_KEY_3=url.https://pigweed.googlesource.com/.pushInsteadOf",
			"GIT_CONFIG_VALUE_3=https://pigweed.googlesource.com/",
			"GIT_CONFIG_KEY_4=url.https://pigweed.googlesource.com/.pushInsteadOf",
			"GIT_CONFIG_VALUE_4=https://pigweed-review.googlesource.com/",
			"GIT_CONFIG_KEY_5=http.https://pigweed.googlesource.com/.extraHeader",
			"GIT_CONFIG_VALUE_5=Authorization: Bearer mac-luci-bearer-tok",
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
		WithHermeticAuth(t, AuthResolvers{
			LookPath: func(file string) (string, error) {
				if file == "git-remote-rpc" {
					return "/usr/bin/git-remote-rpc", nil
				}
				return "", fmt.Errorf("not found: %s", file)
			},
		})

		var pushes int
		mockGit := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				call := strings.Join(args, " ")
				if call == "config --get remote.origin.url" {
					stdout.Write([]byte("sso://pigweed/pigweed/pigweed\n"))
					return nil
				}
				if call == "push origin HEAD:refs/for/main" {
					pushes++
					if pushes == 1 {
						stderr.Write([]byte("sso: credentials expired. Try running gcert first\n"))
						return fmt.Errorf("exit status 128")
					}
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

	t.Run("CLA rejection from rpc fallback is terminal", func(t *testing.T) {
		WithHermeticAuth(t, AuthResolvers{
			LookPath: func(file string) (string, error) {
				if file == "git-remote-rpc" {
					return "/usr/bin/git-remote-rpc", nil
				}
				return "", fmt.Errorf("not found: %s", file)
			},
		})

		var pushes int
		mockGit := &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				call := strings.Join(args, " ")
				if call == "config --get remote.origin.url" {
					stdout.Write([]byte("sso://pigweed/pigweed/pigweed\n"))
					return nil
				}
				if !slices.Contains(args, "push") {
					return nil
				}
				pushes++
				if pushes == 1 {
					stderr.Write([]byte("sso: credentials expired. Try running gcert first\n"))
					return fmt.Errorf("exit status 128")
				}
				stderr.Write([]byte("remote: A Contributor License Agreement is required. See https://cla.developers.google.com/\n"))
				return fmt.Errorf("exit status 1")
			},
		}

		cmd := &cobra.Command{}
		cmd.SetOut(io.Discard)
		cmd.SetErr(io.Discard)
		cmd.SetContext(ctx)
		cfg := &Config{Git: mockGit, Host: "pigweed-review.googlesource.com", Remote: "origin"}

		err := executePush(ctx, cmd, cfg, "main", PushOptions{}, false)
		if err == nil || !strings.Contains(err.Error(), "Contributor License Agreement (CLA) has not been signed") {
			t.Fatalf("expected CLA remediation error after rpc:// fallback, got: %v", err)
		}
		if pushes != 2 {
			t.Errorf("push attempts = %d, want 2 (no HTTPS retry after a CLA rejection)", pushes)
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
