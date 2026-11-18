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

The ``pw_daq`` architecture is structured into decoupled components spanning
configuration management, backend plugin registration, an abstract hardware
backend interface with concrete driver implementations, a high-level Python
client library, and the ``daq-cli`` command runner:

* **Configuration & Registry** (``pw_daq.config``,
  ``pw_daq.backend_registry``): ``DaqConfig`` discovers and parses layered YAML
  configuration files according to schema precedence. Backend plugins are
  registered with ``BackendRegistry``, allowing backend classes to be loaded
  dynamically and instantiated with device-specific options.
* **Backend Abstraction** (``pw_daq.backend``): The abstract ``DaqBackend`` base
  class defines the lifecycle (``open``, ``close``, ``configure``) and unified
  hardware commands (``create``, ``destroy``, ``list``, ``get``, ``set``,
  ``on``, ``listen``, ``do``, ``dump``, ``clear``, ``info``, ``capabilities``,
  ``reset``).
* **Hardware Backends** (``pw_daq.backends.*``): Concrete subclasses of
  ``DaqBackend`` implement target-specific communication:

  * ``MockBackend``: In-memory simulation and test harness.
  * ``DhalsimBackend``: Connects to Pigweed RPC firmware targets (running
    ``pw_rpc`` DAQ services) over serial or simulated transports.

* **High-Level DAQ Client** (``pw_daq.lib``, ``pw_daq.common``): ``Daq`` wraps
  an active ``DaqBackend``, exposing object-oriented ``Block`` abstractions
  (such as ``DigitalBlock`` and ``UartBlock``) and strongly typed endpoints
  (``Attribute``, ``Signal``, and ``Action``) addressable via ``BlockRef`` and
  ``EndpointRef``.
* **CLI Execution Layer** (``pw_daq.cli``): Parses command-line invocations,
  loads configurations, initializes logging and device backends, and dispatches
  user commands.

