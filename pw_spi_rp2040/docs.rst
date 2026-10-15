.. _module-pw_spi_rp2040:

==============
 pw_spi_rp2040
==============
.. pigweed-module::
   :name: pw_spi_rp2040

``pw_spi_rp2040`` implements the :ref:`module-pw_spi` interface using the
`Raspberry Pi Pico SDK <https://github.com/raspberrypi/pico-sdk/>`_.

The implementation is based on the SPI driver from the Pico SDK. SPI transfers
use the blocking driver API which uses busy waiting under the hood.

The PicoSDK's ``spi_init()`` must be called before using the initiator in order
to enable the SPI peripheral and set its bus speed.

.. note::
   There is currently no support for RP2040 hardware CSn
   pins. :cc:`pw::spi::DigitalOutChipSelector` is
   used instead.

Usage
=====
.. code-block:: cpp

   #include "hardware/spi.h"
   #include "pw_digital_io/polarity.h"
   #include "pw_digital_io_rp2040/digital_io.h"
   #include "pw_spi/chip_selector_digital_out.h"
   #include "pw_spi/device.h"
   #include "pw_sync/borrow.h"
   #include "pw_sync/mutex.h"

   constexpr pw::spi::Config kSpiConfig8Bit{
       .polarity = pw::spi::ClockPolarity::kActiveHigh,
       .phase = pw::spi::ClockPhase::kFallingEdge,
       .bits_per_word = pw::spi::BitsPerWord(8),
       .bit_order = pw::spi::BitOrder::kMsbFirst,
   };

   pw::digital_io::Rp2040DigitalInOut display_cs_pin({
       .pin = 12,
       .polarity = pw::digital_io::Polarity::kActiveLow,
   });
   DigitalOutChipSelector spi_chip_selector(display_cs_pin);

   Rp2040Initiator spi_initiator(spi0);
   VirtualMutex spi_initiator_mutex;
   Borrowable<Initiator> borrowable_spi_initiator(spi_initiator,
                                                  spi_initiator_mutex);
   pw::spi::Device spi_8_bit(borrowable_spi_initiator,
                             kSpiConfig8Bit,
                             spi_chip_selector);
