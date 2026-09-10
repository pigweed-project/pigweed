.. _module-pw_rpc-setup:

====================
Integration & setup
====================
.. pigweed-module-subpage::
   :name: pw_rpc

This guide is designed for **project owners, platform engineers, and system architects**
who are setting up the ``pw_rpc`` infrastructure for their project---especially if you
are integrating ``pw_rpc`` into an existing codebase, a custom RTOS, or a bare-metal
environment.

While application engineers define ``.proto`` services and implement business
logic, the platform owner is responsible for building the underlying communication
plumbing:

#. Integrating core Pigweed prerequisites (asserts, logging, synchronization).
#. Choosing physical transports and mapping them to **Channels**.
#. Implementing :cpp:class:`pw::rpc::ChannelOutput` to handle packet transmission (TX).
#. Creating the ingress (RX) task or polling loop to unframe packets and pass them
   to :cpp:func:`pw::rpc::Server::ProcessPacket`.
#. Registering services with the RPC Server.
#. (Optional) Configuring MCU-to-MCU C++ Clients or dual-role :cpp:class:`pw::rpc::ClientServer` endpoints.
#. (Optional) Setting up Host tooling (Python, TypeScript, Java/Android).

-------------------
System architecture
-------------------
The system architecture separates the concerns between **Pigweed-provided core libraries**
(protocol handling, packet encoding/decoding, service dispatch) and the **platform components
you implement** (transport drivers, framing, task contexts, and business logic).

1. Overall system architecture
==============================
This high-level overview shows the boundary between customer transport plumbing,
the ``pw_rpc`` engine, and application services:

.. mermaid::

   flowchart TB
       classDef pw fill:#e8f0fe,stroke:#1a73e8,stroke-width:2px,color:#174ea6;
       classDef user fill:#fef7e0,stroke:#f9ab00,stroke-width:2px,color:#b06000;
       classDef medium fill:#f1f3f4,stroke:#5f6368,stroke-width:1px,stroke-dasharray: 4 4,color:#3c4043;

       subgraph Host["Host / Client Peer"]
           PeerClient["Client / Host Tooling (PIGWEED + YOUR SCRIPTS)"]:::pw
       end

       subgraph Transport["Physical Transport"]
           Medium["Hardware Bus / Medium (UART, SPI, USB, BLE, Sockets)"]:::medium
       end

       subgraph Target["Target Device (Embedded MCU)"]
           direction TB

           subgraph IngressPlumbing["1. Transport Ingress Pipeline (YOU IMPLEMENT)"]
               Ingress["• Hardware Driver (DMA / ISR / Serial)<br/>• Dispatch Context (RTOS Task or Main Loop)<br/>• Framing / Packetizer (e.g. pw_hdlc)"]:::user
           end

           subgraph RpcCore["2. pw_rpc Core Engine (PIGWEED)"]
               RpcServer["• Packet Validation & Channel Routing<br/>• Protobuf Request Deserialization & Response Encoding<br/>• Service Dispatch"]:::pw
           end

           subgraph ServicePlumbing["3. Application Services (YOU IMPLEMENT)"]
               AppServices["• Method Handlers (Unary & Streaming)<br/>• Device Business Logic"]:::user
           end

           subgraph EgressPlumbing["4. Transport Egress Pipeline (YOU IMPLEMENT)"]
               Egress["• ChannelOutput::Send()<br/>• Transport Framing & Transmit Driver"]:::user
           end
       end

       PeerClient <-->|Transmits / Receives| Medium
       Medium -->|Raw Inbound Bytes| Ingress
       Ingress -->|Complete Packet Buffer| RpcServer
       RpcServer -->|Dispatches Request| AppServices
       AppServices -->|Response / Stream Data| RpcServer
       RpcServer -->|Encoded Packet Buffer| Egress
       Egress -->|Framed Outbound Bytes| Medium

