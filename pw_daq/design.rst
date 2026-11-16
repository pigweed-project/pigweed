.. _module-pw_daq-design:

=============
Module design
=============
.. pigweed-module-subpage::
   :name: pw_daq

--------------
Design concept
--------------
The design of ``pw_daq`` is based around an abstract DAQ device. This device is
made up of :ref:`blocks <module-pw_daq-design-blocks>` which each provide some
consistent set of functionality. Examples of blocks are UART, DigitalIO, PWM,
ADC, DAC, etc.

Each block provides a set of :ref:`endpoints <module-pw_daq-design-endpoints>`,
which can be an Attribute, a Signal, or an Action. Attributes represent
configuration options or properties of a block that can be set at creation or
queried and modified at runtime. Signals emit an Event when a relevant physical
event or block state change occurs and may carry an associated output value.
Actions may be invoked with an input value (``InValue``). Actions either cause
some physical action (possibly leading to a Signal emitting an Event) or change
the state of the block.

.. mermaid::

   block
      columns 2
      daq:2
      digital0
      uart0
      digital1
      uart1
      digital2
      i2c0
      adc0
      spi0
      adc1
      pwm0
      dac0
      pwm1

.. _module-pw_daq-design-blocks:

Blocks
======
Blocks are a virtual resource that a backend will tie to a specific hardware
peripheral, set of peripherals, or other resource on the concrete DAQ device.

Blocks typically represent a real peripheral or other basic functionality
which can be used to monitor or interact with the connected hardware system.
Some examples include DigitalIO, ADC, DAC, PWM, UART, and Timer blocks.

Blocks are "created" dynamically and only operate after creation. If two blocks
share the same hardware resources (e.g. pin mappings) the DAQ backend may
disallow two blocks from existing at the same time.

A backend does not need to map blocks to the exact physical capabilities of a
DAQ. For instance, the LabJack DAQs have programmable muxes for digital inputs
and outputs, but a backend could reasonably tie ``digital0`` to a single pin.

Instances of blocks are named using the ``<block_type><N>`` format, where
``<block_type>`` is one of the types listed below and ``<N>`` is a number
representing the index of the peripheral or function, e.g. ``uart1`` or
``digital0``. How virtual blocks and resources map to actual hardware on a
concrete device is described in the device's
:ref:`capabilities <module-pw_daq-design-capabilities>`.

.. _module-pw_daq-design-endpoints:

Endpoints
=========
Endpoints are Attributes, Signals, or Actions associated with a Block. Endpoints
are specified using the following notation: ``<block>.<endpoint>``, where
``<block>`` is the specific name of the owning block, e.g. ``digital1``, and
``<endpoint>`` is the name of the Attribute, Signal, or Action, e.g. ``pull`` or
``write``. So in these examples, the Endpoints would be noted as:
``digital1.pull`` or ``digital1.write``.

.. note::

   A leading ``.`` is used in documentation to indicate an Attribute, Signal, or
   Action outside the context of a specific Block. When specified on the command
   line, however, an Attribute, Signal, or Action will always be paired with a
   Block to form a complete endpoint.

Attributes
----------
Attributes are Endpoints that represent configuration options or properties
associated with a Block—options that can be set at creation or changed
dynamically according to the attribute's ``mutability`` trait in the abstract
DAQ specification, such as a UART block's baud rate.

Attributes may have a limited set of valid values. These values will be defined
below in :ref:`module-pw_daq-design-block-details`.

Attributes may have a default value (if no other value is specified at
creation, the attribute will start with this value). Default values will be
specified in :ref:`module-pw_daq-design-block-details`. If no default is
specified, the attribute must be specified at creation of the block.

Traits
^^^^^^
Attributes can have "traits". Traits define the specific abilities and
interactions an attribute can have. The below table defines all traits defined
by the abstract DAQ.

.. list-table::
   :header-rows: 1

   * - Trait
     - Values
     - Description
   * - ``mutability``
     -
     - Defines when an attribute can be modified.
   * -
     - ``const``
     - The attribute is immutable. It cannot be changed even at block creation.
   * -
     - ``init``
     - The attribute is mutable at init-time only (at block creation).
   * -
     - ``(empty)`` / ``none``
     - The attribute is run-time mutable (noted as *(empty)* in diagrams, or
       ``none`` in tables).

