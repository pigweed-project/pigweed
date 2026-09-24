.. _module-pw_ghish-roadmap:

================
Status & roadmap
================
.. pigweed-module-subpage::
   :name: pw_ghish

This document outlines the current operational status of ``pw_ghish`` (``./gh``),
what is implemented and working today across its four command families, and the
planned roadmap as the tool expands from upstream Pigweed to broader
multi-project Gerrit ecosystems.

.. warning::

   **EXPERIMENTAL**: ``pw_ghish`` is **experimental for upstream Pigweed** and
   is **not ready for other projects yet**. While multi-project profile
   hooks exist in the codebase, non-Pigweed Gerrit and CI workflows are not yet
   supported.

--------------------------------
Current status: what works today
--------------------------------
``pw_ghish`` provides GitHub CLI workflows mapped onto Gerrit, LUCI
Buildbucket, Google Issue Tracker (Buganizer), and local Git/Bazel worktree
pools:

.. list-table::
   :widths: 22 20 58
   :header-rows: 1

   * - Capability Family
     - Status
     - Summary & Reference
   * - **Code Review** (``./gh pr``)
     - **Experimental (Pigweed)**
     - Full Gerrit CL lifecycle: ``status``, ``list``, ``view`` (with
       ``--comments`` and ``--json``), ``diff``, ``checkout``, ``cherry-pick``,
       ``create``, ``push`` (with server-side branch memory and stack guards),
       ``edit`` (preserving Git trailers), ``review``, ``comment`` (threaded
       replies, ``--resolved``, ``--draft``), ``ready``, ``close``, ``reopen``,
       and ``merge`` (``--auto`` and ``--cq``). See :ref:`module-pw_ghish-pr`.
   * - **CI & Tryjobs** (``./gh run`` & ``./gh pr checks``)
     - **Experimental (Pigweed)**
     - LUCI Buildbucket integration: ``pr checks`` with ``--watch``,
       ``--fail-fast``, and merge-gate exit codes (``0``/``8``/``1``);
       ``run list``; ``run view`` with collapsed recipe step trees and step
       failure log extraction (``--log-failed``); and ``run rerun`` via
       ``bb add``. See :ref:`module-pw_ghish-run`.
   * - **Issue Tracking** (``./gh issue``)
     - **Experimental (Pigweed)**
     - End-to-end Buganizer workflows: ``status``, ``list``, ``view``,
       ``develop`` (branch or ``--worktree`` allocation), ``create`` (with
       atomic ``--amend`` / ``--commit`` ``Bug:`` trailer insertion),
       ``comment``, ``edit`` (structured ``priority:``, ``severity:``,
       ``type:``, ``component:``, and ``hotlist:`` labels), ``close``, and
       ``reopen``. See :ref:`module-pw_ghish-issue`.
   * - **Worktree Management** (``./gh wt`` / ``./gh worktree``)
     - **Very Experimental**
     - Multi-agent Git worktree slot pool (``~/wrk/slots/pw-01..N``) paired with
       logical project symlinks (``~/wrk/projects/<name>``), shared 80 GB Bazel
       disk/repo caches, LRU ``PARKED`` eviction, and automatic Antigravity
       (Jetski) IDE sidebar registration. See :ref:`module-pw_ghish-worktree`.
   * - **Project Profiles**
     - **Working (Code-defined)**
     - Built-in profiles for ``pigweed``, ``fuchsia``, and ``generic`` Gerrit
       hosts, plus automatic ``Auto-Submit`` label discovery. See
       :ref:`module-pw_ghish-project-integration`.
   * - ``gh setup`` / **Agent Hooks**
     - **Planned**
     - Unified pre-flight credential diagnostics and automated agent tool-guard
       hook installation.
   * - **Declarative Config** (``.ghish.toml``)
     - **Planned**
     - Decoupling Go profile policies into repository-level declarative config
       files.
   * - **Alternative SCM** (``jj``)
     - **Under Evaluation**
     - Exploring interoperability with Jujutsu (``jj``) branchless workflows.

-----------------------------
Roadmap: planned capabilities
-----------------------------
The following sections describe planned features, architectural improvements,
and areas under active evaluation.

Multi-project policy decoupling
===============================
While ``pw_ghish`` includes built-in project profiles (Pigweed, Fuchsia, and
generic Gerrit instances), project policies are currently defined in Go code
(``pw_ghish/profile.go``).

Future milestones will decouple policy into declarative configuration:

* **Declarative project configuration**: Support repository-level configuration
  files (e.g. ``.ghish.toml`` or Git configuration) defining project behavior.
* **Configurable gating labels**: Enable projects to declare custom Commit-Queue
  votes and gating labels (e.g. ``Presubmit-Verified``,
  ``Integration-Verified``) alongside automatic auto-submit label detection.
* **Dynamic Buildbucket & Buganizer mapping**: Decouple LUCI bucket schemes
  (e.g. ``<project>/try``), builder classification rules, and default Buganizer
  component IDs so external teams can adopt ``pw_ghish`` with zero Go code
  modifications.
* **Project-specific URL templates**: Allow customizable shortlink resolvers
  and web UI links for private or downstream Gerrit instances.

Setup and onboarding automation
===============================
While ``./gh wt init`` already automates worktree slot pool creation, Bazel
cache configuration, and Gerrit ``commit-msg`` hook installation, a future
top-level ``./gh setup`` (or ``./gh auth status``) command will expand
pre-flight diagnostics across all backends:

* **Unified credential pre-flight diagnostics**: Verify that Gerrit credentials
  (``gob-curl`` or ``~/.gitcookies``), LUCI credentials (``luci-auth`` /
  ``bb auth-login``), and Buganizer OAuth2 scopes + GCP quota projects are
  configured and unexpired in a single check.
* **Agent pre-execution tool guards**: Install protective hooks for AI coding
  agents to prevent raw ``git push`` calls or unverified force-pushes.

Alternative SCM evaluation (Jujutsu / jj)
=========================================
The team is evaluating support for alternative source control management
systems, notably Jujutsu (``jj``):

* **Anonymous branchless commits**: Investigate how ``pw_ghish`` can resolve
  active changes and issues in repositories using ``jj``'s change-based model
  where named Git branches are optional.
* **Change-Id and trailer handling**: Ensure ``jj describe`` and automated
  commit rewrites preserve Gerrit ``Change-Id:`` and ``Bug:`` trailers cleanly.
* **Worktree interoperability**: Evaluate how ``./gh wt`` warm Bazel slot
  pooling interacts with ``jj workspace`` checkouts.

Agent safety and hook integration
=================================
Enhancing guardrails for autonomous workflows:

* **Universal dry-run mode**: Expand ``--dry-run`` across all mutating commands
  (``pr create``, ``pr push``, ``pr edit``, ``pr merge``, ``issue create``,
  ``issue edit``) matching ``run rerun --dry-run`` and ``wt gc --dry-run`` so
  agents can preview proposed mutations without remote side effects.
* **Local pre-push presubmit integration**: Allow ``pr push`` and ``pr create``
  to optionally trigger fast local formatting/lint checks prior to uploading
  patchsets, avoiding round-trip tryjob failures on trivial formatting issues.
