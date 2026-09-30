.. _module-pw_ghish-agent-integration:

=================
Agent integration
=================
.. pigweed-module-subpage::
   :name: pw_ghish

``pw_ghish`` (``./gh``) is designed to work across AI coding assistants—such as
**Antigravity (Jetski)**, **Claude Code**, **OpenAI Codex**, **Cursor**, and
**OpenCode**—without locking developers into a single agent harness or operating
system.

* **Agent integration (this page)**: How ``pw_ghish`` exposes rules and skills
  to AI coding assistants across harnesses.
* :ref:`module-pw_ghish-project-integration`: For repository maintainers
  adopting ``gh-ish`` and configuring ``ProjectProfile`` in a Gerrit/LUCI repo.
* :ref:`module-pw_ghish-ai-workflows`: Day-to-day pair-programming workflows
  (private draft steering, staged review replies, and automated CI triage).

-------------------------------------
The three levers of agent integration
-------------------------------------
``pw_ghish`` structures its repository context so all coding harnesses share a
single source of truth without duplicating prompt files or bloating unrelated
tasks:

1. **The ./gh CLI (//pw_ghish:gh-ish) — Zero-shot ergonomics**:
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

.. note::

   **Upcoming**: Opt-in pre-execution tool hooks and Git hooks (via ``./gh hook``)
   are in development to deterministically block raw ``git push``, direct REST
   ``curl`` calls, and manual ``bb add`` invocations before execution.
