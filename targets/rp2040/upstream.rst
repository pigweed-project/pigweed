.. _target-rp2-upstream:

=====================================
Working with RP2s in upstream Pigweed
=====================================
RP2 is a family of microcontrollers (MCUs) from Raspberry Pi. The RP2040 MCU
(which powers the Pico 1) and the RP2350 MCU (which powers the Pico 2) are both
part of the RP2 family.

This guide shows :ref:`docs-glossary-upstream` maintainers how to do common
RP2-related development tasks in upstream Pigweed, such as building the upstream
repo for the RP2350, running on-device unit tests on a Pico 2, etc.

Most maintainers should use the newer Bazel-based workflows. If you need to use
the older GN-based workflows, see :ref:`target-rp2-upstream-gn`.

---------------
Target Hardware
---------------

.. list-table:: MCU support
   :widths: 1 1
   :header-rows: 1

   * - Target
     - Supported
   * - RP2040
     - ✅ Yes
   * - RP2350
     - ✅ Yes

.. list-table:: Board support
   :widths: 1 1
   :header-rows: 1

   * - Board
     - Support
   * - Pico 1
     - ✅ Yes
   * - Pico 2
     - ✅ Yes
   * - Pico 1W
     - ❌ No
   * - Pico 2W
     - ❌ No

-------------------------
Additional Debug Hardware
-------------------------
.. _PicoPico: https://pigweed.googlesource.com/pigweed/hardware/picopico
.. _Debug Probe: https://www.raspberrypi.com/products/debug-probe/

The core Pigweed team uses `PicoPico`_, a custom development board that makes
parallel on-device testing easier.

If you don't have access to a PicoPico board, the next best option is a Pico 2
and a `Debug Probe`_ (either the official product, or a second Pico or Pico 2
to use as one).

The debug probe also is the way to go if you are are working with a project
like our :ref:`Sense <showcase-sense>`, where the target Pico is connected to
other hardware.

.. _target-rp2-debug-probe:

Setting up a debug probe
========================
After setting one up, using a debug probe is the most hands-off way of
interacting with the target device.

You won't need to use the :kbd:`BOOTSEL` button when flashing the target
afterwards, as the debug probe will be able to drive the flashing process.

You will also be able to debug issues on the target with ``gdb`` and
`OpenOCD <https://openocd.org/>`_

Using the official Raspberry Pi debug probe product
---------------------------------------------------
The `Debug Probe`_ itself is really just a RP2040 on a small board with a few
extra connectors to make wiring it to your target Pi Pico easier.
You can `learn more about it
<https://www.raspberrypi.com/documentation/microcontrollers/debug-probe.html#about-the-debug-probe>`_
on the Raspberry Pi website.

Their website also has good setup instructions.

1. `Flash the latest firmware <https://www.raspberrypi.com/documentation/microcontrollers/debug-probe.html#updating-the-firmware-on-the-debug-probe>`_
2. `Connecting to the target Pico <https://www.raspberrypi.com/documentation/microcontrollers/debug-probe.html#getting-started>`_

Using a second Pi Pico as a debug probe
---------------------------------------
1. First, flash your second Pi Pico with the latest ``debugprobe_on_pico.uf2`` from
`debugprobe releases <https://github.com/raspberrypi/debugprobe/releases/latest>`_.

2. Connect the two Pico boards as follows:

- Debug connection

  - 2nd debug probe Pico GND ⭤ target Pico GND

  - 2nd debug probe Pico GP2 ⭤ target Pico SWCLK

  - 2nd debug probe Pico GP3 ⭤ target Pico SWDIO

- UART connection

  - 2nd debug probe Pico GP4/UART1 TX ⭤ target Pico GP1/UART0 RX

  - 2nd debug probe Pico GP5/UART1 RX ⭤ target Pico GP0/UART0 TX

.. tip::

   You can also connect the VSYS pins between the two boards to power both off
   of the USB connection to the debug probe Pico. Then you won't need to power
   the target Pico via its USB connection.

For more detailed instructions on using a debug probe Pico, including wiring
pictures, see ``Appendix A: Debugprobe`` of the
`Getting started with Raspberry Pi Pico
<https://datasheets.raspberrypi.com/pico/getting-started-with-pico.pdf>`_
guide.

