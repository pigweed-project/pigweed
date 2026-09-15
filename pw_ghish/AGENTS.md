# AI Agent Guidelines for `pw_ghish`

## Architectural Scope: Gerrit / LUCI Ecosystem

`pw_ghish` (`./gh`) provides GitHub CLI (`gh pr`) ergonomics for projects across the **Gerrit and LUCI ecosystem** (including Pigweed, Fuchsia, Chromium, and others).

* **Decoupled Policy**: Avoid baking project-specific assumptions directly into core commands. High-level commands (`pr view`, `diff`, `comment`, `checks`, `checks log`, `checks rerun`, `push`, `create`) rely on standard Gerrit REST, git push `refs/for/*`, and Buildbucket / LogDog APIs.
* **Project Profiles**: Project-specific policies (such as auto-submit labels, try bucket locations, and rerun command syntax) belong strictly in `ProjectProfile` implementations in [`profile.go`](file:///usr/local/google/home/keir/wrk/pw-ghish/pw_ghish/profile.go), rather than hardcoded in the core CLI handlers.

---

## STRICT ENGINEERING DISCIPLINE: ZERO SILENT FAILURES, NO DEFENSIVE MASKING, NO DRIFT

AI agents modifying `pw_ghish` MUST adhere strictly to these non-negotiable engineering invariants. Regressing into silent failures, swallowed errors, defensive masking, or hacking without tests is STRICTLY PROHIBITED.

### 1. ABSOLUTELY ZERO SILENT FAILURES (`RunE` EVERYWHERE)
* **MANDATORY**: Every Cobra subcommand MUST use `RunE: func(cmd *cobra.Command, args []string) error`.
* **STRICTLY PROHIBITED**: NEVER use void-returning `Run`. NEVER return `nil` when an underlying operation, RPC, or subprocess failed.
* **WHY**: Returning `nil` or using `Run` exits with status code 0, silently masking failures from users, scripts, and CI runners.

### 2. NEVER MASK INVARIANT VIOLATIONS (`if ptr == nil return 0 / return nil`)
* **STRICTLY PROHIBITED**: When a pointer, dependency, or runner is an internal invariant that should *never* be nil, DO NOT defensively sweep it under the rug with `if ptr == nil return nil` or `return 0`.
* **MANDATORY**: Fail immediately with an explicit error (`fmt.Errorf("internal error: runner is uninitialized")`) or panic if it represents an unrecoverable programmer defect. Never disguise broken code as a valid empty response.

### 3. MANDATORY TEST-DRIVEN DEVELOPMENT (TDD) — NEVER HACK WITHOUT TESTS
* **MANDATORY**: You MUST follow strict Test-Driven Development (TDD). Every new feature, bug fix, flag, or error path MUST have corresponding unit tests in `*_test.go` written alongside or before the implementation.
* **STRICTLY PROHIBITED**: Never hack away at code without writing tests and declare victory.
* **MANDATORY**: Test both happy paths and negative/error cases. Verify that failure conditions produce non-zero exit codes and actionable stderr diagnostics.

### 4. FAIL FAST ON CONFIGURATION & PRECONDITIONS (NO SILENT FALLBACKS)
* **MANDATORY**: If a user or environment explicitly specifies a configuration option (such as `--profile <name>` or `GH_ISH_AUTH_METHOD=<method>`), fail immediately with a descriptive error if the option is invalid, unknown, or if its prerequisites are missing.
* **STRICTLY PROHIBITED**: NEVER silently fall back to default profiles or anonymous access when an explicit configuration was requested.
* **MANDATORY**: Strictly validate `--json <fields>` against the schema and reject unknown fields with an error. Never silently omit unknown fields or return empty `{}` objects.

### 5. OUTPUT STREAM DISCIPLINE
* **MANDATORY**: All primary command data/output must be written to `cmd.OutOrStdout()` (or `runner.Out`).
* **MANDATORY**: All error messages, warnings, and diagnostic progress must be written to `cmd.ErrOrStderr()` (or `runner.Err`).
* **STRICTLY PROHIBITED**: NEVER call `fmt.Print`, `fmt.Println`, or write directly to `os.Stdout`/`os.Stderr`. Direct terminal writes hijack output streams and break hermetic test buffer captures.

### 6. NO DIRECT `os.Exit(1)` IN SUBCOMMANDS OR LIBRARIES
* **MANDATORY**: `os.Exit(1)` is reserved exclusively for `cmd/main.go`. All subcommands, runners, and library functions MUST propagate `error` up the call stack to allow `defer` cleanups and hermetic unit testing.

### 7. ZERO DATA DESTRUCTION (COMMIT TRAILERS & BRANCH MEMORY)
* **MANDATORY**: Operations that edit commit messages or metadata (such as `pr edit`) MUST NEVER wipe out commit bodies or Git trailers (`Change-Id:`, `Bug:`, `Fixed:`, `Reviewed-on:`). Always parse, validate, and preserve trailers using `ExtractTrailers` and `MergeTrailers`. Reject empty replacement messages before invoking Git.
* **MANDATORY**: Multi-commit stack safety: pushing multiple commits without `--stack` must halt immediately to prevent accidental multi-CL creation on the remote server.
* **MANDATORY**: Branch memory: when updating existing changes (`pr push`), target the branch recorded on Gerrit unless explicitly overridden by `--base`.

### 8. ACTIONABLE ERRORS & QUALITY REPORTING (THE 4 PILLARS)
* **MANDATORY**: Error messages and failure cases MUST NOT be dead-ends. Every error must provide useful context and debug crumbs to make it easy for human SWEs and autonomous AI agents to move forward without confusion.
* **THE 4 PILLARS OF ACTIONABLE ERRORS**:
  1. **What Happened**: Clear, unambiguous description of the failure condition (never a bare internal error or cryptic HTTP status).
  2. **Preconditions / Why**: Explain the underlying state or invariant (e.g., change is already merged, working tree is dirty, branch is synced with origin).
  3. **Actionable Remediation**: Provide concrete, copy-pasteable commands or step-by-step instructions that directly resolve the issue.
  4. **Discovery Fallback**: Point the user/agent to inspection commands (e.g., `gh pr list`, `gh pr checks`, `gh pr view --comments`) when the next step depends on external state.
* **ESTABLISHED ERROR PATTERNS**:
  * **Centralized REST Errors (`FormatGerritError` / `ChangeContext.FormatError`)**:
    * **HTTP 401/403**: Never return raw HTTP 401/403. Provide host-specific authentication remediation (`https://<host>/new-password`, `export GERRIT_TOKEN=`, `gcert`).
    * **HTTP 404**: Distinguish missing change vs missing patchset, and suggest `gh pr list` or `gh pr view <id>`.
    * **HTTP 409**: Translate raw conflict responses into domain explanations (`change is already merged and cannot be modified`, `change is already closed`).
  * **Subprocess Git & Gerrit Interception**:
    * **Missing Change-Id on push**: Parse `stderr`, emit curl commit-msg hook installation command, `git commit --amend --no-edit`, and retry command.
    * **Cherry-pick conflicts**: Emit 4-step conflict resolution guide (`git status`, edit, `git add`, `git cherry-pick --continue` / `--abort`).
    * **Dirty worktree checkout**: Provide `git stash` instructions.
    * **Detached HEAD checkout**: Emit explicit warning with branch creation command (`git checkout -b <branch> FETCH_HEAD`).
  * **Validation & Enum Guards**:
    * **Missing required action flags** (e.g. bare `pr review` or empty `pr edit`): Reject immediately with concrete, high-frequency command examples.
    * **Invalid parameter values / enums** (e.g. `--state bogus` or invalid label format): Enumerate all valid choices (`'open', 'closed', 'merged', 'all'`) and show correct syntax.
    * **Unmatched query targets** (e.g. `checks log <builder>` or `checks rerun`): Query and list the available or failed builders present on the change rather than failing with an empty diagnostic.

---

## Testing & Verification Architecture

Testing `pw_ghish` spans hermetic unit tests, live corp-authenticated infrastructure, and GenAI agent behavioral compliance:

1. **Hermetic Unit Tests**
   * Run via Bazel or Go:
     ```bash
     bazelisk test //pw_ghish/...
     # or
     go test ./pw_ghish
     ```
   * Fast, hermetic tests for flag parsing, rendering, Change-Id repair, and REST mock handling.

2. **Live Integration Tests (FTE SWE Invoked)**
   * Run via Go with the `live` tag:
     ```bash
     go test -v -tags=live ./pw_ghish -run TestLive
     ```
   * Must be executed by an FTE SWE with active corp credentials (`gob-curl` / `.gitcookies` / SSO).
   * Validates live RPCs against `pigweed-review.googlesource.com`, Buildbucket pRPC, LogDog log streams, and ephemeral draft comment lifecycles.
    * Implementation: [`live_test.go`](live_test.go).

3. **Agent Behavioral Evaluation (Human & Prompt Validation)**
   * Tests how coding agents interact with `gh-ish` in real development sessions.
   * Verifies agents do **not** regress into anti-patterns (no manual `curl`, no `gob-curl`, no Python scraping scripts, no raw `git push`).
   * Verifies agents respect safety guards, use `./gh pr push` to iterate on patchsets, and use `./gh pr create` to create new changes.
   * Evaluation rubric: [`agent_eval.rst`](agent_eval.rst).
   * Prompt scenarios & triggers: [`.agents/skills/ghish/TEST.md`](../.agents/skills/ghish/TEST.md).

---

## Primary Pointers

* **User & API Documentation**: [`docs.rst`](docs.rst)
* **Agent Skill Definition**: [`.agents/skills/ghish/SKILL.md`](../.agents/skills/ghish/SKILL.md)
* **Live Test Suite**: [`live_test.go`](live_test.go)
* **Agent Evaluation Rubric**: [`agent_eval.rst`](agent_eval.rst)

