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
	"fmt"
	"os"
	"regexp"
	"slices"
	"strconv"
	"strings"

	"github.com/spf13/cobra"
)

// CommitMessageFlags holds structured CLI flags for creating or updating local
// Git commit messages and Gerrit bug trailers without clobbering Change-Id or
// existing footers.
type CommitMessageFlags struct {
	Title    string
	Body     string
	BodyFile string
	Bug      string
	Fixed    string
}

type resolvedTrailerFlag struct {
	set    bool
	isNone bool
	ids    []int64
}

// ResolvedCommitMessageFlags holds validated commit-message inputs and parsed
// bug trailer flags ready to apply to a new or existing local commit message.
type ResolvedCommitMessageFlags struct {
	HasTitle bool
	Title    string
	HasBody  bool
	Body     string
	bug      resolvedTrailerFlag
	fixed    resolvedTrailerFlag
}

type trailerFlagSpec struct {
	flag   string
	key    string
	value  string
	closes bool
}

func trailerFlagSpecs(flags *CommitMessageFlags) [2]trailerFlagSpec {
	return [2]trailerFlagSpec{
		{flag: "bug", key: "Bug", value: flags.Bug, closes: false},
		{flag: "fixed", key: "Fixed", value: flags.Fixed, closes: true},
	}
}

// AddCommitMessageFlags registers shared commit-message and trailer flags on cmd.
func AddCommitMessageFlags(cmd *cobra.Command) {
	//ghish:flag mode=upstream gh="pr create --title"
	cmd.Flags().StringP("title", "t", "", "Title for the change (first line of commit message)")
	//ghish:flag mode=upstream gh="pr create --body"
	cmd.Flags().StringP("body", "b", "", "Prose body for the change (preserves existing trailers)")
	//ghish:flag mode=upstream gh="pr create --body-file"
	cmd.Flags().StringP("body-file", "F", "", "Read prose body from file (use \"-\" to read from standard input)")
	//ghish:flag mode=extension reason="Set Gerrit Bug: trailer"
	cmd.Flags().String("bug", "", "Set the Bug: trailer without closing on submit (e.g. b/<id>, URL, CSV, or \"none\") (ghish-only)")
	//ghish:flag mode=extension reason="Set Gerrit Fixed: trailer"
	cmd.Flags().String("fixed", "", "Set the Fixed: trailer and close on submit (e.g. b/<id>, URL, CSV, or \"none\") (ghish-only)")
}

// ParseCommitMessageFlags reads CommitMessageFlags from cmd.
func ParseCommitMessageFlags(cmd *cobra.Command) CommitMessageFlags {
	title, _ := cmd.Flags().GetString("title")
	body, _ := cmd.Flags().GetString("body")
	bodyFile, _ := cmd.Flags().GetString("body-file")
	bug, _ := cmd.Flags().GetString("bug")
	fixed, _ := cmd.Flags().GetString("fixed")
	return CommitMessageFlags{
		Title:    title,
		Body:     body,
		BodyFile: bodyFile,
		Bug:      bug,
		Fixed:    fixed,
	}
}

var commitMessageFlagNames = [...]string{"title", "body", "body-file", "bug", "fixed"}

// AddAsDisabled registers commit-message flags on editCmd as hidden
// unsupported-guard flags so callers receive an actionable redirect error
// instead of an unknown-flag error while `--help` only lists supported flags.
func (f *CommitMessageFlags) AddAsDisabled(editCmd *cobra.Command) {
	//ghish:flag mode=unsupported-guard gh="pr edit --title"
	editCmd.Flags().StringVarP(&f.Title, "title", "t", "", "New commit title (first line of commit message)")
	//ghish:flag mode=unsupported-guard gh="pr edit --body"
	editCmd.Flags().StringVarP(&f.Body, "body", "b", "", "New commit body")
	//ghish:flag mode=unsupported-guard gh="pr edit --body-file"
	editCmd.Flags().StringVarP(&f.BodyFile, "body-file", "F", "", "Read commit body from file")
	//ghish:flag mode=unsupported-guard reason="Hidden guard redirecting --bug to gh pr push"
	editCmd.Flags().StringVar(&f.Bug, "bug", "", "Set the Bug: trailer")
	//ghish:flag mode=unsupported-guard reason="Hidden guard redirecting --fixed to gh pr push"
	editCmd.Flags().StringVar(&f.Fixed, "fixed", "", "Set the Fixed: trailer")
	for _, name := range commitMessageFlagNames {
		_ = editCmd.Flags().MarkHidden(name)
	}
}

