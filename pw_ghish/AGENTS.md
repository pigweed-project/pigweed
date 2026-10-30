# AI Agent Guidelines for `pw_ghish`

## Architectural Scope: Gerrit / LUCI Ecosystem

`pw_ghish` (`./gh`) provides GitHub CLI (`gh pr`, `gh run`, `gh issue`, `gh auth`) commands and multi-agent worktree management (`gh wt`, `gh sense`, `gh hook`) for projects across the **Gerrit and LUCI ecosystem** (including Pigweed, Fuchsia, Chromium, Android, and downstream repositories).

* **Decoupled Policy**: Avoid baking project-specific assumptions directly into core commands. High-level commands (`pr view`, `diff`, `comment`, `checks`, `run view`, `run rerun`, `issue`, `push`, `create`) rely on standard Gerrit REST, git push `refs/for/*`, Buildbucket / LogDog / Busytown, and Buganizer APIs.
* **Project Profiles & `.ghish.toml`**: Project-specific policies (such as commit-queue labels, try bucket locations, shortlinks, default Buganizer components, and rerun command syntax) belong strictly in `.ghish.toml` (`ProjectConfig` in [`project_config.go`](project_config.go)) or `ProjectProfile` implementations in [`profile.go`](profile.go), rather than hardcoded in the core CLI handlers.

---

## STRICT ENGINEERING & REVIEW INVARIANTS

Every change to `pw_ghish` (and every review via `/review`, `.agents/agents/go_ghish_reviewer.md`, and `.agents/agents/ghish_security_reviewer.md`) MUST enforce these 16 non-negotiable invariants. Regressing into silent failures, swallowed errors, defensive masking, data loss, policy leakage, context bloat, or hacking without tests is STRICTLY PROHIBITED.

### 1. NO SILENT FAILURES (`RunE` & EXPLICIT `Try*` CONTRACTS)
* **MANDATORY**: Every Cobra subcommand with an action MUST use `RunE: func(cmd *cobra.Command, args []string) error`. NEVER use void-returning `Run`.
* **STRICTLY PROHIBITED**: NEVER return `nil` when an underlying operation, RPC, config parse (`LoadCommandProjectConfig`), or subprocess failed. Returning `nil` exits with status code 0, silently masking failures from users, scripts, and CI runners.
* **THE `return nil` CONTRACT RULE**: `if err != nil { return nil }` is permitted ONLY inside a helper explicitly named `Try*` / ` BestEffort*` (or whose doc comment explicitly declares a best-effort non-fatal contract), AND MUST include both:
  1. An inline `// OK: <why failure is expected and safe to ignore>` comment, AND
  2. A debug log (`slog.Debug("...", "err", err)`) or structured `Warning` attachment so `-v` / `--verbose` surfaces the root cause.

### 2. NEVER MASK INVARIANT VIOLATIONS (`if ptr == nil return 0 / return nil`)
* **STRICTLY PROHIBITED**: When a pointer, dependency, or runner is an internal invariant that should *never* be nil, DO NOT defensively sweep it under the rug with `if ptr == nil return nil` or `return 0`.
* **MANDATORY**: Fail immediately with an explicit error (`fmt.Errorf("internal error: runner is uninitialized")`) or panic if it represents an unrecoverable programmer defect. Never disguise broken code as a valid empty response.

### 3. DRY & COPY-PASTA PREVENTION
* **MANDATORY**: Never duplicate command-config loading, Gerrit error translation, git trailer parsing, or stack validation across subcommands:
  * Always call `LoadCommandProjectConfig(cmd)` instead of re-implementing `GetConfig(cmd)` + `LoadProjectConfig(ctx)`.
  * Always issue Gerrit REST requests through the native `*GerritClient` ([`gerrit.go`](gerrit.go)), inspect status codes with `IsGerritHTTPStatus` (never external Gerrit client libraries, raw Gerrit HTTP requests, or error-string status scraping), and route Gerrit REST errors through `ChangeContext.FormatError` / `FormatGerritError`.
  * Keep pre-push commit-stack validation in a single shared pass (`ValidateCommitStack` in `push_pipeline.go`) rather than running duplicate `git log` / `git rev-list` passes in `create.go` and `push.go`.

