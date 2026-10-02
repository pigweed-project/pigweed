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
	"strings"
)

// CommitMessage is a structured intermediate representation (IR) of a Git
// commit message that enables lossless parsing, structured trailer inspection
// and editing, and faithful reconstitution without manual slice index math.
type CommitMessage struct {
	// Subject is the first line of the commit message.
	Subject string
	// Paragraphs holds the blank-line-delimited paragraphs following the
	// subject line, in order of appearance. Each paragraph records its exact
	// preceding blank lines so round-trip formatting never collapses author
	// spacing.
	Paragraphs []CommitParagraph
}

// CommitParagraph represents a blank-line-delimited block of lines following
// the commit subject. A paragraph is either an ordinary prose/code body block
// or a structured trailer block.
type CommitParagraph struct {
	// LeadingBlankLines holds the blank separator lines immediately preceding
	// this paragraph (typically a single ""), preserving exact vertical spacing
	// when formatting back to text.
	LeadingBlankLines []string
	// Lines holds the raw lines of a non-trailer (body) paragraph.
	Lines []string
	// Trailers holds the parsed trailer entries when IsTrailerBlock is true.
	Trailers []CommitTrailer
	// IsTrailerBlock reports whether this paragraph is a valid trailer block.
	IsTrailerBlock bool
}

// CommitTrailer represents a single structured trailer or footer entry within a
// trailer paragraph of a commit message.
type CommitTrailer struct {
	// Key is the trailer key before the colon (e.g. "Bug", "Change-Id").
	// Empty when IsKeylessFooter is true.
	Key string
	// Value is the trailer value on the first line after the colon.
	Value string
	// Continuations holds any indented continuation lines belonging to this
	// trailer (with their leading indentation preserved).
	Continuations []string
	// IsKeylessFooter is true for recognized non-keyed footer lines such as
	// "(cherry picked from commit ...)". When true, Value holds the full line.
	IsKeylessFooter bool

	rawFirstLine string
}

// FirstLine returns the formatted first line of the trailer (excluding any
// continuation lines).
func (t CommitTrailer) FirstLine() string {
	if t.IsKeylessFooter {
		return t.Value
	}
	if t.rawFirstLine != "" {
		if k, v, ok := splitKeyValueTrailer(t.rawFirstLine); ok && k == t.Key && v == t.Value {
			return t.rawFirstLine
		}
	}
	return t.Key + ": " + t.Value
}

// Lines returns the first line followed by any continuation lines.
func (t CommitTrailer) Lines() []string {
	out := make([]string, 0, 1+len(t.Continuations))
	out = append(out, t.FirstLine())
	out = append(out, t.Continuations...)
	return out
}

// Format returns the full multi-line string representation of this trailer
// (including any continuation lines joined by "\n").
func (t CommitTrailer) Format() string {
	return strings.Join(t.Lines(), "\n")
}

// SetFirstLine updates the trailer's Key and Value from a formatted `Key: value`
// line while preserving any existing continuation lines.
func (t *CommitTrailer) SetFirstLine(line string) bool {
	trimmed := strings.TrimSpace(strings.TrimRight(line, "\r"))
	k, v, ok := splitKeyValueTrailer(trimmed)
	if !ok {
		return false
	}
	t.Key = k
	t.Value = v
	t.IsKeylessFooter = false
	t.rawFirstLine = trimmed
	return true
}

func splitKeyValueTrailer(line string) (key, value string, ok bool) {
	if !trailerRegex.MatchString(line) {
		return "", "", false
	}
	parts := strings.SplitN(line, ":", 2)
	return strings.TrimSpace(parts[0]), strings.TrimSpace(parts[1]), true
}

