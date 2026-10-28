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
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

func TestCheckAgentCommand(t *testing.T) {
	tmpDir := t.TempDir()
	validMsgFile := filepath.Join(tmpDir, "valid_msg.txt")
	if err := os.WriteFile(validMsgFile, []byte("pw_ghish: Title\n\nBody.\n\nChange-Id: I0123456789abcdef0123456789abcdef01234567\n"), 0644); err != nil {
		t.Fatal(err)
	}
	missingChangeIDFile := filepath.Join(tmpDir, "missing_changeid.txt")
	if err := os.WriteFile(missingChangeIDFile, []byte("pw_ghish: Title without Change-Id\n\nBody.\n"), 0644); err != nil {
		t.Fatal(err)
	}
	multiChangeIDFile := filepath.Join(tmpDir, "multi_changeid.txt")
	if err := os.WriteFile(multiChangeIDFile, []byte("pw_ghish: Squashed\n\nChange-Id: I1111111111111111111111111111111111111111\nChange-Id: I2222222222222222222222222222222222222222\n"), 0644); err != nil {
		t.Fatal(err)
	}

	allowed := []string{
		"./gh pr create --cq",
		"./gh pr push",
		"./gh pr view 472267 --comments",
		"./gh pr diff 472267",
		"./gh pr checks 472267 --watch --fail-fast",
		"./gh run view 467905 --log-failed",
		"./gh run rerun 467905 --failed",
		"./gh issue view 315378787 --comments",
		"./gh wt use my-proj --json",
		"git status",
		"git commit -a --amend --no-edit",
		`git commit -m "Replace raw git push and bb add with ./gh"`,
		`git commit -m "pw_ghish: Block git commit --amend -m without Change-Id"`,
		`git commit --amend -m "pw_ghish: Title" -m "Change-Id: I0123456789abcdef0123456789abcdef01234567"`,
		`git commit --amend -m "pw_ghish: Title\n\nChange-Id: I0123456789abcdef0123456789abcdef01234567"`,
		`printf "pw_ghish: Title\n\nChange-Id: I0123456789abcdef0123456789abcdef01234567\n" | git commit --amend -F -`,
		"git commit --amend --only -F " + validMsgFile,
		"git log -1 --format=%B HEAD > .git/COMMIT_EDITMSG_TMP && git commit --amend --only -F .git/COMMIT_EDITMSG_TMP",
		"git commit --amend -C HEAD",
		"git reset HEAD file.go && git commit -m 'pw_ghish: New commit'",
		`git reset --soft HEAD~1 && git commit -m "pw_ghish: Squashed\n\nChange-Id: I0123456789abcdef0123456789abcdef01234567"`,
		"git fetch origin && git rebase origin/main",
		"git push --help",
		"bazelisk test //pw_ghish/...",
	}
	for _, cmd := range allowed {
		if reason := CheckAgentCommand(cmd); reason != "" {
			t.Errorf("expected CheckAgentCommand(%q) to be allowed, got reason: %s", cmd, reason)
		}
	}

	blockedChangeIDClobber := []string{
		`git commit --amend -m "pw_ghish: Tighten docs"`,
		`git checkout HEAD~1 -- MODULE.bazel.lock && git commit --amend -m "pw_ghish: Tighten docs" -m "Body" && ./gh pr push --stack --cq`,
		`git commit --amend -am "pw_ghish: Quick fix"`,
		`git commit --amend --message="pw_ghish: Quick fix"`,
		`printf "pw_ghish: No Change-Id\n" | git commit --amend -F -`,
		"git commit --amend --only -F " + missingChangeIDFile,
		"git commit --amend --only -F " + multiChangeIDFile,
		`git commit -m "pw_ghish: Bad squash" -m "Change-Id: I1111111111111111111111111111111111111111" -m "Change-Id: I2222222222222222222222222222222222222222"`,
		"git commit --amend -C HEAD~1",
		"git commit --amend --reuse-message=origin/main",
		`git reset --soft HEAD~1 && git commit -m "pw_ghish: Re-commit without Change-Id"`,
		`git reset HEAD~1 && git add -A && git commit -m "pw_ghish: Re-commit without Change-Id"`,
		`git reset HEAD@{1} && git commit -m "pw_ghish: Re-commit without Change-Id"`,
		`git reset a1b2c3d4 && git commit -m "pw_ghish: Re-commit without Change-Id"`,
		`git merge --squash feature && git commit -m "pw_ghish: Squashed without Change-Id"`,
		`git filter-branch --msg-filter 'sed s/foo/bar/' HEAD~2..HEAD`,
	}
	for _, cmd := range blockedChangeIDClobber {
		reason := CheckAgentCommand(cmd)
		if !strings.Contains(reason, "Change-Id") || !strings.Contains(reason, "COMMIT_EDITMSG_TMP") || !strings.Contains(reason, "./gh pr push") || !strings.Contains(reason, "[--bug b/<id> | --fixed b/<id>]") {
			t.Errorf("expected CheckAgentCommand(%q) to block Change-Id clobbering with ./gh pr push and surgical file-edit instructions, got: %q", cmd, reason)
		}
	}

	blockedGitPush := []string{
		"git push origin HEAD:refs/for/main",
		"git push origin main",
		"git -C /path/to/repo push origin HEAD:refs/for/main%ready",
		"git commit -am 'fix' && git push origin HEAD:refs/for/main",
	}
	for _, cmd := range blockedGitPush {
		reason := CheckAgentCommand(cmd)
		if !strings.Contains(reason, "./gh pr create") || !strings.Contains(reason, "./gh pr push") {
			t.Errorf("expected CheckAgentCommand(%q) to block git push with ./gh remedies, got: %q", cmd, reason)
		}
	}

	blockedRest := []string{
		"curl -L https://pigweed-review.googlesource.com/changes/472267/revisions/current/patch?raw",
		"gob-curl https://pigweed-review.googlesource.com/a/changes/472267/comments",
		"curl -sb ~/.gitcookies -X POST https://pigweed-review.googlesource.com/a/changes/472267/revisions/current/review",
		"curl -X POST https://cr-buildbucket.appspot.com/prpc/buildbucket.v2.Builds/GetBuild",
		"cat ~/.gitcookies",
	}
	for _, cmd := range blockedRest {
		reason := CheckAgentCommand(cmd)
		if !strings.Contains(reason, "./gh pr view") {
			t.Errorf("expected CheckAgentCommand(%q) to block REST call with ./gh pr view remedy, got: %q", cmd, reason)
		}
	}

	if reason := CheckAgentCommand("bb add -cl https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/404076/4 pigweed/pigweed.try/pigweed-lintformat"); !strings.Contains(reason, "./gh run rerun") {
		t.Errorf("expected bb add to be blocked with ./gh run rerun remedy, got: %q", reason)
	}
	if reason := CheckAgentCommand("python3 .agents/skills/gerrit/scripts/search_builds.py 406352 6"); !strings.Contains(reason, "./gh pr checks") {
		t.Errorf("expected search_builds.py to be blocked, got: %q", reason)
	}
}

