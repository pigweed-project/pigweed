.. _module-pw_rpc:

======
pw_rpc
======
.. pigweed-module::
   :name: pw_rpc

``pw_rpc`` provides an embedded-friendly remote procedure call (RPC) system for
defining and invoking structured methods over arbitrary serial, bus, or packet
transports (UART, SPI, USB, BLE, Sockets). Services and messages are defined in
shared Protocol Buffer (``.proto``) files.

``pw_rpc`` supports C++ (with Nanopb, ``pw_protobuf``, or Raw RPC codegen), Python,
TypeScript, and Java.

.. tab-set::

   .. tab-item:: blinky.proto

      .. literalinclude:: examples/blinky.proto
         :language: protobuf
         :start-after: [pw_rpc-examples-blinky-proto]

   .. tab-item:: main.cc

      .. literalinclude:: examples/blinky_service.cc
         :language: cpp
         :start-after: [pw_rpc-examples-blinky]
         :end-before: [pw_rpc-examples-blinky]

   .. tab-item:: BUILD.bazel

      .. literalinclude:: examples/BUILD.bazel
         :language: python
         :start-after: [pw_rpc-examples-blinky-build]
         :end-before: [pw_rpc-examples-blinky-build]

-------------------
Where to go next
-------------------

.. grid:: 2

   .. grid-item-card:: :octicon:`tools` Integration & setup
      :link: module-pw_rpc-setup
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      **For Platform Engineers & System Architects.**
      Step-by-step checklist to bring up ``pw_rpc`` on target hardware:
      transports, channels, RX/TX plumbing, and dispatch loops.

   .. grid-item-card:: :octicon:`rocket` Creating services
      :link: module-pw_rpc-services
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      **For Application Developers.**
      How to define ``.proto`` services, generate C++ code, implement unary
      and streaming methods, and write unit tests.

.. grid:: 3

   .. grid-item-card:: :octicon:`code-square` C++ client & server
      :link: module-pw_rpc-cpp
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Core C++ runtime mechanics: channels, call objects, synchronous call
      wrappers, concurrency rules, and test fixtures.

   .. grid-item-card:: :octicon:`file-code` C++ with Nanopb
      :link: module-pw_rpc_nanopb
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Lightweight C struct message generator (recommended for embedded C++).

   .. grid-item-card:: :octicon:`file-code` C++ with pw_protobuf
      :link: module-pw_rpc_pw_protobuf
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Type-safe pure C++ message generator.

.. grid:: 3

   .. grid-item-card:: :octicon:`cpu` C++ with raw RPC
      :link: module-pw_rpc-raw
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Zero-copy byte buffer RPCs and method fallback mechanics.

   .. grid-item-card:: :octicon:`terminal` Python client
      :link: module-pw_rpc-py
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Python client library, custom channels, and ``pw_console`` tools.

   .. grid-item-card:: :octicon:`globe` TypeScript client
      :link: module-pw_rpc-ts
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      WebSerial, WebUSB, and browser/Node.js client library.

.. grid:: 3

   .. grid-item-card:: :octicon:`device-mobile` Java client
      :link: module-pw_rpc-java
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Android and JVM client library in ``dev.pigweed.pw_rpc``.

   .. grid-item-card:: :octicon:`meter` Benchmarking
      :link: module-pw_rpc-benchmarking
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Tools for measuring throughput, latency, and fuzzer testing.

   .. grid-item-card:: :octicon:`info` Wire protocol
      :link: module-pw_rpc-protocol
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Packet wire format and envelope protocol specification.

.. toctree::
   :maxdepth: 1
   :hidden:

   setup
   services
   cpp
   nanopb/docs
   pwpb/docs
   raw/docs
   pw://cc-api-ref
   py/docs
   ts/docs
   java/docs
   benchmarking
   protocol
   design
   Serial & HDLC example <pw://pw_hdlc/rpc_example/docs.html>