// ParseTrailerEntry parses a single trailer string (which may include indented
// continuation lines separated by "\n") into a structured CommitTrailer.
func ParseTrailerEntry(raw string) (CommitTrailer, error) {
	normalized := strings.ReplaceAll(strings.TrimRight(raw, "\r\n"), "\r\n", "\n")
	lines := strings.Split(normalized, "\n")
	if len(lines) == 0 {
		return CommitTrailer{}, fmt.Errorf("internal error: %q is not a `Key: value` trailer", raw)
	}
	first := strings.TrimSpace(lines[0])
	k, v, ok := splitKeyValueTrailer(first)
	if !ok {
		return CommitTrailer{}, fmt.Errorf("internal error: %q is not a `Key: value` trailer", raw)
	}
	var conts []string
	for _, c := range lines[1:] {
		if !strings.HasPrefix(c, " ") && !strings.HasPrefix(c, "\t") {
			return CommitTrailer{}, fmt.Errorf("internal error: continuation line %q in %q must be indented", c, raw)
		}
		conts = append(conts, c)
	}
	return CommitTrailer{
		Key:           k,
		Value:         v,
		Continuations: conts,
		rawFirstLine:  first,
	}, nil
}

// ParseCommitMessage parses a raw commit message string into a structured
// CommitMessage IR.
func ParseCommitMessage(raw string) *CommitMessage {
	normalized := strings.ReplaceAll(raw, "\r\n", "\n")
	body := strings.TrimRight(normalized, "\n")
	if body == "" {
		return &CommitMessage{}
	}

	lines := strings.Split(body, "\n")
	msg := &CommitMessage{
		Subject: lines[0],
	}
	if len(lines) <= 1 {
		return msg
	}

	rest := lines[1:]
	var pendingBlanks []string
	var currentLines []string

	flushParagraph := func() {
		if len(currentLines) == 0 {
			return
		}
		if trailers, ok := parseParagraphTrailers(currentLines); ok {
			msg.Paragraphs = append(msg.Paragraphs, CommitParagraph{
				LeadingBlankLines: pendingBlanks,
				Trailers:          trailers,
				IsTrailerBlock:    true,
			})
		} else {
			copied := make([]string, len(currentLines))
			copy(copied, currentLines)
			msg.Paragraphs = append(msg.Paragraphs, CommitParagraph{
				LeadingBlankLines: pendingBlanks,
				Lines:             copied,
				IsTrailerBlock:    false,
			})
		}
		pendingBlanks = nil
		currentLines = nil
	}

	for _, l := range rest {
		if strings.TrimSpace(l) == "" {
			if len(currentLines) > 0 {
				flushParagraph()
			}
			pendingBlanks = append(pendingBlanks, l)
			continue
		}
		currentLines = append(currentLines, l)
	}
	flushParagraph()

	return msg
}

func parseParagraphTrailers(paragraph []string) ([]CommitTrailer, bool) {
	var trailers []CommitTrailer
	sawKeyedTrailer := false

	for _, l := range paragraph {
		trimmedRight := strings.TrimRight(l, "\r")
		trimmed := strings.TrimSpace(trimmedRight)
		switch {
		case len(trailers) > 0 && (strings.HasPrefix(trimmedRight, " ") || strings.HasPrefix(trimmedRight, "\t")):
			trailers[len(trailers)-1].Continuations = append(trailers[len(trailers)-1].Continuations, trimmedRight)
		case trailerRegex.MatchString(trimmed):
			k, v, _ := splitKeyValueTrailer(trimmed)
			trailers = append(trailers, CommitTrailer{
				Key:          k,
				Value:        v,
				rawFirstLine: trimmed,
			})
			sawKeyedTrailer = true
		case cherryPickFooterRegex.MatchString(trimmed):
			trailers = append(trailers, CommitTrailer{
				Value:           trimmed,
				IsKeylessFooter: true,
				rawFirstLine:    trimmed,
			})
		default:
			return nil, false
		}
	}

	return trailers, sawKeyedTrailer
}

// FormattedLines returns the lines of the paragraph as they appear in a
// reconstituted commit message.
func (p CommitParagraph) FormattedLines() []string {
	if !p.IsTrailerBlock {
		out := make([]string, len(p.Lines))
		copy(out, p.Lines)
		return out
	}
	var out []string
	for _, tr := range p.Trailers {
		out = append(out, tr.Lines()...)
	}
	return out
}

