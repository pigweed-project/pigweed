// Copyright 2020 The Pigweed Authors
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
#pragma once

#include <cstdint>
#include <utility>

#include "pw_bytes/span.h"
#include "pw_chrono/system_clock.h"
#include "pw_i2c/address.h"
#include "pw_i2c/message.h"
#include "pw_status/status.h"

/// Cross-platform I2C library
namespace pw::i2c {

/// @module{pw_i2c}

/// @brief The common, base driver interface for initiating thread-safe
/// transactions with devices on an I2C bus. Other documentation may call this
/// style of interface an I2C "master", <!-- inclusive-language: disable -->
/// "central", or "controller".
///
/// `pw::i2c::Initiator` isn't required to support 10-bit addressing. If only
/// 7-bit addressing is supported, `pw::i2c::Initiator` fails a runtime
/// assertion if given a Message with the kTenBitAddress flag, or when given
/// an address that is out of 7-bit address range.
///
/// The implementer of TransferFor() (or DoWriteReadFor()) is responsible for
/// ensuring thread safety and enabling functionality such as initialization,
/// configuration, enabling and disabling, unsticking SDA, and detecting device
/// address registration collisions.
///
/// @note `pw::i2c::Initiator` uses internal synchronization, so it's safe to
/// initiate transactions from multiple threads. Each call to this class will
/// be executed in a single bus transaction using repeated starts.
///
/// Furthermore, devices may require specific sequences of
/// transactions, and application logic must provide the synchronization to
/// execute these sequences correctly.
class Initiator {
 public:
  /// Defined set of supported i2c features.
  enum class Feature : int {
    // Initiator does not support extended features.
    kStandard = 0,

    // Initiator supports 10-bit addressing mode
    kTenBit = (1 << 0),

    // Initiator supports sending bytes without a start condition or address.
    kNoStart = (1 << 1),
  };

  /// Construct Initiator with default features.
  ///
  /// Note: this constructor enables kTenBit because the older implementation
  /// enabled it by default. Most users will not need kTenBit enabled.
  [[deprecated("Use the Initiator(Feature) constructor")]]
  constexpr Initiator()
      : Initiator(kCompatibilityFeatures) {}

  /// Construct Initiator with defined set of features.
  ///
  /// Supported features are defined in the Feature enum.
  ///
  /// Note: to support only the required features, you should specify
  ///       Initiator(Initiator::Feature::kStandard).
  ///
  /// @code
  ///   Initiator i2c(Initiator::Feature::kStandard);
  /// @endcode
  ///
  /// @code
  ///   Initiator i2c(Initiator::Feature::kTenBit |
  ///                 Initiator::Feature::kNoStart);
  /// @endcode
  explicit constexpr Initiator(Feature supported_features)
      : supported_features_(supported_features) {}

  virtual ~Initiator() = default;

  /// Writes bytes to an I2C device and then reads bytes from that same
  /// device as one atomic I2C transaction.
  ///
  /// The signal on the bus for the atomic transaction should look like this:
  ///
  /// @code
  ///   START + I2C_ADDRESS + WRITE(0) + TX_BUFFER_BYTES +
  ///   START + I2C_ADDRESS + READ(1) + RX_BUFFER_BYTES + STOP
  /// @endcode
  ///
  /// @param[in] device_address The address of the I2C device.
  ///
  /// @param[in] tx_buffer The transmit buffer.
  ///
  /// @param[out] rx_buffer The receive buffer.
  ///
  /// @param[in] timeout The maximum duration to block waiting for both
  /// exclusive bus access and the completion of the I2C transaction.
  ///
  /// @pre The provided address must be supported by the initiator. I.e.
  /// don't use a 10-bit address if the initiator only supports 7-bit
  /// addresses. This method fails a runtime assertion if this precondition
  /// isn't met.
  ///
  /// @returns
  /// * @OK: The transaction or transactions succeeded.
  /// * @DEADLINE_EXCEEDED: Was unable to acquire exclusive initiator access
  ///   and complete the I2C transaction in time.
  /// * @UNAVAILABLE: A NACK condition occurred, meaning the addressed device
  ///   didn't respond or was unable to process the request.
  /// * @FAILED_PRECONDITION: The interface isn't initialized or enabled.
  /// * @UNIMPLEMENTED: The interface doesn't support the necessary I2C
  ///   features or combination of I2C messages.
  ///
  /// @isrsafe{no}
  /// @threadsafe{yes}
  Status WriteReadFor(Address device_address,
                      ConstByteSpan tx_buffer,
                      ByteSpan rx_buffer,
                      chrono::SystemClock::duration timeout) {
    return DoWriteReadFor(device_address, tx_buffer, rx_buffer, timeout);
  }

