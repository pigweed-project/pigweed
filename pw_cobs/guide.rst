.. _module-pw_cobs-guide:

====================
Get started & guides
====================
.. pigweed-module-subpage::
   :name: pw_cobs

This guide shows how to add ``pw_cobs`` to your build and use it to encode and
decode COBS frames.

.. _module-pw_cobs-getstarted:

-----------
Build setup
-----------
.. tab-set::

   .. tab-item:: Bazel
      :sync: bazel

      Add ``@pigweed//pw_cobs/cpp:pw_cobs`` to the ``deps`` list in your Bazel
      target:

      .. code-block:: python

         cc_library(
             name = "my_transport",
             srcs = ["my_transport.cc"],
             deps = [
                 "@pigweed//pw_cobs/cpp:pw_cobs",
             ],
         )

   .. tab-item:: GN
      :sync: gn

      Add ``$dir_pw_cobs/cpp:pw_cobs`` to the ``deps`` list in your
      ``pw_source_set()`` or ``pw_executable()`` build target:

      .. code-block::

         pw_source_set("my_transport") {
           sources = [ "my_transport.cc" ]
           deps = [
             "$dir_pw_cobs/cpp:pw_cobs",
           ]
         }

   .. tab-item:: CMake
      :sync: cmake

      CMake is not supported.

.. _module-pw_cobs-guide-encode-buffer:

-----------------------------------
Encoding in-place with EncodeBuffer
-----------------------------------
:cc:`EncodeBuffer <pw::cobs::EncodeBuffer>` is a fixed-capacity container that
stores the unencoded payload and encodes it in-place when ``Encode()`` is
called, avoiding a second buffer:

.. include:: docs.rst
   :start-after: .. pw_cobs-encoding-example-start
   :end-before: .. pw_cobs-encoding-example-end

You can also populate ``EncodeBuffer`` incrementally using ``append()``,
``push_back()``, or ``resize()``:

.. code-block:: cpp

   #include "pw_cobs/cobs.h"

   void BuildAndEncode(pw::ConstByteSpan header, pw::ConstByteSpan body) {
     pw::cobs::EncodeBuffer<256> buffer;
     buffer.append(header);
     buffer.append(body);
     pw::ConstByteSpan encoded = buffer.Encode();
     WriteToSerial(encoded);
   }

.. _module-pw_cobs-guide-raw:

-----------------------------------------
One-shot encoding and decoding with spans
-----------------------------------------
Use :cc:`Encode <pw::cobs::Encode>` and :cc:`Decode <pw::cobs::Decode>` when
working with separate input and output spans:

.. code-block:: cpp

   #include <array>

   #include "pw_cobs/cobs.h"

   void EncodeAndDecodeSpan(pw::ConstByteSpan payload) {
     std::array<std::byte, pw::cobs::MaxEncodedSize(128)> wire_buf{};
     pw::Result<pw::ByteSpan> encoded = pw::cobs::Encode(payload, wire_buf);
     if (!encoded.ok()) {
       return;
     }

     std::array<std::byte, 128> decoded_buf{};
     pw::Result<pw::ByteSpan> decoded = pw::cobs::Decode(*encoded, decoded_buf);
   }

.. _module-pw_cobs-guide-blocks:

----------------------------------
Streaming encoding with Streamer
----------------------------------
:cc:`Streamer <pw::cobs::Streamer>` (and the owning convenience wrapper
:cc:`StreamerBuffer <pw::cobs::StreamerBuffer>`) incrementally COBS-encodes an
arbitrarily large frame into a fixed-size buffer (at least 257 bytes). Each call
to ``Flush()`` emits all completed COBS blocks currently in the buffer, and
``Finish()`` closes the final block and appends the trailing ``0x00`` delimiter:

.. code-block:: cpp

   #include "pw_cobs/cobs.h"

   void StreamPayload(pw::ConstByteSpan header, pw::ConstByteSpan body) {
     pw::cobs::StreamerBuffer<> streamer;
     for (pw::ConstByteSpan seg : {header, body}) {
       while (!seg.empty()) {
         seg = streamer.Write(seg);
         if (!seg.empty()) {
           WriteToSerial(streamer.Flush());
         }
       }
     }
     WriteToSerial(streamer.Finish());
   }

.. _module-pw_cobs-guide-decode:

------------------------------------
Streaming decode with DecoderBuffer
------------------------------------
Use :cc:`DecoderBuffer <pw::cobs::DecoderBuffer>` (or
:cc:`Decoder <pw::cobs::Decoder>` with an external buffer) to decode incoming
serial bytes incrementally:

.. include:: docs.rst
   :start-after: .. pw_cobs-decoding-example-start
   :end-before: .. pw_cobs-decoding-example-end

Or feed one byte at a time:

.. code-block:: cpp

   pw::Result<pw::ByteSpan> result = decoder.Process(rx_byte);
   if (result.ok()) {
     HandlePacket(*result);
   }

.. _module-pw_cobs-guide-python:

------
Python
------
The ``pw_cobs`` Python package (``//pw_cobs/py:pw_cobs``) provides pure-Python
equivalents of the C++ encoding and decoding primitives:

.. code-block:: python

   from pw_cobs import Decoder, Streamer, decode, encode

   wire_frame = encode(b'hello')
   assert decode(wire_frame) == b'hello'

   streamer = Streamer(buffer_size=257)
   wire_chunks = b''.join(streamer.stream(b'hello', b' world'))

   decoder = Decoder(max_payload_size=256)
   for frame in decoder.process(wire_chunks):
       if frame.ok():
           handle_packet(frame.payload)
