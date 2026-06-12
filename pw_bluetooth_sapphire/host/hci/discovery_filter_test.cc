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

#include "pw_bluetooth_sapphire/internal/host/hci/discovery_filter.h"

#include "pw_bluetooth_sapphire/internal/host/common/advertising_data.h"
#include "pw_bluetooth_sapphire/internal/host/common/supplement_data.h"
#include "pw_bluetooth_sapphire/internal/host/common/uint128.h"
#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_scanner.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_unit_test/framework.h"

namespace bt::hci {
namespace {

constexpr uint16_t kUuid0 = 0x180d;

TEST(DiscoveryFilterTest, Flags) {
  const StaticByteBuffer kNoFlagsData(0x02, 0x09, 'a');
  const StaticByteBuffer kValidFlagsData(0x02, 0x01, 0b101);

  auto no_flags_data = AdvertisingData::FromBytes(kNoFlagsData).value();
  auto valid_flags_data = AdvertisingData::FromBytes(kValidFlagsData).value();

  DiscoveryFilter filter;

  // Empty filter should match everything.
  EXPECT_TRUE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(no_flags_data, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(valid_flags_data, false, hci_spec::kRSSIInvalid));

  filter.set_flags(0b100);
  EXPECT_FALSE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(no_flags_data, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(valid_flags_data, false, hci_spec::kRSSIInvalid));

  filter.set_flags(0b001);
  EXPECT_FALSE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(no_flags_data, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(valid_flags_data, false, hci_spec::kRSSIInvalid));

  // The following filters set multiple bits. As long as one of them is set, the
  // filter should match.
  filter.set_flags(0b101);
  EXPECT_FALSE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(no_flags_data, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(valid_flags_data, false, hci_spec::kRSSIInvalid));

  filter.set_flags(0b111);
  EXPECT_FALSE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(no_flags_data, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(valid_flags_data, false, hci_spec::kRSSIInvalid));

  filter.set_flags(0b011);
  EXPECT_FALSE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(no_flags_data, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(valid_flags_data, false, hci_spec::kRSSIInvalid));

  filter.set_flags(0b010);
  EXPECT_FALSE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(no_flags_data, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(valid_flags_data, false, hci_spec::kRSSIInvalid));

  // The following filters requre that *all* bits be present in the advertising
  // data.
  filter.set_flags(0b101, /*require_all=*/true);
  EXPECT_FALSE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(no_flags_data, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(valid_flags_data, false, hci_spec::kRSSIInvalid));

  filter.set_flags(0b111, /*require_all=*/true);
  EXPECT_FALSE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(no_flags_data, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(valid_flags_data, false, hci_spec::kRSSIInvalid));

  filter.set_flags(0b011, /*require_all=*/true);
  EXPECT_FALSE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(no_flags_data, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(valid_flags_data, false, hci_spec::kRSSIInvalid));

  filter.set_flags(0b010, /*require_all=*/true);
  EXPECT_FALSE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(no_flags_data, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(valid_flags_data, false, hci_spec::kRSSIInvalid));
}

TEST(DiscoveryFilterTest, Connectable) {
  DiscoveryFilter filter;

  // Empty filter should match both.
  EXPECT_TRUE(filter.Matches(std::nullopt, true, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));

  // Filter connectable.
  filter.set_connectable(true);
  EXPECT_TRUE(filter.connectable());
  EXPECT_TRUE(filter.Matches(std::nullopt, true, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));

  // Filter not connectable.
  filter.set_connectable(false);
  EXPECT_TRUE(filter.connectable());
  EXPECT_FALSE(filter.Matches(std::nullopt, true, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));

  filter.Reset();
  EXPECT_FALSE(filter.connectable());
}

TEST(DiscoveryFilterTest, 16BitServiceUuids) {
  constexpr uint16_t kUuid1 = 0x1800;

  // Below, "Incomplete" refers to the "Incomplete Service UUIDs" field while
  // "Complete" refers to "Complete Service UUIDs".

  const auto kIncompleteEmpty(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x01, DataType::kIncomplete16BitServiceUuids))
          .value());
  const auto kIncompleteNoMatch(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x05,
                           DataType::kIncomplete16BitServiceUuids,
                           0x01,
                           0x02,
                           0x03,
                           0x04))
          .value());
  const auto kIncompleteMatch0(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x05,
                           DataType::kIncomplete16BitServiceUuids,
                           0x01,
                           0x02,
                           LowerBits(kUuid0),
                           UpperBits(kUuid0)))
          .value());
  const auto kIncompleteMatch1(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x05,
                           DataType::kIncomplete16BitServiceUuids,
                           LowerBits(kUuid1),
                           UpperBits(kUuid1),
                           0x03,
                           0x04))
          .value());
  const auto kCompleteEmpty(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x01, DataType::kComplete16BitServiceUuids))
          .value());
  const auto kCompleteNoMatch(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x05,
                           DataType::kComplete16BitServiceUuids,
                           0x01,
                           0x02,
                           0x03,
                           0x04))
          .value());
  const auto kCompleteMatch0(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x05,
                           DataType::kComplete16BitServiceUuids,
                           0x01,
                           0x02,
                           LowerBits(kUuid0),
                           UpperBits(kUuid0)))
          .value());
  const auto kCompleteMatch1(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x05,
                           DataType::kComplete16BitServiceUuids,
                           LowerBits(kUuid1),
                           UpperBits(kUuid1),
                           0x03,
                           0x04))
          .value());

  DiscoveryFilter filter;

  // An empty filter should match all payloads.
  EXPECT_TRUE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kIncompleteEmpty, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(
      filter.Matches(kIncompleteNoMatch, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kIncompleteMatch0, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kIncompleteMatch1, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kCompleteEmpty, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kCompleteNoMatch, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kCompleteMatch0, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kCompleteMatch1, false, hci_spec::kRSSIInvalid));

  // Filter for kUuid0 and kUuid1.
  filter.set_service_uuids(std::vector<UUID>{UUID(kUuid0), UUID(kUuid1)});
  EXPECT_FALSE(filter.service_uuids().empty());
  EXPECT_FALSE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(kIncompleteEmpty, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(
      filter.Matches(kIncompleteNoMatch, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kIncompleteMatch0, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kIncompleteMatch1, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(kCompleteEmpty, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(kCompleteNoMatch, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kCompleteMatch0, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kCompleteMatch1, false, hci_spec::kRSSIInvalid));

  filter.Reset();
  EXPECT_TRUE(filter.service_uuids().empty());
}

TEST(DiscoveryFilterTest, 32BitServiceUuids) {
  constexpr uint32_t kUuid1 = 0xabcd1800;

  // Below, "Incomplete" refers to the "Incomplete Service UUIDs" field while
  // "Complete" refers to "Complete Service UUIDs".

  const auto kIncompleteEmpty(
      AdvertisingData::FromBytes(StaticByteBuffer(0x01, 0x04)).value());
  const auto kIncompleteNoMatch(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x09,
                           DataType::kIncomplete32BitServiceUuids,
                           // First UUID
                           0x01,
                           0x02,
                           0x03,
                           0x04,
                           // Second UUID
                           0x05,
                           0x06,
                           0x07,
                           0x08))
          .value());
  const auto kIncompleteMatch0(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x09,
                           DataType::kIncomplete32BitServiceUuids,
                           // First UUID
                           0x01,
                           0x02,
                           0x03,
                           0x04,
                           // kUuid0
                           LowerBits(kUuid0),
                           UpperBits(kUuid0),
                           0x00,
                           0x00))
          .value());
  const auto kIncompleteMatch1(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x09,
                           DataType::kIncomplete32BitServiceUuids,
                           // kUuid1
                           0x00,
                           0x18,
                           0xcd,
                           0xab,
                           // Second UUID
                           0x01,
                           0x02,
                           0x03,
                           0x04))
          .value());
  const auto kCompleteEmpty(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x01, DataType::kComplete32BitServiceUuids))
          .value());
  const auto kCompleteNoMatch(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x09,
                           DataType::kComplete32BitServiceUuids,
                           // First UUID
                           0x01,
                           0x02,
                           0x03,
                           0x04,
                           // Second UUID
                           0x05,
                           0x06,
                           0x07,
                           0x08))
          .value());
  const auto kCompleteMatch0(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x09,
                           DataType::kComplete32BitServiceUuids,
                           // First UUID
                           0x01,
                           0x02,
                           0x03,
                           0x04,
                           // kUuid0
                           LowerBits(kUuid0),
                           UpperBits(kUuid0),
                           0x00,
                           0x00))
          .value());
  const auto kCompleteMatch1(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x09,
                           DataType::kComplete32BitServiceUuids,
                           // kUuid1
                           0x00,
                           0x18,
                           0xcd,
                           0xab,
                           // Second UUID
                           0x01,
                           0x02,
                           0x03,
                           0x04))
          .value());

  DiscoveryFilter filter;

  // An empty filter should match all payloads.
  EXPECT_TRUE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kIncompleteEmpty, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(
      filter.Matches(kIncompleteNoMatch, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kIncompleteMatch0, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kIncompleteMatch1, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kCompleteEmpty, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kCompleteNoMatch, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kCompleteMatch0, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kCompleteMatch1, false, hci_spec::kRSSIInvalid));

  // Filter for kUuid0 and kUuid1.
  filter.set_service_uuids(std::vector<UUID>{UUID(kUuid0), UUID(kUuid1)});
  EXPECT_FALSE(filter.service_uuids().empty());
  EXPECT_FALSE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(kIncompleteEmpty, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(
      filter.Matches(kIncompleteNoMatch, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kIncompleteMatch0, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kIncompleteMatch1, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(kCompleteEmpty, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(kCompleteNoMatch, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kCompleteMatch0, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kCompleteMatch1, false, hci_spec::kRSSIInvalid));

  filter.Reset();
  EXPECT_TRUE(filter.service_uuids().empty());
}

TEST(DiscoveryFilterTest, 128BitServiceUuids) {
  constexpr UInt128 kUuid1 = {0xDE,
                              0xAD,
                              0xBE,
                              0xEF,
                              0xDE,
                              0xAD,
                              0xBE,
                              0xEF,
                              0xDE,
                              0xAD,
                              0xBE,
                              0xEF,
                              0x00,
                              0x18,
                              0xcd,
                              0xab};

  // Below, "Incomplete" refers to the "Incomplete Service UUIDs" field while
  // "Complete" refers to "Complete Service UUIDs".

  const auto kIncompleteEmpty(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x01, DataType::kIncomplete128BitServiceUuids))
          .value());
  const auto kIncompleteNoMatch(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x11,
                           DataType::kIncomplete128BitServiceUuids,

                           // UUID
                           0x00,
                           0x01,
                           0x02,
                           0x03,
                           0x04,
                           0x05,
                           0x06,
                           0x07,
                           0x08,
                           0x09,
                           0x0A,
                           0x0B,
                           0x0C,
                           0x0D,
                           0x0E,
                           0x0F))
          .value());
  const auto kIncompleteMatch0(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x21,
                           DataType::kIncomplete128BitServiceUuids,
                           // First UUID
                           0x00,
                           0x01,
                           0x02,
                           0x03,
                           0x04,
                           0x05,
                           0x06,
                           0x07,
                           0x08,
                           0x09,
                           0x0A,
                           0x0B,
                           0x0C,
                           0x0D,
                           0x0E,
                           0x0F,

                           // kUuid0 - padded with the BT SIG Base UUID.
                           // See Core Spec v5.0, Vol 3, Part B, Section 2.5.1.
                           0xFB,
                           0x34,
                           0x9B,
                           0x5F,
                           0x80,
                           0x00,
                           0x00,
                           0x80,
                           0x00,
                           0x10,
                           0x00,
                           0x00,
                           LowerBits(kUuid0),
                           UpperBits(kUuid0),
                           0x00,
                           0x00))
          .value());
  const auto kIncompleteMatch1(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x21,
                           DataType::kIncomplete128BitServiceUuids,

                           // kUuid1
                           kUuid1[0],
                           kUuid1[1],
                           kUuid1[2],
                           kUuid1[3],
                           kUuid1[4],
                           kUuid1[5],
                           kUuid1[6],
                           kUuid1[7],
                           kUuid1[8],
                           kUuid1[9],
                           kUuid1[10],
                           kUuid1[11],
                           kUuid1[12],
                           kUuid1[13],
                           kUuid1[14],
                           kUuid1[15],

                           // Second UUID
                           0x00,
                           0x01,
                           0x02,
                           0x03,
                           0x04,
                           0x05,
                           0x06,
                           0x07,
                           0x08,
                           0x09,
                           0x0A,
                           0x0B,
                           0x0C,
                           0x0D,
                           0x0E,
                           0x0F))
          .value());
  const auto kCompleteEmpty(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x01, DataType::kComplete128BitServiceUuids))
          .value());
  const auto kCompleteNoMatch(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x11,
                           DataType::kComplete128BitServiceUuids,

                           // UUID
                           0x00,
                           0x01,
                           0x02,
                           0x03,
                           0x04,
                           0x05,
                           0x06,
                           0x07,
                           0x08,
                           0x09,
                           0x0A,
                           0x0B,
                           0x0C,
                           0x0D,
                           0x0E,
                           0x0F))
          .value());
  const auto kCompleteMatch0(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x21,
                           DataType::kComplete128BitServiceUuids,

                           // First UUID
                           0x00,
                           0x01,
                           0x02,
                           0x03,
                           0x04,
                           0x05,
                           0x06,
                           0x07,
                           0x08,
                           0x09,
                           0x0A,
                           0x0B,
                           0x0C,
                           0x0D,
                           0x0E,
                           0x0F,

                           // kUuid0 - padded with the BT SIG Base UUID.
                           // See Core Spec v5.0, Vol 3, Part B, Section 2.5.1.
                           0xFB,
                           0x34,
                           0x9B,
                           0x5F,
                           0x80,
                           0x00,
                           0x00,
                           0x80,
                           0x00,
                           0x10,
                           0x00,
                           0x00,
                           LowerBits(kUuid0),
                           UpperBits(kUuid0),
                           0x00,
                           0x00))
          .value());
  const auto kCompleteMatch1(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x21,
                           DataType::kComplete128BitServiceUuids,

                           // kUuid1
                           kUuid1[0],
                           kUuid1[1],
                           kUuid1[2],
                           kUuid1[3],
                           kUuid1[4],
                           kUuid1[5],
                           kUuid1[6],
                           kUuid1[7],
                           kUuid1[8],
                           kUuid1[9],
                           kUuid1[10],
                           kUuid1[11],
                           kUuid1[12],
                           kUuid1[13],
                           kUuid1[14],
                           kUuid1[15],

                           // Second UUID
                           0x00,
                           0x01,
                           0x02,
                           0x03,
                           0x04,
                           0x05,
                           0x06,
                           0x07,
                           0x08,
                           0x09,
                           0x0A,
                           0x0B,
                           0x0C,
                           0x0D,
                           0x0E,
                           0x0F))
          .value());

  DiscoveryFilter filter;

  // An empty filter should match all payloads.
  EXPECT_TRUE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kIncompleteEmpty, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(
      filter.Matches(kIncompleteNoMatch, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kIncompleteMatch0, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kIncompleteMatch1, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kCompleteEmpty, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kCompleteNoMatch, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kCompleteMatch0, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kCompleteMatch1, false, hci_spec::kRSSIInvalid));

  // Filter for kUuid0 and kUuid1.
  filter.set_service_uuids(std::vector<UUID>{UUID(kUuid0), UUID(kUuid1)});
  EXPECT_FALSE(filter.service_uuids().empty());
  EXPECT_FALSE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(kIncompleteEmpty, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(
      filter.Matches(kIncompleteNoMatch, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kIncompleteMatch0, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kIncompleteMatch1, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(kCompleteEmpty, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(kCompleteNoMatch, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kCompleteMatch0, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kCompleteMatch1, false, hci_spec::kRSSIInvalid));

  filter.Reset();
  EXPECT_TRUE(filter.service_uuids().empty());
}

TEST(DiscoveryFilterTest, 16BitServiceDataUuids) {
  constexpr uint16_t kUuid1 = 0x1800;

  const auto kNoMatch(
      AdvertisingData::FromBytes(
          StaticByteBuffer(
              0x05, DataType::kServiceData16Bit, 0x01, 0x02, 0x03, 0x04))
          .value());
  const auto kMatch0(
      AdvertisingData::FromBytes(StaticByteBuffer(0x05,
                                                  DataType::kServiceData16Bit,
                                                  LowerBits(kUuid0),
                                                  UpperBits(kUuid0),
                                                  0x01,
                                                  0x02))
          .value());
  const auto kMatch1(
      AdvertisingData::FromBytes(StaticByteBuffer(0x05,
                                                  DataType::kServiceData16Bit,
                                                  LowerBits(kUuid1),
                                                  UpperBits(kUuid1),
                                                  0x03,
                                                  0x04))
          .value());

  DiscoveryFilter filter;

  // An empty filter should match all payloads.
  EXPECT_TRUE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kNoMatch, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kMatch0, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kMatch1, false, hci_spec::kRSSIInvalid));

  // Filter for kUuid0 and kUuid1.
  filter.set_service_data_uuids(std::vector<UUID>{UUID(kUuid0), UUID(kUuid1)});
  EXPECT_FALSE(filter.service_data_uuids().empty());
  EXPECT_FALSE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(kNoMatch, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kMatch0, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kMatch1, false, hci_spec::kRSSIInvalid));

  filter.Reset();
  EXPECT_TRUE(filter.service_data_uuids().empty());
}

