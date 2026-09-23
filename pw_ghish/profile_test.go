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
	"bytes"
	"context"
	"errors"
	"io"
	"os/exec"
	"strings"
	"sync"
	"testing"
)

func TestDetectProfile_Pigweed(t *testing.T) {
	tests := []struct {
		desc      string
		remoteURL string
		host      string
	}{
		{
			desc:      "googlesource HTTPS remote",
			remoteURL: "https://pigweed.googlesource.com/pigweed/pigweed",
		},
		{
			desc:      "sso remote",
			remoteURL: "sso://pigweed/pigweed",
		},
		{
			desc: "explicit host flag",
			host: "pigweed-review.googlesource.com",
		},
	}

	for _, tt := range tests {
		t.Run(tt.desc, func(t *testing.T) {
			p, err := DetectProfile(tt.remoteURL, tt.host, "")
			if err != nil {
				t.Fatalf("unexpected error: %v", err)
			}
			if p.Name() != "pigweed" {
				t.Errorf("DetectProfile(%q, %q) = %q, want %q", tt.remoteURL, tt.host, p.Name(), "pigweed")
			}
		})
	}
}

func TestDetectProfile_Fuchsia(t *testing.T) {
	tests := []struct {
		desc      string
		remoteURL string
		host      string
	}{
		{
			desc:      "fuchsia remote",
			remoteURL: "https://fuchsia.googlesource.com/fuchsia",
		},
		{
			desc:      "turquoise-internal remote",
			remoteURL: "sso://turquoise-internal/fuchsia",
		},
		{
			desc: "fuchsia-review host",
			host: "fuchsia-review.googlesource.com",
		},
		{
			desc: "turquoise-internal host",
			host: "turquoise-internal-review.googlesource.com",
		},
	}

	for _, tt := range tests {
		t.Run(tt.desc, func(t *testing.T) {
			p, err := DetectProfile(tt.remoteURL, tt.host, "")
			if err != nil {
				t.Fatalf("unexpected error: %v", err)
			}
			if p.Name() != "fuchsia" {
				t.Errorf("DetectProfile(%q, %q) = %q, want %q", tt.remoteURL, tt.host, p.Name(), "fuchsia")
			}
		})
	}
}

func TestDetectProfile_Generic(t *testing.T) {
	tests := []struct {
		desc      string
		remoteURL string
		host      string
	}{
		{
			desc:      "arbitrary domain",
			remoteURL: "https://github.com/some/repo.git",
		},
		{
			desc: "empty inputs",
		},
	}

	for _, tt := range tests {
		t.Run(tt.desc, func(t *testing.T) {
			p, err := DetectProfile(tt.remoteURL, tt.host, "")
			if err != nil {
				t.Fatalf("unexpected error: %v", err)
			}
			if p.Name() != "generic" {
				t.Errorf("DetectProfile(%q, %q) = %q, want %q", tt.remoteURL, tt.host, p.Name(), "generic")
			}
		})
	}
}

func TestDetectProfile_ExplicitOverride(t *testing.T) {
	// Remote says pigweed, but explicit flag requests fuchsia
	p, err := DetectProfile("https://pigweed.googlesource.com/pigweed/pigweed", "", "fuchsia")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if p.Name() != "fuchsia" {
		t.Errorf("DetectProfile with explicit override got %q, want %q", p.Name(), "fuchsia")
	}

	// Remote says fuchsia, but explicit flag requests pigweed
	p2, err := DetectProfile("https://fuchsia.googlesource.com/fuchsia", "", "pigweed")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if p2.Name() != "pigweed" {
		t.Errorf("DetectProfile with explicit override got %q, want %q", p2.Name(), "pigweed")
	}

	// Unknown explicit profile returns error instead of silently falling back
	_, err = DetectProfile("https://pigweed.googlesource.com/pigweed/pigweed", "", "unknown_profile")
	if err == nil {
		t.Error("expected error for unknown explicit profile, got nil")
	}
	if !strings.Contains(err.Error(), "unknown profile") {
		t.Errorf("expected error about unknown profile, got: %v", err)
	}
}

