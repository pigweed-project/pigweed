.. _module-pw_hdlc-guide:

====================
Get started & guides
====================
.. pigweed-module-subpage::
   :name: pw_hdlc

.. include:: design.rst
   :start-after: .. pw_hdlc-overview-start
   :end-before: .. pw_hdlc-overview-end

.. _module-pw_hdlc-getstarted:

------------------------
Get started with pw_hdlc
------------------------
Depend on the library:

.. tab-set::

   .. tab-item:: Bazel
      :sync: bazel

      Add ``@pigweed//pw_hdlc`` to the ``deps`` list in your Bazel target:

      .. code-block::

         cc_library("...") {
           # ...
           deps = [
             # ...
             "@pigweed//pw_hdlc",
             # ...
           ]
         }

   .. tab-item:: GN
      :sync: gn

      Add ``$dir_pw_hdlc`` to the ``deps`` list in your ``pw_executable()``
      build target:

      .. code-block::

         pw_executable("...") {
           # ...
           deps = [
             # ...
             "$dir_pw_hdlc",
             # ...
           ]
         }


   .. tab-item:: CMake
      :sync: cmake

      Add ``pw_hdlc`` to your ``pw_add_library`` or similar CMake target:

      .. code-block::

         pw_add_library(my_library STATIC
           HEADERS
             # ...
           PRIVATE_DEPS
             # ...
             pw_hdlc
             # ...
         )

   .. tab-item:: Zephyr
      :sync: zephyr

      There are two ways to use ``pw_hdlc`` from a Zephyr project:

      * (Recommended) Depend on ``pw_hdlc`` in your CMake target. See the CMake
        tab. The Pigweed team recommends this approach because it enables
        precise CMake dependency analysis.

      * Add ``CONFIG_PIGWEED_HDLC=y`` to your Zephyr project's configuration.
        Also add ``CONFIG_PIGWEED_HDLC_RPC=y`` if using RPC. Although this is
        the typical Zephyr solution, the Pigweed team doesn't recommend this
        approach because it makes ``pw_hdlc`` a global dependency and exposes
        its includes to all targets.

And then :ref:`encode <module-pw_hdlc-guide-encode>` and
:ref:`decode <module-pw_hdlc-guide-decode>` some data!

Set up RPC over HDLC
====================
See :ref:`module-pw_hdlc-rpc-example`.

.. _module-pw_hdlc-guide-encode:

--------
Encoding
--------
.. include:: docs.rst
   :start-after: .. pw_hdlc-encoding-example-start
   :end-before: .. pw_hdlc-encoding-example-end

Allocating buffers when encoding
================================
.. tab-set::

   .. tab-item:: C++
      :sync: cpp

      HDLC encoding overhead changes depending on the payload size and the
      nature of the data being encoded. ``pw_hdlc`` provides helper functions
      for determining the size of buffers. The helper functions provide
      worst-case sizes of frames given a certain payload size and vice-versa.

      .. code-block:: cpp

         #include "pw_assert/check.h"
         #include "pw_bytes/span.h"
         #include "pw_hdlc/encoded_size.h"
         #include "pw_hdlc/encoder"
         #include "pw_status/status.h"

         // The max on-the-wire size in bytes of a single HDLC frame after encoding.
         constexpr size_t kMtu = 512;
         constexpr size_t kRpcEncodeBufferSize = pw::hdlc::MaxSafePayloadSize(kMtu);
         std::array<std::byte, kRpcEncodeBufferSize> rpc_encode_buffer;

         // Any data encoded to this buffer is guaranteed to fit in the MTU after
         // HDLC encoding.
         pw::ConstByteSpan GetRpcEncodeBuffer() { return rpc_encode_buffer; }

.. _module-pw_hdlc-guide-decode:

--------
Decoding
--------
.. include:: docs.rst
   :start-after: .. pw_hdlc-decoding-example-start
   :end-before: .. pw_hdlc-decoding-example-end


Allocating buffers when decoding
================================
.. tab-set::

   .. tab-item:: C++
      :sync: cpp

      ``pw::hdlc::Decoder`` is a helper for allocating a buffer. It has slightly
      lower overhead because it doesn't need to decode the entire escaped
      frame in-memory.

      .. code-block:: cpp

         #include "pw_hdlc/decoder.h"

         // The max on-the-wire size in bytes of a single HDLC frame after encoding.
         constexpr size_t kMtu = 512;

         // Create a decoder given the MTU constraint.
         constexpr size_t kDecoderBufferSize =
             pw::hdlc::Decoder::RequiredBufferSizeForFrameSize(kMtu);
         pw::hdlc::DecoderBuffer<kDecoderBufferSize> decoder;

Tools
=====
The ``dump_hdlc_frames.py`` script can be used to examine HDLC streams, e.g.,
from a raw UART log:

.. code-block:: console

   $ bazelisk --quiet run //pw_hdlc/py:dump_hdlc_frames -- < raw_uart.bin
   [0000] OK addr=0x01 ctrl=0x03 len=5   'hello'
   [0001] OK addr=0x01 ctrl=0x03 len=5   'world'


-----------------
More pw_hdlc docs
-----------------
.. include:: docs.rst
   :start-after: .. pw_hdlc-nav-start
   :end-before: .. pw_hdlc-nav-end
