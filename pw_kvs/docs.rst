.. _module-pw_kvs:

======
pw_kvs
======
.. pigweed-module::
   :name: pw_kvs

``pw_kvs`` is a flash-backed, persistent key-value storage system with
integrated wear leveling and redundancy, targeting NOR flash devices where a
full filesystem is not needed. Key properties include:

- **Wear leveling**: Spreads writes across flash sectors to extend the life of
  the storage medium.
- **Corruption resilience**: Optionally stores multiple copies of each key-value
  entry, spread over flash sectors. ``pw_kvs`` detects corrupted entries on
  read, and can fall back to a previous version of the data.
- **Flexibility**: Supports configuration changes after a product has shipped.
  For example, a firmware update can increase the total size of the KVS or
  increase or decrease the level of redundancy.
- **Simple on-disk format**: Log-structured database where entries are appended
  sequentially. This design has a minimal set of invariants, which simplifies
  implementation and improves robustness against unexpected power loss.

.. toctree::
   :hidden:
   :maxdepth: 1

   guides
   disk_format
   code_size

.. grid:: 2

   .. grid-item-card:: :octicon:`rocket` Get started & guides
      :link: module-pw_kvs-guides
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Integrate and use ``pw_kvs`` in your project.

   .. grid-item-card:: :octicon:`code-square` API reference
      :link: ../api/cc/group__pw__kvs.html
      :link-type: url
      :class-item: sales-pitch-cta-secondary

      Detailed description of the ``pw_kvs`` classes and methods.

.. grid:: 2

   .. grid-item-card:: :octicon:`server` On-disk format & design
      :link: module-pw_kvs-disk-format
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Understand how ``pw_kvs`` stores data on flash.

   .. grid-item-card:: :octicon:`graph` Code size analysis
      :link: module-pw_kvs-code-size
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Understand ``pw_kvs``'s code footprint.


========
Security
========
See :ref:`module-pw_kvs-security` for the module-specific threat model.

.. toctree::
   :maxdepth: 1
   :hidden:

   security
