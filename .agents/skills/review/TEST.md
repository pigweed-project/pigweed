# Testing `review` Skill (`/review`)

This document defines the verification plan and interactive evaluation scenarios
for the `/review` skill (`.agents/skills/review/SKILL.md`).

---

## 1. Automated Pre-Flight Checks

```bash
# 1. Verify SKILL.md exists and has valid YAML frontmatter
head -n 6 .agents/skills/review/SKILL.md

# 2. Check formatting
./pw format --check .agents/skills/review/SKILL.md .agents/skills/review/TEST.md
```

---

## 2. Interactive Agent Test Scenarios

| # | Scenario | Setup / State | User Prompt | Expected Agent Behavior | Prohibited Anti-Patterns |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **1** | **Self-Review Gate Catches Embedded Violation** | Local commit adds `std::vector` in a core C++ module and omits `BUILD.gn` update | `/review` | Emits `Verdict: NEEDS_FIXES`, flags `[embedded/size]` for `std::vector` and `[build]` for missing `BUILD.gn` parity. | Rubber-stamping (`PASS`) or posting Gerrit comments on an unuploaded local change. |
| **2** | **Calibrated Gerrit CL Review** | Teammate CL (`pwrev/490712`) has 1 real bug and several minor style quirks | `/review pwrev/490712` | Runs `./gh pr view 490712 --comments` and `./gh pr diff 490712`, verifies context with `view_file`/`code_search`, stages `[bug]` and at most 3 `nit:` comments using `./gh pr comment 490712 --path ... --line ... -m "..." --draft`. | Publishing comments without `--draft`; posting >3 nits; posting praise/filler comments. |
| **3** | **Clean CL Review** | High-quality CL with tests, GN/Bazel parity, and clean C++17 headers | `/review <cl>` | Verifies tests/diff, reports `Verdict: PASS` with zero fluff comments staged. | Inventing nitpicks just to have something to say. |

---

## 3. Grading Checklist

- [ ] Distinguishes Mode A (Self-Review Gate) from Mode B (Gerrit CL Reviewer).
- [ ] Every finding uses a calibrated severity prefix (`[bug]`, `[embedded/size]`, `[api]`, `[build]`, `[test]`, `nit:`).
- [ ] Enforces the Nit Budget (at most 3 `nit:` comments) and zero praise/filler comments.
- [ ] Checks Pigweed-specific invariants (zero-heap, C++17 public headers, `BUILD.bazel` + `BUILD.gn` parity, `PW_TEST_EXPECT_OK`).
- [ ] Stages all Gerrit review comments with `--draft` (never publishes live without explicit instruction).
