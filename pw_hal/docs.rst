.. _module-pw_hal:

======
pw_hal
======
.. pigweed-module::
   :name: pw_hal

Pigweed HAL (``pw_hal``) is a unified Hardware Abstraction Layer providing
clean, modern C++ interfaces and driver abstractions for microcontroller
peripherals.

It builds upon the previous efforts in ``pw_i2c``, ``pw_spi``, etc., by unifying
HAL design across peripherals, adding native support for ``pw_async2``, and
focusing on enabling agentic development of low-level drivers in order to speed
up hardware bring-up and broaden support across many silicon vendors.

.. _module-pw_hal-dashboard:

---------
Dashboard
---------
The dashboard below tracks the current status of Pigweed HAL peripheral APIs
and driver implementations across :ref:`roadmap phases <module-pw_hal-roadmap-phases>`.

.. list-table::
   :header-rows: 1
   :widths: 28 14 14 14 14 14

   * - Peripheral Domain
     - Investigation
     - Prototype
     - API Definition
     - First Platform
     - Multiple Platforms
   * - *GPIO: Pinmux*
     -
     -
     -
     -
     -
   * - *GPIO: Digital IO*
     -
     -
     -
     -
     -
   * - *UART: Buffered*
     - ✔
     - ✔
     - ✔
     - ⛏
     -
   * - *UART: Zero Copy*
     - ✔
     - ⛏
     -
     -
     -
   * - *I2C: Initiator*
     - ✔
     - ✔
     - ⛏
     - ⏱
     -
   * - *I2C: Responder*
     - ⏱
     -
     -
     -
     -
   * - *I3C: Controller*
     -
     -
     -
     -
     -
   * - *I3C: Target*
     -
     -
     -
     -
     -
   * - *SPI: Controller*
     - ⛏
     -
     -
     -
     -
   * - *SPI: Target*
     -
     -
     -
     -
     -
   * - *ADC*
     -
     -
     -
     -
     -
   * - *DAC*
     -
     -
     -
     -
     -
   * - *Timer*
     -
     -
     -
     -
     -
   * - *PWM*
     -
     -
     -
     -
     -

.. note::

   * **✔** - Phase completed.
   * **⛏** - Work is actively underway for this phase.
   * **⏱** - Work is planned but not yet started.
   * *(blank)* - Not yet started or planned.

.. _module-pw_hal-roadmap-phases:

Roadmap phases
==============
* **Investigation**: Researching requirements, hardware peripheral capabilities,
  and cross-platform abstraction patterns.
* **Prototype**: Developing experimental interfaces, proof-of-concept drivers,
  and test harnesses.
* **API Definition**: Standardizing and reviewing abstract driver and controller
  interfaces in Pigweed HAL.
* **First Platform**: Implementing and validating a complete driver on
  an initial hardware target with automated testing.
* **Multiple Platforms**: Extending verified driver implementations across
  multiple distinct silicon architectures and vendor SDKs.

-------------------------------
Peripherals under consideration
-------------------------------
These peripherals require additional investigation before including them as
part of the planned ``pw_hal`` interface.

* Clock Tree
* Power Tree
* Flash / EEPROM / NVM
* QSPI
* xSPI / mSPI
* DMA
* Display
* Graphics Acceleration
* I2S
* Watchdog Timer
