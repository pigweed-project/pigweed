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
	"strings"
	"testing"
	"time"

	"github.com/andygrunwald/go-gerrit"
)

func boolPtr(b bool) *bool {
	return &b
}

func timeTimestamp(t time.Time) *gerrit.Timestamp {
	return &gerrit.Timestamp{Time: t}
}

func TestAnalyzeComments_Empty(t *testing.T) {
	summary := AnalyzeComments(nil, nil)
	if summary.TotalThreads != 0 || summary.ResolvedThreads != 0 || summary.UnresolvedThreads != 0 || summary.DraftsCount != 0 {
		t.Errorf("Expected zero counts, got %+v", summary)
	}
	if summary.FormattedText != "    None" {
		t.Errorf("Expected '    None', got %q", summary.FormattedText)
	}
}

func TestAnalyzeComments_OnlyDrafts(t *testing.T) {
	drafts := map[string][]gerrit.CommentInfo{
		"pw_ghish/main.go": {
			{ID: "d1", Line: 10, Message: "Draft 1"},
			{ID: "d2", Line: 20, Message: "Draft 2"},
		},
	}
	summary := AnalyzeComments(nil, drafts)
	if summary.TotalThreads != 0 || summary.DraftsCount != 2 {
		t.Errorf("Expected 0 threads, 2 drafts; got %+v", summary)
	}
	if summary.FormattedText != "    None (2 unpublished drafts)" {
		t.Errorf("Expected '    None (2 unpublished drafts)', got %q", summary.FormattedText)
	}
}

func TestAnalyzeComments_AllResolved(t *testing.T) {
	now := time.Now()
	published := map[string][]gerrit.CommentInfo{
		"pw_ghish/status.go": {
			{
				ID:         "c1",
				Line:       42,
				PatchSet:   1,
				Unresolved: boolPtr(true),
				Updated:    timeTimestamp(now),
				Message:    "Can you fix this?",
			},
			{
				ID:         "c2",
				InReplyTo:  "c1",
				Line:       42,
				PatchSet:   2,
				Unresolved: boolPtr(false),
				Updated:    timeTimestamp(now.Add(time.Minute)),
				Message:    "Done!",
			},
		},
	}
	summary := AnalyzeComments(published, nil)
	if summary.TotalThreads != 1 || summary.ResolvedThreads != 1 || summary.UnresolvedThreads != 0 {
		t.Errorf("Expected 1 thread (1 resolved, 0 unresolved), got %+v", summary)
	}
	if summary.FormattedText != "    All resolved (1 thread)" {
		t.Errorf("Expected '    All resolved (1 thread)', got %q", summary.FormattedText)
	}
}

func TestAnalyzeComments_AllResolved_WithDrafts(t *testing.T) {
	now := time.Now()
	published := map[string][]gerrit.CommentInfo{
		"pw_ghish/status.go": {
			{
				ID:         "c1",
				Line:       42,
				PatchSet:   1,
				Unresolved: boolPtr(false),
				Updated:    timeTimestamp(now),
				Message:    "Looks good!",
			},
			{
				ID:         "c2",
				Line:       100,
				PatchSet:   1,
				Unresolved: boolPtr(false),
				Updated:    timeTimestamp(now),
				Message:    "Nice!",
			},
		},
	}
	drafts := map[string][]gerrit.CommentInfo{
		"pw_ghish/other.go": {
			{ID: "d1", Message: "Unpublished thought"},
		},
	}
	summary := AnalyzeComments(published, drafts)
	if summary.TotalThreads != 2 || summary.ResolvedThreads != 2 || summary.DraftsCount != 1 {
		t.Errorf("Expected 2 resolved threads, 1 draft; got %+v", summary)
	}
	if summary.FormattedText != "    All resolved (2 threads, 1 unpublished draft)" {
		t.Errorf("Expected '    All resolved (2 threads, 1 unpublished draft)', got %q", summary.FormattedText)
	}
}

