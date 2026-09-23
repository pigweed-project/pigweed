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
	"fmt"
	"io"
	"strings"

	"github.com/andygrunwald/go-gerrit"
	"github.com/spf13/cobra"
)

// CommonPushFlags holds parsed CLI flags shared between 'gh pr create' and 'gh pr push'.
type CommonPushFlags struct {
	Base        string
	Stack       bool
	NoVerify    bool
	PushOptions PushOptions
}

// AddCommonPushFlags registers common push flags shared across 'gh pr create' and 'gh pr push'.
func AddCommonPushFlags(cmd *cobra.Command) {
	cmd.Flags().StringSliceP("reviewer", "r", []string{}, "Request a review from someone")
	cmd.Flags().StringSliceP("cc", "c", []string{}, "CC someone on the change")
	cmd.Flags().BoolP("draft", "d", false, "Mark as work in progress (WIP)")
	cmd.Flags().Bool("auto", false, "Automatically submit change when checks and reviews pass")
	cmd.Flags().Bool("auto-submit", false, "Alias for --auto")
	_ = cmd.Flags().MarkHidden("auto-submit")
	cmd.Flags().Int("cq", 0, "Commit-Queue vote (1 = dry run, 2 = submit)")
	cmd.Flags().Lookup("cq").NoOptDefVal = "1"
	cmd.Flags().Bool("publish", false, "Publish draft comments on push")
	cmd.Flags().StringSliceP("push-option", "o", []string{}, "Raw Gerrit push options (passed via %...)")
	cmd.Flags().Bool("no-verify", false, "Bypass pre-push git hooks")
	cmd.Flags().StringP("base", "B", "", "The branch into which you want your code merged")
	cmd.Flags().Bool("stack", false, "Allow pushing multiple commits as a stack of Gerrit changes")
	cmd.Flags().String("topic", "", "Set Gerrit topic for the change")
	cmd.Flags().StringSlice("hashtag", []string{}, "Add hashtags to the change")
}

// ParseCommonPushFlags parses the common push flags from the Cobra command.
func ParseCommonPushFlags(cmd *cobra.Command) CommonPushFlags {
	reviewers, _ := cmd.Flags().GetStringSlice("reviewer")
	cc, _ := cmd.Flags().GetStringSlice("cc")
	draft, _ := cmd.Flags().GetBool("draft")
	ready, _ := cmd.Flags().GetBool("ready")
	auto, _ := cmd.Flags().GetBool("auto")
	autoSubmit, _ := cmd.Flags().GetBool("auto-submit")
	cq, _ := cmd.Flags().GetInt("cq")
	publish, _ := cmd.Flags().GetBool("publish")
	pushOptions, _ := cmd.Flags().GetStringSlice("push-option")
	noVerify, _ := cmd.Flags().GetBool("no-verify")
	base, _ := cmd.Flags().GetString("base")
	stack, _ := cmd.Flags().GetBool("stack")
	topic, _ := cmd.Flags().GetString("topic")
	hashtags, _ := cmd.Flags().GetStringSlice("hashtag")

	return CommonPushFlags{
		Base:     base,
		Stack:    stack,
		NoVerify: noVerify,
		PushOptions: PushOptions{
			Reviewers:    reviewers,
			CC:           cc,
			Draft:        draft,
			Ready:        ready,
			AutoSubmit:   auto || autoSubmit,
			CQ:           cq,
			Publish:      publish,
			Topic:        topic,
			Hashtags:     hashtags,
			ExtraOptions: pushOptions,
		},
	}
}

// defaultBranch resolves the default target branch (defaults to "main").
func defaultBranch(ctx context.Context, cfg *Config, stderr io.Writer) string {
	git := cfg.GitClient()
	for _, ref := range []string{"refs/heads/main", "refs/remotes/origin/main", "origin/main"} {
		if ok, err := git.VerifyRef(ctx, ref); err == nil && ok {
			return "main"
		}
	}
	return "main"
}

