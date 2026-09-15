.. _module-pw_ghish-roadmap:

================
Status & roadmap
================
This document outlines the current operational status of ``pw_ghish``, what is
implemented and working today, and the planned roadmap as the tool expands
from upstream Pigweed to broader multi-project Gerrit ecosystems.

.. note::

   ``pw_ghish`` is currently validated and production-ready for **upstream
   Pigweed**. While multi-project infrastructure (such as profiles for Fuchsia
   and generic Gerrit hosts) is built into the codebase, broader multi-host
   deployment and non-Pigweed CI policies are under active development.

--------------------------------
Current status: what works today
--------------------------------
``pw_ghish`` provides a mature implementation of the core GitHub CLI (``gh``)
concepts mapped onto Gerrit and LUCI Buildbucket infrastructure.

Upstream Pigweed focus
======================
The primary production target for ``pw_ghish`` today is upstream Pigweed:

* **Zero-overhead wrapper**: The ``./gh`` repository script compiles
  ``//pw_ghish:gh-ish`` with cached Git commit hashes, eliminating Bazel startup
  overhead during day-to-day development.
* **Pigweed review policies**: First-class support for Pigweed labels,
  including ``Pigweed-Auto-Submit+1``, ``Commit-Queue+1`` (dry run),
  ``Commit-Queue+2`` (submit), and ``Code-Review``.
* **Buildbucket CI integration**: Direct integration with Pigweed's LUCI
  tryjobs (``pigweed/try``), supporting live progress tracking, error triage,
  and targeted re-execution.

Pull request workflows
======================
Most high-frequency ``gh pr`` workflows are fully functional:

* **Flexible change targeting**: Subcommands resolve the active branch
  automatically, or accept numeric change IDs (``472267``), patchsets
  (``472267/3``), Gerrit URLs, shortlinks (``pwrev/``, ``pwrev.dev/``,
  ``pwrev.dev/i/``, ``fxrev/``, ``crrev.com/c/``), and branch names.
* **Inspection**: ``pr view`` outputs change metadata, labels, and attention
  sets. ``pr view --comments`` formats nested review threads with file and line
  indents. ``pr diff`` fetches server-side patch diffs. ``pr checkout`` checks
  out specific changes and patchsets locally.
* **Push & creation**: ``pr create`` pushes new commits with guards against
  accidental overwrites or multi-commit stacks. ``pr push`` uploads new
  patchsets, automatically discovering remote target branches.
* **Editing & bug linking**: ``pr edit`` safely edits commit messages and
  trailers (preserving ``Change-Id:``, ``Bug:``, ``Fixed:``, and provenance),
  links issues via ``--bug``/``--fixed``, and toggles review votes.
* **Reviewing & commenting**: ``pr comment`` supports inline comments with
  automatic parent thread detection, ``--draft`` private comments, and
  ``--resolved`` thread closure. ``pr review`` supports approvals, change
  requests, and Commit-Queue voting.
* **Landing changes**: ``pr merge`` handles auto-submission (``--auto``),
  Commit-Queue delegation (``--cq``), and direct merges with actionable conflict
  diagnostics.
* **Developer dashboard**: ``pr status`` renders a real-time summary of the
  current branch, open incoming/outgoing reviews, and CI status.

CI monitoring and triage
========================
Pigweed CI builds are monitored and triaged through two complementary tiers:

* **High-level check table** (``gh pr checks``): Queries remote LUCI
  Buildbucket checks for the active change. Supports continuous monitoring
  (``--watch``), immediate exit on failure (``--fail-fast``), automated
  failure log summaries (``--log-failed``), experimental check filtering, and
  a strict exit code contract (``0`` for pass, ``8`` for pending, ``1`` for
  fail).
* **Deep run inspection** (``gh run``): ``run list`` displays tabular build
  summaries. ``run view -j <builder>`` renders hierarchical execution trees with
  1-line failure extractions and LogDog error snippets. ``run rerun`` re-runs
  failed checks (``--failed``) or individual builders without browser
  interaction.

