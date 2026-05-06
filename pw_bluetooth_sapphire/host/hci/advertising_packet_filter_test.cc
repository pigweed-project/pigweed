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

#include "pw_bluetooth_sapphire/internal/host/hci/advertising_packet_filter.h"

#include "gtest/gtest.h"
#include "pw_bluetooth_sapphire/internal/host/common/advertising_data.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/vendor_protocol.h"
#include "pw_bluetooth_sapphire/internal/host/hci/discovery_filter.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_controller.h"

namespace bt::hci {

using bt::testing::FakeController;
using TestingBase = bt::testing::FakeDispatcherControllerTest<FakeController>;

constexpr uint16_t kUuid = 0x1234;

class AdvertisingPacketFilterTest : public TestingBase {
 public:
  AdvertisingPacketFilterTest() = default;
  ~AdvertisingPacketFilterTest() override = default;

 protected:
  void SetUp() override {
    TestingBase::SetUp();

    FakeController::Settings settings;
    settings.ApplyLegacyLEConfig();
    this->test_device()->set_settings(settings);
  }

  void TearDown() override {
    this->test_device()->Stop();
    TestingBase::TearDown();
  }

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(AdvertisingPacketFilterTest);
};

// can set and unset packet filters
TEST_F(AdvertisingPacketFilterTest, SetUnsetPacketFilters) {
  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/false,
       /*max_filters=*/0,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
      transport()->GetWeakPtr());
  ASSERT_EQ(0u, packet_filter.NumScanIds());

  packet_filter.SetPacketFilters(0, {});
  ASSERT_EQ(1u, packet_filter.NumScanIds());

  packet_filter.UnsetPacketFilters(0);
  ASSERT_EQ(0u, packet_filter.NumScanIds());
}

// filtering passes if we haven't added any filters
TEST_F(AdvertisingPacketFilterTest, FilterWithNoScanId) {
  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/false,
       /*max_filters=*/0,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
      transport()->GetWeakPtr());
  EXPECT_TRUE(packet_filter.Matches(
      0, fit::error(AdvertisingData::ParseError::kMissing), true, 0));
}

// filtering passes if we have added an empty filter
TEST_F(AdvertisingPacketFilterTest, FilterWithEmptyFilters) {
  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/false,
       /*max_filters=*/0,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
      transport()->GetWeakPtr());
  packet_filter.SetPacketFilters(0, {});
  EXPECT_TRUE(packet_filter.Matches(
      0, fit::error(AdvertisingData::ParseError::kMissing), true, 0));
}

// filtering passes if we have a simple filter
TEST_F(AdvertisingPacketFilterTest, Filter) {
  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/false,
       /*max_filters=*/0,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
      transport()->GetWeakPtr());

  DiscoveryFilter filter;
  filter.set_connectable(true);
  packet_filter.SetPacketFilters(0, {filter});

  EXPECT_TRUE(packet_filter.Matches(
      0, fit::error(AdvertisingData::ParseError::kMissing), true, 0));
  EXPECT_FALSE(packet_filter.Matches(
      0, fit::error(AdvertisingData::ParseError::kMissing), false, 0));
}

// filtering passes only on the correct filter
TEST_F(AdvertisingPacketFilterTest, MultipleScanIds) {
  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/false,
       /*max_filters=*/0,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
      transport()->GetWeakPtr());

  DiscoveryFilter filter_a;
  filter_a.set_connectable(true);
  packet_filter.SetPacketFilters(0, {filter_a});

  DiscoveryFilter filter_b;
  filter_b.set_name_substring("bluetooth");
  packet_filter.SetPacketFilters(1, {filter_b});

  EXPECT_TRUE(packet_filter.Matches(
      0, fit::error(AdvertisingData::ParseError::kMissing), true, 0));
  EXPECT_FALSE(packet_filter.Matches(
      1, fit::error(AdvertisingData::ParseError::kMissing), true, 0));

  {
    AdvertisingData ad;
    ASSERT_TRUE(ad.SetLocalName("a bluetooth device"));
    EXPECT_FALSE(packet_filter.Matches(0, fit::ok(std::move(ad)), false, 0));
  }

  {
    AdvertisingData ad;
    ASSERT_TRUE(ad.SetLocalName("a bluetooth device"));
    EXPECT_TRUE(packet_filter.Matches(1, fit::ok(std::move(ad)), false, 0));
  }
}

