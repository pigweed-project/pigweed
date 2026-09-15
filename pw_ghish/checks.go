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
	"fmt"
	"net/http"
	"sort"
	"strings"
	"time"

	"github.com/spf13/cobra"
)

var (
	checksJSON         string
	checksTemplateStr  string
	buildbucketHost    string
	checksExperimental bool
	checksWatch        bool
	checksWeb          bool
	checksFailFast     bool
	checksInterval     time.Duration
	checksLogFailed    bool
)

const defaultChecksTemplate = `Checks for Change {{.number}} (Patchset {{.patchset}})
{{range .checks}}  {{.StatusSymbol}}  {{printf "%-40s" .Name}}  {{printf "%-8s" .Duration}}  {{.URL}}
{{end}}{{if gt .omittedExperimental 0}}
({{ .omittedExperimental }} non-blocking experimental builder{{if ne .omittedExperimental 1}}s{{end}} omitted; add --experimental to see them)
{{end}}`

type CheckItem struct {
	ID           string `json:"id"`
	Name         string `json:"name"`
	Bucket       string `json:"bucket"`
	Status       string `json:"status"`
	StatusSymbol string `json:"statusSymbol"`
	Duration     string `json:"duration"`
	URL          string `json:"url"`
	Summary      string `json:"summary,omitempty"`
	Experimental bool   `json:"experimental"`
}

// NewCheckItem creates a CheckItem from a Buildbucket build.
func NewCheckItem(b bbBuild) CheckItem {
	return CheckItem{
		ID:           b.ID,
		Name:         b.Builder.Builder,
		Bucket:       b.Builder.Bucket,
		Status:       b.Status,
		StatusSymbol: getStatusSymbol(b.Status),
		Duration:     formatDuration(b.StartTime, b.EndTime),
		URL:          fmt.Sprintf("https://ci.chromium.org/b/%s", b.ID),
		Summary:      b.SummaryMarkdown,
		Experimental: b.IsExperimental(),
	}
}

// BuildCheckItems converts a slice of Buildbucket builds into CheckItems.
func BuildCheckItems(builds []bbBuild) []CheckItem {
	items := make([]CheckItem, 0, len(builds))
	for _, b := range builds {
		items = append(items, NewCheckItem(b))
	}
	return items
}

// FilterExperimentalBuilds filters builds according to includeExperimental,
// returning the filtered slice and the number of omitted experimental builds.
func FilterExperimentalBuilds(builds []bbBuild, includeExperimental bool) ([]bbBuild, int) {
	var relevant []bbBuild
	omittedCount := 0
	for _, b := range builds {
		if !includeExperimental && b.IsExperimental() {
			omittedCount++
			continue
		}
		relevant = append(relevant, b)
	}
	return relevant, omittedCount
}

// FormatOmittedExperimentalNotice returns the standard notice for omitted experimental builders,
// or an empty string if omittedCount <= 0.
func FormatOmittedExperimentalNotice(omittedCount int) string {
	if omittedCount <= 0 {
		return ""
	}
	plural := "s"
	if omittedCount == 1 {
		plural = ""
	}
	return fmt.Sprintf("(%d non-blocking experimental builder%s omitted; add --experimental to see them)", omittedCount, plural)
}

// getLUCIHTTPClient returns an HTTP client for LUCI Buildbucket queries.
// It standardizes where the LUCI HTTP client comes from across checks and run commands,
// and provides a centralized extension point for authenticated transport in private buckets.
var getLUCIHTTPClient = func(ctx context.Context, bbHost string) *http.Client {
	return http.DefaultClient
}

// queryBuildbucket sends a pRPC request to Buildbucket to search for builds.
func queryBuildbucket(ctx context.Context, bbHost string, gerritHost, project string, changeNum, patchsetNum int, httpClient *http.Client) ([]bbBuild, error) {
	return NewLUCIClient(bbHost, httpClient).SearchBuilds(ctx, gerritHost, project, changeNum, patchsetNum)
}

func getStatusSymbol(status string) string {
	switch status {
	case "SUCCESS":
		return "✓"
	case "FAILURE":
		return "✗"
	case "INFRA_FAILURE":
		return "!"
	case "STARTED":
		return "*"
	case "SCHEDULED":
		return "?"
	default:
		return "-"
	}
}

