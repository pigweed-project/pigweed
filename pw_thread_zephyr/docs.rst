.. _module-pw_thread_zephyr:

================
pw_thread_zephyr
================
.. pigweed-module::
   :name: pw_thread_zephyr

This is the Zephyr RTOS backend for the ``pw_thread`` facades. It implements
four independent facades, each controlled by its own Kconfig ``choice``
that defaults to Pigweed's Zephyr backend (see
:ref:`zephyr-backend-selection`).

---------------
Thread Creation
---------------
Controlled by ``choice PIGWEED_THREAD_BACKEND``:

* ``CONFIG_PIGWEED_THREAD=y`` -- link Pigweed's Zephyr backend for both
  ``pw_thread.thread`` and ``pw_thread.id``. ``pw_thread_zephyr`` fully
  supports the thread creation API; see the
  :ref:`Thread creation section <module-pw_thread-thread-creation>` for
  details on the API itself. Also requires a ``pw_interrupt.context``
  backend (see :ref:`module-pw_interrupt_zephyr`).
* ``CONFIG_PIGWEED_THREAD_NONE=y`` -- don't link a backend.

When ``CONFIG_PIGWEED_THREAD=y``, the following options configure the
threads Pigweed creates:

* ``CONFIG_PIGWEED_THREAD_DEFAULT_PRIORITY`` -- default thread priority
  (defaults to ``CONFIG_MAIN_THREAD_PRIORITY``).
* ``CONFIG_PIGWEED_THREAD_NUM_PREEMPT_PRIORITIES`` -- number of preemptible
  thread priorities (defaults to ``CONFIG_NUM_PREEMPT_PRIORITIES``).
* ``CONFIG_PIGWEED_THREAD_MAX_THREAD_NAME_LEN`` -- maximum length of a
  Pigweed thread's name (defaults to 15; set to 0 if you don't plan on
  naming your threads).
* ``CONFIG_PIGWEED_THREAD_DEFAULT_STACK_SIZE`` -- size of a standard stack
  (defaults to 1024).
* ``CONFIG_PIGWEED_THREAD_MINIMUM_STACK_SIZE`` -- minimum stack size
  (defaults to 256).

------------------------
Thread Iteration Backend
------------------------
Controlled by ``choice PIGWEED_THREAD_ITERATION_BACKEND``:

* ``CONFIG_PIGWEED_THREAD_ITERATION=y`` -- link the ``pw_thread.thread_iteration``
  backend, used to enumerate running threads (e.g. for a thread snapshot
  RPC service). Also requires ``CONFIG_PIGWEED_THREAD=y``, since iteration
  walks Zephyr's native thread list created by Pigweed's own thread
  backend.
* ``CONFIG_PIGWEED_THREAD_ITERATION_NONE=y`` -- don't link a backend.

--------------------
Thread Sleep Backend
--------------------
A backend for ``pw::this_thread::sleep_for()`` and
``pw::this_thread::sleep_until()``. Controlled by ``choice
PIGWEED_THREAD_SLEEP_BACKEND``:

* ``CONFIG_PIGWEED_THREAD_SLEEP=y`` -- link Pigweed's Zephyr backend. Also
  requires a ``pw_chrono.system_clock`` backend (see
  :ref:`module-pw_chrono_zephyr`).
* ``CONFIG_PIGWEED_THREAD_SLEEP_NONE=y`` -- don't link a backend.

--------------------
Thread Yield Backend
--------------------
A backend for ``pw::this_thread::yield()``. Controlled by ``choice
PIGWEED_THREAD_YIELD_BACKEND``:

* ``CONFIG_PIGWEED_THREAD_YIELD=y`` -- link Pigweed's Zephyr backend.
* ``CONFIG_PIGWEED_THREAD_YIELD_NONE=y`` -- don't link a backend.
