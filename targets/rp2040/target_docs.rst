.. _target-rp2040:

==============================
Raspberry Pi RP2040 and RP2350
==============================
.. _Raspberry Pi RP2040: https://www.raspberrypi.com/documentation/microcontrollers/silicon.html#rp2040
.. _Raspberry Pi RP2350: https://www.raspberrypi.com/documentation/microcontrollers/silicon.html#rp2350
.. _Raspberry Pi Pico: https://www.raspberrypi.com/products/raspberry-pi-pico/
.. _Raspberry Pi Pico 2: https://www.raspberrypi.com/products/raspberry-pi-pico-2/

This page explains Pigweed's support for the `Raspberry Pi RP2040`_ and
`Raspberry Pi RP2350`_ micro-controllers, and for
boards built on top of them, such as the `Raspberry Pi Pico`_ and
`Raspberry Pi Pico 2`_ (collectively we call them "RP2s", and just "Pico") .

.. important::

  Some parts of Pigweed such as module names and other configuration files
  still identify themselves as for the "rp2040", as that was first released,
  and so first supported. However the rp2350 is similar enough that the two can
  share most source code, so an rp2350 build will use them.

  That said, you do have to specify the correct target name when using a
  ``--config`` or ``--chip`` argument to certain commands, as there are details
  such as the memory map that are incompatible.


Intended usage
--------------
Pigweed's :ref:`mission <docs-mission>` is to help large teams develop embedded
systems sustainably, robustly, and rapidly. Our support for the RP2s revolves
around making it easier for these teams to develop complex prototypes or mass
market products on top of the RP2s. Our main goals are to make it easy to:

.. _C/C++ SDK: https://www.raspberrypi.com/documentation/microcontrollers/c_sdk.html

* Do your end-to-end development lifecycle (building, flashing, testing, etc.)
  in Bazel.
* Author your embedded system in portable C++. You can write most of your
  system on top of Pigweed's hardware-agnostic
  :ref:`modules <docs-glossary-module>`. If there's anything not covered by
  Pigweed's modules, you can fallback to using the official `C/C++ SDK`_
  directly.

.. _MicroPython SDK: https://www.raspberrypi.com/documentation/microcontrollers/micropython.html

If you're building relatively simple stuff solely for the RP2s,
and just want to get everything working very quickly and easily, then Pigweed
probably won't be a good fit for you. You'll probably be happier with the
official `MicroPython SDK`_ or the official `C/C++ SDK`_.

--------
Examples
--------
.. _Sense showcase: https://pigweed.googlesource.com/pigweed/showcase/sense/

The `Sense showcase`_ is our work-in-progress demonstration product for using
Pigweed with a Pico or Pico 2.

-------
Modules
-------
Most Pigweed :ref:`modules <docs-glossary-module>` work on all hardware
platforms. A few areas such as I2C require integrating with a specific Pigweed
module.

.. csv-table::
   :header: "Description", "Module"

   "Time primitives", ":ref:`module-pw_chrono_rp2040`"
   "GPIO (digital I/O)", ":ref:`module-pw_digital_io_rp2040`"
   "I2C", ":ref:`module-pw_i2c_rp2040`"
   "SPI", ":ref:`module-pw_spi_rp2040`"
   "Basic I/O for bringup and debugging", ":ref:`module-pw_sys_io_rp2040`"

----------------
Unit test runner
----------------
A unit test runner utility is provided to manually run unit tests on device. It
can be invoked using:

.. code-block:: shell

   bazelisk run //targets/rp2040/py:rpc_unit_test_runner -- --chip RP2350 path/to/unit-test-binary

Because using `bazel run` changes the working directory to the bazel runfiles
location, if the path given for the binary is relative, the script will
automatically append `BUILD_WORKING_DIRECTORY` to relative binary paths. If
`BUILD_WORKING_DIRECTORY` is not set (the script is not invoked with
`bazel run`) the path will not be modified.

This allows using syntax such as:

.. code-block:: shell

   bazelisk run //targets/rp2040/py:rpc_unit_test_runner -- --chip RP2350 ./bazel-out/rp2350-fastbuild/bin/pw_containers/vector_test.cc_test

.. toctree::
   :maxdepth: 1
   :hidden:

   Upstream Pigweed <upstream>
