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
	"context"
	"net/http"
	"strings"
	"testing"

	"github.com/andygrunwald/go-gerrit"
)

func TestFindAutoSubmitLabel_MatchesHostSpellings(t *testing.T) {
	for _, tt := range []struct {
		name   string
		labels map[string]gerrit.LabelInfo
		want   LabelVote
	}{
		{
			name: "pigweed",
			labels: map[string]gerrit.LabelInfo{
				"Code-Review":         {},
				"Commit-Queue":        {},
				"Pigweed-Auto-Submit": {},
			},
			want: LabelVote{Name: "Pigweed-Auto-Submit", Value: 1},
		},
		{
			name: "fuchsia",
			labels: map[string]gerrit.LabelInfo{
				"Fuchsia-Auto-Submit": {},
			},
			want: LabelVote{Name: "Fuchsia-Auto-Submit", Value: 1},
		},
		{
			name: "chromium",
			labels: map[string]gerrit.LabelInfo{
				"Auto-Submit": {},
			},
			want: LabelVote{Name: "Auto-Submit", Value: 1},
		},
		{
			name: "unhyphenated",
			labels: map[string]gerrit.LabelInfo{
				"Autosubmit": {},
			},
			want: LabelVote{Name: "Autosubmit", Value: 1},
		},
		{
			name: "underscored and lowercased",
			labels: map[string]gerrit.LabelInfo{
				"auto_submit": {},
			},
			want: LabelVote{Name: "auto_submit", Value: 1},
		},
		{
			name: "votes the label maximum, not a fixed +1",
			labels: map[string]gerrit.LabelInfo{
				"Auto-Submit": {Values: map[string]string{" 0": "No", "+1": "Yes", "+2": "Yes, really"}},
			},
			want: LabelVote{Name: "Auto-Submit", Value: 2},
		},
	} {
		t.Run(tt.name, func(t *testing.T) {
			got, ok, err := FindAutoSubmitLabel(tt.labels)
			if err != nil {
				t.Fatalf("FindAutoSubmitLabel(%v) returned error: %v", tt.labels, err)
			}
			if !ok {
				t.Fatalf("FindAutoSubmitLabel(%v) reported no match, want %+v", tt.labels, tt.want)
			}
			if got != tt.want {
				t.Errorf("FindAutoSubmitLabel() = %+v, want %+v", got, tt.want)
			}
		})
	}
}

func TestFindAutoSubmitLabel_NoMatch(t *testing.T) {
	labels := map[string]gerrit.LabelInfo{
		"Code-Review":  {},
		"Commit-Queue": {},
	}
	got, ok, err := FindAutoSubmitLabel(labels)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if ok {
		t.Errorf("FindAutoSubmitLabel() = %+v, true; want no match", got)
	}
	if got, ok, err := FindAutoSubmitLabel(nil); ok || err != nil {
		t.Errorf("FindAutoSubmitLabel(nil) = %+v, %v, %v; want no match and no error", got, ok, err)
	}
}

// Only the separators label names actually use may join the two words. A
// looser pattern would vote an unrelated label as though it submitted the
// change.
func TestFindAutoSubmitLabel_RejectsNonSeparators(t *testing.T) {
	for _, name := range []string{
		"autoXsubmit",
		"Auto+Submit",
		"Auto.Submit",
		"Auto/Submit",
	} {
		t.Run(name, func(t *testing.T) {
			labels := map[string]gerrit.LabelInfo{name: {}}
			got, ok, err := FindAutoSubmitLabel(labels)
			if err != nil {
				t.Fatalf("FindAutoSubmitLabel(%q) returned error: %v", name, err)
			}
			if ok {
				t.Errorf("FindAutoSubmitLabel(%q) = %+v, true; want no match", name, got)
			}
		})
	}
}