AI agent integration
====================
``pw_ghish`` provides first-class support for AI pair programming:

* **Parametric knowledge**: AI agents pre-trained on GitHub CLI syntax can
  immediately use Gerrit and LUCI without bespoke prompting.
* **Private draft steering**: Humans and agents can exchange guidance via
  private draft comments (pr comment --draft) without alerting human
  reviewers.
* **Reactive CI monitoring**: Autonomous agents can launch ``pr checks --watch``
  and suspend until woken by completion, avoiding token-wasteful polling loops.

-----------------------------
Roadmap: planned capabilities
-----------------------------
The following sections describe planned features, architectural improvements,
and areas under active evaluation.

Multi-project policy decoupling
===============================
While ``pw_ghish`` includes initial support for project profiles (such as
Pigweed, Fuchsia, and generic Gerrit instances), project policies are currently
embedded in Go code.

Future milestones will fully decouple policy into declarative configuration:

* **Declarative project configuration**: Support repository-level configuration
  files (e.g. ``.ghish.toml`` or Git configuration) defining project behavior.
* **Configurable review labels**: Enable projects to define custom auto-submit
  labels (such as ``Auto-Submit`` vs. ``Pigweed-Auto-Submit``), CQ votes, and
  custom gating labels (e.g. ``Presubmit-Verified``, ``Integration-Verified``).
* **Dynamic Buildbucket mapping**: Decouple LUCI bucket schemes (e.g.
  ``<project>/try``) and builder classification rules so external teams can
  adopt ``pw_ghish`` with zero Go code modifications.
* **Project-specific URL templates**: Allow customizable shortlink resolvers
  and web UI links for private or downstream Gerrit instances.

Setup and onboarding automation
===============================
To simplify onboarding for new engineers and automated environments, a future
``gh setup`` (or ``gh init``) command will streamline repository configuration:

* **Automated Git hook installation**: Automatically check for and install the
  Gerrit ``commit-msg`` hook (which generates ``Change-Id:`` footers) if it is
  missing from ``.git/hooks/commit-msg``.
* **Agent pre-execution tool guards**: Install protective hooks and tool guards
  for AI agents (such as Antigravity/Jetski agent hooks). These guards prevent
  accidental operations such as pushing unstacked commits, pushing directly to
  remote branches without code review, or pushing without running local checks.
* **Credential pre-flight diagnostics**: Verify that required authentication
  credentials (``gob-curl`` on corp workstations, ``~/.gitcookies`` for
  external contributors, and ``bb auth-login`` for LUCI reruns) are configured
  and unexpired before executing workflows.

Alternative SCM evaluation (Jujutsu / jj)
=========================================
The team is evaluating the feasibility of supporting alternative source control
management systems, notably Jujutsu (``jj``):

* **Anonymous branchless commits**: Investigate how ``pw_ghish`` can operate in
  repositories using ``jj``'s change-based model where branches are optional.
* **Change-Id and trailer handling**: Ensure ``jj describe`` and automated
  commit rewrites preserve Gerrit ``Change-Id:`` and ``Bug:`` trailers without
  clobbering revision history.
* **Command interoperability**: Explore native ``jj`` command bridges or
  coexistence between ``jj git push`` and ``pw_ghish`` change management.

GitHub CLI command coverage
===========================
The GitHub CLI (``gh``) provides a broad suite of commands. ``pw_ghish``
focuses primarily on ``gh pr`` and ``gh run``, with other namespaces under
evaluation:

* ``gh auth``: Currently, ``pw_ghish`` employs zero-configuration
  workstation detection (``gob-curl`` on Google corp workstations,
  ``~/.gitcookies`` / ``.netrc`` externally). Whether to provide explicit
  ``gh auth login``, ``gh auth status``, and credential switching, or leave
  authentication delegated to Git and workstation tooling, is under evaluation.
