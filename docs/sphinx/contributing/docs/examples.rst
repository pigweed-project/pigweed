.. _contributing-docs-examples:

===================================================
Guidelines for buildable and testable code examples
===================================================
.. inclusive-language: disable
.. _code-block: https://www.sphinx-doc.org/en/master/usage/restructuredtext/directives.html#directive-code-block
.. inclusive-language: enable

To date, most ``pigweed.dev`` code examples have been embedded within our
reStructuredText files via Sphinx's `code-block`_ directive. The problem with
this approach is that the code blocks aren't built or tested, so there's little
guarantee that the code actually works. This guide shows
:ref:`docs-glossary-upstream` maintainers how to migrate these code examples
into a buildable and testable format. The guide is focused on C++ but the
general pattern is theoretically applicable to any language.

.. note::

   Code examples that aren't built or tested are sometimes called "dead"
   examples. Code examples that are built and tested are sometimes called
   "living" examples.

.. _contributing-docs-examples-philosophy:

----------
Philosophy
----------
Our goal is to make all code examples on ``pigweed.dev``:

* Buildable. The examples should be in the critical path of the "build
  everything" command (``bazelisk build //...``) so that upstream Pigweed
  contributors receive immediate feedback that their code changes are breaking
  an example that appears on ``pigweed.dev``.
