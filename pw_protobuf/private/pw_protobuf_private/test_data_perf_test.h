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

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#if defined(_MSC_VER)
#include <intrin.h>
#endif  // defined(_MSC_VER)

#include "pw_bytes/array.h"
#include "pw_span/span.h"

namespace pw::protobuf::test {

// Prevents the compiler from optimizing away `value` or writes visible through
// `&value` (sink for benchmark outputs).
template <typename T>
inline void DoNotOptimize(const T& value) {
#if defined(__clang__) || defined(__GNUC__)
  asm volatile("" : : "r"(&value) : "memory");
#else
#if defined(_MSC_VER)
  _ReadWriteBarrier();
#endif  // defined(_MSC_VER)
  const volatile char* ptr = reinterpret_cast<const volatile char*>(&value);
  for (size_t i = 0; i < sizeof(value); ++i) {
    static_cast<void>(ptr[i]);
  }
#endif  // defined(__clang__) || defined(__GNUC__)
}

// Overload for `pw::span` that observes the span's size and underlying bytes
// rather than just the temporary span object header.
template <typename T, size_t kExtent>
inline void DoNotOptimize(span<T, kExtent> value) {
#if defined(__clang__) || defined(__GNUC__)
  asm volatile("" : : "r"(value.data()), "r"(value.size()) : "memory");
#else
#if defined(_MSC_VER)
  _ReadWriteBarrier();
#endif  // defined(_MSC_VER)
  const volatile char* ptr =
      reinterpret_cast<const volatile char*>(value.data());
  for (size_t i = 0; i < value.size_bytes(); ++i) {
    static_cast<void>(ptr[i]);
  }
#endif  // defined(__clang__) || defined(__GNUC__)
}

// In-place optimization barrier for non-scalar / struct benchmark inputs.
template <typename T>
inline void MakeOpaque(T& value) {
#if defined(__clang__) || defined(__GNUC__)
  asm volatile("" : "+m"(value) : : "memory");
#else
#if defined(_MSC_VER)
  _ReadWriteBarrier();
#endif  // defined(_MSC_VER)
  volatile char* ptr = reinterpret_cast<volatile char*>(&value);
  for (size_t i = 0; i < sizeof(value); ++i) {
    ptr[i] = ptr[i];
  }
#endif  // defined(__clang__) || defined(__GNUC__)
}

// Returns a runtime-opaque copy of `value` so inline encoders/decoders cannot
// constant-fold operations over `constexpr` test inputs under `-c opt` or LTO.
template <typename T>
inline T Opaque(T value) {
#if defined(__clang__) || defined(__GNUC__)
  if constexpr (std::is_scalar_v<T>) {
    asm volatile("" : "+r"(value));
  } else {
    asm volatile("" : "+m"(value) : : "memory");
  }
#else
  MakeOpaque(value);
#endif  // defined(__clang__) || defined(__GNUC__)
  return value;
}

// ============================================================================
// 0. Basic Integer Encoding Constants
// ============================================================================

inline constexpr uint32_t kIntegerFieldNumber = 1;
inline constexpr uint32_t kSmallInteger = 1;
inline constexpr uint32_t kLargerInteger = 4000000000U;

// ============================================================================
// 1. Simple Scalar/Enum Message: ItemInfo (3 fields, 11-byte wire payload)
// ============================================================================

inline constexpr uint64_t kItemOffset = 0x12345678ULL;
inline constexpr uint32_t kItemSize = 4096;

// Pre-encoded wire format for ItemInfo{offset=0x12345678, size=4096,
// access_level=READ_AND_WRITE(3)}:
//   field 1 (varint): 0x08, 0xf8, 0xac, 0xd1, 0x91, 0x01 (6 bytes)
//   field 2 (varint): 0x10, 0x80, 0x20 (3 bytes)
//   field 3 (varint): 0x18, 0x03 (2 bytes)
inline constexpr auto kEncodedItemInfo = bytes::
    Array<0x08, 0xf8, 0xac, 0xd1, 0x91, 0x01, 0x10, 0x80, 0x20, 0x18, 0x03>();

// ============================================================================
// 2. Realistic Composite Message: SensorReport (80-byte wire payload)
// ============================================================================

inline constexpr uint32_t kSensorDeviceId = 42;
inline constexpr bool kSensorActive = true;
inline constexpr int32_t kSensorTemperatureMc = 23500;
inline constexpr std::string_view kSensorLabel = "imu-primary-01";
inline constexpr auto kSensorPayload =
    bytes::Array<0xde, 0xad, 0xbe, 0xef, 0xca, 0xfe, 0xba, 0xbe>();
inline constexpr int32_t kSensorSamples[] = {100, -200, 300, -400, 500, 600};
inline constexpr int32_t kLocationX = -122084;
inline constexpr int32_t kLocationY = 37422;

inline constexpr auto kEncodedSensorReport = bytes::Array<
    // field 1: device_id = 42
    0x08,
    0x2a,
    // field 2: active = true
    0x10,
    0x01,
    // field 3: temperature_mc = 23500
    0x18,
    0xcc,
    0xb7,
    0x01,
    // field 4: label = "imu-primary-01" (14 bytes)
    0x22,
    0x0e,
    'i',
    'm',
    'u',
    '-',
    'p',
    'r',
    'i',
    'm',
    'a',
    'r',
    'y',
    '-',
    '0',
    '1',
    // field 5: payload (8 bytes)
    0x2a,
    0x08,
    0xde,
    0xad,
    0xbe,
    0xef,
    0xca,
    0xfe,
    0xba,
    0xbe,
    // field 6: samples = {100, -200, 300, -400, 500, 600} (packed, 27 bytes)
    0x32,
    0x1b,
    0x64,
    0xb8,
    0xfe,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0x01,
    0xac,
    0x02,
    0xf0,
    0xfc,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0x01,
    0xf4,
    0x03,
    0xd8,
    0x04,
    // field 7: location = {x = -122084, y = 37422} (15 bytes)
    0x3a,
    0x0f,
    0x08,
    0x9c,
    0xc6,
    0xf8,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0x01,
    0x10,
    0xae,
    0xa4,
    0x02>();

// ============================================================================
// 3. Oneof + Optional + Submessage: ResponseInfo (35-byte wire payload)
// ============================================================================

inline constexpr std::string_view kResponseKeyString = "alpha-key-01";
inline constexpr int64_t kResponseTimestamp = 1700000000LL;
inline constexpr bool kResponseHasValue = true;

inline constexpr auto kEncodedResponseInfo = bytes::Array<
    // field 1: key_string = "alpha-key-01" (12 bytes)
    0x0a,
    0x0c,
    'a',
    'l',
    'p',
    'h',
    'a',
    '-',
    'k',
    'e',
    'y',
    '-',
    '0',
    '1',
    // field 3: timestamp = 1700000000
    0x18,
    0x80,
    0xe2,
    0x9f,
    0xaa,
    0x06,
    // field 4: has_value = true
    0x20,
    0x01,
    // field 5: item_info = {offset=0x12345678, size=4096, access_level=READ(1)}
    0x2a,
    0x0b,
    0x08,
    0xf8,
    0xac,
    0xd1,
    0x91,
    0x01,
    0x10,
    0x80,
    0x20,
    0x18,
    0x01>();

// ============================================================================
// 4. Unbounded Callback Fields: SensorLog (47-byte wire payload)
// ============================================================================

inline constexpr std::string_view kSensorLogTag = "session-alpha-42";

inline constexpr auto kEncodedSensorLog = bytes::Array<
    // field 1: tag = "session-alpha-42" (16 bytes)
    0x0a,
    0x10,
    's',
    'e',
    's',
    's',
    'i',
    'o',
    'n',
    '-',
    'a',
    'l',
    'p',
    'h',
    'a',
    '-',
    '4',
    '2',
    // field 2: readings = {100, -200, 300, -400, 500, 600} (packed, 27 bytes)
    0x12,
    0x1b,
    0x64,
    0xb8,
    0xfe,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0x01,
    0xac,
    0x02,
    0xf0,
    0xfc,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0x01,
    0xf4,
    0x03,
    0xd8,
    0x04>();

// ============================================================================
// 5. 3-Level Nested Message: NestedRoot (Tiny: 4B leaf, Large: 1000B leaf)
// ============================================================================

inline constexpr uint64_t kNestedTimestamp = 1000;
inline constexpr uint32_t kNestedKind = 7;
inline constexpr uint32_t kNestedFlags = 0x55;
inline constexpr uint32_t kNestedStatus = 1;
inline constexpr uint64_t kNestedLeafId = 999;
inline constexpr uint32_t kNestedLeafCount = 3;

// Matches NestedLeaf.data max_size in perf_test.options.
inline constexpr size_t kNestedLeafDataMaxSize = 1024;

inline constexpr auto kNestedTinyData = bytes::Array<1, 2, 3, 4>();

// Pre-encoded wire format for NestedRoot with a 4-byte leaf (24 bytes).
inline constexpr auto kEncodedNestedTiny =
    bytes::Array<0x08,
                 0xe8,
                 0x07,  // root.timestamp = 1000
                 0x12,
                 0x11,  // root.mid (17 bytes)
                 0x08,
                 0x07,  // mid.kind = 7
                 0x12,
                 0x0b,  // mid.leaf (11 bytes)
                 0x08,
                 0xe7,
                 0x07,  // leaf.id = 999
                 0x10,
                 0x03,  // leaf.count = 3
                 0x1a,
                 0x04,
                 0x01,
                 0x02,
                 0x03,
                 0x04,  // leaf.data (4 bytes)
                 0x18,
                 0x55,  // mid.flags = 0x55
                 0x18,
                 0x01  // root.status = 1
                 >();

inline constexpr size_t kNestedLargeDataSize = 1000;
inline constexpr std::byte kNestedLargeDataFillByte = std::byte{0x42};

inline constexpr auto kNestedLargeData = [] {
  std::array<std::byte, kNestedLargeDataSize> data{};
  for (auto& b : data) {
    b = kNestedLargeDataFillByte;
  }
  return data;
}();

inline constexpr auto kEncodedNestedLarge = [] {
  std::array<std::byte, 1023> buf{};
  constexpr uint8_t kHeader[] = {
      0x08, 0xe8, 0x07,  // root.timestamp = 1000
      0x12, 0xf7, 0x07,  // root.mid (1015 bytes)
      0x08, 0x07,        // mid.kind = 7
      0x12, 0xf0, 0x07,  // mid.leaf (1008 bytes)
      0x08, 0xe7, 0x07,  // leaf.id = 999
      0x10, 0x03,        // leaf.count = 3
      0x1a, 0xe8, 0x07,  // leaf.data (1000 bytes)
  };
  size_t idx = 0;
  for (uint8_t b : kHeader) {
    buf[idx++] = static_cast<std::byte>(b);
  }
  for (size_t i = 0; i < kNestedLargeDataSize; ++i) {
    buf[idx++] = kNestedLargeDataFillByte;
  }
  constexpr uint8_t kTrailer[] = {
      0x18,
      0x55,  // mid.flags = 0x55
      0x18,
      0x01,  // root.status = 1
  };
  for (uint8_t b : kTrailer) {
    buf[idx++] = static_cast<std::byte>(b);
  }
  return buf;
}();

}  // namespace pw::protobuf::test
