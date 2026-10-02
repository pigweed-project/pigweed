.. _module-pw_rpc2-protocol:

=============
Wire protocol
=============
.. pigweed-module-subpage::
   :name: pw_rpc2

Pigweed RPC2 uses a binary-framed wire protocol designed for low processing
overhead. Each packet consists of a fixed-size header (dependent on the packet
type) sharing a common prefix followed by type-specific fields. In packets with
payloads, the payload is located immediately following the header.

All multi-byte numeric fields are encoded in **little-endian** byte order.

----------------------------------
Connection state and establishment
----------------------------------
As RPC2 runs over a generic ``Socket`` interface, a newly established connection
must first identify that the peer is a compatible RPC endpoint before regular
data flow can begin.

To achieve this, RPC2 uses two distinct categories of packets:

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
       C->>S: START (service_id, method_id, call_id)

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
Packets are labeled with their type flags; see
:ref:`module-pw_rpc2-protocol-types`. Note that the server may always terminate
the RPC early by sending an ``OK_TERMINAL`` packet, and either side may abort it
with an ``ERROR_TERMINAL`` packet.

Unary RPC
---------
.. mermaid::
   :alt: Unary RPC packet sequence
   :align: center

   sequenceDiagram
       participant C as Client
       participant S as Server

       C->>S: START | HAS_PAYLOAD | STREAM_END (request)
       S->>C: SERVER | HAS_PAYLOAD | OK_TERMINAL (response)

Server streaming RPC
--------------------
.. mermaid::
   :alt: Server streaming RPC packet sequence
   :align: center

   sequenceDiagram
       participant C as Client
       participant S as Server

       C->>S: START | HAS_PAYLOAD | STREAM_END (request)
       loop Zero or more
           S->>C: SERVER | HAS_PAYLOAD (response)
       end
       S->>C: SERVER | OK_TERMINAL

Client streaming RPC
--------------------
.. mermaid::
   :alt: Client streaming RPC packet sequence
   :align: center

   sequenceDiagram
       participant C as Client
       participant S as Server

       C->>S: START
       loop Zero or more
           C->>S: HAS_PAYLOAD (request)
       end
       C->>S: STREAM_END
       S->>C: SERVER | HAS_PAYLOAD | OK_TERMINAL (response)

Bidirectional streaming RPC
---------------------------
.. mermaid::
   :alt: Bidirectional streaming RPC packet sequence
   :align: center

   sequenceDiagram
       participant C as Client
       participant S as Server

       C->>S: START
       par Client stream
           loop Zero or more
               C->>S: HAS_PAYLOAD (request)
           end
           C->>S: STREAM_END
       and Server stream
           loop Zero or more
               S->>C: SERVER | HAS_PAYLOAD (response)
           end
       end
       S->>C: SERVER | OK_TERMINAL

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
The ``type`` byte of an RPC packet is not an enumeration. It is a bitfield of
independent properties, and each packet combines them:

.. code-block:: output

   Bit:    7   6   5     4   3        2          1           0
         +---+---+---+-----------+--------+-------------+---------+
         | 0 | 0 | 0 | CloseMode | START  | HAS_PAYLOAD | SERVER  |
         +---+---+---+-----------+--------+-------------+---------+

.. list-table::
   :widths: 15 10 20 55
   :header-rows: 1

   * - Bits
     - Mask
     - Name
     - Meaning
   * - 0
     - ``00001``
     - ``SERVER``
     - ``0``: sent by the client. ``1``: sent by the server.
   * - 1
     - ``00010``
     - ``HAS_PAYLOAD``
     - The packet carries a message, which may be empty, following its header.
       A packet without this bit ends with its header.
   * - 2
     - ``00100``
     - ``START``
     - The packet starts a new call. Its header includes the ``service_id``
       and ``method_id`` to invoke.
   * - 4:3
     - ``11000``
     - ``CloseMode``
     - - ``00`` (``OPEN``, ``00000``): The sender's stream stays open.
       - ``01`` (``STREAM_END``, ``01000``): The sender half-closes its
         stream.
       - ``10`` (``OK_TERMINAL``, ``10000``): The RPC completes successfully
         in both directions.
       - ``11`` (``ERROR_TERMINAL``, ``11000``): The RPC aborts in both
         directions with an error code following the header.
   * - 7:5
     - ``0xE0``
     - Reserved
     - Must be ``0``.

Only the following 14 combinations are valid. A packet of any other type is
rejected as malformed.

