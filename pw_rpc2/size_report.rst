.. _module-pw_rpc2-size_report:

====================
Extended size report
====================
.. pigweed-module-subpage::
   :name: pw_rpc2

``pw_rpc2`` is built on :ref:`module-pw_async2`, :ref:`module-pw_allocator`,
:ref:`module-pw_buf`, and :ref:`module-pw_transport` to provide cooperative
multiplexed calls, connection-oriented transport management, and end-to-end
backpressure. Because its binary size impact depends on whether a device acts
as a server, a client, or both—as well as which codegen API (raw buffers or
``pw_protobuf`` message structs) and execution model (futures or C++20
coroutines) are used—a series of size reports are provided below.

.. _module-pw_rpc2-size_report-overview:

----------------------------
Core runtime and incremental
----------------------------
The core size report measures the cost of adding :cc:`Server <pw::rpc2::Server>`
and :cc:`Client <pw::rpc2::Client>` to an embedded system that already includes
a :cc:`async2::BasicDispatcher <pw::async2::BasicDispatcher>`, a
:cc:`allocator::FirstFitAllocator <pw::allocator::FirstFitAllocator>`, and a
:cc:`transport::ReliableSocket <pw::transport::ReliableSocket>`
transport implementation.

The table below reports:

* **Core server cost**: A :cc:`Server <pw::rpc2::Server>` accepting connections
  from a transport listener, performing the opening handshake, and dispatching
  packets to a registered ``raw`` unary :cc:`Service <pw::rpc2::Service>`.
* **Incremental service cost**: Registering a second ``raw`` unary service on an
  existing server.
* **Core client cost**: A :cc:`Client <pw::rpc2::Client>` performing the
  opening handshake over a transport socket and invoking a ``raw`` unary RPC via
  a generated client.
* **Incremental call cost**: Invoking a second ``raw`` unary RPC method from an
  existing client.
* **Combined server and client cost**: Running both :cc:`Server
  <pw::rpc2::Server>` and :cc:`Client <pw::rpc2::Client>` in the same binary,
  which share packet framing, the opening handshake state machine, connection
  task management, and active call multiplexing.

.. include:: size_report/rpc2_size_report

.. note::

   ``pw_rpc2`` allocates per-connection and per-call state dynamically from the
   configured :cc:`pw::Allocator` while connections and RPCs are active, freeing
   that memory as soon as calls and connections close. The static ``RAM`` delta
   reflects only the global control spinlock.

.. _module-pw_rpc2-size_report-codegen:

--------------------------------
Service and client codegen costs
--------------------------------
``pw_rpc2`` supports two protobuf codegen backends and two asynchronous
programming models:

* **Raw RPCs** (``raw_rpc2_proto_library``): Pass zero-copy :cc:`pw::ConstBuf`
  payloads directly to and from the transport without linking a protobuf encoder
  or decoder.
* **PWPB RPCs** (``pwpb_rpc2_proto_library``): Automatically serialize and
  deserialize :ref:`module-pw_protobuf` ``Message`` structs into reserved
  transport buffers.
* **Manual futures vs. C++20 coroutines**: Services can implement RPCs either as
  hand-written ``<Method>Future`` state machines or as ``pw::async2::Coro<void>``
  coroutines.

The codegen comparisons below use the following ``UnarySizeReport``
(single unary RPC) and ``SizeReport`` (all four RPC types: unary, server
streaming, client streaming, and bidirectional streaming) service definitions:

.. literalinclude:: size_report/size_report.proto
   :language: protobuf
   :start-after: // DOCSTAG: [pw_rpc2-size-report-proto]
   :end-before: // DOCSTAG: [pw_rpc2-size-report-proto]

Generated service comparison
============================
The following table breaks out the one-time cost of :ref:`module-pw_protobuf`
table-based ``Message`` serialization and ``pw_rpc2`` PWPB helpers separately
from the incremental cost of adding a generated service over the core server
baseline (see :cs:`pw_rpc2/size_report/service_codegen_size_report.cc` for the
service implementations):

1. **One-time** ``pw_protobuf`` **table encoder/decoder and** ``pw_rpc2``
   **PWPB serialization**: One-time cost of linking
   :cc:`pw::protobuf::StreamEncoder`, :cc:`pw::protobuf::StreamDecoder`, and
   ``pw_rpc2`` PWPB serialization helpers when using ``pwpb`` RPCs (not needed
   for ``raw`` RPCs or if already linked elsewhere in the binary).
2. **Single unary RPC service** (``UnarySizeReport::Service``):
   Incremental cost of adding a 1-method unary service in ``raw`` vs. ``pwpb``
   (with futures or C++20 coroutines).
3. **Full 4-method service** (``SizeReport::Service``): Incremental cost
   of adding all four RPC types (including streaming ``Reader`` and ``Writer``
   support) in ``raw`` vs. ``pwpb`` (with futures or C++20 coroutines).

.. include:: size_report/service_codegen_size_report

Generated client comparison
===========================
The following table breaks out the one-time :ref:`module-pw_protobuf`
table-based ``Message`` encoder/decoder and ``pw_rpc2`` PWPB serialization cost
separately from the incremental cost of calling generated
``SizeReport::Client`` methods over the core client baseline (see
:cs:`pw_rpc2/size_report/client_codegen_size_report.cc` for the client
implementation):

1. **One-time** ``pw_protobuf`` **table encoder/decoder and** ``pw_rpc2``
   **PWPB serialization**: One-time table serialization cost when using ``pwpb``
   client calls.
2. **Single unary RPC call**: Incremental cost of calling
   ``SizeReport::Client::MeasureUnary`` via ``raw`` vs. ``pwpb``.
3. **All four RPC call types**: Incremental cost of calling unary, server
   streaming, client streaming, and bidirectional streaming methods via ``raw``
   vs. ``pwpb``.

.. include:: size_report/client_codegen_size_report
