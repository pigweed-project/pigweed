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

#pragma once

#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_scanner.h"

namespace bt::hci {

class LocalAddressDelegate;

// LegacyLowEnergyScanner implements the LowEnergyScanner interface for
// controllers that do not support the 5.0 Extended Advertising feature. This
// uses the legacy HCI LE scan commands and events:
//
//     - HCI_LE_Set_Scan_Parameters
//     - HCI_LE_Set_Scan_Enable
//     - HCI_LE_Advertising_Report event
class LegacyLowEnergyScanner final : public LowEnergyScanner {
 public:
  LegacyLowEnergyScanner(
      LocalAddressDelegate* local_addr_delegate,
      const AdvertisingPacketFilter::Config& packet_filter_config,
      Transport::WeakPtr transport,
      pw::async::Dispatcher& pw_dispatcher);
  ~LegacyLowEnergyScanner() override;

  bool StartScan(const ScanOptions& options,
                 ScanStatusCallback callback) override;

  // Parse address field from |report| into |out_addr| and return whether or not
  // it is a resolved address in |out_resolved|. Returns false if the address
  // field was invalid.
  static bool DeviceAddressFromAdvReport(
      const pw::bluetooth::emboss::LEAdvertisingReportDataView& report,
      DeviceAddress* out_addr,
      bool* out_resolved);

 private:
  // Build the HCI command packet to set the scan parameters for the flavor of
  // low energy scanning being implemented.
  std::optional<CommandPacket> BuildSetScanParametersPacket(
      const DeviceAddress& local_address,
      const ScanOptions& options) const override;

  // Build the HCI command packet to enable scanning for the flavor of low
  // energy scanning being implemented.
  CommandPacket BuildEnablePacket(
      const ScanOptions& options,
      pw::bluetooth::emboss::GenericEnableParam enable) const override;

  // Called when a Scan Response is received during an active scan or when we
  // time out waiting
  void HandleScanResponse(const DeviceAddress& address,
                          bool resolved,
                          int8_t rssi,
                          const ByteBuffer& data);

  std::vector<pw::bluetooth::emboss::LEAdvertisingReportDataView>
  ParseAdvertisingReports(const EventPacket& event);

  // Event handler for HCI LE Advertising Report event.
  void OnAdvertisingReportEvent(const EventPacket& event);

  // Our event handler ID for the LE Advertising Report event.
  CommandChannel::EventHandlerId event_handler_id_;

  // Keep this as the last member to make sure that all weak pointers are
  // invalidated before other members get destroyed
  WeakSelf<LegacyLowEnergyScanner> weak_self_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LegacyLowEnergyScanner);
};

}  // namespace bt::hci
