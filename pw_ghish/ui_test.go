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
	"os"
	"strings"
	"testing"
)

func clearUIEnv(t *testing.T) {
	t.Helper()
	t.Setenv("NO_COLOR", "")
	t.Setenv("TERM", "xterm-256color")
	t.Setenv("GHISH_COLOR", "")
	for _, k := range agentEnvVars {
		t.Setenv(k, "")
	}
}

func TestIsAgentEnv(t *testing.T) {
	clearUIEnv(t)
	if IsAgentEnv() {
		t.Fatal("expected IsAgentEnv() == false when all agent env vars are empty")
	}

	t.Setenv("AI_AGENT", "0")
	if IsAgentEnv() {
		t.Fatal("expected IsAgentEnv() == false when AI_AGENT=0")
	}

	for _, k := range agentEnvVars {
		clearUIEnv(t)
		t.Setenv(k, "1")
		if !IsAgentEnv() {
			t.Errorf("expected IsAgentEnv() == true when %s=1", k)
		}
	}
}

func TestIsTTY(t *testing.T) {
	if IsTTY(nil) {
		t.Error("expected IsTTY(nil) == false")
	}
	var buf bytes.Buffer
	if IsTTY(&buf) {
		t.Error("expected IsTTY(*bytes.Buffer) == false")
	}
	r, w, err := os.Pipe()
	if err == nil {
		defer r.Close()
		defer w.Close()
		if IsTTY(w) {
			t.Error("expected IsTTY(os.Pipe) == false")
		}
	}
}

func TestColorEnabled(t *testing.T) {
	var buf bytes.Buffer

	clearUIEnv(t)
	if ColorEnabled(&buf) {
		t.Error("expected ColorEnabled(&buf) == false for non-TTY buffer")
	}

	t.Setenv("GHISH_COLOR", "always")
	if !ColorEnabled(&buf) {
		t.Error("expected ColorEnabled(&buf) == true when GHISH_COLOR=always")
	}

	t.Setenv("NO_COLOR", "1")
	if ColorEnabled(&buf) {
		t.Error("expected ColorEnabled(&buf) == false when NO_COLOR=1")
	}

	clearUIEnv(t)
	t.Setenv("GHISH_COLOR", "always")
	t.Setenv("TERM", "dumb")
	if ColorEnabled(&buf) {
		t.Error("expected ColorEnabled(&buf) == false when TERM=dumb")
	}

	clearUIEnv(t)
	t.Setenv("GHISH_COLOR", "never")
	if ColorEnabled(&buf) {
		t.Error("expected ColorEnabled(&buf) == false when GHISH_COLOR=never")
	}

	clearUIEnv(t)
	t.Setenv("GHISH_COLOR", "always")
	t.Setenv("GEMINI_CLI", "1")
	if ColorEnabled(&buf) {
		t.Error("expected ColorEnabled(&buf) == false when agent env var is set")
	}
}

func TestStylingAndPadRightStyled(t *testing.T) {
	var buf bytes.Buffer
	clearUIEnv(t)

	// Without color:
	if got := StyleGreen(&buf, "ok"); got != "ok" {
		t.Errorf("StyleGreen without color = %q, want %q", got, "ok")
	}
	if got := PadRightStyled(&buf, "✓ ok", 8, StyleGreen); got != "✓ ok    " {
		t.Errorf("PadRightStyled without color = %q, want %q", got, "✓ ok    ")
	}

	// With color forced on:
	t.Setenv("GHISH_COLOR", "always")
	if got := StyleGreen(&buf, "ok"); got != "\x1b[32mok\x1b[0m" {
		t.Errorf("StyleGreen with color = %q", got)
	}
	if got := StyleRed(&buf, "err"); got != "\x1b[31merr\x1b[0m" {
		t.Errorf("StyleRed with color = %q", got)
	}
	if got := StyleYellow(&buf, "warn"); got != "\x1b[33mwarn\x1b[0m" {
		t.Errorf("StyleYellow with color = %q", got)
	}
	if got := StyleCyan(&buf, "info"); got != "\x1b[36minfo\x1b[0m" {
		t.Errorf("StyleCyan with color = %q", got)
	}
	if got := StyleBold(&buf, "bold"); got != "\x1b[1mbold\x1b[0m" {
		t.Errorf("StyleBold with color = %q", got)
	}
	if got := StyleDim(&buf, "dim"); got != "\x1b[2mdim\x1b[0m" {
		t.Errorf("StyleDim with color = %q", got)
	}

	// PadRightStyled pads based on visual rune count (4 runes in "✓ ok"),
	// so width 8 appends 4 trailing spaces after the ANSI reset sequence.
	wantPadded := "\x1b[32m✓ ok\x1b[0m    "
	if got := PadRightStyled(&buf, "✓ ok", 8, StyleGreen); got != wantPadded {
		t.Errorf("PadRightStyled with color = %q, want %q", got, wantPadded)
	}
}

func TestFormatAndPrintReceipt(t *testing.T) {
	var buf bytes.Buffer
	clearUIEnv(t)

	PrintReceipt(&buf, Receipt{
		Summary: "Change 12345 closed (abandoned) successfully.",
		Details: []ReceiptField{
			{Key: "Branch", Value: "feature-x"},
		},
		URL:  "https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/12345",
		Next: "./gh pr reopen 12345",
	})

	want := strings.Join([]string{
		"✓ Change 12345 closed (abandoned) successfully.",
		"  Branch: feature-x",
		"  URL:    https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/12345",
		"  Next:   ./gh pr reopen 12345",
		"",
	}, "\n")
	if buf.String() != want {
		t.Errorf("PrintReceipt got:\n%s\nwant:\n%s", buf.String(), want)
	}
}

func TestFormatReceipt_WidensLabelsForLongKeys(t *testing.T) {
	clearUIEnv(t)
	var buf bytes.Buffer
	got := FormatReceipt(&buf, Receipt{
		Summary: "Project \"x\" mounted in slot pw-01",
		Details: []ReceiptField{
			{Key: "Issue", Value: "b/1"},
			{Key: "Directory", Value: "/p/x"},
		},
		Next: "cd /p/x",
	})
	want := strings.Join([]string{
		"✓ Project \"x\" mounted in slot pw-01",
		"  Issue:     b/1",
		"  Directory: /p/x",
		"  Next:      cd /p/x",
		"",
	}, "\n")
	if got != want {
		t.Errorf("FormatReceipt got:\n%s\nwant:\n%s", got, want)
	}
}

func TestExtractGerritURLFromGitOutput(t *testing.T) {
	gitOut := `Enumerating objects: 5, done.
remote: Processing changes: refs: 1, new: 1, done
remote:
remote: SUCCESS
remote:
remote:   https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/396894 pw_ghish: Add UI helpers [NEW]
remote:
To https://pigweed.googlesource.com/pigweed/pigweed
 * [new reference]   HEAD -> refs/for/main
`
	got := ExtractGerritURLFromGitOutput(gitOut)
	want := "https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/396894"
	if got != want {
		t.Errorf("ExtractGerritURLFromGitOutput = %q, want %q", got, want)
	}

	if empty := ExtractGerritURLFromGitOutput("To https://pigweed.googlesource.com/pigweed/pigweed\n"); empty != "" {
		t.Errorf("expected empty URL when no remote: line has URL, got %q", empty)
	}
}
