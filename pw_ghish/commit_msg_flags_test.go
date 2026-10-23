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
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/spf13/cobra"
)

type zeroByteReader struct{}

func (zeroByteReader) Read(p []byte) (int, error) {
	for i := range p {
		p[i] = 'a'
	}
	return len(p), nil
}

func newTestCommitMsgCmd(t *testing.T, stdin string, args ...string) (*cobra.Command, CommitMessageFlags) {
	t.Helper()
	cmd := &cobra.Command{Use: "test"}
	AddCommitMessageFlags(cmd)
	if stdin != "" {
		cmd.SetIn(strings.NewReader(stdin))
	}
	if err := cmd.ParseFlags(args); err != nil {
		t.Fatalf("ParseFlags(%v) failed: %v", args, err)
	}
	return cmd, ParseCommitMessageFlags(cmd)
}

func TestAddCommitMessageFlags_UsageHints(t *testing.T) {
	cmd := &cobra.Command{Use: "test"}
	AddCommitMessageFlags(cmd)
	for _, name := range []string{"bug", "fixed"} {
		fl := cmd.Flags().Lookup(name)
		if fl == nil {
			t.Fatalf("expected --%s flag to be registered", name)
		}
		if !strings.Contains(fl.Usage, `e.g. b/<id>, URL, CSV, or "none"`) {
			t.Errorf("expected format hint in --%s usage, got: %q", name, fl.Usage)
		}
		if !strings.Contains(fl.Usage, "(ghish-only)") {
			t.Errorf("expected (ghish-only) marker in --%s usage, got: %q", name, fl.Usage)
		}
	}
}

