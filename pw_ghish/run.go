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
	"cmp"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"slices"
	"strings"
	"sync"
	"text/tabwriter"
	"time"

	"github.com/spf13/cobra"
)

type runListOptions struct {
	workflows []string
	limit     int
	status    string
}

var (
	runListOpts          runListOptions
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

const maxStdinTargetBytes = 1 << 20

func isBuildbucketHostFlagChanged(cmd *cobra.Command) bool {
	return cmd != nil && cmd.Flags().Changed("buildbucket-host")
}

func effectiveBuildbucketHost(cmd *cobra.Command, projCfg *ProjectConfig) (string, error) {
	cfgHost := ""
	if projCfg != nil {
		cfgHost = projCfg.CI.BuildbucketHost
	}
	return ResolveEffectiveBuildbucketHost(buildbucketHost, isBuildbucketHostFlagChanged(cmd), cfgHost)
}

func writeIndentedJSON(out io.Writer, v any) error {
	data, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return fmt.Errorf("failed to marshal JSON: %w", err)
	}
	fmt.Fprintln(out, string(data))
	return nil
}

func resolveDefaultLUCIProject(cmd *cobra.Command, projCfg *ProjectConfig) string {
	if projCfg != nil {
		for _, tb := range projCfg.CI.TryBuckets {
			if proj, _, ok := strings.Cut(strings.TrimSpace(tb), "/"); ok && proj != "" {
				return proj
			}
		}
	}
	if cfg := GetConfig(cmd); cfg != nil {
		if prof := cfg.GetProfile(cmd.Context()); prof != nil {
			return prof.BuildbucketProject()
		}
	}
	return ""
}

func readStdinLines(in io.Reader, what string) ([]string, error) {
	if in == nil {
		return nil, fmt.Errorf("cannot read %s from '-': stdin is nil", what)
	}
	raw, err := io.ReadAll(io.LimitReader(in, maxStdinTargetBytes+1))
	if err != nil {
		return nil, fmt.Errorf("failed to read %s from stdin: %w", what, err)
	}
	if len(raw) > maxStdinTargetBytes {
		return nil, fmt.Errorf("stdin for %s exceeds %d bytes", what, maxStdinTargetBytes)
	}
	var out []string
	for _, line := range strings.Split(string(raw), "\n") {
		trimmed := strings.TrimSpace(line)
		if trimmed == "" || strings.HasPrefix(trimmed, "#") {
			continue
		}
		out = append(out, trimmed)
	}
	return out, nil
}

func expandBuilderSpecs(cmd *cobra.Command, workflows []string) ([]string, error) {
	if len(workflows) == 0 {
		return nil, nil
	}
	var raw []string
	readStdinOnce := false
	for _, spec := range workflows {
		item := strings.TrimSpace(spec)
		if item == "" {
			continue
		}
		if item == "-" {
			if readStdinOnce {
				continue
			}
			readStdinOnce = true
			lines, err := readStdinLines(cmd.InOrStdin(), "builder selectors")
			if err != nil {
				return nil, err
			}
			raw = append(raw, lines...)
			continue
		}
		raw = append(raw, item)
	}
	expanded := dedup(raw)
	if len(expanded) == 0 {
		return nil, fmt.Errorf("no LUCI builders provided via -w / --workflow")
	}
	return expanded, nil
}

