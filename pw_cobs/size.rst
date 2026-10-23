.. _module-pw_cobs-size:

.. rst-class:: with-subtitle

==================
Code size analysis
==================
.. pigweed-module-subpage::
   :name: pw_cobs

``pw_cobs`` is designed for small Flash and RAM footprints on microcontrollers.

The table below shows the incremental Flash footprint of:

* **One-shot COBS encode and decode** (:cc:`Encode <pw::cobs::Encode>` and
  :cc:`Decode <pw::cobs::Decode>`).
* **In-place EncodeBuffer** (:cc:`EncodeBuffer <pw::cobs::EncodeBuffer>` and
  :cc:`EncodeInPlace <pw::cobs::EncodeInPlace>`).
* **Streaming COBS Decoder** (:cc:`Decoder <pw::cobs::Decoder>`).
* **All COBS primitives combined**.

.. include:: pw_cobs_size_table