In an end-to-end ``pw_rpc`` system, an external peer (such as a host script or
companion MCU) transmits requests across a physical medium like UART or SPI. On
the target MCU, your transport ingress pipeline collects raw bytes, un-frames
them into discrete RPC packet buffers, and delivers them to the ``pw_rpc`` core
server. The core server verifies packet headers, decodes the protobuf payload,
and invokes the matching method handler in your registered application services.
When the service produces responses or streaming data, it sends them through the
core encoder to your :cpp:class:`pw::rpc::ChannelOutput` implementation, which
frames and transmits the bytes back across the physical medium to the peer.

2. Ingress (RX) packet flow & service dispatch
==============================================
This diagram details how incoming transport data is packaged into a complete packet
buffer, passed to :cpp:func:`pw::rpc::Server::ProcessPacket`, and dispatched to the
targeted service method:

.. mermaid::

   flowchart TB
       classDef pw fill:#e8f0fe,stroke:#1a73e8,stroke-width:2px,color:#174ea6;
       classDef user fill:#fef7e0,stroke:#f9ab00,stroke-width:2px,color:#b06000;
       classDef medium fill:#f1f3f4,stroke:#5f6368,stroke-width:1px,stroke-dasharray: 4 4,color:#3c4043;

       subgraph HW_Layer["1. Transport Ingress (YOU IMPLEMENT)"]
           PhysRX["Physical Inbound Data"]:::medium
           Driver["Transport Driver (DMA / Serial / Bus)"]:::user
           Dispatch["Dispatch Context (RTOS Task or Main Loop)"]:::user
           Framing["Packetizer / Framing (e.g. pw_hdlc or native packets)"]:::user
           PhysRX --> Driver --> Dispatch --> Framing
       end

       subgraph Core_Dispatch["2. pw_rpc Ingress Engine (PIGWEED CORE)"]
           ProcessPacket["Server::ProcessPacket(packet_buffer)"]:::pw
           Lookup["Channel & Service / Method Lookup"]:::pw
           ProtobufDecode["Protobuf Request Deserialization"]:::pw
           ProcessPacket --> Lookup --> ProtobufDecode
       end

       subgraph Service_Targets["3. Registered Services (YOU IMPLEMENT)"]
           Svc1["EchoService::Echo(request, response)"]:::user
           Svc2["SensorService::GetReading(request, writer)"]:::user
           SvcN["DeviceService::Reboot(request, response)"]:::user
       end

       Framing -->|Complete Packet Buffer<br/><i>span&lt;const std::byte&gt;</i>| ProcessPacket
       ProtobufDecode -->|Invokes Method| Svc1
       ProtobufDecode -->|Invokes Method| Svc2
       ProtobufDecode -->|Invokes Method| SvcN

During ingress, physical hardware transfers raw serial or packet bytes into your
device driver. A dispatch context (such as an RTOS worker task or main event loop)
feeds these bytes to a framer (e.g., HDLC) that reconstructs the boundary of each
discrete RPC packet. Once a complete buffer is formed, your dispatch loop passes
it as a ``span<const std::byte>`` to :cpp:func:`pw::rpc::Server::ProcessPacket`.
The ``pw_rpc`` engine parses the packet header, verifies the channel ID, locates
the targeted service and method, deserializes the protobuf request fields, and
invokes your service method handler synchronously.

3. Egress (TX) packet flow & channel routing
============================================
This diagram shows how responses and streaming packets from multiple independent
services re-combine into the ``pw_rpc`` core encoder, resolve the target channel,
and transmit via your :cpp:class:`pw::rpc::ChannelOutput`:

.. mermaid::

   flowchart TB
       classDef pw fill:#e8f0fe,stroke:#1a73e8,stroke-width:2px,color:#174ea6;
       classDef user fill:#fef7e0,stroke:#f9ab00,stroke-width:2px,color:#b06000;
       classDef medium fill:#f1f3f4,stroke:#5f6368,stroke-width:1px,stroke-dasharray: 4 4,color:#3c4043;

       subgraph Service_Origins["1. Service Execution & Client Calls (YOU IMPLEMENT)"]
           Svc1["EchoService Handler<br/>(Unary Response)"]:::user
           Svc2["SensorService Handler<br/>(ServerWriter Stream)"]:::user
           ClientCall["MCU Client Call<br/>(ClientWriter / Request)"]:::user
       end

       subgraph Core_Encoding["2. pw_rpc Core Encoding & Routing (PIGWEED CORE)"]
           ProtobufEncode["Protobuf Response / Payload Serialization"]:::pw
           PacketAssemble["RPC Packet Assembly<br/>(Envelope Header + Payload)"]:::pw
           ChannelResolve["Channel Lookup by Channel ID"]:::pw
           ProtobufEncode --> PacketAssemble --> ChannelResolve
       end

       subgraph Transport_Egress["3. Channel & Transport Egress (YOU IMPLEMENT)"]
           ChannelOut["pw::rpc::ChannelOutput::Send(packet_buffer)"]:::user
           TxFraming["Framing / Driver / DMA TX"]:::user
           PhysTX["Physical Outbound Medium"]:::medium
           ChannelOut --> TxFraming --> PhysTX
       end

       Svc1 -->|Returns response| ProtobufEncode
       Svc2 -->|writer.Write| ProtobufEncode
       ClientCall -->|client.Invoke| ProtobufEncode
       ChannelResolve -->|Calls Output| ChannelOut

During egress, service method handlers and client callers initiate transmissions
by returning unary responses, calling ``writer.Write()`` on streaming writers, or
invoking new client requests. The ``pw_rpc`` core library serializes the message
into protobuf wire format, wraps it with RPC envelope metadata (including channel
ID, service ID, method ID, and sequence numbers), and looks up the active channel.
The core then passes the assembled packet buffer directly to your
:cpp:class:`pw::rpc::ChannelOutput` subclass via
:cpp:func:`pw::rpc::ChannelOutput::Send`, where your driver optionally frames the
packet and transmits it across the physical hardware.

------------------
Setup at a glance
------------------
Setting up ``pw_rpc`` involves 7 core steps:

.. list-table::
   :header-rows: 1
   :widths: 15 25 60

   * - Step
     - Topic
     - What You Provide / Configure
   * - :ref:`Step 1 <module-pw_rpc-setup-prereqs>`
     - **Pigweed Prerequisites**
     - Build integration, :ref:`module-pw_assert`, :ref:`module-pw_log`, :ref:`module-pw_sync` (or null backend for bare metal), Protobuf generator.
   * - :ref:`Step 2 <module-pw_rpc-setup-channels>`
     - **Transport & Channels**
     - Channel ID mapping, packet MTU sizing, transport framing selection.
   * - :ref:`Step 3 <module-pw_rpc-setup-egress>`
     - **Egress (TX) Path**
     - Subclass :cpp:class:`pw::rpc::ChannelOutput`, implement :cpp:func:`pw::rpc::ChannelOutput::Send`, transport mutex.
   * - :ref:`Step 4 <module-pw_rpc-setup-ingress>`
     - **Ingress (RX) Path**
     - Instantiation of :cpp:class:`pw::rpc::Server`, RX dispatch loop (RTOS task or main loop) calling :cpp:func:`pw::rpc::Server::ProcessPacket`.
   * - :ref:`Step 5 <module-pw_rpc-setup-services>`
     - **Service Registration**
     - Implement ``.proto`` services and register them via :cpp:func:`pw::rpc::Server::RegisterService`.
   * - :ref:`Step 6 <module-pw_rpc-setup-clients>`
     - **C++ Clients (Optional)**
     - :cpp:class:`pw::rpc::Client` or :cpp:class:`pw::rpc::ClientServer` for inter-MCU RPCs.
   * - :ref:`Step 7 <module-pw_rpc-setup-host>`
     - **Host Tooling (Optional)**
     - Python (:ref:`module-pw_rpc-py`), TypeScript (:ref:`module-pw_rpc-ts`), or Java/Android integrations.

