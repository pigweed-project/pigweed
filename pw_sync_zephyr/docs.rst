.. _module-pw_sync_zephyr:

==============
pw_sync_zephyr
==============
.. pigweed-module::
   :name: pw_sync_zephyr

--------
Overview
--------
This is the Zephyr RTOS backend for the ``pw_sync`` facades. It implements
seven independent facades, each controlled by its own Kconfig ``choice``
that defaults to Pigweed's Zephyr backend (see
:ref:`zephyr-backend-selection`).

.. list-table:: Facades and their Kconfig choices
   :header-rows: 1

   * - Facade
     - Kconfig backend member
     - "None" member
   * - ``pw_sync.mutex``
     - ``CONFIG_PIGWEED_SYNC_MUTEX``
     - ``CONFIG_PIGWEED_SYNC_MUTEX_NONE``
   * - ``pw_sync.timed_mutex``
     - ``CONFIG_PIGWEED_SYNC_TIMED_MUTEX``
     - ``CONFIG_PIGWEED_SYNC_TIMED_MUTEX_NONE``
   * - ``pw_sync.binary_semaphore``
     - ``CONFIG_PIGWEED_SYNC_BINARY_SEMAPHORE``
     - ``CONFIG_PIGWEED_SYNC_BINARY_SEMAPHORE_NONE``
   * - ``pw_sync.counting_semaphore``
     - ``CONFIG_PIGWEED_SYNC_COUNTING_SEMAPHORE``
     - ``CONFIG_PIGWEED_SYNC_COUNTING_SEMAPHORE_NONE``
   * - ``pw_sync.interrupt_spin_lock``
     - ``CONFIG_PIGWEED_SYNC_INTERRUPT_SPIN_LOCK``
     - ``CONFIG_PIGWEED_SYNC_INTERRUPT_SPIN_LOCK_NONE``
   * - ``pw_sync.thread_notification``
     - ``CONFIG_PIGWEED_SYNC_THREAD_NOTIFICATION``
     - ``CONFIG_PIGWEED_SYNC_THREAD_NOTIFICATION_NONE``
   * - ``pw_sync.timed_thread_notification``
     - ``CONFIG_PIGWEED_SYNC_TIMED_THREAD_NOTIFICATION``
     - ``CONFIG_PIGWEED_SYNC_TIMED_THREAD_NOTIFICATION_NONE``

These backends also use ``pw_chrono.system_clock`` and
``pw_interrupt.context``. ``pw_sync.timed_mutex`` is built on
``pw_sync.mutex``, and both thread notifications are built on
``pw_sync.binary_semaphore``, so keep those enabled together.
