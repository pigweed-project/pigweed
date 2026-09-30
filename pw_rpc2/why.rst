.. _module-pw_rpc2-why:

=========
Why RPC2?
=========
.. pigweed-module-subpage::
   :name: pw_rpc2

This doc explains the history behind ``pw_rpc``, how we got to where we are, and
what motivated us to write a ``pw_rpc2``. For a straightforward comparison
between the two versions, see :ref:`module-pw_rpc2-comparison`.

----------
Background
----------
Pigweed RPC1 (``pw_rpc``) was first introduced in 2020. At that time, many of
Pigweed's core primitives did not yet exist --- including formal operating
system abstractions (:ref:`module-pw_sync`), dynamic memory allocators
(:ref:`module-pw_allocator`), and cooperative asynchronous execution
(:ref:`module-pw_async2`).

RPC1 was designed to support all platforms down to bare-metal loops without OS
dependencies, and made specific architectural decisions: static compile-time
channel allocation, blocking synchronous dispatch loops, and packet handling
decoupled from underlying transports.

As product usage grew, these constraints presented challenges:

* A single **global mutex** protected all channels, calls, and encoding buffers.
* **No transport backpressure** led to dropped packets and workarounds such as
  converting streaming RPCs to repeated unary calls.
* An **asymmetric, unbuffered transport abstraction**
  (``pw::rpc::ChannelOutput``) could not handle transient transmission pauses
  or retries.
* **Stateless packet envelopes** sent messages regardless of whether anyone
  was listening.
* **Protobuf-encoded packets** with variable-length metadata and
  varint-delimited fields required payloads to be copied into their final
  location after they were fully constructed.

``pw_rpc2`` is an architectural redesign built on top of modern Pigweed
infrastructure to solve these challenges.

-----------------------
Architectural deep dive
-----------------------

From global locks to cooperative tasks
======================================
RPC1 predated Pigweed's synchronization primitives. When written, it assumed
that users could simply handle their own locking. This naturally failed as soon
as it touched a real production system.

Concurrency was then retrofitted onto the framework using a single global
mutex (``rpc_lock()``), with a years-long tail of races and bug fixes. Every
packet send, incoming packet decode, channel lookup, and call state transition
competed for the same lock. RPC internals became heavily covered by lock
annotations scattered throughout many unrelated functions, describing a contract
about who is allowed to hold the lock and when. User callbacks and call
destruction were particularly fiddly, requiring hard-to-reason-about code.

In ``pw_rpc2``, execution is driven instead by cooperative async tasks running
on a :ref:`module-pw_async2` ``Dispatcher``. All tasks --- RPC internal and user
--- on a single client or server ("endpoint") share the same thread, eliminating
most locking except around the edges.

* An endpoint runs internal ``ConnectionTask`` drivers for each connection that
  poll for incoming packets and dispatch to active RPCs.
* User code executes inside asynchronous tasks, constructed per-call, that
  fully own their state.
* Tasks that are blocked on resources yield back to the dispatcher, allowing it
  to continue running others without users having to provision dedicated
  threads.

Transport abstractions: state and backpressure
==============================================
RPC1 has separate, distinct APIs for sending and receiving data. Neither API is
sufficient to express the realities of an actual transport.

RPC1 sends outgoing packets through a ``ChannelOutput``, which provides a
virtual ``Send(ConstByteSpan)`` function to which RPC passes encoded packets
while holding its global lock. Channels expose no other state beyond this.
As long as the RPC server has a channel registered, it assumes that it is
available.

Notably, any non-OK status returned by ``Send`` is treated by RPC as a terminal
failure. Given that channels are typically statically registered, this has
resulted in some products silently dropping packets and returning an OK when a
link is temporarily unavailable, just so that RPC can continue using it.

For incoming packets, there is no structured read API, nor do reads reference a
channel at all. The RPC endpoint simply exposes a
``ProcessPacket(ConstByteSpan)``, into which users are expected to pass a
complete RPC packet, regardless of its origin. Processing of the packet is
handled synchronously on the calling thread.

All RPC1 packets are unreliable by design. However, there is no way for users to
know whether or not a packet has been dropped as the protocol contains no
concept of something like a sequence ID.

These limitations have resulted in some products using only unary RPCs --- even
for streamed data --- just so that they can recognize that packets were lost,
or express a form of flow control to delay responses. And not just downstream:
``pw_transfer`` implements a transport-layer-like reliable delivery protocol on
top of RPC. When you're shipping official layering violations, you know
something isn't quite right.

Stateful, reliable transports
-----------------------------
The most fundamental change being made alongside RPC2 is the addition of a
formal transport layer abstraction, via a new :ref:`module-pw_transport` module.
The scope of this extends beyond just RPC, but RPC will be its first and primary
consumer.

The core model behind ``pw_transport`` is that connections (``Socket`` objects)
are active. Unlike RPC1's channels, which were opaque data sinks, the new
transport connections have explicit lifecycles, and guarantee that someone is
listening while they are open. Connections are established by a client using a
``Connector``, while a server has a corresponding ``Listener`` that it uses to
accept incoming ones.