.. tip::
   A detailed :ref:`Deployment Checklist <module-pw_rpc-setup-checklist>` is provided
   at the end of this guide to track implementation tasks.

.. _module-pw_rpc-setup-prereqs:

------------------------------------------------
Step 1: Integrate Pigweed basics (Prerequisites)
------------------------------------------------
``pw_rpc`` is designed to be lightweight and modular, but relies on a few fundamental
Pigweed building blocks.

Required modules
================
.. list-table::
   :header-rows: 1
   :widths: 25 30 45

   * - Module
     - Purpose in ``pw_rpc``
     - Notes
   * - :ref:`module-pw_status` / :ref:`module-pw_result`
     - Standardized error handling
     - Core status codes (``OkStatus()``, ``UNAVAILABLE``, ``DATA_LOSS``, etc.).
   * - :ref:`module-pw_assert`
     - Invariant checking
     - Configure a backend for your platform (e.g. ``pw_assert_basic`` or custom).
   * - :ref:`module-pw_log`
     - Diagnostic logging
     - Used by ``pw_rpc`` to log packet decode errors and state warnings.
   * - :ref:`module-pw_sync`
     - Thread synchronization
     - Mutex, BinarySemaphore, TimedThreadNotification backends.
   * - :ref:`module-pw_span` / :ref:`module-pw_bytes`
     - Memory views
     - Zero-copy ``span<const std::byte>`` buffer handling.
   * - :ref:`module-pw_containers`
     - Intrusive lists
     - Internal tracking of registered services and channels.

.. note::
   **Bare-Metal & Single-Threaded Deployments:**
   ``pw_rpc`` works seamlessly on bare-metal (superloop / single-threaded) targets.
   Because ``pw_rpc`` internally uses synchronization primitives for thread safety,
   you only need to configure a **null mutex backend** (e.g. ``pw_sync_baremetal``
   or a platform null backend). With a null backend, locking operations compile
   down to zero-overhead no-ops while keeping the API compatible.

Protobuf backend selection
==========================
Choose the protobuf code generator suited for your project:

* **Nanopb** (``.rpc.pb.h``) [Recommended for most projects] -- Generates
  lightweight C structs. Fully supports unary and streaming RPCs with minimal RAM overhead.
* **pw_protobuf** (``.rpc.pwpb.h``) -- Pure C++ type-safe generator. Useful when
  avoiding C struct code generators.
* **Raw RPC** (``.raw_rpc.pb.h``) -- Provides direct access to raw bytes
  (``pw::ConstByteSpan``). Best for zero-copy streaming, custom deserializers,
  or performance benchmarking.

.. _module-pw_rpc-setup-channels:

----------------------------------------------
Step 2: Transport layer & channel architecture
----------------------------------------------
.. note::
   This section focuses on transport selection and system-level channel design for
   project integrators. For the complete C++ API reference, dynamic channel allocation,
   and channel ID remapping, see :ref:`module-pw_rpc-cpp`.

What is a Channel?
==================
In ``pw_rpc``, a **Channel** (:cpp:class:`pw::rpc::Channel`) represents a logical
communication pathway between a client and server. Each channel binds:

#. A unique integer **Channel ID** (``uint32_t``).
#. A :cpp:class:`pw::rpc::ChannelOutput` interface responsible for transmitting encoded RPC packets.

Key design principles of channels
=================================

1. Stateless and "Implied Open"
-------------------------------
``pw_rpc`` channels do **not** perform handshake negotiations, connection setup
packets, SYN/ACK handshakes, or keep-alive pings.

* A channel is considered "open" simply by existing in the channel list with a valid
  Channel ID and :cpp:class:`pw::rpc::ChannelOutput`.
* **Memory Benefit:** Because ``pw_rpc`` maintains no per-channel connection state
  machines, its RAM footprint is exceptionally small (only a few bytes per channel).

2. Channel limitations & transport responsibilities
---------------------------------------------------
Because ``pw_rpc`` channels are intentionally minimal, your underlying transport
layer must handle certain responsibilities:

* **No Built-in Framing:**
  ``pw_rpc`` packets are discrete byte buffers. If your physical transport is
  stream-oriented (like UART, SPI, or TCP), your transport layer **must**
  provide framing (e.g., HDLC via :ref:`module-pw_hdlc`, SLIP, or
  length-prefixed headers) to delimit packet boundaries before passing them to
  :cpp:func:`pw::rpc::Server::ProcessPacket`.
* **No Built-in Backpressure / Flow Control:**
  ``pw_rpc`` does not throttle the sender if the receiver is busy. If an
  endpoint generates stream packets faster than the physical medium or receiver
  can consume them, packets may be dropped. Use hardware flow control (e.g.
  UART RTS/CTS) or transport-level flow control if necessary.
* **No Built-in Retries / Reliability:**
  ``pw_rpc`` does not retransmit lost packets. In lossy environments (e.g.
  noisy serial or wireless), implement reliability at the transport layer (e.g.
  ARQ / ACK-NACK protocols) or handle timeouts and retries at the application
  layer.

Sizing and MTU considerations
=============================
The maximum size of an RPC message is governed by two constraints:

#. :c:macro:`PW_RPC_ENCODING_BUFFER_SIZE_BYTES`: Compile-time configuration
   (default: 512 bytes) that defines the maximum encoded RPC packet size ``pw_rpc``
   can construct in memory.
#. :cpp:func:`pw::rpc::ChannelOutput::MaximumTransmissionUnit`: The maximum packet
   size the physical transport can transmit in one frame.

Use the helper :cpp:func:`pw::rpc::MaxSafePayloadSize` to determine the maximum
payload size your service can safely write without exceeding encode buffers.

.. _module-pw_rpc-setup-egress:

------------------------------------------------------
Step 3: Implement ChannelOutput and create channels
------------------------------------------------------
To send packets out of ``pw_rpc``, create a class derived from :cpp:class:`pw::rpc::ChannelOutput`.

Subclassing ChannelOutput
=========================
In the ``pw_rpc`` architecture, packet transmission via :cpp:func:`pw::rpc::ChannelOutput::Send`
cannot fail from the perspective of the RPC engine. If the underlying transport driver cannot
transmit the packet (for example, if a hardware queue is full or a peer is disconnected), the
packet should simply be dropped.

.. literalinclude:: examples/custom_channel_output.cc
   :language: cpp
   :start-after: [pw_rpc-examples-channel-output-subclass]
   :end-before: [pw_rpc-examples-channel-output-subclass]

Thread synchronization for shared transports
============================================
If multiple channels or threads share the same physical transport (for example,
if both RPC and a logging stream write to the same UART), wrap the output with
a mutex:

.. literalinclude:: examples/custom_channel_output.cc
   :language: cpp
   :start-after: [pw_rpc-examples-synchronized-channel-output]
   :end-before: [pw_rpc-examples-synchronized-channel-output]

Creating channel instances
==========================
Instantiate your channels with explicit, non-zero IDs:

.. literalinclude:: examples/custom_channel_output.cc
   :language: cpp
   :start-after: [pw_rpc-examples-channel-instantiation]
   :end-before: [pw_rpc-examples-channel-instantiation]

.. _module-pw_rpc-setup-ingress:

-----------------------------------------------
Step 4: Server setup & ingress (RX) pipeline
-----------------------------------------------
The RPC server processes incoming requests, dispatches them to registered services,
and encodes responses.

