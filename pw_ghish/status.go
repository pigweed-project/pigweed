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
	"maps"
	"slices"
	"strings"
	"sync"

	"github.com/spf13/cobra"
)

var (
	statusJSON        string
	statusTemplateStr string
	statusAll         bool
)

const defaultStatusTemplate = `{{if .current_branch}}Current branch
{{if .current_branch.number}}  #{{.current_branch.number}}  {{.current_branch.title}}
    Branch:      {{.current_branch.branch}}{{if .current_branch.patchset}} (Patchset {{.current_branch.patchset}}){{end}}
    Status:      {{.current_branch.status}}
    Submittable: {{.current_branch.submittable}}{{if .current_branch.blockers}} (Blockers: {{.current_branch.blockers}}){{end}}
{{if .current_branch.labels}}    Labels:
{{range .current_branch.labels}}      {{.Name}}: {{.Value}}
{{end}}{{end}}{{if .current_branch.checks_summary}}    Checks:      {{.current_branch.checks_summary}}
{{end}}{{if .current_branch.comments_summary}}    Comments:{{.current_branch.comments_summary}}
{{end}}{{else}}  {{.current_branch.message}}
{{end}}
{{end}}{{if .authenticated}}Created by you{{if .time_filtered}} (last 30 days){{end}}:
{{if .created_by_you}}{{range .created_by_you}} {{if .attention}}*{{else}} {{end}} #{{.number}}  {{printf "%-50.50s" .title}} [CR: {{printf "%+d" .cr_score}}, V: {{printf "%+d" .v_score}}]
{{if .submittable}}    Submittable: true
{{end}}{{if .blockers}}    Blockers: {{.blockers}}
{{end}}{{end}}{{else}}  None
{{end}}{{if .more_created}}  ... and {{.more_created}} older changes (use -A / --all to show all)
{{end}}
Requesting a code review from you{{if .time_filtered}} (last 30 days){{end}}:
{{if .reviewing}}{{range .reviewing}} {{if .attention}}*{{else}} {{end}} #{{.number}}  {{printf "%-50.50s" .title}} by {{.author}} [CR: {{printf "%+d" .cr_score}}, V: {{printf "%+d" .v_score}}]
{{if .submittable}}    Submittable: true
{{end}}{{if .blockers}}    Blockers: {{.blockers}}
{{end}}{{end}}{{else}}  None
{{end}}{{if .more_reviewing}}  ... and {{.more_reviewing}} older changes (use -A / --all to show all)
{{end}}
Verdict: {{.verdict}}
Next: gh pr view <id>  (or gh pr checks)
{{else}}Personal changes and review requests skipped ({{.verdict}}).

Verdict: {{.verdict}}
Next: gh auth status
{{end}}`

