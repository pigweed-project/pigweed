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
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

func isRepoLocalGitEnvKey(key string) bool {
	switch key {
	case "GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE",
		"GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES",
		"GIT_COMMON_DIR", "GIT_PREFIX",
		"GIT_CONFIG", "GIT_CONFIG_PARAMETERS", "GIT_CONFIG_COUNT":
		return true
	}
	return false
}

// scrubRepoLocalGitEnv returns a copy of env with repository-local Git
// environment overrides removed so git subprocesses executing in an explicit
// directory operate on that directory rather than an inherited outer repo
// (such as during `git rebase -x` or git hook execution).
func scrubRepoLocalGitEnv(env []string) []string {
	filtered := make([]string, 0, len(env))
	for _, kv := range env {
		k, _, _ := strings.Cut(kv, "=")
		if isRepoLocalGitEnvKey(k) {
			continue
		}
		filtered = append(filtered, kv)
	}
	return filtered
}

// StackCommit represents a single commit in a local stack ahead of a target branch.
type StackCommit struct {
	Hash     string
	Subject  string
	Body     string
	ChangeID string
}

// GitClient defines high-level Git queries and operations on top of GitRunner.
type GitClient interface {
	GitRunner
	CurrentBranch(ctx context.Context) (string, error)
	HeadCommitMessage(ctx context.Context) (string, error)
	CommitMessage(ctx context.Context, ref ...string) (string, error)
	ConfigGet(ctx context.Context, key string) (string, error)
	RevParse(ctx context.Context, args ...string) (string, error)
	VerifyRef(ctx context.Context, ref string) (bool, error)
	GitDir(ctx context.Context) (string, error)
	CountCommitsAhead(ctx context.Context, branch string) (int, error)
	StackCommits(ctx context.Context, branch string) ([]StackCommit, error)
	Fetch(ctx context.Context, remote, ref string, stdout, stderr io.Writer) error
	Checkout(ctx context.Context, ref string, stdout, stderr io.Writer) error
	CherryPick(ctx context.Context, ref string, stdout, stderr io.Writer) error
	// HasStagedChanges reports whether the Git index differs from HEAD.
	HasStagedChanges(ctx context.Context) (bool, error)
	// CommitNewAll commits only staged index changes if HasStagedChanges is
	// true (leaving unstaged edits untouched), or passes -a to stage modified
	// tracked files when nothing is staged yet, reporting whether -a was used.
	CommitNewAll(ctx context.Context, message string, noVerify bool, stdout, stderr io.Writer) (autoStaged bool, err error)
	// CommitAmendMessage rewrites HEAD's commit message via a temporary file
	// without altering HEAD's tree or staged index changes.
	CommitAmendMessage(ctx context.Context, message string, noVerify bool, stdout, stderr io.Writer) error
	// CommitAmendNoEdit runs commit-msg hooks on HEAD without altering the commit
	// tree or staged index entries.
	CommitAmendNoEdit(ctx context.Context, stdout, stderr io.Writer) error
	// UpdateHeadRef atomically moves HEAD to targetSHA only if HEAD currently
	// resolves to expectedCurrentSHA, leaving index and working tree untouched.
	UpdateHeadRef(ctx context.Context, targetSHA, expectedCurrentSHA, reason string) error
	IsHeadRemoteTip(ctx context.Context) (bool, []string, error)
	HeadAuthorEmail(ctx context.Context) (string, error)
	UserEmail(ctx context.Context) (string, error)
	// CheckAmendAllowed verifies that HEAD is safe to amend locally (not on a
	// remote tracking tip, merge commit, or active sequencer operation).
	CheckAmendAllowed(ctx context.Context) error
	ResolveRemote(ctx context.Context) string
	ResolveDefaultBranch(ctx context.Context) string
}

// defaultGitClient implements GitClient by executing commands via an underlying GitRunner.
type defaultGitClient struct {
	runner GitRunner
}

// NewGitClient wraps runner into a GitClient. If runner is nil, DefaultGitRunner is used.
func NewGitClient(runner GitRunner) GitClient {
	if runner == nil {
		runner = DefaultGitRunner
	}
	if gc, ok := runner.(GitClient); ok {
		return gc
	}
	return &defaultGitClient{runner: runner}
}

// NewGitClientInDir returns a GitClient configured to execute git commands in dir.
func NewGitClientInDir(dir string) GitClient {
	return NewGitClient(&RealGitRunner{Dir: dir})
}

// GitClient returns a GitClient for the configuration, falling back to DefaultGitRunner if unconfigured.
func (c *Config) GitClient() GitClient {
	if c == nil || c.Git == nil {
		return NewGitClient(DefaultGitRunner)
	}
	return NewGitClient(c.Git)
}

func (c *defaultGitClient) Run(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
	if c == nil || c.runner == nil {
		return fmt.Errorf("git runner not initialized")
	}
	if ctx == nil {
		return fmt.Errorf("context cannot be nil")
	}
	if stdout == nil {
		stdout = io.Discard
	}
	if stderr == nil {
		stderr = io.Discard
	}
	return c.runner.Run(ctx, stdout, stderr, args...)
}

func (c *defaultGitClient) runOutput(ctx context.Context, args ...string) (string, error) {
	if len(args) == 0 {
		return "", fmt.Errorf("git command arguments cannot be empty")
	}
	var stdout, stderr bytes.Buffer
	err := c.Run(ctx, &stdout, &stderr, args...)
	if err != nil {
		errStr := strings.TrimSpace(stderr.String())
		if errStr == "" {
			errStr = strings.TrimSpace(stdout.String())
		}
		cmdStr := strings.Join(args, " ")
		if errStr != "" {
			return "", fmt.Errorf("git %s failed: %w: %s", cmdStr, err, errStr)
		}
		return "", fmt.Errorf("git %s failed: %w", cmdStr, err)
	}
	return strings.TrimSpace(stdout.String()), nil
}

func isValidRemoteName(s string) bool {
	s = strings.TrimSpace(s)
	if s == "" || s == "." {
		return false
	}
	return !strings.Contains(s, "://") && !strings.Contains(s, "/") && !strings.ContainsAny(s, " \t\r\n")
}

// ResolveRemote resolves the Gerrit git remote name in priority order:
// 1. git config ghish.gerrit.remote
// 2. current branch tracking remote (branch.<cur>.remote or @{u} prefix)
// 3. "origin" if present in `git remote`
// 4. common Gerrit remotes ("goog", "aosp", "partner") or first remote in `git remote`
// 5. fallback "origin"
func (c *defaultGitClient) ResolveRemote(ctx context.Context) string {
	if c == nil || c.runner == nil {
		return "origin"
	}
	if ctx == nil {
		ctx = context.Background()
	}

	if v, err := c.runOutput(ctx, "config", "--get", "ghish.gerrit.remote"); err == nil && isValidRemoteName(v) {
		return strings.TrimSpace(v)
	}

	if curBranch, err := c.runOutput(ctx, "branch", "--show-current"); err == nil {
		curBranch = strings.TrimSpace(curBranch)
		if curBranch != "" && curBranch != "HEAD" && !strings.Contains(curBranch, "://") {
			if r, err := c.runOutput(ctx, "config", "--get", fmt.Sprintf("branch.%s.remote", curBranch)); err == nil && isValidRemoteName(r) {
				return strings.TrimSpace(r)
			}
		}
	}

	if upstream, err := c.runOutput(ctx, "rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{u}"); err == nil {
		upstream = strings.TrimSpace(upstream)
		if !strings.Contains(upstream, "://") {
			if rem, _, ok := strings.Cut(upstream, "/"); ok && isValidRemoteName(rem) && rem != "refs" {
				return rem
			}
		}
	}

	if out, err := c.runOutput(ctx, "remote"); err == nil && strings.TrimSpace(out) != "" {
		var remotes []string
		remoteSet := make(map[string]bool)
		for _, line := range strings.Split(out, "\n") {
			r := strings.TrimSpace(line)
			if isValidRemoteName(r) {
				remotes = append(remotes, r)
				remoteSet[r] = true
			}
		}
		if len(remotes) > 0 {
			for _, preferred := range []string{"origin", "goog", "aosp", "partner"} {
				if remoteSet[preferred] {
					return preferred
				}
			}
			return remotes[0]
		}
	}

	return "origin"
}

