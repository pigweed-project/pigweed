.. _module-pw_cobs:

.. rst-class:: with-subtitle

=======
pw_cobs
=======
.. pigweed-module::
   :name: pw_cobs

   - **Predictable wire overhead**: Adds at most 1 byte per 254 payload bytes
   - **In-place & streaming encoding**: Encode in-place with
     ``EncodeBuffer`` or stream zero-copy payload chunks with ``Streamer``
   - **One-shot & streaming decoding**: Decode complete ``0x00``-delimited
     frames or feed incoming serial bytes incrementally with ``Decoder``

``pw_cobs`` implements `Consistent Overhead Byte Stuffing
<https://en.wikipedia.org/wiki/Consistent_Overhead_Byte_Stuffing>`_ (COBS) for
framing binary data over byte streams such as UART, USB CDC, or SPI. COBS
transforms arbitrary payloads into blocks of non-zero bytes (``0x01..0xFF``) so
that ``0x00`` unambiguously marks frame boundaries on the wire.

-----------
Quick start
-----------
Encode a payload in-place using :cc:`EncodeBuffer <pw::cobs::EncodeBuffer>`:

.. pw_cobs-encoding-example-start

.. code-block:: cpp

   #include "pw_cobs/cobs.h"

   void SendPacket(pw::ConstByteSpan payload) {
     pw::cobs::EncodeBuffer<256> buffer(payload);
     pw::ConstByteSpan encoded = buffer.Encode();
     WriteToSerial(encoded);
   }

.. pw_cobs-encoding-example-end

Decode incoming wire bytes incrementally with
:cc:`DecoderBuffer <pw::cobs::DecoderBuffer>`:

.. pw_cobs-decoding-example-start

.. code-block:: cpp

   #include "pw_cobs/cobs.h"

   constexpr size_t kMaxPayloadBytes = 256;
   pw::cobs::DecoderBuffer<kMaxPayloadBytes> decoder;

   void OnSerialBytesReceived(pw::ConstByteSpan rx_bytes) {
     decoder.Process(rx_bytes, [](pw::Result<pw::ByteSpan> frame) {
       if (frame.ok()) {
         HandlePacket(*frame);
       }
     });
   }

.. pw_cobs-decoding-example-end

-----------------
Where to go next
-----------------
.. grid:: 2

   .. grid-item-card:: :octicon:`rocket` Get started & guides
      :link: module-pw_cobs-guide
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Integrate ``pw_cobs`` into your build and learn how to encode, iterate
      blocks, and decode frames.

   .. grid-item-card:: :octicon:`code-square` API reference
      :link: ../api/cc/group__pw__cobs.html
      :link-type: url
      :class-item: sales-pitch-cta-secondary

      Doxygen reference for the classes and functions in ``pw_cobs/cobs.h``.

.. grid:: 2

   .. grid-item-card:: :octicon:`stack` Design
      :link: module-pw_cobs-design
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Wire format, block structure, and comparison with ``pw_hdlc``.

   .. grid-item-card:: :octicon:`graph` Code size analysis
      :link: module-pw_cobs-size
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Flash footprint of ``pw_cobs`` encoding and decoding primitives.

.. toctree::
   :maxdepth: 1
   :hidden:

   guide
   design
   size
   pw://cc-api-ref
