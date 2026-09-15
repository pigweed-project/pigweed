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
	"testing"

	"github.com/google/go-cmp/cmp"
)

func TestExtractBugLinks(t *testing.T) {
	tests := []struct {
		name string
		msg  string
		want []BugLink
	}{
		{
			name: "empty message",
			msg:  "",
			want: []BugLink{},
		},
		{
			name: "subject only is never a trailer",
			msg:  "pw_foo: Add bar",
			want: []BugLink{},
		},
		{
			name: "no bug trailer",
			msg:  "pw_foo: Add bar\n\nBody.\n\nChange-Id: I1234\n",
			want: []BugLink{},
		},
		{
			name: "bug trailer links without closing",
			msg:  "pw_foo: Add bar\n\nChange-Id: I1234\nBug: b/123456\n",
			want: []BugLink{{ID: "b/123456", Closes: false}},
		},
		{
			name: "fixed trailer closes",
			msg:  "pw_foo: Add bar\n\nFixed: b/123456\n",
			want: []BugLink{{ID: "b/123456", Closes: true}},
		},
		{
			name: "fixes and fixing are the same trailer to gerrit",
			msg:  "pw_foo: Add bar\n\nFixes: 123456\nFixing: b/222222\n",
			want: []BugLink{
				{ID: "b/123456", Closes: true},
				{ID: "b/222222", Closes: true},
			},
		},
		{
			name: "issue is an alias for bug",
			msg:  "pw_foo: Add bar\n\nIssue: b/123456\n",
			want: []BugLink{{ID: "b/123456", Closes: false}},
		},
		{
			name: "key case does not matter",
			msg:  "pw_foo: Add bar\n\nbug: b/7654321\n",
			want: []BugLink{{ID: "b/7654321", Closes: false}},
		},
		{
			name: "tracker urls are canonicalized to b/ ids",
			msg:  "pw_foo: Add bar\n\nBug: https://issues.pigweed.dev/issues/123456\n",
			want: []BugLink{{ID: "b/123456", Closes: false}},
		},
		{
			name: "comma separated chain yields one link each",
			msg:  "pw_foo: Add bar\n\nBug: b/111111, b/222222\n",
			want: []BugLink{
				{ID: "b/111111", Closes: false},
				{ID: "b/222222", Closes: false},
			},
		},
		{
			name: "the same bug linked and fixed closes",
			msg:  "pw_foo: Add bar\n\nBug: b/123456\nFixed: b/123456\n",
			want: []BugLink{{ID: "b/123456", Closes: true}},
		},
		{
			// The other order too: a later non-closing trailer must not
			// downgrade a bug the change does close.
			name: "the same bug fixed and linked still closes",
			msg:  "pw_foo: Add bar\n\nFixed: b/123456\nBug: b/123456\n",
			want: []BugLink{{ID: "b/123456", Closes: true}},
		},
		{
			name: "an explicit none is reported, not dropped",
			msg:  "pw_foo: Add bar\n\nBug: none\n",
			want: []BugLink{{ID: "None", Closes: false}},
		},
		{
			name: "free text value is reported verbatim",
			msg:  "pw_foo: Add bar\n\nBug: see the design doc\n",
			want: []BugLink{{ID: "see the design doc", Closes: false}},
		},
		{
			name: "a bug line inside a prose paragraph is not a trailer",
			msg:  "pw_foo: Add bar\n\nThe old code did this because of\nBug: b/999999\nwhich is now fixed upstream.\n",
			want: []BugLink{},
		},
		{
			name: "trailers in an earlier paragraph still count",
			msg:  "pw_foo: Add bar\n\nBug: b/123456\n\nSome trailing prose.\n\nChange-Id: I1234\n",
			want: []BugLink{{ID: "b/123456", Closes: false}},
		},
		{
			name: "continuation lines do not become links",
			msg:  "pw_foo: Add bar\n\nBug: b/123456\n  and see the linked doc\nChange-Id: I1234\n",
			want: []BugLink{{ID: "b/123456", Closes: false}},
		},
		{
			name: "crlf line endings",
			msg:  "pw_foo: Add bar\r\n\r\nFixed: b/123456\r\n",
			want: []BugLink{{ID: "b/123456", Closes: true}},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := ExtractBugLinks(tt.msg)
			if got == nil {
				t.Fatal("ExtractBugLinks returned nil; it must always return a slice so that JSON output is [] rather than null")
			}
			if diff := cmp.Diff(tt.want, got); diff != "" {
				t.Errorf("ExtractBugLinks(%q) mismatch (-want +got):\n%s", tt.msg, diff)
			}
		})
	}
}

func TestFormatBugLinks(t *testing.T) {
	tests := []struct {
		name  string
		links []BugLink
		want  string
	}{
		{name: "none", links: []BugLink{}, want: ""},
		{name: "one", links: []BugLink{{ID: "b/1"}}, want: "b/1"},
		{
			name:  "several, closing or not",
			links: []BugLink{{ID: "b/1"}, {ID: "b/2", Closes: true}},
			want:  "b/1, b/2",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := FormatBugLinks(tt.links); got != tt.want {
				t.Errorf("FormatBugLinks(%v) = %q, want %q", tt.links, got, tt.want)
			}
		})
	}
}

// TestExtractBugLinks_AgreesWithUpsertTrailer pins the round trip that makes
// the field useful: whatever `pr edit --bug` writes must be what `pr view
// --json bug` reads back. If these two ever drift, an agent that sets a bug
// and then verifies it would see its own write disappear.
func TestExtractBugLinks_AgreesWithUpsertTrailer(t *testing.T) {
	const orig = "pw_foo: Add bar\n\nBody.\n\nChange-Id: I1234\n"

	for _, tt := range []struct {
		key    string
		value  string
		want   string
		closes bool
	}{
		{key: "Bug", value: "123456", want: "b/123456", closes: false},
		{key: "Fixed", value: "b/123456", want: "b/123456", closes: true},
		{key: "Bug", value: "https://issues.pigweed.dev/issues/42", want: "b/42", closes: false},
	} {
		t.Run(tt.key+" "+tt.value, func(t *testing.T) {
			updated, err := UpsertTrailer(orig, NormalizeTrailer(tt.key+": "+tt.value))
			if err != nil {
				t.Fatalf("UpsertTrailer: %v", err)
			}
			want := []BugLink{{ID: tt.want, Closes: tt.closes}}
			if diff := cmp.Diff(want, ExtractBugLinks(updated)); diff != "" {
				t.Errorf("round trip mismatch (-want +got):\n%s\nmessage:\n%s", diff, updated)
			}
		})
	}
}
