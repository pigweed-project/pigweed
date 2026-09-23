.. _module-pw_ghish-cli:

==============
CLI User Guide
==============
.. pigweed-module-subpage::
   :name: pw_ghish

``pw_ghish`` (invoked via the ``./gh`` repository wrapper) provides GitHub CLI
(``gh pr`` and ``gh issue``) command syntax on top of Gerrit code reviews,
Google Issue Tracker (Buganizer), and LUCI CI infrastructure.

This guide provides detailed documentation and examples for each subcommand
available in ``./gh pr``. For Buganizer issue tracking workflows (``./gh
issue``), see :ref:`module-pw_ghish-issue`.

---------------
Getting started
---------------
The primary way to use ``pw_ghish`` in Pigweed is via the ``./gh`` repository
wrapper located at the root of the Pigweed repository:

.. code-block:: console

   $ ./gh pr status

The ``./gh`` wrapper automatically builds and caches the binary in ``out/gh/``
keyed by Git commit hash, executing instantly without Bazel invocation overhead
when the commit hash has not changed.

Building with Bazel
===================
You can also compile the standalone binary target directly:

.. code-block:: console

   $ bazelisk build //pw_ghish:gh-ish

Running tests
=============
Run all hermetic Go unit tests:

.. code-block:: console

   $ bazelisk test //pw_ghish:pw_ghish_test

-----------------------
Command-line references
-----------------------
All commands follow the ``./gh pr <subcommand>`` pattern (or top-level aliases
such as ``./gh push``).

Targeting changes: numbers, URLs, branches, and active PRs
==========================================================
Subcommands that operate on a change (such as ``view``, ``diff``, ``checkout``,
``checks``, ``run``, ``review``, ``comment``, ``merge``, ``close``, ``reopen``,
``ready``, and ``edit``) accept any of the following targets:

* **Omitted argument**: Automatically resolves the active change associated with
  your current Git branch or HEAD commit.
* **Change number**: A Gerrit change number (e.g. ``472267``) or change number
  with specific patchset (e.g. ``472267/3``).
* **Gerrit URL**: Full web or REST URLs
  (e.g. ``https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/472267`` or
  ``https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/472267/3``).
* **Shortlink**: Shortlinks such as ``pwrev/472267``, ``pwrev/472267/3``,
  ``fxrev/472267``, or ``crrev.com/c/472267``.
* **Branch name**: Local branch names (e.g. ``my-feature``, ``cl/472267``,
  ``change-472267``). ``pw_ghish`` inspects the branch's tip commit or
  ``branch.<name>.gerrit-change-id`` Git configuration to discover its Gerrit
  ``Change-Id``.

Listing open changes: ``pr list``
=================================
Lists open changes for the current repository:

.. code-block:: console

   $ ./gh pr list
   $ ./gh pr list --limit 10 --state open
   $ ./gh pr list --author "hepler@google.com" --base main

To output structured JSON for programmatic consumption:

.. code-block:: console

   $ ./gh pr list --json number,title,state,branch

Inspecting a change: ``pr view``
================================
Displays change metadata, including owner, reviewers, attention set status,
and current review scores. If no change ID is specified, ``pr view`` automatically
detects and displays the active change for your current branch/commit:

.. code-block:: console

   # View active change on current branch:
   $ ./gh pr view

   # View a specific change by number:
   $ ./gh pr view 413992

To display the change along with all inline comment threads indented by file
and line number:

.. code-block:: console

   $ ./gh pr view --comments
   $ ./gh pr view 413992 --comments

To output structured JSON for scripts or agents:

.. code-block:: console

   $ ./gh pr view --json number,title,state,author,files
   $ ./gh pr view 413992 --json number,title,state,author,files

``pw_ghish`` strictly validates requested JSON fields against its supported
schema (e.g. ``number``, ``title``, ``state``, ``status``, ``author``,
``files``, ``url``, ``branch``, ``reviewers``). Requesting an unknown field
aborts immediately with an actionable error rather than silently omitting data.
Both ``state`` and ``status`` are supported as aliases for GitHub CLI
compatibility.

Reading the bug link: ``bug`` and ``bugs``
------------------------------------------
``pr view`` reports the bugs a change is linked to, so a tool that just wrote
a link with ``pr edit --bug`` can read it back and confirm it took. This is the
Gerrit counterpart to ``gh pr view --json closingIssuesReferences``:

.. code-block:: console

   $ ./gh pr view 413992 --json bug,bugs
   {
     "bug": "b/123456",
     "bugs": [
       {
         "id": "b/123456",
         "closes": true
       }
     ]
   }

``bug`` is the flat, comma-separated list, matching how ``pr view`` presents
its other list-valued fields. ``bugs`` distinguishes the two Gerrit trailers
that GitHub does not: ``closes`` is true for a ``Fixed:`` trailer, which closes
the bug when the change is submitted, and false for a ``Bug:`` trailer, which
only links it. The default (non-JSON) output shows the same information as a
``Bug:`` line.

Both fields are read from the commit message trailers, which is where Gerrit
itself reads them from. Only real trailers count, so a ``Bug:`` line written in
the middle of a body paragraph is prose and is not reported -- Gerrit will not
link it either.

.. note::

   If Gerrit returns no commit message for the patchset, asking for ``bug`` or
   ``bugs`` is an error rather than an empty answer. An empty ``bug`` asserts
   "no bug is linked", and asserting that on no evidence is how a tool ends up
   attaching a duplicate bug over one that is already there. Every other field
   still works.


Checking CI status: ``pr checks``
=================================
Queries LUCI Buildbucket using lightweight pRPC to display the status, duration,
and log links of CI builds. If no change ID is specified, ``pr checks`` automatically
inspects the active change for your current branch:

.. code-block:: console

   # Check active change on current branch:
   $ ./gh pr checks

   # Check a specific change by number:
   $ ./gh pr checks 413992
   Checks for Change 413992 (Patchset 1)
     ✓  static-checks-pigweed           16s       https://ci.chromium.org/b/8680709829694997521

You can also query status for an older patchset by appending ``/<patchset>``:

.. code-block:: console

   $ ./gh pr checks 413992/1

By default, non-blocking experimental builders are omitted from the display.
Pass ``--experimental`` (or ``-e``) to include them:

.. code-block:: console

   $ ./gh pr checks --experimental
   $ ./gh pr checks 413992 -e

Watching checks until completion: ``--watch`` & ``--fail-fast``
---------------------------------------------------------------
Use ``--watch`` (``-w``) to continuously monitor checks until all blocking checks finish.
Combine with ``--fail-fast`` to exit immediately as soon as any check fails. By default,
failing checks automatically print their failure reports and LogDog log snippets upon exit:

.. code-block:: console

   # Watch until all blocking checks complete (default 15s interval):
   $ ./gh pr checks --watch

   # Fast-fail: exit immediately on first check failure and print root-cause logs:
   $ ./gh pr checks --watch --fail-fast

   # Custom poll interval (e.g. every 30s):
   $ ./gh pr checks --watch --fail-fast --interval 30s

   # Open checks overview in web browser:
   $ ./gh pr checks --web

   # Output structured JSON:
   $ ./gh pr checks --json

Exit codes
----------
``gh pr checks`` reports the state of the change through its process exit code,
matching the GitHub CLI:

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

The command fails closed. A change with no reported checks exits ``1``, because
CI that was never scheduled is not CI that passed. Experimental (non-blocking)
builders never affect the exit code, even with ``--experimental``, since the
Commit Queue does not gate on them; ``--experimental`` only controls what is
displayed.

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


Managing CI runs & builders: ``gh run``
=======================================
``pw_ghish`` provides a first-class ``gh run`` command suite matching the
official GitHub CLI. While ``gh pr checks`` provides the high-level status
table for a pull request, ``gh run`` allows developers and AI agents to
deeply inspect workflow runs, drill down into individual jobs and execution
steps, view failure logs and diagnostics without opening web browsers, and rerun
builders.

All ``run`` commands operate on the active change on your current branch by
default, or accept explicit change numbers (e.g. ``472267`` or ``472267/3``) or
direct Buildbucket build IDs (e.g. ``8671182706745774001``).

Listing runs: ``run list``
--------------------------
Lists all checks and tryjobs for a change with duration, ID, status, and URL:

.. code-block:: console

   # List runs for active change:
   $ ./gh run list

   # List runs for a specific change:
   $ ./gh run list 472267

   # Include non-blocking experimental checks:
   $ ./gh run list --experimental

   # Output structured JSON:
   $ ./gh run list --json

Viewing runs, jobs, and diagnostics: ``run view``
-------------------------------------------------
``run view`` provides three levels of inspection:

1. **Change-level run overview**: Running ``./gh run view`` renders a
   structured overview matching upstream GitHub CLI's presentation:

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

