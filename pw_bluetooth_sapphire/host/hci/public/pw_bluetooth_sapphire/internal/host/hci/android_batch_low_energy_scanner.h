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

#pragma once

#include "pw_bluetooth/hci_android.emb.h"
#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_scanner.h"
#include "pw_bluetooth_sapphire/wake_alarm.h"

namespace bt::hci {

class LocalAddressDelegate;

// AndroidBatchLowEnergyScanner implements the Android HCI vendor extensions
// batch scanning feature. By reducing how often the controller notifies the
// host app processor to scan results, the host app processor can stay in
// idle/sleep longer. This reduces power consumption in the host.
//
// For more information, see
// https://source.android.com/docs/core/connect/bluetooth/hci_requirements
class AndroidBatchLowEnergyScanner final : public LowEnergyScanner {
 public:
  // Usually the Controller will send a storage Threshold breach event to
  // indicate that it's running out of space for batched scan results. However,
  // we don't want to let scan results just sit in the Controller until that
  // happens. The default maximum read delay for batched scan results.
  //
  // Note: This value is a placeholder that resembles the timeouts that android
  // uses for its nearby and location scanning. We want to establish a good
  // balance here and not wake up the host too often to request peers from the
  // controller in a very quiet environment. We also don't want to wait too long
  // because then appear might just hang out inside the controller and the user
  // will think scanning is very slow. Android actually has an API that allows
  // the user to configure how long it should wait instead of hard coding the
  // amount. We don't have that in our API at the moment, but we can certainly
  // add it in the future.
  static constexpr pw::chrono::SystemClock::duration kDefaultMaxReadDelay =
      std::chrono::seconds(3);

  static constexpr pw::chrono::SystemClock::duration kMaxReadDelay =
      kDefaultMaxReadDelay;

  // Android's batch scanning vendor extension allows us to store scan results
  // in two formats: full mode and truncated mode. Truncated mode includes only
  // the peer address, transmission power, rssi, and a timestamp. Full mode
  // includes all of truncated mode's data along with the peer's advertising
  // data and scan response data, if present. We store only full mode scan
  // results because we need advertising and scan response data from peers.
  static constexpr uint8_t kFullModeStoragePercentage = 100;
  static constexpr uint8_t kTruncatedModeStoragePercentage = 0;

  // The percentage of storage that needs to be consumed by batched scan results
  // before the Controller notifies us with a Storage Threshold Breach subevent.
  static constexpr uint8_t kStorageThresholdBreachNotificationPercent = 75;

  AndroidBatchLowEnergyScanner(
      LocalAddressDelegate* local_addr_delegate,
      const AdvertisingPacketFilter::Config& packet_filter_config,
      Transport::WeakPtr transport,
      pw::async::Dispatcher& pw_dispatcher,
      std::optional<
          std::reference_wrapper<pw::bluetooth_sapphire::WakeAlarmProvider>>
          wake_alarm_provider,
      pw::chrono::SystemClock::duration max_read_delay = kDefaultMaxReadDelay);
  ~AndroidBatchLowEnergyScanner() override;

  bool StartScan(const ScanOptions& options,
                 ScanStatusCallback callback) override;

  bool StopScan() override;

 private:
  // Enqueue the packets necessary to start a scan to the hci_cmd_runner().
  bool EnqueueStartScanPackets(const DeviceAddress& local_address,
                               const ScanOptions& options) override;

  // Enqueue the packets necessary to stop a scan to the hci_cmd_runner().
  void EnqueueStopScanPackets() override;

  // Build the HCI command packet to enable scanning for the flavor of low
  // energy scanning being implemented.
  CommandPacket BuildEnablePacket(
      const ScanOptions& options,
      pw::bluetooth::emboss::GenericEnableParam enable) const override;

  // Build the HCI command packet to set the scan parameters for the flavor of
  // low energy scanning being implemented.
  std::optional<CommandPacket> BuildSetScanParametersPacket(
      const DeviceAddress& local_address,
      const ScanOptions& options) const override;

  // Build the HCI command packet to read the scan results that are currently
  // stored in Controller memory.
  CommandPacket BuildReadScanResultsPacket() const;

  // Parse out the scan results returned by the Controller
  std::vector<pw::bluetooth::vendor::android_hci::LEBatchScanFullResultView>
  ParseScanResults(const pw::bluetooth::vendor::android_hci::
                       LEBatchScanReadResultsCommandCompleteEventView& view);

  // Handle scan results sent by the Controller by notifying the delegate of any
  // peers found.
  void HandleScanResults(
      const std::vector<
          pw::bluetooth::vendor::android_hci::LEBatchScanFullResultView>&
          results);

  // Event handler for the Storage Threshold Breach subevent
  void ReadScanResults(
      std::optional<pw::bluetooth_sapphire::Lease> wake_lease = std::nullopt);

  void SendReadCommand(std::optional<pw::bluetooth_sapphire::Lease> lease);

  // Event handler ID for the Storage Threshold Breach subevent
  CommandChannel::EventHandlerId event_handler_id_;

  void ScheduleNextRead();

  void HandleScanStatus(ScanStatus status);

  std::optional<
      std::reference_wrapper<pw::bluetooth_sapphire::WakeAlarmProvider>>
      wake_alarm_provider_;
  std::optional<pw::bluetooth_sapphire::WakeAlarm> wake_alarm_;

  // Task that periodically reads scan results from the Controller
  SmartTask read_scan_results_task_;

  pw::chrono::SystemClock::duration max_read_delay_;

  // Keep this as the last member to make sure that all weak pointers are
  // invalidated before other members get destroyed
  WeakSelf<AndroidBatchLowEnergyScanner> weak_self_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(AndroidBatchLowEnergyScanner);
};

}  // namespace bt::hci
