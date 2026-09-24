.. _module-pw_ghish-cli-comparison:
.. _module-pw_ghish-flag-compatibility:

=====================
GitHub CLI comparison
=====================
.. pigweed-module-subpage::
   :name: pw_ghish

``pw_ghish`` (``./gh``) provides GitHub CLI (``gh``) syntax on top of Gerrit
code review, LUCI Buildbucket CI, Google Issue Tracker (Buganizer), and local
Git/Bazel worktree pools.

This page summarizes how each ``./gh`` command family maps to its backend
service, lists flags whose underlying Gerrit/LUCI/Buganizer meaning differs
from upstream ``gh``, and defines the flag compatibility policy.

----------------------------
Ecosystem translation matrix
----------------------------
Each top-level command family in ``./gh`` maps a GitHub workflow to its
counterpart in the Pigweed / Gerrit ecosystem:

.. list-table::
   :header-rows: 1
   :widths: 20 22 28 30

   * - Command Family
     - Upstream ``gh`` Target
     - ``pw_ghish`` (``./gh``) Target
     - Key Adaptations & Guide
   * - ``./gh pr``
     - GitHub Pull Requests
     - **Gerrit Change Lists (CLs)**
     - Commit-based ``Change-Id`` patchsets, ``pr push``, Commit-Queue /
       Auto-Submit voting, inline thread replies, and private drafts. See
       :ref:`module-pw_ghish-pr`.
   * - ``./gh run`` & ``./gh pr checks``
     - GitHub Actions Runs & Checks
     - **LUCI Buildbucket & Recipes**
     - CL-scoped tryjob defaults, collapsed recipe step trees, 1-line failure
       extractions, step failure log fetching, and ``bb add`` reruns. See
       :ref:`module-pw_ghish-run`.
   * - ``./gh issue``
     - GitHub Issues
     - **Google Issue Tracker (Buganizer)**
     - Structured ``priority:``/``severity:``/``type:``/``component:`` labels,
       ``Bug: b/<id>`` commit trailer linkage, and zero-arg branch issue
       inference. See :ref:`module-pw_ghish-issue`.
   * - ``./gh wt`` (``./gh worktree``)
     - *(None — gh-ish extension)*
     - **Local Git Worktrees & Bazel Cache Pool**
     - Warm physical build slots (``pw-01..N``), logical project symlinks, LRU
       parking, and Antigravity (Jetski) IDE sidebar sync. See
       :ref:`module-pw_ghish-worktree`.

------------------------------------
Values that mean something else here
------------------------------------
``pw_ghish`` does not reuse an upstream ``gh`` short flag for an unrelated
action: where a single-letter shorthand would collide with ``gh``, it is left
unbound so that the command fails with an unknown-flag error rather than
performing an unintended operation.

The table below lists the remaining places where a flag is spelled as it is in
upstream ``gh``, but the underlying Gerrit, LUCI, or Buganizer concept differs:

.. list-table::
   :header-rows: 1
   :widths: 28 30 42

   * - You type
     - Real ``gh``
     - ``pw_ghish`` (``./gh``)
   * - ``pr list -a / --assignee``
     - Filters by GitHub PR assignee.
     - Queries Gerrit ``reviewer:`` (Gerrit removed assignees in 3.8).
   * - ``pr list -l / --label``
     - Filters by GitHub issue label.
     - Queries a Gerrit **vote predicate**, such as ``Code-Review+2``.
   * - ``issue -l / --label``
     - Free-form text label (e.g. ``bug``).
     - Structured Buganizer field: ``priority:P0..P4``, ``severity:S0..S4``,
       ``type:BUG|FEATURE|TASK``, ``component:<id>``, or ``hotlist:<id>``.
   * - ``run list --json`` / ``run view --json``
     - Requires a field list (``--json <fields>``).
     - Boolean switch; takes no field list (``pr view --json`` and
       ``issue view --json`` do take field lists).
   * - ``--json state``
     - ``OPEN`` / ``CLOSED`` / ``MERGED``
     - Gerrit's ``NEW`` / ``MERGED`` / ``ABANDONED`` (on ``pr``) or Buganizer
       status (on ``issue``).
   * - ``pr review --request-changes``
     - Blocks the pull request from merging.
     - Votes ``Code-Review-1``, which is advisory and does **not** block
       submission. ``Code-Review-2`` is the veto.
   * - ``pr comment --draft``
     - n/a (``--draft`` is on ``pr create``)
     - Creates an unpublished server-side **draft comment** visible only to you
       until published.

