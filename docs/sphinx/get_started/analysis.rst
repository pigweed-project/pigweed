.. _docs-automated-analysis:

===========================
Static and runtime analysis
===========================
Automated static analysis and runtime sanitizers help ensure the correctness of
your project's code. Using these tools has long been a best practice in
developing software running in data centers, desktops, and mobile devices.
Pigweed makes these tools easy to integrate into embedded development, too!

If you're integrating Pigweed into an existing project using Bazel, you can get
started on static and runtime analysis as soon as you've completed
:ref:`docs-bazel-integration-add-pigweed-as-a-dependency`.

-------------
The Checklist
-------------
These are the tools we recommend using when developing firmware with Pigweed.

.. list-table::
   :widths: 20 45 3
   :header-rows: 1

   * - Tool
     - Summary
     - Check?
   * - :ref:`docs-automatic-analysis-clang-tidy`
     - C++ linter and static analyzer
     - |checkbox|
   * - :ref:`docs-automatic-analysis-pylint`
     - Python linter
     - |checkbox|
   * - :ref:`docs-automatic-analysis-mypy`
     - Python type checker
     - |checkbox|
   * - :ref:`docs-automatic-analysis-pyrefly`
     - Fast Python type checker
     - |checkbox|
   * - :ref:`docs-automatic-analysis-sanitizers`
     - C++ runtime checks
     - |checkbox|
   * -  :ref:`Fuzzing <module-pw_fuzzer>`
     - Finds bugs on random code paths
     - |checkbox|

The rest of this document explains why you should use each of these tools, and
how to do so.

.. note::

   We don't discuss CI integration in this guide, but it's absolutely
   essential! If you're using GitHub Actions, see :ref:`docs-github-actions`
   for instructions.

---------------
Static analysis
---------------
Static analysis attempts to find correctness or performance issues in software
without executing it.

.. _docs-automatic-analysis-clang-tidy:

clang-tidy
==========
`clang-tidy`_ is a C++ "linter" and static analysis tool. It identifies
bug-prone patterns (e.g., use after move), non-idiomatic usage (e.g., creating
``std::unique_ptr`` with ``new`` rather than ``std::make_unique``), and
performance issues (e.g., unnecessary copies of loop variables).

While powerful, clang-tidy defines a very large number of checks, many of which
are special-purpose (e.g., only applicable to FPGA HLS code, or code using the
`Abseil`_ library) or have high false positive rates. Here's a list of
recommended checks which are relevant to embedded C/C++ projects and have good
signal-to-noise ratios. See `the list of checks in clang-tidy documentation
<https://clang.llvm.org/extra/clang-tidy/checks/list.html>`__ for detailed
documentation for each check.

.. literalinclude:: ../.clang-tidy
   :dedent:
   :start-after: # DOCSTAG: [clang-tidy-checks]
   :end-before: # DOCSTAG: [clang-tidy-checks]

We will extend this list over time, see :bug:`234876263`.

We do not currently recommend the `Clang Static Analyzers`_ because they suffer
from false positives, and their findings are time-consuming to manually verify.

.. _clang-tidy: https://clang.llvm.org/extra/clang-tidy/
.. _Abseil: https://abseil.io/
.. _.clang-tidy: https://cs.pigweed.dev/pigweed/+/main:.clang-tidy
.. _Clang Static Analyzers: https://clang-analyzer.llvm.org/available_checks.html

How to enable clang-tidy
------------------------
.. tab-set::

   .. tab-item:: Bazel
      :sync: bazel

      We recommend using `bazel_clang_tidy
      <https://github.com/erenon/bazel_clang_tidy>`__ to run clang-tidy from
      Bazel.

      If you're using Pigweed's own host toolchain configuration, see the
      :ref:`module-pw_toolchain-bazel-clang-tidy` section for simple
      step-by-step instructions.

   .. tab-item:: GN
      :sync: gn

      `pw_toolchain/static_analysis_toolchain.gni`_ provides the
      ``pw_static_analysis_toolchain`` template that can be used to create a
      build group performing static analysis. See :ref:`module-pw_toolchain`
      documentation for more details. This group can then be added as a
      presubmit step using :ref:`module-pw_presubmit`.

      You can place a ``.clang-tidy`` file at the root of your repository to
      control which checks are executed. See the `clang documentation`_ for a
      discussion of how the tool chooses which ``.clang-tidy`` files to apply
      when run on a particular source file.

.. _pw_toolchain/static_analysis_toolchain.gni: https://cs.pigweed.dev/pigweed/+/main:pw_toolchain/static_analysis_toolchain.gni
.. _clang documentation: https://clang.llvm.org/extra/clang-tidy/

.. _docs-automatic-analysis-pylint:

Pylint
======
`Pylint`_ is a customizable Python linter that detects problems such as overly
broad ``catch`` statements, unused arguments/variables, and mutable default
parameter values.

.. _Pylint: https://pylint.org/

How to enable Pylint
--------------------
.. tab-set::

   .. tab-item:: Bazel
      :sync: bazel

      Add the following to your ``.bazelrc`` file:

      .. code-block::

         build:pylint --aspects @pigweed//pw_build:pw_pylint.bzl%pylint_aspect
         build:pylint --output_groups=report
         build:pylint --build_tag_filters=-nopylint

      Run Pylint on all your Python targets:

      .. code-block:: sh

         bazelisk build --config=pylint //...

   .. tab-item:: GN
      :sync: gn

      Pylint can be configured to run every time your project is built by adding
      ``python.lint.pylint`` to your default build group. You can also add this
      to a presubmit step (`examples`_), or directly include the
      `python_checks.gn_python_lint`_ presubmit step.

.. _examples: https://cs.opensource.google/search?q=file:pigweed_presubmit.py%20%22python.lint%22&sq=&ss=pigweed%2Fpigweed
.. _python_checks.gn_python_lint: https://cs.pigweed.dev/pigweed/+/main:pw_presubmit/py/pw_presubmit/python_checks.py?q=file:python_checks.py%20gn_python_lint&ss=pigweed%2Fpigweed

Pylint configuration (Bazel only)
---------------------------------

Disabling Pylint for individual build targets
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
To exempt a particular build target from being linted with Pylint, add ``tags =
["nopylint"]`` to its attributes.

Configuring the pylintrc file
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
By default, Pigweed's Pylint aspect uses an empty pylintrc file. To use a
pylintrc file, set the ``@pigweed//pw_build:pylintrc`` label flag. For example,
to use the ``.pylintrc`` at the root of your repository, add the following line
to your ``.bazelrc``:

.. code-block::

   build:pylint --@pigweed//pw_build:pylintrc=//:.pylintrc

You may wish to use `Pigweed's own
<https://cs.pigweed.dev/pigweed/+/main:.pylintrc>`__ ``.pylintrc`` as a starting
point.

Configuring the Pylint binary
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
By default, Pigweed's Pylint aspect fetches Pylint from PyPI using pip, based on
upstream Pigweed's pip requirements. To use a different Pylint binary, set the
``@pigweed//pw_build:pylint`` label flag to point to the binary you wish to use.

In particular, if you also wish to use Pylint from PyPI but at a different
version, create a `py_console_script_binary
<https://rules-python.readthedocs.io/en/latest/api/rules_python/python/entry_points/py_console_script_binary.html>`__
to wrap it and point the label flag to it.

Known limitations
^^^^^^^^^^^^^^^^^
*  The `wrong-import-order
   <https://pylint.readthedocs.io/en/latest/user_guide/messages/convention/wrong-import-order.html>`__
   check is unsupported. Because the directory layout of Python files in the
   Bazel sandbox is different from a regular Python venv, Pylint is confused
   about which imports are first- versus third-party. This seems hard to fix because
   Pylint uses undocumented heuristics to categorize the imports.

.. _docs-automatic-analysis-mypy:

Mypy
====
Python 3 allows for `type annotations`_ for variables, function arguments, and
return values. Most of Pigweed's Python code has type annotations, and these
annotations have caught real bugs in code that didn't yet have unit tests.
`Mypy`_ is an analysis tool that enforces these annotations.

Mypy helps find bugs like when a string is passed into a function that expects a
list of strings—since both are iterables this bug might otherwise be hard to
track down.

.. _type annotations: https://docs.python.org/3/library/typing.html
.. _Mypy: http://mypy-lang.org/

How to enable Mypy
------------------
.. tab-set::

   .. tab-item:: Bazel
      :sync: bazel

      We recommend using `rules_mypy <https://github.com/theoremlp/rules_mypy>`__ for
      running Mypy from Bazel.

      Examples of enabling Mypy in existing projects that may be helpful:

      *  `Sense <http://pwrev.dev/311099>`__, for a downstream project *without* pip
         dependencies.
      *  `Upstream Pigweed <http://pwrev.dev/309473>`__, for a library *with* pip
         dependencies.

   .. tab-item:: GN
      :sync: gn

      Similarly to Pylint, Mypy can be configured to run every time your project
      is built by adding ``python.lint.mypy`` to your default build group.

.. _docs-automatic-analysis-pyrefly:

Pyrefly
=======
`Pyrefly`_ is a fast, Rust-based Python type checker and language server.
Pigweed runs Pyrefly alongside Mypy and Pylint to provide rapid type-checking
feedback across both Bazel and GN builds.

