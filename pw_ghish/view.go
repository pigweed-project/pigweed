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
	"strconv"
	"strings"
	"sync"
	"text/template"

	"github.com/andygrunwald/go-gerrit"
	"github.com/spf13/cobra"
)

var (
	jsonOutputFields string
	showComments     bool
	templateStr      string
	viewWeb          bool
)

const defaultViewTemplate = `Change {{.number}}
Subject: {{.title}}
Status:  {{.state}}
Owner:   {{.author}}
{{if .patchset}}Patchset: {{.patchset}}
{{end}}{{if .assignees}}Assignees: {{.assignees}}
{{end}}{{if .reviewers}}Reviewers: {{.reviewers}}
{{end}}Branch:  {{.branch}}
Project: {{.project}}
{{if .topic}}Topic:   {{.topic}}
{{end}}{{if .hashtags}}Hashtags: {{.hashtags}}
{{end}}{{if .bug}}Bug:     {{.bug}}
{{end}}{{if .checks}}Checks:  {{.checks}}
{{end}}{{if hasAnyScore .labels}}
Labels:
{{range $name, $info := .labels}}{{if hasScore $info}}  {{$name}}: {{getLabelSummary $info}}
{{end}}{{end}}{{end}}{{if .body}}
{{.body}}
{{end}}
{{if .comments}}Comments:
{{printComments .comments}}
{{end}}`

