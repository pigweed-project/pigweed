.. _docs-os-zephyr-features:

========================
Advantages & differences
========================
Pigweed's OS abstraction layers (``pw_sync``, ``pw_thread``, and ``pw_chrono``)
provide type-safe C++ interfaces with robust compile-time and runtime checking.
When targeting Zephyr, these APIs offer significant design and safety
advantages over raw native Zephyr APIs.

-------------------------
Synchronization vs Zephyr
-------------------------

.. note::
   For internal design details, configuration options, and constraints, see the
   :ref:`module-pw_sync_zephyr` backend documentation.

C++ RAII and safety guardrails
==============================
Zephyr's native synchronization APIs (such as ``k_mutex``, ``k_sem``, and
``k_event``) are raw C-level interfaces. They do not natively support C++
Resource Acquisition Is Initialization (RAII) conventions, compile-time thread safety
auditing, or object-oriented lifetime management.

Pigweed's ``pw::sync::Mutex`` and ``pw::sync::TimedMutex`` wrap Zephyr's native
primitives into C++ classes conforming to standard C++ ``BasicLockable``
requirements. This adds several safety guardrails:

* **RAII compatibility**: Enables standard C++ lock managers (such as
  ``std::lock_guard`` and ``std::unique_lock``) to manage lock lifetimes
  automatically, preventing deadlocks caused by forgotten unlocks on early return
  paths.
* **Strict non-recursive locking**: Zephyr's native ``k_mutex`` supports
  reentrancy (recursive locking) by default, which can hide structural design flaws.
  Pigweed's Mutex enforces strict non-recursive semantics, asserting against
  recursive lock attempts in debug builds via ``PW_DASSERT(native_type_.lock_count == 1)``.
* **Interrupt-context guardrails**: Locking or unlocking a mutex within an
  interrupt handler causes undefined behavior. Pigweed's Mutex asserts that it
  is not called from interrupt context via ``PW_DASSERT(!interrupt::InInterruptContext())``.
* **Unlock assertions**: Pigweed asserts that releasing the lock succeeded via
  ``PW_ASSERT(k_mutex_unlock(&native_type_) == 0)``.

Compile-time lock safety via thread-safety annotations
======================================================
Native Zephyr mutexes are C-level structures with no integration with C++
compile-time analysis tools. The compiler cannot verify whether a shared
variable is accessed under the correct lock, nor can it enforce consistent lock
acquisition orders, making data races and deadlocks easy to introduce.

Pigweed's C++ synchronization wrappers (such as ``pw::sync::Mutex`` and
``pw::sync::InterruptSpinLock``) integrate natively with Clang's static thread
safety analysis. By annotating members with attributes like ``PW_GUARDED_BY``
and methods with ``PW_EXCLUSIVE_LOCKS_REQUIRED``, the compiler statically checks
and warns against missing locks or improper release sequences at compile-time.

For detailed configuration requirements and lists of supported lock safety macros,
refer to the :ref:`thread-safety lock annotations <module-pw_sync-thread-safety-lock-annotations>`
documentation.

Port-safe interrupt spinlocks
=============================
Pigweed's ``pw::sync::InterruptSpinLock`` provides a unified, port-safe
spinlock implementation wrapping Zephyr's native ``k_spinlock``. It prevents
accidental recursive locking inside the same execution context through debug
assertions, providing a consistent and safe critical-section mechanism across
all targets.

Timeout overflow protection
============================
Zephyr timeouts are typically represented as ticks or milliseconds. If a
relative timeout exceeds the maximum value representable by the native kernel
type (especially on configurations without 64-bit timeouts), it can overflow or
be truncated.

Pigweed's timed synchronization APIs (such as ``TimedMutex::try_lock_for``)
automatically detect durations that exceed native tick limits and split them into
smaller loop iterations under the hood, ensuring arbitrarily long timeouts behave correctly.

-------------------
Sleeping vs Zephyr
-------------------

.. note::
   For details on thread options, stack allocations, and iteration, see the
   :ref:`module-pw_thread_zephyr` backend documentation.

Safe sleep and yield APIs
=========================
In native Zephyr, calling sleep or yield functions from an interrupt context leads
to crashes or undefined kernel state.

Pigweed's ``pw::this_thread::sleep_for()``, ``sleep_until()``, and
``pw::thread::yield()`` enforce safety by asserting they are only invoked from
thread contexts via ``PW_DCHECK(get_id() != Thread::id())``.

Safe arbitrarily long durations
===============================
Similar to timed synchronization primitives, Pigweed's sleep APIs automatically
split relative sleep timeouts exceeding kernel limits into loop iterations,
preventing numeric overflow of ticks.

------------------------
Time & timers vs Zephyr
------------------------

.. note::
   For configuration requirements and operational expectations, see the
   :ref:`module-pw_chrono_zephyr` backend documentation.

Non-overflowing system clock
============================
Zephyr's native tick counter (obtained via ``k_uptime_ticks()``) returns a
signed 64-bit integer (``int64_t``). At standard tick rates, this counter will
not overflow for millions of years.

Pigweed's ``pw::chrono::SystemClock`` maps directly to this signed 64-bit tick
counter, providing a portable standard interface for time arithmetic without the
wraparound and overflow issues common to platforms with 32-bit native clocks.

Race-free system timer destructor
=================================
Native Zephyr ``k_timer`` structures are stopped via ``k_timer_stop()``. However,
if the timer's callback is currently executing in interrupt context (ISR) when the
timer is stopped or destroyed, it can trigger race conditions or use-after-free
bugs in application code.

