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
	"encoding/base64"
	"fmt"
	"path/filepath"
	"slices"
	"strings"

	"github.com/andygrunwald/go-gerrit"
	"github.com/spf13/cobra"
)

const defaultDiffMaxLines = 400

var (
	diffNameOnly bool
	diffStat     bool
	diffAll      bool
	diffPaths    []string
)

func formatDiffStat(files map[string]gerrit.FileInfo) string {
	var paths []string
	maxPathLen := 0
	totalInsertions := 0
	totalDeletions := 0

	for p := range files {
		if p == "/COMMIT_MSG" {
			continue
		}
		paths = append(paths, p)
		if len(p) > maxPathLen {
			maxPathLen = len(p)
		}
	}
	slices.Sort(paths)

	var sb strings.Builder
	for _, p := range paths {
		info := files[p]
		ins := info.LinesInserted
		del := info.LinesDeleted
		total := ins + del
		totalInsertions += ins
		totalDeletions += del

		graph := ""
		if total > 0 {
			plusCount := ins
			minusCount := del
			if total > 40 {
				plusCount = (ins * 40) / total
				minusCount = (del * 40) / total
			}
			graph = strings.Repeat("+", plusCount) + strings.Repeat("-", minusCount)
		}
		sb.WriteString(fmt.Sprintf(" %-*s | %d %s\n", maxPathLen, p, total, graph))
	}

	fileWord := "files"
	if len(paths) == 1 {
		fileWord = "file"
	}
	sb.WriteString(fmt.Sprintf(" %d %s changed, %d insertions(+), %d deletions(-)\n", len(paths), fileWord, totalInsertions, totalDeletions))
	return sb.String()
}

var diffCmd = &cobra.Command{
	Use:         "diff [<id>[/<patchset>]]",
	Annotations: map[string]string{KindAnnotation: KindView, OutputAnnotation: OutputRaw},
	Short:       "View changes as a diff",
	Args:        cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		chCtx, err := ResolveChangeContext(cmd, args)
		if err != nil {
			return err
		}

		if diffNameOnly || diffStat {
			files, _, err := chCtx.Client.Changes.ListFiles(chCtx.Context, chCtx.ChangeID, chCtx.Revision, nil)
			if err != nil {
				errStr := err.Error()
				if (strings.Contains(errStr, "404") || strings.Contains(errStr, "Not Found")) && chCtx.Revision != "current" && chCtx.Revision != "" {
					return fmt.Errorf("error listing files for change %s (patchset %s): patchset not found (HTTP 404).\n\nTo view the change and available patchsets, run:\n  gh pr view %s\n\nUnderlying error: %w", chCtx.ChangeID, chCtx.Revision, chCtx.ChangeID, err)
				}
				return chCtx.FormatError(err, "listing files for")
			}

			if diffNameOnly {
				var paths []string
				for p := range files {
					if p == "/COMMIT_MSG" {
						continue
					}
					paths = append(paths, p)
				}
				slices.Sort(paths)
				for _, p := range paths {
					fmt.Fprintln(cmd.OutOrStdout(), p)
				}
				return nil
			}

			fmt.Fprint(cmd.OutOrStdout(), formatDiffStat(files))
			return nil
		}

		patchPtr, _, err := chCtx.Client.Changes.GetPatch(chCtx.Context, chCtx.ChangeID, chCtx.Revision, nil)
		if err != nil {
			errStr := err.Error()
			if (strings.Contains(errStr, "404") || strings.Contains(errStr, "Not Found")) && chCtx.Revision != "current" && chCtx.Revision != "" {
				return fmt.Errorf("error getting patch for change %s (patchset %s): patchset not found (HTTP 404).\n\nTo view the change and available patchsets, run:\n  gh pr view %s\n\nUnderlying error: %w", chCtx.ChangeID, chCtx.Revision, chCtx.ChangeID, err)
			}
			return chCtx.FormatError(err, "getting patch for")
		}

		if patchPtr == nil {
			fmt.Fprintln(cmd.OutOrStdout(), "No patch returned.")
			return nil
		}

		decoded, err := base64.StdEncoding.DecodeString(*patchPtr)
		rawPatch := string(decoded)
		if err != nil {
			// If it's not base64, check if it's already a raw diff
			trimmed := strings.TrimSpace(*patchPtr)
			if strings.HasPrefix(trimmed, "diff --git") || strings.HasPrefix(trimmed, "From ") || strings.HasPrefix(trimmed, "Index: ") {
				rawPatch = *patchPtr
			} else {
				return fmt.Errorf("failed to decode patch diff: %w", err)
			}
		}

		fmt.Fprintln(cmd.OutOrStdout(), formatUnifiedDiff(rawPatch, chCtx.ChangeID, diffAll, diffPaths))
		return nil
	},
}

type diffFileSection struct {
	path  string
	lines []string
}

func splitDiffSections(rawPatch string) (preamble []string, sections []diffFileSection) {
	trimmed := strings.TrimRight(rawPatch, "\n")
	if trimmed == "" {
		return nil, nil
	}
	allLines := strings.Split(trimmed, "\n")
	curIdx := -1
	for _, line := range allLines {
		if strings.HasPrefix(line, "diff --git ") {
			sections = append(sections, diffFileSection{
				path:  parseDiffGitPath(line),
				lines: []string{line},
			})
			curIdx = len(sections) - 1
			continue
		}
		if curIdx == -1 {
			preamble = append(preamble, line)
		} else {
			sections[curIdx].lines = append(sections[curIdx].lines, line)
		}
	}
	return preamble, sections
}

