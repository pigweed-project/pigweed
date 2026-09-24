.. _module-pw_ghish-pr:
.. _module-pw_ghish-cli:

===================
Code review (gh pr)
===================
.. pigweed-module-subpage::
   :name: pw_ghish

``./gh pr`` maps GitHub CLI pull request commands (``gh pr``) to **Gerrit Code
Review**. You can create changes, upload new patchsets, edit commit trailers,
reply to inline comment threads, and submit CLs from the command line.

For a step-by-step walkthrough of creating, iterating on, and landing a Gerrit
change, see :ref:`module-pw_ghish-life-of-a-pr`.

---------------
Quick reference
---------------
.. code-block:: console

   # View review status for the current branch and your open CLs:
   $ ./gh pr status

   # Inspect the active change and all inline comment threads:
   $ ./gh pr view --comments

   # Push a local commit as a new Gerrit change and start presubmits:
   $ ./gh pr create -r "reviewer@google.com" --cq --auto

   # Upload a new patchset to the active Gerrit change:
   $ ./gh pr push --cq

   # Reply to an inline comment thread and mark as resolved:
   $ ./gh pr comment --path pw_string/string.cc --line 42 -m "Done." --resolved

   # Enable automated submission once review and CI checks pass:
   $ ./gh pr merge --auto

---------------------------
Creating changes: pr create
---------------------------
Pushes your local ``HEAD`` commit to Gerrit as a **new** change:

.. code-block:: console

   $ ./gh pr create -r "reviewer@google.com" --cq --auto

Supported flags
===============
* ``-t, --title <str>`` and ``-b, --body <str>``: Create a new Git commit
  before pushing.
* ``-B, --base <branch>``: Target base branch (e.g. ``sandbox/experiment``).
  Defaults to the upstream tracking branch or repository default.
* ``-r, --reviewer <email>``: Add reviewers to the change.
* ``-c, --cc <email>``: CC users on the change.
* ``--auto`` (alias ``--auto-submit``): Vote the host's auto-submit label upon
  upload.
* ``--cq [1|2]``: Trigger a Commit-Queue dry run (default ``1`` when omitted;
  specify ``2`` to submit). See :ref:`module-pw_ghish-pr-cq-auto` for how
  ``--cq``, ``--auto``, and ``pr merge`` interact.
* ``-d, --draft``: Push as a work-in-progress (WIP) draft.
* ``--publish``: Publish pending draft comments on upload.
* ``--stack``: Allow pushing multiple commits as a stack of Gerrit changes.
* ``--force``: Force creation even if a change with this ``Change-Id`` already
  exists on Gerrit. (There is no ``-f`` shorthand because ``gh`` uses ``-f``
  for ``--fill``.)
* ``-o, --push-option <opt>``: Pass Gerrit push options (e.g.
  ``-o topic=my-feature``).
* ``--no-verify``: Bypass local pre-push Git hooks.

Existing change guard
=====================
If a change with the same ``Change-Id`` already exists on Gerrit, ``pr create``
stops with an error showing the existing change URL and directs you to use
``pr push`` instead. This prevents confusion between creating a new change and
uploading a new patchset to an existing one. Pass ``--force`` to bypass this
check.

Multi-commit stack guard
========================
In Gerrit, every unpushed commit in your branch history creates a separate
Change List (CL). If more than one commit would be pushed, ``pr create`` stops
and displays the commit count and target branch, requiring ``--stack`` to
proceed:

.. code-block:: console

   $ ./gh pr create --stack

Automated Change-Id hook installation
=====================================
Gerrit requires a ``Change-Id: I...`` footer in each commit message. If your
commit is missing a ``Change-Id``, ``pr create`` automatically downloads the
Gerrit ``commit-msg`` hook if absent and runs ``git commit --amend --no-edit``
before pushing.

----------------------------
Uploading patchsets: pr push
----------------------------
Uploads local commits to Gerrit as a **new patchset** on an existing change.
Available as ``./gh pr push``, top-level ``./gh push``, or ``./gh pr upload``:

.. code-block:: console

   # Push a new patchset for the current branch:
   $ ./gh pr push

   # Push using the top-level alias:
   $ ./gh push

   # Push with updated reviewers and mark ready for review:
   $ ./gh pr push -r "colleague@google.com" --ready --auto

Supported flags
===============
* ``-B, --base <branch>``: Override the target merge branch recorded on Gerrit.
* ``-r, --reviewer <email>``: Add reviewers to the change.
* ``-c, --cc <email>``: CC users on the change.
* ``--ready``: Mark the change as ready for review (removes WIP status).
* ``-d, --draft``: Mark the change as a work-in-progress (WIP) draft.
* ``--auto`` (alias ``--auto-submit``): Vote the host's auto-submit label.
* ``--cq [1|2]``: Trigger a Commit-Queue dry run (default ``1`` when omitted;
  specify ``2`` to submit). If the local commit matches the latest patchset on
  Gerrit, votes are applied via the Gerrit REST API without re-pushing.
* ``--publish``: Publish pending draft comments on upload.
* ``--stack``: Allow pushing multiple commits as a stack of Gerrit changes.
* ``-o, --push-option <opt>``: Pass Gerrit push options (e.g.
  ``-o topic=my-feature``).
* ``--no-verify``: Bypass local pre-push Git hooks.

Target branch discovery and stack guard
=======================================
When updating an existing change, ``pr push`` queries Gerrit by the commit's
``Change-Id`` to determine the target branch recorded on the server (for
example, ``sandbox/my-experiment``) and pushes to
``refs/for/<recorded-branch>``. Use ``-B, --base <branch>`` to override the
target branch explicitly. Like ``pr create``, ``pr push`` requires ``--stack``
when uploading multiple unpushed commits.

----------------------------------------
Editing metadata & linking bugs: pr edit
----------------------------------------
Updates the commit message, bug trailers, reviewers, topic, hashtags, and votes
of an existing change through the Gerrit REST API without pushing a new
patchset:

.. code-block:: console

   # Link a Buganizer issue without touching the rest of the commit message:
   $ ./gh pr edit 413992 --bug b/123456

   # Rewrite the description body while preserving the subject line and trailers:
   $ ./gh pr edit 413992 --body "A clearer explanation of the change."

   # Add a reviewer and trigger a CQ dry run:
   $ ./gh pr edit 413992 --add-reviewer colleague@google.com --cq

Supported flags
===============
* ``--bug <id>``: Set or update the ``Bug: b/<id>`` trailer (or ``--bug none``).
* ``--fixed <id>``: Set or update the ``Fixed: b/<id>`` trailer (closes the bug
  when the CL is submitted).
* ``-t, --title <str>``: Update the commit subject line, preserving the body
  and all commit trailers.
* ``-b, --body <str>``: Update the commit body paragraphs, preserving the
  subject line and all commit trailers.
* ``--message <str>``: Replace the entire commit message (requires
  ``--drop-trailers`` if existing trailers would be removed).
* ``--add-reviewer <email>`` / ``--remove-reviewer <email>``: Add or remove
  reviewers.
* ``--add-label <Label=Value>``: Apply a Gerrit label vote (e.g.
  ``--add-label Commit-Queue=1``).
* ``--cq [1|2]``: Vote on ``Commit-Queue`` (default ``1``).
* ``--topic <str>`` / ``--hashtag <str>``: Set the Gerrit topic or add a
  hashtag.

