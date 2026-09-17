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
	"encoding/json"
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/spf13/cobra"
)

func resolveTargetIssueID(ctx context.Context, cmd *cobra.Command, args []string) (int64, error) {
	if len(args) > 0 && strings.TrimSpace(args[0]) != "" {
		return ParseIssueID(args[0])
	}

	cfg := GetConfig(cmd)
	if cfg == nil {
		cfg = &Config{Host: HostFlag, Git: DefaultGitRunner}
	}
	msg, err := cfg.GitClient().CommitMessage(ctx, "HEAD")
	if err != nil {
		return 0, fmt.Errorf("no issue <number> provided and failed to inspect HEAD commit: %w\n\n"+
			"Remediation:\n"+
			"  Provide an explicit issue number or URL:\n"+
			"    gh issue %s <issue_id>", err, cmd.Name())
	}

	links := ExtractBugLinks(msg)
	var ids []int64
	seen := make(map[int64]bool)
	for _, link := range links {
		if id, err := ParseIssueID(link.ID); err == nil && id > 0 {
			if !seen[id] {
				seen[id] = true
				ids = append(ids, id)
			}
		}
	}

	if len(ids) == 0 {
		return 0, fmt.Errorf("no issue <number> provided and HEAD commit contains no Bug: or Fixed: trailer.\n\n"+
			"Remediation:\n"+
			"  1. Specify the Buganizer issue ID explicitly:\n"+
			"     gh issue %s <issue_id>\n"+
			"  2. Or link an issue to your current commit:\n"+
			"     git commit --amend -m \"...\\n\\nBug: b/<issue_id>\"", cmd.Name())
	}

	if len(ids) > 1 {
		var formatted []string
		for _, id := range ids {
			formatted = append(formatted, fmt.Sprintf("b/%d", id))
		}
		return 0, fmt.Errorf("HEAD commit references multiple bug IDs (%s); cannot automatically choose one.\n\n"+
			"Remediation:\n"+
			"  Specify which issue you want to target explicitly:\n"+
			"    gh issue %s %d", strings.Join(formatted, ", "), cmd.Name(), ids[0])
	}

	fmt.Fprintf(cmd.ErrOrStderr(), "Using issue b/%d from HEAD commit trailer.\n", ids[0])
	return ids[0], nil
}

func resolveComponentID(ctx context.Context, cfg *Config, explicitFlag int64) (int64, error) {
	if explicitFlag > 0 {
		return explicitFlag, nil
	}
	if cfg != nil {
		if val, err := cfg.GitClient().ConfigGet(ctx, "ghish.componentid"); err == nil && strings.TrimSpace(val) != "" {
			trimmed := strings.TrimSpace(val)
			parsed, parseErr := strconv.ParseInt(trimmed, 10, 64)
			if parseErr != nil || parsed <= 0 {
				return 0, fmt.Errorf("invalid git config 'ghish.componentid' value %q: must be a positive integer component ID.\n\n"+
					"Remediation:\n"+
					"  Set a valid numeric Buganizer component ID:\n"+
					"    git config ghish.componentid 1194524", trimmed)
			}
			return parsed, nil
		}
		if def := cfg.GetProfile(ctx).DefaultComponentID(); def > 0 {
			return def, nil
		}
	}
	return 0, nil
}

func resolveUserEmailArg(ctx context.Context, cfg *Config, arg string) (string, error) {
	trimmed := strings.TrimSpace(arg)
	if trimmed == "me" || trimmed == "@me" {
		email, err := cfg.GitClient().UserEmail(ctx)
		if err != nil || strings.TrimSpace(email) == "" {
			return "", fmt.Errorf("cannot resolve %q: git config 'user.email' is not configured.\n\n"+
				"Remediation:\n"+
				"  Configure your git user email:\n"+
				"    git config user.email \"you@example.com\"", trimmed)
		}
		return strings.TrimSpace(email), nil
	}
	return trimmed, nil
}

func readBodyInput(bodyFlag, bodyFileFlag string) (string, error) {
	if bodyFileFlag != "" {
		data, err := os.ReadFile(bodyFileFlag)
		if err != nil {
			return "", fmt.Errorf("failed to read body file %q: %w", bodyFileFlag, err)
		}
		return string(data), nil
	}
	return bodyFlag, nil
}

