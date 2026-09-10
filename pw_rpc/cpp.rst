.. TODO: b/442244233 - Move API reference content to Doxygen comments

.. _module-pw_rpc-cpp:

====================
C++ client & server
====================
.. pigweed-module-subpage::
   :name: pw_rpc

This page provides in-depth guidance on the C++ server, client, channel, and
call object APIs, concurrency rules, and testing fixtures.

.. tip::
   * **Looking to bring up pw_rpc on your hardware?** See the :ref:`module-pw_rpc-setup`.
   * **Looking to define and implement RPC services?** See :ref:`module-pw_rpc-services`.

----------
RPC server
----------
Declare an instance of ``rpc::Server`` and register services with it.

Size report
===========
The following size report showcases the memory usage of the core RPC server. It
is configured with a single channel using a basic transport interface that
directly reads from and writes to ``pw_sys_io``. The transport has a 128-byte
packet buffer, which comprises the plurality of the example's RAM usage. This is
not a suitable transport for an actual product; a real implementation would have
additional overhead proportional to the complexity of the transport.

.. include:: server_size

RPC server implementation
=========================

The Method class
----------------
The RPC Server depends on the ``pw::rpc::internal::Method`` class. ``Method``
serves as the bridge between the ``pw_rpc`` server library and the user-defined
RPC functions. Each supported protobuf implementation extends ``Method`` to
implement its request and response proto handling. The ``pw_rpc`` server
calls into the ``Method`` implementation through the base class's ``Invoke``
function.

``Method`` implementations store metadata about each method, including a
function pointer to the user-defined method implementation. They also provide
``static constexpr`` functions for creating each type of method. ``Method``
implementations must satisfy the ``MethodImplTester`` test class in
``pw_rpc/internal/method_impl_tester.h``.

See ``pw_rpc/internal/method.h`` for more details about ``Method``.

Packet flow
-----------

Requests
^^^^^^^^

.. mermaid::
   :alt: Request Packet Flow

   flowchart LR
       packets[Packets]

       subgraph pw_rpc [pw_rpc Library]
           direction TB
           internalMethod[[internal::Method]]
           Server --> Service --> internalMethod
       end

       packets --> Server

       generatedServices{{generated services}}
       userDefinedRPCs(user-defined RPCs)

       generatedServices --> userDefinedRPCs
       internalMethod --> generatedServices

Responses
^^^^^^^^^

.. mermaid::
   :alt: Request Packet Flow

   flowchart LR
       generatedServices{{generated services}}
       userDefinedRPCs(user-defined RPCs)

       subgraph pw_rpc [pw_rpc Library]
           direction TB
           internalMethod[[internal::Method]]
           internalMethod --> Server --> Channel
       end

       packets[Packets]
       Channel --> packets

       userDefinedRPCs --> generatedServices
       generatedServices --> internalMethod

----------
RPC client
----------
The RPC client is used to send requests to a server and manages the contexts of
ongoing RPCs.

Setting up a client
===================
The ``pw::rpc::Client`` class is instantiated with a list of channels that it
uses to communicate. These channels can be shared with a server, but multiple
clients cannot use the same channels.

To send incoming RPC packets from the transport layer to be processed by a
client, the client's ``ProcessPacket`` function is called with the packet data.

.. code-block:: c++

   #include "pw_rpc/client.h"

   namespace {

   pw::rpc::Channel my_channels[] = {
       pw::rpc::Channel::Create<1>(&my_channel_output)};
   pw::rpc::Client my_client(my_channels);

   }  // namespace

   // Called when the transport layer receives an RPC packet.
   void ProcessRpcPacket(ConstByteSpan packet) { my_client.ProcessPacket(packet); }

Note that client processing such as callbacks will be invoked within
the body of ``ProcessPacket``.

If certain packets need to be filtered out, or if certain client processing
needs to be invoked from a specific thread or context, the ``PacketMeta`` class
can be used to determine which service or channel a packet is targeting. After
filtering, ``ProcessPacket`` can be called from the appropriate environment.

.. _module-pw_rpc-making-calls:

Making RPC calls
================
RPC calls are not made directly through the client, but using one of its
registered channels instead. A service client class is generated from a .proto
file for each selected protobuf library, which is then used to send RPC requests
through a given channel. The API for this depends on the protobuf library;
please refer to the
:ref:`service authoring documentation <module-pw_rpc-services>`. Multiple
service client implementations can exist simulatenously and share the same
``Client`` class.

When a call is made, a call object is returned to the caller. This object tracks
the ongoing RPC call, and can be used to manage it. An RPC call is only active
as long as its call object is alive.

.. tip::

   Use ``std::move`` when passing around call objects to keep RPCs alive.

Example
-------
.. literalinclude:: examples/client_example.cc
   :language: cpp
   :start-after: [pw_rpc-examples-async-client-call]
   :end-before: [pw_rpc-examples-async-client-call]

--------
Channels
--------
``pw_rpc`` sends all of its packets over channels. These are logical,
application-layer routes used to tell the RPC system where a packet should go.

Channels over a client-server connection must all have a unique ID, which can be
assigned statically at compile time or dynamically.