func TestPigweedProfile_Properties(t *testing.T) {
	p, ok := GetProfile("pigweed")
	if !ok {
		t.Fatal("Pigweed profile not registered")
	}

	cqLabel, hasCQ := p.CQLabel()
	if !hasCQ {
		t.Error("Pigweed profile expected to support CQLabel")
	}
	if cqLabel.Name != "Commit-Queue" || cqLabel.Value != 2 {
		t.Errorf("CQLabel() = %+v, want Commit-Queue=2", cqLabel)
	}

	crLabel := p.ReviewLabel()
	if crLabel.Name != "Code-Review" || crLabel.Value != 2 {
		t.Errorf("ReviewLabel() = %+v, want Code-Review=2", crLabel)
	}

	if p.BuildbucketProject() != "pigweed" {
		t.Errorf("BuildbucketProject() = %q, want \"pigweed\"", p.BuildbucketProject())
	}

	if p.DefaultComponentID() != 1194524 {
		t.Errorf("DefaultComponentID() = %d, want 1194524", p.DefaultComponentID())
	}
	if p.IssueTrackerAPIEndpoint() != "https://issuetracker.googleapis.com/v1" {
		t.Errorf("IssueTrackerAPIEndpoint() = %q, want https://issuetracker.googleapis.com/v1", p.IssueTrackerAPIEndpoint())
	}
	if gotURL := p.IssueWebURL(345678); gotURL != "https://issues.pigweed.dev/issues/345678" {
		t.Errorf("IssueWebURL(345678) = %q, want https://issues.pigweed.dev/issues/345678", gotURL)
	}

	// Test FormatPushRef with AutoSubmit
	ref := p.FormatPushRef("main", PushOptions{
		Reviewers:       []string{"reviewer@google.com"},
		AutoSubmit:      true,
		AutoSubmitLabel: LabelVote{Name: "Pigweed-Auto-Submit", Value: 1},
	})
	if !strings.HasPrefix(ref, "refs/for/main%") {
		t.Errorf("FormatPushRef prefix invalid: %q", ref)
	}
	if !strings.Contains(ref, "r=reviewer@google.com") {
		t.Errorf("FormatPushRef missing reviewer: %q", ref)
	}
	if !strings.Contains(ref, "l=Pigweed-Auto-Submit+1") {
		t.Errorf("FormatPushRef missing auto-submit label: %q", ref)
	}

	// Test rich PushOptions (CC, CQ, Publish, ExtraOptions)
	richRef := p.FormatPushRef("main", PushOptions{
		Reviewers:    []string{"alice@google.com"},
		CC:           []string{"bob@google.com"},
		Draft:        true,
		CQ:           1,
		Publish:      true,
		ExtraOptions: []string{"topic=test-topic"},
	})
	for _, expected := range []string{"r=alice@google.com", "cc=bob@google.com", "wip", "publish-comments", "l=Commit-Queue+1", "topic=test-topic"} {
		if !strings.Contains(richRef, expected) {
			t.Errorf("FormatPushRef missing expected component %q in %q", expected, richRef)
		}
	}
}

// The label is resolved from the host before the push, and no profile adds one
// of its own: voting a second, compiled-in name would fail on the name the host
// does not define.
func TestFormatPushRef_VotesOnlyTheResolvedAutoSubmitLabel(t *testing.T) {
	for _, tt := range []struct {
		profile string
		absent  string
	}{
		{profile: "pigweed", absent: "l=Pigweed-Auto-Submit+1"},
		{profile: "fuchsia", absent: "l=Commit-Queue+2"},
		{profile: "generic", absent: ""},
	} {
		t.Run(tt.profile, func(t *testing.T) {
			p, ok := GetProfile(tt.profile)
			if !ok {
				t.Fatalf("%s profile not registered", tt.profile)
			}
			ref := p.FormatPushRef("main", PushOptions{
				AutoSubmit:      true,
				AutoSubmitLabel: LabelVote{Name: "Auto-Submit", Value: 1},
			})
			if !strings.Contains(ref, "l=Auto-Submit+1") {
				t.Errorf("FormatPushRef() = %q, want it to vote the resolved label", ref)
			}
			if tt.absent != "" && strings.Contains(ref, tt.absent) {
				t.Errorf("FormatPushRef() = %q, want no %q once a label was resolved", ref, tt.absent)
			}
		})
	}
}