// --- Subcommands ---

var (
	issueViewComments bool
	issueViewWeb      bool
	issueViewJSON     string
)

var issueViewCmd = &cobra.Command{
	Use:   "view [<number> | <url>]",
	Short: "View a Buganizer issue",
	Args:  cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		jsonFields, err := ValidateIssueJSONFields(issueViewJSON)
		if err != nil {
			return err
		}

		issueID, err := resolveTargetIssueID(ctx, cmd, args)
		if err != nil {
			return err
		}

		cfg := GetConfig(cmd)
		if cfg == nil {
			cfg = &Config{Host: HostFlag, Git: DefaultGitRunner}
		}
		profile := cfg.GetProfile(ctx)

		if issueViewWeb {
			urlStr := profile.IssueWebURL(issueID)
			fmt.Fprintf(cmd.OutOrStdout(), "Opening %s in your browser.\n", urlStr)
			return OpenBrowserFn(urlStr)
		}

		client, err := NewIssueTrackerClientForCommand(ctx, cmd)
		if err != nil {
			return err
		}

		issue, err := client.GetIssue(ctx, issueID)
		if err != nil {
			return err
		}

		var comments []BuganizerComment
		needComments := issueViewComments
		for _, f := range jsonFields {
			if f == "comments" || f == "body" {
				needComments = true
			}
		}
		if needComments {
			comments, err = client.ListAllComments(ctx, issueID)
			if err != nil {
				return err
			}
		}

		if len(jsonFields) > 0 {
			m := IssueToJSONMap(issue, comments, profile, jsonFields)
			out, err := json.MarshalIndent(m, "", "  ")
			if err != nil {
				return fmt.Errorf("failed to serialize issue JSON: %w", err)
			}
			fmt.Fprintln(cmd.OutOrStdout(), string(out))
			return nil
		}

		out := cmd.OutOrStdout()
		fmt.Fprintf(out, "%s #%d\n", SanitizeUntrustedText(issue.State.Title), issue.IssueID)
		fmt.Fprintf(out, "Status:    %s\n", issue.State.Status)
		if issue.State.Priority != "" {
			fmt.Fprintf(out, "Priority:  %s\n", issue.State.Priority)
		}
		if issue.State.Type != "" {
			fmt.Fprintf(out, "Type:      %s\n", issue.State.Type)
		}
		if issue.State.ComponentID > 0 {
			fmt.Fprintf(out, "Component: %d\n", issue.State.ComponentID)
		}
		if issue.State.Assignee != nil && issue.State.Assignee.EmailAddress != "" {
			fmt.Fprintf(out, "Assignee:  %s\n", issue.State.Assignee.EmailAddress)
		} else {
			fmt.Fprintf(out, "Assignee:  (unassigned)\n")
		}
		if issue.State.Reporter != nil && issue.State.Reporter.EmailAddress != "" {
			fmt.Fprintf(out, "Reporter:  %s\n", issue.State.Reporter.EmailAddress)
		}
		fmt.Fprintf(out, "URL:       %s\n\n", profile.IssueWebURL(int64(issue.IssueID)))

		desc := ""
		if effDesc := issue.EffectiveDescription(); effDesc != nil {
			desc = effDesc.Comment
		} else if len(comments) > 0 {
			desc = comments[0].Comment
		}
		header := fmt.Sprintf("--- BEGIN UNTRUSTED ISSUE DESCRIPTION (b/%d) ---", issue.IssueID)
		footer := "--- END UNTRUSTED ISSUE DESCRIPTION ---"
		fmt.Fprintln(out, FormatUntrustedBlock(header, footer, desc))

		if issueViewComments && len(comments) > 1 {
			fmt.Fprintln(out, "\nComments:")
			for i, c := range comments {
				if c.CommentNumber == 1 || (c.CommentNumber == 0 && i == 0) {
					continue
				}
				author := c.EffectiveAuthorEmail()
				cHeader := fmt.Sprintf("--- BEGIN UNTRUSTED COMMENT #%d by %s (%s) ---",
					c.CommentNumber, author, c.CreatedTime.Format(time.RFC3339))
				cFooter := "--- END UNTRUSTED COMMENT ---"
				fmt.Fprintln(out, "")
				fmt.Fprintln(out, FormatUntrustedBlock(cHeader, cFooter, c.Comment))
			}
		}

		return nil
	},
}

