.. _module-pw_bytes:

========
pw_bytes
========
.. pigweed-module::
   :name: pw_bytes

- **Type-safe**: Work with binary data using ``std::byte``.
- **Compile-time**: Construct and validate byte arrays at compile time.
- **Convenient**: Easily handle endianness, alignment, and byte units.

``pw_bytes`` is a collection of utilities for robust and efficient
manipulation of binary data in C++. It provides tools for constructing
byte arrays at compile time, handling data alignment, converting
endianness, and defining byte quantities with clear, standard units.

.. code-block:: cpp

   #include "pw_bytes/array.h"
   #include "pw_bytes/endian.h"
   #include "pw_bytes/units.h"

   // Clearly define buffer sizes with IEC units.
   using namespace pw::bytes::unit_literals;
   constexpr size_t kTelemetryPacketMaxSize = 1_KiB;

   // Construct byte arrays at compile time.
   constexpr auto kHeader = pw::bytes::Array<'P', 'W', 'R', 'D'>;
   constexpr uint32_t kPayloadId = 0x12345678;

   // Combine data with specified endianness.
   constexpr auto kPacketPrefix = pw::bytes::Concat(
       kHeader, pw::bytes::CopyInOrder(pw::endian::big, kPayloadId));

   // kPacketPrefix now contains:
   // {'P', 'W', 'R', 'D', 0x12, 0x34, 0x56, 0x78}

Check out :ref:`module-pw_bytes-guide` for more code samples.

.. admonition:: Is it right for you?
   :class: tip

   ``pw_bytes`` is useful any time you need to work with raw binary data,
   manage memory layout precisely, or ensure type safety at the byte level
   in embedded C++.

   If your project primarily deals with high-level data structures and doesn't
   require direct byte manipulation, other Pigweed modules or standard library
   features might be more suitable.

   ``pw_bytes`` utilities are designed to be lightweight and can be beneficial
   even in less resource-constrained environments by improving code clarity
   and safety when dealing with binary data.

.. grid:: 2

   .. grid-item-card:: :octicon:`rocket` Quickstart & guides
      :link: module-pw_bytes-guide
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Integrate pw_bytes into your project and learn common use cases

   .. grid-item-card:: :octicon:`code-square` API reference
      :link: ../api/cc/group__pw__bytes.html
      :link-type: url
      :class-item: sales-pitch-cta-secondary

      Detailed description of the pw_bytes interface

.. grid:: 2

   .. grid-item-card:: :octicon:`code-square` Design & roadmap
      :link: module-pw_bytes-design
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Learn why pw_bytes is designed the way it is, and upcoming plans

   .. grid-item-card:: :octicon:`graph` Code size analysis
      :link: module-pw_bytes-code-size
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Understand pw_bytes's code footprint

------------
Dependencies
------------
* ``pw_preprocessor``
* ``pw_status``
* ``pw_span``

------
Zephyr
------
To enable ``pw_bytes`` for Zephyr add ``CONFIG_PIGWEED_BYTES=y`` to the
project's configuration.

----------
Python API
----------
``pw_bytes`` provides a Python package containing binary stream read/write
utilities. See :ref:`module-pw_bytes-python` for details.

--------
Rust API
--------
``pw_bytes``'s Rust API is documented in our
`rustdoc API docs </rustdoc/pw_bytes/>`_.

.. toctree::
   :hidden:
   :maxdepth: 1

   code_size
   design
   guide
   python
