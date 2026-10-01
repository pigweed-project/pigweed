.. _toolchain-roadmap:

================
Status & roadmap
================
Pigweed Toolchain is actively developed and used in production across upstream
Pigweed and partner projects. This page summarizes what is ready for production
use today and what is on the roadmap across Pigweed and upstream LLVM.

------
Status
------
* **Compiler, linker, and binary utilities**: Clang, LLD, and the LLVM binary
  utilities (``llvm-objdump``, ``llvm-nm``, ``llvm-size``, ``llvm-readelf``,
  ``llvm-strip``, ``llvm-objcopy``, ``llvm-cov``) are production-ready and
  continuously built and tested against tip-of-tree LLVM on Linux, macOS, and
  Windows hosts.
* **Baremetal C and C++ runtimes**: Prebuilt ``compiler-rt``, ``LLVM libc``,
  and ``libc++`` multilibs ship with the toolchain for standard Arm Cortex-M,
  Arm Cortex-A, and 32-bit RISC-V targets (see :ref:`toolchain-targets`).
* **Build system support**:

  * **Bazel**: First-class hermetic toolchains using ``rules_cc`` and the
    packaged LLVM runtimes (``LLVM libc``, ``libc++``, ``compiler-rt``) with
    no external GCC dependency.
  * **GN**: Supported for host and Arm Cortex-M targets via CIPD; Arm Cortex-M
    GN toolchains currently borrow C standard library headers and archives from
    ``arm-none-eabi-gcc``.
  * **CMake**: Helper functions are provided in
    :cs:`pw_toolchain/arm_clang/clang_flags.cmake`, but full CMake toolchain
    files are not yet available.

* **Static and dynamic analysis**: ``clang-tidy``, ``clang-format``,
  ``clangd``, compile-time thread/lifetime safety annotations, trapping UBSan,
  and source-based code coverage are supported out of the box.

.. _toolchain-gaps:

------------------------
Roadmap and ongoing work
------------------------
Pigweed and upstream LLVM engineers are actively working on the following areas
to make baremetal LLVM adoption and GNU-to-LLVM migrations smoother:

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
* **Stack frame sizing**: Inlining heuristics can change stack frame usage.
  Because embedded tasks typically operate with fixed stack allocations, stack
  usage should be monitored when changing optimization levels. `Call Graph
  Information
  <https://discourse.llvm.org/t/rfc-call-graph-information-from-clang-llvm-for-c-c/88255>`_
  that is under development in Clang/LLVM can be used to analyze stack usage.
* **Linker script semantics**: Embedded applications rely on linker scripts to
  place code and data into distinct SRAM and flash memory regions. Differences
  between GNU LD and LLD, particularly regarding memory region allocation and
  non-contiguous section packing, require careful script evaluation.
* **LTO with heterogeneous memory layouts**: Placing symbols into designated
  memory sections using ``section`` attributes can interact unexpectedly with
  whole-program optimization passes. Developing higher-level memory placement
  representations for LTO remains an area of ongoing discussion.

If your project is blocked on a missing target, runtime feature, or linker
script construct, reach out to the Pigweed team (:ref:`toolchain-support`).
