// Copyright 2022 The Pigweed Authors
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
#include <climits>
#include <cstdint>
#include <string_view>

#include "pw_assert/assert.h"
#include "pw_result/result.h"
#include "pw_span/span.h"
#include "pw_string/hex.h"
#include "pw_string/string.h"
#include "pw_uuid/uuid.h"

namespace pw::bluetooth {

// A 128-bit Universally Unique Identifier (UUID).
// See Core Spec v5.3 Volume 3, Part B, Section 2.5.1.
//
// Bluetooth defines 16-bit, 32-bit and 128-bit UUID representations for a
// 128-bit UUID, all of which are used in the protocol. 16-bit UUIDs values
// define only the "YYYY" portion in the following UUID pattern (with XXXX set
// as 0), while 32-bit UUID value define the "XXXXYYYY" portion. When using
// these short UUIDs, the remaining bits are set by the Bluetooth_Base_UUID as
// follows:
//   XXXXYYYY-0000-1000-8000-00805f9b34fb
//
// This class always stores UUID in their 128-bit representation in little
// endian format.
class Uuid {
 public:
  // Create a UUID from a span of 128-bit data. UUIDs are represented as
  // little endian bytes.
  constexpr Uuid(const span<const uint8_t, 16> uuid_span) : uuid_() {
    Result<uuid::Uuid> uuid = uuid::Uuid::FromSpan(uuid_span);
    PW_ASSERT(uuid.ok());
    uuid_ = uuid.value();
  }

  // Create a UUID from its string representation.
  constexpr Uuid(std::string_view str) : uuid_() {
    Result<uuid::Uuid> uuid = uuid::Uuid::FromString(str);
    PW_ASSERT(uuid.ok());
    uuid_ = uuid.value();
  }
  // The Bluetooth_Base_UUID defined by the specification. This is the base for
  // all 16-bit and 32-bit short UUIDs.
  static constexpr const Uuid& BluetoothBase();

  constexpr Uuid() : uuid_() {}

  // Create a UUID combining 96-bits from a base UUID with a 16-bit or 32-bit
  // value. 16-bit values will be extended to 32-bit ones, meaning the that the
  // 16 most significant bits will be set to 0 regardless of the value on the
  // base UUID.
  constexpr Uuid(uint32_t short_uuid, const Uuid& base_uuid) : uuid_() {
    std::array<uint8_t, uuid::Uuid::kSizeBytes> data{};
    for (size_t i = 0; i < kBaseOffset; i++) {
      data[i] = base_uuid.As128BitSpan()[i];
    }
    data[kBaseOffset] = static_cast<uint8_t>(short_uuid & 0xff);
    data[kBaseOffset + 1] =
        static_cast<uint8_t>((short_uuid >> CHAR_BIT) & 0xff);
    data[kBaseOffset + 2] =
        static_cast<uint8_t>((short_uuid >> CHAR_BIT * 2) & 0xff);
    data[kBaseOffset + 3] =
        static_cast<uint8_t>((short_uuid >> CHAR_BIT * 3) & 0xff);
    Result<uuid::Uuid> result = uuid::Uuid::FromSpan(data);
    PW_ASSERT(result.ok());
    uuid_ = result.value();
  }

  // Create a short UUID (32-bit or 16-bit) using the standard Bluetooth base
  // UUID.
  explicit constexpr Uuid(uint32_t short_uuid)
      : Uuid(short_uuid, BluetoothBase()) {}

  constexpr Uuid(const Uuid&) = default;
  constexpr Uuid& operator=(const Uuid&) = default;

  // Return a 2-byte span containing the 16-bit little endian representation of
  // the UUID. This is useful when Same112BitBase(BluetoothBase()) is true.
  constexpr span<const uint8_t, 2> As16BitSpan() const {
    return span<const uint8_t, 2>{As128BitSpan().data() + kBaseOffset, 2u};
  }

  // Return a 4-byte span containing the 32-bit little endian representation of
  // the UUID. This is useful when Same96BitBase(BluetoothBase()) is true.
  constexpr span<const uint8_t, 4> As32BitSpan() const {
    return span<const uint8_t, 4>{As128BitSpan().data() + kBaseOffset, 4u};
  }

  // Return the 128-bit (16-byte) little endian representation of the UUID.
  constexpr span<const uint8_t, 16> As128BitSpan() const {
    return uuid_.GetSpan();
  }

  // Return whether the UUID shares the same 112-bit base with another UUID.
  // Sharing the same 112-bit base with BluetoothBase() means that this UUID
  // can be resented as a 16-bit UUID.
  constexpr bool Same112BitBase(const Uuid& other) const {
    return Same96BitBase(other) &&
           (As128BitSpan().data()[14] == other.As128BitSpan().data()[14]) &&
           (As128BitSpan().data()[15] == other.As128BitSpan().data()[15]);
  }

  // Return whether the UUID shares the same 96-bit base with another UUID.
  // Sharing the same 96-bit base with BluetoothBase() means that this UUID
  // can be resented as a 32-bit UUID.
  constexpr bool Same96BitBase(const Uuid& other) const {
    for (size_t i = 0; i < kBaseOffset; i++) {
      if (As128BitSpan().data()[i] != other.As128BitSpan().data()[i])
        return false;
    }
    return true;
  }

  // Return whether the UUID is a 16-bit UUID represented as 128-bit using the
  // BluetoothBase() as the base.
  constexpr bool Is16BitUuid() const { return Same112BitBase(BluetoothBase()); }

  // Return whether the UUID is a 32-bit UUID represented as 128-bit using the
  // BluetoothBase() as the base.
  constexpr bool Is32BitUuid() const { return Same96BitBase(BluetoothBase()); }

  // Return an inline pw_string representation of the UUID in hexadecimal.
  constexpr InlineString<uuid::Uuid::kStringSize> ToString() const {
    return uuid_.ToString();
  }

 private:
  // Offset at which the short 16-bit and 32-bit UUID little-endian data starts
  // in the uuid_ array.
  static constexpr size_t kBaseOffset = 12;

  uuid::Uuid uuid_;
};

namespace internal {
// When BluetoothBase() is used in constexpr expressions it would normally be
// evaluated to a final different Uuid, such as when used in Uuid(uint32_t),
// however if a reference to the return value of BluetoothBase() is needed this
// variable would be the only global symbol that provides it even if it is used
// from multiple translation units.
inline constexpr Uuid kBluetoothBaseUuid{
    "00000000-0000-1000-8000-00805F9B34FB"};
}  // namespace internal

inline constexpr const Uuid& Uuid::BluetoothBase() {
  return internal::kBluetoothBaseUuid;
}

// Uuid comparators:
constexpr bool operator==(const Uuid& a, const Uuid& b) {
  const auto a_span = a.As128BitSpan();
  const auto b_span = b.As128BitSpan();
  for (size_t i = 0; i < a_span.size(); i++) {
    if (a_span[i] != b_span[i]) {
      return false;
    }
  }
  return true;
}

constexpr bool operator!=(const Uuid& a, const Uuid& b) { return !(a == b); }

}  // namespace pw::bluetooth
