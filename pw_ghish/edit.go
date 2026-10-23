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
	"net/url"
	"regexp"
	"strconv"
	"strings"

	"github.com/andygrunwald/go-gerrit"
	"github.com/spf13/cobra"
)

type editOptions struct {
	title          string
	body           string
	addReviewer    []string
	reviewer       []string
	addOwner       bool
	owner          bool
	removeReviewer []string
	addAssignee    []string
	removeAssignee []string
	addAttention   []string
	removeAttn     []string
	addLabels      []string
	topic          string
	removeTopic    bool
	addHashtags    []string
	removeHashtags []string
	cq             int
	trigger        int
	bug            string
	fixed          string
}

func (opts *editOptions) runE() func(*cobra.Command, []string) error {
	return func(cmd *cobra.Command, args []string) error {
		chCtx, err := ResolveChangeContext(cmd, args)
		if err != nil {
			return err
		}
		ctx := chCtx.Context
		changeID := chCtx.ChangeID
		client := chCtx.Client

		if cmd.Flags().Changed("title") && strings.TrimSpace(opts.title) == "" {
			return fmt.Errorf("cannot set an empty commit title")
		}

		rawAddReviewers := append([]string{}, opts.addReviewer...)
		rawAddReviewers = append(rawAddReviewers, opts.reviewer...)
		if opts.addOwner || opts.owner {
			rawAddReviewers = append(rawAddReviewers, "@owners")
		}

		hasTrailerEdit := cmd.Flags().Changed("bug") || cmd.Flags().Changed("fixed")
		hasMsgEdit := opts.title != "" || cmd.Flags().Changed("body") || hasTrailerEdit
		hasTopicEdit := cmd.Flags().Changed("topic") || opts.removeTopic
		hasHashtagEdit := len(opts.addHashtags) > 0 || len(opts.removeHashtags) > 0
		hasCQEdit := cmd.Flags().Changed("cq") || cmd.Flags().Changed("trigger")
		hasAttentionEdit := len(opts.addAttention) > 0 || len(opts.removeAttn) > 0
		if !hasMsgEdit && len(rawAddReviewers) == 0 && len(opts.removeReviewer) == 0 &&
			len(opts.addAssignee) == 0 && len(opts.removeAssignee) == 0 && len(opts.addLabels) == 0 &&
			!hasTopicEdit && !hasHashtagEdit && !hasCQEdit && !hasAttentionEdit {
			return fmt.Errorf("at least one of --title, --body, --bug, --fixed, --add-reviewer, --add-owner, --remove-reviewer, --add-assignee, --remove-assignee, --add-attention, --remove-attention, --add-label, --trigger, --cq, --topic, --remove-topic, --add-hashtag, or --remove-hashtag must be specified\n\nExample edit commands:\n  gh pr edit 123 --title \"New title\"\n  gh pr edit 123 --bug b/456\n  gh pr edit 123 --trigger\n  gh pr edit 123 --cq\n  gh pr edit 123 --topic \"my-feature\"\n  gh pr edit 123 --add-hashtag \"bugfix\"\n  gh pr edit 123 --add-reviewer user@google.com\n  gh pr edit 123 --add-owner\n  gh pr edit 123 --remove-attention me\n  gh pr edit 123 --add-label Commit-Queue=1")
		}

		addAttention, err := normalizeAttentionAccounts("--add-attention", opts.addAttention)
		if err != nil {
			return err
		}
		removeAttention, err := normalizeAttentionAccounts("--remove-attention", opts.removeAttn)
		if err != nil {
			return err
		}
		labelVotes, err := parseLabelVotes(opts.addLabels)
		if err != nil {
			return err
		}

		// Reject GitHub issue syntax before anything is written. Every piece
		// of commit message text the user can supply goes through the same
		// check, so `Fixes #456` cannot slip in through one flag and not
		// another.
		for _, text := range []struct{ where, value string }{
			{"--title", opts.title},
			{"--body", opts.body},
		} {
			if err := CheckGitHubIssueSyntax(text.value, text.where); err != nil {
				return err
			}
		}

		// --bug/--fixed and hand-written trailer text are two ways to say the
		// same thing. If they disagree, guessing would silently discard one.
		trailerFlags := []struct{ flag, key, value string }{
			{"bug", "Bug", opts.bug},
			{"fixed", "Fixed", opts.fixed},
		}
		for _, tf := range trailerFlags {
			if !cmd.Flags().Changed(tf.flag) {
				continue
			}
			if strings.TrimSpace(tf.value) == "" {
				return fmt.Errorf("--%s requires a value\n\n"+
					"Accepted forms: a bare number (123456), a Buganizer ID (b/123456), an issue\n"+
					"URL (https://issues.pigweed.dev/issues/123456), a comma-separated list, or\n"+
					"\"none\".\n\nExample:\n  gh pr edit %s --%s b/123456", tf.flag, changeID, tf.flag)
			}
			for _, supplied := range []struct{ where, text string }{
				{"--body", opts.body},
			} {
				if supplied.text == "" || !MentionsTrailerKey(supplied.text, tf.key) {
					continue
				}
				return fmt.Errorf("--%s and %s both set a %s: trailer\n\n"+
					"Only one of them can win, and picking for you would silently discard the\n"+
					"other. Remove the %s: line from %s, or drop --%s.",
					tf.flag, supplied.where, tf.key, tf.key, supplied.where, tf.flag)
			}
		}

		if hasMsgEdit {
			// TODO(b/567763970): Re-enable commit-message editing once safe
			// local/remote synchronization is implemented. Keep
			// buildUpdatedCommitMessage referenced so its implementation stays
			// compiled while disabled.
			_ = buildUpdatedCommitMessage
			return fmt.Errorf(
				"editing commit messages via 'gh pr edit' (--title, --body, --bug, --fixed) is currently disabled (see b/567763970).\n\n" +
					"Why: In Gerrit, a CL's description is stored in the Git commit message rather than\n" +
					"a separate database record. Editing it on the Gerrit server creates a remote patchset\n" +
					"that diverges from your local Git branch and gets overwritten on the next 'gh pr push'.\n\n" +
					"To update a commit message locally:\n" +
					"  1. For the current HEAD commit (surgical file edit, preserving Change-Id:):\n" +
					"       git log -1 --format=%%B HEAD > \"$(git rev-parse --git-dir)/COMMIT_EDITMSG_TMP\"\n" +
					"       # Edit COMMIT_EDITMSG_TMP surgically (keep the Change-Id: footer!)\n" +
					"       git commit --amend --only -F \"$(git rev-parse --git-dir)/COMMIT_EDITMSG_TMP\"\n" +
					"       ./gh pr push\n" +
					"  2. For an earlier commit in a stack:\n" +
					"       Use 'git rebase' to update the commit message (retaining each commit's\n" +
					"       Change-Id: footer, or only the earliest commit's Change-Id when squashing),\n" +
					"       then run './gh pr push --stack'.",
			)
		}

		expandedReviewers, err := ExpandReviewersForChange(cmd, chCtx, rawAddReviewers)
		if err != nil {
			return err
		}
		for _, reviewer := range expandedReviewers {
			if _, _, err := client.Changes.AddReviewer(ctx, changeID, &gerrit.ReviewerInput{Reviewer: reviewer}); err != nil {
				return chCtx.FormatError(err, fmt.Sprintf("adding reviewer %s to", reviewer))
			}
		}
		if len(expandedReviewers) > 0 {
			fmt.Fprintf(cmd.OutOrStdout(), "Reviewer added successfully: %s\n", strings.Join(expandedReviewers, ", "))
		}

		for _, reviewer := range opts.removeReviewer {
			_, err = client.Changes.DeleteReviewer(ctx, changeID, reviewer)
			if err != nil {
				return fmt.Errorf("error deleting reviewer %s: %w", reviewer, err)
			}
			fmt.Fprintln(cmd.OutOrStdout(), "Reviewer removed successfully.")
		}

		for _, assignee := range opts.addAssignee {
			if err := chCtx.AddCC(assignee); err != nil {
				return err
			}

			review := &gerrit.ReviewInput{
				AddToAttentionSet: []gerrit.AttentionSetInput{
					{User: assignee, Reason: "Assigned via gh-ish"},
				},
			}
			if err := chCtx.SetReviewRevision("current", review); err != nil {
				return fmt.Errorf("error adding assignee to attention set %s: %w", assignee, err)
			}
			fmt.Fprintln(cmd.OutOrStdout(), "Assignee added successfully.")
		}

		for _, assignee := range opts.removeAssignee {
			_, err = client.Changes.DeleteReviewer(ctx, changeID, assignee)
			if err != nil {
				return fmt.Errorf("error deleting assignee %s: %w", assignee, err)
			}
			fmt.Fprintln(cmd.OutOrStdout(), "Assignee removed successfully.")
		}

		for _, account := range addAttention {
			if err := addToAttentionSet(chCtx, account); err != nil {
				return err
			}
			fmt.Fprintf(cmd.OutOrStdout(), "Added %s to the attention set.\n", account)
		}

		for _, account := range removeAttention {
			if err := removeFromAttentionSet(chCtx, account); err != nil {
				return err
			}
			fmt.Fprintf(cmd.OutOrStdout(), "Removed %s from the attention set.\n", account)
		}

		if len(labelVotes) > 0 {
			input := &gerrit.ReviewInput{
				Labels: labelVotes,
			}

			if err := chCtx.SetReviewRevision("current", input); err != nil {
				return fmt.Errorf("error applying labels for change %s: %w", changeID, err)
			}
			fmt.Fprintln(cmd.OutOrStdout(), "Labels added successfully.")
		}

		if hasTopicEdit {
			if opts.removeTopic || (cmd.Flags().Changed("topic") && strings.TrimSpace(opts.topic) == "") {
				if _, err := client.Changes.DeleteTopic(ctx, changeID); err != nil {
					return chCtx.FormatError(err, "deleting topic on")
				}
				fmt.Fprintln(cmd.OutOrStdout(), "Topic removed successfully.")
			} else {
				targetTopic := strings.TrimSpace(opts.topic)
				chInfo, chErr := chCtx.GetChange(&gerrit.ChangeOptions{
					AdditionalFields: []string{"SUBMIT_REQUIREMENTS"},
				})
				if chErr != nil {
					fmt.Fprintf(cmd.ErrOrStderr(), "Warning: failed to query submit requirements for change %s before setting topic: %v\n", changeID, chErr)
				}
				if err := CheckTopicAllowed(ctx, chCtx.Config, chInfo, targetTopic, fmt.Sprintf("gh pr edit %s (without --topic)", changeID)); err != nil {
					return err
				}
				if _, _, err := client.Changes.SetTopic(ctx, changeID, &gerrit.TopicInput{Topic: targetTopic}); err != nil {
					return chCtx.FormatError(err, "setting topic on")
				}
				fmt.Fprintf(cmd.OutOrStdout(), "Topic set to %q successfully.\n", targetTopic)
			}
		}

		if hasHashtagEdit {
			if _, _, err := client.Changes.SetHashtags(ctx, changeID, &gerrit.HashtagsInput{
				Add:    opts.addHashtags,
				Remove: opts.removeHashtags,
			}); err != nil {
				return chCtx.FormatError(err, "updating hashtags on")
			}
			fmt.Fprintln(cmd.OutOrStdout(), "Hashtags updated successfully.")
		}

		if hasCQEdit {
			requestedCQ := opts.cq
			if cmd.Flags().Changed("trigger") {
				requestedCQ = opts.trigger
			}
			cqName, cqScore := chCtx.ResolveCQVote(requestedCQ)
			input := &gerrit.ReviewInput{
				Labels: map[string]int{cqName: cqScore},
			}
			if err := chCtx.SetReviewRevision("current", input); err != nil {
				return chCtx.FormatError(err, fmt.Sprintf("setting %s on", cqName))
			}
			if cqScore == 0 {
				fmt.Fprintf(cmd.OutOrStdout(), "%s vote removed successfully.\n", cqName)
			} else {
				fmt.Fprintf(cmd.OutOrStdout(), "%s+%d set successfully.\n", cqName, cqScore)
			}
		}

		return nil
	}
}