func TestEvaluatePreToolUsePayload_Harnesses(t *testing.T) {
	// 1. Jetski payload
	jetskiIn := `{"toolCall":{"name":"run_command","args":{"CommandLine":"git push origin HEAD:refs/for/main"}}}`
	jetskiOut, err := EvaluatePreToolUsePayload([]byte(jetskiIn))
	if err != nil {
		t.Fatalf("EvaluatePreToolUsePayload(jetski) failed: %v", err)
	}
	var jetskiResp map[string]any
	if err := json.Unmarshal(jetskiOut, &jetskiResp); err != nil {
		t.Fatalf("unmarshal jetski response: %v", err)
	}
	if jetskiResp["decision"] != "deny" {
		t.Errorf("expected jetski decision=deny, got %v", jetskiResp)
	}

	// 2. Claude Code payload
	claudeIn := `{"tool_name":"Bash","tool_input":{"command":"git push origin HEAD:refs/for/main"}}`
	claudeOut, err := EvaluatePreToolUsePayload([]byte(claudeIn))
	if err != nil {
		t.Fatalf("EvaluatePreToolUsePayload(claude) failed: %v", err)
	}
	var claudeResp map[string]any
	if err := json.Unmarshal(claudeOut, &claudeResp); err != nil {
		t.Fatalf("unmarshal claude response: %v", err)
	}
	hookOut, _ := claudeResp["hookSpecificOutput"].(map[string]any)
	if hookOut["permissionDecision"] != "deny" {
		t.Errorf("expected claude permissionDecision=deny, got %v", claudeResp)
	}

	// 3. Cursor payload
	cursorIn := `{"hook_event_name":"beforeShellExecution","command":"git push origin HEAD:refs/for/main"}`
	cursorOut, err := EvaluatePreToolUsePayload([]byte(cursorIn))
	if err != nil {
		t.Fatalf("EvaluatePreToolUsePayload(cursor) failed: %v", err)
	}
	var cursorResp map[string]any
	if err := json.Unmarshal(cursorOut, &cursorResp); err != nil {
		t.Fatalf("unmarshal cursor response: %v", err)
	}
	if cursorResp["permission"] != "deny" {
		t.Errorf("expected cursor permission=deny, got %v", cursorResp)
	}
}

