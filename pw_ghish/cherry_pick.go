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

var cherryPickCmd = &cobra.Command{
	Use:   "cherry-pick <id>",
	Short: "Cherry-pick a pull request's changes locally",
	Args:  cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		chCtx, err := ResolveChangeContext(cmd, args)
		if err != nil {
			return err
		}

		git, err := chCtx.GitClient()
		if err != nil {
			return err
		}

		opt := &gerrit.ChangeOptions{}
		if chCtx.Revision != "" && chCtx.Revision != "current" {
			opt.AdditionalFields = []string{"ALL_REVISIONS"}
		} else {
			opt.AdditionalFields = []string{"CURRENT_REVISION"}
		}

		change, err := chCtx.GetChange(opt)
		if err != nil {
			return err
		}

		revision, err := chCtx.ExtractRevision(change)
		if err != nil {
			return err
		}

		ref, err := chCtx.ExtractFetchRef(change, revision)
		if err != nil {
			return err
		}

		fmt.Fprintf(cmd.OutOrStdout(), "Fetching ref %q...\n", ref)

		if err := git.Fetch(chCtx.Context, "origin", ref, cmd.OutOrStdout(), cmd.ErrOrStderr()); err != nil {
			return fmt.Errorf("failed to fetch ref: %w", err)
		}

		fmt.Fprintln(cmd.OutOrStdout(), "Cherry-picking FETCH_HEAD...")
		if err := git.CherryPick(chCtx.Context, "FETCH_HEAD", cmd.OutOrStdout(), cmd.ErrOrStderr()); err != nil {
			return fmt.Errorf("cherry-pick halted with an error (e.g. merge conflicts or uncommitted changes): %w\n\n"+
				"To resolve merge conflicts:\n"+
				"  1. Inspect conflicted files:          git status\n"+
				"  2. Edit conflicted files to resolve markers\n"+
				"  3. Mark files as resolved:            git add <file>...\n"+
				"  4. Complete the cherry-pick:          git cherry-pick --continue\n\n"+
				"To abort the cherry-pick cleanly:\n"+
				"  git cherry-pick --abort", err)
		}

		fmt.Fprintf(cmd.OutOrStdout(), "Change %d cherry-picked successfully.\n", change.Number)
		return nil
	},
}

func init() {
	PrCmd.AddCommand(cherryPickCmd)
}
