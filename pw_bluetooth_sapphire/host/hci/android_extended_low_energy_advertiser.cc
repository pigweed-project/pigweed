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

#include "pw_bluetooth_sapphire/internal/host/hci/android_extended_low_energy_advertiser.h"

#include <pw_assert/check.h>
#include <pw_bluetooth/hci_android.emb.h>
#include <pw_bluetooth/hci_common.emb.h>

#include "pw_bluetooth_sapphire/internal/host/hci-spec/vendor_protocol.h"
#include "pw_bluetooth_sapphire/internal/host/transport/transport.h"

namespace bt::hci {
namespace pwemb = pw::bluetooth::emboss;

static CommandPacket BuildReadAdvertisingTxPower() {
  return CommandPacket::New<pwemb::LEReadAdvertisingChannelTxPowerCommandView>(
      hci_spec::kLEReadAdvertisingChannelTxPower);
}

namespace android_hci = hci_spec::vendor::android;
namespace android_emb = pw::bluetooth::vendor::android_hci;

AndroidExtendedLowEnergyAdvertiser::AndroidExtendedLowEnergyAdvertiser(
    hci::Transport::WeakPtr hci_ptr,
    pw::async::Dispatcher& dispatcher,
    uint8_t max_advertisements)
    : LowEnergyAdvertiser(std::move(hci_ptr),
                          dispatcher,
                          hci_spec::kMaxLEAdvertisingDataLength),
      WeakSelf(this),
      advertising_handle_map_(max_advertisements) {
  state_changed_event_handler_id_ =
      hci()->command_channel()->AddVendorEventHandler(
          android_hci::kLEMultiAdvtStateChangeSubeventCode,
          [this](const EventPacket& event_packet) {
            return OnAdvertisingStateChangedSubevent(event_packet);
          });
}

AndroidExtendedLowEnergyAdvertiser::~AndroidExtendedLowEnergyAdvertiser() {
  // This object is probably being destroyed because the stack is shutting down,
  // in which case the HCI layer may have already been destroyed.
  if (!hci().is_alive() || !hci()->command_channel()) {
    return;
  }

  hci()->command_channel()->RemoveEventHandler(state_changed_event_handler_id_);
  // TODO(fxbug.dev/42063496): This will only cancel one advertisement, after
  // which the SequentialCommandRunner will have been destroyed and no further
  // commands will be sent.
  StopAdvertising();
}

void AndroidExtendedLowEnergyAdvertiser::AttachInspect(inspect::Node& parent) {
  node_ = parent.CreateChild("low_energy_advertiser");
  advertising_handle_map_.AttachInspect(node_);
}

CommandPacket AndroidExtendedLowEnergyAdvertiser::BuildEnablePacket(
    AdvertisementId advertisement_id, pwemb::GenericEnableParam enable) const {
  hci_spec::AdvertisingHandle advertising_handle =
      advertising_handle_map_.GetHandle(advertisement_id);

  auto packet =
      hci::CommandPacket::New<android_emb::LEMultiAdvtEnableCommandWriter>(
          android_hci::kLEMultiAdvt);
  auto packet_view = packet.view_t();
  packet_view.vendor_command().sub_opcode().Write(
      android_hci::kLEMultiAdvtEnableSubopcode);
  packet_view.enable().Write(enable);
  packet_view.advertising_handle().Write(advertising_handle);
  return packet;
}

std::optional<LowEnergyAdvertiser::SetAdvertisingParams>
AndroidExtendedLowEnergyAdvertiser::BuildSetAdvertisingParams(
    const DeviceAddress& address,
    const AdvertisingEventProperties& properties,
    const AdvertisingOptions&,
    pwemb::LEOwnAddressType own_address_type,
    const AdvertisingIntervalRange& interval) {
  std::optional<AdvertisementId> advertisement_id =
      advertising_handle_map_.Insert(address);
  if (!advertisement_id) {
    bt_log(WARN,
           "hci-le",
           "could not allocate advertising handle for address: %s",
           bt_str(address));
    return std::nullopt;
  }

  auto packet = hci::CommandPacket::New<
      android_emb::LEMultiAdvtSetAdvtParamCommandWriter>(
      android_hci::kLEMultiAdvt);
  auto view = packet.view_t();

  view.vendor_command().sub_opcode().Write(
      android_hci::kLEMultiAdvtSetAdvtParamSubopcode);
  view.adv_interval_min().Write(interval.min());
  view.adv_interval_max().Write(interval.max());
  view.adv_type().Write(
      AdvertisingEventPropertiesToLEAdvertisingType(properties));
  view.own_addr_type().Write(own_address_type);
  view.adv_channel_map().channel_37().Write(true);
  view.adv_channel_map().channel_38().Write(true);
  view.adv_channel_map().channel_39().Write(true);
  view.adv_filter_policy().Write(pwemb::LEAdvertisingFilterPolicy::ALLOW_ALL);
  view.adv_handle().Write(
      advertising_handle_map_.GetHandle(advertisement_id.value()));
  view.adv_tx_power().Write(hci_spec::kLEAdvertisingTxPowerMax);

  // We don't support directed advertising yet, so leave peer_address and
  // peer_address_type as 0x00
  // (|packet| parameters are initialized to zero above).

  return SetAdvertisingParams{std::move(packet), advertisement_id.value()};
}

std::optional<CommandPacket>
AndroidExtendedLowEnergyAdvertiser::BuildSetAdvertisingRandomAddr(
    AdvertisementId advertisement_id) const {
  hci_spec::AdvertisingHandle advertising_handle =
      advertising_handle_map_.GetHandle(advertisement_id);
  DeviceAddress address = advertising_handle_map_.GetAddress(advertisement_id);

  auto packet =
      CommandPacket::New<android_emb::LEMultiAdvtSetRandomAddrCommandWriter>(
          android_hci::kLEMultiAdvt);
  auto view = packet.view_t();

  view.vendor_command().sub_opcode().Write(
      android_hci::kLEMultiAdvtSetRandomAddrSubopcode);
  view.adv_handle().Write(advertising_handle);
  view.random_address().CopyFrom(address.value().view());

  return packet;
}

std::vector<CommandPacket>
AndroidExtendedLowEnergyAdvertiser::BuildSetAdvertisingData(
    AdvertisementId advertisement_id,
    const AdvertisingData& data,
    AdvFlags flags) const {
  if (data.CalculateBlockSize() == 0) {
    std::vector<CommandPacket> packets;
    return packets;
  }

  auto packet =
      hci::CommandPacket::New<android_emb::LEMultiAdvtSetAdvtDataCommandWriter>(
          android_hci::kLEMultiAdvt);
  auto view = packet.view_t();

  uint8_t adv_data_length = data.CalculateBlockSize(/*include_flags=*/true);
  view.vendor_command().sub_opcode().Write(
      android_hci::kLEMultiAdvtSetAdvtDataSubopcode);
  view.adv_data_length().Write(adv_data_length);
  view.adv_handle().Write(advertising_handle_map_.GetHandle(advertisement_id));

  MutableBufferView data_view(view.adv_data().BackingStorage().data(),
                              adv_data_length);
  data.WriteBlock(&data_view, flags);

  std::vector<CommandPacket> packets;
  packets.reserve(1);
  packets.emplace_back(std::move(packet));
  return packets;
}

CommandPacket AndroidExtendedLowEnergyAdvertiser::BuildUnsetAdvertisingData(
    AdvertisementId advertisement_id) const {
  auto packet =
      hci::CommandPacket::New<android_emb::LEMultiAdvtSetAdvtDataCommandWriter>(
          android_hci::kLEMultiAdvt);
  auto view = packet.view_t();

  view.vendor_command().sub_opcode().Write(
      android_hci::kLEMultiAdvtSetAdvtDataSubopcode);
  view.adv_data_length().Write(0);
  view.adv_handle().Write(advertising_handle_map_.GetHandle(advertisement_id));

  return packet;
}

std::vector<CommandPacket>
AndroidExtendedLowEnergyAdvertiser::BuildSetScanResponse(
    AdvertisementId advertisement_id, const AdvertisingData& data) const {
  if (data.CalculateBlockSize() == 0) {
    std::vector<CommandPacket> packets;
    return packets;
  }

  auto packet = hci::CommandPacket::New<
      android_emb::LEMultiAdvtSetScanRespDataCommandWriter>(
      android_hci::kLEMultiAdvt);
  auto view = packet.view_t();

  uint8_t scan_rsp_length = data.CalculateBlockSize();
  view.vendor_command().sub_opcode().Write(
      android_hci::kLEMultiAdvtSetScanRespSubopcode);
  view.scan_resp_length().Write(scan_rsp_length);
  view.adv_handle().Write(advertising_handle_map_.GetHandle(advertisement_id));

  MutableBufferView data_view(view.scan_resp_data().BackingStorage().data(),
                              scan_rsp_length);
  data.WriteBlock(&data_view, std::nullopt);

  std::vector<CommandPacket> packets;
  packets.reserve(1);
  packets.emplace_back(std::move(packet));
  return packets;
}

CommandPacket AndroidExtendedLowEnergyAdvertiser::BuildUnsetScanResponse(
    AdvertisementId advertisement_id) const {
  auto packet = hci::CommandPacket::New<
      android_emb::LEMultiAdvtSetScanRespDataCommandWriter>(
      android_hci::kLEMultiAdvt);
  auto view = packet.view_t();

  view.vendor_command().sub_opcode().Write(
      android_hci::kLEMultiAdvtSetScanRespSubopcode);
  view.scan_resp_length().Write(0);
  view.adv_handle().Write(advertising_handle_map_.GetHandle(advertisement_id));

  return packet;
}

std::optional<CommandPacket>
AndroidExtendedLowEnergyAdvertiser::BuildRemoveAdvertisingSet(
    AdvertisementId advertisement_id) const {
  auto packet =
      hci::CommandPacket::New<android_emb::LEMultiAdvtEnableCommandWriter>(
          android_hci::kLEMultiAdvt);
  auto packet_view = packet.view_t();
  packet_view.vendor_command().sub_opcode().Write(
      android_hci::kLEMultiAdvtEnableSubopcode);
  packet_view.enable().Write(pwemb::GenericEnableParam::DISABLE);
  packet_view.advertising_handle().Write(
      advertising_handle_map_.GetHandle(advertisement_id));
  return packet;
}

void AndroidExtendedLowEnergyAdvertiser::StartAdvertising(
    const DeviceAddress& address,
    const AdvertisingData& data,
    const AdvertisingData& scan_rsp,
    const AdvertisingOptions& options,
    ConnectionCallback connect_callback,
    ResultFunction<AdvertisementId> result_callback) {
  if (options.extended_pdu) {
    bt_log(WARN,
           "hci-le",
           "android vendor extensions cannot use extended advertising PDUs");
    result_callback(ToResult(HostError::kNotSupported).take_error());
    return;
  }

  fit::result<HostError> can_start_result =
      CanStartAdvertising(address, data, scan_rsp, options, connect_callback);
  if (can_start_result.is_error()) {
    result_callback(ToResult(can_start_result.error_value()).take_error());
    return;
  }

  AdvertisingData copied_data;
  data.Copy(&copied_data);

  AdvertisingData copied_scan_rsp;
  scan_rsp.Copy(&copied_scan_rsp);

  // if there is an operation currently in progress, enqueue this operation and
  // we will get to it the next time we have a chance
  if (!hci_cmd_runner().IsReady()) {
    bt_log(INFO,
           "hci-le",
           "hci cmd runner not ready, queuing advertisement commands for now");

    op_queue_.push([this,
                    address,
                    data_copy = std::move(copied_data),
                    scan_rsp_copy = std::move(copied_scan_rsp),
                    options_copy = options,
                    conn_cb = std::move(connect_callback),
                    result_cb = std::move(result_callback)]() mutable {
      StartAdvertising(address,
                       data_copy,
                       scan_rsp_copy,
                       options_copy,
                       std::move(conn_cb),
                       std::move(result_cb));
    });

    return;
  }

  auto result_cb_wrapper = [this, cb = std::move(result_callback)](
                               StartAdvertisingInternalResult result) {
    if (result.is_error()) {
      auto [error, advertisement_id] = result.error_value();
      if (advertisement_id.has_value()) {
        advertising_handle_map_.Erase(advertisement_id.value());
      }
      cb(fit::error(error));
      return;
    }
    cb(result.take_value());
  };

  if (options.include_tx_power_level) {
    uint64_t pending_start_id = next_pending_start_id_++;
    pending_starts_.emplace(pending_start_id,
                            PendingStart{address,
                                         std::move(copied_data),
                                         std::move(copied_scan_rsp),
                                         options,
                                         std::move(connect_callback),
                                         std::move(result_cb_wrapper)});

    auto power_cb = [self = GetWeakPtr(), pending_start_id](
                        CommandChannel::TransactionId,
                        const hci::EventPacket& event) {
      if (!self.is_alive()) {
        return;
      }

      std::unordered_map<uint64_t, PendingStart>::iterator it =
          self->pending_starts_.find(pending_start_id);
      if (it == self->pending_starts_.end()) {
        bt_log(
            INFO, "hci-le", "Advertising start cancelled during TX Power read");
        return;
      }

      PendingStart pending_start = std::move(it->second);
      self->pending_starts_.erase(it);

      if (HCI_IS_ERROR(event, WARN, "hci-le", "read TX power level failed")) {
        pending_start.result_callback(fit::error(std::make_tuple(
            event.ToResult().error_value(), std::optional<AdvertisementId>())));
        return;
      }

      auto view = event.unchecked_view<
          pw::bluetooth::emboss::
              LEReadAdvertisingChannelTxPowerCommandCompleteEventView>();
      if (!view.Ok()) {
        bt_log(WARN, "hci-le", "read TX power level event malformed");
        pending_start.result_callback(
            fit::error(std::make_tuple(Error(HostError::kPacketMalformed),
                                       std::optional<AdvertisementId>())));
        return;
      }

      int8_t tx_power = view.tx_power_level().Read();
      pending_start.data.SetTxPower(tx_power);
      if (pending_start.scan_rsp.CalculateBlockSize()) {
        pending_start.scan_rsp.SetTxPower(tx_power);
      }

      self->StartAdvertisingInternal(pending_start.address,
                                     pending_start.data,
                                     pending_start.scan_rsp,
                                     pending_start.options,
                                     std::move(pending_start.connect_callback),
                                     std::move(pending_start.result_callback));
    };

    auto cmd_res = hci()->command_channel()->SendCommand(
        BuildReadAdvertisingTxPower(), std::move(power_cb));
    if (cmd_res.ok()) {
      return;
    }

    bt_log(WARN, "hci-le", "failed to send read TX power level command");
    result_cb_wrapper(fit::error(std::make_tuple(
        Error(HostError::kFailed), std::optional<AdvertisementId>())));
    return;
  }

  StartAdvertisingInternal(address,
                           copied_data,
                           copied_scan_rsp,
                           options,
                           std::move(connect_callback),
                           std::move(result_cb_wrapper));
}

void AndroidExtendedLowEnergyAdvertiser::StopAdvertising(
    fit::function<void(Result<>)> result_cb) {
  StopAdvertisingInternal(std::move(result_cb));
  advertising_handle_map_.Clear();

  std::unordered_map<uint64_t, PendingStart> pending_starts;
  pending_starts.swap(pending_starts_);
  for (auto& item : pending_starts) {
    item.second.result_callback(fit::error(std::make_tuple(
        Error(HostError::kCanceled), std::optional<AdvertisementId>())));
  }

  // std::queue doesn't have a clear method so we have to resort to this
  // tomfoolery :(
  decltype(op_queue_) empty;
  std::swap(op_queue_, empty);
}

void AndroidExtendedLowEnergyAdvertiser::StopAdvertising(
    AdvertisementId advertisement_id, fit::function<void(Result<>)> result_cb) {
  // if there is an operation currently in progress, enqueue this operation and
  // we will get to it the next time we have a chance
  if (!hci_cmd_runner().IsReady()) {
    bt_log(
        INFO,
        "hci-le",
        "hci cmd runner not ready, queueing stop advertising command for now");
    op_queue_.push(
        [this, advertisement_id, cb = std::move(result_cb)]() mutable {
          StopAdvertising(advertisement_id, std::move(cb));
        });
    return;
  }

  StopAdvertisingInternal(advertisement_id, std::move(result_cb));
  advertising_handle_map_.Erase(advertisement_id);
}

void AndroidExtendedLowEnergyAdvertiser::OnIncomingConnection(
    hci_spec::ConnectionHandle handle,
    pwemb::ConnectionRole role,
    const DeviceAddress& peer_address,
    const hci_spec::LEConnectionParameters& conn_params) {
  staged_connections_map_[handle] = {role, peer_address, conn_params};
}

// The LE multi-advertising state change subevent contains the mapping between
// connection handle and advertising handle. After the LE multi-advertising
// state change subevent, we have all the information necessary to create a
// connection object within the Host layer.
CommandChannel::EventCallbackResult
AndroidExtendedLowEnergyAdvertiser::OnAdvertisingStateChangedSubevent(
    const EventPacket& event) {
  PW_CHECK(event.event_code() == hci_spec::kVendorDebugEventCode);
  PW_CHECK(event.view<pwemb::VendorDebugEventView>().subevent_code().Read() ==
           android_hci::kLEMultiAdvtStateChangeSubeventCode);

  Result<> result = event.ToResult();
  if (bt_is_error(result,
                  ERROR,
                  "hci-le",
                  "advertising state change event, error received %s",
                  bt_str(result))) {
    return CommandChannel::EventCallbackResult::kContinue;
  }

  auto view = event.view<android_emb::LEMultiAdvtStateChangeSubeventView>();
  hci_spec::AdvertisingHandle adv_handle = view.advertising_handle().Read();
  std::optional<AdvertisementId> advertisement_id =
      advertising_handle_map_.GetId(adv_handle);

  // We use the identity address as the local address if we aren't advertising
  // or otherwise don't know about this advertising set. This is obviously
  // wrong. However, the link will be disconnected in that case before it can
  // propagate to higher layers.
  static DeviceAddress identity_address =
      DeviceAddress(DeviceAddress::Type::kLEPublic, {0});
  DeviceAddress local_address = identity_address;
  if (advertisement_id.has_value()) {
    local_address =
        advertising_handle_map_.GetAddress(advertisement_id.value());
  }

  hci_spec::ConnectionHandle connection_handle =
      view.connection_handle().Read();
  auto staged_node = staged_connections_map_.extract(connection_handle);
  if (staged_node.empty()) {
    bt_log(ERROR,
           "hci-le",
           "advertising state change event, staged params not available "
           "(handle: %d)",
           view.advertising_handle().Read());
    return CommandChannel::EventCallbackResult::kContinue;
  }

  StagedConnectionParameters staged = staged_node.mapped();
  CompleteIncomingConnection(connection_handle,
                             staged.role,
                             local_address,
                             staged.peer_address,
                             staged.conn_params,
                             advertisement_id);

  return CommandChannel::EventCallbackResult::kContinue;
}

void AndroidExtendedLowEnergyAdvertiser::OnCurrentOperationComplete() {
  if (op_queue_.empty()) {
    return;  // no more queued operations so nothing to do
  }

  fit::closure closure = std::move(op_queue_.front());
  op_queue_.pop();
  closure();
}

}  // namespace bt::hci