// ResolveDefaultBranch resolves the repository's default upstream branch in priority order:
// 1. git config ghish.gerrit.defaultbranch
// 2. current branch's tracked upstream merge target (branch.<cur>.merge when branch.<cur>.remote is set and target != cur)
// 3. git symbolic-ref refs/remotes/<remote>/HEAD
// 4. git config init.defaultBranch
// 5. refs/heads/main, refs/remotes/<remote>/main, and legacy default branch refs
// 6. fallback "main"
func (c *defaultGitClient) ResolveDefaultBranch(ctx context.Context) string {
	if c == nil || c.runner == nil {
		return "main"
	}
	if ctx == nil {
		ctx = context.Background()
	}

	if v, err := c.runOutput(ctx, "config", "--get", "ghish.gerrit.defaultbranch"); err == nil {
		v = strings.TrimSpace(v)
		if v != "" && !strings.Contains(v, "://") {
			return v
		}
	}

	if curBranch, err := c.runOutput(ctx, "branch", "--show-current"); err == nil {
		curBranch = strings.TrimSpace(curBranch)
		if curBranch != "" && curBranch != "HEAD" && !strings.Contains(curBranch, "://") {
			if r, rErr := c.runOutput(ctx, "config", "--get", fmt.Sprintf("branch.%s.remote", curBranch)); rErr == nil && isValidRemoteName(r) {
				if mergeRef, mErr := c.runOutput(ctx, "config", "--get", fmt.Sprintf("branch.%s.merge", curBranch)); mErr == nil {
					mergeBranch := strings.TrimPrefix(strings.TrimSpace(mergeRef), "refs/heads/")
					if mergeBranch != "" && mergeBranch != "HEAD" && mergeBranch != curBranch && !strings.Contains(mergeBranch, "://") {
						return mergeBranch
					}
				}
			}
		}
	}

	remote := c.ResolveRemote(ctx)
	symRefTarget := fmt.Sprintf("refs/remotes/%s/HEAD", remote)
	if sym, err := c.runOutput(ctx, "symbolic-ref", symRefTarget); err == nil {
		sym = strings.TrimSpace(sym)
		prefix := fmt.Sprintf("refs/remotes/%s/", remote)
		if strings.HasPrefix(sym, prefix) {
			if b := strings.TrimPrefix(sym, prefix); b != "" && b != "HEAD" {
				return b
			}
		}
	}

	if v, err := c.runOutput(ctx, "config", "--get", "init.defaultBranch"); err == nil {
		v = strings.TrimSpace(v)
		if v != "" && !strings.Contains(v, "://") {
			return v
		}
	}

	type refCandidate struct {
		ref    string
		branch string
	}
	candidates := []refCandidate{
		{ref: "refs/heads/main", branch: "main"},
		{ref: fmt.Sprintf("refs/remotes/%s/main", remote), branch: "main"},
		{ref: fmt.Sprintf("refs/remotes/%s/master", remote), branch: "master"}, // inclusive-language: ignore
		{ref: "refs/heads/master", branch: "master"},                           // inclusive-language: ignore
	}
	for _, cand := range candidates {
		if err := c.Run(ctx, io.Discard, io.Discard, "rev-parse", "--verify", cand.ref); err == nil {
			return cand.branch
		}
	}

	return "main"
}

func (c *defaultGitClient) CurrentBranch(ctx context.Context) (string, error) {
	return c.runOutput(ctx, "branch", "--show-current")
}

func (c *defaultGitClient) HeadCommitMessage(ctx context.Context) (string, error) {
	return c.CommitMessage(ctx)
}

