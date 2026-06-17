.. _docs-os-freertos-features:

========================
Advantages & differences
========================
Pigweed's OS abstraction layers (``pw_sync``, ``pw_thread``, and ``pw_chrono``)
provide type-safe C++ interfaces with robust compile-time and runtime checking.
When targeting FreeRTOS, these APIs offer significant design and safety
advantages over raw native FreeRTOS APIs.

---------------------------
Synchronization vs FreeRTOS
---------------------------

.. note::
   For internal design details, configuration options, and constraints (such as
   how ``InterruptSpinLock`` manages synchronous versus asynchronous yield behavior or how
   to customize the notification index to prevent task notification collisions), see the
   :ref:`module-pw_sync_freertos` backend documentation.

Type-safe mutex interfaces
==========================
Native FreeRTOS mutexes are represented by raw ``SemaphoreHandle_t`` variables
created via ``xSemaphoreCreateMutexStatic()``. There is no distinction at the
type level between a mutex, a counting semaphore, or a binary semaphore.

Pigweed's mutex wrapper utilizes ``StaticSemaphore_t`` static allocations underneath
to bypass dynamic memory overhead. For configuration requirements and API mappings, see the
:ref:`module-pw_sync_freertos-mutex` backend reference.

Pigweed's ``pw::sync::Mutex`` and ``pw::sync::TimedMutex`` are distinct,
type-safe C++ classes. They enforce correct usage at compile-time and restrict
invalid runtime operations:

* **Interrupt-context guardrails**: Locking or unlocking a mutex within an
  interrupt handler causes undefined behavior or deadlock in FreeRTOS.
  Pigweed's implementations assert that they are not called in interrupt
  context via ``PW_DASSERT(!interrupt::InInterruptContext())``.
* **Release assertions**: Unlocking a native FreeRTOS semaphore can silently
  fail or succeed under incorrect ownership depending on configuration.
  Pigweed's ``Mutex::unlock()`` asserts that releasing the lock succeeded
  via ``PW_ASSERT(xSemaphoreGive(...) == pdTRUE)``.

Port-safe interrupt spinlocks
=============================
FreeRTOS does not offer a native interrupt spinlock API. Developers typically
attempt to implement mutual exclusion between threads and interrupts using task
critical sections.

However, on FreeRTOS ports that implement synchronous context switches
(synchronous yields), calling signaling APIs inside a critical section can
trigger ``taskYIELD()`` immediately, causing a context switch and enabling
interrupts prematurely.

Pigweed's ``pw::sync::InterruptSpinLock`` provides a unified, port-safe spinlock
implementation. It automatically detects ports with synchronous yield behavior
and disables/restores scheduling via ``vTaskSuspendAll()`` /
``xTaskResumeAll()`` within the critical section to prevent immediate task
switching. It also catches accidental recursive locking via
``PW_DASSERT(!native_type_.locked)``.

For a deep dive into how Pigweed implements this to handle critical-section yields on
ports with synchronous context switches, see the
:ref:`module-pw_sync_freertos-spinlock` design notes.

Optimal task notifications
==========================
FreeRTOS direct-to-task notifications are lightweight but suffer from potential
collision because they share a single global state (``ucNotifyState``) and
value array index (historically only index 0) per task's TCB. Multiple
independent libraries attempting to use index 0 of direct task notifications
will conflict, triggering subtle, silent, and difficult-to-debug race
conditions.

Pigweed's ``pw::sync::ThreadNotification`` and
``pw::sync::TimedThreadNotification`` provide a clean, object-oriented
abstraction that maps optimally to FreeRTOS task notifications:

* **Zero Extra Allocation**: They consume task notifications directly inside
  the TCB, incurring zero extra RAM or static semaphore memory allocation.
* **Safe and Clean Lifecycle**: Pigweed only registers the task for
  notifications when it is about to block, and cleanly clears the notification
  state on timeout or abort. This matches the internal mechanism of FreeRTOS
  Stream/Message Buffers, making it safe and highly efficient.
* **Customizable Indices**: The target index is configurable via
  ``PW_SYNC_FREERTOS_CONFIG_THREAD_NOTIFICATION_INDEX`` to avoid collisions
  with existing external libraries.

For details on how this optimized backend interacts with native task notifications
and how the lifecycle is cleaned up on timeout or abort, see the
:ref:`module-pw_sync_freertos-threadnotification` backend reference.

Compile-time lock safety via thread-safety annotations
======================================================
Native FreeRTOS mutexes are C-level structures and handles (``SemaphoreHandle_t``)
with no integration with compile-time analysis tools. The compiler cannot verify
whether a shared variable is accessed under the correct lock, nor can it enforce
consistent lock acquisition orders, making concurrency bugs like data races and
deadlocks easy to introduce and difficult to diagnose.

Pigweed's C++ synchronization wrappers (such as ``pw::sync::Mutex`` and
``pw::sync::InterruptSpinLock``) integrate natively with Clang's static thread
safety analysis. By annotating members with attributes like ``PW_GUARDED_BY``
and methods with ``PW_EXCLUSIVE_LOCKS_REQUIRED``, the compiler statically checks
and warns against missing locks or improper release sequences at compile-time.

For detailed configuration requirements and lists of supported lock safety macros,
refer to the :ref:`pw_sync thread-safety lock annotations <module-pw_sync-thread-safety-lock-annotations>`
reference documentation.

--------------------
Sleeping vs FreeRTOS
--------------------

.. note::
   For details on thread options, static versus dynamic stack allocations, joining support,
   thread iteration, and snapshot integration, see the :ref:`module-pw_thread_freertos`
   backend documentation.

