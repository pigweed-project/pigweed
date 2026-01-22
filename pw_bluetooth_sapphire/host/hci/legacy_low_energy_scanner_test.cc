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

#include "pw_bluetooth_sapphire/internal/host/hci/legacy_low_energy_scanner.h"

#include "pw_bluetooth/hci_events.emb.h"
#include "pw_bluetooth_sapphire/internal/host/hci/advertising_packet_filter.h"
#include "pw_bluetooth_sapphire/internal/host/hci/fake_local_address_delegate.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_peer.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"

namespace bt::hci {

using bt::testing::FakeController;
using bt::testing::FakePeer;
using TestingBase = bt::testing::FakeDispatcherControllerTest<FakeController>;

constexpr pw::chrono::SystemClock::duration kPwScanResponseTimeout =
    std::chrono::seconds(2);

const StaticByteBuffer kPlainAdvDataBytes(
    5, bt::DataType::kCompleteLocalName, 'T', 'e', 's', 't');
const StaticByteBuffer kPlainScanRspBytes(
    5, bt::DataType::kCompleteLocalName, 'D', 'a', 't', 'a');

const DeviceAddress kPublicAddr(DeviceAddress::Type::kLEPublic, {1});

class LegacyLowEnergyScannerTest : public TestingBase,
                                   public LowEnergyScanner::Delegate {
 public:
  LegacyLowEnergyScannerTest() = default;
  ~LegacyLowEnergyScannerTest() override = default;

 protected:
  void SetUp() override {
    TestingBase::SetUp();

    FakeController::Settings settings;
    settings.ApplyLegacyLEConfig();
    test_device()->set_settings(settings);

    scanner_ = std::make_unique<LegacyLowEnergyScanner>(
        fake_address_delegate(),
        AdvertisingPacketFilter::Config(
            false,
            0,
            AdvertisingPacketFilter::Config::DeliveryMode::kImmediate),
        transport()->GetWeakPtr(),
        dispatcher());
    scanner_->SetPacketFilters(0, {});
    scanner_->set_delegate(this);

    auto p = std::make_unique<FakePeer>(kPublicAddr,
                                        dispatcher(),
                                        /*scannable=*/true,
                                        /*send_advertising_report=*/true);
    p->set_use_extended_advertising_pdus(true);
    p->set_advertising_data(kPlainAdvDataBytes);
    p->set_scan_response(kPlainScanRspBytes);
    peers_.push_back(std::move(p));
  }

  void TearDown() override {
    scanner_ = nullptr;
    test_device()->Stop();
    TestingBase::TearDown();
  }

  bool StartScan(bool active,
                 pw::chrono::SystemClock::duration period =
                     LowEnergyScanner::kPeriodInfinite) {
    LowEnergyScanner::ScanOptions options{
        .active = active,
        .filter_duplicates = true,
        .period = period,
        .scan_response_timeout = kPwScanResponseTimeout};
    return scanner()->StartScan(options, [](auto) {});
  }

  using PeerFoundCallback = fit::function<void(const LowEnergyScanResult&)>;
  void set_peer_found_callback(PeerFoundCallback cb) {
    peer_found_cb_ = std::move(cb);
  }

  // LowEnergyScanner::Delegate override:
  void OnPeerFound(const std::unordered_set<uint16_t>& /*scan_ids*/,
                   const LowEnergyScanResult& result) override {
    if (peer_found_cb_) {
      peer_found_cb_(result);
    }
  }

  LowEnergyScanner* scanner() const { return scanner_.get(); }
  FakeLocalAddressDelegate* fake_address_delegate() {
    return &fake_address_delegate_;
  }

  static constexpr size_t event_prefix_size =
      pw::bluetooth::emboss::LEAdvertisingReportSubevent::MinSizeInBytes();
  static constexpr size_t report_prefix_size =
      pw::bluetooth::emboss::LEAdvertisingReportData::MinSizeInBytes();

 private:
  std::unique_ptr<LowEnergyScanner> scanner_;
  PeerFoundCallback peer_found_cb_;
  std::vector<std::unique_ptr<FakePeer>> peers_;
  FakeLocalAddressDelegate fake_address_delegate_{dispatcher()};
};

// Ensure we can parse a single advertising report correctly
TEST_F(LegacyLowEnergyScannerTest, ParseAdvertisingReportsSingleReport) {
  {
    auto peer = std::make_unique<FakePeer>(kPublicAddr,
                                           dispatcher(),
                                           /*connectable=*/false,
                                           /*scannable=*/false,
                                           /*send_advertising_report=*/false);
    peer->set_advertising_data(kPlainAdvDataBytes);
    test_device()->AddPeer(std::move(peer));
  }

  bool peer_found_callback_called = false;
  std::unordered_map<DeviceAddress, std::unique_ptr<DynamicByteBuffer>> map;

  set_peer_found_callback([&](const LowEnergyScanResult& result) {
    peer_found_callback_called = true;
    map[result.address()] =
        std::make_unique<DynamicByteBuffer>(result.data().size());
    result.data().Copy(&*map[result.address()]);
  });

  ASSERT_TRUE(StartScan(true));
  RunUntilIdle();

  auto peer = test_device()->FindPeer(kPublicAddr);
  DynamicByteBuffer buffer = peer->BuildLegacyAdvertisingReportEvent(false);
  test_device()->SendCommandChannelPacket(buffer);

  RunUntilIdle();
  ASSERT_TRUE(peer_found_callback_called);
  ASSERT_EQ(1u, map.count(peer->address()));
  ASSERT_TRUE(ContainersEqual(kPlainAdvDataBytes, *map[peer->address()]));
}

// Ensure we can parse multiple extended advertising reports correctly
TEST_F(LegacyLowEnergyScannerTest, ParseAdvertisingReportsMultipleReports) {
  {
    auto peer = std::make_unique<FakePeer>(kPublicAddr,
                                           dispatcher(),
                                           /*connectable=*/true,
                                           /*scannable=*/true,
                                           /*send_advertising_report=*/false);
    peer->set_advertising_data(kPlainAdvDataBytes);
    peer->set_scan_response(kPlainScanRspBytes);
    test_device()->AddPeer(std::move(peer));
  }

  bool peer_found_callback_called = false;
  std::unordered_map<DeviceAddress, std::unique_ptr<DynamicByteBuffer>> map;

  set_peer_found_callback([&](const LowEnergyScanResult& result) {
    peer_found_callback_called = true;
    map[result.address()] =
        std::make_unique<DynamicByteBuffer>(result.data().size());
    result.data().Copy(&*map[result.address()]);
  });

  ASSERT_TRUE(StartScan(true));
  RunUntilIdle();

  auto peer = test_device()->FindPeer(kPublicAddr);
  DynamicByteBuffer buffer = peer->BuildLegacyAdvertisingReportEvent(true);
  test_device()->SendCommandChannelPacket(buffer);

  RunUntilIdle();
  ASSERT_TRUE(peer_found_callback_called);
  ASSERT_EQ(1u, map.count(peer->address()));
  ASSERT_EQ(kPlainAdvDataBytes.ToString() + kPlainScanRspBytes.ToString(),
            map[peer->address()]->ToString());
}

// Test that we check for enough data being present before constructing a view
// on top of it. This case hopefully should never happen since the
// Controller should always send back valid data but it's better to be
// careful and avoid a crash.
TEST_F(LegacyLowEnergyScannerTest, ParseAdvertisingReportsNotEnoughData) {
  {
    auto peer = std::make_unique<FakePeer>(kPublicAddr,
                                           dispatcher(),
                                           /*connectable=*/true,
                                           /*scannable=*/true,
                                           /*send_advertising_report=*/false);
    peer->set_advertising_data(kPlainAdvDataBytes);
    test_device()->AddPeer(std::move(peer));
  }

  ASSERT_TRUE(StartScan(true));
  RunUntilIdle();

  auto peer = test_device()->FindPeer(kPublicAddr);
  DynamicByteBuffer buffer = peer->BuildLegacyAdvertisingReportEvent(false);
  auto params = pw::bluetooth::emboss::LEAdvertisingReportSubeventWriter(
      buffer.mutable_data(), buffer.size());
  auto report = pw::bluetooth::emboss::LEAdvertisingReportDataWriter(
      params.BackingStorage().data(), params.BackingStorage().SizeInBytes());
  report.data_length().Write(report.data_length().Read() + 1);
  test_device()->SendCommandChannelPacket(buffer);

  // there wasn't enough data available so we shouldn't have parsed out any
  // advertising reports
  set_peer_found_callback([&](const LowEnergyScanResult&) { FAIL(); });

  RunUntilIdle();
}

}  // namespace bt::hci
