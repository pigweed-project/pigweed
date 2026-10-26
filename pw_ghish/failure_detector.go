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
	"regexp"
	"strings"
	"unicode/utf8"
)

const (
	maxFailureSummaryLines = 8
	maxFailureLogLines     = 25
)

var (
	diffFileRegex = regexp.MustCompile(`^\+\+\+\s+(?:[ab]/)?([^\s\t]+)`)
	botPathRegex  = regexp.MustCompile(`\[\.\.\.\]/[^:]+/`)

	infraNoiseKeywordRegex = regexp.MustCompile(`(?i)\b(upload|isolate|cas|rdb)\b|\[INFO\]`)
	timestampOnlyLineRegex = regexp.MustCompile(`^\s*[-0-9:.TZ /\[\],]*\d[-0-9:.TZ /\[\],]*\s*$`)
	locatedCompilerErrRe   = regexp.MustCompile(`[^\s:]+\.[a-zA-Z0-9_+]+:\d+(?::\d+)?:\s*(?:fatal\s+)?error:`)
	rustcErrCodeRe         = regexp.MustCompile(`^error\[E\d+\]:`)
	gnBazelLocatedErrRe    = regexp.MustCompile(`^ERROR(?::|\s+at)\s+[^\s:]+:\d+:`)
	gtestFailureLineRe     = regexp.MustCompile(`[^\s:]+\.[a-zA-Z0-9_+]+:\d+:\s*Failure\b`)
	standaloneErrorWordRe  = regexp.MustCompile(`\berror\b`)
)

// TruncateVisible truncates s to at most maxRunes Unicode code points (runes),
// appending "..." when truncated so multi-byte UTF-8 sequences are never split.
func TruncateVisible(s string, maxRunes int) string {
	if maxRunes <= 0 {
		return ""
	}
	if utf8.RuneCountInString(s) <= maxRunes {
		return s
	}
	runes := []rune(s)
	if maxRunes <= 3 {
		return string(runes[:maxRunes])
	}
	return string(runes[:maxRunes-3]) + "..."
}

// cleanBuildSummary removes internal infra experiment noise from the top-level build summary.
func cleanBuildSummary(summary string) string {
	summary = strings.TrimSpace(summary)
	if summary == "" {
		return ""
	}
	lines := strings.Split(summary, "\n")
	var kept []string
	inExperiments := false
	for _, line := range lines {
		trimmed := strings.TrimSpace(line)
		if strings.HasPrefix(trimmed, "**Experiments**") || strings.HasPrefix(trimmed, "Experiments:") {
			inExperiments = true
			continue
		}
		if inExperiments {
			if strings.HasPrefix(trimmed, "* ") || strings.HasPrefix(trimmed, "- ") || trimmed == "" {
				continue
			}
			inExperiments = false
		}
		kept = append(kept, line)
	}
	return strings.TrimSpace(strings.Join(kept, "\n"))
}

// cleanStepSummary sanitizes recipe step summaries, filtering out multi-line dumps,
// Python repr objects, raw JSON/dicts, and disk usage noise.
func cleanStepSummary(summary string) string {
	summary = strings.TrimSpace(summary)
	if summary == "" {
		return ""
	}
	// Suppress multi-line summaries or raw data dumps
	if strings.Contains(summary, "\n") {
		return ""
	}
	// Suppress Python repr / object dumps
	if strings.HasPrefix(summary, "Change(") || strings.HasPrefix(summary, "applied [") || strings.HasPrefix(summary, "[Change(") {
		return ""
	}
	// Suppress JSON / YAML / dict dumps
	if strings.HasPrefix(summary, "{") || strings.HasPrefix(summary, "[") || strings.HasPrefix(summary, "remote:") || strings.Contains(summary, "{\"") || strings.Contains(summary, "{'") {
		return ""
	}
	// Suppress disk usage noise
	if strings.Contains(summary, "GB used") {
		return ""
	}
	// Suppress markdown links/bullets that don't belong inline
	if strings.HasPrefix(summary, "* ") || strings.HasPrefix(summary, "- ") {
		return ""
	}
	return TruncateVisible(summary, 120)
}

