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
	"io"
	"strings"
	"sync"
	"text/tabwriter"
	"time"

	"github.com/spf13/cobra"
)

var (
	runListJSON          bool
	runListExperimental  bool
	runListAll           bool
	runViewLogFailed     bool
	runViewLog           bool
	runViewVerbose       bool
	runViewJob           string
	runViewTarget        string
	runViewWeb           bool
	runViewJSON          bool
	runViewExperimental  bool
	runViewAll           bool
	runRerunFailed       bool
	runRerunJob          string
	runRerunDryRun       bool
	runRerunExperimental bool
)

func isBuildbucketID(s string) bool {
	if len(s) < 11 {
		return false
	}
	return isDigitsOnly(s)
}

func isDirectBuildIdentifier(s string) bool {
	if isBuildbucketID(s) || IsBusytownBuildID(s) {
		return true
	}
	if strings.TrimSpace(runViewTarget) != "" && isDigitsOnly(strings.TrimSpace(s)) {
		return true
	}
	return false
}

// parseRunTargetArgs resolves target arguments for 'run view' and 'run rerun'.
// It handles 0, 1, or 2 positional arguments where a single argument (or -j flag)
// may be a Buildbucket/Busytown build ID (if allowBuildID is true), a Gerrit change identifier,
// or a builder name.
func parseRunTargetArgs(cmd *cobra.Command, args []string, jobFlag string, allowBuildID bool) (rawID, targetBuilder, directBuildID string, err error) {
	if allowBuildID && isDirectBuildIdentifier(jobFlag) {
		return "", "", jobFlag, nil
	}
	if len(args) == 0 {
		id, err := ResolveTargetChangeID(cmd.Context(), cmd, nil)
		if err != nil {
			return "", "", "", err
		}
		return id, jobFlag, "", nil
	}
	if len(args) == 1 {
		arg := args[0]
		if allowBuildID && isDirectBuildIdentifier(arg) {
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
	if allowBuildID && isDirectBuildIdentifier(targetBuilder) {
		return "", "", targetBuilder, nil
	}
	return rawID, targetBuilder, "", nil
}

// buildMatchesJob checks whether a Buildbucket or Busytown build matches a user-supplied
// job selector, which may be "<builder>", "<bucket>/<builder>", or
// "<project>/<bucket>/<builder>" (case-insensitive).
func buildMatchesJob(b bbBuild, job string) bool {
	if strings.EqualFold(b.Builder.Builder, job) || (b.Target != "" && strings.EqualFold(b.Target, job)) {
		return true
	}
	if b.Builder.Bucket != "" && strings.EqualFold(b.Builder.Bucket+"/"+b.Builder.Builder, job) {
		return true
	}
	if b.Builder.Project != "" && b.Builder.Bucket != "" && strings.EqualFold(b.Builder.Project+"/"+b.Builder.Bucket+"/"+b.Builder.Builder, job) {
		return true
	}
	return false
}

// collectFailedBuilds returns deduplicated failed builds from the latest builds,
// skipping any builds that match skipRetryFilters (defaulting to ["skip-retry-in-gerrit:subbuild"]).
func collectFailedBuilds(builds []bbBuild, includeExperimental bool, skipRetryFilters ...string) []bbBuild {
	if len(skipRetryFilters) == 0 {
		skipRetryFilters = []string{"skip-retry-in-gerrit:subbuild"}
	}
	var failed []bbBuild
	for _, b := range deduplicateLatestBuilds(builds) {
		if b.Status == "FAILURE" || b.Status == "INFRA_FAILURE" {
			if !includeExperimental && b.IsExperimental() {
				continue
			}
			if b.MatchesTagFilters(skipRetryFilters) {
				continue
			}
			failed = append(failed, b)
		}
	}
	return dedupBy(failed, func(b bbBuild) string {
		return b.Builder.Project + "/" + b.Builder.Bucket + "/" + b.Builder.Builder
	})
}

// collectFailedBuilders returns deduplicated failed builder names from the latest builds.
func collectFailedBuilders(builds []bbBuild, includeExperimental bool, skipRetryFilters ...string) []string {
	var failed []string
	for _, b := range collectFailedBuilds(builds, includeExperimental, skipRetryFilters...) {
		failed = append(failed, b.Builder.Builder)
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
	Annotations:  map[string]string{KindAnnotation: KindList},
	Short:        "List recent CI/CD workflow runs for a change",
	SilenceUsage: true,
	Args:         cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		projCfg, err := LoadCommandProjectConfig(cmd)
		if err != nil {
			return err
		}

		rawID, err := ResolveTargetChangeID(ctx, cmd, args)
		if err != nil {
			return err
		}
		res, err := ResolveCIContext(cmd, rawID)
		if err != nil {
			return fmt.Errorf("failed to load runs for %q: %w", rawID, err)
		}

		hideFilters := projCfg.CI.HideTagFilters
		if runListAll {
			hideFilters = nil
		}
		filteredBuilds, hiddenCount := FilterBuildsByTags(deduplicateLatestBuilds(res.Builds), hideFilters)
		relevant, omittedExp := FilterExperimentalBuilds(filteredBuilds, runListExperimental)

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

		writeRunListTable(cmd.OutOrStdout(), relevant, res.PatchsetNum, runListAll || VerboseFlag)

		if notice := FormatOmittedExperimentalNotice(omittedExp); notice != "" {
			fmt.Fprintf(cmd.OutOrStdout(), "\n%s\n", notice)
		}
		if notice := FormatHiddenByConfigNotice(hiddenCount, projCfg.CI.HideTagFilters); notice != "" {
			fmt.Fprintf(cmd.OutOrStdout(), "\n%s\n", notice)
		}
		writeRunVerdictFooter(cmd.OutOrStdout(), res.Change.Number, relevant)
		return nil
	},
}

// writeRunListTable renders the `run list` STATUS/BUILDER/DURATION/ID/URL
// table, ordering failures first and collapsing passing builds beyond
// maxExpandedPassingChecks unless showAllPassing is set.
func writeRunListTable(out io.Writer, relevant []bbBuild, patchsetNum int, showAllPassing bool) {
	visible, omittedLine := selectVisibleBuilds(relevant, showAllPassing)
	if len(visible) > 0 {
		w := tabwriter.NewWriter(out, 0, 0, 2, ' ', 0)
		fmt.Fprintln(w, "STATUS\tBUILDER\tDURATION\tID\tURL")
		for _, b := range visible {
			fmt.Fprintf(w, "%s\t%s\t%s\t%s\t%s%s\n",
				getStatusSymbol(b.Status), b.Builder.Builder, formatDuration(b.StartTime, b.EndTime), b.ID, BuildURL(b), patchsetNote(b.Patchset, patchsetNum))
		}
		w.Flush()
	}
	if omittedLine != "" {
		fmt.Fprintf(out, "  %s  %s\n", GlyphPass, omittedLine)
	}
}

// ciVerdict tallies one patchset's blocking checks for the Verdict:/Next:
// footer shared by `run list`, `run view`, and `pr checks`.
type ciVerdict struct {
	total    int
	failed   int
	canceled int
	running  int
	passed   int
	// failedGates lists Gerrit gates (submit requirements or automated
	// labels) that failed; they only affect the verdict when no check failed.
	failedGates []string
	// experimental counts non-blocking builders; it only explains an empty
	// result.
	experimental int
	// pendingNext overrides the Next: command for a PENDING verdict; the
	// default is `gh run watch <id>`.
	pendingNext string
}

// tallyCIVerdict classifies builds with classifyCheckStatus, so the verdict
// fails closed exactly like the `gh pr checks` exit code. Experimental
// builders are non-blocking: like `pr checks`, they are counted separately
// and never affect the verdict, even when shown with --experimental.
func tallyCIVerdict(builds []bbBuild) ciVerdict {
	var v ciVerdict
	for _, b := range builds {
		if b.IsExperimental() {
			v.experimental++
			continue
		}
		v.total++
		switch classifyCheckStatus(b.Status) {
		case checkFailed:
			v.failed++
		case checkCanceled:
			v.canceled++
		case checkPending:
			v.running++
		case checkPassed:
			v.passed++
		}
	}
	return v
}

// writeRunVerdictFooter writes the shared CI Verdict:/Next: footer for the
// given builds.
func writeRunVerdictFooter(out io.Writer, changeNum int, relevant []bbBuild) {
	writeCIVerdictFooter(out, changeNum, tallyCIVerdict(relevant))
}

// writeCIVerdictFooter writes a blank line followed by a one-line Verdict:
// and the single most useful Next: command. Precedence (failed, failed gates,
// canceled, running, none, passed) matches checksExitStatus.
func writeCIVerdictFooter(out io.Writer, changeNum int, v ciVerdict) {
	fmt.Fprintln(out)
	switch {
	case v.failed > 0:
		fmt.Fprintf(out, "Verdict: FAILED (%d of %d checks failed)\n", v.failed, v.total)
		fmt.Fprintf(out, "Next:    gh run view %d --log-failed  (or gh run rerun %d --failed)\n", changeNum, changeNum)
	case len(v.failedGates) > 0:
		fmt.Fprintf(out, "Verdict: FAILED (Gerrit gates failed: %s)\n", strings.Join(v.failedGates, ", "))
		fmt.Fprintf(out, "Next:    gh pr view %d --comments\n", changeNum)
	case v.canceled > 0:
		fmt.Fprintf(out, "Verdict: CANCELED (%d of %d checks canceled, likely superseded by a newer patchset)\n", v.canceled, v.total)
		fmt.Fprintf(out, "Next:    gh pr checks %d  (or gh pr edit %d --cq)\n", changeNum, changeNum)
	case v.running > 0:
		next := v.pendingNext
		if next == "" {
			next = fmt.Sprintf("gh run watch %d", changeNum)
		}
		fmt.Fprintf(out, "Verdict: PENDING (%d running, %d passed)\n", v.running, v.passed)
		fmt.Fprintf(out, "Next:    %s\n", next)
	case v.total == 0 && v.experimental > 0:
		fmt.Fprintf(out, "Verdict: NO BLOCKING CHECKS (%d non-blocking experimental builders ran)\n", v.experimental)
		fmt.Fprintf(out, "Next:    gh pr checks %d --experimental\n", changeNum)
	case v.total == 0:
		fmt.Fprintln(out, "Verdict: NO CHECKS (CI not scheduled yet, or no tryjobs configured)")
		fmt.Fprintf(out, "Next:    gh pr edit %d --cq\n", changeNum)
	default:
		fmt.Fprintf(out, "Verdict: PASSED (%s passed)\n", countNoun(v.passed, "check"))
		fmt.Fprintf(out, "Next:    gh pr view %d\n", changeNum)
	}
}

var runViewCmd = &cobra.Command{
	Use:          "view [<id> | <builder>] [flags]",
	Annotations:  map[string]string{KindAnnotation: KindView},
	Short:        "View details, step trees, or logs for a CI/CD build",
	SilenceUsage: true,
	Args:         cobra.MaximumNArgs(2),
	RunE: func(cmd *cobra.Command, args []string) error {
		rawID, targetBuilder, directBuildID, err := parseRunTargetArgs(cmd, args, runViewJob, true)
		if err != nil {
			return err
		}

		ctx := cmd.Context()
		projCfg, err := LoadCommandProjectConfig(cmd)
		if err != nil {
			return err
		}

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
				if bid, target, ok := ParseBusytownBuildTarget(directBuildID, runViewTarget); ok && (IsBusytownBuildID(directBuildID) || runViewTarget != "") {
					targetURL = BusytownBuildURL(bid, target)
				} else {
					targetURL = fmt.Sprintf("https://ci.chromium.org/b/%s", directBuildID)
				}
			} else if targetBuilder != "" {
				for _, b := range deduplicateLatestBuilds(res.Builds) {
					if buildMatchesJob(b, targetBuilder) {
						targetURL = BuildURL(b)
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
			var directProvider CIProvider
			if directBuildID != "" {
				if bid, target, ok := ParseBusytownBuildTarget(directBuildID, runViewTarget); ok && (IsBusytownBuildID(directBuildID) || runViewTarget != "") {
					directProvider = &BusytownProvider{}
					builderName := target
					if builderName == "" {
						builderName = bid
					}
					targetBuilds = append(targetBuilds, bbBuild{
						ID:       bid,
						Provider: "busytown",
						Target:   target,
						Status:   "FAILURE",
						ViewURL:  BusytownBuildURL(bid, target),
						Builder: bbBuilder{
							Project: "android-build",
							Bucket:  "presubmit",
							Builder: builderName,
						},
					})
				} else {
					details, err := luciClient.GetBuildDetails(ctx, directBuildID)
					if err != nil {
						return fmt.Errorf("failed to fetch build %s: %w", directBuildID, err)
					}
					directProvider = &BuildbucketProvider{LUCIClient: luciClient}
					targetBuilds = append(targetBuilds, bbBuild{
						ID:       details.ID,
						Provider: "buildbucket",
						Builder:  details.Builder,
						Status:   details.Status,
					})
				}
			} else if targetBuilder != "" {
				// Report on the newest build of the builder, the same build
				// `gh pr checks` reports. res.Builds can span several
				// code-equivalent patchsets plus retries, so preferring any
				// failure would surface one that a later build superseded.
				for _, b := range deduplicateLatestBuilds(res.Builds) {
					if buildMatchesJob(b, targetBuilder) || b.ID == targetBuilder {
						targetBuilds = append(targetBuilds, b)
						break
					}
				}
			} else {
				hideFilters := projCfg.CI.HideTagFilters
				if runViewAll {
					hideFilters = nil
				}
				visibleBuilds, _ := FilterBuildsByTags(deduplicateLatestBuilds(res.Builds), hideFilters)
				for _, b := range visibleBuilds {
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
					for _, b := range res.Builds {
						if b.Builder.Builder != "" {
							available = append(available, b.Builder.Builder)
						}
					}
					available = dedup(available)
					if len(available) > 0 {
						return fmt.Errorf("no check found matching builder %q on change %s.\n\nAvailable builders on this change:\n  - %s\n\nRun 'gh run list %d' to view all checks",
							targetBuilder, rawID, strings.Join(available, "\n  - "), res.Change.Number)
					}
					return fmt.Errorf("no check found matching builder %q on change %s (no checks found on this change).\n\nRun 'gh run list %d' to view check status", targetBuilder, rawID, res.Change.Number)
				}
				var failedExp []string
				for _, b := range res.Builds {
					if (b.Status == "FAILURE" || b.Status == "INFRA_FAILURE") && b.IsExperimental() {
						failedExp = append(failedExp, b.Builder.Builder)
					}
				}
				if notice := FormatOmittedExperimentalNotice(len(dedup(failedExp))); notice != "" {
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

			activeProvider := directProvider
			if activeProvider == nil && res != nil {
				activeProvider = res.Provider
			}
			if activeProvider == nil {
				activeProvider = &BuildbucketProvider{LUCIClient: luciClient}
			}

			fetched := make([]*FailureReport, len(targetBuilds))
			fetchErrs := make([]error, len(targetBuilds))
			sem := make(chan struct{}, 8)
			var wg sync.WaitGroup
			for i, b := range targetBuilds {
				if b.Status != "FAILURE" && b.Status != "INFRA_FAILURE" {
					continue
				}
				wg.Add(1)
				go func(idx int, build bbBuild) {
					defer wg.Done()
					sem <- struct{}{}
					defer func() { <-sem }()

					rep, err := activeProvider.FetchFailureReportWithOptions(ctx, build, FailureReportOptions{
						MaxLogLines:            maxLines,
						PreferredLogs:          projCfg.PreferredLogs(),
						IncludeSummaryMarkdown: projCfg.IncludeSummaryMarkdown(),
						Target:                 runViewTarget,
					})
					if err != nil {
						fetchErrs[idx] = err
						return
					}
					if rep != nil {
						if res != nil && build.Patchset > 0 && build.Patchset != res.PatchsetNum {
							rep.Patchset = build.Patchset
						}
						fetched[idx] = rep
					}
				}(i, b)
			}
			wg.Wait()

			var reports []FailureReport
			for i, b := range targetBuilds {
				if fetchErrs[i] != nil {
					fmt.Fprintf(cmd.ErrOrStderr(), "Warning: failed to get details for build %s: %v\n", b.ID, fetchErrs[i])
					continue
				}
				if fetched[i] != nil {
					reports = append(reports, *fetched[i])
				}
			}

			if runViewJSON {
				data, _ := json.MarshalIndent(reports, "", "  ")
				fmt.Fprintln(cmd.OutOrStdout(), string(data))
				return nil
			}

			if len(reports) == 0 {
				for _, b := range targetBuilds {
					note := ""
					if res != nil {
						note = patchsetNote(b.Patchset, res.PatchsetNum)
					}
					if b.Status == "STARTED" || b.Status == "SCHEDULED" {
						viewCmd := fmt.Sprintf("gh run view -j %s -v", b.Builder.Builder)
						if res != nil && res.Change != nil && res.Change.Number > 0 {
							viewCmd = fmt.Sprintf("gh run view %d -j %s -v", res.Change.Number, b.Builder.Builder)
						}
						fmt.Fprintf(cmd.OutOrStdout(), "Check %q%s is currently running (status: %s).\nTo monitor progress, run:\n  %s\n", b.Builder.Builder, note, b.Status, viewCmd)
						continue
					}
					dur := formatDuration(b.StartTime, b.EndTime)
					fmt.Fprintf(cmd.OutOrStdout(), "Check %q%s has status %s (%s).\nBuild details: %s\n", b.Builder.Builder, note, b.Status, dur, BuildURL(b))
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
			var matchedBuild *bbBuild
			if directBuildID != "" {
				bID = directBuildID
			} else if targetBuilder != "" {
				latest := deduplicateLatestBuilds(res.Builds)
				for i := range latest {
					b := &latest[i]
					if buildMatchesJob(*b, targetBuilder) {
						bID = b.ID
						bName = b.Builder.Builder
						matchedBuild = b
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
				// Pick the first failed build, or else the first build, among
				// the builds `gh pr checks` reports.
				latest := deduplicateLatestBuilds(res.Builds)
				for i := range latest {
					b := &latest[i]
					if b.Status == "FAILURE" || b.Status == "INFRA_FAILURE" {
						bID = b.ID
						bName = b.Builder.Builder
						matchedBuild = b
						break
					}
				}
				if bID == "" && len(latest) > 0 {
					bID = latest[0].ID
					bName = latest[0].Builder.Builder
					matchedBuild = &latest[0]
				}
				if bID == "" {
					return fmt.Errorf("no checks found on change %s", rawID)
				}
			}

			var details *LUCIBuildDetails
			var err error
			if directBuildID != "" && (IsBusytownBuildID(directBuildID) || runViewTarget != "") {
				details, err = (&BusytownProvider{}).GetBuildDetails(ctx, directBuildID, &bbBuild{
					Target: runViewTarget,
					Status: "FAILURE",
				})
			} else if res != nil && res.Provider != nil {
				details, err = res.Provider.GetBuildDetails(ctx, bID, matchedBuild)
			} else {
				details, err = luciClient.GetBuildDetails(ctx, bID)
			}
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
		hideFilters := projCfg.CI.HideTagFilters
		if runViewAll {
			hideFilters = nil
		}
		filteredBuilds, hiddenCount := FilterBuildsByTags(deduplicateLatestBuilds(res.Builds), hideFilters)
		relevant, omittedExp := FilterExperimentalBuilds(filteredBuilds, runViewExperimental)
		// Derive the header from the same tally as the Verdict: footer so
		// the two can never disagree.
		verdict := tallyCIVerdict(relevant)
		passedCount := verdict.passed
		failedCount := verdict.failed

		if len(relevant) == 0 {
			fmt.Fprintf(cmd.OutOrStdout(), "No checks scheduled for Change %d (Patchset %d).\n", res.Change.Number, res.PatchsetNum)
			return nil
		}

		overallSymbol := GlyphPass
		overallStatus := "SUCCESS"
		switch {
		case verdict.failed > 0:
			overallSymbol = GlyphFail
			overallStatus = "FAILURE"
		case verdict.canceled > 0:
			overallSymbol = GlyphFail
			overallStatus = "CANCELED"
		case verdict.running > 0:
			overallSymbol = GlyphRunning
			overallStatus = "RUNNING"
		case verdict.total == 0:
			overallSymbol = GlyphWarn
			overallStatus = "NO_BLOCKING_CHECKS"
		}

		changeURL := fmt.Sprintf("https://%s/c/%s/+/%d", res.GerritHost, res.Change.Project, res.Change.Number)
		if res.PatchsetNum > 0 {
			changeURL = fmt.Sprintf("%s/%d", changeURL, res.PatchsetNum)
		}

		if runViewJSON {
			checkItems := BuildCheckItems(relevant)
			summary := map[string]any{
				"change":              res.Change.Number,
				"patchset":            res.PatchsetNum,
				"equivalentPatchsets": res.EquivalentPatchsets,
				"subject":             res.Change.Subject,
				"branch":              res.Change.Branch,
				"status":              overallStatus,
				"url":                 changeURL,
				"total":               len(relevant),
				"passed":              passedCount,
				"failed":              failedCount,
				"jobs":                checkItems,
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

		visible, omittedLine := selectVisibleBuilds(relevant, runViewAll)
		if len(visible) > 0 || omittedLine != "" {
			w := tabwriter.NewWriter(cmd.OutOrStdout(), 0, 0, 2, ' ', 0)
			for _, b := range visible {
				infraNote := ""
				if b.Status == "INFRA_FAILURE" {
					infraNote = " [infra failure]"
				}
				fmt.Fprintf(w, "%s\t%s\tin %s\t(ID %s)%s%s\n",
					getStatusSymbol(b.Status), b.Builder.Builder, formatDuration(b.StartTime, b.EndTime), b.ID, patchsetNote(b.Patchset, res.PatchsetNum), infraNote)
			}
			if omittedLine != "" {
				fmt.Fprintf(w, "%s\t%s\n", GlyphPass, omittedLine)
			}
			w.Flush()
		}

		for _, cluster := range ClusterFailedBuilds(relevant) {
			fmt.Fprintf(cmd.OutOrStdout(), "  %s\n", cluster)
		}

		if notice := FormatOmittedExperimentalNotice(omittedExp); notice != "" {
			fmt.Fprintf(cmd.OutOrStdout(), "\n%s\n", notice)
		}
		if notice := FormatHiddenByConfigNotice(hiddenCount, projCfg.CI.HideTagFilters); notice != "" {
			fmt.Fprintf(cmd.OutOrStdout(), "\n%s\n", notice)
		}

		fmt.Fprintln(cmd.OutOrStdout())
		if verdict.failed > 0 {
			fmt.Fprintf(cmd.OutOrStdout(), "To view failed logs:   gh run view %d --log-failed\n", res.Change.Number)
		}
		fmt.Fprintf(cmd.OutOrStdout(), "To view step tree:     gh run view %d -j <builder>\n", res.Change.Number)
		if verdict.failed > 0 {
			fmt.Fprintf(cmd.OutOrStdout(), "If flaky, rerun:       gh run rerun %d --failed\n", res.Change.Number)
			if hint := effectiveLocalPresubmitHint(projCfg, res.Profile); hint != "" {
				fmt.Fprintf(cmd.OutOrStdout(), "To reproduce locally:  %s\n", hint)
			}
		}
		fmt.Fprintf(cmd.OutOrStdout(), "To view in browser:    gh run view %d -w\n", res.Change.Number)
		writeRunVerdictFooter(cmd.OutOrStdout(), res.Change.Number, relevant)
		return nil
	},
}

var runRerunCmd = &cobra.Command{
	Use:          "rerun [<id> | <builder>] [flags]",
	Annotations:  map[string]string{KindAnnotation: KindMutation},
	Short:        "Rerun CI/CD checks for a change",
	SilenceUsage: true,
	Args:         cobra.MaximumNArgs(2),
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		projCfg, err := LoadCommandProjectConfig(cmd)
		if err != nil {
			return err
		}

		rawID, targetBuilder, _, err := parseRunTargetArgs(cmd, args, runRerunJob, false)
		if err != nil {
			return err
		}

		res, err := ResolveCIContext(cmd, rawID)
		if err != nil {
			return fmt.Errorf("failed to load checks for %q: %w", rawID, err)
		}

		skipRetryFilters := projCfg.EffectiveSkipRetryTagFilters()
		var buildsToRerun []bbBuild
		if targetBuilder != "" {
			found := false
			for _, b := range deduplicateLatestBuilds(res.Builds) {
				if buildMatchesJob(b, targetBuilder) {
					found = true
					buildsToRerun = append(buildsToRerun, b)
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
			buildsToRerun = collectFailedBuilds(res.Builds, runRerunExperimental, skipRetryFilters...)
			if len(buildsToRerun) == 0 {
				fmt.Fprintln(cmd.OutOrStdout(), "No failed checks found to rerun.")
				return nil
			}
		} else if allBusytownBuilds(res.Builds) || (res.Provider != nil && res.Provider.Name() == "busytown") {
			// Busytown / TreeHugger reruns are patchset-wide (treetop~runaction / Presubmit-Ready+1),
			// so 'gh run rerun [<id>]' without --failed or -j can re-trigger the patchset presubmit directly.
			buildsToRerun = deduplicateLatestBuilds(res.Builds)
		} else {
			failedBuilders := collectFailedBuilders(res.Builds, runRerunExperimental, skipRetryFilters...)
			if len(failedBuilders) > 0 {
				return fmt.Errorf("no builder specified to rerun: specify a builder name or pass --failed to rerun all failed checks.\n\nUsage examples:\n  gh run rerun %[1]d --failed\n  gh run rerun %[1]d <builder-name>\n  gh run rerun %[1]d -j <builder-name>\n\nFailed checks available to rerun:\n  - %[2]s", res.Change.Number, strings.Join(failedBuilders, "\n  - "))
			}
			return fmt.Errorf("no builder specified to rerun and no failed checks found on this change: specify a builder name or pass --failed.\n\nUsage:\n  gh run rerun %[1]d <builder-name>\n\nRun 'gh pr checks %[1]d' to view check status", res.Change.Number)
		}

		chRef := GerritChangeRef{
			Host:     res.GerritHost,
			Project:  res.Change.Project,
			ChangeID: res.Change.Number,
			Patchset: res.PatchsetNum,
		}

		provider := res.Provider
		if provider == nil {
			provider = ResolveCIProvider(res.Config, projCfg, res.Client, res.GerritHost, res.Change)
			setProviderProfile(provider, res.Profile, cmd)
		}

		if err := provider.RerunBuilds(ctx, chRef, buildsToRerun, runRerunDryRun, cmd.OutOrStdout()); err != nil {
			return err
		}
		if !runRerunDryRun {
			PrintReceipt(cmd.OutOrStdout(), Receipt{
				Summary: fmt.Sprintf("Triggered rerun for Change %d (Patchset %d)", res.Change.Number, res.PatchsetNum),
				Details: []ReceiptField{
					{Key: "Builders", Value: fmt.Sprintf("%d triggered", len(buildsToRerun))},
				},
				Next: fmt.Sprintf("gh run watch %d", res.Change.Number),
			})
		}
		return nil
	},
}

func allBusytownBuilds(builds []bbBuild) bool {
	if len(builds) == 0 {
		return false
	}
	for _, b := range builds {
		if !b.IsBusytown() {
			return false
		}
	}
	return true
}

var runWatchCmd = &cobra.Command{
	Use:          "watch [<id>[/<patchset>]]",
	Annotations:  map[string]string{KindAnnotation: KindStream},
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
	//ghish:flag mode=extension reason="Show all individual passing checks without collapsing"
	runListCmd.Flags().BoolVar(&runListAll, "all", false, "Include builds hidden by .ghish.toml hide_tag_filters (ghish-only)")
	runListCmd.Flags().StringVar(&buildbucketHost, "buildbucket-host", "cr-buildbucket.appspot.com", "Buildbucket host to query")
	runListCmd.Flags().MarkHidden("buildbucket-host")

	runViewCmd.Flags().BoolVar(&runViewLogFailed, "log-failed", false, "Output log snippet and diagnostics for failed checks")
	runViewCmd.Flags().BoolVar(&runViewLog, "log", false, "Output full log for check")
	runViewCmd.Flags().BoolVarP(&runViewVerbose, "verbose", "v", false, "Show step execution tree")
	runViewCmd.Flags().StringVarP(&runViewJob, "job", "j", "", "View a specific builder by name")
	runViewCmd.Flags().StringVar(&runViewTarget, "target", "", "Busytown build target name for direct build ID inspection (ghish-only)")
	runViewCmd.Flags().BoolVarP(&runViewWeb, "web", "w", false, "Open check in web browser")
	runViewCmd.Flags().BoolVar(&runViewJSON, "json", false, "Output in JSON format")
	runViewCmd.Flags().BoolVarP(&runViewExperimental, "experimental", "e", false, "Include non-blocking experimental checks")
	//ghish:flag mode=extension reason="Show all individual passing checks without collapsing"
	runViewCmd.Flags().BoolVar(&runViewAll, "all", false, "Show all passing checks without collapsing and include hidden builds (ghish-only)")
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
	//ghish:flag mode=extension reason="Show all individual passing checks without collapsing"
	runWatchCmd.Flags().BoolVar(&checksAll, "all", false, "Show all passing checks without collapsing and include hidden builds (ghish-only)")
	runWatchCmd.Flags().StringVar(&buildbucketHost, "buildbucket-host", "cr-buildbucket.appspot.com", "Buildbucket host to query")
	runWatchCmd.Flags().MarkHidden("buildbucket-host")

	RunCmd.AddCommand(runListCmd)
	RunCmd.AddCommand(runViewCmd)
	RunCmd.AddCommand(runRerunCmd)
	RunCmd.AddCommand(runWatchCmd)

	RootCmd.AddCommand(RunCmd)
}
