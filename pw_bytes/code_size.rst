.. _module-pw_bytes-code-size:

==================
Code size analysis
==================
.. pigweed-module-subpage::
   :name: pw_bytes

This page analyzes the code size impact of using components from the
``pw_bytes`` module.

Size report: using ByteBuffer vs. manual manipulation
-----------------------------------------------------
The following report compares the code size of using
:cc:`pw::ByteBuffer` for constructing and parsing a simple byte sequence
versus manually performing similar operations with ``std::memcpy`` and bitwise
logic for endianness.

:cc:`pw::ByteBuffer` can offer improved type safety and convenience,
potentially at a small code size cost for very simple operations, but can lead
to more maintainable and less error-prone code for complex byte manipulations.

.. include:: byte_builder_size_report