// Nothing resolved means the host named no auto-submit label. The commands
// refuse before they get here, so emitting a guess would only ever vote a
// label that does not exist.
func TestFormatPushRef_UnresolvedAutoSubmitVotesNothing(t *testing.T) {
	for _, name := range []string{"pigweed", "fuchsia", "generic"} {
		t.Run(name, func(t *testing.T) {
			p, ok := GetProfile(name)
			if !ok {
				t.Fatalf("%s profile not registered", name)
			}
			ref := p.FormatPushRef("main", PushOptions{AutoSubmit: true})
			if strings.Contains(ref, "l=") {
				t.Errorf("FormatPushRef() = %q, want no label vote", ref)
			}
		})
	}
}

// --cq names a score outright. The dry run --auto settles for on a host with
// no auto-submit label must not quietly vote the same label a second time.
func TestFormatPushRef_ExplicitCQOutranksTheAutoSubmitDryRun(t *testing.T) {
	p, ok := GetProfile("generic")
	if !ok {
		t.Fatal("generic profile not registered")
	}
	ref := p.FormatPushRef("main", PushOptions{
		CQ:              2,
		AutoSubmit:      true,
		AutoSubmitLabel: LabelVote{Name: "Commit-Queue", Value: 1},
	})
	if !strings.Contains(ref, "l=Commit-Queue+2") {
		t.Errorf("FormatPushRef() = %q, want the explicit --cq vote", ref)
	}
	if strings.Contains(ref, "l=Commit-Queue+1") {
		t.Errorf("FormatPushRef() = %q, want no duplicate Commit-Queue vote", ref)
	}
}

func TestFuchsiaProfile_Properties(t *testing.T) {
	p, ok := GetProfile("fuchsia")
	if !ok {
		t.Fatal("Fuchsia profile not registered")
	}

	cqLabel, hasCQ := p.CQLabel()
	if !hasCQ {
		t.Error("Fuchsia profile expected to support CQLabel")
	}
	if cqLabel.Name != "Commit-Queue" || cqLabel.Value != 2 {
		t.Errorf("CQLabel() = %+v, want Commit-Queue=2", cqLabel)
	}

	if p.BuildbucketProject() != "fuchsia" {
		t.Errorf("BuildbucketProject() = %q, want \"fuchsia\"", p.BuildbucketProject())
	}

	if p.DefaultComponentID() != 1363195 {
		t.Errorf("DefaultComponentID() = %d, want 1363195", p.DefaultComponentID())
	}
	if gotURL := p.IssueWebURL(98765); gotURL != "https://issues.fuchsia.dev/issues/98765" {
		t.Errorf("IssueWebURL(98765) = %q, want https://issues.fuchsia.dev/issues/98765", gotURL)
	}

	// Fuchsia no longer treats Commit-Queue+2 as an auto-submit label of its
	// own; whatever --auto settles on is resolved from the host and passed in.
	ref := p.FormatPushRef("main", PushOptions{
		AutoSubmit:      true,
		AutoSubmitLabel: LabelVote{Name: "Commit-Queue", Value: 1},
	})
	if ref != "refs/for/main%l=Commit-Queue+1" {
		t.Errorf("FormatPushRef() = %q, want \"refs/for/main%%l=Commit-Queue+1\"", ref)
	}

	// Explicit CQ=1 is the only Commit-Queue vote when nothing was resolved.
	refCQ1 := p.FormatPushRef("main", PushOptions{
		AutoSubmit: true,
		CQ:         1,
	})
	if refCQ1 != "refs/for/main%l=Commit-Queue+1" {
		t.Errorf("FormatPushRef() = %q, want \"refs/for/main%%l=Commit-Queue+1\"", refCQ1)
	}
}