.. _Pyrefly: https://pyrefly.org/

How to enable Pyrefly
---------------------
.. tab-set::

   .. tab-item:: Bazel
      :sync: bazel

      Pigweed provides ``//pw_build/pyrefly:pyrefly.bzl%pyrefly_aspect`` to run
      Pyrefly as a Bazel aspect:

      .. code-block:: sh

         bazelisk build --config=pyrefly //...

      To exempt an individual target from Pyrefly checking, add ``tags =
      ["nopyrefly"]`` to its attributes.

   .. tab-item:: GN
      :sync: gn

      Add ``"pyrefly"`` to ``pw_build_PYTHON_STATIC_ANALYSIS_TOOLS`` in ``.gn``
      (included by default in upstream Pigweed) and add ``python.lint.pyrefly``
      to your default build group.

.. _docs-automatic-analysis-sanitizers:

----------------
Clang sanitizers
----------------
Clang sanitizers detect serious errors common in C/C++ programs.

* asan: `AddressSanitizer`_ detects memory errors such as out-of-bounds access
  and use-after-free.
* tsan: `ThreadSanitizer`_ detects data races.
* ubsan: `UndefinedBehaviorSanitizer`_ is a fast undefined behavior detector.

.. note::
   Pigweed does not yet support `MemorySanitizer`_ (msan). See :bug:`234876100`
   for details.

Unlike clang-tidy, the clang sanitizers are runtime instrumentation: the
instrumented binary needs to be run for issues to be detected. We recommend you
run all of your project's host unit tests (and the host ports of your
application code) with all these sanitizers.

Unfortunately, the different sanitizers cannot generally be enabled at the same
time. We recommend that in CI you run all of your tests three times: once with
asan, again with ubsan, and again with tsan.

.. _AddressSanitizer: https://clang.llvm.org/docs/AddressSanitizer.html
.. _MemorySanitizer: https://clang.llvm.org/docs/MemorySanitizer.html
.. _Pigweed CI console: https://ci.chromium.org/p/pigweed/g/pigweed/console
.. _ThreadSanitizer: https://clang.llvm.org/docs/ThreadSanitizer.html
.. _UndefinedBehaviorSanitizer: https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html

How to enable the sanitizers
============================

Sanitizers in Bazel
-------------------
If you're using Pigweed's own host toolchain configuration, you can enable asan,
tsan, and ubsan by building with the appropriate flags:

.. code-block:: sh

   bazelisk build --@pigweed//pw_toolchain/host_clang:asan //...
   bazelisk build --@pigweed//pw_toolchain/host_clang:ubsan //...
   bazelisk build --@pigweed//pw_toolchain/host_clang:tsan //...

If you're building your own clang-based toolchain, you can add to it
``@pigweed//pw_toolchain/cc/args:asan`` (and analogous flags for tsan and
ubsan).

Sanitizers in GN
----------------
There are two ways to enable sanitizers for your build.

GN args on debug toolchains
^^^^^^^^^^^^^^^^^^^^^^^^^^^
If you are already building your tests with one of the following toolchains (or
a toolchain derived from one of them):

* ``pw_toolchain_host_clang.debug``
* ``pw_toolchain_host_clang.speed_optimized``
* ``pw_toolchain_host_clang.size_optimized``

You can enable the clang sanitizers simply by setting the GN arg
``pw_toolchain_SANITIZERS`` to the desired subset of
``["address", "thread", "undefined"]``. For example, if you only want
to run asan and ubsan, then your arg value should be
``["address", "undefined"]``.

Example
.......
If your project defines a toolchain ``host_clang_debug`` that is derived from
one of the above toolchains, and you'd like to run the ``pw_executable`` target
``sample_binary`` defined in the ``BUILD.gn`` file in ``examples/sample`` with
asan, you would run this:

.. code-block:: bash

   gn gen out --args='pw_toolchain_SANITIZERS=["address"]'
   ninja -C out host_clang_debug/obj/example/sample/bin/sample_binary
   out/host_clang_debug/obj/example/sample/bin/sample_binary

Sanitizer toolchains
^^^^^^^^^^^^^^^^^^^^
Otherwise, instead of using ``gn args`` you can build your tests with the
appropriate toolchain from the following list (or a toolchain derived from one
of them):

* ``pw_toolchain_host_clang.asan``
* ``pw_toolchain_host_clang.ubsan``
* ``pw_toolchain_host_clang.tsan``

See the :ref:`module-pw_toolchain` module documentation for more
about Pigweed toolchains.

-------
Fuzzers
-------
See the :ref:`module-pw_fuzzer` module documentation.

.. |checkbox| raw:: html

   <input type="checkbox">