// HasAny reports whether any commit-message flag was explicitly provided on cmd.
func (f CommitMessageFlags) HasAny(cmd *cobra.Command) bool {
	for _, name := range commitMessageFlagNames {
		if cmd.Flags().Changed(name) {
			return true
		}
	}
	return false
}

var githubHashIssueTokenRegex = regexp.MustCompile(`^#(\d+)$`)

func parseTrailerFlag(raw, flagName, exampleCmdPrefix string) (resolvedTrailerFlag, error) {
	trimmed := strings.TrimSpace(raw)
	if trimmed == "" {
		return resolvedTrailerFlag{}, fmt.Errorf(
			"--%s requires a value\n\n"+
				"Accepted forms: a bare number (123456), a Buganizer ID (b/123456), an issue\n"+
				"URL (https://issuetracker.google.com/issues/123456), a comma-separated list, or\n"+
				"\"none\".\n\nExample:\n  %s --%s b/123456",
			flagName, exampleCmdPrefix, flagName,
		)
	}
	if err := CheckGitHubIssueSyntax(raw, "--"+flagName); err != nil {
		return resolvedTrailerFlag{}, err
	}

	if strings.EqualFold(trimmed, "none") {
		return resolvedTrailerFlag{set: true, isNone: true}, nil
	}

	tokens := bugChainSeparatorRegex.Split(trimmed, -1)
	var ids []int64
	for _, tok := range tokens {
		tok = strings.TrimSpace(tok)
		if tok == "" {
			continue
		}
		if m := githubHashIssueTokenRegex.FindStringSubmatch(tok); m != nil {
			return resolvedTrailerFlag{}, GitHubIssueSyntaxError([]GitHubIssueRef{
				{Match: tok, Keyword: "#" + m[1], Number: m[1]},
			}, "--"+flagName)
		}
		id, err := ParseIssueID(tok)
		if err != nil {
			return resolvedTrailerFlag{}, fmt.Errorf(
				"invalid --%s value %q: %w\n\n"+
					"Example:\n  %s --%s b/123456\n\n"+
					"To find existing issues:\n  gh issue list",
				flagName, tok, err, exampleCmdPrefix, flagName,
			)
		}
		if !slices.Contains(ids, id) {
			ids = append(ids, id)
		}
	}
	if len(ids) == 0 {
		return resolvedTrailerFlag{}, fmt.Errorf(
			"invalid --%s value %q: no issue identifiers found\n\n"+
				"Example:\n  %s --%s b/123456\n\n"+
				"To find existing issues:\n  gh issue list",
			flagName, raw, exampleCmdPrefix, flagName,
		)
	}

	return resolvedTrailerFlag{set: true, ids: ids}, nil
}

func formatResolvedTrailerFlag(key string, tf resolvedTrailerFlag, trailerFormat string) string {
	if tf.isNone {
		return NormalizeTrailerWithFormat(key+": None", trailerFormat)
	}
	tokens := make([]string, 0, len(tf.ids))
	for _, id := range tf.ids {
		tokens = append(tokens, "b/"+strconv.FormatInt(id, 10))
	}
	return NormalizeTrailerWithFormat(key+": "+strings.Join(tokens, ", "), trailerFormat)
}