func TestGenericProfile_Properties(t *testing.T) {
	p, ok := GetProfile("generic")
	if !ok {
		t.Fatal("Generic profile not registered")
	}

	_, hasCQ := p.CQLabel()
	if hasCQ {
		t.Error("Generic profile should not have CQLabel")
	}

	if p.DefaultComponentID() != 0 {
		t.Errorf("DefaultComponentID() = %d, want 0", p.DefaultComponentID())
	}
	if gotURL := p.IssueWebURL(55555); gotURL != "https://issuetracker.google.com/issues/55555" {
		t.Errorf("IssueWebURL(55555) = %q, want https://issuetracker.google.com/issues/55555", gotURL)
	}

	ref := p.FormatPushRef("main", PushOptions{
		Wip: true,
	})
	if ref != "refs/for/main%wip" {
		t.Errorf("FormatPushRef() = %q, want \"refs/for/main%%wip\"", ref)
	}
}

type customProfile struct {
	genericProfile
}

func (c *customProfile) Name() string {
	return "custom-project"
}

func TestRegisterProfile(t *testing.T) {
	cp := &customProfile{}
	RegisterProfile(cp)

	retrieved, ok := GetProfile("custom-project")
	if !ok || retrieved.Name() != "custom-project" {
		t.Errorf("GetProfile(custom-project) failed: ok=%v", ok)
	}

	detected, err := DetectProfile("", "", "custom-project")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if detected.Name() != "custom-project" {
		t.Errorf("DetectProfile with custom-project got %q, want %q", detected.Name(), "custom-project")
	}
}

func TestPigweedProfile_FormatRerunCommand(t *testing.T) {
	p := &pigweedProfile{}
	ch := GerritChangeRef{
		Host:     "pigweed-review.googlesource.com",
		Project:  "pigweed/pigweed",
		ChangeID: 472267,
		Patchset: 3,
	}
	cmd := p.FormatRerunCommand(ch, "pigweed-mac-arm-vscode")
	expected := "bb add -cl https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/472267/3 pigweed/pigweed.try/pigweed-mac-arm-vscode"
	if cmd != expected {
		t.Errorf("FormatRerunCommand() = %q, want %q", cmd, expected)
	}
}

func TestFuchsiaProfile_FormatRerunCommand(t *testing.T) {
	p := &fuchsiaProfile{}
	ch := GerritChangeRef{
		Host:     "fuchsia-review.googlesource.com",
		Project:  "fuchsia",
		ChangeID: 12345,
		Patchset: 1,
	}
	cmd := p.FormatRerunCommand(ch, "core.x64-release")
	expected := "bb add -cl https://fuchsia-review.googlesource.com/c/fuchsia/+/12345/1 fuchsia/try/core.x64-release"
	if cmd != expected {
		t.Errorf("FormatRerunCommand() = %q, want %q", cmd, expected)
	}
}

func TestGenericProfile_FormatRerunCommand(t *testing.T) {
	p := &genericProfile{}
	ch := GerritChangeRef{
		Host:     "custom-review.example.com",
		Project:  "my/custom/repo",
		ChangeID: 999,
		Patchset: 2,
	}
	cmd := p.FormatRerunCommand(ch, "test-builder")
	expected := "bb add -cl https://custom-review.example.com/c/my/custom/repo/+/999/2 my/custom/repo/try/test-builder"
	if cmd != expected {
		t.Errorf("FormatRerunCommand() = %q, want %q", cmd, expected)
	}
}

func TestDefaultFormatRerunCommand_Fallback(t *testing.T) {
	ch := GerritChangeRef{
		Host:     "example.com",
		Project:  "platform/infra",
		ChangeID: 100,
		Patchset: 5,
	}

	// Explicit project
	explicit := DefaultFormatRerunCommand("custom-proj", "custom-bucket", ch, "my-builder")
	wantExplicit := "bb add -cl https://example.com/c/platform/infra/+/100/5 custom-proj/custom-bucket/my-builder"
	if explicit != wantExplicit {
		t.Errorf("DefaultFormatRerunCommand(explicit) = %q, want %q", explicit, wantExplicit)
	}

	// Empty project fallback
	fallback := DefaultFormatRerunCommand("", "try", ch, "my-builder")
	wantFallback := "bb add -cl https://example.com/c/platform/infra/+/100/5 platform/infra/try/my-builder"
	if fallback != wantFallback {
		t.Errorf("DefaultFormatRerunCommand(fallback) = %q, want %q", fallback, wantFallback)
	}
}

