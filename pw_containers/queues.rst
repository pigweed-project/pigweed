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
      :cc:`PW_VARIABLE_LENGTH_ENTRY_QUEUE_DECLARE` macro.

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

.. _module-pw_containers-lock_free_spsc_queue:

-------------------------
Lock-free SPSC FIFO queue
-------------------------
``pw_containers`` provides a wait-free single-producer, single-consumer (SPSC)
FIFO queue that synchronizes using C++ atomics with acquire/release memory
ordering rather than locks. This makes it suitable for passing typed elements
between two threads or between a thread and an Interrupt Service Routine (ISR).

Like :cc:`pw::Queue` and :cc:`pw::FixedQueue`, two classes are provided:

- :cc:`pw::LockFreeSpscQueue` -- uses a fixed-capacity external storage buffer
  and serves as the capacity-erased base class.
- :cc:`pw::FixedLockFreeSpscQueue` -- uses a fixed-capacity storage buffer,
  which may be statically allocated inline (``FixedLockFreeSpscQueue<T, N>``) or
  dynamically/externally allocated (``FixedLockFreeSpscQueue<T>``).

Producer operations (:cc:`pw::LockFreeSpscQueue::try_push`,
:cc:`pw::LockFreeSpscQueue::try_emplace`, :cc:`pw::LockFreeSpscQueue::push`,
:cc:`pw::LockFreeSpscQueue::emplace`) may be called concurrently with consumer
operations (:cc:`pw::LockFreeSpscQueue::try_pop`,
:cc:`pw::LockFreeSpscQueue::try_peek`, :cc:`pw::LockFreeSpscQueue::front`,
:cc:`pw::LockFreeSpscQueue::pop`) as long as there is at most one producer
context and at most one consumer context active at a time.

.. note::

   Unlike other ``pw_containers`` queues that default ``SizeType`` to
   ``uint16_t``, :cc:`pw::LockFreeSpscQueue` defaults ``SizeType`` to
   ``size_t`` so that atomic index loads and stores are guaranteed to be
   lock-free across all supported target architectures.

Memory layout and SMP false sharing
===================================
:cc:`pw::LockFreeSpscQueue` stores ``head_`` and ``tail_`` as adjacent members
without cache-line padding (16 bytes total queue metadata on 32-bit targets, or
32 bytes on 64-bit targets). For single-core microcontrollers and uncached
multi-core MCUs, this compact layout is optimal and avoids 64–128+ bytes of
alignment padding per queue.

On multi-core SMP systems with coherent data caches (such as multi-core
Cortex-A or host platforms), concurrent producer pushes and consumer pops
across cores remain lock-free and functionally correct, but ``head_``,
``tail_``, and small inline buffers may share a cache line and experience false
sharing (cache-line ping-pong) under high contention.

Platform and toolchain atomic support (e.g. ARM Cortex-M0/M0+)
==============================================================
:cc:`pw::LockFreeSpscQueue` only performs aligned atomic ``load`` and ``store``
operations on its ``head_`` and ``tail_`` indices and does not use atomic
read-modify-write (RMW) operations such as ``fetch_add`` or
``compare_exchange``. This has important implications for architectures that
support hardware atomic loads and stores and memory barriers, but lack hardware
atomic RMW instructions (such as Armv6-M / ARM Cortex-M0 and Cortex-M0+, which
provide single-copy atomic 8-, 16-, and 32-bit ``LDR``/``STR`` and ``DMB``
instructions, but lack ``LDREX``/``STREX``):

* ``std::atomic::is_always_lock_free`` **is** ``false``: Per the C++ standard,
  ``std::atomic<T>::is_always_lock_free`` only evaluates to ``true`` if *all*
  atomic operations on ``T``—including RMW operations—are lock-free. On
  architectures without hardware RMW instructions, compilers report
  ``is_always_lock_free == false`` even when ``load`` and ``store`` can be
  executed atomically in hardware. :cc:`pw::LockFreeSpscQueue` accounts for
  this on Armv6-M by allowing any ``SizeType`` up to 32 bits.
