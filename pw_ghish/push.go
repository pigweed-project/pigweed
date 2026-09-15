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

func runPush(cmd *cobra.Command, args []string) error {
	ctx := cmd.Context()
	cfg := GetConfig(cmd)

	flags := ParseCommonPushFlags(cmd)

	// Ensure HEAD commit has a Gerrit Change-Id
	if err := EnsureChangeID(ctx, cfg, cmd.OutOrStdout(), cmd.ErrOrStderr()); err != nil {
		return fmt.Errorf("error verifying Change-Id: %w", err)
	}

	// Read once: both the syntax guard and the branch-memory lookup below
	// need the HEAD commit message, and the guard must run even when --base
	// makes the lookup unnecessary.
	logMsg, err := cfg.GitClient().HeadCommitMessage(ctx)
	if err != nil {
		return fmt.Errorf("error reading HEAD commit message: %w", err)
	}
	if err := CheckGitHubIssueSyntax(logMsg, "the HEAD commit message"); err != nil {
		return err
	}

	branch := flags.Base
	if branch == "" {
		// Attempt to remember the origin branch from Gerrit for this Change-Id
		changeID := ExtractChangeID(logMsg)
		if changeID != "" {
			client, cErr := NewGerritClient(ctx, cmd)
			if cErr == nil && client != nil {
				opt := &gerrit.QueryChangeOptions{}
				opt.Query = []string{changeID}
				changes, _, qErr := client.Changes.QueryChanges(ctx, opt)
				if qErr == nil && changes != nil && len(*changes) > 0 {
					existing := (*changes)[0]
					if existing.Branch != "" {
						branch = existing.Branch
					}
				}
			}
		}
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