func TestDefaultRerunCheck_Success(t *testing.T) {
	var calledBin string
	var calledArgs []string

	restore := SetRerunExecForTesting(
		func(file string) (string, error) {
			if file == "bb" {
				return "/opt/bin/bb", nil
			}
			return "", exec.ErrNotFound
		},
		func(ctx context.Context, name string, args []string, stdout, stderr io.Writer) error {
			calledBin = name
			calledArgs = args
			stdout.Write([]byte("Job scheduled successfully\n"))
			return nil
		},
	)
	defer restore()

	var stdout, stderr bytes.Buffer
	cmdStr := "bb add -cl https://example.com/c/repo/+/1/1 repo/try/builder"
	err := DefaultRerunCheck(context.Background(), cmdStr, "", &stdout, &stderr)
	if err != nil {
		t.Fatalf("DefaultRerunCheck returned unexpected error: %v", err)
	}

	if calledBin != "/opt/bin/bb" {
		t.Errorf("called binary = %q, want /opt/bin/bb", calledBin)
	}
	expectedArgs := []string{"add", "-cl", "https://example.com/c/repo/+/1/1", "repo/try/builder"}
	if len(calledArgs) != len(expectedArgs) {
		t.Fatalf("calledArgs len = %d, want %d", len(calledArgs), len(expectedArgs))
	}
	for i, arg := range expectedArgs {
		if calledArgs[i] != arg {
			t.Errorf("arg[%d] = %q, want %q", i, calledArgs[i], arg)
		}
	}
	if !strings.Contains(stdout.String(), "Job scheduled") {
		t.Errorf("stdout missing expected output, got %q", stdout.String())
	}
}

func TestDefaultRerunCheck_CommandFailure(t *testing.T) {
	restore := SetRerunExecForTesting(
		func(file string) (string, error) {
			return "/bin/bb", nil
		},
		func(ctx context.Context, name string, args []string, stdout, stderr io.Writer) error {
			stderr.Write([]byte("RPC error: permission denied\n"))
			return errors.New("exit status 1")
		},
	)
	defer restore()

	var stdout, stderr bytes.Buffer
	err := DefaultRerunCheck(context.Background(), "bb add -cl url builder", "", &stdout, &stderr)
	if err == nil {
		t.Fatal("expected error on command failure, got nil")
	}
	if !strings.Contains(err.Error(), "exit status 1") {
		t.Errorf("expected error to contain exit status 1, got: %v", err)
	}
	if !strings.Contains(stderr.String(), "permission denied") {
		t.Errorf("stderr missing expected error, got: %q", stderr.String())
	}
}

func TestDefaultRerunCheck_NotFoundOnPath(t *testing.T) {
	restore := SetRerunExecForTesting(
		func(file string) (string, error) {
			return "", exec.ErrNotFound
		},
		nil,
	)
	defer restore()

	var stdout, stderr bytes.Buffer
	cmdStr := "bb add -cl https://example.com/123/1 builder"

	// Without hint
	errNoHint := DefaultRerunCheck(context.Background(), cmdStr, "", &stdout, &stderr)
	if errNoHint == nil {
		t.Fatal("expected error when bb not found, got nil")
	}
	if !strings.Contains(errNoHint.Error(), "'bb' CLI not found on PATH") {
		t.Errorf("error missing 'bb' CLI not found on PATH: %v", errNoHint)
	}
	if !strings.Contains(errNoHint.Error(), cmdStr) {
		t.Errorf("error missing manual command %q: %v", cmdStr, errNoHint)
	}
	if strings.Contains(errNoHint.Error(), "activate.sh") {
		t.Errorf("error should not contain hint when none provided: %v", errNoHint)
	}

	// With hint
	hint := "(Hint: run source env.sh)"
	errWithHint := DefaultRerunCheck(context.Background(), cmdStr, hint, &stdout, &stderr)
	if errWithHint == nil {
		t.Fatal("expected error when bb not found, got nil")
	}
	if !strings.Contains(errWithHint.Error(), hint) {
		t.Errorf("error missing hint %q: %v", hint, errWithHint)
	}
}

