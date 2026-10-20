.. _docs-category-os_backends:

========
Backends
========
.. categories-start
.. +----------------------------------------------------------+
.. | WARNING: AUTO-GENERATED SECTION. DO NOT MANUALLY EDIT!   |
.. |                                                          |
.. | To regenerate this section, run:                         |
.. |   bazelisk run //docs/common/metadata:sync               |
.. |                                                          |
.. | Docs: https://pigweed.dev/contributing/docs/website.html |
.. +----------------------------------------------------------+

------
Zephyr
------
Modules, backends, and helpers for the Zephyr RTOS

Learn more: :doc:`/os/zephyr/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_allocator_zephyr`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_assert_zephyr`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_chrono_zephyr`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_digital_io_zephyr`", ":bdg-warning:`experimental`", "", "C++"
   ":ref:`module-pw_env_setup_zephyr`", ":bdg-warning:`experimental`", "Zephyr utilities", ""
   ":ref:`module-pw_i2c_zephyr`", ":bdg-warning:`experimental`", "Zephyr implementation for pw_i2c", ""
   ":ref:`module-pw_interrupt_zephyr`", ":bdg-secondary:`unstable`", "", ""
   ":ref:`module-pw_log_zephyr`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_spi_zephyr`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_sync_zephyr`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_sys_io_zephyr`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_thread_zephyr`", ":bdg-primary:`stable`", "", ""

---------------
Amazon FreeRTOS
---------------
Safe, non-overflowing OS abstractions & wrappers for FreeRTOS

Learn more: :doc:`/os/freertos/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_chrono_freertos`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_interrupt_freertos`", ":bdg-warning:`experimental`", "", ""
   ":ref:`module-pw_malloc_freertos`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_sync_freertos`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_thread_freertos`", ":bdg-primary:`stable`", "", ""

---------------
Eclipse ThreadX
---------------
Thread-safe OS abstraction layers & backends for Azure RTOS ThreadX

Learn more: :doc:`/os/threadx/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_chrono_threadx`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_sync_threadx`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_thread_threadx`", ":bdg-primary:`stable`", "", ""

------------
Segger embOS
------------
RTOS abstraction layers & thread-safety integrations for SEGGER embOS

Learn more: :doc:`/os/embos/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_chrono_embos`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_sync_embos`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_thread_embos`", ":bdg-primary:`stable`", "", ""

---
STL
---
C++ Standard Template Library backends for host and POSIX targets

Learn more: :doc:`/os/stl/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_chrono_stl`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_sync_stl`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_thread_stl`", ":bdg-primary:`stable`", "", ""

----------
Bare metal
----------
Backends for running directly on target hardware without an OS or scheduler

Learn more: :doc:`/os/baremetal/index`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_sync_baremetal`", ":bdg-primary:`stable`", "", ""

-------
Android
-------
Utilities and toolchains for using Pigweed on Android

Learn more: :doc:`/targets/android/target_docs`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_android_toolchain`", ":bdg-secondary:`unstable`", "GN toolchains for the Android NDK", "GN"
   ":ref:`module-pw_build_android`", ":bdg-primary:`stable`", "Utilities for using Pigweed in Android platform", "C++"
   ":ref:`module-pw_chre`", ":bdg-warning:`experimental`", "Android Context Hub Runtime Environment backend", "C++"
   ":ref:`module-pw_log_android`", ":bdg-primary:`stable`", "Android logging backend", "C++"

-------
Fuchsia
-------
Backends and platform integrations for Fuchsia OS

Learn more: :doc:`/targets/fuchsia/target_docs`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_assert_fuchsia`", ":bdg-warning:`experimental`", "pw_assert backend for Fuchsia", "C, C++"
   ":ref:`module-pw_async_fuchsia`", ":bdg-warning:`experimental`", "Fuchsia backend for pw_async", "C, C++"
   ":ref:`module-pw_log_fuchsia`", ":bdg-warning:`experimental`", "Fuchsia logging backend", "C, C++"
   ":ref:`module-pw_random_fuchsia`", ":bdg-warning:`experimental`", "Fuchsia implementation of pw_random", ""

.. Do not edit this toctree. To add non-module docs to this category,
.. create a separate toctree outside the categories-start /
.. categories-end block.
.. toctree::
   :maxdepth: 1
   :hidden:

   self
   Zephyr <zephyr/index>
   Amazon FreeRTOS <freertos/index>
   Eclipse ThreadX <threadx/index>
   Segger embOS <embos/index>
   STL <stl/index>
   Bare metal <baremetal/index>
   Android <../targets/android/target_docs>
   Fuchsia <../targets/fuchsia/target_docs>

.. +----------------------------------------------------------+
.. | END OF AUTO-GENERATED SECTION                            |
.. +----------------------------------------------------------+
.. categories-end
