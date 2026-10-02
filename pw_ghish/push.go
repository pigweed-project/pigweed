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
	force, _ := cmd.Flags().GetBool("force")

	state, err := VerifyHeadForPush(ctx, cmd, cfg, !force)
	if err != nil {
		return err
	}
	if err := CheckTopicAllowed(ctx, cfg, state.ExistingChange, RequestedTopic(flags.PushOptions), "gh pr push (without --topic)"); err != nil {
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

	if err := checkPushSubmodulePolicy(ctx, cmd, cfg, state.ExistingChange, branch, flags.Stack); err != nil {
		return err
	}

	if !force {
		if flags.Stack {
			if err := VerifyStackChanges(ctx, cmd, cfg, branch, state, "push"); err != nil {
				return err
			}
		}
		if state.GerritQueried && state.ExistingChange == nil {
			return FormatMissingChangePushError(ctx, cmd, cfg, state, flags.Stack)
		}
	}

	// Pushing %l=<label> for a label the host does not define is rejected
	// outright, taking the whole patchset with it, so the name is looked up
	// rather than assumed. An existing change lists its own labels; a commit
	// Gerrit has never seen has to ask the project.
	if flags.PushOptions.CQ > 0 && state.ExistingChange != nil && len(state.ExistingChange.Labels) > 0 {
		cqName, cqScore := ResolveCQVoteForLabels(state.ExistingChange.Labels, flags.PushOptions.CQ)
		flags.PushOptions.CQLabelName = cqName
		flags.PushOptions.CQ = cqScore
	}

	var autoSubmit AutoSubmitDecision
	if flags.PushOptions.AutoSubmit {
		if state.ExistingChange != nil {
			autoSubmit, err = DecideAutoSubmit(state.ExistingChange.Labels, fmt.Sprintf("change %d", state.ExistingChange.Number))
		} else {
			autoSubmit, err = decideProjectAutoSubmit(ctx, cmd, cfg)
		}
		if err != nil {
			return err
		}
		flags.PushOptions.AutoSubmitLabel = autoSubmit.Vote
		flags.PushOptions.AutoSubmitUnsupported = autoSubmit.Unsupported
	}

	fmt.Fprintf(cmd.OutOrStdout(), "Pushing patchset for branch %s...\n", branch)

	if err := executePush(ctx, cmd, cfg, branch, flags.PushOptions, flags.NoVerify); err != nil {
		if autoSubmit.Unsupported != nil && err == autoSubmit.Unsupported {
			return err
		}
		return fmt.Errorf("error pushing patchset: %w", err)
	}

	fmt.Fprintln(cmd.OutOrStdout(), "\nPatchset pushed successfully.")
	// The patchset is up, so this is reported last: the push is not the part
	// that failed, the promise that something would submit it is.
	return autoSubmit.Unsupported
}

func newPushCommand() *cobra.Command {
	cmd := &cobra.Command{
		Use:     "push",
		Aliases: []string{"upload"},
		Short:   "Push current HEAD commit to Gerrit as a patchset",
		Long: `Push the current HEAD commit to Gerrit to upload a new patchset.

If no change with HEAD's Change-Id exists on Gerrit, this command will error
and suggest using 'gh pr create' (or restoring an overwritten Change-Id). Use --force to bypass this check.

Supports rich push options:
  - Reviewers and CCs: --reviewer, --cc
  - Auto-submit: --auto (alias --auto-submit)
  - Presubmit / Commit queue: --trigger (alias --cq; default dry run, or specify vote: 1 = dry run, 2 = submit)
  - Publish draft comments: --publish
  - Mark ready for review: --ready
  - Work in progress: --draft
  - Raw push options: -o / --push-option
  - Skip pre-push hooks: --no-verify`,
		RunE: runPush,
	}

	AddCommonPushFlags(cmd)
	cmd.Flags().Bool("ready", false, "Mark as ready for review (removes WIP)")
	cmd.Flags().Bool("force", false, "Force push even if no change with this Change-Id exists on Gerrit")

	return cmd
}

func init() {
	PrCmd.AddCommand(newPushCommand())
	RootCmd.AddCommand(newPushCommand())
}
