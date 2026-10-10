---
name: manual-cli-test
description: >-
  Interactively "taste the CLI" and "kick the tires" on any CLI tool or
  subcommand (/manual-cli-test, /agent-cli-kick-tires). Guides agents through
  decoupling branch-dependent binaries, setting up disposable Git worktree
  sandboxes, running multi-axis scenario matrices, critiquing real stdout/stderr
  ergonomics from a consumer's perspective, and using fresh-eyes subagents.
disable-model-invocation: true
---

# Manual CLI Testing & Tasting (`/manual-cli-test` / `agent-cli-kick-tires`)

**Unit tests and mocked integration tests are necessary, but they are never
sufficient for a CLI tool.**

Unit tests only verify assertions the author *remembered* to write against
synthetic fixtures. A CLI command can have 100% unit test pass rates while still
emitting output that is contradictory on adjacent lines, bloated with bot noise,
missing critical fields needed for the next step, or actively misleading when run
against real repository and server state.

`/manual-cli-test` (**"tasting the CLI"** or **"kicking the tires"**) is a
disciplined workflow where you—the LLM—step out of the code-author mindset and
into the consumer mindset. You build a standalone snapshot of the CLI, exercise
it across a matrix of realistic and adversarial states in a safe sandbox, read
every line of `stdout` and `stderr` critically, fix what feels wrong, and
re-taste until the CLI is crisp, honest, and ergonomic.

---

## Core Philosophy: Why You Must "Taste" the CLI

1. **Mocks Hide Real-World Messiness**: Live Gerrit CLs, Buganizer issues, LUCI
   CI runs, and Git worktrees contain bot spam, HTML entities, reverse
   chronological sorting, detached `HEAD` states, and half-finished rebases that
   hand-crafted unit test structs rarely model.
2. **Consumer Goal Alignment**: Every CLI command exists to support a downstream
   decision (by a human developer or an autonomous workflow like `/crank`).
   Tasting asks: *"If I only had this stdout, could I take the exact right next
   step immediately without running three extra diagnostic commands?"*
3. **Curse of Knowledge**: Because you just wrote the code, your brain
   automatically fills in missing context when reading the output. Deliberate
   tasting—especially via a naive "fresh-eyes" subagent—exposes where the output
   is ambiguous or self-contradictory.

---

## Phase 1: Safe Execution Strategy (Decoupling Binary & Repo State)

### The Branch-Dependent Binary Trap

When testing a CLI subcommand that lives on your current feature branch (such as
a new `./gh` or `./pw` subcommand), **do not** test branch-switching, checkout,
rebase, or `--prepare` flows by invoking the repo wrapper (`./gh`) inside your
active development worktree:

1. **The Binary Disappears**: Switching branches or checking out an older CL in
   the active worktree replaces the source tree—causing `./gh` to rebuild an
   older commit where your new subcommand or fix does not exist!
2. **Worktree Contamination**: Simulating dirty files, merge conflicts, or branch
   checkouts in your primary worktree risks clobbering uncommitted work or
   dirtying `MODULE.bazel.lock`.
3. **Bazel Cache Churn**: Repeatedly switching branches in your primary worktree
   invalidates incremental build state.

### The Decoupled Binary + Disposable Worktree Sandbox Pattern

Always separate **the binary under test** from **the Git worktree being
manipulated**:

1. **Compile & Snapshot the Binary to `/tmp`**:
   Build the target once in your primary feature worktree and copy the compiled
   executable to `/tmp`:
   ```bash
   bazelisk build --noshow_progress --noshow_loading_progress //pw_ghish:gh-ish
   cp -f bazel-bin/pw_ghish/gh-ish_/gh-ish /tmp/gh-under-test
   chmod +x /tmp/gh-under-test
   ```
   *(Whenever you edit code during the fix loop, re-run this one-liner to
   refresh `/tmp/gh-under-test`.)*

2. **Use Secondary Worktrees for Read-Only Cross-Worktree Sensing**:
   If `git worktree list` or `./gh wt list` shows another clean worktree while
   your primary worktree has an active CL, you can test read-only commands there
   via a subshell:
   ```bash
   (cd /path/to/other/worktree && /tmp/gh-under-test <subcmd>)
   ```

