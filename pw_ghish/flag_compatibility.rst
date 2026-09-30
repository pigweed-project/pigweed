.. _module-pw_ghish-flag-compatibility:
.. _module-pw_ghish-cli-comparison:

=============
Compat policy
=============
.. pigweed-module-subpage::
   :name: pw_ghish

``pw_ghish`` (``./gh``) uses GitHub CLI (``gh``) command and flag syntax on top
of Gerrit, LUCI Buildbucket, and Google Issue Tracker (Buganizer). This page
defines the policy for keeping flags compatible with upstream ``gh``. For
command-specific differences, see the comparison sections in
:ref:`gh pr <module-pw_ghish-pr-comparison>`,
:ref:`gh run <module-pw_ghish-run-comparison>`,
:ref:`gh issue <module-pw_ghish-issue-comparison>`,
:ref:`gh auth <module-pw_ghish-auth-comparison>`,
:ref:`gh hook <module-pw_ghish-hook-comparison>`, and
:ref:`gh wt <module-pw_ghish-worktree-comparison>`.

Both developers and coding agents bring existing habits from ``gh``. The goal
of ``pw_ghish`` is that typing a familiar ``gh`` command either works as
expected against the Gerrit/LUCI/Buganizer equivalent, or stops immediately
with an error that explains the difference and shows the right ``./gh``
command. It should never silently ignore a flag or repurpose an upstream ``gh``
flag to do something unexpected.

-------------------------
Flag compatibility policy
-------------------------
All flags in ``pw_ghish`` follow four rules:

1. **Semantic alignment for upstream flags**: When a flag exists in upstream
   GitHub CLI (e.g. ``--web``, ``--undo``, ``--comments``, ``--json``,
   ``--base``, ``--limit``, ``--state``), ``pw_ghish`` provides the same
   user-facing behavior or a direct Gerrit/LUCI/Buganizer equivalent.
2. **No silent divergence**: If an upstream flag cannot be supported safely in
   Gerrit, ``pw_ghish`` rejects it with an explicit error explaining the
   limitation rather than ignoring it.
3. **Non-intersecting flags for ecosystem-specific concepts**: When adding flags
   for Gerrit, LUCI, or Buganizer mechanics with no GitHub analogue (e.g.
   ``--cq``, ``--auto-submit``, ``--publish``, ``--resolved``, ``--bug``,
   ``--fixed``, ``--amend``, ``--auth-mode``), ``pw_ghish`` chooses
   non-intersecting flag names.
4. **Explicit designation of ghish-only flags**: Ecosystem-specific flags are
   marked as **ghish-only** in help text and documentation.

Unbound shorthands and out-of-scope commands
============================================
``pw_ghish`` does not reuse an upstream ``gh`` short flag for an unrelated
action. Where a single-letter shorthand would collide with upstream ``gh``, it
is left unbound so that passing the short flag fails with an unknown-flag error
rather than performing an unintended operation:

* **Long-form only flags**:

  * ``--auto`` on ``pr`` (in ``gh``, ``-a`` is ``--assignee``)
  * ``--auth-mode`` (in ``gh auth status``, ``-a`` is ``--active``)
  * ``--publish`` on ``pr push`` (in ``gh``, ``-p`` is ``--project``)
  * ``--force`` on ``pr checkout`` (in ``gh``, ``-f`` is ``--fill``)
  * ``--cq`` on ``pr`` (in ``gh``, ``-q`` is ``--jq``)
  * ``--message`` on ``pr edit`` and ``pr merge`` (in ``gh``, ``-m`` is
    ``--milestone`` and ``--merge``)

* **Out-of-scope upstream commands and flags**:

  * ``--jq`` / ``-q`` as an output filter
  * ``gh api``, ``-R/--repo``, ``gh release``, and ``gh gist``
  * ``pr merge --squash``, ``--rebase``, and ``--delete-branch`` (Gerrit submits
    changes as patchsets and configures the merge strategy at the repository
    level)

--------------------------
Adding new flags in gh-ish
--------------------------
When adding or proposing a new flag to ``pw_ghish``:

1. **Check official GitHub CLI reference**: Consult ``gh help <command>`` (or
   `cli.github.com/manual <https://cli.github.com/manual/>`_) to see if an
   official flag already exists for the desired functionality. If it exists,
   adopt the same flag name, short option, and value format.
2. **Verify non-intersection**: If introducing a Gerrit, LUCI, or Buganizer
   feature, verify that the proposed flag name and shorthand do not collide
   with current ``gh`` flags on that subcommand.
3. **No silent no-ops**: Never accept a flag without implementing its behavior.
   If a flag cannot be supported, return an explicit error.
