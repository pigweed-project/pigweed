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
	"encoding/json"
	"fmt"
	"strings"
	"text/tabwriter"
	"time"

	"github.com/spf13/cobra"
)

var (
	runListJSON          bool
	runListExperimental  bool
	runViewLogFailed     bool
	runViewLog           bool
	runViewVerbose       bool
	runViewJob           string
	runViewWeb           bool
	runViewJSON          bool
	runViewExperimental  bool
	runRerunFailed       bool
	runRerunJob          string
	runRerunDryRun       bool
	runRerunExperimental bool
)

func isBuildbucketID(s string) bool {
	if len(s) < 10 {
		return false
	}
	for _, c := range s {
		if c < '0' || c > '9' {
			return false
		}
	}
	return true
}

// parseRunTargetArgs resolves target arguments for 'run view' and 'run rerun'.
// It handles 0, 1, or 2 positional arguments where a single argument may be a
// Buildbucket build ID (if allowBuildID is true), a Gerrit change identifier,
// or a builder name.
func parseRunTargetArgs(cmd *cobra.Command, args []string, jobFlag string, allowBuildID bool) (rawID, targetBuilder, directBuildID string, err error) {
	if len(args) == 0 {
		id, err := ResolveTargetChangeID(cmd.Context(), cmd, nil)
		if err != nil {
			return "", "", "", err
		}
		return id, jobFlag, "", nil
	}
	if len(args) == 1 {
		arg := args[0]
		if allowBuildID && isBuildbucketID(arg) {
			return "", "", arg, nil
		}
		if isChangeIdentifier(arg) {
			return arg, jobFlag, "", nil
		}
		id, err := ResolveTargetChangeID(cmd.Context(), cmd, nil)
		if err != nil {
			return "", "", "", err
		}
		targetBuilder = arg
		if jobFlag != "" {
			targetBuilder = jobFlag
		}
		return id, targetBuilder, "", nil
	}
	rawID = args[0]
	targetBuilder = args[1]
	if jobFlag != "" {
		targetBuilder = jobFlag
	}
	return rawID, targetBuilder, "", nil
}

// collectFailedBuilders returns deduplicated failed builder names from the latest builds.
func collectFailedBuilders(builds []bbBuild, includeExperimental bool) []string {
	var failed []string
	seen := make(map[string]bool)
	for _, b := range deduplicateLatestBuilds(builds) {
		if b.Status == "FAILURE" || b.Status == "INFRA_FAILURE" {
			if !includeExperimental && b.IsExperimental() {
				continue
			}
			if !seen[b.Builder.Builder] {
				seen[b.Builder.Builder] = true
				failed = append(failed, b.Builder.Builder)
			}
		}
	}
	return failed
}

// RunCmd is the top-level command for managing CI/CD runs (from Buildbucket).
var RunCmd = &cobra.Command{
	Use:          "run",
	Short:        "View and manage CI/CD workflow runs (Buildbucket)",
	SilenceUsage: true,
	Long: `View and manage CI/CD workflow runs and tryjob builds executed on Buildbucket.

Commands:
  list    List recent builds for a change
  view    View build details, step trees (-v), and failure logs (--log-failed)
  rerun   Rerun failed checks (--failed) or specific builders (-j <builder>)
  watch   Watch checks until completion`,
}

var runListCmd = &cobra.Command{
	Use:          "list [<id>[/<patchset>]]",
	Short:        "List recent CI/CD workflow runs for a change",
	SilenceUsage: true,
	Args:         cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		rawID, err := ResolveTargetChangeID(cmd.Context(), cmd, args)
		if err != nil {
			return err
		}
		res, err := ResolveCIContext(cmd, rawID)
		if err != nil {
			return fmt.Errorf("failed to load runs for %q: %w", rawID, err)
		}

		relevant, omittedExp := FilterExperimentalBuilds(deduplicateLatestBuilds(res.Builds), runListExperimental)

		if len(relevant) == 0 {
			fmt.Fprintf(cmd.OutOrStdout(), "No checks scheduled for Change %d (Patchset %d).\n", res.Change.Number, res.PatchsetNum)
			return nil
		}

		if runListJSON {
			items := BuildCheckItems(relevant)
			data, err := json.MarshalIndent(items, "", "  ")
			if err != nil {
				return fmt.Errorf("failed to marshal JSON: %w", err)
			}
			fmt.Fprintln(cmd.OutOrStdout(), string(data))
			return nil
		}

		fmt.Fprintf(cmd.OutOrStdout(), "Showing %d checks for Change %d (Patchset %d) • %s\n\n", len(relevant), res.Change.Number, res.PatchsetNum, res.Change.Subject)

		w := tabwriter.NewWriter(cmd.OutOrStdout(), 0, 0, 2, ' ', 0)
		fmt.Fprintln(w, "STATUS\tBUILDER\tDURATION\tID\tURL")
		for _, b := range relevant {
			sym := getStatusSymbol(b.Status)
			dur := formatDuration(b.StartTime, b.EndTime)
			urlStr := fmt.Sprintf("https://ci.chromium.org/b/%s", b.ID)
			fmt.Fprintf(w, "%s\t%s\t%s\t%s\t%s\n", sym, b.Builder.Builder, dur, b.ID, urlStr)
		}
		w.Flush()

		if notice := FormatOmittedExperimentalNotice(omittedExp); notice != "" {
			fmt.Fprintf(cmd.OutOrStdout(), "\n%s\n", notice)
		}
		return nil
	},
}

