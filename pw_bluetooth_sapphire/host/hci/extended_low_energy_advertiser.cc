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

#include "pw_bluetooth_sapphire/internal/host/hci/extended_low_energy_advertiser.h"

#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/internal/host/transport/transport.h"

namespace bt::hci {
namespace pwemb = pw::bluetooth::emboss;

ExtendedLowEnergyAdvertiser::ExtendedLowEnergyAdvertiser(
    hci::Transport::WeakPtr hci_ptr,
    pw::async::Dispatcher& dispatcher,
    uint16_t max_advertising_data_length)
    : LowEnergyAdvertiser(
          std::move(hci_ptr), dispatcher, max_advertising_data_length) {
  event_handler_id_ = hci()->command_channel()->AddLEMetaEventHandler(
      hci_spec::kLEAdvertisingSetTerminatedSubeventCode,
      [this](const EventPacket& event) {
        OnAdvertisingSetTerminatedEvent(event);
        return CommandChannel::EventCallbackResult::kContinue;
      });
}

ExtendedLowEnergyAdvertiser::~ExtendedLowEnergyAdvertiser() {
  // This object is probably being destroyed because the stack is shutting down,
  // in which case the HCI layer may have already been destroyed.
  if (!hci().is_alive() || !hci()->command_channel()) {
    return;
  }

  hci()->command_channel()->RemoveEventHandler(event_handler_id_);

  // TODO(fxbug.dev/42063496): This will only cancel one advertisement,
  // after which the SequentialCommandRunner will have been destroyed and no
  // further commands will be sent.
  StopAdvertising();
}

void ExtendedLowEnergyAdvertiser::AttachInspect(inspect::Node& node) {
  node_ = node.CreateChild("low_energy_advertiser");
  advertising_handle_map_.AttachInspect(node_);
}

CommandPacket ExtendedLowEnergyAdvertiser::BuildEnablePacket(
    hci::AdvertisementId advertisement_id,
    pwemb::GenericEnableParam enable) const {
  std::optional<hci_spec::AdvertisingHandle> advertising_handle =
      advertising_handle_map_.GetHandle(advertisement_id);
  PW_CHECK(advertising_handle);

  // We only enable or disable a single address at a time. The multiply by 1 is
  // set explicitly to show that data[] within
  // LESetExtendedAdvertisingEnableData is of size 1.
  constexpr size_t kPacketSize =
      pwemb::LESetExtendedAdvertisingEnableCommand::MinSizeInBytes() +
      (1 * pwemb::LESetExtendedAdvertisingEnableData::IntrinsicSizeInBytes());
  auto packet = hci::CommandPacket::New<
      pwemb::LESetExtendedAdvertisingEnableCommandWriter>(
      hci_spec::kLESetExtendedAdvertisingEnable, kPacketSize);
  auto view = packet.view_t();
  view.enable().Write(enable);
  view.num_sets().Write(1);
  view.data()[0].advertising_handle().Write(advertising_handle.value());
  view.data()[0].duration().Write(hci_spec::kNoAdvertisingDuration);
  view.data()[0].max_extended_advertising_events().Write(
      hci_spec::kNoMaxExtendedAdvertisingEvents);

  return packet;
}

static void WriteAdvertisingEventProperties(
    const ExtendedLowEnergyAdvertiser::AdvertisingEventProperties& properties,
    pwemb::LESetExtendedAdvertisingParametersV1CommandWriter& view) {
  view.advertising_event_properties().connectable().Write(
      properties.connectable);
  view.advertising_event_properties().scannable().Write(properties.scannable);
  view.advertising_event_properties().directed().Write(properties.directed);
  view.advertising_event_properties()
      .high_duty_cycle_directed_connectable()
      .Write(properties.high_duty_cycle_directed_connectable);
  view.advertising_event_properties().use_legacy_pdus().Write(
      properties.use_legacy_pdus);
  view.advertising_event_properties().anonymous_advertising().Write(
      properties.anonymous_advertising);
  view.advertising_event_properties().include_tx_power().Write(
      properties.include_tx_power);
}

std::optional<LowEnergyAdvertiser::SetAdvertisingParams>
ExtendedLowEnergyAdvertiser::BuildSetAdvertisingParams(
    const DeviceAddress& address,
    const AdvertisingEventProperties& properties,
    const AdvertisingOptions& options,
    pwemb::LEOwnAddressType own_address_type,
    const AdvertisingIntervalRange& interval) {
  auto packet = hci::CommandPacket::New<
      pwemb::LESetExtendedAdvertisingParametersV1CommandWriter>(
      hci_spec::kLESetExtendedAdvertisingParameters);
  auto view = packet.view_t();

  // advertising handle
  std::optional<hci::AdvertisementId> advertisement_id =
      advertising_handle_map_.Insert(address);
  if (!advertisement_id) {
    bt_log(WARN,
           "hci-le",
           "could not allocate advertising handle for address: %s",
           bt_str(address));
    return std::nullopt;
  }

  std::optional<hci_spec::AdvertisingHandle> advertising_handle =
      advertising_handle_map_.GetHandle(advertisement_id.value());
  PW_CHECK(advertising_handle);
  view.advertising_handle().Write(advertising_handle.value());

  WriteAdvertisingEventProperties(properties, view);

  // advertising interval, NOTE: LE advertising parameters allow for up to 3
  // octets (10 ms to 10428 s) to configure an advertising interval. However, we
  // expose only the recommended advertising interval configurations to users,
  // as specified in the Bluetooth Spec Volume 3, Part C, Appendix A. These
  // values are expressed as uint16_t so we simply copy them (taking care of
  // endianness) into the 3 octets as is.
  view.primary_advertising_interval_min().Write(interval.min());
  view.primary_advertising_interval_max().Write(interval.max());

  // advertise on all channels
  view.primary_advertising_channel_map().channel_37().Write(true);
  view.primary_advertising_channel_map().channel_38().Write(true);
  view.primary_advertising_channel_map().channel_39().Write(true);

  view.own_address_type().Write(own_address_type);
  view.advertising_filter_policy().Write(
      pwemb::LEAdvertisingFilterPolicy::ALLOW_ALL);
  view.advertising_tx_power().Write(options.max_tx_power);
  view.scan_request_notification_enable().Write(
      pwemb::GenericEnableParam::DISABLE);

  // TODO(fxbug.dev/42161929): using legacy PDUs requires advertisements
  // on the LE 1M PHY.
  view.primary_advertising_phy().Write(pwemb::LEPrimaryAdvertisingPHY::LE_1M);
  view.secondary_advertising_phy().Write(
      pwemb::LESecondaryAdvertisingPHY::LE_1M);

  // Payload values were initialized to zero above. By not setting the values
  // for the following fields, we are purposely ignoring them:
  //
  // advertising_sid: We use only legacy PDUs, the controller ignores this field
  // in that case peer_address: We don't support directed advertising yet
  // peer_address_type: We don't support directed advertising yet
  // secondary_adv_max_skip: We use only legacy PDUs, the controller ignores
  // this field in that case

  return SetAdvertisingParams{std::move(packet), advertisement_id.value()};
}

std::optional<CommandPacket>
ExtendedLowEnergyAdvertiser::BuildSetAdvertisingRandomAddr(
    hci::AdvertisementId advertisement_id) const {
  auto packet = hci::CommandPacket::New<
      pwemb::LESetAdvertisingSetRandomAddressCommandWriter>(
      hci_spec::kLESetAdvertisingSetRandomAddress);
  auto view = packet.view_t();
  view.advertising_handle().Write(
      advertising_handle_map_.GetHandle(advertisement_id));
  DeviceAddress address = advertising_handle_map_.GetAddress(advertisement_id);
  view.random_address().CopyFrom(address.value().view());

  return packet;
}

// TODO(fxbug.dev/330935479): we can reduce code duplication by
// templatizing this method. However, we first have to rename
// advertising_data_length and advertising_data in
// LESetExtendedAdvertisingDataCommand to just data_length and data,
// respectively. We would also have to do the same in
// LESetExtendedScanResponseDataCommand. It's not worth it to do this rename
// right now since Sapphire lives in the Fuchsia repository and we would need to
// do an Emboss naming transition. Once Sapphire is back in the Pigweed
// repository, we can do this in one fell swoop rather than going through the
// Emboss naming transition.
CommandPacket ExtendedLowEnergyAdvertiser::BuildAdvertisingDataFragmentPacket(
    hci_spec::AdvertisingHandle handle,
    const BufferView& data,
    pwemb::LESetExtendedAdvDataOp operation,
    pwemb::LEExtendedAdvFragmentPreference fragment_preference) const {
  size_t kPayloadSize =
      pwemb::LESetExtendedAdvertisingDataCommandView::MinSizeInBytes().Read() +
      data.size();
  auto packet =
      CommandPacket::New<pwemb::LESetExtendedAdvertisingDataCommandWriter>(
          hci_spec::kLESetExtendedAdvertisingData, kPayloadSize);
  auto params = packet.view_t();

  params.advertising_handle().Write(handle);
  params.operation().Write(operation);
  params.fragment_preference().Write(fragment_preference);
  params.advertising_data_length().Write(static_cast<uint8_t>(data.size()));

  MutableBufferView data_view(params.advertising_data().BackingStorage().data(),
                              data.size());
  data.Copy(&data_view);

  return packet;
}

CommandPacket ExtendedLowEnergyAdvertiser::BuildScanResponseDataFragmentPacket(
    hci_spec::AdvertisingHandle handle,
    const BufferView& data,
    pwemb::LESetExtendedAdvDataOp operation,
    pwemb::LEExtendedAdvFragmentPreference fragment_preference) const {
  size_t kPayloadSize =
      pwemb::LESetExtendedScanResponseDataCommandView::MinSizeInBytes().Read() +
      data.size();
  auto packet =
      CommandPacket::New<pwemb::LESetExtendedScanResponseDataCommandWriter>(
          hci_spec::kLESetExtendedScanResponseData, kPayloadSize);
  auto params = packet.view_t();

  params.advertising_handle().Write(handle);
  params.operation().Write(operation);
  params.fragment_preference().Write(fragment_preference);
  params.scan_response_data_length().Write(static_cast<uint8_t>(data.size()));

  MutableBufferView data_view(
      params.scan_response_data().BackingStorage().data(), data.size());
  data.Copy(&data_view);

  return packet;
}

std::vector<CommandPacket> ExtendedLowEnergyAdvertiser::BuildSetAdvertisingData(
    hci::AdvertisementId advertisement_id,
    const AdvertisingData& data,
    AdvFlags flags) const {
  if (data.CalculateBlockSize() == 0) {
    std::vector<CommandPacket> packets;
    return packets;
  }

  hci_spec::AdvertisingHandle advertising_handle =
      advertising_handle_map_.GetHandle(advertisement_id);

  AdvertisingData adv_data;
  data.Copy(&adv_data);
  if (staged_advertising_parameters_.include_tx_power_level) {
    adv_data.SetTxPower(staged_advertising_parameters_.selected_tx_power_level);
  }

  size_t block_size = adv_data.CalculateBlockSize(/*include_flags=*/true);
  DynamicByteBuffer buffer(block_size);
  adv_data.WriteBlock(&buffer, flags);

  size_t max_length =
      pwemb::LESetExtendedAdvertisingDataCommand::advertising_data_length_max();

  // If all data fits into a single HCI packet, we don't need to do any
  // fragmentation ourselves. The Controller may still perform fragmentation
  // over the air but we don't have to when sending the data to the Controller.
  if (block_size <= max_length) {
    CommandPacket packet = BuildAdvertisingDataFragmentPacket(
        advertising_handle,
        buffer.view(),
        pwemb::LESetExtendedAdvDataOp::COMPLETE,
        pwemb::LEExtendedAdvFragmentPreference::SHOULD_NOT_FRAGMENT);

    std::vector<CommandPacket> packets;
    packets.reserve(1);
    packets.emplace_back(std::move(packet));
    return packets;
  }

  // We have more data than will fit in a single HCI packet. Calculate the
  // amount of packets we need to send, perform the fragmentation, and queue up
  // the multiple LE Set Extended Advertising Data packets to the Controller.
  size_t num_packets = block_size / max_length;
  if (block_size % max_length != 0) {
    num_packets++;
  }

  std::vector<CommandPacket> packets;
  packets.reserve(num_packets);

  for (size_t i = 0; i < num_packets; i++) {
    size_t packet_size = max_length;
    pwemb::LESetExtendedAdvDataOp operation =
        pwemb::LESetExtendedAdvDataOp::INTERMEDIATE_FRAGMENT;

    if (i == 0) {
      operation = pwemb::LESetExtendedAdvDataOp::FIRST_FRAGMENT;
    } else if (i == num_packets - 1) {
      operation = pwemb::LESetExtendedAdvDataOp::LAST_FRAGMENT;

      if (block_size % max_length != 0) {
        packet_size = block_size % max_length;
      }
    }

    size_t offset = i * max_length;
    BufferView buffer_view(buffer.data() + offset, packet_size);

    CommandPacket packet = BuildAdvertisingDataFragmentPacket(
        advertising_handle,
        buffer_view,
        operation,
        pwemb::LEExtendedAdvFragmentPreference::SHOULD_NOT_FRAGMENT);
    packets.push_back(packet);
  }

  return packets;
}

CommandPacket ExtendedLowEnergyAdvertiser::BuildUnsetAdvertisingData(
    hci::AdvertisementId advertisement_id) const {
  hci_spec::AdvertisingHandle advertising_handle =
      advertising_handle_map_.GetHandle(advertisement_id);

  constexpr size_t kPacketSize =
      pwemb::LESetExtendedAdvertisingDataCommandView::MinSizeInBytes().Read();
  auto packet =
      CommandPacket::New<pwemb::LESetExtendedAdvertisingDataCommandWriter>(
          hci_spec::kLESetExtendedAdvertisingData, kPacketSize);
  auto payload = packet.view_t();

  payload.advertising_handle().Write(advertising_handle);
  payload.operation().Write(pwemb::LESetExtendedAdvDataOp::COMPLETE);
  payload.fragment_preference().Write(
      pwemb::LEExtendedAdvFragmentPreference::SHOULD_NOT_FRAGMENT);
  payload.advertising_data_length().Write(0);

  return packet;
}

std::vector<CommandPacket> ExtendedLowEnergyAdvertiser::BuildSetScanResponse(
    hci::AdvertisementId advertisement_id, const AdvertisingData& data) const {
  if (data.CalculateBlockSize() == 0) {
    std::vector<CommandPacket> packets;
    return packets;
  }

  hci_spec::AdvertisingHandle advertising_handle =
      advertising_handle_map_.GetHandle(advertisement_id);

  AdvertisingData scan_rsp;
  data.Copy(&scan_rsp);
  if (staged_advertising_parameters_.include_tx_power_level) {
    scan_rsp.SetTxPower(staged_advertising_parameters_.selected_tx_power_level);
  }

  size_t block_size = scan_rsp.CalculateBlockSize(/*include_flags=*/false);
  DynamicByteBuffer buffer(block_size);
  scan_rsp.WriteBlock(&buffer, std::nullopt);

  size_t max_length = pwemb::LESetExtendedScanResponseDataCommand::
      scan_response_data_length_max();

  // If all data fits into a single HCI packet, we don't need to do any
  // fragmentation ourselves. The Controller may still perform fragmentation
  // over the air but we don't have to when sending the data to the Controller.
  if (block_size <= max_length) {
    CommandPacket packet = BuildScanResponseDataFragmentPacket(
        advertising_handle,
        buffer.view(),
        pwemb::LESetExtendedAdvDataOp::COMPLETE,
        pwemb::LEExtendedAdvFragmentPreference::SHOULD_NOT_FRAGMENT);

    std::vector<CommandPacket> packets;
    packets.reserve(1);
    packets.emplace_back(std::move(packet));
    return packets;
  }

  // We have more data than will fit in a single HCI packet. Calculate the
  // amount of packets we need to send, perform the fragmentation, and queue up
  // the multiple LE Set Extended Advertising Data packets to the Controller.
  size_t num_packets = block_size / max_length;
  if (block_size % max_length != 0) {
    num_packets++;
  }

  std::vector<CommandPacket> packets;
  packets.reserve(num_packets);

  for (size_t i = 0; i < num_packets; i++) {
    size_t packet_size = max_length;
    pwemb::LESetExtendedAdvDataOp operation =
        pwemb::LESetExtendedAdvDataOp::INTERMEDIATE_FRAGMENT;

    if (i == 0) {
      operation = pwemb::LESetExtendedAdvDataOp::FIRST_FRAGMENT;
    } else if (i == num_packets - 1) {
      operation = pwemb::LESetExtendedAdvDataOp::LAST_FRAGMENT;

      if (block_size % max_length != 0) {
        packet_size = block_size % max_length;
      }
    }

    size_t offset = i * max_length;
    BufferView buffer_view(buffer.data() + offset, packet_size);

    CommandPacket packet = BuildScanResponseDataFragmentPacket(
        advertising_handle,
        buffer_view,
        operation,
        pwemb::LEExtendedAdvFragmentPreference::SHOULD_NOT_FRAGMENT);
    packets.push_back(packet);
  }

  return packets;
}

CommandPacket ExtendedLowEnergyAdvertiser::BuildUnsetScanResponse(
    hci::AdvertisementId advertisement_id) const {
  hci_spec::AdvertisingHandle advertising_handle =
      advertising_handle_map_.GetHandle(advertisement_id);

  constexpr size_t kPacketSize =
      pwemb::LESetExtendedScanResponseDataCommandView::MinSizeInBytes().Read();
  auto packet =
      CommandPacket::New<pwemb::LESetExtendedScanResponseDataCommandWriter>(
          hci_spec::kLESetExtendedScanResponseData, kPacketSize);
  auto payload = packet.view_t();

  payload.advertising_handle().Write(advertising_handle);
  payload.operation().Write(pwemb::LESetExtendedAdvDataOp::COMPLETE);
  payload.fragment_preference().Write(
      pwemb::LEExtendedAdvFragmentPreference::SHOULD_NOT_FRAGMENT);
  payload.scan_response_data_length().Write(0);

  return packet;
}

std::optional<CommandPacket>
ExtendedLowEnergyAdvertiser::BuildRemoveAdvertisingSet(
    hci::AdvertisementId advertisement_id) const {
  hci_spec::AdvertisingHandle advertising_handle =
      advertising_handle_map_.GetHandle(advertisement_id);

  auto packet =
      hci::CommandPacket::New<pwemb::LERemoveAdvertisingSetCommandWriter>(
          hci_spec::kLERemoveAdvertisingSet);
  auto view = packet.view_t();
  view.advertising_handle().Write(advertising_handle);

  return packet;
}

void ExtendedLowEnergyAdvertiser::OnSetAdvertisingParamsComplete(
    const EventPacket& event) {
  auto event_view = event.view<pw::bluetooth::emboss::EventHeaderView>();
  PW_CHECK(event_view.event_code().Read() ==
           pw::bluetooth::emboss::EventCode::COMMAND_COMPLETE);

  auto cmd_complete_view =
      event.view<pw::bluetooth::emboss::CommandCompleteEventView>();
  PW_CHECK(
      cmd_complete_view.command_opcode().Read() ==
      pw::bluetooth::emboss::OpCode::LE_SET_EXTENDED_ADVERTISING_PARAMETERS_V1);

  Result<> result = event.ToResult();
  if (bt_is_error(result,
                  WARN,
                  "hci-le",
                  "set advertising parameters, error received: %s",
                  bt_str(result))) {
    return;  // full error handling done in super class, can just return here
  }

  auto view = event.view<
      pw::bluetooth::emboss::
          LESetExtendedAdvertisingParametersCommandCompleteEventView>();
  if (staged_advertising_parameters_.include_tx_power_level) {
    staged_advertising_parameters_.selected_tx_power_level =
        view.selected_tx_power().Read();
  }
}

void ExtendedLowEnergyAdvertiser::StartAdvertising(
    const DeviceAddress& address,
    const AdvertisingData& data,
    const AdvertisingData& scan_rsp,
    const AdvertisingOptions& options,
    ConnectionCallback connect_callback,
    ResultFunction<hci::AdvertisementId> result_callback) {
  // if there is an operation currently in progress, enqueue this operation and
  // we will get to it the next time we have a chance
  if (!hci_cmd_runner().IsReady()) {
    bt_log(DEBUG,
           "hci-le",
           "hci cmd runner not ready, queuing advertisement commands for now");

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

  fit::result<HostError> can_start_result =
      CanStartAdvertising(address, data, scan_rsp, options, connect_callback);
  if (can_start_result.is_error()) {
    result_callback(ToResult(can_start_result.error_value()).take_error());
    return;
  }

  staged_advertising_parameters_.clear();
  staged_advertising_parameters_.include_tx_power_level =
      options.include_tx_power_level;
  staged_advertising_parameters_.extended_pdu = options.extended_pdu;

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

  // Core Spec, Volume 4, Part E, Section 7.8.58: "the number of advertising
  // sets that can be supported is not fixed and the Controller can change it at
  // any time. The memory used to store advertising sets can also be used for
  // other purposes."
  //
  // Depending on the memory profile of the controller, a new advertising set
  // may or may not be accepted. We could use
  // HCI_LE_Read_Number_of_Supported_Advertising_Sets to check if the controller
  // has space for another advertising set. However, the value may change after
  // the read and before the addition of the advertising set. Furthermore,
  // sending an extra HCI command increases the latency of our stack. Instead,
  // we simply attempt to add. If the controller is unable to support another
  // advertising set, it will respond with a memory capacity exceeded error.
  StartAdvertisingInternal(address,
                           data,
                           scan_rsp,
                           options,
                           std::move(connect_callback),
                           std::move(result_cb_wrapper));
}

void ExtendedLowEnergyAdvertiser::StopAdvertising(
    fit::function<void(Result<>)> result_cb) {
  StopAdvertisingInternal(std::move(result_cb));
  advertising_handle_map_.Clear();

  // std::queue doesn't have a clear method so we have to resort to this
  // tomfoolery :(
  decltype(op_queue_) empty;
  std::swap(op_queue_, empty);
}

void ExtendedLowEnergyAdvertiser::StopAdvertising(
    hci::AdvertisementId advertisement_id,
    fit::function<void(Result<>)> result_cb) {
  // if there is an operation currently in progress, enqueue this operation and
  // we will get to it the next time we have a chance
  if (!hci_cmd_runner().IsReady()) {
    bt_log(
        DEBUG,
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

void ExtendedLowEnergyAdvertiser::OnIncomingConnection(
    hci_spec::ConnectionHandle handle,
    pwemb::ConnectionRole role,
    const DeviceAddress& peer_address,
    const hci_spec::LEConnectionParameters& conn_params) {
  // Core Spec Volume 4, Part E, Section 7.8.56: Incoming connections to LE
  // Extended Advertising occur through two events: HCI_LE_Connection_Complete
  // and HCI_LE_Advertising_Set_Terminated. This method is called as a result of
  // the HCI_LE_Connection_Complete event. At this point, we only have a
  // connection handle but don't know the locally advertised address that the
  // connection is for. Until we receive the HCI_LE_Advertising_Set_Terminated
  // event, we stage these parameters.
  staged_connections_[handle] = {role, peer_address, conn_params};
}

// The HCI_LE_Advertising_Set_Terminated event contains the mapping between
// connection handle and advertising handle. After the
// HCI_LE_Advertising_Set_Terminated event, we have all the information
// necessary to create a connection object within the Host layer.
void ExtendedLowEnergyAdvertiser::OnAdvertisingSetTerminatedEvent(
    const EventPacket& event) {
  Result<> result = event.ToResult();
  if (bt_is_error(result,
                  ERROR,
                  "hci-le",
                  "advertising set terminated event, error received %s",
                  bt_str(result))) {
    return;
  }

  auto params = event.view<pwemb::LEAdvertisingSetTerminatedSubeventView>();

  hci_spec::ConnectionHandle connection_handle =
      params.connection_handle().Read();
  auto staged_parameters_node = staged_connections_.extract(connection_handle);

  if (staged_parameters_node.empty()) {
    bt_log(ERROR,
           "hci-le",
           "advertising set terminated event, staged params not available "
           "(handle: %d)",
           params.advertising_handle().Read());
    return;
  }

  hci_spec::AdvertisingHandle adv_handle = params.advertising_handle().Read();
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

  StagedConnectionParameters staged = staged_parameters_node.mapped();

  CompleteIncomingConnection(connection_handle,
                             staged.role,
                             local_address,
                             staged.peer_address,
                             staged.conn_params,
                             advertisement_id);

  staged_advertising_parameters_.clear();
}

void ExtendedLowEnergyAdvertiser::OnCurrentOperationComplete() {
  if (op_queue_.empty()) {
    return;  // no more queued operations so nothing to do
  }

  fit::closure closure = std::move(op_queue_.front());
  op_queue_.pop();
  closure();
}

}  // namespace bt::hci
