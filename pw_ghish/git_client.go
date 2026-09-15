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
)

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
	Fetch(ctx context.Context, remote, ref string, stdout, stderr io.Writer) error
	Checkout(ctx context.Context, ref string, stdout, stderr io.Writer) error
	CherryPick(ctx context.Context, ref string, stdout, stderr io.Writer) error
	CommitAmendNoEdit(ctx context.Context, stdout, stderr io.Writer) error
	IsHeadRemoteTip(ctx context.Context) (bool, []string, error)
	HeadAuthorEmail(ctx context.Context) (string, error)
	UserEmail(ctx context.Context) (string, error)
	CheckAmendAllowed(ctx context.Context) error
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
	if dir == "" {
		return "", fmt.Errorf("git rev-parse --git-dir returned empty path")
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

func (c *defaultGitClient) CheckAmendAllowed(ctx context.Context) error {
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

func (c *defaultGitClient) CommitAmendNoEdit(ctx context.Context, stdout, stderr io.Writer) error {
	if err := c.CheckAmendAllowed(ctx); err != nil {
		return err
	}
	return c.Run(ctx, stdout, stderr, "commit", "--amend", "--no-edit")
}
