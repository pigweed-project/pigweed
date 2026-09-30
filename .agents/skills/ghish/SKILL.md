---
name: ghish
description: >-
  Use the ./gh CLI (gh-ish) for all Gerrit code reviews (CLs, pwrev), LUCI
  Buildbucket CI checks and tryjobs, Buganizer issues (b/, pwbug.dev), and
  pushing changes using GitHub CLI syntax.
---

# `gh-ish` (`./gh`): Gerrit, Buildbucket & Buganizer CLI

Pigweed provides `./gh` (a cached repository wrapper around
`//pw_ghish:gh-ish`), exposing Gerrit code reviews (`./gh pr`), LUCI Buildbucket
CI checks (`./gh pr checks`, `./gh run`), Google Issue Tracker / Buganizer
(`./gh issue`), and worktree pools (`./gh wt`) through standard GitHub CLI
(`gh`) syntax.

This skill is the single source of truth for Gerrit, Buildbucket, and Buganizer
workflows in Pigweed, completely replacing legacy raw `git push`, manual `curl`
commands, `search_builds.py`, and `.gitcookies` scripts.

## Quick Command Reference

All commands run via `./gh <family> <subcommand>` (or root aliases `./gh view`,
`./gh diff`, `./gh push`, `./gh checks`, `./gh status`, `./gh worktree`):

### 1. Change Targeting & Inspection
Subcommands accepting `[<id>]` support:
- **Omitted argument**: Automatically resolves the active change on the current
  Git branch.
- **Change number**: `472267` or with patchset `472267/3`.
- **Gerrit URL**:
  `https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/472267`
  (or `/+/472267/3`).
- **Shortlink**: `pwrev/472267`, `pwrev.dev/472267`, `pwrev.dev/i/472267`
  (internal review), `fxrev/472267`, `fxrev.dev/i/472267`, `crrev.com/c/472267`.
- **Branch name**: `my-feature`, `cl/472267`, `change-472267` (resolves via
  branch commit `Change-Id` or `branch.<name>.gerrit-change-id` config).

Commands:
- **`./gh pr view [<id>]`**: View change metadata (patchset, owner, reviewers,
  attention set, status, labels). Defaults to active change on current branch.
  - `-c, --comments`: Display all file and inline comment threads indented by
    file and line number.
  - `--json <fields>`: Output structured JSON
    (e.g. `number,title,state,author,files,reviewers`). Unknown fields are
    strictly rejected.
  - `--json bug,bugs`: Read back the linked bugs — the Gerrit counterpart to
    GitHub's `closingIssuesReferences`. `bug` is a flat `"b/123456, b/789"`
    string; `bugs` is `[{"id": "b/123456", "closes": true}]`, where `closes`
    distinguishes a `Fixed:` trailer (closes the bug on submit) from a `Bug:`
    trailer (links only). Use this to verify a `./gh pr edit --bug` actually
    took, and to check whether a bug is already linked before adding one.
- **`./gh pr diff [<id>[/<patchset>]]`**: View unified patch diff.
- **`./gh pr checkout [<id>[/<patchset>]]`**: Fetch and check out change branch
  or specific patchset locally at `FETCH_HEAD`.

### 2. Push & Edit
- **`./gh pr create`**: Push local commit(s) to Gerrit as a **new** change.
  - *Safety Guard*: Fails with a clear error and URL if the change already
    exists (use `pr push` to update existing changes).
  - *Stack Guard*: Halts if pushing multiple commits unless `--stack` is
    specified.
  - Flags: `-r, --reviewer <email>`, `-c, --cc <email>`, `--auto`,
    `--cq [1|2]` (defaults to `+1` dry run; `2` submits), `-d, --draft` (WIP),
    `-B, --base <branch>`, `--stack`, `--publish`, `-o, --push-option <opt>`,
    `--no-verify`.
- **`./gh pr push`** (aliases: `./gh push`, `./gh pr upload`): Push local
  commit(s) to Gerrit to upload a **new patchset** on an existing change.
  - *Branch Memory*: Automatically queries Gerrit by `Change-Id` to discover
    the CL's target branch (e.g. sandbox branch), guaranteeing updates land
    on the right branch.
  - *Stack Guard*: Requires `--stack` if pushing multiple commits ahead of
    origin.
  - Supports `--ready` (remove WIP) and all `create` flags.
  - *Smart Fallback*: If pushed with `--cq` or metadata on an already
    up-to-date commit, `pr push` automatically applies updates via the Gerrit
    API instead of failing.
- **`./gh pr edit [<id>]`**: Edit Gerrit CL metadata:
  - Trigger CQ dry run: `./gh pr edit --cq` (or `--cq 2` to submit, `--cq 0` to
    remove vote; also supports `--add-label <Name>=<Score>`).
  - Update reviewers/assignees: `./gh pr edit --add-reviewer user@google.com`
    (`--remove-reviewer`, `--add-assignee`, `--remove-assignee`).
  - Set or remove topic/hashtags: `./gh pr edit --topic <name>`
    (`--remove-topic`, `--add-hashtag`, `--remove-hashtag`).
  - *Commit-Message Edits (`b/567763970`)*: Unlike GitHub (where PR titles and
    descriptions live in the server database), Gerrit stores the CL description
    inside the Git commit message of each patchset. Commit-message flags
    (`--title`, `--body`, `--message`, `--bug`, `--fixed`) are disabled in
    `gh pr edit` and redirect to local surgical commit-message editing:
    1. `git log -1 --format=%B HEAD > "$(git rev-parse --git-dir)/COMMIT_EDITMSG_TMP"`
    2. Surgically edit `"$(git rev-parse --git-dir)/COMMIT_EDITMSG_TMP"` (preserving `Change-Id:`)
    3. `git commit --amend --only -F "$(git rev-parse --git-dir)/COMMIT_EDITMSG_TMP" && ./gh pr push`

### 3. Review & Comment
- **`./gh pr comment [<id>] --path <file> --line <line> -m <msg>`**: Post an
  inline comment (auto-threads onto any existing thread on `<file>:<line>`).
  - `--resolved`: Mark thread as resolved (requires both `--path` and `--line`).
  - `--draft`: Save comment as a private unpublished draft visible only to you.
- **`./gh pr comment [<id>] -m <msg>`**: Post a change-level comment
  (`-F <file>` reads from file).
- **`./gh pr review [<id>] [--approve | --request-changes] [--cq [1|2]] [-m <msg>]`**:
  Submit review (`--approve` votes `Code-Review+2`; `--request-changes` votes
  `Code-Review-1`). For full review criteria, see
  [`.agents/skills/code_review/SKILL.md`](../code_review/SKILL.md).

### 4. Monitor & Rerun CI / Buildbucket Checks
- **`./gh pr checks [<id>[/<patchset>]]`**: Query remote LUCI Buildbucket checks
  (`./pw presubmit` runs local host validation).
  - `-w, --watch`: Monitor checks until all blocking checks finish.
  - `--fail-fast`: Exit immediately upon the first blocking failure (implies `--watch`).
  - `--log-failed`: Automatically display failure reports and LogDog snippets on exit (default: `true`).
  - `-e, --experimental`: Include non-blocking experimental checks in output.
  - **Exit codes**: `0` = all blocking checks passed, `8` = checks still
    running, `1` = blocking check failed, canceled, or no checks reported.
    Branch on the exit code; never scrape the table.
  - *Rebase Note*: Builds from prior patchsets remain applicable across
    `TRIVIAL_REBASE`, `TRIVIAL_REBASE_WITH_MESSAGE_UPDATE`, `NO_CODE_CHANGE`,
    and `NO_CHANGE` patchsets.
  - *Gerrit UI Check Count Note*: `./gh pr checks` returns top-level Buildbucket
    builders (e.g. ~72), whereas the Gerrit UI "Checks" tab (~132) also counts
    individual `pw_presubmit` sub-steps and static analyzers (AyeAye, SLSA).
- **`./gh run view [<id>] [-j|--job <builder|id>] [--log-failed] [--log] [-v] [--json]`**:
  Inspect failed builders, LogDog failure snippets (`--log-failed`), or the
  hierarchical step tree (`-j <builder>`).
- **`./gh run rerun [<id>] [--failed | -j <builder>] [--dry-run]`**: Rerun all
  failed builders or a specific builder via `bb add`.
- **`./gh run list [<id>]`** & **`./gh run watch [<id>]`**.

### 5. Listing, Merging & Status
- **`./gh pr list [--limit 30] [--state open|merged|closed|all] [--json <fields>]`**.
- **`./gh pr merge [<id>] [--auto] [--cq]`**: Submit change to target branch.
  Prefer `--auto` (auto-submit upon approval) or `--cq` (`Commit-Queue+2`) over
  bare `pr merge`.
- **`./gh pr status [--all]`**: Focused dashboard of current branch, CLs created
  by you, and incoming reviews (scoped to last 30 days unless `--all` is passed).

### 6. Buganizer Issue Management (`./gh issue`)
- **`./gh issue status`** & **`./gh issue list [--assignee @me|<email>] [--state open|closed|all] [--label priority:P1|type:BUG|component:<id>|hotlist:<id>] [--search "<q>"]`**.
- **`./gh issue view [<id>|<url>] [--comments] [--json <fields>]`**: View issue
  (resolves `Bug:`/`Fixed:` trailer from `HEAD` if `<id>` is omitted).
- **`./gh issue develop <id> [--checkout | -w|--worktree]`**: Create feature
  branch or allocate an isolated warm worktree slot (`./gh wt use --issue <id>`).
