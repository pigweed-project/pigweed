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
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"strings"

	"github.com/spf13/cobra"
)

var createCmd = &cobra.Command{
	Use:         "create",
	Annotations: map[string]string{KindAnnotation: KindMutation},
	Short:       "Create a new change by pushing current HEAD to Gerrit",
	Long: `Create a new Gerrit change by pushing the current HEAD commit to refs/for/<branch>.

If a change with the same Change-Id already exists on Gerrit, this command will error
and suggest using 'gh pr push' to upload a new patchset instead. Use --force to bypass this check.

Supports rich push options:
  - Commit message & bug trailers: --title, --body, --body-file, --bug, --fixed
  - Reviewers and CCs: --reviewer, --cc
  - Auto-submit: --auto (alias --auto-submit)
  - Scheduled submission (Gerrit Flows): --nightly, --submit-at <HH:MM>
  - Presubmit / Commit queue: --trigger (alias --cq; default dry run, or specify vote: 1 = dry run, 2 = submit)
  - Publish draft comments: --publish
  - Raw push options: -o / --push-option
  - Work in progress: --draft
  - Skip pre-push hooks: --no-verify`,
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		cfg := GetConfig(cmd)

		flags := ParseCommonPushFlags(cmd)
		if err := ValidatePushScheduleFlags(cmd, &flags.PushOptions); err != nil {
			return err
		}
		force, _ := cmd.Flags().GetBool("force")

		guard, branch, err := prepareCommitForCreate(ctx, cmd, cfg, flags)
		if err != nil {
			return err
		}
		defer guard.Rollback(ctx, cmd.ErrOrStderr())

		state, err := VerifyHeadForPush(ctx, cmd, cfg, !force, flags.NoVerify)
		if err != nil {
			return err
		}
		if err := CheckTopicAllowed(ctx, cfg, state.ExistingChange, RequestedTopic(flags.PushOptions), "gh pr create (without --topic)"); err != nil {
			return err
		}
		if state.ExistingChange != nil {
			gerritURL, _ := cfg.GerritURL(ctx)
			baseURL := strings.TrimSuffix(gerritURL, "/a")
			changeURL := fmt.Sprintf("%s/c/%s/+/%d", baseURL, state.ExistingChange.Project, state.ExistingChange.Number)
			return fmt.Errorf("a pull request for Change-Id %s already exists:\n  %s\n\nTo upload a new patchset, run:\n  gh pr push", state.ChangeID, changeURL)
		}

		// A change that does not exist yet has no labels to read, but the
		// project it is going to does. Ask, so --auto votes this host's label
		// rather than whatever the profile happens to be compiled with.
		var autoSubmit AutoSubmitDecision
		if flags.PushOptions.AutoSubmit {
			autoSubmit, err = decideProjectAutoSubmit(ctx, cmd, cfg)
			if err != nil {
				return err
			}
			flags.PushOptions.AutoSubmitLabel = autoSubmit.Vote
			flags.PushOptions.AutoSubmitUnsupported = autoSubmit.Unsupported
		}

		if err := ValidateCommitStack(ctx, cfg.GitClient(), branch, flags.Stack, "create"); err != nil {
			return err
		}

		if err := checkPushSubmodulePolicy(ctx, cmd, cfg, state.ExistingChange, branch, flags.Stack); err != nil {
			return err
		}

		if flags.Stack && !force {
			if err := VerifyStackChanges(ctx, cmd, cfg, branch, state, "create"); err != nil {
				return err
			}
		}

		flags.PushOptions.Reviewers, err = ExpandReviewersForPush(ctx, cmd, cfg, branch, nil, flags.PushOptions.Reviewers)
		if err != nil {
			return err
		}

		if flags.PushOptions.StackChangeIDs, err = stackChangeIDsToSchedule(ctx, cfg, branch, flags); err != nil {
			return err
		}

		fmt.Fprintf(cmd.OutOrStdout(), "Creating change for branch %s...\n", branch)

		var pushBuf bytes.Buffer
		origOut := cmd.OutOrStdout()
		origErr := cmd.ErrOrStderr()
		cmd.SetOut(io.MultiWriter(origOut, &pushBuf))
		cmd.SetErr(io.MultiWriter(origErr, &pushBuf))
		err = executePush(ctx, cmd, cfg, branch, flags.PushOptions, flags.NoVerify)
		if cmd.HasParent() {
			cmd.SetOut(nil)
			cmd.SetErr(nil)
		} else {
			cmd.SetOut(origOut)
			cmd.SetErr(origErr)
		}
		if err != nil {
			if autoSubmit.Unsupported != nil && err == autoSubmit.Unsupported {
				guard.Keep()
				return err
			}
			// The change exists; only the schedule failed. Prefixing this
			// with "error creating change" would send the user to create it
			// again.
			var flowErr *ScheduleFlowError
			if errors.As(err, &flowErr) {
				guard.Keep()
				flowErr.Done = "change created"
				return flowErr
			}
			return fmt.Errorf("error creating change: %w", err)
		}

		guard.Keep()
		fmt.Fprintln(cmd.OutOrStdout())
		var details []ReceiptField
		if branch != "" {
			details = append(details, ReceiptField{Key: "Branch", Value: branch})
		}
		if state != nil && state.ChangeID != "" {
			details = append(details, ReceiptField{Key: "Change", Value: state.ChangeID})
		}
		PrintReceipt(cmd.OutOrStdout(), Receipt{
			Summary: "Change created successfully.",
			Details: details,
			URL:     ExtractGerritURLFromGitOutput(pushBuf.String()),
			Next:    "gh pr checks --watch [--fail-fast]",
		})
		// The change exists, so this is reported last: creating it is not the
		// part that failed, the promise that something would submit it is.
		return autoSubmit.Unsupported
	},
}

