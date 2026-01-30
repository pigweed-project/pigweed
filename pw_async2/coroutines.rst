.. _module-pw_async2-coro:

==========
Coroutines
==========
.. pigweed-module-subpage::
   :name: pw_async2

For projects using C++20, ``pw_async2`` provides first-class support for
coroutines via :cc:`Coro <pw::async2::Coro>`. This allows you to write
asynchronous logic in a sequential, synchronous style, eliminating the need to
write explicit state machines. The ``co_await`` keyword is used to suspend
execution until an asynchronous operation is ``Ready``.

.. code-block:: cpp

   Coro<Status> ReadAndSend(Reader& reader, Writer& writer) {
     // co_await suspends the coroutine until the Read operation completes.
     Result<Data> data = co_await reader.Read();
     if (!data.ok()) {
       co_return data.status();
     }

     // The coroutine resumes here and continues.
     co_await writer.Write(*data);
     co_return OkStatus();
   }

See also :ref:`docs-blog-05-coroutines`, a blog post on how Pigweed implements
coroutines without heap allocation, and challenges encountered along the way.

.. _module-pw_async2-coro-tasks:

------------
Define tasks
------------
The following code example demonstrates basic usage:

.. literalinclude:: examples/basic_coro.cc
   :language: cpp
   :linenos:
   :start-after: [pw_async2-examples-basic-coro]
   :end-before: [pw_async2-examples-basic-coro]

Any :ref:`future <module-pw_async2-futures>` can be passed to ``co_await``,
which will return with a ``T`` when the result is ready.

To return from a coroutine, ``co_return <expression>`` must be used instead of
the usual ``return <expression>`` syntax. Because of this, the
:c:macro:`PW_TRY` and :c:macro:`PW_TRY_ASSIGN` macros are not usable within
coroutines. :c:macro:`PW_CO_TRY` and :c:macro:`PW_CO_TRY_ASSIGN` should be
used instead.

For a more detailed explanation of Pigweed's coroutine support, see
:cc:`Coro <pw::async2::Coro>`.

------
Memory
------
When using C++20 coroutines, the compiler generates code to save the
coroutine's state (including local variables) across suspension points
(``co_await``). ``pw_async2`` hooks into this mechanism to control where this
state is stored.

A :cc:`CoroContext <pw::async2::CoroContext>`, which holds a
:cc:`pw::Allocator`, must be passed to any function that
returns a :cc:`Coro <pw::async2::Coro>`. This allocator is used to allocate the
coroutine frame. If allocation fails, the resulting ``Coro`` will be invalid
and will immediately return a ``Ready(Status::Internal())`` result when polled.
This design makes coroutine memory usage explicit and controllable.

.. _module-pw_async2-coro-passing-data:

-------------------------------
Passing data between coroutines
-------------------------------
Just like when :ref:`module-pw_async2-guides-passing-data`, there are two
patterns for sending data between coroutines, with very much the same solutions.

This section just briefly describes how to ``co_await`` the data, as all the
details around construction and sending a value are the same as
:ref:`module-pw_async2-guides-passing-data`.
