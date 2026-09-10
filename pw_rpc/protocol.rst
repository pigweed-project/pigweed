.. _module-pw_rpc-protocol:

=============
Wire protocol
=============
.. pigweed-module-subpage::
   :name: pw_rpc

Pigweed RPC servers and clients communicate using ``pw_rpc`` packets. These
packets are used to send requests and responses, control streams, cancel ongoing
RPCs, and report errors.

Packet format
=============
Pigweed RPC packets consist of a type and a set of fields. The packets are
encoded as protocol buffers. The full packet format is described in
``pw_rpc/pw_rpc/internal/packet.proto``.

.. literalinclude:: internal/packet.proto
   :language: protobuf
   :lines: 14-

The packet type and RPC type determine which fields are present in a Pigweed RPC
packet. Each packet type is only sent by either the client or the server.
These tables describe the meaning of and fields included with each packet type.

Client-to-server packets
------------------------
+---------------------------+-------------------------------------+
| packet type               | description                         |
+===========================+=====================================+
| REQUEST                   | Invoke an RPC                       |
|                           |                                     |
|                           | .. code-block:: text                |
|                           |                                     |
|                           |   - channel_id                      |
|                           |   - service_id                      |
|                           |   - method_id                       |
|                           |   - payload                         |
|                           |     (unary & server streaming only) |
|                           |   - call_id (optional)              |
|                           |                                     |
+---------------------------+-------------------------------------+
| CLIENT_STREAM             | Message in a client stream          |
|                           |                                     |
|                           | .. code-block:: text                |
|                           |                                     |
|                           |   - channel_id                      |
|                           |   - service_id                      |
|                           |   - method_id                       |
|                           |   - payload                         |
|                           |   - call_id (if set in REQUEST)     |
|                           |                                     |
+---------------------------+-------------------------------------+
| CLIENT_REQUEST_COMPLETION | Client requested stream completion  |
|                           |                                     |
|                           | .. code-block:: text                |
|                           |                                     |
|                           |   - channel_id                      |
|                           |   - service_id                      |
|                           |   - method_id                       |
|                           |   - call_id (if set in REQUEST)     |
|                           |                                     |
+---------------------------+-------------------------------------+
| CLIENT_ERROR              | Abort an ongoing RPC                |
|                           |                                     |
|                           | .. code-block:: text                |
|                           |                                     |
|                           |   - channel_id                      |
|                           |   - service_id                      |
|                           |   - method_id                       |
|                           |   - status                          |
|                           |   - call_id (if set in REQUEST)     |
|                           |                                     |
+---------------------------+-------------------------------------+

**Client errors**

The client sends ``CLIENT_ERROR`` packets to a server when it receives a packet
it did not request. If possible, the server should abort it.

The status code indicates the type of error. The status code is logged, but all
status codes result in the same action by the server: aborting the RPC.

* ``CANCELLED`` -- The client requested that the RPC be cancelled.
* ``ABORTED`` -- The RPC was aborted due its channel being closed.
* ``NOT_FOUND`` -- Received a packet for a service method the client does not
  recognize.
* ``FAILED_PRECONDITION`` -- Received a packet for a service method that the
  client did not invoke.
* ``DATA_LOSS`` -- Received a corrupt packet for a pending service method.
* ``INVALID_ARGUMENT`` -- The server sent a packet type to an RPC that does not
  support it (a ``SERVER_STREAM`` was sent to an RPC with no server stream).
* ``UNAVAILABLE`` -- Received a packet for an unknown channel.

Server-to-client packets
------------------------
+-------------------+-------------------------------------+
| packet type       | description                         |
+===================+=====================================+
| RESPONSE          | The RPC is complete                 |
|                   |                                     |
|                   | .. code-block:: text                |
|                   |                                     |
|                   |   - channel_id                      |
|                   |   - service_id                      |
|                   |   - method_id                       |
|                   |   - status                          |
|                   |   - payload                         |
|                   |     (unary & client streaming only) |
|                   |   - call_id (if set in REQUEST)     |
|                   |                                     |
+-------------------+-------------------------------------+
| SERVER_STREAM     | Message in a server stream          |
|                   |                                     |
|                   | .. code-block:: text                |
|                   |                                     |
|                   |   - channel_id                      |
|                   |   - service_id                      |
|                   |   - method_id                       |
|                   |   - payload                         |
|                   |   - call_id (if set in REQUEST)     |
|                   |                                     |
+-------------------+-------------------------------------+
| SERVER_ERROR      | Received unexpected packet          |
|                   |                                     |
|                   | .. code-block:: text                |
|                   |                                     |
|                   |   - channel_id                      |
|                   |   - service_id (if relevant)        |
|                   |   - method_id (if relevant)         |
|                   |   - status                          |
|                   |   - call_id (if set in REQUEST)     |
|                   |                                     |
+-------------------+-------------------------------------+