2. **Job step execution tree** (``-j <builder>``): Inspect the real-time
   execution steps and timing of any builder on a change without opening
   a web browser.

   ``pw_ghish`` collapses hundreds of internal recipe plumbing micro-steps into
   clean top-level phases, hierarchically highlights failing child steps, and
   automatically extracts 1-line failure diagnostics (e.g. formatting diffs or
   compiler/typechecker errors):

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

   Pass ``-v`` or ``--verbose`` to inspect all unfiltered recipe micro-steps.

3. **Log inspection** (``--log-failed`` & ``--log``): Directly view failure
   summaries and LogDog error snippets in the terminal:

.. code-block:: console

   # Inspect all failed checks on current change:
   $ ./gh run view --log-failed

   # Inspect a specific failed builder:
   $ ./gh run view -j pigweed-lintformat --log-failed

   # Dump full log stream instead of tail snippet:
   $ ./gh run view -j pigweed-lintformat --log

   # Output structured JSON report:
   $ ./gh run view --log-failed --json

   # Open build in web browser:
   $ ./gh run view -j pigweed-lintformat --web

Rerunning CI checks: ``run rerun``
----------------------------------
Rerun specific or all failed builders on a change (automatically constructs
and executes the project profile's ``bb add`` invocation):

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
----------------------------
Continuously monitor checks until all blocking builds complete:

.. code-block:: console

   $ ./gh run watch
   $ ./gh run watch 472267 --interval 30s

Viewing patch diffs: ``pr diff``
================================
Displays the unified diff of the latest patchset, or a specific revision. If
no change ID is specified, ``pr diff`` defaults to the current branch's change:

.. code-block:: console

   # View diff of active change on current branch:
   $ ./gh pr diff

   # View diff of a specific change or patchset:
   $ ./gh pr diff 413992
   $ ./gh pr diff 413992/2

Review dashboard: ``pr status``
===============================
Shows a fast, focused dashboard containing:

1. **Current branch**: Displays the active change ID, title, target branch,
   patchset number, submittability, labels/flags (e.g. Code-Review,
   Presubmit-Verified, Lint), live tryjob check status, and an inline
   comments summary showing unresolved threads, unpublished drafts, and
   previews of up to 2 unresolved comment threads with hysteresis limits.
2. **Created by you**: Lists open changes you authored, scoped to the last 30
   days by default for fast rendering and minimal token consumption.
3. **Requesting a code review from you**: Lists changes awaiting your review,
   also scoped to the last 30 days.

If older open changes exist outside the 30-day window, a notice is displayed
with the count of older changes and a hint to use ``--all`` (``-A``):

.. code-block:: console

   # Default focused view (last 30 days + active branch):
   $ ./gh pr status

   # Show all changes across your account without 30-day filter:
   $ ./gh pr status --all

Creating new changes: ``pr create``
===================================
Pushes your local HEAD commit to Gerrit as a **new** change:

.. code-block:: console

   $ ./gh pr create -r "reviewer@google.com" --auto

Safety guard against existing changes
-------------------------------------
If a change with the same ``Change-Id`` already exists on Gerrit, ``pr create``
halts with an error indicating the existing change's URL and directs you to
use ``pr push`` instead. This prevents confusion between creating a new change
and amending an existing one. Use ``--force`` if you explicitly intend to
bypass this guard.

Multi-commit stack guard
------------------------
When pushing to Gerrit, every unpushed commit in your branch history creates an
individual Gerrit Change List (CL). If you push from a branch containing multiple
commits (e.g. 50 commits on a sandbox branch), raw Gerrit would create 50
separate CLs against the target branch.

``pw_ghish`` inspects the commit count before pushing. If more than 1 commit
would be pushed, ``pr create`` halts with an error displaying the count and target
branch, requiring the ``--stack`` flag to proceed:

.. code-block:: console

   $ ./gh pr create --stack

Supported flags for ``pr create``:

* ``-t, --title <str>`` and ``-b, --body <str>``: Create a new Git commit
  before pushing.
* ``--force``: Force creation even if a change with this ``Change-Id`` already exists.
  There is no ``-f`` shorthand: ``gh`` uses ``-f`` for ``--fill``.
* ``--stack``: Allow pushing multiple commits as a stack of Gerrit changes.
* ``-B, --base <branch>``: Target base branch (e.g. ``sandbox/experiment``).
  Defaults to upstream tracking branch or repository default.
* ``-r, --reviewer <email>``: Add reviewers to the change.
* ``-c, --cc <email>``: CC users on the change.
* ``--auto`` (alias ``--auto-submit``): Enable auto-submit upon upload.
* ``--cq [1|2]``: Trigger Commit-Queue dry-run (default ``1`` when omitted; specify ``2`` to submit).
* ``-d, --draft``: Push as a work-in-progress (WIP) draft.
* ``--publish``: Automatically publish draft comments on push.
* ``-o, --push-option <opt>``: Pass arbitrary Gerrit push options (e.g.
  ``-o topic=my-feature``).
* ``--no-verify``: Bypass local pre-push Git hooks.

Automated Change-Id repair
--------------------------
Gerrit requires a ``Change-Id: I...`` line in the footer of each commit
message. If your commit is missing a ``Change-Id``, ``pr create`` automatically:

1. Checks if the Gerrit ``commit-msg`` hook is installed in your Git repository.
2. Downloads the hook directly from the Gerrit server if absent.
3. Runs ``git commit --amend --no-edit`` to generate the footer before pushing.

Pushing patchsets: ``pr push``
==============================
Pushes local commits to Gerrit to upload a **new patchset** on an existing change.
Available as ``./gh pr push``, top-level ``./gh push``, or ``./gh pr upload``:

.. code-block:: console

   # Push a new patchset for the current branch:
   $ ./gh pr push

   # Push using top-level alias:
   $ ./gh push

   # Push with updated reviewers and mark ready for review:
   $ ./gh pr push -r "colleague@google.com" --ready --auto

Branch memory & stack safety
----------------------------
When updating an existing change, ``pw_ghish`` queries Gerrit using the commit's
``Change-Id`` to discover the change's target branch on the server (for instance,
a sandbox branch such as ``sandbox/my-experiment``). It automatically targets
that branch, ensuring you never accidentally push updates to the wrong branch.
To override the target branch, use ``-B, --base <branch>``.

