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
	"strings"

	"github.com/andygrunwald/go-gerrit"
	"github.com/spf13/cobra"
)

var (
	listLimit        int
	listState        string
	listLabel        []string
	listBase         string
	listAssignee     string
	listAuthor       string
	listSearch       string
	listReviewer     string
	listMergedAfter  string
	listMergedBefore string
	listJSON         string
	listTemplateStr  string
)

const defaultListTemplate = `ID         STATUS     SUBJECT                                            OWNER
----------------------------------------------------------------------------------------------
{{range .}}{{printf "%-10d %-10s %-50.50s %-20s\n" .number .state .title .author}}{{end}}`

var listCmd = &cobra.Command{
	Use:   "list",
	Short: "List open changes",
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()

		client, err := NewGerritClient(ctx, cmd)
		if err != nil {
			return fmt.Errorf("failed to create Gerrit client: %w", err)
		}

		var queryParts []string

		switch listState {
		case "open":
			queryParts = append(queryParts, "status:open")
		case "closed":
			queryParts = append(queryParts, "status:abandoned")
		case "merged":
			queryParts = append(queryParts, "status:merged")
		case "all":
			// No status filter
		default:
			return fmt.Errorf("unknown state: %s.\n\nValid states are: 'open', 'closed', 'merged', 'all'.\nExample:\n  gh pr list --state merged", listState)
		}

		if listBase != "" {
			queryParts = append(queryParts, fmt.Sprintf("branch:%s", listBase))
		}

		if listAssignee != "" {
			assignee := listAssignee
			if assignee == "@me" || assignee == "me" {
				assignee = "self"
			}
			queryParts = append(queryParts, fmt.Sprintf("reviewer:%s", assignee))
		}

		if listAuthor != "" {
			author := listAuthor
			if author == "@me" || author == "me" {
				author = "self"
			}
			queryParts = append(queryParts, fmt.Sprintf("owner:%s", author))
		}

		for _, l := range listLabel {
			queryParts = append(queryParts, fmt.Sprintf("label:%s", l))
		}

		if listSearch != "" {
			queryParts = append(queryParts, listSearch)
		}

		if listReviewer != "" {
			reviewer := listReviewer
			if reviewer == "@me" || reviewer == "me" {
				reviewer = "self"
			}
			queryParts = append(queryParts, fmt.Sprintf("reviewer:%s", reviewer))
		}

		if listMergedAfter != "" {
			queryParts = append(queryParts, fmt.Sprintf("mergedafter:%s", listMergedAfter))
		}

		if listMergedBefore != "" {
			queryParts = append(queryParts, fmt.Sprintf("mergedbefore:%s", listMergedBefore))
		}

		queryString := strings.Join(queryParts, " ")
		if queryString == "" {
			queryString = "status:open" // Fallback if no filters
		}

		opt := &gerrit.QueryChangeOptions{
			QueryOptions: gerrit.QueryOptions{
				Query: []string{queryString},
				Limit: listLimit,
			},
			ChangeOptions: gerrit.ChangeOptions{
				AdditionalFields: []string{"DETAILED_ACCOUNTS"},
			},
		}

		changes, _, err := client.Changes.QueryChanges(ctx, opt)
		if err != nil {
			return fmt.Errorf("failed to query changes: %w", err)
		}

		if listJSON == "" && listTemplateStr == "" && (changes == nil || len(*changes) == 0) {
			var msg string
			switch listState {
			case "open":
				msg = "No open changes found"
			case "merged":
				msg = "No merged changes found"
			case "closed":
				msg = "No abandoned changes found"
			default:
				msg = "No changes found"
			}

			hasCriteria := listAuthor != "" || listReviewer != "" || listMergedAfter != "" || listMergedBefore != "" || listBase != "" || len(listLabel) > 0 || listSearch != ""

			if hasCriteria {
				msg = msg + " matching criteria."
			} else {
				msg = msg + "."
			}

			fmt.Fprintln(cmd.OutOrStdout(), msg)
			return nil
		}

		data := []map[string]any{}
		if changes != nil {
			for _, c := range *changes {
				owner := FormatAccount(c.Owner)
				data = append(data, map[string]any{
					"number":  c.Number,
					"title":   c.Subject,
					"state":   c.Status,
					"status":  c.Status,
					"author":  owner,
					"branch":  c.Branch,
					"project": c.Project,
					"topic":   c.Topic,
				})
			}
		}

		r := &Renderer{
			Out:             cmd.OutOrStdout(),
			JSONFields:      listJSON,
			Template:        listTemplateStr,
			DefaultTemplate: defaultListTemplate,
		}

		if err := r.Render(data); err != nil {
			return fmt.Errorf("failed to render output: %w", err)
		}
		return nil
	},
}

func init() {
	listCmd.Flags().IntVarP(&listLimit, "limit", "L", 30, "Maximum number of items to fetch")
	listCmd.Flags().StringVarP(&listState, "state", "s", "open", "State of the pull request: {open|closed|merged|all}")
	listCmd.Flags().StringSliceVarP(&listLabel, "label", "l", nil, "Filter by label")
	listCmd.Flags().StringVarP(&listBase, "base", "B", "", "Filter by base branch")
	listCmd.Flags().StringVarP(&listAssignee, "assignee", "a", "", "Filter by assignee")
	listCmd.Flags().StringVarP(&listAuthor, "author", "A", "", "Filter by author")
	listCmd.Flags().StringVar(&listSearch, "search", "", "Search pull requests")
	listCmd.Flags().StringVar(&listReviewer, "reviewer", "", "Filter by reviewer")
	listCmd.Flags().StringVar(&listMergedAfter, "merged-after", "", "Filter by merged after date")
	listCmd.Flags().StringVar(&listMergedBefore, "merged-before", "", "Filter by merged before date")
	listCmd.Flags().StringVar(&listJSON, "json", "", "Output JSON with specified fields")
	listCmd.Flags().StringVarP(&listTemplateStr, "template", "t", "", "Format output using a Go template")
	PrCmd.AddCommand(listCmd)
}