// extractStepDiagnostic extracts a concise single-line failure diagnostic from
// recipe step summaries (such as unified diffs, compiler diagnostics, or failure reasons).
func extractStepDiagnostic(summary string) string {
	summary = strings.TrimSpace(summary)
	summary = strings.Trim(summary, "`")
	summary = strings.TrimSpace(summary)
	if summary == "" {
		return ""
	}

	// 1. Check for unified diff headers (linters and formatters)
	var diffFiles []string
	lines := strings.Split(summary, "\n")
	for _, line := range lines {
		line = strings.TrimSpace(line)
		m := diffFileRegex.FindStringSubmatch(line)
		if len(m) > 1 {
			rawPath := m[1]
			cleanPath := rawPath
			if idx := strings.Index(cleanPath, "/co/"); idx != -1 {
				cleanPath = cleanPath[idx+4:]
			} else if idx := strings.Index(cleanPath, "/checkout/"); idx != -1 {
				cleanPath = cleanPath[idx+10:]
			} else if strings.Contains(cleanPath, "/s/w/ir/") {
				parts := strings.Split(cleanPath, "/")
				cleanPath = parts[len(parts)-1]
			}
			cleanPath = strings.TrimSpace(cleanPath)
			if cleanPath != "" {
				diffFiles = append(diffFiles, cleanPath)
			}
		}
	}
	diffFiles = dedup(diffFiles)
	if len(diffFiles) == 1 {
		return fmt.Sprintf("formatting diff in %s", diffFiles[0])
	} else if len(diffFiles) > 1 && len(diffFiles) <= 3 {
		return fmt.Sprintf("formatting diff in %d files: %s", len(diffFiles), strings.Join(diffFiles, ", "))
	} else if len(diffFiles) > 3 {
		return fmt.Sprintf("formatting diff in %d files: %s, %s (+%d more)", len(diffFiles), diffFiles[0], diffFiles[1], len(diffFiles)-2)
	}

	// 2. Check for compiler, linter, or fatal error lines
	var errorLine string
	for i, line := range lines {
		l := strings.TrimSpace(line)
		if l == "" || strings.HasPrefix(l, "``") || strings.HasPrefix(l, "---") || strings.HasPrefix(l, "+++") {
			continue
		}
		if strings.Contains(l, "error:") || strings.Contains(l, "error [") || strings.Contains(l, "fatal error:") {
			cleaned := botPathRegex.ReplaceAllString(l, "")
			// If the line ends with "error:", pull in the next non-empty line
			if (strings.HasSuffix(cleaned, "error:") || strings.HasSuffix(cleaned, "fatal error:")) && i+1 < len(lines) {
				next := strings.TrimSpace(lines[i+1])
				if next != "" && !strings.HasPrefix(next, "``") {
					cleaned = cleaned + " " + next
				}
			}
			errorLine = cleaned
			break
		}
		if strings.HasPrefix(l, "FAILED:") && errorLine == "" {
			errorLine = l
		}
	}
	if errorLine != "" {
		return TruncateVisible(errorLine, 80)
	}

	// 3. Check for test failure summaries (e.g. "Found 1 error in 1 file", "FAILED (failures=1)")
	for _, line := range lines {
		l := strings.TrimSpace(line)
		if strings.HasPrefix(l, "Found ") && strings.Contains(l, "error") {
			return TruncateVisible(l, 80)
		}
	}

	// 4. Single-line summary fallback if clean and not raw data
	if !strings.Contains(summary, "\n") {
		clean := cleanStepSummary(summary)
		if clean != "" {
			return clean
		}
	}

	// 5. First non-empty, non-fence, non-header line as fallback
	for _, line := range lines {
		l := strings.TrimSpace(line)
		if l == "" || strings.HasPrefix(l, "``") || strings.HasPrefix(l, "[ACTION") || strings.HasPrefix(l, "python ") {
			continue
		}
		if strings.HasPrefix(l, "Change(") || strings.HasPrefix(l, "{") || strings.HasPrefix(l, "[") {
			continue
		}
		return TruncateVisible(l, 80)
	}

	return ""
}

