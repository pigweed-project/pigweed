.. _toolchain:

=================
Pigweed Toolchain
=================
Pigweed Toolchain is an LLVM/Clang-based toolchain distribution tailored for
embedded and baremetal software development. Rather than relying on
fragmented, vendor-specific GNU toolchain forks, Pigweed provides a unified
LLVM distribution that cross-compiles for many microcontroller architectures
from Linux, macOS, and Windows hosts.

The distribution is built from upstream LLVM and shared with other large
systems projects: the same CIPD packages that Pigweed consumes are the Clang
packages produced and used by the Fuchsia project. All of upstream Pigweed is
built and tested with it continuously.

.. grid:: 2

   .. grid-item-card:: :octicon:`rocket` Get started
      :link: toolchain-get-started
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Add the toolchain to a Bazel or GN project.

   .. grid-item-card:: :octicon:`checklist` Supported targets
      :link: toolchain-targets
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Check whether your architecture and host platform are supported.

.. _toolchain-targets:

---------------------------
Supported targets and hosts
---------------------------
A single toolchain installation targets all of the architectures below. The
table lists the Clang toolchains that Pigweed provides out of the box. Custom
toolchains for other architectures and CPUs can be assembled from the same
distribution.

.. list-table::
   :header-rows: 1

   * - Target
     - Bazel toolchain
     - GN toolchain scope
   * - Arm Cortex-M0, M0+ (Armv6-M)
     - ``arm_clang_cc_toolchain_cortex-m0``,
       ``arm_clang_cc_toolchain_cortex-m0plus``
     - ``pw_toolchain_arm_clang.cortex_m0plus_*``
   * - Arm Cortex-M3 (Armv7-M)
     - ``arm_clang_cc_toolchain_cortex-m3``
     - ``pw_toolchain_arm_clang.cortex_m3_*``
   * - Arm Cortex-M4, M4F (Armv7E-M)
     - ``arm_clang_cc_toolchain_cortex-m4``
     - ``pw_toolchain_arm_clang.cortex_m4_*``,
       ``pw_toolchain_arm_clang.cortex_m4f_*``
   * - Arm Cortex-M7, M7F (Armv7E-M)
     - Not provided
     - ``pw_toolchain_arm_clang.cortex_m7_*``,
       ``pw_toolchain_arm_clang.cortex_m7f_*``
   * - Arm Cortex-M33, M33F (Armv8-M)
     - ``arm_clang_cc_toolchain_cortex-m33``
     - ``pw_toolchain_arm_clang.cortex_m33_*``,
       ``pw_toolchain_arm_clang.cortex_m33f_*``
   * - Arm Cortex-M55, M55F (Armv8.1-M)
     - ``arm_clang_cc_toolchain_cortex-m55``
     - ``pw_toolchain_arm_clang.cortex_m55_*``,
       ``pw_toolchain_arm_clang.cortex_m55f_*``
   * - Arm Cortex-A35 (AArch64)
     - ``arm_clang_cc_toolchain_cortex-a35``
     - Not provided
   * - RISC-V 32-bit (``rv32imc``, ``rv32imac``)
     - ``riscv_clang_cc_toolchain_rv32imc``,
       ``riscv_clang_cc_toolchain_rv32imac``
     - Not provided
   * - Host (Linux, macOS)
     - ``host_cc_toolchain_linux``, ``host_cc_toolchain_macos``
     - ``pw_toolchain_host_clang.*``

Bazel targets live in :cs:`pw_toolchain/arm_clang/BUILD.bazel`,
:cs:`pw_toolchain/riscv_clang/BUILD.bazel`, and
:cs:`pw_toolchain/host_clang/BUILD.bazel`. GN scopes live in
:cs:`pw_toolchain/arm_clang/toolchains.gni` and
:cs:`pw_toolchain/host_clang/toolchains.gni`. Each GN scope is available in
``debug``, ``size_optimized``, and ``speed_optimized`` variants.

Supported development hosts:

.. |br| raw:: html

   <br />

.. list-table::
   :header-rows: 1

   * - Host
     - Notes
   * - Linux (x86-64, arm64) |br| macOS (x86-64, arm64)
     - Fully supported.
   * - Windows (x86-64)
     - Contains some limitations around user-side licensing. |br| Pigweed
       doesn't provide a Bazel host toolchain for Windows.

