.. _module-pw_ghish-cli-comparison:

=====================
GitHub CLI Comparison
=====================
.. pigweed-module-subpage::
   :name: pw_ghish

``pw_ghish`` (``./gh``) provides GitHub CLI (``gh pr``) syntax on top of
Gerrit code review and LUCI CI infrastructure. Its design emphasizes
consistency with standard GitHub CLI commands while accommodating Gerrit's
conventions, including commit-based patchsets, multi-label scoring, and
Commit-Queue integration.

This document compares ``pw_ghish`` commands with ``gh pr`` and outlines
architectural differences between GitHub and Gerrit.

------------------------
Command comparison table
------------------------
Most ``pw_ghish`` commands share identical syntax with the GitHub CLI (``gh pr``),
translating directly to Gerrit and LUCI equivalents. Where Gerrit concepts
diverge from GitHub (such as patchset updates or tryjobs), ``pw_ghish`` provides
targeted adaptations.

.. list-table::
   :header-rows: 1
   :widths: 22 38 40

   * - Command
     - Gerrit / LUCI Behavior
     - Comparison with GitHub CLI (``gh pr``)
   * - ``pr view [<id>]``
     - Queries change metadata via Gerrit REST API. Displays change details,
       reviewers, attention set, and current scores.
     - Identical syntax. Accepts Gerrit change numbers, full URLs, shortlinks
       (such as ``pwrev/472267``), or infers the active change from the current
       branch. Supports ``--comments`` for inline threads and ``--json``.
   * - ``pr diff [<id>]``
     - Fetches and renders unified patch diff from Gerrit.
     - Identical syntax. Defaults to active change. Supports optional
       ``<id>/<patchset>`` syntax to inspect historical patchsets.
   * - ``pr checkout [<id>]``
     - Fetches change ref (``refs/changes/...``) from Gerrit and checks out
       ``FETCH_HEAD``.
     - Identical syntax. While GitHub checks out a branch head, ``pw_ghish``
       checks out the specific patchset commit and configures local tracking
       branch metadata.
   * - ``pr create``
     - Pushes local commit to Gerrit's virtual ref (``refs/for/<base>``) as a
       **new** change.
     - Matches ``gh pr create`` for initial submission. Unlike GitHub PRs
       (which map to Git branches), Gerrit requires a unique ``Change-Id`` footer.
       Halts with an error if the ``Change-Id`` already exists on Gerrit
       (directing to ``pr push``). Requires ``--stack`` if multiple unpushed
       commits are present.
   * - ``pr push`` (or ``./gh push``)
     - Pushes local commit to upload a **new patchset** on an existing Gerrit
       change.
     - **Gerrit adaptation** (replaces ``git push``). In GitHub, pushing to a
       branch automatically updates the PR. In Gerrit, new patchsets are
       uploaded to ``refs/for/<branch>``. Uses branch memory to prevent
       branch drift. Supports ``--auto``, ``--cq``, ``--ready``, and ``--publish``.
   * - ``pr edit [<id>]``
     - Updates change metadata on Gerrit or amends the local commit message.
     - Identical syntax. Preserves all Git commit trailers (``Change-Id:``,
       ``Bug:``, etc.). Supports ``--add-label`` (e.g. ``Commit-Queue=1``) and
       ``--add-reviewer``.
   * - ``pr review [<id>]``
     - Submits review scores to Gerrit via REST API.
     - Identical syntax. Maps ``--approve`` to ``Code-Review+2`` and
       ``--request-changes`` to ``Code-Review-1``. Can target a specific
       patchset.
   * - ``pr comment [<id>]``
     - Posts top-level or inline review comments on Gerrit.
     - Matches ``gh pr comment``, with Gerrit thread semantics: specifying
       ``--path`` and ``--line`` automatically threads replies to existing
       comments. Supports ``--resolved`` to resolve threads and ``--draft``
       to stage unpublished comments.
   * - ``pr checks [<id>]``
     - Queries LUCI Buildbucket tryjob statuses via lightweight pRPC.
       Supports ``--watch``, ``--fail-fast``, ``--interval``, and ``--experimental``.
     - Identical syntax to ``gh pr checks``. Displays LUCI tryjob builders,
       execution durations, status icons (``✓``, ``✕``, ``*``, ``?``), and direct
       Milo build links instead of GitHub Actions checks.
   * - ``run list [<id>]``
     - Lists all Buildbucket runs/tryjobs scheduled on a change.
     - Identical syntax to ``gh run list``. Supports ``--json``, ``--limit``,
       and ``--experimental``.
   * - ``run view [<id>]``
     - Renders a structured summary of checks on a change, or a detailed step
       execution tree for a specific job/builder (via ``-j <builder>`` or direct
       build ID). Automatically extracts 1-line failure diagnostics (formatting
       diffs, compilation errors) and provides ``--log-failed``, ``--log``
       (full log), and ``--web``.
     - Matches ``gh run view``. Parallels GitHub CLI's structured summary and
       job step inspection, bridging LUCI step logs and LogDog streams into the
       terminal without browser context switching.
   * - ``run rerun [<id>]``
     - Triggers builder retry on LUCI Buildbucket via ``bb add``.
     - Matches ``gh run rerun``. Supports ``--failed`` to rerun all failed
       checks, ``-j, --job <builder>`` to rerun a specific builder, and
       ``--dry-run``.
   * - ``run watch [<id>]``
     - Watches tryjobs on a change until all blocking checks complete.
     - Matches ``gh run watch``.
   * - Root-level aliases
     - ``./gh checks``, ``./gh run``, ``./gh view``, ``./gh diff``,
       ``./gh status``, and ``./gh push``.
     - Ergonomic shortcuts matching common developer muscle memory.
   * - ``pr merge [<id>]``
     - Submits change to the target branch via Gerrit REST API.
     - Matches ``gh pr merge``. In Gerrit, changes cannot merge without passing
       review and CI gates. Attempts immediate submit if gates pass, or sets
       ``--auto`` (Auto-Submit) and ``--cq`` (Commit-Queue+2). Reports clear
       diagnostics if gates block submission.
   * - ``pr status``
     - Displays status dashboard for the active branch, authored changes, and
       review requests.
     - Identical syntax. Displays active change review scores, CI status, and
       unresolved/draft comment indicators. Scopes lists to 30 days by default
       (expandable with ``--all``).
   * - ``pr list``
     - Lists open changes for the current repository from Gerrit.
     - Identical syntax. Supports filtering by ``--state``, ``--author``,
       ``--base``, and pagination via ``--limit``, plus structured output via
       ``--json``.
   * - ``pr close [<id>]``
     - Abandons the change in Gerrit.
     - Identical syntax (maps closing a pull request to abandoning a Gerrit
       change).
   * - ``pr reopen [<id>]``
     - Restores an abandoned change in Gerrit.
     - Identical syntax (maps reopening a pull request to restoring an abandoned
       Gerrit change).
   * - ``pr ready [<id>]``
     - Removes Work-In-Progress (WIP) status in Gerrit (or with ``-u, --undo``,
       marks change as WIP). Supports an optional status message via
       ``-m, --message``.
     - Matches ``gh pr ready``. Marks change ready for review. ``-u, --undo``
       converts the change back to draft (WIP).
   * - ``pr cherry-pick <id>``
     - Fetches a remote patchset ref from Gerrit and applies it to the current
       branch via ``git cherry-pick FETCH_HEAD``.
     - **Gerrit shorthand**. Simplifies testing or adopting changes without
       manually constructing Gerrit change refspecs.

