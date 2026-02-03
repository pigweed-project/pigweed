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
#pragma once

#include "pw_bytes/endian.h"
#include "pw_bytes/span.h"
#include "pw_chrono/system_clock.h"
#include "pw_i2c/address.h"
#include "pw_i2c/device.h"
#include "pw_i2c/initiator.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_status/try.h"

namespace pw {
namespace i2c {

/// @module{pw_i2c}

enum class RegisterAddressSize {
  k1Byte = 1,
  k2Bytes = 2,
  k4Bytes = 4,
};

/// The common interface for I2C register devices. Contains methods to help
/// read and write the device's registers.
///
/// @warning This interface assumes that you know how to consult your device's
/// datasheet to determine correct address sizes, data sizes, endianness, etc.
class RegisterDevice : public Device {
 public:
  /// This constructor specifies the endianness of the register address and
  /// data separately. If your register address and data have the same
  /// endianness and you'd like to specify them both with a single argument,
  /// see the other `pw::i2c::RegisterDevice` constructor.
  ///
  /// @param[in] initiator A `pw::i2c::Initiator` instance for the bus that the
  /// device is on.
  ///
  /// @param[in] address The address of the I2C device.
  ///
  /// @param[in] register_address_order The endianness of the register address.
  ///
  /// @param[in] data_order The endianness of the data.
  ///
  /// @param[in] register_address_size The size of the register address.
  constexpr RegisterDevice(Initiator& initiator,
                           Address address,
                           endian register_address_order,
                           endian data_order,
                           RegisterAddressSize register_address_size)
      : Device(initiator, address),
        register_address_order_(register_address_order),
        data_order_(data_order),
        register_address_size_(register_address_size) {}

  /// This constructor specifies the endianness of the register address and
  /// data with a single argument. If your register address and data have
  /// different endianness, use the other `pw::i2c::RegisterDevice`
  /// constructor.
  ///
  /// @param[in] initiator A `pw::i2c::Initiator` instance for the bus that the
  /// device is on.
  ///
  /// @param[in] address The address of the I2C device.
  ///
  /// @param[in] order The endianness of both the register address and register
  /// data.
  ///
  /// @param[in] register_address_size The size of the register address.
  constexpr RegisterDevice(Initiator& initiator,
                           Address address,
                           endian order,
                           RegisterAddressSize register_address_size)
      : Device(initiator, address),
        register_address_order_(order),
        data_order_(order),
        register_address_size_(register_address_size) {}

  /// Writes data to multiple contiguous registers starting at a specific
  /// register. This method is byte-addressable.
  ///
  /// `register_address` and `register_data` use the endianness that was
  /// provided when this `pw::i2c::RegisterDevice` instance was constructed.
  ///
  /// @pre This method assumes that you've verified that your device supports
  /// bulk writes and that `register_data` is a correct size for your device.
  ///
  /// @param[in] register_address The register address to begin writing at.
  ///
  /// @param[in] register_data The data to write. Endianness is taken into
  /// account if the data is 2 or 4 bytes.
  ///
  /// @param[in] buffer A buffer for constructing the write data. The size of
  /// this buffer must be at least as large as the size of `register_address`
  /// plus the size of `register_data`.
  ///
  /// @param[in] timeout The maximum duration to block waiting for both
  /// exclusive bus access and the completion of the I2C transaction.
  ///
  /// @returns
  /// * @OK: The bulk write was successful.
  /// * @DEADLINE_EXCEEDED: Unable to acquire exclusive bus access and
  ///   complete the transaction in time.
  /// * @FAILED_PRECONDITION: The interface is not initialized or enabled.
  /// * @INTERNAL: An issue occurred while building `register_data`.
  /// * @INVALID_ARGUMENT: `register_address` is larger than the 10-bit address
  ///   space.
  /// * @OUT_OF_RANGE: The size of `buffer` is less than the size of
  ///   `register_address` plus the size of `register_data`.
  /// * @UNAVAILABLE: The device took too long to respond to the NACK.
  Status WriteRegisters(uint32_t register_address,
                        ConstByteSpan register_data,
                        ByteSpan buffer,
                        chrono::SystemClock::duration timeout);

  /// Variant of `pw::i2c::RegisterDevice::WriteRegisters()` that writes
  /// `register_data`, a span of 8-bit values, to the bus.
  Status WriteRegisters8(uint32_t register_address,
                         span<const uint8_t> register_data,
                         ByteSpan buffer,
                         chrono::SystemClock::duration timeout);