TEST(DiscoveryFilterTest, 32BitServiceDataUuids) {
  constexpr uint32_t kUuid1 = 0xabcd1800;

  const auto kNoMatch(
      AdvertisingData::FromBytes(StaticByteBuffer(0x09,
                                                  DataType::kServiceData32Bit,
                                                  // Random UUID
                                                  0x01,
                                                  0x02,
                                                  0x03,
                                                  0x04,
                                                  // Random UUID
                                                  0x05,
                                                  0x06,
                                                  0x07,
                                                  0x08))
          .value());
  const auto kMatch0(
      AdvertisingData::FromBytes(StaticByteBuffer(0x09,
                                                  DataType::kServiceData32Bit,
                                                  // kUuid0
                                                  LowerBits(kUuid0),
                                                  UpperBits(kUuid0),
                                                  0x00,
                                                  0x00,
                                                  // Data
                                                  0x01,
                                                  0x02,
                                                  0x03,
                                                  0x04))
          .value());
  const auto kMatch1(
      AdvertisingData::FromBytes(StaticByteBuffer(0x09,
                                                  DataType::kServiceData32Bit,
                                                  // kUuid1
                                                  0x00,
                                                  0x18,
                                                  0xcd,
                                                  0xab,
                                                  // Random UUID
                                                  0x01,
                                                  0x02,
                                                  0x03,
                                                  0x04))
          .value());

  DiscoveryFilter filter;

  // An empty filter should match all payloads.
  EXPECT_TRUE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kNoMatch, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kMatch0, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kMatch1, false, hci_spec::kRSSIInvalid));

  // Filter for kUuid0 and kUuid1.
  filter.set_service_data_uuids(std::vector<UUID>{UUID(kUuid0), UUID(kUuid1)});
  EXPECT_FALSE(filter.service_data_uuids().empty());
  EXPECT_FALSE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(kNoMatch, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kMatch0, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kMatch1, false, hci_spec::kRSSIInvalid));

  filter.Reset();
  EXPECT_TRUE(filter.service_data_uuids().empty());
}

