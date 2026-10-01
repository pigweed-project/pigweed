.. _toolchain-get-started:

===========
Get started
===========
The Pigweed Toolchain is downloaded hermetically by the build system. You don't
need to install anything system-wide or modify your host ``PATH``.

.. _toolchain-get-started-bazel:

-----
Bazel
-----
#. Add Pigweed to your ``MODULE.bazel``. Pigweed isn't published to the Bazel
   Central Registry yet, so a ``git_override`` is required. See
   :ref:`docs-bazel-integration-add-pigweed-as-a-dependency` for the full
   instructions and the required ``.bazelrc`` flags.

   .. code-block:: py

      bazel_dep(name = "pigweed")

      git_override(
          module_name = "pigweed",
          commit = "c00e9e430addee0c8add16c32eb6d8ab94189b9e",
          remote = "https://pigweed.googlesource.com/pigweed/pigweed.git",
      )

#. Register the Clang toolchains that you need. Registering only the
   toolchains you use avoids Pigweed's device toolchains getting selected for
   your device builds:

   .. code-block:: py

      register_toolchains(
          "@pigweed//pw_toolchain/arm_clang:arm_clang_cc_toolchain_cortex-m0",
          "@pigweed//pw_toolchain/arm_clang:arm_clang_cc_toolchain_cortex-m33",
          "@pigweed//pw_toolchain/riscv_clang:riscv_clang_cc_toolchain_rv32imc",
          "@pigweed//pw_toolchain/host_clang:host_cc_toolchain_linux",
          "@pigweed//pw_toolchain/host_clang:host_cc_toolchain_macos",
          dev_dependency = True,
      )

   See :ref:`module-pw_toolchain-bazel-upstream-pigweed-toolchains` for the
   complete list of toolchains and for guidance on defining your own.

#. Build for your target platform. Toolchain selection is driven by the
   platform's constraints, so there's no toolchain flag to set:

   .. code-block:: console

      $ bazelisk build --platforms=//platforms:my_device //src:firmware

.. admonition:: Migrating an existing Cortex-M project from GCC?
   :class: tip

   Pigweed also provides ``@pigweed//pw_toolchain:cc_toolchain_cortex-m*``
   toolchains that switch between GCC and Clang with the
   ``--@pigweed//pw_toolchain:cortex-m_toolchain_kind`` flag (default:
   ``gcc``). This is useful for A/B comparisons during a migration, but it's
   an explicitly interim mechanism that's likely to change; prefer registering
   the ``arm_clang`` toolchains directly for production builds.

.. _toolchain-get-started-bazel-tools:

Run the toolchain's tools
=========================
The ``@llvm_toolchain`` repository provides the individual binaries (for
example ``@llvm_toolchain//:bin/clangd``), but prefer the runnable targets in
``//pw_toolchain/cc/current_toolchain``. They resolve to the correct tool for
whichever toolchain is active, so the same command works for LLVM, Arm GCC,
and Zephyr builds:

.. code-block:: console

   $ bazelisk build --config=rp2350 //pw_status:status_test
   $ bazelisk run --config=rp2350 //pw_toolchain/cc/current_toolchain:objdump -- -d "$PWD/bazel-bin/pw_status/status_test"
   $ bazelisk run --config=rp2350 //pw_toolchain/cc/current_toolchain:size -- "$PWD/bazel-bin/pw_status/status_test"

``objdump``, ``nm``, ``size``, ``readelf``, ``strip``, ``ar``, ``ld``, and
``cov`` are available. Build the target first, then pass absolute paths,
because the tools run from the Bazel execution root rather than your working
directory. Build the tool with the same platform as the artifact you're
inspecting, otherwise you may get a host tool that can't read your binary
(``--config=rp2350`` is an upstream Pigweed shorthand for a platform; use your
project's own flags). Use these targets interactively only; don't add them to
the ``srcs`` or ``deps`` of other rules.

For Clang-Tidy and the other static analysis integrations, see
:ref:`docs-automated-analysis`.

.. _toolchain-get-started-gn:

--
GN
--
For GN and Ninja projects, the toolchain is distributed as a CIPD package that
Pigweed's environment setup downloads for you.

#. Bootstrap the environment once per checkout. This downloads the toolchain
   packages and activates them for the current shell:

   .. tab-set::

      .. tab-item:: Linux and macOS
         :sync: linux

         .. code-block:: console

            $ . ./bootstrap.sh

      .. tab-item:: Windows
         :sync: windows

         .. code-block:: doscon

            > bootstrap.bat

#. In later shell sessions, activate the environment instead of
   bootstrapping. Activation is much faster because nothing is downloaded:

   .. tab-set::

      .. tab-item:: Linux and macOS
         :sync: linux

         .. code-block:: console

            $ . ./activate.sh

      .. tab-item:: Windows
         :sync: windows

         .. code-block:: doscon

            > activate.bat

#. Define a toolchain for your target from one of the ``pw_toolchain_arm_clang``
   scopes and generate it. Downstream targets typically layer their own
   defaults on top of the Pigweed scope:

   .. code-block::

      import("$dir_pw_toolchain/arm_clang/toolchains.gni")
      import("$dir_pw_toolchain/generate_toolchain.gni")

      my_target_toolchains = [
        {
          name = "my_target_size_optimized"
          _toolchain_base = pw_toolchain_arm_clang.cortex_m4f_size_optimized
          forward_variables_from(_toolchain_base, "*", [ "name" ])
        },
      ]

      generate_toolchains("target_toolchains") {
        toolchains = my_target_toolchains
      }

   :cs:`targets/mimxrt595_evk/target_toolchains.gni` is a complete worked
   example of this pattern. See :ref:`module-pw_toolchain-gn` for details.

.. note::

   The GN Arm Clang toolchains don't ship a C standard library. They locate an
   ``arm-none-eabi-gcc`` installation on the ``PATH`` and borrow its headers
   and runtime libraries; see :cs:`pw_toolchain/arm_clang/clang_config.gni`.
   The Bazel Arm Clang toolchains use LLVM libc instead and need no GCC
   installation.

.. _toolchain-get-started-cmake:

-----
CMake
-----
CMake support is limited. Pigweed provides
:cs:`pw_toolchain/arm_clang/clang_flags.cmake`, which derives Clang flags the
same way the GN toolchains do (including borrowing runtime libraries from
``arm-none-eabi-gcc``), but there's no supported CMake toolchain file. If
CMake support matters to you, let us know (:ref:`toolchain-support`).

.. _toolchain-get-started-direct:

---------------
Direct download
---------------
The toolchain binaries can also be downloaded directly from `CIPD
<https://chrome-infra-packages.appspot.com/p/fuchsia/third_party/clang>`_ and
used with any build system. This path is unsupported: you're responsible for
sysroots, runtime libraries, and flags that the Pigweed toolchain definitions
would otherwise provide.
