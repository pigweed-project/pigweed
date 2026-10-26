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
	"fmt"
	"strings"
	"testing"
	"unicode/utf8"
)

func TestFailureLineRank(t *testing.T) {
	tests := []struct {
		name string
		line string
		want int
	}{
		{name: "empty line", line: "", want: 0},
		{name: "ordinary compiler progress", line: "[12/100] Compiling pw_rpc/client.cc", want: 0},
		{name: "identifier containing error substring", line: "int error_count = 0;", want: 0},
		{name: "located C++ compiler error", line: "pw_rpc/foo.cc:42:10: error: use of undeclared identifier 'bar'", want: 1},
		{name: "located C++ fatal error", line: "pw_string/builder.h:12:3: fatal error: 'span' file not found", want: 1},
		{name: "Rust compiler error code", line: "error[E0425]: cannot find value `foo` in this scope", want: 1},
		{name: "Bazel located BUILD error", line: "ERROR: /workspace/pw_ghish/BUILD.bazel:22:11: GoCompilePkg failed", want: 1},
		{name: "GN located error", line: "ERROR at //pw_build/target_types.gni:95:5: Undefined identifier", want: 1},
		{name: "GoogleTest located failure", line: "pw_status/status_test.cc:88: Failure", want: 1},
		{name: "TAP failure line", line: "not ok 4 - TestSelfAccountCache", want: 1},
		{name: "lld linker error", line: "ld.lld: error: undefined symbol: pw::foo()", want: 1},
		{name: "gnu ld undefined reference", line: "foo.cc:(.text+0x20): undefined reference to `bar'", want: 1},
		{name: "darwin ld Undefined symbols", line: "Undefined symbols for architecture arm64:", want: 1},
		{name: "collect2 linker error", line: "collect2: error: ld returned 1 exit status", want: 1},
		{name: "Ninja FAILED header", line: "FAILED: out/obj/pw_rpc/client.cc.o", want: 2},
		{name: "Go test FAIL header", line: "--- FAIL: TestResolveRemote (0.01s)", want: 2},
		{name: "Rust panic header", line: "thread 'main' panicked at 'index out of bounds'", want: 2},
		{name: "Python Traceback header", line: "Traceback (most recent call last):", want: 2},
		{name: "Assertion failure", line: "AssertionError: expected 0 but got 1", want: 2},
		{name: "Generic ERROR colon", line: "ERROR: build interrupted", want: 2},
		{name: "Standalone lowercase error word", line: "ninja: build stopped: subcommand encountered an error during build", want: 3},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			if got := failureLineRank(tc.line); got != tc.want {
				t.Errorf("failureLineRank(%q) = %d, want %d", tc.line, got, tc.want)
			}
		})
	}
}

func TestIsInfraNoiseLine(t *testing.T) {
	tests := []struct {
		name string
		line string
		want bool
	}{
		{name: "CAS upload line", line: "Uploading 42 blobs to CAS...", want: true},
		{name: "ResultDB stream line", line: "rdb stream: uploading test results", want: true},
		{name: "Isolate archive line", line: "Archiving output to isolate server", want: true},
		{name: "INFO log line", line: "[INFO] Starting test runner", want: true},
		{name: "Timestamp only line", line: "2026-10-23T22:15:04.123Z", want: true},
		{name: "Real compiler error", line: "pw_ghish/luci.go:42:2: error: undefined", want: false},
		{name: "Real build step line", line: "[14/80] Building CXX object foo.o", want: false},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			if got := isInfraNoiseLine(tc.line); got != tc.want {
				t.Errorf("isInfraNoiseLine(%q) = %v, want %v", tc.line, got, tc.want)
			}
		})
	}
}