var statusCmd = &cobra.Command{
	Use:         "status",
	Annotations: map[string]string{KindAnnotation: KindList},
	Short:       "Show status of relevant pull requests",
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		cfg := GetConfig(cmd)
		projCfg, err := LoadCommandProjectConfig(cmd)
		if err != nil {
			return err
		}

		client, err := NewGerritClient(ctx, cmd)
		if err != nil {
			return fmt.Errorf("failed to create Gerrit client: %w", err)
		}

		var (
			selfAccount *AccountInfo
			selfErr     error
		)
		selfRef, selfAccount := CachedSelfAccountRef(ctx, cmd, client)

		// Prepare queries
		var myQuery string
		var reviewingQuery string
		if statusAll {
			myQuery = fmt.Sprintf("owner:%s status:open", selfRef)
			reviewingQuery = fmt.Sprintf("reviewer:%s status:open -owner:%s", selfRef, selfRef)
		} else {
			myQuery = fmt.Sprintf("owner:%s status:open -age:30d", selfRef)
			reviewingQuery = fmt.Sprintf("reviewer:%s status:open -owner:%s -age:30d", selfRef, selfRef)
		}

		queries := []string{myQuery, reviewingQuery}
		statusChangeFields := []string{"DETAILED_LABELS", "SUBMITTABLE", "DETAILED_ACCOUNTS", "SUBMIT_REQUIREMENTS"}

		var (
			rootWg sync.WaitGroup

			// 1. Current branch data
			currentBranchData map[string]any

			// 3. My changes
			myBatches  = make([][]ChangeInfo, len(queries))
			batchesErr error

			moreCreated, moreReviewing int
		)

		// Pipeline 1: Inspect Active PR / Current branch
		rootWg.Add(1)
		go func() {
			defer rootWg.Done()
			activeID, aErr := ResolveActiveChangeID(ctx, cfg)
			if aErr != nil || activeID == "" {
				currBranch := ""
				if cfg != nil && cfg.Git != nil {
					currBranch, _ = cfg.GitClient().CurrentBranch(ctx)
				}
				var msg string
				if currBranch != "" {
					msg = fmt.Sprintf("There is no pull request associated with the current branch %q.\n    - Find open changes:   gh pr list\n    - Check out a change:  gh pr checkout <id>\n    - Create a change:     gh pr create", currBranch)
				} else {
					msg = "There is no pull request associated with the current branch.\n    - Find open changes:   gh pr list\n    - Check out a change:  gh pr checkout <id>\n    - Create a change:     gh pr create"
				}
				currentBranchData = map[string]any{
					"message": msg,
				}
				return
			}

			var (
				hostWg sync.WaitGroup
				gHost  string
			)
			hostWg.Add(1)
			go func() {
				defer hostWg.Done()
				gHost = cfg.GerritHost(ctx)
			}()

			activeChange, cErr := client.GetChange(ctx, activeID, "DETAILED_LABELS", "ALL_REVISIONS", "DETAILED_ACCOUNTS", "SUBMITTABLE", "SUBMIT_REQUIREMENTS")
			hostWg.Wait()
			if cErr != nil || activeChange == nil {
				currentBranchData = map[string]any{
					"message": fmt.Sprintf("Failed to load active change %s: %v", activeID, cErr),
				}
				return
			}

			patchsetNum := 0
			if activeChange.Revisions != nil && activeChange.CurrentRevision != "" {
				if rev, ok := activeChange.Revisions[activeChange.CurrentRevision]; ok {
					patchsetNum = rev.Number
				}
			}

			blockers := extractBlockers(activeChange)

			srNames := make(map[string]bool, len(activeChange.SubmitRequirements))
			for _, sr := range activeChange.SubmitRequirements {
				if sr.Name != "" {
					srNames[sr.Name] = true
				}
			}

			type labelItem struct {
				Name  string `json:"name"`
				Value string `json:"value"`
			}
			var labelItems []labelItem
			rank := func(n string) int {
				switch n {
				case "Code-Review":
					return 1
				case "Presubmit-Verified", "Verified":
					return 2
				case "Lint", "Copybara-Verified":
					return 3
				case "Commit-Queue", "Presubmit-Ready", "Autosubmit", "Auto-Submit":
					return 4
				default:
					return 10
				}
			}
			labelNames := slices.SortedFunc(maps.Keys(activeChange.Labels), func(a, b string) int {
				if c := cmp.Compare(rank(a), rank(b)); c != 0 {
					return c
				}
				return cmp.Compare(a, b)
			})
			for _, name := range labelNames {
				info := activeChange.Labels[name]
				summary := getLabelSummary(info)
				isCore := name == "Code-Review" || name == "Presubmit-Verified" || name == "Lint" || name == "Commit-Queue" || srNames[name]
				if isCore || summary != "No score" {
					labelItems = append(labelItems, labelItem{
						Name:  name,
						Value: summary,
					})
				}
			}

			var checks []CheckItem
			var checksSummary string

			var (
				subWg             sync.WaitGroup
				publishedComments map[string][]CommentInfo
				draftComments     map[string][]CommentInfo
				commentsErr       error
				draftsErr         error
			)

			if patchsetNum > 0 && activeChange.Project != "" && gHost != "" {
				patchsets := EquivalentPatchsets(activeChange, patchsetNum)
				subWg.Add(1)
				go func() {
					defer subWg.Done()
					provider := ResolveCIProvider(cfg, projCfg, client, gHost, activeChange)
					builds, bErr := SearchProviderBuildsForPatchsets(ctx, provider, gHost, activeChange.Number, patchsets, activeChange)
					if bErr == nil && builds != nil {
						deduped := deduplicateLatestBuilds(builds)
						visibleBuilds, _ := FilterBuildsByTags(deduped, projCfg.CI.HideTagFilters)
						checksSummary = formatCheckSummary(visibleBuilds, activeChange.Number)
						checks = BuildCheckItems(visibleBuilds)
					} else if ExitCodeFor(bErr) == ExitCodeAuth {
						checksSummary = GlyphFail + " LUCI authentication required (run 'luci-auth login' or 'gh auth status')"
					}
				}()
			}

			subWg.Add(1)
			go func() {
				defer subWg.Done()
				publishedComments, commentsErr = client.ListChangeComments(ctx, activeID)
			}()

			if client.Authenticated() {
				subWg.Add(1)
				go func() {
					defer subWg.Done()
					draftComments, draftsErr = client.ListChangeDrafts(ctx, activeID)
				}()
			}

			subWg.Wait()

			commentsSummary := AnalyzeComments(publishedComments, draftComments)
			if commentsErr != nil {
				commentsSummary.FormattedText = "    " + GlyphFail + " Failed to load comments"
			} else if draftsErr != nil {
				commentsSummary.FormattedText += " (" + GlyphFail + " failed to load drafts)"
			}

			currentBranchData = map[string]any{
				"number":           activeChange.Number,
				"title":            activeChange.Subject,
				"branch":           activeChange.Branch,
				"status":           activeChange.Status,
				"patchset":         patchsetNum,
				"submittable":      activeChange.Submittable,
				"blockers":         strings.Join(blockers, ", "),
				"labels":           labelItems,
				"checks":           checks,
				"checks_summary":   checksSummary,
				"comments":         commentsSummary,
				"comments_summary": commentsSummary.FormattedText,
			}
		}()

		if client.Authenticated() {
			// Pipeline 2: Self account (when not already resolved from cache above)
			if selfAccount == nil && selfErr == nil {
				rootWg.Add(1)
				go func() {
					defer rootWg.Done()
					var gHost string
					if cfg != nil {
						gHost = cfg.GerritHost(ctx)
					}
					acc, err := GetSelfAccountCached(ctx, client, gHost)
					selfErr = err
					if err == nil {
						selfAccount = acc
					}
				}()
			}

			// Pipeline 3: Created by you and reviewing
			rootWg.Add(1)
			go func() {
				defer rootWg.Done()
				myBatches, batchesErr = client.QueryChangesMulti(ctx, queries, statusChangeFields...)
			}()

			// Pipeline 4: Older change counts
			rootWg.Add(1)
			go func() {
				defer rootWg.Done()
				if !statusAll {
					olderQueries := []string{
						fmt.Sprintf("owner:%s status:open age:30d limit:100", selfRef),
						fmt.Sprintf("reviewer:%s status:open -owner:%s age:30d limit:100", selfRef, selfRef),
					}
					batches, _ := client.QueryChangesMulti(ctx, olderQueries)
					moreCreated, moreReviewing = len(batches[0]), len(batches[1])
				}
			}()
		}

		rootWg.Wait()

		if selfErr != nil {
			return fmt.Errorf("failed to get self account: %w", selfErr)
		}
		if batchesErr != nil {
			return fmt.Errorf("failed to query my changes: %w", batchesErr)
		}

		var myAccountID int
		if selfAccount != nil {
			myAccountID = selfAccount.AccountID
		}

		processChanges := func(changes []ChangeInfo) []map[string]any {
			result := []map[string]any{} // [] rather than null in --json output
			for _, change := range changes {
				crScore := ExtractLabelScore(change.Labels, "Code-Review")
				vScore := ExtractLabelScore(change.Labels, "Verified")
				if _, hasVerified := change.Labels["Verified"]; !hasVerified {
					vScore = ExtractLabelScore(change.Labels, "Presubmit-Verified")
				}

				attnMap := BuildAttentionMap(change.AttentionSet)
				inAttentionSet := attnMap[myAccountID]

				blockers := extractBlockers(&change)

				owner := FormatAccount(change.Owner)

				result = append(result, map[string]any{
					"number":      change.Number,
					"title":       change.Subject,
					"state":       change.Status,
					"author":      owner,
					"cr_score":    crScore,
					"v_score":     vScore,
					"submittable": change.Submittable,
					"blockers":    strings.Join(blockers, ", "),
					"attention":   inAttentionSet,
					"labels":      change.Labels,
				})
			}
			return result
		}

		createdByYou := processChanges(myBatches[0])
		reviewing := processChanges(myBatches[1])
		verdict := statusVerdict(createdByYou, reviewing)
		if !client.Authenticated() {
			host := CleanGerritHost(HostFlag)
			if host == "" && cfg != nil {
				host = CleanGerritHost(cfg.Host)
			}
			if host == "" {
				host = CleanGerritHost(client.BaseURL())
			}
			verdict = fmt.Sprintf("not signed in to %s", cmp.Or(host, "Gerrit"))
		}
		data := map[string]any{
			"authenticated":  client.Authenticated(),
			"current_branch": currentBranchData,
			"created_by_you": createdByYou,
			"reviewing":      reviewing,
			"verdict":        verdict,
			"time_filtered":  !statusAll,
			"more_created":   moreCreated,
			"more_reviewing": moreReviewing,
		}

		r := &Renderer{
			Out:             cmd.OutOrStdout(),
			JSONFields:      statusJSON,
			Template:        statusTemplateStr,
			DefaultTemplate: defaultStatusTemplate,
		}

		if err := r.Render(data); err != nil {
			return fmt.Errorf("failed to render output: %w", err)
		}
		return nil
	},
}

