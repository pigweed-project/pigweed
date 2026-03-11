.. _module-pw_toolchain-bazel:

===============================
Bazel build system integrations
===============================
Pigweed provides a suite of Bazel build integrations to compliment existing
Bazel toolchain constructs such as `rules_cc toolchains <https://github.com/bazelbuild/rules_cc/blob/main/cc/toolchains/README.md>`_
to make it easier to design robust, feature-rich toolchains.

.. _module-pw_toolchain-bazel-upstream-pigweed-toolchains:

---------------------------
Upstream Pigweed toolchains
---------------------------
Pigweed's C/C++ toolchains can be registered from a bzlmod project by adding
the following to your ``MODULE.bazel``:

.. code-block:: py

   register_toolchains(
       "@pigweed//pw_toolchain:cc_toolchain_cortex-m0",
       "@pigweed//pw_toolchain:cc_toolchain_cortex-m0plus",
       "@pigweed//pw_toolchain:cc_toolchain_cortex-m33",
       "@pigweed//pw_toolchain/arm_gcc:arm_gcc_cc_toolchain_cortex-m3",
       "@pigweed//pw_toolchain/arm_gcc:arm_gcc_cc_toolchain_cortex-m4",
       "@pigweed//pw_toolchain/host_clang:host_cc_toolchain_linux",
       "@pigweed//pw_toolchain/host_clang:host_cc_toolchain_macos",
       "@pigweed//pw_toolchain/riscv_clang:riscv_clang_cc_toolchain_rv32imc",
       "@pigweed//pw_toolchain/riscv_clang:riscv_clang_cc_toolchain_rv32imac",
       dev_dependency = True,
   )

If you create custom ARM or RISC-V toolchains, you may want to remove
Pigweed's device-specific toolchains to avoid them accidentally getting selected
for your device build.

.. admonition:: Note
   :class: warning

   Pigweed's upstream toolchains are subject to change without notice. If you
   would prefer more stability in toolchain configurations, consider declaring
   custom toolchains in your project.

Injecting custom flags
======================
If you wish to add extra arguments to Pigweed's toolchains, you can set
the ``--@pigweed//pw_toolchain/cc/args:extra_toolchain_args`` label flag
to point at a ``cc_args`` or ``cc_args_list`` rule that contains the flags
you'd like to add.

For platform-specific flags, you can set this flag in your ``platform`` flags
rather than in a ``.bazelrc`` file.

.. _module-pw_toolchain-bazel-layering-check:

Layering check
==============
Upstream Pigweed toolchains have support for `layering check
<https://maskray.me/blog/2022-09-25-layering-check-with-clang>`__. In short,
enabling layering check makes it a compile-time error to ``#include`` a header
that's not in the ``hdrs`` of a ``cc_library`` you directly depend on. This
produces cleaner dependency graphs and is recommended for all users.

.. admonition:: Note

   Layering check requires Bazel 8.0.0 or newer.

How to enable layering check?
-----------------------------
#. Add ``common --@pigweed//pw_toolchain/host_clang:layering_check`` to your
   ``.bazelrc``. This does not by itself enable the check, it only instructs
   Bazel to include support for it in the toolchain configuration. (This flag
   will become true by default and be removed once all known Pigweed users are
   on Bazel 8.)
#. Enable the ``layering_check`` feature to enable enforcement. The recommended
   way to do this is to add a `REPO.bazel
   <https://bazel.build/external/overview#repo.bazel>`__ file at the root of
   your project, with the following content:

   .. code-block:: py

      repo(
          features = ["layering_check"],
      )

   This will enable ``layering_check`` for all code in your project, but not for
   code coming from any external dependencies also built with Bazel.

Gradual rollout
---------------
When you enable layering check for the first time, you will likely get a very
large number of errors, since many packages in your project will contain
layering violations. If there are too many errors to fix at once, here's one way
to roll out ``layering_check`` gradually:

#. Enable the ``layering_check`` feature for the entire repo, as discussed
   above.
#. In the same commit, disable layering check for each individual package
   (``BUILD.bazel`` file). This can be done using the following `buildozer
   <https://github.com/bazelbuild/buildtools/blob/main/buildozer/README.md>`__
   one-liner:

   .. code-block:: console

      buildozer 'add features -layering_check' '//...:__pkg__'

   Note the ``-`` in front of ``layering_check`` in the command above!

#. Re-enable layering check package by package by removing the lines added by
   buildozer. When no ``-layering_check`` remains in your codebase, you've
   enabled layering check for the entire repo!

You can also enable or disable the ``layering_check`` feature for individual
targets, using the `features
<https://bazel.build/reference/be/common-definitions#common.features>`__
attribute.

Limitations
-----------
#. Layering check only applies to ``#include`` statements in source files, not
   in header files. To also apply it to header files, you need the
   ``parse_headers`` feature. Pigweed's toolchains do not yet contain its
   implementation. Adding it is tracked at :bug:`391367050`.
#. Layering check will not prevent you from using symbols from transitively
   included headers. For this, use `misc-include-cleaner
   <https://clang.llvm.org/extra/clang-tidy/checks/misc/include-cleaner.html>`__
   in clang-tidy.  See also :bug:`329671260`.