func TestAnalyzeComments_OneUnresolved(t *testing.T) {
	published := map[string][]gerrit.CommentInfo{
		"pw_ghish/status.go": {
			{
				ID:         "c1",
				Line:       142,
				PatchSet:   14,
				Unresolved: boolPtr(true),
				Author:     gerrit.AccountInfo{Email: "reviewer@google.com"},
				Message:    "Consider sorting these keys so test output is deterministic.",
			},
		},
	}
	summary := AnalyzeComments(published, nil)
	if summary.TotalThreads != 1 || summary.UnresolvedThreads != 1 {
		t.Fatalf("Expected 1 unresolved thread, got %+v", summary)
	}
	expected := "⚠ 1 unresolved thread:\n        • pw_ghish/status.go:142 [PS14] by reviewer@google.com:\n          \"Consider sorting these keys so test output is deterministic.\""
	if !strings.Contains(summary.FormattedText, expected) {
		t.Errorf("Expected formatted text to contain %q, but got:\n%s", expected, summary.FormattedText)
	}
}

func TestAnalyzeComments_TwoUnresolved_OutOfThreeThreads(t *testing.T) {
	published := map[string][]gerrit.CommentInfo{
		"pw_ghish/status.go": {
			{
				ID:         "c1",
				Line:       142,
				PatchSet:   14,
				Unresolved: boolPtr(true),
				Author:     gerrit.AccountInfo{Email: "reviewer@google.com"},
				Message:    "Sorting issue.",
			},
		},
		"pw_ghish/checks.go": {
			{
				ID:         "c2",
				Line:       55,
				PatchSet:   15,
				Unresolved: boolPtr(true),
				Author:     gerrit.AccountInfo{Name: "Alice Reviewer"},
				Message:    "Handle patchset 0.",
			},
			{
				ID:         "c3",
				Line:       90,
				PatchSet:   15,
				Unresolved: boolPtr(false),
				Author:     gerrit.AccountInfo{Name: "Alice Reviewer"},
				Message:    "Resolved nit.",
			},
		},
	}
	drafts := map[string][]gerrit.CommentInfo{
		"pw_ghish/status.go": {
			{ID: "d1", InReplyTo: "c1", Message: "Draft reply on status.go"},
		},
	}

	summary := AnalyzeComments(published, drafts)
	if summary.TotalThreads != 3 || summary.UnresolvedThreads != 2 || summary.DraftsCount != 1 {
		t.Fatalf("Expected 3 threads (2 unresolved, 1 draft), got %+v", summary)
	}

	if !strings.Contains(summary.FormattedText, "⚠ 2 unresolved threads (out of 3 threads, 1 unpublished draft):") {
		t.Errorf("Expected summary line with out of 3 threads and draft, got:\n%s", summary.FormattedText)
	}
	if !strings.Contains(summary.FormattedText, "• pw_ghish/status.go:142 [PS14] by reviewer@google.com (has unpublished draft reply):") {
		t.Errorf("Expected draft reply note on c1, got:\n%s", summary.FormattedText)
	}
	if !strings.Contains(summary.FormattedText, "• pw_ghish/checks.go:55 [PS15] by Alice Reviewer:") {
		t.Errorf("Expected c2 preview, got:\n%s", summary.FormattedText)
	}
}

func TestAnalyzeComments_ThreeUnresolved_Hysteresis(t *testing.T) {
	published := map[string][]gerrit.CommentInfo{
		"a.go": {
			{ID: "c1", Line: 10, PatchSet: 1, Unresolved: boolPtr(true), Author: gerrit.AccountInfo{Name: "Rev1"}, Message: "First issue"},
		},
		"b.go": {
			{ID: "c2", Line: 20, PatchSet: 2, Unresolved: boolPtr(true), Author: gerrit.AccountInfo{Name: "Rev2"}, Message: "Second issue"},
		},
		"c.go": {
			{ID: "c3", Line: 30, PatchSet: 3, Unresolved: boolPtr(true), Author: gerrit.AccountInfo{Name: "Rev3"}, Message: "Third issue"},
		},
	}
	summary := AnalyzeComments(published, nil)
	if summary.TotalThreads != 3 || summary.UnresolvedThreads != 3 {
		t.Fatalf("Expected 3 unresolved threads, got %+v", summary)
	}

	// Should show hysteresis hint in header
	if !strings.Contains(summary.FormattedText, "⚠ 3 unresolved threads (use 'gh pr view --comments' to view all):") {
		t.Errorf("Expected hysteresis header, got:\n%s", summary.FormattedText)
	}
	// Should show first 2 (a.go and b.go)
	if !strings.Contains(summary.FormattedText, "a.go:10") || !strings.Contains(summary.FormattedText, "b.go:20") {
		t.Errorf("Expected a.go and b.go previews, got:\n%s", summary.FormattedText)
	}
	// Should NOT show c.go inline
	if strings.Contains(summary.FormattedText, "c.go:30") {
		t.Errorf("Did not expect c.go preview inline, got:\n%s", summary.FormattedText)
	}
	// Should show remaining counter
	if !strings.Contains(summary.FormattedText, "... and 1 more unresolved thread") {
		t.Errorf("Expected '... and 1 more unresolved thread', got:\n%s", summary.FormattedText)
	}
}