var (
	issueListAssignee      string
	issueListAuthor        string
	issueListLabels        []string
	issueListState         string
	issueListSearch        string
	issueListLimit         int
	issueListComponent     int64
	issueListAllComponents bool
	issueListJSON          string
)

var issueListCmd = &cobra.Command{
	Use:   "list",
	Short: "List Buganizer issues",
	Args:  cobra.NoArgs,
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		jsonFields, err := ValidateIssueJSONFields(issueListJSON)
		if err != nil {
			return err
		}

		stateFilter := strings.ToLower(strings.TrimSpace(issueListState))
		if stateFilter == "" {
			stateFilter = "open"
		}
		if stateFilter != "open" && stateFilter != "closed" && stateFilter != "all" {
			return fmt.Errorf("invalid --state value %q; valid choices are 'open', 'closed', or 'all'", issueListState)
		}

		cfg := GetConfig(cmd)
		if cfg == nil {
			cfg = &Config{Host: HostFlag, Git: DefaultGitRunner}
		}
		profile := cfg.GetProfile(ctx)

		var queryParts []string
		if !issueListAllComponents {
			compID, err := resolveComponentID(ctx, cfg, issueListComponent)
			if err != nil {
				return err
			}
			if compID > 0 {
				queryParts = append(queryParts, fmt.Sprintf("componentid:%d", compID))
			}
		}
		if stateFilter == "open" {
			queryParts = append(queryParts, "status:open")
		} else if stateFilter == "closed" {
			queryParts = append(queryParts, "status:closed")
		}

		if issueListAssignee != "" {
			assignee, err := resolveUserEmailArg(ctx, cfg, issueListAssignee)
			if err != nil {
				return err
			}
			queryParts = append(queryParts, fmt.Sprintf("assignee:%s", assignee))
		}
		if issueListAuthor != "" {
			author, err := resolveUserEmailArg(ctx, cfg, issueListAuthor)
			if err != nil {
				return err
			}
			queryParts = append(queryParts, fmt.Sprintf("reporter:%s", author))
		}

		for _, l := range issueListLabels {
			token, err := LabelToQueryToken(l)
			if err != nil {
				return err
			}
			queryParts = append(queryParts, token)
		}

		if issueListSearch != "" {
			queryParts = append(queryParts, issueListSearch)
		}

		client, err := NewIssueTrackerClientForCommand(ctx, cmd)
		if err != nil {
			return err
		}

		queryStr := strings.Join(queryParts, " ")
		resp, err := client.ListIssues(ctx, queryStr, issueListLimit, "")
		if err != nil {
			return err
		}

		if len(jsonFields) > 0 {
			outList := make([]map[string]any, 0, len(resp.Issues))
			for _, iss := range resp.Issues {
				outList = append(outList, IssueToJSONMap(iss, nil, profile, jsonFields))
			}
			data, err := json.MarshalIndent(outList, "", "  ")
			if err != nil {
				return fmt.Errorf("failed to serialize issue list JSON: %w", err)
			}
			fmt.Fprintln(cmd.OutOrStdout(), string(data))
			return nil
		}

		out := cmd.OutOrStdout()
		if len(resp.Issues) == 0 {
			fmt.Fprintln(out, "No issues found matching query.")
			return nil
		}

		for _, iss := range resp.Issues {
			assignee := "unassigned"
			if iss.State.Assignee != nil && iss.State.Assignee.EmailAddress != "" {
				assignee = iss.State.Assignee.EmailAddress
			}
			pri := iss.State.Priority
			if pri == "" {
				pri = "--"
			}
			fmt.Fprintf(out, "b/%-9d  %-10s  %-4s  %-24s  %s\n",
				iss.IssueID, iss.State.Status, pri, assignee, SanitizeUntrustedText(iss.State.Title))
		}
		return nil
	},
}

var issueStatusJSON string

