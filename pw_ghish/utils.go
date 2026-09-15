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
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
	"time"

	"github.com/spf13/cobra"
)

var (
	changeIDRegex        = regexp.MustCompile(`(?m)^Change-Id:\s+I[0-9a-fA-F]{40}\s*$`)
	changeIDCaptureRegex = regexp.MustCompile(`(?m)^Change-Id:\s+(I[0-9a-fA-F]{40})\s*$`)
	trailerRegex         = regexp.MustCompile(`^[A-Za-z][A-Za-z0-9_-]*:\s+.*$`)

	// gerritURLPlusRegex matches URLs containing /+/ followed by changeID (number or Change-Id) and optional patchset.
	// e.g. https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/472267/3
	// e.g. https://pigweed-review.googlesource.com/+/472267/2
	gerritURLPlusRegex = regexp.MustCompile(`/\+/([0-9]+|I[0-9a-fA-F]{40})(?:/([0-9]+))?/?$`)

	// gerritChangesURLRegex matches Gerrit REST API URLs like /changes/<id>(/revisions/<rev>)?
	gerritChangesURLRegex = regexp.MustCompile(`/changes/([0-9]+|I[0-9a-fA-F]{40}|[^/]+~[^/]+~I[0-9a-fA-F]{40})(?:/revisions/([0-9]+|current))?/?$`)

	// gerritDirectURLRegex matches short Gerrit web URLs like https://<host>/<change-number>(/<rev>)?
	gerritDirectURLRegex = regexp.MustCompile(`^https?://[^/]+/([0-9]+)(?:/([0-9]+))?/?$`)

	// gerritShortlinkRegex matches pwrev/12345, pwrev.dev/i/12345, fxrev/12345, fxrev.dev/i/12345, crrev.com/c/12345, https://pwrev.dev/12345
	gerritShortlinkRegex = regexp.MustCompile(`^(?:https?://)?(?:[a-zA-Z0-9-]+\.)?(?:pwrev|fxrev|crrev)(?:\.dev|\.com)?(?:/[ci])?/([0-9]+)(?:/([0-9]+))?/?$`)

	// branchChangeNumRegex matches branch naming conventions that encode a change number,
	// e.g. cl/472267, cl/472267/2, change-472267, review/472267, patch/472267, pr/472267.
	branchChangeNumRegex = regexp.MustCompile(`^(?:cl|change|review|cr|patch|pr)[/-]([0-9]+)(?:/([0-9]+))?$`)
)

// ExtractChangeID extracts the Gerrit Change-Id from a commit message footer, or returns "" if none.
func ExtractChangeID(commitMsg string) string {
	m := changeIDCaptureRegex.FindStringSubmatch(commitMsg)
	if len(m) > 1 {
		return m[1]
	}
	return ""
}

// cherryPickFooterRegex matches the provenance line that Gerrit and
// `git cherry-pick -x` append inside the trailer block. It is not a `key:
// value` trailer, but it belongs to the block and must not disqualify it.
var cherryPickFooterRegex = regexp.MustCompile(`^\(cherry picked from commit [0-9a-fA-F]+\)$`)

// ExtractTrailers returns the trailer lines of a commit message that must
// survive a rewrite of the body.
//
// A trailer paragraph is a blank-line-delimited block, excluding the subject,
// in which every line is either a `Key: value` trailer, an indented
// continuation of the preceding trailer, or a recognized footer such as the
// cherry-pick provenance line -- and at least one line is a trailer. Every
// trailer paragraph in the message contributes, not just the last one.
//
// Scanning all paragraphs is what makes this lossless. Looking only at the
// final paragraph meant that a message ending in a prose paragraph such as
// "Note: reviewers please look at the retry logic." treated that prose as the
// trailer block and silently discarded the real Change-Id and Bug lines above
// it. A change that loses its Change-Id cannot be updated in place; the next
// push creates a duplicate CL.
//
// Requiring the whole paragraph to qualify is what keeps it honest in the
// other direction: a paragraph of ordinary prose is not promoted into
// trailers just because one of its lines happens to contain a colon.
//
// Trailer keys are preserved exactly as written. Only the value of a
// bug-style trailer is canonicalized, so `Bug: <url>` becomes `Bug: b/123`
// while `Fixed:` stays `Fixed:`. Rewriting keys during an unrelated body edit
// silently changes a commit message the caller did not ask to change.
func ExtractTrailers(commitMsg string) []string {
	normalized := strings.ReplaceAll(commitMsg, "\r\n", "\n")
	lines := strings.Split(normalized, "\n")

	// The subject is never a trailer, even though it usually has the exact
	// shape of one ("pw_foo: Add bar").
	if len(lines) < 2 {
		return nil
	}

	var trailers []string
	for _, paragraph := range splitParagraphs(lines[1:]) {
		if found, ok := paragraphTrailers(paragraph); ok {
			trailers = append(trailers, found...)
		}
	}
	return trailers
}

