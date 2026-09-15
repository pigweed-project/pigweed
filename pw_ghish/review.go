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

	"github.com/andygrunwald/go-gerrit"
	"github.com/spf13/cobra"
)

type reviewOptions struct {
	message        string
	body           string
	approve        bool
	requestChanges bool
	comment        bool
	cq             int
}

func newReviewCmd() *cobra.Command {
	opts := reviewOptions{cq: -1}
	cmd := &cobra.Command{
		Use:   "review [<id>]",
		Short: "Review a change",
		Long: `Review a Gerrit change by adding a message, approving, requesting changes, or voting on Commit-Queue.

If no change ID is specified, the active change for the current branch is reviewed.

Examples:
# Trigger Commit-Queue dry run on the active change
gh-ish pr review --cq

# Approve the active change on the current branch
gh-ish pr review --approve -m "Looks good to me"

# Approve and trigger Commit-Queue dry run
gh-ish pr review --approve --cq

# Approve a change by number with a message
gh-ish pr review 12345 --approve -m "Looks good to me"

# Request changes on a specific change
gh-ish pr review 12345 --request-changes -m "Please fix the typo"

# Leave a comment without voting
gh-ish pr review 12345 --comment -m "Just a question"`,
		Args: cobra.MaximumNArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			if opts.body != "" {
				if opts.message != "" {
					return fmt.Errorf("cannot specify both --message and --body")
				}
				opts.message = opts.body
			}

			count := 0
			if opts.approve {
				count++
			}
			if opts.requestChanges {
				count++
			}
			if opts.comment {
				count++
			}

			if count > 1 {
				return fmt.Errorf("cannot specify more than one of --approve, --request-changes, and --comment")
			}

			if opts.comment && opts.message == "" {
				return fmt.Errorf("--comment requires a message (specify -m \"...\" or --body \"...\")")
			}

			hasCQ := cmd.Flags().Changed("cq")
			if !opts.approve && !opts.requestChanges && !opts.comment && opts.message == "" && !hasCQ {
				targetID := "<id>"
				if len(args) > 0 {
					targetID = args[0]
				}
				return fmt.Errorf("no review action or message specified.\n\n"+
					"Specify at least one review action or message:\n"+
					"  gh pr review %s --cq                             # Trigger CQ dry run\n"+
					"  gh pr review %s --approve                        # Vote Code-Review+2\n"+
					"  gh pr review %s --approve --cq                   # Approve and trigger CQ dry run\n"+
					"  gh pr review %s --approve -m \"Looks great!\"       # Approve with a message\n"+
					"  gh pr review %s --request-changes -m \"See typo\"   # Vote Code-Review-1\n"+
					"  gh pr review %s --comment -m \"Just a question\"    # Leave comment without voting\n"+
					"  gh pr review %s -m \"Review message\"               # Leave message without voting",
					targetID, targetID, targetID, targetID, targetID, targetID, targetID)
			}

			chCtx, err := ResolveChangeContext(cmd, args)
			if err != nil {
				return err
			}

			input := &gerrit.ReviewInput{
				Message: opts.message,
				Labels:  make(map[string]int),
			}

			if opts.approve {
				input.Labels["Code-Review"] = 2
			} else if opts.requestChanges {
				input.Labels["Code-Review"] = -1
			}
			if hasCQ {
				input.Labels["Commit-Queue"] = opts.cq
			}

			if err := chCtx.SetReview(input); err != nil {
				return err
			}

			fmt.Fprintln(cmd.OutOrStdout(), "Review submitted successfully.")
			return nil
		},
	}

	cmd.Flags().StringVarP(&opts.message, "message", "m", "", "Review message")
	cmd.Flags().StringVar(&opts.body, "body", "", "Review message (alias for --message)")
	cmd.Flags().BoolVarP(&opts.approve, "approve", "a", false, "Approve change (Code-Review+2)")
	cmd.Flags().BoolVar(&opts.requestChanges, "request-changes", false, "Request changes (Code-Review-1)")
	cmd.Flags().BoolVarP(&opts.comment, "comment", "c", false, "Comment on change without voting")
	cmd.Flags().IntVar(&opts.cq, "cq", -1, "Set Commit-Queue vote (default 1: 1 = dry run, 2 = submit, 0 = remove)")
	cmd.Flags().Lookup("cq").NoOptDefVal = "1"

	return cmd
}

func init() {
	PrCmd.AddCommand(newReviewCmd())
}
