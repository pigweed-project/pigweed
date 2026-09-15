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

var readyCmd = &cobra.Command{
	Use:   "ready [<id>]",
	Short: "Mark a change as ready for review",
	Long: `Mark a change as ready for review.

With -u, --undo, converts the change back to work-in-progress (WIP / draft).
An optional message can be supplied with -m, --message.`,
	Args: cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		chCtx, err := ResolveChangeContext(cmd, args)
		if err != nil {
			return err
		}

		undo, _ := cmd.Flags().GetBool("undo")
		message, _ := cmd.Flags().GetString("message")

		if undo {
			if err := chCtx.SetWorkInProgress(message); err != nil {
				return err
			}
			fmt.Fprintln(cmd.OutOrStdout(), "Change marked as work in progress successfully.")
			return nil
		}

		if _, err := chCtx.Client.Changes.SetReadyForReview(chCtx.Context, chCtx.ChangeID, &gerrit.ReadyForReviewInput{Message: message}); err != nil {
			return chCtx.FormatError(err, "marking change as ready for review")
		}

		fmt.Fprintln(cmd.OutOrStdout(), "Change marked ready for review successfully.")
		return nil
	},
}

func init() {
	readyCmd.Flags().StringP("message", "m", "", "Optional message explaining why the change is ready for review (or moving to WIP)")
	readyCmd.Flags().BoolP("undo", "u", false, "Convert the change back to work-in-progress (WIP / draft)")
	PrCmd.AddCommand(readyCmd)
}