Safe sleep and yield APIs
=========================
In native FreeRTOS, calling ``vTaskDelay()`` or ``taskYIELD()`` from an
interrupt context leads to crashes or corrupts kernel state.

Pigweed's ``pw::this_thread::sleep_for()``, ``sleep_until()``, and
``pw::thread::yield()`` enforce safety by asserting they are only invoked from
thread contexts via ``PW_DCHECK(get_id() != Thread::id())``.

These delay and yield functions map directly to ``vTaskDelay()`` and ``taskYIELD()``
as detailed in the :ref:`module-pw_thread_freertos-sleep` backend reference.

Safe arbitrarily long durations
===============================
Native FreeRTOS delay functions require timeouts represented as a
``TickType_t``. If a thread requests a sleep or synchronization timeout that
exceeds the maximum value representable by ``TickType_t``, it can overflow or
truncate.

Pigweed's sleep and timed synchronization APIs automatically split long
durations exceeding ``pw::chrono::freertos::kMaxTimeout`` into loop
iterations under the hood, ensuring timeouts of arbitrary lengths behave
correctly.

-------------------------
Time & timers vs FreeRTOS
-------------------------

.. note::
   For configuration requirements (such as software timer configuration) and operational
   expectations (like how frequently ``SystemClock::now()`` must be called to handle native tick
   overflows), see the :ref:`module-pw_chrono_freertos` backend documentation.

Non-overflowing 64-bit system clock
===================================
FreeRTOS's native tick counters (``xTaskGetTickCount()`` and
``xTaskGetTickCountFromISR()``) return a ``TickType_t`` (typically unsigned
32-bit). At a standard 1 kHz tick rate, a 32-bit counter wraps around in
approximately 49.7 days. Failing to handle this wraparound correctly in
application logic is a frequent source of long-term stability bugs.

Pigweed's ``pw::chrono::SystemClock`` uses a thread- and interrupt-safe
``InterruptSpinLock`` to track tick overflows, expanding the tick count into
a signed 64-bit value. This signed 64-bit clock will not overflow for millions
of years, allowing developers to safely write standard time arithmetic without
overflow concerns.

For requirements regarding clock overflow tracking and periodic ``now()`` invocation details,
see the :ref:`module-pw_chrono_freertos-systemclock` backend reference.

Race-free system timer destructor
=================================
Native FreeRTOS software timers are deleted asynchronously via
``xTimerDelete()`` through a command queue. This creates a race condition:
if a timer is about to expire and the timer service task has lower priority,
the timer's callback may run *after* ``xTimerDelete()`` is called, potentially
accessing freed memory or invalid application state.

Pigweed's ``pw::chrono::SystemTimer`` wrapper eliminates this race condition.
Its destructor calls ``Cancel()``, enqueues ``xTimerDelete()``, and then
yields or blocks in a loop until the kernel reports that the timer is
officially inactive (``xTimerIsTimerActive()`` is false).

Additionally, ``SystemTimer`` supports arbitrarily long deadlines by
automatically rescheduling itself under the hood if the user-provided
deadline is further out than the maximum native timeout.

For more details on how the static timer API and daemon tasks are integrated, see the
:ref:`module-pw_chrono_freertos-systemtimer` backend reference.

-------------------
Other functionality
-------------------
Some native FreeRTOS features are not wrapped by generic facades in Pigweed. In
most cases, Pigweed offers modern, target-agnostic alternatives that should be
used instead:

Event groups (``EventGroupHandle_t``)
=====================================
Pigweed does not provide a multi-bit event-flag/event-group facade. Instead,
developers can achieve similar synchronization using:

* ``pw::sync::ThreadNotification``: For simple thread unblocking/signaling.
* ``pw::sync::BinarySemaphore`` / ``pw::sync::CountingSemaphore``: For
  signaling and resource sharing.

Queues and queue sets (``QueueHandle_t``)
=========================================
Pigweed does not wrap FreeRTOS's C-style queue primitives. Instead, developers
should use Pigweed's type-safe and allocation-free containers and utilities:

* ``pw_containers``: For standard queues, circular buffers, or deques.
* ``pw::work_queue::WorkQueue``: For offloading tasks to a thread.
* ``pw::ring_buffer::Reader``: For circular byte-buffers.

Stream buffers and message buffers
==================================
Pigweed does not wrap FreeRTOS stream and message buffers. Target-agnostic
messaging can be achieved using:

* ``pw_rpc``: For robust, structured, and cross-process/cross-device
  streaming.
* ``pw_ring_buffer``: For safe, lock-free or locked data streaming between
  threads or interrupts.

Direct task notifications as general-purpose flags/registers
============================================================
FreeRTOS task notifications can be used to store custom flags or values
directly in a task's TCB. Pigweed's ``pw::sync::ThreadNotification`` uses
task notifications internally but hides the raw indexes. Pigweed does not
expose a facade for custom task notification flags.

Repeating/periodic software timers
==================================
``pw::chrono::SystemTimer`` is exclusively a one-shot timer. Pigweed does
not wrap FreeRTOS's auto-reload/periodic software timers. Periodic behavior
should be implemented by rescheduling the ``SystemTimer`` from within its
expiry callback.

Co-routines
===========
FreeRTOS includes a legacy co-routines feature. Pigweed does not wrap or
support FreeRTOS co-routines. Modern cooperative multitasking in Pigweed
is supported through modern asynchronous frameworks like
:ref:`module-pw_async2`.
