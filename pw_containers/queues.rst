.. _module-pw_containers-queues:

======
Queues
======
.. pigweed-module-subpage::
   :name: pw_containers

A queue is an ordered collection designed to add items at one end and remove
them from the other. This allows "first in, first out", or FIFO, behavior.
Pigweed provides both single and double-ended queues that are backed by fixed
or dynamic storage.

Pigweed provides many queue and deque implementations to meet different needs.

-------------------------------------------
``std::deque`` & ``std::queue`` equivalents
-------------------------------------------
``pw_containers`` provides a family of deques and queues that implement the
``std:deque`` or ``std::queue`` API, but offer much more control with
significantly smaller code size.

The ``std::deque``-like containers are:

- :cc:`pw::Deque` -- uses a fixed-capacity external storage buffer
- :cc:`pw::FixedDeque` -- uses a fixed-capacity storage buffer, which is
  optionally owned and may be statically or dynamically allocated
- :cc:`pw::DynamicDeque` -- dynamically allocates a storage buffer using a
  :cc:`pw::Allocator`
- :cc:`pw::InlineDeque` -- uses an inline, statically allocated storage buffer

Equivalent ``std::queue``-like classes are also provided:

- :cc:`pw::Queue`
- :cc:`pw::FixedQueue`
- :cc:`pw::DynamicQueue`
- :cc:`pw::InlineQueue`

---------------------------------------
POD deque specialization for FixedDeque
---------------------------------------
When the element type of a :cc:`pw::FixedDeque` is a POD (Plain Old Data) type
-- specifically, a type that is both trivially copyable and trivially
destructible (e.g., primitives like ``uint32_t`` or simple structs) --
``pw_containers`` automatically uses an optimized, type-erased specialization.

Because the underlying byte-manipulation implementation is shared across
different POD element types rather than re-instantiated per template parameter,
additional instantiations of ``FixedDeque`` with POD types incur significantly
less code size overhead.

Code size savings
=================
* **Additional POD instantiations**: Additional instances of ``FixedDeque`` with
  POD element types save **~59.6%** in flash memory compared to the initial
  instance cost, whereas non-POD types only save ~20% across additional
  instantiations.

  .. math::

     \text{Savings} = \frac{\text{FlashSize}_{\text{Initial}} -
     \text{FlashSize}_{\text{Additional}}}{\text{FlashSize}_{\text{Initial}}}

* **Primitive POD types**: For simple primitive types like ``uint32_t`` or
  ``uint64_t``, static ``FixedDeque`` instantiations save ~25% flash memory on
  additional instances.

.. note::

   In size reporting comparisons between POD and non-POD ``FixedDeque``
   instances, initial flash numbers for POD types may appear slightly higher
   than expected compared to other deques. This is because the benchmark uses
   a data structure slightly more complex than a primitive to minimize code
   size disparity from non-trivial constructors/destructors, bringing their
   initial flash size numbers closer for a fair comparison.

.. _module-pw_containers-inlinevarlenentryqueue:

--------------------------
pw::InlineVarLenEntryQueue
--------------------------
:cc:`InlineVarLenEntryQueue` is a queue of inline variable-length binary
entries. It is implemented as a ring (circular) buffer and supports operations
to append entries and overwrite if necessary. Entries may be zero bytes up to
the maximum size supported by the queue.

``InlineVarLenEntryQueue`` has a few interesting properties:

- Data and metadata are stored inline in a contiguous block of
  ``uint32_t``-aligned memory.
- The data structure is trivially copyable.
- All state changes are accomplished with a single update to a ``uint32_t``.
  The memory is always in a valid state and may be parsed offline.

This data structure is a much simpler version of ``PrefixedEntryRingBuffer``.
Prefer this sized-entry ring buffer to ``PrefixedEntryRingBuffer`` when:

- A simple ring buffer of variable-length entries is needed. Advanced
  features like multiple readers and a user-defined preamble are not
  required.
- A consistent, parsable, in-memory representation is required (e.g. to
  decode the buffer from a block of memory).
- C support is required.

``InlineVarLenEntryQueue`` is implemented in C and provides complete C and C++
APIs. The ``InlineVarLenEntryQueue`` C++ class is structured similarly to
:cc:`pw::InlineQueue` and :cc:`pw::Vector`.