var runViewCmd = &cobra.Command{
	Use:          "view [<id> | <builder>] [flags]",
	Short:        "View details, step trees, or logs for a CI/CD build",
	SilenceUsage: true,
	Args:         cobra.MaximumNArgs(2),
	RunE: func(cmd *cobra.Command, args []string) error {
		rawID, targetBuilder, directBuildID, err := parseRunTargetArgs(cmd, args, runViewJob, true)
		if err != nil {
			return err
		}

		ctx := cmd.Context()
		luciClient := NewLUCIClient(buildbucketHost, getLUCIHTTPClient(ctx, buildbucketHost))

		var res *CIContext
		if directBuildID == "" {
			r, err := ResolveCIContext(cmd, rawID)
			if err != nil {
				return fmt.Errorf("failed to load checks for %q: %w", rawID, err)
			}
			res = r
		}

		// Handle -w, --web
		if runViewWeb {
			targetURL := ""
			if directBuildID != "" {
				targetURL = fmt.Sprintf("https://ci.chromium.org/b/%s", directBuildID)
			} else if targetBuilder != "" {
				for _, b := range res.Builds {
					if strings.EqualFold(b.Builder.Builder, targetBuilder) {
						targetURL = fmt.Sprintf("https://ci.chromium.org/b/%s", b.ID)
						break
					}
				}
				if targetURL == "" {
					return fmt.Errorf("builder %q not found on change %s", targetBuilder, rawID)
				}
			} else {
				targetURL = fmt.Sprintf("https://%s/c/%s/+/%d", res.GerritHost, res.Change.Project, res.Change.Number)
			}
			fmt.Fprintf(cmd.OutOrStdout(), "Opening %s in your browser.\n", targetURL)
			return OpenBrowserFn(targetURL)
		}

		// Handle logs (--log-failed or --log)
		if runViewLogFailed || runViewLog {
			var targetBuilds []bbBuild
			if directBuildID != "" {
				details, err := luciClient.GetBuildDetails(ctx, directBuildID)
				if err != nil {
					return fmt.Errorf("failed to fetch build %s: %w", directBuildID, err)
				}
				targetBuilds = append(targetBuilds, bbBuild{
					ID:      details.ID,
					Builder: details.Builder,
					Status:  details.Status,
				})
			} else if targetBuilder != "" {
				var matching []bbBuild
				for _, b := range res.Builds {
					if strings.EqualFold(b.Builder.Builder, targetBuilder) {
						matching = append(matching, b)
					}
				}
				if len(matching) > 0 {
					chosen := matching[0]
					for _, b := range matching {
						if b.Status == "FAILURE" || b.Status == "INFRA_FAILURE" {
							chosen = b
							break
						}
					}
					targetBuilds = append(targetBuilds, chosen)
				}
			} else {
				for _, b := range deduplicateLatestBuilds(res.Builds) {
					if b.Status == "FAILURE" || b.Status == "INFRA_FAILURE" {
						if !runViewExperimental && b.IsExperimental() {
							continue
						}
						targetBuilds = append(targetBuilds, b)
					}
				}
			}

			if len(targetBuilds) == 0 {
				if targetBuilder != "" {
					var available []string
					seen := make(map[string]bool)
					for _, b := range res.Builds {
						if !seen[b.Builder.Builder] && b.Builder.Builder != "" {
							seen[b.Builder.Builder] = true
							available = append(available, b.Builder.Builder)
						}
					}
					if len(available) > 0 {
						return fmt.Errorf("no check found matching builder %q on change %s.\n\nAvailable builders on this change:\n  - %s\n\nRun 'gh run list' to view all checks",
							targetBuilder, rawID, strings.Join(available, "\n  - "))
					}
					return fmt.Errorf("no check found matching builder %q on change %s (no checks found on this change).\n\nRun 'gh run list' to view check status", targetBuilder, rawID)
				}
				failedExpCount := 0
				seenExp := make(map[string]bool)
				for _, b := range res.Builds {
					if (b.Status == "FAILURE" || b.Status == "INFRA_FAILURE") && b.IsExperimental() && !seenExp[b.Builder.Builder] {
						seenExp[b.Builder.Builder] = true
						failedExpCount++
					}
				}
				if notice := FormatOmittedExperimentalNotice(failedExpCount); notice != "" {
					fmt.Fprintf(cmd.OutOrStdout(), "No failed blocking checks found on this change %s.\n", notice)
					return nil
				}
				fmt.Fprintln(cmd.OutOrStdout(), "No failed checks found on this change.")
				return nil
			}

			maxLines := 100
			if runViewLog {
				maxLines = 0
			}

			var reports []FailureReport
			for _, b := range targetBuilds {
				details, err := luciClient.GetBuildDetails(ctx, b.ID)
				if err != nil {
					fmt.Fprintf(cmd.ErrOrStderr(), "Warning: failed to get details for build %s: %v\n", b.ID, err)
					continue
				}
				rep := luciClient.ExtractFailureReport(ctx, details, maxLines)
				if rep != nil {
					reports = append(reports, *rep)
				}
			}

			if runViewJSON {
				data, _ := json.MarshalIndent(reports, "", "  ")
				fmt.Fprintln(cmd.OutOrStdout(), string(data))
				return nil
			}

			if len(reports) == 0 {
				for _, b := range targetBuilds {
					if b.Status == "STARTED" || b.Status == "SCHEDULED" {
						fmt.Fprintf(cmd.OutOrStdout(), "Check %q is currently running (status: %s).\nTo monitor progress, run:\n  gh run view -j %s -v\n", b.Builder.Builder, b.Status, b.Builder.Builder)
						continue
					}
					dur := formatDuration(b.StartTime, b.EndTime)
					fmt.Fprintf(cmd.OutOrStdout(), "Check %q has status %s (%s).\nBuild details: https://ci.chromium.org/b/%s\n", b.Builder.Builder, b.Status, dur, b.ID)
				}
				return nil
			}

			fmt.Fprint(cmd.OutOrStdout(), FormatFailureReports(reports))
			return nil
		}

		// Handle job step tree: if targeted by -j, positional builder, direct build ID, or -v
		if directBuildID != "" || targetBuilder != "" || runViewVerbose {
			var bID string
			var bName string
			if directBuildID != "" {
				bID = directBuildID
			} else if targetBuilder != "" {
				for _, b := range res.Builds {
					if strings.EqualFold(b.Builder.Builder, targetBuilder) {
						bID = b.ID
						bName = b.Builder.Builder
						break
					}
				}
				if bID == "" {
					var available []string
					for _, b := range deduplicateLatestBuilds(res.Builds) {
						if b.Builder.Builder != "" {
							available = append(available, b.Builder.Builder)
						}
					}
					sortMsg := ""
					if len(available) > 0 {
						sortMsg = fmt.Sprintf("\n\nAvailable checks on Change %d (Patchset %d):\n  - %s", res.Change.Number, res.PatchsetNum, strings.Join(available, "\n  - "))
					}
					return fmt.Errorf("check %q not found on change %s%s", targetBuilder, rawID, sortMsg)
				}
			} else {
				// Pick first build or failed build
				for _, b := range res.Builds {
					if b.Status == "FAILURE" || b.Status == "INFRA_FAILURE" {
						bID = b.ID
						bName = b.Builder.Builder
						break
					}
				}
				if bID == "" && len(res.Builds) > 0 {
					bID = res.Builds[0].ID
					bName = res.Builds[0].Builder.Builder
				}
				if bID == "" {
					return fmt.Errorf("no checks found on change %s", rawID)
				}
			}

			details, err := luciClient.GetBuildDetails(ctx, bID)
			if err != nil {
				return fmt.Errorf("failed to fetch steps for build %s: %w", bID, err)
			}
			if bName == "" && details.Builder.Builder != "" {
				bName = details.Builder.Builder
			}
			if runViewJSON {
				data, err := json.MarshalIndent(details, "", "  ")
				if err != nil {
					return fmt.Errorf("failed to marshal steps JSON: %w", err)
				}
				fmt.Fprintln(cmd.OutOrStdout(), string(data))
				return nil
			}
			fmt.Fprint(cmd.OutOrStdout(), FormatBuildStepsVerbose(details, runViewVerbose))
			return nil
		}

		// Default view: structured summary of the whole workflow run
		relevant, omittedExp := FilterExperimentalBuilds(deduplicateLatestBuilds(res.Builds), runViewExperimental)
		hasFailure := false
		hasRunning := false
		passedCount := 0
		failedCount := 0

		for _, b := range relevant {
			if b.Status == "FAILURE" || b.Status == "INFRA_FAILURE" {
				hasFailure = true
				failedCount++
			} else if b.Status == "STARTED" || b.Status == "SCHEDULED" {
				hasRunning = true
			} else if b.Status == "SUCCESS" {
				passedCount++
			}
		}

		if len(relevant) == 0 {
			fmt.Fprintf(cmd.OutOrStdout(), "No checks scheduled for Change %d (Patchset %d).\n", res.Change.Number, res.PatchsetNum)
			return nil
		}

		overallSymbol := "✓"
		overallStatus := "SUCCESS"
		if hasFailure {
			overallSymbol = "✗"
			overallStatus = "FAILURE"
		} else if hasRunning {
			overallSymbol = "*"
			overallStatus = "RUNNING"
		}

		changeURL := fmt.Sprintf("https://%s/c/%s/+/%d", res.GerritHost, res.Change.Project, res.Change.Number)
		if res.PatchsetNum > 0 {
			changeURL = fmt.Sprintf("%s/%d", changeURL, res.PatchsetNum)
		}

		if runViewJSON {
			checkItems := BuildCheckItems(relevant)
			summary := map[string]any{
				"change":   res.Change.Number,
				"patchset": res.PatchsetNum,
				"subject":  res.Change.Subject,
				"branch":   res.Change.Branch,
				"status":   overallStatus,
				"url":      changeURL,
				"total":    len(relevant),
				"passed":   passedCount,
				"failed":   failedCount,
				"jobs":     checkItems,
			}
			data, err := json.MarshalIndent(summary, "", "  ")
			if err != nil {
				return fmt.Errorf("failed to marshal run summary JSON: %w", err)
			}
			fmt.Fprintln(cmd.OutOrStdout(), string(data))
			return nil
		}

		fmt.Fprintf(cmd.OutOrStdout(), "%s Change %d (Patchset %d) • %s\n", overallSymbol, res.Change.Number, res.PatchsetNum, res.Change.Subject)
		if res.Change.Branch != "" {
			fmt.Fprintf(cmd.OutOrStdout(), "Branch:  %s\n", res.Change.Branch)
		}
		fmt.Fprintf(cmd.OutOrStdout(), "URL:     %s\n\n", changeURL)
		fmt.Fprintln(cmd.OutOrStdout(), "JOBS")

		w := tabwriter.NewWriter(cmd.OutOrStdout(), 0, 0, 2, ' ', 0)
		for _, b := range relevant {
			sym := getStatusSymbol(b.Status)
			dur := formatDuration(b.StartTime, b.EndTime)
			fmt.Fprintf(w, "%s\t%s\tin %s\t(ID %s)\n", sym, b.Builder.Builder, dur, b.ID)
		}
		w.Flush()

		if notice := FormatOmittedExperimentalNotice(omittedExp); notice != "" {
			fmt.Fprintf(cmd.OutOrStdout(), "\n%s\n", notice)
		}

		fmt.Fprintln(cmd.OutOrStdout())
		fmt.Fprintln(cmd.OutOrStdout(), "To view failed logs:   gh run view --log-failed")
		fmt.Fprintln(cmd.OutOrStdout(), "To view step tree:     gh run view -j <builder>")
		if hasFailure {
			fmt.Fprintln(cmd.OutOrStdout(), "To rerun failed:       gh run rerun --failed")
		}
		fmt.Fprintln(cmd.OutOrStdout(), "To view in browser:    gh run view -w")
		return nil
	},
}

