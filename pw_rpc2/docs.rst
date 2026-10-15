.. _module-pw_rpc2:

=======
pw_rpc2
=======
.. pigweed-module::
   :name: pw_rpc2

``pw_rpc2`` is the evolution of Pigweed's Remote Procedure Call (RPC) framework
for embedded systems, redesigned from the ground up for asynchronous operations.

Built on :ref:`cooperative async primitives <module-pw_async2>` and
:ref:`transport abstractions <module-pw_transport>`, ``pw_rpc2`` provides
end-to-end backpressure, cooperatively multiplexed RPC calls, a stateful
connection model, and zero-copy packet handling.

.. note::

   ``pw_rpc2`` is under active development. C++ support includes a server and
   client, with support for making calls and defining services with either raw
   or PWPB APIs, manual async state machines or coroutines.

   Other language runtimes are planned.

----------
Highlights
----------
* **Stateful connections**: An active ``Socket`` replaces RPC1's static
  ``Channel``, ensuring that RPCs know when a peer is listening or disconnects.
* **End-to-end backpressure**: Asynchronous buffer reservations from the
  transport layer pause tasks until space becomes available, naturally
  integrating backpressure into every RPC call.
* **Zero-copy payloads**: Using :ref:`pw_buf <module-pw_buf>`, all reads and
  writes operate directly on views of transport buffers, without intermediate
  staging or copying.
* **Cooperative concurrency**: All RPC calls run in their own async2
  :ref:`Task <module-pw_async2-tasks>`, multiplexing operations on a shared
  dispatcher thread, and avoiding the global lock contention of RPC1.

-----------------
C++ documentation
-----------------
.. grid:: 2

   .. grid-item-card:: :octicon:`code-square` C++ client and server
      :link: module-pw_rpc2-cpp
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      **C++ runtime mechanics.**
      How to set up and use the C++ client and server, RPC call and
      error semantics, and protobuf codegen.

   .. grid-item-card:: :octicon:`stack` C++ examples
      :link: module-pw_rpc2-examples
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      **End-to-end C++ example suites.**
      Examples covering common RPC patterns and use cases.

-------------------------
Protocol and architecture
-------------------------
.. grid:: 1

   .. grid-item-card:: :octicon:`file-code` Wire protocol
      :link: module-pw_rpc2-protocol
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      **Binary framing and control plane.**
      Packet header layout, packet types, the opening handshake, and regular
      message exchange.

------------------------
Evaluation and migration
------------------------
.. grid:: 2

   .. grid-item-card:: :octicon:`info` Why RPC2?
      :link: module-pw_rpc2-why
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      **Architectural departures from RPC1.**
      Why RPC2 was created: the history behind RPC, pain points with RPC1,
      and modernization and evolution.

   .. grid-item-card:: :octicon:`git-compare` RPC1 vs. RPC2
      :link: module-pw_rpc2-comparison
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      **Choosing the right RPC framework.**
      Trade-offs between RPC1 and RPC2, with a guide on when to use which.

.. toctree::
   :maxdepth: 1
   :hidden:

   C++ client and server <cpp>
   C++ examples <examples>
   Wire protocol <protocol>
   Why RPC2? <why>
   RPC1 vs. RPC2 <comparison>
