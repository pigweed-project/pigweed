.. _contrib-docs-build:

=========================================
Working with the pigweed.dev build system
=========================================
``pigweed.dev`` is built with Bazel. When you want to add or remove files
used by ``pigweed.dev``, you'll need to interact with this Bazel-based
documentation generation (docgen) system.

Check out :ref:`contrib-docs-build-appendix-architecture` for a top-down explanation
of the main components of the docgen system.

.. _contrib-docs-build-quickstart:

----------
Quickstart
----------
#. Build the docs:

   .. code-block:: console

      $ bazelisk build //docs

#. Locally preview the docs:

   .. code-block:: console

      $ bazelisk run //docs:serve

.. _contrib-docs-build-setup:

-----
Setup
-----
Before you can do anything with the Bazel-based docgen system, you must
complete this setup:

#. Complete :ref:`docs-first-time-setup`.

#. :ref:`docs-install-bazel`.

.. _contrib-docs-build-files:

---------------------------
Add files to the docs build
---------------------------

.. _contrib-docs-build-files-doxygen:

Add files to the C/C++ API reference auto-generation system (Doxygen)
=====================================================================
See the Doxygen-specific :ref:`contrib-doxygen-quickstart` for instructions.

.. _contrib-docs-build-files-autodoc:

Add files to the Python API reference auto-generation system (autodoc)
======================================================================
If you see an error like this:

.. code-block:: text

   sphinx.errors.SphinxWarning: autodoc: failed to import module 'benchmark'
   from module 'pw_rpc'; the following exception was raised:
   No module named 'pw_rpc.benchmark'

.. inclusive-language: disable
.. _autodoc: https://www.sphinx-doc.org/en/master/usage/extensions/autodoc.html
.. inclusive-language: enable

It means that `autodoc`_ (the program we use to auto-generate Python API
references) could not find the source code for the module that it was
supposed to document. To fix this:

#. Add your Python target as a dependency to the ``sphinx_build_binary``
   rule in ``//docs/sphinx/BUILD.bazel``:

   .. code-block:: py

      sphinx_build_binary(
          name = "sphinx_build",
          target_compatible_with = incompatible_with_mcu(),
          deps = [
              # …
              "//pw_rpc/py:pw_rpc_benchmark",
              # …
          ],
      )

.. _contrib-docs-build-files-sphinx:

Add reStructuredText files to Sphinx
====================================
#. Package your inputs into a ``sphinx_docs_library``:

   .. code-block:: py

      load("@rules_python//sphinxdocs:sphinx_docs_library.bzl", "sphinx_docs_library")
      load("//pw_build:compatibility.bzl", "incompatible_with_mcu")

      sphinx_docs_library(
          name = "docs",
          srcs = [
              "docs.rst",
          ],
          prefix = "pw_elf/",
          target_compatible_with = incompatible_with_mcu(),
          visibility = ["//visibility:public"],
      )

#. Update ``docs`` in ``//docs/sphinx/BUILD.bazel`` to take a dependency on
   your new ``sphinx_docs_library``:

   .. code-block:: py

      sphinx_docs(
          name = "docs",
          # …
          deps = [
              # …
              "//pw_elf:docs",
              # …
          ]
      )

.. _toctree: https://documatt.com/restructuredtext-reference/element/toctree.html

#. Add your new reStructuredText files to an existing `toctree`_, or create a new one.

.. _contrib-docs-build-files-source:

Add source code to the docs build
=================================
Whenever possible, don't manually write code examples in your reStructuredText
(reST) docs. These code examples will bitrot over time. Instead, put your code
examples in real source code that can actually be built and tested, and then
use Sphinx's ``literalinclude`` feature to insert the code example into your
doc.

#. Put your code example into a unit test:

   .. code-block:: c++

      // examples.cc

      TEST(StringExamples, BufferExample) {
        // START: BufferExample
        // …
        // END: BufferExample
      }

#. Include the code example in your reST:

   .. code-block:: rest

      .. literalinclude:: ./examples.cc
         :language: cpp
         :dedent:
         :start-after: // START: BufferExample
         :end-before: // END: BufferExample

#. Add the source code file to the ``srcs`` list in your
   ``sphinx_docs_library`` target:

   .. code-block:: py

      sphinx_docs_library(
          name = "docs",
          srcs = [
              # …
              "examples.cc",
              # …
          ],
      )

.. _contrib-docs-build-files-images:

Add images
==========
Images should not be checked into the Pigweed repo.
See :ref:`contrib-docs-website-images`.

.. _contrib-docs-build-files-remove:

----------------------------------------
Remove or change files in the docs build
----------------------------------------
Here's the general workflow:

#. Remove or change files that are used in the docs build.

#. :ref:`contrib-docs-build-build`.

#. When the docs build fails, Bazel's logs will tell you what you need to do
   next. If Bazel's logs aren't informative, try some of the tips described
   in :ref:`contrib-docs-build-debug`.

You may need to do some or all of these steps:

* In your module's ``BUILD.bazel`` files, update these rules:

  * ``sphinx_docs_library`` targets (usually named ``docs``)

  * ``filegroup`` targets named ``doxygen``

* Update ``//docs/sphinx/BUILD.bazel``.

* :ref:`redirects <contrib-docs-website-redirects>`.

.. _contrib-docs-build-build:

--------------
Build the docs
--------------
.. code-block:: console

   $ bazelisk build //docs

.. _contrib-docs-build-build-watch:

Watch the docs (automatically rebuild when files change)
========================================================
.. code-block:: console

   $ bazelisk run //:watch build //docs

.. tip::

   Try :ref:`locally previewing the docs <contrib-docs-build-preview>` in one console
   tab and watching the docs in another tab.

.. _contrib-docs-build-preview:

------------------------
Locally preview the docs
------------------------
.. code-block:: console

   $ bazelisk run //docs:serve

A message like this should get printed to ``stdout``:

.. code-block:: text

   Serving...
     Address: http://0.0.0.0:8000
     Serving directory: /home/kayce/pigweed/pigweed/bazel-out/k8-fastbuild/bin/docs/docs/_build/html
         url: file:///home/kayce/pigweed/pigweed/bazel-out/k8-fastbuild/bin/docs/docs/_build/html
     Server CWD: /home/kayce/.cache/bazel/_bazel_kayce/9659373b1552c281136de1c8eeb3080d/execroot/_main/bazel-out/k8-fastbuild/bin/docs/docs.serve.runfiles/_main