func (c *defaultGitClient) CommitMessage(ctx context.Context, ref ...string) (string, error) {
	if len(ref) > 1 {
		return "", fmt.Errorf("git log -1 accepts at most one ref, got %d", len(ref))
	}
	args := []string{"log", "-1", "--format=%B"}
	if len(ref) == 1 {
		trimmed := strings.TrimSpace(ref[0])
		if trimmed == "" {
			return "", fmt.Errorf("commit ref cannot be empty")
		}
		args = append(args, trimmed)
	}
	return c.runOutput(ctx, args...)
}

func (c *defaultGitClient) ConfigGet(ctx context.Context, key string) (string, error) {
	key = strings.TrimSpace(key)
	if key == "" {
		return "", fmt.Errorf("git config key cannot be empty")
	}
	return c.runOutput(ctx, "config", "--get", key)
}

func (c *defaultGitClient) RevParse(ctx context.Context, args ...string) (string, error) {
	if len(args) == 0 {
		return "", fmt.Errorf("git rev-parse requires at least one argument")
	}
	for i, arg := range args {
		if strings.TrimSpace(arg) == "" {
			return "", fmt.Errorf("git rev-parse argument %d cannot be empty", i)
		}
	}
	return c.runOutput(ctx, append([]string{"rev-parse"}, args...)...)
}

func (c *defaultGitClient) VerifyRef(ctx context.Context, ref string) (bool, error) {
	if c == nil || c.runner == nil {
		return false, fmt.Errorf("git runner not initialized")
	}
	if ctx == nil {
		return false, fmt.Errorf("context cannot be nil")
	}
	if err := ctx.Err(); err != nil {
		return false, err
	}
	ref = strings.TrimSpace(ref)
	if ref == "" {
		return false, fmt.Errorf("ref cannot be empty")
	}

	var stderr bytes.Buffer
	err := c.Run(ctx, io.Discard, &stderr, "rev-parse", "--verify", ref)
	if err == nil {
		return true, nil
	}
	errStr := strings.TrimSpace(stderr.String())
	errMsg := err.Error()
	if strings.Contains(errStr, "Needed a single revision") ||
		strings.Contains(errStr, "Not a valid object name") ||
		strings.Contains(errStr, "not found") ||
		strings.Contains(errMsg, "not found") ||
		strings.Contains(errMsg, "Needed a single revision") {
		return false, nil
	}
	if strings.Contains(errStr, "not a git repository") || strings.Contains(errMsg, "not a git repository") {
		return false, fmt.Errorf("not a git repository: %w", err)
	}
	if errStr != "" {
		return false, fmt.Errorf("git rev-parse --verify %s failed: %w: %s", ref, err, errStr)
	}
	return false, fmt.Errorf("git rev-parse --verify %s failed: %w", ref, err)
}

func (c *defaultGitClient) GitDir(ctx context.Context) (string, error) {
	dir, err := c.RevParse(ctx, "--git-dir")
	if err != nil {
		return "", err
	}
	dir = strings.TrimSpace(dir)
	if dir == "" {
		return "", fmt.Errorf("git rev-parse --git-dir returned empty path")
	}
	if !filepath.IsAbs(dir) {
		if realRunner, ok := c.runner.(*RealGitRunner); ok && realRunner.Dir != "" {
			dir = filepath.Join(realRunner.Dir, dir)
		}
	}
	return dir, nil
}

func (c *defaultGitClient) CountCommitsAhead(ctx context.Context, branch string) (int, error) {
	branch = strings.TrimSpace(branch)
	if branch == "" {
		return 0, fmt.Errorf("cannot count commits ahead: branch name is empty")
	}
	return CountCommitsAhead(ctx, c, branch)
}