.. code-block:: cpp

   // Creating a channel with the static ID 3.
   pw::rpc::Channel static_channel = pw::rpc::Channel::Create<3>(&output);

   // Grouping channel IDs within an enum can lead to clearer code.
   enum ChannelId {
     kUartChannel = 1,
     kSpiChannel = 2,
   };

   // Creating a channel with a static ID defined within an enum.
   pw::rpc::Channel another_static_channel =
       pw::rpc::Channel::Create<ChannelId::kUartChannel>(&output);

   // Creating a channel with a dynamic ID (note that no output is provided; it
   // will be set when the channel is used.
   pw::rpc::Channel dynamic_channel;

Sometimes, the ID and output of a channel are not known at compile time as they
depend on information stored on the physical device. To support this use case, a
dynamically-assignable channel can be configured once at runtime with an ID and
output.

.. code-block:: cpp

   // Create a dynamic channel without a compile-time ID or output.
   pw::rpc::Channel dynamic_channel;

   void Init() {
     // Called during boot to pull the channel configuration from the system.
     dynamic_channel.Configure(GetChannelId(), some_output);
   }

Adding and removing channels
============================
New channels may be registered with the ``OpenChannel`` function. If dynamic
allocation is enabled (:c:macro:`PW_RPC_DYNAMIC_ALLOCATION` is 1), any number of
channels may be registered. If dynamic allocation is disabled, new channels may
only be registered if there are availale channel slots in the span provided to
the RPC endpoint at construction.

A channel may be closed and unregistered with an endpoint by calling
``ChannelClose`` on the endpoint with the corresponding channel ID.  This
will terminate any pending calls and call their ``on_error`` callback
with the ``ABORTED`` status.

.. code-block:: cpp

   // When a channel is closed, any pending calls will receive
   // on_error callbacks with ABORTED status.
   client->CloseChannel(1);

Default Channel Output
============================
Sometimes it may be necessary to register a ``ChannelOutput`` that handles all
messages which do not match a specific channel, for example, if the server
itself doesn't have direct access to the clients and it has to be routed through
some middleware. For cases like this, a default channel output can be registered
which processes all packets with unrecognized channel ids.

.. code-block:: cpp

   // Only registered channel has id of 1.
   std::array<Channel, 1> channels = {Channel::Create<1>(/*...*/)};
   // `MyDefaultChannelOutput` is a class that implements the
   // `pw::rpc::ChannelOutput` interface. It will process all packets that don't
   // have a packet id of 1.
   MyDefaultChannelOutput default_channel_output;
   Server server(channels);
   PW_ASSERT_OK(server.SetDefaultChannelOutput(default_channel_output));

.. _module-pw_rpc-remap:

Remapping channels
==================
Some pw_rpc deployments may find it helpful to remap channel IDs in RPC packets.
This can remove the need for globally known channel IDs. Clients can use a
generic channel ID. The server remaps the generic channel ID to an ID associated
with the transport the client is using.

A future revision of the pw_rpc protocol will remove the need for global channel
IDs without requiring remapping.

API reference
-------------
Moved: :cc:`Channel functions <pw_rpc_channel>`

Example deployment
==================
This section describes a hypothetical pw_rpc deployment that supports arbitrary
pw_rpc clients with one pw_rpc server. Note that this assumes that the
underlying transport provides some sort of addressing that the server-side can
associate with a channel ID.

- A pw_rpc server is running on one core. A variable number of pw_rpc clients
  need to call RPCs on the server from a different core.
- The client core opens a socket (or similar feature) to connect to the server
  core.
- The server core detects the inbound connection and allocates a new channel ID.
  It creates a new channel by calling :cpp:func:`pw::rpc::Server::OpenChannel`
  with the channel ID and a :cpp:class:`pw::rpc::ChannelOutput` associated with
  the new connection.
- The server maintains a mapping between channel IDs and pw_rpc client
  connections.
- On the client core, pw_rpc clients all use the same channel ID (e.g.  ``1``).
- As packets arrive from pw_rpc client connections, the server-side code calls
  :cpp:func:`pw::rpc::ChangeEncodedChannelId` on the encoded packet to replace
  the generic channel ID (``1``) with the server-side channel ID allocated when
  the client connected. The encoded packet is then passed to
  :cpp:func:`pw::rpc::Server::ProcessPacket`.
- When the server sends pw_rpc packets, the :cpp:class:`pw::rpc::ChannelOutput`
  calls :cpp:func:`pw::rpc::ChangeEncodedChannelId` to set the channel ID back
  to the generic ``1``.

------------------------------
C++ payload sizing limitations
------------------------------
The individual size of each sent RPC request or response is limited by
``pw_rpc``'s ``PW_RPC_ENCODING_BUFFER_SIZE_BYTES`` configuration option when
using Pigweed's C++ implementation. While multiple RPC messages can be enqueued
(as permitted by the underlying transport), if a single individual sent message
exceeds the limitations of the statically allocated encode buffer, the packet
will fail to encode and be dropped.