### 4. NO DATA LOSS (TRAILERS, DRAFTS, REFLOG & WORKTREES)
* **Commit Trailers & Amending**: Operations that edit commit messages or metadata MUST NEVER wipe out commit bodies or Git trailers (`Change-Id:`, `Bug:`, `Fixed:`, `Reviewed-on:`). Always parse, validate, and preserve trailers using `ExtractTrailers` and `MergeTrailers`, and guard history rewrites with `CheckAmendAllowed` (blocking upstream remote branch tips, multi-parent merge commits, and active `rebase`/`cherry-pick`/`revert`/`bisect` sequencer states, while permitting local non-tip detached `HEAD`s such as `gh pr checkout`).
* **Unpublished Gerrit Drafts (`SetReview`)**: `GerritClient.SetReview` (`ChangeContext.SetReview`) MUST default `Drafts: "KEEP"` unless `--publish` is explicitly passed so voting or commenting never publishes unrelated private draft comments by accident.
* **Inline Draft Updates**: Updating an existing draft in `pr comment --draft` MUST preserve the existing draft's `Side`, character `Range`, and `Unresolved` state unless explicitly overridden by a flag.
* **Multi-Commit Stack & Branch Memory**: Pushing multiple commits without `--stack` must halt immediately. `pr push` must query Gerrit for the change (scoped by `(project, branch, Change-Id)` or change number) to push to its recorded target branch when unambiguously resolved unless `--base` is explicitly set.
* **Worktree Path Confinement**: Any destructive filesystem operation (`os.RemoveAll` in `worktree/`) MUST validate paths through `safe_fs.go` (`ValidateSafePath`, symlink-escape checks, and non-root depth guards) and refuse to evict or park dirty working trees without `--force`.

### 5. ACTIONABLE ERRORS (THE 4 PILLARS)
* **MANDATORY**: Error messages and failure cases MUST NOT be dead-ends. Every user-facing error must provide:
  1. **What Happened**: Clear, unambiguous description of the failure condition (never a bare internal error or cryptic HTTP status).
  2. **Preconditions / Why**: Explain the underlying state or invariant (e.g., change is already merged, working tree is dirty, branch is synced with origin).
  3. **Actionable Remediation**: Concrete, copy-pasteable commands or step-by-step instructions that directly resolve the issue.
  4. **Discovery Fallback**: Inspection commands (e.g., `gh pr list`, `gh pr checks`, `gh pr view --comments`) when the next step depends on external state.
* **Established Error Patterns**:
  * **HTTP 401/403**: Never return raw HTTP 401/403; provide host- and OS-aware authentication remediation (`https://<host>/new-password`, `luci-auth login`, `gcert`).
  * **HTTP 404 / 409**: Distinguish missing change vs. missing patchset; translate 409 conflicts into domain explanations (`change is already merged`, `change is already closed`).
  * **Validation & Enum Guards**: Strictly validate `--json <fields>` and enum flags (`--state`, `--mode`, `--reason`), listing all valid choices on mismatch.

### 6. CONFIG SCHEMA POLICY (THE 3-LAYER RULE)
* **Layer 1 — `.ghish.toml` (Checked-in Repository Policy)**: Shared project settings (`[gerrit]`, `[ci]`, `[issue]`, `[worktree]`, `[oncall]`). Strictly validated via `md.Undecoded()` in `ParseProjectConfigTOML`—unknown sections or keys fail immediately with `file:line`.
* **Layer 2 — `git config ghish.<section>.<key>` (Local User Override)**: Every `.ghish.toml` setting MUST have a 1:1 local override in `applyGitConfigOverrides` in [`project_config.go`](project_config.go), plus a minimal set of user/machine-only Git config keys (`ghish.profile`, `ghish.authmode`, `ghish.blockrawpush`).
* **Layer 3 — `ProjectProfile` (`profile.go`)**: Built-in fallback defaults when no `.ghish.toml` is present.
* **STRICTLY PROHIBITED**: NEVER add ad-hoc `git.ConfigGet(ctx, "ghish.<foo>")` calls inside individual command files (`cmd_issue.go`, `push.go`, etc.). All configuration keys MUST be declared on `ProjectConfig` in [`project_config.go`](project_config.go) and documented in [`project_setup.rst`](project_setup.rst) (enforced by `invariants_ast_test.go` and `doc_sync_test.go`).

