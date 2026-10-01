.. _toolchain-why:
.. _toolchain-background:

===========================
Why Clang/LLVM for embedded
===========================
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

Pigweed Toolchain tracks tip-of-tree LLVM. Rolling the toolchain on a regular
cadence amortizes upgrade overhead over time, accelerates access to new
compiler features, and provides immediate feedback to upstream LLVM developers
(see :ref:`toolchain-versioning`).

--------------------------------------------------
Unified cross-compilation across targets and hosts
--------------------------------------------------
LLVM is a modular cross-compiler, meaning a single installation can target
multiple architectures. Systems combining different microcontroller
architectures (such as Arm Cortex-M and RISC-V) can be targeted using the same
compiler binary and matching standard library versions. LLVM also supports a
variety of host platforms, so embedded developers running Linux, macOS, and
Windows can use the same compiler and configuration, eliminating
platform-dependent build variations.

-------------------------------------
Modern tooling and language standards
-------------------------------------
Adopting Clang brings modern software development tooling and language
standards (including C++20 and C++23) to baremetal environments. Integration
with Clang-Tidy, the Clang Static Analyzer, and ``clangd`` language server
protocol (LSP) servers provides in-editor indexing, auto-completion, and
automated refactoring. See :ref:`docs-automated-analysis` for how Pigweed
wires these up.

------------------------------------------
Modular and permissively licensed runtimes
------------------------------------------
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

-----------------------------------------
Binary size and optimization capabilities
-----------------------------------------
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

--------------------------------
Safety, sanitizers, and coverage
--------------------------------
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