var gerritLabelNameRegex = regexp.MustCompile(`^[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?$`)

func parseLabelVotes(rawLabels []string) (map[string]int, error) {
	if len(rawLabels) == 0 {
		return nil, nil
	}
	labels := make(map[string]int, len(rawLabels))
	for _, labelStr := range rawLabels {
		var labelName, scoreStr string
		if idx := strings.Index(labelStr, "="); idx != -1 {
			labelName = labelStr[:idx]
			scoreStr = labelStr[idx+1:]
		} else if idx := strings.LastIndexAny(labelStr, "+-"); idx != -1 {
			labelName = labelStr[:idx]
			scoreStr = labelStr[idx:]
		}
		if !gerritLabelNameRegex.MatchString(labelName) {
			return nil, fmt.Errorf("error parsing label %q: must be in the format 'Name+Score', 'Name-Score', or 'Name=Score' (e.g. --add-label Commit-Queue=1, Code-Review+2, or Code-Review-1)", labelStr)
		}
		score, err := strconv.Atoi(scoreStr)
		if err != nil {
			return nil, fmt.Errorf("error parsing score in label %q: score must be an integer (e.g. Commit-Queue=1, Code-Review+2): %w", labelStr, err)
		}
		labels[labelName] = score
	}
	return labels, nil
}