func TestInjectChangeID_Placement(t *testing.T) {
	cid := "I" + strings.Repeat("1", 40)
	cl := "Change-Id: " + cid
	existing := "pw_foo: Fix\n\nChange-Id: I" + strings.Repeat("a", 40) + "\n"
	tests := []struct {
		name   string
		in     string
		want   string
		wantOK bool
	}{
		{name: "body only", in: "pw_foo: Fix bar\n\nDetailed body.\n", want: "pw_foo: Fix bar\n\nDetailed body.\n\n" + cl + "\n", wantOK: true},
		{name: "subject only", in: "pw_foo: Fix bar\n", want: "pw_foo: Fix bar\n\n" + cl + "\n", wantOK: true},
		{name: "subject without trailing newline", in: "pw_foo: Fix bar", want: "pw_foo: Fix bar\n\n" + cl + "\n", wantOK: true},
		{name: "Bug only stays a footer", in: "pw_foo: Fix bar\n\nBody.\n\nBug: b/123\n", want: "pw_foo: Fix bar\n\nBody.\n\nBug: b/123\n" + cl + "\n", wantOK: true},
		{name: "Bug and Signed-off-by", in: "pw_foo: Fix bar\n\nBody.\n\nBug: b/123\nSigned-off-by: Alice <alice@example.com>\n", want: "pw_foo: Fix bar\n\nBody.\n\nBug: b/123\n" + cl + "\nSigned-off-by: Alice <alice@example.com>\n", wantOK: true},
		{name: "case-insensitive Signed-off-by", in: "pw_foo: Fix\n\nsigned-off-by: A <a@x.com>\n", want: "pw_foo: Fix\n\n" + cl + "\nsigned-off-by: A <a@x.com>\n", wantOK: true},
		{name: "two Signed-off-by only", in: "pw_foo: Fix bar\n\nSigned-off-by: A <a@x.com>\nSigned-off-by: B <b@x.com>\n", want: "pw_foo: Fix bar\n\n" + cl + "\nSigned-off-by: A <a@x.com>\nSigned-off-by: B <b@x.com>\n", wantOK: true},
		{name: "Fixed with continuation", in: "pw_foo: Fix\n\nFixed: b/1\nTest: one\n  two\n", want: "pw_foo: Fix\n\nFixed: b/1\nTest: one\n  two\n" + cl + "\n", wantOK: true},
		{name: "cherry-pick footer", in: "pw_foo: Fix\n\nBug: b/1\n(cherry picked from commit abc123)\n", want: "pw_foo: Fix\n\nBug: b/1\n(cherry picked from commit abc123)\n" + cl + "\n", wantOK: true},
		{name: "#include mid-body", in: "pw_foo: Fix include\n\n#include <foo.h>\nis now required.\n\nBug: b/123\n", want: "pw_foo: Fix include\n\n#include <foo.h>\nis now required.\n\nBug: b/123\n" + cl + "\n", wantOK: true},
		{name: "#include last line", in: "pw_foo: Fix\n\nAdd\n#include <foo.h>\n", want: "pw_foo: Fix\n\nAdd\n#include <foo.h>\n\n" + cl + "\n", wantOK: true},
		{name: "markdown heading", in: "pw_foo: Docs\n\n# Heading\n\nText.\n", want: "pw_foo: Docs\n\n# Heading\n\nText.\n\n" + cl + "\n", wantOK: true},
		{name: "CRLF", in: "pw_foo: Fix\r\n\r\nBody\r\n", want: "pw_foo: Fix\n\nBody\n\n" + cl + "\n", wantOK: true},
		{name: "Co-authored-by and Signed-off-by", in: "pw_foo: Fix\n\nCo-authored-by: B <b@x.com>\nSigned-off-by: A <a@x.com>\n", want: "pw_foo: Fix\n\nCo-authored-by: B <b@x.com>\n" + cl + "\nSigned-off-by: A <a@x.com>\n", wantOK: true},
		// A paragraph made only of `Key: value` lines is a trailer block, as in git.
		{name: "pseudo-trailer paragraph", in: "pw_foo: Fix\n\nNote: foo\n", want: "pw_foo: Fix\n\nNote: foo\n" + cl + "\n", wantOK: true},
		// Unlike git's 25%-with-a-git-generated-trailer rule, a paragraph is only a trailer block if every line is a trailer, so the Change-Id starts a new final paragraph.
		{name: "body then Signed-off-by in one paragraph", in: "pw_foo: Fix\n\nBody.\nSigned-off-by: A <a@x.com>\n", want: "pw_foo: Fix\n\nBody.\nSigned-off-by: A <a@x.com>\n\n" + cl + "\n", wantOK: true},
		{name: "cherry-pick footer only", in: "pw_foo: Fix\n\nBody.\n\n(cherry picked from commit abc123)\n", want: "pw_foo: Fix\n\nBody.\n\n(cherry picked from commit abc123)\n\n" + cl + "\n", wantOK: true},
		{name: "Bug right after subject", in: "pw_foo: Fix\nBug: b/1\n", want: "pw_foo: Fix\n\nBug: b/1\n" + cl + "\n", wantOK: true},
		{name: "leading blank line", in: "\npw_foo: Fix\n", want: "pw_foo: Fix\n\n" + cl + "\n", wantOK: true},
		{name: "fixup!", in: "fixup! pw_foo: Fix bar\n", want: "fixup! pw_foo: Fix bar\n"},
		{name: "leading blank line before fixup!", in: "\nfixup! pw_foo: Fix bar\n", want: "\nfixup! pw_foo: Fix bar\n"},
		{name: "squash!", in: "squash! pw_foo: Fix bar\n\nBody.\n", want: "squash! pw_foo: Fix bar\n\nBody.\n"},
		{name: "amend!", in: "amend! pw_foo: Fix bar\n\nNew msg.\n", want: "amend! pw_foo: Fix bar\n\nNew msg.\n"},
		{name: "not autosquash without space", in: "fixup!pw_foo: no space\n", want: "fixup!pw_foo: no space\n\n" + cl + "\n", wantOK: true},
		{name: "already has Change-Id", in: existing, want: existing},
		{name: "empty", in: "", want: ""},
		{name: "blank", in: "\n  \n", want: "\n  \n"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got, ok := injectChangeID(tc.in, cid)
			if got != tc.want || ok != tc.wantOK {
				t.Errorf("injectChangeID(%q) = (%q, %v), want (%q, %v)", tc.in, got, ok, tc.want, tc.wantOK)
			}
		})
	}
}

