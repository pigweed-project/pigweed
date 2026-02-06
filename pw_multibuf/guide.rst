.. _module-pw_multibuf-guide:

==============
Usage examples
==============
.. pigweed-module-subpage::
   :name: pw_multibuf

MultiBufs are flexible binary data structures. As such, there is no single guide
on how to use them. Instead, several examples are presented that demonstrate
various aspects of how they can be used.

In each of the guides below, a :cc:`pw::Allocator` instance is needed to
instantiate a MultiBuf instance. This allocator is used to allocate the memory
for the MultiBuf instance's deque of :ref:`module-pw_multibuf-concepts-entries`.
For the purposes of these examples, the simple, low-performance
:cc:`AllocatorForTest <pw::allocator::test::AllocatorForTest>` is used.

.. literalinclude:: examples/basic.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-basic-allocator]
   :end-before: [pw_multibuf-examples-basic-allocator]


See :ref:`module-pw_allocator` for more details on allocator selection.

-----------------------------------
Iterating over heterogeneous memory
-----------------------------------
A MultiBuf instance can represent a sequence of non-contiguous regions of
memory. These regions may also have different
:ref:`module-pw_multibuf-concepts-ownership` semantics. For example, a
MultiBuf instance could be composed of some memory that it owns, some that
is shared, and some that is unmanaged (i.e. static or stack-allocated).

The following example creates just such a MultiBuf instance:

.. literalinclude:: examples/iterate.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-iterate-create]
   :end-before: [pw_multibuf-examples-iterate-create]

Note the use of :cc:`ConstMultiBuf <pw::multibuf::v2::ConstMultiBuf>`. This type
alias of :cc:`GenericMultiBuf <pw::multibuf::v2::internal::GenericMultiBuf>`
includes the :cc:`kConst <pw::multibuf::v2::Property>` as one of its
:ref:`module-pw_multibuf-design-properties`.

Regardless of how the underlying memory is stored, MultiBuf methods can
iterate over the data, both by individual bytes or by contiguous
:ref:`module-pw_multibuf-concepts-chunks`.

Iterating by bytes treats the MultiBuf instance as a single, contiguous
buffer. This is useful when the logic does not need to be aware of the
underlying memory layout. Be careful to avoid assuming that the memory is
contiguous; code such as ``std::memcpy(dst, &(*mbuf.begin()), mbuf.size())`` is
almost certainly wrong.

The following example calculates a CRC32 checksum by iterating through every
byte in the MultiBuf instance:

.. literalinclude:: examples/iterate.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-iterate-bytes]
   :end-before: [pw_multibuf-examples-iterate-bytes]

Alternatively, it is possible to iterate over the individual contiguous memory
chunks that make up the MultiBuf instance. This is useful for operations
that can be optimized by working with larger, contiguous blocks of data at once,
such as sending data over a network or writing to a file.

The following example calculates the same CRC32 checksum, but does so by
operating on whole chunks (i.e. ``ConstByteSpan`` objects) at a time:

.. literalinclude:: examples/iterate.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-iterate-chunks]
   :end-before: [pw_multibuf-examples-iterate-chunks]

Both methods iterate over the exact same data, just with different levels of
granularity. The choice of which to use depends on the specific requirements of
the task.

For the complete example, see :cs:`pw_multibuf/examples/iterate.cc`.

---------------------------
Variable-length entry queue
---------------------------
To further demonstrate how memory regions can be added and removed from a
MultiBuf instance consider the following example. This implements a
variable-length queue of binary data entries, very similar to
:ref:`module-pw_containers-inlinevarlenentryqueue`.

.. literalinclude:: examples/queue.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-queue]
   :end-before: [pw_multibuf-examples-queue]

This queue does all of its dynamic allocation in the factory method. After that
method succeeds, all the queue methods are
:ref:`infallible <module-pw_multibuf-design-infallible>`.

For the complete example, see :cs:`pw_multibuf/examples/queue.cc`.

