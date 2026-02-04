.. _module-pw_channel-design:

======
Design
======
.. pigweed-module-subpage::
   :name: pw_channel

.. _module-pw_channel-design-why:

--------------
Why pw_channel
--------------

Flow control
============
Flow control ensures that:

- Channels do not send more data than the receiver is prepared to handle.
- Channels do not request more data than they are prepared to handle.

If one node sends more data than the other node can receive, network performance
degrades. The effects vary, but could include data loss, throughput drops, or
even crashes in one or both nodes. The ``Channel`` API avoids these issues by
providing backpressure.

What is backpressure?
---------------------
In networking, backpressure is when a node, overwhelmed by inbound traffic,
exterts pressure on upstream nodes. Communications APIs have to provide ways to
release the pressure, allowing higher layers to can reduce data rates or drop
stale or unnecessary packets.

Pitfalls of simplistic backpressure APIs
----------------------------------------
Expressing backpressure in an API might seem simple. You could just return a
status code that indicates that the link isn't ready, and retry when it is.

..  code-block:: cpp

   // Returns `UNAVAILABLE` if the link isn't ready for data yet; retry later.
   Status Write(std::span<const std::byte> packet);

In practice, this is very difficult to work with:

..  code-block:: cpp

    std::span packet = ExpensiveOperationToPreprarePacket();
    if (Write(packet).IsUnavailable()) {
      // ... then what?
    }

Now what do you do? You did work to prepare the packet, but you can't send it.
Do you store the packet somewhere and retry? Or, wait a bit and recreate the
packet, then try to send again? How long do you wait before sending?

The result is inefficient code that is difficult to write correctly.

There are other options: you can add an ``IsReadyToWrite()`` function, and only
call ``Write`` when that is true. But what if ``IsReadyToWrite()`` becomes false
while you're preparing the packet? Then, you're back in the same situation.
Another approach: block until the link is ready for a write. But this means an
entire thread and its resources are locked up for an arbitrary amount of time.

How pw_channel addresses write-side backpressure
------------------------------------------------
When writing into a ``Channel`` instance, the ``Channel`` may provide
backpressure in several locations:

- :cc:`PendReadyToWrite <pw::channel::AnyChannel::PendReadyToWrite>` --
  Before writing to a channel, users must check that it is ready to receive
  writes. If the channel is not ready, the channel will wake up the async task
  when it becomes ready to accept outbound data.
- ``GetWriteAllocator`` --
  Once the channel becomes ready to receive writes, the writer must ensure that
  there is space in an outgoing write buffer for the message they wish to send.
  If there is not yet enough space, the channel will wake up the async task
  once there is space again in the future.

Only once these two operations have completed can the writing task may place its
data into the outgoing buffer and send it into the channel.

How pw_channel addresses read-side backpressure
-----------------------------------------------
When reading from a ``Channel`` instance, the consumer of the ``Channel`` data
exerts backpressure by *not* invoking :cc:`PendRead
<pw::channel::AnyChannel::PendRead>`. The buffers returned by ``PendRead`` are
allocated by the ``Channel`` itself.

Zero-copy
=========
It's common to see async IO APIs like this:

..  code-block:: cpp

   Status Read(pw::Function<void(pw::Result<std::span<const std::byte>)> callback);

These APIs suffer from an obvious problem: what is the lifetime of the span
passed into the callback? Usually, it only lasts for the duration of the
callback. Users must therefore copy the data into a separate buffer if
they need it to persist.

Another common structure uses user-provided buffers:

..  code-block:: cpp

   Status ReadIntoProvidedBuffer(std::span<const std::byte> buffer, pw::Function<...> callback);

But this a similar problem: the low-level implementor of the read interface
must copy data from its source (usually a lower-level protocol buffer or
a peripheral-associated DMA buffer) into the user-provided buffer. This copy
is also required when passing between layers of the stack that need to e.g.
erase headers, perform defragmentation, or otherwise modify the structure
of the incoming data.

This process requires both runtime overhead due to copying and memory overhead
due to the need for multiple buffers to hold every message.

``Channel`` avoids this problem by using :cc:`MultiBuf
<pw::multibuf::v1::MultiBuf>`. The lower layers of the stack are responsible for
allocating peripheral-compatible buffers that are then passed up the stack for
the application code to read from or write into. ``MultiBuf`` allows for
fragementation, coalescing, insertion of headers, footers etc. without the need
for a copy.