TEST(DiscoveryFilterTest, 128BitServiceDataUuids) {
  constexpr UInt128 kUuid1 = {0xDE,
                              0xAD,
                              0xBE,
                              0xEF,
                              0xDE,
                              0xAD,
                              0xBE,
                              0xEF,
                              0xDE,
                              0xAD,
                              0xBE,
                              0xEF,
                              0x00,
                              0x18,
                              0xcd,
                              0xab};

  const auto kNoMatch(
      AdvertisingData::FromBytes(StaticByteBuffer(0x11,
                                                  DataType::kServiceData128Bit,

                                                  // Random UUID
                                                  0x00,
                                                  0x01,
                                                  0x02,
                                                  0x03,
                                                  0x04,
                                                  0x05,
                                                  0x06,
                                                  0x07,
                                                  0x08,
                                                  0x09,
                                                  0x0A,
                                                  0x0B,
                                                  0x0C,
                                                  0x0D,
                                                  0x0E,
                                                  0x0F))
          .value());
  const auto kMatch0(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x21,
                           DataType::kServiceData128Bit,
                           // kUuid0 - padded with the BT SIG Base UUID. See
                           // Core Spec v5.0, Vol 3, Part B, Section 2.5.1.
                           0xFB,
                           0x34,
                           0x9B,
                           0x5F,
                           0x80,
                           0x00,
                           0x00,
                           0x80,
                           0x00,
                           0x10,
                           0x00,
                           0x00,
                           LowerBits(kUuid0),
                           UpperBits(kUuid0),
                           0x00,
                           0x00,
                           // Random Data
                           0x00,
                           0x01,
                           0x02,
                           0x03,
                           0x04,
                           0x05,
                           0x06,
                           0x07,
                           0x08,
                           0x09,
                           0x0A,
                           0x0B,
                           0x0C,
                           0x0D,
                           0x0E,
                           0x0F))
          .value());
  const auto kMatch1(
      AdvertisingData::FromBytes(StaticByteBuffer(0x21,
                                                  DataType::kServiceData128Bit,

                                                  // kUuid1
                                                  kUuid1[0],
                                                  kUuid1[1],
                                                  kUuid1[2],
                                                  kUuid1[3],
                                                  kUuid1[4],
                                                  kUuid1[5],
                                                  kUuid1[6],
                                                  kUuid1[7],
                                                  kUuid1[8],
                                                  kUuid1[9],
                                                  kUuid1[10],
                                                  kUuid1[11],
                                                  kUuid1[12],
                                                  kUuid1[13],
                                                  kUuid1[14],
                                                  kUuid1[15],

                                                  // Random UUID
                                                  0x00,
                                                  0x01,
                                                  0x02,
                                                  0x03,
                                                  0x04,
                                                  0x05,
                                                  0x06,
                                                  0x07,
                                                  0x08,
                                                  0x09,
                                                  0x0A,
                                                  0x0B,
                                                  0x0C,
                                                  0x0D,
                                                  0x0E,
                                                  0x0F))
          .value());

  DiscoveryFilter filter;

  // An empty filter should match all payloads.
  EXPECT_TRUE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kNoMatch, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kMatch0, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kMatch1, false, hci_spec::kRSSIInvalid));

  // Filter for kUuid0 and kUuid1.
  filter.set_service_data_uuids(std::vector<UUID>{UUID(kUuid0), UUID(kUuid1)});
  EXPECT_FALSE(filter.service_data_uuids().empty());
  EXPECT_FALSE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(kNoMatch, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kMatch0, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kMatch1, false, hci_spec::kRSSIInvalid));

  filter.Reset();
  EXPECT_TRUE(filter.service_data_uuids().empty());
}

