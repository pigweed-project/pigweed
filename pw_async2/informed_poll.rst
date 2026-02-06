.. _module-pw_async2-informed-poll:

=============
Informed poll
=============
.. pigweed-module-subpage::
   :name: pw_async2

The *informed poll* programming model is the core design philosophy behind
``pw_async2``. Informed poll is an alternative to callback-based asynchronous
programming that simplifies state management for complex concurrent operations.

This overview helps you build a mental model of how informed poll works.
It's easier to build robust and correct asynchronous systems with
``pw_async2`` when you've got a thorough understanding of the informed poll
programming model.

.. _module-pw_async2-informed-poll-summary:

-------
Summary
-------
The central idea is that asynchronous work is encapsulated in :cc:`Tasks
<pw::async2::Task>`, which are similar to `green threads`_. Instead of
registering callbacks for different events, a central :cc:`Dispatcher
<pw::async2::Dispatcher>` *polls* tasks to see if they can make progress.
Tasks drive one or more asynchronous operations to completion. The async
operations communicate whether or not their values are ready via :ref:`futures
<module-pw_async2-futures>`, the basic async primitive in ``pw_async2``.  When
none of the future values are ready, the task notifies the dispatcher that it
can't progress, and the dispatcher sleeps the task. When a future's value
becomes ready, the future uses a :cc:`Waker <pw::async2::Waker>` to *inform* the
dispatcher that the task associated with the :cc:`Waker <pw::async2::Waker>`
can make more progress and therefore should be polled again.

.. mermaid::

   sequenceDiagram

   participant d as Dispatcher
   participant t as Task
   participant o as Async Operation
   d->>t: Run task
   t->>o: Start async operation
   o->>t: Vend a future
   t->>t: Poll the future, not ready yet
   t->>d: Sleep
   o->>o: Future is ready
   o->>d: Wake the task
   d->>t: Run task
   t->>t: Consume the future's value

.. _module-pw_async2-informed-poll-components:

---------------
Core components
---------------
This section provides more explanation of the core components of the
``pw_async2`` framework: :ref:`dispatchers
<module-pw_async2-informed-poll-components-dispatcher>`, :ref:`tasks
<module-pw_async2-informed-poll-components-task>`, :ref:`futures
<module-pw_async2-informed-poll-components-future>`, and :ref:`wakers
<module-pw_async2-informed-poll-components-waker>`.

The following diagram summarizes how asynchronous work generally progresses
in a ``pw_async2`` system.

.. mermaid::

   stateDiagram-v2
   direction LR

   Dispatcher --> Task
   Task --> Future
   Future --> Waker
   Waker --> Dispatcher

.. _module-pw_async2-informed-poll-components-dispatcher:

Dispatcher: The cooperative scheduler
=====================================
The dispatcher maintains a queue of tasks that are ready to be polled. The
dispatcher runs whatever task has been ready the longest. There is
no concept of task `priority`_. The dispatcher drives a task forward by
calling the task's :cc:`Pend() <pw::async2::Task::Pend>` method, which is a
non-virtual wrapper around :cc:`DoPend() <pw::async2::Task::DoPend>`. The core
logic of a task is implemented in its ``DoPend()`` method.

.. note::

   ``Pend()`` is also the core interface for futures and coroutines.

When the dispatcher is informed that a task can't make any more progress,
the dispatcher removes the task from its ready queue and places the task in its
sleep queue. When the dispatcher is informed that a sleeping task can make more
progress, it places the task back into its ready queue.

.. _module-pw_async2-informed-poll-components-task:

Tasks: Logical collections of async work
========================================
:cc:`Tasks <pw::async2::Task>` are similar to `green threads`_, i.e. threads
that are `cooperatively scheduled`_ by a runtime library, not `preemptively
scheduled`_ by an underlying OS. Tasks usually represent logical collections of
work. For example, in the :ref:`pw_async2 codelab <module-pw_async2-codelab>`,
where you write firmware for a vending machine, one task handles user input
(coin insertions and item selection) while another task manages the item
dispenser machinery (controlling the motors to dispense an item and detecting
when an item has dropped).

.. _module-pw_async2-informed-poll-components-task-state:

Communicating task state to the dispatcher
------------------------------------------
A task communicates to the dispatcher what state it's in by returning one of
these values in its ``Pend()`` implementation:

* :cc:`Ready() <pw::async2::Ready>`: The task has finished its work. The
  ``Dispatcher`` should not poll it again.
* :cc:`Pending() <pw::async2::Pending>`: The task is not yet finished
  because it is waiting for an external event. E.g. it's waiting for a timer to
  finish or for data to arrive. The dispatcher should sleep the task and
  then run it again later.

.. _module-pw_async2-informed-poll-components-future:

Futures: The basic async primitive
==================================
Tasks invoke asynchronous operations that return :ref:`futures
<module-pw_async2-futures>`, which are values that may not be ready yet. In the
vending machine example mentioned in
:ref:`module-pw_async2-informed-poll-components-task`, coin insertions, item
selections, motor control, and item drop detection are all examples of async
operations.

