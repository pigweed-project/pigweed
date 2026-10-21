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

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "lib/stdcompat/bit.h"

namespace pw {

/// @submodule{pw_containers,sets}

/// A `constexpr`-friendly fixed-size sequence of bits, similar to
/// `std::bitset`.
///
/// This container allows for compile-time manipulation of a fixed number of
/// bits. It supports common bitwise operations and is optimized for size by
/// using the smallest possible underlying integer type.
///
/// @tparam kBits The number of bits in the set (must be <= 64).
template <size_t kBits>
class BitSet {
 public:
  /// The underlying type used to store the bits. Uses the smallest integer type
  /// capable of representing the bits.
  using value_type = std::conditional_t<
      kBits <= 8,
      uint8_t,
      std::conditional_t<kBits <= 16,
                         uint16_t,
                         std::conditional_t<kBits <= 32, uint32_t, uint64_t>>>;

  /// Initializes a bitset from an integer (e.g. 0b1010). The integer must fit
  /// within `size()` bits.
  template <unsigned long long kValue>
  static constexpr BitSet Of() {
    static_assert(cpp20::countl_zero(kValue) >= sizeof(kValue) * 8 - kBits,
                  "The value must fit within the BitSet");
    return BitSet(static_cast<value_type>(kValue));
  }

  /// Initialize a bitset from `true`/`false` or `1`/`0`, with the LEAST
  /// significant bit first.
  template <typename... Args,
            typename = std::enable_if_t<(std::is_same_v<Args, bool> && ...)>>
  static constexpr BitSet LittleEndian(Args... bits_least_to_most_significant) {
    static_assert(sizeof...(Args) == kBits,
                  "One bool argument must be provided for each bit");
    return BitSet(LittleBoolsToBits(bits_least_to_most_significant...));
  }

  constexpr BitSet() : BitSet(0) {}

  constexpr BitSet(const BitSet&) = default;
  constexpr BitSet& operator=(const BitSet&) = default;

  template <size_t kOtherBits, typename = std::enable_if_t<kBits >= kOtherBits>>
  constexpr BitSet(const BitSet<kOtherBits>& other) : bits_{other.bits_} {}

  template <size_t kOtherBits, typename = std::enable_if_t<kBits >= kOtherBits>>
  constexpr BitSet& operator=(const BitSet& other) {
    bits_ = other.bits_;
  }

  // Observers

  [[nodiscard]] constexpr bool operator==(const BitSet& other) const {
    return bits_ == other.bits_;
  }
  [[nodiscard]] constexpr bool operator!=(const BitSet& other) const {
    return bits_ != other.bits_;
  }

  template <size_t kBit>
  [[nodiscard]] constexpr bool test() const {
    CheckBitIndices<kBit>();
    return (bits_ & (value_type{1} << kBit)) != 0;
  }

  [[nodiscard]] constexpr bool all() const {
    if constexpr (kBits == 0) {
      return true;
    }
    return bits_ == kAllSet;
  }

  [[nodiscard]] constexpr bool any() const { return bits_ != 0; }
  [[nodiscard]] constexpr bool none() const { return bits_ == 0; }

  [[nodiscard]] constexpr size_t count() const {
    return static_cast<size_t>(cpp20::popcount(bits_));
  }
  [[nodiscard]] constexpr size_t size() const { return kBits; }

  // Modifiers

  constexpr BitSet& set() {
    bits_ = kAllSet;
    return *this;
  }

  template <size_t... kBitsToSet>
  constexpr BitSet& set() {
    CheckBitIndices<kBitsToSet...>();
    bits_ |= ((value_type{1} << kBitsToSet) | ...);
    return *this;
  }

  constexpr BitSet& reset() {
    bits_ = 0;
    return *this;
  }

  template <size_t... kBitsToReset>
  constexpr BitSet& reset() {
    CheckBitIndices<kBitsToReset...>();
    bits_ &= ~((value_type{1} << kBitsToReset) | ...);
    return *this;
  }

  constexpr BitSet& flip() {
    bits_ ^= kAllSet;
    return *this;
  }

  template <size_t... kBitsToFlip>
  constexpr BitSet& flip() {
    CheckBitIndices<kBitsToFlip...>();
    bits_ ^= ((value_type{1} << kBitsToFlip) | ...);
    return *this;
  }

  // Bitwise operators

  constexpr BitSet& operator&=(const BitSet& other) {
    bits_ &= other.bits_;
    return *this;
  }
  constexpr BitSet& operator|=(const BitSet& other) {
    bits_ |= other.bits_;
    return *this;
  }
  constexpr BitSet& operator^=(const BitSet& other) {
    bits_ ^= other.bits_;
    return *this;
  }

