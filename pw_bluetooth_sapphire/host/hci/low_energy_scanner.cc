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

#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_scanner.h"

#include <pw_assert/check.h>

namespace bt::hci {

static std::string ScanStateToString(LowEnergyScanner::State state) {
  switch (state) {
    case LowEnergyScanner::State::kIdle:
      return "(idle)";
    case LowEnergyScanner::State::kStopping:
      return "(stopping)";
    case LowEnergyScanner::State::kInitiating:
      return "(initiating)";
    case LowEnergyScanner::State::kActiveScanning:
      return "(active scanning)";
    case LowEnergyScanner::State::kPassiveScanning:
      return "(passive scanning)";
    default:
      break;
  }

  PW_CRASH("invalid scanner state: %u", static_cast<unsigned int>(state));
  return "(unknown)";
}

void LowEnergyScanResult::AppendData(const ByteBuffer& data) {
  size_t bytes_needed = data_size_ + data.size();
  if (buffer_.size() < bytes_needed) {
    buffer_.expand(bytes_needed);
  }

  buffer_.Write(data, data_size_);
  data_size_ += data.size();
}

LowEnergyScanResult& LowEnergyScanResult::operator=(
    const LowEnergyScanResult& other) {
  address_ = other.address_;
  resolved_ = other.resolved_;
  connectable_ = other.connectable_;
  rssi_ = other.rssi_;
  advertising_sid_ = other.advertising_sid_;
  periodic_advertising_interval_ = other.periodic_advertising_interval_;
  data_size_ = other.data_size_;

  if (buffer_.size() < other.buffer_.size()) {
    buffer_.expand(other.buffer_.size());
  }
  other.buffer_.Copy(&buffer_);

  return *this;
}

LowEnergyScanner::PendingScanResult::PendingScanResult(
    LowEnergyScanResult&& result,
    pw::async::Dispatcher& dispatcher,
    pw::chrono::SystemClock::duration timeout,
    fit::closure timeout_handler)
    : result_(result), timeout_(timeout), timeout_task_(dispatcher) {
  timeout_task_.set_function(
      [timeout_handler_cb = std::move(timeout_handler)](
          pw::async::Context /*ctx*/, pw::Status status) {
        if (status.ok()) {
          timeout_handler_cb();
        }
      });
  StartTimer();
}

LowEnergyScanner::LowEnergyScanner(
    LocalAddressDelegate* local_addr_delegate,
    const AdvertisingPacketFilter::Config& packet_filter_config,
    Transport::WeakPtr hci,
    pw::async::Dispatcher& pw_dispatcher)
    : pw_dispatcher_(pw_dispatcher),
      scan_timeout_task_(pw_dispatcher_),
      packet_filter_(packet_filter_config, hci->GetWeakPtr()),
      local_addr_delegate_(local_addr_delegate),
      hci_(std::move(hci)) {
  PW_DCHECK(local_addr_delegate_);
  PW_DCHECK(hci_.is_alive());

  hci_cmd_runner_ = std::make_unique<SequentialCommandRunner>(
      hci_->command_channel()->AsWeakPtr());

  scan_timeout_task_.set_function(
      [this](pw::async::Context /*ctx*/, pw::Status status) {
        if (status.ok() && IsScanning()) {
          StopScanInternal(false);
        }
      });
}

void LowEnergyScanner::AddPendingResult(LowEnergyScanResult&& scan_result) {
  auto pending = std::make_unique<PendingScanResult>(
      std::move(scan_result),
      pw_dispatcher_,
      scan_response_timeout_,
      [this, address = scan_result.address()] {
        std::unique_ptr<PendingScanResult> result =
            RemovePendingResult(address);
        NotifyPeerFound(result->result());
      });
  pending_results_.emplace(scan_result.address(), std::move(pending));
}

std::unique_ptr<LowEnergyScanner::PendingScanResult>
LowEnergyScanner::RemovePendingResult(const DeviceAddress& address) {
  auto node = pending_results_.extract(address);
  if (node.empty()) {
    return nullptr;
  }

  node.mapped()->CancelTimeout();
  return std::move(node.mapped());
}

void LowEnergyScanner::SetPacketFilters(
    uint16_t scan_id, const std::vector<DiscoveryFilter>& filters) {
  packet_filter_.SetPacketFilters(scan_id, filters);
}

void LowEnergyScanner::UnsetPacketFilters(uint16_t scan_id) {
  packet_filter_.UnsetPacketFilters(scan_id);
}

void LowEnergyScanner::NotifyCachedPeers(uint16_t scan_id) {
  for (const LowEnergyScanResult& result : cached_scan_results_) {
    AdvertisingData::ParseResult ad = AdvertisingData::FromBytes(result.data());
    bool connectable = result.connectable();
    int8_t rssi = result.rssi();

    if (packet_filter_.Matches(scan_id, ad, connectable, rssi)) {
      delegate()->OnPeerFound({scan_id}, result);
    }
  }
}

void LowEnergyScanner::NotifyPeerFound(const LowEnergyScanResult& result) {
  bt_log(DEBUG,
         "hci-le",
         "peer found (address: %s, connectable: %d)",
         bt_str(result.address()),
         result.connectable());

  cached_scan_results_.push_back(result);

  AdvertisingData::ParseResult ad = AdvertisingData::FromBytes(result.data());
  std::unordered_set<uint16_t> scan_ids =
      packet_filter_.Matches(ad, result.connectable(), result.rssi());

  if (!scan_ids.empty()) {
    delegate()->OnPeerFound(scan_ids, result);
  }
}

bool LowEnergyScanner::StartScan(const ScanOptions& options,
                                 ScanStatusCallback callback) {
  PW_CHECK(callback);
  PW_CHECK(options.window < options.interval);

  if (state_ != State::kIdle) {
    bt_log(ERROR,
           "hci-le",
           "cannot start scan while in state: %s",
           ScanStateToString(state_).c_str());
    return false;
  }

  state_ = State::kInitiating;
  scan_response_timeout_ = options.scan_response_timeout;
  scan_cb_ = std::move(callback);

  // Obtain the local address type.
  local_addr_delegate_->EnsureLocalAddress(
      /*address_type=*/std::nullopt,
      [this, options, cb = std::move(callback)](
          fit::result<HostError, const DeviceAddress> result) mutable {
        if (result.is_error()) {
          cb(ScanStatus::kFailed);
          return;
        }
        StartScanInternal(result.value(), options, std::move(cb));
      });

  return true;
}

void LowEnergyScanner::StartScanInternal(const DeviceAddress& local_address,
                                         const ScanOptions& options,
                                         ScanStatusCallback) {
  // Check if the scan request was canceled by StopScan() while we were waiting
  // for the local address.
  if (state_ != State::kInitiating) {
    bt_log(DEBUG,
           "hci-le",
           "scan request was canceled while obtaining local address");
    return;
  }

  bt_log(DEBUG,
         "hci-le",
         "requesting scan (%s, address: %s, interval: %#.4x, window: %#.4x)",
         (options.active ? "active" : "passive"),
         local_address.ToString().c_str(),
         options.interval,
         options.window);

  CommandPacket scan_params_command =
      BuildSetScanParametersPacket(local_address, options);
  CommandPacket scan_enable_command = BuildEnablePacket(
      options, pw::bluetooth::emboss::GenericEnableParam::ENABLE);

  hci_cmd_runner_->QueueCommand(std::move(scan_params_command));
  hci_cmd_runner_->QueueCommand(std::move(scan_enable_command));
  hci_cmd_runner_->RunCommands([this,
                                active = options.active,
                                period = options.period](Result<> status) {
    PW_DCHECK(scan_cb_);
    PW_DCHECK(state_ == State::kInitiating);

    if (status.is_error()) {
      if (status == ToResult(HostError::kCanceled)) {
        bt_log(DEBUG, "hci-le", "scan canceled");
        return;
      }

      bt_log(ERROR, "hci-le", "failed to start scan: %s", bt_str(status));
      state_ = State::kIdle;
      scan_cb_(ScanStatus::kFailed);
      return;
    }

    // Schedule the timeout.
    if (period != kPeriodInfinite) {
      scan_timeout_task_.PostAfter(period);
    }

    if (active) {
      state_ = State::kActiveScanning;
      scan_cb_(ScanStatus::kActive);
    } else {
      state_ = State::kPassiveScanning;
      scan_cb_(ScanStatus::kPassive);
    }
  });
}

bool LowEnergyScanner::StopScan() {
  if (state_ == State::kStopping || state_ == State::kIdle) {
    bt_log(DEBUG,
           "hci-le",
           "cannot stop scan while in state: %s",
           ScanStateToString(state_).c_str());
    return false;
  }

  // Scan is either being initiated or already running. Cancel any in-flight HCI
  // command sequence.
  if (!hci_cmd_runner_->IsReady()) {
    hci_cmd_runner_->Cancel();
  }

  // We'll tell the controller to stop scanning even if it is not (this is OK
  // because the command will have no effect; see Core Spec v5.0, Vol 2, Part E,
  // Section 7.8.11, paragraph 4).
  StopScanInternal(true);
  return true;
}

void LowEnergyScanner::StopScanInternal(bool stopped_by_user) {
  PW_DCHECK(scan_cb_);

  scan_timeout_task_.Cancel();
  state_ = State::kStopping;

  // Notify any pending scan results unless the scan was terminated by the user.
  if (!stopped_by_user) {
    for (auto& result : pending_results_) {
      const std::unique_ptr<PendingScanResult>& pending = result.second;
      NotifyPeerFound(pending->result());
    }
  }

  // Either way clear all results from the previous scan period.
  pending_results_.clear();
  cached_scan_results_.clear();

  PW_DCHECK(hci_cmd_runner_->IsReady());

  // Tell the controller to stop scanning.
  ScanOptions options;
  CommandPacket command = BuildEnablePacket(
      options, pw::bluetooth::emboss::GenericEnableParam::DISABLE);

  hci_cmd_runner_->QueueCommand(std::move(command));
  hci_cmd_runner_->RunCommands([this, stopped_by_user](Result<> status) {
    PW_DCHECK(scan_cb_);
    PW_DCHECK(state_ == State::kStopping);
    state_ = State::kIdle;

    // Something went wrong but there isn't really a meaningful way to recover,
    // so we just fall through and notify the caller with ScanStatus::kFailed
    // instead.
    bt_is_error(
        status, WARN, "hci-le", "failed to stop scan: %s", bt_str(status));

    ScanStatus scan_status = ScanStatus::kFailed;
    if (status.is_error()) {
      scan_status = ScanStatus::kFailed;
    } else if (stopped_by_user) {
      scan_status = ScanStatus::kStopped;
    } else {
      scan_status = ScanStatus::kComplete;
    }

    scan_cb_(scan_status);
  });
}
}  // namespace bt::hci