// resolvePushBranch determines the target branch for a push.
// Priority:
// 1. Explicit --base flag
// 2. Tracking branch upstream (@{upstream})
// 3. Remote branch on origin matching current branch name
// 4. Default branch (main)
func resolvePushBranch(ctx context.Context, cfg *Config, baseFlag string, stderr io.Writer) string {
	if baseFlag != "" {
		return baseFlag
	}
	git := cfg.GitClient()
	// 1. Try tracking upstream (@{upstream})
	if upstream, err := git.RevParse(ctx, "--abbrev-ref", "@{upstream}"); err == nil {
		if idx := strings.IndexByte(upstream, '/'); idx != -1 {
			branch := upstream[idx+1:]
			if branch != "" {
				return branch
			}
		}
	}
	// 2. Check if current local branch matches an existing remote branch on origin
	if curr, err := git.CurrentBranch(ctx); err == nil && curr != "" {
		if ok, err := git.VerifyRef(ctx, "origin/"+curr); err == nil && ok {
			return curr
		}
	}
	// 3. Fall back to default repo branch (main)
	return defaultBranch(ctx, cfg, stderr)
}

// ValidateCommitStack checks if pushing HEAD would push multiple commits ahead of the target branch.
// Returns an actionable error if count > 1 and stack is false.
func ValidateCommitStack(ctx context.Context, git GitClient, branch string, stack bool, commandName string) error {
	count, countErr := git.CountCommitsAhead(ctx, branch)
	if countErr != nil {
		if ctx.Err() != nil {
			return ctx.Err()
		}
		return nil
	}
	if count == 0 && commandName == "create" {
		return fmt.Errorf("cannot create a new change: HEAD has 0 commits ahead of target branch %q.\n\n"+
			"In Gerrit, each change corresponds to a local commit.\n"+
			"To create a change, first stage and commit your modifications:\n"+
			"  git add <files>\n"+
			"  git commit -m \"<module>: <summary>\"\n"+
			"  gh pr create", branch)
	}
	if count > 1 && !stack {
		if commandName == "create" {
			return fmt.Errorf("pushing HEAD would create %d separate Gerrit changes targeting branch %q.\n\nTo target a different base branch, specify:\n  gh pr create --base <branch>\n\nTo create a stack of %d changes, pass --stack", count, branch, count)
		}
		return fmt.Errorf("pushing HEAD would push %d commits targeting branch %q.\n\nTo target a different base branch, specify:\n  gh pr push --base <branch>\n\nTo push a stack of %d changes, pass --stack", count, branch, count)
	}
	return nil
}

// executePush runs git push to Gerrit with the specified push options.
func executePush(ctx context.Context, cmd *cobra.Command, cfg *Config, branch string, pushOpts PushOptions, noVerify bool) error {
	if cmd == nil {
		return fmt.Errorf("internal error: cmd is uninitialized in executePush")
	}
	if cfg == nil {
		return fmt.Errorf("internal error: cfg is uninitialized in executePush")
	}
	profile := cfg.GetProfile(ctx)
	refStr := profile.FormatPushRef(branch, pushOpts)

	pushArgs := []string{"push"}
	if noVerify {
		pushArgs = append(pushArgs, "--no-verify")
	}
	pushArgs = append(pushArgs, "origin", "HEAD:"+refStr)

	var errBuf bytes.Buffer
	if err := cfg.GitClient().Run(ctx, cmd.OutOrStdout(), &errBuf, pushArgs...); err != nil {
		errStr := errBuf.String()
		if strings.Contains(errStr, "missing Change-Id") {
			host := cfg.Host
			if host == "" {
				host = "<gerrit-host>"
			}
			hookURL := fmt.Sprintf("https://%s/tools/hooks/commit-msg", host)
			return fmt.Errorf("remote rejected push because a commit is missing a Change-Id in its footer.\n\nGerrit requires a Change-Id line in each commit message.\nTo resolve this:\n  1. Ensure the commit-msg hook is installed:\n     curl -Lo .git/hooks/commit-msg %s && chmod +x .git/hooks/commit-msg\n  2. Amend your commit to generate the Change-Id:\n     git commit --amend --no-edit\n  3. Retry push:\n     gh pr push\n\nUnderlying error: %w", hookURL, err)
		}
		if strings.Contains(errStr, "no new changes") {
			if hasMetadataUpdates(pushOpts) {
				if restErr := applyPushOptionsViaREST(ctx, cmd, cfg, pushOpts); restErr != nil {
					return restErr
				}
				return nil
			}
			return fmt.Errorf("no new changes to push (HEAD is already up-to-date with Gerrit).\n\n" +
				"To push an update, make code changes and commit or amend first:\n" +
				"  git commit --amend\n" +
				"  gh pr push\n\n" +
				"To update change metadata (CQ, topic, reviewers) without a new commit:\n" +
				"  gh pr edit --cq\n" +
				"  gh pr edit --topic <topic>\n" +
				"  gh pr edit --add-reviewer <user>")
		}
		cmd.ErrOrStderr().Write(errBuf.Bytes())
		return err
	}
	cmd.ErrOrStderr().Write(errBuf.Bytes())
	return nil
}