  /// Variant of `pw::i2c::RegisterDevice::WriteRegisters()` that writes
  /// `register_data`, a span of 16-bit values, to the bus.
  Status WriteRegisters16(uint32_t register_address,
                          span<const uint16_t> register_data,
                          ByteSpan buffer,
                          chrono::SystemClock::duration timeout);

  /// Variant of `pw::i2c::RegisterDevice::WriteRegisters()` that writes
  /// `register_data`, a span of 32-bit values, to the bus.
  Status WriteRegisters32(uint32_t register_address,
                          span<const uint32_t> register_data,
                          ByteSpan buffer,
                          chrono::SystemClock::duration timeout);

  /// Reads data from multiple contiguous registers starting from a specific
  /// offset or register. This method is byte-addressable.
  ///
  /// `register_address` and `return_data` use the endianness that was
  /// provided when this `pw::i2c::RegisterDevice` instance was constructed.
  ///
  /// @pre This method assumes that you've verified that your device supports
  /// bulk reads and that `return_data` is a correct size for your device.
  ///
  /// @param[in] register_address The register address to begin reading at.
  ///
  /// @param[out] return_data The area to read the data into. The amount of
  /// data that will be read is equal to the size of this span. Endianness is
  /// taken into account if this span is 2 or 4 bytes.
  ///
  /// @param[in] timeout The maximum duration to block waiting for both
  /// exclusive bus access and the completion of the I2C transaction.
  ///
  /// @returns
  /// * @OK: The bulk read was successful.
  /// * @DEADLINE_EXCEEDED: Unable to acquire exclusive bus access and
  ///   complete the transaction in time.
  /// * @FAILED_PRECONDITION: The interface is not initialized or enabled.
  /// * @INTERNAL: An issue occurred while building `return_data`.
  /// * @INVALID_ARGUMENT: `register_address` is larger than the 10-bit
  ///   address space.
  /// * @UNAVAILABLE: The device took too long to respond to the NACK.
  Status ReadRegisters(uint32_t register_address,
                       ByteSpan return_data,
                       chrono::SystemClock::duration timeout);

  /// Variant of `pw::i2c::RegisterDevice::ReadRegisters()` that reads from the
  /// bus into `return_data`, which is a span of 8-bit values.
  Status ReadRegisters8(uint32_t register_address,
                        span<uint8_t> return_data,
                        chrono::SystemClock::duration timeout);

  /// Variant of `pw::i2c::RegisterDevice::ReadRegisters()` that reads from the
  /// bus into `return_data`, which is a span of 16-bit values.
  Status ReadRegisters16(uint32_t register_address,
                         span<uint16_t> return_data,
                         chrono::SystemClock::duration timeout);

  /// Variant of `pw::i2c::RegisterDevice::ReadRegisters()` that reads from the
  /// bus into `return_data`, which is a span of 32-bit values.
  Status ReadRegisters32(uint32_t register_address,
                         span<uint32_t> return_data,
                         chrono::SystemClock::duration timeout);

  /// Sends a register address to write to and then writes to that address.
  ///
  /// `register_address` and `register_data` use the endianness that was
  /// provided when this `pw::i2c::RegisterDevice` instance was constructed.
  ///
  /// @pre This method assumes that you've verified that `register_data` is a
  /// correct size for your device.
  ///
  /// @param[in] register_address The register address to write to.
  ///
  /// @param[in] register_data The data that should be written at the address.
  /// The maximum allowed size is 4 bytes.
  ///
  /// @param[in] timeout The maximum duration to block waiting for both
  /// exclusive bus access and the completion of the I2C transaction.
  ///
  /// @returns
  /// * @OK: The write was successful.
  /// * @DEADLINE_EXCEEDED: Unable to acquire exclusive bus access and complete
  ///   the transaction in time.
  /// * @FAILED_PRECONDITION: The interface is not initialized or enabled.
  /// * @INTERNAL: An issue occurred while writing the data.
  /// * @INVALID_ARGUMENT: `register_address` is larger than the 10-bit address
  ///   space.
  /// * @UNAVAILABLE: The device took too long to respond to the NACK.
  Status WriteRegister(uint32_t register_address,
                       std::byte register_data,
                       chrono::SystemClock::duration timeout);

