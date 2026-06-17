.. _threadx-backends:

=======================
ThreadX backend modules
=======================
Pigweed provides native ThreadX backends for several of its core OS abstraction
layers.

.. list-table:: OS backend modules
   :header-rows: 1

   * - **API group**
     - **Backend module**
     - **Description**
   * - **Time**
     - :ref:`module-pw_chrono_threadx`
     - Non-overflowing 64-bit ``SystemClock`` implementation.
   * - **Synchronization**
     - :ref:`module-pw_sync_threadx`
     - Type-safe Mutex, TimedMutex, InterruptSpinLock, Semaphores, and ThreadNotification.
   * - **Threading**
     - :ref:`module-pw_thread_threadx`
     - Thread creation, stack allocation, sleep, yield, iteration, and snapshot utilities.

.. toctree::
   :maxdepth: 1
   :hidden:

   Chrono <pw://pw_chrono_threadx/docs.html>
   Sync <pw://pw_sync_threadx/docs.html>
   Thread <pw://pw_thread_threadx/docs.html>
