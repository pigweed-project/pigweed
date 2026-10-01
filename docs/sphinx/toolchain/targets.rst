.. _toolchain-targets:
.. _toolchain-prebuilts:

===========================
Supported targets and hosts
===========================
A single Pigweed Toolchain installation targets all of the architectures below.
The table lists the Clang toolchains that Pigweed provides out of the box.
Custom toolchains for other architectures and CPUs can be assembled from the
same distribution.

--------------------
Target architectures
--------------------
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

-----------------
Development hosts
-----------------
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