func (c *defaultGitClient) StackCommits(ctx context.Context, branch string) ([]StackCommit, error) {
	branch = strings.TrimSpace(branch)
	if branch == "" {
		return nil, fmt.Errorf("cannot list stack commits: branch name is empty")
	}
	remote := c.ResolveRemote(ctx)
	primaryRef := fmt.Sprintf("%s/%s", remote, branch)

	candidates := []string{
		primaryRef,
		fmt.Sprintf("refs/remotes/%s/%s", remote, branch),
		"HEAD@{upstream}",
		branch,
	}
	baseRef := ""
	for _, cand := range candidates {
		if err := c.Run(ctx, io.Discard, io.Discard, "rev-parse", "--verify", cand); err == nil {
			baseRef = cand
			break
		}
	}
	if baseRef == "" {
		return nil, fmt.Errorf(
			"could not resolve upstream base branch %q on remote %q (tried %s, refs/remotes/%s/%s, HEAD@{upstream}, and %s)\n\n"+
				"Why: Stack inspection requires a valid upstream reference to bound the commit range and never falls back to walking full repository history.\n"+
				"Fix: Fetch the remote branch or specify an explicit base branch:\n"+
				"  git fetch %s %s\n"+
				"  gh pr create --base <branch> --stack",
			branch, remote, primaryRef, remote, branch, branch, remote, branch,
		)
	}

	rangeSpec := fmt.Sprintf("%s..HEAD", baseRef)
	var outBuf, errBuf bytes.Buffer
	if err := c.Run(ctx, &outBuf, &errBuf, "log", "--reverse", "--format=%h%x00%s%x00%B%x1e", rangeSpec); err != nil {
		if ctx != nil && ctx.Err() != nil {
			return nil, ctx.Err()
		}
		return nil, fmt.Errorf("git log %s failed: %w", rangeSpec, err)
	}
	raw := outBuf.String()
	if strings.TrimSpace(raw) == "" {
		return nil, nil
	}
	records := strings.Split(raw, "\x1e")
	var commits []StackCommit
	for _, rec := range records {
		rec = strings.TrimLeft(rec, "\r\n")
		if strings.TrimSpace(rec) == "" {
			continue
		}
		parts := strings.SplitN(rec, "\x00", 3)
		if len(parts) < 3 {
			continue
		}
		hash := strings.TrimSpace(parts[0])
		subj := strings.TrimSpace(parts[1])
		body := parts[2]
		commits = append(commits, StackCommit{
			Hash:     hash,
			Subject:  subj,
			Body:     body,
			ChangeID: ExtractChangeID(body),
		})
	}
	return commits, nil
}

func (c *defaultGitClient) Fetch(ctx context.Context, remote, ref string, stdout, stderr io.Writer) error {
	remote = strings.TrimSpace(remote)
	if remote == "" {
		return fmt.Errorf("git fetch requires a remote")
	}
	ref = strings.TrimSpace(ref)
	if ref == "" {
		return fmt.Errorf("git fetch requires a ref")
	}
	return c.Run(ctx, stdout, stderr, "fetch", remote, ref)
}

func (c *defaultGitClient) Checkout(ctx context.Context, ref string, stdout, stderr io.Writer) error {
	ref = strings.TrimSpace(ref)
	if ref == "" {
		return fmt.Errorf("git checkout requires a ref")
	}
	return c.Run(ctx, stdout, stderr, "checkout", ref)
}

func (c *defaultGitClient) CherryPick(ctx context.Context, ref string, stdout, stderr io.Writer) error {
	ref = strings.TrimSpace(ref)
	if ref == "" {
		return fmt.Errorf("git cherry-pick requires a ref")
	}
	return c.Run(ctx, stdout, stderr, "cherry-pick", ref)
}

func (c *defaultGitClient) IsHeadRemoteTip(ctx context.Context) (bool, []string, error) {
	var stdout, stderr bytes.Buffer
	if err := c.Run(ctx, &stdout, &stderr, "for-each-ref", "--points-at=HEAD", "--format=%(refname)", "refs/remotes/"); err != nil {
		return false, nil, fmt.Errorf("failed to check remote tracking branches: %w (stderr: %s)", err, stderr.String())
	}
	var remotes []string
	for _, line := range strings.Split(stdout.String(), "\n") {
		line = strings.TrimSpace(line)
		if line != "" {
			remotes = append(remotes, line)
		}
	}
	return len(remotes) > 0, remotes, nil
}