.. _toolchain-targets-stability:

Stability
=========
.. warning::

   Pigweed's upstream toolchain definitions are subject to change without
   notice. Toolchain target names, flags, and defaults may be renamed or
   removed between Pigweed revisions. Pin the Pigweed revision that you depend
   on (see :ref:`toolchain-versioning`) and expect to make small updates when
   you roll it. If you need stronger guarantees, talk to us
   (:ref:`toolchain-support`).

.. _toolchain-why:

---------------------------
Why Clang/LLVM for embedded
---------------------------
Embedded projects have historically depended on vendor-supplied GNU
toolchains. These toolchains are often out-of-tree forks of GCC and Binutils
which are updated infrequently and rely on non-portable compiler extensions.
For systems integrating heterogeneous cores, such as Arm Cortex-M and RISC-V
cores, teams are frequently forced to maintain disparate toolchains with
differing host requirements and option flags.

Pigweed co-evolves embedded toolchain support within the LLVM project. Instead
of maintaining downstream forks, we contribute all improvements directly to
LLVM. This prevents toolchain divergence and shares maintenance across the
broader compiler community.

Unified cross-compilation across targets and hosts
==================================================
LLVM is a modular cross-compiler, meaning a single installation can target
multiple architectures. Systems combining different microcontroller
architectures (such as Arm Cortex-M and RISC-V) can be targeted using the same
compiler binary and matching standard library versions. LLVM also supports a
variety of host platforms, so embedded developers running Linux, macOS, and
Windows can use the same compiler and configuration, eliminating
platform-dependent build variations.

Modern tooling and language standards
=====================================
Adopting Clang brings modern software development tooling and language
standards (including C++20 and C++23) to baremetal environments. Integration
with Clang-Tidy, the Clang Static Analyzer, and ``clangd`` language server
protocol (LSP) servers provides in-editor indexing, auto-completion, and
automated refactoring. See :ref:`docs-automated-analysis` for how Pigweed
wires these up.

Modular and permissively licensed runtimes
==========================================
A production embedded toolchain requires complete runtime libraries that
govern binary size and execution behavior. Pigweed integrates LLVM runtime
components released under the Apache 2.0 license with LLVM Exceptions:

* **compiler-rt**: Replaces ``libgcc`` with target-specific builtins,
  low-level integer and floating-point operations, and architectural
  primitives.
* **LLVM libc**: A modular, permissively licensed C standard library
  structured to scale down to resource-constrained microcontrollers. It
  includes baremetal memory allocators, core math routines, and a lightweight
  embedding platform API to connect standard I/O and process primitives
  directly to hardware abstraction layers.
* **LLVM libc++**: A configurable C++ standard library. It supports disabling
  unsupported or expensive features in restricted systems, including dynamic
  heap allocations, exceptions, runtime type information (RTTI), thread-local
  storage (TLS), and localization tables.

Binary size and optimization capabilities
=========================================
Embedded systems are constrained by strict SRAM and flash size. Clang and the
LLD linker provide several features to reduce the footprint of embedded
applications:

* **Link-Time Optimization (LTO)**: Whole-program optimization enables
  dead-code elimination and inter-procedural inlining across translation
  units. FatLTO packages bitcode and native object code into a single file to
  simplify distribution.
* **Identical Code Folding (ICF) and garbage collection (GC)**: LLD's
  ``--gc-sections`` removes unreferenced symbols, while ``--icf=all`` merges
  identical read-only functions and template instantiations to reduce flash
  usage.
* **Profile-Guided Optimization (PGO)**: Developers can capture execution
  profiles and feed them back into the compiler to guide inlining and
  placement decisions.
* **Machine Learning Guided Optimization (MLGO)**: Integration with the MLGO
  framework leverages trained machine learning models to improve code-size
  inlining decisions beyond traditional heuristics.

Use :ref:`module-pw_bloat` to measure the effect of these options on your own
binaries.

Safety, sanitizers, and coverage
================================
LLVM enables modern testing and verification workflows directly on embedded
hardware:

* **Compile-time safety**: Compile-time thread and lifetime safety annotations
  allow the compiler to enforce locking discipline and guard shared resources
  statically with zero memory or cycle overhead.
* **Sanitizers**: Sanitizers in trapping mode or paired with a minimal
  embedded runtime can detect issues such as unaligned memory access,
  arithmetic overflow, and out-of-bounds memory accesses.
