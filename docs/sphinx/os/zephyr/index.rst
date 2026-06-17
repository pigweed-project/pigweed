.. _docs-os-zephyr:

======
Zephyr
======
Pigweed's Zephyr integration provides a suite of abstractions and helpers designed to
make developing C++ applications on Zephyr safer, more robust, and fully
compatible with Pigweed's portable facades.

* **Compile-time lock safety**: Support for Clang's static thread-safety analysis
  annotations, enabling compile-time detection of data races and incorrect lock ordering.
* **Safe non-recursive mutexes**: Enforces strict non-recursive mutex semantics
  and assertions to detect bugs and prevent deadlocks.
* **Safe callback execution**: Routes timer callbacks to delayable work queues
  running in thread context, enabling race-free synchronous timer deletion.
* **Tokenized logs and asserts**: Drastically reduces binary size by tokenizing
  all assertion and logging statements in both the application and the Zephyr kernel.

.. grid:: 1

   .. grid-item-card:: Advantages & differences
      :link: docs-os-zephyr-features
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Learn how Pigweed's wrappers improve safety, catch bugs at compile-time or
      runtime, and compare directly with raw Zephyr APIs.

.. grid:: 1

   .. grid-item-card:: Setup
      :link: docs-os-zephyr-setup
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Get started using Pigweed with Zephyr, build the starter project, and run
      tests.

.. grid:: 1

   .. grid-item-card:: Zephyr backend modules
      :link: zephyr-backends
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Pigweed provides native Zephyr backends for several of its core OS
      abstraction layers.

.. toctree::
   :maxdepth: 1
   :hidden:

   setup
   features
   backends
