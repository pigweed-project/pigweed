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
	"regexp"
	"strings"
)

// gitHubClosingKeywordRegex matches GitHub's issue-closing syntax: one of its
// closing keywords followed by `#<number>`.
//
// GitHub accepts close/closes/closed, fix/fixes/fixed and
// resolve/resolves/resolved. The colon is optional here even though GitHub
// does not require one, because `Fixed: #456` -- someone reaching for a Gerrit
// trailer but supplying a GitHub number -- is exactly as broken and worth
// catching.
//
// The `#` is required. Without it this would match `Fixed: b/12345`, which is
// the correct Gerrit spelling and must never be flagged.
var gitHubClosingKeywordRegex = regexp.MustCompile(
	`(?i)\b(close[sd]?|fix(?:e[sd])?|resolve[sd]?)\b\s*:?\s+#(\d+)\b`)

// GitHubIssueRef is a GitHub-style issue reference found in text that is
// destined for a Gerrit commit message.
type GitHubIssueRef struct {
	// Match is the matched text, e.g. "Fixes #456".
	Match string
	// Keyword is the closing keyword as written, e.g. "Fixes".
	Keyword string
	// Number is the GitHub issue number, e.g. "456".
	Number string
}

// FindGitHubIssueRefs returns the GitHub issue-closing references in text.
//
// This exists because the single most transferable piece of `gh` knowledge is
// also the one that silently does nothing on Gerrit. On GitHub, a pull request
// body containing "Fixes #456" closes issue 456 on merge. Gerrit has no such
// parsing: the line is ordinary prose, the bug is never linked, and nothing
// reports a problem. Detecting it is the difference between a broken link and
// an actionable error.
//
// Two kinds of text are deliberately not matched, because a false refusal
// blocks somebody who did nothing wrong:
//
//   - A bare "#456" with no keyword. It is far more often prose ("see #3 of
//     the design doc") than an issue link.
//   - Indented lines, which are code samples. This matches how trailers are
//     recognized elsewhere, where indentation likewise means "content".
func FindGitHubIssueRefs(text string) []GitHubIssueRef {
	if text == "" {
		return nil
	}

	var refs []GitHubIssueRef
	for _, line := range strings.Split(strings.ReplaceAll(text, "\r\n", "\n"), "\n") {
		if strings.HasPrefix(line, " ") || strings.HasPrefix(line, "\t") {
			continue
		}
		for _, m := range gitHubClosingKeywordRegex.FindAllStringSubmatch(line, -1) {
			refs = append(refs, GitHubIssueRef{Match: m[0], Keyword: m[1], Number: m[2]})
		}
	}
	return refs
}

// GitHubIssueSyntaxError builds the diagnostic for GitHub issue syntax found
// in where (for example "--body" or "the HEAD commit message").
//
// The references are reported rather than rewritten. A GitHub issue number is
// repo-local and usually small, while a Buganizer ID is usually eight or nine
// digits, so translating "#456" into "b/456" would link a real but unrelated
// bug. Silently attaching the wrong bug is worse than attaching none.
func GitHubIssueSyntaxError(refs []GitHubIssueRef, where string) error {
	if len(refs) == 0 {
		return nil
	}

	var quoted []string
	seen := make(map[string]bool)
	for _, ref := range refs {
		if seen[ref.Match] {
			continue
		}
		seen[ref.Match] = true
		quoted = append(quoted, "  "+ref.Match)
	}

	return fmt.Errorf(
		"%s contains GitHub issue syntax, which has no effect on Gerrit:\n\n%s\n\n"+
			"On GitHub, %q in a pull request body closes that issue when the PR merges.\n"+
			"Gerrit does not parse it: the line is ordinary prose, so the bug is never\n"+
			"linked, never closed, and nothing warns you. Buganizer links come from a\n"+
			"commit message trailer instead:\n\n"+
			"  Fixed: b/<id>    links the bug and closes it when the change is submitted\n"+
			"  Bug: b/<id>      links the bug without closing it\n\n"+
			"gh-ish can write the trailer for you, and accepts a bare number, a b/ ID, or\n"+
			"an issue URL:\n\n"+
			"  gh pr edit <change> --fixed b/<id>\n"+
			"  gh pr edit <change> --bug b/<id>\n\n"+
			"This is not translated automatically on purpose: #%s is a repo-local GitHub\n"+
			"number, so b/%s would almost certainly be an unrelated Buganizer issue.",
		where, strings.Join(quoted, "\n"), refs[0].Match, refs[0].Number, refs[0].Number)
}

// CheckGitHubIssueSyntax returns an actionable error if text uses GitHub
// issue-closing syntax. It is the single entry point commands should call, so
// that every path that accepts commit message text rejects it identically.
func CheckGitHubIssueSyntax(text, where string) error {
	return GitHubIssueSyntaxError(FindGitHubIssueRefs(text), where)
}