func TestCompactFailureSummary(t *testing.T) {
	t.Run("empty summary", func(t *testing.T) {
		if got := compactFailureSummary("   \n  ", 8); got != "" {
			t.Errorf("compactFailureSummary(empty) = %q, want empty", got)
		}
	})

	t.Run("strips argparse usage block", func(t *testing.T) {
		input := strings.Join([]string{
			"usage: python_runner.py [-h] [--out OUT]",
			"                        [--depfile DEPFILE]",
			"  -h, --help  show this help message",
			"python_runner.py: error: unrecognized arguments: --bogus",
		}, "\n")
		got := compactFailureSummary(input, 8)
		if strings.Contains(got, "usage: python_runner.py") || strings.Contains(got, "--depfile") {
			t.Errorf("expected argparse usage block to be stripped, got:\n%s", got)
		}
		if !strings.Contains(got, "unrecognized arguments: --bogus") {
			t.Errorf("expected actual error line to be preserved, got:\n%s", got)
		}
	})

	t.Run("truncates beyond maxLines while preserving repro hint", func(t *testing.T) {
		var lines []string
		for i := 1; i <= 12; i++ {
			lines = append(lines, fmt.Sprintf("failure summary line %02d", i))
		}
		lines = append(lines, "Reproduce locally with: fx repro 87654321")
		got := compactFailureSummary(strings.Join(lines, "\n"), 8)
		if !strings.Contains(got, "failure summary line 08") {
			t.Errorf("expected line 08 to be kept, got:\n%s", got)
		}
		if strings.Contains(got, "failure summary line 09") {
			t.Errorf("expected line 09 to be omitted, got:\n%s", got)
		}
		if !strings.Contains(got, "... (5 summary lines omitted)") {
			t.Errorf("expected omitted summary count notice, got:\n%s", got)
		}
		if !strings.Contains(got, "Reproduce locally with: fx repro 87654321") {
			t.Errorf("expected repro line to be preserved after truncation, got:\n%s", got)
		}
	})
}

func TestExtractFailureSnippet(t *testing.T) {
	var lines []string
	for i := 1; i <= 20; i++ {
		lines = append(lines, fmt.Sprintf("[%02d/60] Compiling module_%02d.cc", i, i))
	}
	lines = append(lines, "FAILED: out/module_21.o")
	lines = append(lines, "pw_module/module_21.cc:77:5: error: static assertion failed: buffer too small")
	for i := 23; i <= 60; i++ {
		lines = append(lines, fmt.Sprintf("ninja: subcommand progress chatter %02d", i))
	}

	got := extractFailureSnippet(strings.Join(lines, "\n"), 15)
	if !strings.Contains(got, "pw_module/module_21.cc:77:5: error: static assertion failed") {
		t.Errorf("expected Rank 1 compiler error in snippet, got:\n%s", got)
	}
	if !strings.Contains(got, "FAILED: out/module_21.o") {
		t.Errorf("expected leading context before compiler error in snippet, got:\n%s", got)
	}
	if strings.Contains(got, "chatter 60") {
		t.Errorf("expected trailing teardown chatter to be excluded, got:\n%s", got)
	}
}

