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

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "pw_assert/assert.h"
#include "pw_bytes/span.h"
#include "pw_containers/algorithm.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_string/hex.h"
#include "pw_string/string.h"

/// 128-bit universally unique identifier library
namespace pw::uuid {

/// @module{pw_uuid}

/// Represents a 128-bit universally unique identifier (UUID).
class Uuid {
 public:
  /// Size of the UUID in bytes.
  static constexpr size_t kSizeBytes = 16;
  /// Length of the UUID's string representation.
  static constexpr size_t kStringSize =
      std::string_view{"xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"}.size();

  // Default constructor initializes the uuid to the Nil UUID
  constexpr explicit Uuid() : uuid_() {}

  /// @brief Creates a Uuid from a pw::span of 16 const bytes.
  ///
  /// This constructor is `constexpr` and guaranteed to be successful as the
  /// size is enforced by the type system.
  ///
  /// @param uuid_span A span containing the 16 bytes of the UUID.
  constexpr explicit Uuid(span<const uint8_t, kSizeBytes> uuid_span) : uuid_() {
    pw::copy(uuid_span.begin(), uuid_span.end(), uuid_.begin());
  }

  /// @brief Creates a Uuid from a pw::span of 16 const std::byte.
  ///
  /// This constructor is `constexpr` and guaranteed to be successful as the
  /// size is enforced by the type system.
  ///
  /// @param uuid_span A span containing the 16 bytes of the UUID.
  constexpr explicit Uuid(span<const std::byte, kSizeBytes> uuid_span)
      : uuid_() {
    // Manually copy to use static_cast which is constexpr
    for (size_t i = 0; i < uuid_.size(); i++) {
      uuid_[i] = static_cast<uint8_t>(uuid_span[i]);
    }
  }

  /// @brief Create a Uuid from a const uint8_t span.
  ///
  /// @param uuid_span A span containing the UUID bytes.
  static constexpr Result<Uuid> FromSpan(span<const uint8_t> uuid_span) {
    if (uuid_span.size() != kSizeBytes) {
      return Status::FailedPrecondition();
    }
    return Uuid(
        span<const uint8_t, kSizeBytes>(uuid_span.data(), uuid_span.size()));
  }

  /// @brief Create a Uuid from a const std::byte span.
  ///
  /// @param uuid_span A span containing the UUID bytes.
  static constexpr Result<Uuid> FromSpan(ConstByteSpan uuid_span) {
    if (uuid_span.size() != kSizeBytes) {
      return Status::FailedPrecondition();
    }
    return Uuid(
        span<const std::byte, kSizeBytes>(uuid_span.data(), uuid_span.size()));
  }

  /// @brief Creates a Uuid from a std::array of 16 const uint8_t.
  ///
  /// This is guaranteed to be successful as the size is enforced by the type
  /// system.
  ///
  /// @param uuid_array A std::array containing the 16 bytes of the UUID.
  template <size_t kSize, typename = std::enable_if_t<kSize == kSizeBytes>>
  static constexpr Uuid FromSpan(const std::array<uint8_t, kSize>& uuid_array) {
    return Uuid(uuid_array);
  }

  /// @brief Creates a Uuid from a std::array of 16 const bytes.
  ///
  /// This is guaranteed to be successful as the size is enforced by the type
  /// system.
  ///
  /// @param uuid_array A std::array containing the 16 bytes of the UUID.
  template <size_t kSize, typename = std::enable_if_t<kSize == kSizeBytes>>
  static constexpr Uuid FromSpan(
      const std::array<std::byte, kSize>& uuid_array) {
    return Uuid(uuid_array);
  }

  /// @brief Creates a Uuid from a C-style array of 16 const uint8_t.
  ///
  /// This is guaranteed to be successful as the size is enforced by the type
  /// system.
  ///
  /// @param uuid_array A C-style array containing the 16 bytes of the UUID.
  template <size_t kSize, typename = std::enable_if_t<kSize == kSizeBytes>>
  static constexpr Uuid FromSpan(const uint8_t (&uuid_array)[kSize]) {
    return Uuid(span<const uint8_t, kSizeBytes>(uuid_array));
  }