-------------------
Additional Software
-------------------

.. _target-rp2-udev:

Setting up udev rules
=====================
On Linux, you may need to update your udev rules to access the device as a
normal user (not root).

Add the following rules to ``/etc/udev/rules.d/49-pico.rules`` or
``/usr/lib/udev/rules.d/49-pico.rules``. Create the file if it doesn't exist.

.. literalinclude:: /targets/rp2040/49-pico.rules
   :language: linuxconfig
   :start-at: # Raspberry

Then reload the rules:

.. code-block:: console

   sudo udevadm control --reload-rules
   sudo udevadm trigger

.. _target-rp2-installing-openocd:

OpenOCD (The Open On-Chip-Debugger)
===================================

OpenOCD supports a variety of embedded targets, and supports a variety of
programming (flashing) and other low-level debugging operations. It also acts
as a server for GDB to connect to for debugging higher-level languages like C
and C++.

You can obtain OpenOCD in many ways, including your operating system package
manager. Note that OpenOCD does not support the RP2350 as of the 0.12.0
release. However changes since that release have added support, and you may have
to built it from source if you need it.

An alternative is to get a fork of OpenOCD from the Raspberry Pi Foundation's
GitHub repositories, which does include RP2350 support, however their fork lags
behind the latest upstream sources.

- Prebuilt binaries: https://github.com/raspberrypi/pico-sdk-tools
- Source code: https://github.com/raspberrypi/openocd

.. note::

   If you are building OpenOCD from it source code, you can find some notes on
   the thread just below.

   https://forums.raspberrypi.com/viewtopic.php?p=2322149#p2322149

   The `cmsis-dap.cfg` driver support for the Debug Probe is optional, and is
   only included in the build by the `configure` step if your host's equivalent
   to the ``libusb-1.0-0-dev`` package is found.

The base command for using OpenOCD with a RP2x target is below, and without
any other commands (``-c`` option) it will just act as a server for GDB to
connect to.

.. code-block:: console

   $ openocd -f interface/cmsis-dap.cfg               \
             -f target/rp2040.cfg        # or rp2350  \
             -c "adapter speed 5000"

------------------
Common Build notes
------------------

MCU selection flags (``--config`` and ``--chip``)
=================================================
If your command requires a ``-config`` flag (e.g. ``--config=<mcu>``)
or a ``--chip`` flag (e.g. ``--chip <mcu>``) then you must replace
the ``<mcu>`` placeholder with one of these values:

* ``rp2040``
* ``rp2350``

For example, to build upstream Pigweed for a Pico 1 you run:

.. code-block:: console

   $ bazelisk build --config=rp2040 //...

Whereas to build upstream Pigweed for a Pico 2 you run:

.. code-block:: console

   $ bazelisk build --config=rp2350 //...

.. important::

   If the build command uses  target path that begins ``//targets/rp2040``,
   then you should use for both the RP2040 and the RP2350.

   Bug :bug:`449742221` tracks the effort to clean this up, so that the target
   path would become the more generic ``//targets/rp2``.

   When we originally created ``//targets/rp2040``, the RP2350 did not yet
   exist, and we didn't know there would be a future product that was mostly
   compatible, which is why the RP2350 uses the RP2040 target configuration,
   but is then modified by the ``--config`` and ``--chip`` flags where needed.

   If you forget, you will get a error that the target package dos not exist,
   which may look like:

   .. code-block:: console

      ERROR: no such package 'targets/rp2350/py': BUILD file not found in any
      of the following directories. Add a BUILD file to a directory to mark it
      as a package.

.. _target-rp2-upstream-build:

---------------
Bazel workflows
---------------

Building
========

This builds everything for <mcu>

.. code-block:: console

   $ bazelisk build --config=<mcu> //...

Flashing
========
Some modules have flash targets named "flash_<mcu>" which can be used to flash
a binary to the target, using a debug-probe or picopico connected to your host
via USB.

The ``--config`` does not need to be used for those.

.. code-block:: console

   $ bazelisk run //path/to:flash_<mcu>

For example:

.. code-block:: console

   $ bazelisk run //pw_system:flash_rp2350

