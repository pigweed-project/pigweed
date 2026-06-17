.. _docs-os-threadx-features:

========================
Advantages & differences
========================
Pigweed's OS abstraction layers (``pw_sync``, ``pw_thread``, and ``pw_chrono``)
provide type-safe C++ interfaces with robust compile-time and runtime checking.
When targeting ThreadX, these APIs offer significant design and safety
advantages over raw native ThreadX APIs.

--------------------------
Synchronization vs ThreadX
--------------------------

.. note::
   For internal design details and configuration options, see the
   :ref:`module-pw_sync_threadx` backend documentation.

Type-safe mutex interfaces
==========================
Native ThreadX mutexes are represented by raw ``TX_MUTEX`` structures and
handles, created via ``tx_mutex_create()``. There is no distinction at the
type level between a mutex and other types of synchronization objects.

Pigweed's ``pw::sync::Mutex`` and ``pw::sync::TimedMutex`` are distinct,
type-safe C++ classes. They enforce correct usage at compile-time and restrict
invalid runtime operations:

* **Interrupt-context guardrails**: Locking or unlocking a mutex within an
  interrupt handler causes undefined behavior or deadlock in ThreadX.
  Pigweed's Mutex and TimedMutex implementations assert that they are not
  called in interrupt context via ``PW_DASSERT(!interrupt::InInterruptContext())``.
* **Recursive locking prevention**: Native ThreadX mutexes support recursive locking.
  However, Pigweed's Mutex interface does not support recursive locking. To enforce
  this contract, Pigweed's implementation asserts that the mutex is not recursively
  held: ``PW_DASSERT(backend::NotRecursivelyHeld(native_type_))``, which checks
  that ``tx_mutex_ownership_count == 1``.
* **Release assertions**: Unlocking a native ThreadX mutex returns a status that
  can silently go unchecked. Pigweed's ``Mutex::unlock()`` asserts that releasing the lock
  succeeded via ``PW_ASSERT(tx_mutex_put(&native_type_) == TX_SUCCESS)``.

Port-safe interrupt spinlocks
=============================
ThreadX does not offer a native interrupt spinlock API. Developers typically
attempt to implement mutual exclusion between threads and interrupts using global
interrupt control flags.

Pigweed's ``pw::sync::InterruptSpinLock`` provides a unified, port-safe spinlock
implementation. It uses ``tx_interrupt_control(TX_INT_DISABLE)`` to create a critical
section. Furthermore, to prevent accidental thread context switches while the
``InterruptSpinLock`` is held by a thread, Pigweed's implementation raises the
preemption threshold of the current thread to ``0`` (the highest priority) using
``tx_thread_preemption_change()``.

It also detects recursive locking or unlocking from an incorrect context (e.g., locking
in thread context and unlocking in interrupt context, or vice-versa) via state
verification checks on ``native_type_.state``.

.. warning::
   This backend does not support SMP yet as there's no internal lock to spin on.

Thread notifications
====================
Although one may be tempted to use ``tx_thread_sleep`` and ``tx_thread_wait_abort``
to implement direct-to-thread signaling in ThreadX, this contains a race condition:
if another thread or interrupt attempts to invoke ``tx_thread_wait_abort`` before the
blocking thread has actually executed ``tx_thread_sleep``, the wait abort would fail
silently.

To prevent this race condition, Pigweed's ``pw::sync::ThreadNotification`` and
``pw::sync::TimedThreadNotification`` backends for ThreadX are backed by the binary
semaphore backends (``pw_sync:binary_semaphore_thread_notification_backend``).
This ensures race-free, reliable thread signaling.

Compile-time lock safety via thread-safety annotations
======================================================
Native ThreadX mutexes are C-level structures with no integration with compile-time
analysis tools. The compiler cannot verify whether a shared variable is accessed
under the correct lock, nor can it enforce consistent lock acquisition orders.

Pigweed's C++ synchronization wrappers (such as ``pw::sync::Mutex`` and
``pw::sync::InterruptSpinLock``) integrate natively with Clang's static thread
safety analysis. By annotating members with attributes like ``PW_GUARDED_BY``
and methods with ``PW_EXCLUSIVE_LOCKS_REQUIRED``, the compiler statically checks
and warns against missing locks or improper release sequences at compile-time.

For detailed configuration requirements and lists of supported lock safety macros,
refer to the :ref:`pw_sync thread-safety lock annotations <module-pw_sync-thread-safety-lock-annotations>`
reference documentation.