func TestExtractStepDiagnostic(t *testing.T) {
	tests := []struct {
		name     string
		input    string
		expected string
	}{
		{
			name:     "empty",
			input:    "",
			expected: "",
		},
		{
			name:     "single file diff",
			input:    "````\n--- /b/s/w/ir/x/w/co/docs/common/header.js  (original)\n+++ /b/s/w/ir/x/w/co/docs/common/header.js  (reformatted)\n@@ -67,7 +67,11 @@\n````",
			expected: "formatting diff in docs/common/header.js",
		},
		{
			name:     "multi file diff",
			input:    "````\n--- /b/s/w/ir/x/w/co/docs/common/header.py  (original)\n+++ /b/s/w/ir/x/w/co/docs/common/header.py  (reformatted)\n--- /b/s/w/ir/x/w/co/docs/common/nav.py  (original)\n+++ /b/s/w/ir/x/w/co/docs/common/nav.py  (reformatted)\n--- /b/s/w/ir/x/w/co/docs/tests/header_test.py  (original)\n+++ /b/s/w/ir/x/w/co/docs/tests/header_test.py  (reformatted)\n--- /b/s/w/ir/x/w/co/docs/tests/search_test.py  (original)\n+++ /b/s/w/ir/x/w/co/docs/tests/search_test.py  (reformatted)\n````",
			expected: "formatting diff in 4 files: docs/common/header.py, docs/common/nav.py (+2 more)",
		},
		{
			name:     "compiler error with line number",
			input:    "````\n[ACTION //pw_build/py:workflows.lint.mypy(//pw_build/python_toolchain:python)]\nFAILED: [code=1] python/gen/pw_build/py/workflows.lint.mypy.pw_pystamp\npython [...]/py/pw_build/python_runner.py ...\n[...]/pw_build/workflows/build_driver.py:66: error: Argument 1 to \"ParseDict\" has incompatible type \"str\"\nFound 1 error in 1 file (checked 13 source files)\n````",
			expected: "build_driver.py:66: error: Argument 1 to \"ParseDict\" has incompatible type \"str\"",
		},
		{
			name:     "split line compiler error where error message is on next line",
			input:    "````\n[ACTION //pw_build/py:workflows.lint.mypy(//pw_build/python_toolchain:python)]\nFAILED: [code=1] python/gen/pw_build/py/workflows.lint.mypy.pw_pystamp\n[...]/pw_build/workflows/build_driver.py:66: error:\nArgument 1 to \"ParseDict\" has incompatible type \"str\"; expected \"dict[str, Any]\"\nFound 1 error in 1 file\n````",
			expected: "build_driver.py:66: error: Argument 1 to \"ParseDict\" has incompatible type \"s...",
		},
		{
			name:     "single line failure message",
			input:    "ninja execution failed",
			expected: "ninja execution failed",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			actual := extractStepDiagnostic(tc.input)
			if actual != tc.expected {
				t.Errorf("extractStepDiagnostic() =\n%q\nwant:\n%q", actual, tc.expected)
			}
		})
	}
}

func TestCleanStepSummary(t *testing.T) {
	tests := []struct {
		name     string
		input    string
		expected string
	}{
		{
			name:     "empty",
			input:    "",
			expected: "",
		},
		{
			name:     "short error",
			input:    "ninja: error: 'foo.cc' not found",
			expected: "ninja: error: 'foo.cc' not found",
		},
		{
			name:     "longer useful error between 60 and 120 chars",
			input:    "clang: error: unable to execute command: Segmentation fault (core dumped) in /workspace/pigweed/foo/bar.cc:42",
			expected: "clang: error: unable to execute command: Segmentation fault (core dumped) in /workspace/pigweed/foo/bar.cc:42",
		},
		{
			name:     "error exceeding 120 chars truncated",
			input:    "this is a very long error message that exceeds one hundred and twenty characters in length and therefore should be truncated cleanly with an ellipsis",
			expected: "this is a very long error message that exceeds one hundred and twenty characters in length and therefore should be tr...",
		},
		{
			name:     "json dump suppressed",
			input:    "{\"error\": \"details\", \"status\": 500}",
			expected: "",
		},
		{
			name:     "embedded json data dump suppressed",
			input:    "returned unexpected payload: {\"key\": \"value\"}",
			expected: "",
		},
		{
			name:     "python repr suppressed",
			input:    "Change(number=123, project='pw')",
			expected: "",
		},
		{
			name:     "multiline suppressed",
			input:    "line 1\nline 2",
			expected: "",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got := cleanStepSummary(tc.input)
			if got != tc.expected {
				t.Errorf("cleanStepSummary(%q) = %q, want %q", tc.input, got, tc.expected)
			}
		})
	}
}