// deduplicateSummaryAgainstLog removes fenced code blocks and truncation notices
// from summary when logSnippet already provides the full failure log content,
// preserving actionable footer lines (such as `fx repro` instructions).
func deduplicateSummaryAgainstLog(summary, logSnippet string) string {
	summary = strings.TrimSpace(summary)
	if summary == "" || logSnippet == "" {
		return summary
	}
	if strings.Contains(logSnippet, summary) {
		return ""
	}
	lines := strings.Split(summary, "\n")
	var kept []string
	inFence := false
	fenceMarker := ""
	var fenceLines []string

	flushFenceIfNotRedundant := func() {
		if len(fenceLines) == 0 {
			return
		}
		firstNonEmpty := ""
		for _, fl := range fenceLines[1:] {
			t := strings.TrimSpace(fl)
			if t != "" && !strings.HasPrefix(t, "```") {
				firstNonEmpty = t
				break
			}
		}
		if firstNonEmpty == "" || !strings.Contains(logSnippet, firstNonEmpty) {
			kept = append(kept, fenceLines...)
		}
		fenceLines = nil
	}

	for _, line := range lines {
		trimmed := strings.TrimSpace(line)
		if !inFence && strings.HasPrefix(trimmed, "```") {
			inFence = true
			fenceMarker = "```"
			if strings.HasPrefix(trimmed, "````") {
				fenceMarker = "````"
			}
			fenceLines = append(fenceLines, line)
			continue
		}
		if inFence {
			fenceLines = append(fenceLines, line)
			if strings.HasPrefix(trimmed, fenceMarker) {
				inFence = false
				flushFenceIfNotRedundant()
			}
			continue
		}
		if strings.HasPrefix(trimmed, "(failure summary truncated") {
			continue
		}
		kept = append(kept, line)
	}
	if inFence {
		flushFenceIfNotRedundant()
	}
	return strings.TrimSpace(strings.Join(kept, "\n"))
}

func firstNonEmptySummaryLine(summary string) string {
	for _, line := range strings.Split(summary, "\n") {
		trimmed := strings.TrimSpace(line)
		if trimmed != "" {
			return trimmed
		}
	}
	return ""
}

// ClusterFailedBuilds inspects failing builds (FAILURE or INFRA_FAILURE) and
// returns cluster summary lines for any diagnostic (first non-empty line of
// SummaryMarkdown) shared by 2 or more failed builders.
func ClusterFailedBuilds(builds []bbBuild) []string {
	var order []string
	counts := make(map[string]int)
	for _, b := range builds {
		if b.Status != "FAILURE" && b.Status != "INFRA_FAILURE" {
			continue
		}
		firstLine := firstNonEmptySummaryLine(b.SummaryMarkdown)
		if firstLine == "" {
			continue
		}
		if counts[firstLine] == 0 {
			order = append(order, firstLine)
		}
		counts[firstLine]++
	}
	var clusters []string
	for _, line := range order {
		if counts[line] >= 2 {
			clusters = append(clusters, fmt.Sprintf("Cluster: %d builders failed with the same diagnostic: %s", counts[line], line))
		}
	}
	return clusters
}

func compactFailureSummary(summary string, maxLines int) string {
	summary = strings.TrimSpace(summary)
	if summary == "" {
		return ""
	}
	rawLines := strings.Split(summary, "\n")
	var filtered []string
	skippingUsage := false
	for _, line := range rawLines {
		trimmed := strings.TrimSpace(line)
		lower := strings.ToLower(trimmed)
		if strings.HasPrefix(lower, "usage:") && strings.Contains(lower, "[-h]") {
			skippingUsage = true
			continue
		}
		if skippingUsage {
			if trimmed == "" || strings.HasPrefix(line, " ") || strings.HasPrefix(line, "\t") || strings.HasPrefix(trimmed, "[") || strings.HasPrefix(trimmed, "-") {
				continue
			}
			skippingUsage = false
		}
		filtered = append(filtered, line)
	}

	var nonEmptyIdx []int
	for i, l := range filtered {
		if strings.TrimSpace(l) != "" {
			nonEmptyIdx = append(nonEmptyIdx, i)
		}
	}
	if len(nonEmptyIdx) <= maxLines {
		return strings.TrimSpace(strings.Join(filtered, "\n"))
	}

	cutoff := nonEmptyIdx[maxLines-1]
	kept := append([]string(nil), filtered[:cutoff+1]...)
	var reproLines []string
	for _, l := range filtered[cutoff+1:] {
		if strings.Contains(strings.ToLower(l), "repro") {
			reproLines = append(reproLines, strings.TrimSpace(l))
		}
	}
	kept = append(kept, fmt.Sprintf("... (%d summary lines omitted)", len(nonEmptyIdx)-maxLines))
	kept = append(kept, reproLines...)
	return strings.TrimSpace(strings.Join(kept, "\n"))
}

func isInfraNoiseLine(line string) bool {
	if infraNoiseKeywordRegex.MatchString(line) {
		return true
	}
	return timestampOnlyLineRegex.MatchString(line)
}