1. Instantiating the Server
===========================
.. literalinclude:: examples/custom_channel_output.cc
   :language: cpp
   :start-after: [pw_rpc-examples-server-instantiation]
   :end-before: [pw_rpc-examples-server-instantiation]

2. Building the RX Ingress Pipeline
===================================
Incoming raw bytes from the transport must be collected, unframed into discrete
RPC packet buffers, and passed to :cpp:func:`pw::rpc::Server::ProcessPacket`.

Choosing an Ingress Execution Context
-------------------------------------
:cpp:func:`pw::rpc::Server::ProcessPacket` decodes the packet and executes the
corresponding service method handler **synchronously on the calling thread/context**.
You have two architectural choices for where to run ``ProcessPacket()``:

* **Option A: Dedicated RTOS Thread / Task (Recommended for multi-threaded systems):**
  A dedicated thread (e.g. FreeRTOS task, Zephyr thread, or ``pw::thread::Thread``)
  blocks waiting on incoming bytes, unframes packets, and processes them. This isolates
  RPC processing from other application tasks. Ensure the task has adequate stack
  space (typically 2--4 KB, depending on message sizes).
* **Option B: Existing Main Loop / Superloop / Event Loop (Ideal for bare-metal & cooperative systems):**
  You do **not** need a separate thread. In bare-metal or event-driven systems,
  you can simply poll the transport non-blockingly and invoke
  ``server.ProcessPacket()`` directly from your main ``while (true)`` loop or event
  handler whenever a complete frame is available.

.. warning::
   **Never Invoke ProcessPacket() from an Interrupt Service Routine (ISR)!**
   Service method handlers execute synchronously within ``ProcessPacket()`` and
   may perform complex computations, lock mutexes, or write responses to
   ``ChannelOutput``. If your hardware uses interrupt-driven RX (e.g. UART RX
   interrupt or DMA transfer-complete interrupt), the ISR should only buffer raw
   bytes into a ring buffer or queue and wake up a task or notify the main loop.
   **Never** pass packets to ``ProcessPacket()`` inside an interrupt context.

Option A Example: Dedicated Ingress Thread (RTOS)
-------------------------------------------------
.. literalinclude:: examples/ingress_thread.cc
   :language: cpp
   :start-after: [pw_rpc-examples-ingress-thread]
   :end-before: [pw_rpc-examples-ingress-thread]

Option B Example: Main Loop / Bare-Metal Polling
------------------------------------------------
.. literalinclude:: examples/ingress_thread.cc
   :language: cpp
   :start-after: [pw_rpc-examples-ingress-polling]
   :end-before: [pw_rpc-examples-ingress-polling]

.. _module-pw_rpc-setup-services:

---------------------------------------------
Step 5: Registering services on the server
---------------------------------------------
Once the server and ingress path are running, instantiate your service classes and
register them with the server.

.. literalinclude:: examples/sensor_service.cc
   :language: cpp
   :start-after: [pw_rpc-examples-sensor-register]
   :end-before: [pw_rpc-examples-sensor-register]

.. tip::
   For a detailed guide on creating ``.proto`` files, build rules, and implementing
   unary/streaming methods, refer to :ref:`module-pw_rpc-services`.

.. _module-pw_rpc-setup-clients:

---------------------------------------------------
Step 6: Embedded C++ client setup (MCU-to-MCU)
---------------------------------------------------
When a microcontroller needs to make RPC calls to another MCU or to a host, it
acts as an **RPC Client**.

1. Creating the Client
======================
The :cpp:class:`pw::rpc::Client` is instantiated with its own list of channels
(or shares channels with a server):

.. literalinclude:: examples/client_example.cc
   :language: cpp
   :start-after: [pw_rpc-examples-client-instantiation]
   :end-before: [pw_rpc-examples-client-instantiation]

