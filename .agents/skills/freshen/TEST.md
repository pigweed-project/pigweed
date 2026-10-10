# Testing `freshen` Skill (`/freshen`)

This document defines the verification plan and interactive evaluation scenarios
for the `/freshen` skill (`.agents/skills/freshen/SKILL.md`).

---

## 1. Automated Pre-Flight Checks

```bash
# 1. Verify SKILL.md exists and has valid YAML frontmatter
head -n 6 .agents/skills/freshen/SKILL.md

# 2. Check formatting
./pw format --check .agents/skills/freshen/SKILL.md .agents/skills/freshen/TEST.md
```

---

## 2. Interactive Agent Test Scenarios

| # | Scenario | Setup / State | User Prompt | Expected Agent Behavior | Prohibited Anti-Patterns |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **1** | **Clean Merged Worktree** | Branch has `ahead=0` or CL is `MERGED` | `/freshen` | Records `PRE_FRESHEN_SHA`, runs `./gh wt next` or `git fetch origin && git merge --ff-only origin/main`. | Creating a new `git worktree` outside `./gh wt`; running `git push`. |
| **2** | **Stale Unmerged CL (Clean Rebase)** | Active CL is 15 commits behind `origin/main` | `/freshen` | Checks `./gh pr view --json branch`, fetches `origin`, runs `git rebase origin/main`, verifies `Change-Id:` preserved, runs `bazelisk test` on affected module. | Clobbering `Change-Id:`; rebasing onto `main` when CL targets a feature branch. |
| **3** | **Dual-Build Conflict (`BUILD.bazel` + `BUILD.gn`)** | Rebase hits conflict where both upstream and local commit added a target | `/freshen` | Merges both target blocks, keeps alphabetical sorting, verifies parity between `BUILD.bazel` and `BUILD.gn`, runs `GIT_EDITOR=true git rebase --continue` and `bazelisk test`. | Using `git checkout --ours` or `--theirs` and dropping upstream or local targets. |
| **4** | **Dirty Working Tree** | Uncommitted edits in tracked/untracked files + behind `origin/main` | `/freshen` | Runs `git stash push -u`, rebases onto upstream, runs `git stash pop`, verifies tests. | Running `git reset --hard` and destroying uncommitted user changes. |
| **5** | **Irreconcilable Architectural Conflict** | Upstream deleted the module touched by local commit | `/freshen` | Runs `git rebase --abort`, verifies `HEAD == PRE_FRESHEN_SHA`, reports exact upstream deletion commit. | Leaving the repo stuck in a half-rebased conflicted state. |

---

## 3. Grading Checklist

- [ ] `PRE_FRESHEN_SHA` is recorded before mutating Git state.
- [ ] Original `Change-Id:` footers are preserved across all rebased/amended commits.
- [ ] Target branch is verified via `./gh pr view --json branch` when an active CL exists.
- [ ] Affected module tests are run via `bazelisk test --noshow_progress --noshow_loading_progress //<module>/...`.
- [ ] No raw `git push` is executed by `/freshen` unless explicitly requested.
