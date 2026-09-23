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
	"net/http"
	"sort"
	"strings"
	"sync"

	"github.com/andygrunwald/go-gerrit"
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
{{end}}Created by you{{if .time_filtered}} (last 30 days){{end}}:
{{if .created_by_you}}{{range .created_by_you}} {{if .attention}}*{{else}} {{end}} #{{.number}}  {{printf "%-50.50s" .title}} [CR: {{printf "%+d" .cr_score}}, V: {{printf "%+d" .v_score}}]
    Submittable: {{.submittable}}
{{if .blockers}}    Blockers: {{.blockers}}
{{end}}{{end}}{{else}}  None
{{end}}{{if .more_created}}  ... and {{.more_created}} older changes (use -A / --all to show all)
{{end}}
Requesting a code review from you{{if .time_filtered}} (last 30 days){{end}}:
{{if .reviewing}}{{range .reviewing}} {{if .attention}}*{{else}} {{end}} #{{.number}}  {{printf "%-50.50s" .title}} [CR: {{printf "%+d" .cr_score}}, V: {{printf "%+d" .v_score}}]
    Submittable: {{.submittable}}
{{if .blockers}}    Blockers: {{.blockers}}
{{end}}{{end}}{{else}}  None
{{end}}{{if .more_reviewing}}  ... and {{.more_reviewing}} older changes (use -A / --all to show all)
{{end}}`

var statusCmd = &cobra.Command{
	Use:   "status",
	Short: "Show status of relevant pull requests",
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		cfg := GetConfig(cmd)

		client, err := NewGerritClient(ctx, cmd)
		if err != nil {
			return fmt.Errorf("failed to create Gerrit client: %w", err)
		}

		// Prepare queries
		var myQuery string
		var reviewingQuery string
		if statusAll {
			myQuery = "owner:self status:open"
			reviewingQuery = "reviewer:self status:open -owner:self"
		} else {
			myQuery = "owner:self status:open -age:30d"
			reviewingQuery = "reviewer:self status:open -owner:self -age:30d"
		}

		myChangesOpt := &gerrit.QueryChangeOptions{
			QueryOptions: gerrit.QueryOptions{
				Query: []string{myQuery},
			},
			ChangeOptions: gerrit.ChangeOptions{
				AdditionalFields: []string{"DETAILED_LABELS", "SUBMITTABLE", "DETAILED_ACCOUNTS"},
			},
		}

		reviewingChangesOpt := &gerrit.QueryChangeOptions{
			QueryOptions: gerrit.QueryOptions{
				Query: []string{reviewingQuery},
			},
			ChangeOptions: gerrit.ChangeOptions{
				AdditionalFields: []string{"DETAILED_LABELS", "SUBMITTABLE", "DETAILED_ACCOUNTS"},
			},
		}

		olderMyOpt := &gerrit.QueryChangeOptions{
			QueryOptions: gerrit.QueryOptions{
				Query: []string{"owner:self status:open age:30d"},
				Limit: 100,
			},
		}

		olderReviewingOpt := &gerrit.QueryChangeOptions{
			QueryOptions: gerrit.QueryOptions{
				Query: []string{"reviewer:self status:open -owner:self age:30d"},
				Limit: 100,
			},
		}

		var (
			rootWg sync.WaitGroup

			// 1. Current branch data
			currentBranchData map[string]any

			// 2. Self account
			selfAccount *gerrit.AccountInfo
			selfErr     error

			// 3. My changes
			myChanges   *[]gerrit.ChangeInfo
			myErr       error
			moreCreated int

			// 4. Reviewing changes
			reviewingChanges *[]gerrit.ChangeInfo
			reviewingErr     error
			moreReviewing    int
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

			opt := &gerrit.ChangeOptions{
				AdditionalFields: []string{"DETAILED_LABELS", "CURRENT_REVISION", "DETAILED_ACCOUNTS", "SUBMITTABLE"},
			}
			activeChange, _, cErr := client.Changes.GetChange(ctx, activeID, opt)
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

			type labelItem struct {
				Name  string `json:"name"`
				Value string `json:"value"`
			}
			var labelItems []labelItem
			var labelNames []string
			for k := range activeChange.Labels {
				labelNames = append(labelNames, k)
			}
			sort.Slice(labelNames, func(i, j int) bool {
				rank := func(n string) int {
					switch n {
					case "Code-Review":
						return 1
					case "Presubmit-Verified":
						return 2
					case "Lint":
						return 3
					case "Commit-Queue":
						return 4
					default:
						return 10
					}
				}
				rI, rJ := rank(labelNames[i]), rank(labelNames[j])
				if rI != rJ {
					return rI < rJ
				}
				return labelNames[i] < labelNames[j]
			})
			for _, name := range labelNames {
				info := activeChange.Labels[name]
				summary := getLabelSummary(info)
				isCore := name == "Code-Review" || name == "Presubmit-Verified" || name == "Lint" || name == "Commit-Queue"
				if isCore || summary != "No score" {
					labelItems = append(labelItems, labelItem{
						Name:  name,
						Value: summary,
					})
				}
			}

			var checks []CheckItem
			var checksSummary string
			gHost := cfg.GerritHost(ctx)

			bbHost := buildbucketHost
			if bbHost == "" {
				bbHost = "cr-buildbucket.appspot.com"
			}

			var (
				subWg             sync.WaitGroup
				publishedComments map[string][]gerrit.CommentInfo
				draftComments     map[string][]gerrit.CommentInfo
			)

			if patchsetNum > 0 && activeChange.Project != "" && gHost != "" {
				subWg.Add(1)
				go func() {
					defer subWg.Done()
					if builds, bErr := queryBuildbucket(ctx, bbHost, gHost, activeChange.Project, activeChange.Number, patchsetNum, http.DefaultClient); bErr == nil && builds != nil {
						checksSummary = formatCheckSummary(builds)
						checks = BuildCheckItems(builds)
					}
				}()
			}

			subWg.Add(1)
			go func() {
				defer subWg.Done()
				if cMap, _, err := client.Changes.ListChangeComments(ctx, activeID); err == nil && cMap != nil {
					publishedComments = *cMap
				}
			}()

			subWg.Add(1)
			go func() {
				defer subWg.Done()
				if dMap, _, err := client.Changes.ListChangeDrafts(ctx, activeID); err == nil && dMap != nil {
					draftComments = *dMap
				}
			}()

			subWg.Wait()

			commentsSummary := AnalyzeComments(publishedComments, draftComments)

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

		// Pipeline 2: Self account
		rootWg.Add(1)
		go func() {
			defer rootWg.Done()
			var acc *gerrit.AccountInfo
			acc, _, selfErr = client.Accounts.GetAccount(ctx, "self")
			if selfErr == nil {
				selfAccount = acc
			}
		}()

		// Pipeline 3: Created by you
		rootWg.Add(1)
		go func() {
			defer rootWg.Done()
			var mySubWg sync.WaitGroup
			mySubWg.Add(1)
			go func() {
				defer mySubWg.Done()
				myChanges, _, myErr = client.Changes.QueryChanges(ctx, myChangesOpt)
			}()
			if !statusAll {
				mySubWg.Add(1)
				go func() {
					defer mySubWg.Done()
					if olderMy, _, err := client.Changes.QueryChanges(ctx, olderMyOpt); err == nil && olderMy != nil {
						moreCreated = len(*olderMy)
					}
				}()
			}
			mySubWg.Wait()
		}()

		// Pipeline 4: Reviewing
		rootWg.Add(1)
		go func() {
			defer rootWg.Done()
			var revSubWg sync.WaitGroup
			revSubWg.Add(1)
			go func() {
				defer revSubWg.Done()
				reviewingChanges, _, reviewingErr = client.Changes.QueryChanges(ctx, reviewingChangesOpt)
			}()
			if !statusAll {
				revSubWg.Add(1)
				go func() {
					defer revSubWg.Done()
					if olderReviewing, _, err := client.Changes.QueryChanges(ctx, olderReviewingOpt); err == nil && olderReviewing != nil {
						moreReviewing = len(*olderReviewing)
					}
				}()
			}
			revSubWg.Wait()
		}()

		rootWg.Wait()

		if selfErr != nil {
			return fmt.Errorf("failed to get self account: %w", selfErr)
		}
		if myErr != nil {
			return fmt.Errorf("failed to query my changes: %w", myErr)
		}
		if reviewingErr != nil {
			return fmt.Errorf("failed to query changes I'm reviewing: %w", reviewingErr)
		}

		myAccountID := selfAccount.AccountID

		processChanges := func(changes []gerrit.ChangeInfo) []map[string]any {
			var result []map[string]any
			for _, change := range changes {
				crScore := extractLabelScore(change.Labels, "Code-Review")
				vScore := extractLabelScore(change.Labels, "Verified")

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

		var myChangesList []gerrit.ChangeInfo
		if myChanges != nil {
			myChangesList = *myChanges
		}
		var reviewingChangesList []gerrit.ChangeInfo
		if reviewingChanges != nil {
			reviewingChangesList = *reviewingChanges
		}

		data := map[string]any{
			"current_branch": currentBranchData,
			"created_by_you": processChanges(myChangesList),
			"reviewing":      processChanges(reviewingChangesList),
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

// ExtractLabelScore extracts the decisive score for a label from ApprovalInfo entries.
// In Gerrit, any negative score (e.g., -1, -2) blocks submission or indicates attention needed,
// otherwise the highest positive score wins.
func ExtractLabelScore(labels map[string]gerrit.LabelInfo, labelName string) int {
	return extractLabelScore(labels, labelName)
}

func extractLabelScore(labels map[string]gerrit.LabelInfo, labelName string) int {
	info, ok := labels[labelName]
	if !ok {
		return 0
	}
	score, _ := castVote(info)
	return score
}

// extractBlockers computes blocking reasons for an unsubmitted change.
func extractBlockers(change *gerrit.ChangeInfo) []string {
	if change.Submittable {
		return nil
	}
	var blockers []string
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
		if info.Rejected.AccountID != 0 || info.Disliked.AccountID != 0 {
			blockers = append(blockers, fmt.Sprintf("%s (Rejected)", name))
		}
	}
	return blockers
}