  /// Performs multiple arbitrary reads and writes to an I2C device as one
  /// atomic transaction. Each part of the transaction is referred to as a
  /// "message".
  ///
  /// For a series of 0...N messages, the signal on the bus for the atomic
  /// transaction of two messages should look like this:
  ///
  /// @code
  ///   START + #0.I2C_ADDRESS + #0.WRITE/READ(0/1) + #0.BYTES +
  ///   START + #1.I2C_ADDRESS + #1.WRITE/READ(0/1) + #1.BYTES +
  ///   ...
  ///   START + #N.I2C_ADDRESS + #N.WRITE/READ(0/1) + #N.BYTES + STOP
  /// @endcode
  ///
  ///
  /// @param[in] messages An array of pw::i2c::Message objects to transmit
  /// as one i2c bus transaction.
  ///
  /// For each Message msg in messages:
  ///
  /// If msg.GetAddress().IsTenBit() is true:
  /// * The implementation should transmit that message using the 10-bit
  ///   addressing scheme defined in the i2c spec.
  /// * The implementation should CHECK or return an error if 10-bit addressing
  ///   is unsupported.
  ///
  /// If msg.GetAddress().IsWriteContinuation() is true:
  /// * The implementation should transmit this message without a start
  /// condition or address.
  /// * The implementation should CHECK or return an error if the hardware
  ///   or initiator does not support this feature.
  ///
  /// @param[in] timeout The maximum duration to block waiting for both
  /// exclusive bus access and the completion of the bus transaction.
  ///
  /// @pre The provided addresses of each message must be supported by the
  /// initiator, Don't use a 10-bit address if the initiator only supports
  /// 7-bit addresses. This method fails a runtime assertion if this
  /// precondition isn't met.
  ///
  /// @returns
  /// * @OK: The transaction succeeded.
  /// * @INVALID_ARGUMENT: The arguments can never be valid. For example,
  ///   a `WriteContinuation` without a preceding `Write` message.
  /// * @DEADLINE_EXCEEDED: Was unable to acquire exclusive initiator access
  ///   and complete the I2C transaction in time.
  /// * @UNAVAILABLE: A NACK condition occurred, meaning the addressed device
  ///   didn't respond or was unable to process the request.
  /// * @FAILED_PRECONDITION: The interface isn't initialized or enabled.
  /// * @UNIMPLEMENTED: The interface doesn't support the necessary I2C
  ///   features or combination of I2C messages.
  ///
  /// @isrsafe{no}
  /// @threadsafe{yes}
  Status TransferFor(span<const Message> messages,
                     chrono::SystemClock::duration timeout) {
    return DoValidateAndTransferFor(messages, timeout);
  }

  /// A variation of `pw::i2c::Initiator::WriteReadFor` that accepts explicit
  /// sizes for the amount of bytes to write to the device and read from the
  /// device.
  Status WriteReadFor(Address device_address,
                      const void* tx_buffer,
                      size_t tx_size_bytes,
                      void* rx_buffer,
                      size_t rx_size_bytes,
                      chrono::SystemClock::duration timeout) {
    return WriteReadFor(
        device_address,
        span(static_cast<const std::byte*>(tx_buffer), tx_size_bytes),
        span(static_cast<std::byte*>(rx_buffer), rx_size_bytes),
        timeout);
  }