### 7. ZERO PIGWEED / FUCHSIA POLICY LEAKAGE & MECHANISM VS. POLICY
* **Zero Hardcoded Project Policy Outside `profile.go`**: Core command handlers (`push_pipeline.go`, `cmd_issue.go`, `luci.go`, `sense.go`, `worktree/`) MUST NOT hardcode Pigweed or Fuchsia hostnames, shortlinks (`pwbug.dev`), directory paths (`~/wrk/pigweed`), or GCP project IDs (`pigweed-gce`). Always read from `ProjectConfig` or `ProjectProfile`.
* **Mechanism vs. Policy Boundary**: `pw_ghish` is a fast Go transport and workflow engine (`<100ms`), NOT a general-purpose linter or build runner:
  * **Belongs in `pw_ghish` (Mechanism)**: Checks that require Gerrit/Git transport state or protect `refs/for/*` integrity (`Change-Id` presence and per-stack uniqueness, `Fixes #123` GitHub syntax trap, multi-commit `--stack` guard, `submodule_policy`, `forbid_topics`).
  * **Does NOT belong hardcoded in `pw_ghish` (Repo Policy)**: Project-specific commit message subject-length limits (50/72 chars), module-prefix rules (`pw_foo:`), or language linters—unless opt-in and driven by declarative `[commit]` schema in `.ghish.toml`.

### 8. 1P vs. 3P & WORKSTATION PORTABILITY (CLOUDTOP / MAC / CHROMEBOOK)
* **3P / External Open-Source Safety**: Never assume `@google.com` identity, `sso://` remotes, `gob-curl`, `sso_client`, or `/google/data/ro/...` paths exist unless `ResolveAuthMode` confirms `googler` mode AND `LookPathFn` / `os.Stat` confirms the binary exists on disk.
* **Workstation Matrix**:
  * **1P Linux Cloudtop / gLinux**: Full `gob-curl`, `git-remote-rpc` (`rpc://` 7-day LOAS2), `sso_client`, and `/google/data/ro/projects/android/fetch_artifact` support.
  * **1P Corp macOS Laptop**: Has `gob-curl` and `gcert`, but typically lacks `git-remote-rpc` and `/google/data/ro/...`. Always check `LookPathFn("git-remote-rpc")` before attempting `rpc://` rewrites, and check `LookPathFn("gcert")` / `LookPathFn("gob-curl")` before prescribing them in remediation messages.
  * **1P Chromebook (Crostini) & 3P Workstations**: Treat as standard Linux without corp daemons unless binaries are detected on `PATH`; guide users to `luci-auth login` and `.gitcookies`.

### 9. SECURITY & NTK CONFIDENTIALITY
* **Credential Safety**: NEVER pass OAuth2 bearer tokens or secrets on subprocess command lines (e.g. `git -c http.extraHeader="Authorization: Bearer ..."`), because CLI arguments are world-readable in `ps aux` / `/proc/<pid>/cmdline` and leak into verbose/error logs. Use `GIT_CONFIG_COUNT` / `GIT_CONFIG_KEY_0` / `GIT_CONFIG_VALUE_0` in `cmd.Env` or temporary `0600` credential files, and redact tokens in all debug/error output.
* **Need-To-Know (NTK) Boundary**: Upstream `pw_ghish` source, tests, docs, and skills are **PUBLIC**. Never include internal partner codenames, internal builder names, or internal `sso://` URLs in code, comments, or test fixtures.