If you need to flash in some other way, you will have to build the binary, and
flash it from the ``bazel-bin/`` build path.

.. _target-rp2-upstream-tests:

Run on-device tests
===================
.. _Updating the firmware on the Debug Probe: https://www.raspberrypi.com/documentation/microcontrollers/debug-probe.html#updating-the-firmware-on-the-debug-probe
.. _Getting Started: https://www.raspberrypi.com/documentation/microcontrollers/debug-probe.html#getting-started

#. Set up your hardware:

   .. tab-set::

      .. tab-item:: PicoPico
         :sync: picopico

         Connect the USB-Micro port of the **DEBUG PROBE** Pico
         to a USB port on your development host. Don't connect the
         **DEVICE UNDER TEST** Pico to anything.

      .. tab-item:: Debug Probe
         :sync: probe

         Make sure that your Debug Probe is running firmware version 2.0.1
         or later. See `Updating the firmware on the Debug Probe`_.

         Wire up your Pico to the Debug Probe as described in `Getting Started`_.

      .. tab-item:: Standalone
         :sync: standalone

         You can run Pigweed's tests with a single Pico and no additional
         hardware with the following limitations:

         * Tests will not be parallelized if more than one Pico is attached.

         * If the Pico crashes during a test, the failure will cascade into
           subsequent tests. You'll need to manually disconnect and re-connect
           the device to get it working again.

#. Start the test runner server:

   .. tab-set::

      .. tab-item:: PicoPico
         :sync: picopico

         .. code-block:: console

            # For the RP2040 use this:
            $ bazelisk run //targets/rp2040/py:unit_test_server -- --chip RP2040

            # For the RP2350 use the same target, but specify "--chip RP2350"
            $ bazelisk run //targets/rp2040/py:unit_test_server -- --chip RP2350

      .. tab-item:: Debug Probe
         :sync: probe

         .. code-block:: console

            # For the RP2040 use this:
            $ bazelisk run //targets/rp2040/py:unit_test_server -- --chip RP2040 --debug-probe-only

            # For the RP2350 use the same target, but specify "--chip RP2350"
            $ bazelisk run //targets/rp2040/py:unit_test_server -- --chip RP2350 --debug-probe-only

      .. tab-item:: Standalone
         :sync: standalone

         .. code-block:: console

            # For the RP2040 use this:
            $ bazelisk run //targets/rp2040/py:unit_test_server -- --chip RP2040

            # For the RP2350 use the same target, but specify "--chip RP2350"
            $ bazelisk run //targets/rp2040/py:unit_test_server -- --chip RP2350


#. Open another terminal and run the tests:

   .. code-block:: console

      $ bazelisk test --config=<mcu> //...

.. _target-rp2-upstream-gn:

----------------------------------
GN workflows (less maintained)
----------------------------------
The following guides may be outdated. We're keeping them around for Pigweed
contributors that are maintaining the upstream GN build system.

First-time setup
================
To build for a RP2 target, Pigweed must be set up to use FreeRTOS and the Pico
SDK HAL.

For the GN build, the supported repositories can be downloaded via
``pw package``, and then the build must be manually configured to point to the
locations the repositories were downloaded to.

When using Bazel, those dependencies are automatically installed.

.. warning::

   The packages downloaded do not include ``libusb-1.0`` as needed by
   ``picotool``.  If the ``picotool`` installation fails due to missing
   headers, it can be fixed by installing them manually.

   .. tab-set::

      .. tab-item:: Linux
         :sync: linux

         .. code-block:: console

            $ sudo apt-get install libusb-1.0-0-dev

         .. admonition:: Note
            :class: tip

            These instructions assume a Debian/Ubuntu Linux distribution.

      .. tab-item:: macOS
         :sync: macos

         .. code-block:: console

            $ brew install libusb
            $ brew install pkg-config

         .. admonition:: Note
            :class: tip

            These instructions assume a brew is installed and used for package
            management.

.. code-block:: console

   $ pw package install freertos
   $ pw package install pico_sdk
   $ pw package install picotool

   $ gn gen out --export-compile-commands --args="
       dir_pw_third_party_freertos=\"//environment/packages/freertos\"
       PICO_SRC_DIR=\"//environment/packages/pico_sdk\"
     "