func init() {
	statusCmd.Flags().StringVar(&statusJSON, "json", "", "Output JSON with specified fields")
	statusCmd.Flags().StringVarP(&statusTemplateStr, "template", "t", "", "Format output using a Go template")
	statusCmd.Flags().BoolVarP(&statusAll, "all", "A", false, "Show all open changes (bypasses 30-day time filter)")
	statusCmd.Flags().StringVar(&buildbucketHost, "buildbucket-host", "cr-buildbucket.appspot.com", "Buildbucket host to query")
	statusCmd.Flags().MarkHidden("buildbucket-host")
	PrCmd.AddCommand(statusCmd)
}

// statusVerdict summarizes `pr status` in one line: how many of your own
// changes and of the review requests have you in their attention set. The
// counts cover only the lists shown (the last 30 days unless --all, and
// subject to the query limit), not every open change.
func statusVerdict(createdByYou, reviewing []map[string]any) string {
	countAttention := func(changes []map[string]any) int {
		n := 0
		for _, c := range changes {
			if attn, _ := c["attention"].(bool); attn {
				n++
			}
		}
		return n
	}
	yours, reviews := countAttention(createdByYou), countAttention(reviewing)
	if yours == 0 && reviews == 0 {
		return fmt.Sprintf("nothing awaits you (%s open, %s)",
			countNoun(len(createdByYou), "change"), countNoun(len(reviewing), "review request"))
	}
	changeVerb, reviewVerb := "need", "await"
	if yours == 1 {
		changeVerb = "needs"
	}
	if reviews == 1 {
		reviewVerb = "awaits"
	}
	return fmt.Sprintf("%d of your %s %s attention, %s %s you",
		yours, countNoun(len(createdByYou), "change"), changeVerb, countNoun(reviews, "review request"), reviewVerb)
}