// paragraphRanges returns the [start, end) line index ranges of the
// blank-line-delimited paragraphs in lines, skipping empty ones.
//
// Ranges rather than copies, because editing a commit message in place
// requires knowing *where* a paragraph is. Rebuilding a message from copied
// paragraphs would silently reflow it, collapsing runs of blank lines the
// author put there.
func paragraphRanges(lines []string) [][2]int {
	var ranges [][2]int
	start := -1
	for i, l := range lines {
		if strings.TrimSpace(l) == "" {
			if start >= 0 {
				ranges = append(ranges, [2]int{start, i})
				start = -1
			}
			continue
		}
		if start < 0 {
			start = i
		}
	}
	if start >= 0 {
		ranges = append(ranges, [2]int{start, len(lines)})
	}
	return ranges
}

// splitParagraphs groups lines into blank-line-delimited paragraphs, dropping
// empty ones.
func splitParagraphs(lines []string) [][]string {
	ranges := paragraphRanges(lines)
	if len(ranges) == 0 {
		return nil
	}
	paragraphs := make([][]string, 0, len(ranges))
	for _, r := range ranges {
		paragraphs = append(paragraphs, lines[r[0]:r[1]])
	}
	return paragraphs
}

// paragraphTrailers reports whether a paragraph is a trailer block, and if so
// returns its trailer lines with continuations folded into the trailer they
// belong to.
func paragraphTrailers(paragraph []string) ([]string, bool) {
	var trailers []string
	sawTrailer := false

	for _, l := range paragraph {
		trimmed := strings.TrimSpace(strings.TrimRight(l, "\r"))
		switch {
		// An indented line continues the previous trailer. This is checked
		// first so that an indented `key: value` stays a continuation instead
		// of being mistaken for a new trailer and losing its indentation.
		case len(trailers) > 0 && (strings.HasPrefix(l, " ") || strings.HasPrefix(l, "\t")):
			trailers[len(trailers)-1] += "\n" + strings.TrimRight(l, "\r")
		case trailerRegex.MatchString(trimmed):
			trailers = append(trailers, normalizeTrailerValue(trimmed))
			sawTrailer = true
		case cherryPickFooterRegex.MatchString(trimmed):
			trailers = append(trailers, trimmed)
		default:
			// Ordinary prose: this paragraph is body text, not trailers.
			return nil, false
		}
	}

	return trailers, sawTrailer
}

// normalizeTrailerValue canonicalizes the value of a bug-style trailer while
// preserving the key exactly as the author wrote it.
func normalizeTrailerValue(trailerLine string) string {
	key, value, _, ok := parseBugTrailer(trailerLine)
	if !ok {
		return trailerLine
	}
	return key + ": " + NormalizeBugChain(value)
}

// trailerKey returns the key of a trailer line. The second result is false for
// a keyless footer such as the cherry-pick provenance line, which has no key
// to compare on and must be matched whole.
func trailerKey(trailer string) (string, bool) {
	first := trailer
	if i := strings.Index(first, "\n"); i >= 0 {
		first = first[:i]
	}
	if !trailerRegex.MatchString(first) {
		return "", false
	}
	// trailerRegex requires a colon, so the split always yields two parts.
	return strings.TrimSpace(strings.SplitN(first, ":", 2)[0]), true
}