The ``Socket`` itself is an abstract interface to a transport, defining
asynchronous read/write operations that have certain guarantees. Specifically,
the interface used by RPC2 is reliable and datagram-oriented (think
``SOCK_SEQPACKET``). This interface can be implemented on top of various
underlying transports, such as BLE or TCP/IP (with simple framing).

This does make the tradeoff of a higher initial setup cost, since not every
device will already have a transport that can easily adapt to these
requirements, especially when you're just bootstrapping with a serial UART.
However, based on our experience working with real products over the years, we
strongly believe that most of them actually do want a reliable transport,
despite its costs, and the solutions that we have seen retrofitted (including
our very own ``pw_transfer``) make much worse tradeoffs.

Following our new RPC system and transport interfaces, Pigweed intends to build
its own simple, reliable transport protocol, which projects will be able to use
as an off-the-shelf solution.

Backpressure
------------
By moving onto ``pw_async2``, backpressure falls out mostly for free. The new
``pw_transport`` socket API provides two async functions: ``Read``, which waits
for a packet to arrive, and ``ReserveWrite``, which requests buffer space to
send an outgoing packet.

The use of a ``ReserveWrite`` API means that any call wishing to send data waits
until the capacity to send is available, however the specific transport chooses
to express that. Whether it's space in the local TX queue, or network-level
credits, the application code never has to concern itself with flow control,
nor does it have to produce or serialize data until it knows that the space is
available. Once it receives a reservation, it can write directly into the
transport buffer.

On ingress, an RPC2 endpoint's connection task only calls ``Read`` once its
local call has space to receive, managed via a small in-process queue. By not
reading, the endpoint naturally signals backpressure to the implementation of
the connection, causing its peer to pause sending until ready.

Improving the protocol
======================
Both versions of ``pw_rpc`` use protobuf for request/response payloads,
following gRPC, allowing a ``.proto`` file to define a language-agnostic
interface. However, RPC1 went further and used protobuf as the envelope for
its packets as well, mostly because it was there. This has led to several
issues.

1. Protobuf uses varints extensively, both for regular values and to store the
   size of length-delimited fields. This is a problem, as the size of a packet's
   metadata cannot be known until the payload size is known. In RPC1, this
   necessitated reserving the worst-case space up front, followed by a
   ``memmove`` of the user's payload into its final location after its size was
   known.

2. While the runtime costs of encoding and decoding protos are small, they are
   not negligible, especially in systems that send many small messages. Some RPC
   setups inspect raw packets before passing them into RPC itself, such as for
   routing or QoS decisions, so this cost reaches beyond RPC core.

The use of protobuf is undoubtedly the largest mistake in RPC1's protocol, but
it isn't the only one. Even ignoring the proto overhead, RPC1 packets are
inefficiently encoded. For historical reasons, every single RPC1 message sends a
method and service ID (proto ``fixed32``, for a total of 10 bytes per message),
despite them only being used by the initial request.

RPC1 also requires each RPC to send a status in its final response. This "user
status" is different from an RPC-layer status, and only made sense in a handful
of methods. Having two separate statuses to handle made client APIs awkward, as
it was easy to either conflate them, or miss handling one or the other.

Writing RPC2 gave us a clean slate to revisit these decisions.

Packet framing
--------------
RPC2 does away with protobuf packets, replacing them with a fixed-size header
dependent on the packet type. A common prefix stores the call ID and type,
beyond which additional fields are added only where required. The size of these
headers ends up minimal in practice, with the largest (the initial request)
coming in at 13 bytes, and the others under 8 bytes.

Zero-copy payloads
------------------
Since packet headers are fixed-size, the offset of the payload is always known.
Alongside the new ``pw_transport`` and :ref:`module-pw_buf` modules, this allows
us to support full zero-copy throughout the stack. A user sending a message
requests a buffer, which comes directly from the transport layer; space is
reserved for intermediate transport and RPC headers and footers; and the
resulting view of the payload region is handed to the user to fill, writing
their message directly into its final location.

Handshakes
----------
One reason that packets are allowed to be so small is that RPC2 implements an
initial handshake phase between a client and server when a connection is first
established. This phase first confirms that both peers are indeed RPC endpoints,
then establishes version compatibility. While nothing else is done in the
initial implementation of the handshake, its design enables us to add future
negotiations of RPC connection parameters.

-----------------
Why not fix RPC1?
-----------------
RPC1 is battle-tested. Tons of effort has been invested in strengthening it. The
APIs are well-understood. Once past the initial setup, the code is fairly simple
to write. Millions of devices run it and have for years. So why not just fix it?

Because those millions of devices are in the field, and they need to continue to
be able to communicate. Which is an issue when the problems most in need of
fixing are those that compatibility would preclude: the protocol, the channel
model, the lack of backpressure. And despite all the patches that have landed in
RPC1 over the years, they are still just workarounds. The core model, with all
of its flaws, has not moved much from when it was first written.

We did originally consider an "RPC1.5": a set of sweeping changes to modernize
RPC1 onto all of Pigweed's new interfaces. Opt-in, pick and choose what you
need. We quickly realized it would amount to maintaining two parallel systems,
without any of the benefits of designing a second parallel system.

RPC1 isn't going anywhere for a long time. It just isn't suited to the realities
of newer products.
