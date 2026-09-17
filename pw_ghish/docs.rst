.. _module-pw_ghish:

========
pw_ghish
========
.. pigweed-module::
   :name: pw_ghish

   * **Familiar GitHub CLI interface**: Seamlessly interact with Gerrit code
     reviews and LUCI CI checks using standard ``gh pr`` command syntax.
   * **Unlocks parametric agent knowledge**: AI coding agents pre-trained on
     the GitHub CLI can review, comment, push, and submit changes without
     custom prompting.
   * **Autonomous CI driving & agentic polling**: Eliminates human context
     switching by enabling AI agents to watch, poll, extract step failure logs,
     and drive changes to ground without human intervention.
   * **Zero-setup repository wrapper**: Instant execution via ``./gh`` with
     automatic per-commit compilation and caching.
   * **Pigweed CI & review integration**: First-class support for Pigweed
     auto-submit, Commit-Queue, and LUCI tryjobs.
   * **Buganizer issue integration**: Triage, branch, create, and resolve issues
     using standard ``gh issue`` commands linked directly to Git commits.
   * **Multi-project support**: Pluggable profiles and standalone binary
     distribution for Fuchsia and generic Gerrit projects.

``pw_ghish`` (invoked via the ``./gh`` wrapper at the repository root) provides
GitHub CLI (``gh pr`` and ``gh issue``) ergonomics on top of Gerrit code review,
Google Issue Tracker (Buganizer), and LUCI CI infrastructure.

.. warning::

   ``pw_ghish`` is currently validated and production-ready only for **upstream
   Pigweed**. While foundational profile infrastructure for other projects
   exists, multi-project decoupling and non-Pigweed CI workflows are under
   active development. See the :ref:`module-pw_ghish-roadmap` page for current
   status, feature availability, and upcoming milestones.

---------------
Quick reference
---------------
Run the ``./gh`` repository wrapper from anywhere in your Pigweed checkout:

.. code-block:: console

   # View review dashboard for active branch & your open CLs:
   $ ./gh pr status

   # Check open Buganizer issues assigned to or reported by you:
   $ ./gh issue status

   # File a new bug and automatically add 'Bug: b/<id>' to your HEAD commit:
   $ ./gh issue create --title "pw_foo: Fix bar overflow" --body "..." --amend

   # Push a new patchset on the current branch to Gerrit:
   $ ./gh pr push

   # Check live status and duration of LUCI tryjob builders:
   $ ./gh pr checks

   # View failure summaries and step log snippets in the terminal:
   $ ./gh run view --log-failed

   # Reply to an inline comment thread and mark as resolved:
   $ ./gh pr comment --path <file> --line <line> -m "Done." --resolved

   # Enable automated submission once review and CI checks pass:
   $ ./gh pr merge --auto

-------------
Documentation
-------------
.. grid:: 2

   .. grid-item-card:: :octicon:`terminal` CLI User Guide (gh pr)
      :link: module-pw_ghish-cli
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Subcommand reference and examples for viewing, pushing, reviewing,
      commenting, inspecting CI, and merging Gerrit changes.

   .. grid-item-card:: :octicon:`issue-opened` Issue Tracking (gh issue)
      :link: module-pw_ghish-issue
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      End-to-end user journey for triaging, branching, filing bugs with commit
      trailers, commenting, and resolving Buganizer issues.

.. grid:: 2

   .. grid-item-card:: :octicon:`cpu` AI Workflows
      :link: module-pw_ghish-ai-workflows
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Workflows for AI pair programming: private draft steering, staged review
      replies, CL handoffs, and CI failure triage.

   .. grid-item-card:: :octicon:`table` GitHub CLI Comparison
      :link: module-pw_ghish-cli-comparison
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Command comparison with GitHub CLI (``gh``), flag mapping, and
      architectural differences in Gerrit.

.. grid:: 2

   .. grid-item-card:: :octicon:`milestone` Status & Roadmap
      :link: module-pw_ghish-roadmap
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Operational status, upstream Pigweed focus, and roadmap for multi-project
      decoupling, SCM exploration, setup automation, and hooks.

   .. grid-item-card:: :octicon:`gear` Project Adoption Guide
      :link: module-pw_ghish-project-integration
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Installing standalone binaries, setting up repository wrappers,
      configuring project profiles, and authentication.

.. grid:: 2

   .. grid-item-card:: :octicon:`sliders` Flag Compatibility Policy
      :link: module-pw_ghish-flag-compatibility
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Compatibility principles with upstream GitHub CLI, compatibility tiers,
      and ghish-only flag designations.

   .. grid-item-card:: :octicon:`checklist` Agent Evaluation Runbook
      :link: module-pw_ghish-agent-eval
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Verification runbook for coding agents, behavioral rubric, and live
      integration test suite.

.. toctree::
   :maxdepth: 1
   :hidden:

   cli
   issue
   ai_workflows
   cli_comparison
   flag_compatibility
   roadmap
   project_integration
   agent_eval