  /// Variant of `pw::i2c::RegisterDevice::WriteRegister()` that writes exactly
  /// 8 bits to the bus.
  Status WriteRegister8(uint32_t register_address,
                        uint8_t register_data,
                        chrono::SystemClock::duration timeout);

  /// Variant of `pw::i2c::RegisterDevice::WriteRegister()` that writes exactly
  /// 16 bits to the bus.
  Status WriteRegister16(uint32_t register_address,
                         uint16_t register_data,
                         chrono::SystemClock::duration timeout);

  /// Variant of `pw::i2c::RegisterDevice::WriteRegister()` that writes exactly
  /// 32 bits to the bus.
  Status WriteRegister32(uint32_t register_address,
                         uint32_t register_data,
                         chrono::SystemClock::duration timeout);

  /// Sends a register address to read from and then reads from that address.
  ///
  /// `register_address` and the return data use the endianness that was
  /// provided when this `pw::i2c::RegisterDevice` instance was constructed.
  ///
  /// @pre This method assumes that you've verified that the return data size
  /// is a correct size for your device.
  ///
  /// @param[in] register_address The register address to read.
  ///
  /// @param[in] timeout The maximum duration to block waiting for both
  /// exclusive bus access and the completion of the I2C transaction.
  ///
  /// @returns
  /// * @OK: Returns the register data.
  /// * @DEADLINE_EXCEEDED: Unable to acquire exclusive bus access and
  ///   complete the transaction in time.
  /// * @FAILED_PRECONDITION: The interface is not initialized or enabled.
  /// * @INTERNAL: An issue occurred while building the return data.
  /// * @INVALID_ARGUMENT: `register_address` is larger than the 10-bit address
  ///   space.
  /// * @UNAVAILABLE: The device took too long to respond to the NACK.
  Result<std::byte> ReadRegister(uint32_t register_address,
                                 chrono::SystemClock::duration timeout);

  /// Variant of `pw::i2c::RegisterDevice::ReadRegister()` that returns exactly
  /// 8 bits read from the bus.
  Result<uint8_t> ReadRegister8(uint32_t register_address,
                                chrono::SystemClock::duration timeout);

  /// Variant of `pw::i2c::RegisterDevice::ReadRegister()` that returns exactly
  /// 16 bits read from the bus.
  Result<uint16_t> ReadRegister16(uint32_t register_address,
                                  chrono::SystemClock::duration timeout);

  /// Variant of `pw::i2c::RegisterDevice::ReadRegister()` that returns exactly
  /// 32 bits read from the bus.
  Result<uint32_t> ReadRegister32(uint32_t register_address,
                                  chrono::SystemClock::duration timeout);

 private:
  // Helper write registers.
  Status WriteRegisters(uint32_t register_address,
                        ConstByteSpan register_data,
                        const size_t register_data_size,
                        ByteSpan buffer,
                        chrono::SystemClock::duration timeout);

