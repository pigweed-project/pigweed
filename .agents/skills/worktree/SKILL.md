---
name: Multi-Agent Worktree & Bazel Cache Management
description: Instructions for managing warm Git worktree slots, logical project symlinks (~/wrk/projects/<project>), shared Bazel caches, and Jetski IDE workspace synchronization using `./gh wt`.
---

# Multi-Agent Worktree & Bazel Cache Management (`./gh wt`)

Pigweed developers and AI agents use `./gh wt` (`pw_ghish/worktree`) to juggle multiple concurrent workstreams without Git branch clobbering or cold Bazel rebuilds.

## Core Concepts

1. **Fixed Warm Slots (`~/wrk/slots/<slot_prefix>01..N`):**
   Physical Git worktrees live at fixed paths (default prefix `pw-`, configurable via `[worktree] slot_prefix` in `.ghish.toml` or derived from the repository name) so their Bazel MD5 `output_base` hashes never change. Every slot keeps its own warm Bazel JVM server and Skyframe analysis graph (or operates with `warmup_driver = "none"` in non-Bazel repositories).
2. **Logical Project Symlinks (`~/wrk/projects/<project>`):**
   Agents and developers work inside `~/wrk/projects/<project>`, which is a POSIX symlink pointing to the currently mounted slot (e.g., `~/wrk/slots/pw-03`).
3. **Orthogonal Residency (`MOUNTED` vs. `PARKED`):**
   - `MOUNTED`: Occupies a warm slot in `~/wrk/slots/<slot_prefix>XX` and appears as an active project in the Jetski left sidebar.
   - `PARKED`: Shelved in Git (`refs/heads/<branch>`) and Gerrit (`pwrev/XXX` or configured shortlink), consuming **0 disk slots** and archived in the Jetski sidebar. Supports juggling $M$ projects on $N$ slots ($M > N$) via automatic LRU swap-out of clean/unleased slots.
4. **Never Manually Edit `~/.bazelrc`:**
   All shared Bazel cache settings (`~/.config/pw_ghish/bazelrc.worktrees` and the `try-import` line in `~/.bazelrc`) are managed exclusively by `./gh wt init`. **Agents must NEVER manually edit or delete `~/.bazelrc`.**

---

## Agent Protocol: Responding to "Project `<name>`: ..."

When the user instructs you to work on a specific project (e.g., *"Project rpc-fix: investigate the buffer overflow"* or *"Switch to project sensor-driver"*):

1. **Allocate or Resume the Project Slot:**
   Run `./gh wt use <project> --json` (or allocate directly from a Buganizer issue via `./gh wt use --issue <id> --json` / `./gh issue develop <id> --worktree`):
   ```bash
   ./gh wt use <project> --json
   # Or from a Buganizer issue ID:
   ./gh wt use --issue 315378787 --json
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
   - If another agent holds an active write lease on `<project>`, `./gh wt use` automatically **warm-forks** to `<project>-fork-1` in a free slot to prevent Git index collisions.
   - If the branch `<project>` is already checked out in an unmanaged worktree (e.g., `~/wrk/pw-ghish` or `~/wrk/pigweed`) or has unmerged local commits from an earlier session, `./gh wt use` automatically creates `<project>-wt` at `origin/main` without touching the unmanaged worktree.
   - If all slots are occupied, `./gh wt use` automatically **LRU-parks** the oldest idle, clean (`!DIRTY`) project to free a warm slot.

2. **Strict One-Shot Rule — NEVER Run Manual `git worktree` Commands:**
   - **NEVER** run `git worktree add`, `git worktree remove`, `git worktree list`, or `git checkout --detach` in other directories to work around branch collisions. `./gh wt use <project>` is a **single one-shot command** that handles slot allocation, unmanaged worktree branch collisions, and stale local branches automatically.

3. **Execute All Commands Inside `symlink_path`:**
   Use the returned `symlink_path` (`/usr/local/google/home/keir/wrk/projects/<project>`) as the `Cwd` for all subsequent `run_command` and file editing tool calls.

---

## Common CLI Workflows

```bash
./gh wt init --check         # Read-only health check of slots, hooks, unmanaged worktrees, Bazel caches, and IDE sync
./gh wt init --slots 10      # Idempotently create/repair slots and configure shared Bazel caches
./gh wt use <project>        # One-shot allocate/resume slot, auto-resolving any unmanaged worktree branch collisions
./gh wt list [--json]        # Live dashboard with Gerrit review/CI badges (🔥 NEEDS_ATTENTION, 🚀 READY_TO_LAND)
./gh wt next [<project>]     # After a CL merges: fetch origin & rebase mounted slot onto origin/main in-place
./gh wt park [<project>]     # Shelve clean mounted slot to PARKED (0 disk slots; dirty trees require --force)
./gh wt close <project>      # Permanently close a completed workstream
./gh wt gc [--dry-run]       # Remove orphaned Bazel output bases in ~/.cache/bazel/_bazel_$USER
```

