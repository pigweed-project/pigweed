.. _docs-os-embos-features:

========================
Advantages & differences
========================
Pigweed's OS abstraction layers (``pw_sync``, ``pw_thread``, and ``pw_chrono``)
provide type-safe C++ interfaces with robust compile-time and runtime checking.
When targeting SEGGER embOS, these APIs offer significant design and safety
advantages over raw native embOS APIs.

------------------------
Synchronization vs embOS
------------------------

.. note::
   For internal design details and configuration options, see the
   :ref:`module-pw_sync_embos` backend documentation.

Type-safe mutex interfaces
==========================
Native embOS mutexes are represented by ``OS_RSEMA`` structures and created via
``OS_CreateRSema()``. While embOS distinguishes between resource semaphores
(``OS_RSEMA``) and counting semaphores (``OS_CSEMA``) at the type level, they are
raw C-level structures that lack C++ class ergonomics and compile-time safety
safeguards.

Pigweed's ``pw::sync::Mutex`` and ``pw::sync::TimedMutex`` are distinct,
type-safe C++ classes. They enforce correct usage at compile-time and restrict
invalid runtime operations:

* **Interrupt-context guardrails**: Locking or unlocking a mutex within an
  interrupt handler causes undefined behavior or deadlock in embOS.
  Pigweed's Mutex and TimedMutex implementations assert that they are not
  called in interrupt context via ``PW_DASSERT(!interrupt::InInterruptContext())``.
* **Recursive locking prevention**: Native embOS resources (``OS_RSEMA``) support
  recursive locking. However, Pigweed's Mutex interface does not support
  recursive locking. To enforce this contract, Pigweed's implementation
  asserts that the mutex is not recursively held: checking that the returned
  lock count from ``OS_Use()`` is exactly ``1`` (and checking semaphore value
  for ``try_lock()``).
* **Release semantics**: Unlocking a mutex maps directly to ``OS_Unuse()``.
  Because ``OS_Unuse()`` has a ``void`` return type, the backend does not
  perform release success assertions, but the wrapper ensures the resource is
  safely released.

Compile-time lock safety via thread-safety annotations
======================================================
Native embOS synchronization primitives are C-level structures with no integration with compile-time
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

Port-safe interrupt spinlocks
=============================
embOS does not offer a native interrupt spinlock API. Developers typically
attempt to implement mutual exclusion between threads and interrupts using global
interrupt disable/enable functions.

Pigweed's ``pw::sync::InterruptSpinLock`` provides a unified, port-safe spinlock
implementation. It uses ``OS_IncDI()`` to mask interrupts and ``OS_SuspendAllTasks()``
to disable task switching. This prevents context switches or scheduling modifications while the
lock is held.

This backend does not support SMP; on a uniprocessor target, lock acquisition
cannot fail. However, recursive locking is still caught via assertions on a
backend boolean: ``PW_DCHECK(!native_type_.locked, "Recursive InterruptSpinLock::lock() detected")``.

Thread notifications
====================
For thread notifications, Pigweed's ``pw::sync::ThreadNotification`` and
``pw::sync::TimedThreadNotification`` are implemented using the generic binary
semaphore backend (``pw_sync:binary_semaphore_thread_notification_backend``).

On embOS, this maps directly to ``OS_CreateCSema`` / ``OS_DeleteCSema``.
Unlike FreeRTOS, embOS does not offer native direct-to-task notifications,
meaning this backend allocates a full ``OS_CSEMA`` semaphore structure per
notification object.

-------------------------------
Threading and sleeping vs embOS
-------------------------------

.. note::
   For details on thread options, static stack allocations, joining support,
   thread iteration, and snapshot integration, see the :ref:`module-pw_thread_embos`
   backend documentation.

C++ thread lifecycle management (joining and detaching)
=======================================================
embOS does not provide a native API to join or wait for terminating threads.
Pigweed's ``pw::thread::Thread`` implements C++11-style ``join()`` and ``detach()``
semantics on top of embOS event objects (``OS_EVENT``), allowing portable
C++ thread management code to run unmodified on embOS.

