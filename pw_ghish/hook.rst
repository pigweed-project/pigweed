.. _module-pw_ghish-hook:

===============
Hooks (gh hook)
===============
.. pigweed-module-subpage::
   :name: pw_ghish

``./gh hook`` (also aliased as ``./gh agent``) manages and executes **AI agent
pre-execution tool guards** (``PreToolUse`` / ``beforeShellExecution``) and
**Git repository hooks** (``commit-msg`` and ``pre-push``) directly inside the
compiled ``gh-ish`` Go binary—eliminating external Python or POSIX shell script
dependencies across Linux, macOS, and Windows.

.. code-block:: console

   # Inspect installed user-level agent hooks and repository Git hooks:
   $ ./gh hook status

   # Stage the gh-ish binary and install user-level PreToolUse guards:
   $ ./gh hook install --agent

   # Install only for a specific harness (jetski, claude, cursor, or all):
   $ ./gh hook install --agent=jetski

   # Install the pure-Go Gerrit commit-msg hook and opt-in pre-push guard:
   $ ./gh hook install --git --block-raw-push

   # Remove installed agent or Git pre-push hooks:
   $ ./gh hook uninstall --agent --git

---------------------------------
How user-level installation works
---------------------------------
When you run ``./gh hook install --agent``, ``pw_ghish`` performs two steps:

1. **Stages a branch-independent binary**: Copies the compiled ``gh-ish``
   executable into ``~/.config/pw_ghish/bin/gh-ish`` (or
   ``%USERPROFILE%\.config\pw_ghish\bin\gh-ish.exe`` on Windows). Because the
   hook points to this staged binary outside the working tree, hooks execute in
   a few milliseconds without invoking Bazel and continue to work when
   switching branches or bisecting older commits.
2. **Updates user-level harness configurations**: Merges the ``gh-ish hook
   pre-tool-use`` command entry into your user-level harness settings while
   preserving existing configuration keys:

   * **Antigravity / Jetski** (``--agent=jetski``):
     ``~/.gemini/config/hooks.json`` (``PreToolUse`` matching ``run_command``)
   * **Claude Code** (``--agent=claude``):
     ``~/.claude/settings.json`` (``PreToolUse`` matching ``Bash``)
   * **Cursor** (``--agent=cursor``):
     ``~/.cursor/hooks.json`` (``beforeShellExecution``)

-----------------
Runtime callbacks
-----------------
The following subcommands are invoked automatically by your coding agent harness
or by Git once installed:

gh hook pre-tool-use
====================
Reads a ``PreToolUse`` or ``beforeShellExecution`` JSON payload from ``stdin``,
auto-detects the calling harness schema (**Antigravity/Jetski**, **Claude
Code**, or **Cursor**), and evaluates the proposed shell command before
execution:

.. list-table::
   :header-rows: 1

   * - Intercepted Command Pattern
     - Action & Remediation Returned to Agent
   * - Raw ``git push`` (except ``--help``)
     - **Blocked**: Directs the agent to ``./gh pr create`` (new CL),
       ``./gh pr push`` (new patchset), or ``--stack``.
   * - ``curl``, ``gob-curl``, ``wget``, or ``.gitcookies`` access targeting
       ``*-review.googlesource.com`` or ``cr-buildbucket.appspot.com``
     - **Blocked**: Directs the agent to ``./gh pr view [<id>] --comments``,
       ``./gh pr diff [<id>]``, ``./gh pr checks [<id>]``, or
       ``./gh run view [<id>] --log-failed``.
   * - Manual ``bb add`` or legacy ``search_builds.py``
     - **Blocked**: Directs the agent to ``./gh run rerun [<id>] --failed`` (or
       ``-j <builder>``) or ``./gh pr checks [<id>]``.

Because ``pre-tool-use`` only runs on AI agent tool calls, human developers
running ``git push`` or ``curl`` in an interactive terminal are never affected.

gh hook commit-msg
==================
.. code-block:: console

   $ ./gh hook commit-msg <path-to-commit-msg-file>

Ensures a valid Gerrit ``Change-Id: I<sha1>`` trailer is present in the commit
message file, inserting it before ``Signed-off-by:`` (or at the end of the
message) using Go's standard ``crypto/sha1`` without requiring ``awk`` or
``/dev/urandom``.

gh hook pre-push
================
.. code-block:: console

   $ ./gh hook pre-push

When opted in via ``./gh hook install --block-raw-push`` (which sets ``git
config ghish.blockrawpush true``), blocks direct ``git push`` invocations at
the Git layer unless invoked via ``./gh`` (``GH_ISH_ACTIVE=1``) or explicitly
bypassed for manual recovery via ``GH_ISH_ALLOW_RAW_PUSH=1``.