func TestInjectChangeIDInCommitMsgFile(t *testing.T) {
	cid := "I" + strings.Repeat("1", 40)
	cl := "Change-Id: " + cid
	tests := []struct {
		name   string
		in     string
		want   string
		wantOK bool
	}{
		{name: "editor template", in: "pw_foo: Fix bar\n\nBug: b/123\n\n# Please enter the commit message for your changes.\n# Lines starting with '#' will be ignored.\n", want: "pw_foo: Fix bar\n\nBug: b/123\n" + cl + "\n\n# Please enter the commit message for your changes.\n# Lines starting with '#' will be ignored.\n", wantOK: true},
		{name: "verbose scissors", in: "pw_foo: Fix\n\n# Please enter\n# ------------------------ >8 ------------------------\n# Do not modify or remove the line above.\ndiff --git a/x b/x\n+#include <y>\n", want: "pw_foo: Fix\n\n" + cl + "\n\n# Please enter\n# ------------------------ >8 ------------------------\n# Do not modify or remove the line above.\ndiff --git a/x b/x\n+#include <y>\n", wantOK: true},
		{name: "verbose scissors with trailing whitespace", in: "pw_foo: Fix\n\n# Please enter\n# ------------------------ >8 ------------------------  \t \n# Do not modify or remove the line above.\ndiff --git a/x b/x\n+#include <y>\n", want: "pw_foo: Fix\n\n" + cl + "\n\n# Please enter\n# ------------------------ >8 ------------------------  \t \n# Do not modify or remove the line above.\ndiff --git a/x b/x\n+#include <y>\n", wantOK: true},
		{name: "-m message without comments", in: "pw_foo: Fix\n\nBug: b/1\n", want: "pw_foo: Fix\n\nBug: b/1\n" + cl + "\n", wantOK: true},
		{name: "only comments", in: "\n# Please enter the commit message\n", want: "\n# Please enter the commit message\n"},
		{name: "has Change-Id above comments", in: "pw_foo: Fix\n\n" + cl + "\n\n# Please enter\n", want: "pw_foo: Fix\n\n" + cl + "\n\n# Please enter\n"},
		{name: "CRLF", in: "pw_foo: Fix\r\n\r\nBug: b/1\r\n\r\n# Please enter\r\n", want: "pw_foo: Fix\n\nBug: b/1\n" + cl + "\n\n# Please enter\n", wantOK: true},
		{name: "fixup", in: "fixup! pw_foo: Fix\n\n# Please enter\n", want: "fixup! pw_foo: Fix\n\n# Please enter\n"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got, ok := injectChangeIDInCommitMsgFile(tc.in, cid)
			if got != tc.want || ok != tc.wantOK {
				t.Errorf("injectChangeIDInCommitMsgFile(%q) = (%q, %v), want (%q, %v)", tc.in, got, ok, tc.want, tc.wantOK)
			}
		})
	}
}

func TestInjectChangeID_GeneratesValidID(t *testing.T) {
	updated, changed := InjectChangeID("pw_foo: Fix\n")
	if !changed || !HasChangeID(updated) {
		t.Fatalf("InjectChangeID() = (%q, %v), want a valid Change-Id line", updated, changed)
	}
	again, changedAgain := InjectChangeID(updated)
	if changedAgain || again != updated {
		t.Errorf("InjectChangeID(%q) = (%q, %v), want unchanged", updated, again, changedAgain)
	}
}

func TestEnsureChangeIDInFile(t *testing.T) {
	p := filepath.Join(t.TempDir(), "COMMIT_EDITMSG")
	in := "pw_foo: Fix bar\n\nBug: b/123\n\n# Please enter the commit message for your changes.\n# Lines starting with '#' will be ignored.\n"
	if err := os.WriteFile(p, []byte(in), 0644); err != nil {
		t.Fatal(err)
	}
	if err := EnsureChangeIDInFile(p); err != nil {
		t.Fatalf("EnsureChangeIDInFile() error: %v", err)
	}
	first, err := os.ReadFile(p)
	if err != nil {
		t.Fatal(err)
	}
	want := regexp.MustCompile(`^pw_foo: Fix bar\n\nBug: b/123\nChange-Id: I[0-9a-f]{40}\n\n# Please enter`)
	if !want.Match(first) {
		t.Fatalf("unexpected file contents:\n%s", first)
	}
	if err := EnsureChangeIDInFile(p); err != nil {
		t.Fatalf("second EnsureChangeIDInFile() error: %v", err)
	}
	second, err := os.ReadFile(p)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(first, second) {
		t.Errorf("second EnsureChangeIDInFile() changed the file:\n%s\nto:\n%s", first, second)
	}

	if err := EnsureChangeIDInFile(filepath.Join(t.TempDir(), "nonexistent")); err == nil {
		t.Errorf("EnsureChangeIDInFile(nonexistent) = nil, want error")
	}
	for _, untouched := range []string{"fixup! pw_foo: Fix\n", "\n# Only comments\n"} {
		f := filepath.Join(t.TempDir(), "COMMIT_EDITMSG")
		if err := os.WriteFile(f, []byte(untouched), 0644); err != nil {
			t.Fatal(err)
		}
		if err := EnsureChangeIDInFile(f); err != nil {
			t.Fatalf("EnsureChangeIDInFile(%q) error: %v", untouched, err)
		}
		got, err := os.ReadFile(f)
		if err != nil || string(got) != untouched {
			t.Errorf("EnsureChangeIDInFile(%q) wrote %q, err=%v; want unchanged", untouched, got, err)
		}
	}
}

