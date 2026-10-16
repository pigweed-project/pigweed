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

#include "pw_bluetooth_sapphire/internal/host/hci/legacy_low_energy_advertiser.h"

#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/internal/host/common/advertising_data.h"
#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/hci/sequential_command_runner.h"
#include "pw_bluetooth_sapphire/internal/host/transport/transport.h"

namespace bt::hci {
namespace pwemb = pw::bluetooth::emboss;

LegacyLowEnergyAdvertiser::~LegacyLowEnergyAdvertiser() {
  // This object is probably being destroyed because the stack is shutting down,
  // in which case the HCI layer may have already been destroyed.
  if (!hci().is_alive() || !hci()->command_channel()) {
    return;
  }

  StopAdvertising();
}

CommandPacket LegacyLowEnergyAdvertiser::BuildEnablePacket(
    AdvertisementId, pwemb::GenericEnableParam enable) const {
  auto packet =
      hci::CommandPacket::New<pwemb::LESetAdvertisingEnableCommandWriter>(
          hci_spec::kLESetAdvertisingEnable);
  auto packet_view = packet.view_t();
  packet_view.advertising_enable().Write(enable);
  return packet;
}

std::optional<CommandPacket>
LegacyLowEnergyAdvertiser::BuildSetAdvertisingRandomAddr(
    AdvertisementId) const {
  // In legacy advertising, random addresses use a single, global address set by
  // the controlleer
  return std::nullopt;
}

std::vector<CommandPacket> LegacyLowEnergyAdvertiser::BuildSetAdvertisingData(
    AdvertisementId, const AdvertisingData& data, AdvFlags flags) const {
  if (data.CalculateBlockSize() == 0) {
    std::vector<CommandPacket> packets;
    return packets;
  }

  auto packet = CommandPacket::New<pwemb::LESetAdvertisingDataCommandWriter>(
      hci_spec::kLESetAdvertisingData);
  auto params = packet.view_t();
  const uint8_t data_length =
      static_cast<uint8_t>(data.CalculateBlockSize(/*include_flags=*/true));
  params.advertising_data_length().Write(data_length);

  MutableBufferView adv_view(params.advertising_data().BackingStorage().data(),
                             data_length);
  data.WriteBlock(&adv_view, flags);

  std::vector<CommandPacket> packets;
  packets.reserve(1);
  packets.emplace_back(std::move(packet));
  return packets;
}

std::vector<CommandPacket> LegacyLowEnergyAdvertiser::BuildSetScanResponse(
    AdvertisementId, const AdvertisingData& scan_rsp) const {
  if (scan_rsp.CalculateBlockSize() == 0) {
    std::vector<CommandPacket> packets;
    return packets;
  }

  auto packet = CommandPacket::New<pwemb::LESetScanResponseDataCommandWriter>(
      hci_spec::kLESetScanResponseData);
  auto params = packet.view_t();
  const uint8_t data_length =
      static_cast<uint8_t>(scan_rsp.CalculateBlockSize());
  params.scan_response_data_length().Write(data_length);

  MutableBufferView scan_data_view(
      params.scan_response_data().BackingStorage().data(), data_length);
  scan_rsp.WriteBlock(&scan_data_view, /*flags=*/std::nullopt);

  std::vector<CommandPacket> packets;
  packets.reserve(1);
  packets.emplace_back(std::move(packet));
  return packets;
}

std::optional<LowEnergyAdvertiser::SetAdvertisingParams>
LegacyLowEnergyAdvertiser::BuildSetAdvertisingParams(
    const DeviceAddress&,
    const AdvertisingEventProperties& properties,
    const AdvertisingOptions&,
    pwemb::LEOwnAddressType own_address_type,
    const AdvertisingIntervalRange& interval) {
  auto packet =
      CommandPacket::New<pwemb::LESetAdvertisingParametersCommandWriter>(
          hci_spec::kLESetAdvertisingParameters);
  auto params = packet.view_t();
  params.advertising_interval_min().Write(interval.min());
  params.advertising_interval_max().Write(interval.max());
  params.adv_type().Write(
      AdvertisingEventPropertiesToLEAdvertisingType(properties));
  params.own_address_type().Write(own_address_type);
  params.advertising_channel_map().BackingStorage().WriteUInt(
      hci_spec::kLEAdvertisingChannelAll);
  params.advertising_filter_policy().Write(
      pwemb::LEAdvertisingFilterPolicy::ALLOW_ALL);

  // We don't support directed advertising yet, so leave peer_address and
  // peer_address_type as 0x00
  // (|packet| parameters are initialized to zero above).

  return SetAdvertisingParams{std::move(packet),
                              active_advertisement_id_.value()};
}

CommandPacket LegacyLowEnergyAdvertiser::BuildUnsetAdvertisingData(
    AdvertisementId) const {
  return CommandPacket::New<pwemb::LESetAdvertisingDataCommandWriter>(
      hci_spec::kLESetAdvertisingData);
}

CommandPacket LegacyLowEnergyAdvertiser::BuildUnsetScanResponse(
    AdvertisementId) const {
  auto packet = CommandPacket::New<pwemb::LESetScanResponseDataCommandWriter>(
      hci_spec::kLESetScanResponseData);
  return packet;
}

static CommandPacket BuildReadAdvertisingTxPower() {
  return CommandPacket::New<pwemb::LEReadAdvertisingChannelTxPowerCommandView>(
      hci_spec::kLEReadAdvertisingChannelTxPower);
}

void LegacyLowEnergyAdvertiser::StartAdvertising(
    const DeviceAddress& address,
    const AdvertisingData& data,
    const AdvertisingData& scan_rsp,
    const AdvertisingOptions& options,
    ConnectionCallback connect_callback,
    ResultFunction<AdvertisementId> result_callback) {
  if (options.extended_pdu) {
    bt_log(INFO,
           "hci-le",
           "legacy advertising cannot use extended advertising PDUs");
    result_callback(fit::error(HostError::kNotSupported));
    return;
  }

  fit::result<HostError> can_start_result =
      CanStartAdvertising(address, data, scan_rsp, options, connect_callback);
  if (can_start_result.is_error()) {
    result_callback(can_start_result.take_error());
    return;
  }

  if (active_advertisement_id_) {
    bt_log(INFO,
           "hci-le",
           "already advertising (only one advertisement supported at a time)");
    result_callback(fit::error(HostError::kNotSupported));
    return;
  }

  if (!hci_cmd_runner().IsReady()) {
    bt_log(DEBUG,
           "hci-le",
           "hci cmd runner not ready, queing advertisement commands for now");

    AdvertisingData copied_data;
    data.Copy(&copied_data);

    AdvertisingData copied_scan_rsp;
    scan_rsp.Copy(&copied_scan_rsp);

    op_queue_.push([this,
                    address_copy = address,
                    data_copy = std::move(copied_data),
                    scan_rsp_copy = std::move(copied_scan_rsp),
                    options_copy = options,
                    conn_cb = std::move(connect_callback),
                    result_cb = std::move(result_callback)]() mutable {
      StartAdvertising(address_copy,
                       data_copy,
                       scan_rsp_copy,
                       options_copy,
                       std::move(conn_cb),
                       std::move(result_cb));
    });

    return;
  }

  starting_ = true;
  local_address_ = DeviceAddress();
  active_advertisement_id_.emplace(next_advertisement_id_++);

  auto result_cb_wrapper =
      [this, address_copy = address, cb = std::move(result_callback)](
          StartAdvertisingInternalResult result) {
        if (result.is_error()) {
          ResetAdvertisingState();
          cb(fit::error(std::get<Error>(result.error_value())));
          return;
        }
        starting_ = false;
        local_address_ = address_copy;
        cb(result.take_value());
      };

  // If the TX Power Level is requested, read it from the controller, update the
  // data buf, and proceed with starting advertising.
  //
  // If advertising was canceled during the TX power level read (either
  // |starting_| was reset or the |result_callback| was moved), return early.
  if (options.include_tx_power_level) {
    AdvertisingData data_copy;
    data.Copy(&data_copy);

    AdvertisingData scan_rsp_copy;
    scan_rsp.Copy(&scan_rsp_copy);

    staged_params_ = StagedParams{address,
                                  std::move(data_copy),
                                  std::move(scan_rsp_copy),
                                  options,
                                  std::move(connect_callback),
                                  std::move(result_cb_wrapper)};

    auto power_cb = [this](auto, const hci::EventPacket& event) mutable {
      PW_CHECK(staged_params_.has_value());
      if (!starting_ || !staged_params_.value().result_callback) {
        bt_log(
            INFO, "hci-le", "Advertising canceled during TX Power Level read.");
        return;
      }

      if (HCI_IS_ERROR(event, WARN, "hci-le", "read TX power level failed")) {
        staged_params_.value().result_callback(fit::error(std::make_tuple(
            event.ToResult().error_value(), std::optional<AdvertisementId>())));
        staged_params_ = {};
        return;
      }

      auto staged_params = std::move(staged_params_.value());
      staged_params_ = {};

      // Update the advertising and scan response data with the TX power level.
      auto view = event.view<
          pw::bluetooth::emboss::
              LEReadAdvertisingChannelTxPowerCommandCompleteEventView>();
      int8_t tx_power = static_cast<int8_t>(view.tx_power_level().Read());
      staged_params.data.SetTxPower(tx_power);
      if (staged_params.scan_rsp.CalculateBlockSize()) {
        staged_params.scan_rsp.SetTxPower(tx_power);
      }

      StartAdvertisingInternal(staged_params.address,
                               staged_params.data,
                               staged_params.scan_rsp,
                               staged_params.options,
                               std::move(staged_params.connect_callback),
                               std::move(staged_params.result_callback));
    };

    hci()
        ->command_channel()
        ->SendCommand(BuildReadAdvertisingTxPower(), std::move(power_cb))
        .IgnoreError();
    return;
  }

  StartAdvertisingInternal(address,
                           data,
                           scan_rsp,
                           options,
                           std::move(connect_callback),
                           std::move(result_cb_wrapper));
}

void LegacyLowEnergyAdvertiser::StopAdvertising(
    fit::function<void(Result<>)> result_cb) {
  StopAdvertisingInternal(std::move(result_cb));
  ResetAdvertisingState();
}

void LegacyLowEnergyAdvertiser::StopAdvertising(
    AdvertisementId advertisement_id, fit::function<void(Result<>)> result_cb) {
  if (!active_advertisement_id_ ||
      active_advertisement_id_.value() != advertisement_id) {
    if (result_cb) {
      result_cb(ToResult(HostError::kInvalidParameters));
    }
    return;
  }

  if (!hci_cmd_runner().IsReady()) {
    hci_cmd_runner().Cancel();
  }

  StopAdvertisingInternal(advertisement_id, std::move(result_cb));
  ResetAdvertisingState();
}

void LegacyLowEnergyAdvertiser::OnIncomingConnection(
    hci_spec::ConnectionHandle connection_handle,
    pwemb::ConnectionRole role,
    const DeviceAddress& peer_address,
    const hci_spec::LEConnectionParameters& conn_params) {
  static DeviceAddress identity_address =
      DeviceAddress(DeviceAddress::Type::kLEPublic, {0});

  // We use the identity address as the local address if we aren't advertising.
  // If we aren't advertising, this is obviously wrong. However, the link will
  // be disconnected in that case before it can propagate to higher layers.
  DeviceAddress local_address = identity_address;
  if (active_advertisement_id_) {
    local_address = local_address_;
  }

  CompleteIncomingConnection(connection_handle,
                             role,
                             local_address,
                             peer_address,
                             conn_params,
                             active_advertisement_id_);
}

void LegacyLowEnergyAdvertiser::ResetAdvertisingState() {
  starting_ = false;
  local_address_ = DeviceAddress();
  active_advertisement_id_.reset();
}

void LegacyLowEnergyAdvertiser::OnCurrentOperationComplete() {
  if (op_queue_.empty()) {
    return;  // no more queued operations so nothing to do
  }

  fit::closure closure = std::move(op_queue_.front());
  op_queue_.pop();
  closure();
}

}  // namespace bt::hci
