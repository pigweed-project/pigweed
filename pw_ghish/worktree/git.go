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

package worktree

import (
	"bytes"
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"

	"pigweed.dev/pw_ghish"
)

// GitRunner abstracts all Git and worktree operations for hermetic testing.
type GitRunner interface {
	// WorktreeAdd creates a new git worktree at path detached at origin/main (or HEAD).
	WorktreeAddDetached(primaryRepo, worktreePath string) error
	// SwitchBranch switches a worktree directory to branchName. If createFromRef is non-empty
	// and branchName does not exist locally, creates it tracking createFromRef using non-forcing -c.
	SwitchBranch(worktreePath, branchName, createFromRef string) error
	// SwitchDetach switches a worktree directory to detached HEAD at targetRef (e.g. "origin/main").
	SwitchDetach(worktreePath, targetRef string) error
	// SwitchDetachForce resets uncommitted/untracked files and detaches HEAD at targetRef.
	SwitchDetachForce(worktreePath, targetRef string) error
	// StatusPorcelain returns uncommitted file status lines (`git status --porcelain`).
	StatusPorcelain(worktreePath string) (string, error)
	// CurrentBranch returns the checked out branch name (or "HEAD" if detached).
	CurrentBranch(worktreePath string) (string, error)
	// RevParse returns the full SHA of rev in repoOrWorktreePath.
	RevParse(repoOrWorktreePath, rev string) (string, error)
	// ExtractChangeID parses the Gerrit Change-Id trailer from the commit message of rev.
	ExtractChangeID(repoOrWorktreePath, rev string) (string, error)
	// CommitsAhead returns the number of commits in rev that are not in baseRef (e.g. "origin/main..HEAD").
	CommitsAhead(repoOrWorktreePath, baseRef, rev string) (int, error)
	// FetchOrigin runs `git fetch origin` in repoOrWorktreePath.
	FetchOrigin(repoOrWorktreePath string) error
	// FetchRef runs `git fetch <remote> <ref>` in repoOrWorktreePath.
	FetchRef(repoOrWorktreePath, remote, ref string) error
	// RebaseOriginMain rebases the current branch in worktreePath onto origin/main.
	RebaseOriginMain(worktreePath string) error
	// BranchExists reports whether refs/heads/<branchName> exists in primaryRepo, or returns an error on git failure.
	BranchExists(primaryRepo, branchName string) (bool, error)
	// EnsureCommitMsgHook verifies and installs the Gerrit commit-msg hook in primaryRepo.
	EnsureCommitMsgHook(primaryRepo string) (installed bool, err error)
}

// ExecGitRunner executes real git subprocess commands using pw_ghish primitives.
type ExecGitRunner struct{}

func NewExecGitRunner() *ExecGitRunner {
	return &ExecGitRunner{}
}

func (g *ExecGitRunner) runGit(dir string, args ...string) (string, error) {
	cmd := exec.Command("git", args...)
	cmd.Dir = dir
	var stdout, stderr bytes.Buffer
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	if err := cmd.Run(); err != nil {
		errMsg := strings.TrimSpace(stderr.String())
		if errMsg == "" {
			errMsg = strings.TrimSpace(stdout.String())
		}
		return "", fmt.Errorf("git %s in %s failed: %w\nDetails: %s", strings.Join(args, " "), dir, err, errMsg)
	}
	return strings.TrimSpace(stdout.String()), nil
}

func (g *ExecGitRunner) WorktreeAddDetached(primaryRepo, worktreePath string) error {
	if _, err := os.Stat(worktreePath); err == nil {
		// Directory already exists; verify it has a .git file or directory.
		if _, gitErr := os.Stat(filepath.Join(worktreePath, ".git")); gitErr == nil {
			return nil
		}
	}
	if err := os.MkdirAll(filepath.Dir(worktreePath), 0755); err != nil {
		return fmt.Errorf("failed to create parent directory for worktree %s: %w", worktreePath, err)
	}
	// Try detaching at origin/main first; fall back to HEAD if origin/main is absent (e.g. local test repos).
	if _, err := g.runGit(primaryRepo, "worktree", "add", "--detach", worktreePath, "origin/main"); err != nil {
		if _, fallbackErr := g.runGit(primaryRepo, "worktree", "add", "--detach", worktreePath, "HEAD"); fallbackErr != nil {
			return fmt.Errorf("failed to add git worktree at %s: %w", worktreePath, err)
		}
	}
	return nil
}

func (g *ExecGitRunner) BranchExists(primaryRepo, branchName string) (bool, error) {
	client := pw_ghish.NewGitClientInDir(primaryRepo)
	return client.VerifyRef(context.Background(), "refs/heads/"+branchName)
}

func (g *ExecGitRunner) resolveBaseRef(worktreePath, ref string) (string, error) {
	if ref == "" {
		ref = "origin/main"
	}
	client := pw_ghish.NewGitClientInDir(worktreePath)
	ok, err := client.VerifyRef(context.Background(), ref)
	if err != nil {
		return "", fmt.Errorf("failed to verify ref %q in %s: %w", ref, worktreePath, err)
	}
	if ok {
		return ref, nil
	}
	// Only allow fallback from origin/main -> main for local repositories without remotes.
	// NEVER silently fall back to HEAD!
	if ref == "origin/main" {
		mainOk, mainErr := client.VerifyRef(context.Background(), "main")
		if mainErr == nil && mainOk {
			return "main", nil
		}
	}
	return "", fmt.Errorf("base ref %q does not exist in repository %s", ref, worktreePath)
}

