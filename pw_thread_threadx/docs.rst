.. _module-pw_thread_threadx:

=================
pw_thread_threadx
=================
.. pigweed-module::
   :name: pw_thread_threadx

This is a set of backends for :ref:`module-pw_thread` based on ThreadX.

-----------------------
Thread Creation Backend
-----------------------
A backend for :cc:`pw::Thread` is offered using ``tx_thread_create``. Optional
joining support is enabled via a ``TX_EVENT_FLAGS_GROUP`` in each thread's
context.

This backend permits users to start threads where contexts must be explicitly
allocated and passed in as an option. As a quick example, a detached thread can
be created as follows:

.. code-block:: c++

   #include "pw_thread/detached_thread.h"
   #include "pw_thread_threadx/config.h"
   #include "pw_thread_threadx/context.h"
   #include "pw_thread_threadx/options.h"
   #include "tx_api.h"

   constexpr UINT kFooPriority = pw::thread::threadx::config::kDefaultPriority;
   constexpr ULONG kFooTimeSliceInterval =
       pw::thread::threadx::config::kDefaultTimeSliceInterval;
   constexpr size_t kFooStackSizeWords =
       pw::thread::threadx::config::kDefaultStackSizeWords;

   pw::thread::threadx::ContextWithStack<kFooStackSizeWords>
       example_thread_context;
   void StartExampleThread() {
     pw::thread::DetachedThread(pw::thread::threadx::Options()
                                    .set_name("example_thread")
                                    .set_priority(kFooPriority)
                                    .set_time_slice_interval(kFooTimeSliceInterval)
                                    .set_context(example_thread_context),
                                example_thread_function);
   }

.. list-table::

   * - :ref:`module-pw_thread` Facade
     - Backend Target
     - Description
   * - ``pw_thread:id``
     - ``pw_thread_threadx:id``
     - Thread identification.
   * - ``pw_thread:yield``
     - ``pw_thread_threadx:yield``
     - Thread scheduler yielding.
   * - ``pw_thread:sleep``
     - ``pw_thread_threadx:sleep``
     - Thread scheduler sleeping.
   * - ``pw_thread:thread``
     - ``pw_thread_threadx:thread``
     - Thread creation.

Module Configuration Options
============================
The following configurations can be adjusted via compile-time configuration of
this module, see the
:ref:`module documentation <module-structure-compile-time-configuration>` for
more details.

* :cc:`PW_THREAD_THREADX_CONFIG_JOINING_ENABLED`: Whether thread joining is
  enabled. By default this is disabled.

  We suggest only enabling this when thread joining is required to minimize
  the RAM and ROM cost of threads.

  Enabling this grows the RAM footprint of every :cc:`pw::Thread` as it adds a
  ``TX_EVENT_FLAGS_GROUP`` to every thread's
  :cc:`Context <pw::thread::threadx::Context>`. In addition, there is a minute
  ROM cost to construct and destroy this added object.

  ``PW_THREAD_JOINING_ENABLED`` gets set to this value.
* :cc:`PW_THREAD_THREADX_CONFIG_DEFAULT_STACK_SIZE_WORDS`: The default stack
  size in words. By default this uses the minimal ThreadX stack size.
* :cc:`PW_THREAD_THREADX_CONFIG_MAX_THREAD_NAME_LEN`: The maximum length of a
  thread's name, not including null termination. By default this is arbitrarily
  set to ``15``. This results in an array of characters which is this length +
  1 bytes in every :cc:`pw::Thread`'s context.
* :cc:`PW_THREAD_THREADX_CONFIG_DEFAULT_TIME_SLICE_INTERVAL`: The round-robin
  time slice tick interval for threads at the same priority. By default this is
  disabled as not all ports support this, using a value of ``0`` ticks.
* :cc:`PW_THREAD_THREADX_CONFIG_MIN_PRIORITY`: The minimum priority level,
  normally based on the number of priority levels.
* :cc:`PW_THREAD_THREADX_CONFIG_DEFAULT_PRIORITY`: The default priority level.
  By default this uses the minimal ThreadX priority level, given that ``0`` is
  the highest priority.
* :cc:`PW_THREAD_THREADX_CONFIG_LOG_LEVEL`: The log level to use for this
  module. Logs below this level are omitted.

ThreadX Thread Options
======================
:cc:`Options <pw::thread::threadx::Options>` configures ThreadX threads:

* :cc:`set_name <pw::thread::threadx::Options::set_name>`: Sets the name for
  the ThreadX thread. Note that this will be deep-copied into the context and
  may be truncated based on :cc:`PW_THREAD_THREADX_CONFIG_MAX_THREAD_NAME_LEN`.

