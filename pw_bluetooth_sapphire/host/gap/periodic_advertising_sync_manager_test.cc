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

#include "pw_bluetooth_sapphire/internal/host/gap/periodic_advertising_sync_manager.h"

#include "pw_bluetooth_sapphire/internal/host/hci/extended_low_energy_scanner.h"
#include "pw_bluetooth_sapphire/internal/host/hci/fake_local_address_delegate.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_controller.h"

namespace bt::gap {
using hci::Error;

using SyncOptions = PeriodicAdvertisingSyncManager::SyncOptions;
using SyncParameters = PeriodicAdvertisingSyncManager::SyncParameters;
using BroadcastIsochronousGroupInfo =
    PeriodicAdvertisingSyncManager::BroadcastIsochronousGroupInfo;
using FakePeer = testing::FakePeer;
using PeriodicAdvertisingSync =
    testing::FakeController::PeriodicAdvertisingSync;

constexpr uint8_t kAdvSid1 = 8;
constexpr uint8_t kAdvSid2 = 9;
constexpr hci_spec::ConnectionHandle kConnectionHandle = 0x0007;
constexpr uint16_t kServiceData = 0x0908;

constexpr hci_spec::BroadcastIsochronousGroupInfo kBigInfo{
    .num_bis = 0x01,
    .nse = 0x02,
    .iso_interval = 0x03,
    .bn = 0x04,
    .pto = 0x05,
    .irc = 0x06,
    .max_pdu = 0x07,
    .sdu_interval = 0x08,
    .max_sdu = 0x09,
    .phy = pw::bluetooth::emboss::IsoPhyType::LE_1M,
    .framing = pw::bluetooth::emboss::BigFraming::FRAMED,
    .encryption = true};

class SyncDelegate : public PeriodicAdvertisingSyncManager::Delegate {
 public:
  const std::vector<std::pair<hci::SyncId, SyncParameters>>&
  on_sync_established_events() {
    return on_sync_established_;
  }

  const std::vector<std::pair<hci::SyncId, PeriodicAdvertisingReport>>&
  advertising_reports() const {
    return advertising_reports_;
  }

  const std::vector<std::pair<hci::SyncId, BroadcastIsochronousGroupInfo>>&
  big_info_reports() const {
    return big_info_reports_;
  }

  const std::vector<std::pair<hci::SyncId, hci::Error>>& sync_lost_events()
      const {
    return sync_lost_events_;
  }

 private:
  void OnSyncEstablished(hci::SyncId sync_id, SyncParameters params) override {
    on_sync_established_.emplace_back(sync_id, params);
  }
  void OnSyncLost(hci::SyncId sync_id, hci::Error error) override {
    sync_lost_events_.emplace_back(sync_id, error);
  }
  void OnAdvertisingReport(hci::SyncId sync_id,
                           const PeriodicAdvertisingReport& report) override {
    PeriodicAdvertisingReport report_copy;
    report.data.Copy(&report_copy.data);
    report_copy.event_counter = report.event_counter;
    report_copy.rssi = report.rssi;
    advertising_reports_.emplace_back(sync_id, std::move(report_copy));
  }
  void OnBigInfoReport(hci::SyncId sync_id,
                       const BroadcastIsochronousGroupInfo& info) override {
    big_info_reports_.emplace_back(sync_id, info);
  }

