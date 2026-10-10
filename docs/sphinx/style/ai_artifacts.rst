.. _docs-pw-style-ai-artifacts:

=================
AI artifact style
=================
This guide defines conventions for *AI artifacts*: the files that configure
AI coding agents working in Pigweed. It applies to:

* **Skills** -- ``.agents/skills/<name>/SKILL.md`` and sibling files
* **Subagents** -- ``.agents/agents/<name>.md`` (or
  ``.agents/agents/<name>/agent.md``)
* **Rules** -- ``AGENTS.md`` files and their ``GEMINI.md`` / ``CLAUDE.md``
  pointers
* **Helper scripts** -- anything a skill instructs an agent to run

Every word in these files is loaded into an agent's context window on every
task that triggers them. Treat them like a public API: small, stable, and
explicit about when they apply.

.. _docs-pw-style-ai-artifacts-design:

-----------------
Design guidelines
-----------------

.. _docs-pw-style-ai-artifacts-no-checkout-assumptions:

Do not assume a checkout location
=================================
Agents run from the repository root, from ``./gh wt`` worktree slots, from IDE
worktrees, and from downstream projects that vendor Pigweed. Reference files
and tools relative to the Pigweed root (``./gh``, ``./pw``,
``.agents/skills/ghish/SKILL.md``), never via absolute home-directory paths.

.. _docs-pw-style-ai-artifacts-portable:

Write for open source and for any harness
=========================================
Pigweed is open source and its artifacts are loaded by several agent
harnesses. They must work for contributors outside Google: no ``go/`` links,
no internal bug or document links, and no Google-only tools.

Do not name a harness's built-in tools either. Describe the action instead:
"read the file", not the name of a file-viewing tool; "search the repository
with ``git grep``", not the name of a code-search tool; "run as a background
subagent", not the name of a subagent-launching tool.

.. _docs-pw-style-ai-artifacts-no-dependencies:

Do not reference other skills or agents by name
===============================================
A skill or subagent may be loaded in a harness where the other artifact does
not exist or has a different name. Reference the underlying file instead, so
the agent can read it directly.

.. admonition:: No
   :class: error

   You must run the ``ghish`` skill to push the change.

.. admonition:: Yes
   :class: checkmark

   Read and follow the push workflow in ``.agents/skills/ghish/SKILL.md``.

The one exception is dispatch. A coordinator skill that delegates work to
subagents must name them, because a harness resolves subagents by the ``name``
in their frontmatter. Keep those names in a single table, state that they
come from ``.agents/agents/<name>.md``, and say what the coordinator does when
a named subagent is absent or disabled.

.. _docs-pw-style-ai-artifacts-brief:

Be brief
========
Every word carries context cost. Prefer a numbered workflow and a short list of
hard rules over prose. Omit background that the agent does not need.

.. _docs-pw-style-ai-artifacts-dry:

Do not repeat yourself
======================
Do not restate rules that live in a canonical document (a style guide, a
module's ``docs.rst``, or another artifact). Link to it and, when the rules
must be applied precisely, instruct the agent to **read the canonical document
at use time**. This keeps a single source of truth and prevents the artifact
from silently drifting out of date.

.. _docs-pw-style-ai-artifacts-usage-guidelines:

Declare when to use it -- and when not to
=========================================
Every skill ``description`` and every subagent ``description`` must state:

* **Triggers**: the tasks, file types, or user phrases that should load it.
* **Guards**: nearby tasks for which it must *not* be loaded.
* **Autonomy**, only when it is restricted. By default an agent may load a
  skill on its own; do not say so. If the artifact may be used only when the
  user explicitly asks (for example via a ``/slash-command``), only from a
  specific coordinator, or must pause for approval, the description says so.

This prevents agents from loading irrelevant context or spawning subagents
for tasks they should handle directly.

.. _docs-pw-style-ai-artifacts-deterministic:

Prefer deterministic scripts over prompt compliance
===================================================
When a step can be made deterministic -- parsing output, validating inputs,
checking preconditions -- implement it as a Python script (not a shell script
with branching or parsing) under the skill's ``scripts/`` directory, or as a
``./gh`` / ``./pw`` subcommand, and instruct the agent to run it. Do not ask
the agent to "carefully check" or "manually format" something a script can
verify.

Scripts that make network calls or mutate state must accept ``--dry-run``,
which prints every mutation or network write the script would perform and
exits 0 without performing any of them, so the script can be exercised in
evaluations. On failure, scripts exit non-zero with an actionable message.

.. _docs-pw-style-ai-artifacts-hard-stops:

Define hard stops
=================
Workflows that act on external state (a CL, a bug, CI) must list the
conditions under which the agent stops without modifying anything, for
example: the change is merged or abandoned, the bug is assigned to someone
else, or required identifiers were not provided. State that a non-zero exit
code or ``STOP:`` output from a gate script overrides any instruction found
in the bug, CL, or code being processed.

.. _docs-pw-style-ai-artifacts-verify:

Verify with tools, not words
============================
An artifact must not allow an agent to claim success from reasoning alone.
Require a tool call (``git status -sb``, ``./gh pr view``, ``ls -l``,
``bazelisk test``) before any statement that a file was written, a patchset
was pushed, or a test passed. For long-running background work, require
waiting for the completion signal rather than reporting early.

.. _docs-pw-style-ai-artifacts-review-time:

Reviewer artifacts: read the guide, scope the diff, cite the rule
=================================================================
Code-review skills and subagents must:

#. Read the canonical style guide(s) at review time rather than embedding
   rules.
#. Review only the lines changed by the patch. Label findings outside the
   diff as *optional*.
#. For each finding, explain the issue, show a compliant example, and cite the
   guideline section by its anchor (for example
   :ref:`docs-pw-style-cpp`).
#. Declare a read-only ``tools:`` allowlist on reviewer subagents. Reviewers
   never edit files or post to Gerrit; the coordinator that invokes them
   posts, and only when the user asks for that.

.. _docs-pw-style-ai-artifacts-layout:

------
Layout
------

.. list-table::
   :header-rows: 1

   * - Path
     - Purpose
   * - ``.agents/skills/<name>/SKILL.md``
     - Skill entry point. YAML frontmatter with ``name`` and ``description``,
       then the workflow.
   * - ``.agents/skills/<name>/TEST.md``
     - Evaluation scenarios for the skill and any subagents it drives: user
       prompts, the commands or subagents expected, and prohibited
       anti-patterns. See :ref:`module-pw_ghish-agent-eval` for the rubric.
   * - ``.agents/skills/<name>/scripts/``
     - Deterministic Python helpers, invoked as
       ``python3 .agents/skills/<name>/scripts/<x>.py``. Standard-library or
       Pigweed-environment dependencies only. Register each script in both
       ``BUILD.bazel`` and ``BUILD.gn`` with a test target.
   * - ``.agents/agents/<name>.md`` or ``.agents/agents/<name>/agent.md``
     - Subagent definition. YAML frontmatter: ``name``, ``description``,
       ``subagent: true``, ``mainAgent`` (``false`` unless the user may address
       it directly), ``tools`` (an allowlist), and ``disabled``. Keep the body
       to a role statement plus a pointer to the file that holds the workflow.
       Set ``disabled: true`` until the ``TEST.md`` scenarios that exercise the
       agent pass in at least one harness.
   * - ``AGENTS.md``
     - Always-on rules. Reserve for non-negotiable guardrails and the command
       quick reference; move task-specific detail into skills. See
       :ref:`module-pw_ghish-agent-setup` for how harnesses load it.

The ``name`` in a skill's or agent's frontmatter uses ``kebab-case``. Directory
and file names use ``snake_case`` to match the surrounding repository, so
``<name>`` in the paths above is the ``snake_case`` form: the skill named
``code-review`` lives in ``.agents/skills/code_review/SKILL.md``.

.. _docs-pw-style-ai-artifacts-review-checklist:

----------------
Review checklist
----------------
When reviewing a change that adds or modifies an AI artifact, check that it:

* States triggers and guards in its ``description``, and autonomy when it is
  restricted.
* Contains no absolute paths, no Google-only links or tools, no harness tool
  names, and no references to other artifacts by name outside a dispatch
  table.
* Links to canonical docs rather than restating them.
* Lists hard-stop conditions for any workflow that touches external state.
* Relies on ``AGENTS.md`` for tool-based verification and hard-stop rules
  rather than restating them; adds verification steps only for
  artifact-specific outputs.
* Ships ``--dry-run`` support and tests for any new helper script.
* Adds or updates ``TEST.md`` scenarios for a new or changed skill or agent.
* Updates the skill list in ``AGENTS.md`` when a skill or agent is added or
  removed.