type hookFakeGitRunner struct {
	gitDir       string
	blockRawPush string
}

func (f *hookFakeGitRunner) Run(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
	if len(args) >= 2 && args[0] == "rev-parse" && args[1] == "--git-common-dir" {
		_, _ = io.WriteString(stdout, f.gitDir+"\n")
		return nil
	}
	if len(args) >= 2 && args[0] == "config" {
		for _, a := range args {
			if a == "true" {
				f.blockRawPush = "true"
				return nil
			}
			if a == "--unset" {
				f.blockRawPush = ""
				return nil
			}
		}
		_, _ = io.WriteString(stdout, f.blockRawPush+"\n")
		return nil
	}
	return nil
}

func TestHookInstallStatusUninstallAndPrePush(t *testing.T) {
	tmpHome := t.TempDir()
	tmpRepo := t.TempDir()
	fakeBin := filepath.Join(t.TempDir(), "gh-ish-fake")
	if err := os.WriteFile(fakeBin, []byte("#!/bin/sh\n"), 0755); err != nil {
		t.Fatal(err)
	}

	origHome := HookUserHomeDir
	origExec := HookExecutablePath
	defer func() {
		HookUserHomeDir = origHome
		HookExecutablePath = origExec
	}()
	HookUserHomeDir = func() (string, error) { return tmpHome, nil }
	HookExecutablePath = func() (string, error) { return fakeBin, nil }

	fakeGit := &hookFakeGitRunner{gitDir: filepath.Join(tmpRepo, ".git")}

	isolateRootCmdForTest(t)

	// 1. Missing flags on install must fail fast with actionable error.
	hookInstallAgentFlag = ""
	hookInstallGitFlag = false
	hookInstallBlockRawPushFlag = false
	var outBuf, errBuf bytes.Buffer
	RootCmd.SetOut(&outBuf)
	RootCmd.SetErr(&errBuf)
	SetConfig(RootCmd, &Config{Git: fakeGit, CWD: tmpRepo})
	RootCmd.SetArgs([]string{"hook", "install"})
	if err := RootCmd.Execute(); err == nil {
		t.Fatalf("expected 'hook install' with no flags to return an error")
	}

	// 2. Invalid --agent target must fail fast.
	RootCmd.SetArgs([]string{"hook", "install", "--agent=bogus"})
	if err := RootCmd.Execute(); err == nil || !strings.Contains(err.Error(), "invalid --agent target") {
		t.Fatalf("expected invalid --agent error, got: %v", err)
	}

	// Seed a custom PreToolUse hook in Claude's settings.json to verify it is preserved.
	claudeCfgPath := agentConfigPath(tmpHome, "claude")
	if err := os.MkdirAll(filepath.Dir(claudeCfgPath), 0755); err != nil {
		t.Fatal(err)
	}
	seedClaude := `{"hooks":{"PreToolUse":[{"matcher":"Edit","hooks":[{"type":"command","command":"my-custom-linter"}]}]}}`
	if err := os.WriteFile(claudeCfgPath, []byte(seedClaude), 0644); err != nil {
		t.Fatal(err)
	}

	// 3. Install --agent=all --git --block-raw-push
	outBuf.Reset()
	RootCmd.SetArgs([]string{"hook", "install", "--agent=all", "--git", "--block-raw-push"})
	if err := RootCmd.Execute(); err != nil {
		t.Fatalf("hook install failed: %v", err)
	}
	if !strings.Contains(outBuf.String(), "Installed gh-ish hooks") || !strings.Contains(outBuf.String(), "Next:   gh hook status") {
		t.Errorf("expected hook install receipt in output, got:\n%s", outBuf.String())
	}
	for _, target := range []string{"jetski", "claude", "cursor"} {
		if !isAgentHookInstalled(tmpHome, target) {
			t.Errorf("expected agent hook for %s to be installed", target)
		}
	}
	claudeAfterInstall, err := os.ReadFile(claudeCfgPath)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(claudeAfterInstall), "my-custom-linter") {
		t.Errorf("expected custom hook to be preserved after install, got:\n%s", string(claudeAfterInstall))
	}

	// 4. Verify pre-push blocks when GH_ISH_ACTIVE is unset, and allows when GH_ISH_ACTIVE=1 or GH_ISH_ALLOW_RAW_PUSH=1.
	t.Setenv("GH_ISH_ACTIVE", "")
	t.Setenv("GH_ISH_ALLOW_RAW_PUSH", "")
	errBuf.Reset()
	RootCmd.SetArgs([]string{"hook", "pre-push"})
	if err := RootCmd.Execute(); err == nil {
		t.Fatalf("expected 'hook pre-push' to block when ghish.blockrawpush=true and GH_ISH_ACTIVE is unset")
	}
	if !strings.Contains(errBuf.String(), "./gh pr create") {
		t.Errorf("expected pre-push stderr to suggest ./gh pr create, got:\n%s", errBuf.String())
	}

	t.Setenv("GH_ISH_ACTIVE", "1")
	RootCmd.SetArgs([]string{"hook", "pre-push"})
	if err := RootCmd.Execute(); err != nil {
		t.Fatalf("expected 'hook pre-push' to succeed when GH_ISH_ACTIVE=1, got: %v", err)
	}

	t.Setenv("GH_ISH_ACTIVE", "")
	t.Setenv("GH_ISH_ALLOW_RAW_PUSH", "1")
	RootCmd.SetArgs([]string{"hook", "pre-push"})
	if err := RootCmd.Execute(); err != nil {
		t.Fatalf("expected 'hook pre-push' to succeed when GH_ISH_ALLOW_RAW_PUSH=1, got: %v", err)
	}

	// 5. Status output check
	outBuf.Reset()
	RootCmd.SetArgs([]string{"hook", "status"})
	if err := RootCmd.Execute(); err != nil {
		t.Fatalf("hook status failed: %v", err)
	}
	if !strings.Contains(outBuf.String(), "enabled") || !strings.Contains(outBuf.String(), "installed") ||
		!strings.Contains(outBuf.String(), "Verdict: 5 active, 0 disabled") || !strings.Contains(outBuf.String(), "Next:    gh pr status") {
		t.Errorf("unexpected hook status output:\n%s", outBuf.String())
	}

	// 6. Uninstall
	outBuf.Reset()
	RootCmd.SetArgs([]string{"hook", "uninstall", "--agent=all", "--git"})
	if err := RootCmd.Execute(); err != nil {
		t.Fatalf("hook uninstall failed: %v", err)
	}
	if !strings.Contains(outBuf.String(), "Uninstalled gh-ish hooks") || !strings.Contains(outBuf.String(), "Next:   gh hook status") {
		t.Errorf("expected hook uninstall receipt in output, got:\n%s", outBuf.String())
	}
	for _, target := range []string{"jetski", "claude", "cursor"} {
		if isAgentHookInstalled(tmpHome, target) {
			t.Errorf("expected agent hook for %s to be removed after uninstall", target)
		}
	}
	claudeAfterUninstall, err := os.ReadFile(claudeCfgPath)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(claudeAfterUninstall), "my-custom-linter") {
		t.Errorf("expected custom hook to remain after uninstall, got:\n%s", string(claudeAfterUninstall))
	}
}
