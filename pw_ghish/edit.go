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
	"github.com/spf13/pflag"
)

type editOptions struct {
	commitMsg      CommitMessageFlags
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
}

func (opts *editOptions) runE() func(*cobra.Command, []string) error {
	return func(cmd *cobra.Command, args []string) error {
		if opts.commitMsg.HasAny(cmd) {
			return formatEditCommitMsgGuardError(args)
		}
		if !hasChangedLocalFlag(cmd) {
			return noEditFlagsError(cmd)
		}

		chCtx, err := ResolveChangeContext(cmd, args)
		if err != nil {
			return err
		}
		ctx := chCtx.Context
		changeID := chCtx.ChangeID
		client := chCtx.Client

		rawAddReviewers := append([]string{}, opts.addReviewer...)
		rawAddReviewers = append(rawAddReviewers, opts.reviewer...)
		if opts.addOwner || opts.owner {
			rawAddReviewers = append(rawAddReviewers, "@owners")
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
			PrintReceipt(cmd.OutOrStdout(), Receipt{
				Summary: fmt.Sprintf("Reviewer added successfully: %s", strings.Join(expandedReviewers, ", ")),
				Next:    fmt.Sprintf("gh pr view %s", changeID),
			})
		}

		for _, reviewer := range opts.removeReviewer {
			_, err = client.Changes.DeleteReviewer(ctx, changeID, reviewer)
			if err != nil {
				return fmt.Errorf("error deleting reviewer %s: %w", reviewer, err)
			}
			PrintReceipt(cmd.OutOrStdout(), Receipt{
				Summary: "Reviewer removed successfully.",
				Next:    fmt.Sprintf("gh pr view %s", changeID),
			})
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
			PrintReceipt(cmd.OutOrStdout(), Receipt{
				Summary: "Assignee added successfully.",
				Next:    fmt.Sprintf("gh pr view %s", changeID),
			})
		}

		for _, assignee := range opts.removeAssignee {
			_, err = client.Changes.DeleteReviewer(ctx, changeID, assignee)
			if err != nil {
				return fmt.Errorf("error deleting assignee %s: %w", assignee, err)
			}
			PrintReceipt(cmd.OutOrStdout(), Receipt{
				Summary: "Assignee removed successfully.",
				Next:    fmt.Sprintf("gh pr view %s", changeID),
			})
		}

		for _, account := range addAttention {
			if err := addToAttentionSet(chCtx, account); err != nil {
				return err
			}
			PrintReceipt(cmd.OutOrStdout(), Receipt{
				Summary: fmt.Sprintf("Added %s to the attention set.", account),
				Next:    fmt.Sprintf("gh pr view %s", changeID),
			})
		}

		for _, account := range removeAttention {
			if err := removeFromAttentionSet(chCtx, account); err != nil {
				return err
			}
			PrintReceipt(cmd.OutOrStdout(), Receipt{
				Summary: fmt.Sprintf("Removed %s from the attention set.", account),
				Next:    fmt.Sprintf("gh pr view %s", changeID),
			})
		}

		if len(labelVotes) > 0 {
			input := &gerrit.ReviewInput{
				Labels: labelVotes,
			}

			if err := chCtx.SetReviewRevision("current", input); err != nil {
				return fmt.Errorf("error applying labels for change %s: %w", changeID, err)
			}
			PrintReceipt(cmd.OutOrStdout(), Receipt{
				Summary: "Labels added successfully.",
				Next:    fmt.Sprintf("gh pr view %s", changeID),
			})
		}

		if cmd.Flags().Changed("topic") || opts.removeTopic {
			if opts.removeTopic || (cmd.Flags().Changed("topic") && strings.TrimSpace(opts.topic) == "") {
				if _, err := client.Changes.DeleteTopic(ctx, changeID); err != nil {
					return chCtx.FormatError(err, "deleting topic on")
				}
				PrintReceipt(cmd.OutOrStdout(), Receipt{
					Summary: "Topic removed successfully.",
					Next:    fmt.Sprintf("gh pr view %s", changeID),
				})
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
				PrintReceipt(cmd.OutOrStdout(), Receipt{
					Summary: fmt.Sprintf("Topic set to %q successfully.", targetTopic),
					Next:    fmt.Sprintf("gh pr view %s", changeID),
				})
			}
		}

		if len(opts.addHashtags) > 0 || len(opts.removeHashtags) > 0 {
			if _, _, err := client.Changes.SetHashtags(ctx, changeID, &gerrit.HashtagsInput{
				Add:    opts.addHashtags,
				Remove: opts.removeHashtags,
			}); err != nil {
				return chCtx.FormatError(err, "updating hashtags on")
			}
			PrintReceipt(cmd.OutOrStdout(), Receipt{
				Summary: "Hashtags updated successfully.",
				Next:    fmt.Sprintf("gh pr view %s", changeID),
			})
		}

		if cmd.Flags().Changed("cq") || cmd.Flags().Changed("trigger") {
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
				PrintReceipt(cmd.OutOrStdout(), Receipt{
					Summary: fmt.Sprintf("%s vote removed successfully.", cqName),
					Next:    fmt.Sprintf("gh pr view %s", changeID),
				})
			} else {
				PrintReceipt(cmd.OutOrStdout(), Receipt{
					Summary: fmt.Sprintf("%s+%d set successfully.", cqName, cqScore),
					Next:    fmt.Sprintf("gh pr view %s", changeID),
				})
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

func hasChangedLocalFlag(cmd *cobra.Command) bool {
	changed := false
	cmd.LocalFlags().VisitAll(func(fl *pflag.Flag) {
		if fl.Name != "help" && fl.Changed {
			changed = true
		}
	})
	return changed
}

func noEditFlagsError(cmd *cobra.Command) error {
	var flags []string
	cmd.LocalFlags().VisitAll(func(fl *pflag.Flag) {
		if !fl.Hidden && fl.Name != "help" {
			flags = append(flags, "--"+fl.Name)
		}
	})
	var list string
	switch len(flags) {
	case 0:
		list = "an edit flag"
	case 1:
		list = flags[0]
	default:
		list = strings.Join(flags[:len(flags)-1], ", ") + ", or " + flags[len(flags)-1]
	}
	return fmt.Errorf(
		"at least one of %s must be specified\n\n"+
			"Example edit commands:\n"+
			"  gh pr edit 123 --trigger\n"+
			"  gh pr edit 123 --cq\n"+
			"  gh pr edit 123 --topic \"my-feature\"\n"+
			"  gh pr edit 123 --add-hashtag \"bugfix\"\n"+
			"  gh pr edit 123 --add-reviewer user@google.com\n"+
			"  gh pr edit 123 --add-owner\n"+
			"  gh pr edit 123 --remove-attention me\n"+
			"  gh pr edit 123 --add-label Commit-Queue=1",
		list,
	)
}

func formatEditCommitMsgGuardError(args []string) error {
	var checkoutHint string
	if len(args) > 0 {
		if target := strings.TrimSpace(SanitizeUntrustedText(args[0])); target != "" {
			checkoutHint = fmt.Sprintf(
				"\n  gh pr checkout %s   # if not already checked out locally",
				target,
			)
		}
	}

	return fmt.Errorf(
		"cannot update commit messages or bug trailers with 'gh pr edit'.\n\n"+
			"Why: 'gh pr edit' updates server-side Gerrit metadata (reviewers, labels,\n"+
			"attention set, topic, hashtags) without creating a patchset. In Gerrit, commit\n"+
			"titles, descriptions, and Bug:/Fixed: trailers live inside the Git commit\n"+
			"message of each patchset.\n\n"+
			"To update the local commit message or trailers and upload cleanly:%s\n"+
			"  gh pr push --title \"...\" --fixed b/123456\n"+
			"  gh pr create --title \"...\" --bug b/123456   # for a brand-new CL\n\n"+
			"For an earlier commit in a stack, edit via 'git rebase -i' (preserving each\n"+
			"commit's Change-Id:) and run 'gh pr push --stack'.",
		checkoutHint,
	)
}

func newEditCmd() *cobra.Command {
	var opts editOptions

	editCmd := &cobra.Command{
		Use:         "edit [<id>]",
		Annotations: map[string]string{KindAnnotation: KindMutation},
		Short:       "Edit change metadata (reviewers, attention set, labels, topic, hashtags)",
		Args:        cobra.MaximumNArgs(1),
		RunE:        opts.runE(),
	}

	opts.commitMsg.AddAsDisabled(editCmd)
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