// DroppedTrailers returns the trailers of origMsg that newMsg does not carry
// forward. It exists so that a command replacing a whole commit message can
// tell the user exactly what it is about to destroy instead of destroying it.
//
// Presence is judged per key, not per line: rewriting `Bug: b/1` to
// `Bug: b/2` is an edit, not a deletion, and reporting it as loss would make
// the check impossible to satisfy. Keyless footers are matched whole.
//
// The test is deliberately generous -- a bare `^Key:` line anywhere in newMsg
// counts -- because the cost of the two errors is not symmetric. A false
// alarm blocks a user who already did the right thing; a false pass silently
// destroys history.
func DroppedTrailers(origMsg, newMsg string) []string {
	var dropped []string
	for _, trailer := range ExtractTrailers(origMsg) {
		key, ok := trailerKey(trailer)
		if !ok {
			if !strings.Contains(newMsg, trailer) {
				dropped = append(dropped, trailer)
			}
			continue
		}
		if !MentionsTrailerKey(newMsg, key) {
			dropped = append(dropped, trailer)
		}
	}
	return dropped
}

// MentionsTrailerKey reports whether text has a line beginning with `key:`.
//
// This is the tolerant test, used to answer "has the user already dealt with
// this trailer?" -- for refusing to drop a trailer, and for detecting that a
// flag and a hand-written message both try to set the same one. It accepts
// leading whitespace on purpose, so that a slightly misindented line still
// counts as the user having addressed it.
//
// It is deliberately not the test MergeTrailers uses, which requires the line
// to sit flush against the left margin because there an indented line is a
// code sample whose indentation must survive.
func MentionsTrailerKey(text, key string) bool {
	matched, _ := regexp.MatchString(`(?mi)^\s*`+regexp.QuoteMeta(key)+`:`, text)
	return matched
}

// UpsertTrailer returns commitMsg with trailerLine as the one and only trailer
// for its key: an existing trailer with that key is replaced where it stands,
// duplicates are collapsed, and if the key is absent the line is appended to
// the trailer block (creating one if the message has none).
//
// Replacing in place rather than appending keeps the trailer block in the
// order the author chose. Collapsing duplicates is what makes `--bug` mean
// what it says: leaving a second, stale `Bug:` line behind would keep linking
// a bug the user just replaced.
//
// Only trailer paragraphs are considered, so a line of prose that happens to
// start with the key is left alone. An error is returned if trailerLine is not
// a `Key: value` line, since that can only be a programming mistake and
// silently returning the message unchanged would hide it.
func UpsertTrailer(commitMsg, trailerLine string) (string, error) {
	key, ok := trailerKey(trailerLine)
	if !ok {
		return "", fmt.Errorf("internal error: %q is not a `Key: value` trailer", trailerLine)
	}

	normalized := strings.ReplaceAll(commitMsg, "\r\n", "\n")
	body := strings.TrimRight(normalized, "\n")
	if body == "" {
		return trailerLine + "\n", nil
	}

	lines := strings.Split(body, "\n")
	// The subject is never a trailer, even when it has the exact shape of one.
	rest := lines[1:]

	replaceAt := -1
	drop := make(map[int]bool)
	lastTrailerBlockEnd := -1

	for _, r := range paragraphRanges(rest) {
		paragraph := rest[r[0]:r[1]]
		if _, isTrailerBlock := paragraphTrailers(paragraph); !isTrailerBlock {
			continue
		}
		lastTrailerBlockEnd = r[1]

		for i := r[0]; i < r[1]; i++ {
			k, isTrailer := trailerKey(strings.TrimSpace(rest[i]))
			if !isTrailer || !strings.EqualFold(k, key) {
				continue
			}
			if replaceAt < 0 {
				replaceAt = i
			} else {
				drop[i] = true
			}
			// Continuation lines belong to this trailer and go with it;
			// stranding them under a new value would leave a fragment of the
			// old one behind.
			for j := i + 1; j < r[1]; j++ {
				if !strings.HasPrefix(rest[j], " ") && !strings.HasPrefix(rest[j], "\t") {
					break
				}
				drop[j] = true
			}
		}
	}

	out := make([]string, 0, len(lines)+2)
	out = append(out, lines[0])
	for i, l := range rest {
		switch {
		case drop[i]:
			continue
		case i == replaceAt:
			out = append(out, trailerLine)
		default:
			out = append(out, l)
		}
	}

	if replaceAt < 0 {
		if lastTrailerBlockEnd < 0 {
			// No trailer block at all, so start one.
			out = append(out, "", trailerLine)
		} else {
			// No lines were dropped in this branch, so `rest` and `out`
			// indices differ only by the subject line.
			at := lastTrailerBlockEnd + 1
			out = append(out, "")
			copy(out[at+1:], out[at:])
			out[at] = trailerLine
		}
	}

	return strings.Join(out, "\n") + "\n", nil
}