Like tasks, futures use ``Ready()`` and ``Pending()`` to communicate whether
they're complete or not. The main difference is that futures can return a
value.

A task's primary role is often to poll multiple futures to completion and
coordinate the values that it's receiving from the futures. Futures are always
composable, which makes it easier for the task to manage complex asynchronous
logic coming from many different async operations.

Futures are always owned by a parent task. The task is responsible for holding
the state of the futures it owns and polling them all to completion, or
canceling them.

.. _module-pw_async2-informed-poll-components-waker:

Wakers: Progress updates for the dispatcher
===========================================
When a task signals to the dispatcher that it can't make any more progress, the
task must ensure that something will eventually trigger it to be run again.
This is accomplished via :cc:`Wakers <pw::async2::Waker>`. When a future's
value is ready, the future signals a waker to inform the dispatcher that its
parent task can make more progress. This mechanism prevents the ``Dispatcher``
from having to wastefully poll tasks that aren't ready, allowing the task to
sleep and save power when no work can be done.

Wakers are an important concept in the informed poll model, but in your code
they are often an implementation detail that you usually don't need to think
about. Pigweed-provided futures like :cc:`ValueFuture
<pw::async2::ValueFuture>` automatically store and invoke wakers on your
behalf.

------------------------------------------------
Comparison with traditional callback-based async
------------------------------------------------
The typical way to structure asynchronous operations in system code is through
callbacks invoked when the operation completes. While familiar, this approach
leads to issues in several areas:

- State management: callers often need to invoke a series of async operations
  in order where callbacks trigger other callbacks, requiring complicated
  context management.
- Ownership and lifetimes: it can be unclear who owns the data passed to a
  callback and how long it is valid.
- Execution context: the callback could run on a different thread or in an ISR,
  making certain operations unsafe to perform.
- Debugging: stack traces don't include much useful information, and it can be
  difficult to determine who originally started the operation.

``pw_async2`` addresses these issues by promoting:

- Encapsulating state: Tasks and futures are objects that hold their own state.
  There is no need to manually pack context into ``void*`` arguments.
- Explicit ownership: The task that owns the future manages its lifetime. If the
  task is destroyed, its futures are also destroyed, preventing use-after-free
  and similar lifetime bugs.
- Predictable execution: All tasks run on the dispatcher they are posted to, so
  you know exactly which thread and stack is executing your code.
- Inspectability: Since tasks are objects, they can be inspected in a debugger
  or by ``pw_async2`` tooling to see exactly what they are waiting for.

.. _module-pw_async2-informed-poll-rust:

---------------------------------------
Comparison with Rust's informed polling
---------------------------------------
Informed polling was first proposed for Pigweed in :ref:`SEED 0112 <seed-0112>`.
SEED 0112 was inspired by Rust's informed polling model.

Informed polling in Rust is built around its `Future`_ trait. ``pw_async2``
futures are conceptually very similar.

A key difference is that in Rust, `async` functions and blocks automatically
generate state machines that implement the `Future` trait. In ``pw_async2``,
you can achieve a similar ergonomic benefit by using
:ref:`coroutines <module-pw_async2-coro>`, which allow you to ``co_await``
futures. Without coroutines, you manually manage the state of futures within a
:cc:`Task <pw::async2::Task>`.

As a result of this, Pigweed's ``Future`` concept includes states in its core
API---empty, pendable, and complete---to make them easier to work with manually
when writing a chain of async operations within a ``Task``. An empty future is
uninitialized and does not represent a pending operation, indicating that the
task should start one. Similarly, a completed future signals that the operation
has completed and the task should move on to its next step.

---------
Evolution
---------
Since the proposal in :ref:`SEED 0112 <seed-0112>`, the design of ``pw_async2``
has evolved in several ways. The most significant change was the formalization
of futures as the standardized interface for asynchronous operations, replacing
ad-hoc "pendable functions" as originally proposed.

"Pendable functions" were simply any C++ function that returned a ``Poll<T>``.
While low-overhead and flexible, this design lacked a standard way to manage the
lifetime and ownership of an asynchronous operation's state and ambiguity about
who could run the operation and when. The lack of a standardized interface also
made these functions difficult to compose.

.. _Future: https://doc.rust-lang.org/std/future/trait.Future.html
.. _Stream: https://docs.rs/futures/latest/futures/prelude/trait.Stream.html
.. _green thread: https://en.wikipedia.org/wiki/Green_thread
.. _green threads: https://en.wikipedia.org/wiki/Green_thread
.. _cooperative scheduler: https://en.wikipedia.org/wiki/Cooperative_multitasking
.. _cooperatively scheduled: https://en.wikipedia.org/wiki/Cooperative_multitasking
.. _preemptively scheduled: https://en.wikipedia.org/wiki/Preemption_(computing)#Preemptive_multitasking
.. _priority: https://en.wikipedia.org/wiki/Priority_queue
