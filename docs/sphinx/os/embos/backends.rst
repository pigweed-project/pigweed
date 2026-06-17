.. _embos-backends:

=====================
embOS backend modules
=====================
Pigweed provides native embOS backends for several of its core OS abstraction
layers.

.. list-table:: OS backend modules
   :header-rows: 1

   * - **API group**
     - **Backend module**
     - **Description**
   * - **Time**
     - :ref:`module-pw_chrono_embos`
     - Non-overflowing 64-bit ``SystemClock`` implementation.
   * - **Synchronization**
     - :ref:`module-pw_sync_embos`
     - Type-safe Mutex, TimedMutex, InterruptSpinLock, Semaphores, and ThreadNotification.
   * - **Threading**
     - :ref:`module-pw_thread_embos`
     - Thread creation, stack allocation, sleep, yield, iteration, and snapshot utilities.

.. toctree::
   :maxdepth: 1
   :hidden:

   Chrono <pw://pw_chrono_embos/docs.html>
   Sync <pw://pw_sync_embos/docs.html>
   Thread <pw://pw_thread_embos/docs.html>
