.. _module-pw_rpc2-protocol:

=============
Wire protocol
=============
.. pigweed-module-subpage::
   :name: pw_rpc2

Pigweed RPC 2 uses a binary-framed wire protocol designed for low processing
overhead. Each packet consists of a fixed-size header (dependent on the packet
type) sharing a common prefix followed by type-specific fields. In packets with
payloads, the payload is located immediately following the header.

All multi-byte numeric fields are encoded in **little-endian** byte order.

----------------------------------
Connection state and establishment
----------------------------------
As RPC 2 runs over a generic ``Socket`` interface, a newly established
connection must first identify that the peer is a compatible RPC endpoint
before regular data flow can begin.

To achieve this, RPC 2 uses two distinct categories of packets:

- **Handshake packets** are used during the initial establishment phase. They
  contain magic numbers which identify the sender as an RPC endpoint.
- **RPC packets** are used for regular data exchange. After the handshake
  completes, all subsequent packets over the connection are one of these.

-------------------
Three-way handshake
-------------------
Each connection initiated by an RPC client and accepted by a server begins with
a three-way handshake loosely modeled after TCP. Its purpose is to identify the
endpoints and ensure compatibility. In the future, it may be extended to support
negotiation of connection parameters.

Handshake flow
==============
The handshake begins when the client opens the connection.

1. The client sends a ``SYN`` (type 1) with its protocol version (currently
   always ``1``).
2. The server responds with a ``SYN_ACK`` (type 2) containing the minimum of its
   version and the client's (currently also ``1``).
3. The client sends an ``ACK`` (type 3), accepting the server's version, to
   complete the handshake.

As there is currently only version ``1``, this handshake primarily establishes
that both endpoints speak RPC. However, clients and servers should be written to
accept newer versions from the peer, falling back if they aren't supported.

If either endpoint receives an invalid or unexpected packet during the
handshake, it should terminate the connection.

.. mermaid::
   :alt: 3-way handshake flow
   :align: center

   sequenceDiagram
       autonumber
       actor C as Initiator (Client)
       actor S as Responder (Server)

       C->>S: SYN (version = 1)
       Note over S: Validate magic ('PRPC')
       S->>C: SYN_ACK (negotiated_version)
       Note over C: Validate response magic and version
       C->>S: ACK (negotiated_version)
       Note over C,S: Connection established, begin RPC phase
       C->>S: REQUEST (service_id, method_id, call_id)

Handshake packet
================
The handshake uses a dedicated 8-byte packet.

.. list-table::
   :header-rows: 1
   :widths: 15 15 20 50

   * - Offset
     - Size
     - Name
     - Description
   * - 0
     - 4
     - ``magic`` (``uint32_t``)
     - ``0x43505250`` ("PRPC")
   * - 4
     - 1
     - ``version`` (``uint8_t``)
     - Protocol version, currently ``1``
   * - 5
     - 1
     - ``type`` (``uint8_t``)
     - ``1`` (SYN), ``2`` (SYN_ACK), or ``3`` (ACK)
   * - 6
     - 2
     - ``reserved`` (``uint16_t``)
     - Written as ``0``, ignored on receipt

-----------
RPC packets
-----------
Once the handshake is complete, RPC packets are sent over the connection.

RPC call flows
==============
The diagrams below show the lifecycles of each of the four RPC call types.
Note that the server may always terminate the RPC early by sending a final
``RESPONSE`` or ``SERVER_STREAM_END`` packet.

Unary RPC
---------
.. mermaid::
   :alt: Unary RPC packet sequence
   :align: center

   sequenceDiagram
       participant C as Client
       participant S as Server

       C->>S: REQUEST (request payload)
       S->>C: RESPONSE (response payload)

Server streaming RPC
--------------------
.. mermaid::
   :alt: Server streaming RPC packet sequence
   :align: center

   sequenceDiagram
       participant C as Client
       participant S as Server

       C->>S: REQUEST (request payload)
       loop Zero or more
           S->>C: SERVER_MESSAGE (response payload)
       end
       S->>C: SERVER_STREAM_END

Client streaming RPC
--------------------
.. mermaid::
   :alt: Client streaming RPC packet sequence
   :align: center

   sequenceDiagram
       participant C as Client
       participant S as Server

       C->>S: REQUEST (no payload)
       loop Zero or more
           C->>S: CLIENT_MESSAGE (request payload)
       end
       C->>S: CLIENT_STREAM_END
       S->>C: RESPONSE (response payload)