// MergeTrailers appends any trailers from origTrailers whose key is not already defined in newBody.
//
// Lines the caller wrote are left alone unless they are unmistakably trailers.
// A trailer sits flush against the left margin, so matching against the raw
// line rather than a trimmed copy is what protects indented content: Pigweed's
// style guide indents code samples by two spaces, and "  key: value" inside
// one is body text that must keep its indentation.
func MergeTrailers(newBody string, origTrailers []string) string {
	newBodyTrimmed := strings.TrimRight(newBody, "\r\n")

	// Canonicalize bug values on trailer lines the caller supplied. Note the
	// match is against l, not strings.TrimSpace(l): an indented line is
	// content, not a trailer.
	if newBodyTrimmed != "" {
		lines := strings.Split(newBodyTrimmed, "\n")
		for i, l := range lines {
			if trailerRegex.MatchString(l) {
				lines[i] = normalizeTrailerValue(l)
			}
		}
		newBodyTrimmed = strings.Join(lines, "\n")
	}

	var toAppend []string
	for _, orig := range origTrailers {
		tr := normalizeTrailerValue(orig)
		// A line with no `Key:` prefix is a footer such as
		// "(cherry picked from commit abc)". It has no key to deduplicate on,
		// so match the whole line; dropping it would discard provenance.
		key, ok := trailerKey(tr)
		if !ok {
			if !strings.Contains(newBodyTrimmed, tr) {
				toAppend = append(toAppend, tr)
			}
			continue
		}
		pattern := `(?mi)^` + regexp.QuoteMeta(key) + `:\s*`
		if matched, _ := regexp.MatchString(pattern, newBodyTrimmed); !matched {
			toAppend = append(toAppend, tr)
		}
	}

	if len(toAppend) == 0 {
		return newBodyTrimmed + "\n"
	}

	if newBodyTrimmed == "" {
		return strings.Join(toAppend, "\n") + "\n"
	}

	lines := strings.Split(newBodyTrimmed, "\n")
	lastLine := strings.TrimSpace(lines[len(lines)-1])
	if trailerRegex.MatchString(lastLine) {
		return newBodyTrimmed + "\n" + strings.Join(toAppend, "\n") + "\n"
	}

	return newBodyTrimmed + "\n\n" + strings.Join(toAppend, "\n") + "\n"
}

