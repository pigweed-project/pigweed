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
Linux udev Setup
----------------
On Linux, install :cs:`targets/rp2040/49-pico.rules` so non-root users have USB
permissions for the on-board debug probe and RP2350 boards. Run the following
commands from the root of the Pigweed repository:

.. code-block:: console

   sudo cp targets/rp2040/49-pico.rules /etc/udev/rules.d/
   sudo udevadm control --reload-rules
   sudo udevadm trigger

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

---------------
Debug Probe CLI
---------------
The ``//targets/pw_rp2350/py:debug_probe_cli`` tool (backed by the
``pw_rp2350_debug_probe`` Python package) communicates with the ``pw_rp2350``
board's on-board debug probe over USB to route the active SWD connection
between the ``dut`` and ``sim`` MCUs, control the RGB status LED, and query
connected target metadata.

Listing Attached Probes
=======================
Use ``list`` to display the USB bus, port, manufacturer, product, and serial
number of each attached debug probe:

.. code-block:: console

   bazelisk run //targets/pw_rp2350/py:debug_probe_cli -- list

Switching the SWD Target
========================
Use ``target`` to route the debug probe's SWD interface to either the device
under test (``dut``) or the simulator MCU (``sim``, also aliased as ``buddy``):

.. code-block:: console

   bazelisk run //targets/pw_rp2350/py:debug_probe_cli -- target dut
   bazelisk run //targets/pw_rp2350/py:debug_probe_cli -- target sim

Controlling the Status LED
==========================
Use ``led`` to control the debug probe's RGB status LED. You can enable
individual color channels with ``-r``/``--red``, ``-g``/``--green``, and
``-b``/``--blue`` (which may be combined), set explicit channel intensities
with ``--rgb R G B`` using decimal (``0`` to ``255``) or ``0x`` hexadecimal
(``0x00`` to ``0xFF``) values, or turn the LED off with ``--off``:

.. code-block:: console

   bazelisk run //targets/pw_rp2350/py:debug_probe_cli -- led -g
   bazelisk run //targets/pw_rp2350/py:debug_probe_cli -- led -r -b
   bazelisk run //targets/pw_rp2350/py:debug_probe_cli -- led --rgb 255 128 0
   bazelisk run //targets/pw_rp2350/py:debug_probe_cli -- led --rgb 0x00 0x80 0xff
   bazelisk run //targets/pw_rp2350/py:debug_probe_cli -- led --off

Querying Probe and Target Info
==============================
Use ``info`` to print the debug probe greeting along with the connected test
target's name, MCU, and supported hardware capabilities:

.. code-block:: console

   bazelisk run //targets/pw_rp2350/py:debug_probe_cli -- info

Selecting a Specific Probe
==========================
When a single debug probe is connected, the CLI selects it automatically. When
multiple probes are attached in an interactive terminal, the CLI prompts you to
choose one. To select a specific probe directly (for example, when multiple
probes are connected or in non-interactive scripts), pass ``--usb-bus`` and
``--usb-port`` using the values reported by ``list``, optionally with
``--no-interactive``:

.. code-block:: console

   bazelisk run //targets/pw_rp2350/py:debug_probe_cli -- --usb-bus 1 --usb-port 1:4 info
   bazelisk run //targets/pw_rp2350/py:debug_probe_cli -- target dut --usb-bus 1 --usb-port 1:4 --no-interactive
