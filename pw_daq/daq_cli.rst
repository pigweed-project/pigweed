.. _module-pw_daq-daq-cli:

=======
daq-cli
=======
.. pigweed-module-subpage::
   :name: pw_daq

The ``daq-cli`` tool is a command line interface for interacting with
``pw_daq``-supported DAQs.

------
Design
------
.. TODO(b/575921101): Add design documentation for daq-cli.

TBD

----------------------
Command-line interface
----------------------
The general form of a ``daq-cli`` call looks like this:

.. code-block:: console

   $ daq-cli [flags] <command> [command args]

Each command has different arguments, which are defined below in the
:ref:`Commands <module-pw_daq-daq-cli-commands>` section.

Flags
=====
These flags are supported by the top level ``daq-cli`` command.

.. code-block:: output

   -b <backend>, --backend=<backend>
                        (optional) Selected backend. Default: dhalsim.
   -c <file>, --config=<file>
                        (optional) Path to a JSON config file registering
                        daq-cli backends/plugins.

Environment variables
=====================
* ``PW_DAQ_BACKEND``: (optional) The selected backend. Default: dhalsim.

.. warning::

   Flags will override environment variables.

--------
Backends
--------
Backends are implementations which support a family of DAQ devices. They
implement the abstract backend interface necessary to provide the functionality
of ``pw_daq``. See :ref:`module-pw_daq-backends` for details on the backend
design, API, and supported backends.

.. _module-pw_daq-daq-cli-commands:

--------
Commands
--------
The command-line interface attempts to mimic the
:ref:`abstract DAQ interface <module-pw_daq-design-detailed-design>` as much as
possible, including providing the same set of commands:

**Block Commands:**

* ``create`` - Create a block.
* ``list`` - List all current blocks.
* ``destroy`` - Tear down the specified block.
* ``set`` - Set an attribute on a block.
* ``get`` - Get an attribute from a block.

**Endpoint Commands:**

* ``on`` - When the specified Event occurs, invoke the supplied Action.
* ``listen`` - Stream new Events from the given event endpoint.
* ``do`` - Invoke the specified Action.

**Log Commands:**

* ``dump`` - Dump all Event logs from the DAQ device.
* ``clear`` - Clear all Event logs.
* ``usage`` - Get stats on log usage and capacity.

**Device Commands:**

* ``info`` - List information about the connected DAQ device or backend.
* ``capabilities`` - List all capabilities of the DAQ.
* ``reset`` - Reset the DAQ device state.

Block commands
==============
Create
------
Create a new block. As soon as a block is created it starts generating Events.
See :ref:`Block creation <module-pw_daq-daq-cli-block-creation>` for more
details on ``block_args``.

.. code-block:: console

   $ daq-cli [flags] create [-f] <block> [block_args]

Supports the ``-f, --force`` flag, which forces creation of the block even if
it requires destroying a conflicting block (for example, if ``uart0`` and
``digital1`` use the same pin).

List
----
List all current blocks.

.. code-block:: console

   $ daq-cli [flags] list

Destroy
-------
Tear down a block (the opposite of ``create``).

.. code-block:: console

   $ daq-cli [flags] destroy <block>

Set
---
Set an attribute on the given block.

.. code-block:: console

   $ daq-cli [flags] set <block>.<attribute> <value>

Get
---
Get the current value of an attribute.

.. code-block:: console

   $ daq-cli [flags] get <block>.<attribute>

The attribute value is printed to stdout using the same ``<value>`` format as
in ``listen``.

Endpoint commands
=================
For details on the endpoints provided by each block, see
:ref:`module-pw_daq-design-block-details`.

On
--
When the specified Event occurs, invoke the specified Action. This may happen
directly on the DAQ itself or by handling in the backend script (the behavior
is backend dependent).

If the Action takes no input value, any output value from the Event is ignored
and the Action is invoked without input. Otherwise, if no arguments are
specified for the action, the backend will assume the Action input is the
output of the Event. The command will return an error before execution if the
types of input and output are different.

.. code-block:: console

   $ daq-cli [flags] on <block>.<event> do <block>.<action> [action_args]

Listen
------
Stream new Events from the specified event endpoint.

.. code-block:: console

   $ daq-cli [flags] listen [-n N] <block>.<event>

Supports the ``-n, --number`` flag with an argument ``N`` which is the number
of events to receive then exit. If no ``-n`` flag is specified, the command
will continue receiving events until ``^C`` is given.

Events are printed to stdout one per line in the format:

.. code-block:: output

   [<timestamp>] <block>.<endpoint>: <value>

``<value>`` is stringified based on the underlying type, so an integer is
written out as a number, a ``char`` is written out as a hex value + character
(e.g. ``0x61 'a'``).