var issueStatusCmd = &cobra.Command{
	Use:   "status",
	Short: "Show status of relevant Buganizer issues",
	Args:  cobra.NoArgs,
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		jsonFields, err := ValidateIssueJSONFields(issueStatusJSON)
		if err != nil {
			return err
		}

		cfg := GetConfig(cmd)
		if cfg == nil {
			cfg = &Config{Host: HostFlag, Git: DefaultGitRunner}
		}
		email, err := cfg.GitClient().UserEmail(ctx)
		if err != nil || strings.TrimSpace(email) == "" {
			return fmt.Errorf("could not determine user email from git config 'user.email'.\n\n" +
				"Remediation:\n" +
				"  Configure your git email address:\n" +
				"    git config user.email \"you@example.com\"")
		}
		email = strings.TrimSpace(email)
		profile := cfg.GetProfile(ctx)

		client, err := NewIssueTrackerClientForCommand(ctx, cmd)
		if err != nil {
			return err
		}

		assignedResp, err := client.ListIssues(ctx, fmt.Sprintf("status:open assignee:%s", email), 25, "")
		if err != nil {
			return err
		}
		reportedResp, err := client.ListIssues(ctx, fmt.Sprintf("status:open reporter:%s", email), 25, "")
		if err != nil {
			return err
		}

		if len(jsonFields) > 0 {
			assignedMaps := make([]map[string]any, 0, len(assignedResp.Issues))
			for _, iss := range assignedResp.Issues {
				assignedMaps = append(assignedMaps, IssueToJSONMap(iss, nil, profile, jsonFields))
			}
			reportedMaps := make([]map[string]any, 0, len(reportedResp.Issues))
			for _, iss := range reportedResp.Issues {
				reportedMaps = append(reportedMaps, IssueToJSONMap(iss, nil, profile, jsonFields))
			}
			payload := map[string]any{
				"assigned": assignedMaps,
				"reported": reportedMaps,
			}
			data, err := json.MarshalIndent(payload, "", "  ")
			if err != nil {
				return err
			}
			fmt.Fprintln(cmd.OutOrStdout(), string(data))
			return nil
		}

		out := cmd.OutOrStdout()
		fmt.Fprintf(out, "Issues assigned to you (%s):\n", email)
		if len(assignedResp.Issues) == 0 {
			fmt.Fprintln(out, "  None")
		} else {
			for _, iss := range assignedResp.Issues {
				fmt.Fprintf(out, "  b/%-9d  %-4s  %s\n", iss.IssueID, iss.State.Priority, SanitizeUntrustedText(iss.State.Title))
			}
		}

		fmt.Fprintf(out, "\nIssues opened by you (%s):\n", email)
		if len(reportedResp.Issues) == 0 {
			fmt.Fprintln(out, "  None")
		} else {
			for _, iss := range reportedResp.Issues {
				fmt.Fprintf(out, "  b/%-9d  %-4s  %s\n", iss.IssueID, iss.State.Priority, SanitizeUntrustedText(iss.State.Title))
			}
		}
		return nil
	},
}

var (
	issueCreateTitle     string
	issueCreateBody      string
	issueCreateBodyFile  string
	issueCreateAssignee  string
	issueCreateLabels    []string
	issueCreatePriority  string
	issueCreateType      string
	issueCreateComponent int64
	issueCreateCCs       []string
	issueCreateAmend     bool
)