* **Source-based code coverage**: Compiler-assisted code coverage using
  single-byte counters can be used to track test exhaustiveness on physical
  hardware with minimal instrumentation overhead.

.. _toolchain-get-started:

-----------
Get started
-----------
The toolchain is downloaded hermetically by the build; you don't install
anything system-wide and you don't modify your ``PATH``.

.. _toolchain-get-started-bazel:

Bazel
=====
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
-------------------------
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

GN
==
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

CMake
=====
CMake support is limited. Pigweed provides
:cs:`pw_toolchain/arm_clang/clang_flags.cmake`, which derives Clang flags the
same way the GN toolchains do (including borrowing runtime libraries from
``arm-none-eabi-gcc``), but there's no supported CMake toolchain file. If
CMake support matters to you, let us know (:ref:`toolchain-support`).

.. _toolchain-get-started-direct:

Direct download
===============
The toolchain binaries can also be downloaded directly from `CIPD
<https://chrome-infra-packages.appspot.com/p/fuchsia/third_party/clang>`_ and
used with any build system. This path is unsupported: you're responsible for
sysroots, runtime libraries, and flags that the Pigweed toolchain definitions
would otherwise provide.

.. _toolchain-versioning:

--------------------
Versioning and rolls
--------------------
Pigweed Toolchain tracks tip-of-tree LLVM. Rolling the toolchain on a regular
cadence amortizes upgrade overhead over time, accelerates access to new
compiler features, and provides immediate feedback to upstream LLVM
developers. Pigweed's infrastructure builds new toolchain packages
continuously and an automated roller updates the pinned revision in Pigweed
after it passes CI.

How the version is pinned:

.. list-table::
   :header-rows: 1

   * - Build system
     - Where the version is pinned
   * - Bazel
     - The ``LLVM_VERSION`` CIPD tag (a ``git_revision:`` of ``llvm-project``)
       in Pigweed's :cs:`MODULE.bazel`, consumed by the ``pw_cxx_toolchain``
       module extension.
   * - GN and CMake
     - The ``fuchsia/third_party/clang`` entry in your project's CIPD
       manifest, for example
       :cs:`pw_env_setup/py/pw_env_setup/cipd_setup/pigweed.json`.

What this means for you:

* **Your toolchain version is pinned by the Pigweed revision you depend on.**
  Pinning the ``git_override`` commit (Bazel) or the CIPD tag (GN) gives you a
  byte-identical toolchain on every machine and every CI run.
* **You choose when to roll.** Updating the toolchain is a deliberate act of
  updating that pin. Toolchain changes land with the Pigweed changes needed to
  stay compatible with them, so rolling Pigweed and rolling the toolchain
  together is the supported path.
* **Rolling frequently is cheaper than rolling rarely.** Tip-of-tree LLVM
  moves quickly; the longer you wait, the more diagnostics and behavior
  changes you absorb at once.

.. _toolchain-migration:

----------------------------
Migrate from a GNU toolchain
----------------------------
When migrating an existing project from a GNU toolchain
(GCC, Binutils, newlib, libstdc++, libgcc) to LLVM (Clang, LLD, LLVM libc,
libc++, compiler-rt), adopt an incremental, three-stage approach and keep the
project building and testable at every stage:

#. **Make it compile.** Switch the compiler invocation to Clang while
   retaining the existing linker and runtime libraries. Resolve diagnostics
   and non-portable GNU extensions.
#. **Make it fit.** Switch the link step to LLD, then swap the runtime
   libraries. Audit linker scripts and get back under your size budget.
#. **Make it run.** Validate on hardware. ABI and alignment problems surface
   here, not at build time.

The rest of this section covers the problems that projects hit most often at
each stage.

.. _toolchain-migration-rollout:

Plan the rollout
================
* **Migrate incrementally.** Don't convert the whole codebase at once. Add
  parallel build targets (for example ``target_clang`` alongside
  ``target_gcc``) so you can work through compilation errors without breaking
  the production build.
* **Split the flags per image.** For projects that build several images, such
  as a bootloader and an application, use separate options for each (for
  example ``build_bootloader_with_clang`` and ``build_prod_app_with_clang``).
  This limits the blast radius and makes runtime failures such as a bricked
  device much easier to isolate.
