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
	"reflect"
	"strings"
	"testing"
)

func TestCommitMessage_ParseAndRoundTripFormat(t *testing.T) {
	const cid = "Change-Id: I1234567890abcdef1234567890abcdef12345678"

	tests := []struct {
		name string
		raw  string
		want string
	}{
		{
			name: "empty message",
			raw:  "",
			want: "",
		},
		{
			name: "subject only",
			raw:  "pw_foo: Add bar\n",
			want: "pw_foo: Add bar\n",
		},
		{
			name: "subject, prose body, and trailer block",
			raw:  "pw_foo: Add bar\n\nDetailed prose body.\nSecond line of prose.\n\nBug: b/12345\n" + cid + "\n",
			want: "pw_foo: Add bar\n\nDetailed prose body.\nSecond line of prose.\n\nBug: b/12345\n" + cid + "\n",
		},
		{
			name: "preserves multiple blank lines between paragraphs",
			raw:  "pw_foo: Add bar\n\n\nParagraph after two blank lines.\n\n\nBug: b/12345\n" + cid + "\n",
			want: "pw_foo: Add bar\n\n\nParagraph after two blank lines.\n\n\nBug: b/12345\n" + cid + "\n",
		},
		{
			name: "preserves continuation lines and cherry-pick footer in trailer block",
			raw: "pw_foo: Add bar\n\nBody.\n\nBug: b/12345\nTest: ran tests\n  with extra flag\n" +
				cid + "\n(cherry picked from commit deadbeef)\n",
			want: "pw_foo: Add bar\n\nBody.\n\nBug: b/12345\nTest: ran tests\n  with extra flag\n" +
				cid + "\n(cherry picked from commit deadbeef)\n",
		},
		{
			name: "normalizes CRLF to LF on Format",
			raw:  "pw_foo: Add bar\r\n\r\nBody.\r\n\r\nBug: b/12345\r\n" + cid + "\r\n",
			want: "pw_foo: Add bar\n\nBody.\n\nBug: b/12345\n" + cid + "\n",
		},
		{
			name: "preserves standalone two-space indented trailer examples",
			raw: "pw_foo: Document trailer format\n\nExample trailers:\n\n" +
				"  Bug: b/<id>\n  Fixed: b/<id>\n\n" +
				"  Change-Id: <id>\n\nBug: b/12345\n" + cid + "\n",
			want: "pw_foo: Document trailer format\n\nExample trailers:\n\n" +
				"  Bug: b/<id>\n  Fixed: b/<id>\n\n" +
				"  Change-Id: <id>\n\nBug: b/12345\n" + cid + "\n",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			ir := ParseCommitMessage(tc.raw)
			if got := ir.Format(); got != tc.want {
				t.Errorf("ParseCommitMessage(%q).Format()\n got: %q\nwant: %q", tc.raw, got, tc.want)
			}
		})
	}
}

