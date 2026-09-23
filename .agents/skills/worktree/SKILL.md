---
name: Multi-Agent Worktree & Bazel Cache Management
description: Instructions for managing warm Git worktree slots, logical project symlinks (~/wrk/projects/<project>), shared Bazel caches, and zero-click Jetski IDE projects using `./gh wt`.
---

# Multi-Agent Worktree & Bazel Cache Management (`./gh wt`)

Pigweed developers and AI agents use `./gh wt` (`pw_ghish/worktree`) to juggle multiple concurrent workstreams without Git branch clobbering or cold Bazel rebuilds.

## Core Concepts

1. **Fixed Warm Slots (`~/wrk/slots/pw-01..N`):**
   Physical Git worktrees live at fixed paths so their Bazel MD5 `output_base` hashes never change. Every slot keeps its own warm Bazel JVM server and Skyframe analysis graph.
2. **Logical Project Symlinks (`~/wrk/projects/<project>`):**
   Agents and developers work inside `~/wrk/projects/<project>`, which is a POSIX symlink pointing to the currently mounted slot (e.g., `~/wrk/slots/pw-03`).
3. **Orthogonal Residency (`MOUNTED` vs. `PARKED`):**
   - `MOUNTED`: Occupies a warm slot in `~/wrk/slots/pw-XX` and appears as an active project in the Jetski left sidebar.
   - `PARKED`: Shelved in Git (`refs/heads/<branch>`) and Gerrit (`pwrev/XXX`), consuming **0 disk slots** and archived in the Jetski sidebar. Supports juggling $M$ projects on $N$ slots ($M > N$) via automatic LRU swap-out of clean/unleased slots.
4. **Never Manually Edit `~/.bazelrc`:**
   All shared Bazel cache settings (`~/.config/pw_ghish/bazelrc.worktrees` and the `try-import` line in `~/.bazelrc`) are managed exclusively by `./gh wt init`. **Agents must NEVER manually edit or delete `~/.bazelrc`.**

---

## Agent Protocol: Responding to "Project `<name>`: ..."

When the user instructs you to work on a specific project (e.g., *"Project rpc-fix: investigate the buffer overflow"* or *"Switch to project sensor-driver"*):

1. **Allocate or Resume the Project Slot:**
   Run `./gh wt use <project> --json`:
   ```bash
   ./gh wt use <project> --json
   ```
   Example JSON output:
   ```json
   {
     "project": "rpc-fix",
     "slot": "pw-02",
     "slot_path": "/usr/local/google/home/keir/wrk/slots/pw-02",
     "symlink_path": "/usr/local/google/home/keir/wrk/projects/rpc-fix",
     "branch": "rpc-fix",
     "mode": "write"
   }
   ```
   - If another agent holds an active write lease on `<project>`, `./gh wt use` automatically **warm-forks** to `<project>-sub1` in a free slot to prevent Git index collisions.
   - If all slots are occupied, `./gh wt use` automatically **LRU-parks** the oldest idle, clean (`!DIRTY`) project to free a warm slot.

2. **Execute All Commands Inside `symlink_path`:**
   Use the returned `symlink_path` (`/usr/local/google/home/keir/wrk/projects/<project>`) as the `Cwd` for all subsequent `run_command` and file editing tool calls.

---

## Common CLI Workflows

### 1. Check Environment Health & Initialize Pool
```bash
# Read-only health check of slots, hooks, Bazel caches, and IDE sync:
./gh wt init --check

# Idempotently create/repair slots and configure shared Bazel caches:
./gh wt init --slots 10
```

### 2. Dashboard of Active & Parked Workstreams
```bash
# View live dashboard with Gerrit review/CI badges (🔥 NEEDS_ATTENTION, 🚀 READY_TO_LAND, etc.):
./gh wt list

# Machine-readable JSON dashboard:
./gh wt list --json
```

### 3. Persistent Project Next-CL Workflow (`CL_MERGED` -> `CLEAN_SYNCED`)
When a CL merges on a long-lived project (e.g. `bluetooth` or `bazel`), do **not** close the project if you intend to start another CL in the same area:
```bash
# Fetches origin and rebases the mounted slot onto origin/main in-place:
./gh wt next <project>
```

### 4. Shelving Idle Workstreams (`MOUNTED` -> `PARKED`)
To free up a physical slot while waiting on code review without losing any state:
```bash
./gh wt park <project>
```
*(Note: Dirty working trees with uncommitted edits are protected and cannot be parked without `--force`.)*

### 5. Permanently Closing Completed Workstreams
When a one-off bugfix or feature is completely done:
```bash
./gh wt close <project>
```

### 6. Cleaning Up Orphaned Bazel Output Bases
If old manual worktrees were deleted outside `./gh wt`:
```bash
# Dry-run inspection:
./gh wt gc --dry-run

# Remove orphaned output bases in ~/.cache/bazel/_bazel_$USER:
./gh wt gc
```
