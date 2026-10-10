---
name: review
description: >-
  High-signal, embedded-aware Pigweed code review skill with strict comment
  calibration. Dispatches the enabled language reviewer subagents in parallel
  (one patch each), merges their findings into one calibrated verdict, and
  falls back to an inline audit for files with no enabled reviewer. Supports
  both Self-Review Gate mode (pre-upload verification for /crank) and Gerrit
  CL Reviewer mode (staging calibrated --draft comments via ./gh pr comment).
  Do not load for a quick LGTM or a single-file look, and not for
  /respond-style comment handling (see .agents/skills/respond/SKILL.md).
disable-model-invocation: true
---

# Pigweed Calibrated Code Review (`/review`)

`/review` performs a rigorous, high-signal engineering review tailored to
Pigweed's embedded C++, Rust (`pw_kernel`), Python, and dual-build (`Bazel` +
`GN`) architecture.

Unlike generic review prompts that eitherrubber-stamp changes or flood CLs with
low-value stylistic noise, `/review` enforces **strict comment calibration**,
**empirical verification**, and **embedded systems invariants**.

---

## Operating Modes

Determine the mode from how `/review` is invoked:

1. **Mode A — Self-Review Gate (Local Pre-Upload / `/crank` Subagent)**:
   - *Trigger*: Bare `/review` on local uncommitted/unpushed changes, or invoked
     by `/crank` before `./gh pr create` / `./gh pr ready`.
   - *Behavior*: Inspects `git diff origin/main...HEAD` (or working tree diff),
     runs targeted verification, and emits a structured verdict (`PASS` or
     `NEEDS_FIXES`) with actionable file/line findings so the author agent fixes
     every `[bug]`, `[embedded/size]`, `[api]`, and `[test]` issue **before** a
     human reviewer ever sees the CL.
