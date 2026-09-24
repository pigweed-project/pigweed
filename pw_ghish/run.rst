.. _module-pw_ghish-run:

=====================
CI & tryjobs (gh run)
=====================
.. pigweed-module-subpage::
   :name: pw_ghish

``./gh run`` and ``./gh pr checks`` map GitHub Actions CLI workflows to **LUCI
Buildbucket**. You can check build statuses, inspect step execution trees, view
step failure logs, and rerun failed tryjob builders from the terminal.

---------------
Quick reference
---------------
.. code-block:: console

   # Check status and duration of LUCI tryjob builders on the active CL:
   $ ./gh pr checks

   # Watch checks until completion, exiting with failure logs if a check fails:
   $ ./gh pr checks --watch --fail-fast

   # View a summary of all tryjobs on the active CL:
   $ ./gh run view

   # Inspect the step execution tree and failure summaries for a builder:
   $ ./gh run view -j pigweed-lintformat

   # Print failure summaries and step log snippets for all failed checks:
   $ ./gh run view --log-failed

   # Rerun all failed builders on the active CL via Buildbucket (bb add):
   $ ./gh run rerun --failed

------------------------------
Overview: pr checks and gh run
------------------------------
Matching the GitHub CLI, ``pw_ghish`` divides CI commands into two groups:

1. **Change-level check status** (``./gh pr checks``): Displays a status table
   for a Gerrit CL, supports polling until completion (``--watch``), and
   returns an exit code suitable for shell pipelines and merge gates.
2. **Run and builder inspection** (``./gh run``): Inspects individual LUCI
   builders and step execution trees (``run view -j <builder>``), fetches step
   failure logs (``--log-failed`` and ``--log``), and triggers builder retries
   (``run rerun``).

Both ``pr checks`` and ``run`` commands resolve the active Gerrit change from
your current Git branch or ``HEAD`` commit when no target argument is supplied.
You can also pass an explicit change number (e.g. ``472267``), patchset suffix
(e.g. ``472267/3``), Gerrit URL/shortlink (see
:ref:`module-pw_ghish-pr-targeting`), or a Buildbucket build ID (e.g.
``8671182706745774001``).

------------------------------------------
Checking and watching CL status: pr checks
------------------------------------------
``./gh pr checks`` queries LUCI Buildbucket via pRPC to display the status,
duration, and build URLs of tryjobs on a change:

.. code-block:: console

   # Check active change on current branch:
   $ ./gh pr checks

   # Check a specific change by number:
   $ ./gh pr checks 413992
   Checks for Change 413992 (Patchset 1)
     ✓  static-checks-pigweed           16s       https://ci.chromium.org/b/8680709829694997521

   # Check a historical patchset:
   $ ./gh pr checks 413992/1

By default, non-blocking experimental builders (``cq_experimental``) are
omitted from the display. Pass ``--experimental`` (or ``-e``) to include them:

.. code-block:: console

   $ ./gh pr checks --experimental
   $ ./gh pr checks 413992 -e

Watching checks: ``--watch`` and ``--fail-fast``
================================================
Use ``--watch`` (``-w``) to poll until all blocking checks finish. Combine with
``--fail-fast`` to exit as soon as any blocking check fails. By default,
failing checks print their step failure summary and log snippet upon exit:

.. code-block:: console

   # Watch until all blocking checks complete (default 15s interval):
   $ ./gh pr checks --watch

   # Exit on first check failure and print step failure logs:
   $ ./gh pr checks --watch --fail-fast

   # Custom poll interval (e.g. every 30s):
   $ ./gh pr checks --watch --fail-fast --interval 30s

   # Open checks overview in web browser:
   $ ./gh pr checks --web

   # Output JSON:
   $ ./gh pr checks --json

Process exit code contract
==========================
``./gh pr checks`` reports the state of the change through its process exit
code, matching the GitHub CLI:

.. list-table::
   :header-rows: 1

   * - Exit code
     - Meaning
   * - ``0``
     - At least one blocking check ran and all of them passed.
   * - ``8``
     - Nothing has failed, but at least one blocking check is still running.
   * - ``1``
     - At least one blocking check failed or was canceled, no checks were
       reported at all, or the command itself errored.

