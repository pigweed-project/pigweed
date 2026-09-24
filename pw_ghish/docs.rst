.. _module-pw_ghish:

========
pw_ghish
========
.. pigweed-module::
   :name: pw_ghish

   * **GitHub CLI syntax for Gerrit, LUCI, and Buganizer**: Interact with
     Gerrit code reviews (``gh pr``), LUCI Buildbucket tryjobs (``gh run``),
     and Buganizer issues (``gh issue``) using standard ``gh`` commands.
   * **Built for humans and coding agents**: Developers and AI agents already
     familiar with the GitHub CLI can create changes, reply to review threads,
     inspect CI logs, and manage bugs without custom REST scripts.
   * **Worktree and Bazel cache pooling**: Manage isolated Git worktrees backed
     by a shared pool of warm Bazel output bases and IDE workspace
     synchronization via ``./gh wt``.
   * **Repository wrapper**: Run ``./gh`` from the repository root with
     automatic per-commit compilation and local binary caching in ``out/gh/``.

``pw_ghish`` (invoked via the ``./gh`` wrapper at the repository root) provides
GitHub CLI commands on top of **Gerrit Code Review**, **LUCI Buildbucket**,
**Google Issue Tracker (Buganizer)**, and **local Git/Bazel worktree pools**.

.. warning::

   **EXPERIMENTAL**: ``pw_ghish`` is **experimental for upstream Pigweed** and
   is **not ready for other projects yet**. Despite its broad functionality,
   the tool is still very fresh and under active iteration—commands, flags, and
   workflows may change. See :ref:`module-pw_ghish-roadmap` for current status
   and upcoming milestones.

---------------
Getting started
---------------
Run the ``./gh`` repository wrapper from anywhere in your Pigweed checkout:

.. code-block:: console

   # View review status for the active branch and your open CLs:
   $ ./gh pr status

   # Push a new patchset on the current branch to Gerrit:
   $ ./gh pr push --cq

   # Watch LUCI tryjob builders and print step failure logs on failure:
   $ ./gh pr checks --watch --fail-fast

   # Inspect step execution trees or rerun failed builders via LUCI:
   $ ./gh run view -j pigweed-lintformat
   $ ./gh run rerun --failed

   # File a Buganizer issue and add 'Bug: b/<id>' to your HEAD commit:
   $ ./gh issue create --title "pw_foo: Fix bar overflow" --body "..." --amend

   # Allocate an isolated warm worktree slot for an issue or feature:
   $ ./gh wt use --issue 315378787

   # Enable automated submission once review and CI checks pass:
   $ ./gh pr merge --auto

The ``./gh`` wrapper compiles and caches the ``//pw_ghish:gh-ish`` binary in
``out/gh/`` keyed by Git commit hash, reusing the cached binary when ``HEAD``
has not changed.

Root-level aliases and global flags
===================================
For common commands, ``./gh`` provides top-level aliases and global flags:

* **Root aliases**: ``./gh push`` (``pr push``), ``./gh status``
  (``pr status``), ``./gh view`` (``pr view``), ``./gh diff`` (``pr diff``),
  ``./gh checks`` (``pr checks``), and ``./gh worktree`` (``wt``).
* **Global flags**: ``--host <domain>`` (override Gerrit host),
  ``--profile <name>`` (force ``pigweed``, ``fuchsia``, or ``generic`` profile),
  and ``-v, --verbose`` (enable debug logging).

-------------
Documentation
-------------
.. grid:: 2

   .. grid-item-card:: :octicon:`rocket` Life of a PR
      :link: module-pw_ghish-life-of-a-pr
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Step-by-step walkthrough of creating a CL, running LUCI presubmits,
      addressing review comments, and landing via Commit-Queue or Auto-Submit.

   .. grid-item-card:: :octicon:`git-pull-request` Code review (gh pr)
      :link: module-pw_ghish-pr
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Gerrit code review commands: creating and pushing patchsets, editing
      commit trailers, replying to inline threads, and merging changes.

.. grid:: 2

   .. grid-item-card:: :octicon:`check-circle` CI & tryjobs (gh run)
      :link: module-pw_ghish-run
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      LUCI Buildbucket commands: watching ``pr checks``, inspecting recipe
      step trees, reading failure logs, and rerunning builders.

   .. grid-item-card:: :octicon:`issue-opened` Issues (gh issue)
      :link: module-pw_ghish-issue
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Buganizer issue workflows: triaging queues, branching, filing bugs with
      commit trailers, updating structured labels, and closing issues.

.. grid:: 2

   .. grid-item-card:: :octicon:`repo-forked` Worktrees (gh wt)
      :link: module-pw_ghish-worktree
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Manage warm Git worktree slots, project symlinks, shared Bazel caches,
      and Antigravity (Jetski) IDE workspace synchronization.

   .. grid-item-card:: :octicon:`cpu` AI workflows
      :link: module-pw_ghish-ai-workflows
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Workflows for AI pair programming: private draft steering, staged review
      replies, CL handoffs, CI failure repair, and parallel agents.

.. grid:: 2

   .. grid-item-card:: :octicon:`table` GitHub CLI comparison
      :link: module-pw_ghish-cli-comparison
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Ecosystem mapping matrix, flag differences from upstream ``gh``, and the
      three-tier flag compatibility policy.

   .. grid-item-card:: :octicon:`gear` Project adoption
      :link: module-pw_ghish-project-integration
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Building standalone binaries, setting up repository wrappers, configuring
      project profiles, and workstation/bot authentication.

.. grid:: 2

   .. grid-item-card:: :octicon:`milestone` Status & roadmap
      :link: module-pw_ghish-roadmap
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Operational status, capability matrix, and roadmap for declarative
      profiles, setup automation, and alternative SCMs.

   .. grid-item-card:: :octicon:`checklist` Agent evaluation
      :link: module-pw_ghish-agent-eval
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Verification runbook for coding agents, behavioral rubric, and live
      integration test suite (``live_test.go``).

.. toctree::
   :maxdepth: 1
   :hidden:

   life_of_a_pr
   pr
   run
   issue
   worktree
   ai_workflows
   cli_comparison
   project_integration
   roadmap
   agent_eval
