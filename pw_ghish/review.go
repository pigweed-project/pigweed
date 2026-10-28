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

type reviewOptions struct {
	message        string
	body           string
	bodyFile       string
	approve        bool
	requestChanges bool
	comment        bool
	publish        bool
	cq             int
	trigger        int
	agent          agentFlag
}

func newReviewCmd() *cobra.Command {
	opts := reviewOptions{cq: -1, trigger: -1, agent: agentAuto}
	cmd := &cobra.Command{
		Use:         "review [<id>]",
		Annotations: map[string]string{KindAnnotation: KindMutation},
		Short:       "Review a change",
		Long: `Review a Gerrit change by adding a message, approving, requesting changes, publishing draft comments, or voting on presubmit / Commit-Queue.

If no change ID is specified, the active change for the current branch is reviewed.

Examples:
# Trigger presubmit / Commit-Queue dry run on the active change
gh-ish pr review --trigger

# Publish all staged draft comments on the active change
gh-ish pr review --publish

# Approve the active change on the current branch
gh-ish pr review --approve -m "Looks good to me"

# Approve and trigger presubmit / Commit-Queue dry run
gh-ish pr review --approve --trigger

# Approve a change by number with a message
gh-ish pr review 12345 --approve -m "Looks good to me"

# Request changes on a specific change
gh-ish pr review 12345 --request-changes -m "Please fix the typo"

# Leave a comment without voting
gh-ish pr review 12345 --comment -m "Just a question"`,
		Args: cobra.MaximumNArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			msg, err := resolveCommentMessage(opts.message, opts.body, opts.bodyFile, cmd.InOrStdin(), false)
			if err != nil {
				return err
			}
			if strings.TrimSpace(msg) == "" {
				msg = ""
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

			if opts.comment && msg == "" {
				return fmt.Errorf("--comment requires a message (specify -m \"...\", -b \"...\", or -F <file>)")
			}

			hasCQ := cmd.Flags().Changed("cq") || cmd.Flags().Changed("trigger")
			if !opts.approve && !opts.requestChanges && !opts.comment && msg == "" && !hasCQ && !opts.publish {
				targetID := "<id>"
				if len(args) > 0 {
					targetID = args[0]
				}
				return fmt.Errorf("no review action or message specified.\n\n"+
					"Specify at least one review action or message:\n"+
					"  gh pr review %s --trigger                        # Trigger presubmit / CQ dry run (alias --cq)\n"+
					"  gh pr review %s --publish                        # Publish staged draft comments\n"+
					"  gh pr review %s --approve                        # Vote Code-Review+2\n"+
					"  gh pr review %s --approve --trigger              # Approve and trigger presubmit / CQ dry run\n"+
					"  gh pr review %s --approve -m \"Looks great!\"       # Approve with a message\n"+
					"  gh pr review %s --request-changes -m \"See typo\"   # Vote Code-Review-1\n"+
					"  gh pr review %s --comment -m \"Just a question\"    # Leave comment without voting\n"+
					"  gh pr review %s -m \"Review message\"               # Leave message without voting",
					targetID, targetID, targetID, targetID, targetID, targetID, targetID, targetID)
			}

			chCtx, err := ResolveChangeContext(cmd, args)
			if err != nil {
				return err
			}

			input := &gerrit.ReviewInput{
				Message: msg,
				Labels:  make(map[string]int),
				Drafts:  "KEEP",
			}
			if opts.publish {
				input.Drafts = "PUBLISH_ALL_REVISIONS"
			}

			reviewLabel := LabelVote{Name: "Code-Review", Value: 2}
			if p, err := chCtx.ResolveProfile(); err == nil && p != nil {
				if rl := p.ReviewLabel(); rl.Name != "" {
					reviewLabel = rl
				}
			}
			var actions []string
			votedCQ := false
			if opts.approve {
				input.Labels[reviewLabel.Name] = reviewLabel.Value
				actions = append(actions, fmt.Sprintf("%s+%d", reviewLabel.Name, reviewLabel.Value))
			} else if opts.requestChanges {
				input.Labels[reviewLabel.Name] = -1
				actions = append(actions, fmt.Sprintf("%s-1", reviewLabel.Name))
			}
			if hasCQ {
				requestedCQ := opts.cq
				if cmd.Flags().Changed("trigger") {
					requestedCQ = opts.trigger
				}
				cqName, cqScore := chCtx.ResolveCQVote(requestedCQ)
				input.Labels[cqName] = cqScore
				if cqScore == 0 {
					actions = append(actions, fmt.Sprintf("%s=0", cqName))
				} else {
					actions = append(actions, fmt.Sprintf("%s+%d", cqName, cqScore))
					if cqScore > 0 {
						votedCQ = true
					}
				}
			}
			if opts.publish {
				actions = append(actions, "published pending drafts")
			}
			if msg != "" {
				actions = append(actions, "message")
			}

			isAI := opts.agent.isAI()
			if msg != "" && isAI != nil && *isAI {
				if err := postReview(chCtx.Context, chCtx.Client, chCtx.ChangeID, chCtx.Revision, aiMessageReview(*input, msg, isAI)); err != nil {
					return chCtx.FormatError(err, "setting review for")
				}
			} else if err := chCtx.SetReview(input); err != nil {
				return err
			}

			nextStep := fmt.Sprintf("gh pr view %s", chCtx.ChangeID)
			if votedCQ {
				nextStep = fmt.Sprintf("gh pr checks %s --watch [--fail-fast]", chCtx.ChangeID)
			}
			if len(actions) > 0 {
				PrintReceipt(cmd.OutOrStdout(), Receipt{
					Summary: fmt.Sprintf("Review submitted successfully (%s).", strings.Join(actions, ", ")),
					Next:    nextStep,
				})
			} else {
				PrintReceipt(cmd.OutOrStdout(), Receipt{
					Summary: "Review submitted successfully.",
					Next:    nextStep,
				})
			}
			return nil
		},
	}

	cmd.Flags().StringVarP(&opts.message, "message", "m", "", "Review message")
	cmd.Flags().StringVarP(&opts.body, "body", "b", "", "Review message (alias for --message)")
	cmd.Flags().StringVarP(&opts.bodyFile, "body-file", "F", "", "Read review message from file (use \"-\" to read from standard input)")
	cmd.Flags().BoolVarP(&opts.approve, "approve", "a", false, "Approve change (Code-Review+2)")
	cmd.Flags().BoolVarP(&opts.requestChanges, "request-changes", "r", false, "Request changes (Code-Review-1)")
	cmd.Flags().BoolVarP(&opts.comment, "comment", "c", false, "Comment on change without voting")
	cmd.Flags().BoolVar(&opts.publish, "publish", false, "Publish all pending draft comments (ghish-only)")
	cmd.Flags().IntVar(&opts.trigger, "trigger", -1, "Trigger presubmit / Commit-Queue vote (default 1: 1 = dry run, 2 = submit, 0 = remove; alias --cq) (ghish-only)")
	cmd.Flags().Lookup("trigger").NoOptDefVal = "1"
	cmd.Flags().IntVar(&opts.cq, "cq", -1, "Set Commit-Queue vote (default 1: 1 = dry run, 2 = submit, 0 = remove; alias for --trigger) (ghish-only)")
	cmd.Flags().Lookup("cq").NoOptDefVal = "1"
	cmd.Flags().VarPF(&opts.agent, "agent", "", agentFlagUsage).NoOptDefVal = string(agentTrue)

	return cmd
}

func init() {
	PrCmd.AddCommand(newReviewCmd())
}