func (c *defaultGitClient) HeadAuthorEmail(ctx context.Context) (string, error) {
	var stdout, stderr bytes.Buffer
	if err := c.Run(ctx, &stdout, &stderr, "log", "-1", "--format=%ae", "HEAD"); err != nil {
		return "", fmt.Errorf("failed to get HEAD author email: %w (stderr: %s)", err, stderr.String())
	}
	return strings.TrimSpace(stdout.String()), nil
}

func (c *defaultGitClient) UserEmail(ctx context.Context) (string, error) {
	return c.ConfigGet(ctx, "user.email")
}

type sequencerMarker struct {
	name      string
	operation string
	fix       string
}

var activeSequencerMarkers = []sequencerMarker{
	{
		name:      "rebase-merge",
		operation: "interactive rebase",
		fix:       "  git rebase --continue\n  # or abort:\n  git rebase --abort",
	},
	{
		name:      "rebase-apply",
		operation: "rebase / patch application",
		fix:       "  git rebase --continue\n  # or abort:\n  git rebase --abort",
	},
	{
		name:      "MERGE_HEAD",
		operation: "merge",
		fix:       "  git merge --continue\n  # or abort:\n  git merge --abort",
	},
	{
		name:      "CHERRY_PICK_HEAD",
		operation: "cherry-pick",
		fix:       "  git cherry-pick --continue\n  # or abort:\n  git cherry-pick --abort",
	},
	{
		name:      "REVERT_HEAD",
		operation: "revert",
		fix:       "  git revert --continue\n  # or abort:\n  git revert --abort",
	},
	{
		name:      "BISECT_LOG",
		operation: "bisect",
		fix:       "  git bisect reset",
	},
}

// CheckAmendAllowed verifies that HEAD is safe to amend locally, rejecting
// active sequencer operations, multi-parent merge commits, and remote tips.
func (c *defaultGitClient) CheckAmendAllowed(ctx context.Context) error {
	gitDir, err := c.GitDir(ctx)
	if err != nil {
		return err
	}
	for _, m := range activeSequencerMarkers {
		if _, statErr := os.Stat(filepath.Join(gitDir, m.name)); statErr == nil {
			return fmt.Errorf(
				"cannot amend HEAD commit: a git %s is currently in progress (%s exists in %s).\n\n"+
					"Why: Amending during an in-progress %s rewrites intermediate commit state and can corrupt your branch history.\n"+
					"To finish or cancel the in-progress operation first:\n%s\n\n"+
					"To inspect current repository status:\n  git status",
				m.operation, m.name, gitDir, m.operation, m.fix,
			)
		}
	}

	parentsOut, err := c.runOutput(ctx, "rev-list", "--parents", "-n", "1", "HEAD")
	if err != nil {
		return err
	}
	if fields := strings.Fields(parentsOut); len(fields) > 2 {
		return fmt.Errorf(
			"cannot amend HEAD commit: HEAD (%s) is a merge commit with %d parents.\n\n"+
				"Why: Amending a merge commit rewrites multi-parent merge ancestry, whereas Gerrit changes are single-parent linear commits.\n"+
				"To linearize your commit history onto the target branch instead:\n"+
				"  git rebase %s/main\n\n"+
				"To inspect recent commit ancestry:\n"+
				"  git log --graph -n 5",
			fields[0], len(fields)-1, c.ResolveRemote(ctx),
		)
	}

	isTip, remotes, err := c.IsHeadRemoteTip(ctx)
	if err != nil {
		return err
	}
	if isTip {
		return fmt.Errorf("cannot amend HEAD commit: HEAD matches remote tracking branch tip (%s).\n\n"+
			"This usually occurs after 'git reset --hard' to upstream or checking out a remote branch.\n"+
			"Amending now will hijack upstream history and original author's identity.\n"+
			"Please stage your changes and create a new commit first (e.g. 'git commit').",
			strings.Join(remotes, ", "))
	}
	return nil
}