func parseDiffGitPath(headerLine string) string {
	// Format: diff --git a/<old> b/<new>
	rest := strings.TrimPrefix(headerLine, "diff --git ")
	if idx := strings.LastIndex(rest, " b/"); idx != -1 {
		return rest[idx+3:]
	}
	fields := strings.Fields(rest)
	if len(fields) >= 2 {
		return strings.TrimPrefix(fields[len(fields)-1], "b/")
	}
	if len(fields) == 1 {
		return strings.TrimPrefix(fields[0], "a/")
	}
	return rest
}

func matchesAnyDiffPath(filePath string, patterns []string) bool {
	base := filepath.Base(filePath)
	for _, pat := range patterns {
		pat = strings.TrimSpace(pat)
		if pat == "" {
			continue
		}
		if filePath == pat || strings.HasPrefix(filePath, strings.TrimSuffix(pat, "/")+"/") || strings.HasPrefix(filePath, pat) {
			return true
		}
		if ok, err := filepath.Match(pat, filePath); err == nil && ok {
			return true
		}
		if ok, err := filepath.Match(pat, base); err == nil && ok {
			return true
		}
	}
	return false
}

func isGeneratedOrLockfile(filePath string) bool {
	base := filepath.Base(filePath)
	switch base {
	case "MODULE.bazel.lock", "Cargo.lock", "package-lock.json", "pnpm-lock.yaml", "go.sum":
		return true
	}
	for _, suffix := range []string{".pb.go", ".pb.cc", ".pb.h", "_pb2.py"} {
		if strings.HasSuffix(base, suffix) {
			return true
		}
	}
	return false
}

func formatUnifiedDiff(rawPatch string, changeID string, includeAll bool, pathPatterns []string) string {
	preamble, sections := splitDiffSections(rawPatch)
	if len(pathPatterns) > 0 {
		var matched []string
		for _, sec := range sections {
			if matchesAnyDiffPath(sec.path, pathPatterns) {
				matched = append(matched, strings.Join(sec.lines, "\n"))
			}
		}
		return strings.Join(matched, "\n")
	}

	trimmed := strings.TrimRight(rawPatch, "\n")
	if trimmed == "" || includeAll || len(sections) == 0 {
		return rawPatch
	}
	totalLines := strings.Count(trimmed, "\n") + 1
	if totalLines <= defaultDiffMaxLines {
		return rawPatch
	}

	var out []string
	if preambleText := strings.TrimRight(strings.Join(preamble, "\n"), "\n"); strings.TrimSpace(preambleText) != "" {
		out = append(out, preambleText, "")
	}
	out = append(out, fmt.Sprintf("Files changed: %d (%d diff lines)", len(sections), totalLines), "")

	usedLines := 0
	emittedNonGenFiles := 0
	var omittedFiles []string
	omittedLines := 0

	for _, sec := range sections {
		secLineCount := len(sec.lines)
		if isGeneratedOrLockfile(sec.path) {
			out = append(out, fmt.Sprintf("# [folded generated/lockfile %s (%d lines) — use --all or --path %s to view]", sec.path, secLineCount, sec.path))
			continue
		}

		if len(omittedFiles) > 0 {
			omittedFiles = append(omittedFiles, sec.path)
			omittedLines += secLineCount
			continue
		}

		if usedLines+secLineCount <= defaultDiffMaxLines {
			out = append(out, strings.Join(sec.lines, "\n"))
			usedLines += secLineCount
			emittedNonGenFiles++
			continue
		}

		if emittedNonGenFiles == 0 {
			// Always emit at least the first file section truncated to defaultDiffMaxLines.
			out = append(out, strings.Join(sec.lines[:defaultDiffMaxLines], "\n"))
			usedLines = defaultDiffMaxLines
			emittedNonGenFiles++
			omittedFiles = append(omittedFiles, sec.path)
			omittedLines += secLineCount - defaultDiffMaxLines
			continue
		}

		omittedFiles = append(omittedFiles, sec.path)
		omittedLines += secLineCount
	}

	if len(omittedFiles) > 0 {
		previewNames := omittedFiles
		suffix := ""
		if len(previewNames) > 5 {
			previewNames = previewNames[:5]
			suffix = ", ..."
		}
		out = append(out, fmt.Sprintf("\nOmitted files: %s%s", strings.Join(previewNames, ", "), suffix))
		out = append(out, fmt.Sprintf("… %d more file(s) (%d lines omitted) · run 'gh pr diff %s --all' or 'gh pr diff %s --path <file>' to view full patch (or 'gh pr checkout %s' / 'gh pr cherry-pick %s')", len(omittedFiles), omittedLines, changeID, changeID, changeID, changeID))
	}

	return strings.Join(out, "\n")
}

func init() {
	diffCmd.Flags().BoolVar(&diffNameOnly, "name-only", false, "Show only names of changed files")
	diffCmd.Flags().BoolVar(&diffStat, "stat", false, "Show diffstat of changed files")
	//ghish:flag mode=extension reason="Show full untruncated diff across all files"
	diffCmd.Flags().BoolVar(&diffAll, "all", false, "Show full untruncated diff across all files (ghish-only)")
	//ghish:flag mode=extension reason="Filter diff to matching file path or glob"
	diffCmd.Flags().StringSliceVar(&diffPaths, "path", nil, "Filter diff to matching file path or glob (ghish-only)")
	PrCmd.AddCommand(diffCmd)
}
