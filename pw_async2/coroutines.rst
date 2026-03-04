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

   Coro<void> ReadAndSend(Reader& reader, Writer& writer) {
     // co_await suspends the coroutine until the Read operation completes.
     Result<Data> data = co_await reader.Read();
     if (!data.ok()) {
       co_return;
     }

     // The coroutine resumes here and continues.
     co_await writer.Write(*data);
     co_return;
   }

See also :ref:`docs-blog-05-coroutines`, a blog post on how Pigweed implements
coroutines without heap allocation, and challenges encountered along the way.

.. _module-pw_async2-coro-tasks:

------------------
Define a coroutine
------------------
The following example shows how to define a coroutine:

.. literalinclude:: examples/basic_coro.cc
   :language: cpp
   :linenos:
   :start-after: [pw_async2-examples-basic-coro]
   :end-before: [pw_async2-examples-basic-coro]

Any :ref:`future <module-pw_async2-futures>` or coroutine can be passed to
``co_await``, which will return with a ``T`` when the result is ready. To return
from a coroutine, use ``co_return <expression>`` instead of the usual ``return
<expression>`` syntax.

.. tip::

   Use :cc:`PW_CO_TRY` and :cc:`PW_CO_TRY_ASSIGN` instead of :cc:`PW_TRY` and
   :cc:`PW_TRY_ASSIGN` when working with :cc:`pw::Status` or :cc:`pw::Result` in
   a coroutine. These macros use ``co_return`` instead of ``return``.

Run a coroutine as a ``pw_async2`` :cc:`task <pw::async2::Task>` using
:cc:`CoroTask <pw::async2::CoroTask>` or :cc:`FallibleCoroTask
<pw::async2::FallibleCoroTask>`.

.. literalinclude:: examples/basic_coro.cc
   :language: cpp
   :start-after: [pw_async2-examples-basic-coro-task]
   :end-before: [pw_async2-examples-basic-coro-task]

For a more details about Pigweed's coroutine support, see :cc:`Coro
<pw::async2::Coro>`.

------
Memory
------
When using C++20 coroutines, the compiler generates code to save the
coroutine's state (including local variables) across suspension points
(``co_await``). ``pw_async2`` hooks into this mechanism to control where this
state is stored and support gracefully handling allocation failures.

A :cc:`CoroContext <pw::async2::CoroContext>`, which has a reference to a
:cc:`pw::Allocator`, must be passed to any function that returns a :cc:`Coro
<pw::async2::Coro>`. Its allocator is used to allocate the coroutine frame.

If allocation fails, the resulting ``Coro`` object is invalid. Coroutine
execution halts, and what happens next depends on the task executing the
coroutine. :cc:`CoroTask <pw::async2::CoroTask>` crashes with ``PW_CRASH`` on
allocation failure. :cc:`FallibleCoroTask <pw::async2::FallibleCoroTask>`
invokes an error handler function instead.

.. _module-pw_async2-coro-passing-data:

-------------------------------
Passing data between coroutines
-------------------------------
Coroutines run within ``pw_async2`` tasks and can pass data in all the same
ways. See :ref:`module-pw_async2-channels` for details about passing data with
channels.
