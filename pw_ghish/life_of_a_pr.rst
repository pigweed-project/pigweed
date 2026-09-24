.. _module-pw_ghish-life-of-a-pr:

============
Life of a PR
============
.. pigweed-module-subpage::
   :name: pw_ghish

This guide walks through the end-to-end lifecycle of a Gerrit change in Pigweed
using ``./gh``: creating a change, running LUCI presubmits, iterating on review
feedback, and landing the change through the Commit Queue.

-------------------------------------
How Gerrit + LUCI differs from GitHub
-------------------------------------
If you are used to GitHub pull requests, keep three differences in mind:

1. **One commit per CL**: A Gerrit Change List (CL) is a single Git commit
   identified by a ``Change-Id: I...`` trailer in its commit message. To update
   a CL after review feedback, you amend your commit (``git commit --amend``)
   and run ``./gh pr push`` to upload a new **patchset**.
2. **Tryjobs run on demand** (``--cq``): Pushing a patchset does not always run
   the full presubmit suite automatically. Passing ``--cq`` (``Commit-Queue+1``)
   starts a LUCI presubmit dry run.
3. **Changes land via Commit-Queue or Auto-Submit**: Rather than clicking a
   direct merge button, changes land when they have ``Code-Review+2`` approval
   and pass a ``Commit-Queue+2`` verification run (triggered automatically via
   ``--auto`` or explicitly via ``./gh pr merge --cq``).

----------------------------------------
Step 1: Commit locally and link a bug ID
----------------------------------------
Create a branch (or allocate an isolated worktree slot with
``./gh wt use <name>``), make your edits, and create a Git commit:

.. code-block:: console

   $ git checkout -b fix-ring-buffer
   $ git commit -am "pw_ring_buffer: Fix off-by-one in Peek()"

If you need to file a new Buganizer issue and add its ``Bug: b/<id>`` trailer
to your commit in one step:

.. code-block:: console

   $ ./gh issue create \
       --title "pw_ring_buffer: Off-by-one in Peek()" \
       --body "Peek() drops the final byte when buffer is full." \
       --amend

Or, if an issue already exists, include ``Bug: b/<id>`` in your commit message
(or add it later with ``./gh pr edit --bug b/123456``).

--------------------------------------------
Step 2: Create the CL and start a CQ dry run
--------------------------------------------
Upload your commit to Gerrit as a new CL, request a reviewer, start a
Commit-Queue dry run (``--cq``), and enable auto-submit (``--auto``):

.. code-block:: console

   $ ./gh pr create -r "reviewer@google.com" --cq --auto

What this command does:

* Installs the Gerrit ``commit-msg`` hook and adds a ``Change-Id:`` footer if
  your commit does not have one yet.
* Pushes ``HEAD`` to ``refs/for/main`` to create Patchset 1.
* Adds ``reviewer@google.com`` as a reviewer.
* Votes ``Commit-Queue+1`` (``--cq``) to start LUCI presubmit tryjobs.
* Votes ``Pigweed-Auto-Submit+1`` (``--auto``) so the CL will automatically
  land once a reviewer approves it with ``Code-Review+2`` and tryjobs pass.

-----------------------------------------
Step 3: Watch tryjobs and fix CI failures
-----------------------------------------
Monitor the presubmit dry run from your terminal:

.. code-block:: console

   $ ./gh pr checks --watch --fail-fast

If a blocking builder fails, ``--fail-fast`` exits immediately and prints the
failing step summary and log snippet. You can also inspect any builder on
demand:

.. code-block:: console

   # Inspect the step execution tree for a specific builder:
   $ ./gh run view -j pigweed-lintformat

   # Print failure summaries and step log excerpts for all failed builders:
   $ ./gh run view --log-failed

To fix a CI failure and upload **Patchset 2**:

.. code-block:: console

   # 1. Fix the issue and amend your local commit:
   $ ./pw format --fix
   $ git commit -a --amend --no-edit

   # 2. Upload the new patchset and restart the CQ dry run:
   $ ./gh pr push --cq

-------------------------------------------
Step 4: Respond to inline reviewer feedback
-------------------------------------------
Check your review dashboard or read inline reviewer comments on your branch's
active CL:

.. code-block:: console

   $ ./gh pr status
   $ ./gh pr view --comments

``./gh pr view --comments`` prints each review thread with its file path, line
number, and resolution state.

As you address each comment:

1. **Stage inline replies and mark threads resolved**:
   Pass ``--path`` and ``--line`` to reply directly inside the existing thread.
   Use ``--draft`` to stage replies privately while you work:

   .. code-block:: console

      $ ./gh pr comment --path pw_ring_buffer/ring_buffer.cc --line 84 \
          -m "Switched to pw::Result<ConstByteSpan>." --resolved --draft

2. **Amend your commit and push the new patchset**:
   Pass ``--publish`` to publish all your staged draft replies together with
   the new patchset, and ``--cq`` to start a fresh dry run:

   .. code-block:: console

      $ git commit -a --amend --no-edit
      $ ./gh pr push --publish --cq

-------------------------------
Step 5: Land (merge) the change
-------------------------------
Once a reviewer grants ``Code-Review+2``, there are two ways the CL lands:

* **Automatic submission** (if you enabled ``--auto``):
  Because ``Pigweed-Auto-Submit+1`` is set, LUCI automatically triggers
  ``Commit-Queue+2`` as soon as ``Code-Review+2`` is applied, rebases your CL
  onto the tip of ``main``, runs any remaining verification, and merges it.
* **Explicit submission** (via ``pr merge``):
  If you did not set ``--auto`` earlier, enable it or trigger
  ``Commit-Queue+2`` directly:

  .. code-block:: console

     # Option A: Enable auto-submit (lands as soon as CR+2 and CQ pass):
     $ ./gh pr merge --auto

     # Option B: Vote Commit-Queue+2 directly (requires CR+2 to be present):
     $ ./gh pr merge --cq

-----------------------------------------------
Quick lookup: ``--cq``, ``--auto``, & ``merge``
-----------------------------------------------
Because Gerrit separates code upload from Commit-Queue voting, the same labels
can be set either **while uploading a patchset** (``pr create`` / ``pr push``)
or **on an already-uploaded CL** (``pr review`` / ``pr merge``):

.. list-table::
   :header-rows: 1
   :widths: 28 30 42

   * - Goal
     - While uploading code
     - Without uploading code
   * - **Run presubmit tryjobs (dry run)**
     - ``./gh pr push --cq``
     - ``./gh pr review --cq``
   * - **Arm auto-submit** (``Auto-Submit+1``)
     - ``./gh pr push --auto``
     - ``./gh pr merge --auto``
   * - **Submit via Commit Queue** (``CQ+2``)
     - ``./gh pr push --cq=2``
     - ``./gh pr merge --cq``
   * - **Immediate REST submit (all gates green)**
     - *(n/a)*
     - ``./gh pr merge``
