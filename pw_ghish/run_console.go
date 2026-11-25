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
	"fmt"
	"io"
	"net/url"
	"slices"
	"strings"
	"text/tabwriter"

	"github.com/spf13/cobra"
)

const (
	defaultConsoleBuildLimit = 5
	maxConsoleBuildLimit     = 100
)

type runConsoleOptions struct {
	projects     []string
	limit        int
	status       string
	experimental bool
	all          bool
	jsonOutput   bool
}

var (
	miloHost       = defaultMiloHost
	runConsoleOpts runConsoleOptions
)

// ConsoleBuilderItem summarizes a discovered LUCI console builder and its recent
// builds for `gh run console --json`.
type ConsoleBuilderItem struct {
	Project             string                   `json:"project"`
	Bucket              string                   `json:"bucket"`
	Builder             string                   `json:"builder"`
	FullName            string                   `json:"fullName"`
	Consoles            []string                 `json:"consoles"`
	BuilderURL          string                   `json:"builderUrl"`
	Experimental        bool                     `json:"experimental"`
	DescriptionLinks    []BuilderDescriptionLink `json:"descriptionLinks"`
	Status              string                   `json:"status"`
	ConsecutiveFailures int                      `json:"consecutiveFailures"`
	RecentBuilds        []CheckItem              `json:"recentBuilds"`
	Error               string                   `json:"error,omitempty"`
}

func effectiveMiloHost() string {
	return cmp.Or(strings.TrimSpace(miloHost), defaultMiloHost)
}

func luciBuilderConsoleURL(project, bucket, builder string) string {
	return fmt.Sprintf("https://ci.chromium.org/p/%s/builders/%s/%s",
		url.PathEscape(project), url.PathEscape(bucket), url.PathEscape(builder))
}

func formatConsoleNextCommand(plan ConsoleSelectorPlan, extraFlag string) string {
	parts := []string{"gh", "run", "console"}
	for _, p := range plan.ExplicitProjects {
		parts = append(parts, "--project", sanitizeSingleLine(p))
	}
	for _, sel := range plan.Selectors {
		parts = append(parts, sanitizeSingleLine(sel.Raw))
	}
	if extraFlag != "" {
		parts = append(parts, extraFlag)
	}
	return strings.Join(parts, " ")
}

func buildConsoleItems(discovered []DiscoveredConsoleBuilder, results []BuilderSearchResult, errOut io.Writer) []ConsoleBuilderItem {
	items := make([]ConsoleBuilderItem, 0, len(discovered))
	for i, res := range results {
		disc := discovered[i]
		consoles := disc.Consoles
		if consoles == nil {
			consoles = []string{}
		}
		links := disc.DescriptionLinks
		if links == nil {
			links = []BuilderDescriptionLink{}
		}
		item := ConsoleBuilderItem{
			Project:          disc.Project,
			Bucket:           disc.Bucket,
			Builder:          disc.Builder,
			FullName:         disc.Project + "/" + disc.Bucket + "/" + disc.Builder,
			Consoles:         consoles,
			BuilderURL:       luciBuilderConsoleURL(disc.Project, disc.Bucket, disc.Builder),
			Experimental:     disc.Experimental,
			DescriptionLinks: links,
			RecentBuilds:     make([]CheckItem, 0, len(res.Builds)),
		}
		if res.Error != "" {
			cleanErr := sanitizeSingleLine(res.Error)
			item.Status = "ERROR"
			item.Error = cleanErr
			fmt.Fprintf(errOut, "Warning: %s: %s\n", sanitizeSingleLine(item.FullName), cleanErr)
			items = append(items, item)
			continue
		}

		var nonCanceled []bbBuild
		for _, b := range res.Builds {
			item.RecentBuilds = append(item.RecentBuilds, NewCheckItem(b))
			if !strings.EqualFold(b.Status, "CANCELED") {
				nonCanceled = append(nonCanceled, b)
			}
		}

		switch {
		case len(res.Builds) == 0:
			item.Status = "UNBUILT"
		case len(nonCanceled) == 0:
			item.Status = "CANCELED"
		default:
			item.Status = nonCanceled[0].Status
			for _, b := range nonCanceled {
				if b.Status == "FAILURE" || b.Status == "INFRA_FAILURE" {
					item.ConsecutiveFailures++
				} else {
					break
				}
			}
		}
		items = append(items, item)
	}
	sortConsoleItems(items)
	return items
}