func TestCommitMessage_IRStructureAndDirectManipulation(t *testing.T) {
	const cid = "I1234567890abcdef1234567890abcdef12345678"
	raw := "pw_foo: Add bar\n\nFirst body paragraph:\n  indented: code_sample\n\n" +
		"Note: prose paragraph with colon on line 1\nand non-indented second line.\n\n" +
		"Bug: https://issues.pigweed.dev/issues/111\n" +
		"Test: step 1\n  step 2\n" +
		"Change-Id: " + cid + "\n" +
		"(cherry picked from commit deadbeef)\n"

	ir := ParseCommitMessage(raw)
	if ir.Subject != "pw_foo: Add bar" {
		t.Fatalf("Subject = %q, want 'pw_foo: Add bar'", ir.Subject)
	}
	if len(ir.Paragraphs) != 3 {
		t.Fatalf("len(Paragraphs) = %d, want 3", len(ir.Paragraphs))
	}
	if ir.Paragraphs[0].IsTrailerBlock {
		t.Errorf("Paragraphs[0] should be a body paragraph, got IsTrailerBlock=true")
	}
	if ir.Paragraphs[1].IsTrailerBlock {
		t.Errorf("Paragraphs[1] should be a body paragraph, got IsTrailerBlock=true")
	}
	if !ir.Paragraphs[2].IsTrailerBlock {
		t.Fatalf("Paragraphs[2] should be a trailer block, got IsTrailerBlock=false")
	}

	trailers := ir.Paragraphs[2].Trailers
	if len(trailers) != 4 {
		t.Fatalf("len(Trailers) = %d, want 4", len(trailers))
	}
	if trailers[0].Key != "Bug" || trailers[0].Value != "https://issues.pigweed.dev/issues/111" {
		t.Errorf("trailers[0] = %+v", trailers[0])
	}
	if trailers[1].Key != "Test" || !reflect.DeepEqual(trailers[1].Continuations, []string{"  step 2"}) {
		t.Errorf("trailers[1] = %+v", trailers[1])
	}
	if trailers[2].Key != "Change-Id" || trailers[2].Value != cid {
		t.Errorf("trailers[2] = %+v", trailers[2])
	}
	if !trailers[3].IsKeylessFooter || trailers[3].Value != "(cherry picked from commit deadbeef)" {
		t.Errorf("trailers[3] = %+v", trailers[3])
	}

	// Mutate the IR directly (subject, trailer value, and normalize bug footers)
	// and verify reconstitution.
	ir.Subject = "pw_foo: Updated subject"
	ir.NormalizeBugFooters("Bug: {id}")
	if err := ir.UpsertTrailer("Reviewed-by: Alice <alice@example.com>"); err != nil {
		t.Fatalf("UpsertTrailer failed: %v", err)
	}

	want := "pw_foo: Updated subject\n\nFirst body paragraph:\n  indented: code_sample\n\n" +
		"Note: prose paragraph with colon on line 1\nand non-indented second line.\n\n" +
		"Bug: 111\n" +
		"Test: step 1\n  step 2\n" +
		"Change-Id: " + cid + "\n" +
		"(cherry picked from commit deadbeef)\n" +
		"Reviewed-by: Alice <alice@example.com>\n"

	if got := ir.Format(); got != want {
		t.Errorf("reconstituted commit message mismatch:\ngot:\n%s\nwant:\n%s", got, want)
	}
}

func TestExtractAndMergeTrailersWithFormat(t *testing.T) {
	origMsg := "fuchsia: Fix zircon timer\n\n" +
		"Bug: https://issues.fuchsia.dev/issues/420001\n" +
		"  (tracked in zircon bug scrub)\n" +
		"Change-Id: I1111111111111111111111111111111111111111\n"
	trailers := ParseCommitMessage(origMsg).ExtractTrailersWithFormat("Bug: fxbug.dev/{id}")
	wantTrailers := []string{
		"Bug: fxbug.dev/420001\n  (tracked in zircon bug scrub)",
		"Change-Id: I1111111111111111111111111111111111111111",
	}
	if !reflect.DeepEqual(trailers, wantTrailers) {
		t.Fatalf("ExtractTrailersWithFormat() = %#v, want %#v", trailers, wantTrailers)
	}

	merged := MergeTrailersWithFormat(
		"fuchsia: Fix zircon timer\n\nUpdated body.\n\nFixed: 420002\n",
		trailers,
		"Bug: fxbug.dev/{id}",
	)
	for _, want := range []string{
		"Fixed: fxbug.dev/420002",
		"Bug: fxbug.dev/420001\n  (tracked in zircon bug scrub)",
		"Change-Id: I1111111111111111111111111111111111111111",
	} {
		if !strings.Contains(merged, want) {
			t.Errorf("MergeTrailersWithFormat() missing %q in:\n%s", want, merged)
		}
	}
}