* **Keep generated build files toolchain-specific.** If your build generates
  files into the source tree, such as extracting compiler flags from CMake
  into GN, give each toolchain its own output directory. Otherwise the GCC and
  Clang builds overwrite each other's flags and fail in confusing ways.
* **Update host-side tooling too.** Size reports, coredump parsers, and
  flashing scripts must move to the LLVM binary utilities (``llvm-size``,
  ``llvm-nm``, ``llvm-objcopy``). ``llvm-objcopy`` is stricter than GNU
  ``objcopy``: removing a section that's still referenced by another section's
  ``sh_link``, such as ``rom_start`` referenced by ``.ARM.exidx``, fails
  unless you pass ``--allow-broken-links``.

.. _toolchain-migration-diagnostics:

Fix diagnostics and non-portable extensions
===========================================
Clang conforms more closely to the C and C++ standards and warns about more
constructs than GCC, so expect a batch of ``-Werror`` failures first:

* **Unused code and shadowing**: Clang flags unused variables, unused private
  members, unused functions, and variable shadowing. Delete the dead code or
  mark deliberate cases with ``[[maybe_unused]]``.
* **Invalid** ``constexpr``: Clang rejects standard violations that GCC
  sometimes accepts, such as ``reinterpret_cast`` in a ``constexpr``
  declaration. Change these to ``const`` or ``inline``.
* **Naked functions**: Clang rejects ``__attribute__((naked))`` functions that
  contain anything other than assembly, including compiler-generated prologue
  and epilogue code. Write them as pure inline assembly, or use a normal
  function with an ``asm`` block.
* ``printf`` **format specifiers**: GCC and Clang may use different underlying
  types for ``uint32_t`` and friends (``unsigned int`` versus ``unsigned
  long`` on 32-bit targets), which produces format warnings. Cast explicitly
  at the call site to match the specifier.
* **Function-level optimization attributes**: ``__attribute__((optimize(...)))``
  isn't supported. Use per-file or per-target compiler options instead.

.. _toolchain-migration-abi:

Match the ABI
=============
ABI mismatches don't fail the link. They corrupt data at runtime, so check
them deliberately:

* **Short enums**: GCC defaults to ``-fshort-enums`` on Arm targets while
  Clang uses 32-bit enums. Mixing the two changes struct layout and produces
  failures such as corrupt OTA updates. Pass ``-fshort-enums`` to Clang
  explicitly to stay compatible with GCC-built code.
* **Alignment**: Clang may emit instructions that require strict alignment,
  such as ``LDRD`` and ``STRD`` on Arm, where GCC emitted alignment-safe
  sequences. Unsafe pointer casts that worked under GCC can hard fault. Audit
  the casts and give the affected structs and buffers explicit alignment with
  ``alignas``.
* **Atomics**: Atomic operations on under-aligned types lower to compiler-rt
  library calls instead of native instructions. Align atomic variables
  naturally; :ref:`module-pw_alignment` provides ``pw::AlignedAtomic`` for
  this.
* **Prebuilt vendor libraries**: Linking Clang-built code against archives
  built by ``arm-none-eabi-gcc`` generally works, but only if the
  ABI-affecting options match. ``-fshort-enums``, the floating-point ABI, and
  packing attributes are the usual culprits.

UBSan's minimal embedded runtime is an effective way to catch the misaligned
accesses and undefined behavior that these mismatches cause.

.. _toolchain-migration-linker:

Update linker scripts for LLD
=============================
LLD is stricter than GNU ``ld`` about both syntax and memory layout:

* **Unsupported syntax**: Some GNU-specific constructs aren't accepted, such
  as ``KEEP(*libgcc.a:save-restore.o)`` or the ``i`` attribute in a memory
  region definition. Replace them with standard patterns or remove them.
* **Section names with spaces**: LLD may quote them in the output ELF, which
  breaks downstream post-processing tools. Rename the sections.
* **Overlapping load addresses**: LLD validates LMAs strictly and fails the
  link if two sections overlap in physical memory, for example a
  ``.sdk_version`` section overlapping ``.bss``. Sequence the LMAs explicitly
  so each section starts after the previous one ends.
* **Global constructor symbols**: Clang and GCC emit slightly different
  constructor symbol names. Match both with a ``_GLOBAL__sub_I_*`` pattern, or
  constructors can land in RAM instead of flash.