.. note::

   In the attribute tables below, a trailing asterisk (``*``) on a value
   indicates that it is the default value when the attribute is not explicitly
   specified. ``const`` attributes do not have a default as they cannot be
   changed, even at block creation.

The abstract DAQ defines the attribute specification—supported attributes,
value types, defaults, and ``mutability`` traits—for each block, and backends
must adhere to this specification without adding custom attributes or relaxing
or tightening ``mutability`` traits. Where an attribute's valid value set is
backend-specific (for example, values marked ``BED`` such as supported UART
baud rates, or valid pin indices), the backend defines the supported values and
reports them via ``capabilities``.

Pins
^^^^
There are many attributes that define which "pins" a block interacts with. The
abstract DAQ defines virtual pins using the ``p<N>`` notation (e.g. ``p0``,
``p1``, ``p2``), where ``<N>`` is a non-negative integer starting at ``0`` up
to some backend-defined maximum. Not all indices need to correspond to a valid
virtual pin, and the mapping between a virtual pin and the actual hardware pin
or terminal is defined in the device's
:ref:`capabilities <module-pw_daq-design-capabilities>`.

.. note::

   Don't assume that the abstract DAQ virtual pin (e.g. ``p0``) has the same
   number as the real DAQ's physical pin numbers. Inspect the device
   ``capabilities`` to determine the mapping from virtual pins to real hardware
   resources.

Signals
-------
Signals are Endpoints that emit an "Event" when a change of state happens in
the associated Block or some hardware event occurs. Signal endpoints generate
Events, which represent each actual occurrence of the signal.

An Event contains three pieces of information:

1. The timestamp (in local DAQ time) at which the event occurred.
2. The endpoint that created the event (including the block index).
3. Optionally, a data value associated with the event. See the individual
   block documentation for more details on the values.

.. note::

   All ``pw_daq`` timestamps are stored as 64-bit integers and represent
   nanoseconds since the DAQ device's epoch.

   Some caveats to keep in mind:

   * The resolution of the DAQ clock may be lower. The minimum resolution is
     guaranteed to be microsecond granularity.
   * Resolution is not accuracy. The timestamps may have jitter or offsets from
     the actual event times. This is backend-specific.
   * Timestamps are guaranteed to be monotonic except across a reset or power
     cycle.

A DAQ device logs Events as they occur. The size of this log depends on the
concrete DAQ being used.

Actions
-------
Actions are Endpoints that may be invoked either directly via ``do`` or via
``on`` triggers. They represent an action that the block may take.

Actions may accept a single input value (``InValue``) as an argument and always
report success or failure. On success, an Action may also return an additional
resulting value (``Result``). For example, ``uart0.tx`` takes a character (or
byte) as input and reports success or failure with no additional result value,
while ``digital0.read`` reports success or failure and returns the current pin
``state`` on success.

If an action is invoked via an ``on`` trigger the returned result is discarded.

Backends
========
Backends are concrete implementations of the abstract DAQ which support an
actual DAQ device. They implement the abstract backend interface necessary to
provide the functionality of ``pw_daq``. See :ref:`module-pw_daq-backends` for
details on the backend design, API, and supported backends.

.. important::

   Backends must adhere to the abstract DAQ attribute specification—including
   supported attributes, value types, defaults, and ``mutability`` traits—for
   each supported block. Backends must not relax or tighten attribute
   ``mutability`` traits (for example, allowing run-time modification of an
   ``init`` attribute or making an ``init`` attribute ``const``) or add custom,
   non-spec attributes.

Backend defined (``BED``)
-------------------------
When the valid value set for an attribute or endpoint defined by the abstract
DAQ depends on the concrete hardware or backend in use, it is marked as ``BED``
in the specification (for example, supported UART ``.baudrate`` values).
Backend-specific value sets, virtual pin assignments, and device-specific error
codes are defined by the backend—and configured/reported via
:ref:`capabilities <module-pw_daq-design-capabilities>` or documented by the
backend—without adding custom attributes or traits to the abstract DAQ
specification.

.. _module-pw_daq-design-capabilities:

Capabilities
------------
Each configured DAQ device defines a ``capabilities`` mapping that describes:

* **Device-specific capabilities**: Which virtual DAQ blocks (such as
  ``digital0`` or ``uart0``) the device supports.
