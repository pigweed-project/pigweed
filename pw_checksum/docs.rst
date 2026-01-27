.. _module-pw_checksum:

-----------
pw_checksum
-----------
.. pigweed-module::
   :name: pw_checksum

The ``pw_checksum`` module provides functions for calculating checksums.

pw_checksum/crc8.h
==================

:cc:`pw::checksum::Crc8`

pw_checksum/crc16_ccitt.h
=========================

:cc:`pw::checksum::Crc16Ccitt`

pw_checksum/crc32.h
===================

:cc:`pw::checksum::Crc32`

.. _CRC32 Implementations:

Implementations
---------------
Pigweed provides 3 different CRC32 implementations with different size and
runtime tradeoffs.  The below table summarizes the variants.  For more detailed
size information see the :ref:`pw_checksum-size-report` below.  Instructions
counts were calculated by hand by analyzing the
`assembly <https://godbolt.org/z/nY1bbb5Pb>`_. Clock Cycle counts were measured
using :ref:`module-pw_perf_test` on a STM32F429I-DISC1 development board.


.. list-table::
   :header-rows: 1

   * - Variant
     - Relative size (see Size Report below)
     - Speed
     - Lookup table size (entries)
     - Instructions/byte (M33/-Os)
     - Clock Cycles (123 char string)
     - Clock Cycles (9 bytes)
   * - 8 bits per iteration (default)
     - large
     - fastest
     - 256
     - 8
     - 1538
     - 170
   * - 4 bits per iteration
     - small
     - fast
     - 16
     - 13
     - 2153
     - 215
   * - 1 bit per iteration
     - smallest
     - slow
     - 0
     - 43
     - 7690
     - 622

The default implementation provided by the APIs above can be selected through
:ref:`Module Configuration Options`.  Additionally ``pw_checksum`` provides
variants of the C++ API to explicitly use each of the implementations.  These
classes provide the same API as ``Crc32``:

* ``Crc32EightBit``
* ``Crc32FourBit``
* ``Crc32OneBit``

.. _pw_checksum-size-report:

Size report
===========
The CRC module currently optimizes for speed instead of binary size, by using
pre-computed 256-entry tables to reduce the CPU cycles per byte CRC
calculation.

.. include:: pw_checksum_size_report

Compatibility
=============
* C
* C++17

Dependencies
============
- :ref:`module-pw_span`

.. _Module Configuration Options:

Module Configuration Options
============================
The following configurations can be adjusted via compile-time configuration of
this module, see the
:ref:`module documentation <module-structure-compile-time-configuration>` for
more details.

.. c:macro:: PW_CHECKSUM_CRC32_DEFAULT_IMPL

  Selects which of the :ref:`CRC32 Implementations` the default CRC32 APIs
  use.  Set to one of the following values:

  * ``PW_CHECKSUM_CRC32_8BITS``
  * ``PW_CHECKSUM_CRC32_4BITS``
  * ``PW_CHECKSUM_CRC32_1BITS``

Zephyr
======
To enable ``pw_checksum`` for Zephyr add ``CONFIG_PIGWEED_CHECKSUM=y`` to the
project's configuration.