func TestClusterFailedBuilds(t *testing.T) {
	builds := []bbBuild{
		FakeBuild("1", "builder-a", "FAILURE", WithSummary("\n  ninja: build stopped: subcommand failed.\nmore details A")),
		FakeBuild("2", "builder-b", "FAILURE", WithSummary("ninja: build stopped: subcommand failed.\nmore details B")),
		FakeBuild("3", "builder-c", "INFRA_FAILURE", WithSummary("unique infra error")),
		FakeBuild("4", "builder-d", "SUCCESS", WithSummary("ninja: build stopped: subcommand failed.")),
	}

	clusters := ClusterFailedBuilds(builds)
	if len(clusters) != 1 {
		t.Fatalf("len(clusters) = %d, want 1 (%v)", len(clusters), clusters)
	}
	want := "Cluster: 2 builders failed with the same diagnostic: ninja: build stopped: subcommand failed."
	if clusters[0] != want {
		t.Errorf("clusters[0] = %q, want %q", clusters[0], want)
	}
}

func TestFormatFailureReports_InfraFailureAnnotation(t *testing.T) {
	reports := []FailureReport{
		{
			Builder:     "mac-arm-zephyr",
			BuildID:     "8671031638107722401",
			BuildURL:    "https://ci.chromium.org/b/8671031638107722401",
			Status:      "INFRA_FAILURE",
			StepSummary: "Task did not start, no resource",
		},
	}

	formatted := FormatFailureReports(reports)
	wantHeader := "Status: INFRA_FAILURE [infra failure]"
	if !strings.Contains(formatted, wantHeader) {
		t.Errorf("Expected FormatFailureReports to contain %q, got:\n%s", wantHeader, formatted)
	}
}

func TestFormatFailureReports_BudgetsLogsOmitsWallsAndDeduplicatesBuilders(t *testing.T) {
	var logLines []string
	for i := 1; i <= 40; i++ {
		logLines = append(logLines, fmt.Sprintf("compiler output line %02d", i))
	}
	longLog := strings.Join(logLines, "\n")

	reports := []FailureReport{
		{
			Builder:     "builder-one",
			BuildID:     "1001",
			BuildURL:    "https://ci.chromium.org/b/1001",
			Status:      "FAILURE",
			FailedStep:  "compile",
			StepSummary: "ninja failed",
			LogName:     "stdout",
			LogSnippet:  longLog,
			FullLogURL:  "https://logs.chromium.org/1001",
		},
		{
			Builder:     "builder-two",
			BuildID:     "1002",
			BuildURL:    "https://ci.chromium.org/b/1002",
			Status:      "FAILURE",
			FailedStep:  "compile",
			StepSummary: "ninja failed",
			LogName:     "stdout",
			LogSnippet:  longLog,
			FullLogURL:  "https://logs.chromium.org/1002",
		},
	}

	formatted := FormatFailureReports(reports)
	if strings.Contains(formatted, strings.Repeat("=", 80)) {
		t.Errorf("Expected no 80-char === walls in FormatFailureReports, got:\n%s", formatted)
	}
	if strings.Contains(formatted, "compiler output line 15") {
		t.Errorf("Expected earlier log lines (<= 15) to be omitted by 25-line budget, got:\n%s", formatted)
	}
	if !strings.Contains(formatted, "compiler output line 16") || !strings.Contains(formatted, "compiler output line 40") {
		t.Errorf("Expected last 25 log lines (16..40) to be retained, got:\n%s", formatted)
	}
	if !strings.Contains(formatted, "... (15 earlier log lines omitted; run 'gh run view -j builder-one --log' for full log)") {
		t.Errorf("Expected truncation notice for builder-one, got:\n%s", formatted)
	}
	if !strings.Contains(formatted, "Same failure output as builder-one (omitted duplicate log)") {
		t.Errorf("Expected cross-builder duplicate log collapsing for builder-two, got:\n%s", formatted)
	}
	if count := strings.Count(formatted, "compiler output line 40"); count != 1 {
		t.Errorf("Expected log snippet to be printed only once across duplicate builders, got %d times:\n%s", count, formatted)
	}
	if !strings.Contains(formatted, "Full Log URL: https://logs.chromium.org/1002") {
		t.Errorf("Expected Full Log URL for builder-two to be preserved, got:\n%s", formatted)
	}
}