var issueCreateCmd = &cobra.Command{
	Use:   "create",
	Short: "Create a new Buganizer issue",
	Args:  cobra.NoArgs,
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		title := strings.TrimSpace(issueCreateTitle)
		if title == "" {
			return fmt.Errorf("--title (-t) is required when creating an issue.\n\n" +
				"Example:\n" +
				"  gh issue create -t \"pw_rpc: Fix channel deadlock\" -b \"Description...\" [-P P1] [--amend]")
		}

		body, err := readBodyInput(issueCreateBody, issueCreateBodyFile)
		if err != nil {
			return err
		}

		cfg := GetConfig(cmd)
		if cfg == nil {
			cfg = &Config{Host: HostFlag, Git: DefaultGitRunner}
		}

		if issueCreateAmend {
			if err := cfg.GitClient().CheckAmendAllowed(ctx); err != nil {
				return err
			}
		}

		compID, err := resolveComponentID(ctx, cfg, issueCreateComponent)
		if err != nil {
			return err
		}
		if compID <= 0 {
			return fmt.Errorf("no Buganizer component ID configured for this project.\n\n" +
				"Remediation:\n" +
				"  1. Specify a component ID via flag:\n" +
				"     gh issue create -C <component_id> -t ...\n" +
				"  2. Or set a default repository component ID in git config:\n" +
				"     git config ghish.componentid <component_id>")
		}

		state := BuganizerState{
			ComponentID: FlexInt64(compID),
			Type:        "BUG",
			Priority:    "P2",
			Status:      "NEW",
			Title:       title,
		}

		for _, l := range issueCreateLabels {
			if _, err := ApplyLabelToState(&state, l); err != nil {
				return err
			}
		}
		if issueCreatePriority != "" {
			if _, err := ApplyLabelToState(&state, issueCreatePriority); err != nil {
				return err
			}
		}
		if issueCreateType != "" {
			if _, err := ApplyLabelToState(&state, issueCreateType); err != nil {
				return err
			}
		}

		if issueCreateAssignee != "" {
			assignee, err := resolveUserEmailArg(ctx, cfg, issueCreateAssignee)
			if err != nil {
				return err
			}
			state.Assignee = &BuganizerUser{EmailAddress: assignee}
			state.Status = "ASSIGNED"
		}

		for _, cc := range issueCreateCCs {
			for _, email := range strings.Split(cc, ",") {
				email = strings.TrimSpace(email)
				if email != "" {
					state.CCs = append(state.CCs, BuganizerUser{EmailAddress: email})
				}
			}
		}

		client, err := NewIssueTrackerClientForCommand(ctx, cmd)
		if err != nil {
			return err
		}

		req := &CreateIssueRequest{
			IssueState: state,
		}
		if body != "" {
			req.IssueComment = &BuganizerComment{Comment: body}
		}

		created, err := client.CreateIssue(ctx, req)
		if err != nil {
			return err
		}

		profile := cfg.GetProfile(ctx)
		out := cmd.OutOrStdout()
		fmt.Fprintf(out, "✓ Created issue b/%d: %s\n", created.IssueID, created.State.Title)

		if issueCreateAmend {
			origMsg, err := cfg.GitClient().CommitMessage(ctx, "HEAD")
			if err != nil {
				return fmt.Errorf("created issue b/%d, but failed to read HEAD commit message for --amend: %w", created.IssueID, err)
			}

			existingLinks := ExtractBugLinks(origMsg)
			var bugValues []string
			for _, link := range existingLinks {
				if !link.Closes && !strings.EqualFold(link.ID, "None") {
					if id, err := ParseIssueID(link.ID); err == nil && id == int64(created.IssueID) {
						continue
					}
					bugValues = append(bugValues, link.ID)
				}
			}
			bugValues = append(bugValues, fmt.Sprintf("b/%d", created.IssueID))
			trailerLine := "Bug: " + strings.Join(bugValues, ", ")

			updatedMsg, err := UpsertTrailer(origMsg, trailerLine)
			if err != nil {
				return fmt.Errorf("created issue b/%d, but failed to update trailer: %w", created.IssueID, err)
			}
			if err := cfg.GitClient().Run(ctx, out, cmd.ErrOrStderr(), "commit", "--amend", "-m", updatedMsg); err != nil {
				return fmt.Errorf("created issue b/%d, but git commit --amend failed: %w", created.IssueID, err)
			}
			fmt.Fprintf(out, "✓ Amended HEAD commit with '%s'\n", trailerLine)
		}

		fmt.Fprintln(out, profile.IssueWebURL(int64(created.IssueID)))
		return nil
	},
}

var (
	issueEditTitle          string
	issueEditAddAssignee    string
	issueEditRemoveAssignee string
	issueEditAddLabels      []string
	issueEditRemoveLabels   []string
	issueEditPriority       string
	issueEditType           string
	issueEditComponent      int64
)

