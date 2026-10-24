.. _module-pw_daq-backend-dhalsim:

=======
dhalsim
=======
Dhalsim is a DAQ and device simulation application which runs on the rp2 family
of Raspberry Pi microcontrollers. Currently, it supports running on the Pigweed
RP2350 HIL board and the Pico 2.

-------
daq-cli
-------
Flags
=====
These flags are added to ``daq-cli`` when the dhalsim backend is in use.

.. code-block:: output

   --dhalsim-config=<file>
                        (optional) A path to a JSON formatted configuration file
                        specifying the configuration options for interacting
                        with the Dhalsim application and hardware.
   --dhalsim-device=<path>
                        A path to the serial port used to communicate with the
                        Dhalsim hardware. Required if no config file specified.
   --dhalsim-baudrate=<baudrate>
                        The baud rate to use when communicating with the Dhalsim
                        application. Required if no config file specified.
   --dhalsim-token-db=<token-db>
                        (optional) The token database (e.g. ELF file) to use
                        when decoding log messages from the Dhalsim application.

Environment variables
=====================
* ``PW_DAQ_DHALSIM_CONFIG``: (optional) A path to a JSON formatted
  configuration file specifying the configuration options for interacting with
  the Dhalsim application and hardware.

.. warning::

   Flags will override environment variables.

JSON configuration file
=======================
The JSON configuration file supplied via ``--dhalsim-config`` or
``PW_DAQ_DHALSIM_CONFIG`` can specify serial connection settings and other
device configurations:

.. code-block:: json

   {
     "device": "/dev/ttyACM4",
     "baudrate": 115200,
     "token_db": "path/to/dhalsim.elf",
     "board": "pw_hil_sim"
   }

--------
Examples
--------
.. code-block:: console

   $ daq-cli create digital0 --direction=out --initial=low
   $ daq-cli create digital1 --direction=in --pull=up
   $ daq-cli set digital1.pull down
   $ daq-cli get digital1.pull
   $ daq-cli on digital1.change do digital0.write
   $ daq-cli do digital0.write high
   $ daq-cli do digital1.read
   $ daq-cli create uart0 --baudrate=115200 --loopback
   $ daq-cli do uart0.tx a
   $ daq-cli listen -n 1 uart0.rx
   $ daq-cli usage
   $ daq-cli dump -
   $ daq-cli clear
   $ daq-cli info
   $ daq-cli capabilities
   $ daq-cli reset
