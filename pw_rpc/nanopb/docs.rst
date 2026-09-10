.. _module-pw_rpc_nanopb:

===============
C++ with Nanopb
===============
``pw_rpc`` can generate services which encode/decode RPC requests and responses
as nanopb message structs.

Usage
=====
To enable nanopb code generation, the build argument
``dir_pw_third_party_nanopb`` must be set to point to a local nanopb
installation. Nanopb 0.4 is recommended, but Nanopb 0.3 is also supported.

Define a ``pw_proto_library`` containing the .proto file defining your service
(and optionally other related protos), then depend on the ``nanopb_rpc``
version of that library in the code implementing the service.

.. code-block::

   # chat/BUILD.gn

   import("$dir_pw_build/target_types.gni")
   import("$dir_pw_protobuf_compiler/proto.gni")

   pw_proto_library("chat_protos") {
     sources = [ "chat_protos/chat_service.proto" ]
   }

   # Library that implements the Chat service.
   pw_source_set("chat_service") {
     sources = [
       "chat_service.cc",
       "chat_service.h",
     ]
     public_deps = [ ":chat_protos.nanopb_rpc" ]
   }

A C++ header file is generated for each input .proto file, with the ``.proto``
extension replaced by ``.rpc.pb.h``. For example, given the input file
``chat_protos/chat_service.proto``, the generated header file will be placed
at the include path ``"chat_protos/chat_service.rpc.pb.h"``.

Generated code API
==================
All examples in this document use the following RPC service definition.

.. literalinclude:: ../examples/chat_service.proto
   :language: protobuf
   :start-after: [pw_rpc-examples-chat-proto]
   :end-before: [pw_rpc-examples-chat-proto]

Server-side
-----------
A C++ class is generated for each service in the .proto file. The class is
located within a special ``pw_rpc::nanopb`` sub-namespace of the file's package.

The generated class is a base class which must be derived to implement the
service's methods. The base class is templated on the derived class.

.. literalinclude:: ../examples/nanopb_chat_service.cc
   :language: cpp
   :start-after: [pw_rpc-nanopb-service-impl]
   :end-before: [pw_rpc-nanopb-service-impl]

The writer and reader helper APIs provide methods to stream and finish calls:

.. cpp:function:: Status NanopbServerWriter::Write(const Response& response)

  Writes a single response message to the stream. The returned status indicates
  whether the write was successful.

.. cpp:function:: Status NanopbServerWriter::Finish(Status status = OkStatus())

  Closes the stream and sends back the RPC's overall status to the client.

.. cpp:function:: Status NanopbServerWriter::TryFinish(Status status = OkStatus())

  Closes the stream and sends back the RPC's overall status to the client only
  if the final packet is successfully sent.

.. attention::

  Make sure to use ``std::move`` when passing the ``NanopbServerWriter`` around to
  avoid accidentally closing it and ending the RPC.

.. cpp:function:: Status NanopbServerReader::Finish(const Response& response, Status status = OkStatus())

  Sends the final unary response message and status to the client, closing the stream.

.. cpp:function:: void NanopbServerReader::set_on_next(Function<void(const Request&)>&& on_next)

  Sets the callback invoked when a new request message arrives from the client.

.. cpp:function:: Status NanopbServerReaderWriter::Write(const Response& response)

  Writes a single response message to the stream.

.. cpp:function:: Status NanopbServerReaderWriter::Finish(Status status = OkStatus())

  Closes the stream and sends back the RPC's overall status to the client.

.. cpp:function:: void NanopbServerReaderWriter::set_on_next(Function<void(const Request&)>&& on_next)

  Sets the callback invoked when an incoming request message arrives from the client.

Client-side
-----------
A corresponding client class is generated for every service defined in the proto
file. To allow multiple types of clients to exist, it is placed under the
``pw_rpc::nanopb`` namespace. The ``Client`` class is nested under
``pw_rpc::nanopb::ServiceName``. For example, the ``Chat`` service would create
``chat::pw_rpc::nanopb::Chat::Client``.

Service clients are instantiated with a reference to the RPC client through
which they will send requests, and the channel ID they will use.

.. admonition:: Callback invocation

  RPC callbacks are invoked synchronously from ``Client::ProcessPacket``.

Unary RPC
~~~~~~~~~
A unary RPC call takes the request struct and a callback to invoke when a
response is received. The callback receives the RPC's status and response
struct.

Server streaming RPC
~~~~~~~~~~~~~~~~~~~~
A server streaming RPC call takes the initial request struct and two callbacks.
The first is invoked on every stream response received, and the second is
invoked once the stream is complete with its overall status.

Client streaming RPC
~~~~~~~~~~~~~~~~~~~~
A client streaming RPC call returns a ``NanopbClientWriter`` object used to send
a stream of requests, and takes a callback invoked when the server's final
response arrives:

.. literalinclude:: ../examples/nanopb_chat_service.cc
   :language: cpp
   :start-after: [pw_rpc-nanopb-client-streaming-call]
   :end-before: [pw_rpc-nanopb-client-streaming-call]

Bidirectional streaming RPC
~~~~~~~~~~~~~~~~~~~~~~~~~~~
A bidirectional streaming RPC call returns a ``NanopbClientReaderWriter`` object
used to send requests, and takes callbacks for incoming stream responses and stream
completion:

.. literalinclude:: ../examples/nanopb_chat_service.cc
   :language: cpp
   :start-after: [pw_rpc-nanopb-client-bidi-streaming-call]
   :end-before: [pw_rpc-nanopb-client-bidi-streaming-call]

Example usage
^^^^^^^^^^^^^
The following example demonstrates how to call an RPC method using a nanopb
service client and receive the response.

.. literalinclude:: ../examples/nanopb_chat_service.cc
   :language: cpp
   :start-after: [pw_rpc-nanopb-client-full-example]
   :end-before: [pw_rpc-nanopb-client-full-example]

Zephyr
======
To enable ``pw_rpc.nanopb.*`` for Zephyr add ``CONFIG_PIGWEED_RPC_NANOPB=y`` to
the project's configuration. This will enable the Kconfig menu for the
following:

* ``pw_rpc.nanopb.method`` which can be enabled via
  ``CONFIG_PIGWEED_RPC_NANOPB_METHOD=y``.
* ``pw_rpc.nanopb.method_union`` which can be enabled via
  ``CONFIG_PIGWEED_RPC_NANOPB_METHOD_UNION=y``.
* ``pw_rpc.nanopb.client`` which can be enabled via
  ``CONFIG_PIGWEED_RPC_NANOPB_CLIENT=y``.
* ``pw_rpc.nanopb.common`` which can be enabled via
  ``CONFIG_PIGWEED_RPC_NANOPB_COMMON=y``.
* ``pw_rpc.nanopb.echo_service`` which can be enabled via
  ``CONFIG_PIGWEED_RPC_NANOPB_ECHO_SERVICE=y``.
