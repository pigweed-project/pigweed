.. _toolchain:

=================
Pigweed Toolchain
=================
Pigweed Toolchain is a Clang/LLVM-based toolchain distribution tailored for
embedded and baremetal software development. Rather than relying on
fragmented, vendor-specific GNU toolchain forks, Pigweed provides a unified
LLVM distribution that cross-compiles for many microcontroller architectures
from Linux, macOS, and Windows hosts.

The distribution is built from upstream LLVM and shared with other large
systems projects: the same CIPD packages that Pigweed consumes are the Clang
packages produced and used by the Fuchsia project. All of upstream Pigweed is
built and tested with it continuously.

--------
Benefits
--------
* **Unified cross-compilation**: Target Arm Cortex-M, Arm Cortex-A, RISC-V, and
  host builds from a single hermetic compiler installation across Linux, macOS,
  and Windows.
* **Modern C++ and tooling**: Use C++20/23 along with ``clangd``,
  ``clang-tidy``, and the Clang Static Analyzer out of the box.
* **Modular, permissively licensed runtimes**: Ships with ``compiler-rt``,
  ``LLVM libc``, and configurable ``libc++`` under Apache 2.0 with LLVM
  Exceptions.
* **Aggressive size optimization**: Link-Time Optimization (LTO/FatLTO),
  Identical Code Folding (``--icf=all``), Profile-Guided Optimization (PGO),
  and ML-Guided Optimization (MLGO) keep flash and SRAM footprints tight.
* **Hardware verification**: Compile-time thread and lifetime safety checks,
  trapping UBSan, and single-byte source-based code coverage run directly on
  baremetal targets.

---------------------------
Supported targets and hosts
---------------------------
Pigweed provides ready-to-use Clang toolchains for Arm Cortex-M (M0, M0+, M3,
M4, M7, M33, M55), Arm Cortex-A (A35), 32-bit RISC-V (``rv32imc``,
``rv32imac``), and host targets across Linux, macOS, and Windows development
machines. See :ref:`toolchain-targets` for the full support matrix and
build-system target names.

.. _toolchain-more:

----------
Learn more
----------
.. grid:: 2

   .. grid-item-card:: :octicon:`rocket` Get started
      :link: toolchain-get-started
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Hermetically integrate Pigweed Toolchain into a Bazel or GN project and
      run LLVM binary analysis tools.

   .. grid-item-card:: :octicon:`question` Why Clang/LLVM for embedded?
      :link: toolchain-why
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Explore the motivations behind co-evolving upstream LLVM, modular
      baremetal runtimes, LTO/MLGO size optimizations, and hardware sanitizers.

.. grid:: 2

   .. grid-item-card:: :octicon:`cpu` Supported targets & hosts
      :link: toolchain-targets
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Check out-of-the-box target architectures, Bazel and GN toolchain names,
      and supported development host platforms.

   .. grid-item-card:: :octicon:`checklist` Migrate from a GNU toolchain
      :link: toolchain-migration
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      A practical three-stage playbook for migrating an embedded codebase from
      GCC/newlib/Binutils to Clang/LLVM libc/LLD.

.. grid:: 2

   .. grid-item-card:: :octicon:`git-branch` Versioning, rolls, & stability
      :link: toolchain-versioning
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Understand how Pigweed pins tip-of-tree LLVM revisions in Bazel and CIPD,
      how rolls work, and what stability guarantees apply.

   .. grid-item-card:: :octicon:`milestone` Status & roadmap
      :link: toolchain-roadmap
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      See what is production-ready today and what is on the roadmap across
      Pigweed Toolchain and upstream LLVM.

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

.. toctree::
   :maxdepth: 2
   :hidden:

   get_started
   why
   targets
   migration
   versioning
   roadmap
