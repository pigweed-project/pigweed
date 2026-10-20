.. _target-arduino:

=======
Arduino
=======

This target supports building Pigweed on a few Arduino cores.

.. seealso::
   There are a few caveats when running Pigweed on top of the Arduino API. See
   :ref:`module-pw_arduino_build` for details.

----------------
Supported Boards
----------------

Currently only Teensy 4.x and 3.x boards are supported.

+------------------------------------------------------------------+-------------------------------------------------------------------+-----------+----------+-------------+
| Core                                                             | Board Name                                                        | Compiling | Flashing | Test Runner |
+==================================================================+===================================================================+===========+==========+=============+
| `teensy <https://www.pjrc.com/teensy/td_download.html>`_         | `Teensy 4.1 <https://www.pjrc.com/store/teensy41.html>`_          | ✓         | ✓        | ✓           |
+------------------------------------------------------------------+-------------------------------------------------------------------+-----------+----------+-------------+
| `teensy <https://www.pjrc.com/teensy/td_download.html>`_         | `Teensy 4.0 <https://www.pjrc.com/store/teensy40.html>`_          | ✓         | ✓        | ✓           |
+------------------------------------------------------------------+-------------------------------------------------------------------+-----------+----------+-------------+
| `teensy <https://www.pjrc.com/teensy/td_download.html>`_         | `Teensy 3.6 <https://www.pjrc.com/store/teensy36.html>`_          | ✓         | ✓        | ✓           |
+------------------------------------------------------------------+-------------------------------------------------------------------+-----------+----------+-------------+
| `teensy <https://www.pjrc.com/teensy/td_download.html>`_         | `Teensy 3.5 <https://www.pjrc.com/store/teensy35.html>`_          | ✓         | ✓        | ✓           |
+------------------------------------------------------------------+-------------------------------------------------------------------+-----------+----------+-------------+
| `teensy <https://www.pjrc.com/teensy/td_download.html>`_         | `Teensy 3.2 <https://www.pjrc.com/store/teensy32.html>`_          | ✓         | ✓        | ✓           |
+------------------------------------------------------------------+-------------------------------------------------------------------+-----------+----------+-------------+
| `arduino-samd <https://github.com/arduino/ArduinoCore-samd>`_    | `Arduino Zero <https://store.arduino.cc/usa/arduino-zero>`_       |           |          |             |
+------------------------------------------------------------------+-------------------------------------------------------------------+-----------+----------+-------------+
| `arduino-sam <https://github.com/arduino/ArduinoCore-sam>`_      | `Arduino Due <https://store.arduino.cc/usa/due>`_                 |           |          |             |
+------------------------------------------------------------------+-------------------------------------------------------------------+-----------+----------+-------------+
| `adafruit-samd <https://github.com/adafruit/ArduinoCore-samd>`_  | `Adafruit Feather M0 <https://www.adafruit.com/?q=feather+m0>`_   |           |          |             |
+------------------------------------------------------------------+-------------------------------------------------------------------+-----------+----------+-------------+
| `adafruit-samd <https://github.com/adafruit/ArduinoCore-samd>`_  | `Adafruit SAMD51 Boards <https://www.adafruit.com/category/952>`_ |           |          |             |
+------------------------------------------------------------------+-------------------------------------------------------------------+-----------+----------+-------------+
| `stm32duino <https://github.com/stm32duino/Arduino_Core_STM32>`_ |                                                                   |           |          |             |
+------------------------------------------------------------------+-------------------------------------------------------------------+-----------+----------+-------------+

-----
Setup
-----

You must first install an Arduino core or let Pigweed know where you have cores
installed using the ``pw_arduino_build_CORE_PATH`` build arg.

Installing Arduino Cores
========================

The ``arduino_builder`` utility can install Arduino cores automatically. It's
recommended to install them to into ``third_party/arduino/cores/``.

.. code-block:: console

   # Setup pigweed environment.
   $ . ./activate.sh
   # Install an arduino core, only teensy is supported
   $ pw package install teensy

--------
Building
--------
To build for this Pigweed target, simply build the top-level "arduino" Ninja
target. You can set Arduino build options using ``gn args out`` or by running:

.. code-block:: console

   $ gn gen out --args="
     pw_arduino_build_CORE_PATH = \"//environment/packages\"
     pw_arduino_build_CORE_NAME = \"teensy\"
     pw_arduino_build_PACKAGE_NAME = \"avr/1.58.1\"
     pw_arduino_build_BOARD = \"teensy40\"
     pw_arduino_build_MENU_OPTIONS=[\"menu.usb.serial\", \"menu.keys.en-us\"]
     "

On a Windows machine it's easier to run:

.. code-block:: console

   $ gn args out

That will open a text file where you can paste the args in:

.. code-block:: text

   pw_arduino_build_CORE_PATH = "//environment/packages"
   pw_arduino_build_CORE_NAME = "teensy"
   pw_arduino_build_PACKAGE_NAME = "avr/1.58.1"
   pw_arduino_build_BOARD = "teensy40"
   pw_arduino_build_MENU_OPTIONS = ["menu.usb.serial", "menu.keys.en-us"]

Save the file and close the text editor.

Then build with:

.. code-block:: console

   $ ninja -C out arduino

To see supported boards and Arduino menu options for a given core:

.. code-block:: console

   $ arduino_builder --arduino-package-path ./environment/packages/teensy \
   > --arduino-package-name avr/1.58.1 \
   > list-boards

.. code-block:: text

   Board Name  Description
   teensy41    Teensy 4.1
   teensy40    Teensy 4.0
   teensy36    Teensy 3.6
   teensy35    Teensy 3.5
   teensy31    Teensy 3.2 / 3.1

You may wish to set different arduino build options in
``pw_arduino_build_MENU_OPTIONS``. Run this to see what's available for your core:

.. code-block:: console

   $ arduino_builder --arduino-package-path ./environment/packages/teensy \
   > --arduino-package-name avr/1.58.1 \
   > list-menu-options \
   > --board teensy40

That will show all menu options that can be added to ``gn args out``.

.. code-block:: text

   All Options
   ----------------------------------------------------------------
   menu.usb.serial             Serial
   menu.usb.serial2            Dual Serial
   menu.usb.serial3            Triple Serial
   menu.usb.keyboard           Keyboard
   menu.usb.touch              Keyboard + Touch Screen
   menu.usb.hidtouch           Keyboard + Mouse + Touch Screen
   menu.usb.hid                Keyboard + Mouse + Joystick
   menu.usb.serialhid          Serial + Keyboard + Mouse + Joystick
   menu.usb.midi               MIDI
   ...

   Default Options
   --------------------------------------
   menu.usb.serial             Serial
   menu.speed.600              600 MHz
   menu.opt.o2std              Faster
   menu.keys.en-us             US English

-------
Testing
-------
When working in upstream Pigweed, building this target will build all Pigweed
modules' unit tests.  These tests can be run on-device in a few different ways.

Run a unit test
===============
If using ``out`` as a build directory, tests will be located in
``out/arduino_debug/obj/[module name]/[test_name].elf``.

Tests can be flashed and run using the `arduino_unit_test_runner` tool. Here is
a sample bash script to run all tests on a Linux machine.

.. code-block:: shell

   #!/bin/bash
   gn gen out --export-compile-commands \
       --args='pw_arduino_build_CORE_PATH="environment/packages"
               pw_arduino_build_CORE_NAME="teensy"
               pw_arduino_build_PACKAGE_NAME="avr/1.58.1"
               pw_arduino_build_BOARD="teensy40"
               pw_arduino_build_MENU_OPTIONS=["menu.usb.serial", "menu.keys.en-us"]' && \
     ninja -C out arduino

   for f in $(find out/arduino_debug/obj/ -iname "*.elf"); do
       arduino_unit_test_runner --verbose \
           --config-file ./out/arduino_debug/gen/arduino_builder_config.json \
           --upload-tool teensyloader \
           $f
   done

Using the test server
=====================

Tests may also be run using the `pw_arduino_use_test_server = true` GN arg.
The server must be run with an `arduino_builder` config file so it can locate
the correct Arduino core, compiler path, and Arduino board used.

.. code-block:: console

   $ arduino_test_server --verbose \
   > --config-file ./out/arduino_debug/gen/arduino_builder_config.json

.. TODO(tonymd): Flesh out this section similar to the stm32f429i target docs.

Flashing Known Issues
=====================

Teensy Boards
-------------

By default Teensyduino uses the `Teensy Loader Application
<https://www.pjrc.com/teensy/loader.html>`_ which has a couple limitations:

- Requires a GUI (or X11 on Linux).
- Can only flash one board at a time.

-----------------
GN Target Example
-----------------

Here is an example `pw_executable` gn rule that includes some Teensyduino
libraries.

.. code-block:: text

   import("//build_overrides/pigweed.gni")
   import("$dir_pw_arduino_build/arduino.gni")
   import("$dir_pw_build/target_types.gni")

   _library_args = [
     "--library-path",
     rebase_path(arduino_core_library_path, root_build_dir),
     "--library-names",
     "Time",
     "Wire",
   ]

   pw_executable("my_app") {
     # All Library Sources
     _library_c_files = exec_script(
             arduino_builder_script,
             arduino_show_command_args + _library_args + [
               "--library-c-files"
             ],
             "list lines")
     _library_cpp_files = exec_script(
             arduino_builder_script,
             arduino_show_command_args + _library_args + [
               "--library-cpp-files"
             ],
             "list lines")

     sources = [ "main.cc" ] + _library_c_files + _library_cpp_files

     deps = [
       "$dir_pw_hex_dump",
       "$dir_pw_log",
       "$dir_pw_string",
     ]

     include_dirs = exec_script(arduino_builder_script,
                                arduino_show_command_args + _library_args +
                                    [ "--library-include-dirs" ],
                                "list lines")

     # Required for using Arduino.h and any Arduino API functions
     remove_configs = [ "$dir_pw_build:strict_warnings" ]
     deps += [ "$pw_external_arduino:arduino_core_sources" ]
   }

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
The following table lists all modules related to Arduino.

.. csv-table::
   :header: "Name", "Status", "Description", "Languages"

   ":ref:`module-pw_arduino_build`", ":bdg-primary:`stable`", "Main function wrapper and build utilities for Arduino", "C++, GN"
   ":ref:`module-pw_sys_io_arduino`", ":bdg-primary:`stable`", "", ""

.. Do not edit this toctree. To add non-module docs to this category,
.. create a separate toctree outside the categories-start /
.. categories-end block.
.. toctree::
   :maxdepth: 1
   :hidden:

   self
   Build <../../pw_arduino_build/docs>
   System I/O <../../pw_sys_io_arduino/docs>

.. +----------------------------------------------------------+
.. | END OF AUTO-GENERATED SECTION                            |
.. +----------------------------------------------------------+
.. categories-end
