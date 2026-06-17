.. _freertos-backends:

========================
FreeRTOS backend modules
========================
Pigweed provides native FreeRTOS backends for several of its core OS abstraction
layers.

.. list-table:: OS backend modules
   :header-rows: 1

   * - **API group**
     - **Backend module**
     - **Description**
   * - **Time**
     - :ref:`module-pw_chrono_freertos`
     - Non-overflowing 64-bit ``SystemClock`` and race-free ``SystemTimer``.
   * - **Interrupts**
     - :ref:`module-pw_interrupt_freertos`
     - Interrupt service routine utilities for task-to-ISR synchronization.
   * - **Memory Allocation**
     - :ref:`module-pw_malloc_freertos`
     - Interface wrapper mapping Pigweed allocation to FreeRTOS memory heaps.
   * - **Synchronization**
     - :ref:`module-pw_sync_freertos`
     - Type-safe Mutex, InterruptSpinLock, Semaphores, and ThreadNotification.
   * - **Threading**
     - :ref:`module-pw_thread_freertos`
     - Thread creation, stack checking, sleep, yield, and iteration utilities.

.. toctree::
   :maxdepth: 1
   :hidden:

   Chrono <pw://pw_chrono_freertos/docs.html>
   Interrupt <pw://pw_interrupt_freertos/docs.html>
   Malloc <pw://pw_malloc_freertos/docs.html>
   Sync <pw://pw_sync_freertos/docs.html>
   Thread <pw://pw_thread_freertos/docs.html>