* **Hardware resources**: The concrete physical resources on the target DAQ
  hardware (such as physical GPIO pins or hardware peripherals).
* **Virtual resource mapping**: How virtual DAQ blocks and virtual resources
  (such as abstract pin indices) map to real hardware resources.
* **Backend-defined configuration options** (``BED``): Device-specific Backend
  Defined values and constraints (such as ``const`` virtual pin assignments or
  supported attribute values).

While the exact schema and resource identifiers within ``capabilities`` are
backend-specific, ``capabilities`` always focuses on how the virtual DAQ
device's blocks and resources map to real hardware resources on the target
device. Backends receive ``capabilities`` through device configuration (via
``configure()``) and use that mapping to instantiate and control blocks on the
real hardware.

Notation
========
Throughout this documentation we will use the following notation to define
Blocks and their Endpoints.

.. mermaid::

   classDiagram
      class block {
         attribute [traits]
         > signal(outvalue)
         < action(invalue) result
      }

.. _module-pw_daq-design-detailed-design:

---------------
Detailed design
---------------
An abstract DAQ device supports these commands:

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
* ``do`` - Invoke the specified Action and return the result.

**Log Commands:**

* ``dump`` - Dump all Event logs from the DAQ device.
* ``clear`` - Clear all Event logs.

**Device Commands:**

* ``info`` - List information about the connected DAQ device or backend,
  including log usage and capacity if available.
* ``capabilities`` - List all capabilities of the DAQ, including supported
  virtual blocks, hardware resources, virtual-to-real resource mappings, and
  ``BED`` configuration options. Does not require a connection to the DAQ
  device, though it may require the backend to be configured.
* ``reset`` - Reset the DAQ device state.

Additional commands may be supported by specific DAQ backends. See the backend
specific documentation for details.

.. note::

   ``on`` commands can only be triggered using Signals, not Actions.

.. note::

   ``do`` commands can only invoke Actions, not Signals.

.. _module-pw_daq-design-block-details:

Block details
=============
The blocks supported by the abstract DAQ device are:

* ``digital`` - A digital input/output peripheral. Can either be input or
  output and supports configurations such as pull-up/pull-down.
* ``adc`` - An analog input peripheral.
* ``dac`` - An analog output peripheral.
* ``pwm`` - A Pulse Width Modulation peripheral.
* ``uart`` - A UART peripheral. Supports standard baud rates and both RX and TX.
* ``spi`` - A SPI peripheral.
* ``i2c`` - An I2C peripheral.
* ``timer`` - A timer function. Supports both one-shot and periodic at a
  specified period.
* ``stopwatch`` - A stopwatch function to track time between events.
* ``lut`` - A lookup table.

.. note::

   A \* in the tables below denotes the default value for an attribute.

Digital
-------
Represents a digital GPIO which may be either input, output, or open drain. Pin
states in the abstract DAQ are represented as ``active`` or ``inactive``.

.. note::

   In standard input and output mode, ``active`` corresponds to logic high and
   ``inactive`` corresponds to logic low. In open drain mode, ``active`` pulls
   the pin low and ``inactive`` releases it (high-impedance). Frontends such as
   ``daq-cli`` may accept convenience aliases (e.g. ``high`` and ``low``) and
   convert them to ``active`` and ``inactive``.

.. mermaid::

   classDiagram
      class digital {
         pin [const]
         direction [init]
         initial [init]
         pull
         > change(state)
         > risingedge()
         > fallingedge()
         < write(state)
         < toggle()
         < read() state
      }

Attributes
^^^^^^^^^^
.. list-table::
   :header-rows: 1

   * - Attribute
     - Traits
     - Values / Type
     - Description
   * - ``.pin``
     - ``const``
     - ``pin``
     - The virtual pin (e.g. ``p0``) associated with this block. The mapping
       between virtual pins and actual hardware pins or terminals is defined in
       the device's ``capabilities``.
   * - ``.direction``
     - ``init``
     - ``in``, ``out``, ``opendrain``
     - The direction of the digital block.
   * - ``.initial``
     - ``init``
     - ``active``, ``inactive``\*
     - The initial value of the pin. Only valid in output or open drain mode.
   * - ``.pull``
     - ``none``
     - ``up``, ``down``, ``none``\*
     - Pull-up or pull-down setting. Only valid in input or open drain mode.