// checkOutcome is the gate-relevant classification of a Buildbucket status.
type checkOutcome int

const (
	checkPassed checkOutcome = iota
	checkPending
	checkCanceled
	checkFailed
)

// classifyCheckStatus maps a Buildbucket status onto the outcome used to
// compute the process exit code.
//
// It fails closed: only an explicit SUCCESS counts as passing, and only the
// two known in-flight statuses count as pending. Any status Buildbucket adds
// in the future is treated as a failure. A check that did not demonstrably
// pass must never let `gh pr checks` report success, because callers use that
// exit code to decide whether to merge.
//
// CANCELED gets its own outcome. It is still not a pass, but calling it a
// failure is a lie: the usual cause is a newer patchset superseding the run,
// and telling someone that 29 checks "failed" sends them hunting for a broken
// build that does not exist.
func classifyCheckStatus(status string) checkOutcome {
	switch status {
	case "SUCCESS":
		return checkPassed
	case "SCHEDULED", "STARTED":
		return checkPending
	case "CANCELED":
		return checkCanceled
	default:
		return checkFailed
	}
}

func formatDuration(startStr, endStr string) string {
	if startStr == "" {
		return "-"
	}
	startTime, err := time.Parse(time.RFC3339Nano, startStr)
	if err != nil {
		return "-"
	}
	if endStr != "" {
		endTime, err := time.Parse(time.RFC3339Nano, endStr)
		if err == nil {
			return endTime.Sub(startTime).Round(time.Second).String()
		}
	}
	return time.Since(startTime).Round(time.Second).String()
}

func formatChecksStatusBreakdown(passed, running, failed, other int) string {
	parts := []string{
		fmt.Sprintf("%d passed", passed),
		fmt.Sprintf("%d running", running),
	}
	if failed > 0 {
		parts = append(parts, fmt.Sprintf("%d failed", failed))
	}
	if other > 0 {
		parts = append(parts, fmt.Sprintf("%d cancelled", other))
	}
	return strings.Join(parts, ", ")
}

// deduplicateLatestBuilds filters a list of builds returned by Buildbucket
// (which are sorted newest-first) to include only the most recent build attempt
// per builder name.
func deduplicateLatestBuilds(builds []bbBuild) []bbBuild {
	var deduped []bbBuild
	seen := make(map[string]bool)
	for _, b := range builds {
		if !seen[b.Builder.Builder] {
			seen[b.Builder.Builder] = true
			deduped = append(deduped, b)
		}
	}
	return deduped
}