// attentionSetReason is recorded by Gerrit alongside each attention set
// update and shown in the change's history. Gerrit requires a reason when
// adding a user.
const attentionSetReason = "Updated via gh-ish"

// normalizeAttentionAccounts validates the accounts given to flag and maps
// GitHub-style "me" / "@me" to Gerrit's "self".
func normalizeAttentionAccounts(flag string, accounts []string) ([]string, error) {
	normalized := make([]string, 0, len(accounts))
	for _, account := range accounts {
		account = strings.TrimSpace(account)
		switch strings.ToLower(account) {
		case "":
			return nil, fmt.Errorf("%s requires an account\n\n"+
				"Accepted forms: an email address, a numeric account ID, or \"me\" for yourself.\n\n"+
				"Example:\n  gh pr edit 123 %s me", flag, flag)
		case "me", "@me", "self":
			account = "self"
		}
		normalized = append(normalized, account)
	}
	return normalized, nil
}

// addToAttentionSet adds account to the change's attention set using
// Gerrit's dedicated endpoint, which, unlike posting a review, creates no
// change message and does not apply the automatic attention set rules.
func addToAttentionSet(chCtx *ChangeContext, account string) error {
	endpoint := fmt.Sprintf("changes/%s/attention", url.PathEscape(chCtx.ChangeID))
	input := &gerrit.AttentionSetInput{User: account, Reason: attentionSetReason}
	if _, err := chCtx.Client.Call(chCtx.Context, "POST", endpoint, input, nil); err != nil {
		return chCtx.FormatError(err, fmt.Sprintf("adding %s to the attention set of", account))
	}
	return nil
}

