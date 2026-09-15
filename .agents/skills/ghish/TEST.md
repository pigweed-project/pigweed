# Testing `ghish` Skill

This document provides instructions on how to verify that the `ghish` (`./gh`) skill functions correctly and interacts reliably with live Gerrit and Buildbucket instances.

## Setup & Prerequisites

1. Run from the repository root:
   ```bash
   cd /usr/local/google/home/keir/wrk/pw-ghish  # or your active pigweed checkout
   ```
2. Verify the `./gh` wrapper script is executable and cached:
   ```bash
   ./gh --help
   ```
   *Verify:* The first run compiles and installs `gh-ish` to `out/gh/gh-ish`; subsequent runs execute immediately without Bazel startup overhead.

---

## Live Instance Test Cases

These test cases run against live Pigweed Gerrit change [CL 472267](https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/472267) or any active change.

### 1. Unified Diff Inspection (`pr diff`)

```bash
./gh pr diff 472267 | head -n 25
```

**Verify:**
- Fetches and displays the unified git patch from Gerrit without requiring raw `curl` commands.
- Includes commit headers, modified files (`MODULE.bazel`, `BUILD.bazel`, etc.), and diff hunks.

### 2. Change Metadata & JSON Output (`pr view`)

```bash
./gh pr view 472267 --json number,title,status,author
```

**Verify:**
- Returns valid JSON with change number `472267`, title `pw_ghish: Gerrit utility for AI`, status `NEW`, and author info.
- Authenticates cleanly without authentication errors.

### 3. Threaded Review Comments (`pr view --comments`)

```bash
./gh pr view 472267 --comments
```

**Verify:**
- Outputs the change overview followed by all review comment threads.
- Accurately reports file path, line number, author, resolution status (`[RESOLVED]` or `[UNRESOLVED]`), and nested reply indentation.

### 4. LUCI Buildbucket Checks Query (`pr checks`)

```bash
./gh pr checks 472267
```

**Verify:**
- Connects to LUCI Buildbucket via pRPC (`cr-buildbucket.appspot.com`).
- Displays current patchset builders (e.g. `static-checks-pigweed`, `docs-builder-newpatchset`).
- Shows status symbols (`✓`, `✕`, `*`, `?`), run durations, and clickable build URLs.

### 5. Inspecting Check Failure Logs (`pr checks log`)

```bash
# View failure summaries and log snippets for failing checks
./gh pr checks log 467905/22
# Or structured JSON:
./gh pr checks log 467905/22 --json
```

**Verify:**
- Identifies failed builders (e.g. `pigweed-lintformat`) and failing step (`python_format|failure summary`).
- Fetches and displays the raw LogDog log snippet showing the exact diff/error.
- Provides direct log URL and status without manual browser navigation.

### 6. Rerunning CI Checks Without URL Bashing (`pr checks rerun`)

```bash
# Preview targeted rerun of a single builder:
./gh pr checks rerun 472267 pigweed-mac-arm-vscode --dry-run
# Preview rerun of all failed builders:
./gh pr checks rerun 467905/22 --failed --dry-run
```

**Verify:**
- Constructs the project-appropriate `bb add` command (e.g. `bb add -cl https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/472267/3 pigweed/pigweed.try/pigweed-mac-arm-vscode`).
- Requires zero URL bashing from developers or AI agents.
- Reruns via the system `bb` CLI if `--dry-run` is omitted.

### 7. Listing Open Changes (`pr list`)

```bash
./gh pr list --limit 5
```

**Verify:**
- Lists the 5 most recent open changes on `pigweed-review.googlesource.com`.
- Displays columns: number, title, branch, and status.

### 8. Safe Draft Commenting (`pr comment --draft`)

To verify comment creation without spamming reviewers or publishing publicly:

```bash
./gh pr comment 472267 --path pw_ghish/docs.rst --line 10 -m "Verification test draft comment from ghish test suite." --draft
```

