.. _module-pw_ghish-issue:

=================
Issues (gh issue)
=================
.. pigweed-module-subpage::
   :name: pw_ghish

.. warning::

   **EXPERIMENTAL**: Buganizer issue integration (``./gh issue``) is
   **experimental** and under active development. Command syntax, JSON schemas,
   and Buganizer API mappings may evolve based on developer and AI agent
   feedback.

``./gh issue`` maps GitHub CLI issue workflows (``gh issue``) to **Google Issue
Tracker (Buganizer)**.

Instead of switching between a browser, Git commit messages, and Gerrit
reviews, you can triage bugs, branch for development, link issues to commits,
post updates, and close issues directly from your terminal using standard
``gh issue`` commands.

---------------
Quick reference
---------------
Run ``./gh issue`` subcommands from anywhere in your Pigweed checkout:

.. code-block:: console

   # Check issues assigned to you or reported by you:
   $ ./gh issue status

   # Inspect an issue and its comment history:
   $ ./gh issue view 315378787 --comments

   # File a new bug and automatically add 'Bug: b/<id>' to your HEAD commit:
   $ ./gh issue create --title "pw_rpc: Fix channel packet framing" \
       --body "Packets exceeding MTU drop trailing bytes." --amend

   # Post a comment to the issue linked in your current Git commit:
   $ ./gh issue comment -m "Uploaded fix in pwrev/477565."

   # Close the active issue once your fix lands:
   $ ./gh issue close --reason fixed -m "Merged in commit 4e43612e0."

----------------------------
Step 1: Triage and discovery
----------------------------
Before writing code, developers and AI agents typically inspect their queue or
search for open bugs in a specific component.

Checking your personal queue: issue status
==========================================
Run ``./gh issue status`` to display a two-section dashboard of open issues
assigned to you and open issues reported by you:

.. code-block:: console

   $ ./gh issue status

If you use :ref:`module-pw_ghish-worktree` to manage parallel tasks,
``./gh issue status`` automatically annotates issues that have an active
worktree project with their residency badge (such as ``[📂 MOUNTED: pw-01]`` or
``[💤 PARKED]``), showing which bugs are currently checked out on disk.

Searching and filtering: issue list
===================================
Use ``./gh issue list`` to query issues across the project. By default, it
lists open issues in the project's configured default Buganizer component:

.. code-block:: console

   # List open issues in the default component:
   $ ./gh issue list

   # List issues assigned to yourself or a specific teammate:
   $ ./gh issue list --assignee @me
   $ ./gh issue list --assignee "keir@google.com"

   # Filter by priority, type, or a specific Buganizer component ID:
   $ ./gh issue list --label priority:P1 --label type:BUG
   $ ./gh issue list --label component:123456 --limit 20

   # Combine structured flags with full-text Buganizer query syntax:
   $ ./gh issue list --search "pw_async2 dispatcher deadlock" --state all

Inspecting details and threads: issue view
==========================================
Once you identify an issue of interest, inspect its description, metadata
(priority, severity, status, assignee, component ID), and discussion history:

.. code-block:: console

   # View issue summary and description:
   $ ./gh issue view 315378787

   # Include the full chronological comment thread:
   $ ./gh issue view 315378787 --comments

---------------------
Step 2: Starting work
---------------------
When you are ready to start fixing an issue, ``./gh issue`` connects your
Buganizer workflow directly to your local Git branch and commit trailers.

Creating a feature branch: issue develop
========================================
If an issue already exists, use ``./gh issue develop`` to create and check out
a local feature branch for that issue:

.. code-block:: console

   # Creates and checks out branch 'b-315378787-<slug>' from main:
   $ ./gh issue develop 315378787 --checkout

   # Specify a custom branch name and base branch:
   $ ./gh issue develop 315378787 --name fix-rpc-framing --base main --checkout

   # Allocate a dedicated warm worktree slot instead of switching in place:
   $ ./gh issue develop 315378787 --worktree

When you pass ``--worktree`` (``-w``), ``./gh issue develop`` delegates to
:ref:`module-pw_ghish-worktree` (``./gh wt use --issue <id>``) to allocate a
warm physical slot and symlink in ``~/wrk/projects/``, leaving your primary Git
checkout untouched.

Filing and linking bugs on the fly: issue create
================================================
Often, you discover a bug while already working on a fix. Instead of opening a
browser to file an issue and manually copying the bug number into your commit
message, pass ``--amend`` to ``./gh issue create``:

.. code-block:: console

   $ ./gh issue create \
       --title "pw_tokenizer: Handle empty string literals in C++20" \
       --body "Empty string literals trigger a zero-length array warning." \
       --label priority:P2 \
       --assignee @me \
       --amend

``./gh issue create --amend`` performs two actions atomically:

1. Creates the issue in Buganizer and prints the new issue ID and URL.
2. Appends ``Bug: b/<new-id>`` to your current ``HEAD`` commit message while
   preserving your existing ``Change-Id`` and commit description.

If you have staged changes but have not created a commit yet, use ``--commit``
instead of ``--amend`` to create a new Git commit using the issue title and
``Bug: b/<new-id>`` trailer.