Linking bugs: ``--bug`` and ``--fixed``
=======================================
``--bug`` and ``--fixed`` canonicalize bare numbers (``123456``), ``b/123456``,
or issue tracker URLs (``https://issues.pigweed.dev/issues/123456``) to
``b/<id>`` and update the trailer block in place:

.. code-block:: console

   # Link a bug:
   $ ./gh pr edit 413992 --bug b/123456

   # Link a bug and close it when the change is submitted:
   $ ./gh pr edit 413992 --fixed 123456

   # Record that no bug applies:
   $ ./gh pr edit 413992 --bug none

For managing Buganizer issues directly, see :ref:`module-pw_ghish-issue`.

Trailer preservation and GitHub ``#123`` syntax guard
=====================================================
* **Preserving commit trailers**: ``--title`` and ``--body`` preserve all
  existing Gerrit commit trailers (``Change-Id:``, ``Bug:``, ``Fixed:``,
  ``Cq-Include-Trybots:``). Full-message replacement via ``--message`` always
  retains ``Change-Id:`` and prompts for ``--drop-trailers`` if any other
  trailer would be deleted.
* **Rejecting GitHub closing keywords**: Gerrit ignores GitHub prose keywords
  like ``Fixes #456``. If ``pr create``, ``pr push``, or ``pr edit`` detects
  ``close``/``fix``/``resolve #<number>``, it stops and prints the ``--bug`` or
  ``--fixed`` trailer flag to use instead.

-----------------------------
Inspecting changes and status
-----------------------------

Review dashboard: ``pr status``
===============================
Displays a summary of:

1. **Current branch**: Shows the active change number, title, target branch,
   patchset number, submittability, label votes (e.g. ``Code-Review``,
   ``Presubmit-Verified``, ``Lint``), tryjob check status, and a summary of
   unresolved comment threads and unpublished drafts.
2. **Created by you**: Lists open changes you authored (scoped to the last 30
   days by default).
3. **Requesting a code review from you**: Lists changes awaiting your review
   (scoped to the last 30 days by default).

.. code-block:: console

   # Default view (last 30 days + active branch):
   $ ./gh pr status

   # Include open changes older than 30 days:
   $ ./gh pr status --all

Listing open changes: ``pr list``
=================================
Lists open changes for the current repository:

.. code-block:: console

   $ ./gh pr list
   $ ./gh pr list --limit 10 --state open
   $ ./gh pr list --author "hepler@google.com" --base main
   $ ./gh pr list --json number,title,state,branch

Inspecting a change: ``pr view``
================================
Displays change metadata, including owner, reviewers, attention set status,
and current review scores. When called without arguments, ``pr view`` inspects
the active change for your current branch:

.. code-block:: console

   # View active change on current branch:
   $ ./gh pr view

   # Include inline comment threads grouped by file and line number:
   $ ./gh pr view 413992 --comments

   # Output JSON (strictly validated against supported fields):
   $ ./gh pr view 413992 --json number,title,state,author,files,bug,bugs

``pr view --json bug,bugs`` parses commit message trailers on the patchset:
``bug`` returns a comma-separated string, while ``bugs`` returns objects with
``{"id": "b/123456", "closes": true}`` (``true`` for ``Fixed:``, ``false`` for
``Bug:``).

Viewing patch diffs: ``pr diff``
================================
Displays the unified diff of the latest patchset, or a specific revision:

.. code-block:: console

   $ ./gh pr diff
   $ ./gh pr diff 413992
   $ ./gh pr diff 413992/2

Local checkout and cherry-pick
==============================
Fetch a remote patchset into your local checkout:

.. code-block:: console

   # Fetch and checkout change at FETCH_HEAD:
   $ ./gh pr checkout 413992

   # Checkout a specific patchset:
   $ ./gh pr checkout 413992/3

   # Cherry-pick a change onto the current branch:
   $ ./gh pr cherry-pick 413992

----------------------------------
Reviewing, commenting, and landing
----------------------------------

Reviewing changes: ``pr review``
================================
Submits review scores or Commit-Queue votes on a Gerrit CL:

.. code-block:: console

   # Trigger Commit-Queue dry run on the active change (Commit-Queue+1):
   $ ./gh pr review --cq

   # Approve active change (Code-Review+2) and trigger CQ dry run:
   $ ./gh pr review --approve --cq

   # Approve a specific change with a comment:
   $ ./gh pr review 413992 --approve -m "Looks good."

   # Vote Code-Review-1 with a comment:
   $ ./gh pr review 413992 --request-changes -m "Please address formatting."

Posting inline comments: ``pr comment``
=======================================
Posts change-level or inline review comments. When ``--path`` and ``--line``
are provided, ``pw_ghish`` checks for an existing thread on that line across
all patchsets and appends your comment as a reply:

.. code-block:: console

   # Reply to an inline thread and mark as resolved:
   $ ./gh pr comment 413992 --path pw_string/string.cc --line 42 \
       -m "Fixed, using pw::Status." --resolved

   # Save an inline comment as a private draft without publishing:
   $ ./gh pr comment 413992 --path pw_string/string.cc --line 42 \
       -m "Consider std::string_view" --draft

   # Explicitly target a specific patchset:
   $ ./gh pr comment 413992/1 --path pw_string/string.cc --line 42 -m "Ack."

``--resolved`` requires both ``--path`` and ``--line`` so a change-level
comment cannot accidentally resolve a thread.

Lifecycle transitions: ``pr ready``, ``pr close``, and ``pr reopen``
====================================================================
Control the work-in-progress (WIP) and abandoned state of a change:

.. code-block:: console

   # Mark a WIP change ready for review:
   $ ./gh pr ready 413992

   # Convert an active change back to WIP (draft) with a message:
   $ ./gh pr ready 413992 --undo -m "Holding for upstream refactor."

   # Abandon or restore a change in Gerrit:
   $ ./gh pr close 413992
   $ ./gh pr reopen 413992

Checking CI status: ``pr checks``
=================================
``./gh pr checks`` queries LUCI Buildbucket tryjobs for the active change and
returns exit code ``0`` when all blocking checks pass, ``8`` while running, and
``1`` on failure:

.. code-block:: console

   $ ./gh pr checks
   $ ./gh pr checks --watch --fail-fast

For full documentation on ``pr checks`` and ``gh run``, see
:ref:`module-pw_ghish-run`.

Submitting changes: ``pr merge``
================================
Submits a change to the target branch:

.. code-block:: console

   # Enable automated submission once presubmits and reviews pass (recommended):
   $ ./gh pr merge 413992 --auto

   # Trigger Commit-Queue+2 submission directly:
   $ ./gh pr merge 413992 --cq

   # Attempt immediate submit (requires all checks and approvals to already be satisfied):
   $ ./gh pr merge 413992

If you run ``./gh pr merge`` without flags while submit requirements are still
pending, Gerrit returns HTTP 409 Conflict and ``pw_ghish`` prints the command to
enable ``--auto`` or ``--cq``.

.. _module-pw_ghish-pr-cq-auto:

Disambiguating ``--cq``, ``--auto``, and ``pr merge``
=====================================================
In GitHub, pushing a branch automatically triggers CI and ``gh pr merge`` merges
the branch directly. In Gerrit and LUCI, code uploads, presubmit dry runs
(``Commit-Queue+1``), auto-submit (``Pigweed-Auto-Submit+1``), and final
submission (``Commit-Queue+2``) are controlled by Gerrit label votes.

You can apply these votes either **when uploading a patchset** (via flags on
``pr create`` or ``pr push``) or **on an existing patchset without pushing
code** (via ``pr review`` or ``pr merge``):

.. list-table::
   :header-rows: 1
   :widths: 26 26 24 24

   * - Action
     - Gerrit Vote / API
     - During Upload
     - Without Upload
   * - **Run CI tryjobs (dry run)**
     - ``Commit-Queue+1``
     - ``./gh pr push --cq``
     - ``./gh pr review --cq``
   * - **Arm auto-submit**
     - ``Pigweed-Auto-Submit+1`` (or ``Auto-Submit+1``)
     - ``./gh pr push --auto``
     - ``./gh pr merge --auto``
   * - **Submit via Commit Queue**
     - ``Commit-Queue+2`` (requires ``Code-Review+2``)
     - ``./gh pr push --cq=2``
     - ``./gh pr merge --cq``
   * - **Direct REST submit**
     - Gerrit ``SubmitChange`` API (requires all gates green)
     - *(n/a)*
     - ``./gh pr merge``

.. tip::

   Note the difference in ``--cq`` defaults: ``pr push --cq`` and
   ``pr review --cq`` default to ``Commit-Queue+1`` (a presubmit dry run),
   whereas ``pr merge --cq`` votes ``Commit-Queue+2`` to land the change.

.. _module-pw_ghish-pr-targeting:

-----------------
Targeting changes
-----------------
Every ``./gh pr`` subcommand that accepts an optional ``[<id>]`` target (such
as ``view``, ``diff``, ``checkout``, ``checks``, ``review``, ``comment``,
``merge``, ``close``, ``reopen``, ``ready``, and ``edit``) supports the
following target formats:

* **Omitted argument**: Resolves the active change from your current Git branch
  or ``HEAD`` commit ``Change-Id``.
* **Change number**: A numeric Gerrit change ID (e.g. ``472267``) or change ID
  with patchset suffix (e.g. ``472267/3``).
* **Gerrit URL**: Full web or REST URLs (e.g.
  ``https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/472267`` or
  ``https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/472267/3``).
* **Shortlink**: Shortlinks such as ``pwrev/472267``, ``pwrev/472267/3``,
  ``fxrev/472267``, or ``crrev.com/c/472267``.
* **Branch name**: Local branch names (e.g. ``my-feature``, ``cl/472267``,
  ``change-472267``), resolved by inspecting the branch tip commit's
  ``Change-Id`` or ``branch.<name>.gerrit-change-id`` in Git config.

----------------------------------
Comparison with GitHub CLI (gh pr)
----------------------------------
.. list-table::
   :header-rows: 1
   :widths: 25 35 40

   * - Command / Flag
     - Gerrit Behavior in ``pw_ghish``
     - Difference from Upstream ``gh pr``
   * - ``pr create``
     - Pushes ``HEAD`` to ``refs/for/<base>`` as a **new** CL.
     - Stops if ``Change-Id`` already exists on Gerrit (use ``pr push``).
       Requires ``--stack`` for multiple commits.
   * - ``pr push``
     - Uploads a **new patchset** to an existing Gerrit CL.
     - **Gerrit adaptation** (replaces ``git push``). Queries target branch on
       Gerrit and supports ``--cq``, ``--auto``, ``--publish``, and ``--ready``.
   * - ``pr view [<id>]``
     - Queries Gerrit REST API for change metadata, votes, and ``--comments``.
     - Supports ``<id>/<patchset>`` and shortlinks (``pwrev/``). ``--json``
       adds ``bug`` and ``bugs`` trailer fields.
   * - ``pr edit [<id>]``
     - Updates commit message, reviewers, topic, hashtags, and votes via REST.
     - Preserves Git trailers (``Change-Id:``, ``Bug:``). Adds ``--bug`` and
       ``--fixed``; rejects ``Fixes #<num>`` syntax.
   * - ``pr list -a / --assignee``
     - Filters changes by Gerrit ``reviewer:``.
     - Gerrit 3.8+ removed assignees; ``-a`` queries reviewers instead.
   * - ``pr list -l / --label``
     - Filters by Gerrit vote predicate (e.g. ``Code-Review+2``).
     - Queries Gerrit label scores rather than GitHub issue labels.
   * - ``pr review --request-changes``
     - Votes ``Code-Review-1``.
     - In Gerrit, ``-1`` is advisory and does **not** block submission;
       ``Code-Review-2`` is the veto.
   * - ``pr comment --draft``
     - Stages an unpublished server-side draft comment.
     - Visible only to you until published (not a comment on a WIP PR).

For the cross-CLI flag compatibility policy and reserved shorthands, see
:ref:`module-pw_ghish-cli-comparison`.