Queue vs. deque
===============
This module provides
:ref:`module-pw_containers-inlinevarlenentryqueue`, but no
corresponding ``InlineVarLenEntryDeque`` class. Following the C++ Standard
Library style, the deque class would provide ``push_front()`` and ``pop_back()``
operations in addition to ``push_back()`` and ``pop_front()`` (equivalent to a
queue's ``push()`` and ``pop()``).

There is no ``InlineVarLenEntryDeque`` class because there is no efficient way
to implement ``push_front()`` and ``pop_back()``. These operations would
necessarily be ``O(n)``, since each entry knows the position of the next entry,
but not the previous, as in a single-linked list. Given that these operations
would be inefficient and unlikely to be used, they are not implemented, and only
a queue class is provided.

Example
=======
.. tab-set::

   .. tab-item:: C++
      :sync: c++

      Queues are declared with their max size
      (``InlineVarLenEntryQueue<kMaxSize>``) but may be used without
      specifying the size (``InlineVarLenEntryQueue<>&``).

      .. code-block:: c++

         void GenericCapacityQueue() {
           // Declare a queue with capacity sufficient for one 10-byte entry or
           // multiple smaller entries.
           pw::InlineVarLenEntryQueue<10> queue;

           // Push an entry, asserting if the entry does not fit.
           queue.push(queue, data);

           // Use push_overwrite() to push entries, overwriting older entries
           // as needed.
           queue.push_overwrite(queue, more_data);

           // Remove an entry.
           queue.pop();
         }

      Alternately, a ``InlineVarLenEntryQueue`` may be initialized in an
      existing ``uint32_t`` array.

      .. code-block:: c++

         void InitFromArray() {
           // Initialize a InlineVarLenEntryQueue.
           uint32_t buffer[32];
           auto& queue = pw::InlineVarLenEntryQueue<>::Init(buffer);

           // Largest supported entry is 114 B (13 B overhead + 1 B prefix)
           assert(queue.max_size_bytes() == 114u);

           // Write data
           queue.push_overwrite(data);
         }

   .. tab-item:: C
      :sync: c

      A ``InlineVarLenEntryQueue`` may be declared and initialized in C with the
      :c:macro:`PW_VARIABLE_LENGTH_ENTRY_QUEUE_DECLARE` macro.

      .. code-block:: c

         // Declare a queue with capacity sufficient for one 10-byte entry or
         // multiple smaller entries.
         PW_VARIABLE_LENGTH_ENTRY_QUEUE_DECLARE(queue, 10);

         // Push an entry, asserting if the entry does not fit.
         pw_InlineVarLenEntryQueue_Push(queue, "12345", 5);

         // Use push_overwrite() to push entries, overwriting older entries
         // as needed.
         pw_InlineVarLenEntryQueue_PushOverwrite(queue, "abcdefg", 7);

         // Remove an entry.
         pw_InlineVarLenEntryQueue_Pop(queue);

      Alternately, a ``InlineVarLenEntryQueue`` may be initialized in an
      existing ``uint32_t`` array.

      .. code-block:: c

         // Initialize a InlineVarLenEntryQueue.
         uint32_t buffer[32];
         pw_InlineVarLenEntryQueue_Init(buffer, 32);

         // Largest supported entry is 114 B (13 B overhead + 1 B prefix)
         assert(pw_InlineVarLenEntryQueue_MaxSizeBytes(buffer) == 114u);

         // Write some data
         pw_InlineVarLenEntryQueue_PushOverwrite(buffer, "123", 3);

Python API reference
====================
.. automodule:: pw_containers.inline_var_len_entry_queue
   :members:

------------
Size reports
------------
The tables below illustrate the memory and code size costs for various queue and
deque implementations, with ``std::deque`` serving as a baseline. The size
reports generally cover:

* The cost of a single instance.
* The cost of instantiating a second container of the same class with a
  different element type, showing the impact of templates on code size.
* The cost of using related classes, such as :cc:`pw::InlineQueue` and
  :cc:`pw::InlineDeque` or static and dynamic :cc:`pw::FixedDeque`\s.

.. TODO: b/394341806 - Add size report for InlineVarLenEntryQueue.

.. include:: queues_size_report

-------------
API reference
-------------
Moved: :cc:`pw_containers_queues`
