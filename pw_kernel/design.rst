.. _module-pw_kernel-design:

======
Design
======
.. pigweed-module-subpage::
   :name: pw_kernel

This document outlines the design philosophy, key features, and current
capabilities of ``pw_kernel``.

--------------------
Key goals & features
--------------------

Reliability
===========
- **Rust-powered core**: Core kernel components are written in Rust, providing
  strong memory safety guarantees and enabling fearless concurrency.
- **Comprehensive unit testing**: A strong emphasis on testability is built-in,
  featuring an integrated unit testing framework. Extensive tests for core
  kernel primitives (like schedulers, synchronization objects, and data
  structures) ensure reliability and correctness from the ground up.

Security
========
``pw_kernel`` is designed with security as a foremost concern, offering
flexible modes to suit diverse needs:

- **Protected mode**: Leverages hardware features like Arm's Memory Protection
  Unit (MPU) and RISC-V's Physical Memory Protection (PMP). This mode
  establishes clear kernel/user distinctions, supporting processes with memory
  isolation and user-mode threads within those processes. The ultimate goal is
  to enable robust sandboxing for applications, potentially supporting mutually
  distrustful applications. For instance, a cryptography library could operate
  in its own userspace process with exclusive, hardware-enforced access to
  crypto peripherals (via MMIO).
- **Lightweight mode**: For highly resource-constrained systems, ``pw_kernel``
  can operate in a mode similar to traditional RTOSes like FreeRTOS. In this
  configuration, user threads run as kernel threads without memory protection
  boundaries, minimizing overhead. This allows scaling the security model from
  no protection (low cost) to high protection (at some resource cost).
- **Limited shared memory**: Facilitates controlled shared memory regions
  between processes in protected mode, utilizing available MPU/PMP regions.
- **Small TCB & certification path**: Aims for a minimal and verifiable Trusted
  Computing Base (TCB). This simplifies security audits, enhances test coverage
  instrumentation, and is a step towards future certification efforts.

Flexibility & interoperability
==============================
- **Architecture support**: Designed for adaptability, with current support for
  ARM Cortex-M and RISC-V. Future plans include enabling out-of-tree
  architecture support, allowing for fork-free integration of proprietary
  architectures.
- **C++ & Rust interoperability**: Provides mechanisms for seamless interaction
  between kernel services (Rust) and application code (Rust or C++). This
  enables gradual adoption or reuse of existing C/C++ components.
- **Scalability & driver models**: Caters to a range of devices, from small
  microcontrollers to larger embedded systems. Supports both user-mode and
  kernel-mode drivers, offering flexible tradeoffs based on product
  requirements.

Efficiency
==========
- **Memory and code size**: A primary focus is on optimizing for efficient
  memory usage and minimal code footprint. Tools like
  :ref:`panic_detector <module-pw_kernel-guides-panic-detector>` help in
  significantly reducing code size, for instance, by identifying and eliminating
  unnecessary panic paths, contributing to a kernel code size in the ~16-24kB
  range.
- **Zero-allocation design**: The kernel is designed to operate without dynamic
  memory allocation, ensuring predictable behavior and eliminating allocation
  failures. This is achieved through static allocation at build time and
  efficient data structures like intrusive lists.
- **Static configuration**: Rather than hardcoding limits into the kernel,
  configuration is handled at build time. This approach generates static
  allocations and pre-wires IPC channels, embodying the static nature of
  embedded development while avoiding runtime allocation complexity.

Developer experience
====================
- **Integrated tooling**: Benefits from Pigweed's broader ecosystem, including
  tools like the panic detector and a system image assembler, to enhance the
  development workflow.
- **Modern language features**: Rust's modern syntax, powerful type system, and
  package management (via Cargo, integrated with Bazel) contribute to a more
  productive and enjoyable development process.

--------------------
Current capabilities
--------------------
As an experimental kernel, ``pw_kernel`` currently includes:

- A preemptive scheduler with thread and process management.
- Synchronization primitives: spinlocks, mutexes, and events.
- Timer services and a timer queue for managing time-based events.
- A system call interface for userspace applications to interact with the
  kernel.
- Basic exception handling tailored for supported architectures (Arm Cortex-M,
  RISC-V).
- Initial support for hardware memory protection (Arm MPU, RISC-V PMP), laying
  the groundwork for robust user/kernel separation and inter-process isolation.
- An integrated unit testing framework, with tests covering core components
  like synchronization primitives, scheduling, and data structures.
- A highly efficient and safe intrusive linked list implementation that's
  used throughout the kernel, particularly in the scheduler for managing
  threads in run queues and wait queues. This implementation avoids dynamic
  memory allocation for list nodes and provides constant-time operations.
- A channel-based IPC system designed for zero-allocation operation, where
  each channel supports one message in flight at a time. IPC channels are
  asymmetric: only the initiator side can start IPC transactions. The handler
  side can use signals for out-of-band notifications.
- A wait group mechanism for efficient event notification, inspired by epoll,
  where kernel objects have signal masks that can be waited on, with all
  allocations happening at add time rather than during wait operations.
- The ability for userspace processes to monitor and control each other.
- Shareable ``.text`` sections and :ref:`module-pw_tokenizer`-based logging
  for reduced overall binary size.
