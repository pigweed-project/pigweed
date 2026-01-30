.. _module-pw_async2-size-reports:

==================
Code size analysis
==================
.. pigweed-module-subpage::
   :name: pw_async2

--------------------------
Core async2 implementation
--------------------------
The following table shows the code size cost of adding ``pw_async2`` to a
system. These size reports assume a baseline system with an RTOS which already
uses a handful of core Pigweed components including HAL abstractions and
``pw_allocator.``

The first row captures the core of ``pw_async2``: the dispatcher, tasks, and
wakers, using the :cc:`pw::async2::BasicDispatcher`. This is the minimum size
cost a system must pay to adopt ``pw_async2``. The following row demonstrates
the cost of adding another task to this system. Of course, the majority of the
cost of the task exists within its implementation --- this simply shows that
there is minimal internal overhead.

.. include:: size_report/full_size_report

-------
Futures
-------
:ref:`Futures <module-pw_async2-futures>` are the core abstraction in
``pw_async2``, providing a standardized way of polling an asynchronous
operation to completion.

The design of futures has some implications for code size:

* All futures are templated on the type of value they produce, which means that
  the compiler must generate separate code for each type.

* Additionally, futures use CRTP for compile-time polymorphism, so each concrete
  future type is a distinct class and may duplicate common behavior.

The following sections detail the code size of various future implementations
and utilities.

ValueFuture
===========
``ValueFuture`` is the simplest future type, used to return a single result from
an asynchronous operation. Its implementation contains effectively the minimal
code required for a future, making it a good baseline for understanding the size
cost of a future in ``pw_async2``.

The table below shows the size of ``ValueFuture``. The first row shows the base
cost of using a single ``ValueFuture``. The second row adds another
``ValueFuture`` with a different return type to demonstrate the incremental cost
of template specialization. The third row shows the size of ``VoidFuture``
(alias for ``ValueFuture<void>``), which is specialized to avoid storing a
value.

.. include:: size_report/value_future_table

----------------
async2 utilities
----------------
Pigweed provides several utilities to simplify writing asynchronous code. Among
these are combinators which operate over several futures, such as ``Join`` which
waits for all futures to complete, and ``Select`` which waits for the first
future to complete.

The table below demonstrates the code size impact of using these utilities.
For both ``Join`` and ``Select``, the report shows:

* The initial cost of using the utility with multiple futures of the same
  type.
* The incremental cost of adding a second call with futures of *different*
  types, which demonstrates the overhead of template specialization.

Additionally, the table includes a comparison showing the code size difference
between using the ``Select`` helper versus manually polling each future.

.. include:: size_report/utilities_size_report

.. _module-pw_async2-channels-size-report:

-------
Channel
-------
:ref:`pw_async2 channels <module-pw_async2-channels>` are the primary mechanism
for communicating between async tasks and threads.

The following size report shows the base cost of using static and dynamic
channels, as well as the marginal cost of adding a new channel of a trivial or
non-trivial type.

.. include:: size_report/channel_size_report