  const endian register_address_order_;
  const endian data_order_;
  const RegisterAddressSize register_address_size_;
};

inline Status RegisterDevice::WriteRegisters(
    uint32_t register_address,
    ConstByteSpan register_data,
    ByteSpan buffer,
    chrono::SystemClock::duration timeout) {
  return WriteRegisters(register_address,
                        register_data,
                        sizeof(decltype(register_data)::value_type),
                        buffer,
                        timeout);
}

inline Status RegisterDevice::WriteRegisters8(
    uint32_t register_address,
    span<const uint8_t> register_data,
    ByteSpan buffer,
    chrono::SystemClock::duration timeout) {
  return WriteRegisters(register_address,
                        as_bytes(register_data),
                        sizeof(decltype(register_data)::value_type),
                        buffer,
                        timeout);
}

inline Status RegisterDevice::WriteRegisters16(
    uint32_t register_address,
    span<const uint16_t> register_data,
    ByteSpan buffer,
    chrono::SystemClock::duration timeout) {
  return WriteRegisters(register_address,
                        as_bytes(register_data),
                        sizeof(decltype(register_data)::value_type),
                        buffer,
                        timeout);
}

inline Status RegisterDevice::WriteRegisters32(
    uint32_t register_address,
    span<const uint32_t> register_data,
    ByteSpan buffer,
    chrono::SystemClock::duration timeout) {
  return WriteRegisters(register_address,
                        as_bytes(register_data),
                        sizeof(decltype(register_data)::value_type),
                        buffer,
                        timeout);
}

inline Status RegisterDevice::WriteRegister(
    uint32_t register_address,
    std::byte register_data,
    chrono::SystemClock::duration timeout) {
  std::array<std::byte, sizeof(register_data) + sizeof(register_address)>
      byte_buffer;
  return WriteRegisters(register_address,
                        span(&register_data, 1),
                        sizeof(register_data),
                        byte_buffer,
                        timeout);
}

inline Status RegisterDevice::WriteRegister8(
    uint32_t register_address,
    uint8_t register_data,
    chrono::SystemClock::duration timeout) {
  std::array<std::byte, sizeof(register_data) + sizeof(register_address)>
      byte_buffer;
  return WriteRegisters(register_address,
                        as_bytes(span(&register_data, 1)),
                        sizeof(register_data),
                        byte_buffer,
                        timeout);
}

inline Status RegisterDevice::WriteRegister16(
    uint32_t register_address,
    uint16_t register_data,
    chrono::SystemClock::duration timeout) {
  std::array<std::byte, sizeof(register_data) + sizeof(register_address)>
      byte_buffer;
  return WriteRegisters(register_address,
                        as_bytes(span(&register_data, 1)),
                        sizeof(register_data),
                        byte_buffer,
                        timeout);
}

inline Status RegisterDevice::WriteRegister32(
    uint32_t register_address,
    uint32_t register_data,
    chrono::SystemClock::duration timeout) {
  std::array<std::byte, sizeof(register_data) + sizeof(register_address)>
      byte_buffer;
  return WriteRegisters(register_address,
                        as_bytes(span(&register_data, 1)),
                        sizeof(register_data),
                        byte_buffer,
                        timeout);
}

inline Status RegisterDevice::ReadRegisters8(
    uint32_t register_address,
    span<uint8_t> return_data,
    chrono::SystemClock::duration timeout) {
  // For a single byte, there's no endian data, and so we can return the
  // data as is.
  return ReadRegisters(
      register_address, as_writable_bytes(return_data), timeout);
}

inline Status RegisterDevice::ReadRegisters16(
    uint32_t register_address,
    span<uint16_t> return_data,
    chrono::SystemClock::duration timeout) {
  PW_TRY(
      ReadRegisters(register_address, as_writable_bytes(return_data), timeout));

  // Post process endian information.
  for (uint16_t& register_value : return_data) {
    register_value = bytes::ReadInOrder<uint16_t>(data_order_, &register_value);
  }

  return pw::OkStatus();
}

inline Status RegisterDevice::ReadRegisters32(
    uint32_t register_address,
    span<uint32_t> return_data,
    chrono::SystemClock::duration timeout) {
  PW_TRY(
      ReadRegisters(register_address, as_writable_bytes(return_data), timeout));

  // TODO: b/185952662 - Extend endian in pw_byte to support this conversion
  //                    as optimization.
  // Post process endian information.
  for (uint32_t& register_value : return_data) {
    register_value = bytes::ReadInOrder<uint32_t>(data_order_, &register_value);
  }

  return pw::OkStatus();
}

inline Result<std::byte> RegisterDevice::ReadRegister(
    uint32_t register_address, chrono::SystemClock::duration timeout) {
  std::byte data = {};
  PW_TRY(ReadRegisters(register_address, span(&data, 1), timeout));
  return data;
}

inline Result<uint8_t> RegisterDevice::ReadRegister8(
    uint32_t register_address, chrono::SystemClock::duration timeout) {
  uint8_t data = 0;
  PW_TRY(ReadRegisters8(register_address, span(&data, 1), timeout));
  return data;
}

inline Result<uint16_t> RegisterDevice::ReadRegister16(
    uint32_t register_address, chrono::SystemClock::duration timeout) {
  std::array<uint16_t, 1> data = {};
  PW_TRY(ReadRegisters16(register_address, data, timeout));
  return data[0];
}

inline Result<uint32_t> RegisterDevice::ReadRegister32(
    uint32_t register_address, chrono::SystemClock::duration timeout) {
  std::array<uint32_t, 1> data = {};
  PW_TRY(ReadRegisters32(register_address, data, timeout));
  return data[0];
}

}  // namespace i2c
}  // namespace pw

// TODO (zengk): Register modification.