--------------------------------------
Values that mean something else here
--------------------------------------
``pw_ghish`` does not re-use a ``gh`` shorthand for a different flag. Where a
spelling would collide, it is left unbound, so the mistake fails with an
unknown-flag error instead of doing the wrong thing quietly. What remains are
the places where a flag is spelled as it is in ``gh`` but the Gerrit concept
underneath is not the same.

.. list-table::
   :header-rows: 1
   :widths: 30 32 38

   * - You type
     - Real ``gh``
     - ``pw_ghish``
   * - ``pr list -a``
     - ``--assignee``
     - Queries Gerrit ``reviewer:``. Gerrit removed assignees in 3.8.
   * - ``pr list -l``
     - ``--label``, an issue label
     - A Gerrit **vote** predicate, such as ``Code-Review+2``.
   * - ``run list --json`` / ``run view --json``
     - A field list
     - A boolean; it takes no field list. ``pr view --json`` does take fields.
   * - ``--json state``
     - ``OPEN`` / ``CLOSED`` / ``MERGED``
     - Gerrit's ``NEW`` / ``MERGED`` / ``ABANDONED``.
   * - ``pr review --request-changes``
     - Blocks the pull request
     - Votes ``Code-Review-1``, which is advisory and does **not** prevent
       submission. ``Code-Review-2`` is the veto.
   * - ``pr comment --draft``
     - n/a
     - An unpublished draft **comment**, visible only to you until published.
       It is not a comment on a work-in-progress change.