// countNoun formats n with noun, adding a plural "s" unless n is 1.
func countNoun(n int, noun string) string {
	if n == 1 {
		return "1 " + noun
	}
	return fmt.Sprintf("%d %ss", n, noun)
}

// ExtractLabelScore extracts the decisive score for a label from ApprovalInfo entries.
// In Gerrit, any negative score (e.g., -1, -2) blocks submission or indicates attention needed,
// otherwise the highest positive score wins.
func ExtractLabelScore(labels map[string]LabelInfo, labelName string) int {
	info, ok := labels[labelName]
	if !ok {
		return 0
	}
	score, _ := castVote(info)
	return score
}

func labelHasNegativeVote(info LabelInfo) bool {
	if info.Rejected.AccountID != 0 || info.Disliked.AccountID != 0 || info.Value < 0 {
		return true
	}
	if v, voted := castVote(info); voted && v < 0 {
		return true
	}
	return false
}

// extractBlockers computes blocking reasons for an unsubmitted change.
func extractBlockers(change *ChangeInfo) []string {
	if change == nil || change.Submittable {
		return nil
	}
	var blockers []string
	if len(change.SubmitRequirements) > 0 {
		seen := make(map[string]bool, len(change.SubmitRequirements))
		for _, sr := range change.SubmitRequirements {
			if !strings.EqualFold(sr.Status, "UNSATISFIED") && !strings.EqualFold(sr.Status, "ERROR") {
				continue
			}
			name := sr.Name
			if name == "" {
				continue
			}
			seen[name] = true
			if info, ok := change.Labels[name]; ok && labelHasNegativeVote(info) {
				blockers = append(blockers, fmt.Sprintf("%s (Rejected)", name))
				continue
			}
			switch name {
			case "Code-Review":
				blockers = append(blockers, "Code-Review (+2 required)")
			case "Verified":
				blockers = append(blockers, "Verified (+1 required)")
			case "Presubmit-Verified":
				blockers = append(blockers, "Presubmit-Verified (+1 required)")
			default:
				blockers = append(blockers, fmt.Sprintf("%s (Unsatisfied)", name))
			}
		}
		var rejectedNames []string
		for name, info := range change.Labels {
			if seen[name] {
				continue
			}
			if labelHasNegativeVote(info) {
				rejectedNames = append(rejectedNames, name)
			}
		}
		slices.Sort(rejectedNames)
		for _, name := range rejectedNames {
			blockers = append(blockers, fmt.Sprintf("%s (Rejected)", name))
		}
		return blockers
	}

	if cr, ok := change.Labels["Code-Review"]; ok {
		if cr.Approved.AccountID == 0 {
			blockers = append(blockers, "Code-Review (+2 required)")
		}
	}
	if v, ok := change.Labels["Verified"]; ok {
		if v.Approved.AccountID == 0 && v.Recommended.AccountID == 0 {
			blockers = append(blockers, "Verified (+1 required)")
		}
	}
	for name, info := range change.Labels {
		if labelHasNegativeVote(info) {
			blockers = append(blockers, fmt.Sprintf("%s (Rejected)", name))
		}
	}
	return blockers
}
