# Testing `respond` Skill (`/respond`)

This document defines the verification plan and interactive evaluation scenarios
for the `/respond` skill (`.agents/skills/respond/SKILL.md`).

---

## 1. Automated Pre-Flight Checks

```bash
# 1. Verify SKILL.md exists and has valid YAML frontmatter
head -n 6 .agents/skills/respond/SKILL.md

# 2. Check formatting
./pw format --check .agents/skills/respond/SKILL.md .agents/skills/respond/TEST.md
```

---

## 2. Interactive Agent Test Scenarios

| # | Scenario | Setup / State | User Prompt | Expected Agent Behavior | Prohibited Anti-Patterns |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **1** | **Author Self-Steering Drafts (CUJ 1)** | CL has 2 private `[DRAFT]` comments placed by the author with instructions | `/respond` | Implements the 2 instructions, runs `bazelisk test`, deletes self-notes with `./gh pr comment --path <file> --line <line> --delete-draft`, amends `HEAD` preserving `Change-Id:`. | Leaving the author's private task notes staged to be published to external reviewers. |
| **2** | **Valid Reviewer Bug Catch** | Unresolved reviewer comment pointing out missing error check | `/respond` | Fixes bug, adds unit test (`PW_TEST_EXPECT_OK`), runs `bazelisk test`, stages reply via `./gh pr comment --path <file> --line <line> -m "..." --resolved --draft`. | Publishing the reply live without `--draft` before CI passes. |
| **3** | **Respectful Pushback on Embedded Violation** | Reviewer asks to replace `pw::Vector<int, 8>` with `std::vector<int>` in a core Pigweed module | `/respond` | Verifies zero-heap rule in `AGENTS.md` / module headers, keeps `pw::Vector`, stages polite evidence-backed reply via `./gh pr comment --path <file> --line <line> -m "..." --draft` (omitting `--resolved`), tags `[PUSHBACK_STAGED]`. | Blindly implementing `std::vector` and breaking Pigweed's zero-heap invariant; marking a pushback thread `--resolved`. |
| **4** | **Out-of-Scope Refactor Deferral** | Reviewer suggests refactoring an unrelated module class | `/respond` | Files follow-up issue with `./gh issue create`, stages reply citing `b/<id>` with `--resolved --draft`. | Ballooning the CL scope with unrelated module-wide churn. |

---

## 3. Grading Checklist

- [ ] Fetches comments and drafts via `./gh pr view --comments` / `--json`.
- [ ] Deletes author self-steering notes via `--delete-draft` after implementing them.
- [ ] Stages all replies to external threads with `--draft` (never publishes prematurely).
- [ ] Evaluates technical merit and pushes back respectfully (leaving thread unresolved) when a suggestion violates Pigweed embedded/style invariants.
- [ ] Preserves `Change-Id:` when amending commits and runs `bazelisk test` + `./pw format`.
