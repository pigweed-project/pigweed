.. _docs-category-hw:

================
Hardware support
================
.. categories-start
.. +----------------------------------------------------------+
.. | WARNING: AUTO-GENERATED SECTION. DO NOT MANUALLY EDIT!   |
.. |                                                          |
.. | To regenerate this section, run:                         |
.. |   bazelisk run //docs/common/metadata:sync               |
.. |                                                          |
.. | Docs: https://pigweed.dev/contributing/docs/website.html |
.. +----------------------------------------------------------+

-------------------
Raspberry Pi RP2XXX
-------------------
Hardware drivers & platform support for Raspberry Pi RP2040 & Pico SDK

Learn more: :doc:`/targets/rp2040/target_docs`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_chrono_rp2040`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_digital_io_rp2040`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_i2c_rp2040`", ":bdg-primary:`stable`", "Raspberry Pi Pico SDK implementation for pw_i2c", "C++"
   ":ref:`module-pw_spi_rp2040`", ":bdg-secondary:`unstable`", "Raspberry Pi RP2040 backend for pw_spi", "C++"
   ":ref:`module-pw_sys_io_rp2040`", ":bdg-primary:`stable`", "", ""

---
NXP
---
Drivers, build utilities, & HAL implementations for NXP MCUXpresso SDK

Learn more: :doc:`/targets/mimxrt595_evk/target_docs`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_build_mcuxpresso`", ":bdg-primary:`stable`", "Utilities for building NXP MCUXpresso SDK targets", "Bazel, GN"
   ":ref:`module-pw_clock_tree_mcuxpresso`", ":bdg-secondary:`unstable`", "NXP MCUXpresso SDK implementation for pw_clock_tree", "C++"
   ":ref:`module-pw_digital_io_mcuxpresso`", ":bdg-primary:`stable`", "Digital I/O for NXP MCUXpresso", "C++"
   ":ref:`module-pw_dma_mcuxpresso`", ":bdg-secondary:`unstable`", "DMA helpers for MCUXpresso SDK", "C++"
   ":ref:`module-pw_i2c_mcuxpresso`", ":bdg-primary:`stable`", "NXP MCUXpresso SDK implementation for pw_i2c", "C++"
   ":ref:`module-pw_spi_mcuxpresso`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_stream_uart_mcuxpresso`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_sys_io_mcuxpresso`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_uart_mcuxpresso`", ":bdg-secondary:`unstable`", "NXP MCUXpresso SDK implementation for pw_uart", "C++"

-----
Ambiq
-----
Hardware drivers & platform support for Ambiq Apollo4 & AmbiqSuite SDK

Learn more: :doc:`/targets/apollo4/target_docs`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_sys_io_ambiq_sdk`", ":bdg-primary:`stable`", "", ""

-------
Arduino
-------
Build utilities, cores, & hardware support for Arduino

Learn more: :doc:`/targets/arduino/target_docs`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_arduino_build`", ":bdg-primary:`stable`", "Main function wrapper and build utilities for Arduino", "C++, GN"
   ":ref:`module-pw_sys_io_arduino`", ":bdg-primary:`stable`", "", ""

-------
Emcraft
-------
Hardware support & HAL integrations for Emcraft SmartFusion2

Learn more: :doc:`/targets/emcraft_sf2_som/target_docs`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_sys_io_emcraft_sf2`", ":bdg-primary:`stable`", "", ""

----------------------------
STMicroelectronics STM32FXXX
----------------------------
Build support & hardware drivers for STMicroelectronics STM32Cube

Learn more: :doc:`/targets/stm32f429i_disc1/target_docs`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_stm32cube_build`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_sys_io_baremetal_stm32f429`", ":bdg-primary:`stable`", "", ""
   ":ref:`module-pw_sys_io_stm32cube`", ":bdg-primary:`stable`", "", ""

-----------------
Texas Instruments
-----------------
Hardware & emulation support for Texas Instruments microcontrollers

Learn more: :doc:`/targets/lm3s6965evb_qemu/target_docs`

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_sys_io_baremetal_lm3s6965evb`", ":bdg-primary:`stable`", "", ""

.. Do not edit this toctree. To add non-module docs to this category,
.. create a separate toctree outside the categories-start /
.. categories-end block.
.. toctree::
   :maxdepth: 1
   :hidden:

   self
   Raspberry Pi RP2XXX <../targets/rp2040/target_docs>
   NXP <../targets/mimxrt595_evk/target_docs>
   Ambiq <../targets/apollo4/target_docs>
   Arduino <../targets/arduino/target_docs>
   Emcraft <../targets/emcraft_sf2_som/target_docs>
   STMicroelectronics STM32FXXX <../targets/stm32f429i_disc1/target_docs>
   Texas Instruments <../targets/lm3s6965evb_qemu/target_docs>

.. +----------------------------------------------------------+
.. | END OF AUTO-GENERATED SECTION                            |
.. +----------------------------------------------------------+
.. categories-end
