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

func TestBuildCommentForest_Empty(t *testing.T) {
	if roots := BuildCommentForest(nil); roots != nil {
		t.Errorf("expected nil roots for nil comments, got %v", roots)
	}
	if roots := BuildCommentForest([]gerrit.CommentInfo{}); roots != nil {
		t.Errorf("expected nil roots for empty comments, got %v", roots)
	}
}

func TestBuildCommentForest_LinearChain(t *testing.T) {
	t1 := time.Now()
	t2 := t1.Add(time.Minute)
	t3 := t2.Add(time.Minute)

	comments := []gerrit.CommentInfo{
		{ID: "c1", Line: 10, Message: "Root", Updated: timeTimestamp(t1)},
		{ID: "c2", InReplyTo: "c1", Line: 10, Message: "Reply 1", Updated: timeTimestamp(t2)},
		{ID: "c3", InReplyTo: "c2", Line: 10, Message: "Reply 2", Updated: timeTimestamp(t3)},
	}

	roots := BuildCommentForest(comments)
	if len(roots) != 1 {
		t.Fatalf("expected 1 root, got %d", len(roots))
	}
	if roots[0].Comment.ID != "c1" {
		t.Errorf("expected root c1, got %s", roots[0].Comment.ID)
	}
	if len(roots[0].Children) != 1 || roots[0].Children[0].Comment.ID != "c2" {
		t.Fatalf("expected child c2 under c1, got %+v", roots[0].Children)
	}
	if len(roots[0].Children[0].Children) != 1 || roots[0].Children[0].Children[0].Comment.ID != "c3" {
		t.Fatalf("expected child c3 under c2, got %+v", roots[0].Children[0].Children)
	}
}

func TestBuildCommentForest_Branching(t *testing.T) {
	t1 := time.Now()
	t2 := t1.Add(time.Minute)
	t3 := t2.Add(time.Minute)

	comments := []gerrit.CommentInfo{
		{ID: "c1", Line: 20, Message: "Root", Updated: timeTimestamp(t1)},
		{ID: "c2", InReplyTo: "c1", Line: 20, Message: "Branch A", Updated: timeTimestamp(t2)},
		{ID: "c3", InReplyTo: "c1", Line: 20, Message: "Branch B", Updated: timeTimestamp(t3)},
	}

	roots := BuildCommentForest(comments)
	if len(roots) != 1 {
		t.Fatalf("expected 1 root, got %d", len(roots))
	}
	if len(roots[0].Children) != 2 {
		t.Fatalf("expected 2 children, got %d", len(roots[0].Children))
	}
	if roots[0].Children[0].Comment.ID != "c2" || roots[0].Children[1].Comment.ID != "c3" {
		t.Errorf("expected children [c2, c3], got [%s, %s]",
			roots[0].Children[0].Comment.ID, roots[0].Children[1].Comment.ID)
	}
}

func TestBuildCommentForest_OrphanPromotion(t *testing.T) {
	comments := []gerrit.CommentInfo{
		{ID: "c1", Line: 10, Message: "Root"},
		{ID: "c2", InReplyTo: "missing_parent", Line: 15, Message: "Orphan"},
	}

	roots := BuildCommentForest(comments)
	if len(roots) != 2 {
		t.Fatalf("expected orphan to be promoted to root, got %d roots", len(roots))
	}
	if roots[0].Comment.ID != "c1" || roots[1].Comment.ID != "c2" {
		t.Errorf("expected roots [c1, c2], got [%s, %s]", roots[0].Comment.ID, roots[1].Comment.ID)
	}
}

func TestBuildCommentForest_CycleHandling(t *testing.T) {
	t.Run("direct cycle c1 <-> c2", func(t *testing.T) {
		comments := []gerrit.CommentInfo{
			{ID: "c1", InReplyTo: "c2", Line: 10, Message: "C1"},
			{ID: "c2", InReplyTo: "c1", Line: 10, Message: "C2"},
		}

		// Must terminate and not crash or infinite loop
		roots := BuildCommentForest(comments)
		if len(roots) == 0 {
			t.Fatal("expected at least 1 root to be preserved")
		}
	})

	t.Run("self-loop c1 -> c1", func(t *testing.T) {
		comments := []gerrit.CommentInfo{
			{ID: "c1", InReplyTo: "c1", Line: 10, Message: "Self"},
		}

		roots := BuildCommentForest(comments)
		if len(roots) != 1 || roots[0].Comment.ID != "c1" {
			t.Fatalf("expected c1 to be root, got %v", roots)
		}
		if len(roots[0].Children) != 0 {
			t.Errorf("expected 0 children for self loop, got %d", len(roots[0].Children))
		}
	})
}

func TestBuildCommentForest_Sorting(t *testing.T) {
	comments := []gerrit.CommentInfo{
		{ID: "c3", Line: 50, Message: "Line 50"},
		{ID: "c1", Line: 0, Message: "File-level"},
		{ID: "c2", Line: 10, Message: "Line 10"},
	}

	roots := BuildCommentForest(comments)
	if len(roots) != 3 {
		t.Fatalf("expected 3 roots, got %d", len(roots))
	}
	if roots[0].Comment.ID != "c1" || roots[1].Comment.ID != "c2" || roots[2].Comment.ID != "c3" {
		t.Errorf("expected line order [c1 (0), c2 (10), c3 (50)], got [%s, %s, %s]",
			roots[0].Comment.ID, roots[1].Comment.ID, roots[2].Comment.ID)
	}
}

