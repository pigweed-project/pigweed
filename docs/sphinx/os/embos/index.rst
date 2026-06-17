.. _docs-os-embos:
.. _module-pw_third_party_embos:

=====
embOS
=====
Pigweed's embOS integration provides a suite of abstractions and helpers
designed to make developing C++ applications on SEGGER embOS safer, more robust,
and fully compatible with Pigweed's portable facades.

* **Compile-time lock safety**: Support for Clang's static thread-safety analysis
  annotations, enabling compile-time detection of data races and incorrect lock ordering.
* **Safe non-recursive mutexes**: Enforces strict non-recursive mutex semantics
  and assertions to detect bugs and prevent deadlocks.
* **Port-safe interrupt spinlocks**: Disables task switching and interrupts to prevent
  deadlocks and context switches during critical sections.

.. grid:: 1

   .. grid-item-card:: Advantages & differences
      :link: docs-os-embos-features
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Learn how Pigweed's wrappers improve safety, catch bugs at compile-time or
      runtime, and compare directly with raw embOS APIs.

.. grid:: 1

   .. grid-item-card:: Setup
      :link: docs-os-embos-setup
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Integrate embOS with GN, configure target parameters, and configure
      module backend options.

.. grid:: 1

   .. grid-item-card:: embOS backend modules
      :link: embos-backends
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Pigweed provides native embOS backends for several of its core OS
      abstraction layers.

.. toctree::
   :maxdepth: 1
   :hidden:

   setup
   features
   backends