2. Dual-Role Nodes with pw::rpc::ClientServer
=============================================
If a single device acts as **both** a Server and a Client over the same channel/transport,
use :cpp:class:`pw::rpc::ClientServer`. It combines both endpoints and routes packets
automatically:

.. literalinclude:: examples/client_example.cc
   :language: cpp
   :start-after: [pw_rpc-examples-client-server]
   :end-before: [pw_rpc-examples-client-server]

3. Invoking RPCs from C++
=========================

Asynchronous Client Call (Non-blocking):
----------------------------------------
.. literalinclude:: examples/client_example.cc
   :language: cpp
   :start-after: [pw_rpc-examples-async-client-call]
   :end-before: [pw_rpc-examples-async-client-call]

Synchronous Client Call (Blocking wrapper):
-------------------------------------------
.. literalinclude:: examples/client_example.cc
   :language: cpp
   :start-after: [pw_rpc-examples-sync-client-call]
   :end-before: [pw_rpc-examples-sync-client-call]

.. _module-pw_rpc-setup-host:

---------------------------------------------
Step 7: Host tooling & Python client setup
---------------------------------------------
Python clients are commonly used for CLI debug tools, automated factory testing,
and integration test harnesses.

How Python pw_rpc Works
=======================
In Python:

#. ``pw_rpc.descriptors.Channel(channel_id, output_callable)`` binds a channel ID
   to a Python send function (``Callable[[bytes], Any]``).
#. When the transport receives bytes, pass them to ``client.process_packet(raw_packet)``.

Example: Custom Transport in Python
===================================
.. literalinclude:: examples/host_client.py
   :language: python
   :start-after: [pw_rpc-examples-python-transport]
   :end-before: [pw_rpc-examples-python-transport]

Other language clients
======================
* TypeScript (:ref:`module-pw_rpc-ts`): WebSerial, WebUSB, and WebSocket interfaces
  via ``pigweedjs/pw_rpc``.
* Java / Kotlin / Android (:ref:`module-pw_rpc-java`): Mobile and JVM tools via
  ``dev.pigweed.pw_rpc``.

--------------------------------
Configuration & memory tuning
--------------------------------
The following compile-time options allow you to tune memory consumption and performance:

.. list-table::
   :header-rows: 1
   :widths: 35 15 50

   * - Macro / Option
     - Default
     - Description & Tuning Advice
   * - :c:macro:`PW_RPC_ENCODING_BUFFER_SIZE_BYTES`
     - ``512``
     - Max size of an encoded RPC packet buffer. Increase if services send large payloads; decrease to save RAM.
   * - :c:macro:`PW_RPC_DYNAMIC_ALLOCATION`
     - ``0``
     - Set to ``0`` for bare-metal / embedded targets to guarantee zero heap allocations. When ``0``, channel spans and services use fixed arrays.
   * - :c:macro:`PW_RPC_LOCKLESS_CHANNEL_SEND`
     - ``0``
     - When ``1``, releases the RPC global lock before calling :cpp:func:`pw::rpc::ChannelOutput::Send`. Useful if ``Send()`` blocks on hardware FIFO.
   * - :c:macro:`PW_RPC_CALLBACK_TIMEOUT_TICKS`
     - Platform default
     - Watchdog timeout ticks to detect deadlocks in RPC user callbacks.

------------------------------------------------
Complete end-to-end C++ integration reference
------------------------------------------------
Below is a complete, standalone example assembling the entire pipeline in an embedded system:

.. literalinclude:: examples/full_integration.cc
   :language: cpp
   :start-after: [pw_rpc-examples-full-integration]
   :end-before: [pw_rpc-examples-full-integration]

.. _module-pw_rpc-setup-checklist:

-----------------------------------
Comprehensive deployment checklist
-----------------------------------
Use this checklist to ensure all architectural and implementation components are in place:

1. Prerequisites & Environment
==============================
* **Build system integration**: ``pw_rpc`` targets and dependencies wired into
  your build system (Bazel, GN, CMake, or native build).
