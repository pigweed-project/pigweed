---
name: ghish
description: >-
  Use the ./gh CLI (gh-ish) for Gerrit code reviews, CI checks, and
  pushing changes using GitHub CLI syntax.
disable-model-invocation: true
---

# `gh-ish` (`./gh`): Gerrit with GitHub CLI Ergonomics

Pigweed provides `./gh` (a zero-overhead cached repository wrapper around
`//pw_ghish:gh-ish`), exposing Gerrit code reviews and LUCI Buildbucket checks
through standard GitHub CLI (`gh pr`) syntax.

This skill is the single source of truth for Gerrit workflows in Pigweed,
completely replacing legacy raw `git push`, manual `curl` commands, and
`.gitcookies` scripts.

## Quick Command Reference

All commands run via `./gh pr <subcommand>` (or standalone
`gh-ish pr <subcommand>`):

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
    took, and to check whether a bug is already linked before adding one. Asking
    for these when Gerrit returned no commit message is an error, not an empty
    answer, so an empty `bug` always means "genuinely nothing linked".
- **`./gh pr diff [<id>[/<patchset>]]`**: View unified patch diff. Defaults to
  active change on current branch.
- **`./gh pr checkout [<id>[/<patchset>]]`**: Fetch and check out change branch
  or specific patchset locally at `FETCH_HEAD`. Defaults to active change on
  current branch.

### 2. Push & Edit
- **`./gh pr create`**: Push local commit(s) to Gerrit as a **new** change.
  - *Safety Guard*: Fails with a clear error and URL if the change already
    exists (use `pr push` to update existing changes).
  - *Stack Guard*: Halts if pushing multiple commits unless `--stack` is
    specified.
  - `-r, --reviewer <email>`: Add reviewer to change.
  - `-c, --cc <email>`: Add CC to change.
  - `--auto`: Automatically submit the change when reviews and checks pass.
  - `--cq [1|2]`: Set Commit-Queue vote (`--cq` defaults to dry run / `+1`;
    specify `2` to submit).
  - `-d, --draft`: Mark change as Work-In-Progress (WIP).
  - `-B, --base <branch>`: Target branch (e.g. `sandbox/experiment`, defaults
    to upstream tracking branch or repository default).
  - `--stack`: Allow pushing multiple commits as a stack of Gerrit changes.
  - `--publish`: Publish draft comments upon pushing.
  - `-o, --push-option <opt>`: Pass raw Gerrit push options (e.g.
    `-o topic=my-topic`).
  - `--no-verify`: Bypass pre-push git hooks.
- **`./gh pr push`** (aliases: `./gh push`, `./gh pr upload`): Push local
  commit(s) to Gerrit to upload a **new patchset** on an existing change.
  - *Branch Memory*: Automatically queries Gerrit by `Change-Id` to discover
    the CL's target branch (e.g. sandbox branch), guaranteeing updates land
    on the right branch.
  - *Stack Guard*: Requires `--stack` if pushing multiple commits ahead of
    origin.
  - Supports all rich push options (`--reviewer`, `--cc`, `--auto`, `--cq`,
    `--draft`, `--ready`, `--publish`, `--stack`, `-o`, `--no-verify`).
  - *Smart Fallback*: If pushed with `--cq` or metadata on an already
    up-to-date commit, `pr push` automatically applies updates via the Gerrit
    API instead of failing.
  - `--ready`: Remove WIP status and mark change as ready for review.
- **`./gh pr edit [<id>]`**: Edit an existing change (defaults to active change
  if omitted):
  - Trigger CQ dry run: `./gh pr edit --cq` (or `--cq 2` to submit, `--cq 0` to
    remove vote; also supports `--add-label <Name>=<Score>`)
  - Update message/reviewers:
    `./gh pr edit --message "new message" --add-reviewer user@google.com`
  - Rewrite only the prose, keeping trailers:
    `./gh pr edit --body "new description"`
  - Link a bug: `./gh pr edit --bug b/123456` (or `--fixed b/123456` to also
    close it on submit; both accept `123456`, `b/123456`, or an issue URL, and
    `--bug none`)
  - *Data Safety*: `--body` and `--title` always preserve every Git trailer
    (`Change-Id:`, `Bug:`, `Fixed:`, `Co-authored-by:`,
    `Cq-Include-Trybots:`, cherry-pick provenance, ...). `--message` replaces
    the **entire** message, so it fails with an error listing any trailers your
    new text leaves out; either carry them forward or pass `--drop-trailers` to
    confirm. `Change-Id:` is always restored regardless.

### 3. Review & Comment
- **`./gh pr comment [<id>] --path <file> --line <line> -m <msg>`**: Post an
  inline comment (defaults to active change if omitted).
  - **Auto-threading**: Automatically detects and appends a reply to the active
    thread on that file and line.
  - `--resolved`: Mark thread as resolved (requires both `--path` and `--line`).
  - `--draft`: Save comment as a private unpublished draft visible only to you
    in Gerrit.
