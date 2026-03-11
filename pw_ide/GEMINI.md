# Pigweed IDE (`pw_ide`) Engineering Guidelines

This document provides foundational principles and technical requirements for AI
agents contributing to `pw_ide`. This document extends the top-level GEMINI.md
guidance with additional instructions more specific to the files contained under
this subdirectory.

## Core Principles

### 1. Single Responsibility & Conciseness
*   **Decompose Logic**: Each function, class, or Bazel rule must have one clear
    responsibility. Decompose complex logic into small, reusable, and focused
    helper functions.
*   **Explicit over Clever**: Prioritize readability and explicit intent over
    "magic" behavior. Use descriptive naming that explains *what* data
    represents, rather than its type.
*   **Pure Functions**: Prefer extracting logic into pure functions with no side
    effects to simplify reasoning and testing.

### 2. Mandatory & Focused Testing
*   **No Unverified Changes**: All new features, refactors, and bug fixes must
    include automated tests.
*   **Granular Tests**: Each test case should verify a single, specific
    behavior. Use descriptive names that clearly state the test's intent.
*   **Tools**:
    * **Python**: Use `unittest` with `pyfakefs` for robust filesystem mocking.
    * **Starlark**: Use `unittest.make` for all Starlark logic verification.

## Python Guidelines

### 1. Path Manipulation with `pathlib`
*   **Use `Path` Objects**: Use `pathlib.Path` exclusively for path operations.
    Avoid `os.path` and string-based path manipulation.
*   **Maintain Type Integrity**: Keep paths as `Path` objects throughout the
    application logic. Convert to strings *only* at serialization boundaries or
    when required by external APIs.
*   **Portability**: Ensure paths in generated project files are relative to the
    workspace root, not absolute.

### 2. Pattern Matching with `re`
*   **Prefer Regex for Complexity**: For all but the simplest string checks, use
    the `re` package. Regular expressions yield faster and more concise code for
    complex pattern matching.

### 3. Error Handling
*   **Exceptions Over Return Codes**: Raise descriptive exceptions to handle
    errors. Avoid returning `None` or error codes, which clutter the "happy
    path" and are easily ignored.

## Bazel & Starlark Guidelines

### 1. Performance & Efficiency
*   **Minimize Depset Conversions**: Converting a `depset` to a list (via
    `.to_list()`) is an $O(N)$ operation. Perform this conversion exactly once
    if needed, and **never** inside a loop.
*   **$O(1)$ Lookups**: Use dictionaries for mapping and filtering to avoid
    nested loops ($O(M \times N)$).

### 2. Safety & Correctness
*   **Explicit Copies**: Use `dict()` to create shallow copies of dictionaries
    to prevent shared state bugs.
*   **No Recursion**: Starlark does not support recursion; use iterative stacks
    or queues for graph traversal.
*   **Explicit Aspect Traversal**: Be specific in `attr_aspects` when writing
    aspects; avoid the `["*"]` wildcard to prevent issues with internal Bazel
    dependencies.
