# Testing Gerrit Skill

This document provides instructions on how to verify that the `Gerrit` skill is correctly functioning and triggering in agent sessions.

## Setup

1. Read `//.agents/skills/gerrit/SKILL.md`.
2. `cd` into the root directory of this repository. All tests should be run from the root directory.

## Test Cases

### 1. `SKILL.md` has correct frontmatter and structure

```console
cat .agents/skills/gerrit/SKILL.md
```

Verify:
* Frontmatter is delimited by `---` markers.
* YAML frontmatter contains `name: Gerrit`.
* `description` field value accurately describes Gerrit and Buildbucket capabilities.

### 2. Helper script exists and runs

```console
python3 .agents/skills/gerrit/scripts/search_builds.py 406352 6
```

Verify:
* Script executes cleanly and outputs the status summary of tryjobs for the patchset.

### 3. Review Comments Intent

**Prompt:**
> "Can you look at review comments on https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/406352?"

**Expected Behavior:**
* **Skill Trigger:** The agent loads `gerrit/SKILL.md`.
* **Action:** The agent fetches comments via the Gerrit REST API (`https://pigweed-review.googlesource.com/changes/406352/comments`), extracts unresolved comments for the latest patchset, and compiles a checklist.

### 4. Builder Retrying Intent

**Prompt:**
> "How do I retry the failed `pigweed-mac-arm-vscode` builder on CL 404076 patchset 4?"

**Expected Behavior:**
* **Skill Trigger:** The agent loads `gerrit/SKILL.md`.
* **Action:** The agent suggests or executes the targeted retry command:
  ```bash
  bb add -cl https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/404076/4 pigweed/pigweed.try/pigweed-mac-arm-vscode
  ```