* Testable. Each example should have one or more unit tests that verify that
  the example actually does what it claims to do. These tests should likewise
  be in the critical path of the "test everything" command
  (``bazelisk test //...``).
* Minimal. Each example should clearly and simply demonstrate one use case.
  ``pigweed.dev`` readers should not need to weed through a lot of unrelated,
  complex code.
* Complete. ``pigweed.dev`` readers should be able to copy-and-paste the code
  example and have it just work, with little or zero modification needed.

.. _contributing-docs-examples-workflow:

--------
Workflow
--------
The exact mechanics of migrating a code example to be buildable and testable
are covered in :ref:`contributing-docs-examples-quickstart-a` (recommended) and
:ref:`contributing-docs-examples-quickstart-b`. Regardless of whether you use Option
A or Option B, we recommend that you always use the following iterative
workflow. This workflow minimizes risk and makes debugging easier.

#. **Make sure that the code example is intended to be compiled.**
   Some examples are pseudo-code. When in doubt, check with the module's
   owners.

#. **Create a minimal test.** Add a single, empty test. Run
   ``bazelisk test //...`` and ensure that all tests pass.

#. **Migrate one code example.** Find a code example in one of the module's
   reStructuredText files. Copy-paste the code example into the minimal test.
   Iterate until the test compiles and passes.

#. **Ensure that the test fails.** Insert a trivial error into the test.
   Run ``bazelisk test //...`` and ensure that the test now fails.

#. **Fix the test.** Get the test back to a passing state. Run
   ``bazelisk test //...`` again and ensure that all tests pass again.

#. **Update the documentation.** Remove the unbuilt and untested code
   example in a ``code-block`` from the reStructuredText file and replace it
   with a ``literalinclude`` that points to the new code example that is
   built and tested.

#. **Verify the documentation build.** Run ``bazelisk build //docs`` and
   ensure that the docs build succeeds.

#. **Fix formatting errors**. Run ``bazelisk run //:format`` to ensure that
   all formatting errors are fixed.

#. **Visually inspect the result.** Compare the old generated HTML with the
   new HTML and verify that the original intent of the documentation is
   preserved.

#. **Repeat.** Repeat the previous steps for another code example.

.. _contributing-docs-examples-quickstart:

----------
Quickstart
----------

Choose either :ref:`contributing-docs-examples-quickstart-a` (recommended) or
:ref:`contributing-docs-examples-quickstart-b`. They're mutually exclusive workflows.

.. _contributing-docs-examples-quickstart-a:

Option A: Separate file for each example
========================================
Placing each example into its own file with its own build target is the
recommended approach because it provides the strongest guarantee that the
example completely compiles and passes tests on its own, without interference
from other examples. However, if this approach feels too toilsome, you're
welcome to use the more lightweight :ref:`contributing-docs-examples-quickstart-b`
instead.

.. _contributing-docs-examples-quickstart-a-dir:

Create an examples directory
----------------------------
Create a directory named ``examples`` for your Pigweed module,
e.g. ``//pw_string/examples/``.

Having a dedicated directory makes it easy to run the unit tests
for the examples with a single wildcard command, like this:

.. code-block:: console

   bazelisk build //pw_string/examples/...

.. _contributing-docs-examples-quickstart-a-file:

Create the code example
-----------------------
#. Create a file for your code example. The filename should end with
   ``_test.cc``, e.g. ``build_string_in_buffer_test.cc``. The first part of
   the filename (``build_string_in_buffer``) should describe the use case.

   .. literalinclude:: ../../pw_string/examples/build_string_in_buffer_test.cc
      :language: cpp
      :start-after: // DOCSTAG: [contributing-docs-examples]
      :end-before: // DOCSTAG: [contributing-docs-examples]

.. _contributing-docs-examples-quickstart-a-key-points:

Key points
~~~~~~~~~~
Guidelines for your code example file:

* Only the code between the  ``DOCSTAG`` comments will be pulled
  into the docs. See :ref:`module-pw_string` to view how the file
  above is actually rendered in the docs.

* Headers that the example code relies on, like ``pw_log/log.h`` and
  ``pw_string/string_builder.h``, should be shown in the user-facing code
  example.

* Wrap the example code in the ``examples`` namespace.

* The primary example code is usually wrapped in a function like
  ``BuildStringInBuffer()``. This makes the example easier to unit test.

* Each file should only contain one example. The main reason for this
  is to ensure that each example actually compiles on its own, without
  interference from other examples.

* If you need another function to demonstrate usage of the primary
  example code, use ``void main()`` for the signature of this
  secondary function.

* Create one or more unit tests for the primary example code.
  Wrap the unit test in an anonymous namespace. All code examples
  across all Pigweed modules should use the ``ExampleTests`` test suite.

* Follow the usual unit testing best practice of making sure that the
  test initially fails.

.. _contributing-docs-examples-quickstart-a-targets:

Create build targets
--------------------
Create build targets for upstream Pigweed's Bazel, GN, and CMake build systems.

.. _contributing-docs-examples-quickstart-a-targets-bazel:

Bazel
~~~~~
#. Create a ``BUILD.bazel`` file in your ``examples`` directory.

   .. literalinclude:: ../../pw_string/examples/BUILD.bazel
      :language: starlark
      :start-after: # DOCSTAG: [contributing-docs-examples]
      :end-before: # DOCSTAG: [contributing-docs-examples]

   The ``sphinx_docs_library`` rule is how you pull the code example into the
   docs build. There is no equivalent of this in the GN or CMake files
   because the docs are only built with Bazel.

.. _contributing-docs-examples-quickstart-a-targets-gn:

GN
~~
#. Create a ``BUILD.gn`` file in your ``examples`` directory.

   .. literalinclude:: ../../pw_string/examples/BUILD.gn
      :language: starlark
      :start-after: # DOCSTAG: [contributing-docs-examples]
      :end-before: # DOCSTAG: [contributing-docs-examples]

#. Update the top-level ``BUILD.gn`` file for your module
   (e.g. ``//pw_string/BUILD.gn``) so that the new code example
   unit tests are run as part of the module's default unit test
   suite.

   .. literalinclude:: ../../pw_string/BUILD.gn
      :language: starlark
      :start-after: # DOCSTAG: [contributing-docs-examples]
      :end-before: # DOCSTAG: [contributing-docs-examples]

   Notice how ``$dir_pw_string/examples:tests`` is included in
   the list of tests.

.. _contributing-docs-examples-quickstart-a-targets-cmake:

CMake
~~~~~
#. Create a ``CMakeLists.txt`` file in your ``examples`` directory.

   .. literalinclude:: ../../pw_string/examples/CMakeLists.txt
      :language: text
      :start-after: # DOCSTAG: [contributing-docs-examples]
      :end-before: # DOCSTAG: [contributing-docs-examples]

.. _contributing-docs-examples-quickstart-a-include:

Pull the example into a doc
---------------------------
Code examples can be pulled into Sphinx documentation (reStructuredText),
Doxygen C/C++ API references, or both.

.. _contributing-docs-examples-quickstart-a-include-sphinx:

Sphinx (reStructuredText)
~~~~~~~~~~~~~~~~~~~~~~~~~
#. In your module's top-level ``BUILD.bazel`` file (e.g.
   ``//pw_string/BUILD.bazel``), update the ``sphinx_docs_library`` target:

   .. literalinclude:: ../../pw_string/BUILD.bazel
      :language: starlark
      :start-after: # DOCSTAG: [contributing-docs-examples]
      :end-before: # DOCSTAG: [contributing-docs-examples]

   Notice how ``//pw_string/examples`` is included in the ``deps``
   of the ``docs`` target. This is how you make the example source code
   available to Sphinx when it builds the docs.

#. Use a ``literalinclude`` directive in your reStructuredText to pull
   the code example into your doc:

   .. literalinclude:: ../../pw_string/docs.rst
      :language: rst
      :dedent:
      :start-after: .. DOCSTAG: [contributing-docs-examples]
      :end-before: .. DOCSTAG: [contributing-docs-examples]

.. _contributing-docs-examples-quickstart-a-include-doxygen:

Doxygen (C/C++ API reference)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Pigweed supports importing testable code snippets from unit tests into
function, method, or class doc comments via a custom ``@example`` alias that
wraps Doxygen's native ``@snippet`` command. The exact same
``// DOCSTAG: [tag-name]`` delimiters used for Sphinx are recognized by
Doxygen as snippet boundaries. Doxygen identifies the snippet by matching the
identifier inside the square brackets (``[tag-name]``) and ignores preceding
comment text such as ``DOCSTAG:``, so ``DOCSTAG`` does not need to be (and
should not be) included in the snippet ID.

#. In your existing unit test file (e.g. ``pw_base64/base64_test.cc``), tag
   the relevant portion of the test with ``// DOCSTAG: [tag-name]``:

   .. code-block:: cpp

      TEST(Base64, ExampleFromRfc3548Section7) {
        // DOCSTAG: [pw_base64-encode]
        constexpr uint8_t input[] = {0x14, 0xfb, 0x9c, 0x03, 0xd9, 0x7e};
        char output[EncodedSize(sizeof(input)) + 1] = {};

        Encode(as_bytes(span(input)), output);
        // DOCSTAG: [pw_base64-encode]
        EXPECT_STREQ("FPucA9l+", output);
        // ...
      }

#. In your module's top-level ``BUILD.bazel`` file (e.g.
   ``//pw_base64/BUILD.bazel``), ensure the test file is explicitly included in
   the ``doxygen`` filegroup:

   .. code-block:: starlark

      filegroup(
          name = "doxygen",
          srcs = [
              "base64_test.cc",
              "public/pw_base64/base64.h",
              # ...
          ],
      )

#. In your C/C++ header file (e.g. ``public/pw_base64/base64.h``),
   reference the snippet in the Doxygen comment for a function or class using
   Pigweed's custom ``@example{<path>,<tag-name>}`` alias:

   .. code-block:: cpp

      /// Encodes the provided data in Base64 and writes the result to the buffer.
      ///
      /// @example{pw_base64/base64_test.cc,pw_base64-encode}
      inline void Encode(span<const std::byte> binary, char* output);

   The first argument is the repository-relative path to the test file
   (e.g. ``pw_base64/base64_test.cc``), and the second argument is the snippet
   tag name inside the brackets (e.g. ``pw_base64-encode``).

   When Doxygen runs, this alias automatically:

   * Creates a structured ``Example`` section (similar to ``@pre`` or
     ``@returns``).
   * Extracts the lines between the opening and closing
     ``// DOCSTAG: [pw_base64-encode]`` comments, strips indentation, applies
     syntax highlighting, and autolinks referenced C++ symbols to their API
     reference entries.
   * Appends a ``Source: <path>`` link pointing directly to the test file in
     Pigweed Code Search (with a ``?q=<tag-name>`` query parameter to highlight
     the snippet tag).

You're done!

.. _contributing-docs-examples-quickstart-b:

Option B: Single file for all examples
======================================
In the Option B approach you place all of your examples and unit tests in a
single file and build target. The main drawback with this approach is that it's
easy to accidentally make your code example incomplete. E.g. you forget to
include a header in an example, because an earlier example already included
that same header. The build target is also harder to read, because all of the
dependencies for all of the code examples are mixed into a single target.

However, Option B is still a major improvement over the status quo
of not building or testing code examples, so you're welcome to use Option B if
:ref:`contributing-docs-examples-quickstart-a` feels too toilsome.

Create a file for the code examples
-----------------------------------
#. Create an ``examples.cc`` file in the root directory of your module. All of
   your code examples and unit tests will go in this single file.

   .. literalinclude:: ../../pw_assert/examples.cc
      :language: cpp
      :start-after: // DOCSTAG: [contributing-docs-examples]
      :end-before: // DOCSTAG: [contributing-docs-examples]

#. Make sure that your code examples and unit tests follow all of the
   guidelines that are described in
   :ref:`contributing-docs-examples-quickstart-a-key-points`.

Create build targets
--------------------
#. In your module's top-level build files (e.g. ``//pw_assert/BUILD.bazel``,
   ``//pw_assert/BUILD.gn``, and ``//pw_assert/CMakeLists.txt``) create build
   targets for your new ``examples.cc`` file.

   Here are examples for each build system:

   Bazel:

   .. literalinclude:: ../../pw_assert/BUILD.bazel
      :language: starlark
      :start-after: # DOCSTAG: [contributing-docs-examples]
      :end-before: # DOCSTAG: [contributing-docs-examples]

   GN:

   .. literalinclude:: ../../pw_assert/BUILD.gn
      :language: py
      :start-after: # DOCSTAG: [contributing-docs-examples]
      :end-before: # DOCSTAG: [contributing-docs-examples]

   CMake:

   .. literalinclude:: ../../pw_assert/CMakeLists.txt
      :language: text
      :start-after: # DOCSTAG: [contributing-docs-examples]
      :end-before: # DOCSTAG: [contributing-docs-examples]

Pull the example into a doc
---------------------------
#. Use a ``literalinclude`` directive in your reStructuredText to pull
   the code example into your doc:

   .. literalinclude:: ../../pw_assert/docs.rst
      :language: rst
      :dedent:
      :start-after: .. DOCSTAG: [contributing-docs-examples]
      :end-before: .. DOCSTAG: [contributing-docs-examples]

You're done!

--------------------------
Instructions for AI agents
--------------------------
This section provides instructions for AI agents like Gemini CLI.
AI agents MUST follow this process:

#. You MUST follow the workflow described in
   :ref:`contributing-docs-examples-workflow`.

#. You MUST use :ref:`contributing-docs-examples-quickstart-a`.
   You MUST NOT use :ref:`contributing-docs-examples-quickstart-b`.

#. Inspect ``//pw_string/docs.rst`` and the files in the
   ``//pw_string/examples`` directory to see a working demonstration
   of buildable and testable code examples.

#. You MUST explicitly list all files in ``BUILD.bazel`` files and MUST NOT
   use ``glob()``.