var viewCmd = &cobra.Command{
	Use:   "view [<id>]",
	Short: "View a change",
	Args:  cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		chCtx, err := ResolveChangeContext(cmd, args)
		if err != nil {
			return err
		}
		ctx := chCtx.Context
		changeID := chCtx.ChangeID
		client := chCtx.Client
		reqRev := chCtx.Revision

		var additionalFields []string
		if reqRev != "" && reqRev != "current" {
			additionalFields = []string{"DETAILED_LABELS", "ALL_REVISIONS", "ALL_COMMITS", "DETAILED_ACCOUNTS"}
		} else {
			additionalFields = []string{"DETAILED_LABELS", "CURRENT_REVISION", "CURRENT_COMMIT", "DETAILED_ACCOUNTS"}
		}
		opt := &gerrit.ChangeOptions{
			AdditionalFields: additionalFields,
		}

		change, err := chCtx.GetChange(opt)
		if err != nil {
			return fmt.Errorf("failed to get change %s: %w", changeID, err)
		}

		if viewWeb {
			gerritURL, err := chCtx.Config.GerritURL(ctx)
			if err != nil {
				return err
			}
			gerritURL = strings.TrimSuffix(gerritURL, "/a")
			gerritURL = strings.TrimSuffix(gerritURL, "/")
			targetURL := fmt.Sprintf("%s/c/%s/+/%d", gerritURL, change.Project, change.Number)
			if chCtx.Revision != "" && chCtx.Revision != "current" {
				targetURL = fmt.Sprintf("%s/%s", targetURL, chCtx.Revision)
			}
			fmt.Fprintf(cmd.OutOrStdout(), "Opening %s in your browser.\n", targetURL)
			return OpenBrowserFn(targetURL)
		}

		owner := FormatAccount(change.Owner)
		assignees, reviewers := FormatChangeReviewersAndAssignees(change)

		patchset := 0
		body := ""
		commitMessage := ""
		rev, err := chCtx.ExtractRevision(change)
		if err == nil {
			patchset = rev.Number
			if rev.Commit.Message != "" {
				commitMessage = rev.Commit.Message
				lines := strings.SplitN(commitMessage, "\n", 2)
				if len(lines) > 1 {
					body = strings.TrimSpace(lines[1])
				}
			}
		} else if chCtx.Revision != "" && chCtx.Revision != "current" {
			return err
		}

		revisionToFetch := "current"
		if reqRev != "" && reqRev != "current" {
			revisionToFetch = reqRev
		}

		gHost := chCtx.Config.GerritHost(ctx)

		bbHost := buildbucketHost
		if bbHost == "" {
			bbHost = "cr-buildbucket.appspot.com"
		}

		var (
			wg            sync.WaitGroup
			fileList      []map[string]any
			comments      map[string][]gerrit.CommentInfo
			checksSummary string
		)

		wg.Add(1)
		go func() {
			defer wg.Done()
			files, _, err := client.Changes.ListFiles(ctx, changeID, revisionToFetch, nil)
			if err == nil && files != nil {
				for path, info := range files {
					fileList = append(fileList, map[string]any{
						"path":      path,
						"additions": info.LinesInserted,
						"deletions": info.LinesDeleted,
					})
				}
			}
		}()

		if showComments {
			wg.Add(1)
			go func() {
				defer wg.Done()
				if cMap, _, err := client.Changes.ListChangeComments(ctx, changeID); err == nil && cMap != nil {
					comments = *cMap
				}
			}()
		}

		if patchset > 0 && change.Project != "" && gHost != "" {
			wg.Add(1)
			go func() {
				defer wg.Done()
				if builds, err := queryBuildbucket(ctx, bbHost, gHost, change.Project, change.Number, patchset, http.DefaultClient); err == nil && builds != nil {
					checksSummary = formatCheckSummary(builds)
				}
			}()
		}

		wg.Wait()

		data := map[string]any{
			"number":         change.Number,
			"patchset":       patchset,
			"title":          change.Subject,
			"state":          change.Status,
			"status":         change.Status,
			"author":         owner,
			"assignees":      strings.Join(assignees, ", "),
			"reviewers":      strings.Join(reviewers, ", "),
			"branch":         change.Branch,
			"project":        change.Project,
			"topic":          change.Topic,
			"hashtags":       strings.Join(change.Hashtags, ", "),
			"labels":         change.Labels,
			"body":           body,
			"commitMessage":  commitMessage,
			"commit_message": commitMessage,
			"files":          fileList,
			"checks":         checksSummary,
		}

		if comments != nil {
			data["comments"] = comments
		}

		// The bug link is a `Bug:` or `Fixed:` trailer in the commit message,
		// the same place Gerrit reads it from. With no message there is
		// nothing to read, and an empty `bug` would assert "no bug is linked"
		// on no evidence -- which is how a caller ends up attaching a second,
		// duplicate trailer over a perfectly good one. So the fields are
		// withheld, and asking for one is an error rather than a quiet lie.
		if commitMessage != "" {
			links := ExtractBugLinks(commitMessage)
			data["bug"] = FormatBugLinks(links)
			data["bugs"] = links
		} else if field, ok := requestedBugField(jsonOutputFields); ok {
			return unavailableBugFieldError(changeID, field)
		}

		r := &Renderer{
			Out:             cmd.OutOrStdout(),
			JSONFields:      jsonOutputFields,
			Template:        templateStr,
			DefaultTemplate: defaultViewTemplate,
			FuncMap: template.FuncMap{
				"getLabelSummary": getLabelSummary,
				"hasScore":        hasScore,
				"hasAnyScore":     hasAnyScore,
				"printComments":   printComments,
			},
		}

		if err := r.Render(data); err != nil {
			return fmt.Errorf("failed to render output: %w", err)
		}
		return nil
	},
}

// bugJSONFields are the pr view fields derived from the commit message
// trailers rather than from the change metadata.
var bugJSONFields = []string{"bug", "bugs"}

// requestedBugField returns the first bug field named in a --json spec.
func requestedBugField(jsonSpec string) (string, bool) {
	for _, requested := range SplitJSONFields(jsonSpec) {
		for _, f := range bugJSONFields {
			if requested == f {
				return requested, true
			}
		}
	}
	return "", false
}

func unavailableBugFieldError(changeID, field string) error {
	return fmt.Errorf(`cannot report --json %s for change %s: Gerrit returned no commit message for the patchset being viewed

The bug link is a Bug: or Fixed: trailer in the commit message, so with no
message there is no way to tell "no bug is linked" from "not looked up".
Answering with an empty %s would be a guess, and a caller acting on it could
attach a duplicate bug over one that is already there.

To fix this:
  - Retry. A change detail without revision data is usually transient.
  - Name the patchset explicitly, e.g. ./gh pr view %s/1 --json %s
  - Read the trailers directly: ./gh pr view %s --json commitMessage`,
		field, changeID, field, changeID, field, changeID)
}