This applies to all C++ RPC service implementations (nanopb, raw, and pwpb),
so it's important to ensure request and response message sizes do not exceed
this limitation.

As ``pw_rpc`` has some additional encoding overhead, a helper,
``pw::rpc::MaxSafePayloadSize()`` is provided to expose the practical max RPC
message payload size.

.. code-block:: cpp

   #include "pw_file/file.raw_rpc.pb.h"
   #include "pw_rpc/channel.h"

   namespace pw::file {

   class FileSystemService
       : public pw_rpc::raw::FileSystem::Service<FileSystemService> {
    public:
     void List(ConstByteSpan request, RawServerWriter& writer);

    private:
     // Allocate a buffer for building proto responses.
     static constexpr size_t kEncodeBufferSize = pw::rpc::MaxSafePayloadSize();
     std::array<std::byte, kEncodeBufferSize> encode_buffer_;
   };

   }  // namespace pw::file

------------
Call objects
------------
An RPC call is represented by a call object. Server and client calls use the
same base call class in C++, but the public API is different depending on the
type of call and whether it is being used by the server or client. See
:ref:`module-pw_rpc-design-lifecycle`.

The public call types are as follows:

.. list-table::
   :header-rows: 1

   * - RPC Type
     - Server call
     - Client call
   * - Unary
     - ``(Raw|Nanopb|Pwpb)UnaryResponder``
     - ``(Raw|Nanopb|Pwpb)UnaryReceiver``
   * - Server streaming
     - ``(Raw|Nanopb|Pwpb)ServerWriter``
     - ``(Raw|Nanopb|Pwpb)ClientReader``
   * - Client streaming
     - ``(Raw|Nanopb|Pwpb)ServerReader``
     - ``(Raw|Nanopb|Pwpb)ClientWriter``
   * - Bidirectional streaming
     - ``(Raw|Nanopb|Pwpb)ServerReaderWriter``
     - ``(Raw|Nanopb|Pwpb)ClientReaderWriter``

Client call API
===============
Client call objects provide a few common methods.

.. cpp:class:: pw::rpc::ClientCallType

   The ``ClientCallType`` will be one of the following types:

   - ``(Raw|Nanopb|Pwpb)UnaryReceiver`` for unary
   - ``(Raw|Nanopb|Pwpb)ClientReader`` for server streaming
   - ``(Raw|Nanopb|Pwpb)ClientWriter`` for client streaming
   - ``(Raw|Nanopb|Pwpb)ClientReaderWriter`` for bidirectional streaming

   .. cpp:function:: bool active() const

      Returns true if the call is active.

   .. cpp:function:: uint32_t channel_id() const

      Returns the channel ID of this call, which is 0 if the call is inactive.

   .. cpp:function:: uint32_t id() const

      Returns the call ID, a unique identifier for this call.

   .. cpp:function:: void Write(RequestType)

      Only available on client and bidirectional streaming calls. Sends a stream
      request. Returns:

      - ``OK`` - the request was successfully sent
      - ``FAILED_PRECONDITION`` - the writer is closed
      - ``INTERNAL`` - pw_rpc was unable to encode message; does not apply to
        raw calls
      - other errors - the :cpp:class:`ChannelOutput` failed to send the packet;
        the error codes are determined by the :cpp:class:`ChannelOutput`
        implementation

   .. cpp:function:: void WriteCallback(Function<StatusWithSize(ByteSpan)>)

      Raw RPC only. Invokes the provided callback with the available RPC payload
      buffer, allowing payloads to be encoded directly into it. Sends a stream
      packet with the payload if the callback is successful.

      The buffer provided to the callback is only valid for the duration of the
      callback. The callback should return an OK status with the size of the
      encoded payload on success, or an error status on failure.

   .. cpp:function:: size_t MaxWriteSizeBytes() const

      Returns the maximum encoded payload size for writes to this call or 0 if
      the call is not active.

   .. cpp:function:: pw::Status RequestCompletion()

      Notifies the server that client has requested for call completion. On
      client and bidirectional streaming calls no further client stream messages
      will be sent.

   .. cpp:function:: pw::Status Cancel()

      Cancels this RPC. Closes the call and sends a ``CANCELLED`` error to the
      server. Return statuses are the same as :cpp:func:`Write`.

   .. cpp:function:: void Abandon()

      Closes this RPC locally. Sends a ``CLIENT_REQUEST_COMPLETION``, but no cancellation
      packet. Future packets for this RPC are dropped, and the client sends a
      ``FAILED_PRECONDITION`` error in response because the call is not active.

   .. cpp:function:: void CloseAndWaitForCallbacks()

      Abandons this RPC and additionally blocks on completion of any running callbacks.

   .. cpp:function:: void set_on_completed(pw::Function<void(ResponseTypeIfUnaryOnly, pw::Status)>)

      Sets the callback that is called when the RPC completes normally. The
      signature depends on whether the call has a unary or stream response.

   .. cpp:function:: void set_on_error(pw::Function<void(pw::Status)>)

      Sets the callback that is called when the RPC is terminated due to an error.

   .. cpp:function:: void set_on_next(pw::Function<void(ResponseType)>)

      Only available on server and bidirectional streaming calls. Sets the callback
      that is called for each stream response.