All server packets contain the same ``call_id`` that was set in the initial
request made by the client, if any.

**Server errors**

The server sends ``SERVER_ERROR`` packets when it receives a packet it cannot
process. The client should abort any RPC for which it receives an error. The
status field indicates the type of error.

* ``NOT_FOUND`` -- The requested service or method does not exist.
* ``FAILED_PRECONDITION`` -- A client stream or cancel packet was sent for an
  RPC that is not pending.
* ``INVALID_ARGUMENT`` -- The client sent a packet type to an RPC that does not
  support it (a ``CLIENT_STREAM`` was sent to an RPC with no client stream).
* ``RESOURCE_EXHAUSTED`` -- The request came on a new channel, but a channel
  could not be allocated for it.
* ``ABORTED`` -- The RPC was aborted due its channel being closed.
* ``INTERNAL`` -- The server was unable to respond to an RPC due to an
  unrecoverable internal error.
* ``UNAVAILABLE`` -- Received a packet for an unknown channel.

Invoking a service method
=========================
Calling an RPC requires a specific sequence of packets. This section describes
the protocol for calling service methods of each type: unary, server streaming,
client streaming, and bidirectional streaming.

The basic flow for all RPC invocations is as follows:

* Client sends a ``REQUEST`` packet. Includes a payload for unary & server
  streaming RPCs.
* For client and bidirectional streaming RPCs, the client may send any number of
  ``CLIENT_STREAM`` packets with payloads.
* For server and bidirectional streaming RPCs, the server may send any number of
  ``SERVER_STREAM`` packets.
* The server sends a ``RESPONSE`` packet. Includes a payload for unary & client
  streaming RPCs. The RPC is complete.

The client may cancel an ongoing RPC at any time by sending a ``CLIENT_ERROR``
packet with status ``CANCELLED``. The server may finish an ongoing RPC at any
time by sending the ``RESPONSE`` packet.

Unary RPC
---------
In a unary RPC, the client sends a single request and the server sends a single
response.

.. mermaid::
   :alt: Unary RPC
   :align: center

   sequenceDiagram
       participant C as Client
       participant S as Server
       C->>+S: request
       Note left of C: PacketType.REQUEST<br>channel ID<br>service ID<br>method ID<br>payload

       S->>-C: response
       Note right of S: PacketType.RESPONSE<br>channel ID<br>service ID<br>method ID<br>payload<br>status

The client may attempt to cancel a unary RPC by sending a ``CLIENT_ERROR``
packet with status ``CANCELLED``. The server sends no response to a cancelled
RPC. If the server processes the unary RPC synchronously (the handling thread
sends the response), it may not be possible to cancel the RPC.

.. mermaid::
   :alt: Cancelled Unary RPC
   :align: center

   sequenceDiagram
       participant C as Client
       participant S as Server
       C->>+S: request
       Note left of C: PacketType.REQUEST<br>channel ID<br>service ID<br>method ID<br>payload

       C->>S: cancel
       Note left of C: PacketType.CLIENT_ERROR<br>channel ID<br>service ID<br>method ID<br>status=CANCELLED


Server streaming RPC
--------------------
In a server streaming RPC, the client sends a single request and the server
sends any number of ``SERVER_STREAM`` packets followed by a ``RESPONSE`` packet.

.. mermaid::
   :alt: Server Streaming RPC
   :align: center

   sequenceDiagram
       participant C as Client
       participant S as Server
       C->>+S: request
       Note left of C: PacketType.REQUEST<br>channel ID<br>service ID<br>method ID<br>payload

       S-->>C: messages (zero or more)
       Note right of S: PacketType.SERVER_STREAM<br>channel ID<br>service ID<br>method ID<br>payload

       S->>-C: done
       Note right of S: PacketType.RESPONSE<br>channel ID<br>service ID<br>method ID<br>status


The client may terminate a server streaming RPC by sending a ``CLIENT_STREAM``
packet with status ``CANCELLED``. The server sends no response.