-------------------
Sleeping vs ThreadX
-------------------

.. note::
   For details on thread options, static stack allocations, joining support,
   thread iteration, and snapshot integration, see the :ref:`module-pw_thread_threadx`
   backend documentation.

Safe sleep and yield APIs
=========================
In native ThreadX, calling ``tx_thread_sleep()`` or ``tx_thread_relinquish()``
from an interrupt context leads to crashes or corrupts kernel state.

Pigweed's ``pw::this_thread::sleep_for()``, ``sleep_until()``, and
``pw::thread::yield()`` enforce safety by asserting they are only invoked from
thread contexts via ``PW_DCHECK(get_id() != Thread::id())``.

These yield and sleep functions map directly to ``tx_thread_relinquish()`` and
``tx_thread_sleep()`` (if sleep duration is at least one tick).

Safe arbitrarily long durations
===============================
Native ThreadX sleep and timeout functions require durations represented as ticks
(``ULONG``). If a thread requests a sleep or synchronization timeout that exceeds
the maximum value representable by ``ULONG``, it can overflow or truncate.

Pigweed's sleep and timed synchronization APIs automatically split long durations
exceeding ``pw::chrono::threadx::kMaxTimeout`` into loop iterations under the hood,
ensuring timeouts of arbitrary lengths behave correctly.

---------------
Time vs ThreadX
---------------

.. note::
   For configuration requirements and operational expectations (such as how
   frequently ``SystemClock::now()`` must be called to handle native tick overflows),
   see the :ref:`module-pw_chrono_threadx` backend documentation.

Non-overflowing 64-bit system clock
===================================
ThreadX's native tick counter (``tx_time_get()``) returns a ``ULONG`` (typically
unsigned 32-bit). At a standard 1 kHz tick rate, a 32-bit counter wraps around
in approximately 49.7 days. Failing to handle this wraparound correctly in
application logic is a frequent source of long-term stability bugs.

Pigweed's ``pw::chrono::SystemClock`` uses a thread- and interrupt-safe
``InterruptSpinLock`` to track tick overflows, expanding the tick count into
a signed 64-bit value. This signed 64-bit clock will not overflow for millions
of years, allowing developers to safely write standard time arithmetic without
overflow concerns.

.. warning::
   This clock backend is not compatible with ``TX_NO_TIMER`` as that disables
   ``tx_time_get()``.

-------------------
Other functionality
-------------------
Some native ThreadX features are not wrapped by generic facades in Pigweed. In
most cases, Pigweed offers modern, target-agnostic C++ alternatives that should
be used instead:

Event flags (``TX_EVENT_FLAGS_GROUP``)
======================================
Pigweed does not provide a multi-bit event-flag or poll facade. Instead,
developers can achieve similar signaling and waiting behaviors using:

* ``pw::sync::ThreadNotification``: For simple thread unblocking/signaling.
* ``pw::sync::BinarySemaphore`` / ``pw::sync::CountingSemaphore``: For
  signaling and resource sharing.
* Cooperative multitasking via :ref:`module-pw_async2`: For waiting on multiple
  asynchronous resources or events without blocking threads.

Queues (``TX_QUEUE``)
=====================
Pigweed does not wrap ThreadX's native C-style queue structures. Instead, developers
should use Pigweed's type-safe and allocation-free containers and utilities:

* ``pw_containers``: For standard queues, circular buffers, or deques.
* ``pw::work_queue::WorkQueue``: For offloading tasks to a thread.
* ``pw::ring_buffer::Reader``: For circular byte-buffers.
* ``pw_rpc``: For robust, structured, and cross-process/cross-device messaging.

Block & byte memory pools (``TX_BLOCK_POOL`` / ``TX_BYTE_POOL``)
================================================================
ThreadX's C-style block and byte memory pool structures are not wrapped by Pigweed.
Developers should prefer standard, type-safe C++ allocator interfaces and wrappers provided by:

* :ref:`module-pw_allocator`: For type-safe, generic, and configurable memory allocation.

Repeating/periodic application timers (``TX_TIMER``)
====================================================
``pw::chrono::SystemTimer`` is exclusively a one-shot timer. Pigweed does
not wrap ThreadX's native periodic application timers. Periodic timer behavior
should be implemented by rescheduling the ``SystemTimer`` from within its expiry
callback.
