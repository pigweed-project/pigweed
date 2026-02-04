.. _module-pw_i2c_mcuxpresso:

=================
pw_i2c_mcuxpresso
=================
.. pigweed-module::
   :name: pw_i2c_mcuxpresso

``pw_i2c_mcuxpresso`` implements the ``pw_i2c`` interface using the
NXP MCUXpresso SDK.

The implementation is based on the i2c driver in SDK. I2C transfers use
non-blocking driver API.

``McuxpressoInitiator`` implements the ``pw_i2c`` initiator interface using the
MCUXpresso FlexComm driver.

``FlexIoMcuxpressoInitiator`` implements the ``pw_i2c`` initiator interface
using the MCUXpresso FlexIo driver. As the FlexIo peripheral emulates I2C, it
has a few limitations:

1. No Clock Stretching support.
2. No Arbitration support in multi-controller buses.
3. No proper differentiation between overflow and ``NACK``.
4. No I2C transfer-flag support, i.e. no support for ``NO_START``,
   ``REPEATED_START`` and ``NO_STOP`` flags. This only allows support for:

  * Single Read or Write operations.
  * Write+Read, for example to query a register value on the device.

``I3cMcuxpressoInitiator`` implements the ``pw_i2c`` initiator interface using
the MCUXpresso I3C driver. It exposes a few I3C specific API's for setting up
the bus, allowing normal I2C API's to work after setup.

-----
Setup
-----
This module requires following setup:

1. Use ``pw_build_mcuxpresso`` to create a ``pw_source_set`` for an
   MCUXpresso SDK.
2. Include the i2c driver component in this SDK definition.
3. Specify the ``pw_third_party_mcuxpresso_SDK`` GN global variable to specify
   the name of this source set.
4. Use ``pw::i2c::McuxpressoInitiator`` implementation of
   ``pw::i2c::Initiator`` while creating ``pw::i2c::Device`` or
   ``pw::i2c::RegisterDevice`` interface to access the I2C devices connected to
   target.

-----
Usage
-----
``McuxpressoInitiator`` example usage.

.. code-block:: cpp

   pw::clock_tree::Element& flexcomm11_clock;

   constexpr uint32_t kI2CBaudRate = 100000;
   constexpr McuxpressoInitiator::Config kConfig = {
       .flexcomm_address = I2C11_BASE,
       .clock_name = kCLOCK_Flexcomm11Clk,
       .baud_rate_bps = kI2CBaudRate,
   };
   McuxpressoInitiator initiator{kConfig, flexcomm11_clock};
   initiator.Enable();

``FlexIoMcuxpressoInitiator`` example usage.

.. note::

  The ``flexio_sda_channel`` and ``flexio_scl_channel`` are platform-specific.
  The NXP MCUXpresso ConfigTool can be used to auto-generate a definition after
  setting an "identifier" to the associated FlexIo pins. The value is an
  enumeration from 0 to the max number of FlexIo pins supported by the MCU.

.. code-block:: cpp

   pw::clock_tree::Element& flexio_clock;

   constexpr uint32_t kI2CBaudRate = 100000;
   constexpr FlexIoMcuxpressoInitiator::Config kFlexioConfig = {
       .flexio_address = FLEXIO0_BASE,
       .clock_name = kCLOCK_FlexioClk,
       .baud_rate_bps = kI2CBaudRate,
       .flexio_sda_channel = BOARD_FLEXIO_I2C_SDA_CHANNEL,
       .flexio_scl_channel = BOARD_FLEXIO_I2C_SCL_CHANNEL,
   };
   FlexIoMcuxpressoInitiator initiator{kFlexioConfig, flexio_clock};
   initiator.Enable();

``I3cMcuxpressoInitiator`` example usage.

.. code-block:: cpp

   pw::clock_tree::Element& i3c_clock;

   constexpr I3cMcuxpressoInitiator::Config kI3c0Config = {
       .base_address = I3C0_BASE,
       .i2c_baud_rate = kI3cI2cBaudRate,
       .i3c_open_drain_baud_rate = kI3cOpenDrainBaudRate,
       .i3c_push_pull_baud_rate = kI3cPushPullBaudRate,
       .enable_open_drain_stop = false,  // NXP default
       .enable_open_drain_high = true,   // necessary to allow bus to operate in
                                         // mixed mode
   };

   I3cMcuxpressoInitiator i3c_0_initiator{kI3c0Config, i3c_clock};

   // Initialize the i3c core library. After this is called, the
   // initiator can be used for regular i2c communication.
   i3c_0_initiator.Enable();

   // Perform an i3c bus initialization by assigning the following targets
   // their i2c static address as their i3c dynamic address.
   constexpr std::array<pw::i2c::Address, 2> kI3cTargets = {
       pw::i2c::Address::SevenBit<0x23>(),
       pw::i2c::Address::SevenBit<0x38>(),
   };
   i3c_initiator.SetStaticAddressList(kI3cTargets);
   PW_TRY(i3c_initiator.Initialize());

``I3cMcuxpressoInitiator`` example of a individual static i3c devices
that comes on and offline to save power.

.. code-block:: cpp

   constexpr I3cMcuxpressoInitiator::Config kI3c0Config = {
       .base_address = I3C0_BASE,
       .i2c_baud_rate = kI3cI2cBaudRate,
       .i3c_open_drain_baud_rate = kI3cOpenDrainBaudRate,
       .i3c_push_pull_baud_rate = kI3cPushPullBaudRate,
       .enable_open_drain_stop = false,  // NXP default
       .enable_open_drain_high = true,   // necessary to allow bus to operate in
                                         // mixed mode
   };

   I3cMcuxpressoInitiator i3c_0_initiator{kI3c0Config};

   // Initialize the i3c core library. After this is called, the
   // initiator can be used for regular i2c communication.
   i3c_0_initiator.Enable();

   constexpr auto address = pw::i2c::Address::SevenBit<0x58>();

   // Assign a fixed i3c address from the static i2c address.
   i3c_0_initiator.SetDasa(address);

   // i3c read write activity against address

   // Power off device.

   // Tell the initiator that the address is no longer assigned.
   i3c_0_initiator.ForgetAssignedAddress(address);

   // Optionally disable the initiator to bring the SDA/SCL lines low.
   // i3c_0_initiator.Disable();