Signals
^^^^^^^
.. list-table::
   :header-rows: 1

   * - Signal
     - OutValue
     - Description
   * - ``.change``
     - ``state``
     - Indicates the pin state changed. OutValue indicates whether the new
       state is ``active`` or ``inactive``. Valid for input, output, and open
       drain modes. Note: In open drain mode, external pin state changes do not
       trigger an event, only internal ``active`` <-> ``inactive`` changes emit
       an event.
   * - ``.risingedge``
     - N/A
     - Indicates the digital input transitioned from ``inactive`` (logic low)
       to ``active`` (logic high). Only valid for input mode.
   * - ``.fallingedge``
     - N/A
     - Indicates the digital input transitioned from ``active`` (logic high)
       to ``inactive`` (logic low). Only valid for input mode.

Actions
^^^^^^^
.. list-table::
   :header-rows: 1

   * - Action
     - InValue
     - Result
     - Description
   * - ``.write``
     - ``state``
     - N/A
     - Write the new state to the pin. InValue is the new state to write
       (``active`` or ``inactive``). Only valid for output and open drain mode.
   * - ``.toggle``
     - N/A
     - N/A
     - Toggle the state of the pin (``active`` <-> ``inactive``). Only valid
       for output and open drain mode.
   * - ``.read``
     - N/A
     - ``state``
     - Read the current state of the pin (``active`` or ``inactive``). Valid in
       all modes. In open drain mode returns the drive state, not the pin
       state.

ADC
---
.. TODO(b/573259645): Add documentation and support for adc block to daq-cli,
   daq-lib, and dhalsim.

TBD

.. mermaid::

   classDiagram
      class adc {
         pin [const]
         vrefpos [const]
         vrefneg [const]
         samplebits [const]
         samplerate [init]
         scalemultiplier [init]
         scaleoffset [init]
         > sampled(value)
         > started()
         > stopped(count)
         < sample() value
         < start()
         < stop()
      }

Attributes
^^^^^^^^^^

Signals
^^^^^^^
.. list-table::
   :header-rows: 1

   * - Signal
     - OutValue
     - Description
   * - TBD
     - TBD
     - TBD

Actions
^^^^^^^
.. list-table::
   :header-rows: 1

   * - Action
     - InValue
     - Result
     - Description
   * - TBD
     - TBD
     - TBD
     - TBD

DAC
---
.. TODO(b/573267325): Add documentation and support for dac block to daq-cli,
   daq-lib, and dhalsim.

TBD

.. mermaid::

   classDiagram
      class dac {
         pin [const]
         vrefpos [const]
         vrefneg [const]
         databits [const]
         > conversion(value)
         < convert(value)
      }

Attributes
^^^^^^^^^^

Signals
^^^^^^^
.. list-table::
   :header-rows: 1

   * - Signal
     - OutValue
     - Description
   * - TBD
     - TBD
     - TBD

Actions
^^^^^^^
.. list-table::
   :header-rows: 1

   * - Action
     - InValue
     - Result
     - Description
   * - TBD
     - TBD
     - TBD
     - TBD

PWM
---
.. TODO(b/573267796): Add documentation and support for pwm block to daq-cli,
   daq-lib, and dhalsim.

TBD

Attributes
^^^^^^^^^^

Signals
^^^^^^^
.. list-table::
   :header-rows: 1

   * - Signal
     - OutValue
     - Description
   * - TBD
     - TBD
     - TBD

Actions
^^^^^^^
.. list-table::
   :header-rows: 1

   * - Action
     - InValue
     - Result
     - Description
   * - TBD
     - TBD
     - TBD
     - TBD

UART
----
Represents a UART peripheral which may have both RX and TX capabilities.

.. mermaid::

   classDiagram
      class uart {
         txpin [const]
         rxpin [const]
         baudrate [init]
         databits [init]
         parity [init]
         stopbits [init]
         loopback [init]
         > rx(character)
         > sent(character)
         > break()
         > error(error)
         < tx(character)
      }

