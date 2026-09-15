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
	"strconv"
	"strings"

	"github.com/andygrunwald/go-gerrit"
	"github.com/spf13/cobra"
)

var (
	editMessage        string
	editTitle          string
	editBody           string
	editAddReviewer    []string
	editRemoveReviewer []string
	editAddAssignee    []string
	editRemoveAssignee []string
	editAddLabels      []string
	editTopic          string
	editRemoveTopic    bool
	editAddHashtags    []string
	editRemoveHashtags []string
	editCQ             int
	editDropTrailers   bool
	editBug            string
	editFixed          string
)

var editCmd = &cobra.Command{
	Use:   "edit [<id>]",
	Short: "Edit a change (commit message, reviewers, labels, topic, hashtags)",
	Args:  cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		chCtx, err := ResolveChangeContext(cmd, args)
		if err != nil {
			return err
		}
		ctx := chCtx.Context
		changeID := chCtx.ChangeID
		client := chCtx.Client

		if cmd.Flags().Changed("title") && strings.TrimSpace(editTitle) == "" {
			return fmt.Errorf("cannot set an empty commit title")
		}
		if cmd.Flags().Changed("message") && strings.TrimSpace(editMessage) == "" {
			return fmt.Errorf("cannot set an empty commit message")
		}

		hasTrailerEdit := cmd.Flags().Changed("bug") || cmd.Flags().Changed("fixed")
		hasMsgEdit := editMessage != "" || editTitle != "" || cmd.Flags().Changed("body") || hasTrailerEdit
		hasTopicEdit := cmd.Flags().Changed("topic") || editRemoveTopic
		hasHashtagEdit := len(editAddHashtags) > 0 || len(editRemoveHashtags) > 0
		hasCQEdit := cmd.Flags().Changed("cq")
		if !hasMsgEdit && len(editAddReviewer) == 0 && len(editRemoveReviewer) == 0 &&
			len(editAddAssignee) == 0 && len(editRemoveAssignee) == 0 && len(editAddLabels) == 0 &&
			!hasTopicEdit && !hasHashtagEdit && !hasCQEdit {
			return fmt.Errorf("at least one of --message, --title, --body, --bug, --fixed, --add-reviewer, --remove-reviewer, --add-assignee, --remove-assignee, --add-label, --cq, --topic, --remove-topic, --add-hashtag, or --remove-hashtag must be specified\n\nExample edit commands:\n  gh pr edit 123 --title \"New title\"\n  gh pr edit 123 --bug b/456\n  gh pr edit 123 --cq\n  gh pr edit 123 --topic \"my-feature\"\n  gh pr edit 123 --add-hashtag \"bugfix\"\n  gh pr edit 123 --add-reviewer user@google.com\n  gh pr edit 123 --add-label Commit-Queue=1")
		}

		if editMessage != "" && (editTitle != "" || cmd.Flags().Changed("body")) {
			return fmt.Errorf("cannot specify both --message and --title/--body")
		}

		if editDropTrailers && editMessage == "" {
			return fmt.Errorf("--drop-trailers only applies to --message\n\n" +
				"--message replaces the entire commit message, so --drop-trailers is how you\n" +
				"confirm that trailers it leaves out are meant to go. --body and --title never\n" +
				"drop trailers, so there is nothing for the flag to permit.\n\n" +
				"Example:\n" +
				"  gh pr edit 123 --drop-trailers -m \"pw_foo: New message\"")
		}

		// Reject GitHub issue syntax before anything is written. Every piece
		// of commit message text the user can supply goes through the same
		// check, so `Fixes #456` cannot slip in through one flag and not
		// another.
		for _, text := range []struct{ where, value string }{
			{"--message", editMessage},
			{"--title", editTitle},
			{"--body", editBody},
		} {
			if err := CheckGitHubIssueSyntax(text.value, text.where); err != nil {
				return err
			}
		}

		// --bug/--fixed and hand-written trailer text are two ways to say the
		// same thing. If they disagree, guessing would silently discard one.
		trailerFlags := []struct{ flag, key, value string }{
			{"bug", "Bug", editBug},
			{"fixed", "Fixed", editFixed},
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
				{"--message", editMessage},
				{"--body", editBody},
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
			// Every path below needs the current message: to keep the title,
			// to preserve trailers, to rescue the Change-Id, or to edit a
			// single trailer in place.
			commitInfo, _, err := client.Changes.GetCommit(ctx, changeID, "current", nil)
			if err != nil {
				return fmt.Errorf("error fetching current commit message for change %s: %w", changeID, err)
			}
			origMessage := commitInfo.Message
			newMessage := origMessage

			if editTitle != "" || cmd.Flags().Changed("body") {
				var origTitle, origBody string
				lines := strings.Split(origMessage, "\n")
				origTitle = lines[0]
				if len(lines) > 1 {
					origBody = strings.TrimPrefix(strings.Join(lines[1:], "\n"), "\n")
				}

				targetTitle := origTitle
				if editTitle != "" {
					targetTitle = editTitle
				}

				targetBody := origBody
				if cmd.Flags().Changed("body") {
					origTrailers := ExtractTrailers(origMessage)
					targetBody = MergeTrailers(editBody, origTrailers)
				}

				if strings.TrimSpace(targetBody) != "" {
					newMessage = strings.TrimSpace(targetTitle) + "\n\n" + strings.TrimLeft(targetBody, "\r\n")
				} else {
					newMessage = strings.TrimSpace(targetTitle) + "\n"
				}
			} else if editMessage != "" {
				origID := ExtractChangeID(origMessage)
				providedID := ExtractChangeID(editMessage)

				if origID != "" && providedID != "" && origID != providedID {
					return fmt.Errorf("cannot change Gerrit Change-Id from %s to %s", origID, providedID)
				}

				// --message replaces the whole message, so anything the new
				// text leaves out is gone and Gerrit keeps no copy. Silently
				// re-appending the missing trailers would override a
				// deliberate deletion, so refuse instead and let the user say
				// which they meant.
				if !editDropTrailers {
					var dropped []string
					for _, trailer := range DroppedTrailers(origMessage, editMessage) {
						// Change-Id identifies the change itself rather than
						// anything the author wrote. It is restored below, so
						// it is never a casualty.
						if key, ok := trailerKey(trailer); ok && strings.EqualFold(key, "Change-Id") {
							continue
						}
						dropped = append(dropped, trailer)
					}
					if len(dropped) > 0 {
						return fmt.Errorf(
							"--message would delete %d trailer(s) from change %s:\n\n  %s\n\n"+
								"--message replaces the entire commit message, and Gerrit keeps no copy of the\n"+
								"previous one. Pick one:\n\n"+
								"  1. Carry them forward by appending them to your --message text.\n"+
								"  2. Edit only the prose and keep trailers automatically:\n"+
								"       gh pr edit %s --body \"...\"\n"+
								"  3. Confirm you really want them gone:\n"+
								"       gh pr edit %s --drop-trailers -m \"...\"\n\n"+
								"To review the current message: gh pr view %s",
							len(dropped), changeID, strings.Join(dropped, "\n  "),
							changeID, changeID, changeID)
					}
				}

				newMessage = editMessage
				if providedID == "" && origID != "" {
					newMessage = strings.TrimRight(editMessage, "\r\n") + "\n\nChange-Id: " + origID + "\n"
				}
			}

			// Trailer flags apply last so that they win over whatever the
			// message construction above produced, and so that --bug works on
			// its own without touching the rest of the message.
			for _, tf := range trailerFlags {
				if !cmd.Flags().Changed(tf.flag) {
					continue
				}
				newMessage, err = UpsertTrailer(newMessage, NormalizeTrailer(tf.key+": "+tf.value))
				if err != nil {
					return fmt.Errorf("error setting the %s: trailer on change %s: %w", tf.key, changeID, err)
				}
			}

			input := &gerrit.CommitMessageInput{
				Message: newMessage,
			}
			_, err = client.Changes.SetCommitMessage(ctx, changeID, input)
			if err != nil {
				return fmt.Errorf("error setting commit message for change %s: %w", changeID, err)
			}
			fmt.Fprintln(cmd.OutOrStdout(), "Commit message updated successfully.")
		}

		for _, reviewer := range editAddReviewer {
			if _, _, err := client.Changes.AddReviewer(ctx, changeID, &gerrit.ReviewerInput{Reviewer: reviewer}); err != nil {
				return chCtx.FormatError(err, fmt.Sprintf("adding reviewer %s to", reviewer))
			}
			fmt.Fprintln(cmd.OutOrStdout(), "Reviewer added successfully.")
		}

		for _, reviewer := range editRemoveReviewer {
			_, err = client.Changes.DeleteReviewer(ctx, changeID, reviewer)
			if err != nil {
				return fmt.Errorf("error deleting reviewer %s: %w", reviewer, err)
			}
			fmt.Fprintln(cmd.OutOrStdout(), "Reviewer removed successfully.")
		}

		for _, assignee := range editAddAssignee {
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

		for _, assignee := range editRemoveAssignee {
			_, err = client.Changes.DeleteReviewer(ctx, changeID, assignee)
			if err != nil {
				return fmt.Errorf("error deleting assignee %s: %w", assignee, err)
			}
			fmt.Fprintln(cmd.OutOrStdout(), "Assignee removed successfully.")
		}

		if len(editAddLabels) > 0 {
			input := &gerrit.ReviewInput{
				Labels: make(map[string]int),
			}
			for _, labelStr := range editAddLabels {
				var labelName string
				var scoreStr string

				if idx := strings.Index(labelStr, "="); idx != -1 {
					labelName = labelStr[:idx]
					scoreStr = labelStr[idx+1:]
				} else if idx := strings.LastIndexAny(labelStr, "+-"); idx != -1 {
					labelName = labelStr[:idx]
					scoreStr = labelStr[idx:]
				} else {
					return fmt.Errorf("error parsing label %q: must be in the format 'Name+Score' or 'Name=Score' (e.g. --add-label Commit-Queue=1 or Code-Review+2)", labelStr)
				}

				score, err := strconv.Atoi(scoreStr)
				if err != nil {
					return fmt.Errorf("error parsing score in label %q: score must be an integer (e.g. Commit-Queue=1, Code-Review+2): %w", labelStr, err)
				}

				input.Labels[labelName] = score
			}

			if err := chCtx.SetReviewRevision("current", input); err != nil {
				return fmt.Errorf("error applying labels for change %s: %w", changeID, err)
			}
			fmt.Fprintln(cmd.OutOrStdout(), "Labels added successfully.")
		}

		if hasTopicEdit {
			if editRemoveTopic || (cmd.Flags().Changed("topic") && strings.TrimSpace(editTopic) == "") {
				if _, err := client.Changes.DeleteTopic(ctx, changeID); err != nil {
					return chCtx.FormatError(err, "deleting topic on")
				}
				fmt.Fprintln(cmd.OutOrStdout(), "Topic removed successfully.")
			} else {
				targetTopic := strings.TrimSpace(editTopic)
				if _, _, err := client.Changes.SetTopic(ctx, changeID, &gerrit.TopicInput{Topic: targetTopic}); err != nil {
					return chCtx.FormatError(err, "setting topic on")
				}
				fmt.Fprintf(cmd.OutOrStdout(), "Topic set to %q successfully.\n", targetTopic)
			}
		}

		if hasHashtagEdit {
			if _, _, err := client.Changes.SetHashtags(ctx, changeID, &gerrit.HashtagsInput{
				Add:    editAddHashtags,
				Remove: editRemoveHashtags,
			}); err != nil {
				return chCtx.FormatError(err, "updating hashtags on")
			}
			fmt.Fprintln(cmd.OutOrStdout(), "Hashtags updated successfully.")
		}

		if hasCQEdit {
			input := &gerrit.ReviewInput{
				Labels: map[string]int{"Commit-Queue": editCQ},
			}
			if err := chCtx.SetReviewRevision("current", input); err != nil {
				return chCtx.FormatError(err, "setting Commit-Queue on")
			}
			if editCQ == 0 {
				fmt.Fprintln(cmd.OutOrStdout(), "Commit-Queue vote removed successfully.")
			} else {
				fmt.Fprintf(cmd.OutOrStdout(), "Commit-Queue+%d set successfully.\n", editCQ)
			}
		}

		return nil
	},
}

