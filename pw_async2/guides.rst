.. _module-pw_async2-guides:

======
Guides
======
.. pigweed-module-subpage::
   :name: pw_async2

This guide covers cross-cutting usage topics such as
:ref:`module-pw_async2-guides-unit-testing`.

---------------------
Core component guides
---------------------
All of the ``pw_async2`` core components have their own dedicated guides.

Tasks
=====
See :ref:`module-pw_async2-tasks`.

Channels
========
See :ref:`module-pw_async2-channels`.

Dispatchers
===========
See :ref:`module-pw_async2-dispatcher`.

Futures
=======
See :ref:`module-pw_async2-futures`.

.. _module-pw_async2-guides-callbacks:

----------------------------------------------
Integrating with existing, callback-based code
----------------------------------------------
See :ref:`module-pw_async2-tasks-callbacks`.

.. _module-pw_async2-guides-interrupts:

-------------------------
Interacting with hardware
-------------------------
A common use case for ``pw_async2`` is interacting with hardware that uses
interrupts. The following example demonstrates this by creating a fake UART
device with an asynchronous reading interface and a separate thread that
simulates hardware interrupts.

The example can be :ref:`built and run in upstream Pigweed <docs-contributing>`
with the following command:

.. code-block:: sh

   bazelisk run //pw_async2/examples:interrupt

``FakeUart`` simulates an interrupt-driven UART with an asynchronous interface
for reading bytes. The ``ReadByte`` method returns a ``ValueFuture`` that
resolves when a byte is available. The ``HandleReceiveInterrupt`` method would
be called from an ISR to resolve pending futures or queue data. (In the example,
this is simulated via keyboard input.)

.. literalinclude:: examples/interrupt.cc
   :language: cpp
   :linenos:
   :start-after: [pw_async2-examples-interrupt-uart]
   :end-before: [pw_async2-examples-interrupt-uart]

A reader task obtains a future from the UART and polls it until it receives
data.

.. literalinclude:: examples/interrupt.cc
   :language: cpp
   :linenos:
   :start-after: [pw_async2-examples-interrupt-reader]
   :end-before: [pw_async2-examples-interrupt-reader]

This example shows how to bridge the gap between low-level, interrupt-driven
hardware and the high-level, cooperative multitasking model of ``pw_async2``.

Full source code for this example: :cs:`pw_async2/examples/interrupt.cc`

.. _module-pw_async2-guides-unit-testing:

------------
Unit testing
------------
Unit testing ``pw_async2`` code is different from testing non-async code. You
must run async code from a :cc:`Task <pw::async2::Task>` on a
:cc:`Dispatcher <pw::async2::Dispatcher>`.

To test ``pw_async2`` code:

#. Add a dependency on ``//pw_async2:testing``.
#. Declare a :cc:`pw::async2::DispatcherForTest`.
#. Create a task to run the async code under test. Either implement
   :cc:`Task <pw::async2::Task>` or use
   :cc:`PendFuncTask <pw::async2::PendFuncTask>` to wrap a lambda.
#. Post the task to the dispatcher.
#. Call :cc:`RunUntilStalled <pw::async2::RunnableDispatcher::RunUntilStalled>`
   to execute the task until it can make no further progress, or
   :cc:`RunToCompletion <pw::async2::RunnableDispatcher::RunToCompletion>` if
   all tasks should complete.

The following example shows the basic structure of a ``pw_async2`` unit test.

.. literalinclude:: examples/unit_test.cc
   :language: c++
   :start-after: pw_async2-minimal-test
   :end-before: pw_async2-minimal-test

It is usually necessary to run the test task multiple times to advance async
code through its states. This improves coverage and ensures that wakers are
stored and woken properly.

To run the test task multiple times:

#. Post the task to the dispatcher.
#. Call :cc:`RunUntilStalled()
   <pw::async2::RunnableDispatcher::RunUntilStalled>`.
#. Perform actions to allow the task to advance.
#. Call :cc:`RunUntilStalled()
   <pw::async2::RunnableDispatcher::RunUntilStalled>` again.
#. Repeat until the task runs to completion, calling :cc:`RunToCompletion()
   <pw::async2::RunnableDispatcher::RunToCompletion>` when the task should
   complete.

The example below runs a task multiple times to test waiting for a
``FortuneTeller`` class to produce a fortune.

.. literalinclude:: examples/unit_test.cc
   :language: c++
   :start-after: pw_async2-multi-step-test
   :end-before: pw_async2-multi-step-test

Full source code for this example: :cs:`pw_async2/examples/unit_test.cc`

.. _module-pw_async2-guides-time:

---------------------------------------------
Interacting with timers, delays, and timeouts
---------------------------------------------
Asynchronous systems often need to interact with time, for example to implement
timeouts, delays, or periodic tasks. ``pw_async2`` provides a flexible and
testable mechanism for this through the :cc:`TimeProvider
<pw::async2::TimeProvider>` interface. ``TimeProvider<SystemClock>`` is
commonly used when interacting with the system's built-in ``time_point`` and
``duration`` types.

:cc:`TimeProvider <pw::async2::TimeProvider>` allows for easily waiting for a
timeout or deadline using the :cc:`WaitFor <pw::async2::TimeProvider::WaitFor>`
and :cc:`WaitUntil <pw::async2::TimeProvider::WaitUntil>` methods.
Additionally, you can test code that uses :cc:`TimeProvider
<pw::async2::TimeProvider>` for timing with simulated time using
:cc:`SimulatedTimeProvider <pw::async2::SimulatedTimeProvider>`. Doing so helps
avoid timing-dependent test flakes and helps ensure that tests are fast since
they don't need to wait for real-world time to elapse.

.. _module-pw_async2-guides-time-and-timers-time-provider:

TimeProvider, timer factory
===========================
The :cc:`TimeProvider <pw::async2::TimeProvider>` is an abstract
interface that acts as a factory for timers. Its key responsibilities are:

* **Providing the current time**: The ``now()`` method returns the current
  time according to a specific clock.
* **Creating timers**: The ``WaitUntil(timestamp)`` and ``WaitFor(delay)``
  methods return a :cc:`TimeFuture <pw::async2::TimeFuture>` object.

This design is friendly to dependency injection. By providing different
implementations of ``TimeProvider``, code that uses timers can be tested with a
simulated clock (like ``pw::chrono::SimulatedClock``), allowing for fast and
deterministic tests without real-world delays. For production code, the
:cc:`GetSystemTimeProvider() <pw::async2::GetSystemTimeProvider>`
function returns a global ``TimeProvider`` that uses the configured system
clock.

.. _module-pw_async2-guides-time-and-timers-time-future:

TimeFuture, time-bound pendable objects
=======================================
A :cc:`TimeFuture <pw::async2::TimeFuture>` is a pendable object that
completes at a specific time. A task can ``Pend`` on a ``TimeFuture`` to
suspend itself until the time designated by the future. When the time is
reached, the ``TimeProvider`` wakes the task, and its next poll of the
``TimeFuture`` will return ``Ready(timestamp)``.

.. _module-pw_async2-guides-time-and-timers-example:

Example
=======
Here is an example of a task that logs a message, sleeps for one second, and
then logs another message.

.. code-block:: cpp

   #include "pw_async2/dispatcher.h"
   #include "pw_async2/system_time_provider.h"
   #include "pw_async2/task.h"
   #include "pw_chrono/system_clock.h"
   #include "pw_log/log.h"

   using namespace std::chrono_literals;

   class LoggingTask : public pw::async2::Task {
    public:
     LoggingTask() : state_(State::kLogFirstMessage) {}

    private:
     enum class State {
       kLogFirstMessage,
       kSleeping,
       kLogSecondMessage,
       kDone,
     };

     Poll<> DoPend(Context& cx) override {
       while (true) {
         switch (state_) {
           case State::kLogFirstMessage:
             PW_LOG_INFO("Hello, async world!");
             future_ = GetSystemTimeProvider().WaitFor(1s);
             state_ = State::kSleeping;
             continue;

           case State::kSleeping:
             if (future_.Pend(cx).IsPending()) {
               return Pending();
             }
             state_ = State::kLogSecondMessage;
             continue;

           case State::kLogSecondMessage:
             PW_LOG_INFO("Goodbye, async world!");
             state_ = State::kDone;
             continue;

           case State::kDone:
             return Ready();
         }
       }
     }

     State state_;
     pw::async2::TimeFuture<pw::chrono::SystemClock> future_;
   };

.. _module-pw_async2-guides-timeouts:

Timing out Futures
==================
See :ref:`module-pw_async2-futures-timeout`.

.. _module-pw_async2-guides-combinators:

-------------------------------------------
Composing async operations with combinators
-------------------------------------------
Combinators allow for the composition of multiple futures:

* :cc:`Join <pw::async2::Join>`: Waits for *all* of a set of futures to
  complete, returning a tuple of their results.
* :cc:`Select <pw::async2::Select>`: Waits for the *first* of a set of
  futures to complete, returning its result.

.. _module-pw_async2-guides-aliases:

------------
Poll aliases
------------
``pw_async2`` provides the following aliases to simplify common return types:

.. csv-table::
   :header: "Alias", "Definition"

   :cc:`PollResult <pw::async2::PollResult>`, ``Poll<pw::Result<T>>``
   :cc:`PollOptional <pw::async2::PollOptional>`, ``Poll<std::optional<T>>``

---------------------
Configuring pw_async2
---------------------
``pw_async2`` offers compile-time configuration options to adapt to different
platforms and constraints.

- :cc:`PW_ASYNC2_LOG_LEVEL` sets the log level for ``pw_async2``.
- :cc:`PW_ASYNC2_DEBUG_WAIT_REASON` controls whether to include debug
  information for blocked tasks.
