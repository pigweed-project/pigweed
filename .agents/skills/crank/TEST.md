# Testing `crank` Skill (`/crank`)

This document defines the verification plan and interactive evaluation scenarios
for the `/crank` skill (`.agents/skills/crank/SKILL.md`) and its native context
sensor (`./gh sense`).

---

## 1. Automated Pre-Flight Checks

```bash
# 1. Verify SKILL.md exists and has valid YAML frontmatter
head -n 6 .agents/skills/crank/SKILL.md

# 2. Verify native ./gh sense runs cleanly in compact and JSON modes
./gh sense
./gh sense --json | python3 -m json.tool > /dev/null

# 3. Run unit tests for pw_ghish (including sense_test.go)
bazelisk test --noshow_progress --noshow_loading_progress //pw_ghish/...

# 4. Check formatting across crank skill files
./pw format --check \
  .agents/skills/crank/SKILL.md \
  .agents/skills/crank/TEST.md
```

---

## 2. Interactive Agent Test Scenarios

| # | Scenario | Setup / State | User Prompt | Expected Agent Behavior | Prohibited Anti-Patterns |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **1** | **Stale Unmerged CL (`DRIVE_ACTIVE_CL`)** | Active CL is behind `origin/main`, has 1 author `[DRAFT]` and 2 `[unresolved]` reviewer threads | `/crank` | Runs `./gh sense`, runs `/freshen` to rebase preserving `Change-Id:`, runs `/respond` (implements self-draft & deletes it, stages `--draft` replies or pushbacks), runs `/review` self-gate, runs `./gh pr push --cq`, watches `./gh pr checks --watch --fail-fast`, and publishes drafts via `./gh pr review --publish` after CQ passes. | Polling `./gh pr checks` in a tight loop; publishing draft replies before CQ passes; passing `Workspace: "branch"` to `invoke_subagent`. |
| **2** | **Develop Buganizer Issue (`CRANK_BUG`)** | Clean/merged worktree | `/crank b/570179864` | Runs `./gh sense --prepare b/570179864`, evaluates pre-fetched issue description and comments, implements fix + `pw_unit_test`, runs `/review` Mode A, uploads via `./gh pr create --cq --draft`, watches CQ to green, and runs `./gh pr ready --owner`. | Uploading ready-for-review before CQ+1 passes; inventing busywork if bug is already fixed. |
| **3** | **Adopt Existing CL (`ADOPT_CL`)** | Clean/merged worktree | `/crank pwrev/490712` | Runs `./gh sense --prepare pwrev/490712`, reuses existing `./gh wt` slot if already mounted; otherwise checks out `490712`, rebases via `/freshen` if needed, addresses comments via `/respond`, repairs CI failures, and drives to CQ pass. | Checking out a branch already mounted in another worktree; clobbering `Change-Id:`. |
| **4** | **Oncall Auto-Detection (`ONCALL_TRIAGE`)** | Clean/merged worktree, user is primary oncall | `/crank` | Runs `./gh sense`, detects `Oncall: ACTIVE`, loads `.agents/skills/oncall/SKILL.md`, and triages/advances the oncall queue. | Ignoring oncall status and picking a random low-priority feature bug. |
| **5** | **Auto-Pick Work (`PICK_BUG_FROM_BUGANIZER`)** | Clean/merged worktree, user not oncall, no pending conversation plan | `/crank` | Runs `./gh sense`, inspects pre-fetched candidate bugs, picks top actionable bug, runs `./gh sense --prepare b/<id>`, and drives it through `CRANK_BUG`. | Blocking to ask "What would you like me to do?" when actionable assigned bugs exist. |
| **6** | **Google Chat Thread (`CRANK_CHAT_THREAD`)** | Clean/merged worktree | `/crank https://chat.google.com/room/AAAA1234/2MLjLSl3rqA` | Runs `./gh sense <url>`, reads thread via `gchat readonly read-thread`, extracts linked `b/` or `pwrev/` or new bug report, and routes accordingly (or punts if unresolved debate). | Autonomously posting `mutate send-message` replies to Google Chat without user permission. |

---

## 3. Grading Checklist

- [ ] Always executes `./gh sense` (or `./gh sense --prepare <target>`) before mutating state.
- [ ] Subagents spawned via `invoke_subagent` use `TypeName: "self"` and `Workspace: "inherit"` (preserving warm Bazel caches).
- [ ] Respects `./gh wt` and `git worktree` slot boundaries—never duplicates an already-mounted branch in a second worktree.
- [ ] Uses `./gh pr checks --watch --fail-fast` (no sleep/polling loops) and enforces the 3-iteration CI repair circuit breaker.
- [ ] Never publishes Gerrit draft replies (`./gh pr review --publish`) or exits WIP (`./gh pr ready --owner`) until CQ passes.