func hasMetadataUpdates(opts PushOptions) bool {
	return opts.CQ > 0 || opts.AutoSubmit || opts.Topic != "" || len(opts.Hashtags) > 0 ||
		opts.Draft || opts.Wip || opts.Ready || opts.Publish ||
		len(opts.Reviewers) > 0 || len(opts.CC) > 0
}

func applyPushOptionsViaREST(ctx context.Context, cmd *cobra.Command, cfg *Config, pushOpts PushOptions) (retErr error) {
	if cmd == nil {
		return fmt.Errorf("internal error: cmd is uninitialized in applyPushOptionsViaREST")
	}
	if cfg == nil {
		return fmt.Errorf("internal error: cfg is uninitialized in applyPushOptionsViaREST")
	}
	var applied bool
	defer func() {
		if retErr != nil && !applied {
			retErr = fmt.Errorf("no new commits to push, and failed to apply metadata updates via Gerrit API: %w", retErr)
		}
	}()

	chCtx, err := ResolveChangeContext(cmd, nil)
	if err != nil {
		return err
	}
	changeID := chCtx.ChangeID

	var autoSubmit AutoSubmitDecision
	if pushOpts.AutoSubmit {
		if pushOpts.AutoSubmitLabel.Name != "" {
			// The push already resolved the label; do not pay for a second
			// round trip to learn the same thing.
			autoSubmit = AutoSubmitDecision{
				Vote:        pushOpts.AutoSubmitLabel,
				Unsupported: pushOpts.AutoSubmitUnsupported,
			}
		} else {
			// The push never got far enough to look the label up (e.g. --base
			// was given, so no change lookup happened). Ask now rather than
			// guess.
			autoSubmit, err = chCtx.DecideAutoSubmit()
			if err != nil {
				return err
			}
		}
	}
	autoSubmitLabel := autoSubmit.Vote

	if pushOpts.CQ > 0 || pushOpts.Publish || pushOpts.AutoSubmit {
		input := &gerrit.ReviewInput{}
		labels := map[string]int{}
		if pushOpts.CQ > 0 {
			labels["Commit-Queue"] = pushOpts.CQ
		}
		if pushOpts.AutoSubmit && autoSubmitLabel.Name != "" {
			// --cq is an explicit request for a specific score; the score
			// --auto settled on must not quietly replace it.
			if _, taken := labels[autoSubmitLabel.Name]; !taken {
				labels[autoSubmitLabel.Name] = autoSubmitLabel.Value
			}
		}
		if len(labels) > 0 {
			input.Labels = labels
		}
		if pushOpts.Publish {
			input.Drafts = "PUBLISH_ALL_REVISIONS"
		}
		if err := chCtx.SetReviewRevision("current", input); err != nil {
			action := "updating review on"
			if pushOpts.CQ > 0 && !pushOpts.Publish {
				action = "setting Commit-Queue on"
			} else if pushOpts.Publish && pushOpts.CQ == 0 && !pushOpts.AutoSubmit {
				action = "publishing drafts on"
			} else if pushOpts.AutoSubmit && pushOpts.CQ == 0 && !pushOpts.Publish {
				action = "setting auto-submit on"
			}
			return chCtx.FormatError(err, action)
		}
	}

	if pushOpts.Topic != "" {
		if _, _, err := chCtx.Client.Changes.SetTopic(chCtx.Context, chCtx.ChangeID, &gerrit.TopicInput{Topic: pushOpts.Topic}); err != nil {
			return chCtx.FormatError(err, "setting topic on")
		}
	}

	if len(pushOpts.Hashtags) > 0 {
		if _, _, err := chCtx.Client.Changes.SetHashtags(chCtx.Context, chCtx.ChangeID, &gerrit.HashtagsInput{Add: pushOpts.Hashtags}); err != nil {
			return chCtx.FormatError(err, "setting hashtags on")
		}
	}

	if pushOpts.Draft || pushOpts.Wip {
		if err := chCtx.SetWorkInProgress(""); err != nil {
			return err
		}
	} else if pushOpts.Ready {
		if _, err := chCtx.Client.Changes.SetReadyForReview(chCtx.Context, chCtx.ChangeID, &gerrit.ReadyForReviewInput{}); err != nil {
			return chCtx.FormatError(err, "marking ready for review")
		}
	}

	for _, r := range pushOpts.Reviewers {
		if _, _, err := chCtx.Client.Changes.AddReviewer(chCtx.Context, chCtx.ChangeID, &gerrit.ReviewerInput{Reviewer: r}); err != nil {
			return chCtx.FormatError(err, fmt.Sprintf("adding reviewer %s to", r))
		}
	}

	for _, c := range pushOpts.CC {
		if err := chCtx.AddCC(c); err != nil {
			return err
		}
	}

	applied = true
	fmt.Fprintf(cmd.OutOrStdout(), "No new commits to push; applied metadata updates to Change %s via Gerrit API.\n", changeID)
	if pushOpts.CQ > 0 {
		fmt.Fprintf(cmd.OutOrStdout(), "Commit-Queue+%d set successfully.\n", pushOpts.CQ)
	}
	if pushOpts.AutoSubmit && autoSubmitLabel.Name != "" {
		if autoSubmit.Unsupported != nil {
			fmt.Fprintf(cmd.OutOrStdout(), "%s%+d set.\n", autoSubmitLabel.Name, autoSubmitLabel.Value)
		} else {
			fmt.Fprintf(cmd.OutOrStdout(), "Auto-submit enabled (%s%+d).\n", autoSubmitLabel.Name, autoSubmitLabel.Value)
		}
	}
	if pushOpts.Publish {
		fmt.Fprintln(cmd.OutOrStdout(), "Draft comments published successfully.")
	}
	return autoSubmit.Unsupported
}