// HasStagedChanges reports whether the Git index differs from HEAD.
func (c *defaultGitClient) HasStagedChanges(ctx context.Context) (bool, error) {
	var stderr bytes.Buffer
	err := c.Run(ctx, io.Discard, &stderr, "diff", "--cached", "--quiet")
	if err == nil {
		return false, nil
	}
	var exitErr *exec.ExitError
	if errors.As(err, &exitErr) && exitErr.ExitCode() == 1 {
		return true, nil
	}
	if errStr := strings.TrimSpace(stderr.String()); errStr != "" {
		return false, fmt.Errorf("git diff --cached --quiet failed: %w: %s", err, errStr)
	}
	return false, fmt.Errorf("git diff --cached --quiet failed: %w", err)
}

func (c *defaultGitClient) commitWithMessageFile(
	ctx context.Context,
	message string,
	baseArgs []string,
	noVerify bool,
	stdout, stderr io.Writer,
) error {
	if strings.TrimSpace(message) == "" {
		return fmt.Errorf("commit message cannot be empty")
	}
	tmpFile, err := os.CreateTemp("", "ghish-commit-msg-*")
	if err != nil {
		return fmt.Errorf("failed to create temporary commit message file: %w", err)
	}
	defer os.Remove(tmpFile.Name())

	_, writeErr := io.WriteString(tmpFile, message)
	if err := errors.Join(writeErr, tmpFile.Close()); err != nil {
		return fmt.Errorf("failed to write temporary commit message file: %w", err)
	}

	args := append([]string{}, baseArgs...)
	if noVerify {
		args = append(args, "--no-verify")
	}
	args = append(args, "-F", tmpFile.Name())
	return c.Run(ctx, stdout, stderr, args...)
}

// CommitNewAll commits staged index changes (or passes -a when nothing is
// staged yet) using a temporary commit message file, returning autoStaged=true
// when -a was used.
func (c *defaultGitClient) CommitNewAll(ctx context.Context, message string, noVerify bool, stdout, stderr io.Writer) (bool, error) {
	hasStaged, err := c.HasStagedChanges(ctx)
	if err != nil {
		return false, err
	}
	autoStaged := !hasStaged
	baseArgs := []string{"commit", "-a"}
	if hasStaged {
		baseArgs = []string{"commit"}
	}
	return autoStaged, c.commitWithMessageFile(ctx, message, baseArgs, noVerify, stdout, stderr)
}

// CommitAmendMessage rewrites HEAD's commit message via a temporary file while
// preserving staged index changes and intentional empty commits.
func (c *defaultGitClient) CommitAmendMessage(ctx context.Context, message string, noVerify bool, stdout, stderr io.Writer) error {
	if err := c.CheckAmendAllowed(ctx); err != nil {
		return err
	}
	return c.commitWithMessageFile(ctx, message, []string{"commit", "--amend", "--only", "--allow-empty"}, noVerify, stdout, stderr)
}

// CommitAmendNoEdit runs commit-msg hooks on HEAD while preserving staged index
// changes and intentional empty commits.
func (c *defaultGitClient) CommitAmendNoEdit(ctx context.Context, stdout, stderr io.Writer) error {
	if err := c.CheckAmendAllowed(ctx); err != nil {
		return err
	}
	return c.Run(ctx, stdout, stderr, "commit", "--amend", "--only", "--allow-empty", "--no-edit")
}

// UpdateHeadRef atomically moves HEAD from expectedCurrentSHA to targetSHA via
// compare-and-swap without modifying the index or working tree.
func (c *defaultGitClient) UpdateHeadRef(ctx context.Context, targetSHA, expectedCurrentSHA, reason string) error {
	targetSHA = strings.TrimSpace(targetSHA)
	if targetSHA == "" {
		return fmt.Errorf("update-ref target SHA cannot be empty")
	}
	expectedCurrentSHA = strings.TrimSpace(expectedCurrentSHA)
	if expectedCurrentSHA == "" {
		return fmt.Errorf("update-ref expected current SHA cannot be empty")
	}
	args := []string{"update-ref"}
	reason = strings.TrimSpace(reason)
	if reason != "" {
		args = append(args, "-m", reason)
	}
	args = append(args, "HEAD", targetSHA, expectedCurrentSHA)
	_, err := c.runOutput(ctx, args...)
	return err
}