func (g *ExecGitRunner) SwitchBranch(worktreePath, branchName, createFromRef string) error {
	exists, err := g.BranchExists(worktreePath, branchName)
	if err != nil {
		return fmt.Errorf("failed to check if branch %q exists in %s: %w", branchName, worktreePath, err)
	}
	if exists {
		_, err := g.runGit(worktreePath, "switch", branchName)
		return err
	}
	baseRef, err := g.resolveBaseRef(worktreePath, createFromRef)
	if err != nil {
		return err
	}
	// Use lowercase -c (non-forcing create) so existing branches are never clobbered.
	_, err = g.runGit(worktreePath, "switch", "-c", branchName, baseRef)
	return err
}

func (g *ExecGitRunner) SwitchDetach(worktreePath, targetRef string) error {
	baseRef, err := g.resolveBaseRef(worktreePath, targetRef)
	if err != nil {
		return err
	}
	_, err = g.runGit(worktreePath, "switch", "--detach", baseRef)
	return err
}

func (g *ExecGitRunner) SwitchDetachForce(worktreePath, targetRef string) error {
	baseRef, err := g.resolveBaseRef(worktreePath, targetRef)
	if err != nil {
		return err
	}
	if _, err := g.runGit(worktreePath, "reset", "--hard"); err != nil {
		return err
	}
	if _, err := g.runGit(worktreePath, "clean", "-fd"); err != nil {
		return err
	}
	_, err = g.runGit(worktreePath, "switch", "--detach", "--discard-changes", baseRef)
	return err
}

func (g *ExecGitRunner) StatusPorcelain(worktreePath string) (string, error) {
	return g.runGit(worktreePath, "status", "--porcelain")
}

func (g *ExecGitRunner) CurrentBranch(worktreePath string) (string, error) {
	out, err := g.runGit(worktreePath, "rev-parse", "--abbrev-ref", "HEAD")
	if err != nil {
		return "", err
	}
	return out, nil
}

func (g *ExecGitRunner) RevParse(repoOrWorktreePath, rev string) (string, error) {
	return g.runGit(repoOrWorktreePath, "rev-parse", rev)
}

func (g *ExecGitRunner) ExtractChangeID(repoOrWorktreePath, rev string) (string, error) {
	msg, err := g.runGit(repoOrWorktreePath, "log", "-n", "1", "--format=%B", rev)
	if err != nil {
		return "", err
	}
	return pw_ghish.ExtractChangeID(msg), nil
}

func (g *ExecGitRunner) CommitsAhead(repoOrWorktreePath, baseRef, rev string) (int, error) {
	if _, err := g.runGit(repoOrWorktreePath, "rev-parse", "--verify", baseRef); err != nil {
		if baseRef == "origin/main" {
			if _, err2 := g.runGit(repoOrWorktreePath, "rev-parse", "--verify", "main"); err2 == nil {
				baseRef = "main"
			} else {
				return 0, err
			}
		} else {
			return 0, err
		}
	}
	out, err := g.runGit(repoOrWorktreePath, "rev-list", "--count", fmt.Sprintf("%s..%s", baseRef, rev))
	if err != nil {
		return 0, err
	}
	return strconv.Atoi(strings.TrimSpace(out))
}

func (g *ExecGitRunner) FetchOrigin(repoOrWorktreePath string) error {
	_, err := g.runGit(repoOrWorktreePath, "fetch", "origin")
	return err
}

func (g *ExecGitRunner) FetchRef(repoOrWorktreePath, remote, ref string) error {
	_, err := g.runGit(repoOrWorktreePath, "fetch", remote, ref)
	return err
}

func (g *ExecGitRunner) RebaseOriginMain(worktreePath string) error {
	_, err := g.runGit(worktreePath, "rebase", "origin/main")
	return err
}

func (g *ExecGitRunner) EnsureCommitMsgHook(primaryRepo string) (bool, error) {
	gitDirOut, err := g.runGit(primaryRepo, "rev-parse", "--git-common-dir")
	if err != nil {
		return false, fmt.Errorf("failed to locate git common dir in %s: %w", primaryRepo, err)
	}
	if !filepath.IsAbs(gitDirOut) {
		gitDirOut = filepath.Join(primaryRepo, gitDirOut)
	}
	hookPath := filepath.Join(gitDirOut, "hooks", "commit-msg")
	if info, err := os.Stat(hookPath); err == nil && info.Mode()&0111 != 0 {
		return false, nil // Already installed and executable
	}
	if err := os.MkdirAll(filepath.Dir(hookPath), 0755); err != nil {
		return false, fmt.Errorf("failed to create hooks directory: %w", err)
	}
	// Check if primaryRepo or parent has a standard commit-msg hook we can copy,
	// or write standard Gerrit Change-Id generator hook stub.
	stubHook := `#!/bin/sh
# Standard Gerrit Change-Id commit-msg hook managed by gh wt init
if grep -q "^Change-Id:" "$1"; then
  exit 0
fi
random_id=$( (git var GIT_AUTHOR_IDENT ; git var GIT_COMMITTER_IDENT ; cat "$1" ; date ; dd if=/dev/urandom bs=64 count=1 2>/dev/null) | git hash-object --stdin )
awk '
  BEGIN { added=0 }
  /^Signed-off-by:/ && !added { print "Change-Id: I" id; added=1 }
  { print }
  END { if (!added) print "\nChange-Id: I" id }
' id="$random_id" "$1" > "$1.tmp" && mv "$1.tmp" "$1"
`
	if err := os.WriteFile(hookPath, []byte(stubHook), 0755); err != nil {
		return false, fmt.Errorf("failed to install commit-msg hook at %s: %w", hookPath, err)
	}
	return true, nil
}