// failureLineRank scores a trimmed log line by diagnostic priority:
//   - Rank 1: located compiler/linker/build/test failure lines (`file:line: error:`, `ld.lld: error:`, `ERROR: path:line:`)
//   - Rank 2: failure headers (`FAILED`, `FAIL: `, `panicked at`, `Assertion`, `Traceback`, `ERROR:`)
//   - Rank 3: standalone word `error`
//   - Rank 0: ordinary output line
func failureLineRank(trimmed string) int {
	if trimmed == "" {
		return 0
	}
	if locatedCompilerErrRe.MatchString(trimmed) ||
		rustcErrCodeRe.MatchString(trimmed) ||
		gnBazelLocatedErrRe.MatchString(trimmed) ||
		gtestFailureLineRe.MatchString(trimmed) ||
		strings.HasPrefix(trimmed, "not ok ") ||
		strings.Contains(trimmed, "Undefined symbols") ||
		strings.Contains(trimmed, "undefined reference to") ||
		strings.Contains(trimmed, "ld.lld: error:") ||
		strings.Contains(trimmed, "collect2: error:") {
		return 1
	}
	if strings.Contains(trimmed, "FAILED") ||
		strings.Contains(trimmed, "FAIL: ") ||
		strings.Contains(trimmed, "panicked at") ||
		strings.Contains(trimmed, "Assertion") ||
		strings.Contains(trimmed, "ASSERT") ||
		strings.Contains(trimmed, "Traceback") ||
		strings.Contains(trimmed, "Error:") ||
		strings.Contains(trimmed, "ERROR:") ||
		strings.Contains(trimmed, "ERRORS:") ||
		strings.Contains(trimmed, "ERROR ") {
		return 2
	}
	if standaloneErrorWordRe.MatchString(trimmed) {
		return 3
	}
	return 0
}

// selectFailureLogWindow selects up to maxLines non-empty lines from nonEmpty,
// filtering out infrastructure noise lines and anchoring a window (with up to
// 5 lines of leading context) around the highest-ranked failure diagnostic.
func selectFailureLogWindow(nonEmpty []string, maxLines int) []string {
	if len(nonEmpty) <= maxLines {
		return nonEmpty
	}
	var candidates []string
	for _, line := range nonEmpty {
		if !isInfraNoiseLine(line) {
			candidates = append(candidates, line)
		}
	}
	if len(candidates) == 0 {
		candidates = nonEmpty
	}
	if len(candidates) <= maxLines {
		return candidates
	}
	bestIdx := -1
	bestRank := 0
	for i, line := range candidates {
		r := failureLineRank(strings.TrimSpace(line))
		if r > 0 && (bestRank == 0 || r < bestRank) {
			bestRank = r
			bestIdx = i
			if bestRank == 1 {
				break
			}
		}
	}
	if bestRank > 0 {
		start := max(bestIdx-5, 0)
		end := min(start+maxLines, len(candidates))
		start = max(end-maxLines, 0)
		return candidates[start:end]
	}
	return candidates[len(candidates)-maxLines:]
}

// extractFailureSnippet extracts up to maxLines non-empty lines from rawLog,
// prioritizing a window around the highest-ranked compiler, linker, build, or
// test diagnostic over trailing teardown chatter.
func extractFailureSnippet(rawLog string, maxLines int) string {
	if maxLines <= 0 {
		maxLines = 40
	}
	rawLog = strings.TrimSpace(rawLog)
	if rawLog == "" {
		return ""
	}
	var nonEmpty []string
	for _, line := range strings.Split(rawLog, "\n") {
		if strings.TrimSpace(line) != "" {
			nonEmpty = append(nonEmpty, TruncateVisible(line, 350))
		}
	}
	if len(nonEmpty) == 0 {
		return ""
	}
	kept := selectFailureLogWindow(nonEmpty, maxLines)
	return strings.TrimSpace(strings.Join(kept, "\n"))
}

// budgetLogSnippet extracts up to maxLines non-empty lines of snippet,
// prioritizing a window around the highest-ranked compiler/linker/test
// diagnostic (with 5 lines of leading context) over trailing Ninja/Bazel
// teardown chatter, and capping each line at 350 runes. When totalLines (the
// full log's line count) is known, the omission notice reports every line not
// shown, including those already dropped when the snippet was tailed from the
// full log.
func budgetLogSnippet(snippet, builder string, maxLines, totalLines int) string {
	snippet = strings.TrimRight(snippet, "\n")
	if snippet == "" {
		return ""
	}
	lines := strings.Split(snippet, "\n")
	var cappedAll []string
	var nonEmpty []string
	for _, line := range lines {
		capped := TruncateVisible(line, 350)
		cappedAll = append(cappedAll, capped)
		if strings.TrimSpace(line) != "" {
			nonEmpty = append(nonEmpty, capped)
		}
	}
	kept, shown := nonEmpty, strings.Join(cappedAll, "\n")
	if len(nonEmpty) > maxLines {
		kept = selectFailureLogWindow(nonEmpty, maxLines)
		shown = strings.Join(kept, "\n")
	}
	omitted := len(nonEmpty) - len(kept)
	if totalLines > 0 {
		shownLines := len(lines)
		if len(nonEmpty) > maxLines {
			shownLines = len(kept)
		}
		omitted = max(totalLines-shownLines, 0)
	}
	if omitted == 0 {
		return shown
	}
	if builder != "" {
		return fmt.Sprintf("... (%d earlier log lines omitted; run 'gh run view -j %s --log' for full log)\n%s", omitted, builder, shown)
	}
	return fmt.Sprintf("... (%d earlier log lines omitted)\n%s", omitted, shown)
}

