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
	cmd.Flags().Int("trigger", 0, "Trigger presubmit / Commit-Queue vote (1 = dry run, 2 = submit; alias --cq)")
	cmd.Flags().Lookup("trigger").NoOptDefVal = "1"
	cmd.Flags().Int("cq", 0, "Commit-Queue vote (1 = dry run, 2 = submit; alias for --trigger)")
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
	if cmd.Flags().Changed("trigger") {
		cq, _ = cmd.Flags().GetInt("trigger")
	}
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
	if cfg != nil {
		if projCfg, err := cfg.LoadProjectConfig(ctx); err == nil && projCfg != nil && projCfg.Gerrit.DefaultBranch != "" {
			return projCfg.Gerrit.DefaultBranch
		}
	}
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

// ValidateCommitStack checks if pushing HEAD would push multiple commits ahead of the target branch,
// and validates commit messages and Change-Ids across all commits in the stack when available.
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

	commits, err := git.StackCommits(ctx, branch)
	if err != nil {
		if ctx.Err() != nil {
			return ctx.Err()
		}
		return nil
	}
	seenChangeIDs := make(map[string]StackCommit, len(commits))
	for _, c := range commits {
		lowerSubj := strings.ToLower(strings.TrimSpace(c.Subject))
		if strings.HasPrefix(lowerSubj, "fixup!") || strings.HasPrefix(lowerSubj, "squash!") || strings.HasPrefix(lowerSubj, "amend!") {
			return fmt.Errorf("commit %s (%q) is an unsquashed fixup/squash commit.\n\nRun 'git rebase -i --autosquash origin/%s' before pushing", c.Hash, c.Subject, branch)
		}
		commitLabel := fmt.Sprintf("commit %s (%q)", c.Hash, c.Subject)
		if err := CheckGitHubIssueSyntax(c.Body, commitLabel); err != nil {
			return err
		}
		if err := CheckMultipleChangeIDs(c.Body, commitLabel); err != nil {
			return err
		}
		if c.ChangeID == "" {
			return fmt.Errorf("commit %s (%q) in stack is missing a Gerrit Change-Id footer", c.Hash, c.Subject)
		}
		if prev, dup := seenChangeIDs[c.ChangeID]; dup {
			return fmt.Errorf("commits %s (%q) and %s (%q) in the stack share the same Change-Id %s.\n\nEach commit in a Gerrit stack must have a unique Change-Id", prev.Hash, prev.Subject, c.Hash, c.Subject, c.ChangeID)
		}
		seenChangeIDs[c.ChangeID] = c
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

	cqLabelName := pushOpts.CQLabelName
	cqScore := pushOpts.CQ
	if cqScore > 0 && cqLabelName == "" {
		cqLabelName, cqScore = chCtx.ResolveCQVote(cqScore)
	}
	if cqLabelName == "" {
		cqLabelName = "Commit-Queue"
	}

	if cqScore > 0 || pushOpts.Publish || pushOpts.AutoSubmit {
		input := &gerrit.ReviewInput{}
		labels := map[string]int{}
		if cqScore > 0 {
			labels[cqLabelName] = cqScore
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
			if cqScore > 0 && !pushOpts.Publish {
				action = fmt.Sprintf("setting %s on", cqLabelName)
			} else if pushOpts.Publish && cqScore == 0 && !pushOpts.AutoSubmit {
				action = "publishing drafts on"
			} else if pushOpts.AutoSubmit && cqScore == 0 && !pushOpts.Publish {
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
	if cqScore > 0 {
		fmt.Fprintf(cmd.OutOrStdout(), "%s+%d set successfully.\n", cqLabelName, cqScore)
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
	GerritQueried  bool
}

// queryExistingChangeByID queries Gerrit for the first change matching changeID.
// Returns (change, true) if Gerrit responded without error (change is nil if 0 matches),
// or (nil, false) if Gerrit could not be queried.
func queryExistingChangeByID(ctx context.Context, client *gerrit.Client, changeID string, additionalFields ...string) (*gerrit.ChangeInfo, bool) {
	if client == nil || changeID == "" {
		return nil, false
	}
	opt := &gerrit.QueryChangeOptions{}
	opt.Query = []string{changeID}
	opt.AdditionalFields = additionalFields
	changes, _, err := client.Changes.QueryChanges(ctx, opt)
	if err != nil || changes == nil {
		return nil, false
	}
	if len(*changes) > 0 {
		return &(*changes)[0], true
	}
	return nil, true
}

// DetectClobberedChangeID inspects HEAD@{1} in the local Git reflog when
// HEAD's Change-Id is not found on Gerrit. If HEAD@{1} carried a different
// Change-Id that does exist on Gerrit, it returns that Change-Id and ChangeInfo.
func DetectClobberedChangeID(ctx context.Context, cmd *cobra.Command, cfg *Config, currentChangeID string) (string, *gerrit.ChangeInfo) {
	if cfg == nil {
		return "", nil
	}
	prevMsg, err := cfg.GitClient().CommitMessage(ctx, "HEAD@{1}")
	if err != nil {
		return "", nil
	}
	prevChangeID := ExtractChangeID(prevMsg)
	if prevChangeID == "" || prevChangeID == currentChangeID {
		return "", nil
	}
	client, err := NewGerritClient(ctx, cmd)
	if err != nil {
		return "", nil
	}
	ch, ok := queryExistingChangeByID(ctx, client, prevChangeID)
	if !ok || ch == nil {
		return "", nil
	}
	return prevChangeID, ch
}

// FormatMissingChangePushError builds the error returned by 'gh pr push' when
// HEAD's Change-Id does not exist on Gerrit, checking HEAD@{1} in the local
// Git reflog to report the exact Change-Id and CL number if it was just clobbered.
func FormatMissingChangePushError(ctx context.Context, cmd *cobra.Command, cfg *Config, state *VerifiedPushState, stack bool) error {
	subject := strings.TrimSpace(strings.SplitN(state.CommitMsg, "\n", 2)[0])
	createCmdStr := "gh pr create"
	if stack {
		createCmdStr = "gh pr create --stack"
	}

	var clobberHint string
	if prevID, prevChange := DetectClobberedChangeID(ctx, cmd, cfg, state.ChangeID); prevChange != nil {
		clobberHint = fmt.Sprintf(
			"\nClobbered Change-Id detected in HEAD@{1}:\n"+
				"  HEAD@{1} had Change-Id: %s (CL #%d: %q)\n"+
				"  Restore 'Change-Id: %s' in HEAD before running 'gh pr push'.\n",
			prevID, prevChange.Number, prevChange.Subject, prevID,
		)
	}

	return fmt.Errorf(
		"no existing Gerrit change found for HEAD's Change-Id %s (%q).\n%s\n"+
			"'gh pr push' updates an existing CL, but no change with this Change-Id exists on Gerrit.\n"+
			"  • If you recently ran 'git commit --amend -m' or rebased, the original Change-Id was replaced.\n"+
			"    Restore the original Change-Id trailer in HEAD before pushing:\n"+
			"      git log -1 --format=%%B HEAD > \"$(git rev-parse --git-dir)/COMMIT_EDITMSG_TMP\"\n"+
			"      # Edit COMMIT_EDITMSG_TMP to restore the original Change-Id: footer\n"+
			"      git commit --amend --only -F \"$(git rev-parse --git-dir)/COMMIT_EDITMSG_TMP\"\n"+
			"  • To create a brand-new CL instead, run:\n"+
			"      %s\n"+
			"  • To bypass this check, pass --force",
		state.ChangeID, subject, clobberHint, createCmdStr,
	)
}

// CheckCommitMessageWarnings inspects a commit message for subject (>72 chars)
// and prose body (>72 chars) line-length violations and returns warning strings.
func CheckCommitMessageWarnings(msg string, label string) []string {
	normalized := strings.ReplaceAll(strings.TrimRight(msg, "\r\n"), "\r\n", "\n")
	if normalized == "" {
		return nil
	}
	lines := strings.Split(normalized, "\n")
	var warnings []string

	subject := strings.TrimSpace(lines[0])
	if len(subject) > 72 {
		warnings = append(warnings, fmt.Sprintf(
			"Warning: %s subject is %d characters (exceeds 72-character limit); amend the commit message (preserving Change-Id:) to shorten it.",
			label, len(subject),
		))
	}

	hasLongBodyLine := false
	for _, line := range lines[1:] {
		trimmed := strings.TrimSpace(line)
		if trimmed == "" {
			continue
		}
		// Skip indented code blocks, trailers, and lines containing URLs.
		if strings.HasPrefix(line, "  ") || strings.HasPrefix(line, "\t") {
			continue
		}
		if trailerRegex.MatchString(trimmed) || cherryPickFooterRegex.MatchString(trimmed) {
			continue
		}
		if strings.Contains(line, "://") {
			continue
		}
		if len(line) > 72 {
			hasLongBodyLine = true
			break
		}
	}
	if hasLongBodyLine {
		warnings = append(warnings, fmt.Sprintf(
			"Warning: %s has body line(s) longer than 72 characters; amend the commit message (preserving Change-Id:) to wrap them.",
			label,
		))
	}
	return warnings
}

// VerifyHeadForPush ensures the HEAD commit has a valid Change-Id, checks that
// the commit message contains no GitHub issue syntax or duplicate Change-Ids,
// and optionally queries Gerrit for an existing change matching the Change-Id.
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
	if err := CheckMultipleChangeIDs(logMsg, "the HEAD commit message"); err != nil {
		return nil, err
	}

	for _, w := range CheckCommitMessageWarnings(logMsg, "HEAD commit") {
		fmt.Fprintln(cmd.ErrOrStderr(), w)
	}

	changeID := ExtractChangeID(logMsg)
	var existing *gerrit.ChangeInfo
	var queried bool
	if queryExisting && changeID != "" {
		if client, err := NewGerritClient(ctx, cmd); err == nil {
			// DETAILED_LABELS and SUBMIT_REQUIREMENTS so callers can see which
			// labels and submit requirements this host actually defines instead
			// of guessing names.
			existing, queried = queryExistingChangeByID(ctx, client, changeID, "DETAILED_LABELS", "SUBMIT_REQUIREMENTS")
		}
	}

	return &VerifiedPushState{
		CommitMsg:      logMsg,
		ChangeID:       changeID,
		ExistingChange: existing,
		GerritQueried:  queried,
	}, nil
}

// RequestedTopic returns the topic specified via --topic or -o topic=<name>
// on PushOptions, or an empty string if none was requested.
func RequestedTopic(opts PushOptions) string {
	if strings.TrimSpace(opts.Topic) != "" {
		return strings.TrimSpace(opts.Topic)
	}
	for _, extra := range opts.ExtraOptions {
		if strings.HasPrefix(extra, "topic=") {
			return strings.TrimSpace(strings.TrimPrefix(extra, "topic="))
		}
	}
	return ""
}

// CheckTopicAllowed verifies that setting a Gerrit topic is permitted by the
// repository's configuration (.ghish.toml / git config) and by the change's
// SubmitRequirements (such as Topics-Not-Supported).
func CheckTopicAllowed(ctx context.Context, cfg *Config, existing *gerrit.ChangeInfo, topic string, commandHint string) error {
	var projCfg *ProjectConfig
	if cfg != nil {
		var err error
		projCfg, err = cfg.LoadProjectConfig(ctx)
		if err != nil {
			return err
		}
	}
	topic = strings.TrimSpace(topic)
	if topic == "" {
		return nil
	}
	if projCfg != nil && projCfg.Gerrit.ForbidTopics {
		return formatTopicsNotSupportedError(topic, "project configuration (forbid_topics = true)", commandHint)
	}
	if existing != nil {
		for _, sr := range existing.SubmitRequirements {
			if strings.EqualFold(sr.Name, "Topics-Not-Supported") && sr.Status != "NOT_APPLICABLE" {
				return formatTopicsNotSupportedError(topic, "Gerrit submit requirement 'Topics-Not-Supported'", commandHint)
			}
		}
	}
	return nil
}

func formatTopicsNotSupportedError(topic, source, commandHint string) error {
	return fmt.Errorf(
		"cannot set Gerrit topic %q: %s forbids topics on this repository (Topics-Not-Supported).\n\n"+
			"Why: Setting a topic on a change in this repository causes the 'Topics-Not-Supported'\n"+
			"submit requirement to become UNSATISFIED, blocking submission.\n\n"+
			"To proceed:\n"+
			"  • Re-run the command without --topic (or -o topic=...):\n"+
			"      %s\n"+
			"  • If an existing change already has a topic blocking submission, remove it with:\n"+
			"      gh pr edit --remove-topic",
		topic, source, commandHint,
	)
}

// VerifyStackChanges inspects all commits in origin/<branch>..HEAD when --stack
// is enabled, emits formatting warnings across the stack, queries Gerrit for each
// commit's Change-Id, prints the stack push plan, and enforces Change-Id continuity:
//   - For 'push', every commit in the stack must match an existing Gerrit change.
//   - For 'create', no [NEW CL] commit may sit below an [UPDATE] commit in the stack.
func VerifyStackChanges(ctx context.Context, cmd *cobra.Command, cfg *Config, branch string, headState *VerifiedPushState, commandName string) error {
	commits, err := cfg.GitClient().StackCommits(ctx, branch)
	if err != nil || len(commits) == 0 {
		return nil
	}

	for i, c := range commits {
		if i == len(commits)-1 {
			continue // HEAD warnings were already emitted by VerifyHeadForPush.
		}
		for _, w := range CheckCommitMessageWarnings(c.Body, fmt.Sprintf("commit %s (%q)", c.Hash, c.Subject)) {
			fmt.Fprintln(cmd.ErrOrStderr(), w)
		}
	}

	if headState == nil || !headState.GerritQueried {
		return nil
	}

	client, err := NewGerritClient(ctx, cmd)
	if err != nil {
		return nil
	}

	existingByIdx := make([]*gerrit.ChangeInfo, len(commits))
	for i, c := range commits {
		if c.ChangeID == headState.ChangeID {
			existingByIdx[i] = headState.ExistingChange
			continue
		}
		ch, ok := queryExistingChangeByID(ctx, client, c.ChangeID)
		if !ok {
			return nil
		}
		existingByIdx[i] = ch
	}

	if len(commits) > 1 {
		fmt.Fprintf(cmd.OutOrStdout(), "Stack plan (%d commits -> %s):\n", len(commits), branch)
		for i, c := range commits {
			status := "[NEW CL]"
			if existingByIdx[i] != nil {
				status = fmt.Sprintf("[UPDATE #%d]", existingByIdx[i].Number)
			}
			fmt.Fprintf(cmd.OutOrStdout(), "  %d. %s %-16s %s\n", i+1, c.Hash, status, c.Subject)
		}
	}

	if commandName == "push" {
		for i, c := range commits {
			if existingByIdx[i] == nil {
				if i == len(commits)-1 && c.ChangeID == headState.ChangeID {
					return FormatMissingChangePushError(ctx, cmd, cfg, headState, true)
				}
				return fmt.Errorf(
					"cannot push stack with 'gh pr push --stack': commit %s (%q) with Change-Id %s does not match any existing change on Gerrit.\n\n"+
						"  • If a commit's Change-Id was overwritten during 'git commit --amend -m' or 'git rebase',\n"+
						"    restore its original Change-Id trailer before pushing.\n"+
						"  • If you added a new commit to the stack and want to create a new CL for it, run:\n"+
						"      gh pr create --stack\n"+
						"  • To bypass this check, pass --force",
					c.Hash, c.Subject, c.ChangeID,
				)
			}
		}
	}

	if commandName == "create" {
		firstNewIdx := -1
		for i, ex := range existingByIdx {
			if ex == nil && firstNewIdx < 0 {
				firstNewIdx = i
			} else if ex != nil && firstNewIdx >= 0 {
				newC := commits[firstNewIdx]
				laterC := commits[i]
				return fmt.Errorf(
					"stack order error: commit %s (%q, Change-Id %s) has no matching change on Gerrit, "+
						"but sits below existing change #%d (%s %q) in the stack.\n\n"+
						"This usually happens when an earlier commit in the stack lost its Change-Id during a rebase or amend.\n"+
						"  • Restore the original Change-Id on commit %s, or\n"+
						"  • Pass --force if you intentionally inserted a new commit in the middle of an existing stack",
					newC.Hash, newC.Subject, newC.ChangeID,
					ex.Number, laterC.Hash, laterC.Subject,
					newC.Hash,
				)
			}
		}
	}

	return nil
}
