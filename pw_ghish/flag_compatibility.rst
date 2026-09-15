.. _module-pw_ghish-flag-compatibility:

=========================
Flag compatibility policy
=========================
.. pigweed-module-subpage::
   :name: pw_ghish

This document outlines the CLI flag compatibility policy for ``pw_ghish``
relative to the official GitHub CLI (``gh``).

------------------------------------
Philosophy & maintenance principles
------------------------------------
``pw_ghish`` is designed to minimize cognitive friction for developers and
autonomous coding agents accustomed to GitHub CLI workflows, while faithfully
respecting the operational reality of Gerrit and LUCI infrastructure.

To ensure long-term stability and avoid divergence bugs, the following rules
govern all flags supported by ``pw_ghish``:

1. **Exact semantic alignment for upstream flags**:
   When a flag exists in upstream GitHub CLI (e.g. ``--web``, ``--undo``,
   ``--comments``, ``--json``, ``--base``, ``--limit``, ``--state``),
   ``pw_ghish`` must provide the same user-facing behavior or a strictly
   faithful Gerrit equivalent.

2. **No silent divergence**:
   A flag must never silently behave differently from its GitHub CLI counterpart.
   If an upstream flag cannot be supported safely or cleanly in Gerrit,
   ``pw_ghish`` must reject it with an explicit error explaining the limitation
   rather than silently ignoring it or applying unintended behavior.

3. **Non-intersecting flags for Gerrit-specific concepts**:
   Gerrit introduces concepts with no direct GitHub analogue (e.g. Commit-Queue
   voting, thread resolution state, server-side draft comments, hashtags).
   When adding flags for these concepts, ``pw_ghish`` must choose
   **non-intersecting flag names** that do not conflict with existing or
   foreseeable upstream GitHub CLI flags.

4. **Explicit designation of ghish-only flags**:
   Gerrit-specific flags are explicitly designated as **ghish-only** in help
   text, man pages, and this documentation. This clarifies to human developers
   and GenAI agents which flags are portable to GitHub and which are tailored
   specifically to the Gerrit/LUCI environment.

-------------------
Compatibility tiers
-------------------

Tier 1: Upstream equivalent flags
=================================
These flags match upstream ``gh`` in syntax, type, and semantic outcome:

.. list-table::
   :widths: 20 20 60
   :header-rows: 1

   * - Command
     - Flag
     - Behavior
   * - ``pr view``
     - ``-w, --web``
     - Opens the change in the web browser.
   * - ``pr view``
     - ``-c, --comments``
     - Displays inline review comments and discussion threads.
   * - ``pr view``
     - ``--json <fields>``
     - Outputs machine-readable JSON matching the requested fields.
   * - ``pr ready``
     - ``-u, --undo``
     - Marks the change as draft / work-in-progress (WIP).
   * - ``pr list``
     - ``-s, --state``
     - Filters changes by state (``open``, ``closed``, ``merged``, ``all``).
   * - ``pr list``
     - ``-L, --limit``
     - Limits the number of returned changes.
   * - ``pr list``
     - ``-B, --base``
     - Filters changes by base branch.
   * - ``pr list``
     - ``-A, --author``
     - Filters changes by author.
   * - ``pr merge``
     - ``--auto``
     - Enables automated submit once CI and reviews pass.
   * - ``pr diff``
     - ``--patch``
     - Outputs raw patch diff suitable for ``git apply``.
   * - ``pr checks``
     - ``--watch``
     - Polls CI checks until all builders complete.

Tier 2: Upstream compatible extensions
======================================
These flags extend standard ``gh`` commands in a way that remains natural and
consistent with GitHub conventions:

.. list-table::
   :widths: 20 20 60
   :header-rows: 1

   * - Command
     - Flag
     - Behavior
   * - ``pr ready``
     - ``-m, --message``
     - Adds an optional status message when marking ready or moving to WIP.
   * - ``pr review``
     - ``--body, -m``
     - Top-level review comment message (standard in ``gh pr review``).
   * - ``pr comment``
     - ``-m, --body``
     - Comment body text (standard in ``gh pr comment``).
   * - ``pr comment``
     - ``--path, --line``
     - Inline file path and line location for code review comments.
   * - ``run view``
     - ``--log-failed``
     - Fetches and displays log snippets for failed build steps.
   * - ``pr push`` / ``create``
     - ``--stack``
     - Permits pushing multiple local commits as a stacked change series.

Tier 3: Gerrit-native / ghish-only flags
========================================
These flags control Gerrit-specific or LUCI-specific mechanics. They are chosen
specifically to avoid namespace collisions with future GitHub CLI features:

.. list-table::
   :widths: 20 20 60
   :header-rows: 1

   * - Command
     - Flag
     - Behavior
   * - ``pr push`` / ``review``
     - ``--cq [vote]``
     - Triggers LUCI Commit-Queue validation (1 = dry run, 2 = submit).
   * - ``pr push`` / ``merge``
     - ``--auto-submit``
     - Sets the project auto-submit label (e.g. ``Pigweed-Auto-Submit+1``).
   * - ``pr push`` / ``create``
     - ``-r, --reviewer``
     - Adds reviewers to the change.
   * - ``pr push`` / ``create``
     - ``--cc``
     - Adds users to the carbon-copy (CC) list without requesting review.
   * - ``pr push``
     - ``--publish``
     - Publishes all pending draft comments upon pushing the new patchset.
   * - ``pr push``
     - ``--push-option, -o``
     - Passes raw Git push options (e.g. ``-o uploadvalidator~skip``).
   * - ``pr comment``
     - ``--draft``
     - Saves an inline comment as a private draft rather than publishing immediately.
   * - ``pr comment``
     - ``--resolved``
     - Toggles the Gerrit thread resolution state (``--resolved=true/false``).
   * - ``pr edit``
     - ``--topic``
     - Sets the Gerrit topic string across related changes.
   * - ``pr edit``
     - ``--hashtag``
     - Adds or removes Gerrit hashtags.

----------------------------
Adding new flags in gh-ish
----------------------------
When adding or proposing a new flag to ``pw_ghish``:

1. **Check official GitHub CLI reference**:
   Consult ``gh help <command>`` to see if an official flag already exists for
   the desired functionality. If it exists, adopt the exact same flag name,
   short option, and expected value format.

2. **Verify non-intersection**:
   If introducing a Gerrit-specific feature, verify that the proposed flag name
   does not collide with any current or planned ``gh`` flags.

3. **No silent no-ops**:
   Never accept a flag without implementing its behavior. If a flag is added for
   forward-compatibility, it must perform the operation or explicitly return an
   unsupported error.
