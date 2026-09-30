.. _module-pw_ghish-agent-setup:
.. _module-pw_ghish-agent-integration:

===========
Agent setup
===========
.. pigweed-module-subpage::
   :name: pw_ghish

``pw_ghish`` (``./gh``) is designed to work across AI coding assistants—such as
**Antigravity (Jetski)**, **Claude Code**, **OpenAI Codex**, **Cursor**, and
**OpenCode**—without locking developers into a single agent harness or operating
system.

* **Agent setup (this page)**: The levers ``pw_ghish`` provides for AI
  coding assistants and how to enable optional tool-call enforcement.
* :ref:`module-pw_ghish-hook`: Reference for the ``./gh hook`` subcommand
  (``install``, ``uninstall``, ``status``, ``pre-tool-use``, ``commit-msg``,
  and ``pre-push``).
* :ref:`module-pw_ghish-project-setup`: For repository maintainers
  adopting ``gh-ish`` and configuring ``ProjectProfile`` in a Gerrit/LUCI repo.
* :ref:`module-pw_ghish-agent-workflows`: Day-to-day pair-programming workflows
  (private draft steering, staged review replies, and automated CI triage).

------------------------------------
The four levers of agent integration
------------------------------------
``pw_ghish`` layers its agent support from default CLI behavior to opt-in
execution guards:

1. **The ./gh CLI (//pw_ghish:gh-ish) — GitHub CLI syntax**:
   Exposes Gerrit (``./gh pr``), LUCI Buildbucket (``./gh pr checks``,
   ``./gh run``), Buganizer (``./gh issue``), and worktrees (``./gh wt``) using
   GitHub CLI (``gh``) syntax that models already know from pre-training, paired
   with structured 4-pillar error diagnostics for LLM self-correction.
2. **Always-on repository rules (AGENTS.md) — Baseline guardrails**:
   Canonical instructions live in ``AGENTS.md`` (with 1-line ``@AGENTS.md``
   pointers in ``CLAUDE.md`` and ``GEMINI.md`` to prevent duplicate prompt
   injection). This gives every harness the core ``./gh`` command reference and
   non-negotiable rules (never use raw ``git push``, never craft manual
   ``curl`` calls, never poll CI in a loop) without bloating unrelated tasks.
3. **On-demand skills (.agents/skills/) — Deep task context**:
   Detailed flag semantics, exit-code contracts (``0`` pass, ``8`` running,
   ``1`` fail), trailer-preservation rules, and multi-step workflows live in
   ``.agents/skills/ghish/SKILL.md`` and ``.agents/skills/worktree/SKILL.md``,
   loaded only when a task involves Gerrit, CI, Buganizer, or worktrees.
4. **Opt-in execution guards (./gh hook) — Deterministic enforcement**:
   Because models under long context windows can still regress to pre-trained
   ``git push origin HEAD:refs/for/main`` or ``curl`` habits, ``./gh hook``
   provides optional user-level ``PreToolUse`` and Git ``pre-push`` guards
   implemented directly in the compiled Go binary (disabled by default so human
   terminal workflows are never disrupted).

---------------------------------
Installing the optional tool hook
---------------------------------
To stage the ``gh-ish`` binary in your user config directory and register the
pre-execution guard across your local coding agents (**Antigravity/Jetski**,
**Claude Code**, and **Cursor**):

.. code-block:: console

   # Install for all supported agents (or pass --agent=jetski|claude|cursor):
   $ ./gh hook install --agent

   # Optionally also install the Git commit-msg and raw git push guards:
   $ ./gh hook install --git --block-raw-push

   # Verify installation status:
   $ ./gh hook status

For full command details or custom harness wiring, see :ref:`module-pw_ghish-hook`
and your harness's hook documentation:

* **Claude Code**: `Claude Code Hooks documentation
  <https://docs.anthropic.com/en/docs/claude-code/hooks>`_
* **Cursor**: `Cursor Hooks documentation
  <https://docs.cursor.com/context/hooks>`_
* **OpenCode**: `OpenCode Plugins documentation
  <https://opencode.ai/docs/plugins>`_
* **OpenAI Codex**: `Codex CLI documentation
  <https://github.com/openai/codex>`_