### 10. GERRIT, LUCI & BUGANIZER INFRA GOOD CITIZENSHIP
* **Bounded Memory (`io.LimitReader`)**: Every `io.ReadAll(resp.Body)` on Gerrit, Buildbucket, LogDog, Busytown, or Buganizer HTTP responses MUST be wrapped in `io.LimitReader(resp.Body, maxBytes)` to prevent OOMs on multi-hundred-megabyte logs or diffs.
* **Bounded Concurrency**: Every parallel fan-out over changes, worktrees, or builds (`errgroup` / `sync.WaitGroup`) MUST bound concurrency with a semaphore (e.g. max 4–8 in-flight RPCs).
* **Batch Gerrit Queries**: Never issue N+1 sequential `GetChangeDetail` RPCs in loops (`wt list`, `pr status`); batch changes into a single `QueryChanges` call (`q=change:1+OR+change:2`).
* **CQ & Rerun Storm Guards**: Guard `pr create/push --stack --cq` and `run rerun --failed` against accidentally scheduling hundreds of duplicate tryjobs, and add random jitter to `pr checks --watch` polling intervals.

### 11. MOCK TESTS vs. GERRIT REALITY
* **Verify Query Options (`o=`)**: Mock Gerrit tests in `*_test.go` must assert that production code requested the required Gerrit `o=` parameters (`CURRENT_REVISION`, `CURRENT_COMMIT`, `DETAILED_LABELS`, `DETAILED_ACCOUNTS`, `MESSAGES`, `SUBMIT_REQUIREMENTS`) before returning fields that Gerrit omits by default (`labels`, `revisions`, `submittable`).
* **Shared Test-Harness Helpers (`test_utils_test.go`, `kind_fixture_test.go`)**: Prefer the `"Declarative Gerrit Fixtures"` and `"Declarative LUCI / Buildbucket Fixtures"` sections of `test_utils_test.go`, `DecodeRequest`, `NewKindSmokeFixture`, and `MockGitRunner` builders instead of duplicating hand-built `ChangeInfo` maps (use `OnDefaultChange` and the `With*` options), hand-written `SearchBuilds` handlers (use `OnSearchBuilds*`), `json.Unmarshal(LastRequest().Body)` (use `DecodeRequest`), or copy-pasted per-flag test functions (use table-driven subtests). Escape hatches: use `server.On` when a test needs per-request behavior (ordering, concurrency, or injected failures) or a real `httptest.NewServer` when exercising client construction/transport fallback directly. Note that `OnDefaultChange` returns `labels` and `revisions` regardless of what `o=` was requested, so tests relying on those fields still must assert the `o=` query options.

### 12. GH CLI FLAG COMPATIBILITY & `//ghish:flag` DIRECTIVES
* **Mandatory Code Directive**: Every flag registration (`Flags().*Var*`, `PersistentFlags().*Var*`) in `pw_ghish` MUST be preceded by a structured `//ghish:flag` directive comment (or registered in `flag_policy_test.go`), verified by `TestFlagPolicy`:
  * `//ghish:flag mode=upstream gh="<cmd> --<flag>"` — Same flag name, shorthand, and semantics as upstream GitHub CLI (`gh`).
  * `//ghish:flag mode=extension reason="<why>"` — Gerrit/LUCI/Buganizer-specific flag with no upstream `gh` equivalent. Visible extension flags MUST include `(ghish-only)` in their usage string and MUST NOT bind a single-letter shorthand that collides with upstream `gh` (`ghShorthandCollisions`).
  * `//ghish:flag mode=divergent gh="<cmd> --<flag>" doc="ghDivergenceHelp"` — Upstream `gh` flag whose underlying Gerrit/Buganizer model differs; MUST be documented in `ghDivergenceHelp` in [`root.go`](root.go) and in the command's `.rst` comparison table.
  * `//ghish:flag mode=unsupported-guard gh="<cmd> --<flag>"` — Upstream `gh` flag intercepted to return an actionable error explaining the Gerrit alternative.
