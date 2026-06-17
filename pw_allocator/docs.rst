.. _module-pw_allocator:

============
pw_allocator
============
.. pigweed-module::
   :name: pw_allocator

   - **Flexible**: Simple interface makes it easy to inject specific behaviors.
   - **Safe**: Can detect memory corruption, e.g. overflows and use-after-free.
   - **Measurable**: Pick what allocations you want to track and measure.

.. literalinclude:: examples/basic.cc
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-basic-allocate]
   :end-before: [pw_allocator-examples-basic-allocate]

.. code-block:: cpp

   // Any of these allocators can be passed to the routine above.
   WithBuffer<LastFitBlockAllocator<uint32_t>, 0x1000> block_allocator;
   LibCAllocator libc_allocator;
   TrackingAllocator tracker(libc_allocator);
   SynchronizedAllocator synced(*block_allocator);

Dynamically allocate without giving up control! Pigweed's allocators let you
easily combine allocator features for your needs, without extra code size or
performance penalties for those you don't. Complex projects in particular can
benefit from dynamic allocation through simplified code, improved memory usage,
increased shared memory, and reduced large reservations.

**Want to allocate objects from specific memory like SRAM or PSRAM?**

Use `dependency injection`_! Write your code to take
:cc:`pw::Allocator` parameters, and you can quickly and
easily change where memory comes from or what additional features are provided
simply by changing what allocator is passed:

.. literalinclude:: examples/linker_sections.cc
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-linker_sections-injection]
   :end-before: [pw_allocator-examples-linker_sections-injection]

Now you can easily allocate objects in the example above using SRAM, PSRAM, or
both:

.. literalinclude:: examples/linker_sections.cc
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-linker_sections-placement]
   :end-before: [pw_allocator-examples-linker_sections-placement]

**Worried about forgetting to deallocate?**

Use a smart pointer!

.. literalinclude:: examples/basic.cc
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-basic-make_unique]
   :end-before: [pw_allocator-examples-basic-make_unique]

**Want to know how much memory has been allocated?**

Pick the metrics you're interested in and track them with a
:cc:`TrackingAllocator <pw::allocator::TrackingAllocator>`:

.. literalinclude:: examples/metrics.cc
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-metrics-custom_metrics1]
   :end-before: [pw_allocator-examples-metrics-custom_metrics1]

.. literalinclude:: examples/metrics.cc
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-metrics-custom_metrics2]
   :end-before: [pw_allocator-examples-metrics-custom_metrics2]

**Need to share the allocator with another thread or an interrupt handler?**

Use a :cc:`SynchronizedAllocator <pw::allocator::SynchronizedAllocator>`
with the lock of your choice:

.. literalinclude:: examples/spin_lock.cc
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-spin_lock]
   :end-before: [pw_allocator-examples-spin_lock]

.. tip:: Check out the :ref:`module-pw_allocator-guides` for even more code
   samples!

--------------------
Is it right for you?
--------------------
.. rst-class:: key-text

Does your project need to use memory...

- Without knowing exactly how much ahead of time?
- From different backing storage, e.g. SRAM vs. PSRAM?
- Across different tasks using a shared pool?
- In multiple places, and you need to know how much each place is using?

If you answered "yes" to any of these questions, ``pw_allocator`` may be able to
help! This module is designed to facilitate dynamic allocation for embedded
projects that are sufficiently complex to make static allocation infeasible.

Smaller projects may be able to enumerate their objects and preallocate any
storage they may need on device when running, and may be subject to extremely
tight memory constraints. In these cases, ``pw_allocator`` may add more costs in
terms of code size, overhead, and performance than it provides benefits in terms
of code simplicity and memory sharing.

At the other extreme, larger projects may be built on platforms with rich
operating system capabilities. On these platforms, the system and language
runtime may already provide dynamic allocation and memory may be less
constrained. In these cases, ``pw_allocator`` may not provide the same
capabilities as the platform.

Between these two is a range of complex projects on RTOSes and other platforms.
These projects may benefit from using the
:cc:`pw::Allocator` interface and its implementations to
manage memory.

.. toctree::
   :hidden:
   :maxdepth: 1

   guide
   design
   backends
   code_size

.. grid:: 2

   .. grid-item-card:: :octicon:`rocket` Guides
      :link: module-pw_allocator-guides
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Integrate pw_allocator into your project and learn common use cases

   .. grid-item-card:: :octicon:`code-square` API reference
      :link: ../api/cc/group__pw__allocator.html
      :link-type: url
      :class-item: sales-pitch-cta-secondary

      Detailed description of the pw_allocator's API

.. grid:: 2

   .. grid-item-card:: :octicon:`code-square` Design & roadmap
      :link: module-pw_allocator-design
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Learn why pw_allocator is designed the way it is, and upcoming plans

   .. grid-item-card:: :octicon:`code-square` Code size analysis
      :link: module-pw_allocator-size-reports
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Understand pw_allocator's code footprint and savings potential

.. _dependency injection: https://en.wikipedia.org/wiki/Dependency_injection