func TestBudgetLogSnippet_RankedWindowExtractsDiagnosticOverTeardownChatter(t *testing.T) {
	var lines []string
	for i := 1; i <= 14; i++ {
		lines = append(lines, fmt.Sprintf("[build %02d/60] compiling unit_%02d.cc", i, i))
	}
	lines = append(lines, "pw_rpc/foo.cc:42:10: error: use of undeclared identifier 'bar'")
	for i := 16; i <= 60; i++ {
		lines = append(lines, fmt.Sprintf("ninja: build stopped: subcommand progress chatter line %02d", i))
	}
	got := budgetLogSnippet(strings.Join(lines, "\n"), "pigweed-linux", 25, 60)
	if !strings.Contains(got, "pw_rpc/foo.cc:42:10: error: use of undeclared identifier 'bar'") {
		t.Errorf("expected located compiler error on line 15 to be retained in 25-line window, got:\n%s", got)
	}
	if !strings.Contains(got, "[build 10/60] compiling unit_10.cc") {
		t.Errorf("expected 5 lines of context before line 15 (starting at line 10), got:\n%s", got)
	}
	if strings.Contains(got, "chatter line 60") {
		t.Errorf("expected trailing teardown chatter at line 60 to be excluded in favor of diagnostic window, got:\n%s", got)
	}
}

func TestTruncateVisible_MultiByteRunes(t *testing.T) {
	// 85 multi-byte runes ('✓' is 3 bytes in UTF-8)
	s := strings.Repeat("✓", 85)
	got := TruncateVisible(s, 80)
	if !utf8.ValidString(got) {
		t.Fatalf("TruncateVisible produced invalid UTF-8: %q", got)
	}
	if utf8.RuneCountInString(got) != 80 {
		t.Errorf("expected 80 runes, got %d", utf8.RuneCountInString(got))
	}
	if !strings.HasSuffix(got, "...") {
		t.Errorf("expected '...' suffix, got %q", got)
	}
}

func TestBudgetLogSnippet_TotalLines(t *testing.T) {
	numbered := func(from, to int) string {
		var lines []string
		for i := from; i <= to; i++ {
			lines = append(lines, fmt.Sprintf("line %d", i))
		}
		return strings.Join(lines, "\n")
	}
	tests := []struct {
		name       string
		snippet    string
		maxLines   int
		totalLines int
		want       string
	}{
		{
			name:       "snippet longer than budget reports lines omitted from the full log",
			snippet:    numbered(101, 200),
			maxLines:   25,
			totalLines: 200,
			want:       "... (175 earlier log lines omitted)\n" + numbered(176, 200),
		},
		{
			name:       "snippet within budget still reports lines tailed away",
			snippet:    numbered(91, 100),
			maxLines:   25,
			totalLines: 100,
			want:       "... (90 earlier log lines omitted)\n" + numbered(91, 100),
		},
		{
			name:       "whole log shown omits nothing",
			snippet:    numbered(1, 10),
			maxLines:   25,
			totalLines: 10,
			want:       numbered(1, 10),
		},
		{
			name:       "blank lines in the snippet are kept within budget and counted as shown",
			snippet:    "a\n\nb",
			maxLines:   25,
			totalLines: 5,
			want:       "... (2 earlier log lines omitted)\na\n\nb",
		},
		{
			name:       "blank lines are dropped when trimming to budget",
			snippet:    "a\n\nb\n\nc",
			maxLines:   2,
			totalLines: 5,
			want:       "... (3 earlier log lines omitted)\nb\nc",
		},
		{
			name:     "unknown total falls back to counting within the snippet",
			snippet:  numbered(1, 30),
			maxLines: 25,
			want:     "... (5 earlier log lines omitted)\n" + numbered(6, 30),
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := budgetLogSnippet(tt.snippet, "", tt.maxLines, tt.totalLines); got != tt.want {
				t.Errorf("budgetLogSnippet() =\n%s\nwant:\n%s", got, tt.want)
			}
		})
	}
}
