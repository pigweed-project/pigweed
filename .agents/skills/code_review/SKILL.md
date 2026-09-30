---
name: Code Review
description: Comprehensive workflow for reviewing Git patches and Gerrit Change Lists (CLs).
---

# Overview

Analyze Git commits or Gerrit Change Lists (CLs) and provide constructive,
concise feedback focused on bugs, edge cases, security vulnerabilities, API
consistency, and Pigweed style.

# Review Criteria

Evaluate every change against:
* **Testing:** Sufficient unit and negative-compilation tests covering edge cases?
* **Functionality:** Works as intended without regressions or subtle bugs?
* **Security:** Buffer overflows, integer overflows, or resource leaks?
* **Style & Consistency:** Adheres to Pigweed coding style (see root `AGENTS.md`) and surrounding module patterns?
* **Commit Message:** Conforms to [Pigweed commit message style](../../../docs/sphinx/style/commit_message.rst)?

# Workflow

1. **Fetch the Patch:**
   - **Local commit (`HEAD`):** `git --no-pager show HEAD > patch_HEAD.diff`
   - **Gerrit CL (`<id>` or current branch):**
     ```bash
     ./gh pr view [<id>] --comments
     ./gh pr diff [<id>] > patch_CL_<ID>.diff
     ```
2. **Track & Analyze (`tasks.md`):**
   - Create a `tasks.md` artifact in the conversation's artifacts directory (`write_to_file` with `ArtifactMetadata`) listing the Review Criteria as checkboxes, and check each item off as you analyze the diff.
3. **Record or Stage Findings:**
   - Save the review report to `review_HEAD.md` or `review_CL_<ID>.md` in the artifacts directory, starting with **LGTM: [✓]** or **LGTM: [x]**. Prefix minor or optional suggestions with `nit:`.
   - When asked to post comments on Gerrit (see [`.agents/skills/ghish/SKILL.md`](../ghish/SKILL.md)), stage inline comments with `--draft` unless instructed to publish immediately:
     ```bash
     # Substantive issue:
     ./gh pr comment [<id>] --path <file> --line <line> -m "Check for empty span before indexing." --draft
     # Minor style or readability nit:
     ./gh pr comment [<id>] --path <file> --line <line> -m "nit: Prefer pw::Status over int return code." --draft
     ```
4. **Final Output:** Provide the LGTM status and absolute paths to the generated artifacts.
