.. _module-pw_ghish-sense:

=====================================
Context sensing and /crank (gh sense)
=====================================
.. pigweed-module-subpage::
   :name: pw_ghish

Moving a change from initial code to a merged commit typically involves a steady
stream of small status checks: seeing whether the branch has fallen behind the
target branch, reading new inline comments in Gerrit, checking whether you left
yourself any private draft notes on the diff, inspecting failing tryjob logs,
and re-running presubmits after uploading a fix.

When pair programming with an AI coding agent, guiding the agent through those
steps one prompt at a time (*"check the comments"*, *"now rebase"*, *"now check
why the build failed"*) quickly becomes repetitive. Almost everything needed to
determine the right next step already exists in your local Git checkout, Gerrit
review threads, LUCI Buildbucket tryjobs, and issue tracker.

The ``/crank`` workflow—powered under the hood by ``./gh sense``—gathers that
context in a single pass and advances your work to its next meaningful
milestone, while keeping you in control of every public action.

----------------------------------
Everyday workflows with ``/crank``
----------------------------------
You can invoke ``/crank`` with no arguments in your current checkout, or point
it at a specific issue or Gerrit change. In each case, ``./gh sense`` inspects
the surrounding state so the agent can focus on the engineering work needed next.

Iterating on an active change
=============================
When you run ``/crank`` on a branch with an open Gerrit change (or local commits
ready to upload), ``./gh sense`` checks your branch's relationship to the target
branch, inspects unresolved reviewer threads and private draft comments, and
summarizes the latest tryjob results.

From that snapshot, ``/crank`` drives the change forward:

* **Thoughtful rebasing**: If your branch is significantly behind the target
  branch or has a merge conflict, ``/crank`` rebases it and resolves routine
  build-file conflicts. However, if your Commit-Queue tryjobs are already
  passing or running and Gerrit reports no merge conflicts, ``/crank``
  deliberately skips rebasing so you do not lose existing approvals or trigger
  redundant 20-minute presubmit runs.
* **Calibrated review responses**: When reviewers leave inline comments,
  ``/crank`` evaluates each thread against codebase constraints before editing
  code. It implements valid fixes, answers questions, and respectfully pushes
  back with evidence if a suggestion would violate embedded or architectural
  requirements (such as introducing dynamic allocation in a no-heap module).
* **Draft-first replies**: All inline replies and thread resolutions are staged
  privately in Gerrit using ``--draft``. Public comments are only published
  after local tests and remote presubmit checks pass, giving you a clean
  patchset-to-patchset diff and drafted replies to inspect.
* **Automated CI diagnosis and repair**: If a blocking tryjob fails, ``./gh
  sense`` surfaces the failing builder and step log excerpt immediately so
  ``/crank`` can reproduce the failure locally, apply a fix, upload a new
  patchset, and watch Commit-Queue until checks are green.

Steering your own change with private Gerrit drafts
===================================================
Sometimes it is much easier to guide an agent by pointing at specific lines in a
side-by-side diff viewer than by describing file paths and function names in a
chat prompt.

With ``/crank`` and ``./gh sense``, you can open your own change in the Gerrit
web UI, click on any line in the diff, and save an unpublished draft comment
with instructions (for example, *"Switch this helper to return pw::Result"* or
*"Add a boundary test for an empty span here"*). Do not click **Send**—leave
the comments saved as private drafts on the server, then run ``/crank``.

``./gh sense`` automatically separates your private self-steering notes from
replies to external reviewers:

1. ``/crank`` reads your private notes alongside any external reviewer feedback.
2. It implements your requested changes in the code and verifies them with unit
   tests.
3. It deletes your temporary self-steering drafts from Gerrit so human reviewers
   never see your internal notes, and uploads the new patchset.

Investigating and fixing an issue
=================================
When you pass an issue identifier or URL—such as ``/crank b/315378787``—``./gh
sense`` fetches the issue description, recent discussion comments, and any
Gerrit changes already linked to that bug. It also checks whether you already
have a local branch or ``./gh wt`` worktree tracking the issue.

* **Avoiding duplicate or obsolete work**: If ``./gh sense`` finds that the
  issue is already closed, or if an initial check on the target branch shows the
  problem has already been fixed upstream, ``/crank`` stops and reports that
  evidence rather than inventing unnecessary code changes.
* **End-to-end resolution**: For active bugs, ``/crank`` prepares a clean
  feature branch (or guides you to your existing worktree), reproduces the
  issue, writes a fix with unit tests, performs a self-review check, and uploads
  the change to Gerrit with Commit-Queue enabled.

Picking up or reviewing a Gerrit change
=======================================
When you pass a Gerrit change shortlink, number, or URL—such as ``/crank
pwrev/472267``—``./gh sense`` inspects the change's status, ownership, open
review threads, and CI results:

* **Resuming or adopting a change**: If the change is yours (or a stalled change
  you are picking up to finish), ``/crank`` checks out the patchset, rebases if
  needed, addresses any outstanding review comments or CI failures, and drives
  it to passing presubmits.
* **Reviewing a teammate's change**: When you use ``/crank`` to review a
  colleague's change, it inspects the diff and stages calibrated inline
  ``--draft`` comments in Gerrit—prioritizing real bugs, safety issues, and API
  concerns while avoiding noisy style nits—so you can review and edit the draft
  feedback in the Gerrit UI before clicking **Send**.

Starting from a clean checkout
==============================
When you finish a change (or after your previous change has merged) and run
``/crank`` in a clean checkout with no arguments, ``./gh sense`` looks across
your broader workspace to help you pick up the next task:

* If you use ``./gh wt`` worktree pools, it highlights other active or parked
  workstreams that have new reviewer comments, failing presubmits, or passing
  approvals ready to land.
* If your project configures an oncall rotation schedule and you are currently
  on duty, it surfaces your rotation status so you can step straight into triage.
* Otherwise, it lists your highest-priority open assigned issues so you can
  start the next bug fix immediately.

---------------------------------------------
Using ``./gh sense`` directly in the terminal
---------------------------------------------
While ``./gh sense`` is designed as the fast context-gathering engine for
``/crank``, you can also run it directly from your terminal whenever you want a
concise status briefing on your current branch or a remote target:

.. code-block:: console

   # Summarize the current checkout, rebase status, active CL, comments, and CI:
   $ ./gh sense

   # Inspect a specific issue or Gerrit CL alongside your local checkout state:
   $ ./gh sense b/315378787
   $ ./gh sense pwrev/472267

   # Include a summary of all managed ./gh wt worktrees across your local pool:
   $ ./gh sense --fleet

   # Safely fetch the target branch and prepare a clean branch for a task:
   $ ./gh sense --prepare b/315378787

Safe branch preparation with ``--prepare``
==========================================
When starting a new bug or checking out a Gerrit change, ``./gh sense
--prepare [<target>]`` can automatically fetch the latest target branch and
switch your checkout to the appropriate feature branch.

Because switching branches in an active checkout can be disruptive if work is
still in progress, ``--prepare`` enforces strict non-clobbering safety checks
before touching your working tree. It will only switch or reset a branch when:

* Your working tree and index have **zero uncommitted or untracked changes**.
* No rebase, merge, cherry-pick, or bisect operation is in progress.
* Your current branch is either already sitting at the target branch with zero
  local commits, or tracks a Gerrit change that has **already been merged** on
  the server with the exact same commit hash as your local ``HEAD``.
* The target issue or change is not already checked out in another active
  worktree.

If any of those conditions is not met—for example, if you have uncommitted edits
or an open unmerged change on the current branch—``--prepare`` leaves your
working tree untouched and explains why.
