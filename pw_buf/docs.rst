.. _module-pw_buf:

======
pw_buf
======
.. pigweed-module::
   :name: pw_buf

``pw_buf`` provides the :cc:`pw::Buf` and :cc:`pw::ConstBuf` classes.

------------------------
``Buf`` and ``ConstBuf``
------------------------
:cc:`pw::Buf` and :cc:`pw::ConstBuf` are views into contiguous blocks of owned
or unowned memory.  The bytes in a ``Buf`` are mutable, while the bytes in a
``ConstBuf`` are read-only.

They can be interacted with like a ``std::span``, but offer
more functionality:

* **Automatic memory management:** Owned memory is automatically freed back
  to its allocator when the ``Buf`` goes out of scope or is reset.
* **Slicing and reclaiming:** A region backing a ``Buf`` can be sliced into a
  smaller region, creating a subspan view that can be passed along. These slices
  can be later reclaimed. This can be useful for reserving headers and footers,
  allowing someone else to populate the payload between.

Ownership and ``pw_allocator``
==============================
``Buf`` regions may optionally be allocated via a ``pw_allocator``, causing the
region's deallocator to travel with the ``Buf`` (and its slices) and
automatically reclaim the memory when it is destroyed.

Despite its name, ``pw_allocator`` does not necessarily mean "heap allocation".
It simply provides an interface for requesting and releasing ownership of a
block of memory. You can, for instance, create a ``pw_allocator`` implementation
that wraps a single static buffer with an "in use" flag; a ``Buf`` created over
it would just signal transfers of ownership without ever allocating any memory.

Slicing and reclaiming
======================
Both :cc:`pw::Buf` and :cc:`pw::ConstBuf` can be truncated or sliced.
A ``Buf`` can reclaim previously truncated or sliced regions.

For example, say you are implementing a simple framing protocol consisting of a
header followed by a payload. You would create a ``Buf`` large enough for both,
if possible directly over the memory region used by the transport (e.g. a DMA
buffer).

From this ``Buf``, you can then slice the size of the header from the front,
creating a new ``Buf`` that owns the full region, but only sees the payload.
This sliced ``Buf`` can be handed up to a higher layer for it to populate.

Depending on the type of protocol, you could handle this in one of two ways:

- If the packets have fixed size and parameters known up front, you can just
  pre-populate the header before slicing the payload ``Buf``. Once the higher
  layer is done, they can immediately hand it back to the transport as a
  well-formed packet without further modifications.
- Alternatively, if some header fields depend on the payload (e.g. length,
  checksum), you would reserve the header space upfront, hand the sliced ``Buf``
  over, then have the higher layer return it to you. At that point, you would
  reclaim the prefix span, inspect the written payload, and write the header
  using the payload's finalized state.

Since ownership of the full underlying region travels with each slice, the
packet ``Buf`` can be safely destroyed at any point, returning its region back
to the allocator that provided it.

This process can be repeated multiple layers up, creating a full protocol stack
where each layer only knows about its own packet format, without ever copying
data between layers.

The ``pw_buf`` API provides utility functions for trimming and restoring views
of a buffer:

- :cc:`pw::Slice`: Shrinks a buffer view to a sub-range of its bytes.
- :cc:`pw::Truncate`: Truncates a buffer view to a smaller size from the start.
- :cc:`pw::Reclaim`: Expands a sliced buffer view back into its originally
  allocated prefix and suffix bytes.

Relationship between Buf and ConstBuf
=====================================
:cc:`pw::Buf` and :cc:`pw::ConstBuf` manage owned or unowned buffer memory, but
serve different purposes:

* **Mutable vs. read-only:** A ``Buf`` provides mutable access to its bytes,
  whereas a ``ConstBuf`` is strictly read-only.
* **Slicing and reclaiming:** A ``Buf`` can be sliced (:cc:`pw::Slice`) and
  later reclaimed (:cc:`pw::Reclaim`), making it ideal for constructing write
  packets where headers or footers are reserved and populated in stages. A
  ``ConstBuf`` only supports slicing and truncating, never reclaiming, which is
  suited for read packets where layers strip headers as data moves up the
  protocol stack without accessing outside their assigned slice.

A ``Buf`` can be converted to a ``ConstBuf`` by moving it:

.. literalinclude:: examples/allocate.cc
   :language: c++
   :start-after: // DOCSTAG: [pw_buf-examples-move]
   :end-before: // DOCSTAG: [pw_buf-examples-move]

Moving the ``Buf`` transfers ownership and leaves the ``Buf`` null. A ``Buf``
may also be used through a ``const ConstBuf&``, though accepting a
``pw::ConstByteSpan`` (or ``pw::span<const std::byte>``) by value is
recommended for functions that only borrow data for reading.

Examples
========

Allocate and TryAllocate a ``Buf``
----------------------------------
.. literalinclude:: examples/allocate.cc
   :language: c++
   :start-after: // DOCSTAG: [pw_buf-examples-allocate]
   :end-before: // DOCSTAG: [pw_buf-examples-allocate]
   :linenos:

Create a ``Buf`` from a ``UniquePtr``
-------------------------------------
.. literalinclude:: examples/create_from_unique_ptr.cc
   :language: c++
   :start-after: // DOCSTAG: [pw_buf-examples-unique_ptr]
   :end-before: // DOCSTAG: [pw_buf-examples-unique_ptr]
   :linenos:

Pass a ``Buf`` as a ``std::span``
---------------------------------
.. literalinclude:: examples/pass_as_span.cc
   :language: c++
   :start-after: // DOCSTAG: [pw_buf-examples-span]
   :end-before: // DOCSTAG: [pw_buf-examples-span]
   :linenos:

Slice and Reclaim a ``Buf``
---------------------------
.. literalinclude:: examples/slice_and_reclaim.cc
   :language: c++
   :start-after: // DOCSTAG: [pw_buf-examples-slice_and_reclaim]
   :end-before: // DOCSTAG: [pw_buf-examples-slice_and_reclaim]
   :linenos:

Using ``Buf`` in a simple network stack
---------------------------------------
The following snippet demonstrates how to use ``pw::Buf`` and ``pw::ConstBuf``
within a packet-oriented connection socket to implement a length-prefixed
protocol.

.. literalinclude:: examples/zero_copy_socket.cc
   :language: c++
   :start-after: // DOCSTAG: [pw_buf-examples-socket]
   :end-before: // DOCSTAG: [pw_buf-examples-socket]
   :linenos:
