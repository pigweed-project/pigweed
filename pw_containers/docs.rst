.. _module-pw_containers:

=============
pw_containers
=============
.. pigweed-module::
   :name: pw_containers

- **Familiar**: Pigweed containers resemble standard library containers.
- **Comprehensive**: You can pick containers for speed, size, or in-between.
- **Embedded-friendly**: No dynamic allocation needed!

.. literalinclude:: examples/vector.cc
   :language: cpp
   :linenos:
   :start-after: [pw_containers-vector]
   :end-before: [pw_containers-vector]

Much like the standard containers library, the ``pw_containers`` module provides
embedded-friendly container class templates and algorithms. These allow
developers to implement data structures for embedded devices that are more
resource-constrained than conventional applications. These containers include:

.. grid:: 2

   .. grid-item-card:: :octicon:`list-ordered` Lists
      :link: module-pw_containers-lists
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Dynamic and intrusive singly and doubly linked lists, and intrusive
      queues.

   .. grid-item-card:: :octicon:`database` Maps
      :link: module-pw_containers-maps
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Key-value stores with constant or logarithmic performance.

.. grid:: 2

   .. grid-item-card:: :octicon:`iterations` Queues
      :link: module-pw_containers-queues
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Uni- and bidirectional FIFOs.

   .. grid-item-card:: :octicon:`list-unordered` Sets
      :link: module-pw_containers-sets
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Unordered collections with fast lookup, insertion, and deletion.

.. grid:: 2

   .. grid-item-card:: :octicon:`tools` Utilities
      :link: module-pw_containers-utilities
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Algorithms and iterators for Pigweed containers.

   .. grid-item-card:: :octicon:`code` Vector
      :link: module-pw_containers-vectors
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Variable length array with fixed storage.

------
Zephyr
------
To enable ``pw_containers`` for Zephyr add ``CONFIG_PIGWEED_CONTAINERS=y`` to
the project's configuration.

.. toctree::
   :hidden:
   :maxdepth: 1

   lists
   maps
   queues
   sets
   vectors
   utilities
   multiple_containers