  constexpr BitSet& operator<<=(size_t pos) {
    bits_ = (bits_ << pos) & kAllSet;
    return *this;
  }

  constexpr BitSet& operator>>=(size_t pos) {
    bits_ >>= pos;
    return *this;
  }

  constexpr BitSet operator~() const {
    return BitSet(static_cast<value_type>(~bits_ & kAllSet));
  }

  friend constexpr BitSet operator&(const BitSet& lhs, const BitSet& rhs) {
    return BitSet(lhs.bits_ & rhs.bits_);
  }
  friend constexpr BitSet operator|(const BitSet& lhs, const BitSet& rhs) {
    return BitSet(lhs.bits_ | rhs.bits_);
  }
  friend constexpr BitSet operator^(const BitSet& lhs, const BitSet& rhs) {
    return BitSet(lhs.bits_ ^ rhs.bits_);
  }

  friend constexpr BitSet operator<<(const BitSet& bs, size_t pos) {
    return BitSet((bs.bits_ << pos) & kAllSet);
  }

  friend constexpr BitSet operator>>(const BitSet& bs, size_t pos) {
    return BitSet(bs.bits_ >> pos);
  }

  /// Returns the bit set as an integer.
  constexpr value_type to_integer() const { return bits_; }

 private:
  static_assert(kBits <= 64, "BitSet currently only supports up to 64 bits");

  template <size_t>
  friend class BitSet;

  template <size_t... kIndices>
  static constexpr void CheckBitIndices() {
    static_assert(((kIndices < kBits) && ...),
                  "The specified bit index is out of range of this BitSet");
  }

  static constexpr value_type LittleBoolsToBits() { return 0; }

  template <typename... Args,
            typename = std::enable_if_t<(std::is_same_v<Args, bool> && ...)>>
  static constexpr value_type LittleBoolsToBits(bool first, Args... bits) {
    return static_cast<value_type>(
        (first ? (value_type{1} << (kBits - sizeof...(Args) - 1)) : 0) |
        LittleBoolsToBits(bits...));
  }

  explicit constexpr BitSet(value_type bits) : bits_(bits) {}

  static constexpr value_type kAllSet =
      static_cast<value_type>(~value_type{0}) >>
      (sizeof(value_type) * 8 - kBits);

  value_type bits_;
};

/// @}

// Specialization of `BitSet` for zero bits.
//
// This specialization provides a zero-size bitset with a minimal API,
// satisfying the requirements of a bitset while consuming no storage.
template <>
class BitSet<0> {
 public:
  using value_type = uint8_t;

  constexpr BitSet() = default;

  constexpr BitSet(const BitSet&) = default;
  constexpr BitSet& operator=(const BitSet&) = default;

  static constexpr BitSet LittleEndian() { return BitSet(); }

  [[nodiscard]] constexpr bool operator==(const BitSet&) const { return true; }
  [[nodiscard]] constexpr bool operator!=(const BitSet&) const { return false; }

  template <size_t kBit>
  [[nodiscard]] constexpr bool test() const {
    static_assert(kBit < 0, "test() is not supported for zero-bit BitSets");
    return false;
  }

  [[nodiscard]] constexpr bool all() const { return true; }
  [[nodiscard]] constexpr bool any() const { return false; }
  [[nodiscard]] constexpr bool none() const { return true; }

  constexpr size_t count() const { return 0; }
  constexpr size_t size() const { return 0; }

  // Indiviadual bit variants of set, reset, and flip are not supported.
  constexpr BitSet& set() { return *this; }
  constexpr BitSet& reset() { return *this; }
  constexpr BitSet& flip() { return *this; }

  constexpr BitSet& operator&=(const BitSet&) { return *this; }
  constexpr BitSet& operator|=(const BitSet&) { return *this; }
  constexpr BitSet& operator^=(const BitSet&) { return *this; }

  constexpr BitSet& operator<<=(size_t) { return *this; }
  constexpr BitSet& operator>>=(size_t) { return *this; }

  constexpr BitSet operator~() const { return *this; }

  friend constexpr BitSet operator&(const BitSet&, const BitSet&) {
    return BitSet();
  }
  friend constexpr BitSet operator|(const BitSet&, const BitSet&) {
    return BitSet();
  }
  friend constexpr BitSet operator^(const BitSet&, const BitSet&) {
    return BitSet();
  }

  friend constexpr BitSet operator<<(const BitSet&, size_t) { return BitSet(); }
  friend constexpr BitSet operator>>(const BitSet&, size_t) { return BitSet(); }

  constexpr value_type to_integer() const { return 0; }
};

}  // namespace pw