.. note::

   Several shorthands are deliberately **not** bound, because ``gh`` gives them
   another meaning: ``-a`` (``gh``: ``--assignee``), ``-p`` (``--project``),
   ``-f`` (``--fill``), ``-q`` (``--jq``), and ``-m`` on ``pr edit`` and
   ``pr merge`` (``--milestone`` and ``--merge``). Use the long form instead:
   ``--auto``, ``--publish``, ``--force``, ``--cq``, ``--message``. Do not
   re-bind them; a ``gh`` habit must fail loudly rather than succeed with the
   wrong meaning. For details on how flags are classified and maintained, see
   :ref:`module-pw_ghish-flag-compatibility`.

Not implemented, and loud about it: ``--jq``/``-q`` as an output filter,
``gh api``, ``gh auth status``, ``-R/--repo``, and
``pr merge --squash/--rebase/--delete-branch`` -- Gerrit submits a whole change,
and the merge strategy is a project setting rather than a per-change choice.

-----------------------------
Key architectural differences
-----------------------------

1. Branch-based PRs vs. commit-based CLs
========================================
In GitHub, a pull request is fundamentally bound to a remote Git branch
(``refs/heads/<branch>``). Pushing any commit to that branch automatically
updates the pull request.

In Gerrit, every change is a distinct **commit** identified by a persistent
``Change-Id: I...`` line in its commit message footer. Pushes are made to
virtual refs (``refs/for/<base>``), which create or update changes rather than
directly mutating branches. A single local branch can also contain a stack of
multiple dependent changes.

To prevent common failure modes (such as accidentally creating unwanted CLs or
overwriting existing work), ``pw_ghish`` establishes clear boundaries:

* **Explicit creation vs. update**: Use ``pr create`` to create a new change.
  If the commit already has a ``Change-Id`` that exists on Gerrit, ``pr create``
  halts immediately and instructs you to use ``pr push``.
* **Branch Memory**: When running ``pr push`` to update an existing change,
  ``pw_ghish`` queries Gerrit by ``Change-Id`` to discover the change's recorded
  target branch (such as a sandbox or feature branch). This guarantees updates
  land on the correct branch even if your local tracking branch changes.
* **Stack Guard**: Pushing multiple commits ahead of origin without ``--stack``
  is rejected to prevent accidental multi-CL creation on the remote server.

2. Merging vs. Gerrit submit and Commit-Queue
=============================================
On GitHub, ``gh pr merge`` performs a direct Git merge, squash, or rebase into
the base branch on the remote server.

In Pigweed, Fuchsia, and most LUCI-managed Gerrit ecosystems, direct merges are
rarely permitted. Changes must satisfy multiple gates before they can be
submitted:

1. **Review approval**: ``Code-Review+2`` from an authorized reviewer.
2. **Presubmit verification**: Automated tests and static analysis passing
   (``Presubmit-Verified+1`` or LUCI tryjobs).
3. **Commit-Queue voting**: Changes are submitted through an asynchronous
   rebase-and-test pipeline triggered by voting ``Commit-Queue+2`` (or
   ``Pigweed-Auto-Submit+1``).

``pw_ghish pr merge`` bridges this by:

* Attempting immediate submission via the Gerrit REST API (which succeeds if all
  required review and CI gates are already satisfied).
* Supporting ``--auto`` (which sets the project profile's auto-submit label)
  and ``--cq`` (which votes ``Commit-Queue+2``).
* Providing an actionable error message when submission fails, listing the exact
  missing labels or checks and providing the command to vote.

3. CI inspection: LUCI vs. GitHub Actions
=========================================
GitHub CLI organizes CI operations across two distinct commands:

1. ``gh pr checks``: Focuses on the pull request, providing a quick status
   overview table of all checks and continuous monitoring (``--watch``).
2. ``gh run``: Operates on individual workflow runs and jobs, handling log
   streaming (``gh run view --log``), failure diagnostics (``gh run view
   --log-failed``), step inspection, and rerun dispatching (``gh run rerun``).

Gerrit projects typically run LUCI Buildbucket, recipes, and LogDog rather than
GitHub Actions. ``pw_ghish`` cleanly maps Gerrit and LUCI concepts onto this
two-tier architecture:

Tier 1: Check table & watching (``gh pr checks``)
-------------------------------------------------
* ``pr checks``: Uses lightweight pRPC queries to Buildbucket to retrieve
  check statuses, run times, and direct build links without browser context
  switching.
* **Continuous watch**: Supports ``--watch`` (``-w``) and ``--fail-fast``,
  polling Buildbucket in the background and exiting immediately upon the first
  blocking failure.

Tier 2: Deep run management & triage (``gh run``)
-------------------------------------------------
* ``run list``: Lists checks on a change with IDs, status symbols, builder
  names, and execution durations.
* ``run view``: Displays a structured run overview matching upstream
  GitHub CLI's presentation. When targeting a specific builder (``-j <builder>``
  or direct build ID), it renders the hierarchical step execution tree:

  * Collapses hundreds of internal recipe plumbing micro-steps into clean
    top-level phases.
  * Hierarchically highlights failing child steps.
  * Automatically extracts 1-line failure diagnostics (e.g. summarizing
    affected files for formatting diffs, or line/error text for compiler
    and typechecker failures) so errors are obvious without digging through
    logs.

* **Terminal failure triage** (``--log-failed`` & ``--log``): Directly downloads
  and renders failure reports and LogDog error snippets straight into stdout,
  eliminating browser context switching.
* **Builder retries** (``run rerun``): Automatically constructs and executes
  ``bb add`` invocations using the project profile's try bucket definitions,
  supporting ``--failed`` and ``--dry-run``.

4. Change targeting syntax
==========================
To maximize ergonomic parity with GitHub CLI, subcommands accepting ``[<id>]``
support uniform targeting rules:

* **Omitted argument**: Automatically resolves the active change from the
  current Git branch or HEAD commit.
* **Change number**: Gerrit numeric change ID (e.g. ``472267``) or with patchset
  (e.g. ``472267/3``).
* **Gerrit URL**: Full web or REST URLs (e.g.
  ``https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/472267``).
* **Shortlinks**: Shortlinks such as ``pwrev/472267``, ``fxrev/472267``, or
  ``crrev.com/c/472267``.
* **Branch name**: Local branch names (e.g. ``my-feature``, ``cl/472267``).
  ``pw_ghish`` queries the branch's tip commit for a ``Change-Id`` or reads
  ``branch.<name>.gerrit-change-id`` from Git configuration.

5. Review comments and threading
================================
GitHub PR comments can be added as standalone comments or batched reviews.
Gerrit comments have strict threading (each reply references a parent comment
ID), server-side draft states, and explicit thread-level resolution flags.

``pw_ghish pr comment`` maps these cleanly:

* **Automatic thread detection**: Supplying ``--path <file>`` and ``--line <num>``
  locates active comment threads on that file and line, automatically replying
  to the thread rather than creating disconnected top-level comments.
* **Thread resolution**: Supplying ``--resolved`` marks the thread resolved, and
  strictly requires both ``--path`` and ``--line`` to prevent invalid requests.
* **Private drafts**: Supplying ``--draft`` stores the comment in Gerrit's
  private draft space, allowing agents and developers to stage review notes
  before publishing.
