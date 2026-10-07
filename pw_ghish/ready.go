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

var readyCmd = &cobra.Command{
	Use:   "ready [<id>]",
	Short: "Mark a change as ready for review",
	Long: `Mark a change as ready for review.

With -u, --undo, converts the change back to work-in-progress (WIP / draft).
An optional message can be supplied with -m, --message.
With -r, --reviewer and --owner, requests review from specific users or module
code owners (ghish-only).`,
	Args: cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		undo, _ := cmd.Flags().GetBool("undo")
		message, _ := cmd.Flags().GetString("message")
		rawReviewers, _ := cmd.Flags().GetStringSlice("reviewer")
		addReviewers, _ := cmd.Flags().GetStringSlice("add-reviewer")
		rawReviewers = append(rawReviewers, addReviewers...)
		owner, _ := cmd.Flags().GetBool("owner")
		addOwner, _ := cmd.Flags().GetBool("add-owner")
		if owner || addOwner {
			rawReviewers = append(rawReviewers, "@owners")
		}

		if undo && len(rawReviewers) > 0 {
			return fmt.Errorf("cannot specify --reviewer (-r) or --owner with --undo (-u): --undo marks the change as work-in-progress (draft)")
		}

		chCtx, err := ResolveChangeContext(cmd, args)
		if err != nil {
			return err
		}

		if undo {
			if err := chCtx.SetWorkInProgress(message); err != nil {
				return err
			}
			fmt.Fprintln(cmd.OutOrStdout(), "Change marked as work in progress successfully.")
			return nil
		}

		reviewers, err := ExpandReviewersForChange(cmd, chCtx, rawReviewers)
		if err != nil {
			return err
		}

		alreadyReady := false
		if _, err := chCtx.Client.Changes.SetReadyForReview(chCtx.Context, chCtx.ChangeID, &gerrit.ReadyForReviewInput{Message: message}); err != nil {
			// Only ignore HTTP 409 ("not work in progress") when -r/--reviewer or
			// --owner is specified so `gh pr ready -r` / `gh pr ready --owner` can
			// idempotently add reviewers to an already-ready CL while bare
			// `gh pr ready` preserves its existing 409 error behavior.
			if len(reviewers) > 0 && isAlreadyReadyConflict(err) {
				alreadyReady = true
			} else {
				return chCtx.FormatError(err, "marking change as ready for review")
			}
		}

		for _, reviewer := range reviewers {
			if _, _, err := chCtx.Client.Changes.AddReviewer(chCtx.Context, chCtx.ChangeID, &gerrit.ReviewerInput{Reviewer: reviewer}); err != nil {
				return chCtx.FormatError(err, fmt.Sprintf("adding reviewer %s to", reviewer))
			}
		}
		if len(reviewers) > 0 {
			fmt.Fprintf(cmd.OutOrStdout(), "Reviewer added successfully: %s\n", strings.Join(reviewers, ", "))
		}

		if !alreadyReady {
			fmt.Fprintln(cmd.OutOrStdout(), "Change marked ready for review successfully.")
		} else {
			fmt.Fprintln(cmd.OutOrStdout(), "Change is ready for review.")
		}
		return nil
	},
}

func isAlreadyReadyConflict(err error) bool {
	if err == nil {
		return false
	}
	errStr := err.Error()
	return reStatus409.MatchString(errStr) ||
		strings.Contains(strings.ToLower(errStr), "not work in progress") ||
		strings.Contains(strings.ToLower(errStr), "not wip") ||
		strings.Contains(strings.ToLower(errStr), "conflict")
}

func init() {
	readyCmd.Flags().StringP("message", "m", "", "Optional message explaining why the change is ready for review (or moving to WIP) (ghish-only)")
	readyCmd.Flags().BoolP("undo", "u", false, "Convert the change back to work-in-progress (WIP / draft)")
	readyCmd.Flags().StringSliceP("reviewer", "r", []string{}, "Request a review from someone, or @owners for code owners (ghish-only)")
	readyCmd.Flags().StringSlice("add-reviewer", []string{}, "Alias for --reviewer (ghish-only)")
	_ = readyCmd.Flags().MarkHidden("add-reviewer")
	readyCmd.Flags().Bool("owner", false, "Request a review from module code owners (ghish-only)")
	readyCmd.Flags().Bool("add-owner", false, "Alias for --owner (ghish-only)")
	_ = readyCmd.Flags().MarkHidden("add-owner")
	PrCmd.AddCommand(readyCmd)
}
