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
	"sort"
	"strings"

	"github.com/andygrunwald/go-gerrit"
	"github.com/spf13/cobra"
)

var (
	diffNameOnly bool
	diffStat     bool
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
	sort.Strings(paths)

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
	Use:   "diff [<id>[/<patchset>]]",
	Short: "View changes as a diff",
	Args:  cobra.MaximumNArgs(1),
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
				sort.Strings(paths)
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
		if err != nil {
			// If it's not base64, check if it's already a raw diff
			trimmed := strings.TrimSpace(*patchPtr)
			if strings.HasPrefix(trimmed, "diff --git") || strings.HasPrefix(trimmed, "From ") || strings.HasPrefix(trimmed, "Index: ") {
				fmt.Fprintln(cmd.OutOrStdout(), *patchPtr)
				return nil
			}
			return fmt.Errorf("failed to decode patch diff: %w", err)
		}

		fmt.Fprintln(cmd.OutOrStdout(), string(decoded))
		return nil
	},
}

func init() {
	diffCmd.Flags().BoolVar(&diffNameOnly, "name-only", false, "Show only names of changed files")
	diffCmd.Flags().BoolVar(&diffStat, "stat", false, "Show diffstat of changed files")
	PrCmd.AddCommand(diffCmd)
}
