.. _target-pw_rp2350:

=========
pw_rp2350
=========
This directory contains Pigweed target definitions and application setups for the
`Pigweed RP2350 board <https://pigweed.googlesource.com/pigweed/hardware/testing/>`_.
It supports running the Pigweed rust examples under three different kernels/RTOS options:

1. **FreeRTOS** (under ``freertos/``)
2. **Pigweed Kernel** (under ``pigweed/``)
3. **Zephyr** (under ``zephyr/``)

All kernels can compile and run the same Rust example binaries, as long as all dependent
backends are supported by the particular RTOS.

All output is logged to the UART.

----------------
Flashing Targets
----------------
Before flashing, connect your PW RP2350 target board to your host machine.

FreeRTOS
========
To compile and flash the FreeRTOS loop example, run the following command from the
root of the Pigweed repository:

.. code-block:: console

   bazelisk run //targets/pw_rp2350/freertos:flash_sleep_example
   bazelisk run //targets/pw_rp2350/freertos:flash_system_clock_example

Pigweed Kernel
==============
To compile and flash the Pigweed kernel loop example, run the following command from
the root of the Pigweed repository:

.. code-block:: console

   bazelisk run //targets/pw_rp2350/pigweed:flash_sleep_example

Zephyr
======
To build and flash the Zephyr loop example, navigate to the Zephyr sub-workspace
directory:

.. code-block:: console

   cd targets/pw_rp2350/zephyr

Then run the flash command:

.. code-block:: console

   bazelisk run //:flash_sleep_example

------------------
Running Unit Tests
------------------
Pigweed supports running on-device unit tests using the RPC unit test runner.
This requires running a test server on the host that communicates with the
connected board.

FreeRTOS
========
1. Start the test server on the host.

   .. code-block:: console

      bazelisk run //targets/pw_rp2350/py:unit_test_server

2. Run the tests using ``bazelisk test`` with the ``pw_rp2350_freertos`` config:

   .. code-block:: console

      bazelisk test --cache_test_results=no --test_output=all --config=pw_rp2350_freertos //targets/pw_rp2350/...

Zephyr
======
1. Start the test server on the host (from the root of the Pigweed repository):

   .. code-block:: console

      bazelisk run //targets/pw_rp2350/py:unit_test_server

2. Navigate to the Zephyr sub-workspace directory:

   .. code-block:: console

      cd targets/pw_rp2350/zephyr

3. Run the tests:

   .. code-block:: console

      bazelisk test --cache_test_results=no --test_output=all --config=pw_rp2350_zephyr //:unit_test_example_cc
      bazelisk test --cache_test_results=no --test_output=all --config=pw_rp2350_zephyr //:unit_test_example_rust