  /// Write bytes to the I2C device.
  ///
  /// The signal on the bus should look like this:
  ///
  /// @code
  ///   START + I2C_ADDRESS + WRITE(0) + TX_BUFFER_BYTES + STOP
  /// @endcode
  ///
  /// @param[in] device_address The address of the I2C device.
  ///
  /// @param[in] tx_buffer The transmit buffer.
  ///
  /// @param[in] timeout The maximum duration to block waiting for both
  /// exclusive bus access and the completion of the I2C transaction.
  ///
  /// @pre The provided address must be supported by the initiator. I.e.
  /// don't use a 10-bit address if the initiator only supports 7-bit
  /// addresses. This method fails a runtime assertion if this precondition
  /// isn't met.
  ///
  /// @returns
  /// * @OK: The transaction succeeded.
  /// * @DEADLINE_EXCEEDED: Was unable to acquire exclusive initiator access
  ///   and complete the I2C transaction in time.
  /// * @UNAVAILABLE: A NACK condition occurred, meaning the addressed device
  ///   didn't respond or was unable to process the request.
  /// * @FAILED_PRECONDITION: The interface isn't initialized or enabled.
  /// * @UNIMPLEMENTED: The interface doesn't support the necessary I2C
  ///   features or combination of I2C messages.
  ///
  /// @isrsafe{no}
  /// @threadsafe{yes}
  Status WriteFor(Address device_address,
                  ConstByteSpan tx_buffer,
                  chrono::SystemClock::duration timeout) {
    return WriteReadFor(device_address, tx_buffer, ByteSpan(), timeout);
  }
  /// A variation of `pw::i2c::Initiator::WriteFor` that accepts an explicit
  /// size for the amount of bytes to write to the device.
  Status WriteFor(Address device_address,
                  const void* tx_buffer,
                  size_t tx_size_bytes,
                  chrono::SystemClock::duration timeout) {
    return WriteFor(
        device_address,
        span(static_cast<const std::byte*>(tx_buffer), tx_size_bytes),
        timeout);
  }

  /// Reads bytes from an I2C device.
  ///
  /// The signal on the bus should look like this:
  ///
  /// @code
  ///   START + I2C_ADDRESS + READ(1) + RX_BUFFER_BYTES + STOP
  /// @endcode
  ///
  /// @param[in] device_address The address of the I2C device.
  ///
  /// @param[out] rx_buffer The receive buffer.
  ///
  /// @param[in] timeout The maximum duration to block waiting for both
  /// exclusive bus access and the completion of the I2C transaction.
  ///
  /// @pre The provided address must be supported by the initiator. I.e.
  /// don't use a 10-bit address if the initiator only supports 7-bit
  /// addresses. This method fails a runtime assertion if this precondition
  /// isn't met.
  ///
  /// @returns
  /// * @OK: The transaction succeeded.
  /// * @DEADLINE_EXCEEDED: Was unable to acquire exclusive initiator access
  ///   and complete the I2C transaction in time.
  /// * @UNAVAILABLE: A NACK condition occurred, meaning the addressed device
  ///   didn't respond or was unable to process the request.
  /// * @FAILED_PRECONDITION: The interface isn't initialized or enabled.
  /// * @UNIMPLEMENTED: The interface doesn't support the necessary I2C
  ///   features or combination of I2C messages.
  ///
  /// @isrsafe{no}
  /// @threadsafe{yes}
  Status ReadFor(Address device_address,
                 ByteSpan rx_buffer,
                 chrono::SystemClock::duration timeout) {
    return WriteReadFor(device_address, ConstByteSpan(), rx_buffer, timeout);
  }
  /// A variation of `pw::i2c::Initiator::ReadFor` that accepts an explicit
  /// size for the amount of bytes to read from the device.
  Status ReadFor(Address device_address,
                 void* rx_buffer,
                 size_t rx_size_bytes,
                 chrono::SystemClock::duration timeout) {
    return ReadFor(device_address,
                   span(static_cast<std::byte*>(rx_buffer), rx_size_bytes),
                   timeout);
  }