var checksCmd = &cobra.Command{
	Use:          "checks [<id>[/<patchset>]]",
	Short:        "Show CI/CD status (from Buildbucket)",
	SilenceUsage: true,
	Long: `Show CI/CD check/build status (via LUCI Buildbucket) for a Gerrit change.
You can optionally specify a patchset number (e.g. 1623660/7) to view status for an older patchset.
Otherwise, the latest patchset is used.

By default, non-blocking experimental builders are omitted from the output.
Pass --experimental (-e) to include them.

Use --watch to continuously monitor checks until all blocking checks finish.
Use --fail-fast to exit immediately as soon as any check fails.
Use --web (-w) to open checks in the browser.`,
	Args: cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		rawID, err := ResolveTargetChangeID(cmd.Context(), cmd, args)
		if err != nil {
			return err
		}
		res, err := ResolveCIContext(cmd, rawID)
		if err != nil {
			return fmt.Errorf("failed to load checks for %q: %w", rawID, err)
		}

		if checksWeb {
			targetURL := fmt.Sprintf("https://%s/c/%s/+/%d", res.GerritHost, res.Change.Project, res.Change.Number)
			if res.PatchsetNum > 0 {
				targetURL = fmt.Sprintf("%s/%d", targetURL, res.PatchsetNum)
			}
			fmt.Fprintf(cmd.OutOrStdout(), "Opening %s in your browser.\n", targetURL)
			return OpenBrowserFn(targetURL)
		}

		ctx := cmd.Context()
		luciClient := NewLUCIClient(buildbucketHost, getLUCIHTTPClient(ctx, buildbucketHost))

		if checksFailFast {
			checksWatch = true
		}

		builds := deduplicateLatestBuilds(res.Builds)
		if checksWatch {
			interval := checksInterval
			if interval <= 0 {
				interval = 15 * time.Second
			}

			out := cmd.OutOrStdout()
			watchStart := time.Now()
			lastHeartbeat := time.Now()
			heartbeatInterval := 4 * interval
			if heartbeatInterval < 20*time.Millisecond {
				heartbeatInterval = 20 * time.Millisecond
			}

			completed := make(map[string]bool)
			announcedDiscovery := false

			firstIteration := true
			for {
				var relevant []bbBuild
				hasPending := false
				hasFailure := false
				passedCount := 0
				failedCount := 0
				runningCount := 0
				otherCount := 0

				for _, b := range builds {
					if !checksExperimental && b.IsExperimental() {
						continue
					}
					relevant = append(relevant, b)
					if b.Status == "SCHEDULED" || b.Status == "STARTED" {
						hasPending = true
						runningCount++
					} else if b.Status == "SUCCESS" {
						passedCount++
					} else if b.Status == "FAILURE" || b.Status == "INFRA_FAILURE" {
						hasFailure = true
						failedCount++
					} else {
						otherCount++
					}
				}

				totalCount := len(relevant)

				if len(relevant) == 0 {
					if firstIteration {
						fmt.Fprintln(out, "Waiting for checks to be scheduled...")
					}
				} else if !announcedDiscovery {
					statusSummary := formatChecksStatusBreakdown(passedCount, runningCount, failedCount, otherCount)
					if totalCount == 1 {
						fmt.Fprintf(out, "Watching 1 check for Change %d (Patchset %d) [%s]...\n", res.Change.Number, res.PatchsetNum, statusSummary)
					} else {
						fmt.Fprintf(out, "Watching %d checks for Change %d (Patchset %d) [%s]...\n", totalCount, res.Change.Number, res.PatchsetNum, statusSummary)
					}
					announcedDiscovery = true
				}

				anyNewlyCompleted := false
				if firstIteration {
					for _, b := range relevant {
						if b.Status == "SUCCESS" {
							completed[b.Builder.Builder] = true
						}
					}
				}

				for _, b := range relevant {
					bName := b.Builder.Builder
					if b.Status == "SUCCESS" {
						if !completed[bName] {
							completed[bName] = true
							anyNewlyCompleted = true
							dur := formatDuration(b.StartTime, b.EndTime)
							fmt.Fprintf(out, "✓  %s passed (%s) [%d/%d]\n", bName, dur, len(completed), totalCount)
						}
					} else if b.Status == "FAILURE" || b.Status == "INFRA_FAILURE" {
						if !completed[bName] {
							completed[bName] = true
							anyNewlyCompleted = true
							dur := formatDuration(b.StartTime, b.EndTime)
							fmt.Fprintf(out, "✗  %s failed (%s) [%d/%d]\n", bName, dur, len(completed), totalCount)
						}
					}
				}

				if checksFailFast && hasFailure {
					fmt.Fprintln(out, "--fail-fast triggered: stopping watch.")
					break
				}
				if !hasPending && len(relevant) > 0 {
					break
				}

				if !firstIteration && !anyNewlyCompleted && len(relevant) > 0 {
					if time.Since(lastHeartbeat) >= heartbeatInterval {
						elapsed := time.Since(watchStart).Round(time.Second)
						fmt.Fprintf(out, "[+%s] %d passed, %d running, %d failed (%d total)\n", elapsed, passedCount, runningCount, failedCount, totalCount)
						lastHeartbeat = time.Now()
					}
				} else if anyNewlyCompleted {
					lastHeartbeat = time.Now()
				}

				firstIteration = false

				select {
				case <-ctx.Done():
					return ctx.Err()
				case <-time.After(interval):
				}

				newBuilds, err := luciClient.SearchBuilds(ctx, res.GerritHost, res.Change.Project, res.Change.Number, res.PatchsetNum)
				if err != nil {
					select {
					case <-ctx.Done():
						return ctx.Err()
					default:
					}
				} else {
					builds = deduplicateLatestBuilds(newBuilds)
				}
			}
		}

		var checks []CheckItem
		var tally checkTally
		omittedCount := 0 // Experimental builders hidden from the output.
		for _, b := range builds {
			isExp := b.IsExperimental()
			if isExp {
				tally.experimental++
				if !checksExperimental {
					omittedCount++
					continue
				}
			}
			checks = append(checks, NewCheckItem(b))
			if isExp {
				// Experimental builders are non-blocking by definition: the
				// Commit-Queue ignores them, so a failing one must not make
				// `gh pr checks` report failure. --experimental controls what
				// is displayed, never whether the change is submittable --
				// otherwise the gate's answer would depend on a display flag.
				continue
			}
			tally.blocking++
			switch classifyCheckStatus(b.Status) {
			case checkFailed:
				tally.failed = append(tally.failed, b)
			case checkCanceled:
				tally.canceled = append(tally.canceled, b)
			case checkPending:
				tally.pending = append(tally.pending, b)
			}
		}
		failedBuilds := tally.failed

		data := map[string]any{
			"number":              res.Change.Number,
			"patchset":            res.PatchsetNum,
			"checks":              checks,
			"omittedExperimental": omittedCount,
		}

		r := &Renderer{
			Out:             cmd.OutOrStdout(),
			JSONFields:      checksJSON,
			Template:        checksTemplateStr,
			DefaultTemplate: defaultChecksTemplate,
		}

		if err := r.Render(data); err != nil {
			return fmt.Errorf("error rendering output: %w", err)
		}

		if checksWatch && checksLogFailed && len(failedBuilds) > 0 {
			var reports []FailureReport
			for _, fb := range failedBuilds {
				details, err := luciClient.GetBuildDetails(ctx, fb.ID)
				if err != nil {
					continue
				}
				rep := luciClient.ExtractFailureReport(ctx, details, 100)
				if rep != nil {
					reports = append(reports, *rep)
				}
			}
			if len(reports) > 0 {
				fmt.Fprintln(cmd.OutOrStdout())
				fmt.Fprint(cmd.OutOrStdout(), FormatFailureReports(reports))
			}
		}

		return checksExitStatus(res.Change.Number, res.PatchsetNum, tally)
	},
}