---------
Observers
---------
With a few small changes, the above example can be used to implement an
efficient, asynchronous producer-consumer queue. This example is for
illustrative purposes only. If you need an asynchronous producer-consumer queue,
consider instead :cc:`Sender <pw::async2::Sender>`,
:cc:`Sender <pw::async2::Receiver>`, and their related types.

What this example can illustrate is how a
:cc:`Observer <pw::multibuf::v2::Observer>` can be used to react to changes in a
MultiBuf. Here, one or more tasks produce data and add it to a queue, while one
or more other tasks consume data from the queue. A MultiBuf instance can manage
the lifecycle of the queued data :ref:`module-pw_multibuf-concepts-chunks`, and
its observer mechanism is used to signal between tasks.

To start, an ``AsyncMultiBufObserver`` instance is added to wake up tasks that
are waiting for the queue to become non-empty (for consumers) or non-full (for
producers):

.. literalinclude:: examples/async_queue.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-async_queue-observer]
   :end-before: [pw_multibuf-examples-async_queue-observer]

This type extends :cc:`Observer <pw::multibuf::v2::Observer>` and receives an
:cc:`Event <pw::multibuf::v2::Observer::Event>` every time the contents or
structure of the MultiBuf instance changes.

With this, the queue can leverage the observer to add the same methods that
produces and consumers can wait on:

.. literalinclude:: examples/async_queue.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-async_queue]
   :end-before: [pw_multibuf-examples-async_queue]

Note that this queue uses a
:cc:`TrackedConstMultiBuf <pw::multibuf::v2::TrackedConstMultiBuf>`. The
"Tracked" prefix indicates the MultiBuf instance supports observers, and the
"Const" prefix indicates the data cannot be modified.

A producer task that adds data to this queue might look like the following:

.. literalinclude:: examples/async_queue.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-async_queue-producer]
   :end-before: [pw_multibuf-examples-async_queue-producer]

Finally, a consumer task that pulls data from this queue might look like the
following:

.. literalinclude:: examples/async_queue.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-async_queue-consumer]
   :end-before: [pw_multibuf-examples-async_queue-consumer]

Altogether, this approach is efficient for passing data as it avoids unnecessary
data copies and leverages the :ref:`module-pw_async2` framework for non-blocking
synchronization.

For the complete example, see :cs:`pw_multibuf/examples/async_queue.cc`.

-------------------
Scatter-gather I/O
-------------------
Another possible use case for the MultiBuf type is to manage buffers for
scatter-gather I/O operations. In this scenario, data is either read from a
single source into multiple memory regions (scatter) or written from multiple
memory regions to a single destination (gather).

As an example, the following container holds
:cc:`Message <pw::i2c::Message>`\s for performing multiple I2C reads and
writes in a single operation:

.. literalinclude:: examples/scatter_gather.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-scatter_gather-message_vector]
   :end-before: [pw_multibuf-examples-scatter_gather-message_vector]

This container has a :cc:`TrackedMultiBuf <pw::multibuf::v2::TrackedMultiBuf>`
for data to be read, and a
:cc:`TrackedConstMultiBuf <pw::multibuf::v2::TrackedConstMultiBuf>` for data to
be written. As the "Tracked" prefix indicates, these accept an observer that can
be used to signal when an I2C transfer is complete:

.. literalinclude:: examples/scatter_gather.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-scatter_gather-observer]
   :end-before: [pw_multibuf-examples-scatter_gather-observer]

With a real device, the :cc:`Message <pw::i2c::Message>`\s would be passed
to an :cc:`Initiator <pw::i2c::Initiator>`. This example uses a simpler
``TestInitiator`` type that simply accepts the messages and then waits for
another thread to indicate the transfer is complete:

.. literalinclude:: examples/scatter_gather.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-scatter_gather-initiator]
   :end-before: [pw_multibuf-examples-scatter_gather-initiator]