func consoleStatusPriority(status string) int {
	switch status {
	case "FAILURE", "INFRA_FAILURE":
		return 0
	case "ERROR":
		return 1
	case "STARTED", "SCHEDULED", "CANCELED":
		return 2
	default: // SUCCESS, UNBUILT
		return 3
	}
}

func sortConsoleItems(items []ConsoleBuilderItem) {
	slices.SortFunc(items, func(a, b ConsoleBuilderItem) int {
		pa, pb := consoleStatusPriority(a.Status), consoleStatusPriority(b.Status)
		if pa != pb {
			return cmp.Compare(pa, pb)
		}
		return cmp.Compare(a.FullName, b.FullName)
	})
}

func consoleItemGlyph(status string) string {
	switch status {
	case "FAILURE", "INFRA_FAILURE", "ERROR", "CANCELED":
		return GlyphFail
	case "STARTED", "SCHEDULED":
		return GlyphRunning
	case "UNBUILT":
		return GlyphDot
	default:
		return GlyphPass
	}
}

func latestRelevantBuild(it ConsoleBuilderItem) (CheckItem, bool) {
	if len(it.RecentBuilds) == 0 {
		return CheckItem{}, false
	}
	if !strings.EqualFold(it.Status, "CANCELED") {
		for _, b := range it.RecentBuilds {
			if !strings.EqualFold(b.Status, "CANCELED") {
				return b, true
			}
		}
	}
	return it.RecentBuilds[0], true
}

func formatConsoleCountParts(passed, running, canceled, unbuilt int) string {
	parts := make([]string, 0, 4)
	if passed > 0 {
		parts = append(parts, fmt.Sprintf("%d passed", passed))
	}
	if running > 0 {
		parts = append(parts, fmt.Sprintf("%d running", running))
	}
	if canceled > 0 {
		parts = append(parts, fmt.Sprintf("%d canceled", canceled))
	}
	if unbuilt > 0 {
		parts = append(parts, fmt.Sprintf("%d unbuilt", unbuilt))
	}
	return strings.Join(parts, ", ")
}