TEST(DiscoveryFilterTest, 16BitSolicitationUuids) {
  constexpr uint16_t kUuid1 = 0x1800;

  const auto kNoMatch(
      AdvertisingData::FromBytes(
          StaticByteBuffer(
              0x05, DataType::kSolicitationUuid16Bit, 0x01, 0x02, 0x03, 0x04))
          .value());
  const auto kMatch0(AdvertisingData::FromBytes(
                         StaticByteBuffer(0x05,
                                          DataType::kSolicitationUuid16Bit,
                                          LowerBits(kUuid0),
                                          UpperBits(kUuid0),
                                          0x01,
                                          0x02))
                         .value());
  const auto kMatch1(AdvertisingData::FromBytes(
                         StaticByteBuffer(0x05,
                                          DataType::kSolicitationUuid16Bit,
                                          LowerBits(kUuid1),
                                          UpperBits(kUuid1),
                                          0x03,
                                          0x04))
                         .value());

  DiscoveryFilter filter;

  // An empty filter should match all payloads.
  EXPECT_TRUE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kNoMatch, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kMatch0, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kMatch1, false, hci_spec::kRSSIInvalid));

  // Filter for kUuid0 and kUuid1.
  filter.set_solicitation_uuids(std::vector<UUID>{UUID(kUuid0), UUID(kUuid1)});
  EXPECT_FALSE(filter.solicitation_uuids().empty());
  EXPECT_FALSE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(kNoMatch, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kMatch0, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kMatch1, false, hci_spec::kRSSIInvalid));

  filter.Reset();
  EXPECT_TRUE(filter.solicitation_uuids().empty());
}