  std::vector<std::pair<hci::SyncId, SyncParameters>> on_sync_established_;
  std::vector<std::pair<hci::SyncId, PeriodicAdvertisingReport>>
      advertising_reports_;
  std::vector<std::pair<hci::SyncId, BroadcastIsochronousGroupInfo>>
      big_info_reports_;
  std::vector<std::pair<hci::SyncId, hci::Error>> sync_lost_events_;
};

using TestingBase =
    testing::FakeDispatcherControllerTest<testing::FakeController>;
class PeriodicAdvertisingSyncManagerTest : public TestingBase {
 public:
  void SetUp() override {
    TestingBase::SetUp();

    test_device()->set_scan_state_callback(
        [this](bool enabled) { scan_states_.push_back(enabled); });

    hci::AdvertisingPacketFilter::Config packet_filter_config(
        /*offloading_enabled=*/false,
        /*max_filters=*/0,
        /*peer_delivery_mode=*/
        hci::AdvertisingPacketFilter::Config::DeliveryMode::kImmediate);
    scanner_.emplace(&address_delegate_,
                     packet_filter_config,
                     transport()->GetWeakPtr(),
                     dispatcher());
    discovery_manager_.emplace(
        &scanner_.value(), &peer_cache_, packet_filter_config, dispatcher());
    sync_manager_.emplace(transport()->GetWeakPtr(),
                          peer_cache_,
                          discovery_manager_->GetWeakPtr(),
                          dispatcher());
  }

  void TearDown() override {
    sync_manager_.reset();
    TestingBase::TearDown();
  }

  PeriodicAdvertisingSyncManager& sync_manager() {
    return sync_manager_.value();
  }

  PeerCache& peer_cache() { return peer_cache_; }

  const std::vector<bool>& scan_states() const { return scan_states_; }

