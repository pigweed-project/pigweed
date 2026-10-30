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

	"github.com/spf13/cobra"
)

var closeCmd = &cobra.Command{
	Use:         "close [<id>]",
	Annotations: map[string]string{KindAnnotation: KindMutation},
	Short:       "Close (abandon) a change",
	Args:        cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		chCtx, err := ResolveChangeContext(cmd, args)
		if err != nil {
			return err
		}

		_, err = chCtx.Client.AbandonChange(chCtx.Context, chCtx.ChangeID, "Abandoned via gh-ish")
		if err != nil {
			return chCtx.FormatError(err, "closing")
		}

		PrintReceipt(cmd.OutOrStdout(), Receipt{
			Summary: fmt.Sprintf("Change %s closed (abandoned) successfully.", chCtx.ChangeID),
			Next:    fmt.Sprintf("gh pr reopen %s  (or gh pr list)", chCtx.ChangeID),
		})
		return nil
	},
}

func init() {
	PrCmd.AddCommand(closeCmd)
}
