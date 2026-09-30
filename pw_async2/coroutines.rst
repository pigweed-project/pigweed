.. _module-pw_async2-coro:

==========
Coroutines
==========
.. pigweed-module-subpage::
   :name: pw_async2

For projects using C++20, ``pw_async2`` provides first-class support for
coroutines via :cc:`Coro <pw::async2::Coro>`. This allows writing asynchronous
logic in a sequential, synchronous style, eliminating the need to write explicit
state machines. The ``co_await`` keyword is used to suspend execution until an
asynchronous operation is ``Ready``.

See also :ref:`docs-blog-05-coroutines`, a blog post on how Pigweed implements
coroutines without heap allocation, and challenges encountered along the way.

.. _module-pw_async2-coro-tasks:

----------------
Using coroutines
----------------

Define a coroutine
==================
A ``pw_async2`` coroutine is a function with :cc:`CoroContext
<pw::async2::CoroContext>` as its first parameter and a return type of
:cc:`Coro<T> <pw::async2::Coro>`. Here is an example of a coroutine that returns
:cc:`pw::Status`:

.. literalinclude:: examples/basic_coro.cc
   :language: cpp
   :linenos:
   :start-after: [pw_async2-examples-basic-coro]
   :end-before: [pw_async2-examples-basic-coro]

Any :ref:`future <module-pw_async2-futures>` or coroutine can be passed to
``co_await``, which evaluates to a ``value_type`` when the result is ready. To
return from a coroutine, use ``co_return <expression>`` instead of the usual
``return <expression>`` syntax.

.. tip::

   Use :cc:`PW_CO_TRY` and :cc:`PW_CO_TRY_ASSIGN` instead of :cc:`PW_TRY` and
   :cc:`PW_TRY_ASSIGN` when working with :cc:`pw::Status` or :cc:`pw::Result` in
   a coroutine. These macros use ``co_return`` instead of ``return``.

Run a coroutine
===============
Run a coroutine as a ``pw_async2`` :cc:`task <pw::async2::Task>` using
:cc:`Dispatcher::Post`. The following posts a coroutine as a :cc:`FutureTask
<pw::async2::FutureTask>`:

.. literalinclude:: examples/basic_coro.cc
   :language: cpp
   :start-after: [pw_async2-examples-basic-allocated]
   :end-before: [pw_async2-examples-basic-allocated]

The coroutine can also be instantiated directly and passed to ``Post``, though
this requires listing the allocator twice:

.. literalinclude:: examples/basic_coro.cc
   :language: cpp
   :start-after: [pw_async2-examples-basic-allocated-explicit]
   :end-before: [pw_async2-examples-basic-allocated-explicit]

In the examples above, :cc:`Dispatcher::Post` returns ``nullptr`` if either the
initial coroutine frame or the :cc:`FutureTask <pw::async2::FutureTask>` fails
to allocate. However, if a nested coroutine invoked with ``co_await`` fails to
allocate while the coroutine is running (or if an unallocated ``Coro<T>`` is
pended directly), :cc:`Coro::Pend <pw::async2::Coro::Pend>` crashes with
``PW_CRASH``.

To handle coroutine allocation failures gracefully, wrap the coroutine with
:cc:`Coro::MakeFallible <pw::async2::Coro::MakeFallible>`:

.. literalinclude:: examples/basic_coro.cc
   :language: cpp
   :start-after: [pw_async2-examples-basic-allocated-fallible]
   :end-before: [pw_async2-examples-basic-allocated-fallible]

:cc:`MakeFallible <pw::async2::Coro::MakeFallible>` returns a
:cc:`FallibleCoro <pw::async2::FallibleCoro>` that intercepts
allocation failures (both during initial coroutine creation and when invoking
nested coroutines with ``co_await``) and resolves gracefully rather than
crashing:

- **Fallback value**: Pass a fallback value or an error handler function that
  returns a fallback value (e.g.
  ``coro.MakeFallible(Status::ResourceExhausted())`` or
  ``coro.MakeFallible([] { return Status::ResourceExhausted(); })``).
  The resulting future yields ``T`` directly.

  .. literalinclude:: examples/basic_coro.cc
     :language: cpp
     :start-after: [pw_async2-examples-basic-allocated-fallible-direct]
     :end-before: [pw_async2-examples-basic-allocated-fallible-direct]

- **Optional value**: If the handler returns ``void`` (e.g.
  ``coro.MakeFallible([] { PW_LOG_ERROR("Alloc failed"); })``) or no
  arguments are passed (``coro.MakeFallible()``), the resulting future
  yields ``std::optional<T>`` (or ``void`` if ``T`` is ``void``), where
  ``std::nullopt`` indicates an allocation failure.

``MakeFallible`` can also be applied to a nested coroutine inside
``co_await`` (e.g.
``co_await InnerCoro(cx).MakeFallible(Status::ResourceExhausted())``) to
handle a child coroutine's allocation failure locally without aborting the
calling coroutine.

:cc:`FutureTask <pw::async2::FutureTask>` (or the :cc:`CoroTask
<pw::async2::CoroTask>` and :cc:`FallibleCoroTask
<pw::async2::FallibleCoroTask>` aliases in ``pw_async2/coro_task.h``, which
discard the coroutine's return value) can also be stack or statically allocated
instead of dynamically allocated with :cc:`Dispatcher::Post`. This is not
recommended, as it is more complex and does not eliminate all allocations.
Coroutines always dynamically allocate their frames.

.. literalinclude:: examples/basic_coro.cc
   :language: cpp
   :start-after: [pw_async2-examples-basic-future-task]
   :end-before: [pw_async2-examples-basic-future-task]

For more details about Pigweed's coroutine support, see :cc:`Coro
<pw::async2::Coro>`.

------
Memory
------
When using C++20 coroutines, the compiler generates code to save the
coroutine's state (including arguments and local variables) across suspension
points (``co_await``). ``pw_async2`` hooks into this mechanism to control where
this state is stored and to support gracefully handling allocation failures.

A ``pw_async2`` coroutine must accept a :cc:`CoroContext
<pw::async2::CoroContext>` by value as its first argument. :cc:`CoroContext
<pw::async2::CoroContext>` wraps a reference to a :cc:`pw::Allocator`, and this
allocator is used to allocate the coroutine frame. When instantiating a
coroutine, simply pass an allocator as the first argument; ``CoroContext`` is
implicitly constructible from an ``Allocator&``.

If initial coroutine frame allocation fails, the resulting ``Coro`` object is
invalid (``!coro.is_pendable()``) and the coroutine body does not execute. If a
nested coroutine invoked via ``co_await`` fails to allocate while a coroutine is
running, the coroutine aborts and unwinds its active coroutine frames,
destroying any local variables in scope.

Pending an invalid ``Coro`` directly or encountering a nested allocation failure
in :cc:`Coro::Pend <pw::async2::Coro::Pend>` crashes with ``PW_CRASH``. To
handle initial and nested allocation failures without crashing, wrap the
coroutine with
:cc:`MakeFallible <pw::async2::Coro::MakeFallible>`.

.. _module-pw_async2-coro-passing-data:

-------------------------------
Passing data between coroutines
-------------------------------
Coroutines run within ``pw_async2`` tasks and can pass data in all the same
ways. See :ref:`module-pw_async2-channels` for details about passing data with
channels.
