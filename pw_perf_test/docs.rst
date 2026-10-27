.. _module-pw_perf_test:

============
pw_perf_test
============

.. pigweed-module::
   :name: pw_perf_test

   - **Simple**: Automatically manages boilerplate like iterations and durations.
   - **Easy**: Uses an intuitive API that resembles GoogleTest.
   - **Reusable**: Integrates with modules like ``pw_log`` that you already use.

Pigweed's perf test module provides an easy way to measure performance on
any test setup!

---------------
Getting started
---------------
You can add a simple performance test using the follow steps:

Configure your toolchain
========================
If necessary, configure your toolchain for performance testing:

.. note:: Currently, ``pw_perf_test`` provides build integration with Bazel and
   GN. Performance tests can be built in CMake, but must be built as regular
   executables.

.. tab-set::

   .. tab-item:: Bazel
      :sync: bazel

       - ``pw_perf_test_timer_backend``: Sets the backend used to measure
         durations. Options include:

         - ``@pigweed//pw_perf_test:chrono_timer``: Uses
           ``pw_chrono::SystemClock`` to measure time.
         - ``@pigweed//pw_perf_test:arm_cortex_timer``: Uses cycle count
           registers available on ARM-Cortex to measure time.
         - ``@pigweed//pw_perf_test:arm_cortex_systick_timer``: Uses the
           architectural 24-bit SysTick counter on baremetal ARM Cortex-M
           targets without ``DWT->CYCCNT`` (such as Cortex-M0/M0+/M23 or QEMU).

       - Currently, only the logging event handler is supported for Bazel.

   .. tab-item:: GN
      :sync: gn

       - ``pw_perf_test_TIMER_INTERFACE_BACKEND``: Sets the backend used to
         measure durations. Options include:

         - ``"$dir_pw_perf_test:chrono_timer"``: Uses
           ``pw_chrono::SystemClock`` to measure time.
         - ``"$dir_pw_perf_test:arm_cortex_timer"``: Uses cycle count
           registers available on ARM-Cortex to measure time.
         - ``"$dir_pw_perf_test:arm_cortex_systick_timer"``: Uses the
           architectural 24-bit SysTick counter on baremetal ARM Cortex-M
           targets without ``DWT->CYCCNT`` (such as Cortex-M0/M0+/M23 or QEMU).

       - ``pw_perf_test_MAIN_FUNCTION``: Indicates the GN target that provides
         a ``main`` function that sets the event handler and runs tests. The
         default is ``"$dir_pw_perf_test:logging_main"``.

Write a test function
=====================
Write a test function that exercises the behavior you wish to benchmark. For
this example, we will simulate doing work with:

.. literalinclude:: examples/example_perf_test.cc
   :language: cpp
   :linenos:
   :start-after: [pw_perf_test_examples-simulate_work]
   :end-before: [pw_perf_test_examples-simulate_work]

Creating a performance test is as simple as using the ``PW_PERF_TEST_SIMPLE``
macro to name the function and optionally provide arguments to it:

.. literalinclude:: examples/example_perf_test.cc
   :language: cpp
   :linenos:
   :start-after: [pw_perf_test_examples-simple_example]
   :end-before: [pw_perf_test_examples-simple_example]

If you need to do additional setup as part of your test, you can use the
``PW_PERF_TEST`` macro, which provides an explicit ``pw::perf_test::State``
reference. the behavior to be benchmarked should be put in a loop that checks
``State::KeepRunning()``:

.. literalinclude:: examples/example_perf_test.cc
   :language: cpp
   :linenos:
   :start-after: [pw_perf_test_examples-full_example]
   :end-before: [pw_perf_test_examples-full_example]

You can even use lambdas in place of standalone functions:

.. literalinclude:: examples/example_perf_test.cc
   :language: cpp
   :linenos:
   :start-after: [pw_perf_test_examples-lambda_example]
   :end-before: [pw_perf_test_examples-lambda_example]

.. _module-pw_perf_test-pw_perf_test:

Build Your Test
===============
.. tab-set::

   .. tab-item:: Bazel
      :sync: bazel

      Add your performance test to the build using the ``pw_cc_perf_test``
      rule from ``//pw_perf_test:pw_cc_perf_test.bzl``.

      **Arguments**

      * All ``native.cc_binary`` arguments are supported.

      **Example**

      .. code-block::

         load("//pw_perf_test:pw_cc_perf_test.bzl", "pw_cc_perf_test")

         pw_cc_perf_test(
             name = "my_perf_test",
             srcs = ["my_perf_test.cc"],
         )

   .. tab-item:: GN
      :sync: gn

      Add your performance test to the build using the ``pw_perf_test``
      template. This template creates two sub-targets.

      * ``<target_name>.lib``: The test sources without a main function.
      * ``<target_name>``: The test suite binary, linked against
        ``pw_perf_test_MAIN_FUNCTION``.

      **Arguments**

      * All ``pw_executable`` arguments are supported.
      * ``enable_if``: Boolean indicating whether the test should be built. If
        false, replaces the test with an empty target. Defaults to true.

      **Example**

      .. code-block::

         import("$dir_pw_perf_test/perf_test.gni")

         pw_perf_test("my_perf_test") {
           sources = [ "my_perf_test.cc" ]
           enable_if = device_has_1m_flash
         }

Run your test
=============
.. tab-set::

   .. tab-item:: GN
      :sync: gn

      To run perf tests from GN, locate the associated binaries from the ``out``
      directory and run/flash them manually.

   .. tab-item:: Bazel
      :sync: bazel

      Use the default Bazel run command: ``bazel run //path/to:target``.

-------------
API reference
-------------
Moved: :cc:`pw_perf_test`

------
Design
------

``pw_perf_test`` uses a ``Framework`` singleton similar to that of
``pw_unit_test``. This singleton is statically created, and tests declared using
macros such as ``PW_PERF_TEST`` will automatically register themselves with it.

A provided ``main`` function interacts with the ``Framework`` by calling
``pw::perf_test::RunAllTests`` and providing an ``EventHandler``. For each
registered test, the ``Framework`` creates a ``State`` object and passes it to
the test function.

The State object tracks the number of iterations. It expects the test function
to include a loop with the condition of ``State::KeepRunning``. This loop
should include the behavior being banchmarked, e.g.

.. code-block:: cpp

   while (state.KeepRunning()) {
     // Code to be benchmarked.
   }

In particular, ``State::KeepRunning`` should be called exactly once before the
first iteration, as in a ``for`` or ``while`` loop. The ``State`` object will
use the timer facade to measure the elapsed duration between successive calls to
``State::KeepRunning``.

Additionally, the ``State`` object receives a reference to the ``EventHandler``
from the ``Framework``, and uses this to report both test progress and
performance measurements.

Timers
======
Currently, Pigweed provides two implementations of the timer interface.
Consumers may provide additional implementations and use them as a backend for
the timer facade.

Chrono Timer
------------
This timer depends :ref:`module-pw_chrono` and will only measure performance in
terms of nanoseconds. It is the default for performance tests on host.

Cycle Count Timer
-----------------
On ARM Cortex devices, clock cycles may more accurately measure the actual
performance of a benchmark.

This implementation is OS-agnostic, as it directly accesses CPU registers.
It enables the `DWT register`_ through the `DEMCR register`_. While this
provides cycle counts directly from the CPU, it notably overflows if the
duration of a test exceeding 2^32 clock cycles. At 100 MHz, this is
approximately 43 seconds.

.. warning::
  The interface only measures raw clock cycles and does not take into account
  other possible sources of pollution such as LSUs, Sleeps and other registers.
  `Read more on the DWT methods of counting instructions.`__

.. __: `DWT methods`_

EventHandlers
=============
Currently, Pigweed provides one implementation of ``EventHandler``. Consumers
may provide additional implementations and use them by providing a dedicated
``main`` function that passes the handler to ``pw::perf_test::RunAllTests``.

LoggingEventHandler
-------------------
The default method of running performance tests uses a ``LoggingEventHandler``.
This event handler only logs the test results to the console and nothing more.
It was chosen as the default method due to its portability and to cut down on
the time it would take to implement other printing log handlers. Make sure to
set a ``pw_log`` backend.

LogCsvEventHandler
-------------------
This event handler logs the results to the console in CSV format.

.. code-block:: text

   INF  test name,total iterations,min,max,mean,unit
   INF  Detokenize_NoMessage,100,1474,1654,1542,ns
   INF  Detokenize_NoArgs,100,3192,3528,3347,ns
   INF  Detokenize_OneArg,100,6185,6999,6435,ns

-------
Roadmap
-------
- `CMake support <https://g-issues.pigweed.dev/issues/309637691>`_
- `Unified framework <https://g-issues.pigweed.dev/issues/309639171>`_.

.. _DWT register: https://developer.arm.com/documentation/ddi0337/e/System-Debug/DWT?lang=en
.. _DEMCR register: https://developer.arm.com/documentation/ddi0337/e/CEGHJDCF
.. _DWT methods: https://developer.arm.com/documentation/ka001499/1-0/
