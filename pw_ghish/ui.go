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
	"io"
	"regexp"
	"strings"
	"unicode/utf8"
)

// Canonical Basic Multilingual Plane (BMP) status and UI glyphs.
const (
	GlyphPass    = "✓"
	GlyphFail    = "✗"
	GlyphWarn    = "⚠"
	GlyphRunning = "●"
	GlyphPending = "○"
	GlyphDot     = "·"
	GlyphArrow   = "→"
)

// StatusGlyph returns the canonical glyph text output shows before a worktree
// or sense status enum, or "" for an unknown status.
func StatusGlyph(status string) string {
	switch status {
	case "NEEDS_ATTENTION":
		return GlyphFail
	case "READY_TO_LAND", "CL_MERGED", "READY_FOR_NEXT", "ABANDONED":
		return GlyphPass
	case "IN_REVIEW":
		return GlyphPending
	case "GERRIT_OFFLINE":
		return GlyphWarn
	case "LOCAL_WIP":
		return GlyphRunning
	case "CLEAN_SYNCED":
		return GlyphDot
	default:
		return ""
	}
}

// FormatStatusWithGlyph returns status prefixed with its StatusGlyph (for
// example "● LOCAL_WIP"), or status unchanged when it has no glyph.
func FormatStatusWithGlyph(status string) string {
	if g := StatusGlyph(status); g != "" {
		return g + " " + status
	}
	return status
}

func applySGR(w io.Writer, code, s string) string {
	if s == "" || !ColorEnabled(w) {
		return s
	}
	return "\x1b[" + code + "m" + s + "\x1b[0m"
}

// StyleGreen wraps s in ANSI green SGR codes when ColorEnabled(w) is true.
func StyleGreen(w io.Writer, s string) string {
	return applySGR(w, "32", s)
}

// StyleRed wraps s in ANSI red SGR codes when ColorEnabled(w) is true.
func StyleRed(w io.Writer, s string) string {
	return applySGR(w, "31", s)
}

// StyleYellow wraps s in ANSI yellow SGR codes when ColorEnabled(w) is true.
func StyleYellow(w io.Writer, s string) string {
	return applySGR(w, "33", s)
}

// StyleCyan wraps s in ANSI cyan SGR codes when ColorEnabled(w) is true.
func StyleCyan(w io.Writer, s string) string {
	return applySGR(w, "36", s)
}

// StyleBold wraps s in ANSI bold SGR codes when ColorEnabled(w) is true.
func StyleBold(w io.Writer, s string) string {
	return applySGR(w, "1", s)
}

// StyleDim wraps s in ANSI dim SGR codes when ColorEnabled(w) is true.
func StyleDim(w io.Writer, s string) string {
	return applySGR(w, "2", s)
}

// PadRightStyled pads raw to width using utf8.RuneCountInString(raw) before
// applying styleFn, ensuring ANSI escape codes never break column alignment.
func PadRightStyled(w io.Writer, raw string, width int, styleFn func(io.Writer, string) string) string {
	runeLen := utf8.RuneCountInString(raw)
	styled := raw
	if styleFn != nil {
		styled = styleFn(w, raw)
	}
	if runeLen >= width {
		return styled
	}
	return styled + strings.Repeat(" ", width-runeLen)
}

// ReceiptField represents a key-value detail line in a structured receipt.
type ReceiptField struct {
	Key   string
	Value string
}

// Receipt represents a structured mutation confirmation receipt.
type Receipt struct {
	Summary string         // e.g. "Change 12345 closed (abandoned) successfully."
	URL     string         // optional canonical URL
	Details []ReceiptField // optional extra key-value lines
	Next    string         // optional next-step command hint
}

// FormatReceipt formats a structured mutation receipt with a leading pass glyph
// and optional key-value details, canonical URL, and next-step hint.
func FormatReceipt(w io.Writer, r Receipt) string {
	// Labels ("Key:") pad to a 7-column field. When any detail key is longer
	// than 7 characters, widen the field to fit it so every value in the
	// receipt stays aligned.
	width := receiptLabelWidth
	for _, d := range r.Details {
		if n := utf8.RuneCountInString(d.Key); d.Value != "" && n > receiptLabelWidth {
			width = max(width, n+1)
		}
	}
	var b strings.Builder
	fmt.Fprintf(&b, "%s %s\n", StyleGreen(w, GlyphPass), r.Summary)
	for _, d := range r.Details {
		if d.Key != "" && d.Value != "" {
			writeReceiptLine(&b, d.Key+":", width, d.Value)
		}
	}
	if r.URL != "" {
		writeReceiptLine(&b, "URL:", width, r.URL)
	}
	if r.Next != "" {
		writeReceiptLine(&b, "Next:", width, r.Next)
	}
	return b.String()
}

// receiptLabelWidth is the default label column width in FormatReceipt.
const receiptLabelWidth = 7

// writeReceiptLine writes "  <label><pad> <value>\n", padding label with
// spaces to width runes.
func writeReceiptLine(b *strings.Builder, label string, width int, value string) {
	b.WriteString("  ")
	b.WriteString(label)
	for n := utf8.RuneCountInString(label); n < width; n++ {
		b.WriteByte(' ')
	}
	b.WriteByte(' ')
	b.WriteString(value)
	b.WriteByte('\n')
}

// PrintReceipt writes a formatted mutation receipt to w.
func PrintReceipt(w io.Writer, r Receipt) {
	fmt.Fprint(w, FormatReceipt(w, r))
}

var gerritRemoteURLRe = regexp.MustCompile(`https://[^\s]+`)

// ExtractGerritURLFromGitOutput scans raw git push output for a Gerrit change
// URL on "remote:" lines.
func ExtractGerritURLFromGitOutput(output string) string {
	var fallback string
	for _, line := range strings.Split(output, "\n") {
		idx := strings.Index(line, "remote:")
		if idx == -1 {
			continue
		}
		rest := line[idx+len("remote:"):]
		matches := gerritRemoteURLRe.FindAllString(rest, -1)
		for _, m := range matches {
			m = strings.TrimRight(m, ".,;)>]\"'")
			if m == "" {
				continue
			}
			if strings.Contains(m, "/c/") || strings.Contains(m, "/+/") {
				return m
			}
			if fallback == "" {
				fallback = m
			}
		}
	}
	return fallback
}
