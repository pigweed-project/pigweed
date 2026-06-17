.. _docs-os-freertos:
.. _module-pw_third_party_freertos:

========
FreeRTOS
========
Pigweed's FreeRTOS integration provides a suite of abstractions and helpers
designed to make developing C++ applications on FreeRTOS safer, more robust, and
fully compatible with Pigweed's portable facades.

* **Safer mutexes & compile-time lock safety**: Prevents undefined behavior by
  asserting against use in interrupt contexts and detecting recursive locks at
  runtime. Additionally, Pigweed's sync primitives support Clang's thread-safety
  analysis annotations, enabling compile-time detection of data races and incorrect
  lock ordering—which raw C-based FreeRTOS mutexes cannot support.
* **Robust timers & clocks**: Avoids 32-bit tick overflows and eliminates
  software timer deletion race conditions common in native FreeRTOS software
  timers.
* **Optimal notifications**: Consumes task notifications directly inside the TCB,
  avoiding the static RAM and handle overhead required by native FreeRTOS binary
  semaphores.

.. grid:: 1

   .. grid-item-card:: Advantages & differences
      :link: docs-os-freertos-features
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Learn how Pigweed's wrappers improve safety, catch bugs at compile-time or
      runtime, and compare directly with raw FreeRTOS APIs.

.. grid:: 1

   .. grid-item-card:: Setup
      :link: docs-os-freertos-setup
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Integrate FreeRTOS with GN, CMake, or Bazel, configure target parameters,
      and set up necessary custom runtime hook functions.

.. grid:: 1

   .. grid-item-card:: FreeRTOS backend modules
      :link: freertos-backends
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Pigweed provides native FreeRTOS backends for several of its core OS
      abstraction layers.

.. toctree::
   :maxdepth: 1
   :hidden:

   setup
   features
   backends