// ResolveAndValidateCommitMessageFlags resolves stdin/file body inputs and
// validates title, body, Change-Id syntax, GitHub issue syntax, and trailer
// flag constraints upfront before touching local Git state or Gerrit.
func ResolveAndValidateCommitMessageFlags(
	cmd *cobra.Command,
	flags *CommitMessageFlags,
	exampleCmdPrefix string,
) (*ResolvedCommitMessageFlags, error) {
	if flags == nil {
		return nil, fmt.Errorf("internal error: commit message flags cannot be nil")
	}

	titleChanged := cmd.Flags().Changed("title")
	if titleChanged {
		if strings.TrimSpace(flags.Title) == "" {
			return nil, fmt.Errorf("cannot set an empty commit title")
		}
		if strings.ContainsAny(flags.Title, "\r\n") {
			return nil, fmt.Errorf(
				"--title (-t) must be a single line (cannot contain newlines).\n\n"+
					"Why: --title sets only the subject line of the Git commit message; multi-line text belongs in the commit body.\n"+
					"Pass multi-line prose via --body (-b) or --body-file (-F):\n"+
					"  %s --title \"module: Summary\" --body \"Detailed explanation...\"\n"+
					"  %s --title \"module: Summary\" --body-file body.txt",
				exampleCmdPrefix, exampleCmdPrefix,
			)
		}
	}

	bodyChanged := cmd.Flags().Changed("body")
	bodyFileChanged := cmd.Flags().Changed("body-file")
	if bodyChanged && bodyFileChanged {
		return nil, fmt.Errorf("cannot specify both --body (-b) and --body-file (-F)")
	}

	hasBody := bodyChanged || bodyFileChanged
	bodySourceLabel := "--body"
	var resolvedBody string
	if bodyFileChanged {
		bodySourceLabel = "--body-file"
		if flags.BodyFile == "-" {
			data, readErr := readBoundedBody(cmd.InOrStdin(), maxHTTPBodyBytes)
			if readErr != nil {
				return nil, fmt.Errorf("failed to read commit body from standard input (-F -): %w", readErr)
			}
			resolvedBody = string(data)
		} else {
			if strings.TrimSpace(flags.BodyFile) == "" {
				return nil, fmt.Errorf("--body-file (-F) requires a file path or \"-\" for standard input")
			}
			f, openErr := os.Open(flags.BodyFile)
			if openErr != nil {
				return nil, fmt.Errorf("failed to read commit body file %q: %w", flags.BodyFile, openErr)
			}
			data, readErr := readBoundedBody(f, maxHTTPBodyBytes)
			_ = f.Close()
			if readErr != nil {
				return nil, fmt.Errorf("failed to read commit body file %q: %w", flags.BodyFile, readErr)
			}
			resolvedBody = string(data)
		}
	} else if bodyChanged {
		resolvedBody = flags.Body
	}

	if err := CheckGitHubIssueSyntax(flags.Title, "--title"); err != nil {
		return nil, err
	}
	if err := CheckGitHubIssueSyntax(resolvedBody, bodySourceLabel); err != nil {
		return nil, err
	}

	bodyBugTrailerKey := make(map[bool]string)
	if hasBody && resolvedBody != "" {
		if err := ValidateChangeIDFooters("subject\n\n"+resolvedBody, bodySourceLabel); err != nil {
			return nil, err
		}
		parsedBody := ParseCommitMessage("subject\n\n" + resolvedBody)
		for _, p := range parsedBody.Paragraphs[parsedBody.ActiveFooterStart():] {
			for _, tr := range p.Trailers {
				if tr.IsKeylessFooter {
					continue
				}
				if rawKey, _, closes, isBug := parseBugTrailer(tr.FirstLine()); isBug {
					if bodyBugTrailerKey[closes] == "" {
						bodyBugTrailerKey[closes] = rawKey
					}
				}
			}
		}
	}

	var parsedFlags [2]resolvedTrailerFlag
	for i, tf := range trailerFlagSpecs(flags) {
		if !cmd.Flags().Changed(tf.flag) {
			continue
		}
		parsed, parseErr := parseTrailerFlag(tf.value, tf.flag, exampleCmdPrefix)
		if parseErr != nil {
			return nil, parseErr
		}
		parsedFlags[i] = parsed
		if existingKey := bodyBugTrailerKey[tf.closes]; existingKey != "" {
			return nil, fmt.Errorf(
				"--%s and %s both set a %s: trailer\n\n"+
					"Only one of them can win, and picking for you would silently discard the\n"+
					"other. Remove the %s: line from %s, or drop --%s.",
				tf.flag, bodySourceLabel, tf.key, existingKey, bodySourceLabel, tf.flag,
			)
		}
	}

	if parsedFlags[0].set && parsedFlags[1].set {
		for _, id := range parsedFlags[0].ids {
			if slices.Contains(parsedFlags[1].ids, id) {
				return nil, fmt.Errorf(
					"issue %d cannot be specified in both --bug and --fixed.\n\n"+
						"Why: 'Bug:' links an issue without closing it on submit, while 'Fixed:' closes the issue automatically when the CL lands.\n"+
						"Pick one:\n"+
						"  %s --fixed %d\n"+
						"  %s --bug %d",
					id, exampleCmdPrefix, id, exampleCmdPrefix, id,
				)
			}
		}
	}

	return &ResolvedCommitMessageFlags{
		HasTitle: titleChanged,
		Title:    strings.TrimSpace(flags.Title),
		HasBody:  hasBody,
		Body:     resolvedBody,
		bug:      parsedFlags[0],
		fixed:    parsedFlags[1],
	}, nil
}

