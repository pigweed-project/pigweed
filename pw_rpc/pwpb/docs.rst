.. _module-pw_rpc_pw_protobuf:

====================
C++ with pw_protobuf
====================
.. caution::

   If you're starting a new project, Pigweed recommends Nanopb over
   ``pw_protobuf``. See :ref:`module-pw_rpc-guides-headers`.

``pw_rpc`` can generate services which encode/decode RPC requests and responses
as ``pw_protobuf`` message structs.

Usage
=====
Define a ``pw_proto_library`` containing the .proto file defining your service
(and optionally other related protos), then depend on the ``pwpb_rpc``
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
     public_deps = [ ":chat_protos.pwpb_rpc" ]
   }

A C++ header file is generated for each input .proto file, with the ``.proto``
extension replaced by ``.rpc.pwpb.h``. For example, given the input file
``chat_protos/chat_service.proto``, the generated header file will be placed
at the include path ``"chat_protos/chat_service.rpc.pwpb.h"``.

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
located within a special ``pw_rpc::pwpb`` sub-namespace of the file's package.

The generated class is a base class which must be derived to implement the
service's methods. The base class is templated on the derived class.

.. literalinclude:: ../examples/pwpb_chat_service.cc
   :language: cpp
   :start-after: [pw_rpc-pwpb-service-impl]
   :end-before: [pw_rpc-pwpb-service-impl]

The writer and reader helper APIs provide methods to stream and finish calls:

.. cpp:function:: Status PwpbServerWriter::Write(const Response::Message& response)

  Writes a single response message to the stream. The returned status indicates
  whether the write was successful.

.. cpp:function:: Status PwpbServerWriter::Finish(Status status = OkStatus())

  Closes the stream and sends back the RPC's overall status to the client.

.. cpp:function:: Status PwpbServerWriter::TryFinish(Status status = OkStatus())

  Closes the stream and sends back the RPC's overall status to the client only
  if the final packet is successfully sent.

.. attention::

  Make sure to use ``std::move`` when passing the ``PwpbServerWriter`` around to
  avoid accidentally closing it and ending the RPC.

.. _module-pw_rpc_pw_protobuf-client:

Client-side
-----------
A corresponding client class is generated for every service defined in the proto
file. To allow multiple types of clients to exist, it is placed under the
``pw_rpc::pwpb`` namespace. The ``Client`` class is nested under
``pw_rpc::pwpb::ServiceName``. For example, the ``Chat`` service would create
``chat::pw_rpc::pwpb::Chat::Client``.

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
A client streaming RPC call returns a ``PwpbClientWriter`` object used to send
a stream of requests, and takes a callback invoked when the server's final
response arrives:

.. literalinclude:: ../examples/pwpb_chat_service.cc
   :language: cpp
   :start-after: [pw_rpc-pwpb-client-streaming-call]
   :end-before: [pw_rpc-pwpb-client-streaming-call]

Bidirectional streaming RPC
~~~~~~~~~~~~~~~~~~~~~~~~~~~
A bidirectional streaming RPC call returns a ``PwpbClientReaderWriter`` object
used to send requests, and takes callbacks for incoming stream responses and stream
completion:

.. literalinclude:: ../examples/pwpb_chat_service.cc
   :language: cpp
   :start-after: [pw_rpc-pwpb-client-bidi-streaming-call]
   :end-before: [pw_rpc-pwpb-client-bidi-streaming-call]

Example usage
^^^^^^^^^^^^^
The following example demonstrates how to call an RPC method using a pw_protobuf
service client and receive the response.

.. literalinclude:: ../examples/pwpb_chat_service.cc
   :language: cpp
   :start-after: [pw_rpc-pwpb-client-full-example]
   :end-before: [pw_rpc-pwpb-client-full-example]
