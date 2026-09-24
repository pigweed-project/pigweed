.. _module-pw_ghish-worktree:
.. _module-pw_ghish-worktrees:

=================
Worktrees (gh wt)
=================
.. pigweed-module-subpage::
   :name: pw_ghish

.. warning::

   **VERY EXPERIMENTAL**: Worktree management (``./gh wt`` / ``./gh worktree``)
   is **very experimental** and under active development. Slot pooling,
   directory layouts, Bazel cache rules, and IDE workspace synchronization may
   change significantly based on usage and feedback.

``./gh wt`` (also available as ``./gh worktree``) is a ``pw_ghish`` extension
that automates **Git worktree pooling, shared Bazel caches, and IDE workspace
synchronization** for humans and parallel AI coding agents.

When you work with multiple AI coding agents or juggle several Gerrit changes in
parallel, managing Git checkouts in a large Bazel repository quickly becomes
tedious:

* Running multiple agents in a single checkout clobbers your Git index, staged
  files, and working tree.
* Creating traditional ``git worktree`` directories at arbitrary paths is slow
  and triggers a cold Bazel build from scratch for every new worktree, consuming
  hundreds of gigabytes of disk space.
* Manually creating, configuring, and cleaning up IDE workspaces for each task
  adds friction to every context switch.

With ``./gh wt``, you can spin up isolated project directories in seconds, run
fast incremental builds backed by a shared pool of warm build slots, and
synchronize workspaces automatically with your IDE.

.. mermaid::

   flowchart LR
       subgraph ActiveProjects ["Logical Projects (~/wrk/projects/)"]
           P1["rpc-buffer-fix"]
           P2["sensor-driver"]
       end

       subgraph SlotPool ["Warm Physical Slots (~/wrk/slots/)"]
           S1["pw-01 (Warm Bazel Server)"]
           S2["pw-02 (Warm Bazel Server)"]
           S3["pw-03 (Available)"]
       end

       subgraph SharedCache ["Shared Bazel Caches (~/.cache/)"]
           RC["Repository Cache (Hardlinks)"]
           DC["80 GB Auto-GC Disk Cache"]
       end

       subgraph ParkedProjects ["Parked Projects (0 Disk Slots)"]
           PP1["bazel-cleanup (Git Branch + Gerrit CL)"]
       end

       P1 -->|symlink| S1
       P2 -->|symlink| S2
       S1 --> SharedCache
       S2 --> SharedCache
       S3 --> SharedCache
       PP1 -.->|swap in on demand| S3

---------------------------
Setup & Quickstart Tutorial
---------------------------
This tutorial walks through setting up your worktree pool and running concurrent
projects with AI agents.

1. Initialize Your Environment
==============================
Run ``./gh wt init`` once from your primary repository checkout to create your
pool of warm worktree slots, install the Gerrit ``commit-msg`` hook, and
configure your shared Bazel cache snippet:

.. code-block:: console

   $ ./gh wt init
   Inspecting & Converging gh-ish Worktree Environment...
   ======================================================

   [✓] Git Primary Repo:        /home/user/wrk/pigweed
   [✓] Gerrit commit-msg Hook:  Checked in primary repository
   [+] Worktree Slot Pool:      Created 10/10 slots in /home/user/wrk/slots
   [✓] Project Symlinks Dir:    /home/user/wrk/projects
   [✓] Antigravity UI Sync:     Ready (using default v2 permission schema template)
   [+] Bazel Config Snippet:    Active at ~/.config/pw_ghish/bazelrc.worktrees
   [+] User ~/.bazelrc Hook:    Added try-import to ~/.bazelrc
   [✓] Bazel Output Bases:      0 orphaned output bases found

   Environment is healthy and ready!

2. Spin Up Your First Project
=============================
Use ``./gh wt use <project>`` to start a new task:

.. code-block:: console

   $ ./gh wt use rpc-buffer-fix
   ✓ Project "rpc-buffer-fix" mounted in slot pw-01 (branch: rpc-buffer-fix)
     Directory: /home/user/wrk/projects/rpc-buffer-fix

This command:

1. Allocates warm physical slot ``pw-01`` and checks out branch
   ``rpc-buffer-fix`` tracking ``origin/main``.
2. Creates the symbolic link ``~/wrk/projects/rpc-buffer-fix`` pointing to
   ``pw-01``.
3. If you use Antigravity (known internally at Google as Jetski), registers
   ``rpc-buffer-fix`` in your left sidebar automatically.

3. Run a Second Agent in Parallel
=================================
While your first agent builds and tests inside
``~/wrk/projects/rpc-buffer-fix``, you can launch a second project without
waiting or clobbering files:

.. code-block:: console

   $ ./gh wt use sensor-driver
   ✓ Project "sensor-driver" mounted in slot pw-02 (branch: sensor-driver)
     Directory: /home/user/wrk/projects/sensor-driver

Because ``pw-01`` and ``pw-02`` share a deduplicated repository cache and an
80 GB disk cache, the second slot reuses downloaded dependencies and compiled
artifacts immediately.

4. Monitor All Workstreams
==========================
Run ``./gh wt list`` at any time to see a live dashboard combining local Git
status with Gerrit review and CI checks:

.. code-block:: console

   $ ./gh wt list
   MOUNTED PROJECTS (2/10 Slots Occupied, 8 Available)
   =====================================================
   PROJECT          SLOT   STATUS              GERRIT CL & DETAILS                             RECOMMENDED ACTION
   rpc-buffer-fix   pw-01  🔥 NEEDS_ATTENTION  pwrev/471888 (CR:+1, 2 threads, 0 failing)      Inspect via `./gh pr view --comments`
   sensor-driver    pw-02  ✨ CLEAN_SYNCED     Synced with origin/main                         Ready for hacking

5. Shelve or Advance Projects
=============================
When you are waiting on code review and want to free a slot without losing your
branch or Gerrit tracking, park the project:

.. code-block:: console

   $ ./gh wt park rpc-buffer-fix
   💤 Parked project "rpc-buffer-fix" (slot freed; branch and Gerrit CL remain tracked)

When a CL merges on a long-lived topic project and you are ready to start the
next CL in the same area, rebase the slot onto ``origin/main`` in place:

.. code-block:: console

   $ ./gh wt next sensor-driver
   ✨ Rebased project "sensor-driver" onto origin/main in-place! Ready for next CL.

--------------
CLI User Guide
--------------
The ``./gh wt`` (or ``./gh worktree``) command tree manages the lifecycle of
slots and projects.

Initializing and Inspecting the Environment
===========================================
Run ``./gh wt init`` to configure the slot pool, install the Gerrit
``commit-msg`` hook in the primary repository, and configure the shared Bazel
cache snippet:

.. code-block:: console

   # Perform a read-only diagnostic check of the environment:
   $ ./gh wt init --check

   # Initialize or repair the environment with 10 physical slots:
   $ ./gh wt init --slots 10

Mounting or Resuming a Project
==============================
Use ``./gh wt use [<project>]`` to allocate a warm slot for a new or parked
project:

.. code-block:: console

   # Mount a project (creates branch if needed and updates ~/wrk/projects/<name>):
   $ ./gh wt use rpc-buffer-fix

   # Mount a project linked to a Buganizer issue (auto-slugs project & branch name):
   $ ./gh wt use --issue 315378787

   # Shorthand equivalent using Buganizer prefix:
   $ ./gh wt use b/315378787

   # Mount a project associated with an existing Gerrit CL:
   $ ./gh wt use sensor-driver --cl 477945

   # Return machine-readable JSON metadata for automated agent scripts:
   $ ./gh wt use rpc-buffer-fix --json

Viewing the Project Dashboard
=============================
Run ``./gh wt list`` to inspect both ``MOUNTED`` and ``PARKED`` projects along
with their live Git working tree status, linked Buganizer issue IDs, and Gerrit
code review state:

.. code-block:: console

   $ ./gh wt list
   MOUNTED PROJECTS (2/10 Slots Occupied, 8 Available)
   =====================================================
   PROJECT          SLOT   STATUS              GERRIT CL & DETAILS                                     RECOMMENDED ACTION
   rpc-buffer-fix   pw-01  🔥 NEEDS_ATTENTION  b/315378787 • pwrev/471888 (CR:+1, 2 threads, 0 failing)  Inspect via `./gh pr view --comments`
   sensor-driver    pw-02  ✨ CLEAN_SYNCED     Synced with origin/main                                 Ready for hacking

   PARKED PROJECTS (1 Shelved in Git/Gerrit — 0 Slots Used)
   =========================================================
   PROJECT          SLOT   STATUS              GERRIT CL & DETAILS                                     RECOMMENDED ACTION
   bazel-cleanup    -      🚀 READY_TO_LAND    pwrev/470111 (CR+2, CQ ready)                           Approved! Ready to land (`./gh pr merge --cq`)

Status Badges
-------------
``./gh wt list`` classifies projects using six status badges:

.. list-table::
   :header-rows: 1

   * - Badge
     - Condition
     - Recommended Action
   * - ``✨ CLEAN_SYNCED``
     - Clean working tree with zero commits ahead of ``origin/main``.
     - Ready for new work.
   * - ``✎ LOCAL_WIP``
     - Uncommitted edits or local commits not yet uploaded to Gerrit.
     - Commit changes or upload via ``./gh pr create``.
   * - ``⏳ IN_REVIEW``
     - Open Gerrit CL awaiting reviewer feedback or CI completion.
     - Safe candidate to shelve via ``./gh wt park``.
   * - ``🔥 NEEDS_ATTENTION``
     - Open Gerrit CL with unresolved comment threads, negative CR score, or
       failing CI checks.
     - Inspect feedback via ``./gh pr view --comments`` or ``./gh pr checks``.
   * - ``🚀 READY_TO_LAND``
     - Open Gerrit CL with ``Code-Review+2`` and no blocking threads or failures.
     - Submit via ``./gh pr merge --cq``.
   * - ``🎉 CL_MERGED``
     - Associated Gerrit CL has been merged into ``origin/main``.
     - Run ``./gh wt next`` to rebase for the next CL, or close the project.

Integration with Buganizer Issues (gh issue)
============================================
``./gh wt`` integrates with :ref:`module-pw_ghish-issue` to link local worktree
slots with Buganizer issues:

* **Issue-driven slot allocation**: Running ``./gh wt use --issue 315378787``
  (or ``./gh issue develop 315378787 --worktree``) fetches the issue title from
  Buganizer, derives a clean slug for both the project symlink and Git branch
  (such as ``b-315378787-fix-channel-framing``), mounts a warm slot, and
  persists the issue ID in the worktree metadata.
* **Zero-commit context resolution**: Inside a mounted worktree project,
  commands such as ``./gh issue view``, ``./gh issue comment``, and
  ``./gh issue close`` automatically infer the target issue ID from the branch
  name or worktree metadata even before any Git commits or ``Bug:`` trailers
  exist on ``HEAD``.
* **Cross-tool visibility**: ``./gh wt list`` displays linked issue IDs
  (``b/<id>``) alongside Gerrit CL details, while ``./gh issue status`` displays
  ``[📂 MOUNTED: pw-XX]`` or ``[💤 PARKED]`` badges next to issues with active
  worktrees.
* **Lifecycle reminders**: When you close a project via ``./gh wt close``, if
  an associated Buganizer issue is still open, ``./gh wt`` prints a reminder to
  resolve the issue via ``./gh issue close <id>``.

Rebasing Persistent Projects for the Next CL
============================================
For long-lived topic areas where you develop multiple sequential CLs, you do
not need to close a project after its CL merges. Run ``./gh wt next`` to fetch
``origin`` and rebase your mounted slot onto ``origin/main`` in place:

.. code-block:: console

   $ ./gh wt next rpc-buffer-fix

Parking and Closing Projects
============================
To explicitly free a physical slot while preserving your branch and Gerrit CL
tracking entry in ``./gh wt list``, park the project:

.. code-block:: console

   $ ./gh wt park rpc-buffer-fix

When a workstream is permanently complete, close the project to remove its
symbolic link and tracking entry:

.. code-block:: console

   $ ./gh wt close rpc-buffer-fix

Cleaning Orphaned Bazel Output Bases
====================================
If you manually delete unmanaged Git worktrees outside the slot pool, their
Bazel output bases in ``~/.cache/bazel/_bazel_$USER/`` may remain on disk. Run
``./gh wt gc`` to identify and remove output bases whose workspace directories
no longer exist on disk:

.. code-block:: console

   # Preview orphaned output bases without deleting files:
   $ ./gh wt gc --dry-run

   # Remove orphaned output bases:
   $ ./gh wt gc

---------------------------
Antigravity IDE Integration
---------------------------
When you run ``./gh wt`` on a host environment with **Antigravity** (known
internally at Google as **Jetski**), the tool automatically synchronizes
project workspaces with your IDE sidebar. ``./gh wt`` detects configuration
directories at ``~/.gemini/config/projects/``,
``~/.antigravity/config/projects/``, or ``~/.config/antigravity/projects/``.

* **Automatic sidebar registration**: Running ``./gh wt use <project>``
  generates a deterministic UUID v5 configuration file in your active
  Antigravity projects directory pointing to ``~/wrk/projects/<project>``. If
  the project is linked to a Buganizer issue, the sidebar entry is titled
  ``pw: b/<id> - <name>``. The IDE server detects file updates via ``fsnotify``
  and adds the project to your sidebar immediately.
* **Lifecycle archival**: When a project transitions to ``PARKED`` (via manual
  ``./gh wt park`` or automatic LRU eviction) or is closed via
  ``./gh wt close``, ``./gh wt`` sets ``"archived": true`` in its project
  configuration file. This removes inactive projects from your active sidebar
  while retaining access to your past agent conversation transcripts.
* **Schema preservation and safety**: When updating project JSON files, the IDE
  driver clones unknown JSON fields from existing entries to maintain forward
  compatibility. During ``./gh wt init``, a schema canary check verifies that
  the host server accepts the configuration format; if divergence is detected,
  IDE synchronization disables itself automatically without interrupting your
  CLI worktree operations.

--------------------------
External IDEs & CLI Agents
--------------------------
You can use ``./gh wt`` across standard Linux and macOS workstations,
third-party IDEs (such as VS Code, Neovim, Cursor, or Zed), and standalone CLI
agent harnesses.

* **Standard POSIX symbolic links**: Because ``~/wrk/projects/<project>`` is a
  standard filesystem symbolic link, you can open ``~/wrk/projects/<project>``
  directly in any editor, terminal multiplexer, or command-line agent without
  IDE-specific plugins.
* **Agent CLI protocol**: You or your automation scripts can pass the ``--json``
  flag to ``./gh wt use <project> --json`` to obtain structured paths
  (``slot_path``, ``symlink_path``, ``branch``, and ``mode``) and execute
  subsequent commands inside ``symlink_path``.
* **Optional IDE synchronization**: If no Antigravity project configuration
  directory is present on your host, the IDE driver automatically operates as a
  no-op. If you prefer to manage IDE workspaces manually on an Antigravity host,
  you can disable IDE synchronization explicitly by setting the environment
  variable ``GH_ISH_IDE_SYNC=0``.

-----------------------------------------
Comparison with GitHub CLI & git worktree
-----------------------------------------
Unlike ``gh pr``, ``gh run``, and ``gh issue``, ``./gh wt`` (``./gh worktree``)
has **no upstream GitHub CLI equivalent**—it is a purpose-built ``pw_ghish``
extension designed for multi-agent C++/Bazel repositories.