**Verify:**
- Command succeeds and reports draft comment created.
- The comment is saved as an unpublished draft in Gerrit (visible only to the authenticated author in the Gerrit web UI).

### 9. Triggering Full CQ Dry Run on Existing Change (`pr edit --add-label`)

```bash
./gh pr edit 472267 --add-label Commit-Queue=1
```

**Verify:**
- Sends a `SetReview` request with `{"labels": {"Commit-Queue": 1}}` via Gerrit REST API.
- Sets the `Commit-Queue` label to `+1` without needing `curl -sb ~/.gitcookies`.

### 10. Strict Input Validation & Fail-Fast Error Reporting

Verify that invalid user inputs fail fast with exit code 1 and actionable stderr diagnostics:

```bash
# 1. Unknown --json field:
./gh pr view 472267 --json invalid_field
# Verify: exits with code 1; stderr shows "unknown JSON field(s): [invalid_field]"

# 2. Unknown --profile:
./gh pr list --profile non_existent
# Verify: exits with code 1; stderr shows "unknown profile: non_existent"

# 3. --resolved without --path and --line:
./gh pr comment 472267 -m "Resolved without thread" --resolved
# Verify: exits with code 1; stderr explains --resolved requires both --path and --line

# 4. pr merge on explicit patchset:
./gh pr merge 472267/1
# Verify: exits with code 1; stderr explains patchsets cannot be merged individually
```

---

## Agent Behavior & Prompt Triggers

Validate that AI agents correctly select and invoke `./gh` when presented with common review prompts:

### Prompt A: Review a Gerrit CL
> *"Can you review CL 472267? Check if the change follows Pigweed style and has adequate tests."*

**Expected Agent Behavior:**
- Agent loads `ghish/SKILL.md`.
- Runs `./gh pr diff 472267` and `./gh pr view 472267`.
- Evaluates code against Pigweed principles (testing, style, safety).
- Posts review via `./gh pr review 472267 --approve -m "..."` or adds comments via `./gh pr comment 472267 ...`.

### Prompt B: Address Review Feedback
> *"Check the review comments on CL 472267 and reply to the reviewer on comment.go line 62."*

**Expected Agent Behavior:**
- Agent runs `./gh pr view 472267 --comments`.
- Identifies the unresolved comment thread on `pw_ghish/comment.go:62`.
- After verifying/fixing code, runs `./gh pr comment 472267 --path pw_ghish/comment.go --line 62 -m "Fixed" --resolved`.

### Prompt C: Query Status & Checks
> *"What is the status and CI check results for CL 472267?"*

**Expected Agent Behavior:**
- Runs `./gh pr view 472267` and `./gh pr checks 472267`.
- Summarizes the patchset, review labels, and passing/failing builders with links.

### Prompt D: Uploading with Presubmit Dry Run
> *"Upload my commit to Gerrit and start a dry run."*

**Expected Agent Behavior:**
- Runs `./gh pr create -q 1` (setting `Commit-Queue+1`).
- Confirms new change/patchset URL from output.

### Prompt E: Diagnose & Rerun Failing Checks
> *"Pigweed CL 467905 is failing checks, can you figure it out and rerun the failed checks?"*

**Expected Agent Behavior:**
- Runs `./gh pr checks 467905` to identify failed builders.
- Runs `./gh pr checks log 467905` to extract the exact failure summary and log snippet without browser context switching.
- Diagnoses the root cause from the error snippet.
- Runs `./gh pr checks rerun 467905 --failed` (or targeted builder name) without manual URL bashing.

### Prompt F: Error Diagnostics & Input Correction
> *"Inspect CL 472267 with --json commit_hash,author."*

**Expected Agent Behavior:**
- Runs `./gh pr view 472267 --json commit_hash,author`.
- Observes non-zero exit code (1) and reads stderr diagnostic: `unknown JSON field(s): [commit_hash]. Valid fields are: ...`.
- Corrects input to valid fields (e.g. `./gh pr view 472267 --json number,title,author`) rather than ignoring the failure or hallucinating a response.

