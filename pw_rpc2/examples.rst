.. _module-pw_rpc2-examples:

========
Examples
========
.. pigweed-module-subpage::
   :name: pw_rpc2

The Echo example implements a service with one RPC of each type and serves it
over TCP. It consists of four standalone programs, which show the two ways to
write ``pw_rpc2`` code: with C++20 coroutines, or with hand-written futures.

.. list-table::
   :header-rows: 1

   * - Program
     - Description
   * - ``server_coro``
     - Implements the service with coroutines.
   * - ``server_polling``
     - Implements the service with futures.
   * - ``client_coro``
     - Calls each RPC from coroutines.
   * - ``client_polling``
     - Calls each RPC from futures and a task.

Any client works with any server. The source code is in
:cs:`pw_rpc2/cpp/examples/echo`.

.. _module-pw_rpc2-examples-run:

---------------
Run the example
---------------
Start a server:

.. code-block:: console

   bazelisk run //pw_rpc2/cpp/examples/echo:server_coro

In another terminal, run a client:

.. code-block:: console

   bazelisk run //pw_rpc2/cpp/examples/echo:client_coro

The client makes each RPC once and logs the responses:

.. literalinclude:: cpp/examples/echo_client.expected
   :language: output

The server logs each request:

.. literalinclude:: cpp/examples/echo_server.expected
   :language: output
   :lines: 2-

.. _module-pw_rpc2-examples-define:

------------------
Define the service
------------------
``echo.proto`` defines the ``Echo`` service:

.. literalinclude:: cpp/examples/echo/echo.proto
   :language: protobuf
   :start-at: syntax = "proto3";

The generated code uses ``pw_protobuf`` message structs. By default,
``pw_protobuf`` encodes and decodes string fields with callbacks.
``echo.pwpb_options`` gives the string fields a fixed capacity instead, so the
structs store them inline as ``pw::InlineString<64>``:

.. literalinclude:: cpp/examples/echo/echo.pwpb_options
   :language: text
   :start-at: examples.EchoMessage.msg

``pwpb_proto_library`` generates the message structs, and
``pwpb_rpc2_proto_library`` generates the service and client code:

.. literalinclude:: cpp/examples/echo/BUILD.bazel
   :language: bazel
   :start-after: # DOCSTAG: [pw_rpc2-examples-echo-codegen]
   :end-before: # DOCSTAG: [pw_rpc2-examples-echo-codegen]

The generated header, ``echo_pb/echo.pwpb.rpc2.h``, declares a base class for
implementing the service, ``examples::pw_rpc2::pwpb::Echo::Service``, and a
client for calling it, ``examples::pw_rpc2::pwpb::Echo::Client``.

.. _module-pw_rpc2-examples-implement:

---------------------
Implement the service
---------------------
A service implementation derives from ``Echo::Service``, passing itself as the
template argument. It implements each RPC either as a coroutine member function
or as a nested ``<Method>Future`` class. An RPC's arguments depend on its method
type:

.. list-table::
   :header-rows: 1

   * - Type
     - Arguments
   * - Unary
     - ``Request``, ``pw::rpc2::UnaryWriter<Response>``
   * - Server streaming
     - ``Request``, ``pw::rpc2::Writer<Response>``
   * - Client streaming
     - ``pw::rpc2::Reader<Request>``, ``pw::rpc2::UnaryWriter<Response>``
   * - Bidirectional streaming
     - ``pw::rpc2::Reader<Request>``, ``pw::rpc2::Writer<Response>``

* ``UnaryWriter::Finish()`` sends the response.
* ``Writer::Write()`` sends a message, and ``Writer::Finish()`` ends the stream.
* ``Reader::Read()`` receives the next message, or ``OUT_OF_RANGE`` once the
  client has finished its stream.

.. _module-pw_rpc2-examples-implement-lifetime:

RPC lifetime
============
An RPC ends when the coroutine or future that implements it completes. If the
RPC does work elsewhere, such as in another task, the coroutine or future must
wait for that work to finish before completing. Otherwise, the RPC ends early.
Readers and writers that the work holds remain safe to use, but their
operations fail.

Conversely, once an RPC is finished or cancelled, ``pw_rpc2`` destroys its
coroutine or future without waiting for it to complete. Do all of the RPC's
work before finishing it.

.. _module-pw_rpc2-examples-implement-coro:

Coroutines
==========
A coroutine RPC takes a :cc:`CoroContext <pw::async2::CoroContext>` as its
first argument and returns ``pw::async2::Coro<void>``. See
:ref:`module-pw_async2-coro` for more about coroutines.

.. literalinclude:: cpp/examples/echo/server_coro.cc
   :language: cpp
   :start-after: // DOCSTAG: [pw_rpc2-examples-echo-server-coro]
   :end-before: // DOCSTAG: [pw_rpc2-examples-echo-server-coro]

Because the RPCs return ``Coro<void>``, they can't use :cc:`PW_CO_TRY`, which
returns a status with ``co_return``. Instead, they check each status and
``co_return`` early if the call has ended.

.. _module-pw_rpc2-examples-implement-futures:

Futures
=======
Without coroutines, each RPC is a nested ``<Method>Future`` class. For each
call, ``pw_rpc2`` constructs the future from the RPC's arguments and polls it
until it completes. The class must be a :ref:`future
<module-pw_async2-futures>` whose ``value_type`` is ``void``.