* **GCC vs. Clang codegen on ARM Cortex-M0/M0+**:

  * **GCC** (``arm-none-eabi-g++``) lowers ``std::atomic`` loads and stores of
    1-, 2-, and 4-byte types directly to inline hardware ``ldr``/``str``
    instructions and ``dmb`` barriers, remaining genuinely lock-free and
    NMI-safe.
  * **Clang** (``--target=armv6m-none-eabi``) sets its maximum inline atomic
    width to 0 on Armv6-M because the target lacks RMW instructions, and
    instead emits library calls to ``__atomic_load_*`` and ``__atomic_store_*``.
    In Pigweed builds using Clang on Cortex-M0/M0+, :ref:`module-pw_atomic`
    implements these builtins by masking interrupts via ``PRIMASK``
    (``cpsid i``), which is safe across threads and maskable ISRs but is not
    NMI-safe.

Similar nuances apply to any other target platform or architecture that
provides single-copy atomic load and store instructions without hardware atomic
RMW instructions.

Element types and ISR safety trade-offs
=======================================
:cc:`pw::LockFreeSpscQueue` supports both trivial (POD) types and non-trivial,
move-only types (such as :cc:`pw::UniquePtr` or zero-copy buffer handles).
When using :cc:`pw::LockFreeSpscQueue` between a thread and an Interrupt
Service Routine (ISR), choose the element type ``T`` with the following
trade-offs in mind:

* **Trivial types** (``std::is_trivially_copyable_v<T>`` and
  ``std::is_trivially_destructible_v<T>``):

  * **Unconditional ISR and wait-free safety**: Construction and destruction
    compile down to ``std::memcpy`` and no-ops. Queue operations can never
    inadvertently invoke a blocking allocator, acquire a mutex, or run an
    unbounded destructor inside an ISR.
  * **Minimal code size**: Avoids generating per-element placement ``new`` and
    destructor loops on ``pop()`` or ``clear()``.

* **Non-trivial and move-only RAII types**:

  * **Hidden constructor/destructor hazards in ISRs**: Calling ``emplace()`` on
    a type whose constructor allocates memory or acquires a lock breaks ISR
    safety. Similarly, calling ``pop()`` or ``clear()`` invokes ``T::~T()``; if
    the last owner of a heap-backed RAII handle (e.g. :cc:`pw::UniquePtr`) is
    destroyed inside an ISR, its deallocator will execute in interrupt context.
  * **Zero-copy ownership transfer via dual queues (submission + reclaim)**:
    Supporting move-only RAII types enables zero-copy ownership transfer across
    an ISR boundary without decomposing smart pointers into raw pointers,
    provided that **all allocation and deallocation are confined to thread
    context** using a paired submission/reclaim (data + free) dual-queue
    design:

    1. **Submission queue (Thread -> ISR)**: The thread allocates or prepares an
       owned buffer/descriptor and moves it into the submission queue. The ISR
       pops the handle by move (a wait-free pointer copy that does not touch the
       allocator).
    2. **Reclaim / free queue (ISR -> Thread)**: When the ISR finishes
       transmitting or filling the buffer, it **moves** the handle into a second
       reclaim queue instead of letting it go out of scope in the ISR. The
       moved-from handle's destructor in the ISR is a no-op, and the thread pops
       completed handles from the reclaim queue to recycle or deallocate them in
       thread context.

Example
=======
.. code-block:: c++

   #include "pw_containers/lock_free_spsc_queue.h"

   pw::FixedLockFreeSpscQueue<int, 16> queue;

   void ProducerConsumerExample() {
     // Producer context (e.g. ISR):
     queue.try_push(42);

     // Consumer context (e.g. worker thread):
     if (std::optional<int> value = queue.try_pop()) {
       // Process *value.
     }
   }

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
