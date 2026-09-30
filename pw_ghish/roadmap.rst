.. _module-pw_ghish-roadmap:

================
Status & roadmap
================
.. pigweed-module-subpage::
   :name: pw_ghish

This document summarizes the current status of ``pw_ghish`` (``./gh``) and the
remaining work required before the tool can be used outside upstream Pigweed.

.. warning::

   **EXPERIMENTAL**: ``pw_ghish`` is **experimental for upstream Pigweed** and
   is **not ready for other projects yet**. While multi-project profile
   hooks exist in the codebase, non-Pigweed Gerrit and CI workflows are not yet
   supported.

--------------------------------
Current status: what works today
--------------------------------
.. list-table::
   :widths: 28 22 50
   :header-rows: 1

   * - Capability
     - Status
     - Summary
   * - **Code Review** (``./gh pr``)
     - **Experimental (Pigweed)**
     - Create, push, view, diff, edit, review, comment on, and merge Gerrit CLs.
       See :ref:`module-pw_ghish-pr`.
   * - **CI & Tryjobs** (``./gh run``, ``pr checks``)
     - **Experimental (Pigweed)**
     - Watch tryjobs, inspect recipe step trees, fetch failure logs, and rerun
       builders. See :ref:`module-pw_ghish-run`.
   * - **Issue Tracking** (``./gh issue``)
     - **Experimental (Pigweed)**
     - List, view, create, branch, edit, comment on, and close Buganizer issues.
       See :ref:`module-pw_ghish-issue`.
   * - **Authentication** (``./gh auth``)
     - **Experimental (Pigweed)**
     - Check Gerrit, LUCI, and Buganizer credentials in ``googler`` and
       ``community`` modes. See :ref:`module-pw_ghish-auth`.
   * - **Hooks** (``./gh hook``)
     - **Experimental (Pigweed)**
     - Install and run coding agent ``PreToolUse`` guards and Gerrit Git hooks.
       See :ref:`module-pw_ghish-hook`.
   * - **Worktrees** (``./gh wt``)
     - **Very Experimental**
     - Pool warm Git/Bazel worktree slots and sync Antigravity (Jetski) IDE
       projects. See :ref:`module-pw_ghish-worktree`.
   * - **Project Profiles**
     - **Working (Code-defined)**
     - Go profiles for ``pigweed``, ``fuchsia``, and ``generic`` hosts. See
       :ref:`module-pw_ghish-project-integration`.
   * - **Declarative Config** (``.ghish.toml``)
     - **Planned**
     - Move project policies and Pigweed defaults out of Go code into
       repository config files.
   * - **Alternative SCM** (``jj``)
     - **Under Evaluation**
     - Evaluate interoperability with Jujutsu (``jj``) branchless workflows.

-----------------------------
Roadmap: planned capabilities
-----------------------------

Multi-project policy decoupling
===============================
While ``pw_ghish`` includes built-in project profiles (``pigweed``, ``fuchsia``,
and ``generic``), project policies and several Pigweed-specific defaults still
live in Go code.

Planned work to support external Gerrit and LUCI repositories:

* **Declarative project configuration**: Support repository-level configuration
  files (e.g. ``.ghish.toml`` or Git configuration) defining project behavior
  without modifying Go code.
* **Removing remaining Pigweed-isms**: Extract hardcoded Pigweed defaults (such
  as bootstrapped CIPD binary paths, default GCP quota projects, and slot
  prefixes) into project configuration.
* **Configurable gating labels**: Allow projects to declare custom Commit-Queue
  votes and gating labels (e.g. ``Presubmit-Verified``,
  ``Integration-Verified``) alongside automatic auto-submit label detection.
* **Dynamic Buildbucket & Buganizer mapping**: Configure LUCI bucket names,
  builder classification rules, and default Buganizer component IDs per
  repository.
* **Project-specific URL templates**: Support custom shortlink resolvers and
  web UI links for private or downstream Gerrit instances.

Alternative SCM evaluation (Jujutsu / jj)
=========================================
Evaluating support for alternative source control management systems, notably
Jujutsu (``jj``):

* **Anonymous branchless commits**: Resolve active changes and issues in
  repositories using ``jj``'s change-based model where named Git branches are
  optional.
* **Change-Id and trailer handling**: Ensure ``jj describe`` and automated
  commit rewrites preserve Gerrit ``Change-Id:`` and ``Bug:`` trailers.
* **Worktree interoperability**: Evaluate how ``./gh wt`` warm Bazel slot
  pooling interacts with ``jj workspace`` checkouts.

Dry-run and local presubmit options
===================================
* **Universal dry-run mode**: Expand ``--dry-run`` across mutating commands
  (``pr create``, ``pr push``, ``pr edit``, ``pr merge``, ``issue create``,
  ``issue edit``) matching ``run rerun --dry-run`` and ``wt gc --dry-run``.
* **Local pre-push checks**: Allow ``pr push`` and ``pr create`` to optionally
  run fast local formatting or lint checks before uploading patchsets.