Composable
==========
Many traditional communications code hard-codes its lower layers, making it
difficult or impossible to reused application code between e.g. a UART-based
protocol and an IP-based one. By providing a single standard interface for byte
and packet streams, ``Channel`` allows communications stacks to be layered on
top of one another in various fashions without need rewrites or intermediate
buffering of data.

Asynchronous
============
``Channel`` uses ``pw_async2`` to allow an unlimited number of channel IO
operations without the need for dedicated threads. ``pw_async2``'s
dispatcher-based structure ensures that work is only done as-needed,
cancellation and timeouts are built-in and composable, and there is no
need for deeply-nested callbacks or careful consideration of what
context a particular callback may be invoked from.

------------------
Channel attributes
------------------
Channels may be reliable, readable, writable, or seekable. A channel may be
substituted for another as long as it provides at least the same set of
capabilities; additional capabilities are okay. The channel's data type
(datagram or byte) implies different read/write semantics, so datagram/byte
channels cannot be used interchangeably in general.

Using datagram channels as byte channels
========================================
For datagram channels, the exact bytes provided to a write call will appear in a
read call on the other end. A zero-byte datagram write results in a zero-byte
datagram read, so empty datagrams may convey information.

For byte channels, bytes written may be grouped differently when read. A
zero-length byte write is meaningless and will not result in a zero-length byte
read. If a zero-length byte read occurs, it is ignored.

To facilitate simple code reuse, datagram-oriented channels may used as
byte-oriented channels when appropriate. Calling
:cc:`IgnoreDatagramBoundaries
<pw::channel::Channel::IgnoreDatagramBoundaries>` on
a datagram channel returns a byte channel reference to it. The byte view of the
channel is simply the concatenation of the contents of the datagrams.

This is only valid if, for the datagram channel:

- datagram boundaries have no significance or meaning,
- zero-length datagrams are not used to convey information, since they are
  meaningless for byte channels,
- short or zero-length writes through the byte API will not result in
  unacceptable overhead.

.. _module-pw_channel-design-hourglass_inheritance_pattern:

-----------------------------
Hourglass inheritance pattern
-----------------------------
:cc:`pw::channel::Channel` uses an uncommon, hourglass-like inheritance
pattern. This pattern offers the advantages of multiple inheritance without the
downsides (overhead, potential for the diamond problem).

Empty base classes define the public interface with strongly typed capability
guarantees. A shared core class privately inherits from all of the empty bases.
This core class is virtual and stores common state variables. A series of
implementation classes inherit from the core class. These correspond with the
empty bases at the top of the hierarchy, expressing their capabilities in the
type system once again.

This pattern is hourglass-like because the hierarchy starts with several types
at the top, narrows to a single type in the middle, then expands out to the
implementation classes at the bottom.

Advantages of this pattern:

- Express capabilities in the type system, with support for optional
  capabilities.
- No multiple virtual inheritance. All supported functionality is in a single
  vtable.
- Trivial and safe conversions between related types. Any type can be used
  through a reference to a compatible type without indirection or memory
  aliasing.

The drawback of this pattern is implementation complexity. While the core
implementation is complicated, the resulting classes are straightforward to use
or extend.

.. mermaid::

   classDiagram
       class TypeA {
           +CommonFunctions()
           +FunctionForA()
       }

       class TypeB {
           +CommonFunctions()
           +FunctionForB()
       }

       class TypeAB {
           +CommonFunctions()
           +FunctionForA()
           +FunctionForB()
       }

       class Core {
           -common_state
           -capabilities
           +CommonFunctions()
           +MaybeFunctionForA()
           +MaybeFunctionForB()

           virtual -DoFunctionForA()
           virtual -DoFunctionForB()
       }

       class ImplementTypeA {
           +CommonFunctions()
           +FunctionForA()
       }

       class ImplementTypeB {
           +CommonFunctions()
           +FunctionForB()
       }

       class ImplementTypeAB {
           +CommonFunctions()
           +FunctionForA()
           +FunctionForB()
       }

       TypeA <|-- Core
       TypeB <|-- Core
       TypeAB <|-- Core


       Core <|-- ImplementTypeA
       Core <|-- ImplementTypeB
       Core <|-- ImplementTypeAB
