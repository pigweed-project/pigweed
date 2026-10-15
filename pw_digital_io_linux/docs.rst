.. _module-pw_digital_io_linux:

.. cpp:namespace-push:: pw::digital_io

===================
pw_digital_io_linux
===================
.. pigweed-module::
   :name: pw_digital_io_linux

``pw_digital_io_linux`` implements the :ref:`module-pw_digital_io` interface
using the `Linux userspace gpio-cdev interface
<https://www.kernel.org/doc/Documentation/ABI/testing/gpio-cdev>`_.

.. note::

   Currently only the v1 userspace ABI is supported (https://pwbug.dev/328268007).


-------------
API reference
-------------
The following classes make up the public API:

``LinuxDigitalIoChip``
======================
Represents an open handle to a GPIO *chip* (e.g.  ``/dev/gpiochip0``).

This can be created using an existing file descriptor
(``LinuxDigitalIoChip(fd)``) or by opening a given path
(``LinuxDigitalIoChip::Open(path)``).

``LinuxDigitalIn``
==================
Represents a single input line and implements :cc:`DigitalIn
<pw::digital_io::DigitalIn>`.

This is acquired by calling ``chip.GetInputLine()`` with an appropriate
``LinuxInputConfig``.

``LinuxDigitalInInterrupt``
===========================
Represents a single input line configured for interrupts and implements
:cc:`DigitalInInterrupt <pw::digital_io::DigitalInInterrupt>`.

This is acquired by calling ``chip.GetInterruptLine()`` with an appropriate
``LinuxInputConfig``.

``LinuxDigitalOut``
===================
Represents a single input line and implements :cc:`DigitalOut
<pw::digital_io::DigitalOut>`.

This is acquired by calling ``chip.GetOutputLine()`` with an appropriate
``LinuxOutputConfig``.

.. warning::

   The ``Disable()`` implementation only closes the GPIO handle file
   descriptor, relying on the underlying GPIO driver / pinctrl to restore
   the state of the line. This may or may not be implemented depending on
   the GPIO driver.


------
Guides
------
Example code to use GPIO pins:

Configure an input pin and get its state
========================================

.. literalinclude:: examples/input.cc
   :language: cpp
   :linenos:
   :lines: 15-


Configure an output pin and set its state
=========================================

.. literalinclude:: examples/output.cc
   :language: cpp
   :linenos:
   :lines: 15-


Configure an interrupt pin and handle events
============================================

.. literalinclude:: examples/interrupt.cc
   :language: cpp
   :linenos:
   :lines: 15-

.. _module-pw_digital_io_linux-cli:

----------------------
Command-Line Interface
----------------------
This module also provides a tool also named ``pw_digital_io_linux`` which
provides a basic command-line interface to the library. It provides the
following sub-commands:

``get``
=======
Configure a GPIO line as an input and get its value.

Usage:

.. code-block:: none

   get [-i] CHIP LINE

Options:

* ``-i``: Invert; configure as active-low.

Arguments:

* ``CHIP``: gpiochip path (e.g. ``/dev/gpiochip0``)
* ``LINE``: GPIO line number (e.g. ``1``)

``set``
=======
Configure a GPIO line as an output and set its value.

.. warning::

   After this process exits, the GPIO line could immediately return to its
   hardware-controlled default state (depending on the GPIO driver).

Usage:

.. code-block:: none

   set [-i] CHIP LINE VALUE

Options:

* ``-i``: Invert; configure as active-low.

Arguments:

* ``CHIP``: gpiochip path (e.g. ``/dev/gpiochip0``)
* ``LINE``: GPIO line number (e.g. ``1``)
* ``VALUE``: the value to set (``0`` = inactive or ``1`` = active)

.. _module-pw_digital_io_linux-cli-watch:

``watch``
=========
Configure a GPIO line as an input and watch for interrupt events.

By default, both rising and falling edges will trigger an event.
This can be changed via the ``-t`` option.

Usage:

.. code-block:: none

   watch [-i] [{-ta,-tb,-td}] CHIP LINE

Options:

* ``-i``: Invert; configure as active-low.
* ``-t``: Trigger for an interrupt:

   * ``-ta`` - activating edge
   * ``-tb`` - both edges (default)
   * ``-td`` - deactivating edge

Arguments:

* ``CHIP``: gpiochip path (e.g. ``/dev/gpiochip0``)
* ``LINE``: GPIO line number (e.g. ``1``)

.. cpp:namespace-pop::