.. list-table::
   :header-rows: 1
   :widths: 25 25 25 25

   * - Capability
     - ``gh pr checkout``
     - Raw ``git worktree add``
     - ``./gh wt`` (``./gh worktree``)
   * - **Parallel agent isolation**
     - Mutates current checkout in place; clobbers index if shared.
     - Creates isolated directory per branch.
     - Creates isolated logical symlink ``~/wrk/projects/<name>`` backed by a
       warm physical slot.
   * - **Bazel build cache behavior**
     - Reuses single output base, but blocks concurrent builds.
     - **Cold output base per path** (Bazel hashes ``realpath()`` via MD5),
       costing tens of GBs and minutes per worktree.
     - **Warm output base pool**: Fixed physical slots (``pw-01..N``) keep Bazel
       analysis servers warm and share a 80 GB disk/repo cache.
   * - **Capacity & disk management**
     - Single directory.
     - Unbounded directories; orphaned Bazel output bases accumulate in
       ``~/.cache/bazel/``.
     - Bounded slot pool with automatic LRU ``PARKED`` eviction and
       ``./gh wt gc`` output-base garbage collection.
   * - **Gerrit & Buganizer awareness**
     - Checks out patchset commit only.
     - Unaware of Gerrit CLs or Buganizer issues.
     - Live ``./gh wt list`` status badges, ``--issue``/``--cl`` mounting, and
       zero-arg issue context resolution.

---------------------------
Architecture & How It Works
---------------------------
Under the hood, Bazel resolves symbolic links using ``realpath()`` before
computing the MD5 hash of the workspace path to select an output base directory
(``~/.cache/bazel/_bazel_$USER/<md5_of_realpath>/``). If you create a new
directory for every task, each directory receives a cold output base and
triggers a full analysis phase and rebuild.

Two-Layer Directory Model
=========================
To keep builds fast while giving each task an intuitive name, ``./gh wt``
decouples the physical workspace path from the semantic project name:

1. **Physical Worktree Pool (Slot Layer)**: A fixed pool of ``N`` Git worktrees
   (by default 10) resides at ``~/wrk/slots/pw-01`` through
   ``~/wrk/slots/pw-10``. Because the physical path ``~/wrk/slots/pw-XX`` is
   reused across tasks, its Bazel output base and analysis cache remain warm.
2. **Semantic Project Symlinks (Project Layer)**: Human-readable symbolic links
   reside at ``~/wrk/projects/<project-name>`` and point to the assigned
   physical slot (for example, ``~/wrk/projects/rpc-buffer-fix ->
   ~/wrk/slots/pw-03``).

Residency States: Mounted vs. Parked
====================================
A project managed by ``./gh wt`` is always in one of two residency states:

.. list-table::
   :header-rows: 1

   * - Residency State
     - Physical Slot
     - Symlink Target
     - Description
   * - ``MOUNTED``
     - Assigned (``pw-01`` .. ``pw-N``)
     - ``~/wrk/projects/<name> -> ~/wrk/slots/pw-XX``
     - Active on disk with a live Git worktree and warm Bazel output base.
   * - ``PARKED``
     - None (0 slots used)
     - ``~/wrk/projects/<name>.parked`` marker
     - Shelved in Git and Gerrit. Consumes zero slot capacity while remaining
       tracked in ``./gh wt list``.

Managing More Projects Than Physical Slots
------------------------------------------
You can track more active projects than you have physical slots configured (for
example, juggling 8 concurrent projects across 5 physical slots). When you run
``./gh wt use <project>`` and all ``N`` slots are occupied:

* ``./gh wt`` identifies mounted projects with clean working trees whose
  commits are already pushed or uploaded to Gerrit (such as projects in
  ``IN_REVIEW`` or ``CLEAN_SYNCED`` state).
* The least-recently-used safe candidate is automatically **parked**: its slot
  is reassigned to your incoming project, while its branch and Gerrit CL remain
  tracked under the ``PARKED`` section of ``./gh wt list``.
* **Dirty tree protection**: Projects with uncommitted local edits or unpushed
  commits are pinned and are never automatically evicted.

Shared Bazel Cache Configuration
================================
``./gh wt init`` writes a shared Bazel configuration file to
``~/.config/pw_ghish/bazelrc.worktrees`` and adds a conditional ``try-import``
to ``~/.bazelrc``:

.. code-block:: ini

   # Managed by ./gh wt init
   build --disk_cache=~/.cache/bazel-disk-cache
   build --repository_cache=~/.cache/bazel-repo-cache
   build --experimental_guard_against_concurrent_changes

This configuration allows all slots to share compiled object artifacts and
downloaded external repositories while preventing race conditions during
concurrent builds.
