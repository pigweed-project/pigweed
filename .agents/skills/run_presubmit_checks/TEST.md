# Testing Run Presubmit Checks

This document provides details how to verify that the `Run Presubmit Checks`
skill is correctly triggering in agent sessions.

## Tests

The following tests verify the agent's behavior.

### 1. Successful presubmit run

This test suite verifies that the agent runs presubmit checks in auto mode,
with the tool passing. The test shall be run with the following prompts:

1.  "Run presubmit checks"
2.  "Run presubmits on my branch"
3.  "Prepare for submission"
4.  "Finish my changes"

For each prompt:

-   Complete the setup as described below.
-   Run the test as specified, using the current prompt.
-   Verify the agent's behavior matches the description.
-   Complete the specified cleanup.
-   Repeat the above for each prompt.

#### Setup

1.  In the git repo, create a new `presubmit-stack-fix-base` branch for this
    test.
2.  Create a stack of 3 commits for testing:

    1.  **Automatic fix**: Delete "#pragma once" from
        `pw_status/public/pw_status/status.h` to break the pragma_once test.
    2.  **Manual fix**: Delete `status.cc` from `pw_status/BUILD.bazel`.
    3.  **No issue**: Add a "// This is a test" comment to
        `pw_status/status.cc`.

3.  Create a new branch from `presubmit-stack-fix-base` for each test case:
    `presubmit-stack-fix-#` where # is the test case number.

#### Test
Run the selected prompt.

-   **Skill Trigger:** The agent should load `prepare_for_submission/SKILL.md`.
-   **Action:** The agent executes:
    ```bash
    ./pw presubmit --mode=auto --ui minimal --base HEAD~3  # or equivalent base
    ```
    The presubmit tool stops on the second commit. The agent fixes the issue by
    adding the missing `status.cc` file to `pw_status/BUILD.bazel`, then runs
    the `./pw presubmit --resume <PATH>` specified by the tool
-   **Failure Mode:** `./pw presubmit` does not complete successfully.

#### Cleanup

Delete the `presubmit-stack-fix*` branches.

### 2. Unsuccessful presubmit run

This is a group of tests for verifying that the agent correctly runs presubmit
checks in auto mode and returns control to the user in an ambiguous situation.

#### Setup

1.  In the git repo, create a new `presubmit-stack-fix-base` branch for this
    test.
2.  Create a commit that adds a new `pw_status/public/pw_status/propagate.h`.
    The header is empty except for TODO comment that says
    `// TODO: Implement critical feature as described in design doc.` This does
    NOT reference a bug or user name, so will fail the TODO presubmit check.

#### Test
Prompt the agent with "Run presubmit checks".

-   **Skill Trigger:** The agent should load `prepare_for_submission/SKILL.md`.
-   **Action:** The agent executes:
    ```bash
    ./pw presubmit --mode=auto --ui minimal --base HEAD~1  # or equivalent base
    ```
    The presubmit tool should idenfify two errors: the file is missing from the
    build, and the file's TODO is incorrectly formatted. The agent may fix the
    build file issue. It should not attempt to fix the TODO issue, however,
    since the resolution is unclear (refer to a bug? remove the TODO? implement
    the feature?).
-   **Failure Mode:** `./pw presubmit` completes successfully, with the agent
    deciding how to resolve the TODO check on its own.

#### Cleanup

Delete the `presubmit-stack-fix*` branches.