TEST(DiscoveryFilterTest, 32BitSolicitationUuids) {
  constexpr uint32_t kUuid1 = 0xabcd1800;

  const auto kNoMatch(AdvertisingData::FromBytes(
                          StaticByteBuffer(0x09,
                                           DataType::kSolicitationUuid32Bit,
                                           // Random UUID
                                           0x01,
                                           0x02,
                                           0x03,
                                           0x04,
                                           // Random UUID
                                           0x05,
                                           0x06,
                                           0x07,
                                           0x08))
                          .value());
  const auto kMatch0(AdvertisingData::FromBytes(
                         StaticByteBuffer(0x09,
                                          DataType::kSolicitationUuid32Bit,
                                          // kUuid0
                                          LowerBits(kUuid0),
                                          UpperBits(kUuid0),
                                          0x00,
                                          0x00,
                                          // Data
                                          0x01,
                                          0x02,
                                          0x03,
                                          0x04))
                         .value());
  const auto kMatch1(AdvertisingData::FromBytes(
                         StaticByteBuffer(0x09,
                                          DataType::kSolicitationUuid32Bit,
                                          // kUuid1
                                          0x00,
                                          0x18,
                                          0xcd,
                                          0xab,
                                          // Random UUID
                                          0x01,
                                          0x02,
                                          0x03,
                                          0x04))
                         .value());

  DiscoveryFilter filter;

  // An empty filter should match all payloads.
  EXPECT_TRUE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kNoMatch, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kMatch0, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kMatch1, false, hci_spec::kRSSIInvalid));

  // Filter for kUuid0 and kUuid1.
  filter.set_solicitation_uuids(std::vector<UUID>{UUID(kUuid0), UUID(kUuid1)});
  EXPECT_FALSE(filter.solicitation_uuids().empty());
  EXPECT_FALSE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(kNoMatch, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kMatch0, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kMatch1, false, hci_spec::kRSSIInvalid));

  filter.Reset();
  EXPECT_TRUE(filter.solicitation_uuids().empty());
}

TEST(DiscoveryFilterTest, 128BitSolicitationUuids) {
  constexpr UInt128 kUuid1 = {0xDE,
                              0xAD,
                              0xBE,
                              0xEF,
                              0xDE,
                              0xAD,
                              0xBE,
                              0xEF,
                              0xDE,
                              0xAD,
                              0xBE,
                              0xEF,
                              0x00,
                              0x18,
                              0xcd,
                              0xab};

  const auto kNoMatch(AdvertisingData::FromBytes(
                          StaticByteBuffer(0x11,
                                           DataType::kSolicitationUuid128Bit,

                                           // Random UUID
                                           0x00,
                                           0x01,
                                           0x02,
                                           0x03,
                                           0x04,
                                           0x05,
                                           0x06,
                                           0x07,
                                           0x08,
                                           0x09,
                                           0x0A,
                                           0x0B,
                                           0x0C,
                                           0x0D,
                                           0x0E,
                                           0x0F))
                          .value());
  const auto kMatch0(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x21,
                           DataType::kSolicitationUuid128Bit,
                           // kUuid0 - padded with the BT SIG Base UUID. See
                           // Core Spec v5.0, Vol 3, Part B, Section 2.5.1.
                           0xFB,
                           0x34,
                           0x9B,
                           0x5F,
                           0x80,
                           0x00,
                           0x00,
                           0x80,
                           0x00,
                           0x10,
                           0x00,
                           0x00,
                           LowerBits(kUuid0),
                           UpperBits(kUuid0),
                           0x00,
                           0x00,
                           // Random Data
                           0x00,
                           0x01,
                           0x02,
                           0x03,
                           0x04,
                           0x05,
                           0x06,
                           0x07,
                           0x08,
                           0x09,
                           0x0A,
                           0x0B,
                           0x0C,
                           0x0D,
                           0x0E,
                           0x0F))
          .value());
  const auto kMatch1(AdvertisingData::FromBytes(
                         StaticByteBuffer(0x21,
                                          DataType::kSolicitationUuid128Bit,

                                          // kUuid1
                                          kUuid1[0],
                                          kUuid1[1],
                                          kUuid1[2],
                                          kUuid1[3],
                                          kUuid1[4],
                                          kUuid1[5],
                                          kUuid1[6],
                                          kUuid1[7],
                                          kUuid1[8],
                                          kUuid1[9],
                                          kUuid1[10],
                                          kUuid1[11],
                                          kUuid1[12],
                                          kUuid1[13],
                                          kUuid1[14],
                                          kUuid1[15],

                                          // Random UUID
                                          0x00,
                                          0x01,
                                          0x02,
                                          0x03,
                                          0x04,
                                          0x05,
                                          0x06,
                                          0x07,
                                          0x08,
                                          0x09,
                                          0x0A,
                                          0x0B,
                                          0x0C,
                                          0x0D,
                                          0x0E,
                                          0x0F))
                         .value());

  DiscoveryFilter filter;

  // An empty filter should match all payloads.
  EXPECT_TRUE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kNoMatch, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kMatch0, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kMatch1, false, hci_spec::kRSSIInvalid));

  // Filter for kUuid0 and kUuid1.
  filter.set_solicitation_uuids(std::vector<UUID>{UUID(kUuid0), UUID(kUuid1)});
  EXPECT_FALSE(filter.solicitation_uuids().empty());
  EXPECT_FALSE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(kNoMatch, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kMatch0, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kMatch1, false, hci_spec::kRSSIInvalid));

  filter.Reset();
  EXPECT_TRUE(filter.solicitation_uuids().empty());
}

