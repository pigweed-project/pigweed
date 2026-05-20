// Copyright 2025 The Pigweed Authors
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

namespace pw::i2c {

/// @module{pw_i2c}

/// A helper class that represents I2C addresses.
///
/// An address instance remembers whether it was constructed as a seven-bit or
/// ten-bit address. This attribute can be used by Initiators to determine the
/// i2c addressing style to transmit.
///
/// Note: Per the above, a ten-bit constructed instance may still have an
/// an address of seven or fewer bits.
///
/// @code{.cpp}
///   #include "pw_i2c/address.h"
///
///   constexpr pw::i2c::Address kAddress1 = pw::i2c::Address::SevenBit<0x42>();
///   uint8_t raw_address_1 = kAddress1.GetSevenBit();
///
///   const pw::i2c::Address kAddress2<0x200>();  // 10-bit
///   uint16_t raw_address_2 = kAddress2.GetAddress();
///   // Note: kAddress2.GetSevenBit() would fail an assertion here.
/// @endcode
class Address {
 public:
  static constexpr uint8_t kMaxSevenBitAddress = (1 << 7) - 1;
  static constexpr uint16_t kMaxTenBitAddress = (1 << 10) - 1;

  /// Creates a `pw::i2c::Address` instance for an address that's 10 bits
  /// or less.
  ///
  /// This constant expression does a compile-time assertion to ensure that the
  /// provided address is 10 bits or less.
  ///
  /// @code{.cpp}
  ///   constexpr pw::i2c::Address kAddress = pw::i2c::Address::TenBit<0x200>();
  /// @endcode
  ///
  /// @returns A `pw::i2c::Address` instance.
  template <uint16_t kAddress>
  static constexpr Address TenBit() {
    static_assert(kAddress <= kMaxTenBitAddress);
    return Address(kAddress, Mode::kTenBit, kAlreadyCheckedAddress);
  }

  /// Creates a `pw::i2c::Address` instance for an address that's 10 bits
  /// or less.
  ///
  /// This constructor does a run-time check to ensure that the provided address
  /// is 10 bits or less.
  ///
  /// @code{.cpp}
  ///   pw::i2c::Address kAddress = pw::i2c::Address::TenBit(0x200);
  /// @endcode
  ///
  /// @returns A `pw::i2c::Address` instance.
  static Address TenBit(uint16_t address) {
    return Address(address, Mode::kTenBit);
  }

  /// Creates a `pw::i2c::Address` instance for an address that's 7 bits
  /// or less.
  ///
  /// This constant expression does a compile-time assertion to ensure that the
  /// provided address is 7 bits or less.
  ///
  /// @code{.cpp}
  ///   constexpr pw::i2c::Address kAddress =
  ///       pw::i2c::Address::SevenBit<0x42>();
  /// @endcode
  ///
  /// @returns A `pw::i2c::Address` instance.
  template <uint8_t kAddress>
  static constexpr Address SevenBit() {
    static_assert(kAddress <= kMaxSevenBitAddress);
    return Address(kAddress, Mode::kSevenBit, kAlreadyCheckedAddress);
  }

  /// Creates a `pw::i2c::Address` instance for an address that's 7 bits
  /// or less.
  ///
  /// This constructor does a run-time check to ensure that the provided address
  /// is 7 bits or less.
  ///
  /// @code{.cpp}
  ///   pw::i2c::Address kAddress = pw::i2c::Address::SevenBit(0x42);
  /// @endcode
  ///
  /// @returns A `pw::i2c::Address` instance.
  static Address SevenBit(uint16_t address) {
    return Address(address, Mode::kSevenBit);
  }

  /// Creates a `pw::i2c::Address` instance from a 7 or 10 bit address.
  ///
  /// @note This function is deprecated. You should almost certainly use
  /// either Address::SevenBit<0x1>() for addresses known at compile time, or
  /// Address::SevenBit(0x1) for addresses known at run-time.
  ///
  /// If the address argument is 7-bits or less, a 7-bit address is constructed
  /// equivalent to Address::SevenBit(address);
  ///
  /// If the address argument is 8, 9 or 10 bits, a ten-bit address is
  /// constructed equivalent to Address::TenBit(address);
  ///
  /// The type of address construced will affect how the i2c address is
  /// transmitted on the bus. You should always use 7-bit addresses unless
  /// you are certain you have a host and device that support 10-bit addresses.
  ///
  /// @param[in] address An address no larger than 10-bits as an unsigned
  /// integer. This method does a runtime assertion to ensure that address
  /// is 10 bits or less.
  ///
  /// @code{.cpp}
  ///   constexpr pw::i2c::Address kAddress(0x200);
  /// @endcode
  [[deprecated("Use one of the factory methods above to construct safely.")]]
  explicit Address(uint16_t address);

  /// Gets the 7-bit address that was provided when this instance was created.
  ///
  /// This method does a runtime assertion to ensure that the address was
  /// constructed in 7-bit mode.
  ///
  /// @returns A 7-bit address as an unsigned integer.
  uint8_t GetSevenBit() const;

  /// Gets the 10-bit address that was provided when this instance was created.
  ///
  /// @returns A 10-bit address as an unsigned integer.
  [[deprecated("Use GetAddress() and IsTenBit() as appropriate.")]]
  uint16_t GetTenBit() const {
    return address_;
  }

  /// Gets the raw address that was provided when this Address was created.
  ///
  /// Use IsTenBit() to know whether the address should be interpreted as
  /// a 7-bit or 10-bit address.
  ///
  /// @returns A an address as an unsigned integer.
  uint16_t GetAddress() const { return address_; }

  /// Operator for testing equality of two Address objects.
  ///
  /// @returns true if the two Address objects are the same.
  friend bool operator==(const Address& a1, const Address& a2);

  /// Getter for whether this object represents a Ten bit address.
  /// Note: The address itself may still be fewer than 10 bits.
  ///
  /// @returns true if the address represents a 10-bit address.
  constexpr bool IsTenBit() const { return mode_ == Mode::kTenBit; }

 private:
  enum class Mode : uint8_t {
    kSevenBit = 0,
    kTenBit = 1,
  };

  Address(uint16_t address, Mode mode);

  enum AlreadyCheckedAddress { kAlreadyCheckedAddress };
  constexpr Address(uint16_t address, Mode mode, AlreadyCheckedAddress)
      : address_(address), mode_(mode) {}

  uint16_t address_ : 10;
  Mode mode_ : 1;
};

// Compares two address objects.
inline bool operator==(const Address& a1, const Address& a2) {
  return a1.address_ == a2.address_ && a1.mode_ == a2.mode_;
}

}  // namespace pw::i2c