#. A pattern we use for swapping header implementations using a label flag
   leads to layering check violations. Figuring out an alternative pattern is
   tracked at :bug:`391394448`.

.. _module-pw_toolchain-bazel-clang-tidy:

clang-tidy
==========
To integrate Pigweed's toolchain with `bazel_clang_tidy
<https://github.com/erenon/bazel_clang_tidy>`_:

#. Add a ``.clang-tidy`` file at the root of your repository listing the checks
   you wish to enable. :cs:`Pigweed's own .clang-tidy file <main:.clang-tidy>`
   shows some checks we recommend.

#. Create a ``filegroup`` target containing that file in ``BUILD.bazel`` at
   the root of your repo.

   .. code-block:: python

      filegroup(
         name = "clang_tidy_config",
         srcs = [".clang-tidy"],
      )

#. Add `bazel_clang_tidy`_ to your ``MODULE.bazel``.

   .. code-block::python

      git_repository = use_repo_rule(
         "@bazel_tools//tools/build_defs/repo:git.bzl",
         "git_repository",
      )
      git_repository(
         name = "bazel_clang_tidy",
         # Check the repository for the latest version!
         commit = "db677011c7363509a288a9fb3bf0a50830bbf791",
         remote = "https://github.com/erenon/bazel_clang_tidy.git",
      )

#. Add a ``clang-tidy`` config in your ``.bazelrc`` file.

   .. code-block:: python

      # clang-tidy configuration
      build:clang-tidy --aspects @bazel_clang_tidy//clang_tidy:clang_tidy.bzl%clang_tidy_aspect
      build:clang-tidy --output_groups=report
      build:clang-tidy --@bazel_clang_tidy//:clang_tidy_config=//:clang_tidy_config
      # Use the clang-tidy executable from Pigweed's toolchain, and include
      # our sysroot headers.
      build:clang-tidy --@bazel_clang_tidy//:clang_tidy_executable=@pigweed//pw_toolchain/host_clang:copy_clang_tidy
      build:clang-tidy --@bazel_clang_tidy//:clang_tidy_additional_deps=@pigweed//pw_toolchain/host_clang:sysroot_root
      # Skip any targets with tags = ["noclangtidy"]. This allows a gradual
      # rollout.
      build:clang-tidy --build_tag_filters=-noclangtidy
      # We need to disable this warning to avoid spurious "#pragma once in main file"
      # warnings for header-only libraries. For another approach, see
      # https://github.com/mongodb-forks/bazel_clang_tidy/pull/2
      build:clang-tidy --copt=-Wno-pragma-once-outside-header

Now ``bazelisk build --config=clang-tidy //...`` will run clang-tidy for all
``cc_library`` targets in your repo!

As an example of this setup, see `the CL that added clang-tidy support to our
Quickstart repo <http://pwrev.dev/266934>`__.

Conversion warnings
===================
By default, upstream Pigweed is built with `-Wconversion
<https://clang.llvm.org/docs/DiagnosticsReference.html#wconversion>`__ enabled.
However, this was not always the case, and many Pigweed targets contain
``-Wconversion`` violations. (:bug:`259746255` tracks fixing all of these.)

Upstream allowlist
------------------
Do not add new ``-Wconversion`` violations to the Pigweed codebase.

If you write new code that fails to build because it includes a header with a
pre-existing ``-Wconversion`` violation, try to fix the pre-existing violation.

As a last resort, you may add the ``features = ["-conversion_warnings"]`` (note
the ``-``!) attribute to your ``cc_library`` or other build target:

.. code-block:: py

   cc_library(
      name = "…",
      features = ["-conversion_warnings"],
   )

This will disable ``-Wconversion`` for this target.

Downstream use
--------------
If you would like to enable ``-Wconversion`` in a downstream project that uses
Pigweed's toolchains, add a `REPO.bazel
<https://bazel.build/external/overview#repo.bazel>`__ file at the root of
your project, with the following content:

.. code-block:: py

   repo(
       features = ["conversion_warnings"],
   )

This will enable ``-Wconversion`` for all code in your project, but not for
code coming from any external dependencies also built with Bazel.

.. _module-pw_toolchain-bazel-running-tools:

-----------------------
Running toolchain tools
-----------------------
Pigweed provides a set of runnable targets that expose the active toolchain's
tools for interactive use. These are essential for debugging and inspecting
build artifacts, as local system tools often lack support for the target
architecture of a cross-compiled build. Furthermore, since Pigweed's compilers
are typically hermetic and managed by Bazel, their binaries are stored within
Bazel's internal directories and are not easily accessible through the shell.

Available tools include ``cc``, ``c++``, ``ld``, ``ar``, ``objdump``, ``nm``,
``readelf``, ``size``, ``strip``, and ``cov``.

These targets are dynamically resolved from the active toolchain's action
mapping. This ensures that when you run a tool via these targets, you are using
the *exact same* binary and configuration that Bazel uses during the build
for that specific target platform.