- **`./gh pr comment [<id>] -m <msg>`**: Post a change-level comment
  (`-F <file>` reads from file).
- **`./gh pr review [<id>]`**: Submit change review (defaults to active
  change on current branch if omitted):
  - `--cq [1|2]`: Vote Commit-Queue (`--cq` defaults to dry run / `+1`;
    `--cq 2` = submit).
  - `--approve -m <msg>`: Vote `Code-Review+2` (approve).
  - `--approve --cq`: Approve and trigger CQ dry run in a single step.
  - `--request-changes -m <msg>`: Vote `Code-Review-1` (request changes).

### 4. Monitor & Rerun CI / Buildbucket Checks
Pigweed organizes CI into two complementary command tiers matching the GitHub
CLI:

#### High-Level Check Table (`gh pr checks`):
- **`./gh pr checks [<id>[/<patchset>]]`**: Query remote LUCI Buildbucket checks
  for a change or patchset (defaults to active change if omitted).
  - Displays status icons (`✓`, `✕`, `*`, `?`), builder names, durations, and
    direct log URLs.
  - `-w, --watch`: Continuously monitor checks until all blocking checks finish.
  - `--fail-fast`: Exit immediately upon the first failure among blocking checks
    (implies `--watch`).
  - `-i, --interval <duration>`: Polling interval when watching
    (default: `15s`).
  - `--log-failed`: Automatically display failure reports and LogDog snippets
    for failed checks on exit (default: `true`).
  - `-e, --experimental`: Include non-blocking experimental checks in output.
  - `--web`: Open checks overview in Milo web browser.
  - **Exit codes** (same contract as the real GitHub CLI, on every invocation,
    regardless of `--watch`/`--json`/`--template`): `0` = all blocking checks
    passed, `8` = nothing failed but checks are still running, `1` = a blocking
    check failed, no checks were reported, or the command errored. Branch on the
    exit code; never scrape the table.
    `./gh pr checks --watch && ./gh pr merge --cq` is a safe gate.
  - Fails closed: a change with **no** reported checks exits `1`. Experimental
    builders never influence the exit code, even with `-e`; that flag only
    changes what is displayed.
  - Canceled checks also exit `1` (a canceled build did not pass), but are
    reported as *canceled*, not *failed*. This is normal when querying an older
    patchset: uploading a new patchset cancels the runs still in flight on the
    previous one. Re-query without the `/<patchset>` suffix to see
    current state.
  - *Rebase Note*: If you are on a patchset of type `TRIVIAL_REBASE`,
    `TRIVIAL_REBASE_WITH_MESSAGE_UPDATE`, `NO_CODE_CHANGE`, or `NO_CHANGE`,
    builds from the prior patchset are generally still applicable.
  - *Distinction*: `./gh pr checks` queries **remote cloud CI builders**;
    `./pw presubmit` runs **local host validation**.

#### Deep Run & Job Management (`gh run`):
- **`./gh run list [<id>]`**: List all checks/runs for a change with duration,
  ID, status, and URL (`--json`, `--limit`, `--experimental`).
- **`./gh run view [<id>]`**: Structured run summary overview matching GitHub
  CLI.
  - `-j, --job <builder>`: View hierarchical step execution tree for a specific
    builder (or direct build ID like `867...`). Collapses internal recipe
    plumbing and highlights failures with 1-line extracted diagnostics
    (formatting diffs, compiler errors).
  - `--log-failed`: Fetch and display failure summaries and LogDog error
    snippets directly in the terminal without opening a browser.
  - `--log`: Dump full log stream instead of tail snippet.
  - `-v, --verbose`: Show all unfiltered recipe micro-steps.
  - `-w, --web`: Open build directly in Milo web browser.
  - `--json`: Output machine-parseable failure reports or step details.
- **`./gh run rerun [<id>]`**: Rerun specific or failed CI builders without
  manual URL bashing.
  - `--failed`: Rerun all failed checks on the change
    (e.g. `./gh run rerun --failed`).
  - `-j, --job <builder>`: Rerun a specific builder
    (e.g. `./gh run rerun -j pigweed-lintformat`).
  - `--dry-run`: Print the underlying `bb add` command without executing it.
- **`./gh run watch [<id>]`**: Watch runs until all blocking checks finish.

### 5. Listing, Merging & Navigation
- **`./gh pr list`**: List repository changes (`--limit 30`,
  `--state open|merged|closed|all`, `--json <fields>`).
  List repository changes.
- **`./gh pr merge [<id>] [--auto] [--cq]`**: Submit change to target branch
  (defaults to active change if omitted). In Pigweed/LUCI, use `--auto`
  (auto-submit upon approval) or `--cq` (vote Commit-Queue+2) for automated
  submission; bare `pr merge` requires all gates already satisfied.