Callbacks
=========
The C++ call objects allow users to set callbacks that are invoked when RPC
:ref:`events <module-pw_rpc-design-events>` occur.

.. list-table::
   :header-rows: 1

   * - Name
     - Stream signature
     - Non-stream signature
     - Server
     - Client
   * - ``on_error``
     - ``void(pw::Status)``
     - ``void(pw::Status)``
     - ✅
     - ✅
   * - ``on_next``
     - n/a
     - ``void(const PayloadType&)``
     - ✅
     - ✅
   * - ``on_completed``
     - ``void(pw::Status)``
     - ``void(const PayloadType&, pw::Status)``
     -
     - ✅
   * - ``on_client_requested_completion``
     - ``void()``
     - n/a
     - ✅ (:c:macro:`optional <PW_RPC_COMPLETION_REQUEST_CALLBACK>`)
     -

Limitations and restrictions
----------------------------
RPC callbacks are free to perform most actions, including invoking new RPCs or
cancelling pending calls. However, the C++ implementation imposes some
limitations and restrictions that must be observed.

Destructors & moves wait for callbacks to complete
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
* Callbacks must not destroy their call object. Attempting to do so will result
  in deadlock.
* Other threads may destroy a call while its callback is running, but that
  thread will block until all callbacks complete.
* Callbacks must not move their call object if it the call is still active. They
  may move their call object after it has terminated. Callbacks may move a
  different call into their call object, since moving closes the destination
  call.
* Other threads may move a call object while it has a callback running, but they
  will block until the callback completes if the call is still active.

.. warning::

   Deadlocks or crashes occur if a callback:

   - attempts to destroy its call object
   - attempts to move its call object while the call is still active
   - never returns

   If ``pw_rpc`` a callback violates these restrictions, a crash may occur,
   depending on the value of :c:macro:`PW_RPC_CALLBACK_TIMEOUT_TICKS`. These
   crashes have a message like the following:

   .. code-block:: text

      A callback for RPC 1:cc0f6de0/31e616ce has not finished after 10000 ticks.
      This may indicate that an RPC callback attempted to destroy or move its own
      call object, which is not permitted. Fix this condition or change the value of
      PW_RPC_CALLBACK_TIMEOUT_TICKS to avoid this crash.

      See https://pigweed.dev/pw_rpc/cpp.html#destructors-moves-wait-for-callbacks-to-complete
      for details.

Only one thread at a time may execute ``on_next``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Only one thread may execute the ``on_next`` callback for a specific service
method at a time. If a second thread calls ``ProcessPacket()`` with a stream
packet before the ``on_next`` callback for the previous packet completes, the
second packet will be dropped. The RPC endpoint logs a warning when this occurs.

Example warning for a dropped stream packet:

.. code-block:: text

   WRN  Received stream packet for 1:cc0f6de0/31e616ce before the callback for
        a previous packet completed! This packet will be dropped. This can be
        avoided by handling packets for a particular RPC on only one thread.

-----------------------
RPC calls introspection
-----------------------
``pw_rpc`` provides ``pw_rpc/method_info.h`` header that allows to obtain
information about the generated RPC method in compile time.

For now it provides only two types: ``MethodRequestType<RpcMethod>`` and
``MethodResponseType<RpcMethod>``. They are aliases to the types that are used
as a request and response respectively for the given RpcMethod.

Example
=======
We have an RPC service ``SpecialService`` with ``MyMethod`` method:

.. code-block:: protobuf

   package some.package;
   service SpecialService {
     rpc MyMethod(MyMethodRequest) returns (MyMethodResponse) {}
   }

We also have a templated Storage type alias:

.. code-block:: c++

   template <auto kMethod>
   using Storage =
       std::pair<MethodRequestType<kMethod>, MethodResponseType<kMethod>>;

``Storage<some::package::pw_rpc::pwpb::SpecialService::MyMethod>`` will
instantiate as:

.. code-block:: c++

   std::pair<some::package::MyMethodRequest::Message,
             some::package::MyMethodResponse::Message>;

.. note::

   Only nanopb and pw_protobuf have real types as
   ``MethodRequestType<RpcMethod>``/``MethodResponseType<RpcMethod>``. Raw has
   them both set as ``void``. In reality, they are ``pw::ConstByteSpan``. Any
   helper/trait that wants to use this types for raw methods should do a custom
   implementation that copies the bytes under the span instead of copying just
   the span.

.. _module-pw_rpc-client-sync-call-wrappers:

--------------------------------
Client synchronous call wrappers
--------------------------------
``pw_rpc`` provides wrappers that convert the asynchronous client API to a
synchronous API. The :cc:`SynchronousCall\<RpcMethod\>
<pw::rpc::SynchronousCall>` functions wrap the asynchronous client RPC call
with a timed thread notification and returns once a result is known or a
timeout has occurred. Only unary methods are supported.