// BuildUpdatedCommitMessage updates origMessage with any explicitly changed
// flags in resolved while preserving Change-Id and existing trailers,
// formatting bug/fix trailers with trailerFormat, and deduplicating issue IDs
// across Bug: <-> Fixed: transitions.
func BuildUpdatedCommitMessage(
	origMessage, trailerFormat string,
	resolved *ResolvedCommitMessageFlags,
) (string, error) {
	if resolved == nil {
		return "", fmt.Errorf("internal error: resolved commit message flags cannot be nil")
	}

	msg := ParseCommitMessage(origMessage)
	if resolved.HasTitle {
		msg.Subject = resolved.Title
	}
	if resolved.HasBody {
		origTrailers := msg.ExtractTrailersWithFormat(trailerFormat)
		mergedBody := MergeTrailersWithFormat(resolved.Body, origTrailers, trailerFormat)
		msg = ParseCommitMessage(msg.Subject + "\n\n" + strings.TrimLeft(mergedBody, "\r\n"))
	}

	b, fx := resolved.bug, resolved.fixed
	dropNonClosing := (b.set && b.isNone) || (fx.set && fx.isNone && !b.set)
	dropClosing := (fx.set && fx.isNone) || (b.set && b.isNone && !fx.set)
	var downgradeIDs []int64
	if b.set && !b.isNone {
		downgradeIDs = b.ids
	}
	pruneActiveBugTrailers(msg, dropNonClosing, dropClosing, downgradeIDs, trailerFormat)

	for _, entry := range []struct {
		key string
		tf  resolvedTrailerFlag
	}{
		{key: "Bug", tf: b},
		{key: "Fixed", tf: fx},
	} {
		if !entry.tf.set {
			continue
		}
		formattedLine := formatResolvedTrailerFlag(entry.key, entry.tf, trailerFormat)
		if err := appendActiveBugTrailer(msg, formattedLine); err != nil {
			return "", fmt.Errorf("error setting the %s: trailer: %w", entry.key, err)
		}
	}

	msg.NormalizeBugFooters(trailerFormat)
	return msg.Format(), nil
}