TEST(DiscoveryFilterTest, NameSubstring) {
  const auto kShortenedName(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x05, 0x08, 'T', 'e', 's', 't'))
          .value());
  const auto kCompleteName(AdvertisingData::FromBytes(StaticByteBuffer(0x0E,
                                                                       0x09,
                                                                       'T',
                                                                       'e',
                                                                       's',
                                                                       't',
                                                                       ' ',
                                                                       'C',
                                                                       'o',
                                                                       'm',
                                                                       'p',
                                                                       'l',
                                                                       'e',
                                                                       't',
                                                                       'e'))
                               .value());

  DiscoveryFilter filter;

  // An empty filter should match all payloads.
  EXPECT_TRUE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kShortenedName, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kCompleteName, false, hci_spec::kRSSIInvalid));

  // Assigning an empty string for the name filter should have the same effect
  // as an empty filter.
  filter.set_name_substring("");
  EXPECT_TRUE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kShortenedName, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kCompleteName, false, hci_spec::kRSSIInvalid));

  filter.set_name_substring("foo");
  EXPECT_FALSE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(kShortenedName, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(kCompleteName, false, hci_spec::kRSSIInvalid));

  filter.set_name_substring("est");
  EXPECT_FALSE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kShortenedName, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kCompleteName, false, hci_spec::kRSSIInvalid));

  filter.set_name_substring("Compl");
  EXPECT_FALSE(filter.name_substring().empty());
  EXPECT_FALSE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(kShortenedName, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kCompleteName, false, hci_spec::kRSSIInvalid));

  filter.Reset();
  EXPECT_TRUE(filter.name_substring().empty());
}

TEST(DiscoveryFilterTest, RSSI) {
  constexpr int8_t kRSSIThreshold = 60;
  DiscoveryFilter filter;
  filter.set_rssi(hci_spec::kRSSIInvalid);

  // |result| reports an invalid RSSI. This should fail to match even though the
  // value numerically satisfies the filter.
  EXPECT_FALSE(filter.Matches(std::nullopt, true, hci_spec::kRSSIInvalid));

  filter.set_rssi(kRSSIThreshold);
  EXPECT_FALSE(filter.Matches(std::nullopt, true, hci_spec::kRSSIInvalid));

  EXPECT_TRUE(filter.Matches(std::nullopt, true, kRSSIThreshold));

  EXPECT_TRUE(filter.Matches(std::nullopt, true, kRSSIThreshold + 1));

  // When a pathloss filter value is set and the scan result does not satisfy it
  // because it didn't include the transmission power level, the filter should
  // match since an RSSI value has been set which was used as a fallback.
  filter.set_pathloss(5);
  EXPECT_TRUE(filter.Matches(std::nullopt, true, kRSSIThreshold + 1));

  // Finally, an empty filter should always succeed.
  filter.Reset();
  EXPECT_TRUE(filter.Matches(std::nullopt, true, kRSSIThreshold + 1));
}

TEST(DiscoveryFilterTest, Pathloss) {
  constexpr int8_t kPathlossThreshold = 70;
  constexpr int8_t kTxPower = 5;
  constexpr int8_t kMatchingRSSI = -65;
  constexpr int8_t kNotMatchingRSSI = -66;
  constexpr int8_t kTooLargeRSSI = 71;

  const auto kDataWithTxPower(
      AdvertisingData::FromBytes(StaticByteBuffer(0x02, 0x0A, kTxPower))
          .value());

  DiscoveryFilter filter;
  filter.set_pathloss(kPathlossThreshold);

  // No Tx Power and no RSSI. Filter should not match.
  EXPECT_FALSE(filter.Matches(std::nullopt, true, hci_spec::kRSSIInvalid));

  // Tx Power is reported but RSSI is unknown. Filter should not match.
  EXPECT_FALSE(filter.Matches(kDataWithTxPower, true, hci_spec::kRSSIInvalid));

  // RSSI is known but Tx Power is not reported.
  EXPECT_FALSE(filter.Matches(std::nullopt, true, kMatchingRSSI));

  // RSSI and Tx Power are present and pathloss is within threshold.
  EXPECT_TRUE(filter.Matches(kDataWithTxPower, true, kMatchingRSSI));

  // RSSI and Tx Power are present but RSSI is larger than Tx Power.
  EXPECT_FALSE(filter.Matches(kDataWithTxPower, true, kTooLargeRSSI));

  // RSSI and Tx Power are present but pathloss is above threshold.
  EXPECT_FALSE(filter.Matches(kDataWithTxPower, true, kNotMatchingRSSI));

  // Assign a RSSI filter. Even though this field alone WOULD satisfy the
  // filter, the match function should not fall back to it when Tx Power is
  // present and the pathloss filter is unsatisfied.
  filter.set_rssi(kNotMatchingRSSI);
  EXPECT_TRUE(filter.pathloss());
  EXPECT_FALSE(filter.Matches(kDataWithTxPower, true, kNotMatchingRSSI));
  EXPECT_TRUE(filter.Matches(std::nullopt, true, kNotMatchingRSSI));

  // Test case for overflow
  // tx_power = 20, rssi = -120. Pathloss = 140. Threshold = 80.
  // Pathloss (140) > Threshold (80), so it should NOT match.
  filter.set_pathloss(80);
  const auto kDataWithOverflowTxPower(
      AdvertisingData::FromBytes(StaticByteBuffer(0x02, 0x0A, 20)).value());
  EXPECT_FALSE(filter.Matches(kDataWithOverflowTxPower, true, -120));

  // Finally, an empty filter should always succeed.
  filter.Reset();
  EXPECT_FALSE(filter.pathloss());
  EXPECT_TRUE(filter.Matches(kDataWithTxPower, true, kNotMatchingRSSI));
}

