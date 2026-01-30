.. _module-pw_async2-futures:

=======
Futures
=======
.. pigweed-module-subpage::
   :name: pw_async2

A ``Future`` is an object that represents the value of an asynchronous operation
which may not yet be complete. Upon completion, the future produces the result
of the operation, if it has one.

Futures are the core interface to ``pw_async2`` asynchronous APIs.

-------------
Core concepts
-------------
Futures operate using the
:ref:`informed poll <module-pw_async2-informed-poll>` model on which
``pw_async2`` is built. This model is summarized below, but it is recommended to
read the full description for important background knowledge.

Future API
==========
Futures use a standard API. There is no `Future` class; futures are unique types
with a common interface, but no shared base. In C++20 and later,
``pw::async2::Future`` is a C++ concept that describes the future interface.

A ``Future<T>`` exposes the following API:

- A default constructor that initializes the future to an empty state. An empty
  future does not represent an asynchronous operation and is neither pendable
  nor complete.
- A destructor that abandons the future so no further operations will access it.
- ``value_type``: Type alias for the value produced by the future.
- ``Poll<value_type> Pend(Context& cx)``: Calling ``Pend`` advances the
  asynchronous operation until no further progress is possible. Returns
  :cc:`Ready <pw::async2::Ready>` if the operation completes. Otherwise, uses
  the provided :cc:`Context <pw::async2::Context>` to store a waker and returns
  :cc:`Pending <pw::async2::Pending>`. The waker wakes the task when ``Pend``
  should be called again.
- ``bool is_pendable()``: Returns whether the future represents an active
  asynchronous operation which can be pended.
- ``bool is_complete()``: Returns whether the future has already completed and
  had its result consumed.

Futures are single-use and track their completion status. It is an error
to poll a future after it has already completed.

``pw_async2`` provides a ``pw::async2::Future`` `concept
<http://go/cppref/cpp/language/constraints.html>`_ that future implementations
must satisfy. Futures do not share a common base class, but may use common
helpers such as :cc:`FutureCore <pw::async2::FutureCore>`.

Ownership and lifetime
======================
Futures are owned by the caller of an asynchronous operation. The task that
receives the future is responsible for storing and polling it.

The provider of a future must either outlive the future or arrange for the
future to be resolved in an error state when the provider is destroyed.

Polling
=======
Futures are lazy and do nothing on their own. The task owning a future must poll
it to drive it to completion. Calling a future's ``Pend`` function advances its
operation and returns a :cc:`Poll <pw::async2::Poll>` containing one of two
values:

* ``Pending()``: The asynchronous operation has not yet finished. The value is
  not available. The task polling the future is be scheduled to wake when the
  future can make additional progress.

  Typically, your task should propagate a ``Pending`` return upwards to notify
  the dispatcher that it is blocked and should sleep.

* ``Ready(T)``: The operation has completed, and the value is now available.

Once a future returns ``Ready``, its state is final. Attempting to poll it again
results in an assertion.

This polling model allows a single thread to manage many concurrent operations
without blocking.

Composability
=============
The power of futures is their ability to compose to construct complex
asynchronous logic from smaller building blocks.

Futures can be classified into two categories: *leaf* futures and *composite*
futures. Leaf futures represent a specific asynchronous operation, such as a
read from a channel, or waiting for a timer. They contain the required state
for their operations and manage the task waiting on them.

Composite futures are built on top of other futures, combining their results
to build advanced asynchronous execution graphs. For example, a ``Join`` future
waits for multiple other futures to complete, returning all of their results at
once. Composite futures can be used to express complex logic in a declarative
way.

Coroutine support
=================
Futures' simple ``Pend`` API makes them easy to use with async2's
:ref:`coroutine adapter <module-pw_async2-coro>`. You can ``co_await`` a
function that returns a future directly, automatically polling the future to
completion.

--------------------
Working with futures
--------------------