* See [`flag_compatibility.rst`](flag_compatibility.rst) for the complete compatibility policy.

### 13. MODERN GO STYLE
* Follow the Pigweed Go Style Guide ([`docs/sphinx/style/go.rst`](../docs/sphinx/style/go.rst)): use Go 1.27+ standard library generics (`slices`, `maps`, `cmp`), `%w` error wrapping with `errors.Is`/`errors.As`, command option structs instead of new package-level flag globals, and `t.Context()` / `t.TempDir()` / `t.Setenv()` / `cmp.Diff` in tests.

### 14. DOCUMENTATION & SKILL SYNCHRONIZATION
* **Programmatic Doc Sync (`doc_sync_test.go`)**: Every non-hidden Cobra subcommand and flag MUST be documented in its `pw_ghish/<family>.rst` reference page, every `.ghish.toml` `[section]` and `key` MUST be documented in [`project_setup.rst`](project_setup.rst), and every `./gh` example in [`.agents/skills/ghish/SKILL.md`](../.agents/skills/ghish/SKILL.md) MUST use valid subcommands and flags.
* **Skill Conciseness Budget**: Keep [`.agents/skills/ghish/SKILL.md`](../.agents/skills/ghish/SKILL.md) high-density and under **300 lines**—put exhaustive tutorials and prose in `pw_ghish/*.rst`, not in the always-loaded skill file.

### 15. UI DESIGN LANGUAGE, RECEIPTS & CONTEXT BUDGETING
* **Answer-First & Progressive Disclosure (`--all`)**:
  * Never flood stdout with unbounded lists of passing CI checks (`>5` passing checks must collapse unless `--all` is passed, ordering failing/running checks first and clustering identical failure diagnostics via `ClusterFailedBuilds`), resolved review threads (`FormatCommentForestWithOptions`), automated bot commit dumps (`summarizeBotIssueComment`), or multi-thousand-line diffs (`defaultDiffMaxLines = 400`, folding lockfiles/generated files unless `--all` or `--path` is passed).
  * Every folded section MUST print a concise 1-line summary with the exact `--all` or `--path` command to unfold it.
* **Canonical Glyphs & TTY/Agent Color Discipline (`ui.go`)**:
  * Use canonical BMP glyph constants (`GlyphPass = "✓"`, `GlyphFail = "✗"`, `GlyphWarn = "⚠"`, `GlyphRunning = "●"`, `GlyphPending = "○"`, `GlyphDot = "·"`, `GlyphArrow = "→"`). Do not introduce decorative ASCII `===` walls or ad-hoc emojis in core commands.
  * Never hardcode raw ANSI escape sequences (`\x1b[` or `\033[`) outside `pw_ghish/ui.go` (enforced by `TestInvariants_NoRawANSIEscapesOutsideUIGo`). Always use `Style*` and `PadRightStyled(w, ...)` so non-TTY pipes, `NO_COLOR`, `TERM=dumb`, and coding-agent environments (`IsAgentEnv()`) receive pure, column-aligned plaintext.
* **Structured Mutation Receipts (`PrintReceipt` / `FormatReceipt`)**:
  * Every mutating command (annotated `ghish:kind: mutation`) MUST emit a structured `✓ <Summary>` receipt with contextual `Details`, `URL`, and `Next:` step hints.
* **Declared Command Kinds & Smoke Harness (`command_kind.go`, `kind_contract_test.go`)**:
  * Follow the **New command checklist** in [`developing.rst`](developing.rst): declare `Annotations: map[string]string{KindAnnotation: Kind<Mutation|View|List|Stream|Meta>}` inline on every runnable `cobra.Command` (and `OutputAnnotation: OutputRaw` if stdout is a raw machine artifact like `pr diff`), and add a `kindSmokeArgs` entry in [`kind_contract_test.go`](kind_contract_test.go) for every runnable `mutation`/`view`/`list` command. Do NOT add `kindSmokeSkip`, `kindSmokeContractGaps`, or `kindSmokeDuplicateSubprocesses` entries to bypass failures on new code.