Unbound shorthands and out-of-scope commands
============================================
* **Long-form only flags**: Several flags intentionally have no single-letter
  shorthand because upstream ``gh`` binds that letter to another meaning:

  * ``--auto`` (in ``gh``, ``-a`` is ``--assignee``)
  * ``--publish`` (in ``gh``, ``-p`` is ``--project``)
  * ``--force`` (in ``gh``, ``-f`` is ``--fill``)
  * ``--cq`` (in ``gh``, ``-q`` is ``--jq``)
  * ``--message`` on ``pr edit`` and ``pr merge`` (in ``gh``, ``-m`` is
    ``--milestone`` and ``--merge``)

* **Not implemented**: ``--jq``/``-q`` as an output filter, ``gh api``,
  ``gh auth status``, ``-R/--repo``, ``gh release``, ``gh gist``, and
  ``pr merge --squash/--rebase/--delete-branch`` (Gerrit submits a whole
  change, and the merge strategy is a repository setting rather than a
  per-change choice).

---------------------------------
Flag compatibility policy & tiers
---------------------------------
All flags in ``pw_ghish`` follow four rules:

1. **Semantic alignment for upstream flags**: When a flag exists in upstream
   GitHub CLI (e.g. ``--web``, ``--undo``, ``--comments``, ``--json``,
   ``--base``, ``--limit``, ``--state``), ``pw_ghish`` provides the same
   user-facing behavior or a direct Gerrit/LUCI/Buganizer equivalent.
2. **No silent divergence**: If an upstream flag cannot be supported safely in
   Gerrit, ``pw_ghish`` rejects it with an explicit error explaining the
   limitation rather than ignoring it.
3. **Non-intersecting flags for ecosystem-specific concepts**: When adding flags
   for Gerrit, LUCI, or Buganizer mechanics with no GitHub analogue (e.g.
   Commit-Queue voting, thread resolution state, server-side draft comments,
   commit trailer amending), ``pw_ghish`` chooses non-intersecting flag names.
4. **Explicit designation of ghish-only flags**: Ecosystem-specific flags are
   marked as **ghish-only** in help text and documentation.

Tier 1: Upstream equivalent flags
=================================
These flags match upstream ``gh`` in syntax, type, and behavior:

.. list-table::
   :widths: 22 22 56
   :header-rows: 1

   * - Command
     - Flag
     - Behavior
   * - ``pr view`` / ``issue view``
     - ``-w, --web``
     - Opens the Gerrit change or Buganizer issue in the web browser.
   * - ``pr view`` / ``issue view``
     - ``-c, --comments``
     - Displays inline review threads (``pr``) or issue comments (``issue``).
   * - ``pr view`` / ``issue view``
     - ``--json <fields>``
     - Outputs JSON validated against the supported field schema.
   * - ``pr ready``
     - ``-u, --undo``
     - Marks the Gerrit change as draft / work-in-progress (WIP).
   * - ``pr list`` / ``issue list``
     - ``-s, --state``
     - Filters changes or issues by state (``open``, ``closed``, ``all``).
   * - ``pr list`` / ``issue list``
     - ``-L, --limit``
     - Limits the number of returned items.
   * - ``pr list`` / ``pr create``
     - ``-B, --base``
     - Filters or targets a specific base branch.
   * - ``pr merge``
     - ``--auto``
     - Enables automated submission once CI and review requirements pass.
   * - ``pr diff``
     - ``--patch``
     - Outputs raw patch diff suitable for ``git apply``.
   * - ``pr checks``
     - ``-w, --watch``
     - Polls LUCI CI checks until all blocking builders complete.
   * - ``run view``
     - ``--log-failed``
     - Fetches and displays failure summaries and step log snippets for failed
       steps.
   * - ``run rerun``
     - ``--failed``
     - Reruns all failed builders on the active change.

