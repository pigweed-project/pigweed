---
name: freshen
description: >-
  Safely rebase or fast-forward the active Pigweed worktree onto its target
  branch (origin/main), resolve Pigweed dual-build and code conflicts, preserve
  Gerrit Change-Id footers, and verify local build/format health.
disable-model-invocation: true
---

# Freshen Worktree & Rebase (`/freshen`)

`/freshen` brings the current Pigweed worktree and branch up to date with its
upstream target branch (defaulting to `origin/main`) without losing warm Bazel
caches, clobbering Gerrit `Change-Id:` footers, or silently breaking dual-build
(`BUILD.bazel` / `BUILD.gn` / `CMakeLists.txt`) parity.

Use `/freshen` standalone whenever a worktree or CL is behind `origin/main`, or
invoke it as the first stage of `/crank`.

---

## Strict Invariants

1. **Record a rollback anchor first**: Before any mutating Git command, capture
   `PRE_FRESHEN_SHA=$(git rev-parse HEAD)` and log it. If an irreconcilable
   conflict occurs, abort (`git rebase --abort`) and verify `HEAD` matches
   `PRE_FRESHEN_SHA`.
2. **Never clobber `Change-Id:` footers**: Every rebased or amended commit MUST
   retain its exact original `Change-Id: I...` line so Gerrit links new
   patchsets to the existing Change List (CL).
3. **Respect target branch memory**: Do not blindly rebase onto `origin/main` if
   the CL targets a feature or release branch (e.g., `sandbox/*`). Query
   `./gh pr view --json branch` first when a CL exists.
4. **Stay inside the active checkout**: Do not create ad-hoc temporary
   checkouts or `git worktree` directories just to rebase—doing so invalidates
   the local Bazel `output_base`.

---

## Step-by-Step Workflow

### Step 1: Sense Branch, Dirty State & Rebase Verdict

Run `./gh sense` (or inspect its output if already called by `/crank`) and record
a rollback anchor before any mutating Git command:

```bash
PRE_FRESHEN_SHA=$(git rev-parse HEAD)
echo "PRE_FRESHEN_SHA=${PRE_FRESHEN_SHA}"
./gh sense
```

`./gh sense` computes an in-memory 3-way merge check (`git merge-tree --write-tree HEAD origin/main`)
and outputs a `Rebase:` verdict:
- **`REBASE_SKIP_KEEP_CQ`**: The branch merges cleanly with `origin/main` in
  memory, has zero unresolved comments/drafts, and already has active/passing
  `CQ+1` tryjobs. **Skip rebasing** unless the user explicitly requested a
  standalone `/freshen`—uploading a trivial rebased patchset would reset active
  CQ tryjobs for zero benefit!
- **`REBASE_CONFLICT` or `REBASE_RECOMMENDED`**: Proceed to Step 2 (`git fetch origin`
  + rebase onto `origin/<target_branch>`).

### Step 2: Select the Freshen Mode

Choose the appropriate mode based on local state:

| Local State | Mode | Exact Action |
| :--- | :--- | :--- |
| **Clean & Merged / Synced** (`ahead == 0` or CL `state == "MERGED"`, clean tree) | **Fast-Forward / Next** | If preparing the checkout for a specific new bug or CL, run `./gh sense --prepare [<target>]`. If simply fast-forwarding onto `origin/main`, run `git fetch origin && git merge --ff-only origin/main` (or `./gh wt next` if using `./gh wt`). |
| **Dirty Working Tree** (`git status --porcelain` non-empty) | **Autostash + Rebase** | Stash tracked and untracked changes before rebasing: `git stash push -u -m "freshen-autostash-${PRE_FRESHEN_SHA}"`, run `git fetch origin && git rebase <UPSTREAM>`, then `git stash pop` and resolve any stash overlap. |
| **Unmerged Local Commit(s) / Active CL** (`ahead > 0`, CL `state == "NEW"` or unuploaded) | **Target-Aware Rebase** | Run `git fetch origin && git rebase <UPSTREAM>`. |
| **Already Mid-Rebase / Mid-Merge** (`.git/rebase-merge` or `MERGE_HEAD` exists) | **Conflict Recovery** | Inspect conflicted files (`git diff --name-only --diff-filter=U`) and proceed directly to Step 3. |

### Step 3: Pigweed-Aware Conflict Resolution

If `git rebase <UPSTREAM>` stops with conflicts, inspect every conflicted file
(`git diff --name-only --diff-filter=U`) and resolve using Pigweed domain rules:

1. **Build File Conflicts (`BUILD.bazel`, `BUILD.gn`, `CMakeLists.txt`)**:
   - **Never** blindly pick `--ours` or `--theirs` when both upstream and the
     local commit added targets, `srcs`, `hdrs`, or `deps`.
   - Combine both additions, keep lists (`srcs`, `hdrs`, `deps`, `public`)
     sorted alphabetically, and ensure any new source/header added in
     `BUILD.bazel` is identically reflected in `BUILD.gn` (and `CMakeLists.txt`
     if present in that module).
2. **`MODULE.bazel` / `MODULE.bazel.lock` / CIPD Tool Rolls**:
   - Keep upstream (`origin/main`) version bumps and pin updates, then re-apply
     only the specific dependency additions introduced by the local commit.
3. **C++ / Rust / Python Header or API Refactorings**:
   - Before editing conflicted code, inspect how upstream changed the file:
     ```bash
     git log -n 5 -p <UPSTREAM> -- <conflicted_file>
     ```
   - Adapt the local change to the new upstream API (e.g., updated `pw::Result`
     signatures, `pw_async2` dispatcher changes, renamed headers) while
     honoring Pigweed style (C++17 compatibility, no dynamic allocation in core
     modules, `#pragma once`).
4. **Commit Continuity & `Change-Id:` Preservation**:
   - Stage resolved files (`git add <files>`) and run:
     ```bash
     GIT_EDITOR=true git rebase --continue
     ```
   - Verify with `git log -1 --format=%B HEAD` that the original `Change-Id:`
     footer is intact.

#### Escape Hatch for Irreconcilable Conflicts
If upstream deleted the target module or replaced the subsystem architecture
such that a mechanical/semantic rebase would require a ground-up redesign:
1. Run `git rebase --abort`.
2. Verify `git rev-parse HEAD` equals `PRE_FRESHEN_SHA`.
3. Report the exact upstream commit (`git log -1 <UPSTREAM> -- <path>`) that
   superseded or conflicted with the change so `/crank` or the user can decide
   whether to abandon or re-architect.

### Step 4: Post-Freshen Verification

Whenever commits were rebased (`ahead > 0` after rebase):

1. **Format Check**:
   Run `./pw format` (or `./pw format --fix`). If formatting modified any files
   in a single-commit CL, fold them into `HEAD` without touching the commit
   message:
   ```bash
   git add -u && git commit --amend --no-edit
   ```
2. **Targeted Build & Test**:
   Identify the affected Pigweed module(s) from `git diff --name-only <UPSTREAM>...HEAD`
   and run hermetic Bazel tests:
   ```bash
   bazelisk test --noshow_progress --noshow_loading_progress //<module>/...
   ```
3. **Structured Summary**:
   Report:
   - Previous vs. new `HEAD` SHA and number of upstream commits integrated.
   - Conflicts resolved (if any) and how they were resolved.
   - Verification status (`./pw format` and `bazelisk test` results).
   - Note that the rebased commit has **not** been pushed yet (unless invoked
     inside `/crank`, which handles pushing in its CI loop).
