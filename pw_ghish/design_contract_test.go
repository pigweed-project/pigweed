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

// Reusable assertions for the pw_ghish UI design language (see "UI design
// language and context budgeting" in developing.rst and invariant #15 in
// AGENTS.md). They operate on captured command output so they can be driven
// from golden cases today and from per-command ghish:kind annotations later.
//
// These helpers are package-internal test code: tests in pw_ghish/worktree
// cannot import them. A kind-driven contract test should therefore live in
// this package and walk RootCmd (which includes the registered wt commands).

import (
	"strings"
	"testing"
	"unicode/utf8"
)

const designLanguageRef = "see \"UI design language and context budgeting\" in pw_ghish/developing.rst"

// canonicalGlyphs are the UI glyphs from ui.go that are always allowed, even
// though some fall inside the denied symbol ranges.
var canonicalGlyphs = map[rune]bool{}

func init() {
	for _, g := range []string{GlyphPass, GlyphFail, GlyphWarn, GlyphRunning, GlyphPending, GlyphDot, GlyphArrow} {
		r, _ := utf8.DecodeRuneInString(g)
		canonicalGlyphs[r] = true
	}
}

// ruleChars are characters that form decorative separator walls when 10 or
// more appear in a row.
const ruleChars = "=-─━═"

const ruleRunLength = 10

// nonEmptyLines returns the lines of out with surrounding whitespace trimmed,
// skipping blank lines.
func nonEmptyLines(out string) []string {
	var lines []string
	for _, line := range strings.Split(out, "\n") {
		if trimmed := strings.TrimSpace(line); trimmed != "" {
			lines = append(lines, trimmed)
		}
	}
	return lines
}

// hasLabeledLine reports whether any line of out, after trimming leading
// whitespace, starts with label (e.g. "Next:" or "Verdict:").
func hasLabeledLine(out, label string) bool {
	for _, line := range nonEmptyLines(out) {
		if strings.HasPrefix(line, label) {
			return true
		}
	}
	return false
}

// assertMutationReceipt checks that out begins with a structured mutation
// receipt as produced by PrintReceipt: the first non-empty line is
// "✓ <Summary>", and the contiguous, indented block that follows it contains
// a "Next:" line.
func assertMutationReceipt(t testing.TB, out string) {
	t.Helper()
	const rule = "mutation receipt rule (emit via PrintReceipt/FormatReceipt; " + designLanguageRef + ")"
	lines := strings.Split(out, "\n")
	start := 0
	for start < len(lines) && strings.TrimSpace(lines[start]) == "" {
		start++
	}
	if start == len(lines) {
		t.Errorf("%s: output is empty", rule)
		return
	}
	if !strings.HasPrefix(lines[start], GlyphPass+" ") {
		t.Errorf("%s: first non-empty line must start with %q, got %q\nfull output:\n%s", rule, GlyphPass+" ", lines[start], out)
	}
	for _, line := range lines[start+1:] {
		if strings.TrimSpace(line) == "" || !strings.HasPrefix(line, " ") {
			break
		}
		if strings.HasPrefix(strings.TrimSpace(line), "Next:") {
			return
		}
	}
	t.Errorf("%s: the receipt block (indented lines directly after the %q summary) has no \"Next:\" line\nfull output:\n%s", rule, GlyphPass, out)
}

// assertVerdict checks that out contains a "Verdict:" line. It checks
// presence only; placement near the top of the output is not enforced yet.
func assertVerdict(t testing.TB, out string) {
	t.Helper()
	if !hasLabeledLine(out, "Verdict:") {
		t.Errorf("answer-first rule (%s): missing a \"Verdict:\" line\nfull output:\n%s", designLanguageRef, out)
	}
}

// assertNextHint checks that out contains a "Next:" line.
func assertNextHint(t testing.TB, out string) {
	t.Helper()
	if !hasLabeledLine(out, "Next:") {
		t.Errorf("answer-first rule (%s): missing a \"Next:\" line\nfull output:\n%s", designLanguageRef, out)
	}
}

func deniedSymbol(r rune) bool {
	switch {
	case canonicalGlyphs[r]:
		return false
	case r >= 0x2190 && r <= 0x2BFF: // arrows, technical, box drawing, dingbats, misc symbols
		return true
	case r == 0xFE0F: // emoji presentation selector
		return true
	case r >= 0x1F000:
		return true
	case r > 0xFFFF:
		return true
	}
	return false
}

func longestRuleRun(line string) (rune, int) {
	var best rune
	bestLen, curLen := 0, 0
	var cur rune
	for _, r := range line {
		if strings.ContainsRune(ruleChars, r) {
			if r == cur {
				curLen++
			} else {
				cur, curLen = r, 1
			}
			if curLen > bestLen {
				best, bestLen = cur, curLen
			}
			continue
		}
		cur, curLen = 0, 0
	}
	return best, bestLen
}

// assertNoDecorativeNoise rejects output elements the design language bans:
// separator walls (10 or more consecutive "=", "-", "─", "━" or "═"),
// non-canonical symbols (U+2190-U+2BFF other than the ui.go glyphs, U+FE0F,
// and anything outside the Basic Multilingual Plane such as emoji), and ANSI
// escape sequences.
func assertNoDecorativeNoise(t testing.TB, out string) {
	t.Helper()
	const rule = "decorative-noise rule (" + designLanguageRef + ")"
	for i, line := range strings.Split(out, "\n") {
		if r, n := longestRuleRun(line); n >= ruleRunLength {
			t.Errorf("%s: line %d has a separator wall of %d %q: %q", rule, i+1, n, r, line)
		}
		if strings.Contains(line, "\x1b[") {
			t.Errorf("%s: line %d contains an ANSI escape sequence: %q", rule, i+1, line)
		}
		for _, r := range line {
			if deniedSymbol(r) {
				t.Errorf("%s: line %d contains non-canonical symbol %q (U+%04X); use a Glyph* constant from ui.go: %q", rule, i+1, r, r, line)
				break
			}
		}
	}
}

// assertByteBudget fails if out is larger than max bytes.
func assertByteBudget(t testing.TB, out string, max int) {
	t.Helper()
	if len(out) > max {
		t.Errorf("context budget rule (%s): output is %d bytes, budget is %d bytes", designLanguageRef, len(out), max)
	}
}
