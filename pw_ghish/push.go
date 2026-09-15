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

func runPush(cmd *cobra.Command, args []string) error {
	ctx := cmd.Context()
	cfg := GetConfig(cmd)

	flags := ParseCommonPushFlags(cmd)

	state, err := VerifyHeadForPush(ctx, cmd, cfg, flags.Base == "")
	if err != nil {
		return err
	}

	branch := flags.Base
	if branch == "" && state.ExistingChange != nil {
		branch = state.ExistingChange.Branch
	}
	if branch == "" {
		branch = resolvePushBranch(ctx, cfg, flags.Base, cmd.ErrOrStderr())
	}

	if err := ValidateCommitStack(ctx, cfg.GitClient(), branch, flags.Stack, "push"); err != nil {
		return err
	}

	fmt.Fprintf(cmd.OutOrStdout(), "Pushing patchset for branch %s...\n", branch)

	if err := executePush(ctx, cmd, cfg, branch, flags.PushOptions, flags.NoVerify); err != nil {
		return fmt.Errorf("error pushing patchset: %w", err)
	}

	fmt.Fprintln(cmd.OutOrStdout(), "\nPatchset pushed successfully.")
	return nil
}

func newPushCommand() *cobra.Command {
	cmd := &cobra.Command{
		Use:     "push",
		Aliases: []string{"upload"},
		Short:   "Push current HEAD commit to Gerrit as a patchset",
		Long: `Push the current HEAD commit to Gerrit to upload a new patchset.
Supports rich push options:
  - Reviewers and CCs: --reviewer, --cc
  - Auto-submit: --auto (alias --auto-submit)
  - Commit queue: --cq (default dry run, or specify vote: 1 = dry run, 2 = submit)
  - Publish draft comments: --publish
  - Mark ready for review: --ready
  - Work in progress: --draft
  - Raw push options: -o / --push-option
  - Skip pre-push hooks: --no-verify`,
		RunE: runPush,
	}

	AddCommonPushFlags(cmd)
	cmd.Flags().Bool("ready", false, "Mark as ready for review (removes WIP)")

	return cmd
}

func init() {
	PrCmd.AddCommand(newPushCommand())
	RootCmd.AddCommand(newPushCommand())
}