  /// Probes the device for an I2C ACK after only writing the address.
  /// This is done by attempting to read a single byte from the specified
  /// device.
  ///
  /// @warning This method is not compatible with all devices. For example, some
  /// I2C devices require a device_address in W mode before they can ack the
  /// device_address in R mode. In this case, use WriteReadFor or TransferFor
  /// to read a register with known value.
  ///
  /// @param[in] device_address The address of the I2C device.
  ///
  /// @param[in] timeout The maximum duration to block waiting for both
  /// exclusive bus access and the completion of the I2C transaction.
  ///
  /// @pre The provided address must be supported by the initiator. I.e.
  /// don't use a 10-bit address if the initiator only supports 7-bit
  /// addresses. This method fails a runtime assertion if this precondition
  /// isn't met.
  ///
  /// @returns
  /// * @OK: The transaction succeeded.
  /// * @DEADLINE_EXCEEDED: Was unable to acquire exclusive initiator access
  ///   and complete the I2C transaction in time.
  /// * @UNAVAILABLE: A NACK condition occurred, meaning the addressed device
  ///   didn't respond or was unable to process the request.
  /// * @FAILED_PRECONDITION: The interface isn't initialized or enabled.
  /// * @UNIMPLEMENTED: The interface doesn't support the necessary I2C
  ///   features or combination of I2C messages.
  ///
  /// @isrsafe{no}
  /// @threadsafe{yes}
  Status ProbeDeviceFor(Address device_address,
                        chrono::SystemClock::duration timeout) {
    std::byte ignored_buffer[1] = {};  // Read a byte to probe.
    return WriteReadFor(
        device_address, ConstByteSpan(), ignored_buffer, timeout);
  }

 private:
  /// For backward API compatibility, we default kTenBit to supported.
  static constexpr Feature kCompatibilityFeatures = Feature::kTenBit;

  // This function should not be overridden by future implementations of
  // Initiator unless dealing with an underlying interface that prefers
  // this format.
  // Implement DoTransferFor(Messages) as a preferred course of action.
  //
  // The default implementation of this function will forward calls to
  // TransferFor().
  //
  // Both the read and write parameters should be transmitted in one bus
  // operation using a repeated start condition.
  // If both parameters are present, the write operation is performed first.
  virtual Status DoWriteReadFor(Address,
                                ConstByteSpan,
                                ByteSpan,
                                chrono::SystemClock::duration);

  // This method should be overridden by implementations of Initiator.
  // All messages in one call to DoTransferFor() should be executed
  // as one bus transaction with repeated starts.
  //
  // The default implementation returns Status::Unimplemented().
  virtual Status DoTransferFor(span<const Message> messages,
                               chrono::SystemClock::duration timeout);

  // Function to wrap calls to DoTransferFor() and Validate message vector's
  // contents.
  [[nodiscard]] Status DoValidateAndTransferFor(
      span<const Message> messages, chrono::SystemClock::duration timeout);

  /// Validates a sequence of messages.
  ///
  /// @returns Status::InvalidArgument on error.
  Status ValidateMessages(span<const Message> messages) const;

  /// Validate a message's features against the supported feature set.
  ///
  /// @returns returns pw::Status::Unimplemented if the Initiator does not
  /// support a feature required by this message.
  [[nodiscard]] Status ValidateMessageFeatures(const Message& msg) const;

  /// Checks whether a feature is supported by this Initiator.
  ///
  /// @returns true if the feature is supposed by this Initiator.
  bool IsSupported(Feature feature) const;

  // Features supported by this Initiator.
  Feature supported_features_ = Feature::kStandard;
};

inline constexpr Initiator::Feature operator|(Initiator::Feature a,
                                              Initiator::Feature b) {
  return static_cast<Initiator::Feature>(static_cast<int>(a) |
                                         static_cast<int>(b));
}

}  // namespace pw::i2c