var issueEditCmd = &cobra.Command{
	Use:   "edit [<number> | <url>]",
	Short: "Edit an existing Buganizer issue",
	Args:  cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		issueID, err := resolveTargetIssueID(ctx, cmd, args)
		if err != nil {
			return err
		}

		client, err := NewIssueTrackerClientForCommand(ctx, cmd)
		if err != nil {
			return err
		}

		current, err := client.GetIssue(ctx, issueID)
		if err != nil {
			return err
		}

		mod := NewIssueModifier(current)

		if cmd.Flags().Changed("title") {
			if err := mod.SetTitle(issueEditTitle); err != nil {
				return err
			}
		}

		if issueEditPriority != "" {
			if err := mod.SetPriority(issueEditPriority); err != nil {
				return err
			}
		}
		if issueEditType != "" {
			if err := mod.SetType(issueEditType); err != nil {
				return err
			}
		}
		if issueEditComponent > 0 {
			mod.SetComponent(issueEditComponent)
		}

		for _, l := range issueEditAddLabels {
			if err := mod.AddLabel(l); err != nil {
				return err
			}
		}
		for _, l := range issueEditRemoveLabels {
			if err := mod.RemoveLabel(l); err != nil {
				return err
			}
		}

		cfg := GetConfig(cmd)
		if cfg == nil {
			cfg = &Config{Host: HostFlag, Git: DefaultGitRunner}
		}

		if issueEditAddAssignee != "" {
			assigneeEmail, err := resolveUserEmailArg(ctx, cfg, issueEditAddAssignee)
			if err != nil {
				return err
			}
			mod.SetAssignee(assigneeEmail)
		}
		if issueEditRemoveAssignee != "" {
			mod.RemoveAssignee()
		}

		if !mod.HasChanges() {
			return fmt.Errorf("no edits specified.\n\n" +
				"Example:\n" +
				"  gh issue edit 345678 -t \"New title\" -P P1 --add-assignee user@google.com")
		}

		updated, err := client.ModifyIssue(ctx, issueID, mod.BuildRequest())
		if err != nil {
			return err
		}

		fmt.Fprintf(cmd.OutOrStdout(), "✓ Updated issue b/%d (%s)\n", updated.IssueID, updated.State.Status)
		return nil
	},
}

var (
	issueCloseComment     string
	issueCloseReason      string
	issueCloseDuplicateOf int64
)

var issueCloseCmd = &cobra.Command{
	Use:   "close [<number> | <url>]",
	Short: "Close a Buganizer issue",
	Args:  cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		issueID, err := resolveTargetIssueID(ctx, cmd, args)
		if err != nil {
			return err
		}

		targetStatus := "FIXED"
		if issueCloseDuplicateOf > 0 {
			targetStatus = "DUPLICATE"
		} else if issueCloseReason != "" {
			switch strings.ToLower(strings.TrimSpace(issueCloseReason)) {
			case "completed", "fixed":
				targetStatus = "FIXED"
			case "not planned", "not_planned", "obsolete":
				targetStatus = "OBSOLETE"
			case "duplicate":
				return fmt.Errorf("--reason duplicate requires --duplicate-of <issue_id>")
			default:
				return fmt.Errorf("invalid --reason %q; valid choices are 'completed' or 'not planned'", issueCloseReason)
			}
		}

		client, err := NewIssueTrackerClientForCommand(ctx, cmd)
		if err != nil {
			return err
		}

		updated, err := client.CloseIssue(ctx, issueID, targetStatus, issueCloseDuplicateOf, issueCloseComment)
		if err != nil {
			return err
		}

		fmt.Fprintf(cmd.OutOrStdout(), "✓ Closed issue b/%d as %s\n", updated.IssueID, updated.State.Status)
		return nil
	},
}

var issueReopenComment string

var issueReopenCmd = &cobra.Command{
	Use:   "reopen [<number> | <url>]",
	Short: "Reopen a closed Buganizer issue",
	Args:  cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		issueID, err := resolveTargetIssueID(ctx, cmd, args)
		if err != nil {
			return err
		}

		client, err := NewIssueTrackerClientForCommand(ctx, cmd)
		if err != nil {
			return err
		}

		updated, err := client.ReopenIssue(ctx, issueID, issueReopenComment)
		if err != nil {
			return err
		}

		fmt.Fprintf(cmd.OutOrStdout(), "✓ Reopened issue b/%d (%s)\n", updated.IssueID, updated.State.Status)
		return nil
	},
}

