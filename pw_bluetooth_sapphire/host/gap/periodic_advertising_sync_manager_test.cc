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

  void set_on_sync_lost_callback(fit::closure cb) {
    on_sync_lost_cb_ = std::move(cb);
  }

 private:
  void OnSyncEstablished(hci::SyncId sync_id, SyncParameters params) override {
    on_sync_established_.emplace_back(sync_id, params);
  }
  void OnSyncLost(hci::SyncId sync_id, hci::Error error) override {
    sync_lost_events_.emplace_back(sync_id, error);
    if (on_sync_lost_cb_) {
      on_sync_lost_cb_();
    }
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
  fit::closure on_sync_lost_cb_;
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

  LowEnergyDiscoveryManager& discovery_manager() {
    return discovery_manager_.value();
  }

  void ResetSyncManager() { sync_manager_.reset(); }

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

TEST_F(PeriodicAdvertisingSyncManagerTest,
       DestroySyncManagerWithPendingAndEstablishedSyncs) {
  SyncDelegate pending_delegate;
  SyncDelegate established_delegate;
  SyncOptions options{.filter_duplicates = true};

  // Peer 1 has a periodic advertisement so its sync establishes; Peer 2 has no
  // controller peer so its sync stays pending in `syncs_`.
  DeviceAddress addr_established(DeviceAddress::Type::kLEPublic, {1});
  auto fake_peer =
      std::make_unique<testing::FakePeer>(addr_established, dispatcher());
  const StaticByteBuffer kAdvData(0x03, 0x03, 0x0d, 0x18);
  fake_peer->AddPeriodicAdvertisement(
      kAdvSid1, DynamicByteBuffer(kAdvData), kBigInfo);
  test_device()->AddPeer(std::move(fake_peer));
  Peer* peer_established =
      peer_cache().NewPeer(addr_established, /*connectable=*/false);

  DeviceAddress addr_pending(DeviceAddress::Type::kLEPublic, {2});
  Peer* peer_pending =
      peer_cache().NewPeer(addr_pending, /*connectable=*/false);
  peer_pending->MutLe();

  hci::Result<PeriodicAdvertisingSyncHandle> established_handle =
      sync_manager().CreateSync(peer_established->identifier(),
                                kAdvSid1,
                                options,
                                established_delegate);
  ASSERT_TRUE(established_handle.is_ok());
  RunUntilIdle();
  ASSERT_EQ(1u, established_delegate.on_sync_established_events().size());

  hci::Result<PeriodicAdvertisingSyncHandle> pending_handle =
      sync_manager().CreateSync(
          peer_pending->identifier(), kAdvSid2, options, pending_delegate);
  ASSERT_TRUE(pending_handle.is_ok());
  RunUntilIdle();
  EXPECT_TRUE(pending_delegate.on_sync_established_events().empty());

  // Destroying `sync_manager_` while client handles are still alive must
  // cleanly drain `syncs_` without re-entrant use-after-free or double-free,
  // and without firing spurious OnSyncLost notifications during teardown.
  ResetSyncManager();
  RunUntilIdle();
  EXPECT_TRUE(pending_delegate.sync_lost_events().empty());
  EXPECT_TRUE(established_delegate.sync_lost_events().empty());
  EXPECT_TRUE(test_device()->periodic_advertising_syncs().empty());
  std::vector<bool> expected_scan_states = {true, false, true, false};
  EXPECT_EQ(scan_states(), expected_scan_states);

  // Releasing handles after manager destruction must be a safe no-op.
  pending_handle->Cancel();
  established_handle->Cancel();
  RunUntilIdle();
}

TEST_F(PeriodicAdvertisingSyncManagerTest,
       ScanStartFailureNotifiesPendingSyncAndRecoversOnRetry) {
  SyncDelegate delegate1;
  SyncOptions options{.filter_duplicates = true};
  DeviceAddress address(DeviceAddress::Type::kLEPublic, {1});

  auto fake_peer = std::make_unique<testing::FakePeer>(address, dispatcher());
  const StaticByteBuffer kAdvData(0x03, 0x03, 0x0d, 0x18);
  fake_peer->AddPeriodicAdvertisement(
      kAdvSid1, DynamicByteBuffer(kAdvData), kBigInfo);
  test_device()->AddPeer(std::move(fake_peer));
  Peer* peer = peer_cache().NewPeer(address, /*connectable=*/false);

  // Cause LE scan start to fail on the controller.
  test_device()->SetDefaultResponseStatus(
      hci_spec::kLESetExtendedScanEnable,
      pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED);

  hci::Result<PeriodicAdvertisingSyncHandle> handle1 =
      sync_manager().CreateSync(
          peer->identifier(), kAdvSid1, options, delegate1);
  ASSERT_TRUE(handle1.is_ok());
  RunUntilIdle();

  ASSERT_EQ(1u, delegate1.sync_lost_events().size());
  EXPECT_EQ(handle1->id(), delegate1.sync_lost_events()[0].first);
  EXPECT_EQ(Error(HostError::kFailed), delegate1.sync_lost_events()[0].second);
  EXPECT_TRUE(test_device()->periodic_advertising_syncs().empty());

  // Clear the transient controller scan error and verify a subsequent
  // CreateSync for a distinct peer/SID starts LE discovery and establishes
  // periodic sync cleanly (proving the failed HCI sync was canceled).
  test_device()->ClearDefaultResponseStatus(hci_spec::kLESetExtendedScanEnable);

  DeviceAddress address2(DeviceAddress::Type::kLEPublic, {2});
  auto fake_peer2 = std::make_unique<testing::FakePeer>(address2, dispatcher());
  fake_peer2->AddPeriodicAdvertisement(
      kAdvSid2, DynamicByteBuffer(kAdvData), kBigInfo);
  test_device()->AddPeer(std::move(fake_peer2));
  Peer* peer2 = peer_cache().NewPeer(address2, /*connectable=*/false);

  SyncDelegate delegate2;
  hci::Result<PeriodicAdvertisingSyncHandle> handle2 =
      sync_manager().CreateSync(
          peer2->identifier(), kAdvSid2, options, delegate2);
  ASSERT_TRUE(handle2.is_ok());
  RunUntilIdle();

  ASSERT_EQ(1u, delegate2.on_sync_established_events().size());
  EXPECT_EQ(handle2->id(), delegate2.on_sync_established_events()[0].first);
  handle2->Cancel();
  RunUntilIdle();
}

TEST_F(PeriodicAdvertisingSyncManagerTest,
       ScanStartFailurePreservesAlreadyEstablishedSync) {
  SyncDelegate established_delegate;
  SyncDelegate pending_delegate;
  SyncOptions options{.filter_duplicates = true};

  DeviceAddress addr1(DeviceAddress::Type::kLEPublic, {1});
  auto fake_peer = std::make_unique<testing::FakePeer>(addr1, dispatcher());
  const StaticByteBuffer kAdvData(0x03, 0x03, 0x0d, 0x18);
  fake_peer->AddPeriodicAdvertisement(
      kAdvSid1, DynamicByteBuffer(kAdvData), kBigInfo);
  test_device()->AddPeer(std::move(fake_peer));
  Peer* peer1 = peer_cache().NewPeer(addr1, /*connectable=*/false);

  hci::Result<PeriodicAdvertisingSyncHandle> established_handle =
      sync_manager().CreateSync(
          peer1->identifier(), kAdvSid1, options, established_delegate);
  ASSERT_TRUE(established_handle.is_ok());
  RunUntilIdle();
  ASSERT_EQ(1u, established_delegate.on_sync_established_events().size());

  // Now fail scan start for a new pending sync and ensure the established sync
  // is unaffected.
  DeviceAddress addr2(DeviceAddress::Type::kLEPublic, {2});
  Peer* peer2 = peer_cache().NewPeer(addr2, /*connectable=*/false);
  peer2->MutLe();

  test_device()->SetDefaultResponseStatus(
      hci_spec::kLESetExtendedScanEnable,
      pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED);

  hci::Result<PeriodicAdvertisingSyncHandle> pending_handle =
      sync_manager().CreateSync(
          peer2->identifier(), kAdvSid2, options, pending_delegate);
  ASSERT_TRUE(pending_handle.is_ok());
  RunUntilIdle();

  EXPECT_EQ(1u, pending_delegate.sync_lost_events().size());
  EXPECT_TRUE(established_delegate.sync_lost_events().empty());
  EXPECT_EQ(1u, test_device()->periodic_advertising_syncs().size());

  established_handle->Cancel();
  RunUntilIdle();
}

TEST_F(PeriodicAdvertisingSyncManagerTest,
       ActiveScanSessionErrorNotifiesPendingSyncAndRecoversOnRetry) {
  SyncDelegate delegate1;
  SyncOptions options{.filter_duplicates = true};
  DeviceAddress address(DeviceAddress::Type::kLEPublic, {1});

  Peer* peer = peer_cache().NewPeer(address, /*connectable=*/false);
  peer->MutLe();

  // Start a pending sync while controller scanning succeeds so
  // `discovery_session_` holds an active `LowEnergyDiscoverySession`.
  hci::Result<PeriodicAdvertisingSyncHandle> handle1 =
      sync_manager().CreateSync(
          peer->identifier(), kAdvSid1, options, delegate1);
  ASSERT_TRUE(handle1.is_ok());
  RunUntilIdle();
  EXPECT_TRUE(delegate1.sync_lost_events().empty());

  // Inject an HCI scan parameter failure for the next scan period restart so
  // stopping the current period succeeds and restarting fails, firing
  // `LowEnergyDiscoverySession::set_error_callback` -> `OnDiscoveryFailure`.
  test_device()->SetDefaultResponseStatus(
      hci_spec::kLESetExtendedScanParameters,
      pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED);
  RunFor(kLEGeneralDiscoveryScanMin);

  ASSERT_EQ(1u, delegate1.sync_lost_events().size());
  EXPECT_EQ(handle1->id(), delegate1.sync_lost_events()[0].first);
  EXPECT_EQ(Error(HostError::kFailed), delegate1.sync_lost_events()[0].second);

  // Clear the error, add the periodic advertiser, and verify a subsequent
  // CreateSync starts a new discovery session and establishes sync cleanly.
  test_device()->ClearDefaultResponseStatus(
      hci_spec::kLESetExtendedScanParameters);
  auto fake_peer = std::make_unique<testing::FakePeer>(address, dispatcher());
  const StaticByteBuffer kAdvData(0x03, 0x03, 0x0d, 0x18);
  fake_peer->AddPeriodicAdvertisement(
      kAdvSid1, DynamicByteBuffer(kAdvData), kBigInfo);
  test_device()->AddPeer(std::move(fake_peer));

  SyncDelegate delegate2;
  hci::Result<PeriodicAdvertisingSyncHandle> handle2 =
      sync_manager().CreateSync(
          peer->identifier(), kAdvSid1, options, delegate2);
  ASSERT_TRUE(handle2.is_ok());
  RunUntilIdle();

  ASSERT_EQ(1u, delegate2.on_sync_established_events().size());
  EXPECT_EQ(handle2->id(), delegate2.on_sync_established_events()[0].first);
  handle2->Cancel();
  RunUntilIdle();
}

TEST_F(PeriodicAdvertisingSyncManagerTest,
       RetryCreateSyncInsideOnSyncLostAfterScanFailureSucceeds) {
  SyncDelegate delegate1;
  SyncDelegate delegate2;
  SyncOptions options{.filter_duplicates = true};
  DeviceAddress address(DeviceAddress::Type::kLEPublic, {1});

  auto fake_peer = std::make_unique<testing::FakePeer>(address, dispatcher());
  const StaticByteBuffer kAdvData(0x03, 0x03, 0x0d, 0x18);
  fake_peer->AddPeriodicAdvertisement(
      kAdvSid1, DynamicByteBuffer(kAdvData), kBigInfo);
  test_device()->AddPeer(std::move(fake_peer));
  Peer* peer = peer_cache().NewPeer(address, /*connectable=*/false);

  test_device()->SetDefaultResponseStatus(
      hci_spec::kLESetExtendedScanEnable,
      pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED);

  std::optional<PeriodicAdvertisingSyncHandle> retry_handle;
  delegate1.set_on_sync_lost_callback([&]() {
    test_device()->ClearDefaultResponseStatus(
        hci_spec::kLESetExtendedScanEnable);
    hci::Result<PeriodicAdvertisingSyncHandle> res = sync_manager().CreateSync(
        peer->identifier(), kAdvSid1, options, delegate2);
    ASSERT_TRUE(res.is_ok());
    retry_handle.emplace(std::move(res.value()));
  });

  hci::Result<PeriodicAdvertisingSyncHandle> handle1 =
      sync_manager().CreateSync(
          peer->identifier(), kAdvSid1, options, delegate1);
  ASSERT_TRUE(handle1.is_ok());
  RunUntilIdle();

  EXPECT_EQ(1u, delegate1.sync_lost_events().size());
  ASSERT_TRUE(retry_handle.has_value());
  ASSERT_EQ(1u, delegate2.on_sync_established_events().size());
  EXPECT_EQ(retry_handle->id(),
            delegate2.on_sync_established_events()[0].first);
  retry_handle->Cancel();
  RunUntilIdle();
}

TEST_F(PeriodicAdvertisingSyncManagerTest,
       CancelAndDestroySiblingDelegateInsideOnSyncLostSkipsCanceledDelegate) {
  SyncOptions options{.filter_duplicates = true};
  DeviceAddress addr1(DeviceAddress::Type::kLEPublic, {1});
  DeviceAddress addr2(DeviceAddress::Type::kLEPublic, {2});
  Peer* peer1 = peer_cache().NewPeer(addr1, /*connectable=*/false);
  peer1->MutLe();
  Peer* peer2 = peer_cache().NewPeer(addr2, /*connectable=*/false);
  peer2->MutLe();

  test_device()->SetDefaultResponseStatus(
      hci_spec::kLESetExtendedScanEnable,
      pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED);

  auto delegate1 = std::make_unique<SyncDelegate>();
  auto delegate2 = std::make_unique<SyncDelegate>();
  std::optional<PeriodicAdvertisingSyncHandle> handle1;
  std::optional<PeriodicAdvertisingSyncHandle> handle2;
  size_t notifications = 0;

  delegate1->set_on_sync_lost_callback([&]() {
    ++notifications;
    if (handle2.has_value()) {
      handle2->Cancel();
      handle2.reset();
      delegate2.reset();
    }
  });
  delegate2->set_on_sync_lost_callback([&]() {
    ++notifications;
    if (handle1.has_value()) {
      handle1->Cancel();
      handle1.reset();
      delegate1.reset();
    }
  });

  hci::Result<PeriodicAdvertisingSyncHandle> res1 = sync_manager().CreateSync(
      peer1->identifier(), kAdvSid1, options, *delegate1);
  ASSERT_TRUE(res1.is_ok());
  handle1.emplace(std::move(res1.value()));

  hci::Result<PeriodicAdvertisingSyncHandle> res2 = sync_manager().CreateSync(
      peer2->identifier(), kAdvSid2, options, *delegate2);
  ASSERT_TRUE(res2.is_ok());
  handle2.emplace(std::move(res2.value()));

  RunUntilIdle();

  EXPECT_EQ(1u, notifications);
  EXPECT_TRUE((delegate1 == nullptr) != (delegate2 == nullptr));
}

TEST_F(
    PeriodicAdvertisingSyncManagerTest,
    JoinActiveScanThatFailsBeforeSessionCallbackDeliversDeadSessionAndRecovers) {
  std::unique_ptr<LowEnergyDiscoverySession> active_session;
  discovery_manager().StartDiscovery(
      /*active=*/true, {}, [&](auto session) {
        active_session = std::move(session);
      });
  RunUntilIdle();
  ASSERT_NE(nullptr, active_session);
  ASSERT_TRUE(active_session->alive());

  test_device()->SetDefaultResponseStatus(
      hci_spec::kLESetExtendedScanParameters,
      pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED);
  fit::closure resume_scan_params;
  test_device()->pause_responses_for_opcode(
      hci_spec::kLESetExtendedScanParameters,
      [&](fit::closure resume) { resume_scan_params = std::move(resume); });

  RunFor(kLEGeneralDiscoveryScanMin);
  ASSERT_TRUE(resume_scan_params);
  ASSERT_TRUE(active_session->alive());

  DeviceAddress address(DeviceAddress::Type::kLEPublic, {1});
  Peer* peer = peer_cache().NewPeer(address, /*connectable=*/false);
  peer->MutLe();
  SyncDelegate delegate1;
  SyncOptions options{.filter_duplicates = true};

  // Resume the failing scan-parameter command first so its CommandComplete
  // failure is queued ahead of StartDiscovery's posted session callback.
  resume_scan_params();
  hci::Result<PeriodicAdvertisingSyncHandle> handle1 =
      sync_manager().CreateSync(
          peer->identifier(), kAdvSid1, options, delegate1);
  ASSERT_TRUE(handle1.is_ok());

  RunUntilIdle();
  EXPECT_FALSE(active_session->alive());
  ASSERT_EQ(1u, delegate1.sync_lost_events().size());
  EXPECT_EQ(handle1->id(), delegate1.sync_lost_events()[0].first);
  EXPECT_EQ(Error(HostError::kFailed), delegate1.sync_lost_events()[0].second);

  // Clear the controller failure and verify a subsequent CreateSync recovers.
  test_device()->pause_responses_for_opcode(
      hci_spec::kLESetExtendedScanParameters,
      [](fit::closure resume) { resume(); });
  test_device()->ClearDefaultResponseStatus(
      hci_spec::kLESetExtendedScanParameters);

  auto fake_peer = std::make_unique<testing::FakePeer>(address, dispatcher());
  const StaticByteBuffer kAdvData(0x03, 0x03, 0x0d, 0x18);
  fake_peer->AddPeriodicAdvertisement(
      kAdvSid1, DynamicByteBuffer(kAdvData), kBigInfo);
  test_device()->AddPeer(std::move(fake_peer));

  SyncDelegate delegate2;
  hci::Result<PeriodicAdvertisingSyncHandle> handle2 =
      sync_manager().CreateSync(
          peer->identifier(), kAdvSid1, options, delegate2);
  ASSERT_TRUE(handle2.is_ok());
  RunUntilIdle();

  ASSERT_EQ(1u, delegate2.on_sync_established_events().size());
  EXPECT_EQ(handle2->id(), delegate2.on_sync_established_events()[0].first);
  handle2->Cancel();
  RunUntilIdle();
}

}  // namespace bt::gap