.. tip::

   Instead of the ``gn gen out`` with args set on the command line above you can
   run:

   .. code-block:: console

      $ gn args out

   Then add the following lines to that text file:

   .. code-block::

      dir_pw_third_party_freertos = getenv("PW_PACKAGE_ROOT") + "/freertos"
      PICO_SRC_DIR = getenv("PW_PACKAGE_ROOT") + "/pico_sdk"


Building
========
.. tab-set::

   .. tab-item:: GN
      :sync: GN

      Once the Pico SDK is configured, the Pi Pico will build as part of the default
      GN build:

      .. code-block:: console

         $ ninja -C out

      The pw_system example is available as a separate build target:

      .. code-block:: console

         $ ninja -C out pw_system_demo

Running unit tests
==================
Unlike most other targets in Pigweed, the RP2040 uses RPC-based unit testing.
This makes it easier to fully automate on-device tests in a scalable and
maintainable way.

Step 1: Start test server
-------------------------
To allow Ninja to properly serialize tests to run on device, Ninja will send
test requests to a server running in the background. The first step is to launch
this server. By default, the script will attempt to automatically detect an
attached Pi Pico running an application with USB serial enabled or a Pi Debug
Probe, then use it for testing. To override this behavior, provide a custom
server configuration file with ``--server-config``.

.. code-block:: console

   $ python -m rp2040_utils.unit_test_server --chip RP2040

.. tip::

   If the server can't find any attached devices, ensure your Pi Pico is
   already running an application that utilizes USB serial.

.. Warning::

   If you connect or disconnect any boards, you'll need to restart the test
   server for hardware changes to take effect.

Step 2: Configure GN
--------------------
By default, this hardware target has incremental testing disabled. Enabling the
``pw_targets_ENABLE_RP2040_TEST_RUNNER`` build arg tells GN to send requests to
a running ``rp2040_utils.unit_test_server``.

.. code-block:: console

   $ gn args out
   # Modify and save the args file to use pw_target_runner.
   pw_targets_ENABLE_RP2040_TEST_RUNNER = true

Step 3: Build changes
---------------------
Now, whenever you run ``ninja -C out pi_pico``, all tests affected by changes
since the last build will be rebuilt and then run on the attached device.
Alternatively, you may use ``pw watch`` to set up Pigweed to trigger
builds/tests whenever changes to source files are detected.

--------
Flashing
--------

Using the mass-storage bootloader
=================================
Hold down the :kbd:`BOOTSEL` button when plugging in the Pico and it will appear
as a mass storage device. Copy the UF2 firmware image (for example,
``out/rp2040.size_optimized/obj/pw_system/system_example.uf2``) to
your Pico, and it will restart and run it.

.. tip::

   This is the simplest solution if you are fine with physically interacting
   with your Pico whenever you want to flash a new firmware image.

.. _target-rp2-flashing-with-openocd:

Flashing with OpenOCD
=====================
You will need (:ref:`OpenOCD<target-rp2-installing-openocd>`), and a Debug
Probe (:ref:`target-rp2-debug-probe`).

On Linux you will also need to follow :ref:`target-rp2-udev`.

You can then flash from the command line:

.. tab-set::

   .. tab-item:: RP2040
      :sync: RP2040

      .. code-block:: console

         $ openocd -f interface/cmsis-dap.cfg \
               -f target/rp2040.cfg -c "adapter speed 5000" \
               -c "program out/rp2040.size_optimized/obj/pw_system/bin/system_example.elf verify reset exit"

   .. tab-item:: RP2350
      :sync: RP2350

      .. code-block:: console

         $ openocd -f interface/cmsis-dap.cfg \
               -f target/rp2350.cfg -c "adapter speed 5000" \
               -c "program out/rp2350.size_optimized/obj/pw_system/bin/system_example.elf verify reset exit"

Typical output:

.. code-block:: none

   xPack Open On-Chip Debugger 0.12.0+dev-01312-g18281b0c4-dirty (2023-09-05-01:33)
   Licensed under GNU GPL v2
   For bug reports, read
      http://openocd.org/doc/doxygen/bugs.html
   Info : Hardware thread awareness created
   Info : Hardware thread awareness created
   adapter speed: 5000 kHz
   Info : Using CMSIS-DAPv2 interface with VID:PID=0x2e8a:0x000c, serial=415032383337300B
   Info : CMSIS-DAP: SWD supported
   Info : CMSIS-DAP: Atomic commands supported
   Info : CMSIS-DAP: Test domain timer supported
   Info : CMSIS-DAP: FW Version = 2.0.0
   Info : CMSIS-DAP: Interface Initialised (SWD)
   Info : SWCLK/TCK = 0 SWDIO/TMS = 0 TDI = 0 TDO = 0 nTRST = 0 nRESET = 0
   Info : CMSIS-DAP: Interface ready
   Info : clock speed 5000 kHz
   Info : SWD DPIDR 0x0bc12477, DLPIDR 0x00000001
   Info : SWD DPIDR 0x0bc12477, DLPIDR 0x10000001
   Info : [rp2040.core0] Cortex-M0+ r0p1 processor detected
   Info : [rp2040.core0] target has 4 breakpoints, 2 watchpoints
   Info : [rp2040.core1] Cortex-M0+ r0p1 processor detected
   Info : [rp2040.core1] target has 4 breakpoints, 2 watchpoints
   Info : starting gdb server for rp2040.core0 on 3333
   Info : Listening on port 3333 for gdb connections
   Warn : [rp2040.core1] target was in unknown state when halt was requested
   [rp2040.core0] halted due to debug-request, current mode: Thread
   xPSR: 0xf1000000 pc: 0x000000ee msp: 0x20041f00
   [rp2040.core1] halted due to debug-request, current mode: Thread
   xPSR: 0xf1000000 pc: 0x000000ee msp: 0x20041f00
   ** Programming Started **
   Info : Found flash device 'win w25q16jv' (ID 0x001540ef)
   Info : RP2040 B0 Flash Probe: 2097152 bytes @0x10000000, in 32 sectors

   Info : Padding image section 1 at 0x10022918 with 232 bytes (bank write end alignment)
   Warn : Adding extra erase range, 0x10022a00 .. 0x1002ffff
   ** Programming Finished **
   ** Verify Started **
   ** Verified OK **
   ** Resetting Target **
   shutdown command invoked

-----------------------
Connect with pw_console
-----------------------
Once the board has been flashed, you can connect to it and send RPC commands
via the Pigweed console:

.. tab-set::

   .. tab-item:: Bazel
      :sync: bazel

      .. code-block:: console

         # RP2040
         $ bazel run --config=rp2040 //pw_system:system_example_console

         # RP2350
         $ bazel run --config=rp2350 //pw_system:system_example_console

   .. tab-item:: GN
      :sync: gn

      .. code-block:: console

         # RP2040
         $ pw-system-console --device /dev/{ttyX} --baudrate 115200 \
             --token-databases \
               out/rp2040.size_optimized/obj/pw_system/bin/system_example.elf

         # RP2350
         $ pw-system-console --device /dev/{ttyX} --baudrate 115200 \
             --token-databases \
               out/rp2350.size_optimized/obj/pw_system/bin/system_example.elf

      Replace ``{ttyX}`` with the appropriate device on your machine. On Linux
      this may look like ``ttyACM0``, and on a Mac it may look like
      ``cu.usbmodem***``. If ``--device`` is omitted the first detected port
      will be used if there is only one. If multiple ports are detected an
      interactive prompt will be shown.

When the console opens, try sending an Echo RPC request. You should get back
the same message you sent to the device.

.. code-block:: pycon

   >>> device.rpcs.pw.rpc.EchoService.Echo(msg="Hello, Pigweed!")
   (Status.OK, pw.rpc.EchoMessage(msg='Hello, Pigweed!'))

You can also try out our thread snapshot RPC service, which should return a
stack usage overview of all running threads on the device in Host Logs.

.. code-block:: pycon

   >>> device.snapshot_peak_stack_usage()

Example output:

.. code-block::

   20220826 09:47:22  INF  PendingRpc(channel=1, method=pw.thread.ThreadSnapshotService.GetPeakStackUsage) completed: Status.OK
   20220826 09:47:22  INF  Thread State
   20220826 09:47:22  INF    5 threads running.
   20220826 09:47:22  INF
   20220826 09:47:22  INF  Thread (UNKNOWN): IDLE
   20220826 09:47:22  INF  Est CPU usage: unknown
   20220826 09:47:22  INF  Stack info
   20220826 09:47:22  INF    Current usage:   0x20002da0 - 0x???????? (size unknown)
   20220826 09:47:22  INF    Est peak usage:  390 bytes, 76.77%
   20220826 09:47:22  INF    Stack limits:    0x20002da0 - 0x20002ba4 (508 bytes)
   20220826 09:47:22  INF
   20220826 09:47:22  INF  ...

You are now up and running!

.. seealso::

   The :ref:`module-pw_console`
   :bdg-ref-primary-line:`module-pw_console-user_guide` for more info on using
   the pw_console UI.

------------------
Debugging with GDB
------------------
To interactively debug a Pico, first ensure you have
:ref:`OpenOCD <target-rp2-installing-openocd>`.

1. In one terminal window, start OpenOCD as a GDB server with the following
   command:

.. tab-set::

   .. tab-item:: RP2040
      :sync: RP2040

      .. code-block:: console

         $ openocd -f interface/cmsis-dap.cfg \
               -f target/rp2040.cfg -c "adapter speed 5000"


   .. tab-item:: RP2350
      :sync: RP2350

      .. code-block:: console

         $ openocd -f interface/cmsis-dap.cfg \
               -f target/rp2350.cfg -c "adapter speed 5000"

.. note::

   If you need to flash the target through some other software (such as a Bazel
   build), you will need to stop ``openocd`` as it will have exclusive access
   to the target.

2. In a second terminal window, connect to the open GDB server, passing the
   binary you will be debugging:

.. tab-set::

   .. tab-item:: RP2040
      :sync: RP2040

      .. code-block:: console

         $ arm-none-eabi-gdb -ex "target extended-remote :3333" \
            out/rp2040.size_optimized/obj/pw_system/bin/system_example.elf

   .. tab-item:: RP2350
      :sync: RP2350

      .. code-block:: console

         $ arm-none-eabi-gdb -ex "target extended-remote :3333" \
            out/rp2350.size_optimized/obj/pw_system/bin/system_example.elf

.. tip::

   You can pass in additional commands to run on the command line with
   additional ``-ex`` flags. Or you can stick multiple commands in a file and
   use ``-x <path>`` to execute them all.

   Some useful commands to add to startup:

   - ``-ex monitor reset halt`` --- Resets and halt after connecting

   - ``-ex monitor cortex_m vector_catch all`` --- Breaks on hardware exceptions,
     such as bus errors (bad memory access) and dividing by zero, as that does
     not otherwise happen by default!

   Note: both the RP2040 (ARM Cortex-M0+) and RP2350 (ARM Cortex-M33) are
   compatible with the cortex_m commands like the last one.


Quick GDB cheat-sheet
=====================
.. list-table::
   :widths: 2 1
   :header-rows: 1

   * - Action
     - shortcut / command
   * - Ask openocd to reset the running device, and breaking before running
     - ``monitor reset halt`` (or just ``mon reset halt``)
   * - Execute an arbitrary OpenOCD command in gdb
     - ``mon <cmd>``
   * - Break while running
     - :kbd:`Ctrl` + :kbd:`C`
   * - Continue execution until pause or breakpoint
     - ``continue`` (abbrev: ``c``)
   * - Set a breakpoint
     - ``break <name or address>`` (abbrev: ``b``)
   * - Set a watchpoint (break on write)
     - ``watch <name or *address>``
   * - Show backtrace
     - ``backtrace`` (abbrev: ``bt``)
   * - Switch context to backtrace frame N
     - ``frame <N>``
   * - Print a value
     - ``print <name>`` (abbrev: ``p``)
   * - Examine memory
     - ``x <name or address>``
   * - Examine 64 bytes of memory as hex
     - ``x/64bx <name or address>``
   * - Examine 64 bytes of memory as decimal 4-byte words
     - ``x/16wd <name or address>``

See also the `GDB Users Manual <https://sourceware.org/gdb/current/onlinedocs/gdb.html/>`_
