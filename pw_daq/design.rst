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
which can either be an Event or an Action. Events are emitted when a relevant
physical event or block state change occurs and may carry an associated output
value. Actions may be invoked with an input value (``InValue``). Actions either
cause some physical action (possibly leading to an Event) or change the state
of the block.

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
``digital0``. The mapping between index number and actual hardware is defined
by the backend in use.

Attributes
----------
Blocks may have attributes associated with them—essentially configuration
options that can be set at creation or changed dynamically (depending on the
backend in use), such as a UART block's baud rate.

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
by the abstract DAQ. Additional traits may be defined by a backend.

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

The abstract DAQ defines traits for all attributes; each backend may be less
restrictive than what the abstract DAQ defines, for example allowing an
attribute to be modified at run-time when it is marked as ``init``. However, a
backend should not be *more* restrictive than the abstract DAQ. This ensures a
moderate level of interoperability between different backends. In either case,
any divergence from the abstract DAQ interface should be well documented.

.. _module-pw_daq-design-endpoints:

Endpoints
=========
Endpoints are either Events or Actions associated with a Block. Endpoints are
specified using the following notation: ``<block>.<endpoint>``, where
``<block>`` is the specific name of the owning block, e.g. ``digital1``, and
``.<endpoint>`` is the name of the Event or Action, e.g. ``.write``. So in this
example, the Endpoint would be noted as: ``digital1.write``.

.. note::

   The leading ``.`` is used in documentation to indicate an Event or Action
   outside the context of a specific Block. When specified on the command line,
   however, the Action or Event will always be paired with a Block to form a
   complete endpoint.

Events
------
Events are Endpoints that are "emitted" when a change of state happens in the
associated Block or some hardware event occurs. Event endpoints generate Event
instances, which are the actual occurrence.

An Event instance contains three pieces of information:

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

A DAQ device logs Event instances as they occur. The size of this log depends
on the concrete DAQ being used.

Actions
-------
Actions are Endpoints that may be invoked either directly via ``do`` or via
``on`` triggers. They represent an action that the block may take.

Actions may accept a single input value (``InValue``) as an argument and return
a resulting value, for example ``uart0.tx`` takes a character (or byte) as
input and returns a ``bool`` to indicate success or failure.

If an action is invoked via an ``on`` trigger the returned result is discarded.

Backends
========
Backends are concrete implementations of the abstract DAQ which support an
actual DAQ device. They implement the abstract backend interface necessary to
provide the functionality of ``pw_daq``. See :ref:`module-pw_daq-backends` for
details on the backend design, API, and supported backends.

.. important::

   While a backend can be less restrictive than the abstract DAQ, it cannot be
   more restrictive. For example, if the abstract DAQ declares an attribute as
   ``init``, the backend can allow run-time modification (less restrictive) but
   cannot change it to ``const``. Either way, the backend should document
   divergence from the abstract DAQ.

Backend defined (``BED``)
-------------------------
If a block, attribute, endpoint, trait, etc. is not specified by the abstract
DAQ and is expected to be defined by the backend in use, it will be marked as
``BED``. If an attribute is marked ``BED``, the backend also defines whether and
what the default value is.

Notation
========
Throughout this documentation we will use the following notation to define
Blocks and their Endpoints.

.. mermaid::

   classDiagram
      class block {
         attribute [traits]
         > event(outvalue)
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

* ``on`` - When the specified Event occurs, invoke the supplied Action.
* ``listen`` - Stream new Events from the given event endpoint.
* ``do`` - Invoke the specified Action and return the result.

**Log Commands:**

* ``dump`` - Dump all Event logs from the DAQ device.
* ``clear`` - Clear all Event logs.
* ``usage`` - Get stats on log usage and capacity.

**Device Commands:**

* ``info`` - List information about the connected DAQ device or backend.
* ``capabilities`` - List all capabilities of the DAQ.
* ``reset`` - Reset the DAQ device state.

Additional commands may be supported by specific DAQ backends. See the backend
specific documentation for details.

.. note::

   ``on`` commands can only be triggered using Events, not Actions.

.. note::

   ``do`` commands can only invoke Actions, not Events.

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
Represents a digital GPIO which may be either input, output, or open drain. In
input and output mode, the pin may take on either ``high`` or ``low`` states.

.. note::

   In open drain mode, the terminology is ``active`` or ``inactive`` since the
   actual physical voltage on the pin is typically inverted. However, throughout
   the documentation we use ``high`` and ``active`` and ``low`` and
   ``inactive`` interchangeably.

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
         < write(state) success
         < read() state
      }

Attributes
^^^^^^^^^^
.. list-table::
   :header-rows: 1

   * - Attribute
     - Traits
     - Values
     - Description
   * - ``pin``
     - ``const``
     - ``BED``
     - The pin identifier (e.g. ``14`` or ``"DIO0"``) associated with this
       block.
   * - ``direction``
     - ``init``
     - ``in``, ``out``, ``opendrain``
     - The direction of the digital block.
   * - ``initial``
     - ``init``
     - ``high`` or ``active``, ``low``\* or ``inactive``\*
     - The initial value of the pin. Only valid in output or open drain mode.
   * - ``pull``
     - ``none``
     - ``up``, ``down``, ``none``\*
     - Pull-up or pull-down setting. Only valid in input or open drain mode.

