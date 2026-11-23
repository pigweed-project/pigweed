.. _target-stm32_nucleo_f103:

=================
stm32_nucleo_f103
=================
Pigweed platform support for the STMicroelectronics **NUCLEO-F103RB**
development board (STM32F103RBT6 ARM Cortex-M3 MCU).

--------
Building
--------

Bazel
=====
Build targets for the board (configured with FreeRTOS) using
``--config=stm32_nucleo_f103``:

.. code-block:: console

   $ bazelisk build --config=stm32_nucleo_f103 //pw_status:status_test

GN / Ninja
==========
Generate the GN build and compile the bare-metal ``stm32_nucleo_f103`` target
group (plus ``python.install`` to install the host flashing and test scripts):

.. code-block:: console

   $ gn gen out
   $ ninja -C out stm32_nucleo_f103 python.install

To build the FreeRTOS GN variant, install the ``freertos`` package and point
``dir_pw_third_party_freertos`` at it via ``PW_PACKAGE_ROOT``:

.. code-block:: console

   $ source activate.sh
   $ pw package install freertos
   $ gn gen out --args='dir_pw_third_party_freertos=getenv("PW_PACKAGE_ROOT") + "/freertos"'
   $ ninja -C out stm32_nucleo_f103_freertos

--------
Flashing
--------

Bazel
=====
Flash an ELF binary using the hermetic ``probe-rs`` flasher:

.. code-block:: console

   $ bazelisk run //targets/stm32_nucleo_f103/py:flash -- path/to/binary.elf

GN / Ninja
==========
With the Pigweed environment activated (and ``ninja -C out python.install``
completed), flash an ELF binary using ``stm32_nucleo_f103_flasher`` (uses CIPD
``probe-rs``):

.. code-block:: console

   $ source activate.sh
   $ stm32_nucleo_f103_flasher out/stm32_nucleo_f103_size_optimized/obj/pw_status/test/status_test.elf

-------
Testing
-------

Bazel
=====
Start the on-device unit test server in one terminal:

.. code-block:: console

   $ bazelisk run //targets/stm32_nucleo_f103/py:unit_test_server

Then run any ``pw_cc_test`` target on the connected Nucleo-F103RB board:

.. code-block:: console

   $ bazelisk test --config=stm32_nucleo_f103 //pw_status:status_test

GN / Ninja
==========
Run a single GN-built test ELF directly on the connected board:

.. code-block:: console

   $ source activate.sh
   $ stm32_nucleo_f103_unit_test_runner out/stm32_nucleo_f103_size_optimized/obj/pw_status/test/status_test.elf

Or enable automatic test execution through
``stm32_nucleo_f103_unit_test_server``:

.. code-block:: console

   $ source activate.sh
   $ gn gen out --args='pw_targets_ENABLE_STM32_NUCLEO_F103_TEST_RUNNER=true'
   $ ninja -C out python.install
   $ stm32_nucleo_f103_unit_test_server &
   $ ninja -C out stm32_nucleo_f103

--------------
Hardware Notes
--------------
* **Debug Console:** ``pw_sys_io`` uses ``USART2`` on ``PA2`` (TX) and ``PA3``
  (RX) at ``115200 8N1``, routed by default to the on-board ST-LINK/V2-1
  Virtual COM Port over USB.
* **Clock Configuration:** Boot configures ``SYSCLK`` to ``72 MHz`` (``36 MHz``
  ``APB1``, ``72 MHz`` ``APB2``) from the on-board ST-LINK ``8 MHz`` MCO clock
  on ``OSC_IN`` (``HSE`` bypass mode), automatically falling back to ``64 MHz``
  (``HSI / 2 * 16``) if ``HSE`` is unavailable.
* **SWD / JTAG Pins:** 2-wire SWD remains enabled on ``PA13``/``PA14`` while
  5-wire JTAG is disabled (``AFIO_MAPR.SWJ_CFG = 0b010``) so ``PA15``, ``PB3``,
  and ``PB4`` can be used as general-purpose I/O.