Calling functions that return futures
=====================================
Consider some asynchronous call which produces a simple value on completion.
Pigweed provides :cc:`ValueFuture<T> <pw::async2::ValueFuture>` for this common
case. The async function has the following signature:

.. code-block:: c++

   class NumberGenerator {
     ValueFuture<int> GetNextNumber();
   };

You would write a task that calls this operation as follows:

.. tab-set::

   .. tab-item:: Standard polling

      .. code-block:: c++

         class MyTask : public pw::async2::Task {
          private:
           pw::async2::Poll<> DoPend(pw::async2::Context& cx) override {
             // The future begins in a default-constructed state, which is not
             // pendable. Initialize it on the task's first run.
             if (!future_.is_pendable()) {
               future_ = generator_.GetNextNumber();
             }

             PW_TRY_READY_ASSIGN(int number, future_.Pend(cx));
             PW_LOG_INFO("Received number: %d", number);

             return pw::async2::Ready();
           }

           NumberGenerator& generator_;
           ValueFuture<int> future_;
         };

   .. tab-item:: C++20 coroutines

      .. code-block:: c++

         pw::async2::Coro<pw::Status> MyCoroutineFunction(pw::async2::CoroContext&,
                                                          NumberGenerator& generator) {
           // Pigweed's coroutine integration allows futures to be awaited directly.
           int number = co_await generator.GetNextNumber();
           PW_LOG_INFO("Received number: %d", number);
           co_return pw::OkStatus();
         }

Writing functions that return futures
=====================================
All future-based ``pw_async2`` APIs have the signature

.. code-block::

   Future<T> DoThing(Args... args);

Where ``Future<T>`` is some concrete future implementation (e.g.
:cc:`ValueFuture <pw::async2::ValueFuture>`) which resolves to a value of type
``T`` and ``Args`` represents any arguments to the operation.

When defining an asynchronous API, the function should always return a
``Future`` directly --- not a ``Result<Future>`` or
``std::optional<Future>``. If the operation is fallible, that should be
expressed by the future's output, e.g. ``Future<Result<T>>``.

This is necessary for proper composability. It makes using asynchronous APIs
consistent and enables higher-level futures which compose other futures to
function cleanly. Additionally, returning a ``Future`` directly is essential to
be able to work with coroutines: ``co_await`` can be used directly and will
resolve to a ``Result<T>``.

Naming conventions
------------------
Follow these conventions for naming functions that interact with ``pw_async2``
futures.

- Name functions that return futures for the operation represented by the
  future, rather than the future itself.

  .. admonition:: **Yes**: Function is named for the Read operation.
     :class: checkmark

     .. code-block:: cpp

        ReadFuture<T> Read();

  .. admonition:: **No**: Function is named for the future it returns.
     :class: error

     .. code-block:: cpp

        ReadFuture<T> GetReadFuture();

- Do not label future-returning functions as "async". Asynchronicity is implied
  by the future return value.

  .. admonition:: **No**: Future-returning function is named as ``Async``.
     :class: error

     .. code-block:: cpp

        ReadFuture<T> AsyncRead();

- Prefix non-blocking functions with ``Try`` to distinguish then from
  future-returning functions.

  .. admonition:: **Yes**: Non-blocking function starts with ``Try``.
     :class: checkmark

     .. code-block:: cpp

        std::optional<T> TryRead();

- Prefix functions that block the current thread with ``Blocking``.

  .. admonition:: **Yes**: Blocking function starts with ``Blocking``.
     :class: checkmark

     .. code-block:: cpp

        std::optional<T> BlockingRead();

.. _module-pw_async2-futures-implementing:

---------------------
Implementing a future
---------------------
``pw_async2`` provides futures like `ValueFuture <pw::async2::ValueFuture>` for
common asynchronous patterns. However, you may want to implement a custom leaf
future if your operation has complex logic where ``Pend()`` would benefit from
reaching deeper into the underlying system, e.g. waiting for a hardware
interrupt.