TEST(DiscoveryFilterTest, ManufacturerCode) {
  const auto kValidData0(
      AdvertisingData::FromBytes(StaticByteBuffer(0x03, 0xFF, 0xE0, 0x00))
          .value());
  const auto kValidData1(
      AdvertisingData::FromBytes(
          StaticByteBuffer(0x06, 0xFF, 0xE0, 0x00, 0x01, 0x02, 0x03))
          .value());
  const auto kInvalidData1(
      AdvertisingData::FromBytes(StaticByteBuffer(0x03, 0xFF, 0x4C, 0x00))
          .value());

  DiscoveryFilter filter;

  // Empty filter should match everything.
  EXPECT_TRUE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kValidData0, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kValidData1, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kInvalidData1, false, hci_spec::kRSSIInvalid));

  filter.set_manufacturer_code(0x00E0);
  EXPECT_TRUE(filter.manufacturer_code());
  EXPECT_FALSE(filter.Matches(std::nullopt, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kValidData0, false, hci_spec::kRSSIInvalid));
  EXPECT_TRUE(filter.Matches(kValidData1, false, hci_spec::kRSSIInvalid));
  EXPECT_FALSE(filter.Matches(kInvalidData1, false, hci_spec::kRSSIInvalid));

  filter.Reset();
  EXPECT_FALSE(filter.manufacturer_code());
}

TEST(DiscoveryFilterTest, Combined) {
  constexpr int8_t kMatchingPathlossThreshold = 70;
  constexpr int8_t kNotMatchingPathlossThreshold = 69;
  constexpr int8_t kTxPower = 5;
  constexpr int8_t kRSSI = -65;

  constexpr uint16_t kMatchingUuid = 0x180d;
  constexpr uint16_t kNotMatchingUuid = 0x1800;
  constexpr uint16_t kMatchingServiceDataUuid = 0x1234;
  constexpr uint16_t kNotMatchingServiceDataUuid = 0x5678;

  constexpr char kMatchingName[] = "test";
  constexpr char kNotMatchingName[] = "foo";

  const auto kAdvertisingData(
      AdvertisingData::FromBytes(StaticByteBuffer(
                                     // Flags
                                     0x02,
                                     0x01,
                                     0x01,

                                     // 16 Bit Service UUIDs
                                     0x03,
                                     0x02,
                                     0x0d,
                                     0x18,

                                     // 16 Bit Service Data UUIDs
                                     0x03,
                                     DataType::kServiceData16Bit,
                                     0x34,
                                     0x12,

                                     // Complete name
                                     0x05,
                                     0x09,
                                     't',
                                     'e',
                                     's',
                                     't',

                                     // Tx Power Level
                                     0x02,
                                     0x0A,
                                     kTxPower,

                                     // Manufacturer specific data
                                     0x05,
                                     0xFF,
                                     0xE0,
                                     0x00,
                                     0x01,
                                     0x02))
          .value());

  DiscoveryFilter filter;

  // Empty filter should match.
  EXPECT_TRUE(filter.Matches(kAdvertisingData, true, kRSSI));

  // Assign all fields and make them match.
  filter.set_flags(0x01);
  filter.set_connectable(true);
  filter.set_service_uuids(std::vector<UUID>{UUID(kMatchingUuid)});
  filter.set_service_data_uuids(
      std::vector<UUID>{UUID(kMatchingServiceDataUuid)});
  filter.set_name_substring(kMatchingName);
  filter.set_pathloss(kMatchingPathlossThreshold);
  filter.set_manufacturer_code(0x00E0);
  EXPECT_TRUE(filter.Matches(kAdvertisingData, true, kRSSI));

  // Toggle each field one by one to test that a single mismatch causes the
  // filter to fail.
  filter.set_flags(0x03, /*require_all=*/true);
  EXPECT_FALSE(filter.Matches(kAdvertisingData, true, kRSSI));
  filter.set_flags(0x01);

  filter.set_connectable(false);
  EXPECT_FALSE(filter.Matches(kAdvertisingData, true, kRSSI));
  filter.set_connectable(true);

  filter.set_service_uuids(std::vector<UUID>{UUID(kNotMatchingUuid)});
  EXPECT_FALSE(filter.Matches(kAdvertisingData, true, kRSSI));
  filter.set_service_uuids(std::vector<UUID>{UUID(kMatchingUuid)});

  filter.set_service_data_uuids(
      std::vector<UUID>{UUID(kNotMatchingServiceDataUuid)});
  EXPECT_FALSE(filter.Matches(kAdvertisingData, true, kRSSI));
  filter.set_service_data_uuids(
      std::vector<UUID>{UUID(kMatchingServiceDataUuid)});

  filter.set_name_substring(kNotMatchingName);
  EXPECT_FALSE(filter.Matches(kAdvertisingData, true, kRSSI));
  filter.set_name_substring(kMatchingName);

  filter.set_pathloss(kNotMatchingPathlossThreshold);
  EXPECT_FALSE(filter.Matches(kAdvertisingData, true, kRSSI));
  filter.set_pathloss(kMatchingPathlossThreshold);

  filter.set_manufacturer_code(0x004C);
  EXPECT_FALSE(filter.Matches(kAdvertisingData, true, kRSSI));
  filter.set_manufacturer_code(0x00E0);

  EXPECT_TRUE(filter.Matches(kAdvertisingData, true, kRSSI));
}

