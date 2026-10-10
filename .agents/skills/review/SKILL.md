---
name: review
description: >-
  High-signal, embedded-aware Pigweed code review skill with strict comment
  calibration. Supports both Self-Review Gate mode (pre-upload verification for
  /crank) and Gerrit CL Reviewer mode (staging calibrated --draft comments via
  ./gh pr comment).
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

### Step 2: Deep Pigweed Technical Audit

Read the full surrounding file context (`view_file`) for modified hunks—never
review a diff hunk in isolation. Check these six Pigweed dimensions:

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

---

### Step 3: Empirical Check (When Local Workspace Matches)

When running in Mode A (or Mode B after `./gh pr checkout <cl>`):
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

#### In Mode B (Gerrit CL Reviewer):
1. Check existing comments (`./gh pr view <cl> --comments`) so you do not
   duplicate feedback already left by another reviewer.
2. Stage each finding as a private Gerrit draft:
   ```bash
   ./gh pr comment <cl> --path <file> --line <line> -m "[<tag>] <concise explanation and concrete suggestion>" --draft
   ```
3. Present a summary table of all staged `--draft` comments to the user, along
   with the command to publish them once reviewed (`./gh pr review <cl> --publish`).
