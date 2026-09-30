## Overview

Pigweed is a collection of embedded libraries, called "modules," that work
together to enable faster and more robust development of embedded systems. It
provides a comprehensive development environment, including a build system,
testing frameworks, and tools for debugging and analysis.

## C++ Development Guidance

This section provides specific guidance for writing C++ code and tests in
Pigweed.

### Testing

Pigweed places a strong emphasis on testing. Here are some key points to
remember:

- **Framework:** Tests are written using the `pw_unit_test` framework, which
  provides a GoogleTest-like API. You should be familiar with the `TEST` and
  `TEST_F` macros for defining test cases, and the `EXPECT_*` and `ASSERT_*`
  macros for assertions.
- **File Location:** Test files should be located in the same module as the code
  they are testing and have a `_test.cc` suffix.
- **Status and Result:** Use `PW_TEST_EXPECT_OK` and `PW_TEST_ASSERT_OK` to test
  functions that return `pw::Status` or `pw::Result`.
- **Negative Compilation Tests:** Pigweed uses negative compilation tests
  (`PW_NC_TEST`) to verify that code fails to compile under specific conditions.
  This is particularly useful for testing static assertions and template
  metaprogramming.
- **Test Naming:** Test suites and test cases should be named clearly and
  descriptively. The test suite name often corresponds to the class or module
  being tested, and the test case name describes the specific behavior being
  tested.

### C++ Style Guide (Condensed)

This is a condensed version of the Pigweed C++ style guide. For more details,
refer to the full style guide.

- **C++ Standard:** All C++ code must compile with `-std=c++17`. C++20 features
  can be used if the code remains C++17 compatible.
- **Formatting:** Format code with `./pw format`.
- **C and C++ Standard Libraries:**
  - Use C++-style headers (e.g., `<cstdlib>` instead of `<stdlib.h>`).
  - A limited subset of the C++ Standard Library is permitted. Dynamic memory
    allocation, streams, and exceptions are disallowed in core Pigweed modules.
  - Use Pigweed's own libraries (e.g., `pw::string`, `pw::sync`, `pw::function`)
    instead of their `std` counterparts.
- **Comments:**
  - Prefer C++-style comments (`//`).
  - Code in comments should be indented with two additional spaces.
- **Control Statements:**
  - Always use braces for loops and conditionals.
  - Prefer early exits with `return` and `continue`.
  - Do not use `else` after a `return` or `continue`.
- **Error Handling:**
  - Use `pw::Status` and `pw::Result` for recoverable errors.
  - Use `PW_ASSERT` and `PW_CHECK` for fatal errors.
- **Include Guards:** Use `#pragma once`.
- **Logging:**
  - Use the `pw_log` module for logging.
  - Log errors as soon as they are unambiguously determined to be errors.
  - Log at the appropriate level (`PW_LOG_DEBUG`, `PW_LOG_INFO`, `PW_LOG_WARN`,
    `PW_LOG_ERROR`, `PW_LOG_CRITICAL`).
- **Memory Allocation:** No dynamic memory allocation in core Pigweed code.
- **Naming:**
  - All C++ code must be in the `pw` namespace.
  - C symbols must be prefixed with the module name (e.g., `pw_tokenizer_*`).
  - Public macros must be prefixed with `PW_MY_MODULE_*`.
- **Pointers and References:**
  - Place the `*` or `&` next to the type (e.g., `int* number`).
  - Prefer references over pointers when possible.
- **Preprocessor Macros:**
  - Use macros only when they significantly improve the code.
  - Standalone statement macros must require a semicolon.
- **Unsigned Integers:** Permitted, but be careful when mixing with signed
  integers.
- **Unused Variables:** Leave function parameters unnamed or use the standard
  C++ attribute `[[maybe_unused]]`.

## Python Development Guidance

### Python Style Guide (Condensed)

- **Style:** Pigweed follows PEP 8. Format code with `./pw format`.
- **Python Versions:** Upstream Pigweed code must support the officially
  supported Python versions.
- **Generated Files:** Python packages with generated files should extend their
  import path in `__init__.py`.

## Project Conventions

### Build System Interaction

Pigweed uses `bazel` and `gn` as its primary build systems. Here are some common
commands:

- **`bazel` (Build system):**
  - **Note:** Always use `bazelisk` and not `bazel` to ensure the correct
    `bazel` version is in use. **Note:** Use `--noshow_progress`
    `--noshow_loading_progress` to reduce the amount of output produced and
    avoid polluting the context window.
  - **Build a target:**
    `bazelisk build --noshow_progress --noshow_loading_progress //path/to/module:target`
  - **Run a test:**
    `bazelisk test --noshow_progress --noshow_loading_progress //path/to/module:target`
  - **Run all tests in a module:**
    `bazelisk test --noshow_progress --noshow_loading_progress //path/to/module/...`

- **`gn` (Meta-build system):**

  - **Generate build files:** `gn gen out`
  - **Build all targets and run tests:** `ninja -C out --quiet`
  - **Clean the build:** `gn clean out`

### Commit Message Conventions

Pigweed follows a specific commit message format. A good commit message should
be concise and descriptive.

- **Subject Line:**

  - Start with the module name affected by the change, followed by a colon.
  - Use the imperative mood (e.g., "Add feature" not "Added feature").
  - Keep it under 72 characters.
  - Example: `pw_foo: Add support for bar feature`

- **Body:**

  - Explain the "what" and "why" of the change, not the "how".
  - Reference any relevant issue trackers.
  - If you are tempted to write a long commit message, consider if the content
    is better written in the docs and referred to from the commit.
  - Use a `Bug:` or `Fixed:` line for bug fixes.
  - Example:

    ``` This change adds support for the bar feature to the `pw_foo` module.
    This is necessary because...

    Bug: b/123456789 ```

## Gerrit, CI, Buganizer Issues, and Worktrees (`./gh`)

Pigweed provides `./gh` (`//pw_ghish:gh-ish`), which exposes Gerrit code
reviews (`./gh pr`), LUCI Buildbucket CI checks (`./gh pr checks`, `./gh run`),
Google Issue Tracker / Buganizer (`./gh issue`), and multi-agent Git/Bazel
worktrees (`./gh wt`) using standard GitHub CLI (`gh`) syntax.

For full workflows and flag references, load the
[`ghish` skill](.agents/skills/ghish/SKILL.md) and
[`worktree` skill](.agents/skills/worktree/SKILL.md).

### Strict Rules for AI Agents

1. **Always use `./gh`**: Use `./gh` for all Gerrit CLs, Buildbucket tryjobs,
   Buganizer issues, and worktree management.
2. **NEVER use raw `git push`**: Always use `./gh pr create` to upload new
   changes and `./gh pr push` (or `./gh push`) to upload new patchsets to
   existing changes.
3. **NEVER use raw `curl`, `gob-curl`, or `.gitcookies` scripts**: Do not craft
   manual REST calls to `pigweed-review.googlesource.com` or
   `cr-buildbucket.appspot.com`. `./gh` handles authentication automatically.
4. **NEVER poll CI in a loop**: To wait on tryjobs, run
   `./gh pr checks --watch --fail-fast` as a background command and stop
   calling tools until notified.

### Quick Command Reference

- **Inspect a CL (`pwrev/1234`, Gerrit URL, or active branch)**:
  - Metadata & review comments: `./gh pr view [<id>] --comments`
  - Unified patch diff: `./gh pr diff [<id>]`
  - Check out a CL locally: `./gh pr checkout <id>`
- **Create, push, or edit a CL**:
  - Create new CL: `./gh pr create [--cq] [--draft] [-r <email>]`
  - Upload new patchset: `./gh pr push [--cq]`
  - Edit description or link bug (preserving Git trailers):
    `./gh pr edit [<id>] --body "<text>" --bug b/<id>`
- **Respond to or post review comments**:
  - Reply to inline thread & mark resolved (use `--draft` to stage privately):
    `./gh pr comment [<id>] --path <file> --line <line> -m "<msg>" --resolved [--draft]`
- **Inspect & rerun LUCI CI / Buildbucket checks**:
  - Check or watch status: `./gh pr checks [<id>] [--watch --fail-fast]`
  - View failure logs in terminal: `./gh run view [<id>] --log-failed`
  - View builder step tree: `./gh run view [<id>] -j <builder>`
  - Rerun failed builders: `./gh run rerun [<id>] --failed` (or `-j <builder>`)
- **Buganizer issues (`./gh issue`)**:
  - Inspect or list issues: `./gh issue view [<id>] --comments`, `./gh issue list`
  - File bug & add `Bug: b/<id>` trailer to `HEAD`:
    `./gh issue create -t "<title>" -b "<body>" --amend`
- **Warm Git/Bazel worktrees (`./gh wt`)**:
  - Allocate or switch project slot: `./gh wt use <project> --json`
  - View active & parked workstreams: `./gh wt list`


