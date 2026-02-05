.. _module-pw_async2-tasks:

=====
Tasks
=====
.. pigweed-module-subpage::
   :name: pw_async2

Tasks are the top-level "thread" primitives of ``pw_async2``. This guide
provides detailed usage information about the :cc:`Task <pw::async2::Task>` API
of ``pw_async2``.

.. _module-pw_async2-tasks-background:

----------
Background
----------
For a detailed conceptual explanation of tasks, see
:ref:`module-pw_async2-informed-poll`. For a hands-on introduction to using
tasks, see :ref:`module-pw_async2-codelab`.

--------------------
Task implementations
--------------------
Tasks are created by deriving from :cc:`pw::async2::Task`. See
:ref:`module-pw_async2-guides-implementing-tasks` for details.

Pigweed also provides several ``Task`` implementations for specific scenarios.
Use these with caution. Systems should be built around composable tasks. Avoid
using tasks as callback-style constructions. To realize the full benefits of
``pw_async2``, implement your system's logic within ``pw_async2`` tasks that
interact with one another through async primitives like :ref:`futures
<module-pw_async2-futures>` and :ref:`channels <module-pw_async2-channels>`.

``FuncTask``: Use a function as a task
======================================
:cc:`FuncTask <pw::async2::FuncTask>` is a convenient way to write a simple
task. ``FuncTask`` implements :cc:`Poll<> Task::DoPend(Context&)
<pw::async2::Task::DoPend>` with a function or lambda. Like any other task,
``FuncTask`` task supports suspending and resuming and must return either
``Pending()`` or ``Ready()``.

.. literalinclude:: examples/task_helpers.cc
   :language: cpp
   :start-after: // DOCSTAG: [pw_async2-examples-func-task]
   :end-before: // DOCSTAG: [pw_async2-examples-func-task]

:cc:`FuncTask <pw::async2::FuncTask>` can create tasks that run a class member
function in a task. This is helpful when the class spawns multiple
tasks, so cannot inherit from :cc:`Task <pw::async2::Task>` itself.

.. literalinclude:: examples/task_helpers.cc
   :language: cpp
   :start-after: // DOCSTAG: [pw_async2-examples-func-task-class]
   :end-before: // DOCSTAG: [pw_async2-examples-func-task-class]

.. admonition:: Do not overuse
   :class: warning

   Tasks usually need to store futures that represent ongoing operations, and a
   :cc:`FuncTask <pw::async2::FuncTask>` that wraps a lambda function is not
   well suited for that. If a ``FuncTask`` must store futures or other data,
   :ref:`convert it to a regular task
   <module-pw_async2-guides-implementing-tasks>`.

``FutureTask``: Run a future in a task
======================================
:cc:`FutureTask <pw::async2::FutureTask>` is a task that runs a single future to
completion in a task. The value produced by the future is accessible by calling
:cc:`Wait() <pw::async2::FutureTask::Wait>` or, if the task has been :cc:`joined
<pw::async2::Task::Join>`, :cc:`value() <pw::async2::FutureTask::value>`.
``FutureTask`` can take ownership of a future:

.. literalinclude:: examples/task_helpers.cc
   :language: cpp
   :dedent:
   :start-after: // DOCSTAG: [pw_async2-examples-future-task-owned]
   :end-before: // DOCSTAG: [pw_async2-examples-future-task-owned]

or use a reference to a future:

.. literalinclude:: examples/task_helpers.cc
   :language: cpp
   :start-after: // DOCSTAG: [pw_async2-examples-future-task-ref]
   :end-before: // DOCSTAG: [pw_async2-examples-future-task-ref]

.. admonition:: Do not overuse
   :class: warning

   :cc:`FutureTask <pw::async2::FutureTask>` is intended for test and occasional
   production use. A ``FutureTask`` does not contain logic, and relying too much
   on ``FutureTasks`` could push logic out of async tasks, which nullifies the
   benefits of ``pw_async2``. Creating a task for each future is also less
   efficient than having one task work with multiple futures.

``CallbackTask``: Bridge between sync and async
===============================================
:cc:`CallbackTask <pw::async2::CallbackTask>` invokes a callback after a future
is ready. This can be used to bridge between async and non-async code. and See
:ref:`module-pw_async2-tasks-callbacks` for details.

``CoroOrElseTask``: Execute a coroutine
=======================================
:cc:`CoroOrElseTask <pw::async2::CoroOrElseTask>` delegates to a
provided coroutine and executes an ``or_else`` handler function on failure.
See :ref:`module-pw_async2-coro` for information about coroutines.

.. _module-pw_async2-tasks-callbacks:

---------
Callbacks
---------
In a system gradually or partially adopting ``pw_async2``, there are often
cases where existing code needs to run asynchronous operations built with
``pw_async2``.  To facilitate this, ``pw_async2`` provides
:cc:`CallbackTask <pw::async2::CallbackTask>`. This task invokes a :ref:`future
<module-pw_async2-futures>`, forwarding its result to a provided callback on
completion.

.. _module-pw_async2-tasks-callbacks-example:

Example
=======
.. code-block:: cpp

   #include "pw_async2/callback_task.h"
   #include "pw_log/log.h"
   #include "pw_result/result.h"

   // Assume the async2 part of the system exposes a function to post tasks to
   // its dispatcher.
   void PostTaskToDispatcher(pw::async2::Task& task);

   // The async2 function we'd like to call.
   ValueFuture<pw::Result<int>> ReadValue();

   // Non-async2 code.
   void ReadAndPrintAsyncValue() {
     pw::async2::CallbackTask task(
         [](pw::Result<int> result) {
           if (result.ok()) {
             PW_LOG_INFO("Read value: %d", result.value());
           } else {
             PW_LOG_ERROR("Failed to read value: %s", result.status().str());
           }
         },
         ReadValue());

     PostTaskToDispatcher(task);

     // In this example, the code allocates the task on the stack, so we would
     // need to wait for it to complete before it goes out of scope. In a real
     // application, the task may be a member of a long-lived object, or you
     // might choose to statically allocate it.
   }

.. _module-pw_async2-tasks-callbacks-considerations:

Considerations for callback-based integration
=============================================
While the ``CallbackTask`` helper is convenient, each instance of it is a
distinct ``Task`` in the system which will compete for execution with other
tasks running on the dispatcher.

If an asynchronous part of the system needs to expose a robust, primary API
based on callbacks to non-``pw_async2`` code, a more integrated solution is
recommended. Instead of using standalone ``CallbackTask`` objects, the ``Task``
that manages the operation should natively support registering and managing a
list of callbacks. This provides a clearer and more efficient interface for
external consumers.

.. _module-pw_async2-tasks-memory:

------
Memory
------
The memory for a ``Task`` object itself is managed by the user. This provides
flexibility in how tasks are allocated and stored. Common patterns include:

* **Static or Member Storage**: For tasks that live for the duration of the
  application or are part of a long-lived object, they can be allocated
  statically or as class members. This is the most common and memory-safe
  approach. The user must ensure the ``Task`` object is not destroyed while it
  is still registered with a ``Dispatcher``. Calling
  :cc:`Task::Deregister() <pw::async2::Task::Deregister>` before
  destruction guarantees safety.

* **Dynamic Allocation**: For tasks with a dynamic lifetime, ``pw_async2``
  provides the :cc:`AllocateTask() <pw::async2::AllocateTask>` helper. See
  :ref:`module-pw_async2-tasks-memory-alloc`.

.. _module-pw_async2-tasks-memory-alloc:

Dynamically allocating tasks
============================
:cc:`AllocateTask() <pw::async2::AllocateTask>` creates a concrete subclass of
``Task``,  but the created task is dynamically allocated using a provided
:cc:`pw::Allocator`. Upon completion the associated memory is automatically
freed by calling the allocator's ``Delete`` method.  This simplifies memory
management for "fire-and-forget" tasks.

.. code-block:: cpp

   // This task will be deallocated from the provided allocator when it's done.
   Task* task = AllocateTask<MyFunction>(my_allocator, arg1, arg2);
   dispatcher.Post(*task);

.. _module-pw_async2-guides-implementing-tasks:

------------------
Implementing tasks
------------------
The following sections provide more guidance about subclassing ``Task``
yourself.

.. _module-pw_async2-guides-implementing-tasks-pend:

Pend() and DoPend(), the core interfaces
========================================
A :ref:`dispatcher <module-pw_async2-dispatcher>` drives a task forward by
invoking the task's :cc:`Pend() <pw::async2::Task::Pend>` method. ``Pend()`` is
a non-virtual wrapper around the task's :cc:`DoPend()
<pw::async2::Task::DoPend>` method. ``DoPend()`` is where the core logic of a
task should be implemented.

.. _module-pw_async2-guides-implementing-tasks-pend-state:

Communicating completion state
------------------------------
When a task is incomplete but can't make any more progress, its ``DoPend()``
method should return :cc:`Pending <pw::async2::Pending>`. The dispatcher will
sleep the task.

When a task has completed all work, it should return :cc:`Ready
<pw::async2::Ready>`.

.. note::

   How does a task wake back up? Tasks are not directly involved in this
   process. The task invokes one or more asynchronous operations that return
   :ref:`futures <module-pw_async2-futures>`, which are values that may not be
   complete yet. When invoking the async operation, the task provides its
   :cc:`context <pw::async2::Context>`. The future grabs a :cc:`waker
   <pw::async2::Waker>` from the context. When the future's value is ready, the
   asynchronous operation invokes the waker to inform the dispatcher that the
   task can make more progress. See
   :ref:`module-pw_async2-informed-poll-components-future` and
   :ref:`module-pw_async2-informed-poll-components-waker` for further
   explanation.

.. _module-pw_async2-guides-implementing-tasks-pend-cleanup:

Cleaning up complete tasks
--------------------------
The behavior of a task after returning ``Ready`` is implementation-specific.
For a one-shot operation, it may be an error to poll it again. For a
stream-like operation (e.g. reading from a channel), polling again after a
``Ready`` result is the way to receive the next value. This behavior should be
clearly documented.

.. _module-pw_async2-guides-passing-data:

--------------------------
Passing data between tasks
--------------------------
See :ref:`module-pw_async2-channels`.

.. _module-pw_async2-guides-debugging:

---------
Debugging
---------
You can inspect tasks registered to a dispatcher by calling
::cc:`Dispatcher::LogRegisteredTasks()
<pw::async2::Dispatcher::LogRegisteredTasks>`, which logs information for each
task in the dispatcher's pending and sleeping queues.

Sleeping tasks will log information about their assigned wakers, with the
wait reason provided for each.

If space is a concern, you can set the module configuration option
:cc:`PW_ASYNC2_DEBUG_WAIT_REASON` to ``0`` to disable wait reason storage
and logging. Under this configuration, the dispatcher only logs the waker count
of a sleeping task.