func TestBuildCommentThreads(t *testing.T) {
	t1 := time.Now()
	t2 := t1.Add(time.Minute)

	comments := []gerrit.CommentInfo{
		{ID: "c1", Line: 10, PatchSet: 1, Unresolved: boolPtr(true), Updated: timeTimestamp(t1), Message: "Fix this"},
		{ID: "c2", InReplyTo: "c1", Line: 10, PatchSet: 2, Unresolved: boolPtr(false), Updated: timeTimestamp(t2), Message: "Fixed"},
		{ID: "c3", Line: 30, PatchSet: 2, Unresolved: boolPtr(true), Updated: timeTimestamp(t2), Message: "Another issue"},
	}
	draftReplies := map[string]bool{
		"c3": true,
	}

	threads := BuildCommentThreads("main.go", comments, draftReplies)
	if len(threads) != 2 {
		t.Fatalf("expected 2 threads, got %d", len(threads))
	}

	// Thread 1: c1 -> c2 (resolved)
	th1 := threads[0]
	if th1.Line != 10 {
		t.Errorf("expected th1 line 10, got %d", th1.Line)
	}
	if th1.Unresolved {
		t.Errorf("expected th1 to be resolved")
	}
	if th1.HasDraftReply {
		t.Errorf("expected th1 not to have draft reply")
	}
	if th1.Latest.ID != "c2" {
		t.Errorf("expected th1 latest to be c2, got %s", th1.Latest.ID)
	}

	// Thread 2: c3 (unresolved, has draft reply)
	th2 := threads[1]
	if th2.Line != 30 {
		t.Errorf("expected th2 line 30, got %d", th2.Line)
	}
	if !th2.Unresolved {
		t.Errorf("expected th2 to be unresolved")
	}
	if !th2.HasDraftReply {
		t.Errorf("expected th2 to have draft reply")
	}
	if th2.Latest.ID != "c3" {
		t.Errorf("expected th2 latest to be c3, got %s", th2.Latest.ID)
	}
}

func TestFindLatestCommentAtLine(t *testing.T) {
	t1 := time.Now()
	t2 := t1.Add(time.Minute)
	t3 := t2.Add(time.Minute)

	comments := []gerrit.CommentInfo{
		{ID: "c1", Line: 10, Updated: timeTimestamp(t1)},
		{ID: "c2", Line: 10, Updated: timeTimestamp(t3)},
		{ID: "c3", Line: 10, Updated: timeTimestamp(t2)},
		{ID: "c4", Line: 20, Updated: timeTimestamp(t3)},
	}

	t.Run("finds latest on line 10", func(t *testing.T) {
		got := FindLatestCommentAtLine(comments, 10)
		if got == nil || got.ID != "c2" {
			t.Errorf("expected latest c2, got %v", got)
		}
	})

	t.Run("finds latest on line 20", func(t *testing.T) {
		got := FindLatestCommentAtLine(comments, 20)
		if got == nil || got.ID != "c4" {
			t.Errorf("expected latest c4, got %v", got)
		}
	})

	t.Run("returns nil for non-matching line", func(t *testing.T) {
		got := FindLatestCommentAtLine(comments, 99)
		if got != nil {
			t.Errorf("expected nil for line 99, got %v", got)
		}
	})

	t.Run("empty slice returns nil", func(t *testing.T) {
		got := FindLatestCommentAtLine(nil, 10)
		if got != nil {
			t.Errorf("expected nil for empty slice, got %v", got)
		}
	})
}

func TestFormatCommentForest(t *testing.T) {
	t.Run("empty map returns empty string", func(t *testing.T) {
		if got := FormatCommentForest(nil); got != "" {
			t.Errorf("expected empty string, got %q", got)
		}
	})

	t.Run("formats nested multi-file comments with indentation", func(t *testing.T) {
		comments := map[string][]gerrit.CommentInfo{
			"b_file.go": {
				{
					ID:      "c1",
					Line:    15,
					Author:  gerrit.AccountInfo{Name: "Alice"},
					Message: "Please rename this function.",
				},
				{
					ID:        "c2",
					InReplyTo: "c1",
					Line:      15,
					Author:    gerrit.AccountInfo{Name: "Bob"},
					Message:   "Done.\nRenamed to Init().",
				},
			},
			"a_file.go": {
				{
					ID:      "c0",
					Line:    0,
					Author:  gerrit.AccountInfo{Name: "Reviewer"},
					Message: "Overall looks good.",
				},
			},
		}

		got := FormatCommentForest(comments)

		// Must sort files alphabetically: a_file.go before b_file.go
		idxA := strings.Index(got, "File: a_file.go")
		idxB := strings.Index(got, "File: b_file.go")
		if idxA == -1 || idxB == -1 || idxA >= idxB {
			t.Fatalf("expected a_file.go before b_file.go, got:\n%s", got)
		}

		// Line 0 displayed as "Line -"
		if !strings.Contains(got, "Line -: Reviewer\n    Overall looks good.") {
			t.Errorf("expected Line - formatting, got:\n%s", got)
		}

		// Root on line 15
		if !strings.Contains(got, "Line 15: Alice\n    Please rename this function.") {
			t.Errorf("expected Line 15 root formatting, got:\n%s", got)
		}

		// Nested reply with indented multi-line message
		if !strings.Contains(got, "-> Bob: Done.\n         Renamed to Init().") {
			t.Errorf("expected indented reply formatting, got:\n%s", got)
		}
	})
}
