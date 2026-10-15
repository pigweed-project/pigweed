.. _module-pw_thread:

=========
pw_thread
=========
.. pigweed-module::
   :name: pw_thread

The ``pw_thread`` module contains utilities for thread creation and thread
execution.

---------------
Thread Sleeping
---------------
C++
===
.. cpp:function:: void pw::this_thread::sleep_for(chrono::SystemClock::duration sleep_duration)

   Blocks the execution of the current thread for at least the specified
   duration. This function may block for longer due to scheduling or resource
   contention delays.

   A sleep duration of 0 will at minimum yield, meaning it will provide a hint
   to the implementation to reschedule the execution of threads, allowing other
   threads to run.

   **Precondition:** This can only be called from a thread, meaning the
   scheduler is running.

.. cpp:function:: void pw::this_thread::sleep_until(chrono::SystemClock::time_point wakeup_time)

   Blocks the execution of the current thread until at least the specified
   time has been reached. This function may block for longer due to scheduling
   or resource contention delays.

   A sleep deadline in the past up to the current time will at minimum yield
   meaning it will provide a hint to the implementation to reschedule the
   execution of threads, allowing other threads to run.

   **Precondition:** This can only be called from a thread, meaning the
   scheduler is running.

Examples in C++
---------------
.. code-block:: cpp

   #include <chrono>

   #include "pw_chrono/system_clock.h"
   #include "pw_thread/sleep.h"

   using std::literals::chrono_literals::ms;

   void FunctionInvokedByThread() { pw::this_thread::sleep_for(42ms); }

   void AnotherFunctionInvokedByThread() {
     pw::this_thread::sleep_until(pw::chrono::SystemClock::now() + 42ms);
   }

C
=
.. cpp:function:: void pw_this_thread_SleepFor(pw_chrono_SystemClock_Duration sleep_duration)

   Invokes ``pw::this_thread::sleep_until(sleep_duration)``.

.. cpp:function:: void pw_this_thread_SleepUntil(pw_chrono_SystemClock_TimePoint wakeup_time)

   Invokes ``pw::this_thread::sleep_until(wakeup_time)``.


---------------
Thread Yielding
---------------
C++
===
.. cpp:function:: void pw::this_thread::yield() noexcept

   Provides a hint to the implementation to reschedule the execution of threads,
   allowing other threads to run.

   The exact behavior of this function depends on the implementation, in
   particular on the mechanics of the OS scheduler in use and the state of the
   system.

   **Precondition:** This can only be called from a thread, meaning the
   scheduler is running.

Example in C++
---------------
.. code-block:: cpp

   #include "pw_thread/yield.h"

   void FunctionInvokedByThread() { pw::this_thread::yield(); }

C
=
.. cpp:function:: void pw_this_thread_Yield(void)

   Invokes ``pw::this_thread::yield()``.

---------------------
Thread Identification
---------------------
The class :cc:`pw::Thread::id <pw::thread::Thread::id>` is a lightweight,
trivially copyable class that serves as a unique identifier of Thread objects.

Instances of this class may also hold the special distinct value that does
not represent any thread. Once a thread has finished, the value of its
Thread::id may be reused by another thread.

This class is designed for use as key in associative containers, both ordered
and unordered.

Although the current API is similar to C++11 STL `std::thread::id
<https://en.cppreference.com/w/cpp/thread/thread/id>`_, it is missing the
required hashing and streaming operators and may diverge further in the future.

A thread's identification (:cc:`pw::Thread::id <pw::thread::Thread::id>`) can
be acquired only in C++ in one of two ways:

1) Using the :cc:`pw::Thread` handle's ``pw::Thread::id get_id() const``
   method.
2) While executing the thread using
   ``pw::Thread::id pw::this_thread::get_id() noexcept``.

.. cpp:function:: pw::Thread::id pw::this_thread::get_id() noexcept

   This is thread safe, not IRQ safe. It is implementation defined whether this
   is safe before the scheduler has started.

Backends may support a `native()` method on their thread id implementation.
This is present in order to provide a way to pass a native thread handle to
native RTOS functions. It is non-portable, and not recommended that this be
used in general, and is provided for the case that no viable alternative exists
to using the native thread handle directly.

Example
=======
.. code-block:: cpp

   #include "pw_thread/thread.h"

   void FunctionInvokedByThread() {
     const pw::Thread::id my_id = pw::this_thread::get_id();
   }

.. _module-pw_thread-thread-creation:

---------------
Thread creation
---------------
The :cc:`pw::Thread` class can be used to create a thread, allowing
multiple functions to execute concurrently.

API reference
=============
Moved: :cc:`pw_thread`

Differences from ``std::thread``
================================
The ``pw::thread:Thread`` API is similar to the C++11 STL `std::thread
<https://en.cppreference.com/w/cpp/thread/thread>`_ class, meaning the object is
effectively a thread handle and not an object which contains the thread's
context. Unlike ``std::thread``, the API requires ``pw::thread::Options`` as an
argument. These options are platform-specific, and allow the user to specify
details such as the thread's name, priority, stack size, and where the thread's
memory will be stored.

We recognize that the C++11's STL ``std::thread`` API has some drawbacks where
it is easy to forget to join or detach the thread handle. Because of this, we
offer helper wrappers like the ``pw::thread::DetachedThread``. Soon we will
extend this by also adding a ``pw::thread::JoiningThread`` helper wrapper which
will also have a lighter weight C++20 ``std::jthread`` like cooperative
cancellation contract to make joining safer and easier.

Execution order
===============
Threads may begin execution immediately upon construction of the associated
thread object (pending any OS scheduling delays), starting at the top-level
function provided as a constructor argument. The top-level function may
communicate its return value by modifying shared variables (which may require
synchronization, see :ref:`module-pw_sync`)

Thread objects may also be in the state that does not represent any thread
(after default construction, move from, detach, or join), and a thread of
execution may be not associated with any thread objects (after detach).

No two Thread objects may represent the same thread of execution; Thread is
not CopyConstructible or CopyAssignable, although it is MoveConstructible and
MoveAssignable.

.. list-table::

  * - *Supported on*
    - *Backend module*
  * - FreeRTOS
    - :ref:`module-pw_thread_freertos`
  * - ThreadX
    - :ref:`module-pw_thread_threadx`
  * - embOS
    - :ref:`module-pw_thread_embos`
  * - STL
    - :ref:`module-pw_thread_stl`
  * - Zephyr
    - Planned
  * - CMSIS-RTOS API v2 & RTX5
    - Planned

Module Configuration Options
============================
The following configurations can be adjusted via compile-time configuration of
this module, see the
:ref:`module documentation <module-structure-compile-time-configuration>` for
more details.

.. c:macro:: PW_THREAD_CONFIG_LOG_LEVEL

  The log level to use for this module. Logs below this level are omitted.

Options
=======
The ``pw::thread::Options`` contains the parameters or attributes needed for a
thread to start.

Pigweed does not generalize options, instead we strive to give you full control
where we provide helpers to do this.

Options are backend specific and ergo the generic base class cannot be
directly instantiated.

The attributes which can be set through the options are backend specific
but may contain things like the thread name, priority, scheduling policy,
core/processor affinity, and/or an optional reference to a pre-allocated
Context (the collection of memory allocations needed for a thread to run).

Options shall NOT have an attribute to start threads as detached vs joinable.
All :cc:`pw::Thread` instances must be explicitly ``join()``'d or
``detach()``'d through the run-time Thread API.

Note that if backends set ``PW_THREAD_JOINING_ENABLED`` to false, backends
may use native OS specific APIs to create native detached threads because the
``join()`` API would be compiled out. However, users must still explicitly
invoke ``detach()``.

Options must not contain any memory needed for a thread to run (TCB,
stack, etc.). The Options may be deleted or re-used immediately after
starting a thread.

Please see the thread creation backend documentation for how their Options work.

Portable Thread Creation
========================
Due to the fact that ``pw::thread::Options`` cannot be created in portable code,
some extra work must be done in order to permit portable thread creation.
Namely, a reference to the portable ``pw::thread::Options`` base class interface
must be provided through a header or extern which points to an instantiation in
non-portable code.

This can be most easily done through a facade and set of backends. This approach
can be powerful; enabling multithreaded unit/integration testing which can run
on both the host and on a device with the device's exact thread options.

Alternatively, it can also be be injected at build time by instantiating backend
specific build rule which share the same common portable source file(s) but
select backend specific source files and/or dependencies which provide the
non-portable option instantiations.

As an example, let's say we want to create a thread on the host and on a device
running FreeRTOS. They could use a facade which contains a ``threads.h`` header
with the following contents:

.. code-block:: cpp

   // Contents of my_app/threads.h
   #pragma once

   #include "pw_thread/thread.h"

   namespace my_app {

   const pw::thread::Options& HellowWorldThreadOptions();

   }  // namespace my_app

This could then be backed by two different backend implementations based on
the thread backend. For example for the STL the backend's ``stl_threads.cc``
source file may look something like:

.. code-block:: cpp

   // Contents of my_app/stl_threads.cc
   #include "my_app/threads.h"
   #include "pw_thread_stl/options.h"

   namespace my_app {

   const pw::thread::Options& HelloWorldThreadOptions() {
     static constexpr auto options = pw::thread::stl::Options();
     return options;
   }

   }  // namespace my_app

While for FreeRTOS the backend's ``freertos_threads.cc`` source file may look
something like:

.. code-block:: cpp

   // Contents of my_app/freertos_threads.cc
   #include "FreeRTOS.h"
   #include "my_app/threads.h"
   #include "pw_thread_freertos/context.h"
   #include "pw_thread_freertos/options.h"
   #include "task.h"

   namespace my_app {

   StaticContextWithStack<kHelloWorldStackWords> hello_world_thread_context;
   const pw::thread::Options& HelloWorldThreadOptions() {
     static constexpr auto options =
         pw::thread::freertos::Options()
             .set_name("HelloWorld")
             .set_static_context(hello_world_thread_context)
             .set_priority(kHelloWorldThreadPriority);
     return options;
   }

   }  // namespace my_app

.. _module-pw_thread-detaching-joining:

Detaching & Joining
===================
The ``Thread::detach()`` API is always available, to let you separate the
thread of execution from the thread object, allowing execution to continue
independently.

The joining API, more specifically ``Thread::join()``, is conditionally
available depending on the selected backend for thread creation and how it is
configured. The backend is responsible for providing the
``PW_THREAD_JOINING_ENABLED`` macro through
``pw_thread_backend/thread_native.h``. This ensures that any users which include
``pw_thread/thread.h`` can use this macro if needed.

Please see the selected thread creation backend documentation for how to
enable joining if it's not already enabled by default.

.. Warning::
  A constructed :cc:`pw::Thread` which represents a thread of execution
  must be EITHER detached or joined, else the destructor will assert!

DetachedThread
==============
To make it slightly easier and cleaner to spawn detached threads without having
to worry about thread handles, a wrapper ``DetachedThread()`` function is
provided which creates a ``Thread`` and immediately detaches it. For example
instead of:

.. code-block:: cpp

   Thread(options, foo).detach();

You can instead use this helper wrapper to:

.. code-block:: cpp

   DetachedThread(options, foo);

The arguments are directly forwarded to the Thread constructor and ergo exactly
match the Thread constuctor arguments for creating a thread of execution.

Thread functions
================
Thread functions are provided by a ``pw::Function<void()>`` (which may be a
lambda or function pointer). Provide a no-argument, void-returning lambda or
an equivalent callable:

.. code-block:: cpp

   Thread thread(options, [] {
     // do some work in a thread.
   });

Note that lambdas can capture up to one pointer-sized argument (or more if
dynamic allocation is enabled). This can be used to call methods on existing
objects (though be sure that the objects' lifetime will outlive the thread,
and note that synchronization may be needed).

.. code-block:: cpp

   class Foo {
    public:
     void DoBar() {}
   };
   Foo foo;

   Thread thread(options, [&foo] { foo.DoBar(); });

The legacy ``ThreadCore`` class may also be used to start a thread. This class
was introduced before ``pw::Function`` was available and should not be used in
new code.

.. code-block:: cpp

   class Foo : public ThreadCore {
    private:
     void Run() override { /* thread logic */ }
   };
   Foo foo;

   // Create the thread, using foo directly.
   Thread(options, foo).detach();

``ThreadCore`` may be replaced by capturing a reference to the class.

.. code-block:: cpp

   class Foo {
    public:
     void Run() { /* thread logic */ }
   };
   Foo foo;

   // Create a thread that executes the Foo::Run().
   Thread(options, [&foo] { foo.Run(); }).detach();

.. warning::

   Because the thread may start after the :cc:`pw::Thread` creation, an
   object which implements the ``ThreadCore`` MUST meet or exceed the lifetime
   of its thread of execution!

-------------------------
Unit testing with threads
-------------------------
As an example, the STL :cc:`pw::thread::test::TestThreadContext` backend
implementation in ``test_thread_context_native.h`` is shown below.