- **`./gh pr status [--all]`**: Show focused review status dashboard:
  - **Current branch**: Displays active change ID, title, target branch,
    patchset, submittability, labels/flags, live tryjob check status, and
    comments overview (unresolved threads, unpublished drafts, and inline
    previews with hysteresis).
  - **Created by you** & **Requesting a code review from you**: Scoped to the
    last 30 days by default for fast rendering and token efficiency.
  - `--all` (`-A`): Show all open changes across your account without 30-day
    filter.

### 6. Root-Level Ergonomic Aliases
Direct shortcuts matching common developer muscle memory:
- `./gh checks` -> `./gh pr checks`
- `./gh view` -> `./gh pr view`
- `./gh diff` -> `./gh pr diff`
- `./gh status` -> `./gh pr status`
- `./gh push` -> `./gh pr push`

### 7. Where `gh` Habits Break
No `gh` shorthand is re-used here to mean a different flag. Where a spelling
would collide it is left **unbound**, so the mistake fails with an unknown-flag
error instead of quietly doing the wrong thing — trust that error rather than
working around it. Run `./gh --help` for the authoritative list; this table is
maintained by hand alongside it.

Use the long form for these, because `gh` gives the shorthand another meaning:
`--auto` (gh `-a` is `--assignee`), `--publish` (`-p` is `--project`), `--force`
(`-f` is `--fill`), `--cq` (`-q` is `--jq`), and `--message` on `pr edit` and
`pr merge` (`-m` is `--milestone` and `--merge`).

What still differs is the meaning underneath a flag that is spelled the same:

| You type | Real `gh` | Here |
|---|---|---|
| `pr list -a` | assignee | queries Gerrit `reviewer:` — Gerrit dropped assignees in 3.8. |
| `pr list -l` | issue label | a Gerrit **vote** predicate, e.g. `Code-Review+2`. |
| `run list/view --json` | a field list | a boolean; it takes no fields. `pr view --json` does take fields. |
| `--json state` | `OPEN`/`CLOSED`/`MERGED` | Gerrit's `NEW`/`MERGED`/`ABANDONED`. |
| `pr review --request-changes` | blocks the PR | votes `Code-Review-1`, which is **advisory**. `Code-Review-2` is the veto. |
| `pr comment --draft` | (n/a) | an unpublished draft **comment** nobody else can see — not a WIP change. |

---

## Workflow 1: Addressing Review Feedback

When tasked with addressing review comments on a change:

1. **Fetch & review comments**:
   ```bash
   ./gh pr view <id> --comments
   ```
2. **Apply code fixes locally**: Edit files and verify with the local
   presubmit gate:
   ```bash
   ./pw presubmit --mode auto --base origin/main
   ```
3. **Upload updated patchset**:
   ```bash
   git commit -a --amend --no-edit
   ./gh pr push
   ```
4. **Reply to threads and mark resolved**:
   ```bash
   ./gh pr comment <id> --path <file> --line <line> -m "Fixed, using pw::Status." --resolved
   ```

---

## Workflow 2: Performing a Code Review

When tasked with reviewing a colleague's change or local commit:

1. **Inspect metadata & diff**:
   ```bash
   ./gh pr view <id>
   ./gh pr diff <id>
   ```
2. **Evaluate against Core Principles**:
  - **Testing**: Are there sufficient unit tests covering edge cases?
  - **Functionality**: Does the change behave correctly without regressions?
  - **Security**: Any buffer overflows, integer issues, or resource leaks?
  - **Style**: Adheres to Pigweed C++17 style (`pw::` types, no dynamic
    allocation, `#pragma once`).
  - **Commit Message**: Conforms to Pigweed style (`module: Imperative message
    under 72 chars`, `Bug: b/...` or `Fixed: b/...`).
3. **Submit review feedback**:
  - Post inline comments for specific lines:
    ```bash
    ./gh pr comment <id> --path <file> --line <line> -m "nit: consider std::string_view" [--draft]
    ```
  - Submit overall review:
    ```bash
    ./gh pr review <id> --approve -m "LGTM! Verified tests pass."
    # or
    ./gh pr review <id> --request-changes -m "Please address comments on error handling."
    ```

---