- **`./gh issue create -t "<title>" -b "<body>" [--amend | --commit]`**: Create
  a Buganizer issue (`--amend` appends `Bug: b/<new-id>` to `HEAD`).
- **`./gh issue comment [<id>] -m "<text>"`**, **`./gh issue edit [<id>]`**,
  **`./gh issue close [<id>]`**, **`./gh issue reopen [<id>]`**.

### 7. Where `gh` Habits Break
Use the long form for `--auto` (`-a` is `--assignee`), `--publish` (`-p` is
`--project`), `--force` (`-f` is `--fill`), `--cq` (`-q` is `--jq`), and
`--message` on `pr merge` (`-m` is `--merge`).

| You type | Real `gh` | Here |
|---|---|---|
| `pr list -a` | assignee | queries Gerrit `reviewer:` — Gerrit dropped assignees in 3.8. |
| `pr list -l` | issue label | a Gerrit **vote** predicate, e.g. `Code-Review+2`. |
| `run list/view --json` | a field list | a boolean; it takes no fields. `pr view --json` does take fields. |
| `--json state` | `OPEN`/`CLOSED`/`MERGED` | Gerrit's `NEW`/`MERGED`/`ABANDONED`. |
| `pr review --request-changes` | blocks the PR | votes `Code-Review-1`, which is **advisory**. `Code-Review-2` is the veto. |
| `pr comment --draft` | (n/a) | an unpublished draft **comment** nobody else can see — not a WIP change. |
| `issue -l/--label` | free-form text | Buganizer priority/type/hotlist (`P0`–`P4`, `bug`, `feature`, `task`, `hotlist:<id>`). |

---

## Key Agent Workflows

### Workflow 1: Addressing Review Feedback
1. **Fetch & review comments**: `./gh pr view <id> --comments`
2. **Apply fixes & verify locally**: `./pw presubmit --mode auto --base origin/main`
3. **Upload updated patchset**: `git commit -a --amend --no-edit && ./gh pr push`
4. **Reply & resolve threads**:
   `./gh pr comment <id> --path <file> --line <line> -m "Fixed." --resolved`

### Workflow 2: CI Triage & Targeted Retry
1. **Check or watch status**: `./gh pr checks <id>` or run
   `./gh pr checks <id> --watch --fail-fast` as a **background command** (never
   poll in an agent loop!).
2. **Inspect failure logs**: `./gh run view <id> --log-failed` (or
   `./gh run view <id> -j <builder>`).
3. **Rerun failed builders**: `./gh run rerun <id> --failed` (or `-j <builder>`).
4. **Full CQ dry run**: `./gh pr edit <id> --cq`.

---

## Critical Rules for AI Agents

1. **NEVER use raw `git push`**: Always use `./gh pr push` (or `./gh push`)
   to update existing changes with new patchsets, and `./gh pr create` to create
   new changes.
2. **Use `--draft` for Private Notes**: Intermediate or preparatory review
   comments should use `--draft` so they remain private to you until ready.
3. **Resolve Threads Explicitly**: When fixing an issue raised by a reviewer,
   always reply with `--resolved --path <file> --line <line>`.
4. **Authentication is Automatic**: Never use manual `curl -sb ~/.gitcookies` or
   `gob-curl`; `./gh` handles corp (`gob-curl`) and `.gitcookies` auth automatically.
5. **Never Ignore Command Failures or Exit Codes**: Invalid inputs fail fast
   with non-zero exit codes. Always inspect stderr and address reported errors.
6. **Preserve `Change-Id` and Commit Trailers**: Preserve `Change-Id` footers
   when editing, amending, squashing, or rebasing commits. Gerrit uses these to
   link git commits to Change Lists. If multiple commits are combined, ensure
   ONLY the `Change-Id` from the earliest commit in the series is retained in
   the final commit message. To edit a commit message, dump it to a file
   (`git log -1 --format=%B HEAD > "$(git rev-parse --git-dir)/COMMIT_EDITMSG_TMP"`), edit the file
   surgically while keeping `Change-Id:` intact, and apply with
   `git commit --amend --only -F "$(git rev-parse --git-dir)/COMMIT_EDITMSG_TMP"`.
7. **Link Bugs With Trailers, Never `Fixes #N`**: GitHub's `Fixes #456` does
   **nothing** on Gerrit. Use `Bug: b/456` or `Fixed: b/456` trailers in the
   commit message (or `./gh issue create --amend`), and verify with
   `./gh pr view <id> --json bug,bugs`.
8. **Submit Changes, Not Patchsets**: Use `./gh pr merge <id> --cq` (or
   `--auto`) without `/<patchset>` suffixes.
9. **NEVER Poll CI in an Agent Loop**: Run `./gh pr checks --watch --fail-fast`
   as a background command and stop calling tools until notified.
