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
	"strings"

	"github.com/spf13/cobra"
)

type mergeOptions struct {
	auto       bool
	autoSubmit bool
	cq         bool
	trigger    bool
	nightly    bool
	submitAt   string
	message    string
}

func newMergeCmd() *cobra.Command {
	opts := mergeOptions{}
	cmd := &cobra.Command{
		Use:         "merge [<id>]",
		Annotations: map[string]string{KindAnnotation: KindMutation},
		Short:       "Merge (submit) a change",
		Long: `Merge (submit) a Gerrit change, or enable auto-submit / Commit-Queue / scheduled submission.

When --auto is passed, pw_ghish detects the host's auto-submit label from the
change (e.g. Pigweed-Auto-Submit+1 or Auto-Submit+1) and votes it. If the host
has no auto-submit label, pw_ghish starts a Commit-Queue+1 dry run (when
available) and returns an error; use --trigger (or --cq) to submit via Commit-Queue+2.

When --trigger (or --cq) is passed, pw_ghish applies the Commit-Queue+2 (or
Autosubmit+1 + Presubmit-Ready+1) vote directly.

When --nightly or --submit-at is passed, pw_ghish creates a Gerrit Flow that,
once the scheduled time in PT/MTV (America/Los_Angeles) is reached, starts a
dry run (Commit-Queue+1, or Presubmit-Ready+1 on hosts that use it) and enables
auto-submit. Auto-submit then lands the change once it is submittable. A
work-in-progress change is marked ready for review. Running it again replaces
the pending schedule.

When none of these flags are passed, pw_ghish attempts to submit the change immediately.
Note: In Pigweed and LUCI-gated projects, direct submission requires all gates
(Code-Review+2, Presubmit-Verified, etc.) to be satisfied already; otherwise
Gerrit rejects immediate submission. Use --auto or --trigger (alias --cq) for automated landing.`,
		Args: cobra.MaximumNArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			chCtx, err := ResolveChangeContext(cmd, args)
			if err != nil {
				return err
			}
			if chCtx.Revision != "" && chCtx.Revision != "current" {
				return fmt.Errorf("cannot merge specific patchset %s: Gerrit submits whole changes, not individual patchsets", chCtx.Revision)
			}

			isAuto := opts.auto || opts.autoSubmit
			isCQ := opts.cq || opts.trigger
			if isAuto && isCQ {
				return fmt.Errorf("cannot specify both --auto and --cq/--trigger")
			}
			isScheduled, err := validateScheduleFlags(opts.nightly, &opts.submitAt, cmd.Flags().Changed("submit-at"), isAuto)
			if err != nil {
				return err
			}
			if isScheduled {
				if isCQ {
					return fmt.Errorf("cannot specify both --cq/--trigger and --nightly/--submit-at")
				}
				// A flow's vote action carries exactly one label vote and no
				// message, so --message has nowhere to go. Refuse it rather
				// than accept a flag that does nothing.
				if cmd.Flags().Changed("message") {
					return fmt.Errorf("cannot specify --message with --nightly/--submit-at: scheduled Gerrit Flows vote labels without a message\n\n"+
						"To leave a note on the change, post it separately:\n"+
						"  %s pr comment %s -m <message>", RootCmd.CommandPath(), chCtx.ChangeID)
				}
				scheduled, err := chCtx.CreateScheduledSubmitFlow(0, opts.submitAt, DefaultScheduleTimezone)
				if err != nil {
					return err
				}
				PrintReceipt(cmd.OutOrStdout(), Receipt{
					Summary: scheduled.Summary(),
					Next:    fmt.Sprintf("gh pr view %s", chCtx.ChangeID),
				})
				return nil
			}

			if isAuto || isCQ {
				var votes []LabelVote
				// Set when the host cannot auto-submit; reported once the
				// vote that *could* be cast has been.
				var unsupported error

				if isCQ {
					var err error
					votes, err = chCtx.DecideCQSubmit()
					if err != nil {
						return err
					}
				} else {
					// Ask the host what its auto-submit label is called rather
					// than assuming the profile's spelling. Hosts disagree
					// (Pigweed-Auto-Submit, Fuchsia-Auto-Submit, Auto-Submit),
					// and voting a name the host does not have is rejected.
					decision, err := chCtx.DecideAutoSubmit()
					if err != nil {
						return err
					}
					votes = []LabelVote{decision.Vote}
					unsupported = decision.Unsupported
				}

				reviewInput := &ReviewInput{
					Message: opts.message,
					Labels:  make(map[string]int, len(votes)),
				}
				var voteStrs []string
				for _, v := range votes {
					reviewInput.Labels[v.Name] = v.Value
					voteStrs = append(voteStrs, fmt.Sprintf("%s%+d", v.Name, v.Value))
				}

				if err := chCtx.SetReviewRevision("current", reviewInput); err != nil {
					actionName := "auto-submit"
					if isCQ {
						actionName = "Commit-Queue"
					}
					return fmt.Errorf("error enabling %s for change %s: %w", actionName, chCtx.ChangeID, err)
				}

				// The vote landed, but it was a consolation prize: say so
				// instead of reporting auto-submit that is not happening.
				if unsupported != nil {
					PrintReceipt(cmd.OutOrStdout(), Receipt{
						Summary: fmt.Sprintf("%s set for change %s.", strings.Join(voteStrs, ", "), chCtx.ChangeID),
					})
					return unsupported
				}

				targetDesc := "Auto-submit"
				if isCQ {
					targetDesc = "Commit-Queue"
				}
				PrintReceipt(cmd.OutOrStdout(), Receipt{
					Summary: fmt.Sprintf("%s enabled for change %s (%s).", targetDesc, chCtx.ChangeID, strings.Join(voteStrs, ", ")),
					Next:    fmt.Sprintf("gh pr checks %s --watch [--fail-fast]", chCtx.ChangeID),
				})
				return nil
			}

			_, err = chCtx.Client.SubmitChange(chCtx.Context, chCtx.ChangeID)
			if err != nil {
				if IsGerritHTTPStatus(err, http.StatusConflict) {
					cmdPath := cmd.CommandPath()
					return fmt.Errorf("change %s cannot be merged immediately: submit requirements (such as Code-Review or Presubmit-Verified) are not yet met.\n\nHint: In Gerrit/LUCI projects, changes are submitted via the Commit Queue:\n  - To enable auto-submission when approved:  %s %s --auto\n  - To trigger Commit-Queue+2 directly:       %s %s --cq", chCtx.ChangeID, cmdPath, chCtx.ChangeID, cmdPath, chCtx.ChangeID)
				}
				return fmt.Errorf("error merging change %s: %w", chCtx.ChangeID, err)
			}

			PrintReceipt(cmd.OutOrStdout(), Receipt{
				Summary: "Change merged (submitted) successfully.",
				Next:    fmt.Sprintf("gh pr view %s", chCtx.ChangeID),
			})
			return nil
		},
	}

	cmd.Flags().BoolVar(&opts.auto, "auto", false, "Automatically merge after requirements (CI checks and approvals) have been met")
	cmd.Flags().BoolVar(&opts.autoSubmit, "auto-submit", false, "Alias for --auto")
	cmd.Flags().MarkHidden("auto-submit")
	cmd.Flags().BoolVar(&opts.trigger, "trigger", false, "Trigger submission via Commit-Queue+2 or Autosubmit+Presubmit-Ready (alias --cq)")
	cmd.Flags().BoolVar(&opts.cq, "cq", false, "Trigger submission via Commit-Queue+2 (runs checks and submits when ready; alias for --trigger)")
	cmd.Flags().BoolVar(&opts.nightly, "nightly", false, "Schedule submission via Gerrit Flows during off-peak hours (21:00-03:00 PT/MTV)")
	cmd.Flags().StringVar(&opts.submitAt, "submit-at", "", "Schedule submission via Gerrit Flows at a specific 24-hour time in PT/MTV (e.g. \"21:45\")")
	cmd.Flags().StringVar(&opts.message, "message", "", "Optional message when auto-submitting or voting CQ")

	return cmd
}

var mergeCmd = newMergeCmd()

func init() {
	PrCmd.AddCommand(mergeCmd)
}
