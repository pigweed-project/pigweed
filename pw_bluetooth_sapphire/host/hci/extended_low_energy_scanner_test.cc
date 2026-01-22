// Copyright 2024 The Pigweed Authors
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

#include "pw_bluetooth_sapphire/internal/host/hci/extended_low_energy_scanner.h"

#include <unordered_map>

#include "pw_bluetooth_sapphire/internal/host/hci/advertising_packet_filter.h"
#include "pw_bluetooth_sapphire/internal/host/hci/fake_local_address_delegate.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_controller.h"

namespace bt::hci {

using bt::testing::FakeController;
using TestingBase = bt::testing::FakeDispatcherControllerTest<FakeController>;
using LEAdvertisingState = FakeController::LEAdvertisingState;

using pw::bluetooth::emboss::LEAdvertisingDataStatus;
using pw::bluetooth::emboss::LEExtendedAdvertisingReportDataView;
using pw::bluetooth::emboss::LEExtendedAdvertisingReportDataWriter;
using pw::bluetooth::emboss::LEExtendedAdvertisingReportSubeventWriter;
using testing::FakePeer;

constexpr pw::chrono::SystemClock::duration kPwScanResponseTimeout =
    std::chrono::seconds(2);

const StaticByteBuffer kPlainAdvDataBytes(
    5, bt::DataType::kCompleteLocalName, 'T', 'e', 's', 't');
const StaticByteBuffer kPlainScanRspBytes(
    5, bt::DataType::kCompleteLocalName, 'D', 'a', 't', 'a');

const DeviceAddress kPublicAddr1(DeviceAddress::Type::kLEPublic, {1});
const DeviceAddress kPublicAddr2(DeviceAddress::Type::kLEPublic, {2});
const DeviceAddress kPublicAddr3(DeviceAddress::Type::kLEPublic, {3});

constexpr uint8_t kAdvertisingSid = 0xa;
constexpr uint16_t kPeriodicAdvertisingInterval = 0x1234;

class ExtendedLowEnergyScannerTest : public TestingBase,
                                     public LowEnergyScanner::Delegate {
 public:
  ExtendedLowEnergyScannerTest() = default;
  ~ExtendedLowEnergyScannerTest() override = default;

 protected:
  void SetUp() override {
    TestingBase::SetUp();

    FakeController::Settings settings;
    settings.ApplyExtendedLEConfig();
    test_device()->set_settings(settings);

    scanner_ = std::make_unique<ExtendedLowEnergyScanner>(
        &fake_address_delegate_,
        AdvertisingPacketFilter::Config(
            false,
            0,
            AdvertisingPacketFilter::Config::DeliveryMode::kImmediate),
        transport()->GetWeakPtr(),
        dispatcher());
    scanner_->SetPacketFilters(0, {});
    scanner_->set_delegate(this);

    // peers(0)
    auto p = std::make_unique<FakePeer>(kPublicAddr1, dispatcher(), true, true);
    p->set_use_extended_advertising_pdus(true);
    p->set_advertising_data(kPlainAdvDataBytes);
    p->set_scan_response(kPlainScanRspBytes);
    peers_.push_back(std::move(p));

    // peers(1)
    p = std::make_unique<FakePeer>(kPublicAddr2, dispatcher(), true, false);
    p->set_use_extended_advertising_pdus(true);
    p->set_advertising_data(kPlainAdvDataBytes);
    p->set_advertising_sid(kAdvertisingSid);
    p->set_periodic_advertising_interval(kPeriodicAdvertisingInterval);
    peers_.push_back(std::move(p));

    // peers(2)
    p = std::make_unique<FakePeer>(kPublicAddr3, dispatcher(), true, false);
    p->set_use_extended_advertising_pdus(true);
    p->set_advertising_data(kPlainAdvDataBytes);
    peers_.push_back(std::move(p));

    StartScan(/*active=*/true);
    RunUntilIdle();
  }

  void TearDown() override {
    scanner_ = nullptr;
    TestingBase::TearDown();
  }

  void OnPeerFound(const std::unordered_set<uint16_t>& /*scan_ids*/,
                   const LowEnergyScanResult& result) override {
    if (peer_found_cb_) {
      peer_found_cb_(result);
    }
  }

  using PeerFoundCallback = fit::function<void(const LowEnergyScanResult&)>;
  void set_peer_found_callback(PeerFoundCallback cb) {
    peer_found_cb_ = std::move(cb);
  }

  bool StartScan(bool active,
                 pw::chrono::SystemClock::duration period =
                     LowEnergyScanner::kPeriodInfinite) {
    LowEnergyScanner::ScanOptions options{
        .active = active,
        .filter_duplicates = true,
        .period = period,
        .scan_response_timeout = kPwScanResponseTimeout};
    return scanner_->StartScan(options, [](auto) {});
  }

  const std::unique_ptr<FakePeer>& peer(int i) const { return peers_[i]; }

  static constexpr size_t event_prefix_size = pw::bluetooth::emboss::
      LEExtendedAdvertisingReportSubevent::MinSizeInBytes();
  static constexpr size_t report_prefix_size =
      pw::bluetooth::emboss::LEExtendedAdvertisingReportData::MinSizeInBytes();

 private:
  std::unique_ptr<ExtendedLowEnergyScanner> scanner_;
  PeerFoundCallback peer_found_cb_;
  std::vector<std::unique_ptr<FakePeer>> peers_;
  FakeLocalAddressDelegate fake_address_delegate_{dispatcher()};

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(ExtendedLowEnergyScannerTest);
};

// Ensure we can parse a single advertising report correctly
TEST_F(ExtendedLowEnergyScannerTest, ParseAdvertisingReportsSingleReport) {
  size_t data_size = peer(1)->advertising_data().size();
  size_t reports_size = report_prefix_size + data_size;
  size_t packet_size = event_prefix_size + reports_size;

  auto event = hci::EventPacket::New<LEExtendedAdvertisingReportSubeventWriter>(
      hci_spec::kLEMetaEventCode, packet_size);
  auto packet = event.view_t();
  packet.le_meta_event().subevent_code().Write(
      hci_spec::kLEExtendedAdvertisingReportSubeventCode);
  packet.num_reports().Write(1);

  LEExtendedAdvertisingReportDataWriter report(
      packet.reports().BackingStorage().begin(), reports_size);
  peer(1)->FillExtendedAdvertisingReport(report,
                                         peer(1)->advertising_data(),
                                         /*is_fragmented=*/false,
                                         /*is_scan_response=*/false);

  test_device()->SendCommandChannelPacket(event.data());

  bool peer_found_callback_called = false;
  set_peer_found_callback([&](const LowEnergyScanResult& result) {
    peer_found_callback_called = true;
    EXPECT_EQ(peer(1)->address(), result.address());
    EXPECT_EQ(peer(1)->advertising_data().size(), result.data().size());
    EXPECT_EQ(peer(1)->advertising_data(), result.data());
    EXPECT_EQ(result.advertising_sid(), kAdvertisingSid);
    EXPECT_EQ(result.periodic_advertising_interval(),
              kPeriodicAdvertisingInterval);
  });

  RunUntilIdle();
  EXPECT_TRUE(peer_found_callback_called);
}

// Ensure we can parse multiple extended advertising reports correctly
TEST_F(ExtendedLowEnergyScannerTest, ParseAdvertisingReportsMultipleReports) {
  size_t data_size = peer(1)->advertising_data().size();
  uint8_t num_reports = 2;
  size_t single_report_size = report_prefix_size + data_size;
  size_t reports_size = num_reports * single_report_size;
  size_t packet_size = event_prefix_size + reports_size;

  auto event = hci::EventPacket::New<LEExtendedAdvertisingReportSubeventWriter>(
      hci_spec::kLEMetaEventCode, packet_size);
  auto packet = event.view_t();
  packet.le_meta_event().subevent_code().Write(
      hci_spec::kLEExtendedAdvertisingReportSubeventCode);
  packet.num_reports().Write(num_reports);

  LEExtendedAdvertisingReportDataWriter report_a(
      packet.reports().BackingStorage().begin(), single_report_size);
  peer(1)->FillExtendedAdvertisingReport(report_a,
                                         peer(1)->advertising_data(),
                                         /*is_fragmented=*/false,
                                         /*is_scan_response=*/false);

  LEExtendedAdvertisingReportDataWriter report_b(
      packet.reports().BackingStorage().begin() + single_report_size,
      single_report_size);
  peer(2)->FillExtendedAdvertisingReport(report_b,
                                         peer(2)->advertising_data(),
                                         /*is_fragmented=*/false,
                                         /*is_scan_response=*/false);

  test_device()->SendCommandChannelPacket(event.data());

  bool peer_found_callback_called = false;
  std::unordered_map<DeviceAddress, std::unique_ptr<DynamicByteBuffer>> map;

  set_peer_found_callback([&](const LowEnergyScanResult& result) {
    peer_found_callback_called = true;
    map[result.address()] =
        std::make_unique<DynamicByteBuffer>(result.data().size());
    result.data().Copy(&*map[result.address()]);
  });

  RunUntilIdle();
  EXPECT_TRUE(peer_found_callback_called);

  EXPECT_EQ(2u, map.size());
  EXPECT_EQ(1u, map.count(peer(1)->address()));
  EXPECT_EQ(*map[peer(1)->address()], peer(1)->advertising_data());

  EXPECT_EQ(1u, map.count(peer(2)->address()));
  EXPECT_EQ(*map[peer(2)->address()], peer(2)->advertising_data());
}

// Test that we check for enough data being present before constructing a view
// on top of it. This case hopefully should never happen since the
// Controller should always send back valid data but it's better to be
// careful and avoid a crash.
TEST_F(ExtendedLowEnergyScannerTest, ParseAdvertisingReportsNotEnoughData) {
  size_t data_size = peer(1)->advertising_data().size();
  size_t reports_size = report_prefix_size + data_size;
  size_t packet_size = event_prefix_size + reports_size;

  auto event = hci::EventPacket::New<LEExtendedAdvertisingReportSubeventWriter>(
      hci_spec::kLEMetaEventCode, packet_size);
  auto packet = event.view_t();
  packet.le_meta_event().subevent_code().Write(
      hci_spec::kLEExtendedAdvertisingReportSubeventCode);
  packet.num_reports().Write(1);

  LEExtendedAdvertisingReportDataWriter report(
      packet.reports().BackingStorage().begin(), reports_size);
  peer(1)->FillExtendedAdvertisingReport(report,
                                         peer(1)->advertising_data(),
                                         /*is_fragmented=*/false,
                                         /*is_scan_response=*/false);

  // claim we need more data than we actually provided to trigger the edge case
  report.data_length().Write(report.data_length().Read() + 1);
  test_device()->SendCommandChannelPacket(event.data());

  // there wasn't enough data available so we shouldn't have parsed out any
  // advertising reports
  set_peer_found_callback([&](const LowEnergyScanResult&) { FAIL(); });

  RunUntilIdle();
}

// If a series of advertising reports claim to have more than
// hci_spec::kMaxLEExtendedAdvertisingDataLength, we should truncate the excess.
// This case hopefully should never happen since the Controller should always
// send back valid data but it's better to be careful and avoid a bug.
TEST_F(ExtendedLowEnergyScannerTest, TruncateToMax) {
  size_t data_size = std::numeric_limits<uint8_t>::max() - report_prefix_size -
                     event_prefix_size;
  size_t reports_size = report_prefix_size + data_size;
  size_t packet_size = event_prefix_size + reports_size;
  size_t num_full_reports =
      hci_spec::kMaxLEExtendedAdvertisingDataLength / data_size;

  for (size_t i = 0; i < num_full_reports; i++) {
    auto event =
        hci::EventPacket::New<LEExtendedAdvertisingReportSubeventWriter>(
            hci_spec::kLEMetaEventCode, packet_size);
    auto packet = event.view_t();
    packet.le_meta_event().subevent_code().Write(
        hci_spec::kLEExtendedAdvertisingReportSubeventCode);
    packet.num_reports().Write(1);

    LEExtendedAdvertisingReportDataWriter report(
        packet.reports().BackingStorage().begin(), reports_size);
    peer(1)->FillExtendedAdvertisingReport(report,
                                           peer(1)->advertising_data(),
                                           /*is_fragmented=*/true,
                                           /*is_scan_response=*/false);
    report.data_length().Write(static_cast<uint8_t>(data_size));

    test_device()->SendCommandChannelPacket(event.data());
  }

  // the final report we send has an extra byte to trigger the edge case
  data_size = hci_spec::kMaxLEExtendedAdvertisingDataLength % data_size + 1;
  reports_size = report_prefix_size + data_size;
  packet_size = event_prefix_size + reports_size;

  auto event = hci::EventPacket::New<LEExtendedAdvertisingReportSubeventWriter>(
      hci_spec::kLEMetaEventCode, packet_size);
  auto packet = event.view_t();
  packet.le_meta_event().subevent_code().Write(
      hci_spec::kLEExtendedAdvertisingReportSubeventCode);
  packet.num_reports().Write(1);

  LEExtendedAdvertisingReportDataWriter report(
      packet.reports().BackingStorage().begin(), reports_size);
  peer(1)->FillExtendedAdvertisingReport(report,
                                         peer(1)->advertising_data(),
                                         /*is_fragmented=*/false,
                                         /*is_scan_response=*/false);
  report.data_length().Write(static_cast<int32_t>(data_size));

  size_t result_data_length = 0;
  set_peer_found_callback([&](const LowEnergyScanResult& result) {
    result_data_length = result.data().size();
  });

  test_device()->SendCommandChannelPacket(event.data());

  RunUntilIdle();
  EXPECT_EQ(hci_spec::kMaxLEExtendedAdvertisingDataLength, result_data_length);
}

// If we receive an event marked as incomplete, there is more data coming in
// another extended advertising report. We should wait for that data and not
// call the peer found callback.
TEST_F(ExtendedLowEnergyScannerTest, Incomplete) {
  size_t data_size = peer(1)->advertising_data().size();
  size_t reports_size = report_prefix_size + data_size;
  size_t packet_size = event_prefix_size + reports_size;

  auto event = hci::EventPacket::New<LEExtendedAdvertisingReportSubeventWriter>(
      hci_spec::kLEMetaEventCode, packet_size);
  auto packet = event.view_t();
  packet.le_meta_event().subevent_code().Write(
      hci_spec::kLEExtendedAdvertisingReportSubeventCode);
  packet.num_reports().Write(1);

  LEExtendedAdvertisingReportDataWriter report(
      packet.reports().BackingStorage().begin(), reports_size);
  peer(1)->FillExtendedAdvertisingReport(report,
                                         peer(1)->advertising_data(),
                                         /*is_fragmented=*/true,
                                         /*is_scan_response=*/false);
  test_device()->SendCommandChannelPacket(event.data());

  bool callback_called = false;
  set_peer_found_callback(
      [&](const LowEnergyScanResult&) { callback_called = true; });

  RunUntilIdle();
  EXPECT_FALSE(callback_called);
}

// If we receive an event marked as incomplete truncated, the data was truncated
// but we won't be receiving any more advertising reports for this particular
// peer. We can go ahead and notify a peer was found with the data we do
// currently have. Ensure that we do.
//
// We specifically use peer(0) here because it is set to be scannable. We want
// to make sure that we continue to scan for a scan response, even if the
// advertising data got truncated.
TEST_F(ExtendedLowEnergyScannerTest, IncompleteTruncated) {
  size_t data_size = peer(0)->advertising_data().size();
  size_t reports_size = report_prefix_size + data_size;
  size_t packet_size = event_prefix_size + reports_size;

  auto event = hci::EventPacket::New<LEExtendedAdvertisingReportSubeventWriter>(
      hci_spec::kLEMetaEventCode, packet_size);
  auto packet = event.view_t();
  packet.le_meta_event().subevent_code().Write(
      hci_spec::kLEExtendedAdvertisingReportSubeventCode);
  packet.num_reports().Write(1);

  LEExtendedAdvertisingReportDataWriter report(
      packet.reports().BackingStorage().begin(), reports_size);
  peer(0)->FillExtendedAdvertisingReport(report,
                                         peer(0)->advertising_data(),
                                         /*is_fragmented=*/false,
                                         /*is_scan_response=*/false);
  report.event_type().data_status().Write(
      LEAdvertisingDataStatus::INCOMPLETE_TRUNCATED);
  test_device()->SendCommandChannelPacket(event.data());

  bool callback_called = false;
  set_peer_found_callback(
      [&](const LowEnergyScanResult&) { callback_called = true; });

  RunUntilIdle();
  EXPECT_FALSE(callback_called);
}

// If we receive an event marked as incomplete truncated, the data was truncated
// but we won't be receiving any more advertising reports for this particular
// peer. We can go ahead and notify a peer was found with the data we do
// currently have.
//
// We specifically use peer(1) here because it is not set to be scannable. We
// want to make sure that we report the peer found right away if the peer isn't
// scannable, essentially treating this event as if the advertising data was
// complete.
TEST_F(ExtendedLowEnergyScannerTest, IncompleteTruncatedNonScannable) {
  size_t data_size = peer(1)->advertising_data().size();
  size_t reports_size = report_prefix_size + data_size;
  size_t packet_size = event_prefix_size + reports_size;

  auto event = hci::EventPacket::New<LEExtendedAdvertisingReportSubeventWriter>(
      hci_spec::kLEMetaEventCode, packet_size);
  auto packet = event.view_t();
  packet.le_meta_event().subevent_code().Write(
      hci_spec::kLEExtendedAdvertisingReportSubeventCode);
  packet.num_reports().Write(1);

  LEExtendedAdvertisingReportDataWriter report(
      packet.reports().BackingStorage().begin(), reports_size);
  peer(1)->FillExtendedAdvertisingReport(report,
                                         peer(1)->advertising_data(),
                                         /*is_fragmented=*/false,
                                         /*is_scan_response=*/false);
  report.event_type().data_status().Write(
      LEAdvertisingDataStatus::INCOMPLETE_TRUNCATED);
  test_device()->SendCommandChannelPacket(event.data());

  bool callback_called = false;
  set_peer_found_callback(
      [&](const LowEnergyScanResult&) { callback_called = true; });

  RunUntilIdle();
  EXPECT_TRUE(callback_called);
}

}  // namespace bt::hci
