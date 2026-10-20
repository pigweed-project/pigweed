.. _target-apollo4:

===========================
Ambiq Apollo4
===========================
-----
Setup
-----
To use this target, Pigweed must be set up to use the `AmbiqSuite SDK`_ HAL
for the Apollo4 series which can be downloaded from the Ambiq website.

.. _AmbiqSuite SDK: https://ambiq.com/apollo4-blue-plus

Once the AmbiqSuite SDK package has been downloaded and extracted, the user
needs to set ``dir_pw_third_party_ambiq_SDK`` build arg to the location of
extracted directory:

.. code-block:: console

   $ gn args out

Then add the following lines to that text file:

.. code-block::

   # Path to the extracted AmbiqSuite SDK package.
   dir_pw_third_party_ambiq_SDK = "/path/to/AmbiqSuite_R4.3.0"

Usage
=====
The Apollo4 is configured to output logs and test results over the UART to an
on-board J-Link Debug Probe (Virtual COM Port) at a baud rate of 115200.

Once the AmbiqSuite SDK is configured, the unit tests for the Apollo4 board
can be build with a command:

.. code-block:: console

   $ ninja -C out apollo4

If using out as a build directory, tests will be located in out/apollo4/obj/[module name]/[test_name].elf.

Flashing using SEGGER's J-Link
==============================
Flashing the Apollo4 board can be done using the J-Flash Lite GUI program or from
a command line using ``JLinkExe`` program. The latter requires a script which
describes the steps of programming. Here is an example bash script to flash
an Apollo4 board using ``JLinkExe`` program:

.. code-block:: shell

   #!/bin/bash
   function flash_jlink()
   {
      local TMP_FLASH_SCRIPT=/tmp/gdb-flash.txt

      cat > $TMP_FLASH_SCRIPT <<- EOF
         r
         h
         loadfile $1
         r
         q
      EOF

      JLinkExe -NoGui 1 -device AMAP42KK-KBR -if SWD -speed 4000 -autoconnect 1 -CommanderScript $TMP_FLASH_SCRIPT

      rm "$TMP_FLASH_SCRIPT"
   }

   flash_jlink $@

Then call this script:

.. code-block:: console

   $ bash ./flash_amap4.sh ./out/apollo4_debug/obj/pw_log/test/basic_log_test.elf

In this case the basic log test is debugged, but substitute your own ELF file.

Debugging
=========
Debugging can be done using the on-board J-Link Debug Probe. First you need to
start ``JLinkGDBServer`` and connect to the on-board J-Link Debug Probe.

.. code-block:: console

   $ JLinkGDBServer -select USB \
   > -device AMAP42KK-KBR \
   > -endian little \
   > -if SWD \
   > -speed 4000 \
   > -noir -LocalhostOnly \
   > -singlerun \
   > -nogui \
   > -excdbg \
   > -rtos GDBServer/RTOSPlugin_FreeRTOS.dylib

The ``-rtos`` option is for `Thread Aware Debugging`_.

.. _Thread Aware Debugging: https://www.segger.com/products/debug-probes/j-link/tools/j-link-gdb-server/thread-aware-debugging/

Then on the second terminal window use ``arm-none-eabi-gdb`` to load an executable
into the target, debug, and run it.

.. code-block:: console

   $ arm-none-eabi-gdb -q out/apollo4_debug/obj/pw_log/test/basic_log_test.elf

This can be combined with a simple bash script. Here is an example of one:

.. code-block:: shell

   #!/bin/bash

   function debug_jlink()
   {
      local TMP_GDB_SCRIPT=/tmp/gdb-debug.txt

      # Create GDB script.

      cat > $TMP_GDB_SCRIPT <<- EOF

      # Backtrace all threads.

      define btall
        thread apply all backtrace
      end

      target remote localhost:2331
      load
      monitor reset
      monitor halt
      b pw_boot_Entry

      EOF

      # Start GDB server.

      set -m
      JLinkGDBServer -select USB       \
                 -device AMAP42KK-KBR  \
                 -endian little        \
                 -if SWD               \
                 -speed 4000           \
                 -noir -LocalhostOnly  \
                 -singlerun            \
                 -nogui                \
                 -excdbg               \
                 -rtos GDBServer/RTOSPlugin_FreeRTOS.dylib &
      set +m

      # Debug program.

      arm-none-eabi-gdb -q $1 -x $TMP_GDB_SCRIPT

      rm "$TMP_GDB_SCRIPT"
   }

   debug_jlink $@

Then call this script:

.. code-block:: console

   $ bash ./debug_amap4.sh ./out/apollo4_debug/obj/pw_log/test/basic_log_test.elf

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
The following table lists all modules related to Ambiq.

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_sys_io_ambiq_sdk`", ":bdg-primary:`stable`", "", ""

.. Do not edit this toctree. To add non-module docs to this category,
.. create a separate toctree outside the categories-start /
.. categories-end block.
.. toctree::
   :maxdepth: 1
   :hidden:

   self
   System I/O <../../pw_sys_io_ambiq_sdk/docs>

.. +----------------------------------------------------------+
.. | END OF AUTO-GENERATED SECTION                            |
.. +----------------------------------------------------------+
.. categories-end

.. toctree::
   :maxdepth: 1
   :hidden:

   ../apollo4_pw_system/target_docs