The Nanopb and pwpb APIs return a :cc:`SynchronousCallResult\<Response\>
<pw::rpc::SynchronousCallResult>` object, which can be queried to determine
whether any error scenarios occurred and, if not, access the response. The raw
API executes a function when the call completes or returns a :cc:`Status
<pw::Status>` if it does not.

``SynchronousCall<RpcMethod>`` blocks indefinitely, whereas
:cc:`SynchronousCallFor\<RpcMethod\> <pw::rpc::SynchronousCallFor>` and
:cc:`SynchronousCallUntil\<RpcMethod\> <pw::rpc::SynchronousCallUntil>`
block for a given timeout or until a deadline, respectively. All wrappers work
with either the standalone static RPC functions or the generated service client
member methods.

.. note::

   Use of the ``SynchronousCall`` wrappers requires a
   :cc:`TimedThreadNotification <pw::sync::TimedThreadNotification>`
   backend.

The following examples use the Nanopb API to make a call that blocks
indefinitely. If you'd like to include a timeout for how long the call
should block for, use the ``SynchronousCallFor()`` or ``SynchronousCallUntil()``
variants.

.. code-block:: cpp

   pw_rpc_EchoMessage request{.msg = "hello"};
   pw::rpc::SynchronousCallResult<pw_rpc_EchoMessage> result =
       pw::rpc::SynchronousCall<EchoService::Echo>(
           rpc_client, channel_id, request);
   if (result.ok()) {
     PW_LOG_INFO("%s", result.response().msg);
   }

Additionally, the use of a generated ``Client`` object is supported:

.. code-block:: cpp

   pw_rpc::nanopb::EchoService::Client client(rpc_client, channel_id);
   pw_rpc_EchoMessage request{.msg = "hello"};
   pw::rpc::SynchronousCallResult<pw_rpc_EchoMessage> result =
       pw::rpc::SynchronousCall<EchoService::Echo>(client, request);

   if (result.ok()) {
     PW_LOG_INFO("%s", result.response().msg);
   }

``SynchronousCall<RpcMethod>`` also supports using an optional custom response
message class, ``SynchronousCall<RpcMethod, Response>``. This enables the use
of response messages with variable-length fields.

.. code-block:: cpp

   pw_rpc_MyMethodRequestMessage request{};
   class CustomResponse : public pw_rpc_MyMethodResponseMessage {
    public:
     CustomResponse() {
       repeated_field.SetDecoder([this](MyMethodResponse::StreamDecoder& decoder) {
         return decoder.ReadRepeatedField(values);
       });
     }
     pw::Vector<uint32_t, 4> values();
   };
   pw::rpc::SynchronousCallResult<CustomResponse> result =
       pw::rpc::SynchronousCall<EchoService::Echo, CustomResponse>(
           rpc_client, channel_id, request);
   if (result.ok()) {
     PW_LOG_INFO("%d", result.response().values[0]);
   }

The raw API works similarly to the Nanopb API, but takes a :cc:`Function
<pw::Function>` and returns a :cc:`Status <pw::Status>`. If the RPC
completes, the ``Function`` is called with the response and returned status,
and the ``SynchronousCall()`` invocation returns :cc:`OkStatus()
<pw::OkStatus>`. If the RPC fails, ``SynchronousCall()`` returns an error.

.. code-block:: cpp

   pw::Status rpc_status = pw::rpc::SynchronousCall<EchoService::Echo>(
       rpc_client,
       channel_id,
       encoded_request,
       [](pw::ConstByteSpan reply, pw::Status status) {
         PW_LOG_INFO(
             "Received %zu bytes with status %s", reply.size(), status.str());
       });

.. warning::

   These wrappers should not be used from any context that cannot be
   blocked! This method will block the calling thread until the RPC completes,
   and translate the response into a ``SynchronousCallResult`` that
   contains the error type and status or the proto response.

Example
=======
.. code-block:: c++

   #include "pw_rpc/synchronous_call.h"

   void InvokeUnaryRpc() {
     pw::rpc::Client client;
     pw::rpc::Channel channel;

     RoomInfoRequest request;
     SynchronousCallResult<RoomInfoResponse> result =
         SynchronousCall<Chat::GetRoomInformation>(client, channel.id(), request);

     if (result.is_rpc_error()) {
       ShutdownClient(client);
     } else if (result.is_server_error()) {
       HandleServerError(result.status());
     } else if (result.is_timeout()) {
       // SynchronousCall will block indefinitely, so we should never get here.
       PW_UNREACHABLE();
     }
     HandleRoomInformation(std::move(result).response());
   }

   void AnotherExample() {
     pw_rpc::nanopb::Chat::Client chat_client(client, channel);
     constexpr auto kTimeout = pw::chrono::SystemClock::for_at_least(500ms);

     RoomInfoRequest request;
     auto result = SynchronousCallFor<Chat::GetRoomInformation>(
         chat_client, request, kTimeout);

     if (result.is_timeout()) {
       RetryRoomRequest();
     } else {
       // ...
     }
   }

The ``SynchronousCallResult<Response>`` is also compatible with the
:c:macro:`PW_TRY` family of macros, but users should be aware that their use
will lose information about the type of error. This should only be used if the
caller will handle all error scenarios the same.