.. _toolchain-migration-runtimes:

Swap the runtime libraries
==========================
Moving from libstdc++ and newlib to libc++, LLVM libc, and compiler-rt is
usually the longest stage:

* **Iterators aren't pointers**: Under libc++, ``std::begin()`` and
  ``std::end()`` return iterator objects that may not be raw pointers. Where a
  pointer is required, such as a ``pw::span`` constructor, use ``.data()``.
* **Fewer transitive includes**: LLVM's headers are more modular, so code that
  compiled under GCC by accident now needs its own includes, for example
  ``<cmath>`` for ``std::abs`` and ``std::round``.
* **No POSIX headers**: LLVM libc's embedded profiles intentionally omit
  ``<unistd.h>``, ``<fcntl.h>``, ``<sys/*.h>``, and POSIX types. Replace
  them with standard types, for example ``ssize_t`` with ``ptrdiff_t`` and
  ``uint`` with ``unsigned``.
* **Third-party libraries assume a hosted environment**: Libraries such as
  mbedTLS and Nanopb reference ``malloc``, or ``printf``. Use their
  configuration macros to disable or redirect dynamic memory and I/O, and add
  stubs to :ref:`module-pw_libc` only when there's no alternative.
* **libc++ needs a few baremetal stubs**: libc++ references symbols such as
  ``_LIBCPP_VERBOSE_ABORT`` and ``operator delete`` for global destructors.
  Provide overrides that map them to a trap or an assert.
* **compiler-rt builtins**: Make sure the build compiles the builtins for your
  architecture, such as the Armv6-M and Armv7-M sources in
  :cs:`third_party/llvm_builtins`, or operations like 64-bit division won't
  link.
* **Duplicate symbols from vendor blobs**: Precompiled vendor libraries often
  bundle their own runtime helpers, which collide with compiler-rt. Exclude
  the conflicting objects from the compiler-rt build.
* ``libnosys`` **conflicts**: LLVM toolchains may link stubs such as
  ``libnosys`` by default, which collides with custom syscall overrides. Make
  it optional or exclude it in your toolchain configuration.

.. _toolchain-migration-size:

Get back under the size budget
==============================
Clang's optimization decisions differ from GCC's, and the first Clang build is
often larger:

* **Inlining**: Even at ``-Oz``, Clang may inline large assembly-heavy
  functions, for example in crypto libraries such as micro-ecc. Mark the
  offenders ``__attribute__((noinline))``.
* **LTO**: Archives built with GCC's LTO can't be linked by LLD, and without
  LTO unused code may not be stripped. Rebuild third-party libraries with
  Clang and enable FatLTO so dead-code elimination works across the whole
  program.
* ``KEEP`` **directives**: Overuse of ``KEEP``, for example on ``.ram_code``
  or vendor sections, prevents the linker from discarding unused code. Remove
  it from everything that isn't strictly required to boot.
* **Garbage collection and folding**: Enable ``--gc-sections`` and
  ``--icf=all``.
* **Very small images**: In a severely constrained partition, such as a 40 KB
  bootloader, you may need to disable subsystems that fit under GCC. Turning
  off logging and console I/O (for example ``CONFIG_LOG``, ``CONFIG_PRINTK``,
  ``CONFIG_CONSOLE``, ``CONFIG_SERIAL`` in Zephyr) removes formatting code such
  as ``vfprintf`` and typically saves 5–10 KB of flash.

Use :ref:`module-pw_bloat` to track size across each of these changes.

.. _toolchain-gaps:

---------------------------
Known gaps and ongoing work
---------------------------
Migrating embedded projects from GNU toolchains to LLVM involves several areas
of ongoing active work across the ecosystem:

* **Linker script semantics**: Embedded applications rely on linker scripts to
  place code and data into distinct SRAM and flash memory regions. Differences
  between GNU LD and LLD, particularly regarding memory region allocation and
  non-contiguous section packing, require careful script evaluation.
* **LTO with heterogeneous memory layouts**: Placing symbols into designated
  memory sections using ``section`` attributes can interact unexpectedly with
  whole-program optimization passes. Developing higher-level memory placement
  representations for LTO remains an area of ongoing discussion.
* **Stack frame sizing**: Inlining heuristics can change stack frame usage.
  Because embedded tasks typically operate with fixed stack allocations, stack
  usage should be monitored when changing optimization levels. `Call Graph
  Information
  <https://discourse.llvm.org/t/rfc-call-graph-information-from-clang-llvm-for-c-c/88255>`_
  that is under development in Clang/LLVM can be used to analyze stack usage.
* **Multilib support**: Selecting the correct runtime library variant based on
  target architecture flags (like hardware floating-point units or specific
  instruction extensions) relies on multilib configurations. Standardizing
  multilib selection mechanisms in Clang and ensuring complete coverage across
  microcontroller variants is an active area of development.
* **Runtime libraries for specialized targets**: While Pigweed ships runtime
  libraries for standard Arm Cortex-M and RISC-V cores, projects with
  specialized architectures or vendor-specific instruction extensions may
  require building custom versions of runtime libraries like compiler-rt, LLVM
  libc, and libc++.

.. _toolchain-community:

-------------------
Talks and community
-------------------
Pigweed and LLVM developers participate in industry-wide efforts to advance
baremetal support in upstream repositories:

* **LLVM Embedded Toolchains Working Group**: A public working group that
  meets every four weeks to discuss embedded compiler features, runtime
  designs, and linker enhancements.
* **LLVM Embedded Toolchains Workshop**: An annual workshop held in
  conjunction with the LLVM Developers' Meeting covering baremetal runtime
  implementations and migration workflows.
* **GitHub embedded tracking**: Baremetal issues and pull requests are tracked
  in the upstream LLVM repository using the |embedded_label|_.

.. |embedded_label| replace::  ``embedded`` label
.. _embedded_label: https://github.com/llvm/llvm-project/issues?q=label%3Aembedded

The following presentations from LLVM Developers' Meetings offer detailed
technical discussions on embedded development with LLVM:

.. raw:: html

   <iframe width="560" height="315"
       src="https://www.youtube.com/embed/CHbyo0Ux60o"
       title="Through the Compiler's Keyhole: Migrating to Clang Without Seeing the Source"
       frameborder="0"
       allow="accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture; web-share"
       referrerpolicy="strict-origin-when-cross-origin"
       allowfullscreen></iframe>

* `Through the Compiler's Keyhole: Migrating to Clang Without Seeing the
  Source (2025) <https://www.youtube.com/watch?v=CHbyo0Ux60o>`_: Presented by
  Petr Hosek at the 2025 US LLVM Developers' Meeting. Covers techniques for
  migrating vendor libraries and firmware to Clang, catching misaligned
  accesses with UBSan, using RPGO with optimization remarks, and validating
  DWARF CFI.

.. raw:: html

   <iframe width="560" height="315"
       src="https://www.youtube.com/embed/5hHQi-Uj34I"
       title="Modern Embedded Development with LLVM"
       frameborder="0"
       allow="accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture; web-share"
       referrerpolicy="strict-origin-when-cross-origin"
       allowfullscreen></iframe>

* `Modern Embedded Development with LLVM (2024)
  <https://www.youtube.com/watch?v=5hHQi-Uj34I>`_: Presented by Petr Hosek at
  the 2024 LLVM Developers' Meeting. Covers the deployment of modular LLVM
  runtimes (LLVM libc, libc++, compiler-rt) on microcontrollers, multi-target
  toolchains, and bringing sanitizers and code coverage to baremetal.

.. raw:: html

   <iframe width="560" height="315"
       src="https://www.youtube.com/embed/0HvgvBUPTyw"
       title="LLVM Toolchain for Embedded Systems"
       frameborder="0"
       allow="accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture; web-share"
       referrerpolicy="strict-origin-when-cross-origin"
       allowfullscreen></iframe>

* `LLVM Toolchain for Embedded Systems (2023)
  <https://www.youtube.com/watch?v=0HvgvBUPTyw>`_: Presented by Prabhu
  Karthikeyan Rajasekaran at the 2023 LLVM Developers' Meeting. Discusses
  motivations for transitioning embedded projects to Clang/LLD, linker script
  nuances, binary size considerations, and stack usage tracking.

.. _toolchain-support:

-------
Support
-------
`File a bug <https://pwbug.dev>`_ or talk to the Pigweed team on `Discord
<https://discord.com/invite/M9NSeTA>`_. Toolchain issues, migration questions,
and requests for architectures or host platforms that aren't listed in
:ref:`toolchain-targets` are all in scope.