.. mermaid::

   classDiagram
      direction TB

      namespace "daq-cli" {
         class DaqConfig {
            +load_config(stage)
            +get_device_config(name) Mapping
            +options() dict
            +devices() dict
         }

         class BackendRegistry {
            +register(name, target)
            +create_backend(name, config) DaqBackend
            +backend_names() Iterator~str~
         }
      }

      namespace backends {
         class DaqBackend {
            <<abstract>>
            +name()* str
            +configure(config) void
            +open() void
            +close() void
            +create(block_ref, attrs)* BlockRef
            +destroy(block_ref)* void
            +list()* Iterable~BlockRef~
            +get(attr_ref)* Value
            +set(attr_ref, value)* void
            +on(signal_ref, action_ref, arg)* void
            +listen(signal_ref, count, timeout)* EventStream
            +do(action_ref, arg)* ActionResult
            +dump()* Iterable~EventRef~
            +clear()* void
            +info()* Mapping
            +capabilities()* Mapping
            +reset()* void
         }

         class MockBackend {
            +configure(config) void
            +create(block_ref, attrs) BlockRef
            +destroy(block_ref) void
            +get(attr_ref) Value
            +set(attr_ref, value) void
            +on(signal_ref, action_ref, arg) void
            +listen(signal_ref, count, timeout) EventStream
            +do(action_ref, arg) ActionResult
         }

         class DhalsimBackend {
            -connection: DeviceConnection
            -device: Device
            -sim: Dhalsim
            +configure(config) void
            +open() void
            +close() void
            +create(block_ref, attrs) BlockRef
            +destroy(block_ref) void
            +get(attr_ref) Value
            +set(attr_ref, value) void
            +on(signal_ref, action_ref, arg) void
            +listen(signal_ref, count, timeout) EventStream
            +do(action_ref, arg) ActionResult
         }
      }

      namespace "daq-lib" {
         class Daq {
            -backend: DaqBackend
            +open() void
            +close() void
            -_bind(ref)
            +digital(index, attrs) DigitalBlock
            +uart(index, attrs) UartBlock
            +create(block, attrs) Block
            +list() list~Block~
            +destroy(block) void
            +set(attr, value) void
            +get(attr) Value
            +on(signal, action, arg) void
            +listen(signal, count, timeout) EventStream
            +wait(signal, timeout, pred) Event
            +do(action, arg) ActionResult
            +dump() Iterable~Event~
            +clear() void
            +info() Mapping
            +capabilities() Mapping
            +reset() void
         }

         class Block {
            -_ref: BlockRef
            -backend: DaqBackend
            +backend() DaqBackend
            +create() Block
            +destroy() void
            +attribute(name) Attribute
            +signal(name) Signal
            +action(name) Action
         }

         class DigitalBlock {
            +pin: Attribute
            +direction: Attribute
            +initial: Attribute
            +pull: Attribute
            +change: Signal
            +risingedge: Signal
            +fallingedge: Signal
            +write: Action
            +toggle: Action
            +read: Action
         }

         class UartBlock {
            +baudrate: Attribute
            +databits: Attribute
            +parity: Attribute
            +stopbits: Attribute
            +loopback: Attribute
            +rx: Signal
            +sent: Signal
            +tx: Action
         }

         class Endpoint {
            -block: Block
            -name: str
            -spec: EndpointSpec
            -_ref: EndpointRef
            +block() Block
            +name() str
            +spec() EndpointSpec
         }

         class Attribute {
            +get() Value
            +set(value) void
         }

         class Signal {
            +listen(count, timeout) EventStream
            +wait(timeout, pred) Event
            +on(action, arg) void
         }

         class Action {
            +do(arg) ActionResult
         }

         class Event {
            +endpoint: Signal
            +timestamp_ns: int
            +value: Value
         }
      }

      namespace common {
         class BlockRef {
            +block_type: BlockType
            +index: int
            +name() str
         }

         class EndpointRef {
            <<abstract>>
            +block: BlockRef
            +endpoint()* str
         }

         class AttributeRef
         class SignalRef
         class ActionRef

         class EventRef {
            +endpoint: SignalRef
            +timestamp_ns: int
            +value: Value
         }

         class ActionResult {
            +success: bool
            +value: Value
         }
      }

      %% Backend inheritance
      DaqBackend <|-- MockBackend : implements
      DaqBackend <|-- DhalsimBackend : implements

      %% Configuration & Registry instantiation
      DaqConfig ..> BackendRegistry : validates backends
      BackendRegistry ..> DaqBackend : instantiates

      %% High-level DAQ wrapping backend
      Daq *-- DaqBackend : delegates to
      Daq ..> Block : manages
      Block o-- DaqBackend : references

      %% Block hierarchy and endpoints
      Block <|-- DigitalBlock : extends
      Block <|-- UartBlock : extends
      Block *-- Endpoint : contains

      Endpoint <|-- Attribute : extends
      Endpoint <|-- Signal : extends
      Endpoint <|-- Action : extends

      %% Reference associations
      BlockRef <-- Block : identifies
      EndpointRef <-- Endpoint : identifies

      EndpointRef <|-- AttributeRef : extends
      EndpointRef <|-- SignalRef : extends
      EndpointRef <|-- ActionRef : extends

      %% Event & Action results
      Signal ..> Event : streams
      Action ..> ActionResult : produces

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

   -d <device>, --device=<device>
                        (optional) Selected configured device name.
   -c <file>, --config-file=<file>
                        (optional) Path to a fully specified YAML config file
                        overriding all other configuration files.
   -v, --verbose
                        (optional) Increase log verbosity.

Environment variables
=====================
* ``PW_DAQ_CONFIG``: (optional) Path to a YAML configuration file overriding
  project and user configuration files. Fails with an error if the file is
  missing, empty, unreadable, or malformed.

.. warning::

   Flags will override environment variables.

.. _module-pw_daq-daq-cli-configuration:

-------------
Configuration
-------------
``pw_daq`` and ``daq-cli`` can be configured through YAML configuration files,
environment variables, and command-line flags.

Configuration methods and precedence
====================================
Configuration files are loaded using
``pw_config_loader.yaml_config_loader_mixin.YamlConfigLoaderMixin`` in the
following order:

1. **Project configuration** (``$PW_PROJECT_ROOT/.pw_daq.yaml``): Shared,
   checked-in project defaults.
2. **Project user configuration** (``$PW_PROJECT_ROOT/.pw_daq.user.yaml``):
   Git-ignored local overrides specific to the current repository checkout.
3. **Global user configuration** (``~/.pw_daq.yaml``): User-level settings in
   the home directory that apply across all checkouts and projects.
4. **Environment variable configuration** (``PW_DAQ_CONFIG``): If set to a
   non-empty path, loads that YAML file exclusively in place of the project
   and user configuration files above.
5. **Explicit configuration file** (``-c <file>`` /
   ``--config-file=<file>``): If specified on the command line, overrides all
   other configuration files (no project, user, or ``PW_DAQ_CONFIG`` files are
   processed). Because no other files are loaded, ``--config-file`` must point
   to a complete configuration file for the target device.
6. **Command-line flags**: Command-line flags (such as ``--device`` or
   ``--verbose``) take the highest precedence.