func init() {
	editCmd.Flags().StringVar(&editMessage, "message", "", "New commit message")
	editCmd.Flags().StringVar(&editTitle, "title", "", "New commit title (first line of commit message)")
	editCmd.Flags().StringVar(&editBody, "body", "", "New commit body")
	editCmd.Flags().BoolVar(&editDropTrailers, "drop-trailers", false,
		"Allow --message to delete trailers present in the current commit message")
	editCmd.Flags().StringVar(&editBug, "bug", "",
		"Set the Bug: trailer, linking a Buganizer issue (accepts 123456, b/123456, an issue URL, a comma-separated list, or \"none\")")
	editCmd.Flags().StringVar(&editFixed, "fixed", "",
		"Set the Fixed: trailer, linking a Buganizer issue and closing it on submit (same accepted forms as --bug)")
	editCmd.Flags().StringArrayVar(&editAddReviewer, "add-reviewer", nil, "Add reviewer by email or ID")
	editCmd.Flags().StringArrayVar(&editRemoveReviewer, "remove-reviewer", nil, "Remove reviewer by email or ID")
	editCmd.Flags().StringArrayVar(&editAddAssignee, "add-assignee", nil, "Add assignee by email or ID")
	editCmd.Flags().StringArrayVar(&editRemoveAssignee, "remove-assignee", nil, "Remove assignee by email or ID")
	editCmd.Flags().StringArrayVar(&editAddLabels, "add-label", nil, "Add Gerrit labels (e.g., Commit-Queue+1)")
	editCmd.Flags().IntVar(&editCQ, "cq", -1, "Set Commit-Queue vote (default 1: 1 = dry run, 2 = submit, 0 = remove)")
	editCmd.Flags().Lookup("cq").NoOptDefVal = "1"
	editCmd.Flags().StringVar(&editTopic, "topic", "", "Set Gerrit topic for the change")
	editCmd.Flags().BoolVar(&editRemoveTopic, "remove-topic", false, "Remove topic from the change")
	editCmd.Flags().StringArrayVar(&editAddHashtags, "add-hashtag", nil, "Add hashtags to the change")
	editCmd.Flags().StringArrayVar(&editRemoveHashtags, "remove-hashtag", nil, "Remove hashtags from the change")
	PrCmd.AddCommand(editCmd)
}