Attributes
^^^^^^^^^^
.. list-table::
   :header-rows: 1

   * - Attribute
     - Traits
     - Values / Type
     - Description
   * - ``.txpin``
     - ``const``
     - ``pin``
     - The virtual pin (e.g. ``p2``) used for sending characters.
   * - ``.rxpin``
     - ``const``
     - ``pin``
     - The virtual pin (e.g. ``p3``) used for receiving characters.
   * - ``.baudrate``
     - ``init``
     - ``BED``
     - The baud rate to configure. Not all baud rates may be supported.
   * - ``.databits``
     - ``init``
     - ``7``, ``8``\*, ``9``
     - The number of data bits in a byte.
   * - ``.parity``
     - ``init``
     - ``odd``, ``even``, ``none``\*
     - Whether a parity bit is included.
   * - ``.stopbits``
     - ``init``
     - ``1``\*, ``2``
     - The number of stop bits framing a byte.
   * - ``.loopback``
     - ``init``
     - ``true``, ``false``\*
     - Whether to automatically loop received characters back to the transmit
       channel.

Signals
^^^^^^^
.. list-table::
   :header-rows: 1

   * - Signal
     - OutValue
     - Description
   * - ``.rx``
     - ``character``
     - Indicates that a character has been received on the UART RX channel. The
       OutValue is the received character.
   * - ``.sent``
     - ``character``
     - Indicates that a character has been sent on the UART TX channel. The
       OutValue is the sent character.
   * - ``.break``
     - N/A
     - Indicates a break character was received.
   * - ``.error``
     - ``error``
     - Indicates an error occurred on either the TX or RX channel. The
       definition of the error code returned is specific to the backend and DAQ
       device.

Actions
^^^^^^^
.. list-table::
   :header-rows: 1

   * - Action
     - InValue
     - Result
     - Description
   * - ``.tx``
     - ``character``
     - N/A
     - Write the given character to the TX channel.

SPI
---
.. TODO(b/573267208): Add documentation and support for spi block to daq-cli,
   daq-lib, and dhalsim.

TBD

Attributes
^^^^^^^^^^

Signals
^^^^^^^
.. list-table::
   :header-rows: 1

   * - Signal
     - OutValue
     - Description
   * - TBD
     - TBD
     - TBD

Actions
^^^^^^^
.. list-table::
   :header-rows: 1

   * - Action
     - InValue
     - Result
     - Description
   * - TBD
     - TBD
     - TBD
     - TBD

I2C
---
.. TODO(b/573266928): Add documentation and support for i2c block to daq-cli,
   daq-lib, and dhalsim.

TBD

Attributes
^^^^^^^^^^

Signals
^^^^^^^
.. list-table::
   :header-rows: 1

   * - Signal
     - OutValue
     - Description
   * - TBD
     - TBD
     - TBD

Actions
^^^^^^^
.. list-table::
   :header-rows: 1

   * - Action
     - InValue
     - Result
     - Description
   * - TBD
     - TBD
     - TBD
     - TBD

Timer
-----
.. TODO(b/573267704): Add documentation and support for timer block to daq-cli,
   daq-lib, and dhalsim.

TBD

Attributes
^^^^^^^^^^

Signals
^^^^^^^
.. list-table::
   :header-rows: 1

   * - Signal
     - OutValue
     - Description
   * - TBD
     - TBD
     - TBD

Actions
^^^^^^^
.. list-table::
   :header-rows: 1

   * - Action
     - InValue
     - Result
     - Description
   * - TBD
     - TBD
     - TBD
     - TBD

Stopwatch
---------
.. TODO(b/573268056): Add documentation and support for stopwatch block to
   daq-cli, daq-lib, and dhalsim.

TBD

Attributes
^^^^^^^^^^

Signals
^^^^^^^
.. list-table::
   :header-rows: 1

   * - Signal
     - OutValue
     - Description
   * - TBD
     - TBD
     - TBD

Actions
^^^^^^^
.. list-table::
   :header-rows: 1

   * - Action
     - InValue
     - Result
     - Description
   * - TBD
     - TBD
     - TBD
     - TBD

LUT
---
.. TODO(b/573302023): Add documentation and support for lut block to daq-cli,
   daq-lib, and dhalsim.

TBD

Attributes
^^^^^^^^^^

Signals
^^^^^^^
.. list-table::
   :header-rows: 1

   * - Signal
     - OutValue
     - Description
   * - TBD
     - TBD
     - TBD

Actions
^^^^^^^
.. list-table::
   :header-rows: 1

   * - Action
     - InValue
     - Result
     - Description
   * - TBD
     - TBD
     - TBD
     - TBD