.. code-block:: c++

   pw::Status SyncRpc() {
     const RoomInfoRequest request;
     PW_TRY_ASSIGN(const RoomInfoResponse& response,
                   SynchronousCall<Chat::GetRoomInformation>(client, request));
     HandleRoomInformation(response);
     return pw::OkStatus();
   }

------------
ClientServer
------------
Sometimes, a device needs to both process RPCs as a server, as well as making
calls to another device as a client. To do this, both a client and server must
be set up, and incoming packets must be sent to both of them.

Pigweed simplifies this setup by providing a ``ClientServer`` class which wraps
an RPC client and server with the same set of channels.

.. code-block:: cpp

   pw::rpc::Channel channels[] = {pw::rpc::Channel::Create<1>(&channel_output)};

   // Creates both a client and a server.
   pw::rpc::ClientServer client_server(channels);

   void ProcessRpcData(pw::ConstByteSpan packet) {
     // Calls into both the client and the server, sending the packet to the
     // appropriate one.
     client_server.ProcessPacket(packet);
   }

.. _module-pw_rpc-cpp-testing:

-------
Testing
-------
``pw_rpc`` provides utilities for unit testing RPC services and client calls.

Client unit testing in C++
==========================
``pw_rpc`` supports invoking RPCs, simulating server responses, and checking
what packets are sent by an RPC client in tests. Raw, Nanopb and Pwpb interfaces
are supported. Code that uses the raw API may be tested with the raw test
helpers, and vice versa. The Nanopb and Pwpb APIs also provides a test helper
with a real client-server pair that supports testing of asynchronous messaging.

To test synchronous code that invokes RPCs, declare a ``RawClientTestContext``,
``PwpbClientTestContext``,  or ``NanopbClientTestContext``. These test context
objects provide a preconfigured RPC client, channel, server fake, and buffer for
encoding packets.

These test classes are defined in ``pw_rpc/raw/client_testing.h``,
``pw_rpc/pwpb/client_testing.h``, or ``pw_rpc/nanopb/client_testing.h``.

Use the context's ``client()`` and ``channel()`` to invoke RPCs. Use the
context's ``server()`` to simulate responses. To verify that the client sent the
expected data, use the context's ``output()``, which is a ``FakeChannelOutput``.

For example, the following tests a class that invokes an RPC. It checks that
the expected data was sent and then simulates a response from the server.

.. code-block:: cpp

   #include "pw_rpc/raw/client_testing.h"

   class ClientUnderTest {
    public:
     // To support injecting an RPC client for testing, classes that make RPC
     // calls should take an RPC client and channel ID or an RPC service client
     // (e.g. pw_rpc::raw::MyService::Client).
     ClientUnderTest(pw::rpc::Client& client, uint32_t channel_id);

     void DoSomethingThatInvokesAnRpc();

     bool SetToTrueWhenRpcCompletes();
   };

   TEST(TestAThing, InvokesRpcAndHandlesResponse) {
     RawClientTestContext context;
     ClientUnderTest thing(context.client(), context.channel().id());

     // Execute the code that invokes the MyService.TheMethod RPC.
     things.DoSomethingThatInvokesAnRpc();

     // Find and verify the payloads sent for the MyService.TheMethod RPC.
     auto msgs = context.output().payloads<pw_rpc::raw::MyService::TheMethod>();
     ASSERT_EQ(msgs.size(), 1u);

     VerifyThatTheExpectedMessageWasSent(msgs.back());

     // Send the response packet from the server and verify that the class reacts
     // accordingly.
     EXPECT_FALSE(thing.SetToTrueWhenRpcCompletes());

     context_.server().SendResponse<pw_rpc::raw::MyService::TheMethod>(
         final_message, OkStatus());

     EXPECT_TRUE(thing.SetToTrueWhenRpcCompletes());
   }

To test client code that uses asynchronous responses, encapsulates multiple
rpc calls to one or more services, or uses a custom service implementation,
declare a ``NanopbClientServerTestContextThreaded`` or
``PwpbClientServerTestContextThreaded``. These test object are defined in
``pw_rpc/nanopb/client_server_testing_threaded.h`` and
``pw_rpc/pwpb/client_server_testing_threaded.h``.

Use the context's ``server()`` to register a ``Service`` implementation, and
``client()`` and ``channel()`` to invoke RPCs. Create a ``Thread`` using the
context as a ``ThreadCore`` to have it asynchronously forward request/responses or
call ``ForwardNewPackets`` to synchronously process all messages. To verify that
the client/server sent the expected data, use the context's
``request(uint32_t index)`` and ``response(uint32_t index)`` to retrieve the
ordered messages.

For example, the following tests a class that invokes an RPC and blocks till a
response is received. It verifies that expected data was both sent and received.