Interrupt-context guardrails for sleep and yield
================================================
In native embOS, calling delay or yield functions from an interrupt context is
undefined behavior and can lead to kernel instability.

Pigweed's ``pw::this_thread::sleep_for()``, ``sleep_until()``, and
``pw::thread::yield()`` enforce safety by asserting they are only invoked from
thread contexts via ``PW_DCHECK(get_id() != Thread::id())``.

These yield and sleep functions map directly to ``OS_Yield()`` and ``OS_Delay()``
(if sleep duration is at least one tick).

Support for arbitrarily long durations
======================================
Native embOS delay and timeout functions accept durations represented in ticks
(``OS_TIME``, which is typically a 32-bit integer). If a duration exceeds the maximum
value representable by ``OS_TIME``, it would overflow or truncate.

Pigweed's sleep and timed synchronization APIs prevent this by automatically
splitting durations that exceed ``pw::chrono::embos::kMaxTimeout`` into multiple
loop iterations under the hood, ensuring timeouts of arbitrary lengths behave
correctly.

----------------------
Time & timers vs embOS
----------------------

.. note::
   For configuration requirements and operational expectations, see the
   :ref:`module-pw_chrono_embos` backend documentation.

Non-overflowing 64-bit system clock
===================================
embOS's native tick counter (``OS_GetTime32()``) returns a 32-bit unsigned tick value.
At a standard 1 kHz tick rate, a 32-bit counter wraps around every 49.7 days.

Pigweed's ``pw::chrono::SystemClock`` expands this tick count to a signed
64-bit value by tracking overflows in software using an ``InterruptSpinLock``.

.. warning::
   To track overflows correctly, ``SystemClock::now()`` must be called at least
   once per overflow period (49.7 days at a 1 kHz tick rate).

Arbitrarily long timer deadlines and static allocation
======================================================
Pigweed's ``pw::chrono::SystemTimer`` is implemented without dynamic memory allocation,
wrapping embOS's native ``OS_TIMER_EX`` structures.

It supports arbitrarily long deadlines by automatically rescheduling itself under the hood
if the user-provided deadline is further out than the maximum native timeout.

-------------------
Other functionality
-------------------
Some native embOS features are not wrapped by generic facades in Pigweed. In
most cases, Pigweed offers modern, target-agnostic C++ alternatives that should
be used instead:

Event variables (``OS_EVENT``)
==============================
Pigweed does not provide a multi-bit event-flag or poll facade. Instead,
developers can achieve similar signaling and waiting behaviors using:

* ``pw::sync::ThreadNotification``: For simple thread unblocking/signaling.
* ``pw::sync::BinarySemaphore`` / ``pw::sync::CountingSemaphore``: For
  signaling and resource sharing.
* Cooperative multitasking via :ref:`module-pw_async2`: For waiting on multiple
  asynchronous resources or events without blocking threads.

Mailboxes and queues (``OS_MAILBOX`` / ``OS_Q``)
================================================
Pigweed does not wrap embOS's native C-style queue and mailbox structures. Instead, developers
should use Pigweed's type-safe and allocation-free containers and utilities:

* ``pw_containers``: For standard queues, circular buffers, or deques.
* ``pw::work_queue::WorkQueue``: For offloading tasks to a thread.
* ``pw::ring_buffer::Reader``: For circular byte-buffers.
* ``pw_rpc``: For robust, structured, and cross-process/cross-device messaging.

Memory pools (``OS_MEMPOOL``)
=============================
embOS's C-style memory pool structures are not wrapped by Pigweed.
Developers should prefer standard, type-safe C++ allocator interfaces and wrappers provided by:

* :ref:`module-pw_allocator`: For type-safe, generic, and configurable memory allocation.

Repeating software timers (``OS_TIMER``)
========================================
``pw::chrono::SystemTimer`` is exclusively a one-shot timer. Pigweed does
not wrap embOS's native repeating software timers. Periodic timer behavior
should be implemented by rescheduling the ``SystemTimer`` from within its expiry
callback.