The contract holds on every invocation, with or without ``--watch``,
``--json``, or ``--template``, so it is safe to use as a merge gate:

.. code-block:: console

   $ ./gh pr checks --watch && ./gh pr merge --cq

The command fails closed: a change with no reported checks exits ``1``, because
CI that was never scheduled is not CI that passed. Experimental (non-blocking)
builders never affect the exit code, even with ``--experimental``, since the
Commit Queue does not gate on them; ``--experimental`` only controls what is
displayed.

Canceled checks on superseded patchsets
---------------------------------------
Canceled checks exit ``1`` as well, since a canceled build did not pass, but
they are reported as canceled rather than failed. Querying an older patchset
usually produces them, because uploading a new patchset cancels the runs still
in flight on the previous one:

.. code-block:: console

   $ ./gh pr checks 472267/43
   ...
   Error: 29 of 75 checks were canceled on Change 472267 (Patchset 43): ... and 24 more.

   Checks are usually canceled because a newer patchset superseded this one.
   To query the current patchset instead:
     gh pr checks 472267

---------------------------------------
Inspecting and rerunning builds: gh run
---------------------------------------

Listing runs: ``run list``
==========================
Lists all checks and tryjobs for a change with duration, Buildbucket build ID,
status, and URL:

.. code-block:: console

   # List runs for active change:
   $ ./gh run list

   # List runs for a specific change:
   $ ./gh run list 472267

   # Include non-blocking experimental checks:
   $ ./gh run list --experimental

   # Output JSON:
   $ ./gh run list --json

Viewing runs, steps, and logs: ``run view``
===========================================
``./gh run view`` supports three modes of inspection:

1. Change-level run overview
----------------------------
Running ``./gh run view`` renders a summary of all builders on the change:

.. code-block:: console

   $ ./gh run view
   Showing 10 checks for Change 467905 (Patchset 22) • docs: Integrate navs
   Gerrit CL: https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/467905

   JOBS
   ✓ static-checks-pigweed                    (22s)
   ✓ pigweed-mac-arm-bazel-python             (4m53s)
   ✓ pigweed-linux-gn-platform                (10m48s)
   ✓ pigweed-linux-gn-host                    (8m55s)
   ✓ pigweed-linux-gn-compatibility-platform  (10m4s)
   ✓ pigweed-linux-gn-compatibility-main      (15m11s)
   ✓ pigweed-linux-bazel-python               (1m24s)
   ✓ docs-builder                             (2m36s)
   ✓ docs-builder-newpatchset                 (3m15s)
   X pigweed-lintformat                       (5m32s)

   To view step execution details for a specific check, run:
     gh run view -j <builder-name>
   To inspect failure summaries and logs for failed checks, run:
     gh run view --log-failed

2. Builder step execution tree (``-j <builder>``)
-------------------------------------------------
Inspect the execution steps and timing of a specific builder on a change.

``pw_ghish`` collapses passing internal recipe substeps into top-level phases,
highlights failing child steps, and extracts 1-line failure diagnostics (such
as formatting diffs or compiler errors):

.. code-block:: console

   $ ./gh run view -j pigweed-lintformat
   Steps for pigweed-lintformat (Build 8671182706745774001)
   Status: FAILURE ✗ | URL: https://ci.chromium.org/b/8671182706745774001

     ✓  setup_build                   running recipe: "pw_presubmit" with Python 3.11.9
     ✓  checkout pigweed
     ✓  environment
     ✓  get steps from programs
     ✓  bazel_format
     ✓  css_format
     !  javascript_format
        └── ✗  failure summary: formatting diff in docs/common/header.js
     ✓  markdown_format
     !  python_format
        └── ✗  failure summary: formatting diff in 3 files: header.py, nav.py, ...
     ✓  restructuredtext_format

Pass ``-v`` or ``--verbose`` to inspect all unfiltered recipe steps.

