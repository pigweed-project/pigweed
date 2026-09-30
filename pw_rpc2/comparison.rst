.. _module-pw_rpc2-comparison:

=============
RPC1 vs. RPC2
=============
.. pigweed-module-subpage::
   :name: pw_rpc2

Pigweed offers two distinct RPC frameworks: the original RPC1 (``pw_rpc``) and a
modernized RPC2 (``pw_rpc2``). This document describes their differences and
helps you choose the one better suited to your system. For background context on
why a second version of RPC was written, see
:ref:`Why RPC2? <module-pw_rpc2-why>`.

While we recommend that most new projects move forward using RPC2, it is not a
universal replacement, nor do existing projects need to migrate. The two
versions of RPC were designed with different goals and constraints in mind, and
in some cases the original may be a better choice for you.

-------------------------------
Quick comparison: RPC1 vs. RPC2
-------------------------------
The following table lists the key design differences between the old and new
systems, some of which will be described in more detail in sections below.

.. list-table::
   :widths: 22 39 39
   :header-rows: 1
   :stub-columns: 1

   * -
     - RPC1 (``pw_rpc``)
     - RPC2 (``pw_rpc2``)

   * - :ref:`Binary size (flash) <module-pw_rpc2-comparison-code-size>`
     - **~7-10 KB baseline** for an embedded server with Nanopb.
     - **Larger (~25 KB)** combined client/server baseline due to async tasks,
       queues, and connection management.

   * - :ref:`Runtime <module-pw_rpc2-comparison-runtime>`
     - Runtime-agnostic. No coupled framework. Call from any RTOS thread or
       bare-metal polling loop.
     - Integrated with :ref:`module-pw_async2`, requiring a dispatcher and
       allocator (:ref:`module-pw_allocator`) to be set up. RPCs run on
       cooperative tasks. Supports C++20 coroutines.

   * - Concurrency and locking
     - A single global mutex protects all channels, calls, and shared encoding
       buffers. Can run into contention under multi-threaded loads.
     - Minimal locking, as all RPC tasks share a dispatcher thread.

   * - :ref:`Flow control and backpressure <module-pw_rpc2-comparison-transport>`
     - None. If a transport buffer is full, ``Send()`` fails or packets are
       silently dropped. No way for slow consumers to signal backpressure to
       producers.
     - Natively supported. Outbound writes asynchronously wait for available
       capacity via ``ReserveWrite``. Ingress pauses until packets are handled.
       Blocked tasks yield cooperatively.

   * - Transport model
     - Asymmetric. Outbound uses a ``ChannelOutput::Send(ConstByteSpan)``
       byte sink. Inbound packets are fed into ``ProcessPacket(ConstByteSpan)``.
       Globally unique, often static channel IDs. Stateless and unreliable.
     - Active, reliable, connection-oriented transports (via
       :ref:`module-pw_transport`). Connections disappear once the peer hangs
       up, with an opening handshake to ensure compatibility.

   * - Packet framing and wire format
     - Enclosed in a protobuf message (``pw_rpc/internal/packet.proto``) with
       varint fields. Requires worst-case buffer reservations and payload copies
       after the length is known. Redundant fields are sent on each message.
     - Small, fixed-size headers which are fast to decode, containing only the
       minimal necessary information, and allow payloads to exist at a known
       offset. Safe ownership and slicing via ``pw::ConstBuf``.

   * - Memory model
     - Static compile-time reservation for every possible call object. All RPC
       calls permanently consume RAM, regardless of how many are used at any
       given time.
     - Dynamic allocation via :ref:`module-pw_allocator`. Active call nodes
       and buffer slices are allocated on demand and freed on call completion.

   * - Status handling
     - Two distinct statuses: framework status and redundant "user status" in
       final responses. Confusing APIs for callers.
     - Framework-level errors only. Method-level domain errors belong in the
       application response message.

   * - :ref:`Production maturity <module-pw_rpc2-comparison-production>`
     - Proven in production across millions of devices since 2020.
     - Under active development.

   * - Language support
     - C++ (Nanopb, pwpb, raw), Python, TypeScript, Java.
     - Initially C++ (raw and pwpb APIs, manual polling or C++20 coroutines),
       with Rust (``no_std``, Embassy) and others planned.

---------------------------------
Important points of consideration
---------------------------------

.. _module-pw_rpc2-comparison-code-size:

Code size and resource budgets
==============================
Code size is often the deciding factor for embedded microcontrollers, and this
constraint may be the strongest reason to stick with RPC1.

* **RPC1** was designed with low resource usage in mind. A baseline RPC1 server
  with Nanopb compiles to **~7-10 KB of flash** on Cortex-M cores. If your
  system is tightly constrained, RPC1 has a much smaller footprint.
* **RPC2** incorporates the machinery of an asynchronous runtime and complete
  transport layer, including complex async state machines, inter-task channels,
  reliable delivery, a more complete protocol, etc. The baseline footprint of a
  combined client and server is **~25 KB of flash**.

While RPC2 has a larger code size, the two are much more competitive on RAM
usage. Since RPC2 uses memory pools through ``pw_allocator``, it reuses the same
blocks of memory for different calls and connections over time, while RPC1
requires you to statically allocate memory for every possible call.
Additionally, RPC2's transport layer is zero-copy by design, while RPC1 requires
some intermediate buffering.

.. _module-pw_rpc2-comparison-runtime:

Runtime and concurrency
=======================
RPC2's execution model is its most fundamental architectural divergence, and a
primary reason why it was written.

* **RPC1 is runtime-agnostic.** It does not prescribe how your firmware
  schedules execution. You pump incoming packets into ``ProcessPacket()`` from
  an RTOS thread or a simple global superloop. Calls run synchronously from
  where ``ProcessPacket()`` is invoked, and a long-running handler will block
  the thread. Method implementations which want to continuously send values over
  time have to manage that themselves; for example, by handing the RPC writer
  to a separate thread. Managing thread safety across multiple threads relies on
  RPC1's internal global mutex, which can become a point of lock contention.
* **RPC2 is async2-native.** RPC2 runs via cooperative tasks on a configured
  :ref:`module-pw_async2` dispatcher. Many concurrent RPC calls can run on the
  same thread without blocking one another. Because everything runs on one
  dispatcher thread, locking and contention are minimal.

If your project is already built around ``pw_async2`` or wants the benefits of
cooperative multitasking, RPC2 is a natural fit. Conversely, if your project is
built on blocking threads, adopting async2 can be a large shift, as async code
tends to spread throughout its callers. At minimum, using RPC2 requires you to
pay the cost of a dispatcher and some sync-to-async adapters.

If you're interested in learning more about how ``pw_async2`` compares to
traditional concurrency models, check out :ref:`module-pw_async2-why`.

.. _module-pw_rpc2-comparison-transport:

Transport flow control and backpressure
=======================================
If your system streams high volumes of data over slow, lossy, or
variable-bandwidth links (e.g. UART, SPI, BLE), the two versions of RPC have
distinct differences.

* In **RPC1**, RPCs do not have flow control or any guarantee of delivery. Each
  message is sent out through a ``ChannelOutput``, and whatever happens beyond
  that is not RPC's concern. If your system produces RPC messages like logs or
  telemetry at a faster rate than the transport can handle, the transport may
  just silently drop them. Teams have often worked around this by turning stream
  RPCs into repeated unary calls to manage flow and know if they were delivered.
* In **RPC2**, backpressure is intrinsic to the transport. ``pw_transport``
  writes require a buffer reservation from the transport, so if the transport is
  unavailable, the task wanting to write just suspends while others run. Inbound
  streams are buffered, and the RPC task stops reading data when its calls are
  saturated, signaling the peer to slow down.

In our experience, a lot of projects don't think that they want a reliable
transport until they suddenly do. The cost of trying to retrofit backpressure
and reliability onto a system that wasn't built for it is often worse than just
spending a little more effort up front.

.. _module-pw_rpc2-comparison-production:

Production readiness and ecosystem
==================================
* **RPC1** has had years of real-world deployments and hardening, running on
  millions of production devices. It has mature host tooling, including an
  interactive Python REPL (:ref:`module-pw_console`), WebSerial/WebUSB clients
  in TypeScript, and Android/JVM clients in Java.
* **RPC2** is new and under active development. The initial C++ implementation
  has yet to ship in a product, while other languages are in various stages of
  planning.

If you require stability, or need to communicate with devices written in other
languages **today**, pick RPC1.

-------------------------
Coexistence and migration
-------------------------
Good news! You will not have to choose between the two exclusively. Pigweed
plans to offer an adapter for existing RPC1 systems, allowing their services to
run on an RPC2 server.

If you are already using RPC1, this will allow you to incrementally migrate your
code over at your own pace, while your existing service implementations continue
to work just as before.