2. **Mode B — Gerrit CL Reviewer (`/review <cl>` or Reviewer Workflow)**:
   - *Trigger*: `/review <cl_id_or_url>` (or bare `/review` when reviewing a
     teammate's CL).
   - *Behavior*: Fetches CL context via `./gh pr view <cl> --comments` and
     `./gh pr diff <cl>`, evaluates the patch, and stages calibrated inline
     comments using `./gh pr comment <cl> --path <file> --line <line> -m "<msg>" --draft`.
     **Never** publishes comments live without `--draft` unless explicitly
     instructed by the user.

---

## Comment Calibration & Anti-Slop Rules

Every comment or finding MUST pass these calibration filters:

1. **Zero Praise / Filler Comments**: Never post inline comments that merely say
   *"Nice refactor!"*, *"Looks good"*, or restate what the code does.
2. **Verify Before Accusing**: Before claiming a symbol is undefined, a header
   is missing, or a lock is unlocked, verify with `code_search`, `view_file`, or
   `bazelisk test`. Zero hallucinated compiler errors.
3. **Mandatory Severity Prefix**: Every inline finding must begin with one of:
   - `[bug]` — Correctness, memory safety, race condition, lifetime/dangling
     reference, or unhandled error status.
   - `[embedded/size]` — Dynamic heap allocation in a no-alloc module, unbounded
     recursion/stack frame (>256B buffer on stack), ISR-unsafe blocking call,
     or unnecessary template/code-size bloat.
   - `[api]` — Public header C++17 incompatibility, missing `#pragma once`,
     symbol outside `pw` namespace, unprefixed macro, or Doxygen mismatch.
   - `[build]` — Missing `BUILD.bazel` / `BUILD.gn` / `CMakeLists.txt` parity,
     missing visibility/deps, or broken negative compilation test (`PW_NC_TEST`).
   - `[test]` — Missing unit test for new behavior/bugfix, or use of raw
     `EXPECT_EQ(status, pw::OkStatus())` instead of `PW_TEST_EXPECT_OK`.
   - `nit:` — Optional, minor readability or naming polish.
4. **Nit Budget (Max 3)**: Emit at most **3 `nit:` comments** across the entire
   CL. If there are blocking `[bug]` or `[embedded/size]` issues, prioritize
   those and drop minor nits entirely.
5. **Respect Formatter Authority**: Never leave inline comments about whitespace,
   line wrapping, or include sorting that `./pw format` handles automatically.
6. **Changed Lines Only**: Review the diff, not the file. Never stage an inline
   comment on unchanged code; put out-of-diff observations, if any, in the
   summary (Mode A) or in one change-level draft (Mode B).
7. **Cite the Rule**: When a finding rests on a Pigweed rule, link it as a
   pigweed.dev URL, e.g.
   `https://pigweed.dev/code_reviews.html#docs-code-reviews-checklist-<aspect>`
   or `https://pigweed.dev/style/<page>.html#<anchor>`.

---

## Step-by-Step Review Workflow

### Step 1: Gather Context & Diff

- **For Mode A (Local Self-Review)**:
  ```bash
  git log -1 --format=%B HEAD
  git diff origin/main...HEAD
  ```
- **For Mode B (Gerrit CL `<cl>`)**:
  ```bash
  ./gh pr view <cl> --comments
  ./gh pr diff <cl>
  ```
  If the CL links a Buganizer issue (`Bug: b/<id>` or `Fixed: b/<id>`), inspect
  it via `./gh issue view <id>` to verify the change actually solves the stated
  problem.
- **Hard stops**: If no target resolves (no `HEAD` ahead of `origin/main` and
  no CL given), or the CL is `MERGED` or `ABANDONED`
  (`./gh pr view <cl> --json state`), stop and say why without dispatching
  anything.

### Step 2: Dispatch Reviewer Subagents & Merge Findings

You coordinate: you do not read changed source yourself when a reviewer
subagent handles the file. Work from the diff gathered in Step 1.

1. **Map files to reviewers.** A row applies only if a changed file matches
   it **and** the agent is enabled (its definition file exists and
   `git grep -l "^disabled: true" -- <definition file>` prints nothing).

   | Files matching | Agent (`name:`) | Definition file |
   | --- | --- | --- |
   | `*.c`, `*.cc`, `*.h`, `*.inc` | `cpp-reviewer` | `.agents/agents/cpp_reviewer.md` |
   | `*.py`, `*.pyi` | `python-reviewer` | `.agents/agents/python_reviewer.md` |
   | `*.rs` | `rust-reviewer` | `.agents/agents/rust_reviewer.md` |

2. **Cut one patch per reviewer without reading it.** `out=$(mktemp -d)`, then
   `git diff origin/main...HEAD -- <its files> > "$out/<agent>.patch"` (Mode
   B: filter the `./gh pr diff <cl>` output the same way).
3. **Dispatch every selected reviewer in one parallel step**, each as a
   read-only background subagent. Give each the checkout path, its patch
   path, its file list, and this instruction verbatim:

   > Read and follow `.agents/skills/review/reviewer_workflow.md`. Return
   > findings in the Finding format defined there and nothing else.

   Wait for every reviewer to finish before merging.
4. **Merge.** Render findings with the severity prefixes above as mapped in
   [`reviewer_workflow.md`](reviewer_workflow.md): `blocking` and
   `should-fix` become `[<tag>] <issue> (<rule>)`, `nit` becomes
   `nit: <issue>`. Deduplicate same file/line/rule; order by severity with
   `scope: in-diff` before `out-of-diff`. Never soften, reword, or drop a
   reviewer's `blocking` finding. Apply the nit budget to the merged set.
5. **Fall back for the rest.** Files matching no row, or only rows whose
   agent is disabled or absent, get the inline audit below. Report which
   files each reviewer handled and which fell back.

#### Fallback: inline audit

For files no enabled reviewer handled, read the full surrounding file context
(`view_file`) for modified hunks—never review a diff hunk in isolation. Check
these six Pigweed dimensions:

#### 1. Embedded Memory & Execution Constraints
- **Zero Dynamic Allocation**: Core Pigweed C++ modules disallow `new`,
  `delete`, `std::vector`, `std::string`, `std::map`, and `std::function`. Verify
  fixed-capacity alternatives (`pw::InlineString`, `pw::Vector`, `pw::Function`,
  `pw::Alloctor` injection) are used.
- **Stack & Code Size (`pw_bloat`)**: Flag large stack allocations (e.g., byte
  arrays >256 bytes on MCU stacks), accidental `<iostream>`/`<sstream>`
  includes, or heavy template instantiations on hot paths.
- **ISR & Thread Safety (`pw_sync`)**: Check `InterruptSpinLock` vs. `Mutex`
  usage in interrupt-callable contexts, lock annotations
  (`PW_GUARDED_BY`, `PW_EXCLUSIVE_LOCKS_REQUIRED`), and `Borrowable` lifetimes.

#### 2. C++17 / Rust (`pw_kernel`) / Python Idioms
- **C++17 Compatibility**: Public headers (`public/.../*.h`) must compile cleanly
  with `-std=c++17`. C++20 enhancements must be guarded with `__cplusplus` or
  `PW_CXX_STANDARD_IS_SUPPORTED(20)`.
- **Error Handling**: Recoverable errors must use `pw::Status` / `pw::Result<T>`
  (no C++ exceptions). Fatal invariant violations must use `PW_CHECK` /
  `PW_ASSERT`.
- **`pw_async2` (if applicable)**: Verify `Pend(Context&)` implementations
  properly register a `Waker` (`PW_ASYNC_STORE_WAKER`) whenever returning
  `Pending()`, and never hold locks across yield points.
- **Rust / `pw_kernel` (if applicable)**: Verify `no_std` compliance, minimal
  `unsafe` blocks with explicit `// SAFETY:` justifications, and panic-free
  error propagation.

#### 3. Build System Parity (`Bazel` + `GN` + `CMake`)
- If a source file (`.cc`), public header (`.h`), or dependency was added or
  removed in `BUILD.bazel`, verify whether the module also maintains `BUILD.gn`
  (and `CMakeLists.txt`). Missing GN parity is one of the top causes of Pigweed
  CQ breakages!

#### 4. Test Rigor & Negative Compilation Tests
- Every bugfix or behavior change requires a `pw_unit_test` (`*_test.cc`) test
  case.
- Assertions on `pw::Status` / `pw::Result` should use `PW_TEST_EXPECT_OK` and
  `PW_TEST_ASSERT_OK`.
- Template/constexpr API constraints should include `PW_NC_TEST` negative
  compilation tests where appropriate.

#### 5. Documentation & Commit Hygiene
- Public API additions/changes in `public/` headers must update Doxygen comments
  and module `.rst` docs (`docs` skill conventions).
- Commit message must follow `<module>: <Imperative subject <=72 chars>`, explain
  the *why*, include `Bug: b/<id>` or `Fixed: b/<id>`, and preserve `Change-Id:`.

#### 6. AI Artifacts
- If the change touches `.agents/`, `AGENTS.md`, `GEMINI.md`, or `CLAUDE.md`:
  read `docs/sphinx/style/ai_artifacts.rst` in full at review time and apply
  its review checklist; cite findings as
  `https://pigweed.dev/style/ai_artifacts.html#<anchor>` (for example
  `#docs-pw-style-ai-artifacts-review-checklist`).

---

### Step 3: Empirical Check (When Local Workspace Matches)

When running in Mode A (or Mode B from a worktree allocated with
`./gh wt use review-<cl> --cl <cl> --json`; never `./gh pr checkout` in a
checkout you did not allocate):
- Run hermetic tests for modified modules:
  ```bash
  bazelisk test --noshow_progress --noshow_loading_progress //<module>/...
  ```
- Check formatting:
  ```bash
  ./pw format --check
  ```

---

### Step 4: Output Verdict & Stage Drafts

#### In Mode A (Self-Review Gate):
Return a structured markdown report:
- **Verdict**: `PASS` (zero `[bug]`, `[embedded/size]`, `[api]`, `[build]`, or
  `[test]` findings) or `NEEDS_FIXES`.
- **Blocking Findings**: File, line, severity tag, and exact fix required.
- **Nits (<= 3)**: Optional minor improvements.
- **Coverage**: which files each reviewer subagent handled, which fell back
  to the inline audit, and any `scope: out-of-diff` observations.

#### In Mode B (Gerrit CL Reviewer):
1. Check existing comments (`./gh pr view <cl> --comments`) so you do not
   duplicate feedback already left by another reviewer.
2. Stage each `in-diff` finding as a private Gerrit draft:
   ```bash
   ./gh pr comment <cl> --path <file> --line <line> -m "[<tag>] <concise explanation and concrete suggestion>" --draft
   ```
   Put `out-of-diff` findings, if any, in one change-level draft
   (`./gh pr comment <cl> -m "<summary>" --draft`). Never vote.
3. Present a summary table of all staged `--draft` comments to the user, along
   with the command to publish them once reviewed (`./gh pr review <cl> --publish`).