// ParseChangeAndRevision parses a change identifier that optionally contains a patchset number,
// a Gerrit Web/REST URL, a shortlink (e.g. pwrev/12345), or branch-encoded change number.
// Examples:
//
//	"12345/3" -> ("12345", "3")
//	"12345" -> ("12345", "current")
//	"https://.../+/472267/3" -> ("472267", "3")
//	"pwrev/472267" -> ("472267", "current")
//	"cl/472267" -> ("472267", "current")
func ParseChangeAndRevision(arg string) (changeID string, revision string) {
	trimmed := strings.TrimSpace(arg)
	if trimmed == "" {
		return "", "current"
	}

	// Strip query parameters and URL fragments if present
	clean := trimmed
	if idx := strings.IndexAny(clean, "?#"); idx != -1 {
		clean = clean[:idx]
	}
	clean = strings.TrimRight(clean, "/")

	// 1. Check /+/ pattern in Gerrit URLs
	if m := gerritURLPlusRegex.FindStringSubmatch(clean); len(m) > 1 {
		rev := "current"
		if len(m) > 2 && m[2] != "" {
			rev = m[2]
		}
		return m[1], rev
	}

	// 2. Check /changes/ pattern in Gerrit REST/web URLs
	if m := gerritChangesURLRegex.FindStringSubmatch(clean); len(m) > 1 {
		rev := "current"
		if len(m) > 2 && m[2] != "" {
			rev = m[2]
		}
		return m[1], rev
	}

	// 3. Check shortlinks (pwrev/12345, fxrev/12345, crrev.com/c/12345, https://pwrev.dev/12345)
	if m := gerritShortlinkRegex.FindStringSubmatch(clean); len(m) > 1 {
		rev := "current"
		if len(m) > 2 && m[2] != "" {
			rev = m[2]
		}
		return m[1], rev
	}

	// 4. Check direct URLs (https://<host>/12345 or https://<host>/12345/3)
	if m := gerritDirectURLRegex.FindStringSubmatch(clean); len(m) > 1 {
		rev := "current"
		if len(m) > 2 && m[2] != "" {
			rev = m[2]
		}
		return m[1], rev
	}

	// 5. Check branch-style identifiers (cl/12345, change-12345, review/12345)
	if m := branchChangeNumRegex.FindStringSubmatch(clean); len(m) > 1 {
		rev := "current"
		if len(m) > 2 && m[2] != "" {
			rev = m[2]
		}
		return m[1], rev
	}

	// 6. Standard format: "12345/3" or "project~branch~I.../2" or "12345"
	if idx := strings.LastIndex(clean, "/"); idx != -1 {
		rev := clean[idx+1:]
		if rev == "" {
			rev = "current"
		}
		return clean[:idx], rev
	}

	return clean, "current"
}

// HasChangeID returns true if the commit message contains a valid Gerrit Change-Id footer.
func HasChangeID(commitMsg string) bool {
	return changeIDRegex.MatchString(commitMsg)
}

// EnsureChangeID checks the HEAD commit message for a Change-Id footer before pushing to Gerrit.
// If missing, it checks for or downloads the Gerrit commit-msg hook and amends the commit.
func EnsureChangeID(ctx context.Context, cfg *Config, stdout, stderr io.Writer) error {
	if cfg == nil || cfg.Git == nil {
		return fmt.Errorf("EnsureChangeID: git runner not initialized")
	}

	git := cfg.GitClient()
	commitMsg, err := git.HeadCommitMessage(ctx)
	if err != nil {
		return fmt.Errorf("failed to get HEAD commit message: %w", err)
	}

	if commitMsg != "" && HasChangeID(commitMsg) {
		return nil
	}

	if err := git.CheckAmendAllowed(ctx); err != nil {
		return fmt.Errorf("cannot insert Change-Id: %w", err)
	}

	fmt.Fprintln(stderr, "Notice: HEAD commit is missing a Gerrit Change-Id footer.")

	// Check if the commit-msg hook is installed
	gitDir, err := git.GitDir(ctx)
	if err != nil {
		return fmt.Errorf("failed to resolve git directory: %w", err)
	}
	if gitDir == "" {
		return fmt.Errorf("resolved git directory is empty")
	}
	hookPath := filepath.Join(gitDir, "hooks", "commit-msg")

	hookInstalled := false
	if fi, err := os.Stat(hookPath); err == nil && (fi.Mode()&0111 != 0) {
		hookInstalled = true
	} else {
		// Attempt to download the hook from the Gerrit host
		gerritURL, err := cfg.GerritURL(ctx)
		if err != nil || gerritURL == "" {
			fmt.Fprintf(stderr, "Warning: could not resolve Gerrit URL to download commit-msg hook: %v\n", err)
		} else {
			baseURL := strings.TrimSuffix(gerritURL, "/a")
			hookURL := baseURL + "/tools/hooks/commit-msg"

			client := &http.Client{Timeout: 5 * time.Second}
			resp, err := client.Get(hookURL)
			if err != nil {
				fmt.Fprintf(stderr, "Warning: failed to download commit-msg hook from %s: %v\n", hookURL, err)
			} else {
				defer resp.Body.Close()
				if resp.StatusCode != http.StatusOK {
					fmt.Fprintf(stderr, "Warning: failed to download commit-msg hook from %s: HTTP %d\n", hookURL, resp.StatusCode)
				} else {
					body, rErr := io.ReadAll(resp.Body)
					if rErr != nil {
						fmt.Fprintf(stderr, "Warning: failed to read commit-msg hook response: %v\n", rErr)
					} else if len(body) == 0 {
						fmt.Fprintf(stderr, "Warning: downloaded commit-msg hook is empty\n")
					} else {
						if err := os.MkdirAll(filepath.Dir(hookPath), 0755); err != nil {
							fmt.Fprintf(stderr, "Warning: failed to create hooks directory: %v\n", err)
						} else if err := os.WriteFile(hookPath, body, 0755); err != nil {
							fmt.Fprintf(stderr, "Warning: failed to write commit-msg hook: %v\n", err)
						} else {
							fmt.Fprintf(stdout, "Downloaded and installed Gerrit commit-msg hook to %s\n", hookPath)
							hookInstalled = true
						}
					}
				}
			}
		}
	}

	if hookInstalled {
		fmt.Fprintln(stdout, "Amending HEAD commit to insert Change-Id footer...")
		if err := git.CommitAmendNoEdit(ctx, stdout, stderr); err != nil {
			return fmt.Errorf("failed to amend commit with Change-Id: %w", err)
		}

		// Re-verify that the commit now actually has a Change-Id
		amendedMsg, err := git.HeadCommitMessage(ctx)
		if err != nil {
			return fmt.Errorf("failed to verify amended commit message: %w", err)
		}
		if HasChangeID(amendedMsg) {
			fmt.Fprintln(stdout, "Successfully amended HEAD commit with Change-Id.")
			return nil
		}
		fmt.Fprintln(stderr, "Warning: commit-msg hook executed but Change-Id was not inserted.")
	}

	// If we reached here, provide the offramp instructions to the user/agent
	gerritHost := cfg.Host
	if gerritHost == "" {
		gerritHost = "<gerrit-host>"
	}
	fmt.Fprintf(stderr, `Hint: Gerrit requires a Change-Id in your commit message footer.
To install the hook and generate a Change-Id:
  curl -Lo $(git rev-parse --git-dir)/hooks/commit-msg https://%s/tools/hooks/commit-msg
  chmod +x $(git rev-parse --git-dir)/hooks/commit-msg
  git commit --amend --no-edit
`, gerritHost)

	return fmt.Errorf("HEAD commit is missing required Gerrit Change-Id")
}