// Two labels that both look like auto-submit are not interchangeable. Picking
// one would vote a label that may not gate submission at all, and the change
// would sit there looking handed off, so this has to fail loudly.
func TestFindAutoSubmitLabel_AmbiguousIsAnError(t *testing.T) {
	labels := map[string]gerrit.LabelInfo{
		"Pigweed-Auto-Submit": {},
		"Auto-Submit":         {},
		"Code-Review":         {},
	}
	got, ok, err := FindAutoSubmitLabel(labels)
	if err == nil {
		t.Fatalf("FindAutoSubmitLabel() = %+v, %v, nil; want an error for ambiguous labels", got, ok)
	}
	if ok || got != (LabelVote{}) {
		t.Errorf("FindAutoSubmitLabel() = %+v, %v; want no label alongside the error", got, ok)
	}
	// The error has to name the candidates, or the user cannot act on it.
	for _, want := range []string{"Auto-Submit", "Pigweed-Auto-Submit", "--add-label"} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("error %q does not mention %q", err, want)
		}
	}
	if strings.Contains(err.Error(), "Code-Review") {
		t.Errorf("error %q should only list auto-submit candidates", err)
	}
}

func TestDecideAutoSubmit_UsesTheHostsLabel(t *testing.T) {
	labels := map[string]gerrit.LabelInfo{
		"Code-Review": {},
		"Auto-Submit": {},
	}
	got, err := DecideAutoSubmit(labels, "change 1")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got.Unsupported != nil {
		t.Fatalf("unexpected Unsupported: %v", got.Unsupported)
	}
	if got.Vote.Name != "Auto-Submit" || got.Vote.Value != 1 {
		t.Errorf("DecideAutoSubmit() voted %+v, want Auto-Submit=1", got.Vote)
	}
}

// --auto asks for the change to be submitted. A host with no auto-submit label
// is not going to do that, so saying nothing and voting something adjacent
// would leave the user believing their change was handed off.
func TestDecideAutoSubmit_NoLabelReportsCommitQueueDryRunAndFails(t *testing.T) {
	labels := map[string]gerrit.LabelInfo{
		"Code-Review":  {},
		"Commit-Queue": {Values: map[string]string{" 0": "no", "+1": "dry run", "+2": "submit"}},
	}
	got, err := DecideAutoSubmit(labels, "change 1")
	if err != nil {
		t.Fatalf("unexpected hard error: %v", err)
	}
	if got.Unsupported == nil {
		t.Fatal("DecideAutoSubmit() reported success for a host with no auto-submit label")
	}
	// +1 is the dry run. +2 would submit the change, which is more than the
	// user asked pw_ghish to arrange and more than it could confirm.
	if got.Vote.Name != "Commit-Queue" || got.Vote.Value != 1 {
		t.Errorf("DecideAutoSubmit() voted %+v, want Commit-Queue=1", got.Vote)
	}
	for _, want := range []string{"no auto-submit label", "Commit-Queue+1", "--cq", "Code-Review, Commit-Queue"} {
		if !strings.Contains(got.Unsupported.Error(), want) {
			t.Errorf("error %q does not mention %q", got.Unsupported, want)
		}
	}
}

// With nothing to vote there is no reason to let the caller act first, so this
// is a plain error rather than a decision carrying one.
func TestDecideAutoSubmit_NoLabelAndNoCommitQueueIsAHardError(t *testing.T) {
	labels := map[string]gerrit.LabelInfo{
		"Code-Review": {},
		"Verified":    {},
	}
	got, err := DecideAutoSubmit(labels, "change 1")
	if err == nil {
		t.Fatalf("DecideAutoSubmit() = %+v, nil; want an error", got)
	}
	if got.Vote != (LabelVote{}) {
		t.Errorf("DecideAutoSubmit() voted %+v, want nothing", got.Vote)
	}
	for _, want := range []string{"no auto-submit label", "Code-Review, Verified"} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("error %q does not mention %q", err, want)
		}
	}
}

// A Commit Queue that cannot be voted +1 is no use as a dry run, so this is
// the same as having none.
func TestDecideAutoSubmit_CommitQueueWithoutDryRunIsNotOffered(t *testing.T) {
	labels := map[string]gerrit.LabelInfo{
		"Commit-Queue": {Values: map[string]string{" 0": "no"}},
	}
	got, err := DecideAutoSubmit(labels, "change 1")
	if err == nil {
		t.Fatalf("DecideAutoSubmit() = %+v, nil; want an error", got)
	}
	if got.Vote != (LabelVote{}) {
		t.Errorf("DecideAutoSubmit() voted %+v, want nothing", got.Vote)
	}
}

