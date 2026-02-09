.. _module-pw_multibuf-design:

======
Design
======
.. pigweed-module-subpage::
   :name: pw_multibuf

The MultiBuf type is designed to allow data to be read or written across
multiple buffers with minimal to zero copying. This allows device I/O such as
RPC, transfer, and sockets, to move data with reduced memory, CPU and latency.

---------------------------
Why are there two versions?
---------------------------
A legacy version of the MultiBuf type encapsulated memory allocation for all
buffers, did not provide support for layered views, and generally had higher
overhead. We are in the process of migrating users to the newer version.

During the migration period, consumers can use
:ref:`module configuration <module-structure-compile-time-configuration>`
options to select which version is in use:

.. `cc``:: PW_MULTIBUF_VERSION

   Controls which MultiBuf version is used. This module configuration option
   should be set in a Pigweed configuration header. Defaults to ``1``.

.. `cc`:: PW_MULTIBUF_WARN_DEPRECATED

  Controls whether code using the v1 API emits deprecation warnings. This module
  configuration option should be set in a Pigweed configuration header. Disabled
  by default.

.. `cc`:: PW_MULTIBUF_INCLUDE_V1_ADAPTERS

  Controls whether the v2 module includes v1 adapter types. These allow setting
  ``PW_MULTIBUF_VERSION`` to ``2`` while still having code that uses the v1 API.
  Enabled by default.

The remainder of this document refers to the newer, "v2" version.

-----------------------------
Sequences of multiple buffers
-----------------------------
A MultiBuf instance manages a sequence of memory buffers using a
:cc:`pw::DynamicDeque` of :ref:`module-pw_multibuf-concepts-entries`. This
deque stores metadata about each memory region, or "chunk" rather than the data
itself. For each chunk, a series of entries tracks the memory address and
different views into it.

The ownership of each memory buffer is tracked within its entry.

New memory :ref:`module-pw_multibuf-concepts-chunks` can be added to the
sequence. The ``PushBack`` methods append data to the end, while the ``Insert``
methods allow data to be added at any position, including at the beginning or
even in the middle of an existing chunk. Data can be removed using the
``Remove`` methods, which extract a range of bytes into a new MultiBuf instance,
or the ``Discard`` method, which simply frees the memory.

When inserting into a chunk or removing a portion of a chunk, the MultiBuf
instance splits the existing entry into two, with some restrictions. For
example, an owned chunk cannot be split by a ``Remove`` operation that would
move part of it to a different MultiBuf instance, as this would create
ambiguous ownership.

These operations seamlessly integrate different memory management strategies by
handling various data types, including  MultiBuf, :cc:`pw::UniquePtr`,
:cc:`pw::SharedPtr`, and ``ByteSpan``.

-------------
Virtual spans
-------------
A MultiBuf instance presents a single, contiguous view of what may be multiple,
non-contiguous memory :ref:`module-pw_multibuf-concepts-chunks`. This virtual
span can be traversed byte-by-byte using a ``ByteIterator`` object, which
conforms to the ``std::random_access_iterator`` concept. These iterators
automatically handle transitions between underlying memory chunks, making it
appear as a single sequence of bytes.

However, this abstraction has limitations. Byte-by-byte iteration across chunk
boundaries incurs a performance overhead, and the underlying memory is not
guaranteed to be contiguous, As a result, using iterators with standard
algorithms like ``std::copy`` may be inefficient. Even worse, taking the address
of a dereferenced iterator to define a range can lead memory corruption!

For efficient bulk data transfer, the MultiBuf type provides ``CopyTo`` and
``CopyFrom`` methods, which are optimized to handle the scatter-gather nature of
the buffer. The range of :cc:`Chunks <pw::multibuf::v2::internal::ChunksImpl>`
can also be accessed using the ``Chunks`` and ``ConstChunks`` methods. Each of
these can provide ``ChunkIterator`` objects that can be used to iterate over the
contiguous spans of memory.

For accessing smaller regions of data, such as protocol headers, the ``Get``
and ``Visit`` methods are preferred. These methods intelligently avoid data
copies. If the requested range of bytes happens to be contiguous in an
underlying chunk, they return a direct ``ByteSpan`` view into that
memory. Only if the range spans multiple, non-contiguous chunks will the data be
copied into a user-provided buffer. This allows for efficient, zero-copy access
in the common case while still correctly handling fragmented data.

.. _module-pw_multibuf-design-infallible:

--------------------
Infallible operation
--------------------
When adding :ref:`module-pw_multibuf-concepts-entries`, a MultiBuf instance may
need to increase the capacity of its internal deque, which in turn allocates
memory. To avoid unpredictable failures in the middle of complex operations, the
MultiBuf type separates the fallible allocation from the infallible
modification.

This is achieved through a pattern of ``TryReserve...`` methods. For example,
before adding a series of :ref:`module-pw_multibuf-concepts-chunks` with the
``PushBack`` or ``Insert`` methods, a caller can use the
``TryReserveForPushBack`` or ``TryReserveForInsert`` methods. These attempt to
allocate the memory needed for a corrspnding call to the ``PushBack`` or
``Insert`` methods. They return ``true`` on success and ``false`` on allocation
failure, without modifying the logical state of the MultiBuf instance. If they
succeed, the calls to ``PushBack`` or ``Insert`` are guaranteed to succeed.

Error handling may be skipped almost altogether if the maximum number of chunks
and :ref:`module-pw_multibuf-concepts-layers` is known when creating a MultiBuf
instance. The ``TryReserveChunks`` and ``TryReserveLayers`` methods allow a
MultiBuf to pre-allocate all memory needed for its internal state, and then
simply use methods like  ``Insert`` and ``PushBack`` infallibly.

.. _module-pw_multibuf-design-properties:

----------
Properties
----------
The :cc:`BasicMultiBuf <pw::multibuf::v2::BasicMultiBuf>` class template uses
:cc:`Property <pw::multibuf::v2::Property>` template parameters to define the
capabilities of a MultiBuf interface. This creates a compile-time system for
specifying behavior. The core properties are:

- ``kConst``: The data within the buffer is read-only.
- ``kLayerable``: The buffer supports adding and removing hierarchical
  :ref:`module-pw_multibuf-concepts-layers` of the data.
- ``kObservable``: The buffer can notify a registered
  :cc:`Observer <pw::multibuf::v2::Observer>` of changes.

:cc:`GenericMultiBuf <pw::multibuf::v2::internal::GenericMultiBuf>` privately
inherits from all valid combinations of ``BasicMultiBuf<...kProperties>``. This
design allows any ``BasicMultiBuf`` reference to be safely ``static_cast`` to a
``GenericMultiBuf`` reference, which holds the actual state (the deque,
observer, etc.). This ``GenericMultiBuf`` can in turn be cast to any other
compatible ``BasicMultiBuf`` interface. This pattern is the same as the one used
in :ref:`module-pw_channel`, and is referred to as an
:ref:`module-pw_channel-design-hourglass_inheritance_pattern`.

To create a concrete objects, use an
:cc:`Instance <pw::multibuf::v2::internal::Instance>` templated on one of the
aliases of a specific ``BasicMultiBuf`` specialization
(e.g., :cc:`TrackedMultiBuf <pw::multibuf::v2::TrackedMultiBuf>`). The
``Instance`` class wraps a ``GenericMultiBuf`` member.

A key feature of this design is seamless and safe convertibility. An
``Instance`` object or a ``BasicMultiBuf`` reference can be implicitly or
explicitly converted to another ``BasicMultiBuf`` type, as long as the
conversion is valid.

kConst
======
The :cc:`kConst <pw::multibuf::v2::Property>` property signifies that the
underlying byte data held by the MultiBuf type is immutable. When this property
is present, methods that would modify the data, such as the ``CopyFrom`` or the
non-const ``operator[]`` methods, are disabled at compile time.

It is important to distinguish this from an immutable *structure*. A
:cc:`ConstMultiBuf <pw::multibuf::v2::ConstMultiBuf>` can still be structurally
modified. Operations like the ``Insert``, ``Remove``, ``PushBack``, or
``AddLayer`` methods are still permitted, as they only change the metadata that
defines the sequence and view of the :ref:`module-pw_multibuf-concepts-chunks`,
not the content of the memory chunks themselves.

This property provides a guarantee of data integrity similar to
``const``-correctness in C++. Any MultiBuf type that is not ``kConst`` can be
safely and implicitly converted to its ``kConst`` equivalent (e.g.,
:cc:`MultiBuf <pw::multibuf::v2::MultiBuf>` to
:cc:`ConstMultiBuf <pw::multibuf::v2::ConstMultiBuf>`). This allows functions
that only need to read data to accept a ``kConst`` version, preventing
accidental modification, while callers can freely pass mutable buffers to them.
The reverse conversion, from ``kConst`` to mutable, is disallowed.

kLayerable
==========
The :cc:`kLayerable <pw::multibuf::v2::Property>` property enables a MultiBuf
type to manage a stack of views, or :ref:`module-pw_multibuf-concepts-layers`.
Each layer represents a subspan of the layer beneath it, effectively creating a
narrower, more specific view of the underlying memory without any data copying.

For example, a MultiBuf instance might initially represent a full Ethernet
frame. An Ethernet handler can process the header, then use the ``AddLayer``
method with a given header size and payload size  to create a new top layer that
exposes only the IP packet within the frame. This new view can then be passed to
an IP handler. The IP handler can, in turn, process its header and add another
layer to expose the TCP segment to the TCP handler.

This process is reversible. After the TCP handler is finished, the TCP layer
can be removed with the ``PopLayer`` method, restoring the view to the IP
packet. This allows each protocol handler in a stack to operate on its relevant
payload in isolation, cleanly managing the boundaries between protocol data
without the overhead and complexity of copying data between intermediate
buffers.

kObservable
===========
A MultiBuf with the :cc:`kObservable <pw::multibuf::v2::Property>` property can
have a :cc:`Observer <pw::multibuf::v2::Observer>` registered via the
``set_observer`` method. This observer will be notified of structural changes to
the buffer. Whenever bytes or :ref:`module-pw_multibuf-concepts-layers` are
added or removed (e.g., through the ``Insert``, ``Remove``, ``AddLayer``,
``PopLayer``, or ``Clear`` methods), the MultiBuf instance invokes the
observer's ``Notify`` method, passing an event with a type like
``kBytesRemoved`` and a corresponding size.

This mechanism is useful for implementing asynchronous workflows and flow
control. For example, consider a system sending a large message contained in an
observable MultiBuf instance. The buffer could be passed to a transport layer
that sends the data in the background. The original sender can register an
observer that waits to be notified with a ``kBytesRemoved`` event of the entire
message size. This notification would be triggered when the transport layer is
done sending the data and calls the ``Clear`` or ``Discard`` methods on the
buffer. This signals to the sender that the transmission is complete and the
associated memory has been freed.

This can be used to implement backpressure. A sender can be notified when
memory is freed, indicating that the receiver has consumed the data and there
is now capacity to send more, preventing the sender from overwhelming the
receiver.
