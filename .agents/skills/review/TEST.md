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

---

## 4. Reviewer Subagent Coordination Scenarios

`/review` dispatches the language reviewer subagents defined in
`.agents/agents/*_reviewer.md`, each following
`.agents/skills/review/reviewer_workflow.md`, and falls back to its inline
audit for files with no enabled reviewer. For the general agent evaluation
rubric, see `pw_ghish/agent_eval.rst`.

### 4.1 Setup

1. Read `.agents/skills/review/SKILL.md` and
   `.agents/skills/review/reviewer_workflow.md`.
2. Run every prompt from the repository root in a fresh agent session.
3. Reviewer agents land with `disabled: true`. For scenarios 5-10, flip
   `disabled: true` to `disabled: false` in the agent files named by the
   scenario **in a scratch worktree only**; never commit that change until
   the scenario passes.
4. Create the fixture commit for scenarios 4-9 by committing the snippets in
   section 4.3 under `pw_scratch/` on a branch ahead of `origin/main`.

### 4.2 Scenarios

| # | Scenario | User prompt | Expected behavior | Prohibited anti-patterns |
| :--- | :--- | :--- | :--- | :--- |
| **1** | **Non-trigger** | *"Show me `git status` and the last three commits."* | Runs `git status -sb`, `git log -3`; skill not loaded. | Loading the review skill or any reviewer. |
| **2** | **Quick look guard** | *"Does this diff look OK?"* (one-file local change) | Answers directly without loading the review skill. | Spawning reviewer subagents. |
| **3** | **Hard stop: merged CL** | *"/review pwrev/\<merged CL\>"* | `./gh pr view <id> --json state`; stops with the reason; nothing spawned. | Dispatching reviewers; reading the diff. |
| **4** | **Fallback: no enabled reviewers** (all agents still `disabled: true`) | *"/review"* on a branch with `.cc` and `.py` changes | Spawns nothing; runs the inline audit on every file; the report's Coverage section lists every file as fallback. | Claiming a reviewer ran; silently skipping files. |
| **5** | **Dispatch** (`cpp-reviewer`, `python-reviewer` enabled) | *"/review"* on a branch changing `a.cc`, `b.py`, `BUILD.bazel`, `docs.rst` | `git diff --name-only origin/main...HEAD`; one patch per reviewer under `mktemp -d`; `cpp-reviewer` gets only `a.cc`, `python-reviewer` only `b.py`; both run in one parallel step; `BUILD.bazel` and `docs.rst` get the inline audit and are listed as fallback. | Coordinator reading `a.cc`/`b.py`; sequential dispatch; reviewing `a.cc` itself. |
| **6** | **Gerrit target isolation** | *"/review pwrev/\<open CL\>"* (Step 3 wanted) | `./gh wt use review-<id> --cl <id> --json`; all later commands run in the returned `symlink_path`. | `./gh pr checkout` in the current checkout; raw `git fetch`/`git checkout` of the CL ref. |
| **7** | **C++ violations** (`cpp-reviewer` enabled, fixture 4.3.1) | *"/review"* | Findings cite `code_reviews.html#docs-code-reviews-checklist-no-silent-failures` (`nullptr` on contract violation), `style/cpp.html` (rule of five), and `style/doxygen.html` (implementation detail in Doxygen), each `severity: blocking` or `should-fix`, `scope: in-diff`, with a `tag:`, a `quote:`, and a code `fix:`; the merged report renders them as `[bug]` / `[api]` and `Verdict: NEEDS_FIXES`. | Findings without a `rule:` URL; findings on unchanged lines marked `in-diff`; dropping a `blocking` finding. |
| **8** | **Python violations** (`python-reviewer` enabled, fixture 4.3.2) | *"/review"* | Findings cite `style/python.html` for the broad `try` and `except Exception: pass`, and `code_reviews.html#docs-code-reviews-checklist-no-silent-failures` for returning `None`. | Missing the swallowed exception. |
| **9** | **Compliant change** (fixture 4.3.3) | *"/review"* | Reviewer returns `No findings.`; report says `Verdict: PASS`. | Inventing nits with no `quote:`; `NEEDS_FIXES`. |
| **10** | **Drafts in Mode B** | After scenario 7 on an open CL: *"/review pwrev/\<id\>"* | One `./gh pr comment <id> --path <file> --line <line> -m "[<tag>] <issue> (<rule>)" --draft` per `in-diff` finding; `out-of-diff` findings, if any, in one change-level draft. | Posting without `--draft`; inline comments on unchanged lines; any vote. |

### 4.3 Fixtures

#### 4.3.1 `pw_scratch/public/pw_scratch/ring.h` (C++ violations)

```cpp
#pragma once
#include <cstddef>
#include <cstdint>

namespace pw::scratch {

/// Ring buffer. Internally stores head and tail as uint16_t and masks with
/// capacity - 1 on every access.
class Ring {
 public:
  explicit Ring(std::byte* data, size_t capacity)
      : data_(data), capacity_(capacity) {}
  ~Ring() { Flush(); }  // declares a destructor but no copy/move members

  // Returns nullptr if index >= Size().
  std::byte* At(size_t index) {
    if (index >= Size()) {
      return nullptr;
    }
    return &data_[(head_ + index) & (capacity_ - 1)];
  }

  size_t Size() const;
  void Flush();

 private:
  std::byte* data_;
  size_t capacity_;
  uint16_t head_ = 0;
  uint16_t tail_ = 0;
};

}  // namespace pw::scratch
```

Expected: `blocking` (`tag: bug`) for `return nullptr` on a documented
precondition, `should-fix` (`tag: api`) for the rule of five, `should-fix`
(`tag: api`) for Doxygen that documents internals instead of behavior,
preconditions, and lifetime of `data`.

#### 4.3.2 `pw_scratch/py/pw_scratch/load.py` (Python violations)

```python
import json
from pathlib import Path


def load_config(path: str):
    try:
        text = Path(path).read_text()
        print(f"loaded {path}")
        return json.loads(text)
    except Exception:
        pass
    return None
```

Expected: `should-fix` for the broad `try` body and `except Exception:
pass`, `blocking` for returning `None` instead of raising, `should-fix` for
the missing return annotation.

#### 4.3.3 `pw_scratch/public/pw_scratch/counter.h` (compliant)

```cpp
#pragma once
#include <cstdint>

namespace pw::scratch {

/// Monotonic event counter.
///
/// Not thread safe; callers synchronize externally.
class Counter {
 public:
  constexpr Counter() = default;

  /// Increments the count. Saturates at `UINT32_MAX` instead of wrapping.
  void Increment() {
    if (count_ != UINT32_MAX) {
      ++count_;
    }
  }

  /// @returns the number of `Increment()` calls, saturated.
  constexpr uint32_t count() const { return count_; }

 private:
  uint32_t count_ = 0;
};

}  // namespace pw::scratch
```

### 4.4 Grading

A session **passes** only if:

- Scenarios 1-3 never spawn a reviewer and the coordinator never reads
  source.
- Scenario 4 spawns nothing and audits every file inline, reporting them as
  fallback.
- Scenario 5 dispatches in one parallel step with per-reviewer patches and
  audits the remaining files inline.
- Scenario 6 allocates a worktree before touching the CL.
- Scenarios 7-8 produce every expected finding with a resolvable `rule:` URL,
  a `quote:`, and a `tag:` that the merged report renders as a severity
  prefix; scenario 9 produces none.
- Scenario 10 posts drafts only, with the specified body, and never votes.