func init() {
	PrCmd.AddCommand(createCmd)

	AddCommonPushFlags(createCmd)
	AddCommitMessageFlags(createCmd)
	createCmd.Flags().Bool("force", false, "Force create even if a change with this Change-Id already exists")
}

func prepareCommitForCreate(
	ctx context.Context,
	cmd *cobra.Command,
	cfg *Config,
	flags CommonPushFlags,
) (*headRollbackGuard, string, error) {
	cmFlags := ParseCommitMessageFlags(cmd)
	hasCommitMsgFlags := cmFlags.HasAny(cmd)
	var resolved *ResolvedCommitMessageFlags
	if hasCommitMsgFlags {
		var err error
		resolved, err = ResolveAndValidateCommitMessageFlags(cmd, &cmFlags, "gh pr create")
		if err != nil {
			return nil, "", err
		}
	}

	branch := resolvePushBranch(ctx, cfg, flags.Base, cmd.ErrOrStderr())
	guard := newHeadRollbackGuard(ctx, cfg, "gh pr create: rollback failed create")
	if !hasCommitMsgFlags {
		return guard, branch, nil
	}

	aheadCount, hasRemoteRef, err := validateCommitMsgSingleCommit(ctx, cfg, branch, "create")
	if err != nil {
		return nil, "", err
	}

	trailerFmt, err := ResolveBugTrailerFormat(ctx, cfg)
	if err != nil {
		return nil, "", err
	}

	if guard.origHEAD == "" || (hasRemoteRef && aheadCount == 0) {
		initialMsg, err := BuildInitialCommitMessage(trailerFmt, resolved)
		if err != nil {
			return nil, "", err
		}
		if err := guard.CommitNewAll(ctx, initialMsg, flags.NoVerify, cmd.OutOrStdout(), cmd.ErrOrStderr()); err != nil {
			return nil, "", err
		}
		return guard, branch, nil
	}

	if err := guard.AmendWithFlags(ctx, cmd, trailerFmt, resolved); err != nil {
		return nil, "", err
	}
	return guard, branch, nil
}