:cc:`FutureCore <pw::async2::FutureCore>` is the primary tool for creating
futures.

FutureCore
==========
This class provides the essential machinery for most custom leaf futures:

- It stores the :cc:`Waker <pw::async2::Waker>` of the task that polls it.
- It manages its membership in an intrusive list of futures.
- It tracks future state with a :cc:`FutureState <pw::async2::FutureState>`.

Future implementations typically have a :cc:`FutureCore
<pw::async2::FutureCore>` member.

FutureList
==========
After you vend a future from an asynchronous operation, you need a way to track
and resolve it once the operation has completed. :cc:`FutureCore
<pw::async2::FutureCore>`\s can be stored in a :cc:`FutureList
<pw::async2::FutureList>`, which wraps an :cc:`pw::IntrusiveForwardList`.

:cc:`FutureList <pw::async2::FutureList>` allows multiple concurrent tasks to
wait on an operation. Pending futures are pushed to the list. When an operation
completes, futures are popped from the list and resolved.

:cc:`FutureList <pw::async2::FutureList>` stores its futures as a linked list of
:cc:`FutureCore <pw::async2::FutureCore>`\s in its :cc:`BaseFutureList
<pw::async2::BaseFutureList>` base. This maximizes code reuse between different
future implementations.

A :cc:`FutureList <pw::async2::FutureList>` is declared with a pointer to the
future implementation's :cc:`FutureCore <pw::async2::FutureCore>` member:
``FutureList<&FutureType::future_core_>``. For example:

.. literalinclude:: examples/custom_future.cc
   :language: cpp
   :linenos:
   :start-after: // DOCSTAG: [pw_async2-examples-future-list]
   :end-before: // DOCSTAG: [pw_async2-examples-future-list]