// checkTally is the gate-relevant summary of one patchset's blocking checks.
// Only blocking (non-experimental) builders are recorded in blocking, failed,
// canceled and pending; experimental counts the non-blocking builders that
// were seen, shown or not, and is used only to explain an empty result.
type checkTally struct {
	blocking     int
	failed       []bbBuild
	canceled     []bbBuild
	pending      []bbBuild
	experimental int
}

// checksExitStatus maps the resolved state of a change's checks onto the
// `gh pr checks` process exit code contract:
//
//	0 - at least one blocking check ran and every one of them passed
//	8 - nothing has failed or been canceled, but at least one check is running
//	1 - a check failed or was canceled, or no blocking checks were reported
//
// This contract must hold on every invocation, independent of --watch, --json
// and --template, because the exit code is the only signal a shell script or
// an autonomous agent has when using `gh pr checks` as a merge gate. Returning
// nil while checks are red or pending silently turns
// `gh pr checks && gh pr merge --cq` into an unconditional merge.
//
// It deliberately fails closed: "no checks reported" is an error rather than a
// success, because a change whose CI has not been scheduled yet is not a
// change whose CI has passed.
//
// Precedence is failed, then canceled, then pending. Canceled outranks pending
// because waiting cannot resolve a canceled build: reporting exit 8 would send
// `--watch` into a loop that can never reach a verdict.
func checksExitStatus(changeNum, patchset int, t checkTally) error {
	location := fmt.Sprintf("Change %d (Patchset %d)", changeNum, patchset)

	if len(t.failed) > 0 {
		var summary string
		if len(t.failed) == 1 {
			summary = fmt.Sprintf("check %q failed on %s", t.failed[0].Builder.Builder, location)
		} else {
			summary = fmt.Sprintf("%d of %d checks failed on %s: %s",
				len(t.failed), t.blocking, location, builderList(t.failed))
		}
		return NewExitCodeError(ExitCodeFailure,
			"%s.\n\n"+
				"To inspect the failure logs and step diagnostics:\n"+
				"  gh run view %d --log-failed\n"+
				"To rerun only the failed builders:\n"+
				"  gh run rerun %d --failed",
			summary, changeNum, changeNum)
	}

	// A canceled check did not pass, so this is not exit 0, but it also did
	// not fail. The wording must not say "failed": the usual cause is a newer
	// patchset superseding this one, and CL 472267/43 (46 passed, 29 canceled,
	// nothing failed) previously reported "29 of 75 checks failed".
	if len(t.canceled) > 0 {
		var summary string
		if len(t.canceled) == 1 {
			summary = fmt.Sprintf("check %q was canceled on %s", t.canceled[0].Builder.Builder, location)
		} else {
			summary = fmt.Sprintf("%d of %d checks were canceled on %s: %s",
				len(t.canceled), t.blocking, location, builderList(t.canceled))
		}
		return NewExitCodeError(ExitCodeFailure,
			"%s.\n\n"+
				"Checks are usually canceled because a newer patchset superseded this one.\n"+
				"To query the current patchset instead:\n"+
				"  gh pr checks %d\n"+
				"To start a fresh Commit-Queue dry run:\n"+
				"  gh pr edit %d --cq",
			summary, changeNum, changeNum)
	}

	if len(t.pending) > 0 {
		return NewExitCodeError(ExitCodePending,
			"%d of %d checks are still running on %s: %s.\n\n"+
				"Exit code %d means \"checks pending\", matching 'gh pr checks'; it is not a failure.\n"+
				"To block until every check finishes:\n"+
				"  gh pr checks %d --watch\n"+
				"To stop as soon as any check fails:\n"+
				"  gh pr checks %d --watch --fail-fast",
			len(t.pending), t.blocking, location, builderList(t.pending),
			ExitCodePending, changeNum, changeNum)
	}

	if t.blocking == 0 {
		if t.experimental > 0 {
			return NewExitCodeError(ExitCodeFailure,
				"no blocking checks reported on %s.\n\n"+
					"%d non-blocking experimental builder(s) ran, but experimental builders do not gate submission.\n"+
					"To see them:\n"+
					"  gh pr checks %d --experimental",
				location, t.experimental, changeNum)
		}
		return NewExitCodeError(ExitCodeFailure,
			"no checks reported on %s.\n\n"+
				"CI may not have been scheduled yet, or this change has no tryjobs configured.\n"+
				"To trigger a Commit-Queue dry run:\n"+
				"  gh pr edit %d --cq\n"+
				"To wait for checks to appear and finish:\n"+
				"  gh pr checks %d --watch",
			location, changeNum, changeNum)
	}

	return nil
}

