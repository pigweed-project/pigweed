# Testing `ghish` Skill

This document provides the evaluation plan, test prompts, and verification
criteria for testing the `ghish` (`./gh`) agent skill.

For the comprehensive agent behavioral evaluation rubric and anti-pattern
specifications, see [`pw_ghish/agent_eval.rst`](../../../pw_ghish/agent_eval.rst).

---

## 1. Automated Verification (Pre-Flight Checks)

Before running interactive agent prompts, verify that the `gh-ish` tool and skill
definitions are healthy:

```bash
# 1. Verify skill file exists and contains valid frontmatter
head -n 10 .agents/skills/ghish/SKILL.md

# 2. Run hermetic unit tests across pw_ghish
bazelisk test --noshow_progress --noshow_loading_progress //pw_ghish/...

# 3. Verify wrapper script and help output
./gh --help
./gh pr --help
```

---

## 2. Interactive Agent Test Scenarios

Run these prompts in fresh agent sessions to verify skill discovery, tool
selection, and adherence to safety guards.

| # | Scenario | User Prompt | Expected `./gh` Commands | Prohibited Anti-Patterns |
| :--- | :--- | :--- | :--- | :--- |
| **1** | **Triage Review Comments** | *"Check the review comments on CL 385134 and tell me what needs to be addressed."* | `./gh pr view 385134 --comments` | `curl`/`gob-curl` to `pigweed-review`, `.gitcookies`, Python parsers |
| **2** | **Perform Code Review** | *"Please review pwrev/385134 and leave draft comments for any issues you find."* | `./gh pr view 385134`, `./gh pr diff 385134`, `./gh pr comment 385134 --path <file> --line <line> -m "..." --draft` | Posting live comments without `--draft` when asked for drafts |
| **3** | **Address Feedback & Resolve** | *"Fix the review comments on the current branch, push a new patchset, and resolve the threads."* | `./pw presubmit --mode auto --base origin/main`, `git commit --amend`, `./gh pr push`, `./gh pr comment --path <file> --line <line> -m "..." --resolved` | `git push origin HEAD:refs/for/...`, `./gh pr create` on an existing CL |
| **4** | **CI Triage & Targeted Rerun** | *"Check why CI failed on CL 385134 and rerun the failed builders."* | `./gh pr checks 385134`, `./gh run view 385134 --log-failed` (or `-j <builder>`), `./gh run rerun 385134 --failed` | `search_builds.py`, manual `bb add`, polling `./gh pr checks` in a loop |
| **5** | **Create New CL & Dry Run** | *"Create a new CL from my local commit, add keir@google.com as reviewer, and start a CQ dry run."* | `./gh pr create -r keir@google.com --cq` | Raw `git push`, `./gh pr push` on uncreated CL |
| **6** | **Multi-Commit Stack Guard** | *(2 local commits ahead of `origin/main`)* *"Push my changes to Gerrit."* | Runs `./gh pr create` or `./gh pr push`, hits stack guard, asks user whether to pass `--stack` or squash | Blindly passing `--stack` without asking, or bypassing via raw `git push` |
| **7** | **Target Branch Memory** | *(CL targets `sandbox/experiment`)* *"Amend the commit and push a new patchset."* | `./gh pr push` (auto-routes to `refs/for/sandbox/experiment`) | Pushing to `refs/for/main` |
| **8** | **Buganizer Integration** | *"File a Buganizer issue for this fix, link it to the commit, and push."* | `./gh issue create -t "..." -b "..." --amend` (or `./gh pr edit --bug b/<id>`), `./gh pr push` | Writing GitHub `Fixes #123` instead of `Bug: b/<id>` |

---

## 3. Grading Checklist

A test session **PASSES** if and only if:
- [ ] **Zero raw `curl` or `gob-curl` calls** are made to `pigweed-review.googlesource.com` or `cr-buildbucket.appspot.com`.
- [ ] **Zero raw `git push` calls** are executed.
- [ ] **Zero bespoke Python/jq scripts** are written to parse Gerrit `)]}'` JSON prefixes.
- [ ] The agent correctly distinguishes `./gh pr create` (new CLs) from `./gh pr push` (new patchsets on existing CLs).
- [ ] Review threads are explicitly resolved with `--resolved` after fixes are pushed.