var (
	issueCommentBody     string
	issueCommentBodyFile string
)

var issueCommentCmd = &cobra.Command{
	Use:   "comment [<number> | <url>]",
	Short: "Add a comment to a Buganizer issue",
	Args:  cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		issueID, err := resolveTargetIssueID(ctx, cmd, args)
		if err != nil {
			return err
		}

		body, err := readBodyInput(issueCommentBody, issueCommentBodyFile)
		if err != nil {
			return err
		}
		if strings.TrimSpace(body) == "" {
			return fmt.Errorf("comment body cannot be empty; specify --body (-b) or --body-file (-F)")
		}

		client, err := NewIssueTrackerClientForCommand(ctx, cmd)
		if err != nil {
			return err
		}

		comment, err := client.CreateComment(ctx, issueID, body)
		if err != nil {
			return err
		}

		fmt.Fprintf(cmd.OutOrStdout(), "✓ Added comment #%d to issue b/%d\n", comment.CommentNumber, issueID)
		return nil
	},
}

var (
	issueDevelopBase string
	issueDevelopName string
)

var issueDevelopCmd = &cobra.Command{
	Use:   "develop [<number> | <url>]",
	Short: "Create and check out a development branch for a Buganizer issue",
	Args:  cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		issueID, err := resolveTargetIssueID(ctx, cmd, args)
		if err != nil {
			return err
		}

		client, err := NewIssueTrackerClientForCommand(ctx, cmd)
		if err != nil {
			return err
		}

		issue, err := client.GetIssue(ctx, issueID)
		if err != nil {
			return err
		}

		branchName := strings.TrimSpace(issueDevelopName)
		if branchName == "" {
			branchName = SlugifyBranchName(issueID, issue.State.Title)
		}

		cfg := GetConfig(cmd)
		if cfg == nil {
			cfg = &Config{Host: HostFlag, Git: DefaultGitRunner}
		}

		gitArgs := []string{"checkout", "-b", branchName}
		if issueDevelopBase != "" {
			gitArgs = append(gitArgs, issueDevelopBase)
		}

		if err := cfg.GitClient().Run(ctx, cmd.OutOrStdout(), cmd.ErrOrStderr(), gitArgs...); err != nil {
			return fmt.Errorf("failed to create development branch %q: %w", branchName, err)
		}

		fmt.Fprintf(cmd.OutOrStdout(), "✓ Checked out new branch %q for issue b/%d (%s)\n",
			branchName, issue.IssueID, issue.State.Title)
		return nil
	},
}