Waking mechanism
================
When a task polls a future and it returns ``Pending``, the future must store the
task's :cc:`Waker <pw::async2::Waker>` from the provided :cc:`Context
<pw::async2::Context>`. This is handled automatically by
:cc:`FutureCore::DoPend <pw::async2::FutureCore::DoPend>`.

On the other side of the asynchronous operation (e.g., in an interrupt handler),
when the operation completes, the provider is used to retrieve the future, and
its ``Wake()`` function is called. This notifies the dispatcher that the task
waiting on this future is ready to make progress and should be polled again.

.. _module-pw_async2-futures-implementing-example:

Example: Waiting for a GPIO interrupt
=====================================
Below is an example of a custom future that waits for a GPIO button press using
interfaces from ``pw_digital_io``.

.. literalinclude:: examples/custom_future.cc
   :language: cpp
   :linenos:
   :start-after: // DOCSTAG: [pw_async2-examples-custom-future]
   :end-before: // DOCSTAG: [pw_async2-examples-custom-future]

This example demonstrates the core mechanics of creating a custom future.  This
pattern of waiting for a single value from a producer is so common that
``pw_async2`` provides :cc:`ValueFuture <pw::async2::ValueFuture>`, which is
produced by a :cc:`ValueProvider <pw::async2::ValueProvider>` or
:cc:`OptionalValueProvider <pw::async2::OptionalValueProvider>`, to handle it.
In practice, you would return a :cc:`VoidFuture <pw::async2::VoidFuture>` (alias
for ``ValueFuture<void>``) from ``WaitForPress`` instead of writing a custom
``ButtonFuture``.

.. _module-pw_async2-futures-combinators:

-----------
Combinators
-----------
Combinators allow you to compose multiple futures into a single future to express
complex control flow.

Join
====
:cc:`Join` waits for multiple futures to complete and returns a tuple of their
results.

.. code-block:: cpp

   #include "pw_async2/join.h"

   ValueFuture<pw::Status> DoWork(int id);

.. tab-set::

   .. tab-item:: Standard polling

      .. code-block:: cpp

         class JoinTask : public pw::async2::Task {
          private:
           pw::async2::Poll<> DoPend(pw::async2::Context& cx) override {
             if (!future_.is_pendable()) {
               // Start three futures concurrently and wait for all of them
               // to complete.
               future_ = pw::async2::Join(DoWork(1), DoWork(2), DoWork(3));
             }

             PW_TRY_READY_ASSIGN(auto results, future_.Pend(cx));
             auto [status1, status2, status3] = *results;

             if (!status1.ok() || !status2.ok() || !status3.ok()) {
               PW_LOG_ERROR("Operation failed");
             } else {
               PW_LOG_INFO("All operations succeeded");
             }

             return pw::async2::Ready();
           }

           JoinFuture<ValueFuture<pw::Status>,
                      ValueFuture<pw::Status>,
                      ValueFuture<pw::Status>>
               future_;
         };

   .. tab-item:: C++20 coroutines

      .. code-block:: cpp

         pw::async2::Coro<pw::Status> JoinExample(pw::async2::CoroContext&) {
           // Start three futures concurrently and wait for all of them to complete.
           auto [status1, status2, status3] =
               co_await pw::async2::Join(DoWork(1), DoWork(2), DoWork(3));

           if (!status1.ok() || !status2.ok() || !status3.ok()) {
             PW_LOG_ERROR("Operation failed");
             co_return pw::Status::Internal();
           }
           PW_LOG_INFO("All operations succeeded");
           co_return pw::OkStatus();
         }

Select
======
:cc:`Select` waits for the *first* of multiple futures to complete. It returns a
:cc:`SelectFuture` which resolves to an :cc:`OptionalTuple` containing the
result. If additional futures happen to complete between the first future
completing the task re-running, the tuple stores all of their results.

.. code-block:: cpp

   #include "pw_async2/select.h"

   ValueFuture<int> DoWork();
   ValueFuture<int> DoOtherWork();

.. tab-set::

   .. tab-item:: Standard polling

      .. code-block:: cpp

         class SelectTask : public pw::async2::Task {
          private:
           pw::async2::Poll<> DoPend(pw::async2::Context& cx) override {
             if (!future_.is_pendable()) {
               // Race two futures and wait for the first one to complete.
               future_ = pw::async2::Select(DoWork(), DoOtherWork());
             }

             PW_TRY_READY_ASSIGN(auto results, future_.Pend(cx));

             // Check which future(s) completed.
             // In this example, we check all of them, but it's common to return
             // after the first result.
             if (results.has_value<0>()) {
               PW_LOG_INFO("DoWork completed with: %d", results.get<0>());
             }
             if (results.has_value<1>()) {
               PW_LOG_INFO("DoOtherWork completed with: %d", results.get<1>());
             }

             return pw::async2::Ready();
           }

           SelectFuture<ValueFuture<int>, ValueFuture<int>> future_;
         };

   .. tab-item:: C++20 coroutines

      .. code-block:: cpp

         pw::async2::Coro<int> SelectExample(pw::async2::CoroContext&) {
           // Race two futures and wait for the first one to complete.
           auto results = co_await pw::async2::Select(DoWork(), DoOtherWork());

           // Check which future(s) completed.
           // In this example, we check all of them, but it's common to return
           // after the first result.
           if (results.has_value<0>()) {
             int result = results.get<0>();
             PW_LOG_INFO("DoWork completed with: %d", result);
           }

           if (results.has_value<1>()) {
             int result = results.get<1>();
             PW_LOG_INFO("DoOtherWork completed with: %d", result);
           }

           co_return pw::OkStatus();
         }

.. _module-pw_async2-guides-primitives-wakers:

Setting up wakers
=================
Futures typically store a waker. When the future is ready to advance, that wake
the task that pended them with this waker. Wakers can be set using one of these
four macros:

- :cc:`PW_ASYNC_STORE_WAKER` and :cc:`PW_ASYNC_CLONE_WAKER`

  The first creates a waker for a given context. The second clones an
  existing waker, allowing the original and/or the clone to wake the task.

  This pair of macros ensure a single task will be woken. They will assert if
  a waker for a different task is created (or cloned) when the destination
  waker already is set up for some task.

- :cc:`PW_ASYNC_TRY_STORE_WAKER` and :cc:`PW_ASYNC_TRY_CLONE_WAKER`

  These are alternatives to :cc:`PW_ASYNC_STORE_WAKER` and
  cc:`PW_ASYNC_CLONE_WAKER` that return ``false`` instead of crashing if the
  waker is already set. This allows the caller to handle cases when the waker is
  already in use.

.. _module-pw_async2-futures-timeout:

------------------
Timing-out Futures
------------------
If you create a :ref:`future <module-pw_async2-futures>`, you can also combine
it with a :cc:`TimeFuture <pw::async2::TimeFuture>` to get a new composite
future (a :cc:`FutureWithTimeout <pw::async2::FutureWithTimeout>`) that can
time out.

There are three main factory functions that construct useful variants of the
composite type.

- ``Timeout(future, [time_provider,] delay)``

  This function returns a composite future that times out after the specified
  delay. If no time provider is given, the function will default to
  :cc:`GetSystemTimeProvider <pw::async2::GetSystemTimeProvider>`. The time
  provider will then be used to construct the
  :cc:`TimeFuture <pw::async2::TimeFuture>` to use in the composite future.

  If the original value future is for a value of type `T`, the created
  composite future uses :cs:`pw::Result<T><pw::Result>`. On timeout, the status
  associated with that result will be ``Status.DeadlineExceeded()`` to make it
  clear no value is available.

  The composite future will handle waiting on both futures, and will prefer to
  resolve to the value provided by the first future if both futures are ready
  when they are next pended.

- ``TimeoutOr(future, [time_provider,] delay, sentinel_value_or_func)``

  Like the first function, this function will construct a composite future that
  will time out after the specified delay.

  However on timeout, this version will resolve to a sentinel value, either
  using a value passed in, or calling a function to obtain it, if that is what
  is passed in.

  Note that a copy of the value that is passed will be stored as part of the
  internal data for a future. For a small and trivially constructible type,
  this makes sense, but for a large type or a type that is not trivially
  constructible you should prefer to pass a function which constructs the
  value.

  .. caution::

    You should only use a sentinel when it there is no chance of confusing the
    sentinel value with the normal values you would obtain from the future when
    it does not time out.

- ``TimeoutOrClosed(channel_future, [time_provider], delay)``

  Like the first function, but intended to be used with the
  :cc:`SendFuture <pw::async2::SendFuture>`,
  :cc:`ReceiveFuture <pw::async2::ReceiveFuture>`, and
  :cc:`ReserveSendFuture <pw::async2::ReserveSendFuture>` futures returned from
  using a channel.

  On timeout, these act like the channel was closed while waiting, and release
  their reference to the channel. For ``SendFuture``, this means it resolves to
  false, and for the other two it means resolving to ``std::nullopt``.

Example
=======
Using them to construct the composite future is easy.

.. code-block:: cpp

   // Obtain a basic ValueFuture<T> or similar from some provider.
   auto value_future = value_provider.Get()
                       // Construct the composite future, which will either resolve
                       // to a `T`, or timeout after 15ms using the system clock.
                       auto future_with_timeout_ex1 =
       Timeout(std::move(value_future), 15ms);

   // You can also construct one this way.
   auto future_with_timeout_ex2 = Timeout(value_provider.Get(), 15ms);

   // For a sentinel with a simple constant:
   auto future_with_timeout_ex3 = TimeoutOr(int_value_provider.Get(), 15ms, -1);

   // To use a function to obtain a sentinnel value:
   auto future_with_timeout_ex3 =
       TimeoutOr(int_value_provider.Get(), 15ms, []() { return -1; });

You can find more examples showing how to use these functions in
:cs:`pw_async2/examples/timeout_test.cc`.