Pigweed's ``pw::chrono::SystemTimer`` backend schedules its callbacks using Zephyr's
delayable work-queue API (``k_work_delayable``). This allows the ``SystemTimer``
destructor to call ``k_work_cancel_delayable_sync()``, which cancels the work item
and blocks until any currently running callback completes. This guarantees that once
the timer object is destroyed, the callback is definitively inactive, eliminating
a common class of RTOS concurrency bugs. Note that while callbacks run in a work
queue thread-context to support synchronous cancellation, they must still conform
to the ``SystemTimer`` contract and never attempt to block.

---------------------------
Asserts & logging vs Zephyr
---------------------------

.. note::
   For configuration details on assertion and logging routing, see the
   :ref:`module-pw_assert_zephyr` and :ref:`module-pw_log_zephyr` backend documentation.

Stable and multi-language tokenization
======================================
Zephyr offers a dictionary-based logging option for format string compression.
However, Zephyr's tokenization is based on mapping the memory addresses of
format strings within the compiled binary. Consequently, any code change or
recompilation shifts these addresses, creating a new token database that must
exactly match the specific binary. Managing these ephemeral databases across
many released firmware versions is challenging, and host-side decoding is mostly
limited to Zephyr's own Python tooling.

In contrast, Pigweed's :ref:`module-pw_tokenizer` uses stable string hashes computed
directly from the format string content:

* **Stable token databases**: Recompiling the binary or changing unrelated code
  does not invalidate the token database. Adding or modifying a log statement
  only adds new entries to the database without altering existing ones. This allows
  you to maintain a single, cumulative token database across all historical builds.
* **Broad language support**: Pigweed provides official detokenization libraries in
  C++, Python, Java, and JavaScript. This enables host applications, web interfaces,
  and mobile companion apps (such as an Android or iOS app receiving logs over
  Bluetooth) to easily decode log streams directly.

----------------------------------------
Portable runtime vs. Zephyr OS simulator
----------------------------------------
Zephyr provides a host execution model via its ``native_sim`` (or
``native_posix``) targets. However, this target compiles the entire Zephyr
kernel, scheduler, and subsystems into a simulated host executable. It is
designed solely for testing and simulation, and is not suitable for running or
shipping to production on host platforms. It also requires the entire Zephyr
build stack and host-side Python tooling.

In contrast, Pigweed's OS abstraction layers are designed from the beginning as
a porting layer, intended to run in production across multiple underlying
operating systems, and multiple build systems. When targeting host platforms
(macOS, Windows, or Linux), Pigweed resolves these facades to standard C++ STL
backends (such as ``std::mutex`` and ``std::thread``) with minimal dependencies.

This approach allows you to compile and run your core application logic on host
platforms with near-zero overhead, and deploy that same code **to production**
on host systems (e.g., as part of a companion application or server backend)—a
workflow that is not possible with Zephyr's native simulation.

For example, :ref:`module-pw_rpc` has shipped to millions of production devices
running inside a Linux application (as part of a larger project). A
Zephyr-centric solution cannot easily be ported to these production host
environments due to its heavy dependence on Zephyr's custom build system (West)
and simulated kernel architecture.

-------------------
Other functionality
-------------------
Some native Zephyr features are not wrapped by generic facades in Pigweed. In
most cases, Pigweed offers modern, target-agnostic C++ alternatives that should
be used instead:

Events (``k_event``) and polling (``k_poll``)
=============================================
Pigweed does not provide a multi-bit event-flag or poll facade. Instead,
developers can achieve similar signaling and waiting behaviors using:

* ``pw::sync::ThreadNotification``: For simple thread unblocking/signaling.
* ``pw::sync::BinarySemaphore`` / ``pw::sync::CountingSemaphore``: For
  signaling and resource sharing.
* Cooperative multitasking via :ref:`module-pw_async2`: For waiting on multiple
  asynchronous resources or events without blocking threads.

Message queues (``k_msgq``) and mailboxes (``k_mbox``)
======================================================
Pigweed does not wrap Zephyr's message queues or mailboxes. Instead, developers
should use Pigweed's type-safe and allocation-free containers and utilities:

* ``pw_containers``: For standard queues, circular buffers, or deques.
* ``pw::work_queue::WorkQueue``: For offloading tasks to a thread.
* ``pw::ring_buffer::Reader``: For circular byte-buffers.
* ``pw_rpc``: For robust, structured, and cross-process/cross-device messaging.

FIFO and LIFO (``k_fifo`` / ``k_lifo``)
=======================================
Zephyr's C-style FIFO and LIFO queues are not wrapped by Pigweed. Developers
should prefer standard, type-safe C++ container classes (e.g., those in
``pw_containers``) to avoid raw pointer casting.

Condition variables (``k_condvar``)
===================================
Pigweed does not provide a ``ConditionVariable`` facade. Synchronization
should be handled via:

* ``pw::sync::ThreadNotification``: For waiting on a single event.
* Primitives from :ref:`module-pw_async2`: For more complex cooperative waiting
  and signaling.

Repeating/periodic software timers
==================================
``pw::chrono::SystemTimer`` is exclusively a one-shot timer. Pigweed does
not wrap Zephyr's native periodic or auto-reload software timers. Periodic
timer behavior should be implemented by rescheduling the ``SystemTimer`` from
within its expiry callback.