In this example, the ``MessageVector`` instance collects a series of I2C
messages. For read operations, it adds a destination buffer to its
``rx_buffers_`` field. For write operations, it adds a source buffer to its
``tx_buffers_`` field. An I2C driver could then iterate over the ``messages_``
vector and use the corresponding buffers from the MultiBuf instances to
perform the I/O operations.

The main addition to the existing :ref:`module-pw_i2c` is that this example
abstracts away the memory management of the individual buffers, and
automatically notifies the observer when the transfer is complete and the
messages are dropped.

For the complete example, see :cs:`pw_multibuf/examples/scatter_gather.cc`.

.. _scatter-gather I/O: https://en.wikipedia.org/wiki/Vectored_I/O

----------------------------------------------
Protocol message composition and decomposition
----------------------------------------------
The MultiBuf type was designed to facilitate creating and parsing network
protocol messages. There are two general approaches to creating packets,
referred to here as "top-down" and "bottom-up". The "top-down" approach starts
with payloads of the top-most protocol layer, and then has lower layer protocol
fields added to it.

The MultiBuf type itself does not include code to interpret memory regions
as protocol fields. Products are expected to use a component such as `Emboss`_
to accomplish this. This example provides some simple methods to get and set
fields:

.. literalinclude:: examples/transfer.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-transfer-fields]
   :end-before: [pw_multibuf-examples-transfer-fields]

These can be used to implement types for serializing and deserializing protocol
messages to and from MultiBuf instances. For example, given a network packet
protocol:

.. literalinclude:: examples/public/pw_multibuf/examples/protocol.h
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-protocol-network_packet]
   :end-before: [pw_multibuf-examples-protocol-network_packet]

A type to represent these network packets might look like:

.. literalinclude:: examples/transfer.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-transfer-network_packet]
   :end-before: [pw_multibuf-examples-transfer-network_packet]

Similarly, for a link frame protocol:

.. literalinclude:: examples/public/pw_multibuf/examples/protocol.h
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-protocol-link_frame]
   :end-before: [pw_multibuf-examples-protocol-link_frame]

A type to represent these link frames might look like:

.. literalinclude:: examples/transfer.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-transfer-link_frame]
   :end-before: [pw_multibuf-examples-transfer-link_frame]

With these, creating packets becomes straightforward:

.. literalinclude:: examples/transfer.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-transfer-create]
   :end-before: [pw_multibuf-examples-transfer-create]

For the complete example, see :cs:`pw_multibuf/examples/transfer.cc`.

.. _Emboss: https://github.com/google/emboss

---------------------
In-place modification
---------------------
Distinct from the previous example, the other approach to composing and
decomposing protocol messages is a "bottom-up" approach. This approach starts
with one or more maximally-sized protocol messages, and then proceeding to add
:ref:`module-pw_multibuf-concepts-layers` to restrict the view of the data to
higher and higher protocols in the stack.

This approach is especially useful when the lower protocol fields need to be
preserved. For example, an application which modifies only the top-most protocol
in-place would benefit from this approach.

As an example of such an in-place modification, consider an "encryptor" that
simply XORs the data with a seeded pseudorandom byte stream.

.. warning::
   This example is only an example, and is NOT cryptographically secure! DO NOT
   use it to protect data!

This example includes the link frames and network packets from before, as well
as a transport segment:

.. literalinclude:: examples/public/pw_multibuf/examples/protocol.h
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-protocol-transport_segment]
   :end-before: [pw_multibuf-examples-protocol-transport_segment]

The types to implement this and the other protocols are similar to the previous
example. A notable departure is how objects for each layer are created, with
callers creating link frames from a pool of memory buffers, and then using
factory methods that consume objects of one protocol layer and produce another,
simply by adding and removing layers.

.. literalinclude:: examples/pseudo_encrypt.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-pseudo_encrypt-from]
   :end-before: [pw_multibuf-examples-pseudo_encrypt-from]

