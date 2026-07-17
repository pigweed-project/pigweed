.. _contrib-changelog:

================================
Changelog guide for contributors
================================
This page shows upstream Pigweed maintainers how to update the changelog and
understand how the changelog automation works.

.. _contrib-changelog-quickstart:

----------
Quickstart
----------
.. todo-check: disable

#. Open an agent product that supports skills, e.g. Antigravity.

#. Start the changelog automation by providing a prompt like this:

   .. code-block:: none

      Create a changelog update for March 2026.

   The agent analyzes small batches of commits and progressively drafts
   the content in ``//docs/agents/changelog/resources/data.toml``. If problems
   are detected, the agent must fix those problems before proceeding to the
   next batch of commits. Eventually the agent will output a complete
   reStructuredText draft to ``//docs/sphinx/changelog/<year>/<month>.rst``.
   See :ref:`contrib-changelog-theory` for more context.

#. Review the draft. It probably will not be good enough to publish.
   Here are some common problems to look out for, as well as suggestions
   on how to fix each.

   **Too noisy**. The draft is covering too many unimportant details.

   .. code-block:: none

      Review @docs/sphinx/changelog/2026/03.rst and
      @docs/sphinx/changelog/2026/04.rst to get a better idea of the kinds
      of stories we want to cover in the changelog. Update
      @docs/agents/changelog/resources/data.toml to focus on important
      stories and ignore unimportant details.

   **Not enough code examples**. Ignore this for now. It will be addressed
   in the next step.

   **Inappropriate writing style**. The writing is too verbose, doesn't flow
   well, etc.

   .. code-block:: none

      Review the writing style in @docs/sphinx/changelog/2026/03.rst and
      update @docs/agents/changelog/resources/data.toml to emulate this
      writing style.


   **Story should be ignored**. Just set the ``score`` for the story to
   ``0`` in ``data.toml``.

   You can also add ``TODO`` comments in ``data.toml`` to address ad hoc
   issues and then prompt the agent to fix the TODOs:

   .. code-block:: none

      fix the TODOs in @docs/agents/changelog/resources/data.toml

#. Instruct the agent to doublecheck the code examples.

   .. code-block:: none

      Review the code examples in @docs/agents/changelog/resources/data.toml
      for accuracy. Look at API signatures, existing examples, and unit tests
      to verify. Create a document at ``EXAMPLES.md`` that details the source
      materials that you used for each example.

   Manually review the new ``EXAMPLES.md`` and verify the sources yourself
   until you're confident that the agent has not hallucinated any code
   examples.

#. During code review, tag a relevant owner to review their respective
   content area. E.g. a ``pw_kernel`` owner should review kernel content,
   a Bluetooth owner should review ``pw_bluetooth_sapphire`` content, etc.

#. Before merging, make sure that temporary artifacts like
   ``data.toml`` and ``EXAMPLES.md`` are deleted.

.. todo-check: enable

.. _contrib-changelog-theory:

-------------------
Theory of operation
-------------------
The changelog automation follows an iterative process to transform a large
number of raw git commits into a curated list of "stories" that are meaningful
to Pigweed users.

#. **Commit ingestion**: The agent runs a script that returns a small batch of
   commit data.

#. **Story aggregation**: The agent groups related commits into a single "story"
   with a title, body, one-line highlight, score (representing user-facing
   impact), and code example (when relevant). The agent does all of its work in
   a TOML file, ``//docs/agents/changelog/resources/data.toml``. This TOML file
   is a temporary artifact.

#. **Iterative refinement**: Similar stories are merged, overly broad ones are
   split.

#. **Validation**: When the agent attempts to fetch the next batch of commits,
   the ``next`` script that the agent invokes validates the WIP data and
   refuses to yield the next batch until the agent fixes the issues that the
   script has detected.

#. **Transformation**: The agent invokes the ``end`` script, and this script
   transforms the TOML data into reStructuredText.

All relevant source code can be found in the following directories:

* :cs:`.agents/skills/changelog/`
* :cs:`docs/sphinx/changelog/`
