.. _module-pw_rpc2-python:

=============
Python client
=============
.. pigweed-module-subpage::
   :name: pw_rpc2

The ``pw_rpc2`` Python package (``//pw_rpc2/py:pw_rpc2``) provides a host client
for ``pw_rpc2`` servers:

* **Synchronous and asyncio APIs**: :py:class:`pw_rpc2.Client` provides
  thread-safe blocking calls; :py:class:`pw_rpc2.aio.Client` provides native
  ``asyncio`` coroutines over the same connection engine.
* **Runtime service generation**: Service and method clients are built at
  runtime from compiled ``_pb2`` modules or protobuf descriptors—no RPC code
  generation step is required.
* **Unary and streaming RPCs**: Supports unary, server streaming, client
  streaming, and bidirectional streaming calls, with dedicated handle types for
  each.

For class and function signatures, see the
:ref:`Python API reference <module-pw_rpc2-python-api>`.

----------
Quickstart
----------
Pass compiled ``_pb2`` modules when connecting to a server, then call methods
under ``client.rpcs`` by walking the protobuf package hierarchy. The examples
below call the ``Echo`` service from the
:ref:`C++ Echo example <module-pw_rpc2-examples>` (``server_coro`` listens on
port ``8000`` by default):

.. tab-set::

   .. tab-item:: Synchronous
      :sync: sync

      .. code-block:: py

         import pw_rpc2
         from pw_transport import framed_tcp
         from echo_pb import echo_pb2

         with pw_rpc2.Client(
             framed_tcp.connect('localhost', 8000), [echo_pb2]
         ) as client:
             reply = client.rpcs.examples.Echo.Echo(msg='hello')
             print(reply.msg)

   .. tab-item:: Asyncio
      :sync: async

      .. code-block:: py

         import asyncio
         from pw_rpc2 import aio
         from pw_transport import framed_tcp
         from echo_pb import echo_pb2


         async def main() -> None:
             async with await aio.Client.connect(
                 lambda: framed_tcp.connect('localhost', 8000), [echo_pb2]
             ) as client:
                 reply = await client.rpcs.examples.Echo.Echo(msg='hello')
                 print(reply.msg)


         asyncio.run(main())

.. _module-pw_rpc2-python-connecting:

----------------------
Connecting to a server
----------------------
Each client owns a private :py:class:`pw_transport.ReliableSocket` connection to
a single server (see :ref:`pw_transport <module-pw_transport-python>`):

* :py:class:`pw_rpc2.Client` takes an open
  :py:class:`~pw_transport.ReliableSocket`.
* :py:meth:`pw_rpc2.aio.Client.connect` takes either an open
  :py:class:`~pw_transport.ReliableSocket` or a zero-argument callable returning
  one. A callable runs on a worker thread so opening the socket does not block
  the event loop. (:py:class:`pw_rpc2.aio.Client` has no public constructor
  because connecting awaits the opening handshake.)

Handshake and lifecycle
=======================
Before returning, the client performs the ``pw_rpc2``
:ref:`three-way handshake <module-pw_rpc2-protocol-connections>`
(``SYN`` → ``SYN_ACK`` → ``ACK``). If the server sends an invalid reply, selects
an unsupported protocol version, disconnects, or does not finish within
``handshake_timeout`` (default :py:data:`~pw_rpc2.DEFAULT_HANDSHAKE_TIMEOUT`,
``5.0`` seconds), the client closes the socket and raises
:py:class:`pw_rpc2.HandshakeError`.

Closing the client—with ``client.close()``, ``await client.close()``, or a
``with`` / ``async with`` block—closes the socket, joins background worker
threads, and ends any active calls locally with
:py:attr:`pw_rpc2.RpcStatus.CLOSED` without sending packets to the server.
``client.closed`` reports whether the connection has closed.

The client's background threads are daemon threads, so a client that is never
closed does not keep the Python interpreter from exiting.

Built-in sockets
================
``pw_rpc2`` accepts any :py:class:`pw_transport.ReliableSocket`, including:

* :py:func:`pw_transport.framed_tcp.connect` (``//pw_transport/py:framed_tcp``)
  for host tools and tests over TCP. It frames datagrams with a 4-byte
  little-endian length prefix matching C++'s
  ``pw::transport::FramedTcpListener`` and
  ``pw::transport::FramedTcpConnector``.
* :py:func:`pw_transport.forwarding_socket.socket_pair`
  (``//pw_transport/py:forwarding_socket``) for in-process testing.
* Custom :ref:`ReliableSocket subclasses <module-pw_rpc2-python-transports>`
  over serial, USB, BLE, or other framed links.

.. tab-set::

   .. tab-item:: Synchronous
      :sync: sync

      .. code-block:: py

         client = pw_rpc2.Client(
             framed_tcp.connect('127.0.0.1', 8000, timeout=5.0),
             protos=[echo_pb2],
             default_timeout=10.0,
             handshake_timeout=5.0,
         )

   .. tab-item:: Asyncio
      :sync: async

      .. code-block:: py

         client = await aio.Client.connect(
             lambda: framed_tcp.connect('127.0.0.1', 8000, timeout=5.0),
             protos=[echo_pb2],
             default_timeout=10.0,
             handshake_timeout=5.0,
         )

.. _module-pw_rpc2-python-services:

-----------------------------------
Services, descriptors, and requests
-----------------------------------
Loading service descriptors
===========================
The ``protos`` argument on :py:class:`pw_rpc2.Client`,
:py:meth:`pw_rpc2.aio.Client.connect`, and ``client.add_services()`` accepts any
iterable of :py:data:`pw_rpc2.ProtoSource` items:

* Generated ``*_pb2`` modules
* ``google.protobuf.descriptor.FileDescriptor`` objects
* ``google.protobuf.descriptor.ServiceDescriptor`` objects

To load schemas built dynamically in a ``DescriptorPool``, look up their
``FileDescriptor`` or ``ServiceDescriptor`` objects (for example, with
``pool.FindFileByName(...)``) and pass them in ``protos``. Calling
``client.add_services(protos)`` registers more services on an open client; a
service with the same full name as an existing one replaces it without
disturbing other service clients.

Navigating ``client.rpcs``
==========================
Registered services are exposed on ``client.rpcs``
(:py:class:`pw_rpc2.Services`):

* **Attribute access** walks protobuf packages to ``<Service>.<Method>``:

  .. code-block:: py

     echo_method = client.rpcs.examples.Echo.Echo

  Services without a protobuf ``package`` are attributes directly on
  ``client.rpcs.<Service>``.

* **Indexing** looks up services by fully qualified name and methods by
  unqualified name:

  .. code-block:: py

     service = client.rpcs['examples.Echo']
     echo_method = service['Echo']

* **Iteration and introspection**: Both ``client.rpcs`` and each
  :py:class:`pw_rpc2.ServiceClient` support ``in``, ``len()``, and iteration;
  ``client.methods()`` yields every method client across all services:

  .. code-block:: py

     for method in client.methods():
         print(method.full_name, method.type)

  :py:class:`~pw_rpc2.ServiceClient` exposes ``name``, ``full_name``,
  ``package``, and ``descriptor``. Method clients expose ``name``,
  ``full_name``, ``type`` (:py:class:`pw_rpc2.MethodType`), ``descriptor``,
  ``request`` (request message class), and ``response`` (response message
  class).

Passing requests
================
Method clients and streaming ``call.write()`` accept a request in either of two
forms (never mixed in the same call):

1. **Keyword arguments** for the request's fields:

   .. code-block:: py

      reply = client.rpcs.examples.Echo.Echo(msg='hello')

2. **A protobuf message** as a single positional argument (including a message
   class from another ``DescriptorPool`` with the same
   ``DESCRIPTOR.full_name``):

   .. code-block:: py

      reply = client.rpcs.examples.Echo.Echo(echo_pb2.EchoMessage(msg='hello'))

Method clients and ``call.write()`` synthesize Python signatures and docstrings
from the protobuf schema, so ``help()`` and IDE tab completion display request
fields, types, and default values.

.. _module-pw_rpc2-python-calls:

----------------
Making RPC calls
----------------
Each RPC cardinality has a dedicated call class exposing only the operations
valid for that mode:

.. list-table::
   :header-rows: 1
   :widths: 24 38 38

   * - RPC type
     - Calling ``method(...)`` returns
     - Call handle class
   * - Unary
     - Response message (``aio``: coroutine)
     - :py:class:`~pw_rpc2.UnaryCall` (via ``method.invoke()``)
   * - Server streaming
     - Call handle immediately
     - :py:class:`~pw_rpc2.ServerStreamingCall`
   * - Client streaming
     - Call handle immediately
     - :py:class:`~pw_rpc2.ClientStreamingCall`
   * - Bidirectional streaming
     - Call handle immediately
     - :py:class:`~pw_rpc2.BidirectionalStreamingCall`

.. note::

   In ``pw_rpc2.aio``, calling a **unary** method returns a coroutine that you
   ``await``. Calling a **streaming** method (or ``method.invoke()``) never
   blocks the event loop—it queues the opening packet on a sender thread and
   returns the call handle directly without ``await``.

Every call handle provides ``call.completed`` (``bool``), ``call.status``
(:py:class:`~pw_rpc2.RpcStatus`, or ``None`` while active or after a local
transport/decode error), and ``call.cancel()``. Using a call handle as a context
manager (``with call:`` or ``async with call:``) cancels the RPC on exit if it
is still active.

Unary RPCs
==========
Calling a unary method sends the request and waits for the single response. If
the wait is interrupted—by :py:class:`pw_rpc2.RpcTimeout`,
``KeyboardInterrupt``, or cancelling an awaiting ``asyncio.Task``—the client
cancels the RPC on the server before propagating the exception.

To start a unary RPC without waiting immediately, call ``method.invoke(...)`` to
get a :py:class:`pw_rpc2.UnaryCall` (or :py:class:`pw_rpc2.aio.UnaryCall`), then
call ``call.response()`` (or ``await call.response()``):

.. tab-set::

   .. tab-item:: Synchronous
      :sync: sync

      .. code-block:: py

         echo = client.rpcs.examples.Echo

         # Blocking unary call:
         reply = echo.Echo(msg='hello', rpc2_timeout=2.0)

         # Concurrent unary calls with .invoke():
         with (
             echo.Echo.invoke(msg='first') as call1,
             echo.Echo.invoke(msg='second') as call2,
         ):
             reply1 = call1.response(timeout=2.0)
             reply2 = call2.response(timeout=2.0)

   .. tab-item:: Asyncio
      :sync: async

      .. code-block:: py

         echo = client.rpcs.examples.Echo

         # Coroutine unary call:
         reply = await echo.Echo(msg='hello', rpc2_timeout=2.0)

         # Concurrent unary calls with asyncio.gather:
         reply1, reply2 = await asyncio.gather(
             echo.Echo(msg='first'),
             echo.Echo(msg='second'),
         )

         # Or non-blocking start with .invoke():
         async with echo.Echo.invoke(msg='hello') as call:
             reply = await call.response(timeout=2.0)

Server streaming RPCs
=====================
Calling a server streaming method starts the RPC and returns a
:py:class:`pw_rpc2.ServerStreamingCall` (or
:py:class:`pw_rpc2.aio.ServerStreamingCall`). Iterate over the call to consume
responses until the server ends the stream, or fetch one message at a time with
``call.read(timeout=...)``, which returns ``None`` at end-of-stream. Wrap the
call in ``with`` / ``async with`` so breaking out of the loop early cancels the
stream on the server:

.. tab-set::

   .. tab-item:: Synchronous
      :sync: sync

      .. code-block:: py

         with client.rpcs.examples.Echo.Repeat(msg='hi', count=3) as call:
             for response in call:
                 print(response.msg)

   .. tab-item:: Asyncio
      :sync: async

      .. code-block:: py

         async with client.rpcs.examples.Echo.Repeat(msg='hi', count=3) as call:
             async for response in call:
                 print(response.msg)

Unlike the :ref:`C++ client <module-pw_rpc2-cpp>`—which holds at most one unread
message per call so slow readers apply transport backpressure—the Python client
buffers incoming stream messages in memory without a fixed limit so its receiver
thread keeps draining the socket. Cancel streams you no longer need so the
server stops sending.

Client streaming RPCs
=====================
Calling a client streaming method opens the stream with no initial request and
returns a :py:class:`pw_rpc2.ClientStreamingCall` (or
:py:class:`pw_rpc2.aio.ClientStreamingCall`). Send requests with
``call.write(...)``, then call ``call.finish(timeout=...)`` to close the
request stream and wait for the server's response (if the server responded
early, ``finish()`` returns that response directly):

.. tab-set::

   .. tab-item:: Synchronous
      :sync: sync

      .. code-block:: py

         with client.rpcs.examples.Echo.Collect() as call:
             call.write(msg='one')
             call.write(msg='two')
             reply = call.finish(timeout=5.0)

   .. tab-item:: Asyncio
      :sync: async

      .. code-block:: py

         async with client.rpcs.examples.Echo.Collect() as call:
             await call.write(msg='one')
             await call.write(msg='two')
             reply = await call.finish(timeout=5.0)

Bidirectional streaming RPCs
============================
Calling a bidirectional streaming method opens the stream and returns a
:py:class:`pw_rpc2.BidirectionalStreamingCall` (or
:py:class:`pw_rpc2.aio.BidirectionalStreamingCall`), combining ``write()``,
``read()`` / iteration, and ``finish()``.

Unlike ``ClientStreamingCall.finish()``,
``BidirectionalStreamingCall.finish()`` takes no timeout and only half-closes
the client's request stream; the client can keep reading server responses until
the server finishes the call.

Both streaming reader types inherit from :py:class:`pw_rpc2.Reader`
(:py:class:`pw_rpc2.aio.Reader`), and both streaming writer types inherit from
:py:class:`pw_rpc2.Writer` (:py:class:`pw_rpc2.aio.Writer`), so helper functions
that split sending and receiving across threads or tasks can annotate only the
half they use:

.. tab-set::

   .. tab-item:: Synchronous
      :sync: sync

      .. code-block:: py

         with client.rpcs.examples.Echo.EchoStream() as call:
             call.write(msg='ping')
             reply = call.read(timeout=2.0)
             call.finish()
             for trailing in call:
                 print(trailing.msg)

   .. tab-item:: Asyncio
      :sync: async

      .. code-block:: py

         async with client.rpcs.examples.Echo.EchoStream() as call:
             await call.write(msg='ping')
             reply = await call.read(timeout=2.0)
             await call.finish()
             async for trailing in call:
                 print(trailing.msg)

.. _module-pw_rpc2-python-timeouts:

-------------------------
Timeouts and cancellation
-------------------------
Timeouts
========
Timeouts are given in seconds and resolve from most specific to least specific:

1. **Per-operation**: ``call.response(timeout=...)``,
   ``call.read(timeout=...)``, or ``ClientStreamingCall.finish(timeout=...)``.
2. **Per-call**: ``method(..., rpc2_timeout=...)`` or
   ``method.invoke(..., rpc2_timeout=...)``. When iterating over a stream, the
   call's timeout applies to each message wait individually.
3. **Client default**: ``client.default_timeout`` (set at construction or
   updated on the property; ``None`` waits indefinitely).

Pass ``math.inf`` as ``rpc2_timeout`` or ``timeout`` to wait indefinitely when a
broader default timeout is set.

The per-call keyword is named ``rpc2_timeout`` so it does not collide with a
request field named ``timeout``. A request field literally named
``rpc2_timeout`` must be set via a request message when calling a method client
(``call.write()`` takes no timeout, so all field names work as keywords there).

Timeouts apply only to waiting for server responses. Outbound writes block only
on transport backpressure (and in ``pw_rpc2.aio``, run on a dedicated sender
thread so backpressure never stalls the event loop).

Expiring a wait raises :py:class:`pw_rpc2.RpcTimeout` (a ``TimeoutError``
subclass). Timing out a direct unary call ``method(...)`` cancels the RPC;
timing out ``call.response()``, ``call.read()``, or ``call.finish()`` leaves the
call open so you can wait again, unless the exception exits a ``with`` /
``async with`` block.

Cancelling calls
================
``call.cancel()`` is synchronous in both ``pw_rpc2`` and ``pw_rpc2.aio``. It
marks the call completed with :py:attr:`pw_rpc2.RpcStatus.CANCELLED`, wakes
local waiters immediately, and queues a ``CANCELLED`` packet for the server
without blocking on transport backpressure or an in-progress ``write()`` (if a
write is in flight, the ``CANCELLED`` packet is sent immediately after it). It
returns ``True`` if the call was active, or ``False`` if it had already ended.

After ``cancel()``, operations that were waiting on the call, and later calls to
``response()``, ``read()``, ``write()``, and ``ClientStreamingCall.finish()``,
raise :py:class:`pw_rpc2.RpcError` with ``status == RpcStatus.CANCELLED``.
``read()`` and iteration first return any responses that arrived before the
cancellation.

Starting a call in ``pw_rpc2.aio`` queues its opening packet on the sender
thread. If the call ends before that packet is sent—for example, a unary call
that times out or is cancelled while an earlier write is blocked on transport
backpressure—the client sends neither the opening packet nor a ``CANCELLED``
packet, so the server never sees the call.

In ``pw_rpc2.aio``, cancelling an ``asyncio.Task`` cancels the RPC only if the
task is awaiting a direct unary call (``await method(...)``). Cancelling a task
that is awaiting ``call.response()``, ``call.read()``, ``call.write()``, or
``call.finish()`` only stops the wait; the call stays open unless the
``asyncio.CancelledError`` exits an ``async with`` block. A cancelled
``write()`` or ``finish()`` may still be sent, because a packet that the sender
thread is already sending cannot be recalled, so ``asyncio.CancelledError`` does
not mean that the server did not receive the request.

.. _module-pw_rpc2-python-errors:

--------------
Error handling
--------------
``pw_rpc2`` raises distinct exceptions for RPC statuses, timeouts, and transport
or encoding failures:

* :py:class:`pw_rpc2.RpcError`: The RPC ended with a non-OK
  :py:class:`pw_rpc2.RpcStatus`. Provides ``err.method`` (method metadata:
  ``name``, ``full_name``, ``type``, ``descriptor``), ``err.status``
  (:py:class:`~pw_rpc2.RpcStatus`), and ``err.detail`` (``str``, which may be
  empty; ``str(err)`` formats the method, status, wire error, and detail).
* :py:class:`pw_rpc2.RpcTimeout`: ``TimeoutError`` subclass raised when waiting
  for a response expires. Exposes ``err.method`` and ``err.timeout``.
* :py:class:`pw_rpc2.HandshakeError`: ``ConnectionError`` subclass raised when
  the opening handshake fails. ``err.status`` is
  :py:attr:`~pw_rpc2.RpcStatus.DISCONNECTED` if the socket closed during the
  handshake, or :py:attr:`~pw_rpc2.RpcStatus.PROTOCOL_ERROR` on an invalid
  reply, unsupported version, or timeout.
* :py:class:`pw_transport.SocketClosed` (re-exported as
  ``pw_rpc2.SocketClosed``): ``ConnectionError`` subclass raised when starting a
  call on a closed client, or by ``read()``, ``response()``, ``write()``, and
  ``ClientStreamingCall.finish()`` on a call whose socket disconnected before
  the call completed. Closing the client locally (``client.close()``) does not
  raise ``SocketClosed`` on existing calls: they end with
  :py:attr:`~pw_rpc2.RpcStatus.CLOSED`, so those operations raise
  :py:class:`pw_rpc2.RpcError` with ``status == RpcStatus.CLOSED``.
* :py:class:`pw_rpc2.StreamClosedError`: ``RuntimeError`` subclass
  (``err.status == RpcStatus.WRITE_CLOSED``) raised when ``write()`` is called
  after the request stream was finished or the call succeeded, or when
  ``BidirectionalStreamingCall.finish()`` is called on an already-closed stream
  or completed call. (If the call ended with an error, ``write()`` raises that
  error instead.)
* :py:class:`pw_rpc2.DecodeError`: Raised when a wire packet header
  (``err.status == RpcStatus.PROTOCOL_ERROR``) or response protobuf payload
  (``err.status == RpcStatus.DATA_ERROR``) cannot be decoded. A malformed packet
  header closes the connection and causes subsequent calls on the same client to
  raise ``DecodeError``; a malformed response payload on ``read()`` drops only
  that message and leaves a streaming call open.
* ``TypeError`` and ``ValueError``: Raised locally for invalid request arguments
  or unknown fields, outbound packets larger than
  ``socket.max_write_message_size`` (nothing is sent and a streaming call stays
  open), negative timeouts, or colliding service/method definitions.

.. code-block:: py

   try:
       reply = client.rpcs.examples.Echo.Echo(msg='hello', rpc2_timeout=1.0)
   except pw_rpc2.RpcTimeout as err:
       print(f'{err.method.full_name} timed out after {err.timeout:g} s')
   except pw_rpc2.RpcError as err:
       print(f'RPC failed ({err.status}): {err}')
   except pw_rpc2.SocketClosed:
       print('Connection lost')

Status codes and wire errors
============================
:py:class:`pw_rpc2.RpcStatus` and the internal ``ProtocolError`` wire enum are
generated from the same TOML definitions as C++, keeping their names and values
identical. See :ref:`C++ error handling <module-pw_rpc2-cpp-errors>` for
guidance on handling each status code, and
:ref:`RPC error codes <module-pw_rpc2-protocol-error-codes>` for the wire
protocol definitions. Server ``ProtocolError`` packets map to
:py:class:`pw_rpc2.RpcStatus` just like C++'s ``PeerErrorToRpcStatus``:

..
   # LINT.IfChange(python_doc_rpc2_peer_error_mapping)

.. list-table::
   :header-rows: 1
   :widths: 20 40 40

   * - ``RpcStatus``
     - Wire ``ProtocolError``
     - Meaning
   * - ``OK``
     - *(normal completion)*
     - Call completed normally.
   * - ``CANCELLED``
     - ``CANCELLED``, ``DROPPED_WITHOUT_RESPONSE``,
       ``METHOD_RETURNED_WITHOUT_FINISHING``
     - Application code on either side ended the call; do not auto-retry.
   * - ``CLOSED``
     - *(local only)*
     - Client was closed locally.
   * - ``DISCONNECTED``
     - *(local only)*
     - Underlying socket closed during the handshake (active calls raise
       ``SocketClosed``).
   * - ``WRITE_CLOSED``
     - *(local only)*
     - Outbound request stream is already closed (``StreamClosedError``).
   * - ``UNAVAILABLE``
     - ``FAILED_TO_ALLOCATE_CALL``
     - Server could not accept the call before the method ran; safe to retry.
   * - ``ABORTED``
     - ``FAILED_TO_ALLOCATE_CALL_RESOURCES_WHILE_RUNNING``,
       ``SERVICE_UNREGISTERED``
     - Server ended the call after it started; retry only if idempotent.
   * - ``UNIMPLEMENTED``
     - ``UNKNOWN_SERVICE``, ``UNKNOWN_METHOD``, ``METHOD_TYPE_MISMATCH``
     - Server does not offer this service or method in the expected form.
   * - ``DATA_ERROR``
     - ``INVALID_REQUEST_PAYLOAD``
     - Request or response payload could not be decoded as a protobuf message.
   * - ``PROTOCOL_ERROR``
     - ``INTERNAL``, ``RECEIVED_PACKET_FOR_WRONG_ENDPOINT``,
       ``RECEIVED_PACKET_FOR_UNKNOWN_CALL``, ``RECEIVED_DUPLICATE_START``,
       ``NONE``, or any invalid or unrecognized code
     - Wire protocol violation or incompatible endpoint implementation.

..
   # LINT.ThenChange(//pw_rpc2/cpp/connection_task.cc:cpp_rpc2_peer_error_mapping)

``RpcStatus`` also defines ``END_OF_STREAM``, ``MESSAGE_TOO_LARGE``, and
``RESOURCE_EXHAUSTED`` from the shared ``rpc_code.toml`` schema; in Python,
end-of-stream returns ``None`` from ``read()`` (or ends iteration) and oversized
packets raise ``ValueError``.

.. _module-pw_rpc2-python-transports:

-----------------
Custom transports
-----------------
``pw_rpc2`` depends only on :ref:`pw_transport <module-pw_transport-python>`
(``//pw_transport/py:pw_transport``) and operates over any
:py:class:`pw_transport.ReliableSocket`—the Python counterpart to C++'s
:cc:`transport::ReliableSocket <pw::transport::ReliableSocket>`. Each
:py:meth:`~pw_transport.Socket.write` sends one complete ``pw_rpc2`` packet, and
each :py:meth:`~pw_transport.Socket.read` returns one complete packet.

To implement a custom transport, subclass
:py:class:`pw_transport.ReliableSocket` and implement ``_read()``, ``_write()``,
``closed``, and ``close()``. See :ref:`module-pw_transport-python` for full
transport contract details:

.. code-block:: py

   import pw_transport


   class CustomReliableSocket(pw_transport.ReliableSocket):
       @property
       def max_write_message_size(self) -> int | None:
           return 1024

       @property
       def closed(self) -> bool:
           ...

       def _read(self, deadline: pw_transport.Deadline) -> bytes:
           # Block until the next framed packet arrives, or raise TimeoutError
           # or SocketClosed.
           ...

       def _write(self, data: bytes, deadline: pw_transport.Deadline) -> None:
           # Transmit one whole framed packet, or raise TimeoutError or
           # SocketClosed.
           ...

       def close(self) -> None:
           # Close the link and unblock pending read() and write() calls.
           ...
