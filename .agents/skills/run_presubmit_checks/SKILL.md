---
name: run_presubmit_checks
description: >-
  How to use pw_presubmit to prepare one or more commits for submission,
automatically fixing issues like formatting.
---

# Presubmit Stack Fix Skill

This skill describes how to use Pigweed's presubmit script in `auto` mode to automatically run checks, apply fixes, and continue a rebase across a stack of commits (CLs).

## Overview

The `auto` mode in `pw_presubmit` streamlines the process of fixing presubmit issues across multiple commits. It starts an interactive rebase and runs checks on each commit. If a check fails but has an automatic fix, it applies the fix and amends the commit. If a check fails and cannot be fixed automatically, it stops and allows the user to fix it manually before resuming.

## Usage

### Prepare for Presubmit

- Ensure the working tree is clean before starting `auto` mode. If it is not clean, abort and tell the user to clean the working tree before starting.
- Run `git rev-parse HEAD` to get the current commit hash (referred to as `<COMMIT>`).
- Message the user to explain what will happen and warn them that it will change their branch (by amending commits).
- Provide the user with the command to fully undo the changes: `git rebase --abort; git reset <COMMIT> --hard`.
- Example message to the user:
  > I am starting an interactive rebase at `<COMMIT>`. This will run presubmit checks on each commit in your stack. If a check fails but has an automatic fix, it will apply the fix and **amend your commit**. This will change your branch history.
  >
  > If you need to abort and fully restore your branch to its original state, you can run:
  > `git rebase --abort; git reset <COMMIT> --hard`

### Running Presubmit in Auto Mode

To run presubmit in auto mode on a stack of commits, use the following command:

```console
$ ./pw presubmit --mode auto --ui minimal --base <BASE>
```

Replace `<BASE>` with the base commit of the stack (e.g., `origin/main` or a specific commit hash).

### Interacting with the Script

- **Automatic Fixes**: The script automatically applies fixes for checks that support them (e.g., formatting, copyright notice) and amends the current commit.
- **Failures**: If a check fails and cannot be automatically fixed, the script stops the rebase at the failing commit and saves its state. It outputs a message indicating failure and provide a path to a resume file (e.g., `resume.json`).

### Workflow for Failures

When the script fails on a commit:

1.  **Identify the failure**: Look at the output to see which step failed and why.
2.  **Fix the issues**: Manually fix the issues in the files.
3.  **Stage and Amend**: You MUST amend the commit with your fixes before resuming. The script requires a clean working tree.
    ```console
    $ git add <fixed_files>
    $ git commit --amend --no-edit
    ```
4.  **Resume**: Run the presubmit command again with the `--resume` flag and the path to the saved state file.
    ```console
    $ ./pw presubmit --resume /tmp/pw_presubmit_auto_XXXXXX/resume.json
    ```
    Replace `/tmp/pw_presubmit_auto_XXXXXX/resume.json` with the path provided in the failure message.

### Complex Problems

If you encounter complex problems that you cannot easily resolve, such as:
- Merge conflicts during the rebase that you are unsure how to resolve.
- Presubmit failures that require significant design changes or user input.
- The script failing repeatedly on the same commit after manual fixes.

**Ask the user for input** before proceeding.
