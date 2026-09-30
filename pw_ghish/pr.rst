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
* ``--force``: Force upload even if no change with this ``Change-Id`` exists on
  Gerrit.
* ``-o, --push-option <opt>``: Pass Gerrit push options (e.g.
  ``-o topic=my-feature``).
* ``--no-verify``: Bypass local pre-push Git hooks.

Target branch discovery and stack guard
=======================================
When updating an existing change, ``pr push`` queries Gerrit by the commit's
``Change-Id`` to determine the target branch recorded on the server (for
example, ``sandbox/my-experiment``) and pushes to
``refs/for/<recorded-branch>``. Use ``-B, --base <branch>`` to override the
target branch explicitly. If no change matching ``HEAD``'s ``Change-Id`` exists
on Gerrit (for example, if ``git commit --amend -m`` replaced the original
``Change-Id``), ``pr push`` inspects ``HEAD@{1}`` in the local Git reflog,
reports the exact clobbered ``Change-Id`` and CL number if ``HEAD@{1}`` matched
an existing change on Gerrit, and directs you to restore the original
``Change-Id`` or run ``pr create`` (pass ``--force`` to bypass).

Like ``pr create``, ``pr push`` requires ``--stack`` when uploading multiple
unpushed commits. With ``--stack``, both commands verify every commit in the
stack for missing, multiple, or duplicate ``Change-Id`` trailers, unsquashed
``fixup!``/``squash!`` commits, and stack ``Change-Id`` continuity.

-------------------------
Editing metadata: pr edit
-------------------------
Updates reviewers, assignees, topic, hashtags, and votes of an existing change
through the Gerrit REST API without pushing a new patchset:

.. code-block:: console

   # Add a reviewer and trigger a CQ dry run:
   $ ./gh pr edit 413992 --add-reviewer colleague@google.com --cq

   # Set a Gerrit topic and add hashtags:
   $ ./gh pr edit 413992 --topic my-feature --add-hashtag triage

Supported flags
===============
* ``--add-reviewer <email>`` / ``--remove-reviewer <email>``: Add or remove
  reviewers.
* ``--add-assignee <email>`` / ``--remove-assignee <email>``: Add or remove
  assignees.
* ``--add-label <Label=Value>``: Apply a Gerrit label vote (e.g.
  ``--add-label Commit-Queue=1``).
* ``--cq [0|1|2]``: Vote on ``Commit-Queue`` (default ``1``; ``0`` removes vote).
* ``--topic <str>`` / ``--remove-topic``: Set or remove the Gerrit topic.
* ``--add-hashtag <str>`` / ``--remove-hashtag <str>``: Add or remove Gerrit
  hashtags.

Editing commit messages and bug trailers
========================================
Unlike GitHub (where pull request titles and descriptions live in the server
database independently of Git commits), Gerrit stores the CL description inside
the Git commit message of each patchset. Editing the commit message remotely on
Gerrit without updating your local Git commit causes the next ``./gh pr push``
or ``git rebase`` to overwrite the remote edit.

For this reason, commit-message flags on ``pr edit`` (``--title``, ``--body``,
``--message``, ``--bug``, and ``--fixed``) are currently disabled
(`b/567763970 <https://issues.pigweed.dev/issues/567763970>`_) and print
instructions for editing the local Git commit message surgically:

.. code-block:: console

   # 1. Dump the current commit message to a temporary file:
   $ git log -1 --format=%B HEAD > "$(git rev-parse --git-dir)/COMMIT_EDITMSG_TMP"

   # 2. Edit "$(git rev-parse --git-dir)/COMMIT_EDITMSG_TMP" surgically (keeping the existing Change-Id: line intact),
   #    then apply the updated message and upload a new patchset:
   $ git commit --amend --only -F "$(git rev-parse --git-dir)/COMMIT_EDITMSG_TMP"
   $ ./gh pr push

For an earlier commit in a multi-CL stack, use ``git rebase -i`` while
preserving every commit's ``Change-Id:`` footer, then run
``./gh pr push --stack``.

GitHub ``#123`` closing keyword guard
=====================================
Gerrit ignores GitHub prose keywords like ``Fixes #456``. If ``pr create`` or
``pr push`` detects ``close``/``fix``/``resolve #<number>`` in a commit
message, it stops and directs you to use ``Bug: b/<id>`` or ``Fixed: b/<id>``
trailers instead.

-----------------------------
Inspecting changes and status
-----------------------------

Review dashboard: ``pr status``
===============================
Displays a summary of:

1. **Current branch**: Shows the active change number, title, target branch,
   patchset number, submittability, label votes (e.g. ``Code-Review``,
   ``Presubmit-Verified``, ``Lint``), tryjob check status, and a summary of
   unresolved comment threads and unpublished drafts (previewing up to two
   unresolved threads and standalone ``[DRAFT]`` comments inline with
   ``[PS<N>]`` tags and message snippets).
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

   # Include inline comment threads and unpublished drafts grouped by file and line:
   $ ./gh pr view 413992 --comments

   # Output JSON (strictly validated against supported fields, including comments and drafts):
   $ ./gh pr view 413992 --json number,title,state,author,files,bug,bugs,comments,drafts

``pr view --comments`` renders each thread with its ``[PS<N>]`` patchset tag,
``[DRAFT]`` indicator, and ``[resolved]`` / ``[unresolved]`` status (or
``Comments: None`` when no comments or drafts exist).

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
Submits review scores, Commit-Queue votes, or staged draft comments on a Gerrit
CL:

.. code-block:: console

   # Trigger Commit-Queue dry run on the active change (Commit-Queue+1):
   $ ./gh pr review --cq

   # Approve active change (Code-Review+2) and trigger CQ dry run:
   $ ./gh pr review --approve --cq

   # Approve a specific change with a comment:
   $ ./gh pr review 413992 --approve -m "Looks good."

   # Vote Code-Review-1 with a comment:
   $ ./gh pr review 413992 --request-changes -m "Please address formatting."

   # Publish all staged draft comments on the change without uploading a patchset:
   $ ./gh pr review 413992 --publish

Posting inline comments: ``pr comment``
=======================================
Posts change-level, file-level, or inline review comments (via ``-m, --message``
or ``-b, --body``). When ``--path`` (and optionally ``--line``) is provided,
``pw_ghish`` checks for an existing thread at that target across patchsets and
appends your comment as a reply while keeping any unrelated drafts private
(``Drafts: KEEP``):

.. code-block:: console

   # Reply to an inline thread and mark as resolved:
   $ ./gh pr comment 413992 --path pw_string/string.cc --line 42 \
       -m "Fixed, using pw::Status." --resolved

   # Save or update an inline comment as a private draft without publishing:
   $ ./gh pr comment 413992 --path pw_string/string.cc --line 42 \
       -m "Consider std::string_view" --draft

   # Delete an unpublished draft comment on a file and line:
   $ ./gh pr comment 413992 --path pw_string/string.cc --line 42 --delete-draft

   # Explicitly target or disambiguate a specific patchset:
   $ ./gh pr comment 413992/1 --path pw_string/string.cc --line 42 -m "Ack."

When ``--draft`` is used on a thread or line that already has an unpublished
draft from you, ``pw_ghish`` updates that draft in place (preserving ``Side``
and character ``Range``) rather than creating a duplicate draft. If multiple
unpublished drafts exist at the same location across different patchsets,
``pr comment`` refuses to guess and prompts you to disambiguate with
``--patchset <N>`` or ``<id>/<N>``. ``--resolved`` requires ``--path`` (and
``--line`` for inline threads) so a change-level comment cannot accidentally
resolve a thread.

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

.. _module-pw_ghish-pr-comparison:

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
       Adds ``--stack``, ``--cq``, ``--auto``, and ``-o, --push-option``.
   * - ``pr push``
     - Uploads a **new patchset** to an existing Gerrit CL.
     - **Gerrit adaptation** (replaces ``git push``). Queries target branch on
       Gerrit and supports ``--stack``, ``--cq``, ``--auto``, ``--publish``,
       ``--ready``, and ``-o, --push-option``.
   * - ``pr view [<id>]``
     - Queries Gerrit REST API for change metadata, votes, and ``--comments``.
     - Supports ``<id>/<patchset>`` and shortlinks (``pwrev/``). ``--json``
       adds ``bug`` and ``bugs`` trailer fields; ``state`` reports Gerrit's
       ``NEW`` / ``MERGED`` / ``ABANDONED`` instead of GitHub's
       ``OPEN`` / ``CLOSED`` / ``MERGED``.
   * - ``pr edit [<id>]``
     - Updates reviewers, assignees, topic, hashtags, and votes via REST.
     - Adds ``--cq``, ``--topic``, ``--remove-topic``, ``--add-hashtag``, and
       ``--remove-hashtag``. Commit-message flags (``--title``, ``--body``,
       ``--message``, ``--bug``, ``--fixed``) redirect to local Git commit
       editing (`b/567763970 <https://issues.pigweed.dev/issues/567763970>`_).
   * - ``pr list -a / --assignee``
     - Filters changes by Gerrit ``reviewer:``.
     - Gerrit 3.8+ removed assignees; ``-a`` queries reviewers instead.
   * - ``pr list -l / --label``
     - Filters by Gerrit vote predicate (e.g. ``Code-Review+2``).
     - Queries Gerrit label scores rather than GitHub issue labels.
   * - ``pr review --request-changes``
     - Votes ``Code-Review-1``.
     - In Gerrit, ``-1`` is advisory and does **not** block submission;
       ``Code-Review-2`` is the veto. Also supports ``--cq`` and ``--publish``.
   * - ``pr comment``
     - Posts change-level or inline threaded comments (``--path``, ``--line``).
     - Adds ``--resolved`` to mark an inline thread resolved, ``--draft`` to
       create or update an unpublished server-side draft comment, and
       ``--delete-draft`` to delete an unpublished draft.
   * - ``pr ready``
     - Marks a WIP change ready for review, or WIP with ``-u, --undo``.
     - Adds ``-m, --message`` to attach a status note to the state transition.
   * - ``pr merge``
     - Submits the CL directly, via ``--cq`` (``Commit-Queue+2``), or via
       ``--auto`` (``Auto-Submit+1``).
     - Does not support ``--squash``, ``--rebase``, or ``--delete-branch``
       (merge strategy is configured on the Gerrit repository).

Several ``pr`` flags intentionally omit single-letter shorthands because
upstream ``gh`` binds those letters to other meanings: ``--auto`` (``-a`` is
``--assignee``), ``--publish`` (``-p`` is ``--project``), ``--force`` on
``checkout`` (``-f`` is ``--fill``), ``--cq`` (``-q`` is ``--jq``), and
``--message`` on ``edit`` and ``merge`` (``-m`` is ``--milestone`` and
``--merge``). See :ref:`module-pw_ghish-flag-compatibility` for the cross-CLI
flag compatibility policy.