// Format reconstitutes the CommitMessage into a canonical commit message string
// ending with a trailing newline (or "" if the message is completely empty).
func (m *CommitMessage) Format() string {
	if m == nil || (m.Subject == "" && len(m.Paragraphs) == 0) {
		return ""
	}

	var out []string
	if m.Subject != "" {
		out = append(out, m.Subject)
	}

	for i, p := range m.Paragraphs {
		pLines := p.FormattedLines()
		if len(pLines) == 0 {
			continue
		}
		if m.Subject != "" || i > 0 {
			if len(p.LeadingBlankLines) > 0 {
				out = append(out, p.LeadingBlankLines...)
			} else {
				out = append(out, "")
			}
		}
		out = append(out, pLines...)
	}

	return strings.Join(out, "\n") + "\n"
}

// ExtractTrailers returns all trailer entries across all trailer paragraphs in
// the commit message, with bug-style trailer values canonicalized and
// continuations folded into the trailer they belong to.
func (m *CommitMessage) ExtractTrailers() []string {
	if m == nil || len(m.Paragraphs) == 0 {
		return nil
	}
	var trailers []string
	for _, p := range m.Paragraphs {
		if !p.IsTrailerBlock {
			continue
		}
		for _, tr := range p.Trailers {
			if tr.IsKeylessFooter {
				trailers = append(trailers, tr.Format())
				continue
			}
			first := normalizeTrailerValue(tr.FirstLine())
			if len(tr.Continuations) > 0 {
				first += "\n" + strings.Join(tr.Continuations, "\n")
			}
			trailers = append(trailers, first)
		}
	}
	return trailers
}

// UpsertTrailer sets trailerLine as the single trailer for its key:
//   - If a trailer with the same key (case-insensitive) exists in a trailer
//     paragraph, the first occurrence is replaced in place and any subsequent
//     duplicate occurrences (along with their continuation lines) are removed.
//   - If the key is absent, the trailer is appended to the last trailer
//     paragraph, or a new trailer paragraph is created at the end of the
//     message if none exists yet.
func (m *CommitMessage) UpsertTrailer(trailerLine string) error {
	if m == nil {
		return fmt.Errorf("internal error: CommitMessage is nil")
	}
	newTr, err := ParseTrailerEntry(trailerLine)
	if err != nil {
		return err
	}

	if m.Subject == "" && len(m.Paragraphs) == 0 {
		m.Paragraphs = []CommitParagraph{
			{
				IsTrailerBlock: true,
				Trailers:       []CommitTrailer{newTr},
			},
		}
		return nil
	}

	replaced := false
	lastTrailerParaIdx := -1

	for pIdx := range m.Paragraphs {
		p := &m.Paragraphs[pIdx]
		if !p.IsTrailerBlock {
			continue
		}
		lastTrailerParaIdx = pIdx
		kept := make([]CommitTrailer, 0, len(p.Trailers))
		for _, tr := range p.Trailers {
			if !tr.IsKeylessFooter && strings.EqualFold(tr.Key, newTr.Key) {
				if !replaced {
					kept = append(kept, newTr)
					replaced = true
				}
				continue
			}
			kept = append(kept, tr)
		}
		p.Trailers = kept
	}

	if !replaced {
		if lastTrailerParaIdx >= 0 {
			m.Paragraphs[lastTrailerParaIdx].Trailers = append(m.Paragraphs[lastTrailerParaIdx].Trailers, newTr)
		} else {
			m.Paragraphs = append(m.Paragraphs, CommitParagraph{
				LeadingBlankLines: []string{""},
				IsTrailerBlock:    true,
				Trailers:          []CommitTrailer{newTr},
			})
		}
	}

	return nil
}

// NormalizeBugFooters rewrites bug and fix trailers across all trailer
// paragraphs to match trailerFormat while leaving prose body paragraphs and
// non-bug trailers untouched. Returns true if the message contains at least one
// paragraph after the subject.
func (m *CommitMessage) NormalizeBugFooters(trailerFormat string) bool {
	if m == nil || m.Subject == "" || len(m.Paragraphs) == 0 {
		return false
	}
	for pIdx := range m.Paragraphs {
		p := &m.Paragraphs[pIdx]
		if !p.IsTrailerBlock {
			continue
		}
		for tIdx := range p.Trailers {
			tr := &p.Trailers[tIdx]
			if tr.IsKeylessFooter {
				continue
			}
			first := tr.FirstLine()
			if _, _, _, ok := parseBugTrailer(first); ok {
				tr.SetFirstLine(NormalizeTrailerWithFormat(first, trailerFormat))
			}
		}
	}
	return true
}