Tier 2: Upstream compatible extensions
======================================
These flags extend standard ``gh`` commands consistent with GitHub CLI
conventions:

.. list-table::
   :widths: 22 22 56
   :header-rows: 1

   * - Command
     - Flag
     - Behavior
   * - ``pr ready``
     - ``-m, --message``
     - Adds an optional status message when marking ready or moving to WIP.
   * - ``pr comment``
     - ``--path, --line``
     - Inline file path and line location for threaded code review comments.
   * - ``pr push`` / ``pr create``
     - ``--stack``
     - Permits pushing multiple local commits as a stacked Gerrit change series.
   * - ``pr checks``
     - ``--fail-fast``
     - Exits ``--watch`` upon the first blocking check failure and prints step
       failure logs.
   * - ``issue develop``
     - ``-w, --worktree``
     - Allocates a warm slot in ``./gh wt`` instead of switching branches in
       place.

Tier 3: Gerrit, LUCI, and Buganizer native flags (ghish-only)
=============================================================
These flags control Gerrit, LUCI, or Buganizer mechanics and avoid collisions
with upstream GitHub CLI flags:

.. list-table::
   :widths: 22 22 56
   :header-rows: 1

   * - Command
     - Flag
     - Behavior
   * - ``pr push`` / ``review`` / ``merge``
     - ``--cq [vote]``
     - Triggers LUCI Commit-Queue validation (``1`` = dry run, ``2`` = submit).
   * - ``pr push`` / ``merge``
     - ``--auto-submit``
     - Votes the host's auto-submit label (e.g. ``Pigweed-Auto-Submit+1``,
       ``Auto-Submit+1``).
   * - ``pr push``
     - ``--publish``
     - Publishes pending server-side draft comments when uploading a patchset.
   * - ``pr push`` / ``create``
     - ``-o, --push-option``
     - Passes raw Git push options (e.g. ``-o topic=my-feature``).
   * - ``pr comment``
     - ``--draft``
     - Saves an inline comment as a private server-side draft.
   * - ``pr comment``
     - ``--resolved``
     - Marks an inline Gerrit comment thread as resolved.
   * - ``pr edit``
     - ``--bug, --fixed``
     - Sets or updates ``Bug: b/<id>`` or ``Fixed: b/<id>`` commit trailers.
   * - ``pr edit``
     - ``--topic, --hashtag``
     - Sets the Gerrit topic string or adds/removes Gerrit hashtags.
   * - ``pr checks`` / ``run list``
     - ``-e, --experimental``
     - Includes non-blocking ``cq_experimental`` LUCI builders in output.
   * - ``issue create``
     - ``--amend, --commit``
     - Amends ``HEAD`` (or creates a commit) with ``Bug: b/<new-id>``.
   * - ``issue close``
     - ``--duplicate-of <id>``
     - Marks a Buganizer issue as a duplicate of another issue ID.

--------------------------
Adding new flags in gh-ish
--------------------------
When adding or proposing a new flag to ``pw_ghish``:

1. **Check official GitHub CLI reference**: Consult ``gh help <command>`` to see
   if an official flag already exists for the desired functionality. If it
   exists, adopt the same flag name, short option, and value format.
2. **Verify non-intersection**: If introducing a Gerrit, LUCI, or Buganizer
   feature, verify that the proposed flag name and shorthand do not collide
   with current or planned ``gh`` flags.
3. **No silent no-ops**: Never accept a flag without implementing its behavior.
   If a flag cannot be supported, return an explicit error.