.. literalinclude:: cpp/examples/echo/server_polling.cc
   :language: cpp
   :start-after: // DOCSTAG: [pw_rpc2-examples-echo-server-polling-unary]
   :end-before: // DOCSTAG: [pw_rpc2-examples-echo-server-polling-unary]

The future for a streaming RPC is a state machine that awaits one operation at
a time. ``EchoStreamFuture`` reads a message, writes it back, and repeats until
the client finishes its stream. Then it finishes its own stream:

.. literalinclude:: cpp/examples/echo/server_polling.cc
   :language: cpp
   :dedent:
   :start-after: // DOCSTAG: [pw_rpc2-examples-echo-server-polling-bidi]
   :end-before: // DOCSTAG: [pw_rpc2-examples-echo-server-polling-bidi]

See :ref:`module-pw_async2-futures-composite` for more about writing futures
like these.

.. _module-pw_rpc2-examples-serve:

-----------------
Serve the service
-----------------
A ``pw::rpc2::Server`` serves its registered services on the connections that
its listeners accept. Both servers set it up the same way:

.. literalinclude:: cpp/examples/echo/server_coro.cc
   :language: cpp
   :dedent:
   :start-after: // DOCSTAG: [pw_rpc2-examples-echo-server-main]
   :end-before: // DOCSTAG: [pw_rpc2-examples-echo-server-main]

``Start()`` runs the server on the dispatcher. The service's RPCs run on the
dispatcher's thread, so state that only the RPCs access does not need locking.

.. _module-pw_rpc2-examples-call:

-------------
Call the RPCs
-------------
``pw::rpc2::Client::Connect()`` connects to a server and resolves to a
``pw::rpc2::Client``, which is a handle to the connection. The generated
``Echo::Client`` binds a ``pw::rpc2::Client`` to the ``Echo`` service. Both are
cheap to copy, so pass them by value.

The generated client creates functions for each RPC. Each function returns a
future that starts the RPC and resolves to:

.. list-table::
   :header-rows: 1

   * - Type
     - Result
   * - Unary
     - ``pw::Result<Response>``
   * - Server streaming
     - ``pw::Result<pw::rpc2::Reader<Response>>``
   * - Client streaming
     - ``pw::Result<pw::rpc2::ClientStreamCall<Request, Response>>``, which
       provides a ``writer()`` for the requests and a ``response()`` future
   * - Bidirectional streaming
     - ``pw::Result<pw::rpc2::BidiStreamCall<Request, Response>>``, which
       provides a ``writer()`` and a ``reader()``

``Writer::Finish()`` ends the client's stream. ``Reader::Read()`` resolves to
``OUT_OF_RANGE`` once the server has finished its stream.

.. _module-pw_rpc2-examples-call-coro:

Coroutines
==========
Each call is a coroutine that makes one RPC and logs the responses:

.. literalinclude:: cpp/examples/echo/client_coro.cc
   :language: cpp
   :start-after: // DOCSTAG: [pw_rpc2-examples-echo-client-coro-calls]
   :end-before: // DOCSTAG: [pw_rpc2-examples-echo-client-coro-calls]

A top-level coroutine connects to the server, makes the calls, and closes the
connection:

.. literalinclude:: cpp/examples/echo/client_coro.cc
   :language: cpp
   :start-after: // DOCSTAG: [pw_rpc2-examples-echo-client-coro-run]
   :end-before: // DOCSTAG: [pw_rpc2-examples-echo-client-coro-run]

``main()`` runs the top-level coroutine as a :cc:`FutureTask
<pw::async2::FutureTask>`. ``RunToCompletion()`` returns once the task has
finished and the connection has closed:

.. literalinclude:: cpp/examples/echo/client_coro.cc
   :language: cpp
   :dedent:
   :start-after: // DOCSTAG: [pw_rpc2-examples-echo-client-coro-main]
   :end-before: // DOCSTAG: [pw_rpc2-examples-echo-client-coro-main]

.. _module-pw_rpc2-examples-call-futures:

Futures
=======
Without coroutines, each streaming call is a composite future. ``RepeatCall``
starts the RPC, then reads responses until the server finishes its stream:

.. literalinclude:: cpp/examples/echo/client_polling.cc
   :language: cpp
   :start-after: // DOCSTAG: [pw_rpc2-examples-echo-client-polling-repeat]
   :end-before: // DOCSTAG: [pw_rpc2-examples-echo-client-polling-repeat]

``CollectCall`` and ``EchoStreamCall`` follow the same pattern. A
:cc:`pw::async2::Task` connects to the server, runs the calls in sequence, and
closes the connection. The unary ``Echo`` call is simpler; the task awaits the
RPC's response future directly:

.. literalinclude:: cpp/examples/echo/client_polling.cc
   :language: cpp
   :start-after: // DOCSTAG: [pw_rpc2-examples-echo-client-polling-task]
   :end-before: // DOCSTAG: [pw_rpc2-examples-echo-client-polling-task]

``main()`` posts the task to a :cc:`pw::async2::BasicDispatcher` and runs the
dispatcher until the task has finished and the connection has closed.