func matchBuildStatusFilter(buildStatus, bbFilter string) bool {
	switch bbFilter {
	case "":
		return true
	case "ENDED_MASK":
		switch buildStatus {
		case "SUCCESS", "FAILURE", "INFRA_FAILURE", "CANCELED":
			return true
		default:
			return false
		}
	default:
		return strings.EqualFold(buildStatus, bbFilter)
	}
}

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
// job selector, which may be "<builder>", "<bucket>/<builder>",
// "<project>/<bucket>/<builder>", or "luci.<project>.<bucket>/<builder>" (case-insensitive).
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
	if parsed, err := ParseBuilderSelector(job, ""); err == nil {
		return strings.EqualFold(b.Builder.Project, parsed.Project) &&
			strings.EqualFold(b.Builder.Bucket, parsed.Bucket) &&
			strings.EqualFold(b.Builder.Builder, parsed.Builder)
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
  list    List recent builds for a change or LUCI builder
  view    View build details, step trees (-v), and failure logs (--log-failed)
  rerun   Rerun failed checks (--failed) or specific builders (-j <builder>)
  watch   Watch checks until completion`,
}

func runBuilderListMode(cmd *cobra.Command, projCfg *ProjectConfig, specs []string) error {
	ctx := cmd.Context()
	defaultProj := ""
	defaultProjResolved := false
	parsed := make([]bbBuilder, 0, len(specs))
	for _, spec := range specs {
		trimmed := strings.TrimPrefix(strings.TrimSpace(spec), "buildbucket/")
		if strings.Count(trimmed, "/") == 1 && !strings.HasPrefix(trimmed, "luci.") && !defaultProjResolved {
			defaultProj = resolveDefaultLUCIProject(cmd, projCfg)
			defaultProjResolved = true
		}
		b, err := ParseBuilderSelector(spec, defaultProj)
		if err != nil {
			return err
		}
		parsed = append(parsed, b)
	}

	bbHost, err := effectiveBuildbucketHost(cmd, projCfg)
	if err != nil {
		return err
	}
	luciClient := NewLUCIClient(bbHost, getLUCIHTTPClient())
	results, err := luciClient.SearchBuilderBuildsBatch(ctx, parsed, runListOpts.limit, runListOpts.status)
	if err != nil {
		return err
	}

	var flat []bbBuild
	for _, r := range results {
		if r.Error != "" {
			fmt.Fprintf(cmd.ErrOrStderr(), "Warning: %s/%s/%s: %s\n",
				r.Project, r.Bucket, r.Builder, SanitizeUntrustedText(r.Error))
		}
		flat = append(flat, r.Builds...)
	}

	if runListJSON {
		return writeIndentedJSON(cmd.OutOrStdout(), BuildCheckItems(flat))
	}

	if len(flat) == 0 {
		fmt.Fprintf(cmd.OutOrStdout(), "No builds found for %s.\n\nVerdict: NO CHECKS\nNext:    gh run list -w <project>/<bucket>/<builder>\n",
			countNoun(len(results), "builder"))
		return nil
	}

	fmt.Fprintf(cmd.OutOrStdout(), "Showing %d build(s) across %s\n\n", len(flat), countNoun(len(results), "builder"))
	writeRunListTable(cmd.OutOrStdout(), flat, 0, runListAll || VerboseFlag || len(results) == 1)
	pendingCmd := fmt.Sprintf("gh run list -w %s/%s/%s", results[0].Project, results[0].Bucket, results[0].Builder)
	writeDirectBuildsVerdictFooter(cmd.OutOrStdout(), flat, pendingCmd, "")
	return nil
}

func writeDirectBuildsVerdictFooter(out io.Writer, builds []bbBuild, pendingNext, viewSuffix string) {
	var failed, canceled, running, passed int
	firstFailedID := ""
	for _, b := range builds {
		switch classifyCheckStatus(b.Status) {
		case checkFailed:
			failed++
			if firstFailedID == "" {
				firstFailedID = b.ID
			}
		case checkCanceled:
			canceled++
		case checkPending:
			running++
		case checkPassed:
			passed++
		}
	}
	fmt.Fprintln(out)
	switch {
	case failed > 0:
		fmt.Fprintf(out, "Verdict: FAILED (%d of %d builds failed)\nNext:    gh run view %s --log-failed\n",
			failed, len(builds), cmp.Or(firstFailedID, builds[0].ID))
	case running > 0:
		fmt.Fprintf(out, "Verdict: PENDING (%d running, %d passed)\nNext:    %s\n", running, passed, pendingNext)
	case canceled > 0:
		fmt.Fprintf(out, "Verdict: CANCELED (%d of %d builds canceled)\nNext:    gh run view %s%s\n",
			canceled, len(builds), builds[0].ID, viewSuffix)
	default:
		fmt.Fprintf(out, "Verdict: PASSED (%s passed)\nNext:    gh run view %s%s\n",
			countNoun(passed, "build"), builds[0].ID, viewSuffix)
	}
}

var runListCmd = &cobra.Command{
	Use:          "list [<id>[/<patchset>]]",
	Annotations:  map[string]string{KindAnnotation: KindList},
	Short:        "List recent CI/CD workflow runs for a change or LUCI builder",
	SilenceUsage: true,
	Args:         cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		bbStatusFilter, err := ParseRunStatusFilter(runListOpts.status)
		if err != nil {
			return err
		}
		if cmd.Flags().Changed("limit") && (runListOpts.limit <= 0 || runListOpts.limit > maxBuilderRunLimit) {
			return fmt.Errorf("invalid --limit %d: must be between 1 and %d", runListOpts.limit, maxBuilderRunLimit)
		}

		projCfg, err := LoadCommandProjectConfig(cmd)
		if err != nil {
			return err
		}

		specs, err := expandBuilderSpecs(cmd, runListOpts.workflows)
		if err != nil {
			return err
		}
		if len(args) == 0 && len(specs) > 0 {
			return runBuilderListMode(cmd, projCfg, specs)
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
		patchsetBuilds := relevant
		if len(specs) > 0 {
			var matched []bbBuild
			for _, b := range relevant {
				if slices.ContainsFunc(specs, func(sel string) bool { return buildMatchesJob(b, sel) }) {
					matched = append(matched, b)
				}
			}
			relevant = matched
		}
		if bbStatusFilter != "" {
			var statusMatched []bbBuild
			for _, b := range relevant {
				if matchBuildStatusFilter(b.Status, bbStatusFilter) {
					statusMatched = append(statusMatched, b)
				}
			}
			relevant = statusMatched
		}
		if runListOpts.limit > 0 && len(relevant) > runListOpts.limit {
			failed, passing := PartitionAndOrderBuilds(relevant)
			ordered := slices.Concat(failed, passing)
			relevant = ordered[:runListOpts.limit]
		}

		if len(relevant) == 0 {
			if runListJSON {
				fmt.Fprintln(cmd.OutOrStdout(), "[]")
				return nil
			}
			if len(patchsetBuilds) > 0 {
				fmt.Fprintf(cmd.OutOrStdout(), "No matching checks found for Change %d (Patchset %d).\n", res.Change.Number, res.PatchsetNum)
				writeRunVerdictFooter(cmd.OutOrStdout(), res.Change.Number, patchsetBuilds)
				return nil
			}
			fmt.Fprintf(cmd.OutOrStdout(), "No checks scheduled for Change %d (Patchset %d).\n", res.Change.Number, res.PatchsetNum)
			return nil
		}

		if runListJSON {
			return writeIndentedJSON(cmd.OutOrStdout(), BuildCheckItems(relevant))
		}

		fmt.Fprintf(cmd.OutOrStdout(), "Showing %d checks for Change %d (Patchset %d) • %s\n\n", len(relevant), res.Change.Number, res.PatchsetNum, res.Change.Subject)

		writeRunListTable(cmd.OutOrStdout(), relevant, res.PatchsetNum, runListAll || VerboseFlag)

		if notice := FormatOmittedExperimentalNotice(omittedExp); notice != "" {
			fmt.Fprintf(cmd.OutOrStdout(), "\n%s\n", notice)
		}
		if notice := FormatHiddenByConfigNotice(hiddenCount, projCfg.CI.HideTagFilters); notice != "" {
			fmt.Fprintf(cmd.OutOrStdout(), "\n%s\n", notice)
		}
		writeRunVerdictFooter(cmd.OutOrStdout(), res.Change.Number, patchsetBuilds)
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

func parseBatchDirectBuildIDs(cmd *cobra.Command, args []string) ([]string, bool, error) {
	if len(args) > 1 && slices.ContainsFunc(args, func(a string) bool { return strings.TrimSpace(a) == "-" }) {
		return nil, true, fmt.Errorf("'-' (read build IDs from stdin) cannot be combined with positional arguments")
	}
	if len(args) == 1 && strings.TrimSpace(args[0]) == "-" {
		lines, err := readStdinLines(cmd.InOrStdin(), "build IDs")
		if err != nil {
			return nil, true, err
		}
		var ids []string
		for _, ln := range lines {
			for _, tok := range strings.Fields(ln) {
				if !isBuildbucketID(tok) {
					return nil, true, fmt.Errorf("invalid Buildbucket build ID %q on stdin: expected a 64-bit numeric Buildbucket build ID (or run 'gh run view <change-id>')", tok)
				}
				ids = append(ids, tok)
			}
		}
		ids = dedup(ids)
		if len(ids) == 0 {
			return nil, true, fmt.Errorf("no Buildbucket build IDs provided on stdin")
		}
		return ids, true, nil
	}
	if len(args) >= 2 {
		if idx := slices.IndexFunc(args, func(a string) bool { return IsBusytownBuildID(strings.TrimSpace(a)) }); idx >= 0 {
			return nil, true, fmt.Errorf("batch 'gh run view' supports Buildbucket build IDs only (got Busytown ID %q); inspect one at a time via 'gh run view %s'",
				args[idx], strings.TrimSpace(args[idx]))
		}
		badIdx := slices.IndexFunc(args, func(a string) bool { return !isBuildbucketID(strings.TrimSpace(a)) })
		if badIdx < 0 {
			ids := make([]string, len(args))
			for i, a := range args {
				ids[i] = strings.TrimSpace(a)
			}
			return dedup(ids), true, nil
		}
		if isBuildbucketID(strings.TrimSpace(args[0])) {
			return nil, true, fmt.Errorf("invalid Buildbucket build ID %q: all arguments after %s must be 64-bit numeric Buildbucket IDs (or run 'gh run view [<change-id>] [<builder>]')",
				args[badIdx], strings.TrimSpace(args[0]))
		}
		if len(args) > 2 {
			return nil, false, fmt.Errorf("too many arguments (%d): pass 'gh run view [<change-id>] [<builder>]' or Buildbucket IDs 'gh run view <build-id>...'", len(args))
		}
	}
	return nil, false, nil
}

func availableBuilderNames(builds []bbBuild) []string {
	var available []string
	for _, b := range builds {
		if b.Builder.Builder != "" {
			available = append(available, b.Builder.Builder)
		}
	}
	return dedup(available)
}

func formatMissingChangeBuilderError(noun, targetBuilder, rawID string, res *CIContext) error {
	available := availableBuilderNames(deduplicateLatestBuilds(res.Builds))
	listMsg := ""
	if len(available) > 0 {
		listMsg = fmt.Sprintf("\n\nAvailable checks on Change %d (Patchset %d):\n  - %s", res.Change.Number, res.PatchsetNum, strings.Join(available, "\n  - "))
	}
	return fmt.Errorf("%s %q not found on change %s%s", noun, targetBuilder, rawID, listMsg)
}

func fetchFailureReportsBounded(ctx context.Context, count int, fn func(idx int) (*FailureReport, error)) ([]FailureReport, []error, error) {
	fetched := make([]*FailureReport, count)
	fetchErrs := make([]error, count)
	sem := make(chan struct{}, 8)
	var wg sync.WaitGroup
	for i := range count {
		wg.Add(1)
		go func(idx int) {
			defer wg.Done()
			select {
			case sem <- struct{}{}:
				defer func() { <-sem }()
			case <-ctx.Done():
				return
			}
			if ctx.Err() == nil {
				fetched[idx], fetchErrs[idx] = fn(idx)
			}
		}(i)
	}
	wg.Wait()
	if err := ctx.Err(); err != nil {
		return nil, nil, err
	}
	reports := make([]FailureReport, 0, count)
	for _, rep := range fetched {
		if rep != nil {
			reports = append(reports, *rep)
		}
	}
	return reports, fetchErrs, nil
}

func writeNonFailedCheckStatus(out io.Writer, name, note, status, startTime, endTime, buildURL, monitorCmd string) {
	if status == "STARTED" || status == "SCHEDULED" {
		fmt.Fprintf(out, "Check %q%s is currently running (status: %s).\nTo monitor progress, run:\n  %s\n", name, note, status, monitorCmd)
		return
	}
	fmt.Fprintf(out, "Check %q%s has status %s (%s).\nBuild details: %s\n", name, note, status, formatDuration(startTime, endTime), buildURL)
}

func runDirectBuildbucketView(cmd *cobra.Command, projCfg *ProjectConfig, luciClient *LUCIClient, buildIDs []string, batchOutput, hasPositionalArgs bool) error {
	ctx := cmd.Context()
	if strings.TrimSpace(runViewTarget) != "" || (strings.TrimSpace(runViewJob) != "" && (batchOutput || hasPositionalArgs || len(buildIDs) != 1 || strings.TrimSpace(runViewJob) != buildIDs[0])) {
		return fmt.Errorf("--job (-j) and --target cannot be combined with direct Buildbucket build IDs")
	}
	if runViewWeb {
		if len(buildIDs) > 1 {
			return fmt.Errorf("--web cannot open %d builds at once; pass a single build ID", len(buildIDs))
		}
		targetURL := fmt.Sprintf("https://ci.chromium.org/b/%s", buildIDs[0])
		fmt.Fprintf(cmd.OutOrStdout(), "Opening %s in your browser.\n", targetURL)
		return OpenBrowserFn(targetURL)
	}

	detailsList, err := luciClient.GetBuildDetailsBatch(ctx, buildIDs)
	if err != nil {
		if !batchOutput {
			verb := "steps"
			if runViewLogFailed || runViewLog {
				verb = "build"
			}
			return fmt.Errorf("failed to fetch %s for build %s: %w", verb, buildIDs[0], err)
		}
		return err
	}
	fetchErrCount := 0
	for _, det := range detailsList {
		if det != nil && det.Error != "" {
			fetchErrCount++
			fmt.Fprintf(cmd.ErrOrStderr(), "Warning: failed to get details for build %s: %s\n",
				det.ID, SanitizeUntrustedText(det.Error))
		}
	}

	if runViewLogFailed || runViewLog {
		maxLines := 100
		if runViewLog {
			maxLines = 0
		}
		reports, _, err := fetchFailureReportsBounded(ctx, len(detailsList), func(idx int) (*FailureReport, error) {
			det := detailsList[idx]
			if det == nil || det.Error != "" || (det.Status != "FAILURE" && det.Status != "INFRA_FAILURE") {
				return nil, nil
			}
			return luciClient.ExtractFailureReportWithOptions(ctx, det, maxLines, projCfg.PreferredLogs(), projCfg.IncludeSummaryMarkdown()), nil
		})
		if err != nil {
			return err
		}
		if runViewJSON {
			return writeIndentedJSON(cmd.OutOrStdout(), reports)
		}
		if len(reports) == 0 {
			if !batchOutput && len(detailsList) == 1 && detailsList[0] != nil {
				d := detailsList[0]
				writeNonFailedCheckStatus(
					cmd.OutOrStdout(),
					cmp.Or(d.Builder.Builder, d.ID),
					"",
					d.Status,
					d.StartTime,
					d.EndTime,
					cmp.Or(d.ViewURL, fmt.Sprintf("https://ci.chromium.org/b/%s", d.ID)),
					fmt.Sprintf("gh run view %s -v", d.ID),
				)
				return nil
			}
			if fetchErrCount > 0 {
				fmt.Fprintf(cmd.OutOrStdout(), "No failed builds found among %d fetched build(s) (%d failed to load; see warnings on stderr).\n",
					len(detailsList)-fetchErrCount, fetchErrCount)
				return nil
			}
			fmt.Fprintln(cmd.OutOrStdout(), "No failed builds found in requested build IDs.")
			return nil
		}
		fmt.Fprint(cmd.OutOrStdout(), FormatFailureReports(reports))
		return nil
	}

	if runViewJSON {
		if !batchOutput {
			return writeIndentedJSON(cmd.OutOrStdout(), detailsList[0])
		}
		return writeIndentedJSON(cmd.OutOrStdout(), detailsList)
	}

	var validBuilds []bbBuild
	for _, det := range detailsList {
		if det == nil || det.Error != "" {
			continue
		}
		if len(validBuilds) > 0 {
			fmt.Fprintln(cmd.OutOrStdout())
		}
		fmt.Fprint(cmd.OutOrStdout(), FormatBuildStepsVerbose(det, runViewVerbose))
		validBuilds = append(validBuilds, bbBuild{ID: det.ID, Status: det.Status})
	}
	if batchOutput && len(validBuilds) > 0 {
		writeDirectBuildsVerdictFooter(cmd.OutOrStdout(), validBuilds, fmt.Sprintf("gh run view %s -v", validBuilds[0].ID), " -v")
	}
	return nil
}

var runViewCmd = &cobra.Command{
	Use:          "view [<id>... | <builder>] [flags]",
	Annotations:  map[string]string{KindAnnotation: KindView},
	Short:        "View details, step trees, or logs for CI/CD builds",
	SilenceUsage: true,
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		projCfg, err := LoadCommandProjectConfig(cmd)
		if err != nil {
			return err
		}
		bbHost, err := effectiveBuildbucketHost(cmd, projCfg)
		if err != nil {
			return err
		}
		luciClient := NewLUCIClient(bbHost, getLUCIHTTPClient())

		batchBuildIDs, isBatchMode, err := parseBatchDirectBuildIDs(cmd, args)
		if err != nil {
			return err
		}
		if isBatchMode {
			return runDirectBuildbucketView(cmd, projCfg, luciClient, batchBuildIDs, true, len(args) > 0)
		}

		rawID, targetBuilder, directBuildID, err := parseRunTargetArgs(cmd, args, runViewJob, true)
		if err != nil {
			return err
		}
		if directBuildID != "" && !IsBusytownBuildID(directBuildID) && strings.TrimSpace(runViewTarget) == "" {
			return runDirectBuildbucketView(cmd, projCfg, luciClient, []string{directBuildID}, false, len(args) > 0)
		}

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
				bid, target, _ := ParseBusytownBuildTarget(directBuildID, runViewTarget)
				targetURL = BusytownBuildURL(bid, target)
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
				bid, target, _ := ParseBusytownBuildTarget(directBuildID, runViewTarget)
				directProvider = NewBusytownProvider()
				builderName := cmp.Or(target, bid)
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
					if available := availableBuilderNames(res.Builds); len(available) > 0 {
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

			reports, fetchErrs, err := fetchFailureReportsBounded(ctx, len(targetBuilds), func(idx int) (*FailureReport, error) {
				build := targetBuilds[idx]
				if build.Status != "FAILURE" && build.Status != "INFRA_FAILURE" {
					return nil, nil
				}
				rep, fetchErr := activeProvider.FetchFailureReportWithOptions(ctx, build, FailureReportOptions{
					MaxLogLines:            maxLines,
					PreferredLogs:          projCfg.PreferredLogs(),
					IncludeSummaryMarkdown: projCfg.IncludeSummaryMarkdown(),
					Target:                 runViewTarget,
				})
				if fetchErr != nil {
					return nil, fetchErr
				}
				if rep != nil && res != nil && build.Patchset > 0 && build.Patchset != res.PatchsetNum {
					rep.Patchset = build.Patchset
				}
				return rep, nil
			})
			if err != nil {
				return err
			}
			for i, b := range targetBuilds {
				if fetchErrs[i] != nil {
					fmt.Fprintf(cmd.ErrOrStderr(), "Warning: failed to get details for build %s: %v\n", b.ID, fetchErrs[i])
				}
			}

			if runViewJSON {
				return writeIndentedJSON(cmd.OutOrStdout(), reports)
			}

			if len(reports) == 0 {
				for _, b := range targetBuilds {
					note := ""
					if res != nil {
						note = patchsetNote(b.Patchset, res.PatchsetNum)
					}
					viewCmd := fmt.Sprintf("gh run view -j %s -v", b.Builder.Builder)
					if res != nil && res.Change != nil && res.Change.Number > 0 {
						viewCmd = fmt.Sprintf("gh run view %d -j %s -v", res.Change.Number, b.Builder.Builder)
					}
					writeNonFailedCheckStatus(cmd.OutOrStdout(), b.Builder.Builder, note, b.Status, b.StartTime, b.EndTime, BuildURL(b), viewCmd)
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
					return formatMissingChangeBuilderError("check", targetBuilder, rawID, res)
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
				details, err = NewBusytownProvider().GetBuildDetails(ctx, directBuildID, &bbBuild{
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
				return writeIndentedJSON(cmd.OutOrStdout(), details)
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
			return writeIndentedJSON(cmd.OutOrStdout(), map[string]any{
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
			})
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
				return formatMissingChangeBuilderError("builder", targetBuilder, rawID, res)
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
	//ghish:flag mode=divergent gh="run list --workflow" doc="ghDivergenceHelp"
	runListCmd.Flags().StringSliceVarP(&runListOpts.workflows, "workflow", "w", nil, "Query or filter runs by LUCI builder (<project>/<bucket>/<builder>, repeatable, or '-' for stdin)")
	//ghish:flag mode=upstream gh="run list --limit"
	runListCmd.Flags().IntVarP(&runListOpts.limit, "limit", "L", 0, "Maximum number of builds to fetch (default 10 for -w/--workflow)")
	//ghish:flag mode=upstream gh="run list --status"
	runListCmd.Flags().StringVarP(&runListOpts.status, "status", "s", "", "Filter builds by status (completed, in_progress, queued, success, failure, infra_failure, canceled)")
	runListCmd.Flags().BoolVar(&runListJSON, "json", false, "Output JSON with specified fields")
	runListCmd.Flags().BoolVarP(&runListExperimental, "experimental", "e", false, "Include non-blocking experimental checks")
	//ghish:flag mode=extension reason="Show all individual passing checks without collapsing"
	runListCmd.Flags().BoolVar(&runListAll, "all", false, "Show all passing checks without collapsing and include hidden builds (ghish-only)")
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
