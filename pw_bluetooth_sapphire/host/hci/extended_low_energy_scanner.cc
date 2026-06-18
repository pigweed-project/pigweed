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

#include <pw_assert/check.h>
#include <pw_bluetooth/hci_common.emb.h>

#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_scanner.h"

namespace bt::hci {

using pw::bluetooth::emboss::BdAddrView;
using pw::bluetooth::emboss::GenericEnableParam;
using pw::bluetooth::emboss::LEAdvertisingDataStatus;
using pw::bluetooth::emboss::LEExtendedAddressType;
using pw::bluetooth::emboss::LEExtendedAdvertisingReportDataView;
using pw::bluetooth::emboss::LEExtendedAdvertisingReportSubeventView;
using pw::bluetooth::emboss::LEExtendedDuplicateFilteringOption;
using pw::bluetooth::emboss::LEMetaEventView;
using pw::bluetooth::emboss::LEScanType;
using pw::bluetooth::emboss::LESetExtendedScanEnableCommandWriter;
using pw::bluetooth::emboss::LESetExtendedScanParametersCommandWriter;
using pw::bluetooth::emboss::MakeLEExtendedAdvertisingReportDataView;

ExtendedLowEnergyScanner::ExtendedLowEnergyScanner(
    LocalAddressDelegate* local_addr_delegate,
    const AdvertisingPacketFilter::Config& packet_filter_config,
    Transport::WeakPtr transport,
    pw::async::Dispatcher& pw_dispatcher)
    : LowEnergyScanner(local_addr_delegate,
                       packet_filter_config,
                       std::move(transport),
                       pw_dispatcher) {
  event_handler_id_ = hci()->command_channel()->AddLEMetaEventHandler(
      hci_spec::kLEExtendedAdvertisingReportSubeventCode,
      [this](const EventPacket& event) {
        OnExtendedAdvertisingReportEvent(event);
        return hci::CommandChannel::EventCallbackResult::kContinue;
      });
}
ExtendedLowEnergyScanner::~ExtendedLowEnergyScanner() {
  // This object is probably being destroyed because the stack is shutting down,
  // in which case the HCI layer may have already been destroyed.
  if (!hci().is_alive() || !hci()->command_channel()) {
    return;
  }

  hci()->command_channel()->RemoveEventHandler(event_handler_id_);
  StopScan();
}

bool ExtendedLowEnergyScanner::StartScan(const ScanOptions& options,
                                         ScanStatusCallback callback) {
  PW_CHECK(options.interval >= hci_spec::kLEExtendedScanIntervalMin);
  PW_CHECK(options.interval <= hci_spec::kLEExtendedScanIntervalMax);
  PW_CHECK(options.window >= hci_spec::kLEExtendedScanIntervalMin);
  PW_CHECK(options.window <= hci_spec::kLEExtendedScanIntervalMax);
  return LowEnergyScanner::StartScan(options, std::move(callback));
}

CommandPacket ExtendedLowEnergyScanner::BuildSetScanParametersPacket(
    const DeviceAddress& local_address, const ScanOptions& options) const {
  // LESetExtendedScanParametersCommand contains a variable amount of data,
  // depending on how many bits are set within the scanning_phys parameter. As
  // such, we must first calculate the size of the variable data before
  // allocating the packet.

  // we scan on the LE 1M PHY and the LE Coded PHY
  constexpr size_t num_phys = 2;
  constexpr size_t fixed_size = pw::bluetooth::emboss::
      LESetExtendedScanParametersCommand::MinSizeInBytes();
  constexpr size_t variable_size = pw::bluetooth::emboss::
      LESetExtendedScanParametersData::IntrinsicSizeInBytes();
  constexpr size_t packet_size = fixed_size + (num_phys * variable_size);

  std::optional<pw::bluetooth::emboss::LEOwnAddressType> own_address_type =
      DeviceAddress::DeviceAddrToLeOwnAddr(local_address.type());
  PW_CHECK(own_address_type.has_value());

  auto packet =
      hci::CommandPacket::New<LESetExtendedScanParametersCommandWriter>(
          hci_spec::kLESetExtendedScanParameters, packet_size);
  auto params = packet.view_t();

  params.scanning_filter_policy().Write(options.filter_policy);
  params.own_address_type().Write(*own_address_type);

  // For maximum compatibility, Sapphire scans on all available PHYs.
  params.scanning_phys().le_1m().Write(true);
  params.scanning_phys().le_coded().Write(true);

  for (size_t i = 0; i < num_phys; i++) {
    params.data()[i].scan_type().Write(LEScanType::PASSIVE);
    if (options.active) {
      params.data()[i].scan_type().Write(LEScanType::ACTIVE);
    }

    params.data()[i].scan_interval().Write(options.interval);
    params.data()[i].scan_window().Write(options.window);
  }

  return packet;
}

CommandPacket ExtendedLowEnergyScanner::BuildEnablePacket(
    const ScanOptions& options, GenericEnableParam enable) const {
  auto packet = CommandPacket::New<LESetExtendedScanEnableCommandWriter>(
      hci_spec::kLESetExtendedScanEnable);
  auto params = packet.view_t();

  params.scanning_enabled().Write(enable);
  params.filter_duplicates().Write(
      LEExtendedDuplicateFilteringOption::DISABLED);
  if (options.filter_duplicates) {
    params.filter_duplicates().Write(
        LEExtendedDuplicateFilteringOption::ENABLED);
  }

  // The scan duration and period parameters control how long the scan
  // continues. Setting these values to hci_spec::kNoScanningDuration and
  // hci_spec::kNoScanningPeriod, respectively, means that scanning continues
  // indefinitely until the client requests it to stop.
  params.duration().Write(hci_spec::kNoScanningDuration);
  params.period().Write(hci_spec::kNoScanningPeriod);

  return packet;
}

// Extract all advertising reports from a given HCI LE Extended Advertising
// Report event
std::vector<LEExtendedAdvertisingReportDataView>
ExtendedLowEnergyScanner::ParseAdvertisingReports(const EventPacket& event) {
  PW_DCHECK(event.event_code() == hci_spec::kLEMetaEventCode);
  PW_DCHECK(event.view<LEMetaEventView>().subevent_code().Read() ==
            hci_spec::kLEExtendedAdvertisingReportSubeventCode);
  auto params = event.view<LEExtendedAdvertisingReportSubeventView>();

  uint8_t num_reports = params.num_reports().Read();
  std::vector<LEExtendedAdvertisingReportDataView> reports;
  reports.reserve(num_reports);

  size_t bytes_read = 0;
  while (bytes_read < params.reports().BackingStorage().SizeInBytes()) {
    size_t min_size = pw::bluetooth::emboss::LEExtendedAdvertisingReportData::
        MinSizeInBytes();
    auto report_prefix = MakeLEExtendedAdvertisingReportDataView(
        params.reports().BackingStorage().begin() + bytes_read, min_size);

    uint8_t data_length = report_prefix.data_length().Read();
    size_t actual_size = min_size + data_length;

    size_t bytes_left =
        params.reports().BackingStorage().SizeInBytes() - bytes_read;
    if (actual_size > bytes_left) {
      bt_log(WARN,
             "hci-le",
             "parsing advertising reports, next report size %zu bytes, but "
             "only %zu bytes left",
             actual_size,
             bytes_left);
      break;
    }

    auto report = MakeLEExtendedAdvertisingReportDataView(
        params.reports().BackingStorage().begin() + bytes_read, actual_size);
    reports.push_back(report);

    bytes_read += actual_size;
  }

  return reports;
}

static std::optional<std::tuple<DeviceAddress, bool>> BuildDeviceAddress(
    LEExtendedAddressType report_type, BdAddrView address_view) {
  std::optional<DeviceAddress::Type> address_type =
      DeviceAddress::LeAddrToDeviceAddr(report_type);
  if (!address_type.has_value()) {
    return std::nullopt;
  }

  bool resolved = false;
  switch (report_type) {
    case LEExtendedAddressType::PUBLIC_IDENTITY:
    case LEExtendedAddressType::RANDOM_IDENTITY:
      resolved = true;
      break;
    case LEExtendedAddressType::PUBLIC:
    case LEExtendedAddressType::RANDOM:
    case LEExtendedAddressType::ANONYMOUS:
    default:
      resolved = false;
      break;
  }

  DeviceAddress address =
      DeviceAddress(*address_type, DeviceAddressBytes(address_view));
  return std::make_tuple(address, resolved);
}

void ExtendedLowEnergyScanner::OnExtendedAdvertisingReportEvent(
    const EventPacket& event) {
  if (!IsScanning()) {
    return;
  }

  std::vector<LEExtendedAdvertisingReportDataView> reports =
      ParseAdvertisingReports(event);
  for (LEExtendedAdvertisingReportDataView report : reports) {
    auto address_opt =
        BuildDeviceAddress(report.address_type().Read(), report.address());
    if (!address_opt.has_value()) {
      bt_log(WARN,
             "hci-le",
             "ignoring advertising report: invalid address type (type: 0x%02x)",
             static_cast<uint8_t>(report.address_type().Read()));
      continue;
    }
    const auto& [address, resolved] = *address_opt;

    bool is_directed = report.event_type().directed().Read();
    bool is_connectable = report.event_type().connectable().Read();
    bool is_scannable = report.event_type().scannable().Read();
    bool is_scan_response = report.event_type().scan_response().Read();

    // scan responses without a pending result from an advertising data result
    // mean they are too late and the timer waiting for them has expired. The
    // delegate has already been notified and we unfortunately need to drop this
    // result.
    if (is_scan_response && !HasPendingResult(address)) {
      bt_log(DEBUG, "hci-le", "dropping unmatched scan response");
      return;
    }

    int8_t rssi = report.rssi().Read();
    BufferView data(report.data().BackingStorage().begin(),
                    report.data_length().Read());

    LowEnergyScanResult result;
    std::unique_ptr<PendingScanResult> pending = RemovePendingResult(address);
    if (pending) {
      result = pending->result();
    } else {
      result = LowEnergyScanResult(address, resolved, is_connectable);
    }

    result.set_resolved(resolved);
    result.set_rssi(rssi);
    result.set_tx_power(report.tx_power().Read());
    result.set_advertising_sid(report.advertising_sid().Read());
    result.set_periodic_advertising_interval(
        report.periodic_advertising_interval().Read());

    // If the next set of data exceeds the maximum allowed in an extended
    // advertising data payload, take as much as we can and report it back.
    size_t size_after_add = result.data().size() + data.size();
    if (size_after_add > hci_spec::kMaxLEExtendedAdvertisingDataLength) {
      bt_log(WARN,
             "hci-le",
             "advertising data for (%s) too long (actual: %zu, max: %zu)! "
             "Ignoring rest.",
             bt_str(address),
             size_after_add,
             hci_spec::kMaxLEExtendedAdvertisingDataLength);

      size_t bytes_allowed =
          hci_spec::kMaxLEExtendedAdvertisingDataLength - result.data().size();
      BufferView truncated_data =
          BufferView(report.data().BackingStorage().begin(), bytes_allowed);
      result.AppendData(truncated_data);

      NotifyPeerFound(result);
      continue;
    }

    result.AppendData(data);

    LEAdvertisingDataStatus data_status =
        report.event_type().data_status().Read();
    if (data_status == LEAdvertisingDataStatus::INCOMPLETE) {
      // There is more data coming in another extended advertising PDU so we
      // just wait for it
      AddPendingResult(std::move(result));
      continue;
    }

    // Incoming data was truncated and we won't receive the rest. Nothing we can
    // do about that so just notify the delegate with the data we currently
    // have.
    if (data_status == LEAdvertisingDataStatus::INCOMPLETE_TRUNCATED) {
      bt_log(WARN,
             "hci-le",
             "data for %s truncated to %zu bytes",
             bt_str(address),
             result.data().size());
    }

    if (is_directed) {
      NotifyDirectedAdvertisement(result);
      continue;
    }

    if (IsActiveScanning() && is_scan_response) {
      NotifyPeerFound(result);
      continue;
    }

    if (IsActiveScanning() && is_scannable) {
      // We need to wait for a scan response. Scan responses have the
      // scannable bit set so it's important that this if statement comes
      // after the one checking for a scan response.
      AddPendingResult(std::move(result));
      continue;
    }

    NotifyPeerFound(result);
  }
}

}  // namespace bt::hci
