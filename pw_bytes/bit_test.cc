
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

#include "pw_bytes/bit.h"

#include <cstdint>
#include <cstring>

#include "pw_unit_test/framework.h"

namespace pw::bytes {
namespace {

// SignExtend unsigned integer values into signed integers.
constexpr uint32_t kNegative24Bits = 0x00FACADE;
constexpr int32_t kExtendedNegative24Bits =
    static_cast<int32_t>(SignExtend<24>(kNegative24Bits));
static_assert(kExtendedNegative24Bits == static_cast<int32_t>(0xFFFACADE));

constexpr uint32_t kPositive20Bits = 0x00000ACE;
constexpr int32_t kExtendedPositive20Bits =
    static_cast<int32_t>(SignExtend<20>(kPositive20Bits));
static_assert(kExtendedPositive20Bits == static_cast<int32_t>(0x00000ACE));

constexpr uint32_t kNegative12Bits = 0x00000ACE;
constexpr int32_t kExtendedNegative12Bits =
    static_cast<int32_t>(SignExtend<12>(kNegative12Bits));
static_assert(kExtendedNegative12Bits == static_cast<int32_t>(0xFFFFFACE));

constexpr uint32_t k32Bits = 0b10100000101000001010000010100000;
// ExtractBits: extract single bit.
constexpr uint8_t kZeroBit = ExtractBits<uint8_t, 4, 4>(k32Bits);
static_assert(kZeroBit == 0);
constexpr uint8_t kOneBit = ExtractBits<uint8_t, 5, 5>(k32Bits);
static_assert(kOneBit == 1);
// ExtractBits: extract 16 bits from  uint32_t to uint16_t.
constexpr uint16_t kExtracted16Bits = 0b1010000010100000;
constexpr uint16_t k15To0Bits = ExtractBits<uint32_t, 15, 0>(k32Bits);
static_assert(k15To0Bits == kExtracted16Bits);
constexpr uint16_t k23To8Bits = ExtractBits<uint16_t, 23, 8>(k32Bits);
static_assert(k23To8Bits == kExtracted16Bits);
constexpr uint16_t k31To16Bits = ExtractBits<uint16_t, 31, 16>(k32Bits);
static_assert(k31To16Bits == kExtracted16Bits);
// ExtractBits: extract 31 bits.
constexpr uint32_t kExtracted31Bits = 0b1010000010100000101000001010000;
constexpr uint32_t k31To1Bits = ExtractBits<uint32_t, 31, 1>(k32Bits);
static_assert(k31To1Bits == kExtracted31Bits);
// ExtractBits: extract all bits.
constexpr uint32_t k31To0Bits = ExtractBits<uint32_t, 31, 0>(k32Bits);
static_assert(k31To0Bits == k32Bits);

TEST(Endian, NativeIsBigOrLittle) {
  EXPECT_TRUE(endian::native == endian::little ||
              endian::native == endian::big);
}

TEST(Endian, NativeIsCorrect) {
  constexpr uint32_t kInteger = 0x11223344u;
  int8_t bytes[sizeof(kInteger)] = {};
  std::memcpy(bytes, &kInteger, sizeof(kInteger));

  if constexpr (endian::native == endian::little) {
    EXPECT_EQ(bytes[0], 0x44);
    EXPECT_EQ(bytes[1], 0x33);
    EXPECT_EQ(bytes[2], 0x22);
    EXPECT_EQ(bytes[3], 0x11);
  } else {
    EXPECT_EQ(bytes[0], 0x11);
    EXPECT_EQ(bytes[1], 0x22);
    EXPECT_EQ(bytes[2], 0x33);
    EXPECT_EQ(bytes[3], 0x44);
  }
}

}  // namespace
}  // namespace pw::bytes
