// Copyright 2021 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.
//
#pragma once

#include "pw_bytes/span.h"
#include "pw_chrono/system_clock.h"
#include "pw_i2c/address.h"
#include "pw_i2c/initiator.h"
#include "pw_status/status.h"

namespace pw {
namespace i2c {

/// @module{pw_i2c}

/// The common interface for generic I2C devices. Reads and writes arbitrary
/// chunks of data over an I2C bus to an I2C device. This class contains
/// `pw::i2c::Address` and wraps the `pw::i2c::Initiator` API. Only works
/// with devices that have a single device address.
///
/// `pw::i2c::Device` is intended to represent ownership of a specific
/// responder. Individual transactions are atomic but there's no synchronization
/// for sequences of transactions. Therefore, shared access should be faciliated
/// with higher-level application abstractions. To help enforce this,
/// `pw::i2c::Device` instances are only movable and not copyable.
class Device {
 public:
  /// Creates a `pw::i2c::Device` instance.
  ///
  /// The address for the I2C device is set in this constructor.
  ///
  /// @param[in] initiator A reference to a `pw::i2c::Initiator` instance.
  ///
  /// @param[in] device_address The address of the I2C device.
  constexpr Device(Initiator& initiator, Address device_address)
      : initiator_(initiator), device_address_(device_address) {}

  Device(const Device&) = delete;
  Device(Device&&) = default;
  ~Device() = default;

  /// Wraps `pw::i2c::Initiator::TransferFor`.
  Status TransferFor(span<const Message> messages,
                     chrono::SystemClock::duration timeout) {
    return initiator_.TransferFor(messages, timeout);
  }

  /// Wraps `pw::i2c::Initiator::WriteReadFor`.
  Status WriteReadFor(ConstByteSpan tx_buffer,
                      ByteSpan rx_buffer,
                      chrono::SystemClock::duration timeout) {
    return initiator_.WriteReadFor(
        device_address_, tx_buffer, rx_buffer, timeout);
  }

  /// Wraps the variation of `pw::i2c::Initiator::WriteReadFor` that accepts
  /// explicit sizes for the amount of bytes to write to the device and read
  /// from the device.
  Status WriteReadFor(const void* tx_buffer,
                      size_t tx_size_bytes,
                      void* rx_buffer,
                      size_t rx_size_bytes,
                      chrono::SystemClock::duration timeout) {
    return initiator_.WriteReadFor(device_address_,
                                   tx_buffer,
                                   tx_size_bytes,
                                   rx_buffer,
                                   rx_size_bytes,
                                   timeout);
  }

  /// Wraps `pw::i2c::Initiator::WriteFor`.
  Status WriteFor(ConstByteSpan tx_buffer,
                  chrono::SystemClock::duration timeout) {
    return initiator_.WriteFor(device_address_, tx_buffer, timeout);
  }
  /// Wraps the variation of `pw::i2c::Initiator::WriteFor` that accepts an
  /// explicit size for the amount of bytes to write to the device.
  Status WriteFor(const void* tx_buffer,
                  size_t tx_size_bytes,
                  chrono::SystemClock::duration timeout) {
    return initiator_.WriteFor(
        device_address_, tx_buffer, tx_size_bytes, timeout);
  }

  /// Wraps `pw::i2c::Initiator::ReadFor`.
  Status ReadFor(ByteSpan rx_buffer, chrono::SystemClock::duration timeout) {
    return initiator_.ReadFor(device_address_, rx_buffer, timeout);
  }
  /// Wraps the variation of `pw::i2c::Initiator::ReadFor` that accepts an
  /// explicit size for the amount of bytes to read from the device.
  Status ReadFor(void* rx_buffer,
                 size_t rx_size_bytes,
                 chrono::SystemClock::duration timeout) {
    return initiator_.ReadFor(
        device_address_, rx_buffer, rx_size_bytes, timeout);
  }

  /// Wraps `pw::i2c::Initiator::ProbeDeviceFor`.
  Status ProbeFor(chrono::SystemClock::duration timeout) {
    return initiator_.ProbeDeviceFor(device_address_, timeout);
  }

  /// @brief Updates the I2C address associated with this device.
  ///
  /// This is not a common operation. Most I2C devices have a fixed address
  /// or an address set at power-on. This method is useful for rare cases
  /// where a device might change its address dynamically, for example,
  /// after a specific command or based on external pin states that can be
  /// reconfigured at runtime.
  ///
  /// @param[in] address The new address for the I2C device.
  void UpdateAddress(Address address) { device_address_ = address; }

 private:
  Initiator& initiator_;
  Address device_address_;
};

}  // namespace i2c
}  // namespace pw