Like ``pr create``, ``pr push`` validates the commit count and requires
``--stack`` if pushing multiple commits.

Supported flags for ``pr push``:

* ``--stack``: Allow pushing multiple commits as a stack of Gerrit changes.
* ``-B, --base <branch>``: Override the target merge branch.
* ``-r, --reviewer <email>``: Add reviewers to the change.
* ``-c, --cc <email>``: CC users on the change.
* ``--ready``: Mark change as ready for review (removes WIP).
* ``-d, --draft``: Push as a work-in-progress (WIP) draft.
* ``--auto`` (alias ``--auto-submit``): Enable auto-submit upon upload.
* ``--cq [1|2]``: Trigger Commit-Queue dry-run (default ``1`` when omitted; specify ``2`` to submit). If pushed on an up-to-date commit, automatically updates metadata via Gerrit API.
* ``--publish``: Automatically publish draft comments on push.
* ``-o, --push-option <opt>``: Pass arbitrary Gerrit push options (e.g.
  ``-o topic=my-feature``).
* ``--no-verify``: Bypass local pre-push Git hooks.

Editing a change: ``pr edit``
=============================
Updates the commit message, reviewers, topic, hashtags, and votes of an
existing change through the Gerrit API, without pushing a new patchset:

.. code-block:: console

   # Rewrite the description, keeping the subject line and all trailers:
   $ ./gh pr edit 413992 --body "A clearer explanation of the change."

   # Change only the subject line:
   $ ./gh pr edit 413992 --title "pw_string: Fix off-by-one in Copy()"

   # Add a reviewer and trigger a CQ dry run:
   $ ./gh pr edit 413992 --add-reviewer colleague@google.com --cq

Commit message safety
---------------------
A Gerrit commit message ends in a block of trailers that carry real meaning:
``Change-Id:`` identifies the change itself, ``Bug:`` and ``Fixed:`` link and
close Buganizer issues, ``Cq-Include-Trybots:`` selects builders, and
``(cherry picked from commit ...)`` records provenance. Gerrit keeps no copy of
the previous message once it is overwritten, so a trailer dropped during an
edit is gone.

``--title`` and ``--body`` therefore only touch what you named. Every trailer
paragraph in the message is preserved, wherever it appears -- including
messages that end in a prose paragraph rather than the trailer block. A trailer
you supply in ``--body`` replaces the original of the same key, so
``--body "...\n\nBug: b/99999"`` updates the bug and leaves everything else
alone. Indented content in your body is left as written, since an indented
``key: value`` line is part of a code sample, not a trailer.

``--message`` is different: it replaces the **entire** commit message.
Rather than silently re-appending the trailers your new text omits -- which
would make deliberate removal impossible -- it refuses and tells you what is at
stake:

.. code-block:: console

   $ ./gh pr edit 413992 --message "pw_string: Fix off-by-one"
   --message would delete 2 trailer(s) from change 413992:

     Bug: b/12345
     Cq-Include-Trybots: luci.pigweed.try:pigweed-linux-clang

   ...

Include the trailers in your message, use ``--body`` to edit only the prose, or
pass ``--drop-trailers`` to confirm the removal. ``Change-Id:`` is exempt: it is
the change's identity rather than something you authored, so it is restored
automatically and never reported as a casualty.

Linking bugs: ``--bug`` and ``--fixed``
---------------------------------------
``--bug`` and ``--fixed`` set the corresponding trailer without touching
anything else in the message:

.. code-block:: console

   # Link a bug:
   $ ./gh pr edit 413992 --bug b/123456

   # Link a bug and close it when the change is submitted:
   $ ./gh pr edit 413992 --fixed 123456

   # Record that no bug applies:
   $ ./gh pr edit 413992 --bug none

Both accept whatever form you happen to have: a bare number, ``b/123456``,
``https://issues.pigweed.dev/issues/123456``, a Chromium issue URL, or a
comma-separated list. The value is canonicalized to ``b/<id>`` and the trailer
is placed in the trailer block, replacing any existing trailer of the same key
rather than adding a second one.

The flags exist because appending a trailer by hand means getting its
placement, spelling, and uniqueness right, and getting any of those wrong fails
silently.

.. note::

   ``Fixed:`` is written rather than ``Fixes:``. Gerrit accepts ``fix``,
   ``fixes``, ``fixing`` and ``fixed`` interchangeably and auto-closes the bug
   on all four, so this is Pigweed house style rather than a functional
   difference.

GitHub issue syntax is rejected
-------------------------------
On GitHub, a pull request body containing ``Fixes #456`` closes issue 456 when
the PR merges. It is the most widely known way to link an issue, and it does
**nothing** on Gerrit: the line is ordinary prose, so the bug is never linked,
never closed, and nothing reports a problem.

``pr edit``, ``pr create`` and ``pr push`` therefore refuse a commit message
containing GitHub's closing keywords (``close``, ``fix``, ``resolve`` and their
variants) followed by ``#<number>``, and point at the trailer to use instead.
``pr create`` and ``pr push`` check the HEAD commit message as well as any
``--title``/``--body`` you supply, so a message written with plain
``git commit`` is caught too.

.. warning::

   The reference is reported, never translated. A GitHub issue number is
   repo-local and usually small, while a Buganizer ID is usually eight or nine
   digits, so rewriting ``#456`` as ``b/456`` would link a real but unrelated
   bug. Attaching the wrong bug is worse than attaching none.

A bare ``#456`` with no keyword is not flagged, and neither is an indented line,
since both are far more often prose or a code sample than an issue link.

Submitting changes: ``pr merge``
================================
Submits a change to the target branch. In Pigweed, merges are executed as clean
rebases performed by the LUCI Commit Queue bot, not merge commits.

.. code-block:: console

   # Enable automated submission once presubmits and reviews pass (recommended):
   $ ./gh pr merge 413992 --auto

   # Trigger Commit-Queue+2 submission directly:
   $ ./gh pr merge 413992 --cq

   # Attempt immediate submit (requires all CI checks and reviews to already be passed):
   $ ./gh pr merge 413992

Submit modes:

* **Auto-submit** (``--auto``): Votes the change's auto-submit label (e.g.
  ``Pigweed-Auto-Submit+1`` or ``Auto-Submit+1``). Once all approvals
  (``Code-Review+2``) and tryjobs pass, LUCI CV automatically rebases and
  submits the change. If the host has no auto-submit label, ``pw_ghish``
  triggers a ``Commit-Queue+1`` dry run (when available) and returns an error;
  use ``--cq`` to submit via the Commit Queue instead.
* **Commit-Queue** (``--cq``): Applies ``Commit-Queue+2`` directly, initiating
  presubmit verification and submitting the change upon success.
* **Immediate submit**: Calls Gerrit's ``SubmitChange`` API directly. In Pigweed,
  if submit requirements (Code-Review, Presubmit-Verified, etc.) are still pending,
  Gerrit rejects the request with HTTP 409 Conflict. ``pw_ghish`` intercepts this
  and provides an actionable hint to use ``--auto`` or ``--cq``.

