.. _module-pw_rpc:

======
pw_rpc
======
.. pigweed-module::
   :name: pw_rpc

``pw_rpc`` provides an embedded-friendly system for defining and using remote
procedure calls (RPCs) over arbitrary serial or packet-oriented transports.
Services and their request/response messages are defined in shared protobuf
files.

``pw_rpc`` supports several languages, and both ``Nanopb`` and ``pw_protobuf``
code generation in C++.

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

.. grid:: 2

   .. grid-item-card:: :octicon:`rocket` Quickstart & guides
      :link: module-pw_rpc-guides
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Check out the ``pw_rpc`` quickstart for more explanation of the code above.
      The guides answer common questions such as whether
      to use ``proto2`` or ``proto3`` syntax.

   .. grid-item-card:: :octicon:`code-square` C++ server and client
      :link: module-pw_rpc-cpp
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      C++ server and client library API guides.

.. grid:: 2

   .. grid-item-card:: :octicon:`info` Packet protocol
      :link: module-pw_rpc-protocol
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      A detailed description of the ``pw_rpc`` packet protocol.

   .. grid-item-card:: :octicon:`info` Design
      :link: module-pw_rpc-design
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      An overview of the RPC call lifecycle, naming conventions,
      and the ``pw_rpc`` roadmap.

.. grid:: 2

   .. grid-item-card:: :octicon:`code-square` Python client
      :link: module-pw_rpc-py
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Python client library API reference.

   .. grid-item-card:: :octicon:`code-square` TypeScript client
      :link: module-pw_rpc-ts
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      TypeScript client library API guide.

.. grid:: 2

   .. grid-item-card:: :octicon:`code-square` Nanopb codegen
      :link: module-pw_rpc_nanopb
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Nanopb codegen library API guide.

   .. grid-item-card:: :octicon:`code-square` pw_protobuf codegen
      :link: module-pw_rpc_pw_protobuf
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      ``pw_protobuf`` codegen library API guide.

.. toctree::
   :maxdepth: 1
   :hidden:

   guides
   libraries
   protocol
   design
   HDLC example <pw://pw_hdlc/rpc_example/docs.html>