All the tasks are derived from the ``Transformer`` type. They receive protocol
messages of a certain type from an async channel, transform them, and send them
to another async queue:

.. literalinclude:: examples/pseudo_encrypt.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-pseudo_encrypt-transformer]
   :end-before: [pw_multibuf-examples-pseudo_encrypt-transformer]

For example, the ``Relay`` type uses the ``From`` methods above to transform
messages up or down the protocl stack:

.. literalinclude:: examples/pseudo_encrypt.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-pseudo_encrypt-relay]
   :end-before: [pw_multibuf-examples-pseudo_encrypt-relay]

The ``Encryptor`` performs the "encryption" by XOR-ing data with a pseudorandom
byte stream:

.. literalinclude:: examples/pseudo_encrypt.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-pseudo_encrypt-encryptor]
   :end-before: [pw_multibuf-examples-pseudo_encrypt-encryptor]

Even the data ``Producer`` and ``Consumer`` take this form to write and verify
payload data, respectively:

.. literalinclude:: examples/pseudo_encrypt.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-pseudo_encrypt-line_transformer]
   :end-before: [pw_multibuf-examples-pseudo_encrypt-line_transformer]

.. literalinclude:: examples/pseudo_encrypt.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-pseudo_encrypt-producer]
   :end-before: [pw_multibuf-examples-pseudo_encrypt-producer]

.. literalinclude:: examples/pseudo_encrypt.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-pseudo_encrypt-transport_segment-payload]
   :end-before: [pw_multibuf-examples-pseudo_encrypt-transport_segment-payload]

.. literalinclude:: examples/pseudo_encrypt.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-pseudo_encrypt-consumer]
   :end-before: [pw_multibuf-examples-pseudo_encrypt-consumer]

Finally, a ``Recycler`` type resets buffers to be reused:

.. literalinclude:: examples/pseudo_encrypt.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-pseudo_encrypt-recycler]
   :end-before: [pw_multibuf-examples-pseudo_encrypt-recycler]

All of these tasks can then be connected to one another, given some buffers to
work with, and run end to end:

.. literalinclude:: examples/pseudo_encrypt.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-pseudo_encrypt-e2e]
   :end-before: [pw_multibuf-examples-pseudo_encrypt-e2e]

For the complete example, see :cs:`pw_multibuf/examples/pseudo_encrypt.cc`.

-------------------------------
Top-down forwarding, and Emboss
-------------------------------
``examples/top_down_forward.cc`` demonstrates how to decode a received packet
(bottom-up decoding), and then excerpt the received payload to use it in
generating a new packet.

.. literalinclude:: examples/top_down_forward.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-top_down_forward-main]
   :end-before: [pw_multibuf-examples-top_down_forward-main]

It also serves as an example of using `Emboss`_, which allows the packed data
for each header to be read and written using view. However it requires some
manual glue code for each header type.

Here for example is how to use the Emboss view types to serialize and
deserialize the 6-byte ``DemoLinkHeader``, unpacking it into an 8-byte C/C++
structure that is friendlier for the CPU to access.

.. literalinclude:: examples/top_down_forward.cc
   :language: cpp
   :linenos:
   :start-after: [pw_multibuf-examples-top_down_forward-demo_link_header_serialize]
   :end-before: [pw_multibuf-examples-top_down_forward-demo_link_header_serialize]

For that, there is a `.emb` file for the demo protocol that describes the packed
layout.

.. literalinclude:: examples/public/pw_multibuf/examples/protocol.emb
   :language: gsql
   :linenos:
   :start-after: [pw_multibuf-examples-protocol-link_frame-emboss]
   :end-before: [pw_multibuf-examples-protocol-link_frame-emboss]

For the complete code, see :cs:`pw_multibuf/examples/top_down_forward.cc`, and
for the Emboss definition, see
:cs:`pw_multibuf/examples/public/pw_multibuf/examples/protocol.emb`