.. note::

   ``pr merge`` operates on the change as a whole and submits its latest revision.
   Specifying an explicit patchset suffix (such as ``./gh pr merge 413992/1``) is
   rejected with an error, as Gerrit does not support submitting historical revisions.

Reviewing changes: ``pr review``
================================
Approves, requests changes, or votes on Commit-Queue on a Gerrit CL. If no change ID is specified, it
reviews the active change on the current branch (matching GitHub CLI):

.. code-block:: console

   # Trigger Commit-Queue dry run on the active change (Commit-Queue+1):
   $ ./gh pr review --cq

   # Approve active change and trigger CQ dry run:
   $ ./gh pr review --approve --cq

   # Approve active change on the current branch (Code-Review+2):
   $ ./gh pr review --approve -m "Looks great!"

   # Approve a specific change by number:
   $ ./gh pr review 413992 --approve -m "Looks great!"

   # Request changes on a specific change or URL (Code-Review-1):
   $ ./gh pr review 413992 --request-changes -m "Please address formatting."

Posting inline comments: ``pr comment``
=======================================
Adds review comments to a change. When given a file path and line number,
``pw_ghish`` automatically locates existing threads on that line and appends
your comment as a reply:

.. code-block:: console

   # Reply to an inline thread and mark as resolved:
   $ ./gh pr comment 413992 --path pw_string/string.cc --line 42 \
       -m "Fixed, using pw::Status." --resolved

   # Save an inline comment as a draft without publishing:
   $ ./gh pr comment 413992 --path pw_string/string.cc --line 42 \
       -m "Consider std::string_view" --draft

The ``--resolved`` flag specifically marks an inline review thread as resolved
in Gerrit and strictly requires both ``--path`` and ``--line``. Attempting to
pass ``--resolved`` to a change-level comment without a thread target is
rejected with an error to prevent accidental unthreaded resolutions.

Responding across patchsets
---------------------------
In Gerrit, comments belong to the patchset on which they were originally posted.
When replying to an inline comment, ``pw_ghish`` inspects the change's comment
history across all revisions:

* If the thread originated on an earlier patchset (such as patchset 1) and a
  newer patchset has since been uploaded, ``pw_ghish`` automatically targets
  the originating patchset and comment ID so that the reply is correctly nested
  in the existing thread and resolved in Gerrit.
* You can also explicitly target a specific patchset by appending
  ``/<patchset>`` (e.g. ``./gh pr comment 413992/1``) or by supplying the
  ``--patchset <num>`` flag.

Local checkout and cherry-pick
==============================
Inspect changes locally in your working directory. ``pr checkout`` accepts
change numbers, specific patchsets, full Gerrit URLs, branch names, or can be run
with no arguments to fetch and checkout the active change:

.. code-block:: console

   # Fetch and checkout change at FETCH_HEAD:
   $ ./gh pr checkout 413992

   # Checkout a specific patchset:
   $ ./gh pr checkout 413992/3

   # Checkout from a Gerrit URL:
   $ ./gh pr checkout https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/413992/2

   # Fetch latest patchset for active change:
   $ ./gh pr checkout

   # Cherry-pick change onto the active branch:
   $ ./gh pr cherry-pick 413992

Root-level aliases
==================
For developer ergonomics and parity with frequent commands, ``pw_ghish``
provides root-level command aliases:

* ``./gh checks`` -> ``./gh pr checks``
* ``./gh run`` -> ``./gh run``
* ``./gh view`` -> ``./gh pr view``
* ``./gh diff`` -> ``./gh pr diff``
* ``./gh status`` -> ``./gh pr status``
* ``./gh push`` -> ``./gh pr push``

-------------------------
Authentication in Pigweed
-------------------------
``pw_ghish`` supports several authentication methods across Pigweed development
environments:

* **Internal workstations**: For developers working on corporate workstations,
  ``./gh`` automatically integrates with local credentials without requiring
  manual credential configuration.
* **Open source contributors**: Reads Netscape-formatted cookies from
  ``~/.gitcookies`` or bearer tokens from the ``GERRIT_TOKEN`` environment
  variable.
* **Anonymous read fallback**: Public read operations (such as ``pr view``,
  ``pr list``, ``pr diff``, and ``pr checks``) automatically succeed on public
  Gerrit hosts even without credentials.

For details on advanced credential configuration, forcing auth methods via
``GH_ISH_AUTH_METHOD``, or setting up automated bots, see
:ref:`Project Adoption Guide <module-pw_ghish-project-integration>`.