* ``gh issue``: Issue tracking in Pigweed is handled via commit trailers
  (``Bug: b/123456``) and Buganizer/Monorail trackers. Future work may explore
  mapping ``gh issue view`` or ``gh issue list`` to Issue Tracker APIs.
* ``gh search``: Provide change search commands (e.g. ``gh search prs``)
  mapped to Gerrit's native search query syntax.
* **Intentionally out-of-scope commands**: Features tied to GitHub-specific
  platform hosting that have no Gerrit equivalents (such as ``gh release``,
  ``gh repo fork``, ``gh codespace``, ``gh secret``, and ``gh gist``) will
  remain unsupported, failing fast with informative guidance.

Agent safety and hook integration
=================================
Enhancing guardrails for autonomous workflows:

* **Universal dry-run mode**: Add a global ``--dry-run`` flag across all
  mutating commands (``pr create``, ``pr push``, ``pr edit``, ``pr merge``,
  ``run rerun``) so agents can preview proposed changes without remote side
  effects.
* **Local pre-push presubmit integration**: Allow ``pr push`` and ``pr create``
  to automatically trigger local presubmit checks (e.g. ``./pw presubmit``)
  prior to uploading patchsets, avoiding failing tryjob runs on trivial issues.

-----------------------
Command coverage matrix
-----------------------
The following table summarizes the implementation status of GitHub CLI command
families in ``pw_ghish``:

.. list-table::
   :widths: 20 20 60
   :header-rows: 1

   * - Command Family
     - Status
     - Description & Gerrit Mapping
   * - ``gh pr view``
     - Working (Today)
     - Full change metadata, review scores, comments (``-c``), and JSON.
   * - ``gh pr diff``
     - Working (Today)
     - Unified patch diff for active change or specified patchset.
   * - ``gh pr checkout``
     - Working (Today)
     - Fetches and checks out change branch or patchset locally.
   * - ``gh pr create``
     - Working (Today)
     - Pushes new change to Gerrit with safety guards against collisions.
   * - ``gh pr push``
     - Working (Today)
     - Uploads new patchset with automatic remote branch discovery.
   * - ``gh pr edit``
     - Working (Today)
     - Edits message, adds reviewers, links bugs, and toggles CQ votes.
   * - ``gh pr comment``
     - Working (Today)
     - Change and inline comments with auto-threading and private drafts.
   * - ``gh pr review``
     - Working (Today)
     - Approves (``+2``), requests changes (``-1``), or votes on CQ.
   * - ``gh pr merge``
     - Working (Today)
     - Enables auto-submit (``--auto``), CQ (``--cq``), or direct submit.
   * - ``gh pr checks``
     - Working (Today)
     - Live Buildbucket CI check status with ``--watch`` and exit codes.
   * - ``gh pr status``
     - Working (Today)
     - Focused review status dashboard for current branch and open CLs.
   * - ``gh run list``
     - Working (Today)
     - Lists all CI runs on the active change with duration and status.
   * - ``gh run view``
     - Working (Today)
     - Step execution tree with failure extractions and LogDog snippets.
   * - ``gh run rerun``
     - Working (Today)
     - Targeted rerun of failed checks (``--failed``) or single builders.
   * - ``gh setup``
     - Planned
     - Automatic ``commit-msg`` hook, agent tool guards, and auth checks.
   * - ``gh auth``
     - Under Evaluation
     - Currently zero-config; evaluating explicit login and status commands.
   * - ``gh issue``
     - Future Roadmap
     - Currently trailers-only; evaluating Issue Tracker API queries.
   * - ``gh search``
     - Future Roadmap
     - Change search mapped to Gerrit query syntax.
   * - ``gh release/gist``
     - Out of Scope
     - Platform-specific GitHub features with no Gerrit counterpart.