// VerifiedPushState holds verified pre-flight commit information for create and push.
type VerifiedPushState struct {
	CommitMsg      string
	ChangeID       string
	ExistingChange *gerrit.ChangeInfo
}

// VerifyHeadForPush ensures the HEAD commit has a valid Change-Id, checks that
// the commit message contains no GitHub issue syntax, and optionally queries
// Gerrit for an existing change matching the Change-Id.
func VerifyHeadForPush(ctx context.Context, cmd *cobra.Command, cfg *Config, queryExisting bool) (*VerifiedPushState, error) {
	if err := EnsureChangeID(ctx, cfg, cmd.OutOrStdout(), cmd.ErrOrStderr()); err != nil {
		return nil, fmt.Errorf("error verifying Change-Id: %w", err)
	}

	logMsg, err := cfg.GitClient().HeadCommitMessage(ctx)
	if err != nil {
		return nil, fmt.Errorf("error reading HEAD commit message: %w", err)
	}
	if err := CheckGitHubIssueSyntax(logMsg, "the HEAD commit message"); err != nil {
		return nil, err
	}

	changeID := ExtractChangeID(logMsg)
	var existing *gerrit.ChangeInfo
	if queryExisting && changeID != "" {
		if client, err := NewGerritClient(ctx, cmd); err == nil {
			opt := &gerrit.QueryChangeOptions{}
			opt.Query = []string{changeID}
			// DETAILED_LABELS so callers can see which labels this host
			// actually defines (and their ranges) instead of guessing names.
			opt.AdditionalFields = []string{"DETAILED_LABELS"}
			if changes, _, err := client.Changes.QueryChanges(ctx, opt); err == nil && len(*changes) > 0 {
				existing = &(*changes)[0]
			}
		}
	}

	return &VerifiedPushState{
		CommitMsg:      logMsg,
		ChangeID:       changeID,
		ExistingChange: existing,
	}, nil
}
