.. _module-pw_build-gn:

GN / Ninja
==========
.. pigweed-module-subpage::
   :name: pw_build

The GN / Ninja build system is the primary build system used for upstream
Pigweed development, and is the most tested and feature-rich build system
Pigweed offers.

This module's ``build.gn`` file contains a number of C/C++ ``config``
declarations that are used by upstream Pigweed to set some architecture-agnostic
compiler defaults. (See Pigweed's ``//BUILDCONFIG.gn``)

``pw_build`` also provides several useful GN templates that are used throughout
Pigweed.

Building upstream Pigweed
-------------------------
Pigweed's root ``BUILD.gn`` file contains a variety of groups to help you
control what parts of the project you'd like to build.

* ``default``: Same as just calling ``ninja -C out``. Builds Pigweed's
  documentation, recommended tests, and python linting, and static analysis.
* ``extended_default``: Everything in ``default``, plus some other useful
  configurations that are tested in CQ.
* ``all``: Attempts to build everything in Pigweed. Note that ``pw package`` may
  need to be used to enable some branches of the build.
* ``docs``: Only build Pigweed's documentation.
* ``stm32f429i``: Only build for the STMicroelectronics STM32F429I-DISC1 board.
* ``host``: Only build for the host.

There are a variety of other groups in the root ``BUILD.gn`` file that may be
helpful for covering more areas of the build, or for reducing iteration time
by only building a subset of the default build.

Some currently broken groups are gated behind the ``pw_BUILD_BROKEN_GROUPS``
build argument. You can set this to ``true`` using ``gn args out`` to try to
build and debug known broken build configurations.

Build system philosophies
-------------------------
While Pigweed's GN build is not hermetic, it strives to adhere to principles of
`hermeticity <https://bazel.build/concepts/hermeticity>`_. Some guidelines to
move towards the ideal of hermeticity include:

* Only rely on pre-compiled tools provided by CIPD (or some other versioned,
  pre-compiled binary distribution mechanism). This eliminates build artifact
  differences caused by different tool versions or variations (e.g. same tool
  version built with slightly different compilation flags).
* Do not use absolute paths in Ninja commands. Typically, these appear when
  using ``rebase_path("//path/to/my_script.py")``. Most of the time, Ninja
  steps should be passed paths rebased relative to the build directory (i.e.
  ``rebase_path("//path/to/my_script.py", root_build_dir)``). This ensures build
  commands are the same across different machines.
* Prevent produced artifacts from relying on or referencing system state. This
  includes time stamps, writing absolute paths to generated artifacts, or
  producing artifacts that reference system state in a way that prevents them
  from working the same way on a different machine.
* Isolate build actions to the build directory. In general, the build system
  should not add or modify files outside of the build directory. This can cause
  confusion to users, and makes the concept of a clean build more ambiguous.

Target types
------------
.. code-block::

   import("$dir_pw_build/target_types.gni")

   pw_source_set("my_library") {
     sources = [ "lib.cc" ]
   }

Pigweed defines wrappers around the four basic GN binary types ``source_set``,
``executable``, ``static_library``, and ``shared_library``. These templates
do several things:

#. **Add default configs/deps**

   Rather than binding the majority of compiler flags related to C++ standard,
   cross-compilation, warning/error policy, etc.  directly to toolchain
   invocations, these flags are applied as configs to all ``pw_*`` C/C++ target
   types. The primary motivations for this are to allow some targets to modify
   the default set of flags when needed by specifying ``remove_configs``, and to
   reduce the complexity of building novel toolchains.

   Pigweed's global default configs are set in ``pw_build/default.gni``, and
   individual platform-specific toolchains extend the list by appending to the
   ``default_configs`` build argument.

   Default deps were added to support polyfill, which has since been deprecated.
   Default dependency functionality continues to exist for backwards
   compatibility.

#. **Optionally add link-time binding**

   Some libraries like pw_assert and pw_log are borderline impossible to
   implement well without introducing circular dependencies. One solution for
   addressing this is to break apart the libraries into an interface with
   minimal dependencies, and an implementation with the bulk of the
   dependencies that would typically create dependency cycles. In order for the
   implementation to be linked in, it must be added to the dependency tree of
   linked artifacts (e.g. ``pw_executable``, ``pw_static_library``). Since
   there's no way for the libraries themselves to just happily pull in the
   implementation if someone depends on the interface, the implementation is
   instead late-bound by adding it as a direct dependency of the final linked
   artifact. This is all managed through ``pw_build_LINK_DEPS``, which is global
   for each toolchain and applied to every ``pw_executable``,
   ``pw_static_library``, and ``pw_shared_library``.

#. **Apply a default visibility policy**

   Projects can globally control the default visibility of pw_* target types by
   specifying ``pw_build_DEFAULT_VISIBILITY``. This template applies that as the
   default visibility for any pw_* targets that do not explicitly specify a
   visibility.

#. **Add source file names as metadata**

   All source file names are collected as
   `GN metadata <https://gn.googlesource.com/gn/+/main/docs/reference.md#metadata_collection>`_.
   This list can be writen to a file at build time using ``generated_file``. The
   primary use case for this is to generate a token database containing all the
   source files. This allows :c:macro:`PW_ASSERT` to emit filename tokens even
   though it can't add them to the elf file because of the reasons described at
   :ref:`module-pw_assert-assert-api`.

   .. note::
      ``pw_source_files``, if not rebased will default to outputing module
      relative paths from a ``generated_file`` target.  This is likely not
      useful. Adding a ``rebase`` argument to ``generated_file`` such as
      ``rebase = root_build_dir`` will result in usable paths.  For an example,
      see ``//pw_tokenizer/database.gni``'s ``pw_tokenizer_filename_database``
      template.

The ``pw_executable`` template provides additional functionality around building
complete binaries. As Pigweed is a collection of libraries, it does not know how
its final targets are built. ``pw_executable`` solves this by letting each user
of Pigweed specify a global executable template for their target, and have
Pigweed build against it. This is controlled by the build variable
``pw_executable_config.target_type``, specifying the name of the executable
template for a project.

In some uncommon cases, a project's ``pw_executable`` template definition may
need to stamp out some ``pw_source_set``\s. Since a pw_executable template can't
import ``$dir_pw_build/target_types.gni`` due to circular imports, it should
import ``$dir_pw_build/cc_library.gni`` instead.

.. tip::

  Prefer to use ``pw_executable`` over plain ``executable`` targets to allow
  cleanly building the same code for multiple target configs.

Arguments
^^^^^^^^^
All of the ``pw_*`` target type overrides accept any arguments supported by
the underlying native types, as they simply forward them through to the
underlying target.

Additionally, the following arguments are also supported:

* **remove_configs**: (optional) A list of configs / config patterns to remove
  from the set of default configs specified by the current toolchain
  configuration.
* **remove_public_deps**: (optional) A list of targets to remove from the set of
  default public_deps specified by the current toolchain configuration.

.. _module-pw_build-link-deps:

Link-only deps
--------------
It may be necessary to specify additional link-time dependencies that may not be
explicitly depended on elsewhere in the build. One example of this is a
``pw_assert`` backend, which may need to leave out dependencies to avoid
circular dependencies. Its dependencies need to be linked for executables and
libraries, even if they aren't pulled in elsewhere.

The ``pw_build_LINK_DEPS`` build arg is a list of dependencies to add to all
``pw_executable``, ``pw_static_library``, and ``pw_shared_library`` targets.
This should only be used as a last resort when dependencies cannot be properly
expressed in the build.

.. _module-pw_build-third-party:

Third party libraries
---------------------
Pigweed includes build files for a selection of third-party libraries. For a
given library, these include:

* ``third_party/<library>/library.gni``: Declares build arguments like
  ``dir_pw_third_party_<library>`` that default to ``""`` but can be set to the
  absolute path of the library in order to use it.
* ``third_party/<library>/BUILD.gn``: Describes how to build the library. This
  should import ``third_party/<library>/library.gni`` and refer to source paths
  relative to ``dir_pw_third_party_<library>``.

To add or update GN build files for libraries that only offer Bazel build files,
the Python script at ``pw_build/py/pw_build/bazel_to_gn.py`` may be used.

.. note::
   The ``bazel_to_gn.py`` script is experimental, and may not work on an
   arbitrary Bazel library.

To generate or update the GN offered by Pigweed from an Bazel upstream project,
first create a ``third_party/<library>/bazel_to_gn.json`` file. This file should
describe a single JSON object, with the following fields:

* ``repo``: Required string containing the Bazel repository name.

  .. code-block::

     "repo": "com_google_absl"

* ``targets``: Optional list of Bazel targets to convert, relative to the repo.

  .. code-block::

     "targets": [ "//pkg1:target1", "//pkg2:target2" ]

* ``defaults``: Optional object mapping attribute names to lists of strings.
  These values are treated as implied for the attribute, and will be skipped
  when converting a Bazel rule into a GN target.

  .. code-block::

     "defaults": {
       "copts": [ "-Werror" ]
     }

* ``generate``: Optional boolean indicating whether to generate GN build files a
  third party library. Default is true. This flag may be useful for a third
  party library whose GN build files are manually maintained, but which is
  referenced by another library whose GN build files are generated.

  .. code-block::

     "generate": true

GN build files may be generated using the following command:

.. code-block::

   python3 pw_build/py/pw_build/bazel_to_gn.py <library>

In Bazel, these third-party dependencies should be added as a ``bazel_dep`` so
they can be overridden downstream.

.. _module-pw_build-python-packages:

Python packages
---------------
GN templates for :ref:`Python build automation <docs-python-build>` are
described in :ref:`module-pw_build-python`.

.. toctree::
  :hidden:

  python

.. _module-pw_build-cc_blob_library:

pw_cc_blob_library
------------------
The ``pw_cc_blob_library`` template is useful for embedding binary data into a
program. The template takes in a mapping of symbol names to file paths, and
generates a set of C++ source and header files that embed the contents of the
passed-in files as arrays of ``std::byte``.

The blob byte arrays are constant initialized and are safe to access at any
time, including before ``main()``.

``pw_cc_blob_library`` is also available in the CMake build. It is provided by
``pw_build/cc_blob_library.cmake``.

Arguments
^^^^^^^^^
* ``blobs``: A list of GN scopes, where each scope corresponds to a binary blob
  to be transformed from file to byte array. This is a required field. Blob
  fields include:

  * ``symbol_name``: The C++ symbol for the byte array.
  * ``file_path``: The file path for the binary blob.
  * ``linker_section``: If present, places the byte array in the specified
    linker section.
  * ``alignas``: If present, uses the specified string or integer verbatim in
    the ``alignas()`` specifier for the byte array.

* ``out_header``: The header file to generate. Users will include this file
  exactly as it is written here to reference the byte arrays.
* ``namespace``: An optional (but highly recommended!) C++ namespace to place
  the generated blobs within.

Example
^^^^^^^
**BUILD.gn**

.. code-block::

   pw_cc_blob_library("foo_bar_blobs") {
     blobs: [
       {
         symbol_name: "kFooBlob"
         file_path: "${target_out_dir}/stuff/bin/foo.bin"
       },
       {
         symbol_name: "kBarBlob"
         file_path: "//stuff/bin/bar.bin"
         linker_section: ".bar_section"
       },
     ]
     out_header: "my/stuff/foo_bar_blobs.h"
     namespace: "my::stuff"
     deps = [ ":generate_foo_bin" ]
   }

.. note:: If the binary blobs are generated as part of the build, be sure to
          list them as deps to the pw_cc_blob_library target.

**Generated Header**

.. code-block::

   #pragma once

   #include <array>
   #include <cstddef>

   namespace my::stuff {

   extern const std::array<std::byte, 100> kFooBlob;

   extern const std::array<std::byte, 50> kBarBlob;

   }  // namespace my::stuff

**Generated Source**

.. code-block::

   #include "my/stuff/foo_bar_blobs.h"

   #include <array>
   #include <cstddef>

   #include "pw_preprocessor/compiler.h"

   namespace my::stuff {

   const std::array<std::byte, 100> kFooBlob = { ... };

   PW_PLACE_IN_SECTION(".bar_section")
   const std::array<std::byte, 50> kBarBlob = { ... };

   }  // namespace my::stuff

.. _module-pw_build-facade:

pw_facade
---------
In their simplest form, a :ref:`facade<docs-module-structure-facades>` is a GN
build arg used to change a dependency at compile time. Pigweed targets configure
these facades as needed.

The ``pw_facade`` template bundles a ``pw_source_set`` with a facade build arg.
This allows the facade to provide header files, compilation options or anything
else a GN ``source_set`` provides.

The ``pw_facade`` template declares one or two targets:

* ``$target_name``: The public-facing ``pw_source_set``, with a ``public_dep``
  on the backend. Always declared.
* ``$target_name.facade``: Target with ``public`` headers, ``public_deps``, and
  ``public_configs`` shared between the public-facing ``pw_source_set`` and
  backend to avoid circular dependencies. Only declared if ``public``,
  ``public_deps``, or ``public_configs`` are provided.

.. code-block::

   # Declares ":foo" and ":foo.facade" GN targets
   pw_facade("foo") {
     backend = pw_log_BACKEND
     public_configs = [ ":public_include_path" ]
     public = [ "public/pw_foo/foo.h" ]
   }

Low-level facades like ``pw_assert`` cannot express all of their dependencies
due to the potential for dependency cycles. Facades with this issue may require
backends to place their implementations in a separate build target to be listed
in ``pw_build_LINK_DEPS`` (see :ref:`module-pw_build-link-deps`). The
``require_link_deps`` variable in ``pw_facade`` asserts that all specified build
targets are present in ``pw_build_LINK_DEPS`` if the facade's backend variable
is set.

.. _module-pw_build-python-action:

pw_python_action
----------------
.. seealso::
   - :ref:`module-pw_build-python` for all of Pigweed's Python build GN templates.
   - :ref:`docs-python-build` for details on how the GN Python build works.

The ``pw_python_action`` template is a convenience wrapper around GN's `action
function <https://gn.googlesource.com/gn/+/main/docs/reference.md#func_action>`_
for running Python scripts. The main benefit it provides is resolution of GN
target labels to compiled binary files. This allows Python scripts to be written
independently of GN, taking only filesystem paths as arguments.

Another convenience provided by the template is to allow running scripts without
any outputs. Sometimes scripts run in a build do not directly produce output
files, but GN requires that all actions have an output. ``pw_python_action``
solves this by accepting a boolean ``stamp`` argument which tells it to create a
placeholder output file for the action.

Arguments
^^^^^^^^^
``pw_python_action`` accepts all of the arguments of a regular ``action``
target. Additionally, it has some of its own arguments:

* ``module``: Run the specified Python module instead of a script. Either
  ``script`` or ``module`` must be specified, but not both.
* ``capture_output``: Optional boolean. If true, script output is hidden unless
  the script fails with an error. Defaults to true.
* ``stamp``: Optional variable indicating whether to automatically create a
  placeholder output file for the script. This allows running scripts without
  specifying ``outputs``. If ``stamp`` is true, a generic output file is
  used. If ``stamp`` is a file path, that file is used as a stamp file. Like any
  output file, ``stamp`` must be in the build directory. Defaults to false.
* ``environment``: Optional list of strings. Environment variables to set,
  passed as NAME=VALUE strings.
* ``working_directory``: Optional file path. When provided the current working
  directory will be set to this location before the Python module or script is
  run.
* ``command_launcher``: Optional string. Arguments to prepend to the Python
  command, e.g. ``'/usr/bin/fakeroot --'`` will run the Python script within a
  fakeroot environment.
* ``venv``: Optional gn target of the pw_python_venv that should be used to run
  this action.
* ``python_deps``: Extra dependencies that are required for running the Python
  script for the ``action``. This must be used with ``module`` to specify the
  build dependency of the ``module`` if it is user-defined code.
* ``python_metadata_deps``: Extra dependencies that are ensured completed before
  generating a Python package metadata manifest, not the overall Python script
  ``action``. This should rarely be used by non-Pigweed code.

.. _module-pw_build-python-action-test:

pw_python_action_test
---------------------
The ``pw_python_action_test`` extends :ref:`module-pw_build-python-action` to
create a test that is run by a Python script, and its associated test metadata.

Include action tests in the :ref:`module-pw_unit_test-pw_test_group` to produce
the JSON metadata that :ref:`module-pw_build-test-info` adds.

This template derives several additional targets:

* ``<target_name>.metadata`` produces the test metadata when included in a
  ``pw_test_group``. This metadata includes the Ninja target that runs the test.
* If``action`` is not provided as a label, ``<target_name>.script`` wraps a
  ``pw_python_action`` to run the test as a standalone ``pw_python_package``.
* ``<target_name>.group`` creates a ``pw_python_group`` in order to apply tools,
  e.g. linters, to the standalone package.
* ``<target_name>.lib`` is an empty group for compatibility with
  ``pw_test_group``.
* ``<target_name>.run`` invokes the test.

Targets defined using this template will produce test metadata with a
``test_type`` of "action_test" and a ``ninja_target`` value that will invoke the
test when passed to Ninja, i.e. ``ninja -C out <ninja_target>``.

Arguments
^^^^^^^^^
``pw_python_action_test`` accepts the following arguments:

* All of the arguments of :ref:`module-pw_unit_test-pw_test`.
* ``action``: An optional string or scope. If a string, this should be a label
  to a ``pw_python_action`` target that performs the test. If a scope, this has
  the same meaning as for ``pw_python_script``.
* Optionally, the ``test_type`` and ``extra_metadata`` arguments of the
  :ref:`module-pw_build-test-info` template.
* Optionally, all of the arguments of the :ref:`module-pw_build-python-action`
  template except ``module``, ``capture_output``, ``stamp``, and
  ``python_metadata_deps``.
* Optionally, all of the arguments of the ``pw_python_package`` template except
  ``setup``, ``generate_setup``, ``tests``, ``python_test_deps``, and
  ``proto_library``.

.. _module-pw_build-test-info:

pw_test_info
------------
``pw_test_info`` generates metadata describing tests. To produce a JSON file
containing this metadata:

#. For new modules, add a :ref:`module-pw_unit_test-pw_test_group` to the
   BUILD.gn file. All modules are required to have a ``tests`` target.
#. Include one or more tests or test groups via ``tests`` or ``group_deps``,
   respectively, in the ``pw_test_group``.
#. Set ``output_metadata`` to ``true`` in the ``pw_test_group`` definition.

This template does not typically need to be used directly, unless adding new
types of tests. It is typically used by other templates, such as the
:ref:`module-pw_unit_test-pw_test` and the
:ref:`module-pw_unit_test-pw_test_group`.

Arguments
^^^^^^^^^
* ``test_type``: One of "test_group", "unit_test", "action_test", "perf_test",
  or "fuzz_test".
* ``test_name``: Name of the test. Defaults to the target name.
* ``build_label``: GN label for the test. Defaults to the test name.
* ``extra_metadata``: Additional variables to add to the metadata.

Specific test templates add additional details using ``extra_metadata``. For
example:

* The :ref:`module-pw_unit_test-pw_test_group` includes its collected list of
  tests and test groups as ``deps``.
* The :ref:`module-pw_unit_test-pw_test` and the
  :ref:`module-pw_perf_test-pw_perf_test` includes the ``test_directory``
  that contains the test executable.
* The :ref:`module-pw_build-python-action-test` includes the Ninja target that
  can be used to invoke the Python action and run the test.

Example
^^^^^^^
Let ``//my_module/BUILD.gn`` contain the following:

.. code-block::

   import("$dir_pw_build/python_action_test.gni")
   import("$dir_pw_perf_test/perf_test.gni")
   import("$dir_pw_unit_test/test.gni")

   pw_test("my_unit_test") {
     sources = [ ... ]
     deps = [ ... ]
   }

   pw_python_action_test("my_action_test") {
     script = [ ... ]
     args = [ ... ]
     deps = [ ... ]
   }

   pw_python_action_test("my_integration_test") {
     script = [ ... ]
     args = [ ... ]
     deps = [ ... ]
     tags = [ "integration" ]
   }

   pw_perf_test("my_perf_test") {
     sources = [ ... ]
     deps = [ ... ]
   }

   pw_test_group("tests") {
     tests = [
      ":my_unit_test",
      ":my_action_test",
      ":my_integration_test",
     ]
   }

Let `//BUILD.gn`` contain the following:

.. code-block::

   import("$dir_pw_unit_test/test.gni")

   group("run_tests") {
     deps = [ ":my_module_tests(//targets/my_targets:my_toolchain)" ]
   }

   pw_test_group("my_module_tests") {
     group_deps = [ "//my_module:tests" ]
     output_metadata = true
   }

Then running ``gn gen out`` will produce the following JSON file at
``out/my_toolchain/my_module_tests.testinfo.json``:

.. code-block:: json

   [
     {
       "build_label": "//my_module:my_unit_test",
       "test_directory": "my_toolchain/obj/my_module/test",
       "test_name": "my_unit_test",
       "test_type": "unit_test"
     },
     {
       "build_label": "//my_module:my_action_test",
       "ninja_target": "my_toolchain/obj/my_module/my_action_test.run.stamp",
       "test_name": "my_action_test",
       "test_type": "action_test"
     },
     {
       "build_label": "//my_module:my_integration_test",
       "ninja_target": "my_toolchain/obj/my_module/my_integration_test.run.stamp",
       "tags": [
         "integration"
       ],
       "test_name": "my_integration_test",
       "test_type": "action_test"
     },
     {
       "build_label": "//my_module:my_perf_test",
       "test_directory": "my_toolchain/obj/my_module/test",
       "test_name": "my_perf_test",
       "test_type": "perf_test"
     },
     {
       "build_label": "//my_module:tests",
       "deps": [
         "//my_module:my_unit_test",
         "//my_module:my_action_test",
         "//my_module:my_integration_test",
       ],
       "test_name": "my_module/tests",
       "test_type": "test_group"
     },
     {
       "build_label": "//:my_module_tests",
       "deps": [
         "//my_module:tests",
       ],
       "test_name": "my_module_tests",
       "test_type": "test_group"
     }
   ]

.. _module-pw_build-python-action-expressions:

Expressions
^^^^^^^^^^^
``pw_python_action`` evaluates expressions in ``args``, the arguments passed to
the script. These expressions function similarly to generator expressions in
CMake. Expressions may be passed as a standalone argument or as part of another
argument. A single argument may contain multiple expressions.

Generally, these expressions are used within templates rather than directly in
BUILD.gn files. This allows build code to use GN labels without having to worry
about converting them to files.

.. note::

  We intend to replace these expressions with native GN features when possible.
  See `b/234886742 <http://issuetracker.google.com/234886742>`_.

The following expressions are supported:

.. describe:: <TARGET_FILE(gn_target)>

  Evaluates to the output file of the provided GN target. For example, the
  expression

  .. code-block::

     "<TARGET_FILE(//foo/bar:static_lib)>"

  might expand to

  .. code-block::

     "/home/User/project_root/out/obj/foo/bar/static_lib.a"

  ``TARGET_FILE`` parses the ``.ninja`` file for the GN target, so it should
  always find the correct output file, regardless of the toolchain's or target's
  configuration. Some targets, such as ``source_set`` and ``group`` targets, do
  not have an output file, and attempting to use ``TARGET_FILE`` with them
  results in an error.

  ``TARGET_FILE`` only resolves GN target labels to their outputs. To resolve
  paths generally, use the standard GN approach of applying the
  ``rebase_path(path, root_build_dir)`` function. This function
  converts the provided GN path or list of paths to be relative to the build
  directory, from which all build commands and scripts are executed.

.. describe:: <TARGET_FILE_IF_EXISTS(gn_target)>

  ``TARGET_FILE_IF_EXISTS`` evaluates to the output file of the provided GN
  target, if the output file exists. If the output file does not exist, the
  entire argument that includes this expression is omitted, even if there is
  other text or another expression.

  For example, consider this expression:

  .. code-block::

     "--database=<TARGET_FILE_IF_EXISTS(//alpha/bravo)>"

  If the ``//alpha/bravo`` target file exists, this might expand to the
  following:

  .. code-block::

     "--database=/home/User/project/out/obj/alpha/bravo/bravo.elf"

  If the ``//alpha/bravo`` target file does not exist, the entire
  ``--database=`` argument is omitted from the script arguments.

.. describe:: <TARGET_OBJECTS(gn_target)>

  Evaluates to the object files of the provided GN target. Expands to a separate
  argument for each object file. If the target has no object files, the argument
  is omitted entirely. Because it does not expand to a single expression, the
  ``<TARGET_OBJECTS(...)>`` expression may not have leading or trailing text.

  For example, the expression

  .. code-block::

     "<TARGET_OBJECTS(//foo/bar:a_source_set)>"

  might expand to multiple separate arguments:

  .. code-block::

     "/home/User/project_root/out/obj/foo/bar/a_source_set.file_a.cc.o"
     "/home/User/project_root/out/obj/foo/bar/a_source_set.file_b.cc.o"
     "/home/User/project_root/out/obj/foo/bar/a_source_set.file_c.cc.o"

Example
^^^^^^^
.. code-block::

   import("$dir_pw_build/python_action.gni")

   pw_python_action("postprocess_main_image") {
     script = "py/postprocess_binary.py"
     args = [
       "--database",
       rebase_path("my/database.csv", root_build_dir),
       "--binary=<TARGET_FILE(//firmware/images:main)>",
     ]
     stamp = true
   }

.. _module-pw_build-evaluate-path-expressions:

pw_evaluate_path_expressions
----------------------------
It is not always feasible to pass information to a script through command line
arguments. If a script requires a large amount of input data, writing to a file
is often more convenient. However, doing so bypasses ``pw_python_action``'s GN
target label resolution, preventing such scripts from working with build
artifacts in a build system-agnostic manner.

``pw_evaluate_path_expressions`` is designed to address this use case. It takes
a list of input files and resolves target expressions within them, modifying the
files in-place.

Refer to ``pw_python_action``'s :ref:`module-pw_build-python-action-expressions`
section for the list of supported expressions.

.. note::

  ``pw_evaluate_path_expressions`` is typically used as an intermediate
  sub-target of a larger template, rather than a standalone build target.

Arguments
^^^^^^^^^
* ``files``: A list of scopes, each containing a ``source`` file to process and
  a ``dest`` file to which to write the result.

Example
^^^^^^^
The following template defines an executable target which additionally outputs
the list of object files from which it was compiled, making use of
``pw_evaluate_path_expressions`` to resolve their paths.

.. code-block::

   import("$dir_pw_build/evaluate_path_expressions.gni")

   template("executable_with_artifacts") {
     executable("${target_name}.exe") {
       sources = invoker.sources
       if defined(invoker.deps) {
         deps = invoker.deps
       }
     }

     _artifacts_input = "$target_gen_dir/${target_name}_artifacts.json.in"
     _artifacts_output = "$target_gen_dir/${target_name}_artifacts.json"
     _artifacts = {
       binary = "<TARGET_FILE(:${target_name}.exe)>"
       objects = "<TARGET_OBJECTS(:${target_name}.exe)>"
     }
     write_file(_artifacts_input, _artifacts, "json")

     pw_evaluate_path_expressions("${target_name}.evaluate") {
       files = [
         {
           source = _artifacts_input
           dest = _artifacts_output
         },
       ]
     }

     group(target_name) {
       deps = [
         ":${target_name}.exe",
         ":${target_name}.evaluate",
       ]
     }
   }

.. _module-pw_build-pw_exec:

pw_exec
-------
``pw_exec`` allows for execution of arbitrary programs. It is a wrapper around
``pw_python_action`` but allows for specifying the program to execute.

.. note::

   Prefer to use ``pw_python_action`` instead of calling out to shell
   scripts, as the Python will be more portable. ``pw_exec`` should generally
   only be used for interacting with legacy/existing scripts.

Arguments
^^^^^^^^^
* ``program``: The program to run. Can be a full path or just a name (in which
  case $PATH is searched).
* ``args``: Optional list of arguments to the program.
* ``deps``: Dependencies for this target.
* ``public_deps``: Public dependencies for this target. In addition to outputs
  from this target, outputs generated by public dependencies can be used as
  inputs from targets that depend on this one. This is not the case for private
  deps.
* ``inputs``: Optional list of build inputs to the program.
* ``outputs``: Optional list of artifacts produced by the program's execution.
* ``env``: Optional list of key-value pairs defining environment variables for
  the program.
* ``env_file``: Optional path to a file containing a list of newline-separated
  key-value pairs defining environment variables for the program.
* ``args_file``: Optional path to a file containing additional positional
  arguments to the program. Each line of the file is appended to the
  invocation. Useful for specifying arguments from GN metadata.
* ``skip_empty_args``: If args_file is provided, boolean indicating whether to
  skip running the program if the file is empty. Used to avoid running
  commands which error when called without arguments.
* ``capture_output``: If true, output from the program is hidden unless the
  program exits with an error. Defaults to true.
* ``working_directory``: The working directory to execute the subprocess with.
  If not specified it will not be set and the subprocess will have whatever
  the parent current working directory is.
* ``venv``: Python virtualenv to pass along to the underlying
  :ref:`module-pw_build-pw_python_action`.
* ``visibility``: GN visibility to apply to the underlying target.

Example
^^^^^^^
.. code-block::

   import("$dir_pw_build/exec.gni")

   pw_exec("hello_world") {
     program = "/bin/sh"
     args = [
       "-c",
       "echo hello \$WORLD",
     ]
     env = [
       "WORLD=world",
     ]
   }

pw_input_group
--------------
``pw_input_group`` defines a group of input files which are not directly
processed by the build but are still important dependencies of later build
steps. This is commonly used alongside metadata to propagate file dependencies
through the build graph and force rebuilds on file modifications.

For example ``pw_docgen`` defines a ``pw_doc_group`` template which outputs
metadata from a list of input files. The metadata file is not actually part of
the build, and so changes to any of the input files do not trigger a rebuild.
This is problematic, as targets that depend on the metadata should rebuild when
the inputs are modified but GN cannot express this dependency.

``pw_input_group`` solves this problem by allowing a list of files to be listed
in a target that does not output any build artifacts, causing all dependent
targets to correctly rebuild.

Arguments
^^^^^^^^^
``pw_input_group`` accepts all arguments that can be passed to a ``group``
target, as well as requiring one extra:

* ``inputs``: List of input files.

Example
^^^^^^^
.. code-block::

   import("$dir_pw_build/input_group.gni")

   pw_input_group("foo_metadata") {
     metadata = {
       files = [
         "x.foo",
         "y.foo",
         "z.foo",
       ]
     }
     inputs = metadata.files
   }

Targets depending on ``foo_metadata`` will rebuild when any of the ``.foo``
files are modified.

pw_zip
------
``pw_zip`` is a target that allows users to zip up a set of input files and
directories into a single output ``.zip`` file—a simple automation of a
potentially repetitive task.

Arguments
^^^^^^^^^
* ``inputs``: List of source files as well as the desired relative zip
  destination. See below for the input syntax.
* ``dirs``: List of entire directories to be zipped as well as the desired
  relative zip destination. See below for the input syntax.
* ``output``: Filename of output ``.zip`` file.
* ``deps``: List of dependencies for the target.

Input Syntax
^^^^^^^^^^^^
Inputs all need to follow the correct syntax:

#. Path to source file or directory. Directories must end with a ``/``.
#. The delimiter (defaults to ``>``).
#. The desired destination of the contents within the ``.zip``. Must start
   with ``/`` to indicate the zip root. Any number of subdirectories are
   allowed. If the source is a file it can be put into any subdirectory of the
   root. If the source is a file, the zip copy can also be renamed by ending
   the zip destination with a filename (no trailing ``/``).

Thus, it should look like the following: ``"[source file or dir] > /"``.

Example
^^^^^^^
Let's say we have the following structure for a ``//source/`` directory:

.. code-block::

   source/
   ├── file1.txt
   ├── file2.txt
   ├── file3.txt
   └── some_dir/
       ├── file4.txt
       └── some_other_dir/
           └── file5.txt

And we create the following build target:

.. code-block::

   import("$dir_pw_build/zip.gni")

   pw_zip("target_name") {
     inputs = [
       "//source/file1.txt > /",             # Copied to the zip root dir.
       "//source/file2.txt > /renamed.txt",  # File renamed.
       "//source/file3.txt > /bar/",         # File moved to the /bar/ dir.
     ]

     dirs = [
       "//source/some_dir/ > /bar/some_dir/",  # All /some_dir/ contents copied
                                               # as /bar/some_dir/.
     ]

     # Note on output: if the specific output directory isn't defined
     # (such as output = "zoo.zip") then the .zip will output to the
     # same directory as the BUILD.gn file that called the target.
     output = "//$target_out_dir/foo.zip"  # Where the foo.zip will end up
   }

This will result in a ``.zip`` file called ``foo.zip`` stored in
``//$target_out_dir`` with the following structure:

.. code-block::

   foo.zip
   ├── bar/
   │   ├── file3.txt
   │   └── some_dir/
   │       ├── file4.txt
   │       └── some_other_dir/
   │           └── file5.txt
   ├── file1.txt
   └── renamed.txt

.. _module-pw_build-relative-source-file-names:

pw_relative_source_file_names
-----------------------------
This template recursively walks the listed dependencies and collects the names
of all the headers and source files required by the targets, and then transforms
them such that they reflect the ``__FILE__`` when pw_build's ``relative_paths``
config is applied. This is primarily intended for side-band generation of
pw_tokenizer tokens so file name tokens can be utilized in places where
pw_tokenizer is unable to embed token information as part of C/C++ compilation.

This template produces a JSON file containing an array of strings (file paths
with ``-ffile-prefix-map``-like transformations applied) that can be used to
:ref:`generate a token database <module-pw_tokenizer-database-creation>`.

Arguments
^^^^^^^^^
* ``deps``: A required list of targets to recursively extract file names from.
* ``outputs``: A required array with a single element: the path to write the
  final JSON file to.

Example
^^^^^^^
Let's say we have the following project structure:

.. code-block::

   project root
   ├── foo/
   │   ├── foo.h
   │   └── foo.cc
   ├── bar/
   │   ├── bar.h
   │   └── bar.cc
   ├── unused/
   │   ├── unused.h
   │   └── unused.cc
   └── main.cc

And a BUILD.gn at the root:

.. code-block::

   pw_source_set("bar") {
     public_configs = [ ":bar_headers" ]
     public = [ "bar/bar.h" ]
     sources = [ "bar/bar.cc" ]
   }

   pw_source_set("foo") {
     public_configs = [ ":foo_headers" ]
     public = [ "foo/foo.h" ]
     sources = [ "foo/foo.cc" ]
     deps = [ ":bar" ]
   }


   pw_source_set("unused") {
     public_configs = [ ":unused_headers" ]
     public = [ "unused/unused.h" ]
     sources = [ "unused/unused.cc" ]
     deps = [ ":bar" ]
   }

   pw_executable("main") {
     sources = [ "main.cc" ]
     deps = [ ":foo" ]
   }

   pw_relative_source_file_names("main_source_files") {
     deps = [ ":main" ]
     outputs = [ "$target_gen_dir/main_source_files.json" ]
   }

The json file written to `out/gen/main_source_files.json` will contain:

.. code-block::

   [
     "bar/bar.cc",
     "bar/bar.h",
     "foo/foo.cc",
     "foo/foo.h",
     "main.cc"
   ]

Since ``unused`` isn't a transitive dependency of ``main``, its source files
are not included. Similarly, even though ``bar`` is not a direct dependency of
``main``, its source files *are* included because ``foo`` brings in ``bar`` as
a transitive dependency.

Note how the file paths in this example are relative to the project root rather
than being absolute paths (e.g. ``/home/user/ralph/coding/my_proj/main.cc``).
This is a result of transformations applied to strip absolute pathing prefixes,
matching the behavior of pw_build's ``$dir_pw_build:relative_paths`` config.

Build time errors: pw_error and pw_build_assert
-----------------------------------------------
In Pigweed's complex, multi-toolchain GN build it is not possible to build every
target in every configuration. GN's ``assert`` statement is not ideal for
enforcing the correct configuration because it may prevent the GN build files or
targets from being referred to at all, even if they aren't used.

The ``pw_error`` GN template results in an error if it is executed during the
build. These error targets can exist in the build graph, but cannot be depended
on without an error.

``pw_build_assert`` evaluates to a ``pw_error`` if a condition fails or nothing
(an empty group) if the condition passes. Targets can add a dependency on a
``pw_build_assert`` to enforce a condition at build time.

The templates for build time errors are defined in ``pw_build/error.gni``.

.. _module-pw_build-gn-pw_coverage_report:

Generate code coverage reports: ``pw_coverage_report``
------------------------------------------------------
Pigweed supports generating coverage reports, in a variety of formats, for C/C++
code using the ``pw_coverage_report`` GN template.

Coverage Caveats
^^^^^^^^^^^^^^^^
There are currently two code coverage caveats when enabled:

#. Coverage reports are only populated based on host tests that use a ``clang``
   toolchain.

#. Coverage reports will only show coverage information for headers included in
   a test binary.

These two caveats mean that all device-specific code that cannot be compiled for
and run on the host will not be able to have reports generated for them, and
that the existence of these files will not appear in any coverage report.

Try to ensure that your code can be written in a way that it can be compiled
into a host test for the purpose of coverage reporting, although this is
sometimes impossible due to requiring hardware-specific APIs to be available.

Coverage Instrumentation
^^^^^^^^^^^^^^^^^^^^^^^^
For the ``pw_coverage_report`` to generate meaningful output, you must ensure
that it is invoked by a toolchain that instruments tests for code coverage
collection and output.

Instrumentation is controlled by two GN build arguments:

- ``pw_toolchain_COVERAGE_ENABLED`` being set to ``true``.
- ``pw_toolchain_PROFILE_SOURCE_FILES`` is an optional argument that provides a
  list of source files to selectively collect coverage.

.. note::

  It is possible to also instrument binaries for UBSAN, ASAN, or TSAN at the
  same time as coverage. However, TSAN will find issues in the coverage
  instrumentation code and fail to properly build.

This can most easily be done by using the ``host_clang_coverage`` toolchain
provided in :ref:`module-pw_toolchain`, but you can also create custom
toolchains that manually set these GN build arguments as well.

``pw_coverage_report``
^^^^^^^^^^^^^^^^^^^^^^
``pw_coverage_report`` is basically a GN frontend to the ``llvm-cov``
`tool <https://llvm.org/docs/CommandGuide/llvm-cov.html>`_ that can be
integrated into the normal build.

It can be found at ``pw_build/coverage_report.gni`` and is available through
``import("$dir_pw_build/coverage_report.gni")``.

The supported report formats are:

- ``text``: A text representation of the code coverage report. This
  format is not suitable for further machine manipulation and is instead only
  useful for cases where a human needs to interpret the report. The text format
  provides a nice summary, but if you desire to drill down into the coverage
  details more, please consider using ``html`` instead.

  - This is equivalent to ``llvm-cov show --format text`` and similar to
    ``llvm-cov report``.

- ``html``: A static HTML site that provides an overall coverage summary and
  per-file information. This format is not suitable for further machine
  manipulation and is instead only useful for cases where a human needs to
  interpret the report.

  - This is equivalent to ``llvm-cov show --format html``.

- ``lcov``: A machine-friendly coverage report format. This format is not human-
  friendly. If that is necessary, use ``text`` or ``html`` instead.

  - This is equivalent to ``llvm-cov export --format lcov``.

- ``json``: A machine-friendly coverage report format. This format is not human-
  friendly. If that is necessary, use ``text`` or ``html`` instead.

  - This is equivalent to ``llvm-cov export --format json``.

Arguments
"""""""""
There are three classes of ``template`` arguments: build, coverage, and test.

**Build Arguments:**

- ``enable_if`` (optional): Conditionally activates coverage report generation when set to
  a boolean expression that evaluates to ``true``. This can be used to allow
  project builds to conditionally enable or disable coverage reports to minimize
  work needed for certain build configurations.

- ``failure_mode`` (optional/unstable): Specify the failure mode for
  ``llvm-profdata`` (used to merge inidividual profraw files from ``pw_test``
  runs). Available options are ``"any"`` (default) or ``"all"``.

  - This should be considered an unstable/deprecated argument that should only
    be used as a last resort to get a build working again. Using
    ``failure_mode = "all"`` usually indicates that there are underlying
    problems in the build or test infrastructure that should be independently
    resolved. Please reach out to the Pigweed team for assistance.

**Coverage Arguments:**

- ``filter_paths`` (optional): List of file paths to include when generating the
  coverage report. These cannot be regular expressions, but can be concrete file
  or folder paths. Folder paths will allow all files in that directory or any
  recursive child directory.

  - These are passed to ``llvm-cov`` by the optional trailing positional
    ``[SOURCES]`` arguments.

- ``ignore_filename_patterns`` (optional): List of file path regular expressions
  to ignore when generating the coverage report.

  - These are passed to ``llvm-cov`` via ``--ignore-filename-regex`` named
    parameters.

**Test Arguments (one of these is required to be provided):**

- ``tests``: A list of ``pw_test`` :ref:`targets<module-pw_unit_test-pw_test>`.

- ``group_deps``: A list of ``pw_test_group``
  :ref:`targets<module-pw_unit_test-pw_test_group>`.

.. note::

  ``tests`` and ``group_deps`` are treated exactly the same by
  ``pw_coverage_report``, so it is not that important to ensure they are used
  properly.

Target Expansion
""""""""""""""""
``pw_coverage_report(<target_name>)`` expands to one concrete target for each
report format.

- ``<target_name>.text``: Generates the ``text`` coverage report.

- ``<target_name>.html``: Generates the ``html`` coverage report.

- ``<target_name>.lcov``: Generates the ``lcov`` coverage report.

- ``<target_name>.json``: Generates the ``json`` coverage report.

To use any of these targets, you need only to add a dependency on the desired
target somewhere in your build.

There is also a ``<target_name>`` target generated that is a ``group`` that adds
a dependency on all of the format-specific targets listed above.

.. note::
  These targets are always available, even when the toolchain executing the
  target does not support coverage or coverage is not enabled. In these cases,
  the targets are set to empty groups.

Coverage Output
^^^^^^^^^^^^^^^
Coverage reports are currently generated and placed into the build output
directory associated with the path to the GN file where the
``pw_coverage_report`` is used in a subfolder named
``<target_name>.<report_type>``.

.. note::

  Due to limitations with telling GN the entire output of coverage reports
  (stemming from per-source-file generation for HTML and text representations),
  it is not as simple as using GN's built-in ``copy`` to be able to move these
  coverage reports to another output location. However, it seems possible to add
  a target that can use Python to copy the entire output directory.

Improved Ninja interface
------------------------
Ninja includes a basic progress display, showing in a single line the number of
targets finished, the total number of targets, and the name of the most recent
target it has either started or finished.

For additional insight into the status of the build, Pigweed includes a Ninja
wrapper, ``pw-wrap-ninja``, that displays additional real-time information about
the progress of the build. The wrapper is invoked the same way you'd normally
invoke Ninja:

.. code-block:: sh

   pw-wrap-ninja -C out

The script lists the progress of the build, as well as the list of targets that
Ninja is currently building, along with a timer that measures how long each
target has been building for:

.. code-block::

   [51.3s] Building [8924/10690] ...
     [10.4s] c++ pw_strict_host_clang_debug/obj/pw_string/string_test.lib.string_test.cc.o
     [ 9.5s] ACTION //pw_console/py:py.lint.mypy(//pw_build/python_toolchain:python)
     [ 9.4s] ACTION //pw_console/py:py.lint.pylint(//pw_build/python_toolchain:python)
     [ 6.1s] clang-tidy ../pw_log_rpc/log_service.cc
     [ 6.1s] clang-tidy ../pw_log_rpc/log_service_test.cc
     [ 6.1s] clang-tidy ../pw_log_rpc/rpc_log_drain.cc
     [ 6.1s] clang-tidy ../pw_log_rpc/rpc_log_drain_test.cc
     [ 5.4s] c++ pw_strict_host_clang_debug/obj/BUILD_DIR/pw_strict_host_clang_debug/gen/pw...
     ... and 109 more

This allows you to, at a glance, know what Ninja's currently building, which
targets are bottlenecking the rest of the build, and which targets are taking
an unusually long time to complete.

``pw-wrap-ninja`` includes other useful functionality as well. The
``--write-trace`` option writes a build trace to the specified path, which can
be viewed in the `Perfetto UI <https://ui.perfetto.dev/>`_, or via Chrome's
built-in ``chrome://tracing`` tool.

.. _module-pw_build-gn-pw_linker_script:

pw_linker_script
----------------
Preprocess a linker script and turn it into a target.

In lieu of direct GN support for linker scripts, this template makes it
possible to run the C Preprocessor on a linker script file so defines can
be properly evaluated before the linker script is passed to linker.

Arguments
^^^^^^^^^
- ``linker_script``: The linker script to send through the C preprocessor.

- ``defines``: Preprocessor defines to apply when running the C preprocessor.

- ``cflags``: Flags to pass to the C compiler.

- ``includes``: Include these files when running the C preprocessor.

- ``inputs``: Files that, when changed, should trigger a re-build of the linker
  script. linker_script and includes are implicitly added to this by the
  template.

Example
^^^^^^^
.. code-block::

   pw_linker_script("generic_linker_script") {
     defines = [
       "PW_HEAP_SIZE=1K",
       "PW_NOINIT_SIZE=512"
     ]
     linker_script = "basic_script.ld"
   }


pw_copy_and_patch_file
----------------------
Provides the ability to patch a file as part of the build.

The source file will not be patched in place, but instead copied into the
output directory before patching. The output of this target will be the
patched file.

Arguments
^^^^^^^^^
- ``source``: The source file to be patched.

- ``out``: The output file containing the patched contents.

- ``patch_file``: The patch file.

- ``root``: The root directory for applying the path.

Example
^^^^^^^

To apply the patch `changes.patch` to the file `data/file.txt` which is located
in the packages directory `<PW_ROOT>/environment/packages/external_sdk`.

.. code-block::

   pw_copy_and_patch_file("apply_patch") {
     source = "$EXTERNAL_SDK/data/file.txt"
     out = "data/patched_file.txt"
     patch_file = "changes.patch"
     root = "$EXTERNAL_SDK"
   }
