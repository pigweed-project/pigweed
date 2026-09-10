.. _module-pw_rpc-java:

===========
Java client
===========
.. pigweed-module-subpage::
   :name: pw_rpc

``pw_rpc`` provides a Java / Kotlin client implementation under the package
``dev.pigweed.pw_rpc`` for Android applications, desktop JVM tools, and
automated test frameworks.

-----------
Quick start
-----------
The Java client interacts with an RPC server through **Channels**, **Services**,
and **MethodClients**.

1. Create Channels and Client
=============================
Define a channel with a send callback (e.g. over USB, BLE, or TCP sockets), and
instantiate the ``dev.pigweed.pw_rpc.Client``:

.. literalinclude:: ../examples/DocsExample.java
   :language: java
   :start-after: [pw_rpc-java-create-client]
   :end-before: [pw_rpc-java-create-client]

2. Route Incoming Packets
=========================
When your transport receives incoming packets from the device, pass them into
``client.processPacket()``:

.. literalinclude:: ../examples/DocsExample.java
   :language: java
   :start-after: [pw_rpc-java-route-packets]
   :end-before: [pw_rpc-java-route-packets]

3. Invoke RPC Methods
=====================

Unary RPC
---------
Invoke a unary method using a ``dev.pigweed.pw_rpc.StreamObserver``:

.. literalinclude:: ../examples/DocsExample.java
   :language: java
   :start-after: [pw_rpc-java-unary-call]
   :end-before: [pw_rpc-java-unary-call]

Server Streaming RPC
--------------------
A server streaming RPC invokes the ``onNext`` callback for each streamed response
packet until the stream completes:

.. literalinclude:: ../examples/DocsExample.java
   :language: java
   :start-after: [pw_rpc-java-server-streaming-call]
   :end-before: [pw_rpc-java-server-streaming-call]

Client & Bidirectional Streaming RPCs
-------------------------------------
For client and bidirectional streaming calls, the returned call object allows
streaming request messages:

.. literalinclude:: ../examples/DocsExample.java
   :language: java
   :start-after: [pw_rpc-java-streaming-call]
   :end-before: [pw_rpc-java-streaming-call]

------------------------
Future-based invocation
------------------------
``pw_rpc`` also supports ``ListenableFuture`` wrappers for asynchronous Java
code:

.. literalinclude:: ../examples/DocsExample.java
   :language: java
   :start-after: [pw_rpc-java-future-call]
   :end-before: [pw_rpc-java-future-call]
