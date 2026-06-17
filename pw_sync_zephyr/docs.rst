.. _module-pw_sync_zephyr:

==============
pw_sync_zephyr
==============
.. pigweed-module::
   :name: pw_sync_zephyr

--------
Overview
--------
This sync backend implements the ``pw_sync`` facade. To enable, set
``CONFIG_PIGWEED_SYNC=y``. After that, specific submodules can be enabled via
the Kconfig menu.

* ``pw_sync.mutex`` can be enabled via ``CONFIG_PIGWEED_SYNC_MUTEX``.
* ``pw_sync.timed_mutex`` can be enabled via ``CONFIG_PIGWEED_SYNC_TIMED_MUTEX``.
* ``pw_sync.binary_semaphore`` can be enabled via
  ``CONFIG_PIGWEED_SYNC_BINARY_SEMAPHORE``.
* ``pw_sync.counting_semaphore`` can be enabled via
  ``CONFIG_PIGWEED_SYNC_COUNTING_SEMAPHORE``.
* ``pw_sync.interrupt_spin_lock`` can be enabled via ``CONFIG_PIGWEED_SYNC_INTERRUPT_SPIN_LOCK``.
* ``pw_sync.thread_notification`` can be enabled via
  ``CONFIG_PIGWEED_SYNC_THREAD_NOTIFICATION``.
* ``pw_sync.timed_thread_notification`` can be enabled via
  ``CONFIG_PIGWEED_SYNC_TIMED_THREAD_NOTIFICATION``.