func TestResolveAndValidateCommitMessageFlags(t *testing.T) {
	t.Run("rejects empty title", func(t *testing.T) {
		cmd, flags := newTestCommitMsgCmd(t, "", "--title", "   ")
		_, err := ResolveAndValidateCommitMessageFlags(cmd, &flags, "gh pr create")
		if err == nil || !strings.Contains(err.Error(), "cannot set an empty commit title") {
			t.Fatalf("expected empty title error, got: %v", err)
		}
	})

	t.Run("rejects multiline title with hint for --body and --body-file", func(t *testing.T) {
		for _, badTitle := range []string{
			"pw_foo: First line\nSecond line",
			"pw_foo: First line\r\nSecond line",
			"pw_foo: Trailing newline\n",
		} {
			cmd, flags := newTestCommitMsgCmd(t, "", "--title", badTitle)
			_, err := ResolveAndValidateCommitMessageFlags(cmd, &flags, "gh pr push")
			if err == nil {
				t.Fatalf("expected error for multiline title %q", badTitle)
			}
			for _, wantSubstr := range []string{"--body", "--body-file"} {
				if !strings.Contains(err.Error(), wantSubstr) {
					t.Errorf("title %q: expected error to mention %q, got:\n%s", badTitle, wantSubstr, err)
				}
			}
		}
	})

	t.Run("rejects both --body and --body-file", func(t *testing.T) {
		cmd, flags := newTestCommitMsgCmd(t, "", "-b", "text", "-F", "file.txt")
		_, err := ResolveAndValidateCommitMessageFlags(cmd, &flags, "gh pr create")
		if err == nil || !strings.Contains(err.Error(), "cannot specify both --body (-b) and --body-file (-F)") {
			t.Fatalf("expected mutual exclusion error, got: %v", err)
		}
	})

	t.Run("reads body from file and stdin while -b - stays literal", func(t *testing.T) {
		bodyPath := filepath.Join(t.TempDir(), "body.md")
		if err := os.WriteFile(bodyPath, []byte("Body from file with `code`.\n"), 0o600); err != nil {
			t.Fatal(err)
		}
		cmdFile, flagsFile := newTestCommitMsgCmd(t, "", "-F", bodyPath)
		resFile, err := ResolveAndValidateCommitMessageFlags(cmdFile, &flagsFile, "gh pr create")
		if err != nil || !resFile.HasBody || resFile.Body != "Body from file with `code`.\n" {
			t.Fatalf("got (%+v, %v)", resFile, err)
		}

		cmdStdin, flagsStdin := newTestCommitMsgCmd(t, "Body from stdin.\n", "-F", "-")
		resStdin, err := ResolveAndValidateCommitMessageFlags(cmdStdin, &flagsStdin, "gh pr create")
		if err != nil || !resStdin.HasBody || resStdin.Body != "Body from stdin.\n" {
			t.Fatalf("got (%+v, %v)", resStdin, err)
		}

		const unreadStdin = "Dash body stdin.\n"
		stdinReader := strings.NewReader(unreadStdin)
		cmdBodyDash := &cobra.Command{Use: "test"}
		AddCommitMessageFlags(cmdBodyDash)
		cmdBodyDash.SetIn(stdinReader)
		if err := cmdBodyDash.ParseFlags([]string{"-b", "-"}); err != nil {
			t.Fatal(err)
		}
		flagsBodyDash := ParseCommitMessageFlags(cmdBodyDash)
		resDash, err := ResolveAndValidateCommitMessageFlags(cmdBodyDash, &flagsBodyDash, "gh pr create")
		if err != nil || !resDash.HasBody || resDash.Body != "-" {
			t.Fatalf("got (%+v, %v)", resDash, err)
		}
		if stdinReader.Len() != len(unreadStdin) {
			t.Fatalf("expected stdin to remain unread for -b -, remaining=%d want=%d", stdinReader.Len(), len(unreadStdin))
		}
	})

	t.Run("bounds oversized -F stdin and -F file reads", func(t *testing.T) {
		cmdBigStdin := &cobra.Command{Use: "test"}
		AddCommitMessageFlags(cmdBigStdin)
		cmdBigStdin.SetIn(io.LimitReader(zeroByteReader{}, maxHTTPBodyBytes+1))
		if err := cmdBigStdin.ParseFlags([]string{"-F", "-"}); err != nil {
			t.Fatal(err)
		}
		flagsBigStdin := ParseCommitMessageFlags(cmdBigStdin)
		if _, err := ResolveAndValidateCommitMessageFlags(cmdBigStdin, &flagsBigStdin, "gh pr create"); err == nil || !strings.Contains(err.Error(), "exceeds maximum allowed size") {
			t.Fatalf("expected oversized stdin error, got: %v", err)
		}

		bigFilePath := filepath.Join(t.TempDir(), "oversized.txt")
		bigFile, err := os.Create(bigFilePath)
		if err != nil {
			t.Fatal(err)
		}
		if err := bigFile.Truncate(maxHTTPBodyBytes + 1); err != nil {
			_ = bigFile.Close()
			t.Fatal(err)
		}
		_ = bigFile.Close()

		cmdBigFile, flagsBigFile := newTestCommitMsgCmd(t, "", "-F", bigFilePath)
		if _, err := ResolveAndValidateCommitMessageFlags(cmdBigFile, &flagsBigFile, "gh pr create"); err == nil || !strings.Contains(err.Error(), "exceeds maximum allowed size") {
			t.Fatalf("expected oversized file error, got: %v", err)
		}
	})

	t.Run("rejects missing body file", func(t *testing.T) {
		cmd, flags := newTestCommitMsgCmd(t, "", "-F", filepath.Join(t.TempDir(), "missing.txt"))
		_, err := ResolveAndValidateCommitMessageFlags(cmd, &flags, "gh pr create")
		if err == nil || !strings.Contains(err.Error(), "failed to read commit body file") {
			t.Fatalf("expected missing file error, got: %v", err)
		}
	})

	t.Run("rejects GitHub issue syntax in title and body-file", func(t *testing.T) {
		bodyPath := filepath.Join(t.TempDir(), "gh.txt")
		if err := os.WriteFile(bodyPath, []byte("Details.\n\nFixes #456\n"), 0o600); err != nil {
			t.Fatal(err)
		}
		cmd, flags := newTestCommitMsgCmd(t, "", "-F", bodyPath)
		_, err := ResolveAndValidateCommitMessageFlags(cmd, &flags, "gh pr create")
		if err == nil || !strings.Contains(err.Error(), "#456") {
			t.Fatalf("expected GitHub syntax error from --body-file, got: %v", err)
		}
	})

	t.Run("rejects malformed Change-Id in body with source label", func(t *testing.T) {
		cmd, flags := newTestCommitMsgCmd(t, "", "-b", "Prose body.\n\nChange-Id: I12345")
		_, err := ResolveAndValidateCommitMessageFlags(cmd, &flags, "gh pr create")
		if err == nil || !strings.Contains(err.Error(), "malformed Gerrit Change-Id") || !strings.Contains(err.Error(), "--body") {
			t.Fatalf("expected malformed Change-Id error naming --body, got: %v", err)
		}
	})

	t.Run("rejects empty or conflicting bug flags across Gerrit aliases only", func(t *testing.T) {
		cmdEmpty, flagsEmpty := newTestCommitMsgCmd(t, "", "--bug", "")
		if _, err := ResolveAndValidateCommitMessageFlags(cmdEmpty, &flagsEmpty, "gh pr create"); err == nil {
			t.Fatal("expected error for empty --bug")
		}

		for _, tc := range []struct {
			body string
			flag string
			val  string
			want string
		}{
			{"Prose\n\nBug: b/111", "--bug", "222", "--bug and --body both set a Bug: trailer"},
			{"Prose\n\nBugs: b/999", "--bug", "123", "--bug and --body both set a Bug: trailer"},
			{"Prose\n\nIssue: b/999", "--bug", "123", "--bug and --body both set a Bug: trailer"},
			{"Prose\n\nFixed: b/999", "--fixed", "123", "--fixed and --body both set a Fixed: trailer"},
			{"Prose\n\nFixes: b/999", "--fixed", "123", "--fixed and --body both set a Fixed: trailer"},
		} {
			cmdDup, flagsDup := newTestCommitMsgCmd(t, "", "-b", tc.body, tc.flag, tc.val)
			if _, err := ResolveAndValidateCommitMessageFlags(cmdDup, &flagsDup, "gh pr create"); err == nil || !strings.Contains(err.Error(), tc.want) {
				t.Fatalf("body=%q %s=%q: expected error containing %q, got: %v", tc.body, tc.flag, tc.val, tc.want, err)
			}
		}

		// Non-trailing trailer-shaped example followed by prose is NOT an active footer.
		cmdNonTrailing, flagsNonTrailing := newTestCommitMsgCmd(t, "", "-b", "Example:\n\nFixes: b/999\n\nExplanation prose.", "--fixed", "123")
		if _, err := ResolveAndValidateCommitMessageFlags(cmdNonTrailing, &flagsNonTrailing, "gh pr create"); err != nil {
			t.Fatalf("expected non-trailing example block to be allowed, got: %v", err)
		}

		// Non-Gerrit `Closes: b/999` is NOT a Gerrit bug trailer alias, whereas
		// GitHub syntax `Closes #123` / `Closes: #123` is caught by CheckGitHubIssueSyntax.
		cmdClosesB, flagsClosesB := newTestCommitMsgCmd(t, "", "-b", "Prose\n\nCloses: b/999", "--fixed", "123")
		if _, err := ResolveAndValidateCommitMessageFlags(cmdClosesB, &flagsClosesB, "gh pr create"); err != nil {
			t.Fatalf("expected non-Gerrit Closes: b/999 not to be treated as bug trailer alias, got: %v", err)
		}
		for _, ghCloses := range []string{"Prose\n\nCloses #123", "Prose\n\nCloses: #123"} {
			cmdClosesGH, flagsClosesGH := newTestCommitMsgCmd(t, "", "-b", ghCloses, "--fixed", "456")
			err := func() error {
				_, e := ResolveAndValidateCommitMessageFlags(cmdClosesGH, &flagsClosesGH, "gh pr create")
				return e
			}()
			if err == nil || !strings.Contains(err.Error(), "GitHub issue syntax") {
				t.Fatalf("body=%q: expected GitHub issue syntax error, got: %v", ghCloses, err)
			}
		}

		cmdSame, flagsSame := newTestCommitMsgCmd(t, "", "--bug", "b/567763970", "--fixed", "https://issues.pigweed.dev/issues/567763970")
		if _, err := ResolveAndValidateCommitMessageFlags(cmdSame, &flagsSame, "gh pr create"); err == nil || !strings.Contains(err.Error(), "567763970") {
			t.Fatalf("expected same issue conflict error, got: %v", err)
		}
	})

	t.Run("allows indented code sample mentioning Fixed in body", func(t *testing.T) {
		codeBody := "Explain usage:\n\n  Fixed: b/<id>\n"
		cmd, flags := newTestCommitMsgCmd(t, "", "-b", codeBody, "--fixed", "42")
		res, err := ResolveAndValidateCommitMessageFlags(cmd, &flags, "gh pr create")
		if err != nil || !res.HasBody || res.Body != codeBody {
			t.Fatalf("expected indented Fixed sample to pass, got (%+v, %v)", res, err)
		}
	})

	t.Run("rejects GitHub tokens and invalid issue values", func(t *testing.T) {
		for _, tc := range []struct {
			flag string
			val  string
			want string
		}{
			{"--bug", "#456", "#456"},
			{"--fixed", "b/123, #456", "#456"},
			{"--bug", "Fixes #789", "#789"},
			{"--bug", "not-a-bug", "invalid --bug value"},
			{"--fixed", "garbage", "invalid --fixed value"},
		} {
			cmd, flags := newTestCommitMsgCmd(t, "", tc.flag, tc.val)
			_, err := ResolveAndValidateCommitMessageFlags(cmd, &flags, "gh pr create")
			if err == nil || !strings.Contains(err.Error(), tc.want) {
				t.Fatalf("%s %q: expected error containing %q, got: %v", tc.flag, tc.val, tc.want, err)
			}
		}
	})
}