3. **Create a Disposable `git worktree` Sandbox in `/tmp` for Mutations**:
   For any scenario that switches branches, creates commits, dirties files,
   triggers merge/rebase conflicts, or tests mutating flags (like `--prepare` or
   `checkout`), create a lightweight detached worktree in `/tmp` with an
   automatic `EXIT` cleanup trap:
   ```bash
   SANDBOX="/tmp/cli-taste-sandbox-$$"
   cleanup() {
     git worktree remove --force "$SANDBOX" 2>/dev/null || rm -rf "$SANDBOX"
     git worktree prune 2>/dev/null || true
     git branch -D test-cli-wip test-cli-conflict 2>/dev/null || true
   }
   trap cleanup EXIT
   git worktree add --detach "$SANDBOX" origin/main
   ```

4. **Verify Your Sandbox Setup Actually Produced the Intended State**:
   - *Common Trap*: When simulating a `git rebase` conflict by editing a file on
     `origin/main~3` and rebasing onto `origin/main`, if that file was not
     actually modified between `origin/main~3` and `origin/main`, Git will
     fast-forward/rebase cleanly without a conflict—and you will accidentally
     test a clean branch thinking you tested a conflict!
   - *Fix*: Dynamically pick a file that *actually* changed in the upstream
     range:
     ```bash
     (
       cd "$SANDBOX"
       git checkout -q -b test-cli-conflict origin/main~1
       FILE=$(git diff --name-only origin/main~1 origin/main | head -n 1)
       echo "# SIMULATED CONFLICT" > "$FILE"
       git commit -q -a -m "pw_foo: Simulated conflict on $FILE"
       git rebase origin/main || true  # Guaranteed UU conflict on $FILE
       /tmp/gh-under-test <subcmd>
       git rebase --abort || true
     )
     ```

5. **Check Primary Worktree Hygiene Afterward**:
   After running your suite, run `git status -s` in your primary worktree and
   revert any incidental lockfile touches (e.g.
   `git checkout -- MODULE.bazel.lock`).

---

## Phase 2: Designing a Multi-Axis Scenario Matrix

Before running commands, design a concrete scenario list of 8–12 cases spanning
four orthogonal axes:

### Axis 1: Local Repository & Worktree State
- **Active Open CL**: Branch is 1 commit ahead of `origin/main` with an open
  Gerrit CL (`status=NEW`).
- **Clean / Merged Idle State**: Worktree is at `origin/main` (`ahead=0`) or on
  a branch whose CL was already `MERGED` or `ABANDONED`.
- **Dirty Local WIP (`ahead=0, dirty=true`)**: Modified tracked files on a brand
  new branch with zero commits ahead of `origin/main`.
- **Stale Branch (Clean vs. Conflicting)**: Branch is $N$ commits behind
  `origin/main` (test both a cleanly mergeable branch and one where in-memory
  `git merge-tree --write-tree HEAD origin/main` detects a conflict).
- **Mid-Rebase Conflict (`REBASE_IN_PROGRESS`)**: Active `git rebase` paused on
  an unmerged (`UU`) file conflict.

### Axis 2: Target & Input Variety
- **Your Own Open CL vs. Teammate's Open CL**: Discover real CL numbers first
  (`/tmp/gh-under-test pr list --limit 10`) so you test against real review
  threads, CI checks, and ownership metadata.
- **Merged / Closed Targets**: Point the CLI at an already-`MERGED` CL and an
  already-`CLOSED` Buganizer issue.
- **Cross-Linked Targets**: Point the CLI at an `OPEN` bug that *already has* an
  open Gerrit CL linked to it (should recommend adopting the CL, not creating a
  duplicate CL!).
- **Alternate Input Formats**: Shortlinks (`pwrev/12345`, `b/12345`), full
  URLs, or external artifact URLs (e.g. Google Chat thread URLs).
- **Non-Existent (`404`) & Malformed Inputs**: Test `pwrev/999999999`,
  `b/999999999`, and `not-a-valid-target`.

### Axis 3: Modes, Flags & Environment Overrides
- **`--help` Inspection**: Always read `<subcmd> --help` first. Are all flags,
  defaults, and examples accurate?
- **Output Modes**: Default human/LLM compact output vs. `--json` vs. `--status`.
- **Scope Flags & Env Vars**: Test flags like `--fleet` / `--all` and
  environment overrides (e.g. `GH_ISH_ONCALL_FILE=/tmp/mock-oncall.cfg`).