.. list-table::
   :widths: 10 35 55
   :header-rows: 1

   * - Value
     - Combination
     - Description
   * - ``00010``
     - ``HAS_PAYLOAD``
     - Client stream message.
   * - ``00011``
     - ``SERVER | HAS_PAYLOAD``
     - Server stream message.
   * - ``00100``
     - ``START``
     - Opens a client or bidirectional streaming call without a message.
   * - ``00110``
     - ``START | HAS_PAYLOAD``
     - Opens a client or bidirectional streaming call with its first message.
   * - ``01000``
     - ``STREAM_END``
     - Client half-closes its stream.
   * - ``01001``
     - ``SERVER | STREAM_END``
     - Server half-closes its stream. The RPC continues until the server ends it
       with ``10001``, ``10011``, or ``11001``. See :ref:`below
       <module-pw_rpc2-protocol-server-half-close>`.
   * - ``01010``
     - ``HAS_PAYLOAD | STREAM_END``
     - Client's final stream message, closing its stream.
   * - ``01011``
     - ``SERVER | HAS_PAYLOAD | STREAM_END``
     - Server's final stream message, half-closing its stream. Like ``01001``,
       the RPC continues until the server ends it.
   * - ``01100``
     - ``START | STREAM_END``
     - Opens a client or bidirectional streaming call and immediately closes
       the client's stream without sending a message.
   * - ``01110``
     - ``START | HAS_PAYLOAD | STREAM_END``
     - Starts a unary or server streaming call with its request, which may be
       empty. Also valid for a client streaming call with a single message.
   * - ``10001``
     - ``SERVER | OK_TERMINAL``
     - Server finishes its stream and completes the RPC.
   * - ``10011``
     - ``SERVER | HAS_PAYLOAD | OK_TERMINAL``
     - Server's response, completing a unary or client streaming RPC.
   * - ``11000``
     - ``ERROR_TERMINAL``
     - Client aborts the RPC. Carries a 16-bit ``ProtocolStatus`` value.
   * - ``11001``
     - ``SERVER | ERROR_TERMINAL``
     - Server aborts the RPC. Carries a 16-bit ``ProtocolStatus`` value.

A packet without ``HAS_PAYLOAD`` must end with its header. A receiver rejects
any packet without ``HAS_PAYLOAD`` that has trailing bytes as malformed.

When the server ends the RPC with ``10001`` or ``10011``, the client stops
sending on any stream it still has open.

.. _module-pw_rpc2-protocol-server-half-close:

Server half-close
-----------------
A server may half-close its stream with ``01001`` or ``01011`` to stop
sending messages while it continues to read the client's stream. This does not
end the RPC: the server still sends a terminal packet (``10001``, ``10011``,
or ``11001``) later, which carries the call's final status.

The protocol allows either the client or the server to half-close its stream.
The C++ server currently always ends the RPC when it closes its stream, for
consistency with gRPC. Server half-close packets are valid, however, so every
client must handle them: after a server half-close, a client drops any further
server messages and waits for the terminal packet before completing the call.

Method type mismatches
----------------------
Because the type byte states what a packet contains, an endpoint does not rely
on its own view of a method's type to parse a packet. If the client and server
disagree about a method's type, the endpoint that receives a packet its method
cannot accept fails the call with ``METHOD_TYPE_MISMATCH`` instead of misreading
or dropping a message:

* A unary or server streaming method takes a single request, so the server
  accepts only ``01110`` (``START | HAS_PAYLOAD | STREAM_END``) to start it.
  It rejects any other ``START`` packet with the server error
  ``METHOD_TYPE_MISMATCH`` without invoking the method.
* A unary or client streaming call takes a single response, so the client
  accepts only ``10011`` (``SERVER | HAS_PAYLOAD | OK_TERMINAL``) or an error
  (``11001``) from the server. For any other packet, it sends the client error
  ``METHOD_TYPE_MISMATCH``, which aborts the call on the server, before
  delivering anything from the packet. If the packet already ended the RPC
  (``10001``), the client sends nothing.

Either way, the client's call completes with ``FAILED_PRECONDITION``.

A streaming endpoint accepts ``01110`` or ``10011``, which represent a
stream of a single message. A mismatch that produces only these packets
completes normally because no message is lost or misread:

.. list-table::
   :widths: 30 30 40
   :header-rows: 1

   * - Client's method type
     - Server's method type
     - Result
   * - Unary
     - Client streaming
     - Works: the client sends ``01110`` and the server replies ``10011``.
   * - Unary
     - Server streaming
     - Fails, unless the server replies with a single ``10011``.
   * - Unary
     - Bidirectional streaming
     - Fails, unless the server replies with a single ``10011``.
   * - Server streaming
     - Unary
     - Works: the server's ``10011`` is a stream of one message.
   * - Server streaming
     - Client streaming
     - Works, as above.
   * - Server streaming
     - Bidirectional streaming
     - Works: the client sends ``01110`` and reads the server's stream.
   * - Client streaming
     - Unary or server streaming
     - Fails: the server requires ``01110``.
   * - Client streaming
     - Bidirectional streaming
     - Fails, unless the server replies with a single ``10011``.
   * - Bidirectional streaming
     - Unary or server streaming
     - Fails: the server requires ``01110``.
   * - Bidirectional streaming
     - Client streaming
     - Works: the server's ``10011`` is a stream of one message.

Packet structures
=================
The fields of each RPC packet are listed below.

.. admonition:: Payload framing

   RPC2 runs over a datagram socket, so each packet from the peer is received in
   full. There is no length field in the protocol itself. Transport
   implementations used with RPC must ensure that buffers read from the peer
   over an RPC connection contain only, and exactly, the bytes of a single
   packet without any padding.

Start packets (``START`` set)
-----------------------------
Starts an RPC. Its header size is **13 bytes**.

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
     - Common RPC packet header with ``START`` set
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
     - Remainder of the packet, present only if ``HAS_PAYLOAD`` is set

Error packets (``ERROR_TERMINAL`` set)
--------------------------------------
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
     - Common RPC packet header with ``type`` ``11000`` or ``11001``
   * - 5
     - 2
     - ``error`` (``uint16_t``)
     - ``ProtocolStatus`` value

All other packets
-----------------
Every other packet has a **5-byte** header consisting only of the common RPC
packet header. If ``HAS_PAYLOAD`` is set, the payload is the remainder of the
packet.

RPC error codes
===============
Error packets (``11000`` and ``11001``) carry a ``ProtocolStatus`` code. Codes
are partitioned into decimal ranges so that common and role-specific codes
remain distinct and can be extended independently:

- **Common codes** (``0``--``99``): Valid in both client (``11000``) and server
  (``11001``) error packets, except ``OK`` (``0``), which is never sent on the
  wire.
- **Server-only codes** (``100``--``199``): Only valid in server error packets
  (``11001``).
- **Reserved** (``200``--``255``): Reserved for future use (e.g., client-only
  error codes).

..
   # LINT.IfChange(rpc2_error_codes)

.. list-table:: Common error codes (0--99)
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
     - The call was deliberately cancelled by application code.
   * - ``4``
     - ``RECEIVED_PACKET_FOR_WRONG_ENDPOINT``
     - The endpoint received a packet type that may only be sent by its own
       role (a server received a server-to-client packet, or a client received
       a client-to-server packet).
   * - ``5``
     - ``METHOD_TYPE_MISMATCH``
     - A packet does not match the method's type. Unary and server streaming
       calls must be started with ``START | HAS_PAYLOAD | STREAM_END``
       (``01110``); unary and client streaming calls must be answered with
       ``SERVER | HAS_PAYLOAD | OK_TERMINAL`` (``10011``).

.. list-table:: Server-only error codes (100--199)
   :header-rows: 1
   :widths: 10 40 50

   * - Value
     - Name
     - Description
   * - ``100``
     - ``DROPPED_WITHOUT_RESPONSE``
     - The server released a unary call without sending a response or
       cancelling it.
   * - ``101``
     - ``SERVICE_UNREGISTERED``
     - The target service was unregistered from the server while the call was
       running.
   * - ``102``
     - ``UNKNOWN_SERVICE``
     - The requested service is not registered on the server.
   * - ``103``
     - ``UNKNOWN_METHOD``
     - The requested method is not registered on the target service.
   * - ``104``
     - ``INVALID_REQUEST_PAYLOAD``
     - The request payload was invalid.
   * - ``105``
     - ``FAILED_TO_ALLOCATE_CALL``
     - Failed to allocate call state for an incoming request.
   * - ``106``
     - ``FAILED_TO_ALLOCATE_CALL_RESOURCES_WHILE_RUNNING``
     - Failed to allocate necessary resources while running the call.

..
   # LINT.ThenChange(//pw_rpc2/cpp/public/pw_rpc2/internal/protocol_status.h:cpp_rpc2_error_codes)
