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

var checkoutBranch string

var checkoutCmd = &cobra.Command{
	Use:   "checkout [<id>]",
	Short: "Check out a pull request's branch locally",
	Args:  cobra.MaximumNArgs(1),
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

		fmt.Fprintf(cmd.OutOrStdout(), "Fetching ref %s...\n", ref)

		if err := git.Fetch(chCtx.Context, "origin", ref, cmd.OutOrStdout(), cmd.ErrOrStderr()); err != nil {
			return fmt.Errorf("error fetching ref: %w", err)
		}

		if checkoutBranch != "" {
			fmt.Fprintf(cmd.OutOrStdout(), "Checking out FETCH_HEAD into branch %q...\n", checkoutBranch)
			if err := git.Run(chCtx.Context, cmd.OutOrStdout(), cmd.ErrOrStderr(), "checkout", "-b", checkoutBranch, "FETCH_HEAD"); err != nil {
				return fmt.Errorf("failed to check out change %d onto branch %q: %w\n\n"+
					"Hint: If you have uncommitted changes that would be overwritten:\n"+
					"  1. Stash changes:     git stash\n"+
					"  2. Re-run checkout:   gh pr checkout %s -b %s\n"+
					"  3. Restore changes:   git stash pop\n"+
					"Or if the branch already exists, delete it or specify a different branch name.",
					change.Number, checkoutBranch, err, chCtx.ChangeID, checkoutBranch)
			}
			fmt.Fprintf(cmd.OutOrStdout(), "Checked out change %d onto new branch %q.\n", change.Number, checkoutBranch)
			return nil
		}

		fmt.Fprintln(cmd.OutOrStdout(), "Checking out FETCH_HEAD...")
		if err := git.Checkout(chCtx.Context, "FETCH_HEAD", cmd.OutOrStdout(), cmd.ErrOrStderr()); err != nil {
			return fmt.Errorf("error checking out change %d: %w\n\n"+
				"Hint: If you have uncommitted changes that would be overwritten:\n"+
				"  1. Stash changes:     git stash\n"+
				"  2. Re-run checkout:   gh pr checkout %s\n"+
				"  3. Restore changes:   git stash pop",
				change.Number, err, chCtx.ChangeID)
		}

		fmt.Fprintf(cmd.OutOrStdout(), "Checked out change %d at FETCH_HEAD.\n\n"+
			"Notice: You are in 'detached HEAD' state. To create a local branch for this change, run:\n"+
			"  git checkout -b <branch-name> FETCH_HEAD\n", change.Number)
		return nil
	},
}

func init() {
	checkoutCmd.Flags().StringVarP(&checkoutBranch, "branch", "b", "", "Local branch name to create and check out into")
	PrCmd.AddCommand(checkoutCmd)
}
