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

func TestFindGitHubIssueRefs(t *testing.T) {
	tests := []struct {
		name string
		text string
		want []GitHubIssueRef
	}{
		{
			// The canonical failure: an agent with `gh` muscle memory writes
			// GitHub's closing keyword into the commit body. On Gerrit this is
			// inert prose -- the bug is never linked and never closed.
			name: "closing keyword",
			text: "pw_foo: Rework retry\n\nRework the retry loop.\n\nFixes #456\n",
			want: []GitHubIssueRef{{Keyword: "Fixes", Number: "456", Match: "Fixes #456"}},
		},
		{
			name: "every GitHub closing keyword is recognized",
			text: "s: t\n\nclose #1\ncloses #2\nclosed #3\nfix #4\nfixes #5\nfixed #6\n" +
				"resolve #7\nresolves #8\nresolved #9\n",
			want: []GitHubIssueRef{
				{Keyword: "close", Number: "1", Match: "close #1"},
				{Keyword: "closes", Number: "2", Match: "closes #2"},
				{Keyword: "closed", Number: "3", Match: "closed #3"},
				{Keyword: "fix", Number: "4", Match: "fix #4"},
				{Keyword: "fixes", Number: "5", Match: "fixes #5"},
				{Keyword: "fixed", Number: "6", Match: "fixed #6"},
				{Keyword: "resolve", Number: "7", Match: "resolve #7"},
				{Keyword: "resolves", Number: "8", Match: "resolves #8"},
				{Keyword: "resolved", Number: "9", Match: "resolved #9"},
			},
		},
		{
			// Someone reaching for a Gerrit trailer but supplying a GitHub
			// number. Still wrong, still worth catching.
			name: "trailer-shaped line with a GitHub number",
			text: "s: t\n\nFixed: #456\n",
			want: []GitHubIssueRef{{Keyword: "Fixed", Number: "456", Match: "Fixed: #456"}},
		},
		{
			// The whole point of the feature: a correct Gerrit trailer must
			// never be flagged.
			name: "correct Gerrit trailers are not flagged",
			text: "s: t\n\nBody.\n\nBug: b/12345\nFixed: b/67890\n",
			want: nil,
		},
		{
			// Deliberately NOT flagged. A bare "#456" is far more likely to be
			// ordinary prose than an issue link, and a false refusal blocks a
			// user who did nothing wrong.
			name: "bare issue number is not flagged",
			text: "s: t\n\nSee #456 for context, and item #3 of the design doc.\n",
			want: nil,
		},
		{
			// Indented text is a code sample, matching how trailers are
			// detected elsewhere. Refusing to edit a commit because its
			// example output mentions "Fixes #1" would be absurd.
			name: "indented code sample is not flagged",
			text: "s: t\n\nExample GitHub workflow:\n\n  Fixes #456\n",
			want: nil,
		},
		{
			name: "word boundaries are respected",
			text: "s: t\n\nThe prefixes #456 and suffixes #789 are unrelated.\n",
			want: nil,
		},
		{
			name: "empty text",
			text: "",
			want: nil,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := FindGitHubIssueRefs(tt.text)
			if !reflect.DeepEqual(got, tt.want) {
				t.Errorf("FindGitHubIssueRefs()\n got: %#v\nwant: %#v", got, tt.want)
			}
		})
	}
}

// TestGitHubIssueSyntaxError checks the diagnostic against the project's four
// pillars: what happened, why it matters, what to type instead, and how to
// find the missing information.
func TestGitHubIssueSyntaxError(t *testing.T) {
	refs := FindGitHubIssueRefs("s: t\n\nFixes #456\n")
	if len(refs) != 1 {
		t.Fatalf("setup: expected 1 ref, got %d", len(refs))
	}
	err := GitHubIssueSyntaxError(refs, "--body")
	if err == nil {
		t.Fatal("Expected an error, got nil")
	}
	msg := err.Error()

	for _, want := range []string{
		"Fixes #456", // pillar 1: quote what was found
		"--body",     // pillar 1: say where
		"Gerrit",     // pillar 2: why it does not work
		"Fixed: b/",  // pillar 3: the trailer to use instead
		"Bug: b/",    // pillar 3: the non-closing variant
		"--fixed",    // pillar 3: the flag that writes it for you
		"--bug",
	} {
		if !strings.Contains(msg, want) {
			t.Errorf("Error message is missing %q:\n%s", want, msg)
		}
	}

	// Pillar 2, the part that justifies refusing rather than translating: the
	// number spaces are different, so b/456 would be an unrelated bug.
	if !strings.Contains(msg, "b/456") {
		t.Errorf("Error should explain that b/456 would be a different bug:\n%s", msg)
	}
}