 private:
  std::vector<bool> scan_states_;
  PeerCache peer_cache_{dispatcher()};
  hci::FakeLocalAddressDelegate address_delegate_{dispatcher()};
  std::optional<hci::ExtendedLowEnergyScanner> scanner_;
  std::optional<LowEnergyDiscoveryManager> discovery_manager_;
  std::optional<PeriodicAdvertisingSyncManager> sync_manager_;
};

TEST_F(PeriodicAdvertisingSyncManagerTest, InvalidPeerId) {
  PeerId peer_id(5);
  SyncDelegate delegate;
  hci::Result<PeriodicAdvertisingSyncHandle> result =
      sync_manager().CreateSync(peer_id,
                                /*advertising_sid=*/1,
                                SyncOptions{.filter_duplicates = true},
                                delegate);
  ASSERT_TRUE(result.is_error());
  EXPECT_EQ(result.error_value(), hci::Error(HostError::kInvalidParameters));
}

TEST_F(PeriodicAdvertisingSyncManagerTest, SyncEstablishedAndTerminated) {
  SyncDelegate delegate;
  SyncOptions options{.filter_duplicates = true};
  DeviceAddress address(DeviceAddress::Type::kLEPublic, {1});
  const std::unordered_set<UUID> kUuids = {UUID(static_cast<uint16_t>(0x180d))};

  auto fake_peer = std::make_unique<testing::FakePeer>(address, dispatcher());
  const StaticByteBuffer kAdvData(
      // Complete 16-bit service UUID
      0x03,
      0x03,
      0x0d,
      0x18);
  fake_peer->AddPeriodicAdvertisement(
      kAdvSid1, DynamicByteBuffer(kAdvData), kBigInfo);
  test_device()->AddPeer(std::move(fake_peer));

  Peer* peer = peer_cache().NewPeer(address, /*connectable=*/false);

  hci::Result<PeriodicAdvertisingSyncHandle> sync_result =
      sync_manager().CreateSync(
          peer->identifier(), kAdvSid1, options, delegate);
  ASSERT_TRUE(sync_result.is_ok());

  RunUntilIdle();
  std::vector<PeriodicAdvertisingSync> syncs =
      test_device()->periodic_advertising_syncs();
  ASSERT_EQ(syncs.size(), 1u);
  EXPECT_EQ(syncs[0].peer_address, address);
  EXPECT_EQ(syncs[0].advertising_sid, kAdvSid1);
  EXPECT_EQ(syncs[0].duplicate_filtering, options.filter_duplicates);

  ASSERT_EQ(delegate.on_sync_established_events().size(), 1u);
  EXPECT_EQ(delegate.on_sync_established_events()[0].first, sync_result->id());

  ASSERT_EQ(delegate.advertising_reports().size(), 1u);
  EXPECT_EQ(delegate.advertising_reports()[0].first, sync_result->id());
  std::unordered_set<UUID> service_uuids =
      delegate.advertising_reports()[0].second.data.service_uuids();

  EXPECT_EQ(service_uuids, kUuids);

  EXPECT_EQ(delegate.advertising_reports()[0].second.event_counter, 0u);

  ASSERT_EQ(delegate.big_info_reports().size(), 1u);
  EXPECT_EQ(delegate.big_info_reports()[0].second, kBigInfo);
  std::vector<bool> expected_scan_states = {true, false};
  EXPECT_EQ(scan_states(), expected_scan_states);

  // Terminate sync
  sync_result->Cancel();
  RunUntilIdle();
  EXPECT_TRUE(test_device()->periodic_advertising_syncs().empty());
}

TEST_F(PeriodicAdvertisingSyncManagerTest, SyncEstablishedAndLost) {
  SyncDelegate delegate;
  SyncOptions options{.filter_duplicates = true};
  DeviceAddress address(DeviceAddress::Type::kLEPublic, {1});

  auto fake_peer = std::make_unique<testing::FakePeer>(address, dispatcher());
  const StaticByteBuffer kAdvData(
      // Complete 16-bit service UUID
      0x03,
      0x03,
      0x0d,
      0x18);
  fake_peer->AddPeriodicAdvertisement(
      kAdvSid1, DynamicByteBuffer(kAdvData), kBigInfo);
  test_device()->AddPeer(std::move(fake_peer));

  Peer* peer = peer_cache().NewPeer(address, /*connectable=*/false);

  hci::Result<PeriodicAdvertisingSyncHandle> sync_result =
      sync_manager().CreateSync(
          peer->identifier(), kAdvSid1, options, delegate);
  ASSERT_TRUE(sync_result.is_ok());

  RunUntilIdle();
  ASSERT_EQ(test_device()->periodic_advertising_syncs().size(), 1u);
  ASSERT_EQ(delegate.on_sync_established_events().size(), 1u);

  test_device()->LosePeriodicSync(address, kAdvSid1);
  RunUntilIdle();
  ASSERT_EQ(delegate.sync_lost_events().size(), 1u);
  EXPECT_EQ(delegate.sync_lost_events()[0].first, sync_result->id());
  EXPECT_EQ(delegate.sync_lost_events()[0].second,
            ToResult(pw::bluetooth::emboss::StatusCode::CONNECTION_TIMEOUT)
                .error_value());
  EXPECT_TRUE(test_device()->periodic_advertising_syncs().empty());
  sync_result->Cancel();
  RunUntilIdle();
}

TEST_F(PeriodicAdvertisingSyncManagerTest, CreateSyncForEstablishedSync) {
  SyncDelegate delegate;
  SyncOptions options1{.filter_duplicates = true};
  SyncOptions options2{.filter_duplicates = false};
  DeviceAddress address(DeviceAddress::Type::kLEPublic, {1});
  const std::unordered_set<UUID> kUuids = {UUID(static_cast<uint16_t>(0x180d))};

  auto fake_peer = std::make_unique<testing::FakePeer>(address, dispatcher());
  const StaticByteBuffer kAdvData(
      // Complete 16-bit service UUID
      0x03,
      0x03,
      0x0d,
      0x18);
  fake_peer->AddPeriodicAdvertisement(
      kAdvSid1, DynamicByteBuffer(kAdvData), kBigInfo);
  test_device()->AddPeer(std::move(fake_peer));

  Peer* peer = peer_cache().NewPeer(address, /*connectable=*/false);

  hci::Result<PeriodicAdvertisingSyncHandle> sync_result_1 =
      sync_manager().CreateSync(
          peer->identifier(), kAdvSid1, options1, delegate);
  ASSERT_TRUE(sync_result_1.is_ok());

  RunUntilIdle();
  std::vector<PeriodicAdvertisingSync> syncs =
      test_device()->periodic_advertising_syncs();
  ASSERT_EQ(syncs.size(), 1u);
  ASSERT_EQ(delegate.on_sync_established_events().size(), 1u);
  EXPECT_EQ(delegate.on_sync_established_events()[0].first,
            sync_result_1->id());

  hci::Result<PeriodicAdvertisingSyncHandle> sync_result_2 =
      sync_manager().CreateSync(
          peer->identifier(), kAdvSid1, options2, delegate);
  ASSERT_TRUE(sync_result_2.is_ok());
  EXPECT_EQ(sync_result_1->id(), sync_result_2->id());
  RunUntilIdle();
  ASSERT_EQ(delegate.on_sync_established_events().size(), 2u);
  EXPECT_EQ(delegate.on_sync_established_events()[1].first,
            sync_result_2->id());
}

TEST_F(PeriodicAdvertisingSyncManagerTest,
       CreateSyncTwiceForSameAdvertisementWithSameDelegateAndDifferentOptions) {
  SyncDelegate delegate;
  SyncOptions options1{.filter_duplicates = true};
  SyncOptions options2{.filter_duplicates = false};
  DeviceAddress address(DeviceAddress::Type::kLEPublic, {1});
  const std::unordered_set<UUID> kUuids = {UUID(static_cast<uint16_t>(0x180d))};

  auto fake_peer = std::make_unique<testing::FakePeer>(address, dispatcher());
  const StaticByteBuffer kAdvData(
      // Complete 16-bit service UUID
      0x03,
      0x03,
      0x0d,
      0x18);
  fake_peer->AddPeriodicAdvertisement(
      kAdvSid1, DynamicByteBuffer(kAdvData), kBigInfo);
  test_device()->AddPeer(std::move(fake_peer));

  Peer* peer = peer_cache().NewPeer(address, /*connectable=*/false);

  hci::Result<PeriodicAdvertisingSyncHandle> sync_result_1 =
      sync_manager().CreateSync(
          peer->identifier(), kAdvSid1, options1, delegate);
  ASSERT_TRUE(sync_result_1.is_ok());

  hci::Result<PeriodicAdvertisingSyncHandle> sync_result_2 =
      sync_manager().CreateSync(
          peer->identifier(), kAdvSid1, options2, delegate);
  ASSERT_TRUE(sync_result_2.is_ok());
  EXPECT_EQ(sync_result_1->id(), sync_result_2->id());

  RunUntilIdle();
  std::vector<PeriodicAdvertisingSync> syncs =
      test_device()->periodic_advertising_syncs();
  ASSERT_EQ(syncs.size(), 1u);
  EXPECT_EQ(syncs[0].peer_address, address);
  EXPECT_EQ(syncs[0].advertising_sid, kAdvSid1);
  EXPECT_EQ(syncs[0].duplicate_filtering, options1.filter_duplicates);

  ASSERT_EQ(delegate.on_sync_established_events().size(), 1u);
  EXPECT_EQ(delegate.on_sync_established_events()[0].first,
            sync_result_1->id());

  ASSERT_EQ(delegate.advertising_reports().size(), 1u);
  EXPECT_EQ(delegate.advertising_reports()[0].first, sync_result_1->id());
  std::unordered_set<UUID> service_uuids =
      delegate.advertising_reports()[0].second.data.service_uuids();

  EXPECT_EQ(service_uuids, kUuids);

  EXPECT_EQ(delegate.advertising_reports()[0].second.event_counter, 0u);

  ASSERT_EQ(delegate.big_info_reports().size(), 1u);
  EXPECT_EQ(delegate.big_info_reports()[0].second, kBigInfo);
  std::vector<bool> expected_scan_states = {true, false};
  EXPECT_EQ(scan_states(), expected_scan_states);

  // Terminate sync
  sync_result_1->Cancel();
  RunUntilIdle();
  EXPECT_FALSE(test_device()->periodic_advertising_syncs().empty());

  sync_result_2->Cancel();
  RunUntilIdle();
  EXPECT_TRUE(test_device()->periodic_advertising_syncs().empty());
}

TEST_F(PeriodicAdvertisingSyncManagerTest,
       TwoSyncsForDifferentAdvertisementsWithDifferentDelegates) {
  SyncDelegate delegate1;
  SyncDelegate delegate2;
  SyncOptions options{.filter_duplicates = true};
  DeviceAddress address(DeviceAddress::Type::kLEPublic, {1});
  const std::unordered_set<UUID> kUuids1 = {
      UUID(static_cast<uint16_t>(0x180d))};
  const std::unordered_set<UUID> kUuids2 = {
      UUID(static_cast<uint16_t>(0x190d))};

  auto fake_peer = std::make_unique<testing::FakePeer>(address, dispatcher());
  const StaticByteBuffer kAdvData1(
      // Complete 16-bit service UUID
      0x03,
      0x03,
      0x0d,
      0x18);
  const StaticByteBuffer kAdvData2(
      // Complete 16-bit service UUID
      0x03,
      0x03,
      0x0d,
      0x19);
  fake_peer->AddPeriodicAdvertisement(
      kAdvSid1, DynamicByteBuffer(kAdvData1), kBigInfo);
  fake_peer->AddPeriodicAdvertisement(
      kAdvSid2, DynamicByteBuffer(kAdvData2), kBigInfo);
  test_device()->AddPeer(std::move(fake_peer));

  Peer* peer = peer_cache().NewPeer(address, /*connectable=*/false);

  hci::Result<PeriodicAdvertisingSyncHandle> sync_result_1 =
      sync_manager().CreateSync(
          peer->identifier(), kAdvSid1, options, delegate1);
  ASSERT_TRUE(sync_result_1.is_ok());

  hci::Result<PeriodicAdvertisingSyncHandle> sync_result_2 =
      sync_manager().CreateSync(
          peer->identifier(), kAdvSid2, options, delegate2);
  ASSERT_TRUE(sync_result_2.is_ok());

  RunUntilIdle();

  std::vector<PeriodicAdvertisingSync> syncs =
      test_device()->periodic_advertising_syncs();
  ASSERT_EQ(syncs.size(), 2u);
  std::sort(syncs.begin(), syncs.end(), [](auto& a, auto& b) {
    return a.advertising_sid < b.advertising_sid;
  });
  EXPECT_EQ(syncs[0].peer_address, address);
  EXPECT_EQ(syncs[0].advertising_sid, kAdvSid1);
  EXPECT_EQ(syncs[0].duplicate_filtering, options.filter_duplicates);
  EXPECT_EQ(syncs[1].peer_address, address);
  EXPECT_EQ(syncs[1].advertising_sid, kAdvSid2);
  EXPECT_EQ(syncs[1].duplicate_filtering, options.filter_duplicates);

  ASSERT_EQ(delegate1.on_sync_established_events().size(), 1u);
  EXPECT_EQ(delegate1.on_sync_established_events()[0].first,
            sync_result_1->id());
  ASSERT_EQ(delegate2.on_sync_established_events().size(), 1u);
  EXPECT_EQ(delegate2.on_sync_established_events()[0].first,
            sync_result_2->id());

  ASSERT_EQ(delegate1.advertising_reports().size(), 1u);
  EXPECT_EQ(delegate1.advertising_reports()[0].first, sync_result_1->id());
  std::unordered_set<UUID> service_uuids1 =
      delegate1.advertising_reports()[0].second.data.service_uuids();
  EXPECT_EQ(service_uuids1, kUuids1);
  EXPECT_EQ(delegate1.advertising_reports()[0].second.event_counter, 0u);
  ASSERT_EQ(delegate1.big_info_reports().size(), 1u);
  EXPECT_EQ(delegate1.big_info_reports()[0].second, kBigInfo);

  ASSERT_EQ(delegate2.advertising_reports().size(), 1u);
  EXPECT_EQ(delegate2.advertising_reports()[0].first, sync_result_2->id());
  std::unordered_set<UUID> service_uuids2 =
      delegate2.advertising_reports()[0].second.data.service_uuids();
  EXPECT_EQ(service_uuids2, kUuids2);
  EXPECT_EQ(delegate2.advertising_reports()[0].second.event_counter, 0u);
  ASSERT_EQ(delegate2.big_info_reports().size(), 1u);
  EXPECT_EQ(delegate2.big_info_reports()[0].second, kBigInfo);

  std::vector<bool> expected_scan_states = {true, false};
  EXPECT_EQ(scan_states(), expected_scan_states);

  // Terminate sync
  sync_result_1->Cancel();
  RunUntilIdle();
  EXPECT_EQ(test_device()->periodic_advertising_syncs().size(), 1u);

  sync_result_2->Cancel();
  RunUntilIdle();
  EXPECT_TRUE(test_device()->periodic_advertising_syncs().empty());
}

TEST_F(PeriodicAdvertisingSyncManagerTest, BrEdrAddressFixedToLEPublic) {
  SyncDelegate delegate;
  SyncOptions options{.filter_duplicates = true};
  DeviceAddress bredr_address(DeviceAddress::Type::kBREDR, {1});
  DeviceAddress le_address(DeviceAddress::Type::kBREDR, {1});

  auto fake_peer =
      std::make_unique<testing::FakePeer>(le_address, dispatcher());
  const StaticByteBuffer kAdvData(
      // Complete 16-bit service UUID
      0x03,
      0x03,
      0x0d,
      0x18);
  fake_peer->AddPeriodicAdvertisement(
      kAdvSid1, DynamicByteBuffer(kAdvData), kBigInfo);
  test_device()->AddPeer(std::move(fake_peer));

  Peer* peer = peer_cache().NewPeer(bredr_address, /*connectable=*/false);
  peer->MutLe();

  hci::Result<PeriodicAdvertisingSyncHandle> sync_result =
      sync_manager().CreateSync(
          peer->identifier(), kAdvSid1, options, delegate);
  ASSERT_TRUE(sync_result.is_ok());

  RunUntilIdle();
  std::vector<PeriodicAdvertisingSync> syncs =
      test_device()->periodic_advertising_syncs();
  ASSERT_EQ(syncs.size(), 1u);
  EXPECT_EQ(syncs[0].peer_address, le_address);

  sync_result->Cancel();
  RunUntilIdle();
}

TEST_F(PeriodicAdvertisingSyncManagerTest, BrEdrAddressRejected) {
  SyncDelegate delegate;
  SyncOptions options{.filter_duplicates = true};
  DeviceAddress bredr_address(DeviceAddress::Type::kBREDR, {1});
  Peer* peer = peer_cache().NewPeer(bredr_address, /*connectable=*/false);

  hci::Result<PeriodicAdvertisingSyncHandle> sync_result =
      sync_manager().CreateSync(
          peer->identifier(), kAdvSid1, options, delegate);
  ASSERT_TRUE(sync_result.is_error());
  EXPECT_EQ(sync_result.error_value(), Error(HostError::kInvalidParameters));
}

TEST_F(PeriodicAdvertisingSyncManagerTest, CancelPendingSyncStopsScan) {
  SyncDelegate delegate;
  SyncOptions options{.filter_duplicates = true};
  DeviceAddress address(DeviceAddress::Type::kLEPublic, {1});

  auto fake_peer = std::make_unique<testing::FakePeer>(address, dispatcher());
  const StaticByteBuffer kAdvData(
      // Complete 16-bit service UUID
      0x03,
      0x03,
      0x0d,
      0x18);
  fake_peer->AddPeriodicAdvertisement(
      kAdvSid1, DynamicByteBuffer(kAdvData), kBigInfo);
  test_device()->AddPeer(std::move(fake_peer));

  Peer* peer = peer_cache().NewPeer(address, /*connectable=*/false);

  hci::Result<PeriodicAdvertisingSyncHandle> sync_result =
      sync_manager().CreateSync(
          peer->identifier(), kAdvSid1, options, delegate);
  ASSERT_TRUE(sync_result.is_ok());

  sync_result->Cancel();
  RunUntilIdle();
  std::vector<PeriodicAdvertisingSync> syncs =
      test_device()->periodic_advertising_syncs();
  ASSERT_EQ(syncs.size(), 0u);

  ASSERT_EQ(delegate.on_sync_established_events().size(), 0u);

  std::vector<bool> expected_scan_states = {true, false};
  EXPECT_EQ(scan_states(), expected_scan_states);
}

TEST_F(PeriodicAdvertisingSyncManagerTest, TransferSync) {
  SyncDelegate delegate;
  SyncOptions options{.filter_duplicates = true};
  const std::unordered_set<UUID> kUuids = {UUID(static_cast<uint16_t>(0x180d))};

  // Peer 0 is the advertising device
  DeviceAddress address_0(DeviceAddress::Type::kLEPublic, {1});
  auto fake_peer_0 =
      std::make_unique<testing::FakePeer>(address_0, dispatcher());
  const StaticByteBuffer kAdvData(
      // Complete 16-bit service UUID
      0x03,
      0x03,
      0x0d,
      0x18);
  fake_peer_0->AddPeriodicAdvertisement(
      kAdvSid1, DynamicByteBuffer(kAdvData), kBigInfo);
  test_device()->AddPeer(std::move(fake_peer_0));
  Peer* peer_0 = peer_cache().NewPeer(address_0, /*connectable=*/false);

  // Peer 1 is the PAST Receiver.
  DeviceAddress address_1(DeviceAddress::Type::kLEPublic, {2});
  auto fake_peer_1 =
      std::make_unique<testing::FakePeer>(address_1, dispatcher());
  FakePeer* fake_peer_ptr_1 = fake_peer_1.get();
  // Simulate a connection to peer 1 so that FakeController can look up the peer
  // by connection handle.
  fake_peer_1->AddLink(kConnectionHandle);
  test_device()->AddPeer(std::move(fake_peer_1));

  hci::Result<PeriodicAdvertisingSyncHandle> sync_result =
      sync_manager().CreateSync(
          peer_0->identifier(), kAdvSid1, options, delegate);
  ASSERT_TRUE(sync_result.is_ok());

  RunUntilIdle();
  std::vector<PeriodicAdvertisingSync> syncs =
      test_device()->periodic_advertising_syncs();
  ASSERT_EQ(syncs.size(), 1u);

  std::optional<hci::Result<>> transfer_result;
  auto callback = [&transfer_result](hci::Result<> result) {
    transfer_result = result;
  };
  sync_manager().TransferSync(
      sync_result->id(), kConnectionHandle, kServiceData, std::move(callback));
  RunUntilIdle();
  ASSERT_TRUE(transfer_result.has_value());
  EXPECT_TRUE(transfer_result->is_ok());

  std::optional<FakePeer::PeriodicAdvertisingSyncTransfer> transfer =
      fake_peer_ptr_1->FindPeriodicAdvertisingSyncTransfer(address_0, kAdvSid1);
  ASSERT_TRUE(transfer.has_value());
  EXPECT_EQ(transfer->service_data, kServiceData);

  sync_result->Cancel();
  RunUntilIdle();
}

}  // namespace bt::gap