## Workflow 3: CI Triage & Targeted Retry (`gh run` / `pr checks`)

 1. **Check builder statuses or watch until done**:
    ```bash
    ./gh pr checks <id>                         # quick status snapshot (blocking checks)
    ./gh pr checks <id> --watch --fail-fast     # watch until complete; fail fast on error and dump failure logs
    ./gh pr checks <id> --experimental          # include non-blocking experimental checks
    ```
    *Agent Tip*: When asked to monitor or wait for CI checks, launch
    `./gh pr checks --watch --fail-fast` as a background command and STOP
    calling tools. **DO NOT** poll in a loop with manual `./gh pr checks` calls
    or recurring schedule timers. The tool itself polls Buildbucket and the
    environment will automatically wake you when the process finishes or when a
    failure occurs.

 2. **Inspect failure logs without browser context switching**:
    ```bash
    ./gh run view <id> --log-failed             # summarize all failed blocking builders, failure steps & snippets
    ./gh run view <id> -j <builder>             # inspect step tree with extracted diagnostics (diffs, compilation errors)
    ./gh run view <id> -j <builder> --log-failed# inspect failure logs for specific builder
    ./gh run view <id> -j <builder> --log       # dump full log stream
    ./gh run view <id> --log-failed --json      # parseable diagnostics for agent analysis
    ```
 3. **Targeted Verification (`gh run rerun`)**: If only specific builders
    failed (e.g. `pigweed-lintformat`), trigger **only those builders** without
    URL bashing:
    ```bash
    ./gh run rerun <id> -j <builder>
    # or rerun all failed checks:
    ./gh run rerun <id> --failed
    ```
    *(Note: Under the hood, `./gh` invokes `bb add` using the active project's
    try bucket. If `bb` is not logged in, the user must run `bb auth-login` in
    an interactive terminal).*
 4. **Full Validation**: Once targeted fixes succeed, trigger a full CQ dry
    run:
    ```bash
    ./gh pr edit <id> --cq
    ```

---

## Critical Rules for AI Agents

1. **NEVER use raw `git push`**: Always use `./gh pr push` (or `./gh push`)
   to update existing changes with new patchsets, and `./gh pr create` to create
   new changes.
2. **Use `--draft` for Private Notes**: Intermediate or preparatory review
   comments should use `--draft` so they remain private to you until ready.
3. **Resolve Threads Explicitly**: When fixing an issue raised by a reviewer,
   always reply with `--resolved --path <file> --line <line>`.
4. **Authentication is Automatic**: No manual `curl -sb ~/.gitcookies` or auth
   tokens needed; `./gh` automatically routes through workstation credentials
   (`gob-curl` on Google corp workstations, or `~/.gitcookies` / git
   `http.cookiefile` / `~/.netrc` for external contributors).
5. **Never Ignore Command Failures or Exit Codes**: `./gh` guarantees that
   invalid inputs (unknown `--json` fields, invalid `--profile`, missing
   `--path`/`--line` for `--resolved`, etc.) fail fast with non-zero exit codes.
   Always inspect stderr and address the reported error rather than assuming
   success.
6. **Preserve Commit Trailers**: Prefer `./gh pr edit --body` / `--title` when
   reworking a description; they keep every trailer (`Change-Id:`, `Bug:`,
   `Fixed:`, `Reviewed-on:`, `Co-authored-by:`, cherry-pick provenance)
   automatically. `--message` replaces the whole message and will refuse if
   that would delete trailers. Treat that error as a stop sign: carry the
   listed trailers into your new message rather than reaching for
   `--drop-trailers`.
7. **Link Bugs With Trailers, Never `Fixes #N`**: GitHub's `Fixes #456` does
   **nothing** on Gerrit — the bug is silently never linked or closed. Use a
   `Bug: b/456` or `Fixed: b/456` trailer, or let the tool write it:
   `./gh pr edit <id> --fixed b/456`. `./gh` rejects GitHub issue syntax rather
   than guessing, because a GitHub issue number and a Buganizer ID are
   different number spaces and `b/456` would be an unrelated bug. Look the
   real Buganizer ID up; do not invent one. Verify the link took with
   `./gh pr view <id> --json bug,bugs` rather than assuming.
8. **Submit Changes, Not Patchsets**: Use `./gh pr merge <id>` without patchset
   suffixes, as Gerrit submits whole changes.
9. **Submit via Commit-Queue**: In Gerrit projects with a `Commit-Queue` label
   or LUCI gates, direct submission via bare `./gh pr merge` attempts an
   immediate submit, which fails if CI gates (e.g. `Presubmit-Verified`) or
   review approvals are not yet satisfied. Use `./gh pr merge --auto` to enable
   automatic submission once requirements pass, `./gh pr merge --cq` to delegate
   the build-and-submit cycle directly to the Commit-Queue, or wait for checks
   with `./gh pr checks --watch` before merging.
10. **NEVER Poll CI in an Agent Loop**: When monitoring tryjobs or driving
    changes to ground, run `./gh pr checks --watch --fail-fast` as a background
    command and let it run. DO NOT set polling timers or execute repeated
    manual `./gh pr checks` calls. The background process will notify you
    automatically when checks finish or when a failure is detected, avoiding
    wasteful turn and token consumption.
