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
	"strconv"
	"strings"
)

var (
	// Matches Google Issue Tracker URLs that resolve to numeric IDs:
	// - https://issues.pigweed.dev/issues/<id>
	// - https://g-issues.pigweed.dev/issues/<id>
	// - https://issues.fuchsia.dev/issues/<id>
	// - https://issuetracker.google.com/issues/<id>
	// - https://bugs.chromium.org/p/<project>/issues/detail?id=<id>
	issueTrackerURLRegex     = regexp.MustCompile(`^https?://(?:(?:g-)?issues\.pigweed\.dev|issues\.fuchsia\.dev|issuetracker\.google\.com)/issues/(\d+)(?:[/?#].*)?$`)
	publicChromiumIssueRegex = regexp.MustCompile(`^https?://bugs\.chromium\.org/p/[^/]+/issues/detail\?id=(\d+)(?:[&#].*)?$`)
	shorthandBugRegex        = regexp.MustCompile(`^(?:b/|b:|pwbug(?:\.dev)?/|fxb/|fxbug\.dev/)(\d+)$`)
	pureNumericBugRegex      = regexp.MustCompile(`^(\d+)$`)

	bugTrailerRegex = regexp.MustCompile(`(?i)^(bug|bugs|bugfix|issue|issues)\s*:\s*(.+)$`)
	fixTrailerRegex = regexp.MustCompile(`(?i)^(fix|fixes|fixed|fixing)\s*:\s*(.+)$`)
)

func extractBugNumber(token string, minNumericDigits int) (int64, bool) {
	token = strings.TrimSpace(token)
	if token == "" {
		return 0, false
	}

	var numStr string
	if m := issueTrackerURLRegex.FindStringSubmatch(token); len(m) > 1 {
		numStr = m[1]
	} else if m := publicChromiumIssueRegex.FindStringSubmatch(token); len(m) > 1 {
		numStr = m[1]
	} else if m := shorthandBugRegex.FindStringSubmatch(token); len(m) > 1 {
		numStr = m[1]
	} else if m := pureNumericBugRegex.FindStringSubmatch(token); len(m) > 1 && len(m[1]) >= minNumericDigits {
		numStr = m[1]
	}

	if numStr == "" {
		return 0, false
	}

	id, err := strconv.ParseInt(numStr, 10, 64)
	if err != nil || id <= 0 {
		return 0, false
	}
	return id, true
}

// ParseIssueID extracts a positive Buganizer issue ID from a numeric string, shorthand (b/123), or URL.
func ParseIssueID(token string) (int64, error) {
	id, ok := extractBugNumber(token, 1)
	if !ok {
		return 0, fmt.Errorf("invalid Buganizer issue identifier %q.\n\n"+
			"Expected formats:\n"+
			"  - Numeric ID (e.g., 345678)\n"+
			"  - Shorthand prefix (e.g., b/345678, pwbug/345678, fxb/345678, fxbug.dev/345678)\n"+
			"  - Issue Tracker URL (e.g., https://issues.pigweed.dev/issues/345678)", token)
	}
	return id, nil
}

// NormalizeBugID attempts to extract a canonical bug ID (e.g. "b/12345") from a token.
// Returns the normalized bug ID if matched, or the original token if not.
func NormalizeBugID(token string) (string, bool) {
	id, ok := extractBugNumber(token, 3)
	if !ok {
		return strings.TrimSpace(token), false
	}
	return fmt.Sprintf("b/%d", id), true
}

// bugChainSeparatorRegex splits a trailer value into candidate bug tokens.
var bugChainSeparatorRegex = regexp.MustCompile(`[\s,;]+`)

// bugChainIDs returns the canonical bug IDs in a trailer value, deduplicated
// and in order.
//
// The result is nil unless *every* token is a bug reference. A value with one
// non-reference token is not a partial list, it is prose -- "none, see the
// design doc" is a sentence, and picking the references out of it would
// invent a list the author did not write.
func bugChainIDs(raw string) []string {
	tokens := bugChainSeparatorRegex.Split(strings.TrimSpace(raw), -1)

	var ids []string
	seen := make(map[string]bool)
	for _, tok := range tokens {
		tok = strings.TrimSpace(tok)
		if tok == "" {
			continue
		}
		norm, ok := NormalizeBugID(tok)
		if !ok {
			return nil
		}
		if !seen[norm] {
			seen[norm] = true
			ids = append(ids, norm)
		}
	}
	return ids
}

// NormalizeBugChain normalizes a raw bug string (single or comma/space/semicolon-separated).
// Formats matching IDs into canonical "b/<id>" separated by ", ".
// If given "none", returns "None". Unmatched strings are preserved cleanly.
func NormalizeBugChain(raw string) string {
	trimmed := strings.TrimSpace(raw)
	if trimmed == "" {
		return ""
	}

	if strings.EqualFold(trimmed, "none") {
		return "None"
	}

	ids := bugChainIDs(trimmed)
	if len(ids) == 0 {
		// Free text such as "see the design doc". Leave the author's
		// sentence exactly as it is.
		return trimmed
	}

	return strings.Join(ids, ", ")
}

// parseBugTrailer splits a bug- or fix-style trailer into its key and value.
// closes reports whether the key is one Gerrit auto-closes the bug on (fix,
// fixes, fixing, fixed); ok is false for any other trailer line.
//
// This is the single place that decides whether a trailer is about a bug, so
// that the reader (ExtractBugLinks) and the two writers (NormalizeTrailer and
// normalizeTrailerValue) can never disagree about what counts.
//
// Only the first line is considered: an indented continuation belongs to the
// trailer but is not part of its value.
func parseBugTrailer(trailerLine string) (key, value string, closes, ok bool) {
	first := strings.TrimSpace(trailerLine)
	if i := strings.Index(first, "\n"); i >= 0 {
		first = strings.TrimSpace(first[:i])
	}
	if m := bugTrailerRegex.FindStringSubmatch(first); len(m) > 2 {
		return m[1], m[2], false, true
	}
	if m := fixTrailerRegex.FindStringSubmatch(first); len(m) > 2 {
		return m[1], m[2], true, true
	}
	return "", "", false, false
}

// NormalizeTrailer returns the canonical spelling of a bug or fix trailer,
// rewriting both the key and the value. Other trailer lines are returned
// unchanged.
//
// This is the single place that decides what a bug trailer looks like when
// gh-ish authors one itself, from `--bug` or `--fixed`. Contrast
// normalizeTrailerValue, which canonicalizes only the value and leaves the key
// exactly as the author wrote it: that one runs when carrying an existing
// trailer forward, where rewriting a key the user did not ask to change would
// be an unrequested edit to their commit message.
//
// The canonical fix key is `Fixed:`. Gerrit accepts fix, fixes, fixing and
// fixed interchangeably and auto-closes the bug on all four, so this is house
// style rather than function -- and Pigweed's house style is `Bug:` or
// `Fixed:`.
func NormalizeTrailer(trailerLine string) string {
	trailerLine = strings.TrimSpace(trailerLine)
	_, value, closes, ok := parseBugTrailer(trailerLine)
	if !ok {
		return trailerLine
	}
	key := "Bug"
	if closes {
		key = "Fixed"
	}
	return fmt.Sprintf("%s: %s", key, NormalizeBugChain(value))
}
