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

#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_advertiser.h"

#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/internal/host/hci/sequential_command_runner.h"

namespace bt::hci {
namespace pwemb = pw::bluetooth::emboss;

LowEnergyAdvertiser::LowEnergyAdvertiser(hci::Transport::WeakPtr hci,
                                         pw::async::Dispatcher& dispatcher,
                                         uint16_t max_advertising_data_length)
    : hci_(std::move(hci)),
      dispatcher_(dispatcher),
      hci_cmd_runner_(std::make_unique<SequentialCommandRunner>(
          hci_->command_channel()->AsWeakPtr())),
      max_advertising_data_length_(max_advertising_data_length) {}

size_t LowEnergyAdvertiser::GetSizeLimit(
    const AdvertisingEventProperties& properties,
    const AdvertisingOptions& options) const {
  if (!properties.use_legacy_pdus) {
    return max_advertising_data_length_;
  }

  // Core Spec Version 5.4, Volume 6, Part B, Section 2.3.1.2: legacy
  // advertising PDUs that use directed advertising (ADV_DIRECT_IND) don't
  // have an advertising data field in their payloads.
  if (properties.IsDirected()) {
    return 0;
  }

  uint16_t size_limit = hci_spec::kMaxLEAdvertisingDataLength;

  // Core Spec Version 5.4, Volume 6, Part B, Section 2.3, Figure 2.5: Legacy
  // advertising PDUs headers don't have a predesignated field for tx power.
  // Instead, we include it in the Host advertising data itself. Subtract the
  // size it will take up from the allowable remaining data size.
  if (options.include_tx_power_level) {
    size_limit -= kTLVTxPowerLevelSize;
  }

  return size_limit;
}

fit::result<HostError> LowEnergyAdvertiser::CanStartAdvertising(
    const DeviceAddress& address,
    const AdvertisingData& data,
    const AdvertisingData& scan_rsp,
    const AdvertisingOptions& options,
    const ConnectionCallback& connect_callback) const {
  PW_CHECK(address.type() != DeviceAddress::Type::kBREDR);

  if (options.anonymous) {
    bt_log(WARN, "hci-le", "anonymous advertising not supported");
    return fit::error(HostError::kNotSupported);
  }

  uint32_t limit_min = hci_spec::kLEAdvertisingIntervalMin;
  uint32_t limit_max = hci_spec::kLEAdvertisingIntervalMax;
  if (IsExtendedAdvertiser()) {
    limit_min = hci_spec::kLEExtendedAdvertisingIntervalMin;
    limit_max = hci_spec::kLEExtendedAdvertisingIntervalMax;
  }

  if (options.interval.min() < limit_min ||
      options.interval.max() > limit_max ||
      options.interval.min() > options.interval.max()) {
    bt_log(WARN,
           "gap-le",
           "advertising interval out of range: [%#.4x, %#.4x] (range: [%#.4x, "
           "%#.4x])",
           options.interval.min(),
           options.interval.max(),
           limit_min,
           limit_max);
    return fit::error(HostError::kInvalidParameters);
  }

  AdvertisingEventProperties properties =
      GetAdvertisingEventProperties(data, scan_rsp, options, connect_callback);

  // Core Spec Version 5.4, Volume 5, Part E, Section 7.8.53: If extended
  // advertising PDU types are being used then the advertisement shall not be
  // both connectable and scannable.
  if (!properties.use_legacy_pdus && properties.connectable &&
      properties.scannable) {
    bt_log(
        WARN,
        "hci-le",
        "extended advertising pdus cannot be both connectable and scannable");
    return fit::error(HostError::kNotSupported);
  }

  size_t size_limit = GetSizeLimit(properties, options);
  if (size_t size = data.CalculateBlockSize(/*include_flags=*/true);
      size > size_limit) {
    bt_log(WARN,
           "hci-le",
           "advertising data too large (actual: %zu, max: %zu)",
           size,
           size_limit);
    return fit::error(HostError::kAdvertisingDataTooLong);
  }

  if (size_t size = scan_rsp.CalculateBlockSize(/*include_flags=*/false);
      size > size_limit) {
    bt_log(WARN,
           "hci-le",
           "scan response too large (actual: %zu, max: %zu)",
           size,
           size_limit);
    return fit::error(HostError::kScanResponseTooLong);
  }

  return fit::ok();
}

static LowEnergyAdvertiser::AdvertisingEventProperties
GetExtendedAdvertisingEventProperties(
    const AdvertisingData&,
    const AdvertisingData& scan_rsp,
    const LowEnergyAdvertiser::AdvertisingOptions& options,
    const LowEnergyAdvertiser::ConnectionCallback& connect_callback) {
  LowEnergyAdvertiser::AdvertisingEventProperties properties;

  if (connect_callback) {
    properties.connectable = true;
  }

  if (scan_rsp.CalculateBlockSize() > 0) {
    properties.scannable = true;
  }

  // don't set the following fields because we don't currently support sending
  // out directed advertisements:
  //   - directed
  //   - high_duty_cycle_directed_connectable

  if (!options.extended_pdu) {
    properties.use_legacy_pdus = true;
  }

  if (options.anonymous) {
    properties.anonymous_advertising = true;
  }

  if (options.include_tx_power_level) {
    properties.include_tx_power = true;
  }

  return properties;
}

static LowEnergyAdvertiser::AdvertisingEventProperties
GetLegacyAdvertisingEventProperties(
    const AdvertisingData&,
    const AdvertisingData& scan_rsp,
    const LowEnergyAdvertiser::AdvertisingOptions&,
    const LowEnergyAdvertiser::ConnectionCallback& connect_callback) {
  LowEnergyAdvertiser::AdvertisingEventProperties properties;
  properties.use_legacy_pdus = true;

  // ADV_IND
  if (connect_callback) {
    properties.connectable = true;
    properties.scannable = true;
    return properties;
  }

  // ADV_SCAN_IND
  if (scan_rsp.CalculateBlockSize() > 0) {
    properties.scannable = true;
    return properties;
  }

  // ADV_NONCONN_IND
  return properties;
}

LowEnergyAdvertiser::AdvertisingEventProperties
LowEnergyAdvertiser::GetAdvertisingEventProperties(
    const AdvertisingData& data,
    const AdvertisingData& scan_rsp,
    const AdvertisingOptions& options,
    const ConnectionCallback& connect_callback) {
  if (options.extended_pdu) {
    return GetExtendedAdvertisingEventProperties(
        data, scan_rsp, options, connect_callback);
  }

  return GetLegacyAdvertisingEventProperties(
      data, scan_rsp, options, connect_callback);
}

pwemb::LEAdvertisingType
LowEnergyAdvertiser::AdvertisingEventPropertiesToLEAdvertisingType(
    const AdvertisingEventProperties& p) {
  // ADV_IND
  if (!p.high_duty_cycle_directed_connectable && !p.directed && p.scannable &&
      p.connectable) {
    return pwemb::LEAdvertisingType::CONNECTABLE_AND_SCANNABLE_UNDIRECTED;
  }

  // ADV_DIRECT_IND
  if (!p.high_duty_cycle_directed_connectable && p.directed && !p.scannable &&
      p.connectable) {
    return pwemb::LEAdvertisingType::CONNECTABLE_LOW_DUTY_CYCLE_DIRECTED;
  }

  // ADV_DIRECT_IND
  if (p.high_duty_cycle_directed_connectable && p.directed && !p.scannable &&
      p.connectable) {
    return pwemb::LEAdvertisingType::CONNECTABLE_HIGH_DUTY_CYCLE_DIRECTED;
  }

  // ADV_SCAN_IND
  if (!p.high_duty_cycle_directed_connectable && !p.directed && p.scannable &&
      !p.connectable) {
    return pwemb::LEAdvertisingType::SCANNABLE_UNDIRECTED;
  }

  // ADV_NONCONN_IND
  return pwemb::LEAdvertisingType::NOT_CONNECTABLE_UNDIRECTED;
}

void LowEnergyAdvertiser::StartAdvertisingInternal(
    const DeviceAddress& address,
    const AdvertisingData& data,
    const AdvertisingData& scan_rsp,
    const AdvertisingOptions& options,
    ConnectionCallback connect_callback,
    StartAdvertisingInternalCallback result_callback) {
  data.Copy(&staged_parameters_.data);
  scan_rsp.Copy(&staged_parameters_.scan_rsp);

  std::optional<pwemb::LEOwnAddressType> own_address_type =
      DeviceAddress::DeviceAddrToLeOwnAddr(address.type());
  PW_CHECK(own_address_type.has_value());

  AdvertisingEventProperties properties =
      GetAdvertisingEventProperties(data, scan_rsp, options, connect_callback);
  std::optional<SetAdvertisingParams> set_adv_params =
      BuildSetAdvertisingParams(
          address, properties, options, *own_address_type, options.interval);
  if (!set_adv_params.has_value()) {
    bt_log(
        WARN, "hci-le", "failed to start advertising for %s", bt_str(address));
    result_callback(fit::error(
        std::make_tuple(Error(HostError::kFailed),
                        std::optional<hci_spec::AdvertisingHandle>())));
    return;
  }

  hci_cmd_runner_->QueueCommand(
      set_adv_params->packet,
      fit::bind_member<&LowEnergyAdvertiser::OnSetAdvertisingParamsComplete>(
          this));

  // In order to support use cases where advertisers use the return parameters
  // of the SetAdvertisingParams HCI command, we place the remaining advertising
  // setup HCI commands in the result callback here. SequentialCommandRunner
  // doesn't allow enqueuing commands within a callback (during a run).
  hci_cmd_runner_->RunCommands(
      [this,
       advertisement_id = set_adv_params->advertisement_id,
       address,
       options,
       result_cb = std::move(result_callback),
       connect_cb = std::move(connect_callback)](hci::Result<> result) mutable {
        if (bt_is_error(
                result,
                WARN,
                "hci-le",
                "failed to start advertising (addr: %s, advertisement_id: %s)",
                bt_str(address),
                bt_str(advertisement_id))) {
          result_cb(fit::error(std::make_tuple(
              result.error_value(), std::optional(advertisement_id))));
          OnCurrentOperationComplete();
          return;
        }

        StartAdvertisingInternalStep2(advertisement_id,
                                      address,
                                      options,
                                      std::move(connect_cb),
                                      std::move(result_cb));
      });
}

void LowEnergyAdvertiser::StartAdvertisingInternalStep2(
    AdvertisementId advertisement_id,
    const DeviceAddress& address,
    const AdvertisingOptions& options,
    ConnectionCallback connect_callback,
    StartAdvertisingInternalCallback result_callback) {
  if (address.type() == DeviceAddress::Type::kLERandom) {
    std::optional<CommandPacket> set_random_addr_packet =
        BuildSetAdvertisingRandomAddr(advertisement_id);
    if (set_random_addr_packet.has_value()) {
      hci_cmd_runner_->QueueCommand(*set_random_addr_packet);
    }
  }

  std::vector<CommandPacket> set_adv_data_packets = BuildSetAdvertisingData(
      advertisement_id, staged_parameters_.data, options.flags);
  for (auto& packet : set_adv_data_packets) {
    hci_cmd_runner_->QueueCommand(std::move(packet));
  }

  std::vector<CommandPacket> set_scan_rsp_packets =
      BuildSetScanResponse(advertisement_id, staged_parameters_.scan_rsp);
  for (auto& packet : set_scan_rsp_packets) {
    hci_cmd_runner_->QueueCommand(std::move(packet));
  }

  CommandPacket enable_packet =
      BuildEnablePacket(advertisement_id, pwemb::GenericEnableParam::ENABLE);
  hci_cmd_runner_->QueueCommand(enable_packet);

  staged_parameters_.reset();
  hci_cmd_runner_->RunCommands([this,
                                advertisement_id,
                                result_cb = std::move(result_callback),
                                connect_cb = std::move(connect_callback)](
                                   Result<> result) mutable {
    if (bt_is_error(result,
                    WARN,
                    "hci-le",
                    "failed to start advertising for %s",
                    bt_str(advertisement_id))) {
      result_cb(fit::error(std::make_tuple(result.error_value(),
                                           std::optional(advertisement_id))));
    } else {
      bt_log(INFO,
             "hci-le",
             "advertising enabled for %s",
             bt_str(advertisement_id));
      connection_callbacks_.emplace(advertisement_id, std::move(connect_cb));
      result_cb(fit::ok(advertisement_id));
    }
    OnCurrentOperationComplete();
  });
}

// We have StopAdvertising(address) so one would naturally think to implement
// StopAdvertising() by iterating through all addresses and calling
// StopAdvertising(address) on each iteration. However, such an implementation
// won't work. Each call to StopAdvertising(address) checks if the command
// runner is running, cancels any pending commands if it is, and then issues
// new ones. Called in quick succession, StopAdvertising(address) won't have a
// chance to finish its previous HCI commands before being cancelled. Instead,
// we must enqueue them all at once and then run them together.
void LowEnergyAdvertiser::StopAdvertisingInternal(
    fit::function<void(Result<>)> result_cb) {
  if (!hci_cmd_runner_->IsReady()) {
    hci_cmd_runner_->Cancel();
  }

  for (auto itr = connection_callbacks_.begin();
       itr != connection_callbacks_.end();) {
    const AdvertisementId advertisement_id = itr->first;

    bool success = EnqueueStopAdvertisingCommands(advertisement_id);
    if (success) {
      itr = connection_callbacks_.erase(itr);
    } else {
      bt_log(WARN,
             "hci-le",
             "cannot stop advertising for %s",
             bt_str(advertisement_id));
      itr++;
    }
  }

  if (hci_cmd_runner_->HasQueuedCommands()) {
    hci_cmd_runner_->RunCommands(
        [this, cb = std::move(result_cb)](hci::Result<> result) {
          bt_log(INFO, "hci-le", "advertising stopped: %s", bt_str(result));
          if (cb) {
            cb(result);
          }
          OnCurrentOperationComplete();
        });
  } else if (result_cb) {
    result_cb(fit::ok());
  }
}

void LowEnergyAdvertiser::StopAdvertisingInternal(
    AdvertisementId advertisement_id, fit::function<void(Result<>)> result_cb) {
  if (!IsAdvertising(advertisement_id)) {
    if (result_cb) {
      result_cb(ToResult(HostError::kFailed));
    }
    return;
  }

  bool success = EnqueueStopAdvertisingCommands(advertisement_id);
  if (!success) {
    bt_log(WARN,
           "hci-le",
           "cannot stop advertising for %s",
           bt_str(advertisement_id));
    if (result_cb) {
      result_cb(ToResult(HostError::kInvalidParameters));
    }
    return;
  }

  hci_cmd_runner_->RunCommands(
      [this, advertisement_id, cb = std::move(result_cb)](Result<> result) {
        bt_log(INFO,
               "hci-le",
               "advertising stopped for %s: %s",
               bt_str(advertisement_id),
               bt_str(result));
        if (cb) {
          cb(result);
        }
        OnCurrentOperationComplete();
      });

  connection_callbacks_.erase(advertisement_id);
}

bool LowEnergyAdvertiser::EnqueueStopAdvertisingCommands(
    AdvertisementId advertisement_id) {
  CommandPacket disable_packet =
      BuildEnablePacket(advertisement_id, pwemb::GenericEnableParam::DISABLE);
  CommandPacket unset_scan_rsp_packet =
      BuildUnsetScanResponse(advertisement_id);
  CommandPacket unset_adv_data_packet =
      BuildUnsetAdvertisingData(advertisement_id);
  std::optional<CommandPacket> remove_packet =
      BuildRemoveAdvertisingSet(advertisement_id);

  hci_cmd_runner_->QueueCommand(disable_packet);
  hci_cmd_runner_->QueueCommand(unset_scan_rsp_packet);
  hci_cmd_runner_->QueueCommand(unset_adv_data_packet);
  if (remove_packet.has_value()) {
    hci_cmd_runner_->QueueCommand(remove_packet.value());
  }

  return true;
}

void LowEnergyAdvertiser::CompleteIncomingConnection(
    hci_spec::ConnectionHandle connection_handle,
    pwemb::ConnectionRole role,
    const DeviceAddress& local_address,
    const DeviceAddress& peer_address,
    const hci_spec::LEConnectionParameters& conn_params,
    std::optional<AdvertisementId> advertisement_id) {
  // Immediately construct a Connection object. If this object goes out of
  // scope following the error checks below, it will send the a command to
  // disconnect the link.
  std::unique_ptr<LowEnergyConnection> link =
      std::make_unique<LowEnergyConnection>(connection_handle,
                                            local_address,
                                            peer_address,
                                            conn_params,
                                            role,
                                            hci(),
                                            dispatcher_);

  if (!advertisement_id) {
    bt_log(ERROR,
           "hci-le",
           "connection received without advertisement (role: %d, local "
           "address: %s, peer "
           "address: %s, connection parameters: %s)",
           static_cast<uint8_t>(role),
           bt_str(local_address),
           bt_str(peer_address),
           bt_str(conn_params));
    return;
  }

  auto iter = connection_callbacks_.find(advertisement_id.value());
  if (iter == connection_callbacks_.end()) {
    bt_log(ERROR,
           "hci-le",
           "connection received without connection callback (role: %d, local "
           "address: %s, peer "
           "address: %s, connection parameters: %s, advertisement id: %s)",
           static_cast<uint8_t>(role),
           bt_str(local_address),
           bt_str(peer_address),
           bt_str(conn_params),
           bt_str(advertisement_id.value()));
    return;
  }

  ConnectionCallback connect_callback = std::move(iter->second);
  if (!connect_callback) {
    bt_log(ERROR,
           "hci-le",
           "connection received when not connectable (role: %d, "
           "local address: %s, "
           "peer address: %s, "
           "connection parameters: %s, advertisement id: %s)",
           static_cast<uint8_t>(role),
           bt_str(local_address),
           bt_str(peer_address),
           bt_str(conn_params),
           bt_str(advertisement_id.value()));
    return;
  }

  StopAdvertising(advertisement_id.value());
  connect_callback(advertisement_id.value(), std::move(link));
}

}  // namespace bt::hci