### Axis 4: Mutating Flags, Safety Gates & Idempotency
- **Blocked Safety Gates**: Run mutating flags (e.g. `--prepare <target>`) while
  on a dirty branch or active open CL and verify it refuses to clobber work with
  a clear reason.
- **Clean Execution**: Run the mutating flag in the clean `/tmp` sandbox and
  verify the mutation succeeds.
- **Second Consecutive Run (Idempotency)**: Immediately run the *exact same*
  mutating command a second time when already on the target branch—and also test
  switching to a branch that already existed locally with commits.

---

## Phase 3: The "Taste, Critique, Fix, Re-Taste" Loop

Run your scenario matrix in batches, read the literal output for each scenario,
and evaluate it against the **6 Tasting Questions**:

1. **Internal Consistency**: Do any two lines in the output contradict each
   other?
2. **Goal Support**: Does the output give the consumer (e.g. `/crank` or a human
   engineer) everything needed to execute the next step without extra CLI
   round-trips?
3. **Actionability of Directives**: Do the recommended `Next:` steps make sense
   for *this exact* state, or are they generic boilerplate that tells the user
   to do something invalid (like fixing a closed bug or checking out a 404 CL)?
4. **Context Separation**: Does local worktree state accidentally bleed into
   remote target inspection (or vice versa)?
5. **Signal-to-Noise & Token Diet**: Is the output polluted by bot boilerplate,
   HTML tags, redundant JSON arrays, or reverse-chronological comment threads?
6. **Latency & Redundant RPCs**: Did any scenario stall for seconds making
   unnecessary network calls (e.g. querying CI checks for an already-merged CL)
   or timing out on cold SSO calls?

### Field Guide: 9 Classic Bugs Only "Tasting" Catches

These real bugs were all discovered in a single `./gh sense` tasting session
after 100% of unit tests were already passing:

| Bug Pattern | What Happened in Practice | Why Unit Tests Missed It & How Tasting Caught It |
| :--- | :--- | :--- |
| **1. Upstream State Leakage on `ahead=0`** | Creating a new branch `git checkout -b wip origin/main` (`ahead=0`) and dirtying a file caused the CLI to read `HEAD`'s `Change-Id:` trailer from `origin/main` and report upstream's last merged CL (`#497018 MERGED`) as the branch's `Active CL`. | Unit tests mocked `ChangeID: ""` whenever `CommitsAhead: 0`. Live Git `HEAD` on `origin/main` *has* a `Change-Id` trailer from the last merged commit! |
| **2. Contradictory Adjacent Lines** | During a mid-rebase conflict, the CLI printed `Summary: Git rebase in progress on HEAD with unresolved conflicts` right above `Rebase: UP_TO_DATE — 0 commits behind origin/main`. | During `git rebase origin/main`, detached `HEAD` sits on `origin/main` (`behind=0`). `computeRebaseVerdict` checked `CommitsBehind == 0` before `RebaseInProgress`. |
| **3. Post-Mutation State Clobbering** | Running `--prepare b/469150426` when the bug branch already existed locally switched to the branch, but overwrote `CommitsAhead = 0` and `ChangeID = ""` instead of re-sensing `gitState` and `activeCL`—hiding the branch's existing open CL! | Unit tests only tested `CanSafelyPrepareWorktree` gate logic, not the state re-sensing after `executeSafePrepare` checked out an existing branch. |
| **4. Active Context Shadowing Target Context** | Running `./gh sense pwrev/490178` (a teammate's CL) from an active worktree (on `#498153`) rendered `Active CL: #498153` in full and completely hid `Target CL: #490178`! | Status card rendering had `if r.ActiveCL != nil { render(r.ActiveCL) } else { render(r.Target.CL) }`, starving the explicit target whenever the local worktree had an active CL. |
| **5. Wrong-Branch File Enrichment** | Inspecting a remote un-checked-out CL (`pwrev/477005`) attempted to read comment file paths from the local `cwd` worktree, attaching code snippets from the wrong branch (or trying to read `/COMMIT_MSG` from disk). | `enrichCommentsWithLocalCode` was called unconditionally inside `senseGerritCL` without checking if the CL was actually checked out at `cwd`. |
| **6. Recommending Duplicate Work** | Sensing an open bug (`b/469150426`) that already showed `pwrev/498953 [NEW]` under `Related Gerrit CLs` still told the agent in `Next:` to implement a fix from scratch and run `./gh pr create`. | Classification logic for `CRANK_BUG` didn't inspect whether `issue.RelatedCLs` already contained a `NEW` CL to adopt. |
| **7. Actioning Closed / Merged / 404 Targets** | Passing a `CLOSED` bug (`b/571616997`) or a non-existent CL (`pwrev/999999999`) still recommended allocating a worktree slot and writing a fix or running `pr checkout 999999999`. | Classifier assumed any valid ID string resolved to an actionable open item without checking `Issue.State == "CLOSED"`, `CL.Status == "MERGED"`, or `NotFound`. |
| **8. Bot Noise & Backwards Comment Order** | Live Buganizer comments printed newest-first (`#4, #3, #2`) with 15 lines of `gitwatcher` HTML (`&nbsp;`, `<details>`) and `blunderbuss` auto-assigner noise. | Mocked issue comments in unit tests were clean 1-line strings in ascending order; live Buganizer API returns reverse-chronological entries with automated bot HTML. |
| **9. Silent Flag No-Ops & Missing Micro-Context** | `--fleet` populated internal structs but never rendered the `Fleet Worktrees` section in text mode; `dirty=true (1 files)` omitted the dirty filenames; `Target CL` omitted `(owner=...)`. | Text formatter was written before `--fleet` was wired up, and only tasting revealed that omitting dirty file paths and CL owner forced extra CLI calls. |

### Closing the Loop

After fixing all issues discovered during tasting:
1. Add unit tests covering every edge case you uncovered so they never regress.
2. Rebuild `/tmp/<tool>-under-test`.
3. **Re-run the failed scenarios in the sandbox** and read the new output side
   by side to confirm every fix works in live execution.

---

## Phase 4: Strategic Use of Subagents for CLI Tasting

When `invoke_subagent` is available, always use `TypeName: "self"` and
`Workspace: "inherit"` (never `"branch"` or `"share"`, which create ad-hoc
checkouts with cold Bazel caches). Subagents unlock three high-leverage CLI
testing patterns:

### Pattern A: The "Fresh-Eyes / Naive Consumer" Subagent (Recommended)

Because the author agent knows the implementation internals, it often overlooks
confusing flag names, cryptic status badges, or missing next-step instructions.
To get an unbiased evaluation, spawn a subagent that has **not** read the
implementation source files:

- **What to give the subagent**:
  1. The path to the decoupled binary (`/tmp/<tool>-under-test`).
  2. A dedicated disposable sandbox directory (`/tmp/cli-sandbox-fresh`).
  3. The high-level user goal (e.g., *"You are an agent running `/crank`. Run
     `/tmp/gh-under-test sense --help` to learn the tool, then run it across
     these 5 scenarios in `/tmp/cli-sandbox-fresh`. Do NOT read the Go source
     code. Report every place where the output is confusing, contradictory,
     noisy, or leaves you unsure what command to run next."*).
- **Why it works**: The subagent experiences the CLI strictly through `--help`,
  `stdout`, `stderr`, and exit codes—just like a real downstream consumer.

### Pattern B: Parallel Sandbox Scenario Runners

When testing a large scenario matrix or slow network/build flows, fan out 2–3
subagents in parallel (`Workspace: "inherit"`), giving each subagent its own
isolated `/tmp/cli-sandbox-<N>` worktree and a slice of the scenario matrix
(e.g., Subagent 1 tests local Git/rebase conflict states; Subagent 2 tests live
Gerrit CL and Buganizer target states; Subagent 3 tests `--prepare` safety gates
and idempotency).

### Pattern C: Trajectory-Aware Retrospective Subagent

After an intensive manual CLI testing and debugging session in the parent
conversation, you can delegate documentation, skill updates, or regression test
authoring to a subagent while preserving the full history of what went wrong:

- Pass the parent conversation's transcript path
  (`<appDataDir>/brain/<parent-conversation-id>/.system_generated/logs/transcript.jsonl`)
  and step range in the subagent prompt.
- Instruct the subagent to read those transcript lines via `view_file` so it can
  see the exact raw CLI outputs, hypotheses, and fixes without cluttering the
  parent agent's context window.
