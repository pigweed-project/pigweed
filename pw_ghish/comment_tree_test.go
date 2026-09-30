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

	t.Run("formats published comments and drafts with resolution and draft indicators", func(t *testing.T) {
		published := map[string][]gerrit.CommentInfo{
			"ring_buffer.cc": {
				{
					ID:         "c1",
					Line:       42,
					Author:     gerrit.AccountInfo{Name: "Alice"},
					Unresolved: boolPtr(true),
					Message:    "Why was this file deleted?",
				},
				{
					ID:         "c2",
					Line:       100,
					Author:     gerrit.AccountInfo{Name: "Bob"},
					Unresolved: boolPtr(false),
					Message:    "Looks good.",
				},
			},
		}
		drafts := map[string][]gerrit.CommentInfo{
			"ring_buffer.cc": {
				{
					ID:         "d1",
					InReplyTo:  "c1",
					Line:       42,
					Unresolved: boolPtr(false),
					Message:    "Reverted.",
				},
			},
			"standalone_draft.cc": {
				{
					ID:         "d2",
					Line:       84,
					Unresolved: boolPtr(true),
					Message:    "FOR GEMINI: Please switch to pw::Result.",
				},
			},
		}

		got := FormatCommentForestWithDrafts(published, drafts)

		if !strings.Contains(got, "Line 42: Alice [unresolved]\n    Why was this file deleted?") {
			t.Errorf("expected unresolved tag on root c1, got:\n%s", got)
		}
		if !strings.Contains(got, "-> [DRAFT, resolved]: Reverted.") {
			t.Errorf("expected draft reply with resolved indicator under c1, got:\n%s", got)
		}
		if !strings.Contains(got, "Line 100: Bob [resolved]\n    Looks good.") {
			t.Errorf("expected resolved tag on root c2, got:\n%s", got)
		}
		if !strings.Contains(got, "File: standalone_draft.cc\n  Line 84: [DRAFT] [unresolved]\n    FOR GEMINI: Please switch to pw::Result.") {
			t.Errorf("expected standalone draft file and root formatting, got:\n%s", got)
		}
	})

	t.Run("includes patchset tags on roots, cross-patchset replies, and cleans up /PATCHSET_LEVEL", func(t *testing.T) {
		published := map[string][]gerrit.CommentInfo{
			"ring_buffer.cc": {
				{
					ID:         "c1",
					Line:       42,
					PatchSet:   1,
					Author:     gerrit.AccountInfo{Name: "Alice"},
					Unresolved: boolPtr(true),
					Message:    "Comment on PS1",
				},
				{
					ID:         "c2",
					InReplyTo:  "c1",
					Line:       42,
					PatchSet:   2,
					Author:     gerrit.AccountInfo{Name: "Bob"},
					Unresolved: boolPtr(false),
					Message:    "Fixed in PS2",
				},
			},
		}
		drafts := map[string][]gerrit.CommentInfo{
			"/PATCHSET_LEVEL": {
				{
					ID:         "d_top",
					PatchSet:   2,
					Unresolved: boolPtr(false),
					Message:    "Overall review draft",
				},
			},
		}

		got := FormatCommentForestWithDrafts(published, drafts)

		for _, want := range []string{
			"File: /PATCHSET_LEVEL (Change comment)",
			"Line - [PS2]: [DRAFT]\n    Overall review draft",
			"File: ring_buffer.cc",
			"Line 42 [PS1]: Alice [resolved]\n    Comment on PS1",
			"-> Bob [PS2]: Fixed in PS2",
		} {
			if !strings.Contains(got, want) {
				t.Errorf("expected %q in formatted forest, got:\n%s", want, got)
			}
		}
		if strings.Contains(got, "Line - [PS2]: [DRAFT] [resolved]") {
			t.Errorf("did not expect spurious [resolved] tag on single-node /PATCHSET_LEVEL draft, got:\n%s", got)
		}
	})
}

