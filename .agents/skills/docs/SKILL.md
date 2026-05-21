---
name: docs
description: >-
  Use for ALL documentation-related workflows: rst style guide,
  changelog updates, C/C++ API reference (Doxygen)
---

# Docs

Pigweed has many docs automation workflows. Rather than list them all here,
this skill helps agents decide which workflows are relevant and then routes
them to the full instructions.

## Usage

1. Summarize the user's main goal in a few sentences.
2. If the user's main goal is not docs-related and there is still
   work to be done, STOP HERE.

   > [!WARNING]
   >
   > The docs workflows may reduce your ability to solve the user's
   > main goal because they create high risk of context pollution.

   Examples of WIP main goals unrelated to docs:

   * The root cause of a bug has not yet been found.
   * The user is exploring a new API and it's not yet clear that they're happy
     with the design.
   * Software architecture or system design research.
   * Code implementations or refactors and tests are still failing.

3. Run ALL workflows that are relevant to the user's main goal.

## Workflows

Each of the sections below represents a workflow. Summary:

* rst: Align with Pigweed's reStructuredText (reST) style guide.
* changelog: Update the Pigweed changelog.
* doxygen: Create C/C++ API reference content.

All workflows follow this pattern:

* `Triggers`: Hints about when to run the workflow. Values in double quotes
  represent example prompts.
* `Guards`: Warnings about when to NOT run the workflow.
* `Path`: The path to the full instructions.

### rst

Triggers:

* `*.rst` files are in context.
* "format the rst"
* "rest formatting"
* The user is creating a significant amount of new documentation
  and has asked for help getting it ready to publish.

Guards:

* Do not waste time on reST formatting for first draft content that
  is likely to change a lot.
* When the main goal requires only minor docs updates, do not touch
  reST that's unrelated to the main goal.

Path: `//docs/agents/rst/AGENTS.md`

### changelog

Triggers:

* "create a changelog update for mar 2026"

Guards:

* "summarize the rust 1.95 changelog"
* "what is the changelog process for the new API we just created?"
* "analyze this codebase in relation to the c++ 2026 changelog"

Path: `//docs/agents/changelog/AGENTS.md`

### doxygen

Triggers:

* A target in `//docs/doxygen` has failed to build
* The user is attempting to create new C/C++ API reference content

Guards:

* "search the doxygen docs"

Path: `//docs/agents/doxygen/AGENTS.md`