You can access the rendered docs at the URL that's printed next to
**Address** (``http://0.0.0.0:8000`` in the example).

.. _contrib-docs-build-list:

---------------------
List all docs sources
---------------------
.. _hermetic: https://bazel.build/basics/hermeticity

Bazel builds the docs in a `hermetic`_ environment. All inputs to the docgen
system must be copied into this hermetic environment. To check that you're
copying your files to the correct directory, run this command:

.. code-block:: console

   $ bazelisk build //docs:sources

The full list of docs sources will be logged:

.. code-block:: text

   bazel-bin/docs/sphinx/_docs/_sources/conf.py
   bazel-bin/docs/sphinx/_docs/_sources/_extensions/bug.py
   bazel-bin/docs/sphinx/_docs/_sources/_extensions/kconfig.py
   bazel-bin/docs/sphinx/_docs/_sources/_extensions/module_metadata.py
   bazel-bin/docs/sphinx/_docs/_sources/_extensions/modules_index.py
   bazel-bin/docs/sphinx/_docs/_sources/_extensions/pigweed_live.py
   bazel-bin/docs/sphinx/_docs/_sources/_extensions/pw_status_codes.py
   bazel-bin/docs/sphinx/_docs/_sources/_extensions/seed_metadata.py
   bazel-bin/docs/sphinx/_docs/_sources/_extensions/sitemap.py
   bazel-bin/docs/sphinx/_docs/_sources/automated_analysis.rst
   bazel-bin/docs/sphinx/_docs/_sources/bazel_compatibility.rst
   bazel-bin/docs/sphinx/_docs/_sources/build_system.rst
   bazel-bin/docs/sphinx/_docs/_sources/changelog.rst
   bazel-bin/docs/sphinx/_docs/_sources/index.rst
   …
   bazel-bin/docs/sphinx/_docs/_sources/third_party/nanopb/docs.rst
   bazel-bin/docs/sphinx/_docs/_sources/third_party/perfetto/docs.rst
   bazel-bin/docs/sphinx/_docs/_sources/third_party/tinyusb/docs.rst

Often times, the Sphinx build fails because a file was not copied to the
correct directory. Listing all docs sources can help you track down where
exactly the file is being incorrectly copied to. You can use the ``prefix`` and
``strip_prefix`` features of ``sphinx_docs_library`` to fix the output path.
Note that ``prefix`` and ``strip_prefix`` are finicky and sometimes don't work
for unknown reasons.

.. _contrib-docs-build-debug:

--------------------
Debug the docs build
--------------------
.. inclusive-language: disable
.. _sphinx-build: https://www.sphinx-doc.org/en/master/man/sphinx-build.html
.. _--verbose: https://www.sphinx-doc.org/en/master/man/sphinx-build.html#cmdoption-sphinx-build-v
.. inclusive-language: enable

When things go wrong, run this command to build the docs in a
non-`hermetic`_ environment:

.. code-block:: console

   $ bazelisk run //docs/sphinx:docs.run

Also consider tweaking these ``extra_opts`` from the ``sphinx_docs`` rule in
``//docs/sphinx/BUILD.bazel``:

* Comment out the ``--silent`` warning to get more verbose logging output.
* Check `sphinx-build`_ to see what other options you might want to add or
  remove. ``sphinx-build`` is the underlying command that the ``sphinx_docs``
  Bazel rule runs.

.. _contrib-docs-build-troubleshoot:

---------------
Troubleshooting
---------------

.. _contrib-docs-build-troubleshoot-autodoc:

autodoc: failed to import module
================================
See :ref:`contrib-docs-build-files-autodoc`.

.. _contrib-docs-build-troubleshoot-emphasis:

Inline emphasis start-string without end-string
===============================================
.. inclusive-language: disable
.. _sphinx-argparse: https://sphinx-argparse.readthedocs.io/en/latest/
.. _autodoc: https://www.sphinx-doc.org/en/master/usage/extensions/autodoc.html
.. _docstrings: https://peps.python.org/pep-0257/
.. inclusive-language: enable

.. code-block:: text

   …/contributing/docs/build.rst:349: WARNING: Inline emphasis start-string without end-string. [docutils]

.. _Docutils: https://www.docutils.org/

This warning originates from `Docutils`_, Sphinx's reStructuredText parser.  It
means that some of our reStructuredText is using emphasis syntax incorrectly.
In reStructuredText you wrap text in single asterisks for italics (e.g.
``*foo*``) or double asterisks for bold (e.g. ``**foo**``). This warning will
get thrown when the asterisks are not closed out (e.g. ``*foo`` or ``**foo``).

Sometimes, Docutils is not able to detect where the error is occurring.
Example: ``:1: (WARNING/2) Inline emphasis start-string without end-string.``

``:1:`` indicates that the error is occurring in the first line of the
source. The source path is usually listed before the line number, see previous
example. When Docutils does not know the source path, it usually means that
the error is originating in our auto-generated Python API reference. The
Python API reference is generated by pulling `docstrings`_ into the Sphinx
docs build via `autodoc`_ or `sphinx-argparse`_. **The docstrings themselves
must be valid reStructuredText**. Example:

.. code-block:: python

   parser.add_argument(
       # …
       help=(
           'Run a build recipe. Include an asterix to match more than one '
           "name. For example: --recipe 'gn_*'"
       ),
       # …
   )

``gn_*`` is the problem in this case. The fix is to wrap the text in double
backticks so that Docutils will parse it as inline code:

.. code-block:: python

   parser.add_argument(
       # …
       help=(
           'Run a build recipe. Include an asterix to match more than one '
           'name. For example: ``--recipe "gn_*"``'
       ),
       # …
   )

To track down the source of the warning, replace the ``--quiet`` option
in ``//docs/sphinx:docs`` with ``-vvv`` (extremely verbose logging), set
the ``--jobs`` option to ``1`` (single-threaded build to make the logging
easier to follow), run the debug build, and pipe the output to a file:

.. code-block:: console

   bazelisk run //docs/sphinx:docs.run > /tmp/sphinx.log

Search the file for ``Inline emphasis start-string without end-string``
and look at what was getting processed immediately before the warning
was thrown.

.. _contrib-docs-build-appendix-architecture:

-------------------------------
Appendix: Architecture overview
-------------------------------
The outputs of some components of the docgen system are used as inputs
to other components.

.. mermaid::

   flowchart LR

     Doxygen --> Breathe
     Breathe --> reST
     reST --> Sphinx
     Rust --> Sphinx
     Python --> Sphinx

.. _Doxygen: https://www.doxygen.nl/
.. _Breathe: https://breathe.readthedocs.io/en/latest/
.. _reStructuredText: https://docutils.sourceforge.io/rst.html
.. _rustdoc: https://doc.rust-lang.org/rustdoc/what-is-rustdoc.html
.. inclusive-language: disable
.. _autodoc: https://www.sphinx-doc.org/en/master/usage/extensions/autodoc.html
.. _Sphinx: https://www.sphinx-doc.org/en/master/
.. inclusive-language: enable
.. _static site generator: https://en.wikipedia.org/wiki/Static_site_generator

* **Doxygen**: We feed a bunch of C/C++ headers to `Doxygen`_. Doxygen parses each
  header and generates XML metadata for all of the classes, functions, structs,
  etc. that it finds. We also publish the Doxygen-generated HTML as a separate
  subsite. This subsite is available at
  `pigweed.dev/doxygen <https://pigweed.dev/doxygen>`_.

* **Breathe**: We provide the Doxygen XML metadata to `Breathe`_ so that C/C++ API
  reference content can be inserted into our reStructuredText files.

* **reST**: We gather up all the `reStructuredText`_ (reST) source files
  that are scattered across the Pigweed repository. Pigweed docs are authored in
  reST. We don't use Markdown.

* **Rust**: `rustdoc`_ generates Rust API reference content, similar to how
  Doxygen generates C/C++ API reference content. The Rust API references are output
  as HTML. It's essentially a separate documentation subsite that is not integrated
  with the rest of ``pigweed.dev`` (yet). This subsite is available at URLs like
  `pigweed.dev/rustdoc/pw_bytes/ <https://pigweed.dev/rustdoc/pw_bytes/>`_.

* **Python**: We use Sphinx's `autodoc`_ feature to auto-generate Python API
  reference content. In order for this to work, the Python modules must be
  listed as dependencies of the ``//docs/sphinx:docs`` target.

* **Sphinx**: Once all the other inputs are ready, we can use `Sphinx`_
  (essentially a `static site generator`_) to build the ``pigweed.dev``
  website.