3. Step failure logs (``--log-failed`` and ``--log``)
-----------------------------------------------------
Fetch failure summaries and raw step log snippets in the terminal:

.. code-block:: console

   # Inspect all failed checks on current change:
   $ ./gh run view --log-failed

   # Inspect a specific failed builder:
   $ ./gh run view -j pigweed-lintformat --log-failed

   # Print the full step log instead of the tail snippet:
   $ ./gh run view -j pigweed-lintformat --log

   # Output JSON report:
   $ ./gh run view --log-failed --json

   # Open build in web browser:
   $ ./gh run view -j pigweed-lintformat --web

Rerunning CI checks: ``run rerun``
==================================
Rerun specific or all failed builders on a change (constructs and executes the
project profile's ``bb add`` invocation):

.. code-block:: console

   # Rerun all failed builders on the current change:
   $ ./gh run rerun --failed

   # Rerun a single builder on the current change:
   $ ./gh run rerun -j pigweed-lintformat

   # Rerun a builder on a specific change:
   $ ./gh run rerun 472267 -j pigweed-mac-arm-vscode

   # Preview the rerun command without executing:
   $ ./gh run rerun --failed --dry-run

Watching runs: ``run watch``
============================
Poll checks until all blocking builds complete:

.. code-block:: console

   $ ./gh run watch
   $ ./gh run watch 472267 --interval 30s

--------------------------------------------------
Comparison with GitHub CLI (gh run & gh pr checks)
--------------------------------------------------
Gerrit projects run LUCI Buildbucket and recipes rather than GitHub Actions.
``./gh run`` and ``./gh pr checks`` map these concepts as follows:

LUCI vs. GitHub Actions concept map
===================================
.. list-table::
   :header-rows: 1
   :widths: 28 32 40

   * - GitHub Actions Concept
     - LUCI Equivalent in ``pw_ghish``
     - Behavioral Notes
   * - **Workflow Run**
     - **Gerrit Patchset Tryjob Set** (or Buildbucket Build ID)
     - Identified by Gerrit change/patchset (``472267/3``) or a 64-bit
       Buildbucket ID (``8671182706745774001``).
   * - **Workflow Job** (``-j <job>``)
     - **LUCI Tryjob Builder** (e.g. ``pigweed-lintformat``)
     - Targeted by builder name via ``-j <builder>`` or direct Buildbucket ID.
   * - **Job Steps**
     - **LUCI Recipe Steps**
     - LUCI recipes emit nested substeps; ``run view -j`` collapses passing
       subtrees and surfaces 1-line failure summaries.
   * - **Action Logs**
     - **Buildbucket Step Logs & SummaryMarkdown**
     - ``--log-failed`` downloads the step failure summary and the raw
       ``stdout``/``stderr`` log stream linked on the failing Buildbucket step.
   * - **Rerun Workflow** (``gh run rerun``)
     - **Buildbucket Retry** (``bb add -cl ...``)
     - Schedules tryjobs in the project profile's ``<project>/try`` bucket.

Key behavioral and flag differences
===================================
* **Change-scoped by default**: In upstream ``gh``, running ``gh run list`` or
  ``gh run view`` with no arguments queries recent workflow runs across the
  entire repository. Because a Gerrit repository has many concurrent tryjobs
  across unrelated changes, ``pw_ghish`` scopes ``run list``, ``run view``,
  ``run watch``, and ``run rerun`` by default to the **active Gerrit CL on your
  current branch**.
* **Experimental builders** (``-e, --experimental``): LUCI distinguishes
  blocking Commit-Queue builders from ``cq_experimental`` builders. Experimental
  builders are hidden by default and do not cause a non-zero exit code in
  ``pr checks``.
* ``--json`` **flag**: On ``run list`` and ``run view``, ``--json``
  is a boolean flag that emits the structured run or failure report (unlike
  ``pr view --json <fields>`` and ``issue view --json <fields>``, which take a
  comma-separated field list).

For the flag compatibility policy across all subcommands, see
:ref:`module-pw_ghish-cli-comparison`.