.. literalinclude:: ../pw_thread_stl/public/pw_thread_stl/test_thread_context_native.h
   :language: cpp
   :lines: 18-36

----------------
Thread Iteration
----------------
C++
===
.. cpp:function:: Status ForEachThread(const ThreadCallback& cb)

   Calls the provided callback for each thread that has not been joined/deleted.

   This function provides a generalized subset of information that a TCB might
   contain to make it easier to introspect system state. Depending on the RTOS
   and its configuration, some of these fields may not be populated, so it is
   important to check that they have values before attempting to access them.

   **Warning:**  The function may disable the scheduler to perform
   a runtime capture of thread information.

-----------------------
Thread Snapshot Service
-----------------------
``pw_thread`` offers an optional RPC service library
(``:thread_snapshot_service``) that enables thread info capture of
running threads on a device at runtime via RPC. The service will guide
optimization of stack usage by providing an overview of thread information,
including thread name, stack bounds, and peak stack usage.

``ThreadSnapshotService`` currently supports peak stack usage capture for
all running threads (``ThreadSnapshotService::GetPeakStackUsage()``) as well as
for a specific thread, filtering by name
(``ThreadSnapshotService::GetPeakStackUsage(name=b"/* thread name */")``).
Thread information capture relies on the thread iteration facade which will
**momentarily halt your RTOS**, collect information about running threads, and
return this information through the service.

RPC service setup
=================
To expose a ``ThreadSnapshotService`` in your application, do the following:

1. Create an instance of ``pw::thread::proto::ThreadSnapshotServiceBuffer``.
   This template takes the number of expected threads, and uses it to properly
   size buffers required for a ``ThreadSnapshotService``. If no thread count
   argument is provided, this defaults to ``PW_THREAD_MAXIMUM_THREADS``.
2. Register the service with your RPC server.

For example:

.. code-block::

   #include "pw_rpc/server.h"
   #include "pw_thread/thread_snapshot_service.h"

   // Note: You must customize the RPC server setup; see pw_rpc.
   pw::rpc::Channel channels[] = {
    pw::rpc::Channel::Create<1>(&uart_output),
   };
   Server server(channels);

  // Thread snapshot service builder instance.
  pw::thread::proto::ThreadSnapshotServiceBuffer</*num threads*/>
      thread_snapshot_service;

   void RegisterServices() {
     server.RegisterService(thread_snapshot_service);
     // Register other services here.
   }

   void main() {
     // ... system initialization ...

     RegisterServices();

     // ... start your application ...
   }

.. c:macro:: PW_THREAD_MAXIMUM_THREADS

  The max number of threads to use by default for thread snapshot service.

.. cpp:function:: constexpr size_t RequiredServiceBufferSize(const size_t num_threads)

  Function provided through the service to calculate buffer sizing. If no
  argument ``num_threads`` is specified, the function will take ``num_threads``
  to be ``PW_THREAD_MAXIMUM_THREADS``.

.. attention::
    Some platforms may only support limited subsets of this service
    depending on RTOS configuration. **Ensure that your RTOS is configured
    properly before using this service.** Please see the thread iteration
    documentation for your backend for more detail on RTOS support.

-----------------------
pw_snapshot integration
-----------------------
``pw_thread`` provides some light, optional integration with pw_snapshot through
helper functions for populating a :cc:`pw::Thread` proto. Some of these
are directly integrated into the RTOS thread backends to simplify the thread
state capturing for snapshots.

SnapshotStack()
===============
The ``SnapshotStack()`` helper captures stack metadata (stack pointer and
bounds) into a :cc:`pw::Thread` proto. After the stack bounds are
captured, execution is passed off to the thread stack collection callback to
capture a backtrace or stack dump. Note that this function does NOT capture the
thread name: that metadata is only required in cases where a stack overflow or
underflow is detected.

Python processor
================
Threads captured as a Thread proto message can be dumped or further analyzed
using using ``pw_thread``'s Python module. This is directly integrated into
pw_snapshot's processor tool to automatically provide rich thread state dumps.

The ``ThreadSnapshotAnalyzer`` class may also be used directly to identify the
currently running thread and produce symbolized thread dumps.

.. Warning::
  Snapshot integration is a work-in-progress and may see significant API
  changes.

.. toctree::
   :hidden:
   :maxdepth: 1

   backends