// pruneActiveBugTrailers strips cleared bug/fix trailers or removes downgraded
// issue IDs from closing trailers within active trailing footer paragraphs only
// (`msg.Paragraphs[msg.ActiveFooterStart():]`).
func pruneActiveBugTrailers(
	msg *CommitMessage,
	dropNonClosing bool,
	dropClosing bool,
	downgradeIDs []int64,
	trailerFormat string,
) {
	if msg == nil || (!dropNonClosing && !dropClosing && len(downgradeIDs) == 0) {
		return
	}
	footerStart := msg.ActiveFooterStart()
	keptParas := make([]CommitParagraph, 0, len(msg.Paragraphs))
	keptParas = append(keptParas, msg.Paragraphs[:footerStart]...)
	for _, p := range msg.Paragraphs[footerStart:] {
		keptTrailers := make([]CommitTrailer, 0, len(p.Trailers))
		for _, tr := range p.Trailers {
			if tr.IsKeylessFooter {
				keptTrailers = append(keptTrailers, tr)
				continue
			}
			_, val, closes, isBug := parseBugTrailer(tr.FirstLine())
			if !isBug {
				keptTrailers = append(keptTrailers, tr)
				continue
			}
			if (!closes && dropNonClosing) || (closes && dropClosing) {
				continue
			}
			if closes && len(downgradeIDs) > 0 {
				var rem []string
				removed := false
				for _, tok := range bugChainSeparatorRegex.Split(strings.TrimSpace(val), -1) {
					tok = strings.TrimSpace(tok)
					if tok == "" {
						continue
					}
					id, err := ParseIssueID(tok)
					if err == nil && slices.Contains(downgradeIDs, id) {
						removed = true
						continue
					}
					if err == nil {
						rem = append(rem, FormatBugIDWithFormat(id, trailerFormat))
					} else {
						rem = append(rem, tok)
					}
				}
				if removed {
					if len(rem) > 0 {
						tr.SetFirstLine("Fixed: " + strings.Join(rem, ", "))
						keptTrailers = append(keptTrailers, tr)
					}
					continue
				}
			}
			keptTrailers = append(keptTrailers, tr)
		}
		if len(keptTrailers) > 0 {
			p.Trailers = keptTrailers
			keptParas = append(keptParas, p)
		}
	}
	msg.Paragraphs = keptParas
}

// appendActiveBugTrailer appends trailerLine into the active trailing footer
// block right above Change-Id: without overwriting existing same-key Bug:/Fixed:
// trailers.
func appendActiveBugTrailer(msg *CommitMessage, trailerLine string) error {
	if msg == nil {
		return fmt.Errorf("internal error: CommitMessage is nil")
	}
	newTr, err := ParseTrailerEntry(trailerLine)
	if err != nil {
		return err
	}
	if msg.Subject == "" && len(msg.Paragraphs) == 0 {
		msg.Paragraphs = []CommitParagraph{
			{
				IsTrailerBlock: true,
				Trailers:       []CommitTrailer{newTr},
			},
		}
		return nil
	}

	footerStart := msg.ActiveFooterStart()
	if footerStart < len(msg.Paragraphs) {
		for pIdx := footerStart; pIdx < len(msg.Paragraphs); pIdx++ {
			p := &msg.Paragraphs[pIdx]
			for i, tr := range p.Trailers {
				if !tr.IsKeylessFooter && strings.EqualFold(tr.Key, "Change-Id") {
					p.Trailers = slices.Insert(p.Trailers, i, newTr)
					return nil
				}
			}
		}
		last := &msg.Paragraphs[len(msg.Paragraphs)-1]
		last.Trailers = append(last.Trailers, newTr)
		return nil
	}

	msg.Paragraphs = append(msg.Paragraphs, CommitParagraph{
		LeadingBlankLines: []string{""},
		IsTrailerBlock:    true,
		Trailers:          []CommitTrailer{newTr},
	})
	return nil
}

// BuildInitialCommitMessage builds a brand-new commit message when HEAD sits on
// an upstream branch tip (or unborn branch) with uncommitted changes.
func BuildInitialCommitMessage(
	trailerFormat string,
	resolved *ResolvedCommitMessageFlags,
) (string, error) {
	if resolved == nil {
		return "", fmt.Errorf("internal error: resolved commit message flags cannot be nil")
	}
	if !resolved.HasTitle || resolved.Title == "" {
		return "", fmt.Errorf(
			"cannot create a new commit without --title (-t): HEAD is at the upstream branch tip with no unpushed local commit to amend.\n\n" +
				"Why: Creating a fresh commit from uncommitted working-tree changes requires a commit subject line.\n" +
				"Pass --title (-t) along with your flags:\n" +
				"  gh pr create -t \"module: Summary of change\" --bug b/123456\n\n" +
				"Or commit locally first and then upload:\n" +
				"  git commit -m \"module: Summary of change\"\n" +
				"  gh pr create",
		)
	}
	return BuildUpdatedCommitMessage("", trailerFormat, resolved)
}