// can update a filter by replacing it
TEST_F(AdvertisingPacketFilterTest, SetPacketFiltersReplacesPrevious) {
  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/false,
       /*max_filters=*/0,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
      transport()->GetWeakPtr());

  packet_filter.SetPacketFilters(0, {});
  EXPECT_TRUE(packet_filter.Matches(
      0, fit::error(AdvertisingData::ParseError::kMissing), false, 0));

  DiscoveryFilter filter;
  filter.set_connectable(true);
  packet_filter.SetPacketFilters(0, {filter});

  EXPECT_FALSE(packet_filter.Matches(
      0, fit::error(AdvertisingData::ParseError::kMissing), false, 0));
  EXPECT_TRUE(packet_filter.Matches(
      0, fit::error(AdvertisingData::ParseError::kMissing), true, 0));
}

// offloading isn't started if we don't ask for it
TEST_F(AdvertisingPacketFilterTest, OffloadingRemainsDisabledIfConfiguredOff) {
  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/false,
       /*max_filters=*/0,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
      transport()->GetWeakPtr());
  packet_filter.SetPacketFilters(0, {});

  RunUntilIdle();
  EXPECT_FALSE(packet_filter.IsUsingOffloadedFiltering());
  EXPECT_FALSE(test_device()->packet_filter_state().enabled);
}

// offloading doesn't begin until we actually have a filter to offload
TEST_F(AdvertisingPacketFilterTest, UsesOffloadedFilteringWhenFiltersAreSet) {
  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/true,
       /*max_filters=*/3,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
      transport()->GetWeakPtr());

  EXPECT_FALSE(packet_filter.IsUsingOffloadedFiltering());
  EXPECT_FALSE(test_device()->packet_filter_state().enabled);

  DiscoveryFilter filter;
  packet_filter.SetPacketFilters(0, {filter});
  RunUntilIdle();
  EXPECT_TRUE(packet_filter.IsUsingOffloadedFiltering());
  EXPECT_TRUE(test_device()->packet_filter_state().enabled);
}

// disable offloading if we can't store all filters on chip
TEST_F(AdvertisingPacketFilterTest, OffloadingDisabledIfMemoryUnavailable) {
  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/true,
       /*max_filters=*/1,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
      transport()->GetWeakPtr());

  DiscoveryFilter filter_a;
  filter_a.set_name_substring("bluetooth");
  packet_filter.SetPacketFilters(0, {filter_a});
  RunUntilIdle();
  EXPECT_TRUE(packet_filter.IsUsingOffloadedFiltering());

  DiscoveryFilter filter_b;
  filter_b.set_name_substring("bluetooth");
  packet_filter.SetPacketFilters(1, {filter_b});
  RunUntilIdle();
  EXPECT_FALSE(packet_filter.IsUsingOffloadedFiltering());
}

// reeneable offloading if we remove filters and memory is now available on the
// Controller
TEST_F(AdvertisingPacketFilterTest, OffloadingReenabledIfMemoryAvailable) {
  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/true,
       /*max_filters=*/1,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
      transport()->GetWeakPtr());

  DiscoveryFilter filter_a;
  filter_a.set_name_substring("bluetooth");
  packet_filter.SetPacketFilters(0, {filter_a});
  RunUntilIdle();

  DiscoveryFilter filter_b;
  filter_b.set_name_substring("bluetooth");
  packet_filter.SetPacketFilters(1, {filter_b});
  RunUntilIdle();
  EXPECT_FALSE(packet_filter.IsUsingOffloadedFiltering());

  packet_filter.UnsetPacketFilters(1);
  RunUntilIdle();
  EXPECT_TRUE(packet_filter.IsUsingOffloadedFiltering());
}

// reenable offloading if memory is available even if we don't remove the filter
// index itself
TEST_F(AdvertisingPacketFilterTest, UnsetFiltersDoesntInadvertentlyEnable) {
  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/true,
       /*max_filters=*/1,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
      transport()->GetWeakPtr());

  DiscoveryFilter filter_a;
  filter_a.set_name_substring("bluetooth");
  packet_filter.SetPacketFilters(0, {filter_a});
  RunUntilIdle();
  ASSERT_TRUE(packet_filter.IsUsingOffloadedFiltering());

  DiscoveryFilter filter_b;
  filter_b.set_name_substring("fuchsia");
  packet_filter.SetPacketFilters(1, {filter_b});
  RunUntilIdle();

  // Offloading should now be disabled because max_filters is 1 but two filters
  // were added
  ASSERT_FALSE(packet_filter.IsUsingOffloadedFiltering());

  filter_b.set_name_substring("another");
  packet_filter.SetPacketFilters(1, {filter_b});
  RunUntilIdle();

  // Offloading should remain off even after calling SetPacketFilters
  ASSERT_FALSE(packet_filter.IsUsingOffloadedFiltering());
}

TEST_F(AdvertisingPacketFilterTest, FilterWithEmptyFiltersOffloadingSupported) {
  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/true,
       /*max_filters=*/1,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
      transport()->GetWeakPtr());

  // Ensure we treat an empty set of filters as the allow all filter and send it
  // to the Controller
  packet_filter.SetPacketFilters(0, {});
  RunUntilIdle();

  ASSERT_TRUE(packet_filter.IsUsingOffloadedFiltering());
  ASSERT_TRUE(test_device()->packet_filter_state().enabled);
  ASSERT_EQ(1u, test_device()->packet_filter_state().filters.size());
}

TEST_F(AdvertisingPacketFilterTest, HostFilteringUsesOnlyAllowAllFilter) {
  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/true,
       /*max_filters=*/1,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
      transport()->GetWeakPtr());

  DiscoveryFilter filter_a;
  filter_a.set_name_substring("bluetooth");
  packet_filter.SetPacketFilters(0, {filter_a});
  RunUntilIdle();
  ASSERT_TRUE(packet_filter.IsUsingOffloadedFiltering());

  {
    uint8_t filter_index = packet_filter.last_filter_index();
    ASSERT_EQ(1u, test_device()->packet_filter_state().filters.size());
    const FakeController::PacketFilter& controller_filter =
        test_device()->packet_filter_state().filters.at(filter_index);
    ASSERT_EQ(controller_filter.local_name, "bluetooth");
  }

  packet_filter.UnsetPacketFilters(0);
  RunUntilIdle();
  ASSERT_FALSE(packet_filter.IsUsingOffloadedFiltering());

  {
    uint8_t filter_index = packet_filter.last_filter_index();
    ASSERT_EQ(1u, test_device()->packet_filter_state().filters.size());
    const FakeController::PacketFilter& controller_filter =
        test_device()->packet_filter_state().filters.at(filter_index);
    ASSERT_FALSE(controller_filter.broadcast_address.has_value());
    ASSERT_FALSE(controller_filter.service_uuid.has_value());
    ASSERT_FALSE(controller_filter.solicitation_uuid.has_value());
    ASSERT_FALSE(controller_filter.local_name.has_value());
    ASSERT_FALSE(controller_filter.manufacturer_data.has_value());
    ASSERT_FALSE(controller_filter.manufacturer_data_mask.has_value());
    ASSERT_FALSE(controller_filter.service_data.has_value());
    ASSERT_FALSE(controller_filter.service_data_mask.has_value());
  }
}

