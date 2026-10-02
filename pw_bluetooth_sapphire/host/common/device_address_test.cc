// Copyright 2023 The Pigweed Authors
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

#include "pw_bluetooth_sapphire/internal/host/common/device_address.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <unordered_map>

#include "pw_unit_test/framework.h"

namespace bt {
namespace {

// Initialize from bytes.
const DeviceAddress kClassic(DeviceAddress::Type::kBREDR,
                             {0x55, 0x44, 0x33, 0x22, 0x11, 0x41});
const DeviceAddress kPublic(DeviceAddress::Type::kLEPublic,
                            {0x42, 0x11, 0x22, 0x33, 0x44, 0x55});
const DeviceAddress kNonResolvable(DeviceAddress::Type::kLERandom,
                                   {0x55, 0x44, 0x33, 0x22, 0x11, 0x00});
const DeviceAddress kResolvable(DeviceAddress::Type::kLERandom,
                                {0x55, 0x44, 0x33, 0x22, 0x11, 0x43});
const DeviceAddress kStatic(DeviceAddress::Type::kLERandom,
                            {0x55, 0x44, 0x33, 0x22, 0x11, 0xC3});

struct TestPayload {
  uint8_t arg0;
  DeviceAddressBytes bdaddr;
} __attribute__((packed));

TEST(DeviceAddressBytesTest, ToString) {
  DeviceAddressBytes bdaddr({1, 15, 2, 255, 127, 3});
  EXPECT_EQ("03:7F:FF:02:0F:01", bdaddr.ToString());

  bdaddr = DeviceAddressBytes();
  EXPECT_EQ("00:00:00:00:00:00", bdaddr.ToString());
}

TEST(DeviceAddressBytesTest, CastFromBytes) {
  std::array<uint8_t, 7> bytes{{10, 1, 15, 2, 255, 127, 3}};
  EXPECT_EQ(bytes.size(), sizeof(TestPayload));

  auto* bdaddr = reinterpret_cast<DeviceAddressBytes*>(bytes.data());
  EXPECT_EQ("7F:FF:02:0F:01:0A", bdaddr->ToString());

  auto* test_payload = reinterpret_cast<TestPayload*>(bytes.data());
  EXPECT_EQ(10, test_payload->arg0);
  EXPECT_EQ("03:7F:FF:02:0F:01", test_payload->bdaddr.ToString());
}

TEST(DeviceAddressBytesTest, FromView) {
  std::array<uint8_t, 6> buffer = {0xfe, 0xff, 0xff, 0xff, 0xff, 0xAA};
  auto bdaddr_view = pw::bluetooth::emboss::MakeBdAddrView(&buffer);
  DeviceAddressBytes addr(bdaddr_view);
  EXPECT_EQ("AA:FF:FF:FF:FF:FE", addr.ToString());
}

TEST(DeviceAddressBytesTest, ToView) {
  DeviceAddressBytes addr = DeviceAddressBytes({0, 0, 0, 0, 0, 0});
  EXPECT_EQ(addr.view().bd_addr().Read(), 0u);

  addr = DeviceAddressBytes({0xfe, 0xff, 0xff, 0xff, 0xff, 0xff});
  EXPECT_EQ(addr.view().bd_addr().Read(), 0xfffffffffffelu);
}

TEST(DeviceAddressBytesTest, Comparison) {
  DeviceAddressBytes bdaddr0, bdaddr1;
  EXPECT_EQ(bdaddr0, bdaddr1);

  bdaddr0 = DeviceAddressBytes({1, 2, 3, 4, 5, 6});
  EXPECT_NE(bdaddr0, bdaddr1);

  bdaddr1 = bdaddr0;
  EXPECT_EQ(bdaddr0, bdaddr1);
}

TEST(DeviceAddressTest, Comparison) {
  DeviceAddress addr0, addr1;
  EXPECT_EQ(addr0, addr1);

  addr0 = DeviceAddress(DeviceAddress::Type::kBREDR, {1, 2, 3, 4, 5, 6});
  EXPECT_NE(addr0, addr1);

  addr1 = DeviceAddress(DeviceAddress::Type::kLEPublic, addr0.value());
  EXPECT_EQ(addr0, addr1);

  addr0 = DeviceAddress(DeviceAddress::Type::kLERandom, addr0.value());
  EXPECT_NE(addr0, addr1);
}

TEST(DeviceAddressTest, Map) {
  std::map<DeviceAddress, int> map;

  DeviceAddress address1;
  DeviceAddress address2(address1);
  DeviceAddress address3(DeviceAddress::Type::kLEPublic, address1.value());
  DeviceAddress address4(DeviceAddress::Type::kLEPublic, {1});

  map[address1] = 1;

  auto iter = map.find(address1);
  EXPECT_NE(map.end(), iter);
  EXPECT_EQ(1, iter->second);

  iter = map.find(address2);
  EXPECT_NE(map.end(), iter);
  EXPECT_EQ(1, iter->second);

  iter = map.find(address3);
  EXPECT_NE(map.end(), iter);
  EXPECT_EQ(1, iter->second);

  iter = map.find(address4);
  EXPECT_EQ(map.end(), iter);

  map[address3] = 2;
  map[address4] = 3;

  EXPECT_EQ(2u, map.size());
  EXPECT_EQ(2, map[address1]);
  EXPECT_EQ(2, map[address2]);
  EXPECT_EQ(2, map[address3]);
  EXPECT_EQ(3, map[address4]);
}

TEST(DeviceAddressTest, UnorderedMap) {
  std::unordered_map<DeviceAddress, int> map;

  DeviceAddress address1;
  DeviceAddress address2(address1);
  DeviceAddress address3(DeviceAddress::Type::kLEPublic, address1.value());
  DeviceAddress address4(DeviceAddress::Type::kLEPublic, {1});

  map[address1] = 1;

  auto iter = map.find(address1);
  EXPECT_NE(map.end(), iter);
  EXPECT_EQ(1, iter->second);

  iter = map.find(address2);
  EXPECT_NE(map.end(), iter);
  EXPECT_EQ(1, iter->second);

  iter = map.find(address3);
  EXPECT_NE(map.end(), iter);
  EXPECT_EQ(1, iter->second);

  iter = map.find(address4);
  EXPECT_EQ(map.end(), iter);

  map[address3] = 2;
  map[address4] = 3;

  EXPECT_EQ(2u, map.size());
  EXPECT_EQ(2, map[address1]);
  EXPECT_EQ(2, map[address2]);
  EXPECT_EQ(2, map[address3]);
  EXPECT_EQ(3, map[address4]);
}

TEST(DeviceAddressTest, IsResolvablePrivate) {
  EXPECT_FALSE(kClassic.IsResolvablePrivate());
  EXPECT_FALSE(kPublic.IsResolvablePrivate());
  EXPECT_FALSE(kNonResolvable.IsResolvablePrivate());
  EXPECT_TRUE(kResolvable.IsResolvablePrivate());
  EXPECT_FALSE(kStatic.IsResolvablePrivate());
}

TEST(DeviceAddressTest, IsNonResolvablePrivate) {
  EXPECT_FALSE(kClassic.IsNonResolvablePrivate());
  EXPECT_FALSE(kPublic.IsNonResolvablePrivate());
  EXPECT_TRUE(kNonResolvable.IsNonResolvablePrivate());
  EXPECT_FALSE(kResolvable.IsNonResolvablePrivate());
  EXPECT_FALSE(kStatic.IsNonResolvablePrivate());
}

TEST(DeviceAddressTest, IsStatic) {
  EXPECT_FALSE(kClassic.IsStaticRandom());
  EXPECT_FALSE(kPublic.IsStaticRandom());
  EXPECT_FALSE(kNonResolvable.IsStaticRandom());
  EXPECT_FALSE(kResolvable.IsStaticRandom());
  EXPECT_TRUE(kStatic.IsStaticRandom());
}

TEST(DeviceAddressTest, IsPublic) {
  EXPECT_TRUE(kClassic.IsPublic());
  EXPECT_TRUE(kPublic.IsPublic());
  EXPECT_FALSE(kNonResolvable.IsPublic());
  EXPECT_FALSE(kResolvable.IsPublic());
  EXPECT_FALSE(kStatic.IsPublic());
}

TEST(DeviceAddressTest, HashEqualityAndBitSensitivity) {
  constexpr std::array<std::array<uint8_t, kDeviceAddressSize>, 3>
      kBaseBytePatterns = {{
          {0x79, 0x56, 0x34, 0x12, 0xAA, 0xC0},
          {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
          {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
      }};

  // Default-constructed (all-zero) addresses must not hash to zero.
  EXPECT_NE(static_cast<uint32_t>(DeviceAddress().Hash()), 0u);
  EXPECT_NE(static_cast<uint32_t>(DeviceAddressBytes().Hash()), 0u);

  for (const auto& base_bytes : kBaseBytePatterns) {
    const DeviceAddress bredr(DeviceAddress::Type::kBREDR, base_bytes);
    const DeviceAddress le_public(DeviceAddress::Type::kLEPublic, base_bytes);
    const DeviceAddress le_random(DeviceAddress::Type::kLERandom, base_bytes);
    const DeviceAddress le_anon(DeviceAddress::Type::kLEAnonymous, base_bytes);

    // Public addresses (kBREDR and kLEPublic) compare equal and must hash
    // equal.
    EXPECT_EQ(bredr, le_public);
    EXPECT_EQ(bredr.Hash(), le_public.Hash());

    // Incompatible types with identical bytes must not share a 32-bit or full
    // hash.
    EXPECT_NE(static_cast<uint32_t>(bredr.Hash()),
              static_cast<uint32_t>(le_random.Hash()));
    EXPECT_NE(static_cast<uint32_t>(le_random.Hash()),
              static_cast<uint32_t>(le_anon.Hash()));
    EXPECT_NE(static_cast<uint32_t>(bredr.Hash()),
              static_cast<uint32_t>(le_anon.Hash()));

    // Flipping any single bit across all 48 bits must change both the lower 32
    // bits (modeling 32-bit size_t targets) and upper 32 bits (on 64-bit
    // targets).
    const std::size_t base_bredr_hash = bredr.Hash();
    const std::size_t base_random_hash = le_random.Hash();
    const std::size_t base_bytes_hash = le_random.value().Hash();
    for (size_t byte_idx = 0; byte_idx < kDeviceAddressSize; ++byte_idx) {
      for (uint8_t bit = 0; bit < 8; ++bit) {
        auto mutated_bytes = base_bytes;
        mutated_bytes[byte_idx] ^= static_cast<uint8_t>(1u << bit);
        DeviceAddress mutated_bredr(DeviceAddress::Type::kBREDR, mutated_bytes);
        DeviceAddress mutated_random(DeviceAddress::Type::kLERandom,
                                     mutated_bytes);
        EXPECT_NE(static_cast<uint32_t>(base_bredr_hash),
                  static_cast<uint32_t>(mutated_bredr.Hash()))
            << "byte=" << byte_idx << " bit=" << static_cast<int>(bit);
        EXPECT_NE(static_cast<uint32_t>(base_random_hash),
                  static_cast<uint32_t>(mutated_random.Hash()))
            << "byte=" << byte_idx << " bit=" << static_cast<int>(bit);
        EXPECT_NE(static_cast<uint32_t>(base_bytes_hash),
                  static_cast<uint32_t>(mutated_random.value().Hash()))
            << "byte=" << byte_idx << " bit=" << static_cast<int>(bit);
        if constexpr (sizeof(std::size_t) > sizeof(uint32_t)) {
          EXPECT_NE(static_cast<uint64_t>(base_bredr_hash) >> 32,
                    static_cast<uint64_t>(mutated_bredr.Hash()) >> 32)
              << "byte=" << byte_idx << " bit=" << static_cast<int>(bit);
          EXPECT_NE(static_cast<uint64_t>(base_random_hash) >> 32,
                    static_cast<uint64_t>(mutated_random.Hash()) >> 32)
              << "byte=" << byte_idx << " bit=" << static_cast<int>(bit);
          EXPECT_NE(static_cast<uint64_t>(base_bytes_hash) >> 32,
                    static_cast<uint64_t>(mutated_random.value().Hash()) >> 32)
              << "byte=" << byte_idx << " bit=" << static_cast<int>(bit);
        }
      }
    }
  }
}

TEST(DeviceAddressTest,
     ThirtyTwoBitTruncatedHashResistsStaticRandomCollisions) {
  // Generate all 256 * 64 = 16,384 valid LE static-random addresses that share
  // the same lower 4 bytes (bytes[0..3]) and differ only in the upper 2 bytes
  // (bytes[4..5]). Static-random addresses must have the two most significant
  // bits of bytes[5] set to 1 (0xC0..0xFF).
  constexpr uint8_t kStaticRandomMinByte5 = 0b1100'0000;
  constexpr size_t kPoolSize = 256 * (0xFF - kStaticRandomMinByte5 + 1);
  std::unordered_map<uint32_t, size_t> addr_hash32_counts;
  std::unordered_map<uint32_t, size_t> bytes_hash32_counts;
  addr_hash32_counts.reserve(kPoolSize);
  bytes_hash32_counts.reserve(kPoolSize);

  for (uint16_t b4 = 0x00; b4 <= 0xFF; ++b4) {
    for (uint16_t b5 = kStaticRandomMinByte5; b5 <= 0xFF; ++b5) {
      const DeviceAddress addr(DeviceAddress::Type::kLERandom,
                               {0x79,
                                0x56,
                                0x34,
                                0x12,
                                static_cast<uint8_t>(b4),
                                static_cast<uint8_t>(b5)});
      ASSERT_TRUE(addr.IsStaticRandom());

      const uint32_t addr_hash32 =
          static_cast<uint32_t>(std::hash<DeviceAddress>{}(addr));
      const uint32_t bytes_hash32 = static_cast<uint32_t>(addr.value().Hash());
      addr_hash32_counts[addr_hash32]++;
      bytes_hash32_counts[bytes_hash32]++;
    }
  }

  size_t max_addr_collisions = 0;
  for (const auto& [_, count] : addr_hash32_counts) {
    max_addr_collisions = std::max(max_addr_collisions, count);
  }

  size_t max_bytes_collisions = 0;
  for (const auto& [_, count] : bytes_hash32_counts) {
    max_bytes_collisions = std::max(max_bytes_collisions, count);
  }

  EXPECT_GT(addr_hash32_counts.size(), 16350u);
  EXPECT_LE(max_addr_collisions, 4u);
  EXPECT_GT(bytes_hash32_counts.size(), 16350u);
  EXPECT_LE(max_bytes_collisions, 4u);
}

TEST(DeviceAddressTest,
     UnorderedMapWith32BitTruncatedHashDistributesBucketsUniformly) {
  struct TruncatedHasher32 {
    std::size_t operator()(const DeviceAddress& addr) const {
      return static_cast<uint32_t>(std::hash<DeviceAddress>{}(addr));
    }
  };

  // Insert valid LE static-random addresses that share the same lower 4 bytes
  // (bytes[0..3]) and differ only in the upper 2 bytes (bytes[4..5]).
  constexpr uint8_t kStaticRandomMinByte5 = 0b1100'0000;
  constexpr size_t kNumAddresses = 256 * (0xFF - kStaticRandomMinByte5 + 1);
  std::unordered_map<DeviceAddress, size_t, TruncatedHasher32> map;
  map.reserve(kNumAddresses);

  for (uint16_t b4 = 0x00; b4 <= 0xFF; ++b4) {
    for (uint16_t b5 = kStaticRandomMinByte5; b5 <= 0xFF; ++b5) {
      const DeviceAddress addr(DeviceAddress::Type::kLERandom,
                               {0x79,
                                0x56,
                                0x34,
                                0x12,
                                static_cast<uint8_t>(b4),
                                static_cast<uint8_t>(b5)});
      map.emplace(addr, map.size());
    }
  }

  ASSERT_EQ(kNumAddresses, map.size());

  size_t max_bucket_size = 0;
  for (size_t b = 0; b < map.bucket_count(); ++b) {
    max_bucket_size = std::max(max_bucket_size, map.bucket_size(b));
  }

  // With a 32-bit uniform hash over 16,384 keys, the maximum bucket size is
  // bounded by a small constant instead of degenerating to kNumAddresses.
  EXPECT_LE(max_bucket_size, 16u);
}

}  // namespace
}  // namespace bt