func formatConsoleScanText(out io.Writer, items []ConsoleBuilderItem, plan ConsoleSelectorPlan, showAll, includeExperimental bool) {
	var failed, errCount, running, canceled, passed, unbuilt int
	firstFailedBuildID := ""
	var visible, tail []ConsoleBuilderItem

	for _, it := range items {
		switch it.Status {
		case "FAILURE", "INFRA_FAILURE":
			failed++
			if firstFailedBuildID == "" {
				if b, ok := latestRelevantBuild(it); ok {
					firstFailedBuildID = b.ID
				}
			}
			visible = append(visible, it)
		case "ERROR":
			errCount++
			visible = append(visible, it)
		case "STARTED", "SCHEDULED":
			running++
			visible = append(visible, it)
		case "CANCELED":
			canceled++
			visible = append(visible, it)
		case "UNBUILT":
			unbuilt++
			tail = append(tail, it)
		default:
			passed++
			tail = append(tail, it)
		}
	}

	omittedCount := 0
	if showAll || len(tail) <= maxExpandedPassingChecks {
		visible = append(visible, tail...)
	} else {
		visible = append(visible, tail[:maxExpandedPassingChecks]...)
		omittedCount = len(tail) - maxExpandedPassingChecks
	}

	if len(visible) > 0 {
		w := tabwriter.NewWriter(out, 0, 0, 2, ' ', 0)
		fmt.Fprintln(w, "STATUS\tBUILDER\tSTREAK\tLATEST\tURL")
		for _, it := range visible {
			streak := "-"
			latestID := sanitizeSingleLine(it.Status)
			urlStr := sanitizeSingleLine(it.BuilderURL)
			if it.Error != "" {
				latestID = "ERROR: " + TruncateVisible(sanitizeSingleLine(it.Error), 60)
			} else if b, ok := latestRelevantBuild(it); ok {
				latestID = sanitizeSingleLine(b.ID)
				if b.URL != "" {
					urlStr = sanitizeSingleLine(b.URL)
				}
				if it.ConsecutiveFailures > 0 {
					streak = fmt.Sprintf("%d fail", it.ConsecutiveFailures)
				} else {
					streak = sanitizeSingleLine(b.Status)
				}
			}
			fmt.Fprintf(w, "%s\t%s\t%s\t%s\t%s\n", consoleItemGlyph(it.Status), sanitizeSingleLine(it.FullName), streak, latestID, urlStr)
		}
		if omittedCount > 0 {
			fmt.Fprintf(w, "%s\t... and %d more passing/unbuilt builder(s) (pass --all to expand)\n", GlyphPass, omittedCount)
		}
		w.Flush()
	} else {
		fmt.Fprintln(out, "No console builders discovered.")
	}

	fmt.Fprintln(out)
	jsonCmd := formatConsoleNextCommand(plan, "--json")
	switch {
	case failed > 0:
		fmt.Fprintf(out, "Verdict: FAILED (%d of %s failing", failed, countNoun(len(items), "builder"))
		if errCount > 0 {
			fmt.Fprintf(out, ", %d error", errCount)
		}
		fmt.Fprintln(out, ")")
		if firstFailedBuildID != "" {
			fmt.Fprintf(out, "Next:    gh run view %s --log-failed\n", sanitizeSingleLine(firstFailedBuildID))
		} else {
			fmt.Fprintf(out, "Next:    %s\n", jsonCmd)
		}
	case errCount > 0:
		fmt.Fprintf(out, "Verdict: FAILED (%d of %s failed to query)\n", errCount, countNoun(len(items), "builder"))
		fmt.Fprintf(out, "Next:    %s\n", jsonCmd)
	case len(items) == 0:
		fmt.Fprintln(out, "Verdict: NO BUILDERS (0 builders matched console filter)")
		if includeExperimental {
			fmt.Fprintf(out, "Next:    %s\n", jsonCmd)
		} else {
			fmt.Fprintf(out, "Next:    %s\n", formatConsoleNextCommand(plan, "-e"))
		}
	case passed == 0:
		switch {
		case running > 0:
			fmt.Fprintf(out, "Verdict: PENDING (%s)\n", formatConsoleCountParts(0, running, canceled, unbuilt))
		case canceled > 0:
			fmt.Fprintf(out, "Verdict: CANCELED (%s)\n", formatConsoleCountParts(0, 0, canceled, unbuilt))
		default:
			fmt.Fprintf(out, "Verdict: INCONCLUSIVE (0 passed, %d unbuilt)\n", unbuilt)
		}
		fmt.Fprintf(out, "Next:    %s\n", jsonCmd)
	default:
		fmt.Fprintf(out, "Verdict: PASSED (%s)\n", formatConsoleCountParts(passed, running, canceled, unbuilt))
		fmt.Fprintf(out, "Next:    %s\n", jsonCmd)
	}
}