func TestCommitMessage_UpsertAndNormalizeBugFooters(t *testing.T) {
	tests := []struct {
		name        string
		orig        string
		upsertLine  string
		trailerFmt  string
		wantMessage string
	}{
		{
			name:        "Bug upgraded to Fixed stays above Change-Id",
			orig:        "pw_foo: Fix crash\n\nBug: b/42\nChange-Id: I1111111111111111111111111111111111111111\n",
			upsertLine:  "Fixed: b/42",
			trailerFmt:  "Bug: b/{id}",
			wantMessage: "pw_foo: Fix crash\n\nFixed: b/42\nChange-Id: I1111111111111111111111111111111111111111\n",
		},
		{
			name:        "Bug None replaced by Fixed stays above Change-Id",
			orig:        "pw_foo: Fix crash\n\nBug: None\nChange-Id: I1111111111111111111111111111111111111111\n",
			upsertLine:  "Fixed: b/42",
			trailerFmt:  "Bug: b/{id}",
			wantMessage: "pw_foo: Fix crash\n\nFixed: b/42\nChange-Id: I1111111111111111111111111111111111111111\n",
		},
		{
			name:        "partial overlap rewrites multi-ID Bug line and keeps Fixed above Change-Id",
			orig:        "pw_foo: Fix crash\n\nBug: b/42, b/99\nChange-Id: I1111111111111111111111111111111111111111\n",
			upsertLine:  "Fixed: b/42",
			trailerFmt:  "Bug: b/{id}",
			wantMessage: "pw_foo: Fix crash\n\nBug: b/99\nFixed: b/42\nChange-Id: I1111111111111111111111111111111111111111\n",
		},
		{
			name:        "distinct closing bugs Fixes b/11 plus Fixed b/22 preserves both above Change-Id",
			orig:        "pw_foo: Fix two crashes\n\nFixes: b/11\nChange-Id: I1111111111111111111111111111111111111111\n",
			upsertLine:  "Fixed: b/22",
			trailerFmt:  "Bug: b/{id}",
			wantMessage: "pw_foo: Fix two crashes\n\nFixed: b/11\nFixed: b/22\nChange-Id: I1111111111111111111111111111111111111111\n",
		},
		{
			name:        "respects custom fxbug.dev trailer format",
			orig:        "zircon: Fix timer\n\nBug: fxbug.dev/42, fxbug.dev/99\nChange-Id: I1111111111111111111111111111111111111111\n",
			upsertLine:  "Fixed: fxbug.dev/42",
			trailerFmt:  "Bug: fxbug.dev/{id}",
			wantMessage: "zircon: Fix timer\n\nBug: fxbug.dev/99\nFixed: fxbug.dev/42\nChange-Id: I1111111111111111111111111111111111111111\n",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			msg := ParseCommitMessage(tc.orig)
			if err := msg.UpsertTrailer(tc.upsertLine); err != nil {
				t.Fatalf("UpsertTrailer(%q) failed: %v", tc.upsertLine, err)
			}
			msg.NormalizeBugFooters(tc.trailerFmt)
			if got := msg.Format(); got != tc.wantMessage {
				t.Errorf("got:\n%s\nwant:\n%s", got, tc.wantMessage)
			}
		})
	}
}

func TestCommitMessage_NormalizeBugFooters_DeduplicationAndPlaceholders(t *testing.T) {
	const cid = "Change-Id: I1111111111111111111111111111111111111111"

	tests := []struct {
		name string
		orig string
		want string
	}{
		{
			name: "deduplicates closing aliases targeting the same bug",
			orig: "pw_foo: Fix crash\n\nFixes: b/11\nFixed: b/11\n" + cid + "\n",
			want: "pw_foo: Fix crash\n\nFixed: b/11\n" + cid + "\n",
		},
		{
			name: "drops Fixed None placeholder when a real Bug is linked",
			orig: "pw_foo: Link issue\n\nFixed: None\nBug: b/42\n" + cid + "\n",
			want: "pw_foo: Link issue\n\nBug: b/42\n" + cid + "\n",
		},
		{
			name: "collapses multiple None placeholders into one",
			orig: "pw_foo: Cleanup\n\nBug: None\nFixed: None\n" + cid + "\n",
			want: "pw_foo: Cleanup\n\nBug: None\n" + cid + "\n",
		},
		{
			name: "deduplicates repeated IDs across multiple Bug lines",
			orig: "pw_foo: Track bugs\n\nBug: b/11\nBug: b/11, b/22\n" + cid + "\n",
			want: "pw_foo: Track bugs\n\nBug: b/11\nBug: b/22\n" + cid + "\n",
		},
		{
			name: "leaves two-space indented trailer examples untouched",
			orig: "pw_foo: Docs\n\n  Bug: 42\n  Fixed: 42\n\nBug: 42\n" + cid + "\n",
			want: "pw_foo: Docs\n\n  Bug: 42\n  Fixed: 42\n\nBug: b/42\n" + cid + "\n",
		},
		{
			name: "ignores non-footer trailer-like lines above prose",
			orig: "pw_foo: Docs\n\nBug: 99\n\nProse after example.\n\nBug: 42\n" + cid + "\n",
			want: "pw_foo: Docs\n\nBug: 99\n\nProse after example.\n\nBug: b/42\n" + cid + "\n",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			msg := ParseCommitMessage(tc.orig)
			msg.NormalizeBugFooters("Bug: b/{id}")
			if got := msg.Format(); got != tc.want {
				t.Errorf("got:\n%s\nwant:\n%s", got, tc.want)
			}
		})
	}
}