---------------------------------
Step 3: Working in branch context
---------------------------------
When working on an issue branch or inside a linked worktree, **you can omit the
issue ID argument from all issue commands**.

``pw_ghish`` automatically resolves the active issue using a three-tier
fallback chain:

1. **HEAD commit trailers**: Inspects ``Bug: b/<id>`` or ``Fixed: b/<id>``
   trailers on your current ``HEAD`` commit.
2. **Active branch name**: Parses standard issue branch naming patterns (such
   as ``b-315378787-fix-rpc``, ``issue-315378787``, or ``315378787-fix``),
   allowing issue commands to work immediately on a newly created branch before
   your first commit.
3. **Worktree metadata**: Queries your active :ref:`module-pw_ghish-worktree`
   project state if the worktree was initialized with ``--issue <id>``.

.. code-block:: console

   # Re-read the problem description for the bug you are currently fixing:
   $ ./gh issue view

   # Check recent comments from teammates on the active bug:
   $ ./gh issue view --comments

   # Post a diagnostic update without looking up the bug number:
   $ ./gh issue comment -m "Reproduced under ASAN; root cause is in framing.cc."

   # Bump the priority of the active bug:
   $ ./gh issue edit --add-label priority:P1

If a commit references multiple ``Bug:`` trailers, ``pw_ghish`` resolves the
primary issue or prompts you to disambiguate explicitly.

----------------------------------
Step 4: Collaborating and updating
----------------------------------
As your investigation progresses, keep stakeholders informed and keep issue
metadata accurate from the command line.

Posting progress updates: issue comment
=======================================
Add comments inline with ``-m`` / ``--body`` or from a file with
``--body-file`` (use ``-`` to read from standard input):

.. code-block:: console

   $ ./gh issue comment 315378787 -m "Patch uploaded to pwrev/477565."
   $ ./gh issue comment --body-file investigation_notes.md

Updating metadata and labels: issue edit
========================================
Modify the title, description, assignee, or structured Buganizer fields using
``./gh issue edit``:

.. code-block:: console

   # Reassign an issue and update its title:
   $ ./gh issue edit 315378787 --assignee "reviewer@google.com" \
       --title "pw_rpc: Fix channel packet MTU framing"

   # Update priority, severity, or component using GitHub-style label syntax:
   $ ./gh issue edit --add-label priority:P1,severity:S1
   $ ./gh issue edit --add-label component:654321

   # Add or remove an issue from a Buganizer hotlist:
   $ ./gh issue edit --add-label hotlist:9876543
   $ ./gh issue edit --remove-label hotlist:9876543

``pw_ghish`` translates GitHub CLI's ``--label`` / ``--add-label`` /
``--remove-label`` flags into native Buganizer fields:

.. list-table::
   :header-rows: 1

   * - Label syntax
     - Buganizer field
     - Examples
   * - ``priority:<P0..P4>``
     - Priority
     - ``--add-label priority:P1``
   * - ``severity:<S0..S4>``
     - Severity
     - ``--add-label severity:S2``
   * - ``type:<TYPE>``
     - Issue Type
     - ``--add-label type:BUG`` (``BUG``, ``FEATURE``, ``TASK``)
   * - ``component:<id>``
     - Component ID
     - ``--add-label component:123456``
   * - ``hotlist:<id>``
     - Hotlist IDs
     - ``--add-label hotlist:9876543``

------------------------
Step 5: Closing the loop
------------------------
When your change merges or an investigation concludes, resolve the issue with
an explicit status and optional closing comment.

Resolving issues: issue close
=============================
Close an issue as ``fixed`` (default) or specify an alternative Buganizer
resolution reason:

.. code-block:: console

   # Mark the active issue as FIXED with a closing comment:
   $ ./gh issue close -m "Fixed in commit 4e43612e0."

   # Close with a specific resolution reason:
   $ ./gh issue close 315378787 --reason wontfix -m "Working as intended."
   $ ./gh issue close 315378787 --reason not_reproducible

   # Mark as a duplicate of another issue:
   $ ./gh issue close 315378787 --reason duplicate --duplicate-of 111222333

Supported ``--reason`` values:

* ``fixed`` (or ``completed``): Maps to Buganizer ``FIXED``.
* ``wontfix`` (or ``not_planned``, ``obsolete``): Maps to ``WONT_FIX_OBSOLETE``.
* ``intended_behavior``: Maps to ``WONT_FIX_INTENDED_BEHAVIOR``.
* ``not_reproducible``: Maps to ``WONT_FIX_INFEASIBLE``.
* ``duplicate``: Maps to ``DUPLICATE`` (requires ``--duplicate-of <id>``).

Reopening issues: issue reopen
==============================
If a regression occurs or follow-up work is needed, reopen a closed issue:

.. code-block:: console

   $ ./gh issue reopen 315378787 -m "Reopening: issue recurs on Cortex-M4."

--------------------------------
Scripting and AI agent workflows
--------------------------------
``./gh issue`` is designed for both human developers and autonomous AI coding
agents.

