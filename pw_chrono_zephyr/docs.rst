.. _module-pw_chrono_zephyr:

================
pw_chrono_zephyr
================
.. pigweed-module::
   :name: pw_chrono_zephyr

--------
Overview
--------
This is the Zephyr RTOS backend for the ``pw_chrono`` facades. It implements
two independent facades, each controlled by its own Kconfig ``choice`` that
defaults to Pigweed's Zephyr backend (see :ref:`zephyr-backend-selection`).

pw_chrono.system_clock
-----------------------
Controlled by ``choice PIGWEED_CHRONO_SYSTEM_CLOCK_BACKEND``:

* ``CONFIG_PIGWEED_CHRONO_SYSTEM_CLOCK=y`` -- link Pigweed's Zephyr
  ``system_clock`` backend, mapping ``pw::chrono::SystemClock`` onto
  Zephyr's signed 64-bit tick counter (``k_uptime_ticks()``).
* ``CONFIG_PIGWEED_CHRONO_SYSTEM_CLOCK_NONE=y`` -- don't link a backend;
  you're supplying your own.

pw_chrono.system_timer
-----------------------
Controlled by ``choice PIGWEED_CHRONO_SYSTEM_TIMER_BACKEND``:

* ``CONFIG_PIGWEED_CHRONO_SYSTEM_TIMER=y`` -- link Pigweed's Zephyr
  ``system_timer`` backend.
* ``CONFIG_PIGWEED_CHRONO_SYSTEM_TIMER_NONE=y`` -- don't link a backend.

``pw::chrono::SystemTimer`` schedules callbacks on Zephyr's delayable
work-queue API (``k_work_delayable``) rather than directly in interrupt
context. This lets the destructor call
``k_work_cancel_delayable_sync()``, which blocks until any in-flight
callback finishes, guaranteeing the callback is inactive once the
``SystemTimer`` object is destroyed. Callbacks still run in a work-queue
thread context and must follow the ``SystemTimer`` contract: never block.
