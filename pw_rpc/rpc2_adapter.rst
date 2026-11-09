.. _module-pw_rpc-rpc2-adapter:
.. _module-pw_rpc-setup-rpc2:

===============
pw_rpc2 adapter
===============
.. pigweed-module-subpage::
   :name: pw_rpc

Projects using :ref:`module-pw_rpc2` can serve existing ``pw_rpc`` C++ services
(Nanopb, ``pw_protobuf``, or raw) from an :cc:`rpc2::Server <pw::rpc2::Server>`
without modifying the service implementations or ``.proto`` files. ``pw_rpc2``
clients generated from the same ``.proto`` file call the adapted services just
like native ``pw_rpc2`` services.

The adapter is a transitional tool with code size, memory, and performance
costs; see :ref:`module-pw_rpc-rpc2-adapter-tradeoffs` and migrate services to
native ``pw_rpc2`` whenever possible.

------------------
Build system setup
------------------
The adapter is defined in ``pw_rpc/rpc2_adapter.h``. Add the adapter dependency
to your build target:

* **Bazel**: ``//pw_rpc:rpc2_adapter``
* **GN**: ``$dir_pw_rpc:rpc2_adapter``
* **CMake**: ``pw_rpc.rpc2_adapter``

Because the adapter interacts with the :cc:`pw::rpc::Server` across multiple
threads, :cc:`PW_RPC_USE_GLOBAL_MUTEX` must be enabled (``1``, the default).

-------------------------
Choosing a server adapter
-------------------------
``pw_rpc`` provides two server adapter classes depending on whether your system
also runs a ``pw_rpc`` server.

Use ``pw_rpc`` services in ``pw_rpc2`` only (``Rpc2Adapter``)
=============================================================
:cc:`Rpc2Adapter <pw::rpc::Rpc2Adapter>` lets you run ``pw_rpc`` services in
``pw_rpc2`` without an existing ``pw_rpc`` server. Register services with it
using :cc:`Rpc2ServiceFor <pw::rpc::Rpc2ServiceFor>`.

.. literalinclude:: examples/use_rpc_services_in_rpc2.cc
   :language: c++
   :dedent:
   :start-after: [pw_rpc-examples-rpc2-adapter]
   :end-before: [pw_rpc-examples-rpc2-adapter]

Integrate with an existing ``pw_rpc`` server (``Rpc2ServerAdapter``)
====================================================================
Use :cc:`Rpc2ServerAdapter <pw::rpc::Rpc2ServerAdapter>` when your firmware
already runs a :cc:`pw::rpc::Server` and you want to expose services on that
server to ``pw_rpc2`` clients as well.

Unless :cc:`PW_RPC_DYNAMIC_ALLOCATION` is enabled, the :cc:`pw::rpc::Server`
must have unassigned channels for the adapter to use. Each ``pw_rpc2``
connection opens a new channel. The server's other channels continue operating
normally, so a single service instance can serve both ``pw_rpc`` and ``pw_rpc2``
clients concurrently.

.. literalinclude:: examples/use_rpc_services_in_rpc2.cc
   :language: c++
   :dedent:
   :start-after: [pw_rpc-examples-rpc2-server-adapter]
   :end-before: [pw_rpc-examples-rpc2-server-adapter]

-----------------
Adapting services
-----------------
Wrap each ``pw_rpc`` service as an :cc:`rpc2::Service <pw::rpc2::Service>` using
either :cc:`Rpc2ServiceFor <pw::rpc::Rpc2ServiceFor>` or
:cc:`Rpc2ServiceAdapter <pw::rpc::Rpc2ServiceAdapter>`, then pass it to
:cc:`rpc2::Server::RegisterService <pw::rpc2::Server::RegisterService>`.

Construct and own a service (``Rpc2ServiceFor``)
================================================
:cc:`Rpc2ServiceFor <pw::rpc::Rpc2ServiceFor>` constructs and owns a ``pw_rpc``
service. Constructor arguments after the adapter are forwarded to the service's
constructor. Access the underlying service via :cc:`service()
<pw::rpc::Rpc2ServiceFor::service>`.

.. literalinclude:: examples/use_rpc_services_in_rpc2.cc
   :language: c++
   :dedent:
   :start-after: [pw_rpc-examples-rpc2-service-for]
   :end-before: [pw_rpc-examples-rpc2-service-for]

Wrap an existing service instance (``Rpc2ServiceAdapter``)
==========================================================
:cc:`Rpc2ServiceAdapter <pw::rpc::Rpc2ServiceAdapter>` wraps an existing
``pw_rpc`` service that is registered with a shared :cc:`rpc::Server
<pw::rpc::Server>`.

.. literalinclude:: examples/use_rpc_services_in_rpc2.cc
   :language: c++
   :dedent:
   :start-after: [pw_rpc-examples-rpc2-service-adapter]
   :end-before: [pw_rpc-examples-rpc2-service-adapter]

.. _module-pw_rpc-rpc2-adapter-status-codes:

Non-``OK`` status codes and response payloads
=============================================
In ``pw_rpc``, unary and client streaming RPCs return both a :cc:`pw::Status`
and a response protobuf, whereas ``pw_rpc2`` has no application status codes and
returns either a response message (with ``kOk``) or an error status with no
payload.

By default, finishing a unary or client streaming ``pw_rpc`` call with any
non-``OK`` status ends the ``pw_rpc2`` call with ``kCancelled`` and discards any
response payload (for server and bidirectional streaming calls, all queued
``SERVER_STREAM`` messages are delivered first before ending with
``kCancelled``).

If a unary or client streaming method encodes failure inside its response
message alongside a non-``OK`` status, pass :cc:`DeliverNonOkResponses()
<pw::rpc::DeliverNonOkResponses>` to :cc:`Rpc2ServiceFor
<pw::rpc::Rpc2ServiceFor>` or :cc:`Rpc2ServiceAdapter
<pw::rpc::Rpc2ServiceAdapter>` so the adapter delivers the response payload to
``pw_rpc2`` clients instead of cancelling the call. Pass specific generated
method functions to opt in individual methods, or call
``DeliverNonOkResponses()`` with no template arguments to opt in all unary and
client streaming methods of a service (for ``Rpc2ServiceFor``, any arguments
after ``DeliverNonOkResponses()`` are forwarded to the service's constructor):

.. literalinclude:: examples/use_rpc_services_in_rpc2.cc
   :language: c++
   :dedent:
   :start-after: [pw_rpc-examples-rpc2-non-ok-service-for]
   :end-before: [pw_rpc-examples-rpc2-non-ok-service-for]

.. literalinclude:: examples/use_rpc_services_in_rpc2.cc
   :language: c++
   :dedent:
   :start-after: [pw_rpc-examples-rpc2-non-ok-service-adapter]
   :end-before: [pw_rpc-examples-rpc2-non-ok-service-adapter]

.. warning::

   ``pw_rpc2`` clients receive ``kOk`` alongside the serialized response for
   **every** non-``OK`` status returned by an opted-in method, including early
   error returns that leave the response protobuf default-initialized. Opted-in
   methods must populate error state in the response message on every non-``OK``
   return path. Only use the service-wide ``DeliverNonOkResponses()`` form when
   every unary and client streaming method in the service follows this contract.

------------------------------------
Behavior differences from ``pw_rpc``
------------------------------------
* **Channels**: Each ``pw_rpc2`` connection gets its own ``pw_rpc`` channel ID
  (starting at :cc:`first_channel_id
  <pw::rpc::Rpc2AdapterConfig::first_channel_id>`, 100 by default, up to
  :cc:`max_channels <pw::rpc::Rpc2AdapterConfig::max_channels>`). When a
  connection closes, its channel remains open and is reused by the next
  ``pw_rpc2`` connection, so per-channel state (such as a ``pw_log_rpc`` drain)
  carries over. Do not close the adapter's channels or use
  ``SetDefaultChannelOutput()`` on a shared server.
* **Threading**: Handlers and stream callbacks for ``pw_rpc2`` calls run on the
  adapter's worker thread. A service shared with other ``pw_rpc`` channels may
  be invoked from both threads concurrently.
* **Allocator**: The adapter allocates per-connection state and queued packets
  from its :cc:`pw::Allocator` across threads (under an internal lock). Do not
  share a non-thread-safe allocator with the
  :cc:`rpc2::Server <pw::rpc2::Server>`.
* **Status codes**: ``pw_rpc2`` does not have application status codes. By
  default, finishing a ``pw_rpc`` call with any non-``OK`` status ends the
  ``pw_rpc2`` call with ``kCancelled`` and discards any final response payload;
  see :ref:`module-pw_rpc-rpc2-adapter-status-codes`.
* **Outbound stream buffering**: The adapter queues up to
  :cc:`max_queued_messages_per_call
  <pw::rpc::Rpc2AdapterConfig::max_queued_messages_per_call>` (8 by default)
  outgoing stream messages per call. Writes fail with ``UNKNOWN`` if the queue
  is full or the adapter's allocator is exhausted.
* **Client stream completion**: Client and bidirectional streaming handlers must
  enable :cc:`PW_RPC_COMPLETION_REQUEST_CALLBACK` and use
  ``set_on_completion_requested`` to detect when the client closes its stream.
* **Resource limits**: New calls fail with ``kUnavailable`` if the adapter
  reaches :cc:`max_channels <pw::rpc::Rpc2AdapterConfig::max_channels>`, has no
  free channel slot on the ``pw_rpc`` server, or cannot allocate memory. An
  active call that runs out of memory ends with an error.
* **Teardown**: Close the :cc:`rpc2::Server <pw::rpc2::Server>`, then call
  :cc:`Rpc2ServerAdapter::Stop <pw::rpc::Rpc2ServerAdapter::Stop>` before
  destroying adapted services or the adapter. ``Stop()`` requires
  ``PW_THREAD_JOINING_ENABLED = 1``; without thread joining, the adapter must
  never be destroyed (for example, declare it with
  :cc:`pw::RuntimeInitGlobal`).

.. _module-pw_rpc-rpc2-adapter-tradeoffs:

--------------------------
Tradeoffs & migration path
--------------------------
The adapter is intended as a transitional tool to enable incremental migration
to :ref:`module-pw_rpc2`, or to reuse pre-existing ``pw_rpc`` services before
native ``pw_rpc2`` versions are available. Migrate services fully to
``pw_rpc2`` whenever possible.

Running ``pw_rpc`` services through the adapter carries significant costs
compared to native ``pw_rpc2`` services:

* **Larger code size (flash)**: The binary must link both the ``pw_rpc`` and
  ``pw_rpc2`` server implementations, plus the adapter layer that bridges them.
* **Extra thread and RAM overhead**: Because ``pw_rpc`` method handlers are
  synchronous and may block, the adapter runs a dedicated worker thread (which
  requires its own stack) so handlers cannot stall the ``pw_rpc2``
  :ref:`module-pw_async2` dispatcher. The adapter also dynamically allocates
  per-connection state and copies of in-flight packets from a
  :cc:`pw::Allocator`, in addition to ``pw_rpc``'s static call objects and
  encoding buffer.
* **Lower performance and higher latency**: Native ``pw_rpc2`` services run
  cooperatively on the dispatcher thread with zero-copy :ref:`module-pw_buf`
  payloads and minimal locking. Every call to an adapted ``pw_rpc`` service
  allocates and copies packet payloads into a work queue, acquires both the
  adapter's mutex and ``pw_rpc``'s global mutex, and context-switches between
  the ``pw_rpc2`` dispatcher thread and the adapter's worker thread.
* **Limited flow control and backpressure**: ``pw_rpc`` writers cannot suspend a
  task while waiting for transport capacity. Instead of end-to-end cooperative
  backpressure, the adapter buffers up to a fixed number of outbound stream
  messages per call (:cc:`max_queued_messages_per_call
  <pw::rpc::Rpc2AdapterConfig::max_queued_messages_per_call>`) and fails writes
  once that queue is full.
* **Reduced status fidelity**: ``pw_rpc2`` does not have application status
  codes in final responses. By default non-``OK`` statuses are collapsed into
  ``kCancelled``. If a unary or client streaming method is configured with
  :cc:`DeliverNonOkResponses() <pw::rpc::DeliverNonOkResponses>`, non-``OK``
  statuses are ignored completely.

Once a service no longer needs to serve ``pw_rpc`` clients, rewrite it as a
native ``pw_rpc2`` service to eliminate these overheads and take full advantage
of ``pw_rpc2``.
