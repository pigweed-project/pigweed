.. _module-pw_kernel:

=========
pw_kernel
=========
.. pigweed-module::
   :name: pw_kernel

``pw_kernel`` is an embedded kernel written in Rust that provides:

* A core preemptive scheduling and context switch system
* Synchronization primitives (spinlocks, mutexes, and events)
* Out-of-tree architecture porting layer
* In-tree support for RV32, ARMv8-M, and ARMv7-M
* Capability-based userspace primitives (processes, IPC, interrupts, and
  wait groups)
* Robust protection against process faults
* Shareable ``.text`` sections and ``pw_tokenizer``-based logging for
  reduced binary size

Embedded systems built on top of ``pw_kernel`` are a composition of
memory-protected userspace apps and drivers. All interactions with the kernel
are mediated through unforgeable tokens called handles. The system is defined
declaratively, and all allocation is static.  Bazel macros are provided for
building, linking, and optimizing multiple processes into a single image.

.. grid:: 2

   .. grid-item-card:: :octicon:`rocket` Quickstart
      :link: quickstart
      :link-type: doc
      :class-item: sales-pitch-cta-primary

      Get a complete, working system powered by ``pw_kernel`` up and running
      in minutes.

   .. grid-item-card:: :octicon:`mortar-board` Tour
      :link: tour
      :link-type: doc
      :class-item: sales-pitch-cta-primary

      A conceptual overview of the software architecture of an embedded system
      built on top of ``pw_kernel``.

.. grid:: 2

   .. grid-item-card:: :octicon:`beaker` Design
      :link: design
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      The design philosophy and goals of ``pw_kernel``.

   .. grid-item-card:: :octicon:`list-unordered` Guides
      :link: guides
      :link-type: doc
      :class-item: sales-pitch-cta-secondary

      Unit testing, panic detecting, and other real-world usage topics.

.. TODO: https://pwbug.dev/424641732 - Re-enable the Rust API reference link.

.. toctree::
   :hidden:
   :maxdepth: 1

   quickstart
   Tour <tour>
   design
   guides
   Upstream <upstream>