// maxNamedBuilders caps how many builder names an error message spells out.
// CL 472267/43 has 29 canceled builders; listing them all produced a single
// 900-character line that buried the remediation steps underneath it.
const maxNamedBuilders = 5

// builderList renders the builder names for a set of builds as a sorted,
// comma-separated list, truncated to maxNamedBuilders with a count of the
// remainder. Sorting keeps the message deterministic regardless of the order
// Buildbucket happens to return builds in.
func builderList(builds []bbBuild) string {
	names := make([]string, 0, len(builds))
	for _, b := range builds {
		names = append(names, b.Builder.Builder)
	}
	sort.Strings(names)

	if len(names) <= maxNamedBuilders {
		return strings.Join(names, ", ")
	}
	return fmt.Sprintf("%s and %d more",
		strings.Join(names[:maxNamedBuilders], ", "), len(names)-maxNamedBuilders)
}

func init() {
	checksCmd.Flags().StringVar(&checksJSON, "json", "", "Output JSON with specified fields")
	checksCmd.Flags().StringVarP(&checksTemplateStr, "template", "t", "", "Format output using a Go template")
	checksCmd.Flags().BoolVarP(&checksExperimental, "experimental", "e", false, "Include non-blocking experimental checks")
	checksCmd.Flags().BoolVar(&checksWatch, "watch", false, "Watch checks until they finish")
	checksCmd.Flags().BoolVarP(&checksWeb, "web", "w", false, "Open checks in web browser")
	checksCmd.Flags().BoolVar(&checksFailFast, "fail-fast", false, "Exit immediately if any check fails when using --watch flag")
	checksCmd.Flags().DurationVarP(&checksInterval, "interval", "i", 15*time.Second, "Refresh interval when using --watch flag")
	checksCmd.Flags().BoolVar(&checksLogFailed, "log-failed", true, "Output failure reports and log snippets for failed checks")
	checksCmd.Flags().StringVar(&buildbucketHost, "buildbucket-host", "cr-buildbucket.appspot.com", "Buildbucket host to query")
	checksCmd.Flags().MarkHidden("buildbucket-host")

	PrCmd.AddCommand(checksCmd)
}