Bidirectional streaming RPC
---------------------------
.. mermaid::
   :alt: Bidirectional streaming RPC packet sequence
   :align: center

   sequenceDiagram
       participant C as Client
       participant S as Server

       C->>S: REQUEST (no payload)
       par Client stream
           loop Zero or more
               C->>S: CLIENT_MESSAGE (request payload)
           end
           C->>S: CLIENT_STREAM_END
       and Server stream
           loop Zero or more
               S->>C: SERVER_MESSAGE (response payload)
           end
       end
       S->>C: SERVER_STREAM_END

RPC packet header structure
===========================
Every RPC packet begins with a common 5-byte header:

.. list-table::
   :header-rows: 1
   :widths: 15 15 20 50

   * - Offset
     - Size
     - Name
     - Description
   * - 0
     - 4
     - ``call_id`` (``uint32_t``)
     - Client-assigned ID for this specific invocation. Selected when sending
       the initial request, and echoed afterwards.
   * - 4
     - 1
     - ``type`` (``uint8_t``)
     - The type of packet. Determines further fields.

.. _module-pw_rpc2-protocol-types:

RPC packet types
================
The protocol defines eight types of RPC packet. In each type, bit 0 indicates
the direction of the packet; a value of 0 means client-to-server, while 1
means server-to-client.

.. list-table::
   :widths: 20 10 15 55
   :header-rows: 1

   * - Packet type
     - Value
     - Direction
     - Description

   * - ``REQUEST``
     - ``0x02``
     - Client → server
     - Client initiates an RPC call. Specifies the target ``service_id``,
       ``method_id``, and optional initial payload.

   * - ``RESPONSE``
     - ``0x03``
     - Server → client
     - The single final response payload from the server in a unary or client
       streaming RPC.

   * - ``CLIENT_MESSAGE``
     - ``0x04``
     - Client → server
     - A stream data message in a client or bidirectional streaming RPC.

   * - ``SERVER_MESSAGE``
     - ``0x05``
     - Server → client
     - A stream data message in a server or bidirectional streaming RPC.

   * - ``CLIENT_STREAM_END``
     - ``0x06``
     - Client → server
     - Signals normal completion of a client-to-server stream
       (client streaming, bidirectional). The server may continue transmitting.

   * - ``SERVER_STREAM_END``
     - ``0x07``
     - Server → client
     - Signals normal completion of a server-to-client stream
       (server streaming, bidirectional) and finishes the RPC call.

   * - ``CLIENT_ERROR``
     - ``0x08``
     - Client → server
     - Signals abnormal call termination or a protocol fault from the client.
       Carries a 16-bit ``ClientError`` value.

   * - ``SERVER_ERROR``
     - ``0x09``
     - Server → client
     - Signals abnormal call termination or a protocol fault from the server.
       Carries a 16-bit ``ServerError`` value.

Packet structures
=================
The fields of each RPC packet are listed below.

.. admonition:: Payload framing

   RPC 2 runs over a datagram socket, so each packet from the peer is received
   in full. There is no length field in the protocol itself. Transport
   implementations used with RPC must ensure that buffers read from the peer
   over an RPC connection contain only, and exactly, the bytes of a single
   packet without any padding.

``REQUEST`` packet (type ``0x02``)
----------------------------------
Initiated by a client to invoke an RPC. Its header size is **13 bytes**.

.. list-table::
   :header-rows: 1
   :widths: 15 15 20 50

   * - Offset
     - Size
     - Name
     - Description
   * - 0
     - 5
     - Header
     - Common RPC packet header with ``type`` ``0x02``
   * - 5
     - 4
     - ``service_id`` (``uint32_t``)
     - ID of the service to invoke
   * - 9
     - 4
     - ``method_id`` (``uint32_t``)
     - ID of the method to invoke
   * - 13
     - Variable
     - Payload
     - Remainder of the packet

``RESPONSE`` packet (type ``0x03``)
-----------------------------------
Carries the final response from the server in a unary or client streaming RPC.
Its header size is **5 bytes**.

.. list-table::
   :header-rows: 1
   :widths: 15 15 20 50

   * - Offset
     - Size
     - Name
     - Description
   * - 0
     - 5
     - Header
     - Common RPC packet header with ``type`` ``0x03``
   * - 5
     - Variable
     - Payload
     - Remainder of the packet

