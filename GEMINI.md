# Pigweed for AI Agents

This document provides an overview of Pigweed for AI agents that assist with
development.

## Overview

Pigweed is a collection of embedded libraries, called "modules," that work
together to enable faster and more robust development of embedded systems. It
provides a comprehensive development environment, including a build system,
testing frameworks, and tools for debugging and analysis.

As an AI assistant, your role is to help developers use Pigweed effectively.
This includes:

- **Writing and understanding code:** You should be familiar with the Pigweed
  C++ and Python style guides and be able to read and write code that conforms
  to them.
- **Using Pigweed modules:** You should understand the purpose of the various
  Pigweed modules and be able to use them to solve common embedded development
  problems.
- **Navigating the Pigweed source code:** You should be able to find your way
  around the Pigweed source code and locate relevant files and documentation.
- **Following Pigweed conventions:** You should be aware of Pigweed's
  conventions for things like commit messages, code formatting, and
  documentation.

## Local guidelines for AI agents

This document provides guidelines for AI agents making changes to the Pigweed
codebase.

Some subdirectories, such as `pw_ide`, contain their own `GEMINI.md` file with
more specific instructions for that area of the codebase. When working in those
areas, please refer to the local `GEMINI.md` for more detailed guidance.

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
- **Formatting:** Code is automatically formatted with `clang-format`.
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
- **Unused Variables:** The `PW_MAYBE_UNUSED` macro is deprecated and has been
  removed. For handling unused variables, the standard C++ attribute
  `[[maybe_unused]]` should be used instead.

## Python Development Guidance

### Python Style Guide (Condensed)

- **Style:** Pigweed follows PEP 8. Code should pass `pw format`, which uses
  `black`.
- **Python Versions:** Upstream Pigweed code must support the officially
  supported Python versions.
- **Generated Files:** Python packages with generated files should extend their
  import path in `__init__.py`.

## Project Conventions

### Documentation Standards (Condensed)

- **Format:** Documentation is written in reStructuredText (reST), not Markdown.
- **Headings:** Use a specific hierarchy of characters for headings (e.g.,
  `H1: ========` over and under, `H2: --------` over and under, `H3: ========`
  under).
- **Directives:** Indent directive content and attributes by 3 spaces.
- **Code Blocks:** Use `.. code-block:: <language>` for code blocks.
- **Writing Style:** Follow the Google Developer Documentation Style Guide. Use
  sentence case for titles and headings.

### Build System Interaction

Pigweed uses `gn` and `bazel` as its primary build systems. Here are some common
commands:

- **`gn` (Meta-build system):**

  - **Generate build files:** `gn gen out`
  - **Build all targets and run tests:** `ninja -C out --quiet`
  - **Clean the build:** `gn clean out`

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

## Fetching Change List (CL) diffs

Fuchsia development happens on Gerrit When the user asks for you to read a CL
for them, do the following:

1. Parse the change id from the CL URL. If the URL is `pwrev/1234`, then the id
   is 1234. If the URL is
   `https://pigweed-review.googlesource.com/c/pigweed/+/1299104`, then the ID is
   `1299104`.
2. If the user asked for a CL hosted at
   `https://pigweed-review.googlesource.com`, run this shell command to get the
   diff from the changelist:
   `curl -L https://pigweed-review.googlesource.com/changes/<ID>/revisions/current/patch?raw`.
3. Use this diff to answer further questions about the changelist

## Code review response workflow

Pigweed development happens on Gerrit, and you can help users get changes
through the review process by automating parts of the review flow. When the user
asks for reading review comments, do this:

1. Get change ID from the last couple git commits or ask user for it
2. Run this shell command to get open comments on the change:
   `curl https://pigweed-review.googlesource.com/changes/<ID>/comments`
3. Read the unresolved comments: i.e. have `unresolved=true`, and are for latest
   `patch_set` only
4. Read the relevant file and get the surrounding context in the file mentioned
5. List down comments (and address them if user asked to) along with exact ONE
   line in code where it belongs

## Agent behavior guidelines

### Proactiveness

Fulfill the user's request thoroughly. After completing the primary request, you
may suggest and, upon user approval, perform directly related follow-up actions.
Before suggesting a follow-up, you must analyze the project's context (e.g.,
read existing files, check tests) to ensure your suggestion is relevant and
adheres to established conventions. Do not take significant actions beyond the
clear scope of the original request without first proposing your plan and
getting confirmation from the user.

### Enhancing agent guidance

When making repeated mistakes or the user requests work be done in a different
way, consider whether this guide is incorrect or incomplete. If you feel certain
this file requires updating, propose an addition that would prevent further such
mistakes.