.. note::

   Tool availability varies by suite (e.g., LLVM vs. GCC vs. Zephyr) and
   configuration. While common tools like ``objdump`` and ``nm`` are present
   across all supported suites (including ARM GCC and Zephyr), others like
   ``cov`` or ``gcov`` may be missing. Attempting to run a missing tool
   will result in a "tool not found" error or fail with a clear error
   message during analysis or execution.

   Additionally, compiler drivers that are symlinks to a multicall binary
   (like ``llvm``) may behave as a generic driver if invoked without
   arguments.

.. code-block:: console

   # Disassemble a binary using the objdump provided by the toolchain
   $ bazel run //pw_toolchain/cc/current_toolchain:objdump -- -d bazel-bin/my_binary

   # Link object files manually (for debugging)
   $ bazel run //pw_toolchain/cc/current_toolchain:ld -- ...

   # List symbols
   $ bazel run //pw_toolchain/cc/current_toolchain:nm -- bazel-bin/my_binary

.. admonition:: Warning
   :class: warning

   These targets are intended **exclusively** for interactive use via ``bazel run``.
   **Do NOT** use them as dependencies in other rules (e.g. in ``srcs``,
   ``tools``, or ``deps``).

   Doing so will likely result in the wrong tool being selected (e.g. the host
   ``objdump`` instead of the target ``objdump``) due to Bazel's configuration
   transition logic.

.. _module-pw_toolchain-bazel-compiler-specific-logic:

-----------------------------
Compiler-specific build logic
-----------------------------
Whenever possible, avoid introducing compiler-specific behaviors in Bazel
``BUILD`` files. Instead, prefer to design build logic against
more intentional :ref:`docs-bazel-compatibility`. For compiler-specific
behavior, this means defining and/or using compiler capabilities like
`@rules_cc//cc/toolchains/capabilities:supports_interface_shared_libraries <https://github.com/bazelbuild/rules_cc/blob/main/cc/toolchains/capabilities/BUILD>`__

If you need to expose a toolchain capability as a choice in a select, you
can use ``pw_cc_toolchain_feature_is_enabled``.

Example:

.. code-block:: py

   load(
       "@pigweed//pw_toolchain/cc/current_toolchain:pw_cc_toolchain_feature_is_enabled.bzl",
       "pw_cc_toolchain_feature_is_enabled",
   )

   pw_cc_toolchain_feature_is_enabled(
       name = "llvm_libc_enabled",
       feature_name = "llvm_libc",
   )

   cc_library(
       name = "libfoo",
       deps = select({
           ":llvm_libc_enabled": ["//foo:llvm_libc_extras"],
           "//conditions:default": [],
       }),
   )

If you absolutely must introduce a ``select`` statement that checks the current
compiler, use Pigweed's helper macros.

Example:

.. code-block:: py

   load(
       "@pigweed//pw_toolchain/cc/current_toolchain:conditions.bzl",
       "if_compiler_is_clang",
       "if_linker_is_gcc",
   )

   cc_library(
       copts = if_compiler_is_clang(
           ["-fno-codegen"],
           otherwise = [],
       ),
       linkopts = if_linker_is_gcc(
           ["-Wl,--delete-main"],
           otherwise = [],
       ),
       srcs = ["lib.cc"],
   )

---------------------------------
Compiler-specific toolchain flags
---------------------------------
In cases where foundationally different toolchains (e.g. Clang, GCC, MSVC) share
large pieces of project-wide configuration, you may want to conditionally add
flags that are compiler-specific. This can be done with the following steps:

#. **Express the type of compiler in your cc_toolchain.** This is how the
   rest of your toolchain rules will know what kind of compiler is active.

   .. code-block:: py

      cc_toolchain(
          name = "arm_clang_toolchain_cortex-a",
          # ...
          enabled_features = [
              "@pigweed//pw_toolchain/cc/capability:compiler_is_clang",
              "@pigweed//pw_toolchain/cc/capability:linker_is_clang",
          ],
      )

#. **Add the list of known toolchain types to your toolchain.** This ensures
   that the ``cc_toolchain`` passes feature correctness validations.

   .. code-block:: py

      cc_toolchain(
          name = "arm_clang_toolchain_cortex-a",
          # ...
          enabled_features = [
              "@pigweed//pw_toolchain/cc/capability:compiler_is_clang",
              "@pigweed//pw_toolchain/cc/capability:linker_is_clang",
          ],
          known_features = [
              "//pw_toolchain/cc/capability:known_toolchain_types",
          ],
      )

#. **Gate the arguments with requires_any_of.** ``cc_args`` gated by
   a ``requires_any_of`` constraint on a toolchain type will only be expanded
   in the compiler/linker invocation if the toolchain tool matches the required
   type. Keep in mind that ``compiler_is_clang`` and ``linker_is_clang`` types
   are offered separately to support cases where the compiler and linker types
   are not the same.

   .. code-block:: py

      cc_args(
          name = "clang_only_extra_pigweed_warnings",
          actions = [
              "@rules_cc//cc/toolchains/actions:compile_actions",
          ],
          requires_any_of = [
              "//pw_toolchain/cc/capability:compiler_is_clang",
          ],
          args = [
              "-Wshadow-all",
          ],
      )
