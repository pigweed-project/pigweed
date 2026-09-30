.. _module-pw_ghish-subcommands:

===========
Subcommands
===========
.. pigweed-module-subpage::
   :name: pw_ghish

``pw_ghish`` (invoked via ``./gh``) organizes its commands by the backend
service or local resource they manage. Each subcommand page documents the
available commands, flags, and differences from upstream GitHub CLI (``gh``).

.. toctree::
   :maxdepth: 1
   :hidden:

   gh pr <pr>
   gh run <run>
   gh issue <issue>
   gh auth <auth>
   gh hook <hook>
   gh wt <worktree>

----------------
Command families
----------------
.. list-table::
   :header-rows: 1
   :widths: 22 22 26 30

   * - Subcommand
     - Upstream ``gh`` Target
     - ``pw_ghish`` Target
     - Summary & Reference
   * - :ref:`gh pr <module-pw_ghish-pr>`
     - GitHub Pull Requests
     - **Gerrit Change Lists (CLs)**
     - Create and push patchsets, vote Commit-Queue / Auto-Submit, edit commit
       trailers, reply to inline threads, and submit changes.
   * - :ref:`gh run <module-pw_ghish-run>` & ``pr checks``
     - GitHub Actions Runs & Checks
     - **LUCI Buildbucket & Recipes**
     - Watch blocking tryjobs, inspect recipe step trees, fetch step failure
       logs, and rerun builders via ``bb add``.
   * - :ref:`gh issue <module-pw_ghish-issue>`
     - GitHub Issues
     - **Google Issue Tracker (Buganizer)**
     - Triage queues, create issues with ``Bug: b/<id>`` commit trailers, apply
       structured priority/severity/type labels, and close bugs.
   * - :ref:`gh auth <module-pw_ghish-auth>`
     - GitHub Host Authentication
     - **Gerrit, LUCI & Buganizer Auth**
     - Check credentials across Gerrit, LUCI Buildbucket, and Buganizer in
       ``googler`` and ``community`` modes.
   * - :ref:`gh hook <module-pw_ghish-hook>`
     - *(None — gh-ish extension)*
     - **Agent Tool Guards & Git Hooks**
     - Install and run coding agent ``PreToolUse`` guards and Gerrit
       ``commit-msg`` / ``pre-push`` Git hooks.
   * - :ref:`gh wt <module-pw_ghish-worktree>`
     - *(None — gh-ish extension)*
     - **Git Worktrees & Bazel Cache Pool**
     - Allocate warm physical build slots (``pw-01..N``), manage project
       symlinks, and sync Antigravity (Jetski) IDE workspaces.

-----------------------------------
Root-level aliases and global flags
-----------------------------------
For common operations, ``./gh`` provides top-level aliases and global flags:

* **Root aliases**:

  * ``./gh push`` (alias for ``./gh pr push``)
  * ``./gh status`` (alias for ``./gh pr status``)
  * ``./gh view`` (alias for ``./gh pr view``)
  * ``./gh diff`` (alias for ``./gh pr diff``)
  * ``./gh checks`` (alias for ``./gh pr checks``)
  * ``./gh worktree`` (alias for ``./gh wt``)
  * ``./gh agent`` (alias for ``./gh hook``)

* **Global flags**:

  * ``--host <domain>``: Override the target Gerrit host.
  * ``--profile <name>``: Force the ``pigweed``, ``fuchsia``, or ``generic``
    project profile.
  * ``--auth-mode <mode>``: Set the authentication mode (``auto``, ``googler``,
    ``community``, or ``none``). See :ref:`module-pw_ghish-auth-modes`.
  * ``-v, --verbose``: Enable debug logging.