// replace filters if we send a new set with the same scan id
TEST_F(AdvertisingPacketFilterTest, OffloadingSetPacketFiltersReplaces) {
  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/true,
       /*max_filters=*/1,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
      transport()->GetWeakPtr());

  DiscoveryFilter filter_a;
  filter_a.set_name_substring("foo");
  packet_filter.SetPacketFilters(0, {filter_a});
  RunUntilIdle();

  {
    uint8_t filter_index = packet_filter.last_filter_index();
    const FakeController::PacketFilter& controller_filter =
        test_device()->packet_filter_state().filters.at(filter_index);
    ASSERT_EQ(controller_filter.local_name, "foo");
  }

  DiscoveryFilter filter_b;
  filter_b.set_name_substring("bar");
  packet_filter.SetPacketFilters(0, {filter_b});
  RunUntilIdle();

  {
    uint8_t filter_index = packet_filter.last_filter_index();
    const FakeController::PacketFilter& controller_filter =
        test_device()->packet_filter_state().filters.at(filter_index);
    ASSERT_EQ(controller_filter.local_name, "bar");
  }
}

// service uuid filter is sent to the controller
TEST_F(AdvertisingPacketFilterTest, OffloadingServiceUUID) {
  UUID uuid(kUuid);

  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/true,
       /*max_filters=*/1,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
      transport()->GetWeakPtr());
  DiscoveryFilter filter;
  filter.set_service_uuids({uuid});
  packet_filter.SetPacketFilters(0, {filter});
  RunUntilIdle();

  uint8_t filter_index = packet_filter.last_filter_index();
  const FakeController::PacketFilter& controller_filter =
      test_device()->packet_filter_state().filters.at(filter_index);
  ASSERT_TRUE(controller_filter.service_uuid.has_value());
  EXPECT_EQ(controller_filter.service_uuid.value(), uuid);
}

// solicitation uuid filter is sent to the controller
TEST_F(AdvertisingPacketFilterTest, OffloadingSolicitationUUID) {
  UUID uuid(kUuid);

  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/true,
       /*max_filters=*/1,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
      transport()->GetWeakPtr());
  DiscoveryFilter filter;
  filter.set_solicitation_uuids({uuid});
  packet_filter.SetPacketFilters(0, {filter});
  RunUntilIdle();

  uint8_t filter_index = packet_filter.last_filter_index();
  const FakeController::PacketFilter& controller_filter =
      test_device()->packet_filter_state().filters.at(filter_index);
  ASSERT_TRUE(controller_filter.solicitation_uuid.has_value());
  EXPECT_EQ(controller_filter.solicitation_uuid.value(), uuid);
}

// local name filter is sent to the controller
TEST_F(AdvertisingPacketFilterTest, OffloadingNameSubstring) {
  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/true,
       /*max_filters=*/1,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
      transport()->GetWeakPtr());
  DiscoveryFilter filter;
  filter.set_name_substring("bluetooth");
  packet_filter.SetPacketFilters(0, {filter});
  RunUntilIdle();

  uint8_t filter_index = packet_filter.last_filter_index();
  const FakeController::PacketFilter& controller_filter =
      test_device()->packet_filter_state().filters.at(filter_index);
  ASSERT_EQ(controller_filter.local_name, "bluetooth");
}

// service data uuid filter is sent to the controller
TEST_F(AdvertisingPacketFilterTest, OffloadingServiceDataUUID) {
  UUID uuid(kUuid);

  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/true,
       /*max_filters=*/1,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
      transport()->GetWeakPtr());
  DiscoveryFilter filter;
  filter.set_service_data_uuids({uuid});
  packet_filter.SetPacketFilters(0, {filter});
  RunUntilIdle();

  uint8_t filter_index = packet_filter.last_filter_index();
  const FakeController::PacketFilter& controller_filter =
      test_device()->packet_filter_state().filters.at(filter_index);
  ASSERT_TRUE(controller_filter.service_data.has_value());
  ASSERT_TRUE(controller_filter.service_data_mask.has_value());
}