.. code-block:: cpp

   #include "my_library_protos/my_service.rpc.pb.h"
   #include "pw_rpc/nanopb/client_server_testing_threaded.h"
   #include "pw_thread_stl/options.h"

   class ClientUnderTest {
    public:
     // To support injecting an RPC client for testing, classes that make RPC
     // calls should take an RPC client and channel ID or an RPC service client
     // (e.g. pw_rpc::raw::MyService::Client).
     ClientUnderTest(pw::rpc::Client& client, uint32_t channel_id);

     Status BlockOnResponse(uint32_t value);
   };

   class TestService final : public MyService<TestService> {
    public:
     Status TheMethod(const pw_rpc_test_TheMethod& request,
                      pw_rpc_test_TheMethod& response) {
       response.value = request.integer + 1;
       return pw::OkStatus();
     }
   };

   TEST(TestServiceTest, ReceivesUnaryRpcResponse) {
     NanopbClientServerTestContextThreaded<> ctx(pw::thread::stl::Options{});
     TestService service;
     ctx.server().RegisterService(service);
     ClientUnderTest client(ctx.client(), ctx.channel().id());

     // Execute the code that invokes the MyService.TheMethod RPC.
     constexpr uint32_t value = 1;
     const auto result = client.BlockOnResponse(value);
     const auto request = ctx.request<MyService::TheMethod>(0);
     const auto response = ctx.response<MyService::TheMethod>(0);

     // Verify content of messages
     EXPECT_EQ(result, pw::OkStatus());
     EXPECT_EQ(request.value, value);
     EXPECT_EQ(response.value, value + 1);
   }

Use the context's
``response(uint32_t index, Response<kMethod>& response)`` to decode messages
into a provided response object. You would use this version if decoder callbacks
are needed to fully decode a message. For instance if it uses ``repeated``
fields.

.. code-block:: cpp

   TestResponse::Message response{};
   response.repeated_field.SetDecoder(
       [&values](TestResponse::StreamDecoder& decoder) {
         return decoder.ReadRepeatedField(values);
       });
   ctx.response<test::GeneratedService::TestAnotherUnaryRpc>(0, response);

Synchronous versions of these test contexts also exist that may be used on
non-threaded systems ``NanopbClientServerTestContext`` and
``PwpbClientServerTestContext``. While these do not allow for asynchronous
messaging they support the use of service implementations and use a similar
syntax. When these are used ``.ForwardNewPackets()`` should be called after each
rpc call to trigger sending of queued messages.

For example, the following tests a class that invokes an RPC that is responded
to with a test service implementation.

.. code-block:: cpp

   #include "my_library_protos/my_service.rpc.pb.h"
   #include "pw_rpc/nanopb/client_server_testing.h"

   class ClientUnderTest {
    public:
     ClientUnderTest(pw::rpc::Client& client, uint32_t channel_id);

     Status SendRpcCall(uint32_t value);
   };

   class TestService final : public MyService<TestService> {
    public:
     Status TheMethod(const pw_rpc_test_TheMethod& request,
                      pw_rpc_test_TheMethod& response) {
       response.value = request.integer + 1;
       return pw::OkStatus();
     }
   };

   TEST(TestServiceTest, ReceivesUnaryRpcResponse) {
     NanopbClientServerTestContext<> ctx();
     TestService service;
     ctx.server().RegisterService(service);
     ClientUnderTest client(ctx.client(), ctx.channel().id());

     // Execute the code that invokes the MyService.TheMethod RPC.
     constexpr uint32_t value = 1;
     const auto result = client.SendRpcCall(value);
     // Needed after ever RPC call to trigger forward of packets
     ctx.ForwardNewPackets();
     const auto request = ctx.request<MyService::TheMethod>(0);
     const auto response = ctx.response<MyService::TheMethod>(0);

     // Verify content of messages
     EXPECT_EQ(result, pw::OkStatus());
     EXPECT_EQ(request.value, value);
     EXPECT_EQ(response.value, value + 1);
   }

Custom packet processing for ClientServerTestContext
====================================================
Optional constructor arguments for nanopb/pwpb ``*ClientServerTestContext`` and
``*ClientServerTestContextThreaded`` allow allow customized packet processing.
By default the only thing is done is ``ProcessPacket()`` call on the
``ClientServer`` instance.

For cases when additional instrumentation or offloading to separate thread is
needed, separate client and server processors can be passed to context
constructors. A packet processor is a function that returns ``pw::Status`` and
accepts two arguments: ``pw::rpc::ClientServer&`` and ``pw::ConstByteSpan``.
Default packet processing is equivalent to the next processor:

.. code-block:: cpp

   [](ClientServer& client_server, pw::ConstByteSpan packet) -> pw::Status {
     return client_server.ProcessPacket(packet);
   };

The Server processor will be applied to all packets sent to the server (i.e.
requests) and client processor will be applied to all packets sent to the client
(i.e. responses).

.. note::

  The packet processor MUST call ``ClientServer::ProcessPacket()`` method.
  Otherwise the packet won't be processed.

.. note::

  If the packet processor offloads processing to the separate thread, it MUST
  copy the ``packet``. After the packet processor returns, the underlying array
  can go out of scope or be reused for other purposes.

SendResponseIfCalled() helper
=============================
``SendResponseIfCalled()`` function waits on ``*ClientTestContext*`` output to
have a call for the specified method and then responses to it. It supports
timeout for the waiting part (default timeout is 100ms).