TEST(DiscoveryFilterTest, GeneralDiscoveryFlags) {
  const auto kLimitedDiscoverableData(
      AdvertisingData::FromBytes(StaticByteBuffer(
                                     // Flags
                                     0x02,
                                     0x01,
                                     0x01))
          .value());
  const auto kGeneralDiscoverableData(
      AdvertisingData::FromBytes(StaticByteBuffer(
                                     // Flags
                                     0x02,
                                     0x01,
                                     0x02))
          .value());
  const auto kNonDiscoverableData(
      AdvertisingData::FromBytes(
          StaticByteBuffer(
              // Flags (all flags are set except for discoverability).
              0x02,
              0x01,
              0xFC))
          .value());

  DiscoveryFilter filter;
  filter.SetGeneralDiscoveryFlags();

  EXPECT_TRUE(filter.Matches(kLimitedDiscoverableData, true, 0));
  EXPECT_TRUE(filter.Matches(kGeneralDiscoverableData, true, 0));
  EXPECT_FALSE(filter.Matches(kNonDiscoverableData, true, 0));
}

TEST(DiscoveryFilter, OperatorEqualsEquals) {
  constexpr uint16_t kUuid1 = 0x1800;

  DiscoveryFilter filter_a;
  filter_a.set_flags(0b100, true);
  filter_a.set_service_uuids(std::vector<UUID>{UUID(kUuid0)});
  filter_a.set_service_data_uuids(std::vector<UUID>{UUID(kUuid0)});
  filter_a.set_solicitation_uuids(std::vector<UUID>{UUID(kUuid0)});
  filter_a.set_name_substring("foobar");
  filter_a.set_connectable(true);
  filter_a.set_manufacturer_code(1);
  filter_a.set_pathloss(1);
  filter_a.set_rssi(1);

  DiscoveryFilter filter_b = filter_a;
  ASSERT_EQ(filter_a, filter_b);

  filter_b = filter_a;
  filter_b.set_flags(0b100, false);
  EXPECT_NE(filter_a, filter_b);

  filter_b = filter_a;
  filter_b.set_flags(0b101, true);
  EXPECT_NE(filter_a, filter_b);

  filter_b = filter_a;
  filter_a.set_service_uuids(std::vector<UUID>{UUID(kUuid1)});
  EXPECT_NE(filter_a, filter_b);

  filter_b = filter_a;
  filter_a.set_service_data_uuids(std::vector<UUID>{UUID(kUuid1)});
  EXPECT_NE(filter_a, filter_b);

  filter_b = filter_a;
  filter_a.set_solicitation_uuids(std::vector<UUID>{UUID(kUuid1)});
  EXPECT_NE(filter_a, filter_b);

  filter_b = filter_a;
  filter_a.set_name_substring("bluetooth");
  EXPECT_NE(filter_a, filter_b);

  filter_b = filter_a;
  filter_a.set_connectable(false);
  EXPECT_NE(filter_a, filter_b);

  filter_b = filter_a;
  filter_a.set_manufacturer_code(0);
  EXPECT_NE(filter_a, filter_b);

  filter_b = filter_a;
  filter_a.set_pathloss(0);
  EXPECT_NE(filter_a, filter_b);

  filter_b = filter_a;
  filter_a.set_rssi(0);
  EXPECT_NE(filter_a, filter_b);
}

TEST(DiscoveryFilter, ToString) {
  DiscoveryFilter filter;
  filter.set_flags(0b100, false);
  filter.set_service_uuids(std::vector<UUID>{UUID(kUuid0)});
  filter.set_service_data_uuids(std::vector<UUID>{UUID(kUuid0)});
  filter.set_solicitation_uuids(std::vector<UUID>{UUID(kUuid0)});
  filter.set_name_substring("foobar");
  filter.set_connectable(true);
  filter.set_manufacturer_code(1);
  filter.set_pathloss(1);
  filter.set_rssi(1);

  std::string output =
      "flags: {0x04}, all flags required: {false}, service uuids: "
      "{0000180d-0000-1000-8000-00805f9b34fb}, service data uuids: "
      "{0000180d-0000-1000-8000-00805f9b34fb}, solicitation uuids: "
      "{0000180d-0000-1000-8000-00805f9b34fb}, name substring: {foobar}, "
      "connectable: {true}, manufacturer code: {0x01}, "
      "pathloss: {0x01}, rssi: {0x01}";
  EXPECT_EQ(output, filter.ToString());
}

TEST(DiscoveryFilter, ToStringDataMissing) {
  DiscoveryFilter filter;
  std::string output =
      "flags: {0x00}, all flags required: {false}, service uuids: {unset}, "
      "service data uuids: {unset}, solicitation uuids: {unset}, name "
      "substring: {unset}, connectable: {unset}, manufacturer code: {0x00}, "
      "pathloss: {0x00}, rssi: {0x00}";
  EXPECT_EQ(output, filter.ToString());
}

}  // namespace
}  // namespace bt::hci
