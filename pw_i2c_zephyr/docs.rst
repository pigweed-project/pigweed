.. _module-pw_i2c_zephyr:

=============
pw_i2c_zephyr
=============
.. pigweed-module::
   :name: pw_i2c_zephyr

``pw_i2c_zephyr`` implements the ``pw_i2c`` interface using the Zephyr I2C
drivers. Currently, only blocking calls are supported.

Support for ten-bit addresses and the no-start flag is enabled but depends on
underlying support by the Zephyr driver.

-----
Setup
-----
Enable the module in your project's ``prj.conf``:

.. code-block:: kconfig

   CONFIG_PIGWEED_I2C=y

For the responder API, also enable:

.. code-block:: kconfig

   CONFIG_PIGWEED_I2C_RESPONDER=y

Unlike the facades documented in :ref:`zephyr-backend-selection`,
``pw_i2c_zephyr`` doesn't have alternate backends to choose between, so
these are plain enable flags rather than a ``choice``. It also needs
``pw_sync.mutex`` and ``pw_chrono.system_clock`` backends -- set
``CONFIG_PIGWEED_SYNC_MUTEX=y`` and ``CONFIG_PIGWEED_CHRONO_SYSTEM_CLOCK=y``
(see :ref:`module-pw_sync_zephyr` and :ref:`module-pw_chrono_zephyr`); if
you forget, CMake fails the build naming the missing facade.

-------------
API reference
-------------
Moved: :cc:`pw_i2c_zephyr`

-----------------
Initiator example
-----------------
A simple example illustrating the usage of the Zephyr initiator:

.. code-block:: C++

   constexpr struct i2c_dt_spec kDeviceSpec =
       I2C_DT_SPEC_GET(DT_NODELABEL(my_dev));
   pw::i2c::ZephyrInitiator kInitiator(kDeviceSpec.bus);
   pw::i2c::Device kI2cDev(kInitiator, Address::SevenBit(kDeviceSpec.addr));

-----------------
Responder example
-----------------
A simple example illustrating the usage of the Zephyr responder:

.. code-block:: C++

   class MyResponderEvents : public pw::i2c::ResponderEvents {
    public:
     constexpr MyResponderEvents() : next_read_index_(0xff) {}
     bool OnWrite(ConstByteSpan data) override {
       // Save the next index to read.
       next_read_index_ = data[0];
       return true;
     }

     Result<ConstByteSpan> OnRead() override {
       // return the value
       return Result<ConstByteSpan>(
         ConstByteSpan(&buffer_[next_read_index_], 1));
     }
    private:
     uint8_t next_read_index_;
     std::byte buffer_[128];
   };
   constexpr struct i2c_dt_spec kDeviceSpec =
       I2C_DT_SPEC_GET(DT_NODELABEL(my_dev));
   constexpr MyResponderEvents kEvents;
   pw::i2c::ZephyrResponder(kDeviceSpec.bus,
       Address::SevenBit(kDeviceSpec.addr), kEvents);