// CountCommitsAhead returns the number of commits in origin/<branch>..HEAD.
// If the remote branch cannot be verified or rev-list fails, it returns 0 and the error.
func CountCommitsAhead(ctx context.Context, git GitRunner, branch string) (int, error) {
	if git == nil {
		return 0, fmt.Errorf("git runner cannot be nil")
	}
	branch = strings.TrimSpace(branch)
	if branch == "" {
		return 0, fmt.Errorf("cannot count commits ahead: branch name is empty")
	}
	targetRef := fmt.Sprintf("origin/%s", branch)
	var verifyErr bytes.Buffer
	if err := git.Run(ctx, io.Discard, &verifyErr, "rev-parse", "--verify", targetRef); err != nil {
		errStr := strings.TrimSpace(verifyErr.String())
		if errStr != "" {
			return 0, fmt.Errorf("git rev-parse --verify %s failed: %w: %s", targetRef, err, errStr)
		}
		return 0, fmt.Errorf("git rev-parse --verify %s failed: %w", targetRef, err)
	}
	var outBuf, listErr bytes.Buffer
	rangeSpec := fmt.Sprintf("origin/%s..HEAD", branch)
	if err := git.Run(ctx, &outBuf, &listErr, "rev-list", "--count", rangeSpec); err != nil {
		errStr := strings.TrimSpace(listErr.String())
		if errStr != "" {
			return 0, fmt.Errorf("git rev-list --count %s failed: %w: %s", rangeSpec, err, errStr)
		}
		return 0, fmt.Errorf("git rev-list --count %s failed: %w", rangeSpec, err)
	}
	var count int
	trimmed := strings.TrimSpace(outBuf.String())
	if _, err := fmt.Sscanf(trimmed, "%d", &count); err != nil {
		return 0, fmt.Errorf("failed to parse commit count from output %q: %w", trimmed, err)
	}
	return count, nil
}