* :cc:`set_priority <pw::thread::threadx::Options::set_priority>`: Sets the
  priority for the ThreadX thread from ``0`` through
  :cc:`PW_THREAD_THREADX_CONFIG_MIN_PRIORITY` (typically ``31``), where a value
  of ``0`` represents the highest priority. See ThreadX ``tx_thread_create``
  for more detail.

* :cc:`set_preemption_threshold
  <pw::thread::threadx::Options::set_preemption_threshold>`: Optionally sets
  the initial preemption threshold for the ThreadX thread (must be less than or
  equal to the priority). Only priorities higher than this level (i.e. lower
  number) are allowed to preempt this thread. Not setting the preemption
  threshold or explicitly specifying a value equal to the priority disables the
  preemption threshold. Time slicing is disabled while the preemption threshold
  is enabled, even if a time slice interval was specified.

* :cc:`set_time_slice_interval
  <pw::thread::threadx::Options::set_time_slice_interval>`: Sets the number of
  ticks (``ULONG``) this thread is allowed to run before other ready threads of
  the same priority are given a chance to run. A value of ``TX_NO_TIME_SLICE``
  (``0``) disables time-slicing of this thread.

* :cc:`set_context <pw::thread::threadx::Options::set_context>`: Sets the
  pre-allocated :cc:`Context <pw::thread::threadx::Context>` (all memory needed
  to run a thread). Note that this is required for this thread creation
  backend! The :cc:`Context <pw::thread::threadx::Context>` can either be
  constructed with an externally provided ``pw::span<ULONG>`` stack or the
  templated form of
  :cc:`ContextWithStack <pw::thread::threadx::ContextWithStack>` can be used.

-----------------------------
Thread Identification Backend
-----------------------------
A backend for ``pw::Thread::id`` and ``pw::this_thread::get_id()`` is offered
using ``tx_thread_identify()``. It uses ``DASSERT`` to ensure that a thread is
executing via ``TX_THREAD_GET_SYSTEM_STATE()``.

--------------------
Thread Sleep Backend
--------------------
A backend for ``pw::this_thread::sleep_for()`` and
``pw::this_thread::sleep_until()`` is offered using ``tx_thread_sleep()`` if
the duration is at least one tick, else ``tx_thread_relinquish()`` is used. It
uses ``pw::this_thread::get_id() != Thread::id()`` to ensure it is invoked only
from a thread.

--------------------
Thread Yield Backend
--------------------
A backend for ``pw::this_thread::yield()`` is offered via
``tx_thread_relinquish()``. It uses
``pw::this_thread::get_id() != Thread::id()`` to ensure it is invoked only from
a thread.

---------
Utilities
---------
``ForEachThread()``
===================
In cases where an operation must be performed for every thread,
:cc:`ForEachThread <pw::thread::threadx::ForEachThread>` can be used to iterate
over all the created thread TCBs. Note that it's only safe to use this while
the scheduler is disabled.

An ``Aborted`` error status is returned if the provided
:cc:`ThreadCallback <pw::thread::threadx::ThreadCallback>` returns ``false`` to
request an early termination of thread iteration.

*Return values*

* ``Aborted``: The callback requested an early-termination of thread iteration.
* ``OkStatus``: The callback has been successfully run with every thread.

--------------------
Snapshot integration
--------------------
This ``pw_thread`` backend provides helper functions that capture ThreadX thread
state to a ``pw.thread.Thread`` proto.

``SnapshotThreads()``
=====================
:cc:`SnapshotThread <pw::thread::threadx::SnapshotThread>` captures the thread
name, state, and stack information for the provided ThreadX TCB to a
``pw::thread::proto::Thread`` protobuf encoder. To ensure the most up-to-date
information is captured, the stack pointer for the currently running thread
must be provided for cases where the running thread is being captured. For ARM
Cortex-M CPUs, you can do something like this:

.. code-block:: c++

   // Capture PSP.
   void* stack_ptr = 0;
   asm volatile("mrs %0, psp\n" : "=r"(stack_ptr));
   pw::thread::ProcessThreadStackCallback cb =
       [](pw::thread::proto::Thread::StreamEncoder& encoder,
          pw::ConstByteSpan stack) -> pw::Status {
     return encoder.WriteRawStack(stack);
   };
   pw::thread::threadx::SnapshotThread(my_thread, stack_ptr, snapshot_encoder, cb);

:cc:`SnapshotThreads <pw::thread::threadx::SnapshotThreads>` wraps the singular
thread capture to instead capture all created threads to a
``pw::thread::proto::SnapshotThreadInfo`` message. This proto message overlays
a snapshot, so it is safe to static cast a
``pw::snapshot::Snapshot::StreamEncoder`` to a
``pw::thread::proto::SnapshotThreadInfo::StreamEncoder`` when calling this
function.

-------------
API reference
-------------
Moved: :cc:`pw_thread_threadx`
