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

	"github.com/andygrunwald/go-gerrit"
	"github.com/spf13/cobra"
)

type mergeOptions struct {
	auto       bool
	autoSubmit bool
	cq         bool
	message    string
}

func newMergeCmd() *cobra.Command {
	opts := mergeOptions{}
	cmd := &cobra.Command{
		Use:   "merge [<id>]",
		Short: "Merge (submit) a change",
		Long: `Merge (submit) a Gerrit change, or enable auto-submit / Commit-Queue.

When --auto is passed, pw_ghish applies the active project's auto-submit label:
  - Pigweed: Pigweed-Auto-Submit+1
  - Fuchsia: Commit-Queue+2

When --cq is passed, pw_ghish applies the Commit-Queue+2 label directly.

When neither flag is passed, pw_ghish attempts to submit the change immediately.
Note: In Pigweed and LUCI-gated projects, direct submission requires all gates
(Code-Review+2, Presubmit-Verified, etc.) to be satisfied already; otherwise
Gerrit rejects immediate submission. Use --auto or --cq for automated landing.`,
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
			if isAuto && opts.cq {
				return fmt.Errorf("cannot specify both --auto and --cq")
			}

			if isAuto || opts.cq {
				profile, err := chCtx.ResolveProfile()
				if err != nil {
					return err
				}

				var labelName string
				var labelValue int

				if opts.cq {
					if cqLabel, ok := profile.CQLabel(); ok {
						labelName = cqLabel.Name
						labelValue = cqLabel.Value
					} else {
						return fmt.Errorf("profile %q does not support Commit-Queue", profile.Name())
					}
				} else {
					if asLabel, ok := profile.AutoSubmitLabel(); ok {
						labelName = asLabel.Name
						labelValue = asLabel.Value
					} else if cqLabel, ok := profile.CQLabel(); ok {
						labelName = cqLabel.Name
						labelValue = cqLabel.Value
					} else {
						return fmt.Errorf("profile %q does not support auto-submit. To submit immediately, run without --auto", profile.Name())
					}
				}

				reviewInput := &gerrit.ReviewInput{
					Message: opts.message,
					Labels: map[string]int{
						labelName: labelValue,
					},
				}

				if err := chCtx.SetReviewRevision("current", reviewInput); err != nil {
					actionName := "auto-submit"
					if opts.cq {
						actionName = "Commit-Queue"
					}
					return fmt.Errorf("error enabling %s for change %s: %w", actionName, chCtx.ChangeID, err)
				}

				targetDesc := "Auto-submit"
				if opts.cq {
					targetDesc = "Commit-Queue"
				}
				fmt.Fprintf(cmd.OutOrStdout(), "%s enabled for change %s (%s%+d).\n", targetDesc, chCtx.ChangeID, labelName, labelValue)
				return nil
			}

			input := &gerrit.SubmitInput{
				WaitForMerge: true,
			}

			_, resp, err := chCtx.Client.Changes.SubmitChange(chCtx.Context, chCtx.ChangeID, input)
			if err != nil {
				var isConflict bool
				if resp != nil && resp.StatusCode == http.StatusConflict {
					isConflict = true
				} else if strings.Contains(err.Error(), "409") || strings.Contains(strings.ToLower(err.Error()), "conflict") || strings.Contains(strings.ToLower(err.Error()), "not ready") {
					isConflict = true
				}
				if isConflict {
					cmdPath := cmd.CommandPath()
					return fmt.Errorf("change %s cannot be merged immediately: submit requirements (such as Code-Review or Presubmit-Verified) are not yet met.\n\nHint: In Gerrit/LUCI projects, changes are submitted via the Commit Queue:\n  - To enable auto-submission when approved:  %s %s --auto\n  - To trigger Commit-Queue+2 directly:       %s %s --cq", chCtx.ChangeID, cmdPath, chCtx.ChangeID, cmdPath, chCtx.ChangeID)
				}
				return fmt.Errorf("error merging change %s: %w", chCtx.ChangeID, err)
			}

			fmt.Fprintln(cmd.OutOrStdout(), "Change merged (submitted) successfully.")
			return nil
		},
	}

	cmd.Flags().BoolVar(&opts.auto, "auto", false, "Automatically merge after requirements (CI checks and approvals) have been met")
	cmd.Flags().BoolVar(&opts.autoSubmit, "auto-submit", false, "Alias for --auto")
	cmd.Flags().MarkHidden("auto-submit")
	cmd.Flags().BoolVar(&opts.cq, "cq", false, "Trigger submission via Commit-Queue+2 (runs checks and submits when ready)")
	cmd.Flags().StringVar(&opts.message, "message", "", "Optional message when auto-submitting or voting CQ")

	return cmd
}

var mergeCmd = newMergeCmd()

func init() {
	PrCmd.AddCommand(mergeCmd)
}