// isChangeIdentifier returns true if arg is a change number, Change-Id, number/patchset,
// Gerrit URL, shortlink, or branch-encoded change number.
func isChangeIdentifier(arg string) bool {
	trimmed := strings.TrimSpace(arg)
	if trimmed == "" {
		return false
	}
	if strings.HasPrefix(trimmed, "I") && len(trimmed) >= 10 {
		return true
	}
	if strings.HasPrefix(trimmed, "http://") || strings.HasPrefix(trimmed, "https://") {
		return true
	}
	if strings.HasPrefix(trimmed, "pwrev/") || strings.HasPrefix(trimmed, "fxrev/") || strings.HasPrefix(trimmed, "crrev") {
		return true
	}
	if branchChangeNumRegex.MatchString(trimmed) {
		return true
	}
	parts := strings.Split(trimmed, "/")
	if len(parts) <= 2 {
		allDigits := true
		for _, p := range parts {
			if _, err := strconv.Atoi(p); err != nil {
				allDigits = false
				break
			}
		}
		if allDigits && len(parts[0]) > 0 {
			return true
		}
	}
	return false
}

// ResolveActiveChangeID attempts to resolve the active Gerrit change identifier from the local git repository.
// If the user is on the default branch (main) and 0 commits ahead of origin, it returns an error.
// Otherwise, it checks:
// 1. HEAD commit for a Gerrit Change-Id footer.
// 2. branch.<currentBranch>.gerrit-change-id in git config.
// 3. branch name encoding a change number (e.g. 472267, cl/472267, change-472267).
// 4. recent commits ahead of origin/main for a Gerrit Change-Id footer.
func ResolveActiveChangeID(ctx context.Context, cfg *Config) (string, error) {
	if cfg == nil || cfg.Git == nil {
		return "", fmt.Errorf("no change ID specified and git runner not initialized")
	}

	git := cfg.GitClient()
	currentBranch, err := git.CurrentBranch(ctx)
	if err != nil {
		return "", fmt.Errorf("failed to determine current branch: %w", err)
	}

	if currentBranch == "main" {
		if count, err := git.CountCommitsAhead(ctx, currentBranch); err == nil && count == 0 {
			return "", fmt.Errorf("no change ID specified and current branch %q is synced with origin (no active change).\n\n"+
				"To inspect or check out existing changes:\n"+
				"  gh pr list              # Find active pull requests\n"+
				"  gh pr view <number>     # View a specific change\n"+
				"  gh pr checkout <number> # Check out a change locally\n\n"+
				"To start working on a new change:\n"+
				"  git checkout -b <branch>\n"+
				"  # make changes && git commit\n"+
				"  gh pr create", currentBranch)
		}
	}

	// 1. Check HEAD commit
	if msg, err := git.HeadCommitMessage(ctx); err == nil {
		if id := ExtractChangeID(msg); id != "" {
			return id, nil
		}
	}

	// 2. Check git config branch.<currentBranch>.gerrit-change-id
	if currentBranch != "" {
		configKey := fmt.Sprintf("branch.%s.gerrit-change-id", currentBranch)
		if id, err := git.ConfigGet(ctx, configKey); err == nil && id != "" {
			return id, nil
		}
	}

	// 3. Check if currentBranch encodes a change number (e.g. 472267, cl/472267, change-472267)
	if currentBranch != "" {
		if chID, _ := ParseChangeAndRevision(currentBranch); chID != "" && chID != currentBranch {
			return chID, nil
		}
		if _, err := strconv.Atoi(currentBranch); err == nil {
			return currentBranch, nil
		}
	}

	// 4. Check recent commits ahead of origin/main
	if id := findChangeIDInCommitRange(ctx, git, "origin/main..HEAD"); id != "" {
		return id, nil
	}

	if currentBranch != "" {
		return "", fmt.Errorf("no change ID specified and no Gerrit Change-Id found for current branch %q.\nSpecify a change number (e.g. 'gh pr view 12345') or create a commit with a Change-Id", currentBranch)
	}
	return "", fmt.Errorf("no change ID specified and no Gerrit Change-Id found in current commit.\nSpecify a change number (e.g. 'gh pr view 12345')")
}