func TestBuildUpdatedCommitMessage(t *testing.T) {
	orig := "pw_foo: Original title\n\nOriginal paragraph.\n\nBug: b/12345, b/99999\nChange-Id: I1111111111111111111111111111111111111111\n"

	t.Run("clears prose while preserving trailers and upgrading Bug", func(t *testing.T) {
		cmd, flags := newTestCommitMsgCmd(t, "", "-t", "pw_foo: Updated title", "-b", "", "--fixed", "12345")
		resolved, err := ResolveAndValidateCommitMessageFlags(cmd, &flags, "gh pr create")
		if err != nil {
			t.Fatal(err)
		}
		got, err := BuildUpdatedCommitMessage(orig, "Bug: b/{id}", resolved)
		if err != nil {
			t.Fatal(err)
		}
		want := "pw_foo: Updated title\n\nBug: b/99999\nFixed: b/12345\nChange-Id: I1111111111111111111111111111111111111111\n"
		if got != want {
			t.Errorf("got:\n%q\nwant:\n%q", got, want)
		}
	})

	t.Run("uniformly appends and preserves across all valid bug and fix spellings", func(t *testing.T) {
		const cid = "Change-Id: I1111111111111111111111111111111111111111\n"
		for _, tc := range []struct {
			name string
			orig string
			args []string
			want string
		}{
			{
				name: "Fixed + --fixed appends both",
				orig: "pw_foo: Fix two issues\n\nFixed: b/11\n" + cid,
				args: []string{"--fixed", "22"},
				want: "pw_foo: Fix two issues\n\nFixed: b/11\nFixed: b/22\n" + cid,
			},
			{
				name: "Fixes + --fixed appends both and canonicalizes",
				orig: "pw_foo: Fix two issues\n\nFixes: b/11\n" + cid,
				args: []string{"--fixed", "22"},
				want: "pw_foo: Fix two issues\n\nFixed: b/11\nFixed: b/22\n" + cid,
			},
			{
				name: "Bug + --bug appends both",
				orig: "pw_foo: Link two issues\n\nBug: b/11\n" + cid,
				args: []string{"--bug", "22"},
				want: "pw_foo: Link two issues\n\nBug: b/11\nBug: b/22\n" + cid,
			},
			{
				name: "Bugs + --bug appends both and canonicalizes",
				orig: "pw_foo: Link two issues\n\nBugs: b/11\n" + cid,
				args: []string{"--bug", "22"},
				want: "pw_foo: Link two issues\n\nBug: b/11\nBug: b/22\n" + cid,
			},
			{
				name: "Bug CSV + --fixed promotes target ID and keeps remaining Bug",
				orig: "pw_foo: Partial close\n\nBug: b/11, b/22\n" + cid,
				args: []string{"--fixed", "22"},
				want: "pw_foo: Partial close\n\nBug: b/11\nFixed: b/22\n" + cid,
			},
			{
				name: "Fixed CSV + --bug downgrades target ID and keeps remaining Fixed",
				orig: "pw_foo: Partial downgrade\n\nFixed: b/11, b/22\n" + cid,
				args: []string{"--bug", "22"},
				want: "pw_foo: Partial downgrade\n\nFixed: b/11\nBug: b/22\n" + cid,
			},
			{
				name: "Fixes CSV + --bug downgrades target ID and canonicalizes remaining Fixed",
				orig: "pw_foo: Partial downgrade\n\nFixes: b/11, b/22\n" + cid,
				args: []string{"--bug", "22"},
				want: "pw_foo: Partial downgrade\n\nFixed: b/11\nBug: b/22\n" + cid,
			},
		} {
			t.Run(tc.name, func(t *testing.T) {
				cmd, flags := newTestCommitMsgCmd(t, "", tc.args...)
				resolved, err := ResolveAndValidateCommitMessageFlags(cmd, &flags, "gh pr push")
				if err != nil {
					t.Fatal(err)
				}
				got, err := BuildUpdatedCommitMessage(tc.orig, "Bug: b/{id}", resolved)
				if err != nil {
					t.Fatal(err)
				}
				if got != tc.want {
					t.Errorf("got:\n%q\nwant:\n%q", got, tc.want)
				}
			})
		}
	})

	t.Run("upgrades Bug to Fixed above Change-Id", func(t *testing.T) {
		bugOrig := "pw_foo: Fix crash\n\nBug: b/42\nChange-Id: I1111111111111111111111111111111111111111\n"
		cmd, flags := newTestCommitMsgCmd(t, "", "--fixed", "42")
		resolved, err := ResolveAndValidateCommitMessageFlags(cmd, &flags, "gh pr push")
		if err != nil {
			t.Fatal(err)
		}
		got, err := BuildUpdatedCommitMessage(bugOrig, "Bug: b/{id}", resolved)
		if err != nil {
			t.Fatal(err)
		}
		want := "pw_foo: Fix crash\n\nFixed: b/42\nChange-Id: I1111111111111111111111111111111111111111\n"
		if got != want {
			t.Errorf("got:\n%q\nwant:\n%q", got, want)
		}
	})

	t.Run("downgrades Fixed to Bug above Change-Id", func(t *testing.T) {
		fixedOrig := "pw_foo: Partial work\n\nFixed: b/42\nChange-Id: I1111111111111111111111111111111111111111\n"
		cmd, flags := newTestCommitMsgCmd(t, "", "--bug", "42")
		resolved, err := ResolveAndValidateCommitMessageFlags(cmd, &flags, "gh pr push")
		if err != nil {
			t.Fatal(err)
		}
		got, err := BuildUpdatedCommitMessage(fixedOrig, "Bug: b/{id}", resolved)
		if err != nil {
			t.Fatal(err)
		}
		want := "pw_foo: Partial work\n\nBug: b/42\nChange-Id: I1111111111111111111111111111111111111111\n"
		if got != want {
			t.Errorf("got:\n%q\nwant:\n%q", got, want)
		}
	})

	t.Run("clears active bug trailers only when setting none and preserves non-active body blocks", func(t *testing.T) {
		fixedOrig := "pw_foo: Clear fixed\n\nFixed: b/999\n\nProse after example.\n\nFixed: b/42\nChange-Id: I1111111111111111111111111111111111111111\n"
		cmdBugNone, flagsBugNone := newTestCommitMsgCmd(t, "", "--bug", "none")
		resBugNone, err := ResolveAndValidateCommitMessageFlags(cmdBugNone, &flagsBugNone, "gh pr push")
		if err != nil {
			t.Fatal(err)
		}
		gotBugNone, err := BuildUpdatedCommitMessage(fixedOrig, "Bug: b/{id}", resBugNone)
		if err != nil {
			t.Fatal(err)
		}
		wantBugNone := "pw_foo: Clear fixed\n\nFixed: b/999\n\nProse after example.\n\nBug: None\nChange-Id: I1111111111111111111111111111111111111111\n"
		if gotBugNone != wantBugNone {
			t.Errorf("got:\n%q\nwant:\n%q", gotBugNone, wantBugNone)
		}

		bugOrig := "pw_foo: Clear bug\n\nBug: b/42\nChange-Id: I1111111111111111111111111111111111111111\n"
		cmdFixedNone, flagsFixedNone := newTestCommitMsgCmd(t, "", "--fixed", "none")
		resFixedNone, err := ResolveAndValidateCommitMessageFlags(cmdFixedNone, &flagsFixedNone, "gh pr push")
		if err != nil {
			t.Fatal(err)
		}
		gotFixedNone, err := BuildUpdatedCommitMessage(bugOrig, "Bug: b/{id}", resFixedNone)
		if err != nil {
			t.Fatal(err)
		}
		wantFixedNone := "pw_foo: Clear bug\n\nFixed: None\nChange-Id: I1111111111111111111111111111111111111111\n"
		if gotFixedNone != wantFixedNone {
			t.Errorf("got:\n%q\nwant:\n%q", gotFixedNone, wantFixedNone)
		}
	})

	t.Run("deduplicates bug ID supplied in body trailer block", func(t *testing.T) {
		cmd, flags := newTestCommitMsgCmd(t, "", "-b", "Updated prose.\n\nFixed: b/12345")
		resolved, err := ResolveAndValidateCommitMessageFlags(cmd, &flags, "gh pr create")
		if err != nil {
			t.Fatal(err)
		}
		got, err := BuildUpdatedCommitMessage(orig, "Bug: b/{id}", resolved)
		if err != nil {
			t.Fatal(err)
		}
		want := "pw_foo: Original title\n\nUpdated prose.\n\nFixed: b/12345\nBug: b/99999\nChange-Id: I1111111111111111111111111111111111111111\n"
		if got != want {
			t.Errorf("got:\n%q\nwant:\n%q", got, want)
		}
	})

	t.Run("strips Bug None when setting real Fixed ID", func(t *testing.T) {
		singleOrig := "pw_foo: Draft\n\nBug: None\nChange-Id: I1111111111111111111111111111111111111111\n"
		cmd, flags := newTestCommitMsgCmd(t, "", "--fixed", "567763970")
		resolved, err := ResolveAndValidateCommitMessageFlags(cmd, &flags, "gh pr create")
		if err != nil {
			t.Fatal(err)
		}
		got, err := BuildUpdatedCommitMessage(singleOrig, "Bug: b/{id}", resolved)
		if err != nil {
			t.Fatal(err)
		}
		want := "pw_foo: Draft\n\nFixed: b/567763970\nChange-Id: I1111111111111111111111111111111111111111\n"
		if got != want {
			t.Errorf("got:\n%q\nwant:\n%q", got, want)
		}
	})

	t.Run("formats trailers with Fuchsia profile template", func(t *testing.T) {
		fuchsiaOrig := "zircon: Fix timer\n\nOld body.\n\nBug: fxbug.dev/424242\nChange-Id: I2222222222222222222222222222222222222222\n"
		cmd, flags := newTestCommitMsgCmd(t, "", "-b", "Updated Fuchsia body.", "--fixed", "888888")
		resolved, err := ResolveAndValidateCommitMessageFlags(cmd, &flags, "gh pr create")
		if err != nil {
			t.Fatal(err)
		}
		got, err := BuildUpdatedCommitMessage(fuchsiaOrig, "Bug: fxbug.dev/{id}", resolved)
		if err != nil {
			t.Fatal(err)
		}
		want := "zircon: Fix timer\n\nUpdated Fuchsia body.\n\nBug: fxbug.dev/424242\nFixed: fxbug.dev/888888\nChange-Id: I2222222222222222222222222222222222222222\n"
		if got != want {
			t.Errorf("got:\n%q\nwant:\n%q", got, want)
		}
	})

	t.Run("allows explicit Change-Id override in body", func(t *testing.T) {
		cmdOverride, flagsOverride := newTestCommitMsgCmd(t, "", "-b", "New prose.\n\nChange-Id: I9999999999999999999999999999999999999999")
		resOverride, err := ResolveAndValidateCommitMessageFlags(cmdOverride, &flagsOverride, "gh pr push")
		if err != nil {
			t.Fatalf("unexpected validation error for valid Change-Id override: %v", err)
		}
		got, err := BuildUpdatedCommitMessage(orig, "Bug: b/{id}", resOverride)
		if err != nil {
			t.Fatalf("unexpected error for explicit valid Change-Id override: %v", err)
		}
		if strings.Count(got, "Change-Id:") != 1 {
			t.Errorf("expected single Change-Id trailer, got:\n%s", got)
		}
		if ExtractChangeID(got) != "I9999999999999999999999999999999999999999" {
			t.Errorf("expected explicit Change-Id override to win, got:\n%s", got)
		}
	})
}