When an event is valueless, the ``: <value>`` portion is omitted:

.. code-block:: output

   [<timestamp>] <block>.<endpoint>

Do
--
Invokes the specified action on the block. Returns any result returned from the
action.

.. code-block:: console

   $ daq-cli [flags] do <block>.<action> [action_args]

If an action returns ``success``, that result only sets the exit code of the
invocation and nothing is printed to stdout. So if
``daq-cli do uart0.tx 'a'`` fails to send the byte, the application will report
a non-zero exit code.

If an action returns a value (other than ``success``) as a result, that value
is printed to stdout using the same ``<value>`` format as in ``listen``.

Log commands
============
Dump
----
Read out all Event logs from the device and dump them to a file.

.. code-block:: console

   $ daq-cli [flags] dump <file>

If ``<file>`` is ``-``, the logs will be redirected to stdout.

The log output is ``.csv`` in the following format:

.. code-block:: output

   timestamp,endpoint,type,value
   1234,block1.event,int,123
   1235,block2.event,,

.. note::

   Events that do not have output values leave the type and value fields blank
   in the CSV.

Clear
-----
Clear the Event logs from the device.

.. code-block:: console

   $ daq-cli [flags] clear

Usage
-----
Print Event log usage statistics, such as log usage and capacity.

.. code-block:: console

   $ daq-cli [flags] usage

Device commands
===============
Info
----
Print information about the device and connection. Details can be found in
the :ref:`backend <module-pw_daq-backends>` documentation.

.. code-block:: console

   $ daq-cli [flags] info

Capabilities
------------
Print the device capabilities, including supported blocks and endpoints.

.. code-block:: console

   $ daq-cli [flags] capabilities

Reset
-----
Reset the configuration of the DAQ device. Destroy any created blocks and
reset to a "power on" state. May include power cycling the device.

.. code-block:: console

   $ daq-cli [flags] reset

.. _module-pw_daq-daq-cli-block-creation:

--------------
Block creation
--------------
The supported blocks are defined in :ref:`module-pw_daq-design-block-details`.
The documentation below is specific to the command-line interface.

When creating a block using the ``create`` command, a block-specific set of
``block_args`` can be passed on the console. These block arguments are used to
set the attributes and initial state of a block. For example, a UART block
needs to know what baud rate to communicate at and a DigitalIO block needs to
know what initial state to set an output pin.

All block arguments are implemented as flags with the attribute name as the
flag. Boolean (``true`` or ``false``) arguments are implemented as binary
``--[no]attribute`` flags.

A block argument can either be optional or required. It is required if it is
marked ``init`` and does not have a default value. See
:ref:`module-pw_daq-design-block-details` for more details.

Example: Digital
================
Creating a ``digital`` block uses the following format:

.. code-block:: output

   $ daq-cli [flags] create digital<N> --direction=DIR [--initial=INIT]
       [--pull=PULL]

         N
                        The index of the digital block to create. Must be
                        supported by both the backend and the specific DAQ
                        device.

         --direction=[in|out|opendrain]
                        The direction of the digital block.

         --initial=[high|active|low|inactive]
                        The initial state of the pin in output or open drain
                        mode, either "high" ("active" in open drain) or "low"
                        ("inactive" in open drain). Default: low (inactive in
                        open drain). Invalid in "in" mode.

         --pull=[up|down|none]
                        Pull-up or pull-down setting. Only valid in input or
                        open drain mode. Default: "none".

Example: UART
=============
Creating a ``uart`` block uses the following format:

.. code-block:: output

   $ daq-cli [flags] create uart<N> --baudrate=BAUDRATE [--databits=DATABITS]
       [--parity=PARITY] [--stopbits=STOPBITS] [--[no]loopback]

         N
                        The index of the UART block to create.

         --baudrate=BAUDRATE
                        The baud rate to configure. Not all baud rates may be
                        supported.

         --databits=[7|8|9]
                        The number of data bits per character. Default: 8.

         --parity=[even|odd|none]
                        The type of parity to insert into each character.
                        Default: none.

         --stopbits=[1|2]
                        The number of stop bits per character. Default: 1.

         --[no]loopback
                        Whether to automatically loop received characters back
                        to the transmit channel. Default: false.

--------
Examples
--------
.. code-block:: console

   $ daq-cli create digital0 --direction=out --initial=low
   $ daq-cli create digital1 --direction=in --pull=down
   $ daq-cli create uart1 --baudrate=115200 --loopback
   $ daq-cli on digital1.change do digital0.write
   $ daq-cli do uart1.tx 'a'
   $ daq-cli listen uart1.rx
   $ daq-cli destroy digital1
   $ daq-cli reset