  /// @brief Creates a Uuid from a C-style array of 16 const bytes.
  ///
  /// This is guaranteed to be successful as the size is enforced by the type
  /// system.
  ///
  /// @param uuid_array A C-style array containing the 16 bytes of the UUID.
  template <size_t kSize, typename = std::enable_if_t<kSize == kSizeBytes>>
  static constexpr Uuid FromSpan(const std::byte (&uuid_array)[kSize]) {
    return Uuid(span<const std::byte, kSizeBytes>(uuid_array));
  }

  /// @brief Create a Uuid from a string.
  ///
  /// @param string A string representation of the UUID.
  static constexpr Result<Uuid> FromString(std::string_view string) {
    Status status = ValidateString(string);
    if (!status.ok()) {
      return status;
    }
    return Uuid(string);
  }

  /// Return the backing span holding the uuid
  constexpr span<const uint8_t, kSizeBytes> GetSpan() const { return uuid_; }

  constexpr bool operator==(const Uuid& other) const {
    for (size_t i = 0; i < kSizeBytes; i++) {
      if (uuid_[i] != other.uuid_[i]) {
        return false;
      }
    }
    return true;
  }

  constexpr bool operator!=(const Uuid& other) const {
    return !(*this == other);
  }

  /// Convert the Uuid to a human readable string
  constexpr InlineString<kStringSize> ToString() const {
    InlineString<kStringSize> out = {};

    for (size_t i = uuid_.size(); i-- != 0;) {
      out += string::NibbleToHex(uuid_[i] >> 4);
      out += string::NibbleToHex(uuid_[i] & 0xf);
      if ((i == 12) || (i == 10) || (i == 8) || (i == 6)) {
        out += '-';
      }
    }
    return out;
  }

 private:
  /// Create a Uuid from a string
  ///
  /// @param uuid_str string containing uuid
  constexpr explicit Uuid(std::string_view uuid_str) : uuid_() {
    size_t out_hex_index = 2 * uuid_.size();  // UUID is stored little-endian.
    for (size_t i = 0; i < kStringSize; i++) {
      // Indices at which we expect to find a hyphen ('-') in a UUID string.
      if (i == 8 || i == 13 || i == 18 || i == 23) {
        PW_ASSERT(uuid_str[i] == '-');
        continue;
      }
      PW_ASSERT(uuid_str[i] != '\0');

      out_hex_index--;
      uint16_t value = string::HexToNibble(uuid_str[i]);
      PW_ASSERT(value <= 0xf);
      if (out_hex_index % 2 == 0) {
        uuid_[out_hex_index / 2] |= static_cast<uint8_t>(value);
      } else {
        uuid_[out_hex_index / 2] = static_cast<uint8_t>(value << 4);
      }
    }
  }

  /// Validate a UUID string
  ///
  /// @param uuid_str string containing uuid
  ///
  /// @returns
  /// * @OK: The UUID is valid.
  /// * @FAILED_PRECONDITION: The string is the wrong size.
  /// * @INVALID_ARGUMENT: The string is malformed.
  static constexpr Status ValidateString(std::string_view uuid_str) {
    if (kStringSize != uuid_str.size()) {
      return Status::FailedPrecondition();
    }
    for (size_t i = 0; i < uuid_str.size(); i++) {
      if (i == 8 || i == 13 || i == 18 || i == 23) {
        if (uuid_str[i] != '-') {
          return Status::InvalidArgument();
        }
        continue;
      }
      if (string::IsHexDigit(uuid_str[i]) == 0) {
        return Status::InvalidArgument();
      }
    }
    return OkStatus();
  }

  std::array<uint8_t, kSizeBytes> uuid_;
};

}  // namespace pw::uuid