Endpoints
^^^^^^^^^
The following tables define the valid Events and Actions supported by a Digital
GPIO block.

.. list-table::
   :header-rows: 1

   * - Event
     - OutValue
     - Description
   * - ``.change``
     - ``state``
     - Indicates the pin state changed. OutValue indicates whether the new
       state is ``high`` (``active``) or ``low`` (``inactive``). Valid for
       input, output, and open drain modes. Note: In open drain mode, external
       pin state changes do not trigger an event, only internal ``active`` <->
       ``inactive`` changes emit an event.
   * - ``.risingedge``
     - N/A
     - Indicates the digital input went from low to high. Only valid for input
       mode.
   * - ``.fallingedge``
     - N/A
     - Indicates the digital input went from high to low. Only valid for input
       mode.

.. list-table::
   :header-rows: 1

   * - Action
     - InValue
     - Result
     - Description
   * - ``.write``
     - ``state``
     - ``success``
     - Write the new state to the pin. InValue is the new state to write:
       ``high`` (output mode) or ``active`` (open drain mode) and ``low``
       (output mode) or ``inactive`` (open drain mode). Only valid for output
       and open drain mode.
   * - ``.read``
     - N/A
     - ``state``
     - Read the current state of the pin. Valid in all modes. In open drain mode
       returns the drive state, not the pin state.

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
         < start() success
         < stop() success
      }

Attributes
^^^^^^^^^^

Endpoints
^^^^^^^^^
The following tables define the valid Events and Actions supported by an ADC
block.

.. list-table::
   :header-rows: 1

   * - Event
     - OutValue
     - Description
   * - TBD
     - TBD
     - TBD

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
         < convert(value) success
      }

Attributes
^^^^^^^^^^

Endpoints
^^^^^^^^^
The following tables define the valid Events and Actions supported by a DAC
block.

.. list-table::
   :header-rows: 1

   * - Event
     - OutValue
     - Description
   * - TBD
     - TBD
     - TBD

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

Endpoints
^^^^^^^^^
The following tables define the valid Events and Actions supported by a PWM
block.

.. list-table::
   :header-rows: 1

   * - Event
     - OutValue
     - Description
   * - TBD
     - TBD
     - TBD

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
         < tx(character) success
      }

Attributes
^^^^^^^^^^
.. list-table::
   :header-rows: 1

   * - Attribute
     - Traits
     - Values
     - Description
   * - ``txpin``
     - ``const``
     - ``BED``
     - The pin identifier (e.g. ``4`` or ``"DIO0"``) used for sending
       characters.
   * - ``rxpin``
     - ``const``
     - ``BED``
     - The pin identifier (e.g. ``5`` or ``"DIO1"``) used for receiving
       characters.
   * - ``baudrate``
     - ``init``
     - ``BED``
     - The baud rate to configure. Not all baud rates may be supported.
   * - ``databits``
     - ``init``
     - ``7``, ``8``\*, ``9``
     - The number of data bits in a byte.
   * - ``parity``
     - ``init``
     - ``odd``, ``even``, ``none``\*
     - Whether a parity bit is included.
   * - ``stopbits``
     - ``init``
     - ``1``\*, ``2``
     - The number of stop bits framing a byte.
   * - ``loopback``
     - ``init``
     - ``true``, ``false``\*
     - Whether to automatically loop received characters back to the transmit
       channel.

Endpoints
^^^^^^^^^
The following tables define the valid Events and Actions supported by a UART
block.

.. list-table::
   :header-rows: 1

   * - Event
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

.. list-table::
   :header-rows: 1

   * - Action
     - InValue
     - Result
     - Description
   * - ``.tx``
     - ``character``
     - ``success``
     - Write the given character to the TX channel.

SPI
---
.. TODO(b/573267208): Add documentation and support for spi block to daq-cli,
   daq-lib, and dhalsim.

TBD

Attributes
^^^^^^^^^^

Endpoints
^^^^^^^^^
The following tables define the valid Events and Actions supported by a SPI
block.

.. list-table::
   :header-rows: 1

   * - Event
     - OutValue
     - Description
   * - TBD
     - TBD
     - TBD

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

Endpoints
^^^^^^^^^
The following tables define the valid Events and Actions supported by an I2C
block.

.. list-table::
   :header-rows: 1

   * - Event
     - OutValue
     - Description
   * - TBD
     - TBD
     - TBD

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

Endpoints
^^^^^^^^^
The following tables define the valid Events and Actions supported by a Timer
block.

.. list-table::
   :header-rows: 1

   * - Event
     - OutValue
     - Description
   * - TBD
     - TBD
     - TBD

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

Endpoints
^^^^^^^^^
The following tables define the valid Events and Actions supported by a
Stopwatch block.

.. list-table::
   :header-rows: 1

   * - Event
     - OutValue
     - Description
   * - TBD
     - TBD
     - TBD

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

Endpoints
^^^^^^^^^
The following tables define the valid Events and Actions supported by a LUT
block.

.. list-table::
   :header-rows: 1

   * - Event
     - OutValue
     - Description
   * - TBD
     - TBD
     - TBD

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
