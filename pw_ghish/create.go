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
	"strings"

	"github.com/spf13/cobra"
)

var createCmd = &cobra.Command{
	Use:   "create",
	Short: "Create a new change by pushing current HEAD to Gerrit",
	Long: `Create a new Gerrit change by pushing the current HEAD commit to refs/for/<branch>.

If a change with the same Change-Id already exists on Gerrit, this command will error
and suggest using 'gh pr push' to upload a new patchset instead. Use --force to bypass this check.

Supports rich push options:
  - Reviewers and CCs: --reviewer, --cc
  - Auto-submit: --auto (alias --auto-submit)
  - Commit queue: --cq (default dry run, or specify vote: 1 = dry run, 2 = submit)
  - Publish draft comments: --publish
  - Raw push options: -o / --push-option
  - Work in progress: --draft
  - Skip pre-push hooks: --no-verify`,
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		cfg := GetConfig(cmd)

		flags := ParseCommonPushFlags(cmd)
		title, _ := cmd.Flags().GetString("title")
		body, _ := cmd.Flags().GetString("body")
		force, _ := cmd.Flags().GetBool("force")

		commitMsg := title
		if body != "" {
			if commitMsg != "" {
				commitMsg = commitMsg + "\n\n" + body
			} else {
				commitMsg = body
			}
		}

		// Check before committing, so that a refusal leaves the working tree
		// exactly as it was found.
		if err := CheckGitHubIssueSyntax(commitMsg, "--title/--body"); err != nil {
			return err
		}

		// If title/body was provided, create a commit first
		if commitMsg != "" {
			fmt.Fprintln(cmd.OutOrStdout(), "Creating new commit with title and body...")
			if err := cfg.GitClient().Run(ctx, cmd.OutOrStdout(), cmd.ErrOrStderr(), "commit", "-a", "-m", commitMsg); err != nil {
				return fmt.Errorf("error creating commit: %w", err)
			}
		}

		state, err := VerifyHeadForPush(ctx, cmd, cfg, !force)
		if err != nil {
			return err
		}
		if state.ExistingChange != nil {
			gerritURL, _ := cfg.GerritURL(ctx)
			baseURL := strings.TrimSuffix(gerritURL, "/a")
			changeURL := fmt.Sprintf("%s/c/%s/+/%d", baseURL, state.ExistingChange.Project, state.ExistingChange.Number)
			return fmt.Errorf("a pull request for Change-Id %s already exists:\n  %s\n\nTo upload a new patchset, run:\n  gh pr push", state.ChangeID, changeURL)
		}

		branch := resolvePushBranch(ctx, cfg, flags.Base, cmd.ErrOrStderr())

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

		fmt.Fprintf(cmd.OutOrStdout(), "Creating change for branch %s...\n", branch)

		if err := executePush(ctx, cmd, cfg, branch, flags.PushOptions, flags.NoVerify); err != nil {
			if autoSubmit.Unsupported != nil && err == autoSubmit.Unsupported {
				return err
			}
			return fmt.Errorf("error creating change: %w", err)
		}

		fmt.Fprintln(cmd.OutOrStdout(), "\nChange created successfully.")
		// The change exists, so this is reported last: creating it is not the
		// part that failed, the promise that something would submit it is.
		return autoSubmit.Unsupported
	},
}

func init() {
	PrCmd.AddCommand(createCmd)

	AddCommonPushFlags(createCmd)
	createCmd.Flags().Bool("force", false, "Force create even if a change with this Change-Id already exists")
	createCmd.Flags().StringP("title", "t", "", "Title for the change")
	createCmd.Flags().StringP("body", "b", "", "Body for the change")
}
