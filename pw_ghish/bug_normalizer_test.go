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
)

func TestNormalizeBugID(t *testing.T) {
	tests := []struct {
		input    string
		wantID   string
		wantNorm bool
	}{
		{"https://issues.pigweed.dev/issues/123456", "b/123456", true},
		{"https://bugs.chromium.org/p/pigweed/issues/detail?id=987654", "b/987654", true},
		{"https://bugs.chromium.org/p/pigweed/issues/detail?id=987654&q=status:open", "b/987654", true},
		{"https://bugs.chromium.org/p/pigweed/issues/detail?id=987654#c2", "b/987654", true},
		{"b/123456", "b/123456", true},
		{"b:123456", "b/123456", true},
		{"fxb/98765", "b/98765", true},
		{"fxbug.dev/98765", "b/98765", true},
		{"123456", "b/123456", true},
		{"none", "none", false},
		{"fxbug/98765", "fxbug/98765", false},
		{"https://github.com/google/pigweed/issues/42", "https://github.com/google/pigweed/issues/42", false},
		{"", "", false},
	}

	for _, tc := range tests {
		got, ok := NormalizeBugID(tc.input)
		if got != tc.wantID || ok != tc.wantNorm {
			t.Errorf("NormalizeBugID(%q) = (%q, %v), want (%q, %v)", tc.input, got, ok, tc.wantID, tc.wantNorm)
		}
	}
}

func TestNormalizeBugChain(t *testing.T) {
	tests := []struct {
		input string
		want  string
	}{
		{"", ""},
		{"none", "None"},
		{"None", "None"},
		{"NONE", "None"},
		{"https://issues.pigweed.dev/issues/123456", "b/123456"},
		{"b/123456", "b/123456"},
		{"b:123456", "b/123456"},
		{"123456", "b/123456"},
		// Chains
		{"123456, 789012", "b/123456, b/789012"},
		{"b/123456, b/789012", "b/123456, b/789012"},
		{"https://issues.pigweed.dev/issues/123, https://issues.pigweed.dev/issues/456", "b/123, b/456"},
		{"b/123 b:456; 789", "b/123, b/456, b/789"},
		// Deduplication
		{"b/123, 123, b:123", "b/123"},
		// Preserving non-standard trackers
		{"https://github.com/google/pigweed/issues/42", "https://github.com/google/pigweed/issues/42"},
	}

	for _, tc := range tests {
		got := NormalizeBugChain(tc.input)
		if got != tc.want {
			t.Errorf("NormalizeBugChain(%q) = %q, want %q", tc.input, got, tc.want)
		}
	}
}

// TestNormalizeTrailer pins the canonical spelling gh-ish writes when it
// authors a bug trailer itself.
//
// The canonical fix key is `Fixed:`, not `Fixes:`. Gerrit accepts fix, fixes,
// fixing and fixed interchangeably and auto-closes on all four, so this is a
// house-style choice rather than a functional one -- and Pigweed's house style
// is clear: in the last 400 commits, `Fixed:` outnumbers `Fixes:` roughly four
// to one, and the contributor docs say to use `Bug:` or `Fixed:`.
func TestNormalizeTrailer(t *testing.T) {
	tests := []struct {
		input string
		want  string
	}{
		{"Bug: https://issues.pigweed.dev/issues/123456", "Bug: b/123456"},
		{"bug: 123456", "Bug: b/123456"},
		{"Bugs: b:123456, b:789012", "Bug: b/123456, b/789012"},
		{"Issue: 123456", "Bug: b/123456"},
		{"Fix: https://issues.pigweed.dev/issues/999", "Fixed: b/999"},
		{"Fixes: 999", "Fixed: b/999"},
		{"Fixed: b/999", "Fixed: b/999"},
		{"Bug: none", "Bug: None"},
		{"Change-Id: I1234567890abcdef1234567890abcdef12345678", "Change-Id: I1234567890abcdef1234567890abcdef12345678"},
		{"Test: bazelisk test //...", "Test: bazelisk test //..."},
	}

	for _, tc := range tests {
		got := NormalizeTrailer(tc.input)
		if got != tc.want {
			t.Errorf("NormalizeTrailer(%q) = %q, want %q", tc.input, got, tc.want)
		}
	}
}

func TestParseIssueID(t *testing.T) {
	tests := []struct {
		input   string
		wantID  int64
		wantErr bool
	}{
		{"123456", 123456, false},
		{"42", 42, false},
		{"b/123456", 123456, false},
		{"b:123456", 123456, false},
		{"pwbug/123456", 123456, false},
		{"pwbug.dev/123456", 123456, false},
		{"fxb/98765", 98765, false},
		{"fxbug.dev/98765", 98765, false},
		{"https://issues.pigweed.dev/issues/123456", 123456, false},
		{"https://g-issues.pigweed.dev/issues/503634", 503634, false},
		{"https://issues.fuchsia.dev/issues/98765", 98765, false},
		{"https://issuetracker.google.com/issues/55555", 55555, false},
		{"https://bugs.chromium.org/p/pigweed/issues/detail?id=987654", 987654, false},
		{"https://bugs.chromium.org/p/pigweed/issues/detail?id=987654&q=foo", 987654, false},
		{"https://bugs.chromium.org/p/pigweed/issues/detail?id=987654#c1", 987654, false},
		{"", 0, true},
		{"none", 0, true},
		{"fxbug/98765", 0, true},
		{"0", 0, true},
		{"-123", 0, true},
		{"https://github.com/google/pigweed/issues/42", 0, true},
	}

	for _, tc := range tests {
		got, err := ParseIssueID(tc.input)
		if tc.wantErr {
			if err == nil {
				t.Errorf("ParseIssueID(%q) expected error, got %d", tc.input, got)
			}
			continue
		}
		if err != nil {
			t.Errorf("ParseIssueID(%q) unexpected error: %v", tc.input, err)
			continue
		}
		if got != tc.wantID {
			t.Errorf("ParseIssueID(%q) = %d, want %d", tc.input, got, tc.wantID)
		}
	}
}