// removeFromAttentionSet removes account from the change's attention set.
// The account remains a reviewer or CC.
func removeFromAttentionSet(chCtx *ChangeContext, account string) error {
	input := &gerrit.AttentionSetInput{Reason: attentionSetReason}
	if _, err := chCtx.Client.Changes.RemoveAttention(chCtx.Context, chCtx.ChangeID, url.PathEscape(account), input); err != nil {
		return chCtx.FormatError(err, fmt.Sprintf("removing %s from the attention set of", account))
	}
	return nil
}

func buildUpdatedCommitMessage(origMessage, changeID string, cmd *cobra.Command, opts *editOptions, trailerFlags []struct{ flag, key, value string }) (string, error) {
	newMessage := origMessage

	if opts.title != "" || cmd.Flags().Changed("body") {
		var origTitle, origBody string
		lines := strings.Split(origMessage, "\n")
		origTitle = lines[0]
		if len(lines) > 1 {
			origBody = strings.TrimPrefix(strings.Join(lines[1:], "\n"), "\n")
		}

		targetTitle := origTitle
		if opts.title != "" {
			targetTitle = opts.title
		}

		targetBody := origBody
		if cmd.Flags().Changed("body") {
			origTrailers := ExtractTrailers(origMessage)
			targetBody = MergeTrailers(opts.body, origTrailers)
		}

		if strings.TrimSpace(targetBody) != "" {
			newMessage = strings.TrimSpace(targetTitle) + "\n\n" + strings.TrimLeft(targetBody, "\r\n")
		} else {
			newMessage = strings.TrimSpace(targetTitle) + "\n"
		}
	}

	// Trailer flags apply last so that they win over whatever the
	// message construction above produced, and so that --bug works on
	// its own without touching the rest of the message.
	for _, tf := range trailerFlags {
		if !cmd.Flags().Changed(tf.flag) {
			continue
		}
		var err error
		newMessage, err = UpsertTrailer(newMessage, NormalizeTrailer(tf.key+": "+tf.value))
		if err != nil {
			return "", fmt.Errorf("error setting the %s: trailer on change %s: %w", tf.key, changeID, err)
		}
	}

	return newMessage, nil
}

