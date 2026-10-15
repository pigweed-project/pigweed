.. _module-pw_ring_buffer:

==============
pw_ring_buffer
==============
.. pigweed-module::
   :name: pw_ring_buffer

The ``pw_ring_buffer`` module will eventually provide several ring buffer
implementations, each with different tradeoffs.

This documentation is incomplete :)

-----------------------
PrefixedEntryRingBuffer
-----------------------
:cpp:class:`pw::ring_buffer::PrefixedEntryRingBuffer` is a circular buffer for
arbitrary length data entries with an optional user-defined preamble byte. It
supports multiple independent readers.

Reading
=======
One or more readers can be attached to a
:cpp:class:`pw::ring_buffer::PrefixedEntryRingBuffer` using the
:cpp:class:`pw::ring_buffer::PrefixedEntryRingBufferMulti::Reader` class. Each
reader is able to peek at the front of the buffer and pop items from the buffer.
Elements are not deleted from the buffer until all the attached readers have
popped them.

There are 2 ways to read the front elmement of the buffer:
1. Copying the data. This can be done using variants of the ``PeekFront`` and
``PeekFrontWithPreamble`` which accept a ``pw::ByteSpan`` that will be written
to.
2. Reading the data directly. This is done using overloads of ``PeekFront`` and
``PeekFrontWithPreamble`` which accept a
``pw::Function<pw::Status(pw::ConstByteSpan)>`` and thus provide a short lived
view into the front entry.

Iterator
========
In crash contexts, it may be useful to scan through a ring buffer that may
have a mix of valid (yet to be read), stale (read), and invalid entries. The
``PrefixedEntryRingBufferMulti::iterator`` class can be used to walk through
entries in the provided buffer.

.. code-block:: cpp

   // A test string to push into the buffer.
   constexpr char kExampleEntry[] = "Example!";

   // Setting up buffers and attaching a reader.
   std::byte buffer[1024];
   std::byte read_buffer[256];
   PrefixedEntryRingBuffer ring_buffer;
   PrefixedEntryRingBuffer::Reader reader;
   ring_buffer.SetBuffer(buffer);
   ring_buffer.AttachReader(reader);

   // Insert some entries and process some entries.
   ring_buffer.PushBack(kExampleEntry);
   ring_buffer.PushBack(kExampleEntry);
   reader.PopFront();

   // !! A function causes a crash before we've read out all entries.
   FunctionThatCrashes();

   // ... Crash Context ...

   // You can use a range-based for-loop to walk through all entries.
   for (auto entry : ring_buffer) {
     PW_LOG_WARN("Read entry of size: %u",
                 static_cast<unsigned>(entry.buffer.size()));
   }

In cases where a crash has caused the ring buffer to have corrupted data, the
iterator will progress until it sees the corrupted section and instead move to
``iterator::end()``. The ``iterator::status()`` function returns a
:cc:`pw::Status` indicating the reason the iterator reached its end.

.. code-block:: cpp

   // ... Crash Context ...

   using iterator = PrefixedEntryRingBufferMulti::iterator;

   // Hold the iterator outside any loops to inspect it later.
   iterator it = ring_buffer.begin();
   for (; it != it.end(); ++it) {
     PW_LOG_WARN("Read entry of size: %u",
                 static_cast<unsigned>(it->buffer.size()));
   }

   // Warn if there was a failure during iteration.
   if (!it.status().ok()) {
     PW_LOG_WARN("Iterator failed to read some entries!");
   }

Iterators come in 2 different flavors:
* ``PrefixedEntryRingBufferMulti::iterator`` which will provide ``Entry``
objects with ``pw::span<std::byte>`` buffer type.
* ``PrefixedEntryRingBufferMulti::const_iterator`` which will provide ``Entry``
objects with ``pw::span<const std::byte>`` buffer type.

Data corruption
===============
``PrefixedEntryRingBufferMulti`` offers a circular ring buffer for arbitrary
length data entries. Some metadata bytes are added at the beginning of each
entry to delimit the size of the entry. Unlike the iterator, the methods in
``PrefixedEntryRingBufferMulti`` require that data in the buffer is not corrupt.
When these methods encounter data corruption, there is no generic way to
recover, and thus, the application crashes. Data corruption is indicative of
other issues.

Deleting from the back
=======================
While most use cases for the ring buffer involve writing to the back and reading
from the front, there are cases where we might want to remove entries from the
back. Consider the following:

.. code-block:: cpp

   // A test string to push into the buffer.
   constexpr char kExampleEntry[] = "Example!";

   // Setting up buffers and attaching a reader.
   std::byte buffer[1024];
   std::byte read_buffer[256];
   PrefixedEntryRingBuffer ring_buffer;
   ring_buffer.SetBuffer(buffer);

   // Try to insert some entries. If any fail, remove everything to reset.
   auto push_status = ring_buffer.TryPushBack(kExampleEntry);
   if (!push_status.ok()) {
     return push_status;
   }
   push_status = ring_buffer.TryPushBack(kExampleEntry);
   if (!push_status.ok()) {
     PW_ASSERT(ring_buffer.DeleteBack(1).ok());
     return push_status;
   }
   push_status = ring_buffer.TryPushBack(kExampleEntry);
   if (!push_status.ok()) {
     PW_ASSERT(ring_buffer.DeleteBack(2).ok());
     return push_status;
   }

   return pw::OkStatus();