// manufacturer code filter is sent to the controller
TEST_F(AdvertisingPacketFilterTest, OffloadingManufacturerCode) {
  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/true,
       /*max_filters=*/1,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
      transport()->GetWeakPtr());
  DiscoveryFilter filter;
  filter.set_manufacturer_code(kUuid);
  packet_filter.SetPacketFilters(0, {filter});
  RunUntilIdle();

  uint8_t filter_index = packet_filter.last_filter_index();
  const FakeController::PacketFilter& controller_filter =
      test_device()->packet_filter_state().filters.at(filter_index);
  ASSERT_TRUE(controller_filter.manufacturer_data.has_value());
  ASSERT_TRUE(controller_filter.manufacturer_data_mask.has_value());
}

// Ensure we don't try enabling packet filtering if the constructor was told the
// feature is disabled
TEST_F(AdvertisingPacketFilterTest, UnsetFiltersDoesntEnableWhenFeatureOff) {
  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/false,
       /*max_filters=*/1,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
      transport()->GetWeakPtr());

  DiscoveryFilter filter;
  filter.set_name_substring("bluetooth");
  packet_filter.SetPacketFilters(0, {filter});
  RunUntilIdle();
  ASSERT_FALSE(test_device()->packet_filter_state().enabled);

  packet_filter.UnsetPacketFilters(0);
  RunUntilIdle();
  ASSERT_FALSE(test_device()->packet_filter_state().enabled);
}

// An offloaded filter with no rssi threhsold set should have the rssi threshold
// set to the lowest possible 8-bit two's complement signed integer
TEST_F(AdvertisingPacketFilterTest, OffloadingSetsDefaultRssiThreshold) {
  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/true,
       /*max_filters=*/1,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
      transport()->GetWeakPtr());

  DiscoveryFilter filter;
  filter.set_name_substring("bluetooth");
  packet_filter.SetPacketFilters(0, {filter});
  RunUntilIdle();

  uint8_t filter_index = packet_filter.last_filter_index();
  const FakeController::PacketFilter& controller_filter =
      test_device()->packet_filter_state().filters.at(filter_index);
  ASSERT_EQ(controller_filter.rssi_high_threshold, 0x80);
}

// Immediate delivery mode is used if the constructor was told that delivery
// mode should be kImmediate
TEST_F(AdvertisingPacketFilterTest, ImmediateDeliveryModeIsUsedWhenConfigured) {
  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/true,
       /*max_filters=*/1,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
      transport()->GetWeakPtr());

  DiscoveryFilter filter;
  filter.set_name_substring("bluetooth");
  packet_filter.SetPacketFilters(0, {filter});
  RunUntilIdle();

  EXPECT_EQ(1u, test_device()->packet_filter_state().filters.count(0));
  const FakeController::PacketFilter& f =
      test_device()->packet_filter_state().filters.find(0)->second;
  EXPECT_EQ(AdvertisingPacketFilter::Config::DeliveryMode::kImmediate,
            f.delivery_mode);
}

// Batched delivery mode is used if the constructor was told that delivery mode
// should be kBatched
TEST_F(AdvertisingPacketFilterTest, BatchedDeliveryModeIsUsedWhenConfigured) {
  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/true,
       /*max_filters=*/1,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kBatched},
      transport()->GetWeakPtr());

  DiscoveryFilter filter;
  filter.set_name_substring("bluetooth");
  packet_filter.SetPacketFilters(0, {filter});
  RunUntilIdle();

  EXPECT_EQ(1u, test_device()->packet_filter_state().filters.count(0));
  const FakeController::PacketFilter& f =
      test_device()->packet_filter_state().filters.find(0)->second;
  EXPECT_EQ(AdvertisingPacketFilter::Config::DeliveryMode::kBatched,
            f.delivery_mode);
}

// Ensure that we fallback to host filtering if an HCI command fails
TEST_F(AdvertisingPacketFilterTest, OffloadingFailsOnHciError) {
  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/true,
       /*max_filters=*/2,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
      transport()->GetWeakPtr());

  DiscoveryFilter filter_a;
  filter_a.set_name_substring("bluetooth");
  packet_filter.SetPacketFilters(0, {filter_a});
  RunUntilIdle();
  ASSERT_TRUE(packet_filter.IsUsingOffloadedFiltering());

  // Instruct the fake controller to fail APCF commands
  test_device()->SetDefaultResponseStatus(
      hci_spec::vendor::android::kLEApcf,
      pw::bluetooth::emboss::StatusCode::HARDWARE_FAILURE);

  DiscoveryFilter filter_b;
  filter_b.set_name_substring("bluetooth");
  packet_filter.SetPacketFilters(0, {filter_b});
  RunUntilIdle();

  EXPECT_FALSE(packet_filter.IsUsingOffloadedFiltering());
}

// Ensure that we still offload filters even if they only contain nonoffloadable
// fields (e.g. rssi). The resulting filter should be the allow all filter in
// terms of APCF features.
TEST_F(AdvertisingPacketFilterTest, OffloadingFilterWithNoOffloadableFields) {
  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/true,
       /*max_filters=*/1,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
      transport()->GetWeakPtr());

  DiscoveryFilter filter;
  filter.set_rssi(-50);
  packet_filter.SetPacketFilters(0, {filter});
  RunUntilIdle();

  ASSERT_TRUE(packet_filter.IsUsingOffloadedFiltering());
  ASSERT_TRUE(test_device()->packet_filter_state().enabled);
  ASSERT_EQ(1u, test_device()->packet_filter_state().filters.size());

  uint8_t filter_index = packet_filter.last_filter_index();
  const FakeController::PacketFilter& controller_filter =
      test_device()->packet_filter_state().filters.at(filter_index);

  // RSSI threshold should be set to the requested value.
  EXPECT_EQ(static_cast<int8_t>(controller_filter.rssi_high_threshold.value()),
            -50);

  // All other content-based fields should be empty.
  EXPECT_FALSE(controller_filter.service_uuid.has_value());
  EXPECT_FALSE(controller_filter.solicitation_uuid.has_value());
  EXPECT_FALSE(controller_filter.local_name.has_value());
  EXPECT_FALSE(controller_filter.manufacturer_data.has_value());
  EXPECT_FALSE(controller_filter.service_data.has_value());
}

// Ensure that filters with no content specific configuration still take up
// a slot in our accounting.
TEST_F(AdvertisingPacketFilterTest,
       MultipleScanIdsWithEmptyFiltersExhaustMemory) {
  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/true,
       /*max_filters=*/2,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
      transport()->GetWeakPtr());

  packet_filter.SetPacketFilters(0, {});
  RunUntilIdle();
  ASSERT_TRUE(packet_filter.IsUsingOffloadedFiltering());
  ASSERT_EQ(1u, test_device()->packet_filter_state().filters.size());

  packet_filter.SetPacketFilters(1, {});
  RunUntilIdle();
  ASSERT_TRUE(packet_filter.IsUsingOffloadedFiltering());
  ASSERT_EQ(2u, test_device()->packet_filter_state().filters.size());

  packet_filter.SetPacketFilters(2, {});
  RunUntilIdle();

  EXPECT_FALSE(packet_filter.IsUsingOffloadedFiltering());
}

// Ensure that UUID accounting is calculated correctly
TEST_F(AdvertisingPacketFilterTest, MultipleUUIDsExhaustMemory) {
  AdvertisingPacketFilter packet_filter(
      {/*offloading_supported=*/true,
       /*max_filters=*/2,
       /*peer_delivery_mode=*/
       AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
      transport()->GetWeakPtr());

  DiscoveryFilter filter;
  filter.set_service_uuids({UUID(uint16_t(0x1234)), UUID(uint16_t(0x5678))});

  packet_filter.SetPacketFilters(0, {filter});
  RunUntilIdle();
  ASSERT_TRUE(packet_filter.IsUsingOffloadedFiltering());

  DiscoveryFilter filter2;
  filter2.set_service_uuids({UUID(uint16_t(0x90ab))});
  packet_filter.SetPacketFilters(1, {filter2});
  RunUntilIdle();

  EXPECT_FALSE(packet_filter.IsUsingOffloadedFiltering());
}
}  // namespace bt::hci