var runConsoleCmd = &cobra.Command{
	Use:          "console [<project>/<console>|<console>...] [flags]",
	Annotations:  map[string]string{KindAnnotation: KindList},
	Short:        "Scan LUCI Milo consoles and recent builder status via LUCI RPCs",
	SilenceUsage: true,
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		if runConsoleOpts.limit <= 0 || runConsoleOpts.limit > maxConsoleBuildLimit {
			return fmt.Errorf("invalid --limit %d: must be between 1 and %d", runConsoleOpts.limit, maxConsoleBuildLimit)
		}
		if _, err := ParseRunStatusFilter(runConsoleOpts.status); err != nil {
			return err
		}

		projCfg, err := LoadCommandProjectConfig(cmd)
		if err != nil {
			return err
		}
		bbHost, err := effectiveBuildbucketHost(cmd, projCfg)
		if err != nil {
			return err
		}

		defaultProj := ""
		if selectorsNeedDefaultProject(args, runConsoleOpts.projects) {
			defaultProj = resolveDefaultLUCIProject(cmd, projCfg)
		}
		plan, err := ParseConsoleSelectors(runConsoleOpts.projects, args, defaultProj)
		if err != nil {
			return err
		}

		httpClient := getLUCIHTTPClient()
		miloClient := NewLUCIClient(effectiveMiloHost(), httpClient)
		bbClient := NewLUCIClient(bbHost, httpClient)

		discovered, err := DiscoverLUCIConsoleBuilders(ctx, miloClient, bbClient, plan, runConsoleOpts.experimental, cmd.ErrOrStderr())
		if err != nil {
			return err
		}
		if len(discovered) == 0 {
			if runConsoleOpts.jsonOutput {
				return writeIndentedJSON(cmd.OutOrStdout(), []ConsoleBuilderItem{})
			}
			formatConsoleScanText(cmd.OutOrStdout(), nil, plan, runConsoleOpts.all || VerboseFlag, runConsoleOpts.experimental)
			return nil
		}

		bbBuilders := make([]bbBuilder, len(discovered))
		for i, d := range discovered {
			bbBuilders[i] = bbBuilder{Project: d.Project, Bucket: d.Bucket, Builder: d.Builder}
		}

		batchResults, err := bbClient.SearchBuilderBuildsBatch(ctx, bbBuilders, runConsoleOpts.limit, runConsoleOpts.status)
		if err != nil {
			return err
		}

		items := buildConsoleItems(discovered, batchResults, cmd.ErrOrStderr())
		if runConsoleOpts.jsonOutput {
			return writeIndentedJSON(cmd.OutOrStdout(), items)
		}
		formatConsoleScanText(cmd.OutOrStdout(), items, plan, runConsoleOpts.all || VerboseFlag, runConsoleOpts.experimental)
		return nil
	},
}

func registerRunConsoleFlags() {
	//ghish:flag mode=extension reason="LUCI project(s) to scan for consoles via Milo RPCs"
	runConsoleCmd.Flags().StringSliceVar(&runConsoleOpts.projects, "project", nil, "LUCI project name to scan (repeatable) (ghish-only)")
	//ghish:flag mode=extension reason="Maximum recent builds fetched per console builder"
	runConsoleCmd.Flags().IntVarP(&runConsoleOpts.limit, "limit", "L", defaultConsoleBuildLimit, "Recent builds to fetch per builder (ghish-only)")
	//ghish:flag mode=extension reason="Filter fetched builds by Buildbucket status"
	runConsoleCmd.Flags().StringVarP(&runConsoleOpts.status, "status", "s", "", "Filter builds by status (completed, in_progress, queued, success, failure, infra_failure, canceled) (ghish-only)")
	//ghish:flag mode=extension reason="Include experimental and non-production console builders"
	runConsoleCmd.Flags().BoolVarP(&runConsoleOpts.experimental, "experimental", "e", false, "Include experimental and non_production console builders (ghish-only)")
	//ghish:flag mode=extension reason="Show all passing and unbuilt console builders without collapsing"
	runConsoleCmd.Flags().BoolVar(&runConsoleOpts.all, "all", false, "Show all console builders without collapsing passing/unbuilt entries (ghish-only)")
	//ghish:flag mode=extension reason="Emit structured JSON array of ConsoleBuilderItem objects"
	runConsoleCmd.Flags().BoolVar(&runConsoleOpts.jsonOutput, "json", false, "Output console scan items as JSON (ghish-only)")
	//ghish:flag mode=extension reason="Hidden test/custom LUCI Milo host override"
	runConsoleCmd.Flags().StringVar(&miloHost, "milo-host", defaultMiloHost, "LUCI Milo host to query (ghish-only)")
	_ = runConsoleCmd.Flags().MarkHidden("milo-host")
	//ghish:flag mode=extension reason="Hidden test/custom Buildbucket host override"
	runConsoleCmd.Flags().StringVar(&buildbucketHost, "buildbucket-host", "cr-buildbucket.appspot.com", "Buildbucket host to query (ghish-only)")
	_ = runConsoleCmd.Flags().MarkHidden("buildbucket-host")
}