.. mermaid::
   :alt: Cancelled Server Streaming RPC
   :align: center

   sequenceDiagram
       participant C as Client
       participant S as Server
       C->>S: request
       Note left of C: PacketType.REQUEST<br>channel ID<br>service ID<br>method ID<br>payload

       S-->>C: messages (zero or more)
       Note right of S: PacketType.SERVER_STREAM<br>channel ID<br>service ID<br>method ID<br>payload

       C->>S: cancel
       Note left of C: PacketType.CLIENT_ERROR<br>channel ID<br>service ID<br>method ID<br>status=CANCELLED

Client streaming RPC
--------------------
In a client streaming RPC, the client starts the RPC by sending a ``REQUEST``
packet with no payload. It then sends any number of messages in
``CLIENT_STREAM`` packets, followed by a ``CLIENT_REQUEST_COMPLETION``. The server sends
a single ``RESPONSE`` to finish the RPC.

.. mermaid::
   :alt: Client Streaming RPC
   :align: center

   sequenceDiagram
       participant C as Client
       participant S as Server
       C->>S: start
       Note left of C: PacketType.REQUEST<br>channel ID<br>service ID<br>method ID<br>payload

       C-->>S: messages (zero or more)
       Note left of C: PacketType.CLIENT_STREAM<br>channel ID<br>service ID<br>method ID<br>payload

       C->>S: done
       Note left of C: PacketType.CLIENT_REQUEST_COMPLETION<br>channel ID<br>service ID<br>method ID

       S->>C: response
       Note right of S: PacketType.RESPONSE<br>channel ID<br>service ID<br>method ID<br>payload<br>status

The server may finish the RPC at any time by sending its ``RESPONSE`` packet,
even if it has not yet received the ``CLIENT_REQUEST_COMPLETION`` packet. The client may
terminate the RPC at any time by sending a ``CLIENT_ERROR`` packet with status
``CANCELLED``.

.. mermaid::
   :alt: Cancelled Client Streaming RPC
   :align: center

   sequenceDiagram
       participant C as Client
       participant S as Server
       C->>S: start
       Note left of C: PacketType.REQUEST<br>channel ID<br>service ID<br>method ID

       C-->>S: messages (zero or more)
       Note left of C: PacketType.CLIENT_STREAM<br>channel ID<br>service ID<br>method ID<br>payload

       C->>S: cancel
       Note right of S: PacketType.CLIENT_ERROR<br>channel ID<br>service ID<br>method ID<br>status=CANCELLED

Bidirectional streaming RPC
---------------------------
In a bidirectional streaming RPC, the client sends any number of requests and
the server sends any number of responses. The client invokes the RPC by sending
a ``REQUEST`` with no payload. It sends a ``CLIENT_REQUEST_COMPLETION`` packet when it
has finished sending requests. The server sends a ``RESPONSE`` packet to finish
the RPC.

.. mermaid::
   :alt: Bidirectional Streaming RPC
   :align: center

   sequenceDiagram
       participant C as Client
       participant S as Server
       C->>S: start
       Note left of C: PacketType.REQUEST<br>channel ID<br>service ID<br>method ID<br>payload

       C-->>S: messages (zero or more)
       Note left of C: PacketType.CLIENT_STREAM<br>channel ID<br>service ID<br>method ID<br>payload

       C-->S: (messages in any order)

       S-->>C: messages (zero or more)
       Note right of S: PacketType.SERVER_STREAM<br>channel ID<br>service ID<br>method ID<br>payload

       C->>S: done
       Note left of C: PacketType.CLIENT_REQUEST_COMPLETION<br>channel ID<br>service ID<br>method ID

       S->>C: done
       Note right of S: PacketType.RESPONSE<br>channel ID<br>service ID<br>method ID<br>status


The server may finish the RPC at any time by sending the ``RESPONSE`` packet,
even if it has not received the ``CLIENT_REQUEST_COMPLETION`` packet. The client may
terminate the RPC at any time by sending a ``CLIENT_ERROR`` packet with status
``CANCELLED``.

.. mermaid::
   :alt: Client Streaming RPC
   :align: center

   sequenceDiagram
       participant C as Client
       participant S as Server
       C->>S: start
       Note left of C: PacketType.REQUEST<br>channel ID<br>service ID<br>method ID<br>payload

       C-->>S: messages (zero or more)
       Note left of C: PacketType.CLIENT_STREAM<br>channel ID<br>service ID<br>method ID<br>payload

       C->>S: done
       Note left of C: PacketType.CLIENT_REQUEST_COMPLETION<br>channel ID<br>service ID<br>method ID

       S->>C: response
       Note right of S: PacketType.RESPONSE<br>channel ID<br>service ID<br>method ID<br>payload<br>status
