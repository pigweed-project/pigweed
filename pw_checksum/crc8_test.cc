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

#include "pw_checksum/crc8.h"

#include <array>

#include "pw_unit_test/framework.h"

namespace pw::checksum {
namespace {

// Test data: "123456789"
constexpr std::array<std::byte, 9> kTestData = {std::byte('1'),
                                                std::byte('2'),
                                                std::byte('3'),
                                                std::byte('4'),
                                                std::byte('5'),
                                                std::byte('6'),
                                                std::byte('7'),
                                                std::byte('8'),
                                                std::byte('9')};

TEST(Crc8Test, StandardCrc8) {
  EXPECT_EQ(0xF4, Crc8::kCrc8.Calculate(kTestData));
}

TEST(Crc8Test, Wcdma) {
  // CRC-8/WCDMA: poly=0x9B, init=0x00, refin=true, refout=true, xorout=0x00
  // "123456789" -> 0x25
  EXPECT_EQ(0x25, Crc8::kWcdma.Calculate(kTestData));
}

TEST(Crc8Test, Maxim) {
  // "123456789" -> 0xA1
  EXPECT_EQ(0xA1, Crc8::kMaxim.Calculate(kTestData));
}

TEST(Crc8Test, Itu) {
  // "123456789" -> 0xA1
  EXPECT_EQ(0xA1, Crc8::kItu.Calculate(kTestData));
}

TEST(Crc8Test, Rohc) {
  // "123456789" -> 0xD0
  EXPECT_EQ(0xD0, Crc8::kRohc.Calculate(kTestData));
}

TEST(Crc8Test, NoData) { EXPECT_EQ(0x00, Crc8::kCrc8.Calculate({})); }

TEST(Crc8Test, SingleByte) {
  // CRC-8: poly=0x07, init=0x00, refin=false, refout=false, xorout=0x00
  // "a" -> 0x20
  const std::byte data[] = {std::byte('a')};
  EXPECT_EQ(0x20, Crc8::kCrc8.Calculate(data));
}

TEST(Crc8Test, Incremental) {
  constexpr std::array<uint8_t, 9> kExpectedCrcs = {
      0x97, 0x72, 0xC0, 0xC2, 0xCB, 0xFD, 0x78, 0xC7, 0xF4};

  for (size_t i = 0; i < kTestData.size(); ++i) {
    EXPECT_EQ(kExpectedCrcs[i],
              Crc8::kCrc8.Calculate(span(kTestData).first(i + 1)));
  }
}

}  // namespace
}  // namespace pw::checksum
