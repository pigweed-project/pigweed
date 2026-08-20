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

#include "pw_bluetooth/hci_android.emb.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/vendor_protocol.h"

namespace bt::hci {

namespace android_hci = hci_spec::vendor::android;
namespace android_emb = pw::bluetooth::vendor::android_hci;
namespace pwemb = pw::bluetooth::emboss;

AndroidBatchLowEnergyScanner::AndroidBatchLowEnergyScanner(
    LocalAddressDelegate* local_addr_delegate,
    const AdvertisingPacketFilter::Config& packet_filter_config,
    Transport::WeakPtr transport,
    pw::async::Dispatcher& pw_dispatcher,
    std::optional<
        std::reference_wrapper<pw::bluetooth_sapphire::WakeAlarmProvider>>
        wake_alarm_provider,
    pw::chrono::SystemClock::duration max_read_delay)
    : LowEnergyScanner(local_addr_delegate,
                       packet_filter_config,
                       std::move(transport),
                       pw_dispatcher),
      wake_alarm_provider_(wake_alarm_provider),
      read_scan_results_task_(dispatcher()),
      max_read_delay_(max_read_delay),
      weak_self_(this) {
  auto self = weak_self_.GetWeakPtr();

  event_handler_id_ = hci()->command_channel()->AddVendorEventHandler(
      android_hci::kStorageThresholdBreachSubeventCode,
      [self]([[maybe_unused]] const EventPacket& event_packet) {
        if (!self.is_alive()) {
          return hci::CommandChannel::EventCallbackResult::kRemove;
        }

        self->ReadScanResults();
        return CommandChannel::EventCallbackResult::kContinue;
      });

  read_scan_results_task_.set_function(
      [self](pw::async::Context /*ctx*/, pw::Status status) {
        if (!status.ok() || !self.is_alive()) {
          return;
        }

        if (self->IsScanning() || self->IsInitiating()) {
          self->ReadScanResults();
        }
      });
}

AndroidBatchLowEnergyScanner::~AndroidBatchLowEnergyScanner() {
  if (!hci().is_alive() || !hci()->command_channel()) {
    return;
  }

  hci()->command_channel()->RemoveEventHandler(event_handler_id_);
}

bool AndroidBatchLowEnergyScanner::StartScan(const ScanOptions& options,
                                             ScanStatusCallback callback) {
  auto self = weak_self_.GetWeakPtr();
  auto wrapped_cb = [self, cb = std::move(callback)](ScanStatus status) {
    if (self.is_alive()) {
      self->HandleScanStatus(status);
    }
    cb(status);
  };

  return LowEnergyScanner::StartScan(options, std::move(wrapped_cb));
}

bool AndroidBatchLowEnergyScanner::StopScan() {
  bool result = LowEnergyScanner::StopScan();
  if (result) {
    read_scan_results_task_.Cancel();
    wake_alarm_.reset();
  }
  return result;
}

bool AndroidBatchLowEnergyScanner::EnqueueStartScanPackets(
    const DeviceAddress& local_address, const ScanOptions& options) {
  std::optional<CommandPacket> scan_params_command =
      BuildSetScanParametersPacket(local_address, options);
  if (!scan_params_command.has_value()) {
    return false;
  }
  hci_cmd_runner().QueueCommand(std::move(*scan_params_command));
  return true;
}

void AndroidBatchLowEnergyScanner::EnqueueStopScanPackets() {
  std::optional<CommandPacket> scan_params_command =
      BuildSetScanParametersPacket(
          DeviceAddress(DeviceAddress::Type::kLEPublic, DeviceAddressBytes()),
          ScanOptions());
  PW_CHECK(scan_params_command.has_value());
  hci_cmd_runner().QueueCommand(std::move(*scan_params_command));
}

CommandPacket AndroidBatchLowEnergyScanner::BuildEnablePacket(
    [[maybe_unused]] const ScanOptions& options,
    pwemb::GenericEnableParam enable) const {
  auto packet =
      hci::CommandPacket::New<android_emb::LEBatchScanEnableCommandWriter>(
          android_hci::kLEBatchScan);

  auto view = packet.view_t();
  view.vendor_command().sub_opcode().Write(
      android_hci::kLEBatchScanEnableSubopcode);
  view.enabled().Write(enable);

  return packet;
}

std::optional<CommandPacket>
AndroidBatchLowEnergyScanner::BuildSetScanParametersPacket(
    const DeviceAddress& local_address, const ScanOptions& options) const {
  if (local_address.type() != DeviceAddress::Type::kLERandom &&
      local_address.type() != DeviceAddress::Type::kLEPublic) {
    return std::nullopt;
  }

  auto packet = hci::CommandPacket::New<
      android_emb::LEBatchScanSetScanParametersCommandWriter>(
      android_hci::kLEBatchScan);

  auto view = packet.view_t();
  view.vendor_command().sub_opcode().Write(
      android_hci::kLEBatchScanSetScanParametersSubopcode);
  view.window().Write(options.window);
  view.interval().Write(options.interval);
  view.discard_rule().Write(android_emb::BatchScanDiscardRule::DISCARD_OLDEST);

  bool is_starting_scan = false;
  if (state() == LowEnergyScanner::State::kInitiating) {
    is_starting_scan = true;
  }
  view.full_mode_enabled().Write(is_starting_scan);

  if (local_address.type() == DeviceAddress::Type::kLERandom) {
    view.own_address_type().Write(android_emb::BatchScanOwnAddressType::RANDOM);
  } else {
    view.own_address_type().Write(android_emb::BatchScanOwnAddressType::PUBLIC);
  }

  return packet;
}

CommandPacket AndroidBatchLowEnergyScanner::BuildReadScanResultsPacket() const {
  auto packet =
      hci::CommandPacket::New<android_emb::LEBatchScanReadResultsCommandWriter>(
          android_hci::kLEBatchScan);

  auto view = packet.view_t();
  view.vendor_command().sub_opcode().Write(
      android_hci::kLEBatchScanReadResultParametersSubopcode);
  view.read_mode().Write(android_emb::BatchScanReadMode::FULL);

  return packet;
}

std::vector<android_emb::LEBatchScanFullResultView>
AndroidBatchLowEnergyScanner::ParseScanResults(
    const android_emb::LEBatchScanReadResultsCommandCompleteEventView& view) {
  uint8_t num_records = view.num_records().Read();
  std::vector<android_emb::LEBatchScanFullResultView> records;
  records.reserve(num_records);

  size_t bytes_read = 0;
  const size_t total_bytes = view.full_results().BackingStorage().SizeInBytes();
  const uint8_t* base_ptr = view.full_results().BackingStorage().begin();

  while (bytes_read < total_bytes) {
    size_t bytes_left = total_bytes - bytes_read;
    size_t min_size = android_emb::LEBatchScanFullResult::MinSizeInBytes();

    if (bytes_left < min_size) {
      bt_log(WARN,
             "hci-le",
             "parsing batched scan results, not enough bytes left for header "
             "(needed %zu, got %zu)",
             min_size,
             bytes_left);
      break;
    }

    auto up_to_advertising_data = android_emb::MakeLEBatchScanFullResultView(
        base_ptr + bytes_read, min_size);

    uint8_t advertising_data_length =
        up_to_advertising_data.advertising_data_length().Read();

    if (bytes_left < min_size + advertising_data_length) {
      bt_log(WARN,
             "hci-le",
             "parsing batched scan results, not enough bytes left for "
             "advertising data "
             "(needed %zu, got %zu)",
             min_size + advertising_data_length,
             bytes_left);
      break;
    }

    size_t size_with_adv = min_size + advertising_data_length;
    auto up_to_scan_response = android_emb::MakeLEBatchScanFullResultView(
        base_ptr + bytes_read, size_with_adv);

    uint8_t scan_response_length =
        up_to_scan_response.scan_response_length().Read();
    size_t actual_size = size_with_adv + scan_response_length;

    if (actual_size > bytes_left) {
      bt_log(WARN,
             "hci-le",
             "parsing batched scan results, next record size %zu bytes, but "
             "only %zu bytes left",
             actual_size,
             bytes_left);
      break;
    }

    auto record = android_emb::MakeLEBatchScanFullResultView(
        base_ptr + bytes_read, actual_size);
    records.push_back(record);

    bytes_read += actual_size;
  }

  return records;
}

// Returns a DeviceAddress and whether or not that DeviceAddress has been
// resolved
static std::optional<std::tuple<DeviceAddress, bool>> BuildDeviceAddress(
    pwemb::LEAddressType report_type, pwemb::BdAddrView address_view) {
  std::optional<DeviceAddress::Type> address_type =
      DeviceAddress::LeAddrToDeviceAddr(report_type);
  if (!address_type.has_value()) {
    return std::nullopt;
  }

  bool resolved = false;
  switch (report_type) {
    case pwemb::LEAddressType::PUBLIC_IDENTITY:
    case pwemb::LEAddressType::RANDOM_IDENTITY:
      resolved = true;
      break;
    case pwemb::LEAddressType::PUBLIC:
    case pwemb::LEAddressType::RANDOM:
    default:
      resolved = false;
      break;
  }

  DeviceAddress address =
      DeviceAddress(*address_type, DeviceAddressBytes(address_view));
  return std::make_tuple(address, resolved);
}

void AndroidBatchLowEnergyScanner::HandleScanResults(
    const std::vector<android_emb::LEBatchScanFullResultView>& results) {
  for (const auto& result : results) {
    std::optional<std::tuple<DeviceAddress, bool>> address_result =
        BuildDeviceAddress(result.peer_address_type().Read(),
                           result.peer_address());
    if (!address_result.has_value()) {
      bt_log(
          WARN, "hci-le", "invalid device address type in batch scan result");
      continue;
    }
    const auto& [address, resolved] = *address_result;

    // The Android vendor extensions don't provide a way to access the event
    // type from the scan result. For now, we assume the advertisement was
    // connectable.
    bool connectable = true;

    LowEnergyScanResult scan_result(address, resolved, connectable);
    scan_result.set_tx_power(result.tx_power().Read());
    scan_result.set_rssi(result.rssi().Read());

    scan_result.AppendData(
        BufferView(result.advertising_data().BackingStorage().data(),
                   result.advertising_data_length().Read()));
    scan_result.AppendData(
        BufferView(result.scan_response_data().BackingStorage().data(),
                   result.scan_response_length().Read()));

    NotifyPeerFound(scan_result);
  }
}

void AndroidBatchLowEnergyScanner::ReadScanResults(
    std::optional<pw::bluetooth_sapphire::Lease> wake_lease) {
  if (!IsScanning()) {
    if (IsInitiating()) {
      ScheduleNextRead();
    }
    return;
  }

  if (!hci_cmd_runner().IsReady()) {
    // Don't cancel the current operation in the command runner. It's probably
    // more important (e.g. starting or stopping a scan) than this one. We can
    // try again later.
    ScheduleNextRead();
    return;
  }

  wake_alarm_.reset();
  SendReadCommand(std::move(wake_lease));
}

void AndroidBatchLowEnergyScanner::SendReadCommand(
    std::optional<pw::bluetooth_sapphire::Lease> wake_lease) {
  if (!IsScanning()) {
    return;
  }

  if (!hci().is_alive() || !hci()->command_channel()) {
    return;
  }

  CommandPacket command = BuildReadScanResultsPacket();
  auto callback = [this, lease = std::move(wake_lease)](
                      CommandChannel::TransactionId /*id*/,
                      const EventPacket& event) mutable {
    if (!IsScanning()) {
      return;
    }

    Result<> result = event.ToResult();
    if (bt_is_error(result, ERROR, "hci-le", "failed reading scan results")) {
      ScheduleNextRead();
      return;
    }

    auto view = event.view<
        android_emb::LEBatchScanReadResultsCommandCompleteEventView>();
    PW_DCHECK(view.read_mode().Read() == android_emb::BatchScanReadMode::FULL);

    uint8_t num_records = view.num_records().Read();
    if (num_records == 0) {
      ScheduleNextRead();
      return;
    }

    auto records = ParseScanResults(view);
    HandleScanResults(records);

    SendReadCommand(std::move(lease));
  };

  auto result = hci()->command_channel()->SendCommand(std::move(command),
                                                      std::move(callback));
  if (!result.ok()) {
    bt_log(ERROR,
           "hci-le",
           "failed to send read batch scan results command: %s",
           result.status().str());
    ScheduleNextRead();
  }
}

void AndroidBatchLowEnergyScanner::ScheduleNextRead() {
  read_scan_results_task_.Cancel();

  if (!wake_alarm_provider_.has_value()) {
    read_scan_results_task_.PostAfter(max_read_delay_);
    return;
  }

  auto self = weak_self_.GetWeakPtr();
  auto deadline = dispatcher().now() + max_read_delay_;

  auto result = wake_alarm_provider_->get().Set(
      PW_SAPPHIRE_WAKE_ALARM_TOKEN_EXPR("AndroidBatchLowEnergyScannerRead"),
      deadline,
      [self](pw::Result<pw::bluetooth_sapphire::Lease> lease_result) {
        if (!self.is_alive()) {
          return;
        }

        if (!lease_result.ok()) {
          if (lease_result.status() == pw::Status::Cancelled()) {
            // the scanner explicitly cancels the active wake alarm when it
            // starts reading results (e.g., when a storage threshold breach
            // event is received from the controller). When the alarm is
            // cancelled, its callback is triggered with a cancelled
            // status. This isn't an error and we can safely ignore this.
            return;
          }

          bt_log(ERROR,
                 "hci-le",
                 "wake alarm failed: %s",
                 lease_result.status().str());
          self->read_scan_results_task_.PostAfter(self->max_read_delay_);
          return;
        }

        self->ReadScanResults(std::move(*lease_result));
      });

  if (!result.ok()) {
    if (result.status() == pw::Status::Unimplemented()) {
      static bool logged_unimplemented = false;
      if (!logged_unimplemented) {
        bt_log(INFO,
               "hci-le",
               "wake alarms not supported by provider, using fallback task");
        logged_unimplemented = true;
      }
    } else {
      bt_log(ERROR,
             "hci-le",
             "failed to set wake alarm: %s",
             result.status().str());
    }
    read_scan_results_task_.PostAfter(max_read_delay_);
  } else {
    wake_alarm_ = std::move(*result);
  }
}

void AndroidBatchLowEnergyScanner::HandleScanStatus(ScanStatus status) {
  switch (status) {
    case ScanStatus::kComplete:
    case ScanStatus::kStopped:
    case ScanStatus::kFailed:
      read_scan_results_task_.Cancel();
      wake_alarm_.reset();
      break;
    case ScanStatus::kActive:
    case ScanStatus::kPassive:
      ScheduleNextRead();
      break;
  }
}
}  // namespace bt::hci