func newEditCmd() *cobra.Command {
	var opts editOptions

	editCmd := &cobra.Command{
		Use:   "edit [<id>]",
		Short: "Edit a change (commit message, reviewers, labels, topic, hashtags)",
		Args:  cobra.MaximumNArgs(1),
		RunE:  opts.runE(),
	}

	editCmd.Flags().StringVar(&opts.title, "title", "", "New commit title (first line of commit message)")
	editCmd.Flags().StringVar(&opts.body, "body", "", "New commit body")
	editCmd.Flags().StringVar(&opts.bug, "bug", "",
		"Set the Bug: trailer, linking a Buganizer issue (accepts 123456, b/123456, an issue URL, a comma-separated list, or \"none\")")
	editCmd.Flags().StringVar(&opts.fixed, "fixed", "",
		"Set the Fixed: trailer, linking a Buganizer issue and closing it on submit (same accepted forms as --bug)")
	editCmd.Flags().StringArrayVar(&opts.addReviewer, "add-reviewer", nil, "Add reviewer by email, ID, or @owners")
	editCmd.Flags().StringArrayVarP(&opts.reviewer, "reviewer", "r", nil, "Alias for --add-reviewer (ghish-only)")
	_ = editCmd.Flags().MarkHidden("reviewer")
	editCmd.Flags().BoolVar(&opts.addOwner, "add-owner", false, "Add module code owners as reviewers (ghish-only)")
	editCmd.Flags().BoolVar(&opts.owner, "owner", false, "Alias for --add-owner (ghish-only)")
	_ = editCmd.Flags().MarkHidden("owner")
	editCmd.Flags().StringArrayVar(&opts.removeReviewer, "remove-reviewer", nil, "Remove reviewer by email or ID")
	editCmd.Flags().StringArrayVar(&opts.addAssignee, "add-assignee", nil, "Add assignee by email or ID")
	editCmd.Flags().StringArrayVar(&opts.removeAssignee, "remove-assignee", nil, "Remove assignee by email or ID")
	editCmd.Flags().StringArrayVar(&opts.addAttention, "add-attention", nil,
		"Add a user to the attention set by email, account ID, or \"me\" (ghish-only)")
	editCmd.Flags().StringArrayVar(&opts.removeAttn, "remove-attention", nil,
		"Remove a user from the attention set by email, account ID, or \"me\"; they stay a reviewer (ghish-only)")
	editCmd.Flags().StringArrayVar(&opts.addLabels, "add-label", nil, "Add Gerrit labels (e.g., Commit-Queue+1)")
	editCmd.Flags().IntVar(&opts.trigger, "trigger", -1, "Trigger presubmit / Commit-Queue vote (default 1: 1 = dry run, 2 = submit, 0 = remove; alias --cq)")
	editCmd.Flags().Lookup("trigger").NoOptDefVal = "1"
	editCmd.Flags().IntVar(&opts.cq, "cq", -1, "Set Commit-Queue vote (default 1: 1 = dry run, 2 = submit, 0 = remove; alias for --trigger)")
	editCmd.Flags().Lookup("cq").NoOptDefVal = "1"
	editCmd.Flags().StringVar(&opts.topic, "topic", "", "Set Gerrit topic for the change")
	editCmd.Flags().BoolVar(&opts.removeTopic, "remove-topic", false, "Remove topic from the change")
	editCmd.Flags().StringArrayVar(&opts.addHashtags, "add-hashtag", nil, "Add hashtags to the change")
	editCmd.Flags().StringArrayVar(&opts.removeHashtags, "remove-hashtag", nil, "Remove hashtags from the change")

	return editCmd
}

func init() {
	PrCmd.AddCommand(newEditCmd())
}