* **JSON Output Contract (`--json`)**:
  * Under `--json`, stdout MUST be a single JSON value (no receipts, progress, warnings, ANSI escapes, or `Verdict:`/`Next:` lines), empty slices and maps MUST marshal as `[]` and `{}` rather than `null`, enum-like fields MUST contain bare ASCII identifiers (never terminal glyphs), and gh-compatible `--json <fields>` flags MUST reject unknown fields with `UnknownJSONFieldsError`. See [`developing.rst`](developing.rst) ("JSON output contract").

### 16. PERFORMANCE BUDGETS (COUNTS, NOT CLOCKS)
* **Counts, Never Wall-Clock**: Perf regressions fail presubmit via deterministic counts only: commands constructed (≈ subprocesses spawned) and HTTP requests issued. NEVER add a presubmit assertion on elapsed time; wall-clock numbers live only in the informational `live_bench_test.go` (`//go:build live`).
* **Single Subprocess Choke Point**: Construct every external process via `ExecCommand` / `ExecCommandContext` in [`procexec.go`](procexec.go) (`pw_ghish.ExecCommand` from `worktree/`). Direct `exec.Command` / `exec.CommandContext`, `exec.Cmd` literals, `os.StartProcess`, and `syscall.Exec` / `syscall.ForkExec` are prohibited elsewhere.
* **Staying Within Budgets**: Keep commands within their kind's default subprocess and RPC count budgets (`kind_contract_test.go`) and exact hot-command pins ([`perfbudget_test.go`](perfbudget_test.go), [`worktree/perfbudget_test.go`](worktree/perfbudget_test.go)). When a budget test fails, prefer removing the extra work (memoize on `Config`/`GitClient`, batch queries, parallelize); if an increase is intentional, update the pin or `ghish:budget-exempt` reason in the same CL and justify it in the CL description.

---

## Testing & Verification Architecture

1. **Hermetic Unit & Policy Tests (Mandatory on Every Change)**
   ```bash
   bazelisk test --noshow_progress --noshow_loading_progress //pw_ghish/...
   ```
   Includes unit tests, AST invariant enforcement (`invariants_ast_test.go`), flag compatibility verification (`flag_policy_test.go`), and CLI/config/skill documentation sync (`doc_sync_test.go`).

2. **Live Integration Tests (FTE SWE Invoked)**
   ```bash
   go test -v -tags=live ./pw_ghish -run TestLive
   ```
   Validates live RPCs against `pigweed-review.googlesource.com`, Buildbucket pRPC, LogDog log streams, and ephemeral draft comment lifecycles ([`live_test.go`](live_test.go)).

3. **Agent Behavioral Evaluation**
   * Evaluation rubric: [`agent_eval.rst`](agent_eval.rst)
   * Prompt scenarios & triggers: [`.agents/skills/ghish/TEST.md`](../.agents/skills/ghish/TEST.md)

---

## Primary Pointers

* **User & API Documentation**: [`docs.rst`](docs.rst), [`subcommands.rst`](subcommands.rst), [`project_setup.rst`](project_setup.rst), [`flag_compatibility.rst`](flag_compatibility.rst)
* **Pigweed Go Style Guide**: [`../docs/sphinx/style/go.rst`](../docs/sphinx/style/go.rst)
* **Agent Skill & Reviewer Definitions**: [`.agents/skills/ghish/SKILL.md`](../.agents/skills/ghish/SKILL.md), [`.agents/agents/go_ghish_reviewer.md`](../.agents/agents/go_ghish_reviewer.md), [`.agents/agents/ghish_security_reviewer.md`](../.agents/agents/ghish_security_reviewer.md)
* **Live Test Suite**: [`live_test.go`](live_test.go)