// Labels are how the host says what it supports. Without them there is nothing
// to reason from, and the profile is not consulted: guessing is what this whole
// path exists to stop.
func TestDecideAutoSubmit_NoLabelsAtAllIsAnError(t *testing.T) {
	if got, err := DecideAutoSubmit(nil, "change 1"); err == nil {
		t.Errorf("DecideAutoSubmit(nil) = %+v, nil; want an error", got)
	}
}

func TestDecideAutoSubmit_AmbiguousIsAHardError(t *testing.T) {
	labels := map[string]gerrit.LabelInfo{
		"Auto-Submit":         {},
		"Pigweed-Auto-Submit": {},
		"Commit-Queue":        {},
	}
	got, err := DecideAutoSubmit(labels, "change 1")
	if err == nil {
		t.Fatalf("DecideAutoSubmit() = %+v, nil; want an error", got)
	}
	// Ambiguity is not a host that lacks the feature, so it must not quietly
	// turn into a Commit-Queue dry run.
	if got.Vote != (LabelVote{}) {
		t.Errorf("DecideAutoSubmit() voted %+v, want nothing", got.Vote)
	}
}

func TestFormatLabelNames(t *testing.T) {
	labels := map[string]gerrit.LabelInfo{
		"Commit-Queue": {},
		"Code-Review":  {},
	}
	if got, want := FormatLabelNames(labels), "Code-Review, Commit-Queue"; got != want {
		t.Errorf("FormatLabelNames() = %q, want %q", got, want)
	}
	if got, want := FormatLabelNames(nil), "none reported"; got != want {
		t.Errorf("FormatLabelNames(nil) = %q, want %q", got, want)
	}
}

func TestGerritProjectFromRemote(t *testing.T) {
	for _, tt := range []struct {
		remote string
		want   string
	}{
		{remote: "https://pigweed.googlesource.com/pigweed/pigweed", want: "pigweed/pigweed"},
		{remote: "https://pigweed.googlesource.com/pigweed/pigweed.git", want: "pigweed/pigweed"},
		{remote: "https://pigweed-review.googlesource.com/a/pigweed/pigweed", want: "pigweed/pigweed"},
		{remote: "sso://chrome-internal/infradata/config", want: "infradata/config"},
		{remote: "sso://pigweed/pigweed", want: "pigweed"},
		{remote: "git@host.example.com:team/repo.git", want: "team/repo"},
		{remote: "https://host.example.com/", want: ""},
		{remote: "https://host.example.com/a", want: ""},
		{remote: "https://host.example.com/a/", want: ""},
		{remote: "", want: ""},
	} {
		if got := GerritProjectFromRemote(tt.remote); got != tt.want {
			t.Errorf("GerritProjectFromRemote(%q) = %q, want %q", tt.remote, got, tt.want)
		}
	}
}

func TestListProjectLabels(t *testing.T) {
	server := NewMockGerritServer(t)
	// The mock server routes on the *decoded* path, so match a glob and assert
	// the escaping separately below.
	server.OnJSON("GET", "/projects/*", http.StatusOK, []map[string]any{
		{"name": "Code-Review", "values": map[string]string{"-2": "no", "+2": "yes"}},
		{"name": "Auto-Submit", "values": map[string]string{" 0": "no", "+1": "yes"}},
	})

	defs, err := ListProjectLabels(context.Background(), server.Client(), "pigweed/pigweed")
	if err != nil {
		t.Fatalf("ListProjectLabels() failed: %v", err)
	}
	if len(defs) != 2 {
		t.Fatalf("ListProjectLabels() returned %d labels, want 2", len(defs))
	}

	// The project name contains a slash; a real Gerrit requires it escaped,
	// otherwise it sees "projects/pigweed/pigweed/labels/" and 404s.
	req := server.LastRequest()
	if req == nil {
		t.Fatal("no request was recorded")
	}
	if got, want := req.URL.EscapedPath(), "/projects/pigweed%2Fpigweed/labels/"; got != want {
		t.Errorf("requested path = %q, want %q", got, want)
	}
}