``CLIENT_MESSAGE`` and ``SERVER_MESSAGE`` packets (types ``0x04`` and ``0x05``)
-------------------------------------------------------------------------------
Carries a streamed datagram from client to server (``0x04``) or server to client
(``0x05``). Its header size is **5 bytes**.

.. list-table::
   :header-rows: 1
   :widths: 15 15 20 50

   * - Offset
     - Size
     - Name
     - Description
   * - 0
     - 5
     - Header
     - Common RPC packet header with ``type`` ``0x04`` or ``0x05``
   * - 5
     - Variable
     - Payload
     - Remainder of the packet

``CLIENT_STREAM_END`` and ``SERVER_STREAM_END`` packets (types ``0x06`` and ``0x07``)
-------------------------------------------------------------------------------------
Signals the normal completion of a stream from client to server (``0x06``) or
server to client (``0x07``). Its header size is **5 bytes**.

.. list-table::
   :header-rows: 1
   :widths: 15 15 20 50

   * - Offset
     - Size
     - Name
     - Description
   * - 0
     - 5
     - Header
     - Common RPC packet header with ``type`` ``0x06`` or ``0x07``

``CLIENT_ERROR`` and ``SERVER_ERROR`` packets (types ``0x08`` and ``0x09``)
---------------------------------------------------------------------------
Aborts an RPC call immediately. Its header size is **7 bytes**.

.. list-table::
   :header-rows: 1
   :widths: 15 15 20 50

   * - Offset
     - Size
     - Name
     - Description
   * - 0
     - 5
     - Header
     - Common RPC packet header with ``type`` ``0x08`` or ``0x09``
   * - 5
     - 2
     - ``error`` (``uint16_t``)
     - ``ClientError`` (``0x08``) or ``ServerError`` (``0x09``) value

RPC error codes
===============
``CLIENT_ERROR`` and ``SERVER_ERROR`` packets contain one of several error
codes, whose meanings are listed below.

..
   # LINT.IfChange(rpc2_error_codes)

.. list-table:: Client error codes
   :header-rows: 1
   :widths: 10 40 50

   * - Value
     - Name
     - Description
   * - ``0``
     - ``OK``
     - No error. Never sent.
   * - ``1``
     - ``UNKNOWN``
     - Unrecognized error code.
   * - ``2``
     - ``INTERNAL``
     - A bug in the RPC implementation.
   * - ``3``
     - ``CANCELLED``
     - The call was deliberately cancelled by the client-side application code.
   * - ``4``
     - ``RECEIVED_PACKET_FOR_SERVER``
     - The client received a packet type that may only be sent from a client
       to a server.

.. list-table:: Server error codes
   :header-rows: 1
   :widths: 10 40 50

   * - Value
     - Name
     - Description
   * - ``0``
     - ``OK``
     - No error. Never sent.
   * - ``1``
     - ``UNKNOWN``
     - Unrecognized error code.
   * - ``2``
     - ``INTERNAL``
     - A bug in the RPC implementation.
   * - ``3``
     - ``CANCELLED``
     - The call was deliberately cancelled by the server-side application code.
   * - ``4``
     - ``RECEIVED_PACKET_FOR_CLIENT``
     - The server received a packet type that may only be sent from a server
       to a client.
   * - ``5``
     - ``DROPPED_WITHOUT_RESPONSE``
     - The server released a unary call without sending a response or
       cancelling it.
   * - ``6``
     - ``SERVICE_UNREGISTERED``
     - The target service was unregistered from the server while the call was
       running.
   * - ``7``
     - ``UNKNOWN_SERVICE``
     - The requested service is not registered on the server.
   * - ``8``
     - ``UNKNOWN_METHOD``
     - The requested method is not registered on the target service.
   * - ``9``
     - ``INVALID_REQUEST_PAYLOAD``
     - The request payload was invalid.
   * - ``10``
     - ``FAILED_TO_ALLOCATE_CALL``
     - Failed to allocate call state for an incoming request.
   * - ``11``
     - ``FAILED_TO_ALLOCATE_CALL_RESOURCES_WHILE_RUNNING``
     - Failed to allocate necessary resources while running the call.

..
   # LINT.ThenChange(//pw_rpc2/cpp/public/pw_rpc2/internal/protocol_status.h:cpp_rpc2_error_codes)