* **Asserts and logs configured**: :ref:`module-pw_assert` and
  :ref:`module-pw_log` backends configured for the target platform.
* **Synchronization backend**: :ref:`module-pw_sync` backend configured (e.g.
  FreeRTOS or Zephyr backend for RTOS, or null mutex backend for bare metal).
* **Protobuf generator selected**: Configured Nanopb
  (``nanopb_rpc_proto_library``), ``pw_protobuf`` (``pwpb_rpc_proto_library``),
  or Raw RPC, and verified proto generation.

2. Transport & Framing
======================
* **Physical transports identified**: Mapped physical links (UART, SPI, USB,
  BLE, Sockets) to logical routes.
* **Framing protocol integrated**: Integrated a framing layer for
  stream-oriented transports (e.g., :ref:`module-pw_hdlc`).
* **Channel IDs assigned**: Assigned unique static integer IDs (``1..127``) for
  each endpoint.
* **Buffer sizing (MTU)**: Configured
  :c:macro:`PW_RPC_ENCODING_BUFFER_SIZE_BYTES` and verified framing decoder
  buffers match transport MTU.

3. Egress (TX) Pipeline
=======================
* **ChannelOutput subclass**: Implemented derived class providing
  :cpp:func:`pw::rpc::ChannelOutput::Send` and
  :cpp:func:`pw::rpc::ChannelOutput::MaximumTransmissionUnit`.
* **Buffer lifecycle verified**: Ensured ``Send()`` transmits synchronously or
  copies data before returning (buffer is not accessed asynchronously).
* **Deadlock prevention**: Verified ``Send()`` never calls ``pw_rpc`` APIs or
  invokes RPC methods directly.
* **Transport mutex**: Added mutex synchronization if physical transmitter is
  shared across channels/threads.
* **Channel instances created**: Instantiated
  ``pw::rpc::Channel::Create<kChannelId>(&my_output)``.

4. Ingress (RX) Pipeline
========================
* **Execution model chosen**: Configured either a dedicated RTOS dispatch
  thread or a main loop / superloop polling function.
* **Stack sizing**: If using an RTOS thread, allocated sufficient stack
  (typically 2--4 KB) for service method execution.
* **Framing & packet ingress**: Incoming bytes fed to decoder -> valid frames
  passed to :cpp:func:`pw::rpc::Server::ProcessPacket`.
* **No ISR invocation**: Verified ``ProcessPacket()`` is **never** called
  directly from an ISR context.

5. Services & Application
=========================
* **Server instantiation**: Instantiated ``pw::rpc::Server server(channels);``.
* **Service implementations**: Implemented service handlers inheriting from
  generated ``Service<Impl>`` base classes.
* **Service registration**: Registered all services during startup via
  ``server.RegisterService(...)``.

6. Embedded Clients (If MCU-to-MCU)
===================================
* **Client / ClientServer**: Instantiated :cpp:class:`pw::rpc::Client` (or
  :cpp:class:`pw::rpc::ClientServer` for dual-role nodes).
* **Response ingress**: Ingress pipeline routes response packets to
  ``client.ProcessPacket(packet)``.
* **Call object lifecycle**: Call objects retained in class members / state
  variables for active RPCs.

7. Host Tooling & Language Integration (If applicable)
======================================================
* **Python / CLI**: Configured Python ``pw_rpc.descriptors.Channel`` with
  serial/socket TX, RX background listener, and ``client.process_packet()``.
* **TypeScript / Java**: Configured web or Android client stubs if tooling
  requires them.

8. Verification & Stress Testing
================================
* **Basic round-trip**: Verified unary RPC request and response.
* **Streaming throughput**: Tested sustained server/client streaming under
  expected data rates.
* **Error recovery**: Verified system handles malformed packets, framing
  errors, and sudden disconnection gracefully.
