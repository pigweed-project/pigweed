.. _module-pw_build-bazel_constraints:

===============================
Bazel compatibility constraints
===============================
.. pigweed-module-subpage::
   :name: pw_build

The ``pw_build`` module provides a limited selection of Bazel compatibility
constraints under ``@pigweed//pw_build/constraints/`` that are helpful for
targeting embedded platforms. These are guided by
:ref:`docs-bazel-compatibility`.

The selection of offered constraints are intentionally limited to reduce the
difficulty of properly configuring an embedded platform.

---
ARM
---
**Constraint Setting**: ``@pigweed//pw_build/constraints/arm:mcpu``

This ``constraint_setting`` corresponds to the ``-mcpu`` compiler flag for ARM
processors, and is the primary constraint used to guide
:ref:`module-pw_toolchain-bazel-upstream-pigweed-toolchains` for ARM platforms.

Additionally, ``@pigweed//pw_build/constraints/arm:lists.bzl`` provides the
``ALL_CORTEX_M_CPUS`` list for convenience.

-----
Board
-----
**Constraint Setting**: ``@pigweed//pw_build/constraints/board:board``

Deprecated. Do not use, and do not extend. If you see usages of these,
consider removing them.

-------
Chipset
-------
**Constraint Setting**: ``@pigweed//pw_build/constraints/chipset:chipset``

Deprecated. Do not use, and do not extend. If you see usages of these,
consider removing them.

------
RISC-V
------

Extensions
==========
**Constraint Settings**: ``@pigweed//pw_build/constraints/riscv/extensions:*``

These boolean constraints define the enabled instruction extensions of the
targeted RISC-V processor, which are the primary mechanism used to configure
:ref:`module-pw_toolchain-bazel-upstream-pigweed-toolchains` for RSIC-V
platforms.

----
RTOS
----
**Constraint Setting**: ``@pigweed//pw_build/constraints/rtos:rtos``

Identifies the target Real-Time Operating System. This is considered orthogonal
to the traditional ``@platforms//os`` constraint. This allows MCUs to have both
``@platforms//os:none`` and ``@pigweed//pw_build/constraints/rtos:freertos``
constraints.

.. _module-pw_build-bazel_constraints-rust:

----
Rust
----
**Constraint Setting**: ``@pigweed//pw_build/constraints/rust:std_enabled``

Indicates whether the Rust ``std`` library is enabled. This is primarily
used to control the available selection of
:ref:`module-pw_third_party_crates_io`.

**Constraint Setting**: ``@pigweed//pw_build/constraints/rust:enabled``

Indicates whether Rust support is enabled for the target platform.
