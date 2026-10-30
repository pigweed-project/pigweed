.. _docs-os-zephyr:

======
Zephyr
======
Pigweed's Zephyr integration provides a suite of abstractions and helpers designed to
make developing C++ applications on Zephyr safer, more robust, and fully
compatible with Pigweed's portable facades.

* **Compile-time lock safety**: Support for Clang's static thread-safety analysis
  annotations, enabling compile-time detection of data races and incorrect lock ordering.
* **Safe non-recursive mutexes**: Enforces strict non-recursive mutex semantics
  and assertions to detect bugs and prevent deadlocks.
* **Safe callback execution**: Routes timer callbacks to delayable work queues
  running in thread context, enabling race-free synchronous timer deletion.
* **Tokenized logs and asserts**: Drastically reduces binary size by tokenizing
  all assertion and logging statements in both the application and the Zephyr kernel.

.. grid:: 1

   .. grid-item-card:: Advantages & differences
      :link: docs-os-zephyr-features
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Learn how Pigweed's wrappers improve safety, catch bugs at compile-time or
      runtime, and compare directly with raw Zephyr APIs.

.. grid:: 1

   .. grid-item-card:: Setup
      :link: docs-os-zephyr-setup
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Get started using Pigweed with Zephyr, build the starter project, and run
      tests.

.. toctree::
   :maxdepth: 1
   :hidden:

   self
   setup
   features
   backends

.. categories-start
.. +----------------------------------------------------------+
.. | WARNING: AUTO-GENERATED SECTION. DO NOT MANUALLY EDIT!   |
.. |                                                          |
.. | To regenerate this section, run:                         |
.. |   bazelisk run //docs/common/metadata:sync               |
.. |                                                          |
.. | Docs: https://pigweed.dev/contributing/docs/website.html |
.. +----------------------------------------------------------+

-------
Modules
-------
The following table lists all modules related to Zephyr.

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

.. Do not edit this toctree. To add non-module docs to this category,
.. create a separate toctree outside the categories-start /
.. categories-end block.
.. toctree::
   :maxdepth: 1
   :hidden:

   Allocators <../../pw_allocator_zephyr/docs>
   Assertions <../../pw_assert_zephyr/docs>
   Chrono <../../pw_chrono_zephyr/docs>
   Digital I/O <../../pw_digital_io_zephyr/docs>
   Environment setup <../../pw_env_setup_zephyr/docs>
   I2C <../../pw_i2c_zephyr/docs>
   Interrupts <../../pw_interrupt_zephyr/docs>
   Logging <../../pw_log_zephyr/docs>
   SPI <../../pw_spi_zephyr/docs>
   Synchronization <../../pw_sync_zephyr/docs>
   System I/O <../../pw_sys_io_zephyr/docs>
   Threads <../../pw_thread_zephyr/docs>

.. +----------------------------------------------------------+
.. | END OF AUTO-GENERATED SECTION                            |
.. +----------------------------------------------------------+
.. categories-end