// findChangeIDInCommitRange scans recent commits in the specified git range
// (e.g. "origin/main..HEAD") and returns the first Gerrit Change-Id trailer found.
func findChangeIDInCommitRange(ctx context.Context, git GitClient, rangeSpec string) string {
	var rangeBuf bytes.Buffer
	if err := git.Run(ctx, &rangeBuf, io.Discard, "log", "-10", "--format=%B", rangeSpec); err == nil {
		for _, block := range strings.Split(rangeBuf.String(), "\n\n") {
			if id := ExtractChangeID(block); id != "" {
				return id
			}
		}
	}
	return ""
}

// ResolveTargetChangeID determines the target change ID from CLI args or the local git state.
// If an argument is provided:
// - If it is a change identifier (number, Change-Id, URL, shortlink), it is returned.
// - If it corresponds to a local branch or git config, the branch's Change-Id is resolved.
// Otherwise, it attempts to resolve the active change ID from the local git branch/HEAD commit.
func ResolveTargetChangeID(ctx context.Context, cmd *cobra.Command, args []string) (string, error) {
	if len(args) == 0 || strings.TrimSpace(args[0]) == "" {
		cfg := GetConfig(cmd)
		return ResolveActiveChangeID(ctx, cfg)
	}

	target := strings.TrimSpace(args[0])

	// If target is already a change number, Change-Id, URL, or shortlink, return it directly.
	if isChangeIdentifier(target) {
		return target, nil
	}

	cfg := GetConfig(cmd)
	if cfg != nil && cfg.Git != nil {
		git := cfg.GitClient()
		// 1. Check if git config branch.<target>.gerrit-change-id is set
		configKey := fmt.Sprintf("branch.%s.gerrit-change-id", target)
		if id, err := git.ConfigGet(ctx, configKey); err == nil && id != "" {
			return id, nil
		}

		// 2. Check if target is a local git branch
		refTarget := fmt.Sprintf("refs/heads/%s", target)
		if ok, err := git.VerifyRef(ctx, refTarget); err == nil && ok {
			// Inspect branch tip commit for Change-Id
			if msg, err := git.CommitMessage(ctx, refTarget); err == nil {
				if id := ExtractChangeID(msg); id != "" {
					return id, nil
				}
			}

			// Inspect recent commits on branch ahead of origin/main
			if id := findChangeIDInCommitRange(ctx, git, fmt.Sprintf("origin/main..%s", refTarget)); id != "" {
				return id, nil
			}

			return "", fmt.Errorf("branch %q has no associated Gerrit Change-Id", target)
		}
	}

	return target, nil
}

// NormalizeCQArgs transforms separate ["--cq", "<val>"] tokens into
// ["--cq=<val>"] for values "0", "1", "2".
// This ensures flags configured with NoOptDefVal accept both valueless invocations
// (--cq -> default 1) and value-bearing invocations (--cq 1, --cq 2, --cq 0).
//
// -q is deliberately not handled: it is gh's --jq, so it is not bound here.
func NormalizeCQArgs(args []string) []string {
	result := make([]string, 0, len(args))
	for i := 0; i < len(args); i++ {
		arg := args[i]
		if arg == "--cq" && i+1 < len(args) {
			next := args[i+1]
			if next == "0" || next == "1" || next == "2" {
				result = append(result, arg+"="+next)
				i++
				continue
			}
		}
		result = append(result, arg)
	}
	return result
}

// OpenBrowserFn opens the specified URL in a web browser. It is defined as a variable so tests can mock it.
var OpenBrowserFn = func(urlStr string) error {
	var cmd *exec.Cmd
	switch runtime.GOOS {
	case "darwin":
		cmd = exec.Command("open", urlStr)
	case "windows":
		cmd = exec.Command("rundll32", "url.dll,FileProtocolHandler", urlStr)
	default:
		browser := os.Getenv("BROWSER")
		if browser != "" {
			cmd = exec.Command(browser, urlStr)
		} else {
			cmd = exec.Command("xdg-open", urlStr)
		}
	}
	return cmd.Start()
}