var runRerunCmd = &cobra.Command{
	Use:          "rerun [<id> | <builder>] [flags]",
	Short:        "Rerun CI/CD checks for a change",
	SilenceUsage: true,
	Args:         cobra.MaximumNArgs(2),
	RunE: func(cmd *cobra.Command, args []string) error {
		rawID, targetBuilder, _, err := parseRunTargetArgs(cmd, args, runRerunJob, false)
		if err != nil {
			return err
		}

		res, err := ResolveCIContext(cmd, rawID)
		if err != nil {
			return fmt.Errorf("failed to load checks for %q: %w", rawID, err)
		}

		var buildersToRerun []string
		if targetBuilder != "" {
			found := false
			for _, b := range res.Builds {
				if strings.EqualFold(b.Builder.Builder, targetBuilder) {
					found = true
					buildersToRerun = append(buildersToRerun, b.Builder.Builder)
					break
				}
			}
			if !found {
				var available []string
				for _, b := range deduplicateLatestBuilds(res.Builds) {
					if b.Builder.Builder != "" {
						available = append(available, b.Builder.Builder)
					}
				}
				sortMsg := ""
				if len(available) > 0 {
					sortMsg = fmt.Sprintf("\n\nAvailable checks on Change %d (Patchset %d):\n  - %s", res.Change.Number, res.PatchsetNum, strings.Join(available, "\n  - "))
				}
				return fmt.Errorf("builder %q not found on change %s%s", targetBuilder, rawID, sortMsg)
			}
		} else if runRerunFailed {
			buildersToRerun = collectFailedBuilders(res.Builds, runRerunExperimental)
			if len(buildersToRerun) == 0 {
				fmt.Fprintln(cmd.OutOrStdout(), "No failed checks found to rerun.")
				return nil
			}
		} else {
			failedBuilders := collectFailedBuilders(res.Builds, runRerunExperimental)
			if len(failedBuilders) > 0 {
				return fmt.Errorf("no builder specified to rerun: specify a builder name or pass --failed to rerun all failed checks.\n\nUsage examples:\n  gh run rerun --failed\n  gh run rerun <builder-name>\n  gh run rerun -j <builder-name>\n\nFailed checks available to rerun:\n  - %s", strings.Join(failedBuilders, "\n  - "))
			}
			return fmt.Errorf("no builder specified to rerun and no failed checks found on this change: specify a builder name or pass --failed.\n\nUsage:\n  gh run rerun <builder-name>\n\nRun 'gh pr checks' to view check status")
		}

		chRef := GerritChangeRef{
			Host:     res.GerritHost,
			Project:  res.Change.Project,
			ChangeID: res.Change.Number,
			Patchset: res.PatchsetNum,
		}

		ctx := cmd.Context()
		for _, bName := range buildersToRerun {
			if runRerunDryRun {
				cmdStr := res.Profile.FormatRerunCommand(chRef, bName)
				fmt.Fprintf(cmd.OutOrStdout(), "[dry-run] %s\n", cmdStr)
				continue
			}

			fmt.Fprintf(cmd.OutOrStdout(), "Rerunning check: %s...\n", bName)
			if err := res.Profile.RerunCheck(ctx, chRef, bName, cmd.OutOrStdout(), cmd.ErrOrStderr()); err != nil {
				return fmt.Errorf("failed to rerun %s: %w", bName, err)
			}
		}
		return nil
	},
}