func TestFindDraftAtTarget(t *testing.T) {
	t1 := time.Now()
	t2 := t1.Add(time.Minute)

	drafts := []gerrit.CommentInfo{
		{ID: "d_line_old", Line: 10, PatchSet: 1, Updated: timeTimestamp(t1), Message: "Old standalone draft"},
		{ID: "d_line_new", Line: 10, PatchSet: 2, Updated: timeTimestamp(t2), Message: "New standalone draft"},
		{ID: "d_reply", Line: 10, PatchSet: 2, InReplyTo: "parent_1", Updated: timeTimestamp(t1), Message: "Reply draft"},
	}

	t.Run("prefers exact InReplyTo match over newer line match", func(t *testing.T) {
		got := FindDraftAtTarget(drafts, 10, "parent_1")
		if got == nil || got.ID != "d_reply" {
			t.Fatalf("expected d_reply, got %+v", got)
		}
	})

	t.Run("does NOT clobber draft reply belonging to a different thread on the same line", func(t *testing.T) {
		if got := FindDraftAtTarget(drafts, 10, "unrelated_thread_id"); got != nil {
			t.Fatalf("expected nil so draft reply on another thread is not clobbered, got %+v", got)
		}
	})

	t.Run("does NOT clobber standalone draft when replying to published comment thread", func(t *testing.T) {
		standaloneOnly := []gerrit.CommentInfo{
			{ID: "d_standalone", Line: 10, PatchSet: 2, Updated: timeTimestamp(t2), Message: "Standalone draft"},
		}
		if got := FindDraftAtTarget(standaloneOnly, 10, "published_parent_id"); got != nil {
			t.Fatalf("expected nil so standalone draft is not clobbered by thread reply, got %+v", got)
		}
	})

	t.Run("matches latest standalone draft when no published thread exists", func(t *testing.T) {
		got := FindDraftAtTarget(drafts, 10, "")
		if got == nil || got.ID != "d_line_new" {
			t.Fatalf("expected d_line_new, got %+v", got)
		}
	})

	t.Run("returns nil when no draft matches", func(t *testing.T) {
		if got := FindDraftAtTarget(drafts, 99, ""); got != nil {
			t.Errorf("expected nil, got %+v", got)
		}
	})
}

func TestBranchedThreadChronologicalResolution(t *testing.T) {
	t1 := time.Now()
	t2 := t1.Add(1 * time.Minute)
	t3 := t1.Add(2 * time.Minute)
	t4 := t1.Add(3 * time.Minute)

	// Root has two child branches:
	//   Branch A: root -> branchA (t2, unresolved=true) -> leafA (t4, unresolved=false)
	//   Branch B: root -> branchB (t3, unresolved=true)
	// Depth-first traversal visits Branch B last, but leafA (t4) is chronologically newer than branchB (t3).
	comments := []gerrit.CommentInfo{
		{ID: "root", Line: 10, Updated: timeTimestamp(t1), Unresolved: boolPtr(true), Message: "Root"},
		{ID: "branchA", InReplyTo: "root", Line: 10, Updated: timeTimestamp(t2), Unresolved: boolPtr(true), Message: "Branch A"},
		{ID: "branchB", InReplyTo: "root", Line: 10, Updated: timeTimestamp(t3), Unresolved: boolPtr(true), Message: "Branch B"},
		{ID: "leafA", InReplyTo: "branchA", Line: 10, Updated: timeTimestamp(t4), Unresolved: boolPtr(false), Message: "Leaf A resolves thread"},
	}

	threads := BuildCommentThreads("foo.cc", comments, nil)
	if len(threads) != 1 {
		t.Fatalf("expected 1 thread, got %d", len(threads))
	}
	if threads[0].Unresolved {
		t.Errorf("expected branched thread to be resolved based on newest node leafA (t4), got Unresolved=true")
	}
}

func TestFindMatchingDraftsAtTarget_PatchSetFilter(t *testing.T) {
	t1 := time.Now()
	t2 := t1.Add(time.Minute)

	drafts := []gerrit.CommentInfo{
		{ID: "d_ps1", Line: 10, PatchSet: 1, Updated: timeTimestamp(t1), Message: "PS1 draft"},
		{ID: "d_ps2", Line: 10, PatchSet: 2, Updated: timeTimestamp(t2), Message: "PS2 draft"},
	}

	all := FindMatchingDraftsAtTarget(drafts, 10, "", 0)
	if len(all) != 2 {
		t.Fatalf("expected 2 drafts when patchSet=0, got %d", len(all))
	}

	ps1Only := FindMatchingDraftsAtTarget(drafts, 10, "", 1)
	if len(ps1Only) != 1 || ps1Only[0].ID != "d_ps1" {
		t.Fatalf("expected [d_ps1] when patchSet=1, got %+v", ps1Only)
	}
}

func TestFindLatestCommentAtLineForPatchSet(t *testing.T) {
	t1 := time.Now()
	t2 := t1.Add(time.Minute)

	comments := []gerrit.CommentInfo{
		{ID: "c_ps1", Line: 10, PatchSet: 1, Updated: timeTimestamp(t1)},
		{ID: "c_ps2", Line: 10, PatchSet: 2, Updated: timeTimestamp(t2)},
	}

	if got := FindLatestCommentAtLineForPatchSet(comments, 10, 1); got == nil || got.ID != "c_ps1" {
		t.Errorf("expected c_ps1 for patchSet=1, got %+v", got)
	}
	if got := FindLatestCommentAtLineForPatchSet(comments, 10, 0); got == nil || got.ID != "c_ps2" {
		t.Errorf("expected c_ps2 for patchSet=0, got %+v", got)
	}
}
