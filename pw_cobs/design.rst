.. _module-pw_cobs-design:

======
Design
======
.. pigweed-module-subpage::
   :name: pw_cobs

--------
Overview
--------
Serial links (UART, SPI, USB CDC) require framing to delineate packet boundaries
over a raw byte stream. Byte-stuffing encodings such as HDLC
(:ref:`module-pw_hdlc`) scan each byte for ``0x7E`` (flag) and ``0x7D``
(escape) and expand each match into a 2-byte escape sequence.

Consistent Overhead Byte Stuffing (COBS, Cheshire & Baker 1999) instead
replaces each ``0x00`` byte with a 1-byte offset to the next ``0x00`` byte (up
to 254 bytes ahead), reserving ``0x00`` exclusively as the frame delimiter. This
provides two practical properties:

1. **Bounded wire expansion**: A payload of :math:`N` bytes adds 1 initial code
   byte plus at most :math:`\lceil N / 254 \rceil - 1` additional code bytes
   (at most 1 extra byte per 254 payload bytes, or ~0.39%), plus 1 trailing
   ``0x00`` delimiter, compared to up to 100% expansion when every byte
   requires escaping.
2. **Contiguous payload spans**: Because non-zero bytes are not modified, blocks
   of up to 254 non-zero bytes remain contiguous in memory and can be passed
   directly to a scatter-gather write or ``std::memcpy()``.

.. list-table:: Comparison of ``pw_hdlc`` and ``pw_cobs``
   :header-rows: 1
   :widths: 28 36 36

   * - Property
     - ``pw_hdlc`` (Byte-Stuffing)
     - ``pw_cobs`` (Consistent Overhead Byte Stuffing)
   * - Frame Delimiter
     - ``0x7E`` (``~``)
     - ``0x00`` (NUL)
   * - Worst-Case Wire Expansion
     - +100% (if every byte is ``0x7E`` or ``0x7D``)
     - +0.39% (:math:`\lceil N / 254 \rceil` bytes)
   * - Typical Wire Framing Overhead
     - :math:`2 + \text{escapes}` bytes
     - 2 bytes (1 initial code byte + 1 trailing ``0x00``) for frames under 254 bytes
   * - Direct Payload Writes
     - Requires splitting at every ``0x7E`` or ``0x7D``
     - Contiguous non-zero blocks up to 254 bytes remain unmodified in the source buffer

-----------
Wire format
-----------
COBS partitions an input buffer into contiguous blocks of 0 to 254 non-zero
bytes (:cc:`kMaxRunLength <pw::cobs::kMaxRunLength>`). Each block is prefixed by
a single non-zero code byte ``C`` (``0x01..0xFF``), and the frame is terminated
by ``0x00``:

* **Codes** ``0x01..0xFE`` (:math:`1 \le C \le 254`):
  The code byte is followed by :math:`C - 1` non-zero data bytes. If the block
  does not end at the ``0x00`` frame delimiter, the decoder appends an implicit
  ``0x00`` byte after those :math:`C - 1` bytes.
* **Code** ``0xFF`` (:math:`C = 255`):
  The code byte is followed by 254 non-zero data bytes with no implicit trailing
  ``0x00``. This splits non-zero runs of 254 or more bytes by inserting one
  ``0xFF`` byte every 254 bytes.

.. code-block:: text

   Unencoded input:   [ 0x11, 0x22, 0x00, 0x33, 0x44, 0x55 ]
                        \________/  ^^^^  \______________/
                         2 bytes    zero      3 bytes
                         (C = 3)              (C = 4)

   COBS encoded:      [ 0x03, 0x11, 0x22, 0x04, 0x33, 0x44, 0x55, 0x00 ]

An empty payload (``{}``) encodes to a single empty block followed by the
trailing delimiter: ``[ 0x01, 0x00 ]``.