.. code-block:: c++

   #include "pw_rpc/test_helpers.h"

   pw::rpc::PwpbClientTestContext client_context;
   other::pw_rpc::pwpb::OtherService::Client other_service_client(
       client_context.client(), client_context.channel().id());

   PW_PWPB_TEST_METHOD_CONTEXT(MyService, GetData)
   context(other_service_client);
   context.call({});

   PW_TEST_ASSERT_OK(pw::rpc::test::SendResponseIfCalled<
                     other::pw_rpc::pwpb::OtherService::GetPart>(client_context,
                                                                 {.value = 42}));

   // At this point MyService::GetData handler received the GetPartResponse.

Integration testing with ``pw_rpc``
===================================
``pw_rpc`` provides utilities to simplify writing integration tests for systems
that communicate with ``pw_rpc``. The integration test utitilies set up a socket
to use for IPC between an RPC server and client process.

The server binary uses the system RPC server facade defined
``pw_rpc_system_server/rpc_server.h``. The client binary uses the functions
defined in ``pw_rpc/integration_testing.h``:

.. cpp:var:: constexpr uint32_t kChannelId

   The RPC channel for integration test RPCs.

.. cpp:function:: pw::rpc::Client& pw::rpc::integration_test::Client()

   Returns the global RPC client for integration test use.

.. cpp:function:: pw::Status pw::rpc::integration_test::InitializeClient(int argc, char* argv[], const char* usage_args = "PORT")

   Initializes logging and the global RPC client for integration testing. Starts
   a background thread that processes incoming.

---------------------
Configuration options
---------------------
See :cc:`Configuration <pw_rpc_config>`.

------------------------------
Sharing server and client code
------------------------------
Streaming RPCs support writing multiple requests or responses. To facilitate
sharing code between servers and clients, ``pw_rpc`` provides the
``pw::rpc::Writer`` interface. On the client side, a client or bidirectional
streaming RPC call object (``ClientWriter`` or ``ClientReaderWriter``) can be
used as a ``pw::rpc::Writer&``. On the server side, a server or bidirectional
streaming RPC call object (``ServerWriter`` or ``ServerReaderWriter``) can be
used as a ``pw::rpc::Writer&``. Call ``as_writer()`` to get a ``Writer&`` of the
client or server call object.

----------------------------
Encoding and sending packets
----------------------------
``pw_rpc`` has to manage interactions among multiple RPC clients, servers,
client calls, and server calls. To safely synchronize these interactions with
minimal overhead, ``pw_rpc`` uses a single, global mutex (when
``PW_RPC_USE_GLOBAL_MUTEX`` is enabled).

Because ``pw_rpc`` uses a global mutex, it also uses a global buffer to encode
outgoing packets. The size of the buffer is set with
``PW_RPC_ENCODING_BUFFER_SIZE_BYTES``, which defaults to 512 B. If dynamic
allocation is enabled, this size does not affect how large RPC messages can be,
but it is still used for sizing buffers in test utilities.

Users of ``pw_rpc`` must implement the :cpp:class:`pw::rpc::ChannelOutput`
interface.

.. _module-pw_rpc-ChannelOutput:
.. cpp:class:: pw::rpc::ChannelOutput

   ``pw_rpc`` endpoints use :cpp:class:`ChannelOutput` instances to send
   packets.  Systems that integrate pw_rpc must use one or more
   :cpp:class:`ChannelOutput` instances.

   .. cpp:member:: static constexpr size_t kUnlimited = std::numeric_limits<size_t>::max()

      Value returned from :cpp:func:`MaximumTransmissionUnit` to indicate an
      unlimited MTU.

   .. cpp:function:: virtual size_t MaximumTransmissionUnit()

      Returns the size of the largest packet the :cpp:class:`ChannelOutput` can
      send. :cpp:class:`ChannelOutput` implementations should only override this
      function if they impose a limit on the MTU. The default implementation
      returns :cpp:member:`kUnlimited`, which indicates that there is no MTU
      limit.

   .. cpp:function:: virtual pw::Status Send(span<std::byte> packet)

      Sends an encoded RPC packet. Returns OK if further packets may be sent,
      even if the current packet could not be sent. Returns any other status if
      the Channel is no longer able to send packets.

      The RPC system's internal lock is held while this function is
      called. Avoid long-running operations, since these will delay any other
      users of the RPC system.

      .. danger::

         No ``pw_rpc`` APIs may be accessed in this function! Implementations
         MUST NOT access any RPC endpoints (:cpp:class:`pw::rpc::Client`,
         :cpp:class:`pw::rpc::Server`) or call objects
         (:cpp:class:`pw::rpc::ServerReaderWriter`
         :cpp:class:`pw::rpc::ClientReaderWriter`, etc.) inside the
         :cpp:func:`Send` function or any descendent calls. Doing so will result
         in deadlock! RPC APIs may be used by other threads, just not within
         :cpp:func:`Send`.

         The buffer provided in ``packet`` must NOT be accessed outside of this
         function. It must be sent immediately or copied elsewhere before the
         function returns.