func TestAnalyzeComments_ChangeLevelComment(t *testing.T) {
	published := map[string][]gerrit.CommentInfo{
		"/PATCHSET_LEVEL": {
			{
				ID:         "c1",
				PatchSet:   14,
				Unresolved: boolPtr(true),
				Author:     gerrit.AccountInfo{Email: "approver@google.com"},
				Message:    "Please update the commit message description.",
			},
		},
	}
	summary := AnalyzeComments(published, nil)
	if summary.TotalThreads != 1 || summary.UnresolvedThreads != 1 {
		t.Fatalf("Expected 1 unresolved thread, got %+v", summary)
	}

	if !strings.Contains(summary.FormattedText, "• Change comment [PS14] by approver@google.com:") {
		t.Errorf("Expected 'Change comment [PS14]' format, got:\n%s", summary.FormattedText)
	}
	if !strings.Contains(summary.FormattedText, "\"Please update the commit message description.\"") {
		t.Errorf("Expected message snippet, got:\n%s", summary.FormattedText)
	}
}

func TestAnalyzeComments_MultiLineAndLongSnippet(t *testing.T) {
	longMsg := "This is the first line.\nThis is the second line with lots of text that goes way beyond eighty characters to verify truncation is working properly."
	published := map[string][]gerrit.CommentInfo{
		"foo.go": {
			{
				ID:         "c1",
				Line:       5,
				Unresolved: boolPtr(true),
				Message:    longMsg,
			},
		},
	}
	summary := AnalyzeComments(published, nil)
	if summary.UnresolvedThreads != 1 {
		t.Fatalf("Expected 1 unresolved thread, got %+v", summary)
	}
	// Verify newlines collapsed
	if strings.Contains(summary.FormattedText, "\nThis is the second line") {
		t.Errorf("Expected newline to be collapsed, got:\n%s", summary.FormattedText)
	}
	// Verify truncation with ellipsis
	if !strings.Contains(summary.FormattedText, "...") {
		t.Errorf("Expected ellipsis truncation, got:\n%s", summary.FormattedText)
	}
}

func TestAnalyzeComments_ReviewerReopensThread(t *testing.T) {
	t1 := time.Now()
	t2 := t1.Add(time.Minute)
	t3 := t2.Add(time.Minute)

	published := map[string][]gerrit.CommentInfo{
		"bar.go": {
			{
				ID:         "c1",
				Line:       25,
				PatchSet:   1,
				Unresolved: boolPtr(true),
				Updated:    timeTimestamp(t1),
				Author:     gerrit.AccountInfo{Name: "Reviewer"},
				Message:    "Needs refactor",
			},
			{
				ID:         "c2",
				InReplyTo:  "c1",
				Line:       25,
				PatchSet:   2,
				Unresolved: boolPtr(false), // author thought they resolved it
				Updated:    timeTimestamp(t2),
				Author:     gerrit.AccountInfo{Name: "Author"},
				Message:    "Refactored",
			},
			{
				ID:         "c3",
				InReplyTo:  "c2",
				Line:       25,
				PatchSet:   3,
				Unresolved: boolPtr(true), // reviewer reopens
				Updated:    timeTimestamp(t3),
				Author:     gerrit.AccountInfo{Name: "Reviewer"},
				Message:    "Still not right, please check nil case",
			},
		},
	}
	summary := AnalyzeComments(published, nil)
	if summary.TotalThreads != 1 || summary.UnresolvedThreads != 1 {
		t.Fatalf("Expected 1 unresolved thread, got %+v", summary)
	}
	if summary.Unresolved[0].Message != "Still not right, please check nil case" {
		t.Errorf("Expected latest message, got %q", summary.Unresolved[0].Message)
	}
	if summary.Unresolved[0].Author != "Reviewer" {
		t.Errorf("Expected latest author 'Reviewer', got %q", summary.Unresolved[0].Author)
	}
	if summary.Unresolved[0].PatchSet != 3 {
		t.Errorf("Expected latest patchset 3, got %d", summary.Unresolved[0].PatchSet)
	}
}