func hasScore(info gerrit.LabelInfo) bool {
	return getLabelSummary(info) != "No score"
}

func hasAnyScore(labels map[string]gerrit.LabelInfo) bool {
	for _, info := range labels {
		if hasScore(info) {
			return true
		}
	}
	return false
}

// labelRange returns the lowest and highest values a label permits.
//
// LabelInfo.Values is populated when DETAILED_LABELS is requested; ok is
// false when the range could not be determined.
func labelRange(info gerrit.LabelInfo) (low, high int, ok bool) {
	for value := range info.Values {
		n, err := strconv.Atoi(strings.TrimSpace(value))
		if err != nil {
			continue
		}
		if !ok || n < low {
			low = n
		}
		if !ok || n > high {
			high = n
		}
		ok = true
	}
	return low, high, ok
}

// castVote returns the most significant vote on a label: the lowest negative
// vote if anyone objected, otherwise the highest positive vote. This mirrors
// how Gerrit itself collapses a label down to a single displayed value.
func castVote(info gerrit.LabelInfo) (value int, voted bool) {
	low, high := 0, 0
	for _, approval := range info.All {
		v := int(approval.Value)
		if v == 0 {
			continue
		}
		voted = true
		if v < low {
			low = v
		}
		if v > high {
			high = v
		}
	}
	if low < 0 {
		return low, voted
	}
	return high, voted
}

// getLabelSummary renders a label's current vote, e.g. "+1 (Approved)".
//
// Gerrit's Approved/Rejected fields mean "someone voted this label's maximum
// (or minimum) value", which is not necessarily +2/-2. Pigweed labels such as
// Docs-Not-Needed ([0,+1]) and Presubmit-Verified ([-1,0,+1]) top out at +1,
// so the number is read from the vote itself and only the wording is derived
// from the label's range. Reporting a fixed +2 here would print a score the
// label cannot hold.
func getLabelSummary(info gerrit.LabelInfo) string {
	low, high, haveRange := labelRange(info)

	value, voted := castVote(info)
	if !voted {
		// All is only populated under DETAILED_LABELS. Fall back to the
		// summary fields, which name a voter but not the value they cast.
		switch {
		case info.Approved.AccountID != 0:
			if haveRange {
				return fmt.Sprintf("%+d (Approved)", high)
			}
			return "Approved"
		case info.Rejected.AccountID != 0:
			if haveRange {
				return fmt.Sprintf("%+d (Rejected)", low)
			}
			return "Rejected"
		case info.Recommended.AccountID != 0:
			return "Recommended"
		case info.Disliked.AccountID != 0:
			return "Disliked"
		}
		return "No score"
	}

	descriptor := "Recommended"
	if value < 0 {
		descriptor = "Disliked"
	}
	if haveRange {
		switch value {
		case high:
			descriptor = "Approved"
		case low:
			descriptor = "Rejected"
		}
	}
	return fmt.Sprintf("%+d (%s)", value, descriptor)
}

func init() {
	viewCmd.Flags().StringVar(&jsonOutputFields, "json", "", "Output JSON with specified fields")
	viewCmd.Flags().StringVarP(&templateStr, "template", "t", "", "Format output using a Go template")
	viewCmd.Flags().BoolVarP(&showComments, "comments", "c", false, "Show comments")
	viewCmd.Flags().BoolVarP(&viewWeb, "web", "w", false, "Open change in web browser")
	viewCmd.Flags().StringVar(&buildbucketHost, "buildbucket-host", "cr-buildbucket.appspot.com", "Buildbucket host to query")
	viewCmd.Flags().MarkHidden("buildbucket-host")
	PrCmd.AddCommand(viewCmd)
}

func printComments(comments map[string][]gerrit.CommentInfo) string {
	return FormatCommentForest(comments)
}