func TestBuildInitialCommitMessage(t *testing.T) {
	t.Run("requires --title when creating fresh commit", func(t *testing.T) {
		cmd, flags := newTestCommitMsgCmd(t, "", "--bug", "123456")
		resolved, err := ResolveAndValidateCommitMessageFlags(cmd, &flags, "gh pr create")
		if err != nil {
			t.Fatal(err)
		}
		if _, err := BuildInitialCommitMessage("Bug: b/{id}", resolved); err == nil || !strings.Contains(err.Error(), "without --title") {
			t.Fatalf("expected missing --title error, got: %v", err)
		}
	})

	t.Run("builds initial message with profile trailer format", func(t *testing.T) {
		cmd, flags := newTestCommitMsgCmd(t, "", "-t", "pw_ghish: Initial commit", "-b", "Detailed explanation.", "--bug", "123456")
		resolved, err := ResolveAndValidateCommitMessageFlags(cmd, &flags, "gh pr create")
		if err != nil {
			t.Fatal(err)
		}
		got, err := BuildInitialCommitMessage("Bug: fxbug.dev/{id}", resolved)
		if err != nil {
			t.Fatal(err)
		}
		want := "pw_ghish: Initial commit\n\nDetailed explanation.\n\nBug: fxbug.dev/123456\n"
		if got != want {
			t.Errorf("got:\n%q\nwant:\n%q", got, want)
		}
	})
}