Default configuration files (1--3) are optional: missing files are skipped, and
empty, unreadable, or malformed files log a warning and are skipped without
discarding earlier valid layers. Explicit configuration overrides
(``PW_DAQ_CONFIG`` and ``-c`` / ``--config-file``) replace default file loading
and fail immediately with an error if the specified file is missing, empty,
unreadable, or malformed.

Configuration schema
====================
All ``pw_daq`` YAML configuration sources (``.pw_daq.yaml``,
``.pw_daq.user.yaml``, ``~/.pw_daq.yaml``, ``PW_DAQ_CONFIG``, and
``--config-file``) follow the same schema with ``config_title: pw_daq``:

* ``options``: A mapping of general ``pw_daq`` configuration options:

  * ``default_device``: (optional) Name of the configured device to use when
    ``-d`` / ``--device`` is not specified on the command line.

* ``devices``: A mapping of device names to device configuration objects
  describing available hardware targets:

  * ``backend``: Name of the registered backend used by this device (built-in,
    passed programmatically via ``extra_backends``, or installed via the
    ``pw_daq.backends`` Python entry-point group; see
    :ref:`module-pw_daq-backends`).
  * ``dev``: (optional) Connection settings for communicating with the physical
    or simulated target (such as ``type``, ``path``, and ``baudrate``). Omitting
    ``dev`` is valid for shared project defaults (such as ``.pw_daq.yaml``) and
    offline commands (``config`` and ``capabilities``), while backends that
    require a device connection (such as ``dhalsim``) return an error when
    opening a connection if ``dev`` is not configured.
  * ``capabilities``: Inline backend-specific mapping describing the device's
    supported virtual blocks, real hardware resources, virtual-to-real resource
    mappings, and Backend Defined (``BED``) configuration options (see
    :ref:`module-pw_daq-design-capabilities`).
  * Additional backend-specific fields (such as ``token_db`` for ``dhalsim``).

.. code-block:: yaml

   config_title: pw_daq

   options:
     default_device: pico2
   devices:
     pico2:
       backend: dhalsim
       dev:
         type: serial
         path: /dev/cu.usbmodem1113301
         baudrate: 115200
       token_db: $PW_PROJECT_ROOT/bazel-bin/pw_dhalsim/rp2350_dhalsim
       capabilities:
         blocks:
           - digital0
           - digital1
           - uart0
         resources:
           - gp4
           - gp5
           - gp14
           - gp21
         mappings:
           p0: gp14
           p1: gp21
           p2: gp4
           p3: gp5
         config:
           digital0:
             attributes:
               pin: p0
           digital1:
             attributes:
               pin: p1
           uart0:
             attributes:
               txpin: p2
               rxpin: p3

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

* ``on`` - When the specified Signal emits an Event, invoke the supplied Action.
* ``listen`` - Stream new Events from the given Signal endpoint.
* ``wait`` - Wait for one new Event from the given Signal endpoint.
* ``do`` - Invoke the specified Action.

**Log Commands:**

* ``dump`` - Dump all Event logs from the DAQ device.
* ``clear`` - Clear all Event logs.

**Device Commands:**

* ``info`` - List information about the connected DAQ device or backend,
  including log usage and capacity if available.
* ``capabilities`` - List all capabilities of the DAQ.
* ``reset`` - Reset the DAQ device state.

**General Commands:**

* ``config`` - Print information about the applied DAQ configuration.

.. warning::

   Some commands such as ``dump`` or ``capabilities`` can output a significant
   amount of text to stdout, which can overwhelm a terminal's buffer. It is
   recommended to pipe these commands to a paging application, such as
   ``less``, or redirect stdout to a file.

Block commands
==============
Create
------
Create a new block. As soon as a block is created it starts generating Events.
See :ref:`Block creation <module-pw_daq-daq-cli-block-creation>` for more
details on ``block_args``.

.. code-block:: console

   $ daq-cli [flags] create <block> [block_args]

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
When the specified Signal emits an Event, invoke the specified Action. This may
happen directly on the DAQ itself or by handling in the backend script (the
behavior is backend dependent).

If the Action takes no input value, any output value from the Signal is ignored
and the Action is invoked without input. Otherwise, if no arguments are
specified for the action, the backend will assume the Action input is the
output of the Signal. The command will return an error before execution if the
types of input and output are different.

.. code-block:: console

   $ daq-cli [flags] on <block>.<signal> do <block>.<action> [action_args]

Listen
------
Stream new Events from the specified Signal endpoint.

.. code-block:: console

   $ daq-cli [flags] listen [-n N] [-t TIMEOUT] [--json] <block>.<signal>

Supports the ``-n, --number`` flag with an argument ``N`` which is the number
of events to receive then exit, and the ``-t, --timeout`` flag with an
argument ``TIMEOUT`` (in seconds) which is the maximum time to wait before
exiting. If neither flag is specified, the command will continue receiving
events until ``^C`` is given.

