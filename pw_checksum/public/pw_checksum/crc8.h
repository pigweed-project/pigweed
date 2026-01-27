// Copyright 2026 The Pigweed Authors
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

#include "pw_span/span.h"

namespace pw::checksum {

// A class for calculating 8-bit Cyclic Redundancy Checks (CRCs).
//
// This class supports various CRC-8 algorithms by configuring the polynomial,
// initial value, reflection settings, and final XOR value.
class Crc8 {
 public:
  /// @brief Constructs a Crc8 calculator with the specified parameters.
  ///
  /// @param polynomial The polynomial used for the CRC calculation.
  /// @param initial_value The initial value of the CRC register.
  /// @param reflect_in Whether the input bytes should be reflected before
  ///                   processing.
  /// @param reflect_out Whether the final CRC value should be reflected.
  /// @param xor_out The value to XOR with the final CRC result.
  constexpr Crc8(uint8_t polynomial,
                 uint8_t initial_value,
                 bool reflect_in,
                 bool reflect_out,
                 uint8_t xor_out)
      : polynomial_(polynomial),
        initial_value_(initial_value),
        reflect_in_(reflect_in),
        reflect_out_(reflect_out),
        xor_out_(xor_out) {}

  /// @brief Calculates the CRC-8 value for the given data.
  ///
  /// @param data The data to calculate the CRC-8 for.
  /// @return The CRC-8 value.
  constexpr uint8_t Calculate(pw::span<const std::byte> data) const;

  /// CRC-8: poly=0x07, init=0x00, refin=false, refout=false, xout=0x00
  static const Crc8 kCrc8;

  /// CRC-8/MAXIM: poly=0x31, init=0x00, refin=true, refout=true, xout=0x00
  static const Crc8 kMaxim;

  /// CRC-8/WCDMA: poly=0x9B, init=0x00, refin=true, refout=true, xout=0x00
  static const Crc8 kWcdma;

  /// CRC-8/ITU: poly=0x07, init=0x00, refin=false, refout=false, xout=0x55
  static const Crc8 kItu;

  /// CRC-8/ROHC: poly=0x07, init=0xFF, refin=true, refout=true, xout=0x00
  static const Crc8 kRohc;

 private:
  const uint8_t polynomial_;
  const uint8_t initial_value_;
  const bool reflect_in_;
  const bool reflect_out_;
  const uint8_t xor_out_;
};

namespace internal {

constexpr uint8_t Reflect(uint8_t value) {
  uint8_t reflected = 0;
  for (uint8_t i = 0; i < 8; ++i) {
    if ((value >> i) & 1) {
      reflected |= 1 << (7 - i);
    }
  }
  return reflected;
}

}  // namespace internal

constexpr uint8_t Crc8::Calculate(pw::span<const std::byte> data) const {
  uint8_t crc = initial_value_;
  for (std::byte byte : data) {
    uint8_t current_byte = static_cast<uint8_t>(byte);
    if (reflect_in_) {
      current_byte = internal::Reflect(current_byte);
    }
    crc ^= current_byte;
    for (uint8_t i = 0; i < 8; ++i) {
      if (crc & 0x80) {
        crc = static_cast<uint8_t>((crc << 1) ^ polynomial_);
      } else {
        crc <<= 1;
      }
    }
  }

  if (reflect_out_) {
    crc = internal::Reflect(crc);
  }

  return crc ^ xor_out_;
}

inline constexpr Crc8 Crc8::kCrc8(0x07, 0x00, false, false, 0x00);
inline constexpr Crc8 Crc8::kMaxim(0x31, 0x00, true, true, 0x00);
inline constexpr Crc8 Crc8::kWcdma(0x9B, 0x00, true, true, 0x00);
inline constexpr Crc8 Crc8::kItu(0x07, 0x00, false, false, 0x55);
inline constexpr Crc8 Crc8::kRohc(0x07, 0xFF, true, true, 0x00);

}  // namespace pw::checksum
