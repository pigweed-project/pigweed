.. _docs-os-embos:
.. _module-pw_third_party_embos:

============
Segger embOS
============
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

.. toctree::
   :maxdepth: 1
   :hidden:

   self
   setup
   features

.. categories-start
.. +----------------------------------------------------------+
.. | WARNING: AUTO-GENERATED SECTION. DO NOT MANUALLY EDIT!   |
.. |                                                          |
.. | To regenerate this section, run:                         |
.. |   bazelisk run //docs/common/metadata:sync               |
.. |                                                          |
.. | Docs: https://pigweed.dev/contributing/docs/website.html |
.. +----------------------------------------------------------+

-------
Modules
-------
The following table lists all modules related to Segger embOS.

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_chrono_embos`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_sync_embos`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_thread_embos`", ":bdg-primary:`stable`", "", ""

.. Do not edit this toctree. To add non-module docs to this category,
.. create a separate toctree outside the categories-start /
.. categories-end block.
.. toctree::
   :maxdepth: 1
   :hidden:

   Chrono <../../pw_chrono_embos/docs>
   Synchronization <../../pw_sync_embos/docs>
   Threads <../../pw_thread_embos/docs>

.. +----------------------------------------------------------+
.. | END OF AUTO-GENERATED SECTION                            |
.. +----------------------------------------------------------+
.. categories-end