func TestDecideAutoSubmitForNewChange(t *testing.T) {
	ctx := context.Background()

	t.Run("uses the project's label", func(t *testing.T) {
		server := NewMockGerritServer(t)
		server.OnJSON("GET", "/projects/*", http.StatusOK, []map[string]any{
			{"name": "Code-Review"},
			{"name": "Auto-Submit"},
		})

		got, err := DecideAutoSubmitForNewChange(ctx, server.Client(), "some/project")
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if got.Unsupported != nil {
			t.Fatalf("unexpected Unsupported: %v", got.Unsupported)
		}
		if got.Vote.Name != "Auto-Submit" || got.Vote.Value != 1 {
			t.Errorf("got %+v, want Auto-Submit=1", got.Vote)
		}
	})

	t.Run("refuses to choose between two candidates", func(t *testing.T) {
		server := NewMockGerritServer(t)
		server.OnJSON("GET", "/projects/*", http.StatusOK, []map[string]any{
			{"name": "Auto-Submit"},
			{"name": "Owners-Auto-Submit"},
		})

		_, err := DecideAutoSubmitForNewChange(ctx, server.Client(), "some/project")
		if err == nil {
			t.Fatal("expected an error for two auto-submit labels, got nil")
		}
		for _, want := range []string{"Auto-Submit", "Owners-Auto-Submit", "some/project"} {
			if !strings.Contains(err.Error(), want) {
				t.Errorf("error %q does not mention %q", err, want)
			}
		}
	})

	// A label that cannot be read cannot be confirmed, and pw_ghish no longer
	// has a compiled-in guess to reach for.
	t.Run("errors when the lookup fails", func(t *testing.T) {
		server := NewMockGerritServer(t)
		server.OnString("GET", "/projects/*", http.StatusForbidden, "text/plain", "forbidden")

		_, err := DecideAutoSubmitForNewChange(ctx, server.Client(), "some/project")
		if err == nil {
			t.Fatal("expected an error when the labels cannot be read, got nil")
		}
		if !strings.Contains(err.Error(), "-o l=<Label>+1") {
			t.Errorf("expected the error to offer a manual remedy, got: %v", err)
		}
	})

	t.Run("errors when the project defines no auto-submit label", func(t *testing.T) {
		server := NewMockGerritServer(t)
		server.OnJSON("GET", "/projects/*", http.StatusOK, []map[string]any{
			{"name": "Code-Review"},
			{"name": "Verified"},
		})

		_, err := DecideAutoSubmitForNewChange(ctx, server.Client(), "some/project")
		if err == nil {
			t.Fatal("expected an error when the project has no auto-submit label, got nil")
		}
		if !strings.Contains(err.Error(), "Code-Review, Verified") {
			t.Errorf("expected the error to list the project's labels, got: %v", err)
		}
	})

	// A Commit Queue is not an auto-submit label, so the push still fails --
	// but the dry run is worth starting on the way out.
	t.Run("offers a Commit-Queue dry run and still fails", func(t *testing.T) {
		server := NewMockGerritServer(t)
		server.OnJSON("GET", "/projects/*", http.StatusOK, []map[string]any{
			{"name": "Code-Review"},
			{"name": "Commit-Queue", "values": map[string]string{" 0": "no", "+1": "dry run", "+2": "submit"}},
		})

		got, err := DecideAutoSubmitForNewChange(ctx, server.Client(), "some/project")
		if err != nil {
			t.Fatalf("unexpected hard error: %v", err)
		}
		if got.Unsupported == nil {
			t.Fatal("expected the project's lack of an auto-submit label to be reported")
		}
		if got.Vote.Name != "Commit-Queue" || got.Vote.Value != 1 {
			t.Errorf("got %+v, want Commit-Queue=1", got.Vote)
		}
	})
}
