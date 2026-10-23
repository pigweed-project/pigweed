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
	"context"
	"errors"
	"fmt"
	"strings"

	"github.com/spf13/cobra"
)

func runPush(cmd *cobra.Command, args []string) error {
	ctx := cmd.Context()
	cfg := GetConfig(cmd)

	flags := ParseCommonPushFlags(cmd)
	if err := ValidatePushScheduleFlags(cmd, &flags.PushOptions); err != nil {
		return err
	}
	cmFlags := ParseCommitMessageFlags(cmd)
	force, _ := cmd.Flags().GetBool("force")

	guard, prefetched, branch, err := prepareCommitForPush(ctx, cmd, cfg, &cmFlags, flags, force)
	if err != nil {
		return err
	}
	defer guard.Rollback(ctx, cmd.ErrOrStderr())

	state, err := verifyHeadForPush(ctx, cmd, cfg, !force, flags.NoVerify, prefetched)
	if err != nil {
		return err
	}
	if err := CheckTopicAllowed(ctx, cfg, state.ExistingChange, RequestedTopic(flags.PushOptions), "gh pr push (without --topic)"); err != nil {
		return err
	}
	if state.ExistingChange != nil {
		flags.PushOptions.ChangeNumber = state.ExistingChange.Number
	}

	// The schedule is created after the push, so on a host without Gerrit
	// Flows the patchset would be uploaded and only then would scheduling
	// fail. When the change already exists, ask first. (For --stack, the
	// other changes are checked as they are scheduled.)
	if (flags.PushOptions.Nightly || flags.PushOptions.SubmitAt != "") && state.ExistingChange != nil {
		client, err := NewGerritClient(ctx, cmd)
		if err != nil {
			return err
		}
		if err := checkFlowsEnabled(ctx, client, cfg.GerritHost(ctx), state.ExistingChange.Number); err != nil {
			return err
		}
	}

	if flags.Base == "" && state.ExistingChange != nil && state.ExistingChange.Branch != "" {
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

	flags.PushOptions.Reviewers, err = ExpandReviewersForPush(ctx, cmd, cfg, branch, state.ExistingChange, flags.PushOptions.Reviewers)
	if err != nil {
		return err
	}

	if flags.PushOptions.StackChangeIDs, err = stackChangeIDsToSchedule(ctx, cfg, branch, flags); err != nil {
		return err
	}

	fmt.Fprintf(cmd.OutOrStdout(), "Pushing patchset for branch %s...\n", branch)

	if err := executePush(ctx, cmd, cfg, branch, flags.PushOptions, flags.NoVerify); err != nil {
		if autoSubmit.Unsupported != nil && err == autoSubmit.Unsupported {
			guard.Keep()
			return err
		}
		// The patchset is up; only the schedule failed. Prefixing this with
		// "error pushing patchset" would send the user to push again.
		var flowErr *ScheduleFlowError
		if errors.As(err, &flowErr) {
			guard.Keep()
			return err
		}
		return fmt.Errorf("error pushing patchset: %w", err)
	}

	guard.Keep()
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
  - Commit message & bug trailers: --title, --body, --body-file, --bug, --fixed
  - Reviewers and CCs: --reviewer, --cc
  - Auto-submit: --auto (alias --auto-submit)
  - Scheduled submission (Gerrit Flows): --nightly, --submit-at <HH:MM>
  - Presubmit / Commit queue: --trigger (alias --cq; default dry run, or specify vote: 1 = dry run, 2 = submit)
  - Publish draft comments: --publish
  - Mark ready for review: --ready
  - Work in progress: --draft
  - Raw push options: -o / --push-option
  - Skip pre-push hooks: --no-verify`,
		RunE: runPush,
	}

	AddCommonPushFlags(cmd)
	AddCommitMessageFlags(cmd)
	cmd.Flags().Bool("ready", false, "Mark as ready for review (removes WIP)")
	cmd.Flags().Bool("force", false, "Force push even if no change with this Change-Id exists on Gerrit")

	return cmd
}

func init() {
	PrCmd.AddCommand(newPushCommand())
	RootCmd.AddCommand(newPushCommand())
}

func prepareCommitForPush(
	ctx context.Context,
	cmd *cobra.Command,
	cfg *Config,
	cmFlags *CommitMessageFlags,
	flags CommonPushFlags,
	force bool,
) (*headRollbackGuard, *VerifiedPushState, string, error) {
	if !cmFlags.HasAny(cmd) {
		return newHeadRollbackGuard(ctx, cfg, "gh pr push: rollback failed push"), nil, "", nil
	}

	resolved, err := ResolveAndValidateCommitMessageFlags(cmd, cmFlags, "gh pr push")
	if err != nil {
		return nil, nil, "", err
	}
	trailerFmt, err := ResolveBugTrailerFormat(ctx, cfg)
	if err != nil {
		return nil, nil, "", err
	}
	if err := cfg.GitClient().CheckAmendAllowed(ctx); err != nil {
		return nil, nil, "", err
	}

	origMsg, err := cfg.GitClient().HeadCommitMessage(ctx)
	if err != nil {
		return nil, nil, "", fmt.Errorf("failed to read HEAD commit message: %w", err)
	}

	var prefetched *VerifiedPushState
	origChangeID := ExtractChangeID(origMsg)
	if !force && origChangeID != "" {
		if client, err := NewGerritClient(ctx, cmd); err == nil {
			existing, queried := queryExistingChangeByID(ctx, client, origChangeID, "DETAILED_LABELS", "SUBMIT_REQUIREMENTS")
			prefetched = &VerifiedPushState{
				CommitMsg:      origMsg,
				ChangeID:       origChangeID,
				ExistingChange: existing,
				GerritQueried:  queried,
			}
		}
	}
	if prefetched != nil && prefetched.GerritQueried && prefetched.ExistingChange == nil && !bodyOverridesChangeID(origChangeID, resolved) {
		return nil, nil, "", FormatMissingChangePushError(ctx, cmd, cfg, prefetched, flags.Stack)
	}

	branch := flags.Base
	if branch == "" && prefetched != nil && prefetched.ExistingChange != nil {
		branch = prefetched.ExistingChange.Branch
	}
	if branch == "" {
		branch = resolvePushBranch(ctx, cfg, flags.Base, cmd.ErrOrStderr())
	}

	aheadCount, hasRemoteRef, err := validateCommitMsgSingleCommit(ctx, cfg, branch, "push")
	if err != nil {
		return nil, nil, "", err
	}
	if hasRemoteRef && aheadCount == 0 {
		remoteName := cfg.GitClient().ResolveRemote(ctx)
		return nil, nil, "", fmt.Errorf(
			"cannot apply commit-message flags: there is no unpushed local commit ahead of %s/%s.\n\n"+
				"Why: Amending HEAD here would rewrite an upstream commit rather than updating an unpushed Gerrit patchset.\n"+
				"To create a new change from uncommitted edits, run:\n"+
				"  gh pr create -t \"<module>: <summary>\"\n\n"+
				"Or stage and commit your changes first:\n"+
				"  git add <files> && git commit",
			remoteName, branch,
		)
	}

	guard := newHeadRollbackGuard(ctx, cfg, "gh pr push: rollback failed push")
	if err := guard.AmendWithFlags(ctx, cmd, trailerFmt, resolved); err != nil {
		return nil, nil, "", err
	}
	return guard, prefetched, branch, nil
}

func bodyOverridesChangeID(origChangeID string, resolved *ResolvedCommitMessageFlags) bool {
	if resolved == nil || !resolved.HasBody || strings.TrimSpace(resolved.Body) == "" {
		return false
	}
	parsed := ParseCommitMessage("subject\n\n" + strings.TrimRight(resolved.Body, "\r\n"))
	for _, p := range parsed.Paragraphs[parsed.ActiveFooterStart():] {
		for _, tr := range p.Trailers {
			if !tr.IsKeylessFooter && strings.EqualFold(tr.Key, "Change-Id") {
				val := strings.TrimSpace(tr.Value)
				return val != "" && val != origChangeID
			}
		}
	}
	return false
}
