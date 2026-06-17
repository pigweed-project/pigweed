.. _zephyr-backends:

======================
Zephyr backend modules
======================
Pigweed provides native Zephyr backends for several of its core OS abstraction
layers.

.. list-table:: OS backend modules
   :header-rows: 1

   * - **API group**
     - **Backend module**
     - **Description**
   * - **Time**
     - :ref:`module-pw_chrono_zephyr`
     - Non-overflowing 64-bit clock support and delayable-work timers.
   * - **Interrupts**
     - :ref:`module-pw_interrupt_zephyr`
     - Interrupt-context detection utilities.
   * - **Memory Allocation**
     - :ref:`module-pw_allocator_zephyr`
     - Interface wrappers mapping allocation to Zephyr kernel heaps.
   * - **Synchronization**
     - :ref:`module-pw_sync_zephyr`
     - Type-safe Mutex, TimedMutex, Semaphores, and ThreadNotification.
   * - **Threading**
     - :ref:`module-pw_thread_zephyr`
     - Thread creation, sleep, yield, and iteration utilities.
   * - **Asserts**
     - :ref:`module-pw_assert_zephyr`
     - Routing and optional tokenization of assertions.
   * - **Logging**
     - :ref:`module-pw_log_zephyr`
     - Plain and tokenized logging backend support.
   * - **Serial I/O**
     - :ref:`module-pw_sys_io_zephyr`
     - Standard I/O operations routed to the console or USB subsystem.
   * - **SPI**
     - :ref:`module-pw_spi_zephyr`
     - Native implementation of SPI interfaces.
   * - **Environment Setup**
     - :ref:`module-pw_env_setup_zephyr`
     - CLI tools like ``pw west`` to integrate the development environment.

.. note:: The version of Zephyr bundled with ``pw package install zephyr`` is
   being migrated to v3.6 as we near the latest release.

.. toctree::
   :maxdepth: 1
   :hidden:

   Allocator <pw://pw_allocator_zephyr/docs.html>
   Assert <pw://pw_assert_zephyr/docs.html>
   Chrono <pw://pw_chrono_zephyr/docs.html>
   Env setup <pw://pw_env_setup_zephyr/docs.html>
   Interrupt <pw://pw_interrupt_zephyr/docs.html>
   Log <pw://pw_log_zephyr/docs.html>
   Sync <pw://pw_sync_zephyr/docs.html>
   SPI <pw://pw_spi_zephyr/docs.html>
   Sys IO <pw://pw_sys_io_zephyr/docs.html>
   Thread <pw://pw_thread_zephyr/docs.html>
