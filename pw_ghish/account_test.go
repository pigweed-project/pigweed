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
	"testing"

	"github.com/andygrunwald/go-gerrit"
)

func TestFormatAccount(t *testing.T) {
	tests := []struct {
		name     string
		acc      gerrit.AccountInfo
		expected string
	}{
		{
			name: "username takes highest priority",
			acc: gerrit.AccountInfo{
				AccountID: 100,
				Username:  "jdoe",
				Name:      "John Doe",
				Email:     "jdoe@example.com",
			},
			expected: "jdoe",
		},
		{
			name: "name takes priority when username is empty",
			acc: gerrit.AccountInfo{
				AccountID: 101,
				Name:      "Jane Smith",
				Email:     "jsmith@example.com",
			},
			expected: "Jane Smith",
		},
		{
			name: "email used when username and name are empty",
			acc: gerrit.AccountInfo{
				AccountID: 102,
				Email:     "bot@example.com",
			},
			expected: "bot@example.com",
		},
		{
			name: "account ID used when string fields are empty",
			acc: gerrit.AccountInfo{
				AccountID: 103,
			},
			expected: "account #103",
		},
		{
			name:     "empty account info returns empty string",
			acc:      gerrit.AccountInfo{},
			expected: "",
		},
		{
			name: "whitespace is trimmed",
			acc: gerrit.AccountInfo{
				Username: "  trimmed_user  ",
				Name:     "  Trimmed Name  ",
			},
			expected: "trimmed_user",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got := FormatAccount(tc.acc)
			if got != tc.expected {
				t.Errorf("FormatAccount() = %q, want %q", got, tc.expected)
			}
		})
	}
}

func TestBuildAttentionMap(t *testing.T) {
	t.Run("nil or empty map", func(t *testing.T) {
		m := BuildAttentionMap(nil)
		if m == nil {
			t.Fatal("expected non-nil map")
		}
		if len(m) != 0 {
			t.Errorf("expected empty map, got %v", m)
		}
		if m[123] {
			t.Error("expected false for arbitrary lookup")
		}
	})

	t.Run("populates valid account IDs and ignores 0", func(t *testing.T) {
		attnSet := map[string]gerrit.AttentionSetInfo{
			"10": {Account: gerrit.AccountInfo{AccountID: 10}},
			"20": {Account: gerrit.AccountInfo{AccountID: 20}},
			"0":  {Account: gerrit.AccountInfo{AccountID: 0}},
		}
		m := BuildAttentionMap(attnSet)
		if !m[10] {
			t.Errorf("expected account 10 in attention set")
		}
		if !m[20] {
			t.Errorf("expected account 20 in attention set")
		}
		if m[0] {
			t.Errorf("did not expect account 0 in attention set")
		}
		if m[30] {
			t.Errorf("did not expect account 30 in attention set")
		}
	})
}

func TestFormatReviewer(t *testing.T) {
	acc := gerrit.AccountInfo{Name: "Alice"}

	tests := []struct {
		name        string
		inAttention bool
		isCC        bool
		expected    string
	}{
		{
			name:        "regular reviewer not in attention",
			inAttention: false,
			isCC:        false,
			expected:    "Alice",
		},
		{
			name:        "regular reviewer in attention",
			inAttention: true,
			isCC:        false,
			expected:    "Alice (attention)",
		},
		{
			name:        "cc not in attention",
			inAttention: false,
			isCC:        true,
			expected:    "Alice (cc)",
		},
		{
			name:        "cc in attention",
			inAttention: true,
			isCC:        true,
			expected:    "Alice (cc, attention)",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got := FormatReviewer(acc, tc.inAttention, tc.isCC)
			if got != tc.expected {
				t.Errorf("FormatReviewer() = %q, want %q", got, tc.expected)
			}
		})
	}
}

func TestFormatChangeReviewersAndAssignees(t *testing.T) {
	t.Run("nil change returns nil", func(t *testing.T) {
		assignees, reviewers := FormatChangeReviewersAndAssignees(nil)
		if assignees != nil || reviewers != nil {
			t.Errorf("expected nil slices for nil change, got %v, %v", assignees, reviewers)
		}
	})

	t.Run("comprehensive change formatting", func(t *testing.T) {
		change := &gerrit.ChangeInfo{
			Owner: gerrit.AccountInfo{
				AccountID: 1,
				Name:      "Owner Alice",
			},
			Reviewers: map[string][]gerrit.AccountInfo{
				"REVIEWER": {
					{AccountID: 2, Name: "Bob"},
					{AccountID: 3, Name: "Charlie"},
				},
				"CC": {
					{AccountID: 4, Name: "Dave"},
					{AccountID: 5, Name: "Eve"},
				},
			},
			AttentionSet: map[string]gerrit.AttentionSetInfo{
				"1": {Account: gerrit.AccountInfo{AccountID: 1, Name: "Owner Alice"}},
				"2": {Account: gerrit.AccountInfo{AccountID: 2, Name: "Bob"}},
				"5": {Account: gerrit.AccountInfo{AccountID: 5, Name: "Eve"}},
				"9": {Account: gerrit.AccountInfo{AccountID: 9, Name: "Frank"}}, // remaining attention set
			},
		}

		assignees, reviewers := FormatChangeReviewersAndAssignees(change)

		wantAssignees := []string{
			"Owner Alice (attention)",
			"Dave (cc)",
			"Eve (cc, attention)",
			"Frank (attention)",
		}
		wantReviewers := []string{
			"Bob (attention)",
			"Charlie",
		}

		if !reflect.DeepEqual(assignees, wantAssignees) {
			t.Errorf("assignees mismatch\ngot:  %v\nwant: %v", assignees, wantAssignees)
		}
		if !reflect.DeepEqual(reviewers, wantReviewers) {
			t.Errorf("reviewers mismatch\ngot:  %v\nwant: %v", reviewers, wantReviewers)
		}
	})
}