Flexible issue targeting
========================
Every subcommand that accepts an issue target supports any of the following
formats interchangeably:

* **Omitted argument**: Resolves automatically from ``HEAD`` commit trailers
  (``Bug:`` / ``Fixed:``), branch naming conventions, or active worktree
  metadata.
* **Numeric ID**: ``315378787``
* **Buganizer shorthand**: ``b/315378787``
* **Issue tracker URLs**: ``https://issues.pigweed.dev/issues/315378787``,
  ``https://issues.chromium.org/issues/315378787``, or
  ``https://issuetracker.google.com/issues/315378787``

Structured JSON output
======================
For programmatic scripts or agent tool calls, pass ``--json`` with a
comma-separated list of fields to ``view``, ``list``, or ``status``:

.. code-block:: console

   $ ./gh issue view 315378787 --json id,title,state,priority,assignee,comments
   $ ./gh issue list --assignee @me --json id,title,priority,url

Supported JSON fields: ``id``, ``number``, ``title``, ``body``, ``state``,
``status``, ``priority``, ``severity``, ``type``, ``assignee``, ``reporter``,
``componentId``, ``hotlistIds``, ``url``, ``createdAt``, ``updatedAt``, and
``comments``.

-------------------------------------
Comparison with GitHub CLI (gh issue)
-------------------------------------
While ``./gh issue`` adopts standard ``gh issue`` commands and flags, Google
Issue Tracker (Buganizer) has a structured data model and integrates with
Gerrit through Git commit trailers rather than pull request prose:

.. list-table::
   :header-rows: 1
   :widths: 26 34 40

   * - Feature / Command
     - Upstream ``gh issue``
     - ``./gh issue`` (Buganizer)
   * - **Issue labels** (``-l, --label``)
     - Free-form text strings (e.g. ``bug``, ``good first issue``).
     - Structured key-value prefixes mapped to Buganizer fields:
       ``priority:P0..P4``, ``severity:S0..S4``, ``type:BUG|FEATURE|TASK``,
       ``component:<id>``, and ``hotlist:<id>``.
   * - **Linking commits / PRs**
     - Prose keywords in PR description (``Fixes #123``).
     - Git commit trailers (``Bug: b/<id>`` and ``Fixed: b/<id>``). Supported
       directly via ``issue create --amend``, ``issue create --commit``, and
       ``pr edit --bug`` / ``--fixed``.
   * - **Omitted issue ID**
     - Requires an explicit issue number on ``view``, ``comment``, ``edit``,
       and ``close``.
     - **Zero-argument resolution**: Automatically infers the active issue from
       ``HEAD`` commit trailers, branch naming (``b-315378787-...``), or
       active :ref:`module-pw_ghish-worktree` metadata.
   * - **Close reasons** (``--reason``)
     - ``completed`` or ``not_planned``.
     - Supports ``completed``/``fixed`` and ``not_planned``/``wontfix``, plus
       Buganizer-specific resolutions: ``intended_behavior``,
       ``not_reproducible``, and ``duplicate --duplicate-of <id>``.
   * - ``issue develop``
     - Creates a Git branch linked to a GitHub issue.
     - Creates branch ``b-<id>-<slug>`` or, with ``-w, --worktree``, allocates
       an isolated warm slot via ``./gh wt use --issue <id>``.

--------------
Authentication
--------------
``./gh issue`` authenticates with Google Issue Tracker using an automatic OAuth2
credential cascade:

1. ``GHISH_ISSUE_TOKEN`` environment variable (if explicitly set).
2. ``luci-auth`` (standard on workstations configured for Pigweed, Fuchsia, or
   Gerrit).
3. ``gcloud auth`` (Google Cloud SDK Application Default Credentials or active
   user credentials).

To log in with the required Buganizer scope:

.. code-block:: console

   # Using LUCI Auth (recommended for Pigweed developers):
   $ luci-auth login -scopes "https://www.googleapis.com/auth/buganizer https://www.googleapis.com/auth/cloud-platform"

   # Or using Google Cloud SDK:
   $ gcloud auth application-default login --scopes="https://www.googleapis.com/auth/buganizer,https://www.googleapis.com/auth/cloud-platform"

API quota project configuration
===============================
When calling the Google Issue Tracker REST API with CLI credentials, Google's
API gateway requires a Google Cloud consumer project ID
(``X-Goog-User-Project``) for rate-limit accounting (Issue Tracker API usage
itself is free / $0).

``pw_ghish`` automatically resolves a quota project from your environment
(``GHISH_QUOTA_PROJECT``, ``git config ghish.quotaproject``, or ``gcloud``
configuration). If no quota project is detected, enable the Issue Tracker API
on any Google Cloud project you have access to and configure ``gh-ish`` to use
it:

.. code-block:: console

   # 1. Enable the Google Issue Tracker API on your GCP project:
   $ gcloud services enable issuetracker.googleapis.com --project=<gcp-project-id>

   # 2. Configure gh-ish to use that project for rate-limit quota:
   $ git config --global ghish.quotaproject <gcp-project-id>