var runWatchCmd = &cobra.Command{
	Use:          "watch [<id>[/<patchset>]]",
	Short:        "Watch checks until they finish",
	SilenceUsage: true,
	Args:         cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		checksWatch = true
		return checksCmd.RunE(cmd, args)
	},
}

func init() {
	runListCmd.Flags().BoolVar(&runListJSON, "json", false, "Output JSON with specified fields")
	runListCmd.Flags().BoolVarP(&runListExperimental, "experimental", "e", false, "Include non-blocking experimental checks")
	runListCmd.Flags().StringVar(&buildbucketHost, "buildbucket-host", "cr-buildbucket.appspot.com", "Buildbucket host to query")
	runListCmd.Flags().MarkHidden("buildbucket-host")

	runViewCmd.Flags().BoolVar(&runViewLogFailed, "log-failed", false, "Output log snippet and diagnostics for failed checks")
	runViewCmd.Flags().BoolVar(&runViewLog, "log", false, "Output full log for check")
	runViewCmd.Flags().BoolVarP(&runViewVerbose, "verbose", "v", false, "Show step execution tree")
	runViewCmd.Flags().StringVarP(&runViewJob, "job", "j", "", "View a specific builder by name")
	runViewCmd.Flags().BoolVarP(&runViewWeb, "web", "w", false, "Open check in web browser")
	runViewCmd.Flags().BoolVar(&runViewJSON, "json", false, "Output in JSON format")
	runViewCmd.Flags().BoolVarP(&runViewExperimental, "experimental", "e", false, "Include non-blocking experimental checks")
	runViewCmd.Flags().StringVar(&buildbucketHost, "buildbucket-host", "cr-buildbucket.appspot.com", "Buildbucket host to query")
	runViewCmd.Flags().MarkHidden("buildbucket-host")

	runRerunCmd.Flags().BoolVar(&runRerunFailed, "failed", false, "Rerun all failed builders on the change")
	runRerunCmd.Flags().StringVarP(&runRerunJob, "job", "j", "", "Rerun a specific builder by name")
	runRerunCmd.Flags().BoolVar(&runRerunDryRun, "dry-run", false, "Print the rerun command without executing it")
	runRerunCmd.Flags().BoolVarP(&runRerunExperimental, "experimental", "e", false, "Include non-blocking experimental checks")
	runRerunCmd.Flags().StringVar(&buildbucketHost, "buildbucket-host", "cr-buildbucket.appspot.com", "Buildbucket host to query")
	runRerunCmd.Flags().MarkHidden("buildbucket-host")

	runWatchCmd.Flags().DurationVarP(&checksInterval, "interval", "i", 15*time.Second, "Refresh interval")
	runWatchCmd.Flags().BoolVar(&checksFailFast, "fail-fast", false, "Exit immediately if any check fails")
	runWatchCmd.Flags().BoolVarP(&checksExperimental, "experimental", "e", false, "Include non-blocking experimental checks")
	runWatchCmd.Flags().StringVar(&buildbucketHost, "buildbucket-host", "cr-buildbucket.appspot.com", "Buildbucket host to query")
	runWatchCmd.Flags().MarkHidden("buildbucket-host")

	RunCmd.AddCommand(runListCmd)
	RunCmd.AddCommand(runViewCmd)
	RunCmd.AddCommand(runRerunCmd)
	RunCmd.AddCommand(runWatchCmd)

	RootCmd.AddCommand(RunCmd)
}
