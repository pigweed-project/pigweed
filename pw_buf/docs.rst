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
automatically reclaim the memory when it is destroyed. :cc:`pw::Buf::owned` and
:cc:`pw::ConstBuf::owned` report whether a buffer owns its memory.

Despite its name, ``pw_allocator`` does not necessarily mean "heap allocation".
It simply provides an interface for requesting and releasing ownership of a
block of memory. You can, for instance, create a ``pw_allocator`` implementation
that wraps a single static buffer with an "in use" flag; a ``Buf`` created over
it would just signal transfers of ownership without ever allocating any memory.

A ``Buf`` can be constructed from a ``pw::UniquePtr<std::byte[]>``, and
:cc:`pw::TakeAllocation` converts an owned ``Buf`` back. The returned
``UniquePtr`` spans the entire underlying allocation, not just the ``Buf``'s
current view. To recreate the view later, record :cc:`pw::Buf::prefix_size` and
``size()`` first and pass them to the ``Buf(UniquePtr&&, offset, size)``
constructor.

Null and zero-length buffers
============================
An empty buffer (``empty() == true``, ``size() == 0``) is distinct from a null
buffer (``buf == nullptr``):

* A **null** buffer has no underlying buffer (``data() == nullptr``). A ``Buf``
  or ``ConstBuf`` is null when default-constructed, moved-from, reset, or when
  allocation fails.
* A **non-null zero-length** buffer has ``size() == 0`` and ``empty() == true``,
  but ``buf != nullptr``. This occurs when a buffer is sliced or truncated to 0
  bytes, when 0 bytes are allocated, or when created with
  :cc:`pw::Buf::ZeroLength` or :cc:`pw::ConstBuf::ZeroLength`.

``ZeroLength()`` returns an empty, non-null, unowned buffer. Allocating 0 bytes
with ``Allocate*`` or ``TryAllocate*`` also returns ``Buf::ZeroLength()``
without invoking the allocator. This is useful when an API needs to distinguish
a valid zero-byte payload (such as an empty datagram) from a null buffer used
to signal closure or allocation failure.

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
- :cc:`pw::Reclaim`, :cc:`pw::ReclaimPrefix`, :cc:`pw::ReclaimSuffix`: Expand a
  sliced buffer view back into its originally allocated prefix and suffix
  bytes, up to :cc:`pw::Buf::prefix_size` and :cc:`pw::Buf::suffix_size`.

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

.. important::

   :cc:`Buf <pw::Buf>` grants full control over the underlying buffer, including
   mutating its contents, slicing, reclaiming prefix or suffix bytes, and taking
   the underlying allocation. It is intended primarily for internal use within a
   module or protocol implementation.

   Do not expose ``Buf`` in public APIs where unexpected slicing, reclamation,
   or ownership transfer could violate invariants. Use :cc:`pw::ByteSpan` /
   :cc:`pw::ConstByteSpan` for borrowed access or :cc:`ConstBuf <pw::ConstBuf>`
   for transferring read-only ownership instead. To transfer mutable ownership,
   use a wrapper type (e.g. :cc:`pw::transport::WriteReservation`).

A ``Buf`` can be converted to a ``ConstBuf`` by moving it:

.. literalinclude:: examples/allocate.cc
   :language: c++
   :start-after: // DOCSTAG: [pw_buf-examples-move]
   :end-before: // DOCSTAG: [pw_buf-examples-move]

Moving the ``Buf`` transfers ownership and leaves the ``Buf`` null. Functions
that only borrow data for reading should accept a ``pw::ConstByteSpan`` (or
``pw::span<const std::byte>``) by value, which accepts both ``Buf`` and
``ConstBuf``.

.. LINT.IfChange(multibuf_relationship)

Relationship to pw_multibuf
===========================
Conceptually, a :ref:`module-pw_multibuf` is a sequence of layered :cc:`pw::Buf`
or :cc:`pw::ConstBuf` instances. Both modules provide abstractions for zero-copy
buffer management and safe ownership tracking with :ref:`module-pw_allocator`,
operating at different levels of granularity:

* **Contiguous buffer vs. sequence of buffers:** :cc:`pw::Buf` and
  :cc:`pw::ConstBuf` manage a *single, contiguous* block of memory. In contrast,
  a ``MultiBuf`` aggregates a *sequence* of multiple contiguous buffers (chunks)
  into a unified virtual span.
* **Slicing vs. layering:** A ``Buf`` allows in-place slicing and reclamation of
  reserved prefix/suffix bytes within its single allocation. A ``MultiBuf``
  generalizes this across its sequence of buffers, allowing callers to add and
  pop non-destructive layers over the entire collection of chunks.
* **Overhead:** ``pw::Buf`` has minimal metadata overhead (storing only view
  pointers and an optional deallocator), making it lightweight for operations on
  a single buffer. ``pw_multibuf`` maintains an internal deque of chunk and
  layer metadata to manage arbitrary fragments and layers across the sequence.

``pw_buf`` and ``pw_multibuf`` integrate directly:

* **Ingestion:** A :cc:`pw::Buf` or :cc:`pw::ConstBuf` can be inserted or
  appended into a ``MultiBuf`` via ``Insert()`` or ``PushBack()``, transferring
  ownership of the underlying allocation into the ``MultiBuf``.
* **Extraction:** Contiguous owned memory can be extracted from a ``MultiBuf``
  back into a :cc:`pw::Buf` or :cc:`pw::ConstBuf` using ``Release()``.
  Sliced views are preserved so that only the visible bytes are exposed,
  while the full underlying allocation is safely tracked for deallocation.

**When to use which:**

* Use ``pw_buf`` when working with a single contiguous allocation (e.g. DMA
  buffers, fixed packet buffers), especially when reserving and populating
  headers and footers in-place via :cc:`pw::Slice` and :cc:`pw::Reclaim`.
* Use ``pw_multibuf`` when assembling or disassembling data across multiple
  disjoint buffers, performing scatter-gather I/O, or composing multi-layer
  network packets from disparate memory sources.

.. LINT.ThenChange(//pw_multibuf/docs.rst:buf_relationship)

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
