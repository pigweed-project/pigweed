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
	"context"

	"pigweed.dev/pw_ghish"
)

// WorktreeInfo describes a single worktree entry from `git worktree list --porcelain`.
type WorktreeInfo = pw_ghish.WorktreeInfo

// ParseWorktreeListPorcelain parses the output of `git worktree list --porcelain`.
func ParseWorktreeListPorcelain(output string) []WorktreeInfo {
	return pw_ghish.ParseWorktreeListPorcelain(output)
}

// GitRunner abstracts all Git and worktree operations for hermetic testing.
type GitRunner interface {
	// WorktreeAdd creates a new git worktree at path detached at origin/main (or HEAD).
	WorktreeAddDetached(primaryRepo, worktreePath string) error
	// SwitchBranch switches a worktree directory to branchName. If createFromRef is non-empty
	// and branchName does not exist locally, creates it tracking createFromRef using non-forcing -c.
	SwitchBranch(worktreePath, branchName, createFromRef string) error
	// SwitchExistingBranch switches a worktree directory to branchName, which the caller
	// has already verified exists locally (BranchExists), without verifying it again.
	SwitchExistingBranch(worktreePath, branchName string) error
	// CreateBranch creates branchName from createFromRef (origin/main when empty) using
	// non-forcing `git switch -c`, for a branch the caller has already verified does not
	// exist locally (BranchExists). If it exists after all, git refuses to clobber it.
	CreateBranch(worktreePath, branchName, createFromRef string) error
	// ResetBranch creates or resets branchName to targetRef (`git switch -C <branchName> <targetRef>`).
	// Callers MUST verify that branchName has 0 unmerged commits ahead of origin/main before calling.
	ResetBranch(worktreePath, branchName, targetRef string) error
	// SwitchDetach switches a worktree directory to detached HEAD at targetRef (e.g. "origin/main").
	SwitchDetach(worktreePath, targetRef string) error
	// SwitchDetachForce resets uncommitted/untracked files and detaches HEAD at targetRef.
	SwitchDetachForce(worktreePath, targetRef string) error
	// ListWorktrees returns all active worktrees registered with the repository (`git worktree list --porcelain`).
	ListWorktrees(repoOrWorktreePath string) ([]WorktreeInfo, error)
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

// ExecGitRunner executes real git subprocess commands using pw_ghish.GitClient.
type ExecGitRunner struct {
	Ctx context.Context
}

func NewExecGitRunner() *ExecGitRunner {
	return &ExecGitRunner{}
}

func (g *ExecGitRunner) ctx() context.Context {
	if g != nil && g.Ctx != nil {
		return g.Ctx
	}
	return context.Background()
}

func (g *ExecGitRunner) WorktreeAddDetached(primaryRepo, worktreePath string) error {
	return pw_ghish.NewGitClientInDir(primaryRepo).WorktreeAddDetached(g.ctx(), worktreePath)
}

func (g *ExecGitRunner) ListWorktrees(repoOrWorktreePath string) ([]WorktreeInfo, error) {
	return pw_ghish.NewGitClientInDir(repoOrWorktreePath).ListWorktrees(g.ctx(), true)
}

func (g *ExecGitRunner) BranchExists(primaryRepo, branchName string) (bool, error) {
	return pw_ghish.NewGitClientInDir(primaryRepo).VerifyRef(g.ctx(), "refs/heads/"+branchName)
}

func (g *ExecGitRunner) SwitchBranch(worktreePath, branchName, createFromRef string) error {
	return pw_ghish.NewGitClientInDir(worktreePath).SwitchBranch(g.ctx(), branchName, createFromRef)
}

func (g *ExecGitRunner) SwitchExistingBranch(worktreePath, branchName string) error {
	return pw_ghish.NewGitClientInDir(worktreePath).SwitchExistingBranch(g.ctx(), branchName)
}

func (g *ExecGitRunner) CreateBranch(worktreePath, branchName, createFromRef string) error {
	return pw_ghish.NewGitClientInDir(worktreePath).CreateBranch(g.ctx(), branchName, createFromRef)
}

func (g *ExecGitRunner) ResetBranch(worktreePath, branchName, targetRef string) error {
	return pw_ghish.NewGitClientInDir(worktreePath).ResetBranch(g.ctx(), branchName, targetRef)
}

func (g *ExecGitRunner) SwitchDetach(worktreePath, targetRef string) error {
	return pw_ghish.NewGitClientInDir(worktreePath).SwitchDetach(g.ctx(), targetRef)
}

func (g *ExecGitRunner) SwitchDetachForce(worktreePath, targetRef string) error {
	return pw_ghish.NewGitClientInDir(worktreePath).SwitchDetachForce(g.ctx(), targetRef)
}

func (g *ExecGitRunner) StatusPorcelain(worktreePath string) (string, error) {
	return pw_ghish.NewGitClientInDir(worktreePath).StatusPorcelain(g.ctx())
}

func (g *ExecGitRunner) CurrentBranch(worktreePath string) (string, error) {
	return pw_ghish.NewGitClientInDir(worktreePath).CurrentBranch(g.ctx())
}

func (g *ExecGitRunner) RevParse(repoOrWorktreePath, rev string) (string, error) {
	return pw_ghish.NewGitClientInDir(repoOrWorktreePath).RevParse(g.ctx(), rev)
}

func (g *ExecGitRunner) ExtractChangeID(repoOrWorktreePath, rev string) (string, error) {
	msg, err := pw_ghish.NewGitClientInDir(repoOrWorktreePath).CommitMessage(g.ctx(), rev)
	if err != nil {
		return "", err
	}
	return pw_ghish.ExtractChangeID(msg), nil
}

// InspectHeadAheadChangeID checks in a single git subprocess whether rev has commits
// ahead of baseRef and, if so, parses the Change-Id trailer from the tip commit message.
func (g *ExecGitRunner) InspectHeadAheadChangeID(dir, baseRef, rev string) (ahead int, changeID string, err error) {
	return pw_ghish.NewGitClientInDir(dir).InspectHeadAheadChangeID(g.ctx(), baseRef, rev)
}

func (g *ExecGitRunner) CommitsAhead(repoOrWorktreePath, baseRef, rev string) (int, error) {
	return pw_ghish.NewGitClientInDir(repoOrWorktreePath).CommitsAheadBetween(g.ctx(), baseRef, rev)
}

func (g *ExecGitRunner) FetchOrigin(repoOrWorktreePath string) error {
	return pw_ghish.FetchInDir(g.ctx(), repoOrWorktreePath, "origin")
}

func (g *ExecGitRunner) FetchRef(repoOrWorktreePath, remote, ref string) error {
	return pw_ghish.FetchInDir(g.ctx(), repoOrWorktreePath, remote, ref)
}

func (g *ExecGitRunner) RebaseOriginMain(worktreePath string) error {
	return pw_ghish.NewGitClientInDir(worktreePath).Rebase(g.ctx(), "origin/main")
}

func (g *ExecGitRunner) EnsureCommitMsgHook(primaryRepo string) (bool, error) {
	return pw_ghish.NewGitClientInDir(primaryRepo).EnsureCommitMsgHook(g.ctx())
}