By default, events are printed to stdout one per line in the human-readable
format:

.. code-block:: output

   [<timestamp>] <block>.<endpoint>: <value>

``<value>`` is stringified based on the underlying type, so an integer is
written out as a number, and a ``char`` is written out as a hex value +
printable character (e.g. ``0x61 'a'``) or hex only if non-printable (e.g.
``0x0a``).

When an event is valueless, the ``: <value>`` portion is omitted:

.. code-block:: output

   [<timestamp>] <block>.<endpoint>

If ``--json`` is passed, each event is printed as a single-line JSON object
(NDJSON) using the same schema as ``dump``:

.. code-block:: json

   {"timestamp_ns": 1234, "endpoint": "uart0.rx", "value": 97, "value_type": "char"}

Wait
----
Wait is effectively an alias for ``listen -n 1``.

.. code-block:: console

   $ daq-cli [flags] wait [-t TIMEOUT] [--json] <block>.<signal>

Do
--
Invokes the specified action on the block.

.. code-block:: console

   $ daq-cli [flags] do <block>.<action> [action_args]

An action always reports success or failure, and that value is used to set the
exit code of the invocation (``0`` on success, ``1`` on failure). So if
``daq-cli do uart0.tx 'a'`` fails to send the byte, the application will report
a non-zero exit code.

On success, an action may also return an additional value. If an action returns
a value on success, that value is printed to stdout using the same ``<value>``
format as in ``listen``.

Log commands
============
Dump
----
Read out all Event logs from the device and print them to stdout.

.. code-block:: console

   $ daq-cli [flags] dump

The log output is NDJSON (JSON Lines), with one JSON object per ``Event``:

.. code-block:: json

   {"timestamp_ns": 1234, "endpoint": "block1.signal", "value": 123, "value_type": "int"}
   {"timestamp_ns": 1235, "endpoint": "block2.signal", "value": null, "value_type": null}
   {"timestamp_ns": 1236, "endpoint": "uart0.rx", "value": 97, "value_type": "char"}

.. note::

   Signals that do not have output values set ``value`` and ``value_type`` to
   ``null``. For ``char`` signals, ``value`` is emitted as an integer byte
   value.

Clear
-----
Clear the Event logs from the device.

.. code-block:: console

   $ daq-cli [flags] clear

Device commands
===============
Info
----
Print information about the device and connection as JSON, including Event log
usage and capacity information if available. The JSON schema is defined by the
backend; details can be found in the :ref:`backend <module-pw_daq-backends>`
documentation.

.. code-block:: console

   $ daq-cli [flags] info

Capabilities
------------
Print the device capabilities as JSON, including supported virtual blocks,
hardware resources, virtual-to-real resource mappings, and ``BED`` configuration
options. This command does not require a connection to the DAQ device, though it
may require the backend to be configured.

.. code-block:: console

   $ daq-cli [flags] capabilities

Reset
-----
Reset the configuration of the DAQ device. Destroy any created blocks and
reset to a "power on" state. May include power cycling the device.

.. code-block:: console

   $ daq-cli [flags] reset

General commands
================
Config
------
Print the full applied ``pw_daq`` configuration to stdout in YAML format. This
command is always available and does not require a connection to the DAQ
device.

.. code-block:: console

   $ daq-cli [flags] config

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
``--[no-]attribute`` flags.

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

         --initial=[active|inactive|high|low|true|false|1|0|on|off]
                        The initial state of the pin in output or open drain
                        mode, either "active" or "inactive" ("high", "low", and
                        truthy/falsy aliases are converted to "active" and
                        "inactive"). Default: inactive. Invalid in "in" mode.

         --pull=[up|down|none]
                        Pull-up or pull-down setting. Only valid in input or
                        open drain mode. Default: "none".

Example: UART
=============
Creating a ``uart`` block uses the following format:

.. code-block:: output

   $ daq-cli [flags] create uart<N> --baudrate=BAUDRATE [--databits=DATABITS]
       [--parity=PARITY] [--stopbits=STOPBITS] [--[no-]loopback]

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

         --[no-]loopback
                        Whether to automatically loop received characters back
                        to the transmit channel. Default: false.

--------
Examples
--------
.. code-block:: console

   $ daq-cli create digital0 --direction=out --initial=inactive
   $ daq-cli create digital1 --direction=in --pull=down
   $ daq-cli create uart1 --baudrate=115200 --loopback
   $ daq-cli on digital1.change do digital0.write
   $ daq-cli do uart1.tx 'a'
   $ daq-cli listen uart1.rx
   $ daq-cli destroy digital1
   $ daq-cli reset
