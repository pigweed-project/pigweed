.. _module-pw_presubmit-format:

===============
Code formatting
===============
.. pigweed-module-subpage::
   :name: pw_presubmit

The ``pw format`` command provides a unified experience for checking and fixing
code formatting for a variety of languages used in Pigweed. It automatically
detects which formatters to apply based on file extensions and names.

--------
Features
--------
* **Check vs. fix**: Run in check-only mode with ``--check`` to find errors
  without modifying files.
* **Targeted formatting**: Format specific files or directories by passing them
  as arguments.
* **Only format modified files**: By default, only formats the files that have
  been modified in the last change (or since the upstream tracking branch, if
  set). Use ``--full`` to format all files in a repo.
* **Automatic configuration**: Discovers and uses project-specific formatter
  configurations like ``.clang-format``, ``.black.toml``, and ``rustfmt.toml``.
* **Parallel execution**: Speed up formatting with the ``-j`` or ``--jobs``
  flag to run on multiple files in parallel.
* **Unified experience**: Regardless of what files you format, you'll always
  have a consistent CLI experience.

Language support:

.. list-table::

   * - **File type**
     - **Flag**
     - **Enabled by default**
   * - Bazel build files
     - ``--@pigweed//pw_presubmit/py:enable_bazel_formatter``
     - ✅
   * - C/C++
     - ``--@pigweed//pw_presubmit/py:enable_cpp_formatter``
     - ✅
   * - CMake
     - ``--@pigweed//pw_presubmit/py:enable_cmake_formatter``
     - ✅
   * - CSS
     - ``--@pigweed//pw_presubmit/py:enable_css_formatter``
     - ✅
   * - GN build files
     - ``--@pigweed//pw_presubmit/py:enable_gn_formatter``
     - ❌
   * - Go
     - ``--@pigweed//pw_presubmit/py:enable_go_formatter``
     - ❌
   * - Java
     - ``--@pigweed//pw_presubmit/py:enable_java_formatter``
     - ✅
   * - JavaScript
     - ``--@pigweed//pw_presubmit/py:enable_javascript_formatter``
     - ❌
   * - JSON
     - ``--@pigweed//pw_presubmit/py:enable_json_formatter``
     - ✅
   * - Markdown
     - ``--@pigweed//pw_presubmit/py:enable_markdown_formatter``
     - ✅
   * - OWNERS files
     - ``--@pigweed//pw_presubmit/py:enable_owners_formatter``
     - ✅
   * - Protobuf
     - ``--@pigweed//pw_presubmit/py:enable_protobuf_formatter``
     - ✅
   * - Python (using ``black``)
     - ``--@pigweed//pw_presubmit/py:enable_python_formatter``
     - ✅
   * - reStructuredText
     - ``--@pigweed//pw_presubmit/py:enable_rst_formatter``
     - ✅
   * - Rust
     - ``--@pigweed//pw_presubmit/py:enable_rust_formatter``
     - ❌
   * - Starlark
     - ``--@pigweed//pw_presubmit/py:enable_starlark_formatter``
     - ✅
   * - TypeScript
     - ``--@pigweed//pw_presubmit/py:enable_typescript_formatter``
     - ❌

-----------
Get started
-----------
The formatter is usually invoked via the ``pw`` entry point at the root of your
project.

Usage examples
==============

.. code-block:: console

   # Fix formatting for all files.
   $ ./pw format

   # Check formatting for all files in the repository without making
   # modifications.
   $ ./pw format --check

   # Format all files tracked by Git.
   $ ./pw format --all

   # Format specific files or directories.
   $ ./pw format pw_foo/ public/pw_foo/foo.h

Project setup
=============
Pigweed's default formatter is designed to work out-of-the-box for most Bazel
projects. You can try it with no additional setup setups with the following
command:

.. code-block:: console

   $ bazel run @pigweed//pw_presubmit/py:format

Add a shortcut
--------------
To make running the formatter easier, add the ``format`` to a Workflow
shortcut. See the Workflows
:ref:`module-pw_build-workflows-launcher-getting-started` guide for more
info.

Enable additional formatters
----------------------------
Some formatters that may not be interesting to most projects are disabled by
default, and may be enabled by adding flags to your project's ``.bazelrc``
file:

.. code-block::

   # Enable GN build file formatting.
   common --@pigweed//pw_presubmit/py:enable_gn_formatter=True

.. note::

   Enabling additional formatters will cause more tooling to be downloaded.

.. _module-pw_presubmit-format-api:

-------------
API reference
-------------

.. admonition:: Note
   :class: warning

   :bug:`326309165`: While the ``pw format`` command interface is very stable,
   the ``pw_presubmit.format`` library is a work-in-progress effort to detach
   the implementation of ``pw format`` from the :ref:`module-pw_presubmit`
   module. Not all formatters are migrated, and the library API is unstable.
   After some of the core pieces land, this library will be moved to
   ``pw_code_format``.

Core
====
.. automodule:: pw_presubmit.format.core
   :members:
   :special-members: DiffCallback
   :noindex:

Formatters
==========
.. autoclass:: pw_presubmit.format.bazel.BuildifierFormatter
   :members:
   :noindex:

.. autoclass:: pw_presubmit.format.cpp.ClangFormatFormatter
   :members:
   :noindex:

.. autoclass:: pw_presubmit.format.gn.GnFormatter
   :members:
   :noindex:

.. autoclass:: pw_presubmit.format.python.BlackFormatter
   :members:
   :noindex:

.. autoclass:: pw_presubmit.format.rust.RustfmtFormatter
   :members:
   :noindex:

Presubmit step
==============
.. autoclass:: pw_presubmit.format.step.CodeFormatting
   :members:
   :noindex:
