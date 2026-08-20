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

#include "pw_bluetooth_sapphire/internal/host/hci/android_batch_low_energy_scanner.h"

#include "pw_bluetooth/hci_common.emb.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/vendor_protocol.h"
#include "pw_bluetooth_sapphire/internal/host/hci/advertising_packet_filter.h"
#include "pw_bluetooth_sapphire/internal/host/hci/fake_local_address_delegate.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_wake_alarm_provider.h"

namespace bt::hci {

namespace android_hci = hci_spec::vendor::android;
namespace android_emb = pw::bluetooth::vendor::android_hci;
namespace pwemb = pw::bluetooth::emboss;

using bt::testing::FakeController;
using bt::testing::FakeWakeAlarmProvider;
using TestingBase = bt::testing::FakeDispatcherControllerTest<FakeController>;
using testing::FakePeer;

constexpr pw::chrono::SystemClock::duration kPwScanResponseTimeout =
    std::chrono::seconds(2);

const StaticByteBuffer kPlainAdvDataBytes(
    5, bt::DataType::kCompleteLocalName, 'T', 'e', 's', 't', 'i', 'n', 'g');
const StaticByteBuffer kPlainScanRspBytes(
    5, bt::DataType::kCompleteLocalName, 'D', 'a', 't', 'a', 'l', 'o', 'l');

const DeviceAddress kPublicAddr1(DeviceAddress::Type::kLEPublic, {1});
const DeviceAddress kPublicAddr2(DeviceAddress::Type::kLEPublic, {2});
const DeviceAddress kPublicAddr3(DeviceAddress::Type::kLEPublic, {3});

class AndroidBatchLowEnergyScannerTest : public TestingBase,
                                         public LowEnergyScanner::Delegate {
 public:
  AndroidBatchLowEnergyScannerTest() = default;
  ~AndroidBatchLowEnergyScannerTest() override = default;

 protected:
  void SetUp() override {
    TestingBase::SetUp();

    FakeController::Settings settings;
    settings.ApplyExtendedLEConfig();
    test_device()->set_settings(settings);

    scanner_ = std::make_unique<AndroidBatchLowEnergyScanner>(
        &fake_address_delegate_,
        AdvertisingPacketFilter::Config(
            false,
            0,
            AdvertisingPacketFilter::Config::DeliveryMode::kImmediate),
        transport()->GetWeakPtr(),
        dispatcher(),
        wake_alarm_provider_);
    scanner_->SetPacketFilters(0, {});
    scanner_->set_delegate(this);

    auto p = std::make_unique<FakePeer>(kPublicAddr1, dispatcher(), true, true);
    p->set_advertising_data(kPlainAdvDataBytes);
    p->set_scan_response(kPlainScanRspBytes);
    test_device()->AddPeer(std::move(p));

    p = std::make_unique<FakePeer>(kPublicAddr2, dispatcher(), true, true);
    p->set_advertising_data(kPlainAdvDataBytes);
    p->set_scan_response(kPlainScanRspBytes);
    test_device()->AddPeer(std::move(p));

    auto enable = pw::bluetooth::emboss::GenericEnableParam::ENABLE;
    auto enable_packet =
        CommandPacket::New<android_emb::LEBatchScanEnableCommandWriter>(
            android_hci::kLEBatchScan);
    auto enable_view = enable_packet.view_t();
    enable_view.vendor_command().sub_opcode().Write(
        android_hci::kLEBatchScanEnableSubopcode);
    enable_view.enabled().Write(enable);

    bool enable_cb_called = false;
    transport()
        ->command_channel()
        ->SendCommand(std::move(enable_packet),
                      [&](auto, const EventPacket& event) {
                        EXPECT_FALSE(event.ToResult().is_error());
                        enable_cb_called = true;
                      })
        .IgnoreError();

    auto storage_packet = CommandPacket::New<
        android_emb::LEBatchScanSetStorageParametersCommandWriter>(
        android_hci::kLEBatchScan);
    auto storage_view = storage_packet.view_t();
    storage_view.vendor_command().sub_opcode().Write(
        android_hci::kLEBatchScanSetStorageParametersSubopcode);
    storage_view.full_max().Write(
        AndroidBatchLowEnergyScanner::kFullModeStoragePercentage);
    storage_view.truncated_max().Write(
        AndroidBatchLowEnergyScanner::kTruncatedModeStoragePercentage);
    storage_view.notify_threshold().Write(
        AndroidBatchLowEnergyScanner::
            kStorageThresholdBreachNotificationPercent);

    bool storage_cb_called = false;
    transport()
        ->command_channel()
        ->SendCommand(std::move(storage_packet),
                      [&](auto, const EventPacket& event) {
                        EXPECT_FALSE(event.ToResult().is_error());
                        storage_cb_called = true;
                      })
        .IgnoreError();

    RunUntilIdle();
    ASSERT_TRUE(enable_cb_called);
    ASSERT_TRUE(storage_cb_called);

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

  void StopScan() { scanner_->StopScan(); }

  FakeWakeAlarmProvider& wake_alarm_provider() { return wake_alarm_provider_; }

 protected:
  AndroidBatchLowEnergyScanner* scanner() const { return scanner_.get(); }

 private:
  std::unique_ptr<AndroidBatchLowEnergyScanner> scanner_;
  PeerFoundCallback peer_found_cb_;
  FakeLocalAddressDelegate fake_address_delegate_{dispatcher()};
  FakeWakeAlarmProvider wake_alarm_provider_{dispatcher()};

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(AndroidBatchLowEnergyScannerTest);
};

// Ensure we read batched scan results when we receive a storage threshold
// breached event from the controller
TEST_F(AndroidBatchLowEnergyScannerTest, StorageThresholdBreach) {
  auto p = std::make_unique<FakePeer>(kPublicAddr3, dispatcher(), true, false);
  p->set_advertising_data(kPlainAdvDataBytes);
  test_device()->AddPeer(std::move(p));

  test_device()->SendScanStorageThresholdBreachEvent();

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

  std::string expected_data =
      kPlainAdvDataBytes.ToString() + kPlainScanRspBytes.ToString();
  EXPECT_EQ(3u, map.size());
  EXPECT_EQ(1u, map.count(kPublicAddr1));
  EXPECT_EQ(expected_data, map[kPublicAddr1]->ToString());
  EXPECT_EQ(1u, map.count(kPublicAddr2));
  EXPECT_EQ(expected_data, map[kPublicAddr2]->ToString());
  EXPECT_EQ(1u, map.count(kPublicAddr3));
  EXPECT_EQ(map[kPublicAddr3]->ToString(), kPlainAdvDataBytes.ToString());
}

// Ensure we repeatedly query the controller for batched scan results if the
// controller can't send all of them in a single command complete event
TEST_F(AndroidBatchLowEnergyScannerTest, MultipleStorageThresholdBreach) {
  // Calculate the number of peers we need to require multiple reads of the
  // batched scan results
  size_t max_hci_packet_size = std::numeric_limits<uint8_t>::max();
  size_t full_result_size =
      android_emb::LEBatchScanFullResult::MinSizeInBytes() +
      kPlainAdvDataBytes.size() + kPlainScanRspBytes.size();
  size_t num_peers = (max_hci_packet_size / full_result_size) * 1.5;

  // We start at 2 because we already added 2 peers in the SetUp method
  for (unsigned int i = 2; i < num_peers; i++) {
    DeviceAddress address(DeviceAddress::Type::kLEPublic,
                          {static_cast<unsigned char>(i + 1)});
    auto p = std::make_unique<FakePeer>(address, dispatcher(), true, true);
    p->set_advertising_data(kPlainAdvDataBytes);
    p->set_scan_response(kPlainScanRspBytes);
    test_device()->AddPeer(std::move(p));
  }

  test_device()->SendScanStorageThresholdBreachEvent();

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
  ASSERT_EQ(num_peers, map.size());

  for (unsigned int i = 0; i < num_peers; i++) {
    DeviceAddress address(DeviceAddress::Type::kLEPublic,
                          {static_cast<unsigned char>(i + 1)});
    std::string expected_data =
        kPlainAdvDataBytes.ToString() + kPlainScanRspBytes.ToString();
    ASSERT_EQ(1u, map.count(address));
    EXPECT_EQ(expected_data, map[address]->ToString());
  }
}

// Ensure we don't try to read results from the Controller if we're not
// scanning at the moment.
TEST_F(AndroidBatchLowEnergyScannerTest, DoNotReadScanResultsWhenNotScanning) {
  StopScan();
  RunUntilIdle();

  test_device()->SendScanStorageThresholdBreachEvent();
  RunUntilIdle();

  bool peer_found_callback_called = false;
  set_peer_found_callback(
      [&](const LowEnergyScanResult&) { peer_found_callback_called = true; });

  RunUntilIdle();
  EXPECT_FALSE(peer_found_callback_called);
}

// Ensure we check that there are enough bytes left to read when parsing scan
// results. We need to make sure we don't crash if we received a malformed
// packet where the header is too short.
TEST_F(AndroidBatchLowEnergyScannerTest, ControllerSendsNotEnoughBytesHeader) {
  test_device()->set_malformed_batch_scan_type(
      FakeController::MalformedBatchScanType::kHeaderTooShort);
  test_device()->SendScanStorageThresholdBreachEvent();
  RunUntilIdle();
}

// Ensure we check that there are enough bytes left to read when parsing scan
// results. We need to make sure we don't crash if we received a malformed
// packet where the advertising data is too short.
TEST_F(AndroidBatchLowEnergyScannerTest, ControllerSendsNotEnoughBytesAdvData) {
  test_device()->set_malformed_batch_scan_type(
      FakeController::MalformedBatchScanType::kAdvDataTooShort);
  test_device()->SendScanStorageThresholdBreachEvent();
  RunUntilIdle();
}

// Ensure we check that there are enough bytes left to read when parsing scan
// results. We need to make sure we don't crash if we received a malformed
// packet where the scan response is too short.
TEST_F(AndroidBatchLowEnergyScannerTest, ControllerSendsNotEnoughBytesScanRsp) {
  test_device()->set_malformed_batch_scan_type(
      FakeController::MalformedBatchScanType::kScanRspTooShort);
  test_device()->SendScanStorageThresholdBreachEvent();
  RunUntilIdle();
}

// Ensure we read batched scan results when we haven't read them in
// a while. Ensure we reschedule a read after reading once.
TEST_F(AndroidBatchLowEnergyScannerTest, TooLongDidntRead) {
  bool peer_found_callback_called = false;
  set_peer_found_callback([&](const LowEnergyScanResult& /*result*/) {
    peer_found_callback_called = true;
  });

  RunFor(AndroidBatchLowEnergyScanner::kMaxReadDelay);
  EXPECT_TRUE(peer_found_callback_called);

  auto p = std::make_unique<FakePeer>(kPublicAddr3, dispatcher(), true, true);
  p->set_advertising_data(kPlainAdvDataBytes);
  p->set_scan_response(kPlainScanRspBytes);
  test_device()->AddPeer(std::move(p));

  peer_found_callback_called = false;
  RunFor(AndroidBatchLowEnergyScanner::kMaxReadDelay);
  EXPECT_TRUE(peer_found_callback_called);
}

// Ensure that if a read of batched scan results fails, we don't stop trying to
// read scan results in the future.
TEST_F(AndroidBatchLowEnergyScannerTest, ReadScanResultsFails) {
  test_device()->SetDefaultResponseStatus(
      hci_spec::vendor::android::kLEBatchScan,
      pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED);
  test_device()->SendScanStorageThresholdBreachEvent();

  bool peer_found_callback_called = false;
  set_peer_found_callback([&](const LowEnergyScanResult& /*result*/) {
    peer_found_callback_called = true;
  });

  RunUntilIdle();
  EXPECT_FALSE(peer_found_callback_called);

  RunFor(AndroidBatchLowEnergyScanner::kMaxReadDelay);
  EXPECT_FALSE(peer_found_callback_called);

  test_device()->ClearDefaultResponseStatus(
      hci_spec::vendor::android::kLEBatchScan);

  RunFor(AndroidBatchLowEnergyScanner::kMaxReadDelay);
  EXPECT_TRUE(peer_found_callback_called);
}

// Ensure that calling StartScan when already scanning (which fails) does not
// reschedule the periodic read task.
TEST_F(AndroidBatchLowEnergyScannerTest,
       StartScanWhileScanningDoesNotRescheduleTask) {
  bool peer_found_callback_called = false;
  set_peer_found_callback([&](const LowEnergyScanResult& /*result*/) {
    peer_found_callback_called = true;
  });

  RunFor(AndroidBatchLowEnergyScanner::kMaxReadDelay / 2);
  EXPECT_FALSE(peer_found_callback_called);

  EXPECT_FALSE(StartScan(/*active=*/true));

  RunFor(AndroidBatchLowEnergyScanner::kMaxReadDelay / 2);
  EXPECT_TRUE(peer_found_callback_called);
}

// Ensure that if the scan period expires while a batch scan read results
// command is in flight, the scanner cancels the read command, stops the scan,
// and does not deadlock.
TEST_F(AndroidBatchLowEnergyScannerTest, ScanPeriodExpiresWhileReadInFlight) {
  // StartScan was already called in SetUp with kPeriodInfinite.
  // We need to stop it first.
  StopScan();
  RunUntilIdle();

  // Start scan with 10s period.
  constexpr pw::chrono::SystemClock::duration kPeriod =
      std::chrono::seconds(10);
  ASSERT_TRUE(StartScan(/*active=*/true, kPeriod));
  RunUntilIdle();

  // Pause responses for the first batch scan command (the read results),
  // but allow subsequent commands (like stop) to proceed.
  bool read_command_received = false;
  fit::closure resume_read;
  int batch_scan_cmd_count = 0;
  test_device()->pause_responses_for_opcode(
      android_hci::kLEBatchScan,
      [&, batch_scan_cmd_count](fit::closure resume) mutable {
        batch_scan_cmd_count++;
        if (batch_scan_cmd_count == 1) {
          read_command_received = true;
          resume_read = std::move(resume);
        } else {
          resume();
        }
      });

  // Trigger a breach event to start a read command.
  test_device()->SendScanStorageThresholdBreachEvent();
  RunUntilIdle();

  // The read command should have been sent and paused.
  EXPECT_TRUE(read_command_received);

  // Now advance time to trigger the scan timeout (10 seconds).
  // The scan timeout task should fire and call StopScanInternal.
  // It should cancel the pending read command, and send the stop command.
  RunFor(kPeriod + std::chrono::seconds(1));
  RunUntilIdle();

  // Clean up by resuming the paused command. This will release the flow control
  // credit and allow the buffered stop command to be sent and processed.
  if (resume_read) {
    resume_read();
  }
  RunUntilIdle();

  // The scanner should be in the idle state (scan stopped).
  EXPECT_EQ(LowEnergyScanner::State::kIdle, scanner()->state());
}

TEST_F(AndroidBatchLowEnergyScannerTest, ScanPeriodExpiresCancelsWakeAlarm) {
  // Stop the default scan started in Setup
  StopScan();
  RunUntilIdle();

  // Set the scan period to 5 seconds. The scan should stop automatically after
  // 5 seconds.
  constexpr std::chrono::seconds kScanPeriod(5);
  ASSERT_TRUE(StartScan(/*active=*/true, kScanPeriod));
  RunUntilIdle();
  ASSERT_EQ(LowEnergyScanner::State::kActiveScanning, scanner()->state());

  // At this point, the scanner should have scheduled the next read.
  // There should be a pending wake alarm set to fire after 3 seconds.
  EXPECT_TRUE(wake_alarm_provider().HasPendingAlarms());

  // 3s: first read should fire and complete.
  RunFor(std::chrono::seconds(3));
  RunUntilIdle();

  // After the first read completes, another wake alarm should be scheduled
  // to fire in 3s (at t=6s).
  EXPECT_TRUE(wake_alarm_provider().HasPendingAlarms());

  // Advance time to 5.5s.
  // At t=5s, the scan period should expire, stopping the scan.
  // The stop sequence should cancel the pending wake alarm (which is set for
  // t=6s).
  RunFor(std::chrono::milliseconds(2500));
  RunUntilIdle();

  // The scanner should be in the idle state (scan stopped).
  EXPECT_EQ(LowEnergyScanner::State::kIdle, scanner()->state());

  // The pending wake alarm should have been cancelled.
  EXPECT_FALSE(wake_alarm_provider().HasPendingAlarms());
  EXPECT_EQ(0u, wake_alarm_provider().active_leases());
}

// Ensure that when a periodic wake alarm triggers a read, an active wake lease
// is held while the read results HCI command is in flight to prevent system
// suspend, and cleanly released as soon as processing completes.
TEST_F(AndroidBatchLowEnergyScannerTest,
       WakeLeaseHeldDuringReadAndReleasedUponCompletion) {
  EXPECT_EQ(0u, wake_alarm_provider().active_leases());

  bool read_command_received = false;
  fit::closure resume_read;
  int batch_scan_cmd_count = 0;
  test_device()->pause_responses_for_opcode(
      android_hci::kLEBatchScan,
      [&, batch_scan_cmd_count](fit::closure resume) mutable {
        batch_scan_cmd_count++;
        if (batch_scan_cmd_count == 1) {
          read_command_received = true;
          resume_read = std::move(resume);
        } else {
          resume();
        }
      });

  RunFor(AndroidBatchLowEnergyScanner::kMaxReadDelay);
  EXPECT_TRUE(read_command_received);
  EXPECT_EQ(1u, wake_alarm_provider().active_leases());

  if (resume_read) {
    resume_read();
  }
  RunUntilIdle();

  EXPECT_EQ(0u, wake_alarm_provider().active_leases());
  EXPECT_TRUE(wake_alarm_provider().HasPendingAlarms());
}

// Ensure that if a scan is stopped via StopScan() while a periodic read command
// is currently paused in flight holding an active wake lease, the scanner does
// not reschedule further reads and cleanly releases the wake lease.
TEST_F(AndroidBatchLowEnergyScannerTest,
       WakeLeaseReleasedOnStopScanWhileReadInFlight) {
  EXPECT_EQ(0u, wake_alarm_provider().active_leases());

  bool read_command_received = false;
  fit::closure resume_read;
  int batch_scan_cmd_count = 0;
  test_device()->pause_responses_for_opcode(
      android_hci::kLEBatchScan,
      [&, batch_scan_cmd_count](fit::closure resume) mutable {
        batch_scan_cmd_count++;
        if (batch_scan_cmd_count == 1) {
          read_command_received = true;
          resume_read = std::move(resume);
        } else {
          resume();
        }
      });

  RunFor(AndroidBatchLowEnergyScanner::kMaxReadDelay);
  EXPECT_TRUE(read_command_received);
  EXPECT_EQ(1u, wake_alarm_provider().active_leases());

  StopScan();
  RunUntilIdle();

  if (resume_read) {
    resume_read();
  }
  RunUntilIdle();

  EXPECT_EQ(0u, wake_alarm_provider().active_leases());
  EXPECT_EQ(LowEnergyScanner::State::kIdle, scanner()->state());
  EXPECT_FALSE(wake_alarm_provider().HasPendingAlarms());
}

// Ensure that if a configured scan period expires naturally while a periodic
// read command is paused in flight holding an active wake lease, the timeout
// sequence cancels ongoing reads without leaking the wake lease.
TEST_F(AndroidBatchLowEnergyScannerTest,
       WakeLeaseReleasedOnScanPeriodExpirationWhileReadInFlight) {
  StopScan();
  RunUntilIdle();
  EXPECT_EQ(0u, wake_alarm_provider().active_leases());

  constexpr pw::chrono::SystemClock::duration kPeriod = std::chrono::seconds(4);
  ASSERT_TRUE(StartScan(/*active=*/true, kPeriod));
  RunUntilIdle();

  bool read_command_received = false;
  fit::closure resume_read;
  int batch_scan_cmd_count = 0;
  test_device()->pause_responses_for_opcode(
      android_hci::kLEBatchScan,
      [&, batch_scan_cmd_count](fit::closure resume) mutable {
        batch_scan_cmd_count++;
        if (batch_scan_cmd_count == 1) {
          read_command_received = true;
          resume_read = std::move(resume);
        } else {
          resume();
        }
      });

  RunFor(AndroidBatchLowEnergyScanner::kMaxReadDelay);
  EXPECT_TRUE(read_command_received);
  EXPECT_EQ(1u, wake_alarm_provider().active_leases());

  RunFor(std::chrono::seconds(1) + std::chrono::milliseconds(100));
  RunUntilIdle();

  if (resume_read) {
    resume_read();
  }
  RunUntilIdle();

  EXPECT_EQ(LowEnergyScanner::State::kIdle, scanner()->state());
  EXPECT_EQ(0u, wake_alarm_provider().active_leases());
  EXPECT_FALSE(wake_alarm_provider().HasPendingAlarms());
}

// Ensure that if the periodic read task fires while the scanner is in
// kInitiating (e.g. slow HCI start sequence), it reschedules the next read,
// and periodic reads continue normally when transitioning to active scanning.
TEST_F(AndroidBatchLowEnergyScannerTest,
       PeriodicReadFiresWhileStartScanInitiating) {
  StopScan();
  RunUntilIdle();

  int batch_scan_cmd_count = 0;
  bool start_command_received = false;
  fit::closure resume_start;
  test_device()->pause_responses_for_opcode(
      android_hci::kLEBatchScan,
      [&, batch_scan_cmd_count](fit::closure resume) mutable {
        batch_scan_cmd_count++;
        if (batch_scan_cmd_count == 1) {
          start_command_received = true;
          resume_start = std::move(resume);
        } else {
          resume();
        }
      });

  ASSERT_TRUE(StartScan(/*active=*/true));
  RunUntilIdle();

  EXPECT_TRUE(start_command_received);
  EXPECT_TRUE(scanner()->IsInitiating());

  // Advance time by kMaxReadDelay. The periodic read task fires while state is
  // still kInitiating. It should reschedule the read instead of dropping it.
  RunFor(AndroidBatchLowEnergyScanner::kMaxReadDelay);

  // Allow scan start sequence to complete.
  if (resume_start) {
    resume_start();
  }
  RunUntilIdle();

  EXPECT_TRUE(scanner()->IsActiveScanning());

  // Add a peer so we can verify results are read.
  auto p = std::make_unique<FakePeer>(kPublicAddr3, dispatcher(), true, true);
  p->set_advertising_data(kPlainAdvDataBytes);
  p->set_scan_response(kPlainScanRspBytes);
  test_device()->AddPeer(std::move(p));

  bool peer_found_callback_called = false;
  set_peer_found_callback([&](const LowEnergyScanResult& /*result*/) {
    peer_found_callback_called = true;
  });

  // Advance time by kMaxReadDelay. Periodic read should fire and find peer.
  RunFor(AndroidBatchLowEnergyScanner::kMaxReadDelay);
  RunUntilIdle();
  EXPECT_TRUE(peer_found_callback_called);
}

}  // namespace bt::hci