func init() {
	issueViewCmd.Flags().BoolVarP(&issueViewComments, "comments", "c", false, "View issue comments")
	issueViewCmd.Flags().BoolVarP(&issueViewWeb, "web", "w", false, "Open issue in browser")
	issueViewCmd.Flags().StringVar(&issueViewJSON, "json", "", "Output JSON with the specified comma-separated fields")

	issueListCmd.Flags().StringVarP(&issueListAssignee, "assignee", "a", "", "Filter by assignee email (or 'me')")
	issueListCmd.Flags().StringVarP(&issueListAuthor, "author", "A", "", "Filter by reporter/author email (or 'me')")
	issueListCmd.Flags().StringSliceVarP(&issueListLabels, "label", "l", nil, "Filter by priority/type/hotlist label (e.g. P1, bug)")
	issueListCmd.Flags().StringVarP(&issueListState, "state", "s", "open", "Filter by state: {open|closed|all}")
	issueListCmd.Flags().StringVarP(&issueListSearch, "search", "S", "", "Search issues with a Buganizer query")
	issueListCmd.Flags().IntVarP(&issueListLimit, "limit", "L", 30, "Maximum number of issues to fetch")
	issueListCmd.Flags().Int64VarP(&issueListComponent, "component", "C", 0, "Filter by Buganizer component ID")
	issueListCmd.Flags().BoolVar(&issueListAllComponents, "all-components", false, "Search across all Buganizer components")
	issueListCmd.Flags().StringVar(&issueListJSON, "json", "", "Output JSON with the specified comma-separated fields")

	issueStatusCmd.Flags().StringVar(&issueStatusJSON, "json", "", "Output JSON with the specified comma-separated fields")

	issueCreateCmd.Flags().StringVarP(&issueCreateTitle, "title", "t", "", "Issue title (required)")
	issueCreateCmd.Flags().StringVarP(&issueCreateBody, "body", "b", "", "Issue description body")
	issueCreateCmd.Flags().StringVarP(&issueCreateBodyFile, "body-file", "F", "", "Read description body from file")
	issueCreateCmd.Flags().StringVarP(&issueCreateAssignee, "assignee", "a", "", "Assign issue to email (or 'me')")
	issueCreateCmd.Flags().StringSliceVarP(&issueCreateLabels, "label", "l", nil, "Add structured labels (e.g. P1, bug, feature)")
	issueCreateCmd.Flags().StringVarP(&issueCreatePriority, "priority", "P", "", "Buganizer priority (P0..P4)")
	issueCreateCmd.Flags().StringVarP(&issueCreateType, "type", "T", "", "Buganizer issue type (bug, feature, task, cleanup, process)")
	issueCreateCmd.Flags().Int64VarP(&issueCreateComponent, "component", "C", 0, "Buganizer component ID")
	issueCreateCmd.Flags().StringSliceVar(&issueCreateCCs, "cc", nil, "Comma-separated emails to CC")
	issueCreateCmd.Flags().BoolVar(&issueCreateAmend, "amend", false, "Amend current Git HEAD commit with 'Bug: b/<new-id>'")

	issueEditCmd.Flags().StringVarP(&issueEditTitle, "title", "t", "", "New issue title")
	issueEditCmd.Flags().StringVar(&issueEditAddAssignee, "add-assignee", "", "Set issue assignee email")
	issueEditCmd.Flags().StringVar(&issueEditRemoveAssignee, "remove-assignee", "", "Remove issue assignee")
	issueEditCmd.Flags().StringSliceVarP(&issueEditAddLabels, "add-label", "l", nil, "Add priority/type/hotlist labels")
	issueEditCmd.Flags().StringSliceVar(&issueEditRemoveLabels, "remove-label", nil, "Remove hotlist labels")
	issueEditCmd.Flags().StringVarP(&issueEditPriority, "priority", "P", "", "Set Buganizer priority (P0..P4)")
	issueEditCmd.Flags().StringVarP(&issueEditType, "type", "T", "", "Set Buganizer issue type (bug, feature, task)")
	issueEditCmd.Flags().Int64VarP(&issueEditComponent, "component", "C", 0, "Move issue to Buganizer component ID")

	issueCloseCmd.Flags().StringVarP(&issueCloseComment, "comment", "c", "", "Post a closing comment")
	issueCloseCmd.Flags().StringVarP(&issueCloseReason, "reason", "r", "", "Reason for closing: {completed|not planned}")
	issueCloseCmd.Flags().Int64Var(&issueCloseDuplicateOf, "duplicate-of", 0, "Close as DUPLICATE of another issue ID")

	issueReopenCmd.Flags().StringVarP(&issueReopenComment, "comment", "c", "", "Post a reopening comment")

	issueCommentCmd.Flags().StringVarP(&issueCommentBody, "body", "b", "", "Comment text")
	issueCommentCmd.Flags().StringVarP(&issueCommentBodyFile, "body-file", "F", "", "Read comment text from file")

	issueDevelopCmd.Flags().StringVarP(&issueDevelopBase, "base", "b", "", "Base branch to branch from")
	issueDevelopCmd.Flags().StringVarP(&issueDevelopName, "name", "n", "", "Custom branch name")

	IssueCmd.AddCommand(issueViewCmd)
	IssueCmd.AddCommand(issueListCmd)
	IssueCmd.AddCommand(issueStatusCmd)
	IssueCmd.AddCommand(issueCreateCmd)
	IssueCmd.AddCommand(issueEditCmd)
	IssueCmd.AddCommand(issueCloseCmd)
	IssueCmd.AddCommand(issueReopenCmd)
	IssueCmd.AddCommand(issueCommentCmd)
	IssueCmd.AddCommand(issueDevelopCmd)
}