// FormatFailureReports formats diagnostic failure reports into a readable string.
func FormatFailureReports(reports []FailureReport) string {
	var sb strings.Builder
	seenSignatures := make(map[string]string, len(reports))

	for i, r := range reports {
		if i > 0 {
			sb.WriteString("\n")
		}
		// A plain glyph-led header keeps each report scannable without
		// decorative rules (see "UI design language" in developing.rst).
		if r.Patchset > 0 {
			fmt.Fprintf(&sb, "%s FAILURE: %s (Build %s, from patchset %d)\n", GlyphFail, r.Builder, r.BuildID, r.Patchset)
		} else {
			fmt.Fprintf(&sb, "%s FAILURE: %s (Build %s)\n", GlyphFail, r.Builder, r.BuildID)
		}

		statusLabel := r.Status
		if r.Status == "INFRA_FAILURE" {
			statusLabel = "INFRA_FAILURE [infra failure]"
		}
		fmt.Fprintf(&sb, "Status: %s | URL: %s\n", statusLabel, r.BuildURL)
		if r.FailedStep != "" {
			fmt.Fprintf(&sb, "Step: %s\n", r.FailedStep)
		}

		normStep := strings.TrimSpace(r.FailedStep)
		normLog := strings.TrimSpace(r.LogSnippet)
		normSum := strings.TrimSpace(r.StepSummary)
		sigKey := ""
		if normLog != "" {
			sigKey = "step:" + normStep + "\x00log:" + normLog
		} else if normSum != "" {
			sigKey = "step:" + normStep + "\x00sum:" + normSum
		}
		if sigKey != "" {
			if firstBuilder, ok := seenSignatures[sigKey]; ok && firstBuilder != "" && firstBuilder != r.Builder {
				fmt.Fprintf(&sb, "Same failure output as %s (omitted duplicate log)\n", firstBuilder)
				if r.FullLogURL != "" {
					fmt.Fprintf(&sb, "\nFull Log URL: %s\n", r.FullLogURL)
				}
				continue
			}
			if _, ok := seenSignatures[sigKey]; !ok && r.Builder != "" {
				seenSignatures[sigKey] = r.Builder
			}
		}

		stepSum := deduplicateSummaryAgainstLog(r.StepSummary, r.LogSnippet)
		buildSum := deduplicateSummaryAgainstLog(r.BuildSummary, r.LogSnippet)
		if stepSum != "" && buildSum != "" {
			if strings.Contains(stepSum, buildSum) {
				buildSum = ""
			} else if strings.Contains(buildSum, stepSum) {
				stepSum = buildSum
				buildSum = ""
			}
		}
		stepSum = compactFailureSummary(stepSum, maxFailureSummaryLines)
		buildSum = compactFailureSummary(buildSum, maxFailureSummaryLines)

		if stepSum != "" {
			sb.WriteString(stepSum)
			sb.WriteString("\n")
		}
		if buildSum != "" && !strings.Contains(stepSum, buildSum) {
			if stepSum != "" {
				sb.WriteString("\n")
			}
			sb.WriteString(buildSum)
			sb.WriteString("\n")
		}
		if r.LogSnippet != "" {
			header := "Log Snippet"
			if r.LogName != "" {
				header = fmt.Sprintf("Log Snippet (%s)", r.LogName)
			}
			fmt.Fprintf(&sb, "\n%s:\n", header)
			sb.WriteString(budgetLogSnippet(r.LogSnippet, r.Builder, maxFailureLogLines, r.LogTotalLines))
			sb.WriteString("\n")
		}
		if r.FullLogURL != "" {
			fmt.Fprintf(&sb, "\nFull Log URL: %s\n", r.FullLogURL)
		}
	}
	return sb.String()
}