func TestDefaultRerunCheck_InvalidCommand(t *testing.T) {
	var stdout, stderr bytes.Buffer

	// Empty string
	errEmpty := DefaultRerunCheck(context.Background(), "", "", &stdout, &stderr)
	if errEmpty == nil {
		t.Error("expected error for empty command, got nil")
	}
	if !strings.Contains(errEmpty.Error(), "invalid rerun command") {
		t.Errorf("expected 'invalid rerun command' error, got: %v", errEmpty)
	}

	// Single token
	errSingle := DefaultRerunCheck(context.Background(), "bb", "", &stdout, &stderr)
	if errSingle == nil {
		t.Error("expected error for single-token command, got nil")
	}
	if !strings.Contains(errSingle.Error(), "invalid rerun command") {
		t.Errorf("expected 'invalid rerun command' error, got: %v", errSingle)
	}
}

func TestProfileRerunCheck_Integration(t *testing.T) {
	restore := SetRerunExecForTesting(
		func(file string) (string, error) {
			return "", exec.ErrNotFound
		},
		nil,
	)
	defer restore()

	ch := GerritChangeRef{
		Host:     "pigweed-review.googlesource.com",
		Project:  "pigweed/pigweed",
		ChangeID: 472267,
		Patchset: 1,
	}

	// Pigweed profile includes activation hint
	pwProf, _ := GetProfile("pigweed")
	var stdout, stderr bytes.Buffer
	errPw := pwProf.RerunCheck(context.Context(context.Background()), ch, "my-builder", &stdout, &stderr)
	if errPw == nil {
		t.Fatal("expected error for Pigweed RerunCheck without bb")
	}
	if !strings.Contains(errPw.Error(), "activate.sh") {
		t.Errorf("Pigweed error missing activate.sh hint: %v", errPw)
	}

	// Fuchsia profile does NOT include Pigweed activation hint
	fuchsiaProf, _ := GetProfile("fuchsia")
	errFuchsia := fuchsiaProf.RerunCheck(context.Context(context.Background()), ch, "my-builder", &stdout, &stderr)
	if errFuchsia == nil {
		t.Fatal("expected error for Fuchsia RerunCheck without bb")
	}
	if strings.Contains(errFuchsia.Error(), "activate.sh") {
		t.Errorf("Fuchsia error should not contain activate.sh: %v", errFuchsia)
	}

	// Generic profile does NOT include Pigweed activation hint
	genericProf, _ := GetProfile("generic")
	errGeneric := genericProf.RerunCheck(context.Context(context.Background()), ch, "my-builder", &stdout, &stderr)
	if errGeneric == nil {
		t.Fatal("expected error for Generic RerunCheck without bb")
	}
	if strings.Contains(errGeneric.Error(), "activate.sh") {
		t.Errorf("Generic error should not contain activate.sh: %v", errGeneric)
	}
}

func TestDetectProfile_Concurrent(t *testing.T) {
	var wg sync.WaitGroup
	for i := 0; i < 50; i++ {
		wg.Add(3)
		go func() {
			defer wg.Done()
			p, err := DetectProfile("https://pigweed.googlesource.com/pigweed/pigweed", "", "")
			if err != nil || p == nil || p.Name() != "pigweed" {
				t.Errorf("DetectProfile pigweed concurrent failed: %v, p=%v", err, p)
			}
		}()
		go func() {
			defer wg.Done()
			p, err := DetectProfile("", "fuchsia-review.googlesource.com", "")
			if err != nil || p == nil || p.Name() != "fuchsia" {
				t.Errorf("DetectProfile fuchsia concurrent failed: %v, p=%v", err, p)
			}
		}()
		go func() {
			defer wg.Done()
			p, err := DetectProfile("https://other.googlesource.com/foo", "", "")
			if err != nil || p == nil || p.Name() != "generic" {
				t.Errorf("DetectProfile generic concurrent failed: %v, p=%v", err, p)
			}
		}()
	}
	wg.Wait()
}
