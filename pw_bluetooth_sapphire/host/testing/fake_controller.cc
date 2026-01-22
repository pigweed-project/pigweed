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

#include "pw_bluetooth_sapphire/internal/host/testing/fake_controller.h"

#include <pw_assert/check.h>
#include <pw_bytes/endian.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "pw_bluetooth/hci_android.emb.h"
#include "pw_bluetooth/hci_data.emb.h"
#include "pw_bluetooth/hci_events.emb.h"
#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/common/packet_view.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/defaults.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/util.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/vendor_protocol.h"
#include "pw_bluetooth_sapphire/internal/host/hci/advertising_packet_filter.h"

namespace bt::testing {
namespace {

template <typename NUM_TYPE, typename ENUM_TYPE>
void SetBit(NUM_TYPE* num_type, ENUM_TYPE bit) {
  *num_type |= static_cast<NUM_TYPE>(bit);
}

template <typename NUM_TYPE, typename ENUM_TYPE>
void UnsetBit(NUM_TYPE* num_type, ENUM_TYPE bit) {
  *num_type &= ~static_cast<NUM_TYPE>(bit);
}

template <typename NUM_TYPE, typename ENUM_TYPE>
bool CheckBit(NUM_TYPE num_type, ENUM_TYPE bit) {
  return (num_type & static_cast<NUM_TYPE>(bit));
}

}  // namespace

namespace android_hci = hci_spec::vendor::android;
namespace android_emb = pw::bluetooth::vendor::android_hci;
namespace pwemb = pw::bluetooth::emboss;

void FakeController::Settings::ApplyDualModeDefaults() {
  le_connection_delay = std::chrono::seconds(0);
  hci_version = pwemb::CoreSpecificationVersion::V5_0;
  num_hci_command_packets = 250;
  event_mask = 0;
  le_event_mask = 0;
  bd_addr = DeviceAddress();
  lmp_features_page0 = 0;
  SetBit(&lmp_features_page0, hci_spec::LMPFeature::kLESupportedHost);
  SetBit(&lmp_features_page0, hci_spec::LMPFeature::kSimultaneousLEAndBREDR);
  SetBit(&lmp_features_page0, hci_spec::LMPFeature::kExtendedFeatures);
  SetBit(&lmp_features_page0, hci_spec::LMPFeature::kRSSIwithInquiryResults);
  SetBit(&lmp_features_page0, hci_spec::LMPFeature::kExtendedInquiryResponse);
  SetBit(&lmp_features_page0,
         hci_spec::LMPFeature::kSecureSimplePairingControllerSupport);
  lmp_features_page1 = 0;
  SetBit(&lmp_features_page1,
         hci_spec::LMPFeature::kSecureSimplePairingHostSupport);
  lmp_features_page2 = 0;
  le_features = 0;
  le_supported_states = 0;
  std::memset(supported_commands, 0, sizeof(supported_commands));
  AddBREDRSupportedCommands();
  AddLESupportedCommands();
  acl_data_packet_length = 512;
  total_num_acl_data_packets = 1;
  le_acl_data_packet_length = 512;
  le_total_num_acl_data_packets = 1;
  // Must be 0x01-0xFF, even if not supported
  synchronous_data_packet_length = 1;
  total_num_synchronous_data_packets = 0;
  iso_data_packet_length = 512;
  total_num_iso_data_packets = 1;
  android_extension_settings.SetToZeros();
}

void FakeController::Settings::ApplyLEOnlyDefaults() {
  ApplyDualModeDefaults();

  UnsetBit(&lmp_features_page0, hci_spec::LMPFeature::kSimultaneousLEAndBREDR);
  SetBit(&lmp_features_page0, hci_spec::LMPFeature::kBREDRNotSupported);
  std::memset(supported_commands, 0, sizeof(supported_commands));

  AddLESupportedCommands();
}

void FakeController::Settings::AddBREDRSupportedCommands() {
  auto view = SupportedCommandsView();
  view.create_connection().Write(true);
  view.create_connection_cancel().Write(true);
  view.disconnect().Write(true);
  view.write_local_name().Write(true);
  view.read_local_name().Write(true);
  view.read_scan_enable().Write(true);
  view.write_scan_enable().Write(true);
  view.read_page_scan_activity().Write(true);
  view.write_page_scan_activity().Write(true);
  view.write_class_of_device().Write(true);
  view.write_synchronous_flow_control_enable().Write(true);
  view.read_inquiry_mode().Write(true);
  view.write_inquiry_mode().Write(true);
  view.read_page_scan_type().Write(true);
  view.write_page_scan_type().Write(true);
  view.read_buffer_size().Write(true);
  view.read_simple_pairing_mode().Write(true);
  view.write_simple_pairing_mode().Write(true);
  view.write_extended_inquiry_response().Write(true);
  view.write_secure_connections_host_support().Write(true);
}

void FakeController::Settings::AddLESupportedCommands() {
  auto view = SupportedCommandsView();
  view.disconnect().Write(true);
  view.set_event_mask().Write(true);
  view.reset().Write(true);
  view.read_local_version_information().Write(true);
  view.read_local_supported_features().Write(true);
  view.read_local_extended_features().Write(true);
  view.write_le_host_support().Write(true);
  view.le_set_event_mask().Write(true);
  view.le_read_buffer_size_v1().Write(true);
  view.le_read_local_supported_features().Write(true);
  view.le_set_random_address().Write(true);
  view.le_set_advertising_parameters().Write(true);
  view.le_set_advertising_data().Write(true);
  view.le_set_scan_response_data().Write(true);
  view.le_set_advertising_enable().Write(true);
  view.le_create_connection().Write(true);
  view.le_create_connection_cancel().Write(true);
  view.le_connection_update().Write(true);
  view.le_read_remote_features().Write(true);
  view.le_start_encryption().Write(true);
  view.le_read_buffer_size_v2().Write(true);
  view.read_local_supported_controller_delay().Write(true);
}

void FakeController::Settings::ApplyLegacyLEConfig() {
  ApplyLEOnlyDefaults();

  hci_version = pwemb::CoreSpecificationVersion::V4_2;

  auto view = pwemb::MakeSupportedCommandsView(supported_commands,
                                               sizeof(supported_commands));
  view.le_set_scan_parameters().Write(true);
  view.le_set_scan_enable().Write(true);
}

void FakeController::Settings::ApplyExtendedLEConfig() {
  ApplyLEOnlyDefaults();

  SetBit(&le_features, hci_spec::LESupportedFeature::kLEExtendedAdvertising);

  auto view = SupportedCommandsView();
  view.le_set_advertising_set_random_address().Write(true);
  view.le_set_extended_advertising_parameters().Write(true);
  view.le_set_extended_advertising_data().Write(true);
  view.le_set_extended_scan_response_data().Write(true);
  view.le_set_extended_advertising_enable().Write(true);
  view.le_read_maximum_advertising_data_length().Write(true);
  view.le_read_number_of_supported_advertising_sets().Write(true);
  view.le_remove_advertising_set().Write(true);
  view.le_clear_advertising_sets().Write(true);
}

void FakeController::Settings::ApplyAndroidVendorExtensionDefaults() {
  // Settings for the android vendor extensions component within the Fake
  // Controller. These settings correspond to the vendor capabilities returned
  // by the controller. See hci_vendor.emb LEGetVendorCapabilities for more
  // information.
  auto view = android_extension_settings.view();
  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.max_advt_instances().Write(3);
  view.version_supported().major_number().Write(0);
  view.version_supported().minor_number().Write(55);
  view.total_scan_results_storage().Write(1);
}

bool FakeController::Settings::is_event_unmasked(
    hci_spec::LEEventMask event) const {
  return le_event_mask | static_cast<uint64_t>(event);
}

void FakeController::SetDefaultCommandStatus(hci_spec::OpCode opcode,
                                             pwemb::StatusCode status) {
  default_command_status_map_[opcode] = status;
}

void FakeController::ClearDefaultCommandStatus(hci_spec::OpCode opcode) {
  default_command_status_map_.erase(opcode);
}

void FakeController::SetDefaultResponseStatus(hci_spec::OpCode opcode,
                                              pwemb::StatusCode status) {
  PW_DCHECK(status != pwemb::StatusCode::SUCCESS);
  default_status_map_[opcode] = status;
}

void FakeController::ClearDefaultResponseStatus(hci_spec::OpCode opcode) {
  default_status_map_.erase(opcode);
}

void FakeController::SetDefaultAndroidResponseStatus(
    hci_spec::OpCode opcode,
    uint8_t subopcode,
    pw::bluetooth::emboss::StatusCode status) {
  PW_DCHECK(status != pwemb::StatusCode::SUCCESS);
  default_android_status_map_.emplace(std::make_pair(opcode, subopcode),
                                      status);
}
void FakeController::ClearDefaultAndroidResponseStatus(hci_spec::OpCode opcode,
                                                       uint8_t subopcode) {
  default_android_status_map_.erase(std::make_pair(opcode, subopcode));
}

bool FakeController::AddPeer(std::unique_ptr<FakePeer> peer) {
  PW_DCHECK(peer);

  if (peers_.count(peer->address()) != 0u) {
    return false;
  }

  peer->set_controller(this);

  // If a scan is enabled then send an advertising report for the peer that just
  // got registered if it supports advertising.
  if (peer->send_advertising_report()) {
    SendAdvertisingReport(*peer);
    SendScanResponseReport(*peer);
  }

  peers_[peer->address()] = std::move(peer);
  return true;
}

void FakeController::RemovePeer(const DeviceAddress& address) {
  peers_.erase(address);
}

FakePeer* FakeController::FindPeer(const DeviceAddress& address) {
  auto iter = peers_.find(address);
  return (iter == peers_.end()) ? nullptr : iter->second.get();
}

void FakeController::SendCommand(pw::span<const std::byte> command) {
  PW_CHECK(command.size() >= sizeof(hci_spec::CommandHeader));

  // Post the packet to simulate async HCI behavior.
  (void)heap_dispatcher().Post(
      [self = GetWeakPtr(), command = DynamicByteBuffer(BufferView(command))](
          pw::async::Context /*ctx*/, pw::Status status) {
        if (!self.is_alive() || !status.ok()) {
          return;
        }
        const size_t payload_size =
            command.size() - sizeof(hci_spec::CommandHeader);
        PacketView<hci_spec::CommandHeader> packet_view(&command, payload_size);
        self->OnCommandPacketReceived(packet_view);
      });
}

FakePeer* FakeController::FindByConnHandle(hci_spec::ConnectionHandle handle) {
  for (auto& [addr, peer] : peers_) {
    if (peer->HasLink(handle)) {
      return peer.get();
    }
  }
  return nullptr;
}

uint8_t FakeController::NextL2CAPCommandId() {
  // TODO(armansito): Guard against overflow?
  return next_le_sig_id_++;
}

void FakeController::RespondWithCommandComplete(pwemb::OpCode opcode,
                                                pwemb::StatusCode status) {
  auto packet = hci::EventPacket::New<pwemb::SimpleCommandCompleteEventWriter>(
      hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.status().Write(status);
  RespondWithCommandComplete(opcode, &packet);
}

void FakeController::RespondWithCommandComplete(pwemb::OpCode opcode,
                                                hci::EventPacket* packet) {
  auto header = packet->template view<pwemb::CommandCompleteEventWriter>();

  header.num_hci_command_packets().Write(settings_.num_hci_command_packets);
  header.command_opcode().Write(opcode);

  SendEvent(hci_spec::kCommandCompleteEventCode, packet);
}

void FakeController::RespondWithCommandStatus(pwemb::OpCode opcode,
                                              pwemb::StatusCode status) {
  auto packet = hci::EventPacket::New<pwemb::CommandStatusEventWriter>(
      hci_spec::kCommandStatusEventCode);
  auto view = packet.view_t();
  view.status().Write(status);
  view.num_hci_command_packets().Write(settings_.num_hci_command_packets);
  view.command_opcode_enum().Write(opcode);

  SendEvent(hci_spec::kCommandStatusEventCode, &packet);
}

void FakeController::SendEvent(hci_spec::EventCode event_code,
                               hci::EventPacket* packet) {
  auto header = packet->template view<pwemb::EventHeaderWriter>();
  uint8_t parameter_total_size = static_cast<uint8_t>(
      packet->size() - pwemb::EventHeader::IntrinsicSizeInBytes());

  header.event_code_uint().Write(event_code);
  header.parameter_total_size().Write(parameter_total_size);

  SendCommandChannelPacket(packet->data());
}

void FakeController::SendACLPacket(hci_spec::ConnectionHandle handle,
                                   const ByteBuffer& payload) {
  PW_DCHECK(payload.size() <= hci_spec::kMaxACLPayloadSize);

  DynamicByteBuffer buffer(sizeof(hci_spec::ACLDataHeader) + payload.size());
  MutablePacketView<hci_spec::ACLDataHeader> acl(&buffer, payload.size());

  acl.mutable_header()->handle_and_flags =
      pw::bytes::ConvertOrderTo(cpp20::endian::little, handle);
  acl.mutable_header()->data_total_length = pw::bytes::ConvertOrderTo(
      cpp20::endian::little, static_cast<uint16_t>(payload.size()));
  acl.mutable_payload_data().Write(payload);

  SendACLDataChannelPacket(buffer);
}

void FakeController::SendL2CAPBFrame(hci_spec::ConnectionHandle handle,
                                     l2cap::ChannelId channel_id,
                                     const ByteBuffer& payload) {
  PW_DCHECK(payload.size() <=
            hci_spec::kMaxACLPayloadSize - sizeof(l2cap::BasicHeader));

  DynamicByteBuffer buffer(sizeof(l2cap::BasicHeader) + payload.size());
  MutablePacketView<l2cap::BasicHeader> bframe(&buffer, payload.size());

  bframe.mutable_header()->length = pw::bytes::ConvertOrderTo(
      cpp20::endian::little, static_cast<uint16_t>(payload.size()));
  bframe.mutable_header()->channel_id =
      pw::bytes::ConvertOrderTo(cpp20::endian::little, channel_id);
  bframe.mutable_payload_data().Write(payload);

  SendACLPacket(handle, buffer);
}

void FakeController::SendL2CAPCFrame(hci_spec::ConnectionHandle handle,
                                     bool is_le,
                                     l2cap::CommandCode code,
                                     uint8_t id,
                                     const ByteBuffer& payload) {
  DynamicByteBuffer buffer(sizeof(l2cap::CommandHeader) + payload.size());
  MutablePacketView<l2cap::CommandHeader> cframe(&buffer, payload.size());

  cframe.mutable_header()->code = code;
  cframe.mutable_header()->id = id;
  cframe.mutable_header()->length = static_cast<uint16_t>(payload.size());
  cframe.mutable_payload_data().Write(payload);

  SendL2CAPBFrame(
      handle,
      is_le ? l2cap::kLESignalingChannelId : l2cap::kSignalingChannelId,
      buffer);
}

void FakeController::SendNumberOfCompletedPacketsEvent(
    hci_spec::ConnectionHandle handle, uint16_t num) {
  constexpr size_t buffer_size =
      pwemb::NumberOfCompletedPacketsEvent::MinSizeInBytes() +
      pwemb::NumberOfCompletedPacketsEventData::IntrinsicSizeInBytes();
  auto event =
      hci::EventPacket::New<pwemb::NumberOfCompletedPacketsEventWriter>(
          hci_spec::kNumberOfCompletedPacketsEventCode, buffer_size);
  auto view = event.view_t();

  view.num_handles().Write(1);
  view.nocp_data()[0].connection_handle().Write(handle);
  view.nocp_data()[0].num_completed_packets().Write(num);

  SendEvent(hci_spec::kNumberOfCompletedPacketsEventCode, &event);
}

void FakeController::ConnectLowEnergy(const DeviceAddress& addr,
                                      pwemb::ConnectionRole role) {
  (void)heap_dispatcher().Post(
      [addr, role, this](pw::async::Context /*ctx*/, pw::Status status) {
        if (!status.ok()) {
          return;
        }
        FakePeer* peer = FindPeer(addr);
        if (!peer) {
          bt_log(WARN,
                 "fake-hci",
                 "no peer found with address: %s",
                 addr.ToString().c_str());
          return;
        }

        // TODO(armansito): Don't worry about managing multiple links per peer
        // until this supports Bluetooth classic.
        if (peer->connected()) {
          bt_log(WARN, "fake-hci", "peer already connected");
          return;
        }

        hci_spec::ConnectionHandle handle = ++next_conn_handle_;
        peer->AddLink(handle);

        NotifyConnectionState(addr, handle, /*connected=*/true);

        uint16_t interval_min = hci_spec::defaults::kLEConnectionIntervalMin;
        uint16_t interval_max = hci_spec::defaults::kLEConnectionIntervalMax;
        uint16_t interval = interval_min + ((interval_max - interval_min) / 2);

        hci_spec::LEConnectionParameters conn_params(
            interval, 0, hci_spec::defaults::kLESupervisionTimeout);
        peer->set_le_params(conn_params);

        auto packet = hci::EventPacket::New<
            pwemb::LEEnhancedConnectionCompleteSubeventV1Writer>(
            hci_spec::kLEMetaEventCode);
        auto view = packet.view_t();
        view.le_meta_event().subevent_code().Write(
            hci_spec::kLEEnhancedConnectionCompleteSubeventCode);
        view.status().Write(pwemb::StatusCode::SUCCESS);
        view.peer_address().CopyFrom(addr.value().view());
        view.peer_address_type().Write(
            DeviceAddress::DeviceAddrToLeAddr(addr.type()));
        view.peripheral_latency().Write(conn_params.latency());
        view.connection_interval().Write(conn_params.interval());
        view.supervision_timeout().Write(conn_params.supervision_timeout());
        view.role().Write(role);
        view.connection_handle().Write(handle);
        SendCommandChannelPacket(packet.data());
      });
}

void FakeController::SendConnectionRequest(const DeviceAddress& addr,
                                           pwemb::LinkType link_type) {
  FakePeer* peer = FindPeer(addr);
  PW_CHECK(peer);
  peer->set_last_connection_request_link_type(link_type);

  bt_log(DEBUG,
         "fake-hci",
         "sending connection request (addr: %s, link: %s)",
         bt_str(addr),
         hci_spec::LinkTypeToString(link_type));
  auto packet = hci::EventPacket::New<pwemb::ConnectionRequestEventWriter>(
      hci_spec::kConnectionRequestEventCode);
  packet.view_t().bd_addr().CopyFrom(addr.value().view());
  packet.view_t().link_type().Write(link_type);
  SendCommandChannelPacket(packet.data());
}

void FakeController::L2CAPConnectionParameterUpdate(
    const DeviceAddress& addr,
    const hci_spec::LEPreferredConnectionParameters& params) {
  (void)heap_dispatcher().Post([addr, params, this](pw::async::Context /*ctx*/,
                                                    pw::Status status) {
    if (!status.ok()) {
      return;
    }
    FakePeer* peer = FindPeer(addr);
    if (!peer) {
      bt_log(WARN,
             "fake-hci",
             "no peer found with address: %s",
             addr.ToString().c_str());
      return;
    }

    if (!peer->connected()) {
      bt_log(WARN, "fake-hci", "peer not connected");
      return;
    }

    PW_DCHECK(!peer->logical_links().empty());

    l2cap::ConnectionParameterUpdateRequestPayload payload;
    payload.interval_min =
        pw::bytes::ConvertOrderTo(cpp20::endian::little, params.min_interval());
    payload.interval_max =
        pw::bytes::ConvertOrderTo(cpp20::endian::little, params.max_interval());
    payload.peripheral_latency =
        pw::bytes::ConvertOrderTo(cpp20::endian::little, params.max_latency());
    payload.timeout_multiplier = pw::bytes::ConvertOrderTo(
        cpp20::endian::little, params.supervision_timeout());

    // TODO(armansito): Instead of picking the first handle we should pick
    // the handle that matches the current LE-U link.
    SendL2CAPCFrame(*peer->logical_links().begin(),
                    /*is_le=*/true,
                    l2cap::kConnectionParameterUpdateRequest,
                    NextL2CAPCommandId(),
                    BufferView(&payload, sizeof(payload)));
  });
}

void FakeController::SendLEConnectionUpdateCompleteSubevent(
    hci_spec::ConnectionHandle handle,
    const hci_spec::LEConnectionParameters& params,
    pwemb::StatusCode status) {
  auto packet =
      hci::EventPacket::New<pwemb::LEConnectionUpdateCompleteSubeventWriter>(
          hci_spec::kLEMetaEventCode);
  auto view = packet.view_t();
  view.le_meta_event().subevent_code().Write(
      hci_spec::kLEConnectionUpdateCompleteSubeventCode);
  view.status().Write(status);
  view.connection_handle().Write(handle);
  view.connection_interval().UncheckedWrite(params.interval());
  view.peripheral_latency().Write(params.latency());
  view.supervision_timeout().UncheckedWrite(params.supervision_timeout());
  SendCommandChannelPacket(packet.data());
}

void FakeController::Disconnect(const DeviceAddress& addr,
                                pwemb::StatusCode reason) {
  (void)heap_dispatcher().Post(
      [this, addr, reason](pw::async::Context /*ctx*/, pw::Status status) {
        if (!status.ok()) {
          return;
        }
        FakePeer* peer = FindPeer(addr);
        if (!peer || !peer->connected()) {
          bt_log(WARN,
                 "fake-hci",
                 "no connected peer found with address: %s",
                 addr.ToString().c_str());
          return;
        }

        auto links = peer->Disconnect();
        PW_DCHECK(!peer->connected());
        PW_DCHECK(!links.empty());

        for (auto link : links) {
          NotifyConnectionState(addr, link, /*connected=*/false);
          SendDisconnectionCompleteEvent(link, reason);
        }
      });
}

void FakeController::SendDisconnectionCompleteEvent(
    hci_spec::ConnectionHandle handle, pwemb::StatusCode reason) {
  auto event = hci::EventPacket::New<pwemb::DisconnectionCompleteEventWriter>(
      hci_spec::kDisconnectionCompleteEventCode);
  event.view_t().status().Write(pwemb::StatusCode::SUCCESS);
  event.view_t().connection_handle().Write(handle);
  event.view_t().reason().Write(reason);
  SendCommandChannelPacket(event.data());
}

void FakeController::SendEncryptionChangeEvent(
    hci_spec::ConnectionHandle handle,
    pwemb::StatusCode status,
    pwemb::EncryptionStatus encryption_enabled) {
  auto response = hci::EventPacket::New<pwemb::EncryptionChangeEventV1Writer>(
      hci_spec::kEncryptionChangeEventCode);
  response.view_t().status().Write(status);
  response.view_t().connection_handle().Write(handle);
  response.view_t().encryption_enabled().Write(encryption_enabled);
  SendCommandChannelPacket(response.data());
}

bool FakeController::MaybeRespondWithDefaultCommandStatus(
    hci_spec::OpCode opcode) {
  auto iter = default_command_status_map_.find(opcode);
  if (iter == default_command_status_map_.end()) {
    return false;
  }

  RespondWithCommandStatus(static_cast<pwemb::OpCode>(opcode), iter->second);
  return true;
}

bool FakeController::MaybeRespondWithDefaultStatus(hci_spec::OpCode opcode) {
  auto iter = default_status_map_.find(opcode);
  if (iter == default_status_map_.end())
    return false;

  bt_log(INFO,
         "fake-hci",
         "responding with error (command: %#.4x, status: %#.2hhx)",
         opcode,
         static_cast<unsigned char>(iter->second));
  RespondWithCommandComplete(static_cast<pwemb::OpCode>(opcode), iter->second);
  return true;
}

bool FakeController::MaybeRespondWithDefaultAndroidStatus(
    hci_spec::OpCode opcode, uint8_t subopcode) {
  auto iter =
      default_android_status_map_.find(std::make_pair(opcode, subopcode));
  if (iter == default_android_status_map_.end()) {
    return false;
  }

  bt_log(INFO,
         "fake-hci",
         "responding with error (command: %#.4x, status: %#.2hhx)",
         opcode,
         static_cast<unsigned char>(iter->second));
  RespondWithCommandComplete(static_cast<pwemb::OpCode>(opcode), iter->second);
  return true;
}

void FakeController::SendInquiryResponses() {
  // TODO(jamuraa): combine some of these into a single response event
  for (const auto& [addr, peer] : peers_) {
    if (!peer->supports_bredr()) {
      continue;
    }

    SendCommandChannelPacket(peer->CreateInquiryResponseEvent(inquiry_mode_));
    inquiry_num_responses_left_--;
    if (inquiry_num_responses_left_ == 0) {
      break;
    }
  }
}

void FakeController::SendAdvertisingReports() {
  if (!le_scan_state_.enabled || peers_.empty()) {
    return;
  }

  for (const auto& iter : peers_) {
    if (iter.second->send_advertising_report()) {
      SendAdvertisingReport(*iter.second);
      SendScanResponseReport(*iter.second);
    }
  }

  // We'll send new reports for the same peers if duplicate filtering is
  // disabled.
  if (!le_scan_state_.filter_duplicates) {
    (void)heap_dispatcher().Post(
        [this](pw::async::Context /*ctx*/, pw::Status status) {
          if (status.ok()) {
            SendAdvertisingReports();
          }
        });
  }
}

void FakeController::SendPeriodicAdvertisingReports() {
  // Send Periodic Advertising report for each sync
  for (auto& [sync_handle, sync] : periodic_advertising_syncs_) {
    auto peer_iter = peers_.find(sync.peer_address);
    if (peer_iter == peers_.end()) {
      continue;
    }
    FakePeer* peer = peer_iter->second.get();

    if (!peer->HasPeriodicAdvertisement(sync.advertising_sid)) {
      continue;
    }

    SendPeriodicAdvertisingReport(*peer, sync_handle, sync.advertising_sid);
  }
}

void FakeController::SendPeriodicAdvertisingReport(
    FakePeer& peer, hci_spec::SyncHandle sync_handle, uint8_t advertising_sid) {
  PW_CHECK(peer.HasPeriodicAdvertisement(advertising_sid));
  DynamicByteBuffer report_event =
      peer.BuildPeriodicAdvertisingReportEvent(sync_handle, advertising_sid);
  SendCommandChannelPacket(report_event);
  std::optional<DynamicByteBuffer> big_info_event =
      peer.BuildBigInfoAdvertisingReportEvent(sync_handle, advertising_sid);
  if (big_info_event) {
    SendCommandChannelPacket(*big_info_event);
  }
}

void FakeController::MaybeSendPeriodicAdvertisingSyncEstablishedEvent() {
  if (!le_scan_state_.enabled || !pending_periodic_advertising_create_sync_) {
    return;
  }
  for (auto& entry : periodic_advertiser_list_) {
    auto peer_iter = peers_.find(entry.address);
    if (peer_iter == peers_.end()) {
      continue;
    }

    if (!peer_iter->second->HasPeriodicAdvertisement(entry.advertising_sid)) {
      continue;
    }

    bool already_synced = false;
    for (auto& [_, sync] : periodic_advertising_syncs_) {
      if (sync.peer_address == entry.address &&
          sync.advertising_sid == entry.advertising_sid) {
        already_synced = true;
        break;
      }
    }
    if (already_synced) {
      continue;
    }

    uint16_t sync_handle = next_periodic_advertising_sync_handle_++;

    periodic_advertising_syncs_.try_emplace(
        sync_handle,
        PeriodicAdvertisingSync{
            .peer_address = entry.address,
            .advertising_sid = entry.advertising_sid,
            .duplicate_filtering = pending_periodic_advertising_create_sync_
                                       ->duplicate_filtering});
    auto packet = hci::EventPacket::New<
        pwemb::LEPeriodicAdvertisingSyncEstablishedSubeventV2Writer>(
        hci_spec::kLEMetaEventCode);
    auto params = packet.view_t();
    params.le_meta_event().subevent_code_enum().Write(
        pw::bluetooth::emboss::LeSubEventCode::
            PERIODIC_ADVERTISING_SYNC_ESTABLISHED_V2);
    params.status().Write(pwemb::StatusCode::SUCCESS);
    params.sync_handle().Write(sync_handle);
    params.advertising_sid().Write(entry.advertising_sid);
    params.advertiser_address_type().Write(
        DeviceAddress::DeviceAddrToLeAddr(entry.address.type()));
    params.advertiser_address().CopyFrom(entry.address.value().view());
    params.advertiser_phy().Write(pw::bluetooth::emboss::LEPhy::LE_1M);
    params.periodic_advertising_interval().Write(0x0006);  // 7.5ms, the minimum
    params.advertiser_clock_accuracy().Write(pwemb::LEClockAccuracy::PPM_500);
    params.num_subevents().Write(0);
    params.subevent_interval().Write(0);      // No subevents
    params.response_slot_delay().Write(0);    // No response slots
    params.response_slot_spacing().Write(0);  // No response slots
    SendCommandChannelPacket(packet.data());
    pending_periodic_advertising_create_sync_.reset();

    SendPeriodicAdvertisingReport(
        *peer_iter->second, sync_handle, entry.advertising_sid);
    break;
  }
}

bool FakeController::DataMatchesWithMask(const std::vector<uint8_t>& a,
                                         const std::vector<uint8_t>& b,
                                         const std::vector<uint8_t>& mask) {
  if (a.size() != b.size()) {
    return false;
  }

  if (a.size() != mask.size()) {
    return false;
  }

  for (size_t i = 0; i < a.size(); ++i) {
    uint8_t byte_a = a[i] & mask[i];
    uint8_t byte_b = b[i] & mask[i];

    if (byte_a != byte_b) {
      return false;
    }
  }

  return true;
}

bool FakeController::FilterMatchesPeer(const FakePeer& p,
                                       const PacketFilter& f) {
  bool filter_broadcast_address = (f.features_selected.broadcast_address ==
                                   android_emb::ApcfFeatureFilterLogic::AND);
  bool filter_service_uuid = (f.features_selected.service_uuid ==
                              android_emb::ApcfFeatureFilterLogic::AND);
  bool filter_solicitation_uuid = (f.features_selected.solicitation_uuid ==
                                   android_emb::ApcfFeatureFilterLogic::AND);
  bool filter_local_name = (f.features_selected.local_name ==
                            android_emb::ApcfFeatureFilterLogic::AND);
  bool filter_service_data = (f.features_selected.service_data ==
                              android_emb::ApcfFeatureFilterLogic::AND);
  bool filter_manufacturer_data = (f.features_selected.manufacturer_data ==
                                   android_emb::ApcfFeatureFilterLogic::AND);

  if (filter_broadcast_address) {
    if (p.address().value() != f.broadcast_address.value()) {
      return false;
    }
  }

  // if there is no advertising data, check if we even needed to filter on
  // advertising data before returning true or false
  auto result = AdvertisingData::FromBytes(p.advertising_data());
  if (result.is_error()) {
    if (filter_service_uuid || filter_solicitation_uuid || filter_local_name ||
        filter_service_data || filter_manufacturer_data) {
      return false;
    }

    return true;
  }

  AdvertisingData ad = std::move(result.value());

  if (filter_service_uuid) {
    bool matches = false;

    for (const UUID& uuid : ad.service_uuids()) {
      if (f.service_uuid == uuid) {
        matches = true;
        break;
      }
    }

    if (!matches) {
      return false;
    }
  }

  if (filter_solicitation_uuid) {
    bool matches = false;

    for (const UUID& uuid : ad.solicitation_uuids()) {
      if (f.solicitation_uuid == uuid) {
        matches = true;
        break;
      }
    }

    if (!matches) {
      return false;
    }
  }

  if (filter_local_name) {
    if (!ad.local_name().has_value()) {
      return false;
    }

    if (ad.local_name().value().name.find(f.local_name.value()) ==
        std::string::npos) {
      return false;
    }
  }

  if (filter_service_data) {
    bool matches = false;

    for (const UUID& uuid : ad.service_data_uuids()) {
      BufferView view = ad.service_data(uuid);
      std::vector<uint8_t> ad_service_data(view.data(),
                                           view.data() + view.size());
      if (DataMatchesWithMask(ad_service_data,
                              f.service_data.value(),
                              f.service_data_mask.value())) {
        matches = true;
        break;
      }
    }

    if (!matches) {
      return false;
    }
  }

  if (filter_manufacturer_data) {
    bool matches = false;

    for (uint16_t manufacturer_data_id : ad.manufacturer_data_ids()) {
      BufferView view = ad.manufacturer_data(manufacturer_data_id);
      std::vector<uint8_t> ad_manufacturer_data(view.data(),
                                                view.data() + view.size());
      if (DataMatchesWithMask(ad_manufacturer_data,
                              f.manufacturer_data.value(),
                              f.manufacturer_data_mask.value())) {
        matches = true;
        break;
      }
    }

    if (!matches) {
      return false;
    }
  }

  return true;
}

void FakeController::SendAdvertisingReport(const FakePeer& peer) {
  if (!le_scan_state_.enabled) {
    return;
  }

  if (!peer.supports_le()) {
    return;
  }

  if (!peer.advertising_enabled()) {
    return;
  }

  DynamicByteBuffer buffer;
  if (scan_procedure() == ExtendedOperationType::kExtended) {
    buffer = peer.BuildExtendedAdvertisingReportEvent();
  } else {
    buffer = peer.BuildLegacyAdvertisingReportEvent();
  }

  if (!packet_filter_state_.enabled) {
    SendCommandChannelPacket(buffer);
    return;
  }

  if (packet_filter_state_.filters.empty()) {
    SendCommandChannelPacket(buffer);
    return;
  }

  for (const auto& [filter_index, filter] : packet_filter_state_.filters) {
    if (FilterMatchesPeer(peer, filter)) {
      SendCommandChannelPacket(buffer);
      return;
    }
  }
}

void FakeController::SendScanResponseReport(const FakePeer& peer) {
  if (!le_scan_state_.enabled) {
    return;
  }

  if (!peer.supports_le()) {
    return;
  }

  if (!peer.advertising_enabled()) {
    return;
  }

  // We want to send scan response packets only during an active scan and if the
  // peer is scannable.
  bool is_active_scan = (le_scan_state_.scan_type == pwemb::LEScanType::ACTIVE);
  bool need_scan_rsp = (is_active_scan && peer.scannable());

  if (!need_scan_rsp) {
    return;
  }

  DynamicByteBuffer buffer;
  if (scan_procedure() == ExtendedOperationType::kExtended) {
    buffer = peer.BuildExtendedScanResponseEvent();
  } else {
    buffer = peer.BuildLegacyScanResponseReportEvent();
  }

  if (!packet_filter_state_.enabled) {
    SendCommandChannelPacket(buffer);
    return;
  }

  if (packet_filter_state_.filters.empty()) {
    SendCommandChannelPacket(buffer);
    return;
  }

  for (const auto& [filter_index, filter] : packet_filter_state_.filters) {
    if (FilterMatchesPeer(peer, filter)) {
      SendCommandChannelPacket(buffer);
      return;
    }
  }
}

void FakeController::LosePeriodicSync(DeviceAddress address,
                                      uint8_t advertising_sid) {
  auto iter =
      std::find_if(periodic_advertising_syncs_.begin(),
                   periodic_advertising_syncs_.end(),
                   [&](auto& sync) {
                     return sync.second.peer_address == address &&
                            sync.second.advertising_sid == advertising_sid;
                   });
  PW_CHECK(iter != periodic_advertising_syncs_.end());

  auto sync_lost =
      hci::EventPacket::New<pwemb::LEPeriodicAdvertisingSyncLostSubeventWriter>(
          hci_spec::kLEMetaEventCode);
  auto view = sync_lost.view_t();
  view.le_meta_event().subevent_code_enum().Write(
      pwemb::LeSubEventCode::PERIODIC_ADVERTISING_SYNC_LOST);
  view.sync_handle().Write(iter->first);
  SendCommandChannelPacket(sync_lost.data());

  periodic_advertising_syncs_.erase(iter);
}

void FakeController::NotifyControllerParametersChanged() {
  if (controller_parameters_cb_) {
    controller_parameters_cb_();
  }
}

void FakeController::NotifyAdvertisingState() {
  if (advertising_state_cb_) {
    advertising_state_cb_();
  }
}

void FakeController::NotifyConnectionState(const DeviceAddress& addr,
                                           hci_spec::ConnectionHandle handle,
                                           bool connected,
                                           bool canceled) {
  if (conn_state_cb_) {
    conn_state_cb_(addr, handle, connected, canceled);
  }
}

void FakeController::NotifyLEConnectionParameters(
    const DeviceAddress& addr, const hci_spec::LEConnectionParameters& params) {
  if (le_conn_params_cb_) {
    le_conn_params_cb_(addr, params);
  }
}

void FakeController::CaptureLEConnectParams(
    const pwemb::LECreateConnectionCommandView& params) {
  le_connect_params_ = LEConnectParams();

  switch (params.initiator_filter_policy().Read()) {
    case pwemb::GenericEnableParam::ENABLE:
      le_connect_params_->use_filter_policy = true;
      break;
    case pwemb::GenericEnableParam::DISABLE:
      le_connect_params_->use_filter_policy = false;
      break;
  }

  le_connect_params_->own_address_type = params.own_address_type().Read();
  le_connect_params_->peer_address = DeviceAddress(
      DeviceAddress::LeAddrToDeviceAddr(params.peer_address_type().Read()),
      DeviceAddressBytes(params.peer_address()));

  LEConnectParams::Parameters& connect_params =
      le_connect_params_
          ->phy_conn_params[LEConnectParams::InitiatingPHYs::kLE_1M];
  connect_params.scan_interval = params.le_scan_interval().Read();
  connect_params.scan_window = params.le_scan_window().Read();
  connect_params.connection_interval_min =
      params.connection_interval_min().Read();
  connect_params.connection_interval_max =
      params.connection_interval_max().Read();
  connect_params.max_latency = params.max_latency().Read();
  connect_params.supervision_timeout = params.supervision_timeout().Read();
  connect_params.min_ce_length = params.min_connection_event_length().Read();
  connect_params.max_ce_length = params.max_connection_event_length().Read();
}

void FakeController::CaptureLEConnectParamsForPHY(
    const pwemb::LEExtendedCreateConnectionCommandV1View& params,
    LEConnectParams::InitiatingPHYs phy) {
  int index = static_cast<int>(phy);

  LEConnectParams::Parameters& connect_params =
      le_connect_params_->phy_conn_params[phy];
  connect_params.scan_interval = params.data()[index].scan_interval().Read();
  connect_params.scan_window = params.data()[index].scan_window().Read();
  connect_params.connection_interval_min =
      params.data()[index].connection_interval_min().Read();
  connect_params.connection_interval_min =
      params.data()[index].connection_interval_max().Read();
  connect_params.max_latency = params.data()[index].max_latency().Read();
  connect_params.supervision_timeout =
      params.data()[index].supervision_timeout().Read();
  connect_params.min_ce_length =
      params.data()[index].min_connection_event_length().Read();
  connect_params.max_ce_length =
      params.data()[index].max_connection_event_length().Read();
}

void FakeController::CaptureLEConnectParams(
    const pwemb::LEExtendedCreateConnectionCommandV1View& params) {
  le_connect_params_ = LEConnectParams();

  switch (params.initiator_filter_policy().Read()) {
    case pwemb::GenericEnableParam::ENABLE:
      le_connect_params_->use_filter_policy = true;
      break;
    case pwemb::GenericEnableParam::DISABLE:
      le_connect_params_->use_filter_policy = false;
      break;
  }

  le_connect_params_->own_address_type = params.own_address_type().Read();
  le_connect_params_->peer_address = DeviceAddress(
      DeviceAddress::LeAddrToDeviceAddr(params.peer_address_type().Read()),
      DeviceAddressBytes(params.peer_address()));

  CaptureLEConnectParamsForPHY(params, LEConnectParams::InitiatingPHYs::kLE_1M);
  CaptureLEConnectParamsForPHY(params, LEConnectParams::InitiatingPHYs::kLE_2M);
  CaptureLEConnectParamsForPHY(params,
                               LEConnectParams::InitiatingPHYs::kLE_Coded);
}

void FakeController::OnCreateConnectionCommandReceived(
    const pwemb::CreateConnectionCommandView& params) {
  acl_create_connection_command_count_++;

  // Cannot issue this command while a request is already pending.
  if (bredr_connect_pending_) {
    RespondWithCommandStatus(pwemb::OpCode::CREATE_CONNECTION,
                             pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  const DeviceAddress peer_address(DeviceAddress::Type::kBREDR,
                                   DeviceAddressBytes(params.bd_addr()));
  pwemb::StatusCode status = pwemb::StatusCode::SUCCESS;

  // Find the peer that matches the requested address.
  FakePeer* peer = FindPeer(peer_address);
  if (peer) {
    if (peer->connected()) {
      status = pwemb::StatusCode::CONNECTION_ALREADY_EXISTS;
    } else {
      status = peer->connect_status();
    }
  }

  // First send the Command Status response.
  RespondWithCommandStatus(pwemb::OpCode::CREATE_CONNECTION, status);

  // If we just sent back an error status then the operation is complete.
  if (status != pwemb::StatusCode::SUCCESS)
    return;

  bredr_connect_pending_ = true;
  pending_bredr_connect_addr_ = peer_address;

  // The procedure was initiated successfully but the peer cannot be connected
  // because it either doesn't exist or isn't connectable.
  if (!peer || !peer->connectable()) {
    bt_log(INFO,
           "fake-hci",
           "requested peer %s cannot be connected; request will time out",
           peer_address.ToString().c_str());

    bredr_connect_rsp_task_.Cancel();
    bredr_connect_rsp_task_.set_function(
        [this, peer_address](pw::async::Context /*ctx*/, pw::Status status) {
          if (!status.ok()) {
            return;
          }
          bredr_connect_pending_ = false;

          auto response =
              hci::EventPacket::New<pwemb::ConnectionCompleteEventWriter>(
                  hci_spec::kConnectionCompleteEventCode);
          response.view_t().status().Write(pwemb::StatusCode::PAGE_TIMEOUT);
          response.view_t().bd_addr().CopyFrom(peer_address.value().view());
          SendCommandChannelPacket(response.data());
        });

    // Default page timeout of 5.12s
    // See Core Spec v5.0 Vol 2, Part E, Section 6.6
    constexpr pw::chrono::SystemClock::duration default_page_timeout =
        std::chrono::microseconds(static_cast<int64_t>(625) * 0x2000);
    bredr_connect_rsp_task_.PostAfter(default_page_timeout);
    return;
  }

  if (next_conn_handle_ == 0x0FFF) {
    // Ran out of handles
    status = pwemb::StatusCode::CONNECTION_LIMIT_EXCEEDED;
  } else {
    status = peer->connect_response();
  }

  auto response = hci::EventPacket::New<pwemb::ConnectionCompleteEventWriter>(
      hci_spec::kConnectionCompleteEventCode);
  response.view_t().status().Write(status);
  response.view_t().bd_addr().CopyFrom(params.bd_addr());
  response.view_t().link_type().Write(pwemb::LinkType::ACL);
  response.view_t().encryption_enabled().Write(
      pwemb::GenericEnableParam::DISABLE);

  if (status == pwemb::StatusCode::SUCCESS) {
    hci_spec::ConnectionHandle handle = ++next_conn_handle_;
    response.view_t().connection_handle().Write(handle);
  }

  // Don't send a connection event if we were asked to force the request to
  // remain pending. This is used by test cases that operate during the pending
  // state.
  if (peer->force_pending_connect()) {
    return;
  }

  bredr_connect_rsp_task_.Cancel();
  bredr_connect_rsp_task_.set_function(
      [response, peer, this](pw::async::Context /*ctx*/,
                             pw::Status status) mutable {
        if (!status.ok()) {
          return;
        }
        bredr_connect_pending_ = false;

        if (response.view_t().status().Read() == pwemb::StatusCode::SUCCESS) {
          bool notify = !peer->connected();
          hci_spec::ConnectionHandle handle =
              response.view_t().connection_handle().Read();
          peer->AddLink(handle);
          if (notify && peer->connected()) {
            NotifyConnectionState(peer->address(), handle, /*connected=*/true);
          }
        }

        SendCommandChannelPacket(response.data());
      });
  bredr_connect_rsp_task_.Post();
}

void FakeController::OnLECreateConnectionCommandReceived(
    const pwemb::LECreateConnectionCommandView& params) {
  le_create_connection_command_count_++;

  if (advertising_procedure() == ExtendedOperationType::kExtended) {
    RespondWithCommandStatus(pwemb::OpCode::LE_CREATE_CONNECTION,
                             pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  if (le_create_connection_cb_) {
    le_create_connection_cb_(params);
  }

  // Cannot issue this command while a request is already pending.
  if (le_connect_pending_) {
    RespondWithCommandStatus(pwemb::OpCode::LE_CREATE_CONNECTION,
                             pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  // The link is considered lost after connection_interval_max * 2. Connection
  // events (when data pdus are transmitted) must occur at least once within
  // that time frame.
  if (params.max_connection_event_length().Read() >
      2 * params.connection_interval_max().Read()) {
    RespondWithCommandStatus(pwemb::OpCode::LE_CREATE_CONNECTION,
                             pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  std::optional<DeviceAddress::Type> addr_type =
      DeviceAddress::LeAddrToDeviceAddr(params.peer_address_type().Read());
  PW_DCHECK(addr_type && *addr_type != DeviceAddress::Type::kBREDR);

  const DeviceAddress peer_address(*addr_type,
                                   DeviceAddressBytes(params.peer_address()));
  pw::bluetooth::emboss::StatusCode status =
      pw::bluetooth::emboss::StatusCode::SUCCESS;

  // Find the peer that matches the requested address.
  FakePeer* peer = FindPeer(peer_address);
  if (peer) {
    if (peer->connected()) {
      status = pwemb::StatusCode::CONNECTION_ALREADY_EXISTS;
    } else {
      status = peer->connect_status();
    }
  }

  // First send the Command Status response.
  RespondWithCommandStatus(pwemb::OpCode::LE_CREATE_CONNECTION, status);

  // If we just sent back an error status then the operation is complete.
  if (status != pwemb::StatusCode::SUCCESS) {
    return;
  }

  le_connect_pending_ = true;
  CaptureLEConnectParams(params);

  // The procedure was initiated successfully but the peer cannot be connected
  // because it either doesn't exist or isn't connectable.
  if (!peer || !peer->connectable()) {
    bt_log(INFO,
           "fake-hci",
           "requested fake peer cannot be connected; request will time out");
    return;
  }

  // Don't send a connection event if we were asked to force the request to
  // remain pending. This is used by test cases that operate during the pending
  // state.
  if (peer->force_pending_connect()) {
    return;
  }

  if (next_conn_handle_ == 0x0FFF) {
    // Ran out of handles
    status = pwemb::StatusCode::CONNECTION_LIMIT_EXCEEDED;
  } else {
    status = peer->connect_response();
  }

  uint16_t interval_min = params.connection_interval_min().Read();
  uint16_t interval_max = params.connection_interval_max().Read();
  uint16_t interval = interval_min + ((interval_max - interval_min) / 2);

  hci_spec::LEConnectionParameters conn_params(
      interval,
      params.max_latency().Read(),
      params.supervision_timeout().Read());
  peer->set_le_params(conn_params);

  bool use_enhanced_connection_complete = settings_.is_event_unmasked(
      hci_spec::LEEventMask::kLEEnhancedConnectionComplete);
  if (use_enhanced_connection_complete) {
    SendEnhancedConnectionCompleteEvent(status,
                                        params,
                                        interval,
                                        params.max_latency().Read(),
                                        params.supervision_timeout().Read());
  } else {
    SendConnectionCompleteEvent(status, params, interval);
  }
}

void FakeController::OnLEExtendedCreateConnectionCommandReceived(
    const pwemb::LEExtendedCreateConnectionCommandV1View& params) {
  if (!EnableExtendedAdvertising()) {
    bt_log(INFO,
           "fake-hci",
           "extended create connection command rejected, another type already "
           "in use");
    RespondWithCommandStatus(pwemb::OpCode::LE_EXTENDED_CREATE_CONNECTION_V1,
                             pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  if (const auto& phys = params.initiating_phys();
      !phys.le_1m().Read() && !phys.le_2m().Read() && phys.le_coded().Read()) {
    RespondWithCommandStatus(pwemb::OpCode::LE_EXTENDED_CREATE_CONNECTION_V1,
                             pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
  }

  // Cannot issue this command while a request is already pending.
  if (le_connect_pending_) {
    RespondWithCommandStatus(pwemb::OpCode::LE_EXTENDED_CREATE_CONNECTION_V1,
                             pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  // The link is considered lost after connection_interval_max * 2. Connection
  // events (when data pdus are transmitted) must occur at least once within
  // that time frame.
  if (params.data()[0].max_connection_event_length().Read() >
      2 * params.data()[0].connection_interval_max().Read()) {
    RespondWithCommandStatus(pwemb::OpCode::LE_EXTENDED_CREATE_CONNECTION_V1,
                             pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  DeviceAddress::Type addr_type =
      DeviceAddress::LeAddrToDeviceAddr(params.peer_address_type().Read());
  const DeviceAddress peer_address(addr_type,
                                   DeviceAddressBytes(params.peer_address()));

  pwemb::StatusCode status = pwemb::StatusCode::SUCCESS;

  // Find the peer that matches the requested address.
  FakePeer* peer = FindPeer(peer_address);
  if (peer) {
    if (peer->connected()) {
      status = pwemb::StatusCode::CONNECTION_ALREADY_EXISTS;
    } else {
      status = peer->connect_status();
    }
  }

  // First send the Command Status response.
  RespondWithCommandStatus(pwemb::OpCode::LE_EXTENDED_CREATE_CONNECTION_V1,
                           status);

  // If we just sent back an error status then the operation is complete.
  if (status != pwemb::StatusCode::SUCCESS) {
    return;
  }

  le_connect_pending_ = true;
  CaptureLEConnectParams(params);

  // The procedure was initiated successfully but the peer cannot be connected
  // because it either doesn't exist or isn't connectable.
  if (!peer || !peer->connectable()) {
    bt_log(INFO,
           "fake-hci",
           "requested fake peer cannot be connected; request will time out");
    return;
  }

  // Don't send a connection event if we were asked to force the request to
  // remain pending. This is used by test cases that operate during the pending
  // state.
  if (peer->force_pending_connect()) {
    return;
  }

  if (next_conn_handle_ == 0x0FFF) {
    // Ran out of handles
    status = pwemb::StatusCode::CONNECTION_LIMIT_EXCEEDED;
  } else {
    status = peer->connect_response();
  }

  uint16_t interval_min = params.data()[0].connection_interval_min().Read();
  uint16_t interval_max = params.data()[0].connection_interval_max().Read();
  uint16_t interval = interval_min + ((interval_max - interval_min) / 2);

  hci_spec::LEConnectionParameters conn_params(
      interval,
      params.data()[0].max_latency().Read(),
      params.data()[0].supervision_timeout().Read());
  peer->set_le_params(conn_params);

  SendEnhancedConnectionCompleteEvent(
      status,
      params,
      interval,
      params.data()[0].max_latency().Read(),
      params.data()[0].supervision_timeout().Read());
}

template <typename T>
void FakeController::SendEnhancedConnectionCompleteEvent(
    pwemb::StatusCode status,
    const T& params,
    uint16_t interval,
    uint16_t max_latency,
    uint16_t supervision_timeout) {
  DeviceAddress::Type addr_type =
      DeviceAddress::LeAddrToDeviceAddr(params.peer_address_type().Read());
  const DeviceAddress peer_address(addr_type,
                                   DeviceAddressBytes(params.peer_address()));

  auto packet = hci::EventPacket::New<
      pwemb::LEEnhancedConnectionCompleteSubeventV1Writer>(
      hci_spec::kLEMetaEventCode);
  auto view = packet.view_t();
  view.le_meta_event().subevent_code().Write(
      hci_spec::kLEEnhancedConnectionCompleteSubeventCode);
  view.status().Write(status);
  view.peer_address().CopyFrom(params.peer_address());
  view.peer_address_type().Write(DeviceAddress::DeviceAddrToLeAddr(addr_type));
  view.peripheral_latency().Write(max_latency);
  view.connection_interval().Write(interval);
  view.supervision_timeout().Write(supervision_timeout);
  view.role().Write(settings_.le_connection_role);
  view.connection_handle().Write(++next_conn_handle_);

  le_connect_rsp_task_.Cancel();
  le_connect_rsp_task_.set_function(
      [packet, address = peer_address, this](pw::async::Context /*ctx*/,
                                             pw::Status status) {
        auto peer = FindPeer(address);
        if (!peer || !status.ok()) {
          // The peer has been removed or dispatcher shut down; Ignore this
          // response
          return;
        }

        le_connect_pending_ = false;

        auto view =
            packet.view<pwemb::LEEnhancedConnectionCompleteSubeventV1View>();
        if (view.status().Read() == pwemb::StatusCode::SUCCESS) {
          bool not_previously_connected = !peer->connected();
          hci_spec::ConnectionHandle handle = view.connection_handle().Read();
          peer->AddLink(handle);
          if (not_previously_connected && peer->connected()) {
            NotifyConnectionState(peer->address(), handle, /*connected=*/true);
          }
        }

        SendCommandChannelPacket(packet.data());
      });

  le_connect_rsp_task_.PostAfter(settings_.le_connection_delay);
}

void FakeController::SendConnectionCompleteEvent(
    pwemb::StatusCode status,
    const pwemb::LECreateConnectionCommandView& params,
    uint16_t interval) {
  DeviceAddress::Type addr_type =
      DeviceAddress::LeAddrToDeviceAddr(params.peer_address_type().Read());
  const DeviceAddress peer_address(addr_type,
                                   DeviceAddressBytes(params.peer_address()));

  auto packet =
      hci::EventPacket::New<pwemb::LEConnectionCompleteSubeventWriter>(
          hci_spec::kLEMetaEventCode);
  auto view = packet.view_t();
  view.le_meta_event().subevent_code().Write(
      hci_spec::kLEConnectionCompleteSubeventCode);
  view.status().Write(status);
  view.peer_address().CopyFrom(params.peer_address());
  view.peer_address_type().Write(
      DeviceAddress::DeviceAddrToLePeerAddrNoAnon(addr_type));

  view.peripheral_latency().CopyFrom(params.max_latency());
  view.connection_interval().Write(interval);
  view.supervision_timeout().CopyFrom(params.supervision_timeout());
  view.role().Write(settings_.le_connection_role);
  view.connection_handle().Write(++next_conn_handle_);

  le_connect_rsp_task_.Cancel();
  le_connect_rsp_task_.set_function(
      [packet, address = peer_address, this](pw::async::Context /*ctx*/,
                                             pw::Status status) {
        auto peer = FindPeer(address);
        if (!peer || !status.ok()) {
          // The peer has been removed or dispatcher shut down; Ignore this
          // response
          return;
        }

        le_connect_pending_ = false;

        auto view = packet.view<pwemb::LEConnectionCompleteSubeventView>();
        if (view.status().Read() == pwemb::StatusCode::SUCCESS) {
          bool not_previously_connected = !peer->connected();
          hci_spec::ConnectionHandle handle = view.connection_handle().Read();
          peer->AddLink(handle);
          if (not_previously_connected && peer->connected()) {
            NotifyConnectionState(peer->address(), handle, /*connected=*/true);
          }
        }

        SendCommandChannelPacket(packet.data());
      });
  le_connect_rsp_task_.PostAfter(settings_.le_connection_delay);
}

void FakeController::OnLEPeriodicAdvertisingCreateSyncCommandReceived(
    const pw::bluetooth::emboss::LEPeriodicAdvertisingCreateSyncCommandView&
        params) {
  if (pending_periodic_advertising_create_sync_) {
    RespondWithCommandStatus(pwemb::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
                             pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }
  RespondWithCommandStatus(pwemb::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
                           pwemb::StatusCode::SUCCESS);

  PeriodicAdvertisingCreateSync create_sync{
      .duplicate_filtering =
          params.options().enable_duplicate_filtering().Read()};
  pending_periodic_advertising_create_sync_.emplace(create_sync);

  MaybeSendPeriodicAdvertisingSyncEstablishedEvent();
}

void FakeController::OnLEPeriodicAdvertisingTerminateSyncCommandReceived(
    const pw::bluetooth::emboss::LEPeriodicAdvertisingTerminateSyncCommandView&
        params) {
  hci_spec::SyncHandle sync_handle = params.sync_handle().Read();
  size_t count = periodic_advertising_syncs_.erase(sync_handle);
  if (count == 0) {
    RespondWithCommandComplete(
        pwemb::OpCode::LE_PERIODIC_ADVERTISING_TERMINATE_SYNC,
        pwemb::StatusCode::UNKNOWN_ADVERTISING_IDENTIFIER);
    return;
  }
  RespondWithCommandComplete(
      pwemb::OpCode::LE_PERIODIC_ADVERTISING_TERMINATE_SYNC,
      pwemb::StatusCode::SUCCESS);
}

void FakeController::OnLEAddDeviceToPeriodicAdvertiserListCommandReceived(
    const pw::bluetooth::emboss::LEAddDeviceToPeriodicAdvertiserListCommandView&
        params) {
  if (pending_periodic_advertising_create_sync_) {
    RespondWithCommandComplete(
        pwemb::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
        pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  DeviceAddress::Type addr_type = DeviceAddress::LeAddrToDeviceAddr(
      params.advertiser_address_type().Read());
  const DeviceAddress address(addr_type,
                              DeviceAddressBytes(params.advertiser_address()));
  PeriodicAdvertiserListEntry entry;
  entry.address = address;
  entry.advertising_sid = params.advertising_sid().Read();
  if (periodic_advertiser_list_.count(entry)) {
    RespondWithCommandComplete(
        pwemb::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  periodic_advertiser_list_.emplace(entry);
  RespondWithCommandComplete(
      pwemb::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pwemb::StatusCode::SUCCESS);
}

void FakeController::OnLERemoveDeviceFromPeriodicAdvertiserListCommandReceived(
    const pw::bluetooth::emboss::
        LERemoveDeviceFromPeriodicAdvertiserListCommandView& params) {
  if (pending_periodic_advertising_create_sync_) {
    RespondWithCommandComplete(
        pwemb::OpCode::LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST,
        pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  DeviceAddress::Type addr_type = DeviceAddress::LeAddrToDeviceAddr(
      params.advertiser_address_type().Read());
  const DeviceAddress address(addr_type,
                              DeviceAddressBytes(params.advertiser_address()));
  PeriodicAdvertiserListEntry entry;
  entry.address = address;
  entry.advertising_sid = params.advertising_sid().Read();
  auto node = periodic_advertiser_list_.extract(entry);
  if (node.empty()) {
    RespondWithCommandComplete(
        pwemb::OpCode::LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST,
        pwemb::StatusCode::UNKNOWN_ADVERTISING_IDENTIFIER);
    return;
  }

  RespondWithCommandComplete(
      pwemb::OpCode::LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST,
      pwemb::StatusCode::SUCCESS);
}

void FakeController::OnLEConnectionUpdateCommandReceived(
    const pwemb::LEConnectionUpdateCommandView& params) {
  hci_spec::ConnectionHandle handle = params.connection_handle().Read();
  FakePeer* peer = FindByConnHandle(handle);
  if (!peer) {
    RespondWithCommandStatus(pwemb::OpCode::LE_CONNECTION_UPDATE,
                             pwemb::StatusCode::UNKNOWN_CONNECTION_ID);
    return;
  }

  PW_DCHECK(peer->connected());

  uint16_t min_interval = params.connection_interval_min().UncheckedRead();
  uint16_t max_interval = params.connection_interval_max().UncheckedRead();
  uint16_t max_latency = params.max_latency().UncheckedRead();
  uint16_t supv_timeout = params.supervision_timeout().UncheckedRead();

  if (min_interval > max_interval) {
    RespondWithCommandStatus(pwemb::OpCode::LE_CONNECTION_UPDATE,
                             pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  RespondWithCommandStatus(pwemb::OpCode::LE_CONNECTION_UPDATE,
                           pwemb::StatusCode::SUCCESS);

  hci_spec::LEConnectionParameters conn_params(
      min_interval + ((max_interval - min_interval) / 2),
      max_latency,
      supv_timeout);
  peer->set_le_params(conn_params);

  auto packet =
      hci::EventPacket::New<pwemb::LEConnectionUpdateCompleteSubeventWriter>(
          hci_spec::kLEMetaEventCode);
  auto view = packet.view_t();
  view.le_meta_event().subevent_code().Write(
      hci_spec::kLEConnectionUpdateCompleteSubeventCode);
  view.connection_handle().CopyFrom(params.connection_handle());
  if (peer->supports_ll_conn_update_procedure()) {
    view.status().Write(pwemb::StatusCode::SUCCESS);
    view.connection_interval().UncheckedWrite(conn_params.interval());
    view.peripheral_latency().CopyFrom(params.max_latency());
    view.supervision_timeout().UncheckedCopyFrom(params.supervision_timeout());
  } else {
    view.status().Write(pwemb::StatusCode::UNSUPPORTED_REMOTE_FEATURE);
  }
  SendCommandChannelPacket(packet.data());

  NotifyLEConnectionParameters(peer->address(), conn_params);
}

void FakeController::OnDisconnectCommandReceived(
    const pwemb::DisconnectCommandView& params) {
  hci_spec::ConnectionHandle handle = params.connection_handle().Read();

  // Find the peer that matches the disconnected handle.
  FakePeer* peer = FindByConnHandle(handle);
  if (!peer) {
    RespondWithCommandStatus(pwemb::OpCode::DISCONNECT,
                             pwemb::StatusCode::UNKNOWN_CONNECTION_ID);
    return;
  }

  PW_DCHECK(peer->connected());

  RespondWithCommandStatus(pwemb::OpCode::DISCONNECT,
                           pwemb::StatusCode::SUCCESS);

  bool notify = peer->connected();
  peer->RemoveLink(handle);
  if (notify && !peer->connected()) {
    NotifyConnectionState(peer->address(), handle, /*connected=*/false);
  }

  if (auto_disconnection_complete_event_enabled_) {
    SendDisconnectionCompleteEvent(handle);
  }
}

void FakeController::OnWriteLEHostSupportCommandReceived(
    const pwemb::WriteLEHostSupportCommandView& params) {
  if (params.le_supported_host().Read() == pwemb::GenericEnableParam::ENABLE) {
    SetBit(&settings_.lmp_features_page1,
           hci_spec::LMPFeature::kLESupportedHost);
  } else {
    UnsetBit(&settings_.lmp_features_page1,
             hci_spec::LMPFeature::kLESupportedHost);
  }

  RespondWithCommandComplete(pwemb::OpCode::WRITE_LE_HOST_SUPPORT,
                             pwemb::StatusCode::SUCCESS);
}

void FakeController::OnWriteSecureConnectionsHostSupport(
    const pwemb::WriteSecureConnectionsHostSupportCommandView& params) {
  // Core Spec Volume 4, Part E, Section 7.3.92: If the Host issues this command
  // while the Controller is paging, has page scanning enabled, or has an ACL
  // connection, the Controller shall return the error code Command Disallowed
  // (0x0C).
  bool has_acl_connection = false;
  for (auto& [_, peer] : peers_) {
    if (peer->connected()) {
      has_acl_connection = true;
      break;
    }
  }
  if (bredr_connect_pending_ || isBREDRPageScanEnabled() ||
      has_acl_connection) {
    RespondWithCommandComplete(
        pwemb::OpCode::WRITE_SECURE_CONNECTIONS_HOST_SUPPORT,
        pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  if (params.secure_connections_host_support().Read() ==
      pwemb::GenericEnableParam::ENABLE) {
    SetBit(&settings_.lmp_features_page1,
           hci_spec::LMPFeature::kSecureConnectionsHostSupport);
  } else {
    UnsetBit(&settings_.lmp_features_page1,
             hci_spec::LMPFeature::kSecureConnectionsHostSupport);
  }

  RespondWithCommandComplete(
      pwemb::OpCode::WRITE_SECURE_CONNECTIONS_HOST_SUPPORT,
      pwemb::StatusCode::SUCCESS);
}

void FakeController::OnReset() {
  // TODO(fxbug.dev/42159137): actually do some resetting of stuff here
  RespondWithCommandComplete(pwemb::OpCode::RESET, pwemb::StatusCode::SUCCESS);
}

void FakeController::OnInquiry(const pwemb::InquiryCommandView& params) {
  // Confirm that LAP array is equal to either kGIAC or kLIAC.
  if (params.lap().Read() != pwemb::InquiryAccessCode::GIAC &&
      params.lap().Read() != pwemb::InquiryAccessCode::LIAC) {
    RespondWithCommandStatus(pwemb::OpCode::INQUIRY,
                             pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  if (params.inquiry_length().Read() == 0x00 ||
      params.inquiry_length().Read() > hci_spec::kInquiryLengthMax) {
    RespondWithCommandStatus(pwemb::OpCode::INQUIRY,
                             pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  inquiry_num_responses_left_ = params.num_responses().Read();
  if (params.num_responses().Read() == 0) {
    inquiry_num_responses_left_ = -1;
  }

  RespondWithCommandStatus(pwemb::OpCode::INQUIRY, pwemb::StatusCode::SUCCESS);

  bt_log(INFO, "fake-hci", "sending inquiry responses..");
  SendInquiryResponses();

  (void)heap_dispatcher().PostAfter(
      [this](pw::async::Context /*ctx*/, pw::Status status) {
        if (!status.ok()) {
          return;
        }
        auto output = hci::EventPacket::New<pwemb::InquiryCompleteEventWriter>(
            hci_spec::kInquiryCompleteEventCode);
        output.view_t().status().Write(pwemb::StatusCode::SUCCESS);
        SendCommandChannelPacket(output.data());
      },
      std::chrono::milliseconds(params.inquiry_length().Read()) * 1280);
}

void FakeController::OnLESetScanEnable(
    const pwemb::LESetScanEnableCommandView& params) {
  if (!EnableLegacyScanning()) {
    bt_log(INFO,
           "fake-hci",
           "legacy advertising command rejected, another type already in use");
    RespondWithCommandStatus(pwemb::OpCode::LE_SET_SCAN_ENABLE,
                             pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  le_scan_state_.enabled = false;
  if (params.le_scan_enable().Read() == pwemb::GenericEnableParam::ENABLE) {
    le_scan_state_.enabled = true;
  }

  le_scan_state_.filter_duplicates = false;
  if (params.filter_duplicates().Read() == pwemb::GenericEnableParam::ENABLE) {
    le_scan_state_.filter_duplicates = true;
  }

  // Post the scan state update before scheduling the HCI Command Complete
  // event. This guarantees that single-threaded unit tests receive the scan
  // state update BEFORE the HCI command sequence terminates.
  if (scan_state_cb_) {
    scan_state_cb_(le_scan_state_.enabled);
  }

  RespondWithCommandComplete(pwemb::OpCode::LE_SET_SCAN_ENABLE,
                             pwemb::StatusCode::SUCCESS);

  if (le_scan_state_.enabled) {
    SendAdvertisingReports();
  }
}

void FakeController::OnLESetExtendedScanEnable(
    const pwemb::LESetExtendedScanEnableCommandView& params) {
  if (!EnableExtendedScanning()) {
    bt_log(
        INFO,
        "fake-hci",
        "extended advertising command rejected, another type already in use");
    RespondWithCommandStatus(pwemb::OpCode::LE_SET_EXTENDED_SCAN_ENABLE,
                             pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  le_scan_state_.enabled = false;
  if (params.scanning_enabled().Read() == pwemb::GenericEnableParam::ENABLE) {
    le_scan_state_.enabled = true;
  }

  le_scan_state_.filter_duplicates = true;
  if (params.filter_duplicates().Read() ==
      pwemb::LEExtendedDuplicateFilteringOption::DISABLED) {
    le_scan_state_.filter_duplicates = false;
  }

  le_scan_state_.duration = params.duration().Read();
  le_scan_state_.period = params.period().Read();

  // Post the scan state update before scheduling the HCI Command Complete
  // event. This guarantees that single-threaded unit tests receive the scan
  // state update BEFORE the HCI command sequence terminates.
  if (scan_state_cb_) {
    scan_state_cb_(le_scan_state_.enabled);
  }

  RespondWithCommandComplete(pwemb::OpCode::LE_SET_EXTENDED_SCAN_ENABLE,
                             pwemb::StatusCode::SUCCESS);

  if (le_scan_state_.enabled) {
    SendAdvertisingReports();
    MaybeSendPeriodicAdvertisingSyncEstablishedEvent();
  }
}

void FakeController::OnLESetScanParameters(
    const pwemb::LESetScanParametersCommandView& params) {
  if (!EnableLegacyScanning()) {
    bt_log(INFO,
           "fake-hci",
           "legacy advertising command rejected, another type already in use");
    RespondWithCommandStatus(pwemb::OpCode::LE_SET_SCAN_PARAMETERS,
                             pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  if (le_scan_state_.enabled) {
    RespondWithCommandComplete(pwemb::OpCode::LE_SET_SCAN_PARAMETERS,
                               pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  le_scan_state_.own_address_type = params.own_address_type().Read();
  le_scan_state_.filter_policy = params.scanning_filter_policy().Read();
  le_scan_state_.scan_type = params.le_scan_type().Read();
  le_scan_state_.scan_interval = params.le_scan_interval().Read();
  le_scan_state_.scan_window = params.le_scan_window().Read();

  RespondWithCommandComplete(pwemb::OpCode::LE_SET_SCAN_PARAMETERS,
                             pwemb::StatusCode::SUCCESS);
}

void FakeController::OnLESetExtendedScanParameters(
    const pwemb::LESetExtendedScanParametersCommandView& params) {
  if (!EnableExtendedScanning()) {
    bt_log(
        INFO,
        "fake-hci",
        "extended advertising command rejected, another type already in use");
    RespondWithCommandStatus(pwemb::OpCode::LE_SET_SCAN_PARAMETERS,
                             pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  if (le_scan_state_.enabled) {
    RespondWithCommandComplete(pwemb::OpCode::LE_SET_SCAN_PARAMETERS,
                               pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  if (params.num_entries().Read() == 0) {
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_SCAN_PARAMETERS,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  le_scan_state_.own_address_type = params.own_address_type().Read();
  le_scan_state_.filter_policy = params.scanning_filter_policy().Read();

  // ExtendedLowEnergyScanner sets the same parameters for both the LE 1M and LE
  // Coded PHYs. We just take the parameters from the LE 1M PHY for now since we
  // don't support using different parameters for different PHYs.
  if (!params.scanning_phys().le_1m().Read()) {
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_SCAN_PARAMETERS,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  le_scan_state_.scan_type = params.data()[0].scan_type().Read();
  le_scan_state_.scan_interval = params.data()[0].scan_interval().Read();
  le_scan_state_.scan_window = params.data()[0].scan_window().Read();
  RespondWithCommandComplete(pwemb::OpCode::LE_SET_EXTENDED_SCAN_PARAMETERS,
                             pwemb::StatusCode::SUCCESS);
}

void FakeController::OnLESetHostFeature(
    const pwemb::LESetHostFeatureCommandView& params) {
  // We only support setting the CIS Host Support Bit
  if (params.bit_number().Read() !=
      static_cast<uint8_t>(hci_spec::LESupportedFeatureBitPos::
                               kConnectedIsochronousStreamHostSupport)) {
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_HOST_FEATURE,
        pwemb::StatusCode::UNSUPPORTED_FEATURE_OR_PARAMETER);
    return;
  }
  if (params.bit_value().Read() ==
      pw::bluetooth::emboss::GenericEnableParam::ENABLE) {
    SetBit(
        &settings_.le_features,
        hci_spec::LESupportedFeature::kConnectedIsochronousStreamHostSupport);
  } else {
    UnsetBit(
        &settings_.le_features,
        hci_spec::LESupportedFeature::kConnectedIsochronousStreamHostSupport);
  }

  RespondWithCommandComplete(pwemb::OpCode::LE_SET_HOST_FEATURE,
                             pwemb::StatusCode::SUCCESS);
}

void FakeController::OnReadLocalExtendedFeatures(
    const pwemb::ReadLocalExtendedFeaturesCommandView& params) {
  auto packet = hci::EventPacket::New<
      pwemb::ReadLocalExtendedFeaturesCommandCompleteEventWriter>(
      hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.page_number().Write(params.page_number().Read());
  view.max_page_number().Write(2);
  view.extended_lmp_features().Write(0);
  switch (params.page_number().Read()) {
    case 0:
      view.extended_lmp_features().Write(settings_.lmp_features_page0);
      break;
    case 1:
      view.extended_lmp_features().Write(settings_.lmp_features_page1);
      break;
    case 2:
      view.extended_lmp_features().Write(settings_.lmp_features_page2);
      break;
    default:
      view.status().Write(pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
  }

  RespondWithCommandComplete(pwemb::OpCode::READ_LOCAL_EXTENDED_FEATURES,
                             &packet);
}

void FakeController::OnSetEventMask(
    const pwemb::SetEventMaskCommandView& params) {
  settings_.event_mask = params.event_mask().Read();
  RespondWithCommandComplete(pwemb::OpCode::SET_EVENT_MASK,
                             pwemb::StatusCode::SUCCESS);
}

void FakeController::OnLESetEventMask(
    const pwemb::LESetEventMaskCommandView& params) {
  settings_.le_event_mask = params.le_event_mask().BackingStorage().ReadUInt();
  RespondWithCommandComplete(pwemb::OpCode::LE_SET_EVENT_MASK,
                             pwemb::StatusCode::SUCCESS);
}

void FakeController::OnLEReadBufferSizeV1() {
  auto packet = hci::EventPacket::New<
      pwemb::LEReadBufferSizeV1CommandCompleteEventWriter>(
      hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.le_acl_data_packet_length().Write(settings_.le_acl_data_packet_length);
  view.total_num_le_acl_data_packets().Write(
      settings_.le_total_num_acl_data_packets);
  RespondWithCommandComplete(pwemb::OpCode::LE_READ_BUFFER_SIZE_V1, &packet);
}

void FakeController::OnLEReadBufferSizeV2() {
  auto packet = hci::EventPacket::New<
      pwemb::LEReadBufferSizeV2CommandCompleteEventWriter>(
      hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();

  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.le_acl_data_packet_length().Write(settings_.le_acl_data_packet_length);
  view.total_num_le_acl_data_packets().Write(
      settings_.le_total_num_acl_data_packets);
  view.iso_data_packet_length().Write(settings_.iso_data_packet_length);
  view.total_num_iso_data_packets().Write(settings_.total_num_iso_data_packets);

  RespondWithCommandComplete(pwemb::OpCode::LE_READ_BUFFER_SIZE_V2, &packet);
}

void FakeController::OnLEReadSupportedStates() {
  auto packet = hci::EventPacket::New<
      pwemb::LEReadSupportedStatesCommandCompleteEventWriter>(
      hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.le_states().BackingStorage().WriteLittleEndianUInt<64>(
      settings_.le_supported_states);
  RespondWithCommandComplete(pwemb::OpCode::LE_READ_SUPPORTED_STATES, &packet);
}

void FakeController::OnLEReadLocalSupportedFeatures() {
  auto packet = hci::EventPacket::New<
      pwemb::LEReadLocalSupportedFeaturesCommandCompleteEventWriter>(
      hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.le_features().BackingStorage().WriteUInt(settings_.le_features);
  RespondWithCommandComplete(pwemb::OpCode::LE_READ_LOCAL_SUPPORTED_FEATURES,
                             &packet);
}

void FakeController::OnLECreateConnectionCancel() {
  if (!le_connect_pending_) {
    RespondWithCommandComplete(pwemb::OpCode::LE_CREATE_CONNECTION_CANCEL,
                               pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  le_connect_pending_ = false;
  le_connect_rsp_task_.Cancel();
  PW_DCHECK(le_connect_params_);

  NotifyConnectionState(le_connect_params_->peer_address,
                        0,
                        /*connected=*/false,
                        /*canceled=*/true);

  bool use_enhanced_connection_complete = settings_.is_event_unmasked(
      hci_spec::LEEventMask::kLEEnhancedConnectionComplete);
  if (use_enhanced_connection_complete) {
    auto packet = hci::EventPacket::New<
        pwemb::LEEnhancedConnectionCompleteSubeventV1Writer>(
        hci_spec::kLEMetaEventCode);
    auto params = packet.view_t();
    params.le_meta_event().subevent_code().Write(
        hci_spec::kLEEnhancedConnectionCompleteSubeventCode);
    params.status().Write(pwemb::StatusCode::UNKNOWN_CONNECTION_ID);
    params.peer_address().CopyFrom(
        le_connect_params_->peer_address.value().view());
    params.peer_address_type().Write(DeviceAddress::DeviceAddrToLeAddr(
        le_connect_params_->peer_address.type()));

    RespondWithCommandComplete(pwemb::OpCode::LE_CREATE_CONNECTION_CANCEL,
                               pwemb::StatusCode::SUCCESS);
    SendCommandChannelPacket(packet.data());
  } else {
    auto packet =
        hci::EventPacket::New<pwemb::LEConnectionCompleteSubeventWriter>(
            hci_spec::kLEMetaEventCode);
    auto params = packet.view_t();
    params.le_meta_event().subevent_code().Write(
        hci_spec::kLEConnectionCompleteSubeventCode);
    params.status().Write(pwemb::StatusCode::UNKNOWN_CONNECTION_ID);
    params.peer_address().CopyFrom(
        le_connect_params_->peer_address.value().view());
    params.peer_address_type().Write(
        DeviceAddress::DeviceAddrToLePeerAddrNoAnon(
            le_connect_params_->peer_address.type()));

    RespondWithCommandComplete(pwemb::OpCode::LE_CREATE_CONNECTION_CANCEL,
                               pwemb::StatusCode::SUCCESS);
    SendCommandChannelPacket(packet.data());
  }
}

void FakeController::OnWriteExtendedInquiryResponse(
    const pwemb::WriteExtendedInquiryResponseCommandView& params) {
  // As of now, we don't support FEC encoding enabled.
  if (params.fec_required().Read() != 0x00) {
    RespondWithCommandStatus(pwemb::OpCode::WRITE_EXTENDED_INQUIRY_RESPONSE,
                             pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
  }

  RespondWithCommandComplete(pwemb::OpCode::WRITE_EXTENDED_INQUIRY_RESPONSE,
                             pwemb::StatusCode::SUCCESS);
}

void FakeController::OnWriteSimplePairingMode(
    const pwemb::WriteSimplePairingModeCommandView& params) {
  // "A host shall not set the Simple Pairing Mode to 'disabled'"
  // Spec 5.0 Vol 2 Part E Sec 7.3.59
  if (params.simple_pairing_mode().Read() !=
      pwemb::GenericEnableParam::ENABLE) {
    RespondWithCommandComplete(
        pwemb::OpCode::WRITE_SIMPLE_PAIRING_MODE,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  SetBit(&settings_.lmp_features_page1,
         hci_spec::LMPFeature::kSecureSimplePairingHostSupport);
  RespondWithCommandComplete(pwemb::OpCode::WRITE_SIMPLE_PAIRING_MODE,
                             pwemb::StatusCode::SUCCESS);
}

void FakeController::OnReadSimplePairingMode() {
  auto event_packet = hci::EventPacket::New<
      pwemb::ReadSimplePairingModeCommandCompleteEventWriter>(
      hci_spec::kCommandCompleteEventCode);
  auto view = event_packet.view_t();
  view.status().Write(pwemb::StatusCode::SUCCESS);
  if (CheckBit(settings_.lmp_features_page1,
               hci_spec::LMPFeature::kSecureSimplePairingHostSupport)) {
    view.simple_pairing_mode().Write(pwemb::GenericEnableParam::ENABLE);
  } else {
    view.simple_pairing_mode().Write(pwemb::GenericEnableParam::DISABLE);
  }

  RespondWithCommandComplete(pwemb::OpCode::READ_SIMPLE_PAIRING_MODE,
                             &event_packet);
}

void FakeController::OnWritePageScanType(
    const pwemb::WritePageScanTypeCommandView& params) {
  page_scan_type_ = params.page_scan_type().Read();
  RespondWithCommandComplete(pwemb::OpCode::WRITE_PAGE_SCAN_TYPE,
                             pwemb::StatusCode::SUCCESS);
}

void FakeController::OnReadPageScanType() {
  auto event_packet =
      hci::EventPacket::New<pwemb::ReadPageScanTypeCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = event_packet.view_t();
  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.page_scan_type().Write(page_scan_type_);
  RespondWithCommandComplete(pwemb::OpCode::READ_PAGE_SCAN_TYPE, &event_packet);
}

void FakeController::OnWriteInquiryMode(
    const pwemb::WriteInquiryModeCommandView& params) {
  inquiry_mode_ = params.inquiry_mode().Read();
  RespondWithCommandComplete(pwemb::OpCode::WRITE_INQUIRY_MODE,
                             pwemb::StatusCode::SUCCESS);
}

void FakeController::OnReadInquiryMode() {
  auto event_packet =
      hci::EventPacket::New<pwemb::ReadInquiryModeCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = event_packet.view_t();
  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.inquiry_mode().Write(inquiry_mode_);
  RespondWithCommandComplete(pwemb::OpCode::READ_INQUIRY_MODE, &event_packet);
}

void FakeController::OnWriteClassOfDevice(
    const pwemb::WriteClassOfDeviceCommandView& params) {
  device_class_ =
      DeviceClass(params.class_of_device().BackingStorage().ReadUInt());
  NotifyControllerParametersChanged();
  RespondWithCommandComplete(pwemb::OpCode::WRITE_CLASS_OF_DEVICE,
                             pwemb::StatusCode::SUCCESS);
}

void FakeController::OnWritePageScanActivity(
    const pwemb::WritePageScanActivityCommandView& params) {
  page_scan_interval_ = params.page_scan_interval().Read();
  page_scan_window_ = params.page_scan_window().Read();
  RespondWithCommandComplete(pwemb::OpCode::WRITE_PAGE_SCAN_ACTIVITY,
                             pwemb::StatusCode::SUCCESS);
}

void FakeController::OnReadPageScanActivity() {
  auto event_packet = hci::EventPacket::New<
      pwemb::ReadPageScanActivityCommandCompleteEventWriter>(
      hci_spec::kCommandCompleteEventCode);
  auto view = event_packet.view_t();
  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.page_scan_interval().Write(page_scan_interval_);
  view.page_scan_window().Write(page_scan_window_);
  RespondWithCommandComplete(pwemb::OpCode::READ_PAGE_SCAN_ACTIVITY,
                             &event_packet);
}

void FakeController::OnWriteScanEnable(
    const pwemb::WriteScanEnableCommandView& params) {
  bredr_scan_state_ = params.scan_enable().BackingStorage().ReadUInt();
  RespondWithCommandComplete(pwemb::OpCode::WRITE_SCAN_ENABLE,
                             pwemb::StatusCode::SUCCESS);
}

void FakeController::OnReadScanEnable() {
  auto event_packet =
      hci::EventPacket::New<pwemb::ReadScanEnableCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = event_packet.view_t();
  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.scan_enable().BackingStorage().WriteUInt(bredr_scan_state_);
  RespondWithCommandComplete(pwemb::OpCode::READ_SCAN_ENABLE, &event_packet);
}

void FakeController::OnReadLocalName() {
  auto event_packet =
      hci::EventPacket::New<pwemb::ReadLocalNameCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = event_packet.view_t();
  view.status().Write(pwemb::StatusCode::SUCCESS);
  unsigned char* name_from_event = view.local_name().BackingStorage().data();
  char* name_as_cstr = reinterpret_cast<char*>(name_from_event);
  std::strncpy(name_as_cstr, local_name_.c_str(), hci_spec::kMaxNameLength);
  RespondWithCommandComplete(pwemb::OpCode::READ_LOCAL_NAME, &event_packet);
}

void FakeController::OnWriteLocalName(
    const pwemb::WriteLocalNameCommandView& params) {
  std::size_t name_len = 0;

  auto local_name = params.local_name().BackingStorage().data();

  for (; name_len < hci_spec::kMaxNameLength; ++name_len) {
    if (local_name[name_len] == '\0') {
      break;
    }
  }
  local_name_ = std::string(local_name, local_name + name_len);
  NotifyControllerParametersChanged();
  RespondWithCommandComplete(pwemb::OpCode::WRITE_LOCAL_NAME,
                             pwemb::StatusCode::SUCCESS);
}

void FakeController::OnCreateConnectionCancel() {
  auto packet = hci::EventPacket::New<
      pwemb::CreateConnectionCancelCommandCompleteEventWriter>(
      hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.bd_addr().CopyFrom(pending_bredr_connect_addr_.value().view());

  if (!bredr_connect_pending_) {
    // No request is currently pending.
    view.status().Write(pwemb::StatusCode::UNKNOWN_CONNECTION_ID);
    RespondWithCommandComplete(pwemb::OpCode::CREATE_CONNECTION_CANCEL,
                               &packet);
    return;
  }

  bredr_connect_pending_ = false;
  bredr_connect_rsp_task_.Cancel();

  NotifyConnectionState(
      pending_bredr_connect_addr_, 0, /*connected=*/false, /*canceled=*/true);

  RespondWithCommandComplete(pwemb::OpCode::CREATE_CONNECTION_CANCEL, &packet);

  auto response = hci::EventPacket::New<pwemb::ConnectionCompleteEventWriter>(
      hci_spec::kConnectionCompleteEventCode);
  response.view_t().status().Write(pwemb::StatusCode::UNKNOWN_CONNECTION_ID);
  response.view_t().bd_addr().CopyFrom(
      pending_bredr_connect_addr_.value().view());
  SendCommandChannelPacket(response.data());
}

void FakeController::OnReadBufferSize() {
  auto packet =
      hci::EventPacket::New<pwemb::ReadBufferSizeCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.acl_data_packet_length().Write(settings_.acl_data_packet_length);
  view.total_num_acl_data_packets().Write(settings_.total_num_acl_data_packets);
  view.synchronous_data_packet_length().Write(
      settings_.synchronous_data_packet_length);
  view.total_num_synchronous_data_packets().Write(
      settings_.total_num_synchronous_data_packets);
  RespondWithCommandComplete(pwemb::OpCode::READ_BUFFER_SIZE, &packet);
}

void FakeController::OnReadBRADDR() {
  auto packet =
      hci::EventPacket::New<pwemb::ReadBdAddrCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.bd_addr().CopyFrom(settings_.bd_addr.value().view());
  RespondWithCommandComplete(pwemb::OpCode::READ_BD_ADDR, &packet);
}

void FakeController::OnLESetAdvertisingEnable(
    const pwemb::LESetAdvertisingEnableCommandView& params) {
  bool enable =
      params.advertising_enable().Read() == pwemb::GenericEnableParam::ENABLE;
  legacy_advertising_state_.enable_history.push_back(enable);

  if (!EnableLegacyAdvertising()) {
    bt_log(INFO,
           "fake-hci",
           "legacy advertising command rejected, another type already in use");
    RespondWithCommandStatus(pwemb::OpCode::LE_SET_ADVERTISING_ENABLE,
                             pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  if (legacy_advertising_state_.enabled == enable) {
    bt_log(INFO,
           "fake-hci",
           "legacy advertising enable rejected; already in desired state: %d",
           enable);
    RespondWithCommandStatus(pwemb::OpCode::LE_SET_ADVERTISING_ENABLE,
                             pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  if (legacy_advertising_state_.own_address_type ==
          pw::bluetooth::emboss::LEOwnAddressType::RANDOM &&
      !legacy_advertising_state_.random_address.has_value()) {
    bt_log(INFO,
           "fake-hci",
           "cannot enable, random address type requires a random address set");
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_ADVERTISING_ENABLE,
        pw::bluetooth::emboss::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  legacy_advertising_state_.enabled = enable;
  RespondWithCommandComplete(pwemb::OpCode::LE_SET_ADVERTISING_ENABLE,
                             pwemb::StatusCode::SUCCESS);
  NotifyAdvertisingState();
}

void FakeController::OnLESetScanResponseData(
    const pwemb::LESetScanResponseDataCommandView& params) {
  if (!EnableLegacyScanning()) {
    bt_log(INFO,
           "fake-hci",
           "legacy advertising command rejected, another type already in use");
    RespondWithCommandStatus(pwemb::OpCode::LE_SET_SCAN_RESPONSE_DATA,
                             pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  legacy_advertising_state_.scan_rsp_length =
      params.scan_response_data_length().Read();

  if (params.scan_response_data_length().Read() == 0) {
    std::memset(legacy_advertising_state_.scan_rsp_data,
                0,
                sizeof(legacy_advertising_state_.scan_rsp_data));
  } else {
    std::memcpy(legacy_advertising_state_.scan_rsp_data,
                params.scan_response_data().BackingStorage().data(),
                params.scan_response_data_length().Read());
  }

  RespondWithCommandComplete(pwemb::OpCode::LE_SET_SCAN_RESPONSE_DATA,
                             pwemb::StatusCode::SUCCESS);
  NotifyAdvertisingState();
}

void FakeController::OnLESetAdvertisingData(
    const pwemb::LESetAdvertisingDataCommandView& params) {
  if (!EnableLegacyAdvertising()) {
    bt_log(INFO,
           "fake-hci",
           "legacy advertising command rejected, another type already in use");
    RespondWithCommandStatus(pwemb::OpCode::LE_SET_ADVERTISING_DATA,
                             pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  legacy_advertising_state_.data_length =
      params.advertising_data_length().Read();

  if (params.advertising_data_length().Read() == 0) {
    std::memset(legacy_advertising_state_.data,
                0,
                sizeof(legacy_advertising_state_.data));
  } else {
    std::memcpy(legacy_advertising_state_.data,
                params.advertising_data().BackingStorage().data(),
                params.advertising_data_length().Read());
  }

  RespondWithCommandComplete(pwemb::OpCode::LE_SET_ADVERTISING_DATA,
                             pwemb::StatusCode::SUCCESS);
  NotifyAdvertisingState();
}

void FakeController::OnLESetAdvertisingParameters(
    const pwemb::LESetAdvertisingParametersCommandView& params) {
  if (!EnableLegacyAdvertising()) {
    bt_log(INFO,
           "fake-hci",
           "legacy advertising command rejected, another type already in use");
    RespondWithCommandStatus(pwemb::OpCode::LE_SET_ADVERTISING_PARAMETERS,
                             pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  if (legacy_advertising_state_.enabled) {
    bt_log(INFO,
           "fake-hci",
           "cannot set advertising parameters while advertising enabled");
    RespondWithCommandComplete(pwemb::OpCode::LE_SET_ADVERTISING_PARAMETERS,
                               pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  uint16_t interval_min = params.advertising_interval_min().UncheckedRead();
  uint16_t interval_max = params.advertising_interval_max().UncheckedRead();

  // Core Spec Volume 4, Part E, Section 7.8.5: For high duty cycle directed
  // advertising, the Advertising_Interval_Min and Advertising_Interval_Max
  // parameters are not used and shall be ignored.
  if (params.adv_type().Read() !=
      pwemb::LEAdvertisingType::CONNECTABLE_HIGH_DUTY_CYCLE_DIRECTED) {
    if (interval_min >= interval_max) {
      bt_log(INFO,
             "fake-hci",
             "advertising interval min (%d) not strictly less than max (%d)",
             interval_min,
             interval_max);
      RespondWithCommandComplete(
          pwemb::OpCode::LE_SET_ADVERTISING_PARAMETERS,
          pwemb::StatusCode::UNSUPPORTED_FEATURE_OR_PARAMETER);
      return;
    }

    if (interval_min < hci_spec::kLEAdvertisingIntervalMin) {
      bt_log(INFO,
             "fake-hci",
             "advertising interval min (%d) less than spec min (%d)",
             interval_min,
             hci_spec::kLEAdvertisingIntervalMin);
      RespondWithCommandComplete(
          pwemb::OpCode::LE_SET_ADVERTISING_PARAMETERS,
          pwemb::StatusCode::UNSUPPORTED_FEATURE_OR_PARAMETER);
      return;
    }

    if (interval_max > hci_spec::kLEAdvertisingIntervalMax) {
      bt_log(INFO,
             "fake-hci",
             "advertising interval max (%d) greater than spec max (%d)",
             interval_max,
             hci_spec::kLEAdvertisingIntervalMax);
      RespondWithCommandComplete(
          pwemb::OpCode::LE_SET_ADVERTISING_PARAMETERS,
          pwemb::StatusCode::UNSUPPORTED_FEATURE_OR_PARAMETER);
      return;
    }
  }

  legacy_advertising_state_.own_address_type = params.own_address_type().Read();
  legacy_advertising_state_.interval_min = interval_min;
  legacy_advertising_state_.interval_max = interval_max;

  pwemb::LEAdvertisingType adv_type = params.adv_type().Read();
  switch (adv_type) {
    case pwemb::LEAdvertisingType::CONNECTABLE_AND_SCANNABLE_UNDIRECTED:
      legacy_advertising_state_.properties.scannable = true;
      legacy_advertising_state_.properties.connectable = true;
      break;
    case pwemb::LEAdvertisingType::CONNECTABLE_LOW_DUTY_CYCLE_DIRECTED:
      legacy_advertising_state_.properties.directed = true;
      legacy_advertising_state_.properties.connectable = true;
      break;
    case pwemb::LEAdvertisingType::CONNECTABLE_HIGH_DUTY_CYCLE_DIRECTED:
      legacy_advertising_state_.properties
          .high_duty_cycle_directed_connectable = true;
      legacy_advertising_state_.properties.directed = true;
      legacy_advertising_state_.properties.connectable = true;
      break;
    case pwemb::LEAdvertisingType::SCANNABLE_UNDIRECTED:
      legacy_advertising_state_.properties.scannable = true;
      break;
    case pwemb::LEAdvertisingType::NOT_CONNECTABLE_UNDIRECTED:
      break;
  }

  bt_log(INFO,
         "fake-hci",
         "start advertising using address type: %hhd",
         static_cast<char>(legacy_advertising_state_.own_address_type));

  RespondWithCommandComplete(pwemb::OpCode::LE_SET_ADVERTISING_PARAMETERS,
                             pwemb::StatusCode::SUCCESS);
  NotifyAdvertisingState();
}

void FakeController::OnLESetRandomAddress(
    const pwemb::LESetRandomAddressCommandView& params) {
  if (!EnableLegacyAdvertising()) {
    bt_log(INFO,
           "fake-hci",
           "legacy advertising command rejected, another type already in use");
    RespondWithCommandStatus(pwemb::OpCode::LE_SET_RANDOM_ADDRESS,
                             pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  if (legacy_advertising_state().enabled || le_scan_state().enabled) {
    bt_log(INFO,
           "fake-hci",
           "cannot set LE random address while scanning or advertising");
    RespondWithCommandComplete(pwemb::OpCode::LE_SET_RANDOM_ADDRESS,
                               pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  legacy_advertising_state_.random_address =
      DeviceAddress(DeviceAddress::Type::kLERandom,
                    DeviceAddressBytes(params.random_address()));
  RespondWithCommandComplete(pwemb::OpCode::LE_SET_RANDOM_ADDRESS,
                             pwemb::StatusCode::SUCCESS);
}

void FakeController::OnReadLocalSupportedFeatures() {
  auto packet = hci::EventPacket::New<
      pwemb::ReadLocalSupportedFeaturesCommandCompleteEventWriter>(
      hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.lmp_features().Write(settings_.lmp_features_page0);
  RespondWithCommandComplete(pwemb::OpCode::READ_LOCAL_SUPPORTED_FEATURES,
                             &packet);
}

void FakeController::OnReadLocalSupportedCommands() {
  auto packet = hci::EventPacket::New<
      pwemb::ReadLocalSupportedCommandsCommandCompleteEventWriter>(
      hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.status().Write(pwemb::StatusCode::SUCCESS);
  std::memcpy(view.supported_commands().BackingStorage().begin(),
              settings_.supported_commands,
              sizeof(settings_.supported_commands));
  RespondWithCommandComplete(pwemb::OpCode::READ_LOCAL_SUPPORTED_COMMANDS,
                             &packet);
}

void FakeController::OnReadLocalVersionInfo() {
  auto packet = hci::EventPacket::New<
      pwemb::ReadLocalVersionInfoCommandCompleteEventWriter>(
      hci_spec::kCommandCompleteEventCode);
  packet.view_t().hci_version().Write(settings_.hci_version);
  RespondWithCommandComplete(pwemb::OpCode::READ_LOCAL_VERSION_INFO, &packet);
}

void FakeController::OnReadRemoteNameRequestCommandReceived(
    const pwemb::RemoteNameRequestCommandView& params) {
  const DeviceAddress peer_address(DeviceAddress::Type::kBREDR,
                                   DeviceAddressBytes(params.bd_addr()));

  // Find the peer that matches the requested address.
  FakePeer* peer = FindPeer(peer_address);
  if (!peer) {
    RespondWithCommandStatus(pwemb::OpCode::REMOTE_NAME_REQUEST,
                             pwemb::StatusCode::UNKNOWN_CONNECTION_ID);
    return;
  }

  RespondWithCommandStatus(pwemb::OpCode::REMOTE_NAME_REQUEST,
                           pwemb::StatusCode::SUCCESS);

  auto response =
      hci::EventPacket::New<pwemb::RemoteNameRequestCompleteEventWriter>(
          hci_spec::kRemoteNameRequestCompleteEventCode);
  auto view = response.view_t();
  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.bd_addr().CopyFrom(params.bd_addr());
  std::strncpy(
      reinterpret_cast<char*>(view.remote_name().BackingStorage().data()),
      peer->name().c_str(),
      view.remote_name().SizeInBytes());
  SendEvent(hci_spec::kRemoteNameRequestCompleteEventCode, &response);
}

void FakeController::OnReadRemoteSupportedFeaturesCommandReceived(
    const pwemb::ReadRemoteSupportedFeaturesCommandView& params) {
  RespondWithCommandStatus(pwemb::OpCode::READ_REMOTE_SUPPORTED_FEATURES,
                           pwemb::StatusCode::SUCCESS);

  auto response = hci::EventPacket::New<
      pwemb::ReadRemoteSupportedFeaturesCompleteEventWriter>(
      hci_spec::kReadRemoteSupportedFeaturesCompleteEventCode);
  auto view = response.view_t();
  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.connection_handle().Write(params.connection_handle().Read());
  view.lmp_features().BackingStorage().WriteUInt(settings_.lmp_features_page0);
  SendCommandChannelPacket(response.data());
}

void FakeController::OnReadRemoteVersionInfoCommandReceived(
    const pwemb::ReadRemoteVersionInfoCommandView& params) {
  RespondWithCommandStatus(pwemb::OpCode::READ_REMOTE_VERSION_INFO,
                           pwemb::StatusCode::SUCCESS);
  auto response =
      hci::EventPacket::New<pwemb::ReadRemoteVersionInfoCompleteEventWriter>(
          hci_spec::kReadRemoteVersionInfoCompleteEventCode);
  auto view = response.view_t();
  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.connection_handle().CopyFrom(params.connection_handle());
  view.version().Write(pwemb::CoreSpecificationVersion::V4_2);
  view.company_identifier().Write(0xFFFF);  // anything
  view.subversion().Write(0xADDE);          // anything
  SendCommandChannelPacket(response.data());
}

void FakeController::OnReadRemoteExtendedFeaturesCommandReceived(
    const pwemb::ReadRemoteExtendedFeaturesCommandView& params) {
  auto response = hci::EventPacket::New<
      pwemb::ReadRemoteExtendedFeaturesCompleteEventWriter>(
      hci_spec::kReadRemoteExtendedFeaturesCompleteEventCode);
  auto view = response.view_t();

  switch (params.page_number().Read()) {
    case 1: {
      view.lmp_features().BackingStorage().WriteUInt(
          settings_.lmp_features_page1);
      break;
    }
    case 2: {
      view.lmp_features().BackingStorage().WriteUInt(
          settings_.lmp_features_page2);
      break;
    }
    default: {
      RespondWithCommandStatus(
          pwemb::OpCode::READ_REMOTE_EXTENDED_FEATURES,
          pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
      return;
    }
  }

  RespondWithCommandStatus(pwemb::OpCode::READ_REMOTE_EXTENDED_FEATURES,
                           pwemb::StatusCode::SUCCESS);
  view.page_number().CopyFrom(params.page_number());
  view.max_page_number().Write(3);
  view.connection_handle().CopyFrom(params.connection_handle());
  view.status().Write(pwemb::StatusCode::SUCCESS);
  SendCommandChannelPacket(response.data());
}

void FakeController::OnAuthenticationRequestedCommandReceived(
    const pwemb::AuthenticationRequestedCommandView& params) {
  hci_spec::ConnectionHandle handle = params.connection_handle().Read();
  FakePeer* peer = FindByConnHandle(handle);
  if (!peer) {
    RespondWithCommandStatus(pwemb::OpCode::AUTHENTICATION_REQUESTED,
                             pwemb::StatusCode::UNKNOWN_CONNECTION_ID);
    return;
  }

  RespondWithCommandStatus(pwemb::OpCode::AUTHENTICATION_REQUESTED,
                           pwemb::StatusCode::SUCCESS);

  auto event = hci::EventPacket::New<pwemb::LinkKeyRequestEventWriter>(
      hci_spec::kLinkKeyRequestEventCode);
  event.view_t().bd_addr().CopyFrom(peer->address_.value().view());
  SendCommandChannelPacket(event.data());
}

void FakeController::OnLinkKeyRequestReplyCommandReceived(
    const pwemb::LinkKeyRequestReplyCommandView& params) {
  DeviceAddress peer_address(DeviceAddress::Type::kBREDR,
                             DeviceAddressBytes(params.bd_addr()));
  FakePeer* peer = FindPeer(peer_address);
  if (!peer) {
    RespondWithCommandStatus(pwemb::OpCode::LINK_KEY_REQUEST_REPLY,
                             pwemb::StatusCode::UNKNOWN_CONNECTION_ID);
    return;
  }

  RespondWithCommandStatus(pwemb::OpCode::LINK_KEY_REQUEST_REPLY,
                           pwemb::StatusCode::SUCCESS);
  RespondWithCommandComplete(pwemb::OpCode::LINK_KEY_REQUEST_REPLY,
                             pwemb::StatusCode::SUCCESS);

  PW_CHECK(!peer->logical_links().empty());
  for (auto& conn_handle : peer->logical_links()) {
    auto event =
        hci::EventPacket::New<pwemb::AuthenticationCompleteEventWriter>(
            hci_spec::kAuthenticationCompleteEventCode);
    event.view_t().status().Write(pwemb::StatusCode::SUCCESS);
    event.view_t().connection_handle().Write(conn_handle);
    SendCommandChannelPacket(event.data());
  }
}

void FakeController::OnLinkKeyRequestNegativeReplyCommandReceived(
    const pwemb::LinkKeyRequestNegativeReplyCommandView& params) {
  FakePeer* peer = FindPeer(DeviceAddress(
      DeviceAddress::Type::kBREDR, DeviceAddressBytes(params.bd_addr())));
  if (!peer) {
    RespondWithCommandStatus(pwemb::OpCode::LINK_KEY_REQUEST_NEGATIVE_REPLY,
                             pwemb::StatusCode::UNKNOWN_CONNECTION_ID);
    return;
  }
  RespondWithCommandStatus(pwemb::OpCode::LINK_KEY_REQUEST_NEGATIVE_REPLY,
                           pwemb::StatusCode::SUCCESS);

  auto event = hci::EventPacket::New<pwemb::IoCapabilityRequestEventWriter>(
      hci_spec::kIOCapabilityRequestEventCode);
  event.view_t().bd_addr().CopyFrom(params.bd_addr());
  SendCommandChannelPacket(event.data());
}

void FakeController::OnIOCapabilityRequestReplyCommand(
    const pwemb::IoCapabilityRequestReplyCommandView& params) {
  RespondWithCommandStatus(pwemb::OpCode::IO_CAPABILITY_REQUEST_REPLY,
                           pwemb::StatusCode::SUCCESS);

  auto io_response =
      hci::EventPacket::New<pwemb::IoCapabilityResponseEventWriter>(
          hci_spec::kIOCapabilityResponseEventCode);
  io_response.view_t().bd_addr().CopyFrom(params.bd_addr());
  io_response.view_t().io_capability().Write(
      pwemb::IoCapability::NO_INPUT_NO_OUTPUT);
  io_response.view_t().oob_data_present().Write(
      pwemb::GenericPresenceParam::NOT_PRESENT);
  io_response.view_t().authentication_requirements().Write(
      pwemb::AuthenticationRequirements::GENERAL_BONDING);
  SendCommandChannelPacket(io_response.data());

  // Event type based on |params.io_capability| and |io_response.io_capability|.
  auto event = hci::EventPacket::New<pwemb::UserConfirmationRequestEventWriter>(
      hci_spec::kUserConfirmationRequestEventCode);
  event.view_t().bd_addr().CopyFrom(params.bd_addr());
  event.view_t().numeric_value().Write(0);
  SendCommandChannelPacket(event.data());
}

void FakeController::OnUserConfirmationRequestReplyCommand(
    const pwemb::UserConfirmationRequestReplyCommandView& params) {
  FakePeer* peer = FindPeer(DeviceAddress(
      DeviceAddress::Type::kBREDR, DeviceAddressBytes(params.bd_addr())));
  if (!peer) {
    RespondWithCommandStatus(pwemb::OpCode::USER_CONFIRMATION_REQUEST_REPLY,
                             pwemb::StatusCode::UNKNOWN_CONNECTION_ID);
    return;
  }

  RespondWithCommandStatus(pwemb::OpCode::USER_CONFIRMATION_REQUEST_REPLY,
                           pwemb::StatusCode::SUCCESS);

  auto pairing_event =
      hci::EventPacket::New<pwemb::SimplePairingCompleteEventWriter>(
          hci_spec::kSimplePairingCompleteEventCode);
  pairing_event.view_t().bd_addr().CopyFrom(params.bd_addr());
  pairing_event.view_t().status().Write(pwemb::StatusCode::SUCCESS);
  SendCommandChannelPacket(pairing_event.data());

  auto link_key_event =
      hci::EventPacket::New<pwemb::LinkKeyNotificationEventWriter>(
          hci_spec::kLinkKeyNotificationEventCode);
  auto link_key_view = link_key_event.view_t();
  link_key_view.bd_addr().CopyFrom(params.bd_addr());
  uint8_t key[] = {0xc0,
                   0xde,
                   0xfa,
                   0x57,
                   0x4b,
                   0xad,
                   0xf0,
                   0x0d,
                   0xa7,
                   0x60,
                   0x06,
                   0x1e,
                   0xca,
                   0x1e,
                   0xca,
                   0xfe};
  link_key_view.link_key().value().BackingStorage().CopyFrom(
      ::emboss::support::ReadOnlyContiguousBuffer(key, sizeof(key)),
      sizeof(key));
  link_key_view.key_type().Write(
      pwemb::KeyType::UNAUTHENTICATED_COMBINATION_FROM_P192);
  SendCommandChannelPacket(link_key_event.data());

  PW_CHECK(!peer->logical_links().empty());
  for (auto& conn_handle : peer->logical_links()) {
    auto event =
        hci::EventPacket::New<pwemb::AuthenticationCompleteEventWriter>(
            hci_spec::kAuthenticationCompleteEventCode);
    event.view_t().status().Write(pwemb::StatusCode::SUCCESS);
    event.view_t().connection_handle().Write(conn_handle);
    SendCommandChannelPacket(event.data());
  }
}

void FakeController::OnUserConfirmationRequestNegativeReplyCommand(
    const pwemb::UserConfirmationRequestNegativeReplyCommandView& params) {
  FakePeer* peer = FindPeer(DeviceAddress(
      DeviceAddress::Type::kBREDR, DeviceAddressBytes(params.bd_addr())));
  if (!peer) {
    RespondWithCommandStatus(
        pwemb::OpCode::USER_CONFIRMATION_REQUEST_NEGATIVE_REPLY,
        pwemb::StatusCode::UNKNOWN_CONNECTION_ID);
    return;
  }

  RespondWithCommandStatus(
      pwemb::OpCode::USER_CONFIRMATION_REQUEST_NEGATIVE_REPLY,
      pwemb::StatusCode::SUCCESS);
  RespondWithCommandComplete(
      pwemb::OpCode::USER_CONFIRMATION_REQUEST_NEGATIVE_REPLY,
      pwemb::StatusCode::SUCCESS);

  auto pairing_event =
      hci::EventPacket::New<pwemb::SimplePairingCompleteEventWriter>(
          hci_spec::kSimplePairingCompleteEventCode);
  pairing_event.view_t().bd_addr().CopyFrom(params.bd_addr());
  pairing_event.view_t().status().Write(
      pwemb::StatusCode::AUTHENTICATION_FAILURE);
  SendCommandChannelPacket(pairing_event.data());
}

void FakeController::OnSetConnectionEncryptionCommand(
    const pwemb::SetConnectionEncryptionCommandView& params) {
  RespondWithCommandStatus(pwemb::OpCode::SET_CONNECTION_ENCRYPTION,
                           pwemb::StatusCode::SUCCESS);
  SendEncryptionChangeEvent(
      params.connection_handle().Read(),
      pwemb::StatusCode::SUCCESS,
      pwemb::EncryptionStatus::ON_WITH_E0_FOR_BREDR_OR_AES_FOR_LE);
}

void FakeController::OnReadEncryptionKeySizeCommand(
    const pwemb::ReadEncryptionKeySizeCommandView& params) {
  auto response = hci::EventPacket::New<
      pwemb::ReadEncryptionKeySizeCommandCompleteEventWriter>(
      hci_spec::kCommandCompleteEventCode);
  auto view = response.view_t();
  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.connection_handle().Write(params.connection_handle().Read());
  view.key_size().Write(16);
  RespondWithCommandComplete(pwemb::OpCode::READ_ENCRYPTION_KEY_SIZE,
                             &response);
}

void FakeController::OnEnhancedAcceptSynchronousConnectionRequestCommand(
    const pwemb::EnhancedAcceptSynchronousConnectionRequestCommandView&
        params) {
  DeviceAddress peer_address(DeviceAddress::Type::kBREDR,
                             DeviceAddressBytes(params.bd_addr()));
  FakePeer* peer = FindPeer(peer_address);
  if (!peer || !peer->last_connection_request_link_type().has_value()) {
    RespondWithCommandStatus(
        pwemb::OpCode::ENHANCED_ACCEPT_SYNCHRONOUS_CONNECTION_REQUEST,
        pwemb::StatusCode::UNKNOWN_CONNECTION_ID);
    return;
  }

  RespondWithCommandStatus(
      pwemb::OpCode::ENHANCED_ACCEPT_SYNCHRONOUS_CONNECTION_REQUEST,
      pwemb::StatusCode::SUCCESS);

  hci_spec::ConnectionHandle sco_handle = ++next_conn_handle_;
  peer->AddLink(sco_handle);

  auto packet =
      hci::EventPacket::New<pwemb::SynchronousConnectionCompleteEventWriter>(
          hci_spec::kSynchronousConnectionCompleteEventCode);
  auto view = packet.view_t();
  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.connection_handle().Write(sco_handle);
  view.bd_addr().CopyFrom(peer->address().value().view());
  view.link_type().Write(peer->last_connection_request_link_type().value());
  view.transmission_interval().Write(1);
  view.retransmission_window().Write(2);
  view.rx_packet_length().Write(3);
  view.tx_packet_length().Write(4);
  view.air_mode().Write(params.connection_parameters()
                            .transmit_coding_format()
                            .coding_format()
                            .Read());
  SendCommandChannelPacket(packet.data());
}

void FakeController::OnEnhancedSetupSynchronousConnectionCommand(
    const pwemb::EnhancedSetupSynchronousConnectionCommandView& params) {
  const hci_spec::ConnectionHandle acl_handle =
      params.connection_handle().Read();
  FakePeer* peer = FindByConnHandle(acl_handle);
  if (!peer) {
    RespondWithCommandStatus(
        pwemb::OpCode::ENHANCED_SETUP_SYNCHRONOUS_CONNECTION,
        pwemb::StatusCode::UNKNOWN_CONNECTION_ID);
    return;
  }

  RespondWithCommandStatus(pwemb::OpCode::ENHANCED_SETUP_SYNCHRONOUS_CONNECTION,
                           pwemb::StatusCode::SUCCESS);

  hci_spec::ConnectionHandle sco_handle = ++next_conn_handle_;
  peer->AddLink(sco_handle);

  auto packet =
      hci::EventPacket::New<pwemb::SynchronousConnectionCompleteEventWriter>(
          hci_spec::kSynchronousConnectionCompleteEventCode);
  auto view = packet.view_t();
  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.connection_handle().Write(sco_handle);
  view.bd_addr().CopyFrom(peer->address().value().view());
  view.link_type().Write(pwemb::LinkType::ESCO);
  view.transmission_interval().Write(1);
  view.retransmission_window().Write(2);
  view.rx_packet_length().Write(3);
  view.tx_packet_length().Write(4);
  view.air_mode().Write(params.connection_parameters()
                            .transmit_coding_format()
                            .coding_format()
                            .Read());
  SendCommandChannelPacket(packet.data());
}

void FakeController::OnLEReadRemoteFeaturesCommand(
    const pwemb::LEReadRemoteFeaturesCommandView& params) {
  if (le_read_remote_features_cb_) {
    le_read_remote_features_cb_();
  }

  const hci_spec::ConnectionHandle handle = params.connection_handle().Read();
  FakePeer* peer = FindByConnHandle(handle);
  if (!peer) {
    RespondWithCommandStatus(pwemb::OpCode::LE_READ_REMOTE_FEATURES,
                             pwemb::StatusCode::UNKNOWN_CONNECTION_ID);
    return;
  }

  RespondWithCommandStatus(pwemb::OpCode::LE_READ_REMOTE_FEATURES,
                           pwemb::StatusCode::SUCCESS);

  auto response =
      hci::EventPacket::New<pwemb::LEReadRemoteFeaturesCompleteSubeventWriter>(
          hci_spec::kLEMetaEventCode);
  auto view = response.view_t();
  view.le_meta_event().subevent_code().Write(
      hci_spec::kLEReadRemoteFeaturesCompleteSubeventCode);
  view.connection_handle().Write(handle);
  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.le_features().BackingStorage().WriteUInt(peer->le_features());
  SendCommandChannelPacket(response.data());
}

void FakeController::OnLEStartEncryptionCommand(
    const pwemb::LEEnableEncryptionCommandView& params) {
  RespondWithCommandStatus(pwemb::OpCode::LE_START_ENCRYPTION,
                           pwemb::StatusCode::SUCCESS);
  SendEncryptionChangeEvent(
      params.connection_handle().Read(),
      pwemb::StatusCode::SUCCESS,
      pwemb::EncryptionStatus::ON_WITH_E0_FOR_BREDR_OR_AES_FOR_LE);
}

void FakeController::OnWriteSynchronousFlowControlEnableCommand(
    [[maybe_unused]] const pwemb::WriteSynchronousFlowControlEnableCommandView&
        params) {
  if (!settings_.SupportedCommandsView()
           .write_synchronous_flow_control_enable()
           .Read()) {
    RespondWithCommandComplete(
        pwemb::OpCode::WRITE_SYNCHRONOUS_FLOW_CONTROL_ENABLE,
        pwemb::StatusCode::UNKNOWN_COMMAND);
    return;
  }
  RespondWithCommandComplete(
      pwemb::OpCode::WRITE_SYNCHRONOUS_FLOW_CONTROL_ENABLE,
      pwemb::StatusCode::SUCCESS);
}

void FakeController::OnLESetAdvertisingSetRandomAddress(
    const pwemb::LESetAdvertisingSetRandomAddressCommandView& params) {
  hci_spec::AdvertisingHandle handle = params.advertising_handle().Read();

  if (!IsValidAdvertisingHandle(handle)) {
    bt_log(ERROR, "fake-hci", "advertising handle outside range: %d", handle);
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_ADVERTISING_SET_RANDOM_ADDRESS,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  if (extended_advertising_states_.count(handle) == 0) {
    bt_log(INFO,
           "fake-hci",
           "unknown advertising handle (%d), "
           "use HCI_LE_Set_Extended_Advertising_Parameters to create one first",
           handle);
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_ADVERTISING_SET_RANDOM_ADDRESS,
        pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  LEAdvertisingState& state = extended_advertising_states_[handle];
  if (state.properties.connectable && state.enabled) {
    bt_log(
        INFO,
        "fake-hci",
        "cannot set LE random address while connectable advertising enabled");
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_ADVERTISING_SET_RANDOM_ADDRESS,
        pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  state.random_address =
      DeviceAddress(DeviceAddress::Type::kLERandom,
                    DeviceAddressBytes(params.random_address()));
  RespondWithCommandComplete(
      pwemb::OpCode::LE_SET_ADVERTISING_SET_RANDOM_ADDRESS,
      pwemb::StatusCode::SUCCESS);
}

void FakeController::OnLESetExtendedAdvertisingParameters(
    const pwemb::LESetExtendedAdvertisingParametersV1CommandView& params) {
  if (!EnableExtendedAdvertising()) {
    bt_log(
        INFO,
        "fake-hci",
        "extended advertising command rejected, another type already in use");
    RespondWithCommandStatus(
        pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_PARAMETERS_V1,
        pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  hci_spec::AdvertisingHandle handle = params.advertising_handle().Read();

  if (!IsValidAdvertisingHandle(handle)) {
    bt_log(ERROR, "fake-hci", "advertising handle outside range: %d", handle);
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_PARAMETERS_V1,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  // we cannot set parameters while an advertising set is currently enabled
  if (extended_advertising_states_.count(handle) != 0) {
    if (extended_advertising_states_[handle].enabled) {
      bt_log(INFO,
             "fake-hci",
             "cannot set parameters while advertising set is enabled");
      RespondWithCommandComplete(
          pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_PARAMETERS_V1,
          pwemb::StatusCode::COMMAND_DISALLOWED);
      return;
    }
  }

  // ensure we can allocate memory for this advertising set if not already
  // present
  if (extended_advertising_states_.count(handle) == 0 &&
      extended_advertising_states_.size() >= num_supported_advertising_sets()) {
    bt_log(INFO,
           "fake-hci",
           "no available memory for new advertising set, handle: %d",
           handle);
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_PARAMETERS_V1,
        pwemb::StatusCode::MEMORY_CAPACITY_EXCEEDED);
    return;
  }

  // ensure we have a valid bit combination in the advertising event properties
  bool connectable = params.advertising_event_properties().connectable().Read();
  bool scannable = params.advertising_event_properties().scannable().Read();
  bool directed = params.advertising_event_properties().directed().Read();
  bool high_duty_cycle_directed_connectable =
      params.advertising_event_properties()
          .high_duty_cycle_directed_connectable()
          .Read();
  bool use_legacy_pdus =
      params.advertising_event_properties().use_legacy_pdus().Read();
  bool anonymous_advertising =
      params.advertising_event_properties().anonymous_advertising().Read();
  bool include_tx_power =
      params.advertising_event_properties().include_tx_power().Read();

  std::optional<pwemb::LEAdvertisingType> adv_type;
  if (use_legacy_pdus) {
    // ADV_IND
    if (!high_duty_cycle_directed_connectable && !directed && scannable &&
        connectable) {
      adv_type = pwemb::LEAdvertisingType::CONNECTABLE_AND_SCANNABLE_UNDIRECTED;
    }

    // ADV_DIRECT_IND
    if (!high_duty_cycle_directed_connectable && directed && !scannable &&
        connectable) {
      adv_type = pwemb::LEAdvertisingType::CONNECTABLE_LOW_DUTY_CYCLE_DIRECTED;
    }

    // ADV_DIRECT_IND
    if (high_duty_cycle_directed_connectable && directed && !scannable &&
        connectable) {
      adv_type = pwemb::LEAdvertisingType::CONNECTABLE_HIGH_DUTY_CYCLE_DIRECTED;
    }

    // ADV_SCAN_IND
    if (!high_duty_cycle_directed_connectable && !directed && scannable &&
        !connectable) {
      adv_type = pwemb::LEAdvertisingType::SCANNABLE_UNDIRECTED;
    }

    // ADV_NONCONN_IND
    if (!high_duty_cycle_directed_connectable && !directed && !scannable &&
        !connectable) {
      adv_type = pwemb::LEAdvertisingType::NOT_CONNECTABLE_UNDIRECTED;
    }

    if (!adv_type) {
      bt_log(INFO,
             "fake-hci",
             "invalid bit combination: %s",
             params.advertising_event_properties()
                 .BackingStorage()
                 .ToString<std::string>()
                 .c_str());
      RespondWithCommandComplete(
          pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_PARAMETERS_V1,
          pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
      return;
    }

    // Core spec Volume 4, Part E, Section 7.8.53: if legacy advertising PDUs
    // are being used, the Primary_Advertising_PHY shall indicate the LE 1M PHY.
    if (params.primary_advertising_phy().Read() !=
        pwemb::LEPrimaryAdvertisingPHY::LE_1M) {
      bt_log(INFO,
             "fake-hci",
             "only legacy pdus are supported, requires advertising on 1M PHY");
      RespondWithCommandComplete(
          pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_PARAMETERS_V1,
          pwemb::StatusCode::UNSUPPORTED_FEATURE_OR_PARAMETER);
      return;
    }
  } else {
    // Core spec Volume 4, Part E, Section 7.8.53: If extended advertising PDU
    // types are being used (bit 4 = 0) then: The advertisement shall not be
    // both connectable and scannable.
    if (connectable && scannable) {
      bt_log(
          INFO,
          "fake-hci",
          "extended advertising pdus can't be both connectable and scannable");
      RespondWithCommandComplete(
          pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_PARAMETERS_V1,
          pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
      return;
    }

    // Core spec Volume 4, Part E, Section 7.8.53: If extended advertising PDU
    // types are being used (bit 4 = 0) then: High duty cycle directed
    // connectable advertising (≤ 3.75 ms advertising interval) shall not be
    // used (bit 3 = 0).
    if (high_duty_cycle_directed_connectable) {
      bt_log(INFO,
             "fake-hci",
             "extended advertising pdus can't use the high duty cycle directed "
             "connectable type");
      RespondWithCommandComplete(
          pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_PARAMETERS_V1,
          pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
      return;
    }
  }

  // In case there is an error below, we want to reject all parameters instead
  // of storing a dead state and taking up an advertising handle. Avoid creating
  // the LEAdvertisingState directly in the map and add it in only once we have
  // made sure all is good.
  LEAdvertisingState state;
  state.properties.connectable = connectable;
  state.properties.scannable = scannable;
  state.properties.directed = directed;
  state.properties.high_duty_cycle_directed_connectable =
      high_duty_cycle_directed_connectable;
  state.properties.use_legacy_pdus = use_legacy_pdus;
  state.properties.anonymous_advertising = anonymous_advertising;
  state.properties.include_tx_power = include_tx_power;

  state.own_address_type = params.own_address_type().Read();
  state.interval_min = params.primary_advertising_interval_min().Read();
  state.interval_max = params.primary_advertising_interval_max().Read();

  if (state.interval_min >= state.interval_max) {
    bt_log(INFO,
           "fake-hci",
           "advertising interval min (%d) not strictly less than max (%d)",
           state.interval_min,
           state.interval_max);
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_PARAMETERS_V1,
        pwemb::StatusCode::UNSUPPORTED_FEATURE_OR_PARAMETER);
    return;
  }

  if (state.interval_min < hci_spec::kLEExtendedAdvertisingIntervalMin) {
    bt_log(INFO,
           "fake-hci",
           "advertising interval min (%d) less than spec min (%dstate.)",
           state.interval_min,
           hci_spec::kLEAdvertisingIntervalMin);
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_PARAMETERS_V1,
        pwemb::StatusCode::UNSUPPORTED_FEATURE_OR_PARAMETER);
    return;
  }

  if (state.interval_max > hci_spec::kLEExtendedAdvertisingIntervalMax) {
    bt_log(INFO,
           "fake-hci",
           "advertising interval max (%d) greater than spec max (%d)",
           state.interval_max,
           hci_spec::kLEAdvertisingIntervalMax);
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_PARAMETERS_V1,
        pwemb::StatusCode::UNSUPPORTED_FEATURE_OR_PARAMETER);
    return;
  }

  uint8_t advertising_channels =
      params.primary_advertising_channel_map().BackingStorage().ReadUInt();
  if (!advertising_channels) {
    bt_log(INFO,
           "fake-hci",
           "at least one bit must be set in primary advertising channel map");
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_PARAMETERS_V1,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  int8_t advertising_tx_power = params.advertising_tx_power().Read();
  if (advertising_tx_power !=
          hci_spec::kLEExtendedAdvertisingTxPowerNoPreference &&
      (advertising_tx_power < hci_spec::kLEAdvertisingTxPowerMin ||
       advertising_tx_power > hci_spec::kLEAdvertisingTxPowerMax)) {
    bt_log(INFO,
           "fake-hci",
           "advertising tx power out of range: %d",
           advertising_tx_power);
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_PARAMETERS_V1,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  // write full state back only at the end (we don't have a reference because we
  // only want to write if there are no errors)
  extended_advertising_states_[handle] = state;

  auto packet = hci::EventPacket::New<
      pwemb::LESetExtendedAdvertisingParametersCommandCompleteEventWriter>(
      hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.selected_tx_power().Write(hci_spec::kLEAdvertisingTxPowerMax);
  RespondWithCommandComplete(
      pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_PARAMETERS_V1, &packet);
  NotifyAdvertisingState();
}

void FakeController::OnLESetExtendedAdvertisingData(
    const pwemb::LESetExtendedAdvertisingDataCommandView& params) {
  if (!EnableExtendedAdvertising()) {
    bt_log(
        INFO,
        "fake-hci",
        "extended advertising command rejected, another type already in use");
    RespondWithCommandStatus(pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_DATA,
                             pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  hci_spec::AdvertisingHandle handle = params.advertising_handle().Read();

  if (!IsValidAdvertisingHandle(handle)) {
    bt_log(ERROR, "fake-hci", "advertising handle outside range: %d", handle);
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_DATA,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  if (extended_advertising_states_.count(handle) == 0) {
    bt_log(INFO,
           "fake-hci",
           "advertising handle (%d) maps to an unknown advertising set",
           handle);
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_DATA,
        pwemb::StatusCode::UNKNOWN_ADVERTISING_IDENTIFIER);
    return;
  }

  LEAdvertisingState& state = extended_advertising_states_[handle];

  // removing advertising data entirely doesn't require us to check for error
  // conditions
  size_t advertising_data_length = params.advertising_data_length().Read();
  if (advertising_data_length == 0) {
    state.data_length = 0;
    std::memset(state.data, 0, sizeof(state.data));
    RespondWithCommandComplete(pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_DATA,
                               pwemb::StatusCode::SUCCESS);
    NotifyAdvertisingState();
    return;
  }

  // directed advertising doesn't support advertising data
  if (state.IsDirectedAdvertising()) {
    bt_log(INFO,
           "fake-hci",
           "cannot provide advertising data when using directed advertising");
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_DATA,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  if (params.operation().Read() ==
      pwemb::LESetExtendedAdvDataOp::UNCHANGED_DATA) {
    RespondWithCommandComplete(pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_DATA,
                               pwemb::StatusCode::SUCCESS);
    return;
  }

  // For backwards compatibility with older devices, we support both legacy and
  // extended advertising pdus. Each pdu type has its own size limits.
  if (state.properties.use_legacy_pdus &&
      advertising_data_length > hci_spec::kMaxLEAdvertisingDataLength) {
    bt_log(INFO,
           "fake-hci",
           "data length (%zu bytes) larger than legacy PDU size limit",
           advertising_data_length);
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_DATA,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  if (!state.properties.use_legacy_pdus &&
      advertising_data_length > pwemb::LESetExtendedAdvertisingDataCommand::
                                    advertising_data_length_max()) {
    bt_log(INFO,
           "fake-hci",
           "data length (%zu bytes) larger than individual extended PDU size "
           "limit",
           advertising_data_length);
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_DATA,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  if (!state.properties.use_legacy_pdus &&
      state.data_length + advertising_data_length >
          max_advertising_data_length_) {
    bt_log(INFO,
           "fake-hci",
           "data length (%zu bytes) larger than total extended PDU size limit",
           advertising_data_length);
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_DATA,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  if (state.properties.use_legacy_pdus ||
      params.operation().Read() == pwemb::LESetExtendedAdvDataOp::COMPLETE ||
      params.operation().Read() ==
          pwemb::LESetExtendedAdvDataOp::FIRST_FRAGMENT) {
    std::memcpy(state.data,
                params.advertising_data().BackingStorage().data(),
                advertising_data_length);
    state.data_length = static_cast<uint16_t>(advertising_data_length);
  } else {
    std::memcpy(state.data + state.data_length,
                params.advertising_data().BackingStorage().data(),
                advertising_data_length);
    state.data_length += advertising_data_length;
  }

  RespondWithCommandComplete(pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_DATA,
                             pwemb::StatusCode::SUCCESS);
  NotifyAdvertisingState();
}

void FakeController::OnLESetExtendedScanResponseData(
    const pwemb::LESetExtendedScanResponseDataCommandView& params) {
  if (!EnableExtendedScanning()) {
    bt_log(
        INFO,
        "fake-hci",
        "extended advertising command rejected, another type already in use");
    RespondWithCommandStatus(pwemb::OpCode::LE_SET_EXTENDED_SCAN_RESPONSE_DATA,
                             pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  hci_spec::AdvertisingHandle handle = params.advertising_handle().Read();

  if (!IsValidAdvertisingHandle(handle)) {
    bt_log(ERROR, "fake-hci", "advertising handle outside range: %d", handle);
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_EXTENDED_SCAN_RESPONSE_DATA,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  if (extended_advertising_states_.count(handle) == 0) {
    bt_log(INFO,
           "fake-hci",
           "advertising handle (%d) maps to an unknown advertising set",
           handle);
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_EXTENDED_SCAN_RESPONSE_DATA,
        pwemb::StatusCode::UNKNOWN_ADVERTISING_IDENTIFIER);
    return;
  }

  LEAdvertisingState& state = extended_advertising_states_[handle];

  // removing scan response data entirely doesn't require us to check for error
  // conditions
  size_t scan_response_data_length = params.scan_response_data_length().Read();
  if (scan_response_data_length == 0) {
    state.scan_rsp_length = 0;
    std::memset(state.scan_rsp_data, 0, sizeof(state.scan_rsp_data));
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_EXTENDED_SCAN_RESPONSE_DATA,
        pwemb::StatusCode::SUCCESS);
    NotifyAdvertisingState();
    return;
  }

  // adding or changing scan response data, check for error conditions
  if (!state.properties.scannable) {
    bt_log(
        INFO,
        "fake-hci",
        "cannot provide scan response data for unscannable advertising types");
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_EXTENDED_SCAN_RESPONSE_DATA,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  if (params.operation().Read() ==
      pwemb::LESetExtendedAdvDataOp::UNCHANGED_DATA) {
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_EXTENDED_SCAN_RESPONSE_DATA,
        pwemb::StatusCode::SUCCESS);
    return;
  }

  // For backwards compatibility with older devices, we support both legacy and
  // extended advertising pdus. Each pdu type has its own size limits.
  if (state.properties.use_legacy_pdus &&
      scan_response_data_length > hci_spec::kMaxLEAdvertisingDataLength) {
    bt_log(INFO,
           "fake-hci",
           "data length (%zu bytes) larger than legacy PDU size limit",
           scan_response_data_length);
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_EXTENDED_SCAN_RESPONSE_DATA,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  if (!state.properties.use_legacy_pdus &&
      scan_response_data_length > pwemb::LESetExtendedAdvertisingDataCommand::
                                      advertising_data_length_max()) {
    bt_log(INFO,
           "fake-hci",
           "data length (%zu bytes) larger than individual extended PDU size "
           "limit",
           scan_response_data_length);
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_EXTENDED_SCAN_RESPONSE_DATA,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  if (!state.properties.use_legacy_pdus &&
      state.scan_rsp_length + scan_response_data_length >
          max_advertising_data_length_) {
    bt_log(INFO,
           "fake-hci",
           "data length (%zu bytes) larger than total extended PDU size limit",
           scan_response_data_length);
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_EXTENDED_SCAN_RESPONSE_DATA,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  if (state.properties.use_legacy_pdus ||
      params.operation().Read() == pwemb::LESetExtendedAdvDataOp::COMPLETE ||
      params.operation().Read() ==
          pwemb::LESetExtendedAdvDataOp::FIRST_FRAGMENT) {
    std::memcpy(state.scan_rsp_data,
                params.scan_response_data().BackingStorage().data(),
                scan_response_data_length);
    state.scan_rsp_length = static_cast<uint16_t>(scan_response_data_length);
  } else {
    std::memcpy(state.scan_rsp_data + state.scan_rsp_length,
                params.scan_response_data().BackingStorage().data(),
                scan_response_data_length);
    state.scan_rsp_length += scan_response_data_length;
  }

  RespondWithCommandComplete(pwemb::OpCode::LE_SET_EXTENDED_SCAN_RESPONSE_DATA,
                             pwemb::StatusCode::SUCCESS);
  NotifyAdvertisingState();
}

void FakeController::OnLESetExtendedAdvertisingEnable(
    const pwemb::LESetExtendedAdvertisingEnableCommandView& params) {
  if (!EnableExtendedAdvertising()) {
    bt_log(
        INFO,
        "fake-hci",
        "extended advertising command rejected, another type already in use");
    RespondWithCommandStatus(pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_ENABLE,
                             pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  uint8_t num_sets = params.num_sets().Read();

  // do some preliminary checks before making any state changes
  if (num_sets != 0) {
    std::unordered_set<hci_spec::AdvertisingHandle> handles;

    for (uint8_t i = 0; i < num_sets; i++) {
      hci_spec::AdvertisingHandle handle =
          params.data()[i].advertising_handle().Read();

      if (!IsValidAdvertisingHandle(handle)) {
        bt_log(
            ERROR, "fake-hci", "advertising handle outside range: %d", handle);
        RespondWithCommandComplete(
            pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_ENABLE,
            pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
        return;
      }

      // cannot have two array entries for the same advertising handle
      if (handles.count(handle) != 0) {
        bt_log(INFO,
               "fake-hci",
               "cannot refer to handle more than once (handle: %d)",
               handle);
        RespondWithCommandComplete(
            pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_ENABLE,
            pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
        return;
      }
      handles.insert(handle);

      // cannot have instructions for an advertising handle we don't know about
      if (extended_advertising_states_.count(handle) == 0) {
        bt_log(INFO,
               "fake-hci",
               "cannot enable/disable an unknown handle (handle: %d)",
               handle);
        RespondWithCommandComplete(
            pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_ENABLE,
            pwemb::StatusCode::UNKNOWN_ADVERTISING_IDENTIFIER);
        return;
      }
    }
  }

  if (params.enable().Read() == pwemb::GenericEnableParam::DISABLE) {
    if (num_sets == 0) {
      // if params.enable == kDisable and params.num_sets == 0, spec asks we
      // disable all
      for (auto& element : extended_advertising_states_) {
        element.second.enabled = false;
      }
    } else {
      for (int i = 0; i < num_sets; i++) {
        hci_spec::AdvertisingHandle handle =
            params.data()[i].advertising_handle().Read();
        extended_advertising_states_[handle].enabled = false;
      }
    }

    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_ENABLE,
        pwemb::StatusCode::SUCCESS);
    NotifyAdvertisingState();
    return;
  }

  // rest of the function deals with enabling advertising for a given set of
  // advertising sets
  PW_CHECK(params.enable().Read() == pwemb::GenericEnableParam::ENABLE);

  if (num_sets == 0) {
    bt_log(
        INFO, "fake-hci", "cannot enable with an empty advertising set list");
    RespondWithCommandComplete(
        pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_ENABLE,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  for (uint8_t i = 0; i < num_sets; i++) {
    // FakeController currently doesn't support testing with duration and max
    // events. When those are used in the host, these checks will fail and
    // remind us to add the necessary code to FakeController.
    PW_CHECK(params.data()[i].duration().Read() == 0);
    PW_CHECK(params.data()[i].max_extended_advertising_events().Read() == 0);

    hci_spec::AdvertisingHandle handle =
        params.data()[i].advertising_handle().Read();
    LEAdvertisingState& state = extended_advertising_states_[handle];

    if (state.IsDirectedAdvertising() && state.data_length == 0) {
      bt_log(
          INFO,
          "fake-hci",
          "cannot enable type requiring advertising data without setting it");
      RespondWithCommandComplete(
          pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_ENABLE,
          pwemb::StatusCode::COMMAND_DISALLOWED);
      return;
    }

    if (state.properties.scannable && state.scan_rsp_length == 0) {
      bt_log(INFO,
             "fake-hci",
             "cannot enable, requires scan response data but hasn't been set");
      RespondWithCommandComplete(
          pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_ENABLE,
          pwemb::StatusCode::COMMAND_DISALLOWED);
      return;
    }

    // Core Spec v6.0, Volume 4, Part E, Section 7.8.56:
    // If the advertising set's Own_Address_Type parameter is set to 0x01 and
    // the random address for the advertising set has not been initialized using
    // the HCI_LE_Set_Advertising_Set_Random_Address command, the Controller
    // shall return the error code Invalid HCI Command Parameters (0x12).
    if (state.own_address_type == pwemb::LEOwnAddressType::RANDOM &&
        !state.random_address.has_value()) {
      bt_log(INFO,
             "fake-hci",
             "cannot enable, requires random address but hasn't been set");
      RespondWithCommandComplete(
          pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_ENABLE,
          pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
      return;
    }
  }

  for (uint8_t i = 0; i < num_sets; i++) {
    hci_spec::AdvertisingHandle handle =
        params.data()[i].advertising_handle().Read();
    LEAdvertisingState& state = extended_advertising_states_[handle];
    state.enabled = true;
  }

  RespondWithCommandComplete(pwemb::OpCode::LE_SET_EXTENDED_ADVERTISING_ENABLE,
                             pwemb::StatusCode::SUCCESS);
  NotifyAdvertisingState();
}

void FakeController::OnLEReadMaximumAdvertisingDataLength() {
  if (!settings_.SupportedCommandsView()
           .le_read_maximum_advertising_data_length()
           .Read()) {
    RespondWithCommandComplete(
        pwemb::OpCode::LE_READ_MAXIMUM_ADVERTISING_DATA_LENGTH,
        pwemb::StatusCode::UNKNOWN_COMMAND);
  }

  auto response = hci::EventPacket::New<
      pwemb::LEReadMaximumAdvertisingDataLengthCommandCompleteEventWriter>(
      hci_spec::kCommandCompleteEventCode);
  auto view = response.view_t();
  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.max_advertising_data_length().Write(max_advertising_data_length_);
  RespondWithCommandComplete(
      pwemb::OpCode::LE_READ_MAXIMUM_ADVERTISING_DATA_LENGTH, &response);
}

void FakeController::OnLEReadNumberOfSupportedAdvertisingSets() {
  auto event = hci::EventPacket::New<
      pwemb::LEReadNumberOfSupportedAdvertisingSetsCommandCompleteEventWriter>(
      hci_spec::kCommandCompleteEventCode);
  auto view = event.view_t();
  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.num_supported_advertising_sets().Write(num_supported_advertising_sets_);
  RespondWithCommandComplete(
      pwemb::OpCode::LE_READ_NUMBER_OF_SUPPORTED_ADVERTISING_SETS, &event);
}

void FakeController::OnLERemoveAdvertisingSet(
    const pwemb::LERemoveAdvertisingSetCommandView& params) {
  hci_spec::AdvertisingHandle handle = params.advertising_handle().Read();

  if (!IsValidAdvertisingHandle(handle)) {
    bt_log(ERROR, "fake-hci", "advertising handle outside range: %d", handle);
    RespondWithCommandComplete(
        pwemb::OpCode::LE_REMOVE_ADVERTISING_SET,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  if (extended_advertising_states_.count(handle) == 0) {
    bt_log(INFO,
           "fake-hci",
           "advertising handle (%d) maps to an unknown advertising set",
           handle);
    RespondWithCommandComplete(
        pwemb::OpCode::LE_REMOVE_ADVERTISING_SET,
        pwemb::StatusCode::UNKNOWN_ADVERTISING_IDENTIFIER);
    return;
  }

  if (extended_advertising_states_[handle].enabled) {
    bt_log(INFO,
           "fake-hci",
           "cannot remove enabled advertising set (handle: %d)",
           handle);
    RespondWithCommandComplete(pwemb::OpCode::LE_REMOVE_ADVERTISING_SET,
                               pwemb::StatusCode::COMMAND_DISALLOWED);
    return;
  }

  extended_advertising_states_.erase(handle);
  RespondWithCommandComplete(pwemb::OpCode::LE_REMOVE_ADVERTISING_SET,
                             pwemb::StatusCode::SUCCESS);
  NotifyAdvertisingState();
}

void FakeController::OnLEClearAdvertisingSets() {
  for (const auto& element : extended_advertising_states_) {
    if (element.second.enabled) {
      bt_log(INFO,
             "fake-hci",
             "cannot remove currently enabled advertising set (handle: %d)",
             element.second.enabled);
      RespondWithCommandComplete(pwemb::OpCode::LE_CLEAR_ADVERTISING_SETS,
                                 pwemb::StatusCode::COMMAND_DISALLOWED);
      return;
    }
  }

  extended_advertising_states_.clear();
  RespondWithCommandComplete(pwemb::OpCode::LE_CLEAR_ADVERTISING_SETS,
                             pwemb::StatusCode::SUCCESS);
  NotifyAdvertisingState();
}

void FakeController::OnLEReadAdvertisingChannelTxPower() {
  if (!respond_to_tx_power_read_) {
    return;
  }

  // Send back arbitrary tx power.
  auto packet = hci::EventPacket::New<
      pwemb::LEReadAdvertisingChannelTxPowerCommandCompleteEventWriter>(
      hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.tx_power_level().Write(9);
  RespondWithCommandComplete(
      pwemb::OpCode::LE_READ_ADVERTISING_CHANNEL_TX_POWER, &packet);
}

void FakeController::SendLEAdvertisingSetTerminatedEvent(
    hci_spec::ConnectionHandle conn_handle,
    hci_spec::AdvertisingHandle adv_handle) {
  auto packet =
      hci::EventPacket::New<pwemb::LEAdvertisingSetTerminatedSubeventWriter>(
          hci_spec::kLEMetaEventCode);
  auto view = packet.view_t();
  view.le_meta_event().subevent_code().Write(
      hci_spec::kLEAdvertisingSetTerminatedSubeventCode);
  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.connection_handle().Write(conn_handle);
  view.advertising_handle().Write(adv_handle);
  SendCommandChannelPacket(packet.data());
}

void FakeController::SendAndroidLEMultipleAdvertisingStateChangeSubevent(
    hci_spec::ConnectionHandle conn_handle,
    hci_spec::AdvertisingHandle adv_handle) {
  auto packet =
      hci::EventPacket::New<android_emb::LEMultiAdvtStateChangeSubeventWriter>(
          hci_spec::kVendorDebugEventCode);
  auto view = packet.view_t();
  view.vendor_event().subevent_code().Write(
      android_hci::kLEMultiAdvtStateChangeSubeventCode);
  view.advertising_handle().Write(adv_handle);
  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.connection_handle().Write(conn_handle);
  SendCommandChannelPacket(packet.data());
}

void FakeController::OnReadLocalSupportedControllerDelay(
    [[maybe_unused]] const pwemb::ReadLocalSupportedControllerDelayCommandView&
        params) {
  auto packet = hci::EventPacket::New<
      pwemb::ReadLocalSupportedControllerDelayCommandCompleteEventWriter>(
      hci_spec::kCommandCompleteEventCode);
  auto response_view = packet.view_t();
  if (settings_.SupportedCommandsView()
          .read_local_supported_controller_delay()
          .Read()) {
    response_view.status().Write(pwemb::StatusCode::SUCCESS);
    response_view.min_controller_delay().Write(0);  // no delay
    response_view.max_controller_delay().Write(
        pwemb::ReadLocalSupportedControllerDelayCommandCompleteEvent::
            max_delay_usecs());  // maximum allowable delay
  } else {
    response_view.status().Write(pwemb::StatusCode::UNKNOWN_COMMAND);
  }

  RespondWithCommandComplete(
      pwemb::OpCode::READ_LOCAL_SUPPORTED_CONTROLLER_DELAY, &packet);
}

void FakeController::OnLERejectCisRequestCommand(
    const pw::bluetooth::emboss::LERejectCISRequestCommandView& params) {
  if (le_cis_reject_cb_) {
    le_cis_reject_cb_(params.connection_handle().Read());
  }
  auto packet = hci::EventPacket::New<
      pwemb::LERejectCisRequestCommandCompleteEventWriter>(
      hci_spec::kCommandCompleteEventCode);
  auto response_view = packet.view_t();
  response_view.status().Write(pwemb::StatusCode::SUCCESS);
  response_view.connection_handle().Write(params.connection_handle().Read());
  RespondWithCommandComplete(pwemb::OpCode::LE_REJECT_CIS_REQUEST, &packet);
}

void FakeController::OnCommandPacketReceived(
    const PacketView<hci_spec::CommandHeader>& command_packet) {
  hci_spec::OpCode opcode = pw::bytes::ConvertOrderFrom(
      cpp20::endian::little, command_packet.header().opcode);

  bt_log(
      TRACE, "fake-hci", "received command packet with opcode: %#.4x", opcode);
  // We handle commands immediately unless a client has explicitly set a
  // listener for `opcode`.
  if (paused_opcode_listeners_.find(opcode) == paused_opcode_listeners_.end()) {
    HandleReceivedCommandPacket(command_packet);
    return;
  }

  bt_log(DEBUG, "fake-hci", "pausing response for opcode: %#.4x", opcode);
  paused_opcode_listeners_[opcode](
      [this, packet_data = DynamicByteBuffer(command_packet.data())]() {
        PacketView<hci_spec::CommandHeader> command_packet(
            &packet_data, packet_data.size() - sizeof(hci_spec::CommandHeader));
        HandleReceivedCommandPacket(command_packet);
      });
}

void FakeController::OnAndroidLEGetVendorCapabilities() {
  // We use the
  // android_emb::LEGetVendorCapabilitiesCommandCompleteEventWriter as
  // storage. This is the full HCI packet, including the header. Ensure we don't
  // accidentally send the header twice by using the overloaded
  // RespondWithCommandComplete that takes in an EventPacket. The one that
  // takes a BufferView allocates space for the header, assuming that it's been
  // sent only the payload.
  auto packet = hci::EventPacket::New<
      android_emb::LEGetVendorCapabilitiesCommandCompleteEventWriter>(
      hci_spec::kCommandCompleteEventCode);
  MutableBufferView buffer = packet.mutable_data();
  settings_.android_extension_settings.data().Copy(&buffer);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_GET_VENDOR_CAPABILITIES,
                             &packet);
}

void FakeController::OnAndroidStartA2dpOffload(
    const android_emb::StartA2dpOffloadCommandView& params) {
  auto packet =
      hci::EventPacket::New<android_emb::A2dpOffloadCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::A2dpOffloadSubOpcode::START_LEGACY);

  // return in case A2DP offload already started
  if (offloaded_a2dp_channel_state_) {
    view.status().Write(pwemb::StatusCode::CONNECTION_ALREADY_EXISTS);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_A2DP_HARDWARE_OFFLOAD,
                               &packet);
    return;
  }

  // SCMS-T is not currently supported
  if (params.scms_t_enable().enabled().Read() ==
      pwemb::GenericEnableParam::ENABLE) {
    view.status().Write(pwemb::StatusCode::UNSUPPORTED_FEATURE_OR_PARAMETER);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_A2DP_HARDWARE_OFFLOAD,
                               &packet);
    return;
  }

  // return in case any parameter has an invalid value
  view.status().Write(pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);

  android_emb::A2dpCodecType const codec_type = params.codec_type().Read();
  switch (codec_type) {
    case android_emb::A2dpCodecType::SBC:
    case android_emb::A2dpCodecType::AAC:
    case android_emb::A2dpCodecType::APTX:
    case android_emb::A2dpCodecType::APTX_HD:
    case android_emb::A2dpCodecType::LDAC:
      break;
      RespondWithCommandComplete(pwemb::OpCode::ANDROID_A2DP_HARDWARE_OFFLOAD,
                                 &packet);
      return;
  }

  android_emb::A2dpSamplingFrequency const sampling_frequency =
      params.sampling_frequency().Read();
  switch (sampling_frequency) {
    case android_emb::A2dpSamplingFrequency::HZ_44100:
    case android_emb::A2dpSamplingFrequency::HZ_48000:
    case android_emb::A2dpSamplingFrequency::HZ_88200:
    case android_emb::A2dpSamplingFrequency::HZ_96000:
      break;
    default:
      RespondWithCommandComplete(pwemb::OpCode::ANDROID_A2DP_HARDWARE_OFFLOAD,
                                 &packet);
      return;
  }

  android_emb::A2dpBitsPerSample const bits_per_sample =
      static_cast<android_emb::A2dpBitsPerSample>(
          params.bits_per_sample().Read());
  switch (bits_per_sample) {
    case android_emb::A2dpBitsPerSample::BITS_PER_SAMPLE_16:
    case android_emb::A2dpBitsPerSample::BITS_PER_SAMPLE_24:
    case android_emb::A2dpBitsPerSample::BITS_PER_SAMPLE_32:
      break;
    default:
      RespondWithCommandComplete(pwemb::OpCode::ANDROID_A2DP_HARDWARE_OFFLOAD,
                                 &packet);
      return;
  }

  android_emb::A2dpChannelMode const channel_mode =
      static_cast<android_emb::A2dpChannelMode>(params.channel_mode().Read());
  switch (channel_mode) {
    case android_emb::A2dpChannelMode::MONO:
    case android_emb::A2dpChannelMode::STEREO:
      break;
    default:
      RespondWithCommandComplete(pwemb::OpCode::ANDROID_A2DP_HARDWARE_OFFLOAD,
                                 &packet);
      return;
  }

  uint32_t const encoded_audio_bitrate = pw::bytes::ConvertOrderFrom(
      cpp20::endian::little, params.encoded_audio_bitrate().Read());
  // Bits 0x01000000 to 0xFFFFFFFF are reserved
  if (encoded_audio_bitrate >= 0x01000000) {
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_A2DP_HARDWARE_OFFLOAD,
                               &packet);
    return;
  }

  OffloadedA2dpChannel state;
  state.codec_type = codec_type;
  state.max_latency = pw::bytes::ConvertOrderFrom(cpp20::endian::little,
                                                  params.max_latency().Read());
  state.scms_t_enable.view().CopyFrom(params.scms_t_enable());
  state.sampling_frequency = sampling_frequency;
  state.bits_per_sample = bits_per_sample;
  state.channel_mode = channel_mode;
  state.encoded_audio_bitrate = encoded_audio_bitrate;
  state.connection_handle = pw::bytes::ConvertOrderFrom(
      cpp20::endian::little, params.connection_handle().Read());
  state.l2cap_channel_id = pw::bytes::ConvertOrderFrom(
      cpp20::endian::little, params.l2cap_channel_id().Read());
  state.l2cap_mtu_size = pw::bytes::ConvertOrderFrom(
      cpp20::endian::little, params.l2cap_mtu_size().Read());
  offloaded_a2dp_channel_state_ = state;

  view.status().Write(pwemb::StatusCode::SUCCESS);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_A2DP_HARDWARE_OFFLOAD,
                             &packet);
}

void FakeController::OnAndroidStopA2dpOffload() {
  auto packet =
      hci::EventPacket::New<android_emb::A2dpOffloadCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::A2dpOffloadSubOpcode::STOP_LEGACY);

  if (!offloaded_a2dp_channel_state_) {
    view.status().Write(pwemb::StatusCode::REPEATED_ATTEMPTS);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_A2DP_HARDWARE_OFFLOAD,
                               &packet);
    return;
  }

  offloaded_a2dp_channel_state_ = std::nullopt;

  view.status().Write(pwemb::StatusCode::SUCCESS);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_A2DP_HARDWARE_OFFLOAD,
                             &packet);
}

void FakeController::OnAndroidA2dpOffloadCommand(
    const PacketView<hci_spec::CommandHeader>& command_packet) {
  const auto& payload = command_packet.payload_data();

  uint8_t subopcode = payload.To<uint8_t>();
  switch (subopcode) {
    case android_hci::kStartA2dpOffloadCommandSubopcode: {
      auto view = android_emb::MakeStartA2dpOffloadCommandView(
          command_packet.data().data(), command_packet.size());
      OnAndroidStartA2dpOffload(view);
      break;
    }
    case android_hci::kStopA2dpOffloadCommandSubopcode:
      OnAndroidStopA2dpOffload();
      break;
    default:
      bt_log(WARN,
             "fake-hci",
             "unhandled android A2DP offload command, subopcode: %#.4x",
             subopcode);
      RespondWithCommandComplete(pwemb::OpCode::ANDROID_A2DP_HARDWARE_OFFLOAD,
                                 pwemb::StatusCode::UNKNOWN_COMMAND);
      break;
  }
}

void FakeController::OnAndroidLEMultiAdvtSetAdvtParam(
    const android_emb::LEMultiAdvtSetAdvtParamCommandView& params) {
  auto packet =
      hci::EventPacket::New<android_emb::LEMultiAdvtCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(
      android_emb::LEMultiAdvtSubOpcode::SET_ADVERTISING_PARAMETERS);

  if (!EnableVendorAdvertising()) {
    bt_log(INFO,
           "fake-hci",
           "vendor advertising command rejected, another advertising in use");
    view.status().Write(pwemb::StatusCode::COMMAND_DISALLOWED);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
    return;
  }

  hci_spec::AdvertisingHandle handle = params.adv_handle().Read();
  if (!IsValidAdvertisingHandle(handle)) {
    bt_log(ERROR, "fake-hci", "advertising handle outside range: %d", handle);

    view.status().Write(pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
    return;
  }

  // ensure we can allocate memory for this advertising set if not already
  // present
  if (extended_advertising_states_.count(handle) == 0 &&
      extended_advertising_states_.size() >= num_supported_advertising_sets()) {
    bt_log(INFO,
           "fake-hci",
           "no available memory for new advertising set, handle: %d",
           handle);

    view.status().Write(pwemb::StatusCode::MEMORY_CAPACITY_EXCEEDED);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
    return;
  }

  // In case there is an error below, we want to reject all parameters instead
  // of storing a dead state and taking up an advertising handle. Avoid creating
  // the LEAdvertisingState directly in the map and add it in only once we have
  // made sure all is good.
  LEAdvertisingState state;
  state.own_address_type = params.own_addr_type().Read();

  pwemb::LEAdvertisingType adv_type = params.adv_type().Read();
  switch (adv_type) {
    case pwemb::LEAdvertisingType::CONNECTABLE_AND_SCANNABLE_UNDIRECTED:
      state.properties.connectable = true;
      state.properties.scannable = true;
      break;
    case pwemb::LEAdvertisingType::CONNECTABLE_LOW_DUTY_CYCLE_DIRECTED:
      state.properties.directed = true;
      state.properties.connectable = true;
      break;
    case pwemb::LEAdvertisingType::CONNECTABLE_HIGH_DUTY_CYCLE_DIRECTED:
      state.properties.high_duty_cycle_directed_connectable = true;
      state.properties.directed = true;
      state.properties.connectable = true;
      break;
    case pwemb::LEAdvertisingType::SCANNABLE_UNDIRECTED:
      state.properties.scannable = true;
      break;
    case pwemb::LEAdvertisingType::NOT_CONNECTABLE_UNDIRECTED:
      break;
  }

  state.interval_min = params.adv_interval_min().Read();
  state.interval_max = params.adv_interval_max().Read();

  if (state.interval_min >= state.interval_max) {
    bt_log(INFO,
           "fake-hci",
           "advertising interval min (%d) not strictly less than max (%d)",
           state.interval_min,
           state.interval_max);

    view.status().Write(pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
    return;
  }

  if (state.interval_min < hci_spec::kLEAdvertisingIntervalMin) {
    bt_log(INFO,
           "fake-hci",
           "advertising interval min (%d) less than spec min (%d)",
           state.interval_min,
           hci_spec::kLEAdvertisingIntervalMin);
    view.status().Write(pwemb::StatusCode::UNSUPPORTED_FEATURE_OR_PARAMETER);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
    return;
  }

  if (state.interval_max > hci_spec::kLEAdvertisingIntervalMax) {
    bt_log(INFO,
           "fake-hci",
           "advertising interval max (%d) greater than spec max (%d)",
           state.interval_max,
           hci_spec::kLEAdvertisingIntervalMax);
    view.status().Write(pwemb::StatusCode::UNSUPPORTED_FEATURE_OR_PARAMETER);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
    return;
  }

  // write full state back only at the end (we don't have a reference because we
  // only want to write if there are no errors)
  extended_advertising_states_[handle] = state;

  view.status().Write(pwemb::StatusCode::SUCCESS);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
  NotifyAdvertisingState();
}

void FakeController::OnAndroidLEMultiAdvtSetAdvtData(
    const android_emb::LEMultiAdvtSetAdvtDataCommandView& params) {
  auto packet =
      hci::EventPacket::New<android_emb::LEMultiAdvtCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(
      android_emb::LEMultiAdvtSubOpcode::SET_ADVERTISING_DATA);

  if (!EnableVendorAdvertising()) {
    bt_log(INFO,
           "fake-hci",
           "vendor advertising command rejected, another advertising in use");
    view.status().Write(pwemb::StatusCode::COMMAND_DISALLOWED);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
    return;
  }

  hci_spec::AdvertisingHandle handle = params.adv_handle().Read();
  if (!IsValidAdvertisingHandle(handle)) {
    bt_log(ERROR, "fake-hci", "advertising handle outside range: %d", handle);

    view.status().Write(pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
    return;
  }

  if (extended_advertising_states_.count(handle) == 0) {
    bt_log(INFO,
           "fake-hci",
           "advertising handle (%d) maps to an unknown advertising set",
           handle);

    view.status().Write(pwemb::StatusCode::UNKNOWN_ADVERTISING_IDENTIFIER);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
    return;
  }

  LEAdvertisingState& state = extended_advertising_states_[handle];

  // removing advertising data entirely doesn't require us to check for error
  // conditions
  if (params.adv_data_length().Read() == 0) {
    state.data_length = 0;
    std::memset(state.data, 0, sizeof(state.data));
    view.status().Write(pwemb::StatusCode::SUCCESS);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
    NotifyAdvertisingState();
    return;
  }

  // directed advertising doesn't support advertising data
  if (state.IsDirectedAdvertising()) {
    bt_log(INFO,
           "fake-hci",
           "cannot provide advertising data when using directed advertising");

    view.status().Write(pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
    return;
  }

  if (params.adv_data_length().Read() > hci_spec::kMaxLEAdvertisingDataLength) {
    bt_log(INFO,
           "fake-hci",
           "data length (%d bytes) larger than legacy PDU size limit",
           params.adv_data_length().Read());

    view.status().Write(pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
    return;
  }

  state.data_length = params.adv_data_length().Read();
  std::memcpy(state.data,
              params.adv_data().BackingStorage().data(),
              params.adv_data_length().Read());

  view.status().Write(pwemb::StatusCode::SUCCESS);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
  NotifyAdvertisingState();
}

void FakeController::OnAndroidLEMultiAdvtSetScanResp(
    const android_emb::LEMultiAdvtSetScanRespDataCommandView& params) {
  auto packet =
      hci::EventPacket::New<android_emb::LEMultiAdvtCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(
      android_emb::LEMultiAdvtSubOpcode::SET_SCAN_RESPONSE_DATA);

  if (!EnableVendorAdvertising()) {
    bt_log(INFO,
           "fake-hci",
           "vendor advertising command rejected, another advertising in use");
    view.status().Write(pwemb::StatusCode::COMMAND_DISALLOWED);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
    return;
  }

  hci_spec::AdvertisingHandle handle = params.adv_handle().Read();
  if (!IsValidAdvertisingHandle(handle)) {
    bt_log(ERROR, "fake-hci", "advertising handle outside range: %d", handle);

    view.status().Write(pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
    return;
  }

  if (extended_advertising_states_.count(handle) == 0) {
    bt_log(INFO,
           "fake-hci",
           "advertising handle (%d) maps to an unknown advertising set",
           handle);

    view.status().Write(pwemb::StatusCode::UNKNOWN_ADVERTISING_IDENTIFIER);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
    return;
  }

  LEAdvertisingState& state = extended_advertising_states_[handle];

  // removing scan response data entirely doesn't require us to check for error
  // conditions
  if (params.scan_resp_length().Read() == 0) {
    state.scan_rsp_length = 0;
    std::memset(state.scan_rsp_data, 0, sizeof(state.scan_rsp_data));

    view.status().Write(pwemb::StatusCode::SUCCESS);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
    NotifyAdvertisingState();
    return;
  }

  // adding or changing scan response data, check for error conditions
  if (!state.properties.scannable) {
    bt_log(
        INFO,
        "fake-hci",
        "cannot provide scan response data for unscannable advertising types");

    view.status().Write(pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
    return;
  }

  if (params.scan_resp_length().Read() >
      hci_spec::kMaxLEAdvertisingDataLength) {
    bt_log(INFO,
           "fake-hci",
           "data length (%d bytes) larger than legacy PDU size limit",
           params.scan_resp_length().Read());

    view.status().Write(pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
    return;
  }

  state.scan_rsp_length = params.scan_resp_length().Read();
  std::memcpy(state.scan_rsp_data,
              params.scan_resp_data().BackingStorage().data(),
              params.scan_resp_length().Read());

  view.status().Write(pwemb::StatusCode::SUCCESS);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
  NotifyAdvertisingState();
}

void FakeController::OnAndroidLEMultiAdvtSetRandomAddr(
    const android_emb::LEMultiAdvtSetRandomAddrCommandView& params) {
  auto packet =
      hci::EventPacket::New<android_emb::LEMultiAdvtCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(
      android_emb::LEMultiAdvtSubOpcode::SET_RANDOM_ADDRESS);

  if (!EnableVendorAdvertising()) {
    bt_log(INFO,
           "fake-hci",
           "vendor advertising command rejected, another advertising in use");
    view.status().Write(pwemb::StatusCode::COMMAND_DISALLOWED);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
    return;
  }

  hci_spec::AdvertisingHandle handle = params.adv_handle().Read();
  if (!IsValidAdvertisingHandle(handle)) {
    bt_log(ERROR, "fake-hci", "advertising handle outside range: %d", handle);

    view.status().Write(pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
    return;
  }

  if (extended_advertising_states_.count(handle) == 0) {
    bt_log(INFO,
           "fake-hci",
           "advertising handle (%d) maps to an unknown advertising set",
           handle);

    view.status().Write(pwemb::StatusCode::UNKNOWN_ADVERTISING_IDENTIFIER);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
    return;
  }

  LEAdvertisingState& state = extended_advertising_states_[handle];
  if (state.properties.connectable && state.enabled) {
    bt_log(
        INFO,
        "fake-hci",
        "cannot set LE random address while connectable advertising enabled");

    view.status().Write(pwemb::StatusCode::COMMAND_DISALLOWED);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
    return;
  }

  state.random_address =
      DeviceAddress(DeviceAddress::Type::kLERandom,
                    DeviceAddressBytes(params.random_address()));

  view.status().Write(pwemb::StatusCode::SUCCESS);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
}

void FakeController::OnAndroidLEMultiAdvtEnable(
    const android_emb::LEMultiAdvtEnableCommandView& params) {
  auto packet =
      hci::EventPacket::New<android_emb::LEMultiAdvtCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::LEMultiAdvtSubOpcode::ENABLE);

  if (!EnableVendorAdvertising()) {
    bt_log(INFO,
           "fake-hci",
           "vendor advertising command rejected, another advertising in use");
    view.status().Write(pwemb::StatusCode::COMMAND_DISALLOWED);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
    return;
  }

  hci_spec::AdvertisingHandle handle = params.advertising_handle().Read();

  if (!IsValidAdvertisingHandle(handle)) {
    bt_log(ERROR, "fake-hci", "advertising handle outside range: %d", handle);

    view.status().Write(pwemb::StatusCode::UNKNOWN_ADVERTISING_IDENTIFIER);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
    return;
  }

  bool enabled = false;
  if (params.enable().Read() == pwemb::GenericEnableParam::ENABLE) {
    enabled = true;
  }

  extended_advertising_states_[handle].enabled = enabled;

  view.status().Write(pwemb::StatusCode::SUCCESS);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT, &packet);
  NotifyAdvertisingState();
}

void FakeController::OnAndroidLEMultiAdvt(
    const PacketView<hci_spec::CommandHeader>& command_packet) {
  const auto& payload = command_packet.payload_data();
  uint8_t subopcode = payload.To<uint8_t>();

  if (MaybeRespondWithDefaultAndroidStatus(command_packet.header().opcode,
                                           subopcode)) {
    return;
  }

  switch (subopcode) {
    case android_hci::kLEMultiAdvtSetAdvtParamSubopcode: {
      auto params = android_emb::MakeLEMultiAdvtSetAdvtParamCommandView(
          command_packet.data().data(), command_packet.size());
      OnAndroidLEMultiAdvtSetAdvtParam(params);
      break;
    }
    case android_hci::kLEMultiAdvtSetAdvtDataSubopcode: {
      auto params = android_emb::MakeLEMultiAdvtSetAdvtDataCommandView(
          command_packet.data().data(), command_packet.size());
      OnAndroidLEMultiAdvtSetAdvtData(params);
      break;
    }
    case android_hci::kLEMultiAdvtSetScanRespSubopcode: {
      auto params = android_emb::MakeLEMultiAdvtSetScanRespDataCommandView(
          command_packet.data().data(), command_packet.size());
      OnAndroidLEMultiAdvtSetScanResp(params);
      break;
    }
    case android_hci::kLEMultiAdvtSetRandomAddrSubopcode: {
      auto params = android_emb::MakeLEMultiAdvtSetRandomAddrCommandView(
          command_packet.data().data(), command_packet.size());
      OnAndroidLEMultiAdvtSetRandomAddr(params);
      break;
    }
    case android_hci::kLEMultiAdvtEnableSubopcode: {
      auto view = android_emb::MakeLEMultiAdvtEnableCommandView(
          command_packet.data().data(), command_packet.size());
      OnAndroidLEMultiAdvtEnable(view);
      break;
    }
    default: {
      bt_log(WARN,
             "fake-hci",
             "unhandled android multiple advertising command, subopcode: %#.4x",
             subopcode);
      RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_MULTI_ADVT,
                                 pwemb::StatusCode::UNKNOWN_COMMAND);
      break;
    }
  }
}

void FakeController::OnAndroidLEApcfEnableCommand(
    const android_emb::LEApcfEnableCommandView& params) {
  if (params.enabled().Read() == pwemb::GenericEnableParam::ENABLE) {
    packet_filter_state_.enabled = true;
  } else {
    packet_filter_state_.enabled = false;
  }

  auto packet = hci::EventPacket::New<
      android_emb::LEApcfEnableCommandCompleteEventWriter>(
      hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::ENABLE);
  view.enabled().Write(params.enabled().Read());
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfSetFilteringParametersCommandAdd(
    const android_emb::LEApcfSetFilteringParametersCommandView& params) {
  uint8_t filter_index = params.filter_index().Read();

  PacketFilter filter;
  filter.filter_index = params.filter_index().Read();

  if (params.feature_selection().broadcast_address().Read()) {
    filter.features_selected.broadcast_address =
        android_emb::ApcfFeatureFilterLogic::AND;
  }

  if (params.feature_selection().service_uuid().Read()) {
    filter.features_selected.service_uuid =
        android_emb::ApcfFeatureFilterLogic::AND;
  }

  if (params.feature_selection().service_solicitation_uuid().Read()) {
    filter.features_selected.solicitation_uuid =
        android_emb::ApcfFeatureFilterLogic::AND;
  }

  if (params.feature_selection().local_name().Read()) {
    filter.features_selected.local_name =
        android_emb::ApcfFeatureFilterLogic::AND;
  }

  if (params.feature_selection().manufacturer_data().Read()) {
    filter.features_selected.manufacturer_data =
        android_emb::ApcfFeatureFilterLogic::AND;
  }

  if (params.feature_selection().service_data().Read()) {
    filter.features_selected.service_data =
        android_emb::ApcfFeatureFilterLogic::AND;
  }

  if (params.feature_selection().ad_type().Read()) {
    filter.features_selected.ad_type = android_emb::ApcfFeatureFilterLogic::AND;
  }

  // Sapphire only supports the OR operation across individual packet filter
  // lists. No need to implement the extra feature when we don't use
  // it. However, make sure that we don't accidentally try to use it in our
  // code.
  PW_CHECK(!params.list_logic_type().broadcast_address().Read());
  PW_CHECK(!params.list_logic_type().service_uuid().Read());
  PW_CHECK(!params.list_logic_type().service_solicitation_uuid().Read());
  PW_CHECK(!params.list_logic_type().local_name().Read());
  PW_CHECK(!params.list_logic_type().manufacturer_data().Read());
  PW_CHECK(!params.list_logic_type().service_data().Read());
  PW_CHECK(!params.list_logic_type().ad_type().Read());

  filter.filter_logic_type = params.filter_logic_type().Read();
  filter.rssi_high_threshold = params.rssi_high_threshold().Read();
  filter.rssi_low_threshold = params.rssi_low_threshold().Read();

  switch (params.delivery_mode().Read()) {
    case android_emb::ApcfDeliveryMode::IMMEDIATE:
      filter.delivery_mode =
          hci::AdvertisingPacketFilter::Config::DeliveryMode::kImmediate;
      break;
    case android_emb::ApcfDeliveryMode::BATCHED:
      filter.delivery_mode =
          hci::AdvertisingPacketFilter::Config::DeliveryMode::kBatched;
      break;
    case android_emb::ApcfDeliveryMode::ON_FOUND:
      PW_CRASH("fake controller doesn't support the on_found delivery mode");
      break;
  }

  // We ignore devliery modes other than immediate delivery for testing
  // purposes: fields related to a delivery mode of ON_FOUND aren't read
  // here. The testing focus is on the logic and functionality in the
  // implementation. The delivery mode parameter simply delays the delivery of
  // matching advertising packets.

  packet_filter_state_.filters[filter_index] = filter;

  uint8_t available_filters =
      packet_filter_state_.max_filters - packet_filter_state_.filters.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::SET_FILTERING_PARAMETERS);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfSetFilteringParametersCommandDelete(
    const android_emb::LEApcfSetFilteringParametersCommandView& params) {
  uint8_t filter_index = params.filter_index().Read();
  if (packet_filter_state_.filters.count(filter_index) == 0) {
    bt_log(WARN,
           "fake-hci",
           "packet filter index (%d) doesn't exist",
           filter_index);
    RespondWithCommandComplete(
        pwemb::OpCode::ANDROID_APCF,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  packet_filter_state_.filters_broadcast_address.erase(filter_index);
  packet_filter_state_.filters_service_uuid.erase(filter_index);
  packet_filter_state_.filters_solicitation_uuid.erase(filter_index);
  packet_filter_state_.filters_manufacturer_data.erase(filter_index);
  packet_filter_state_.filters_service_data.erase(filter_index);
  packet_filter_state_.filters_advertising_data.erase(filter_index);
  packet_filter_state_.filters.erase(filter_index);

  uint8_t available_filters =
      packet_filter_state_.max_filters - packet_filter_state_.filters.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::SET_FILTERING_PARAMETERS);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfSetFilteringParametersCommandClear(
    const android_emb::LEApcfSetFilteringParametersCommandView& params) {
  packet_filter_state_.filters_broadcast_address.clear();
  packet_filter_state_.filters_service_uuid.clear();
  packet_filter_state_.filters_solicitation_uuid.clear();
  packet_filter_state_.filters_manufacturer_data.clear();
  packet_filter_state_.filters_service_data.clear();
  packet_filter_state_.filters_advertising_data.clear();
  packet_filter_state_.filters.clear();

  uint8_t available_filters =
      packet_filter_state_.max_filters - packet_filter_state_.filters.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::SET_FILTERING_PARAMETERS);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfSetFilteringParametersCommand(
    const android_emb::LEApcfSetFilteringParametersCommandView& params) {
  android_emb::ApcfAction action = params.action().Read();

  switch (action) {
    case android_emb::ApcfAction::ADD:
      OnAndroidLEApcfSetFilteringParametersCommandAdd(params);
      break;
    case android_emb::ApcfAction::DELETE:
      OnAndroidLEApcfSetFilteringParametersCommandDelete(params);
      break;
    case android_emb::ApcfAction::CLEAR:
      OnAndroidLEApcfSetFilteringParametersCommandClear(params);
      break;
  }
}

void FakeController::OnAndroidLEApcfBroadcastAddressCommandAdd(
    const android_emb::LEApcfBroadcastAddressCommandView& params) {
  uint8_t filter_index = params.filter_index().Read();
  if (packet_filter_state_.filters.count(filter_index) == 0) {
    bt_log(WARN,
           "fake-hci",
           "packet filter index (%d) doesn't exist",
           filter_index);
    RespondWithCommandComplete(
        pwemb::OpCode::ANDROID_APCF,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  PacketFilter* filter = &packet_filter_state_.filters[filter_index];
  filter->broadcast_address = DeviceAddressBytes(params.broadcaster_address());
  packet_filter_state_.filters_broadcast_address[filter_index] = filter;

  uint8_t available_filters =
      packet_filter_state_.max_filters - packet_filter_state_.filters.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::BROADCAST_ADDRESS);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfBroadcastAddressCommandDelete(
    const android_emb::LEApcfBroadcastAddressCommandView& params) {
  uint8_t filter_index = params.filter_index().Read();
  if (packet_filter_state_.filters.count(filter_index) == 0) {
    bt_log(WARN,
           "fake-hci",
           "packet filter index (%d) doesn't exist",
           filter_index);
    RespondWithCommandComplete(
        pwemb::OpCode::ANDROID_APCF,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  packet_filter_state_.filters[filter_index].broadcast_address.reset();
  packet_filter_state_.filters_broadcast_address.erase(filter_index);

  uint8_t available_filters =
      packet_filter_state_.max_filters - packet_filter_state_.filters.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::BROADCAST_ADDRESS);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfBroadcastAddressCommandClear(
    const android_emb::LEApcfBroadcastAddressCommandView& params) {
  for (auto [_, filter] : packet_filter_state_.filters_broadcast_address) {
    filter->broadcast_address.reset();
  }
  packet_filter_state_.filters_broadcast_address.clear();

  uint8_t available_filters =
      packet_filter_state_.max_filters -
      packet_filter_state_.filters_broadcast_address.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::BROADCAST_ADDRESS);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfBroadcastAddressCommand(
    const android_emb::LEApcfBroadcastAddressCommandView& params) {
  android_emb::ApcfAction action = params.action().Read();

  switch (action) {
    case android_emb::ApcfAction::ADD:
      OnAndroidLEApcfBroadcastAddressCommandAdd(params);
      break;
    case android_emb::ApcfAction::DELETE:
      OnAndroidLEApcfBroadcastAddressCommandDelete(params);
      break;
    case android_emb::ApcfAction::CLEAR:
      OnAndroidLEApcfBroadcastAddressCommandClear(params);
      break;
  }
}

void FakeController::OnAndroidLEApcfServiceUUID16CommandAdd(
    const android_emb::LEApcfServiceUUID16CommandView& params) {
  uint8_t filter_index = params.filter_index().Read();
  if (packet_filter_state_.filters.count(filter_index) == 0) {
    bt_log(WARN,
           "fake-hci",
           "packet filter index (%d) doesn't exist",
           filter_index);
    RespondWithCommandComplete(
        pwemb::OpCode::ANDROID_APCF,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  BufferView uuid(params.uuid().BackingStorage().data(),
                  params.uuid().SizeInBytes());

  PacketFilter* filter = &packet_filter_state_.filters[filter_index];
  filter->service_uuid = UUID(uuid);
  packet_filter_state_.filters_service_uuid[filter_index] = filter;

  uint8_t available_filters = packet_filter_state_.max_filters -
                              packet_filter_state_.filters_service_uuid.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::SERVICE_UUID);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfServiceUUID16CommandDelete(
    const android_emb::LEApcfServiceUUID16CommandView& params) {
  uint8_t filter_index = params.filter_index().Read();
  if (packet_filter_state_.filters.count(filter_index) == 0) {
    bt_log(WARN,
           "fake-hci",
           "packet filter index (%d) doesn't exist",
           filter_index);
    RespondWithCommandComplete(
        pwemb::OpCode::ANDROID_APCF,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  packet_filter_state_.filters[filter_index].service_uuid.reset();
  packet_filter_state_.filters_service_uuid.erase(filter_index);

  uint8_t available_filters = packet_filter_state_.max_filters -
                              packet_filter_state_.filters_service_uuid.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::SERVICE_UUID);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfServiceUUID16CommandClear(
    const android_emb::LEApcfServiceUUID16CommandView& params) {
  for (auto [_, filter] : packet_filter_state_.filters_service_uuid) {
    filter->service_uuid.reset();
  }
  packet_filter_state_.filters_service_uuid.clear();

  uint8_t available_filters = packet_filter_state_.max_filters -
                              packet_filter_state_.filters_service_uuid.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::SERVICE_UUID);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfServiceUUID16Command(
    const android_emb::LEApcfServiceUUID16CommandView& params) {
  android_emb::ApcfAction action = params.action().Read();

  switch (action) {
    case android_emb::ApcfAction::ADD:
      OnAndroidLEApcfServiceUUID16CommandAdd(params);
      break;
    case android_emb::ApcfAction::DELETE:
      OnAndroidLEApcfServiceUUID16CommandDelete(params);
      break;
    case android_emb::ApcfAction::CLEAR:
      OnAndroidLEApcfServiceUUID16CommandClear(params);
      break;
  }
}

void FakeController::OnAndroidLEApcfServiceUUID32CommandAdd(
    const android_emb::LEApcfServiceUUID32CommandView& params) {
  uint8_t filter_index = params.filter_index().Read();
  if (packet_filter_state_.filters.count(filter_index) == 0) {
    bt_log(WARN,
           "fake-hci",
           "packet filter index (%d) doesn't exist",
           filter_index);
    RespondWithCommandComplete(
        pwemb::OpCode::ANDROID_APCF,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  BufferView uuid(params.uuid().BackingStorage().data(),
                  params.uuid().SizeInBytes());

  PacketFilter* filter = &packet_filter_state_.filters[filter_index];
  filter->service_uuid = UUID(uuid);
  packet_filter_state_.filters_service_uuid[filter_index] = filter;

  uint8_t available_filters = packet_filter_state_.max_filters -
                              packet_filter_state_.filters_service_uuid.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::SERVICE_UUID);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfServiceUUID32CommandDelete(
    const android_emb::LEApcfServiceUUID32CommandView& params) {
  uint8_t filter_index = params.filter_index().Read();
  if (packet_filter_state_.filters.count(filter_index) == 0) {
    bt_log(WARN,
           "fake-hci",
           "packet filter index (%d) doesn't exist",
           filter_index);
    RespondWithCommandComplete(
        pwemb::OpCode::ANDROID_APCF,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  packet_filter_state_.filters[filter_index].service_uuid.reset();
  packet_filter_state_.filters_service_uuid.erase(filter_index);

  uint8_t available_filters = packet_filter_state_.max_filters -
                              packet_filter_state_.filters_service_uuid.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::SERVICE_UUID);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfServiceUUID32CommandClear(
    const android_emb::LEApcfServiceUUID32CommandView& params) {
  for (auto [_, filter] : packet_filter_state_.filters_service_uuid) {
    filter->service_uuid.reset();
  }
  packet_filter_state_.filters_service_uuid.clear();

  uint8_t available_filters = packet_filter_state_.max_filters -
                              packet_filter_state_.filters_service_uuid.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::SERVICE_UUID);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfServiceUUID32Command(
    const android_emb::LEApcfServiceUUID32CommandView& params) {
  android_emb::ApcfAction action = params.action().Read();

  switch (action) {
    case android_emb::ApcfAction::ADD:
      OnAndroidLEApcfServiceUUID32CommandAdd(params);
      break;
    case android_emb::ApcfAction::DELETE:
      OnAndroidLEApcfServiceUUID32CommandDelete(params);
      break;
    case android_emb::ApcfAction::CLEAR:
      OnAndroidLEApcfServiceUUID32CommandClear(params);
      break;
  }
}

void FakeController::OnAndroidLEApcfServiceUUID128CommandAdd(
    const android_emb::LEApcfServiceUUID128CommandView& params) {
  uint8_t filter_index = params.filter_index().Read();
  if (packet_filter_state_.filters.count(filter_index) == 0) {
    bt_log(WARN,
           "fake-hci",
           "packet filter index (%d) doesn't exist",
           filter_index);
    RespondWithCommandComplete(
        pwemb::OpCode::ANDROID_APCF,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  BufferView uuid(params.uuid().BackingStorage().data(),
                  params.uuid().SizeInBytes());

  PacketFilter* filter = &packet_filter_state_.filters[filter_index];
  filter->service_uuid = UUID(uuid);
  packet_filter_state_.filters_service_uuid[filter_index] = filter;

  uint8_t available_filters = packet_filter_state_.max_filters -
                              packet_filter_state_.filters_service_uuid.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::SERVICE_UUID);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfServiceUUID128CommandDelete(
    const android_emb::LEApcfServiceUUID128CommandView& params) {
  uint8_t filter_index = params.filter_index().Read();
  if (packet_filter_state_.filters.count(filter_index) == 0) {
    bt_log(WARN,
           "fake-hci",
           "packet filter index (%d) doesn't exist",
           filter_index);
    RespondWithCommandComplete(
        pwemb::OpCode::ANDROID_APCF,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  packet_filter_state_.filters[filter_index].service_uuid.reset();
  packet_filter_state_.filters_service_uuid.erase(filter_index);

  uint8_t available_filters = packet_filter_state_.max_filters -
                              packet_filter_state_.filters_service_uuid.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::SERVICE_UUID);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfServiceUUID128CommandClear(
    const android_emb::LEApcfServiceUUID128CommandView& params) {
  for (auto [_, filter] : packet_filter_state_.filters_service_uuid) {
    filter->service_uuid.reset();
  }
  packet_filter_state_.filters_service_uuid.clear();

  uint8_t available_filters = packet_filter_state_.max_filters -
                              packet_filter_state_.filters_service_uuid.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::SERVICE_UUID);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfServiceUUID128Command(
    const android_emb::LEApcfServiceUUID128CommandView& params) {
  android_emb::ApcfAction action = params.action().Read();

  switch (action) {
    case android_emb::ApcfAction::ADD:
      OnAndroidLEApcfServiceUUID128CommandAdd(params);
      break;
    case android_emb::ApcfAction::DELETE:
      OnAndroidLEApcfServiceUUID128CommandDelete(params);
      break;
    case android_emb::ApcfAction::CLEAR:
      OnAndroidLEApcfServiceUUID128CommandClear(params);
      break;
  }
}

void FakeController::OnAndroidLEApcfSolicitationUUID16CommandAdd(
    const android_emb::LEApcfSolicitationUUID16CommandView& params) {
  uint8_t filter_index = params.filter_index().Read();
  if (packet_filter_state_.filters.count(filter_index) == 0) {
    bt_log(WARN,
           "fake-hci",
           "packet filter index (%d) doesn't exist",
           filter_index);
    RespondWithCommandComplete(
        pwemb::OpCode::ANDROID_APCF,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  BufferView uuid(params.uuid().BackingStorage().data(),
                  params.uuid().SizeInBytes());

  PacketFilter* filter = &packet_filter_state_.filters[filter_index];
  filter->solicitation_uuid = UUID(uuid);
  packet_filter_state_.filters_solicitation_uuid[filter_index] = filter;

  uint8_t available_filters =
      packet_filter_state_.max_filters -
      packet_filter_state_.filters_solicitation_uuid.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::SOLICITATION_UUID);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfSolicitationUUID16CommandDelete(
    const android_emb::LEApcfSolicitationUUID16CommandView& params) {
  uint8_t filter_index = params.filter_index().Read();
  if (packet_filter_state_.filters.count(filter_index) == 0) {
    bt_log(WARN,
           "fake-hci",
           "packet filter index (%d) doesn't exist",
           filter_index);
    RespondWithCommandComplete(
        pwemb::OpCode::ANDROID_APCF,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  packet_filter_state_.filters[filter_index].solicitation_uuid.reset();
  packet_filter_state_.filters_solicitation_uuid.erase(filter_index);

  uint8_t available_filters =
      packet_filter_state_.max_filters -
      packet_filter_state_.filters_solicitation_uuid.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::SOLICITATION_UUID);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfSolicitationUUID16CommandClear(
    const android_emb::LEApcfSolicitationUUID16CommandView& params) {
  for (auto [_, filter] : packet_filter_state_.filters_solicitation_uuid) {
    filter->solicitation_uuid.reset();
  }
  packet_filter_state_.filters_solicitation_uuid.clear();

  uint8_t available_filters =
      packet_filter_state_.max_filters -
      packet_filter_state_.filters_solicitation_uuid.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::SOLICITATION_UUID);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfSolicitationUUID16Command(
    const android_emb::LEApcfSolicitationUUID16CommandView& params) {
  android_emb::ApcfAction action = params.action().Read();

  switch (action) {
    case android_emb::ApcfAction::ADD:
      OnAndroidLEApcfSolicitationUUID16CommandAdd(params);
      break;
    case android_emb::ApcfAction::DELETE:
      OnAndroidLEApcfSolicitationUUID16CommandDelete(params);
      break;
    case android_emb::ApcfAction::CLEAR:
      OnAndroidLEApcfSolicitationUUID16CommandClear(params);
      break;
  }
}

void FakeController::OnAndroidLEApcfSolicitationUUID32CommandAdd(
    const android_emb::LEApcfSolicitationUUID32CommandView& params) {
  uint8_t filter_index = params.filter_index().Read();
  if (packet_filter_state_.filters.count(filter_index) == 0) {
    bt_log(WARN,
           "fake-hci",
           "packet filter index (%d) doesn't exist",
           filter_index);
    RespondWithCommandComplete(
        pwemb::OpCode::ANDROID_APCF,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  BufferView uuid(params.uuid().BackingStorage().data(),
                  params.uuid().SizeInBytes());

  PacketFilter* filter = &packet_filter_state_.filters[filter_index];
  filter->solicitation_uuid = UUID(uuid);
  packet_filter_state_.filters_solicitation_uuid[filter_index] = filter;

  uint8_t available_filters =
      packet_filter_state_.max_filters -
      packet_filter_state_.filters_solicitation_uuid.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::SOLICITATION_UUID);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfSolicitationUUID32CommandDelete(
    const android_emb::LEApcfSolicitationUUID32CommandView& params) {
  uint8_t filter_index = params.filter_index().Read();
  if (packet_filter_state_.filters.count(filter_index) == 0) {
    bt_log(WARN,
           "fake-hci",
           "packet filter index (%d) doesn't exist",
           filter_index);
    RespondWithCommandComplete(
        pwemb::OpCode::ANDROID_APCF,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  packet_filter_state_.filters[filter_index].solicitation_uuid.reset();
  packet_filter_state_.filters_solicitation_uuid.erase(filter_index);

  uint8_t available_filters =
      packet_filter_state_.max_filters -
      packet_filter_state_.filters_solicitation_uuid.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::SOLICITATION_UUID);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfSolicitationUUID32CommandClear(
    const android_emb::LEApcfSolicitationUUID32CommandView& params) {
  for (auto [_, filter] : packet_filter_state_.filters_solicitation_uuid) {
    filter->solicitation_uuid.reset();
  }
  packet_filter_state_.filters_solicitation_uuid.clear();

  uint8_t available_filters =
      packet_filter_state_.max_filters -
      packet_filter_state_.filters_solicitation_uuid.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::SOLICITATION_UUID);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfSolicitationUUID32Command(
    const android_emb::LEApcfSolicitationUUID32CommandView& params) {
  android_emb::ApcfAction action = params.action().Read();

  switch (action) {
    case android_emb::ApcfAction::ADD:
      OnAndroidLEApcfSolicitationUUID32CommandAdd(params);
      break;
    case android_emb::ApcfAction::DELETE:
      OnAndroidLEApcfSolicitationUUID32CommandDelete(params);
      break;
    case android_emb::ApcfAction::CLEAR:
      OnAndroidLEApcfSolicitationUUID32CommandClear(params);
      break;
  }
}

void FakeController::OnAndroidLEApcfSolicitationUUID128CommandAdd(
    const android_emb::LEApcfSolicitationUUID128CommandView& params) {
  uint8_t filter_index = params.filter_index().Read();
  if (packet_filter_state_.filters.count(filter_index) == 0) {
    bt_log(WARN,
           "fake-hci",
           "packet filter index (%d) doesn't exist",
           filter_index);
    RespondWithCommandComplete(
        pwemb::OpCode::ANDROID_APCF,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  BufferView uuid(params.uuid().BackingStorage().data(),
                  params.uuid().SizeInBytes());

  PacketFilter* filter = &packet_filter_state_.filters[filter_index];
  filter->solicitation_uuid = UUID(uuid);
  packet_filter_state_.filters_solicitation_uuid[filter_index] = filter;

  uint8_t available_filters =
      packet_filter_state_.max_filters -
      packet_filter_state_.filters_solicitation_uuid.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::SOLICITATION_UUID);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfSolicitationUUID128CommandDelete(
    const android_emb::LEApcfSolicitationUUID128CommandView& params) {
  uint8_t filter_index = params.filter_index().Read();
  if (packet_filter_state_.filters.count(filter_index) == 0) {
    bt_log(WARN,
           "fake-hci",
           "packet filter index (%d) doesn't exist",
           filter_index);
    RespondWithCommandComplete(
        pwemb::OpCode::ANDROID_APCF,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  packet_filter_state_.filters[filter_index].solicitation_uuid.reset();
  packet_filter_state_.filters_solicitation_uuid.erase(filter_index);

  uint8_t available_filters =
      packet_filter_state_.max_filters -
      packet_filter_state_.filters_solicitation_uuid.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::SOLICITATION_UUID);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfSolicitationUUID128CommandClear(
    const android_emb::LEApcfSolicitationUUID128CommandView& params) {
  for (auto [_, filter] : packet_filter_state_.filters_solicitation_uuid) {
    filter->solicitation_uuid.reset();
  }
  packet_filter_state_.filters_solicitation_uuid.clear();

  uint8_t available_filters =
      packet_filter_state_.max_filters -
      packet_filter_state_.filters_solicitation_uuid.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::SOLICITATION_UUID);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfSolicitationUUID128Command(
    const android_emb::LEApcfSolicitationUUID128CommandView& params) {
  android_emb::ApcfAction action = params.action().Read();

  switch (action) {
    case android_emb::ApcfAction::ADD:
      OnAndroidLEApcfSolicitationUUID128CommandAdd(params);
      break;
    case android_emb::ApcfAction::DELETE:
      OnAndroidLEApcfSolicitationUUID128CommandDelete(params);
      break;
    case android_emb::ApcfAction::CLEAR:
      OnAndroidLEApcfSolicitationUUID128CommandClear(params);
      break;
  }
}

void FakeController::OnAndroidLEApcfLocalNameCommandAdd(
    const android_emb::LEApcfLocalNameCommandView& params) {
  uint8_t filter_index = params.filter_index().Read();
  if (packet_filter_state_.filters.count(filter_index) == 0) {
    bt_log(WARN,
           "fake-hci",
           "packet filter index (%d) doesn't exist",
           filter_index);
    RespondWithCommandComplete(
        pwemb::OpCode::ANDROID_APCF,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  PacketFilter* filter = &packet_filter_state_.filters[filter_index];
  BufferView local_name(params.local_name().BackingStorage().data(),
                        params.local_name().SizeInBytes());
  filter->local_name = local_name.AsString();
  packet_filter_state_.filters_local_name[filter_index] = filter;

  uint8_t available_filters = packet_filter_state_.max_filters -
                              packet_filter_state_.filters_local_name.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::LOCAL_NAME);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfLocalNameCommandDelete(
    const android_emb::LEApcfLocalNameCommandView& params) {
  uint8_t filter_index = params.filter_index().Read();
  if (packet_filter_state_.filters.count(filter_index) == 0) {
    bt_log(WARN,
           "fake-hci",
           "packet filter index (%d) doesn't exist",
           filter_index);
    RespondWithCommandComplete(
        pwemb::OpCode::ANDROID_APCF,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  packet_filter_state_.filters[filter_index].local_name.reset();
  packet_filter_state_.filters_local_name.erase(filter_index);

  uint8_t available_filters = packet_filter_state_.max_filters -
                              packet_filter_state_.filters_local_name.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::LOCAL_NAME);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfLocalNameCommandClear(
    const android_emb::LEApcfLocalNameCommandView& params) {
  for (auto [_, filter] : packet_filter_state_.filters_local_name) {
    filter->local_name.reset();
  }
  packet_filter_state_.filters_local_name.clear();

  uint8_t available_filters = packet_filter_state_.max_filters -
                              packet_filter_state_.filters_local_name.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::LOCAL_NAME);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfLocalNameCommand(
    const android_emb::LEApcfLocalNameCommandView& params) {
  android_emb::ApcfAction action = params.action().Read();

  switch (action) {
    case android_emb::ApcfAction::ADD:
      OnAndroidLEApcfLocalNameCommandAdd(params);
      break;
    case android_emb::ApcfAction::DELETE:
      OnAndroidLEApcfLocalNameCommandDelete(params);
      break;
    case android_emb::ApcfAction::CLEAR:
      OnAndroidLEApcfLocalNameCommandClear(params);
      break;
  }
}

void FakeController::OnAndroidLEApcfManufacturerDataCommandAdd(
    const android_emb::LEApcfManufacturerDataCommandView& params) {
  uint8_t filter_index = params.filter_index().Read();
  if (packet_filter_state_.filters.count(filter_index) == 0) {
    bt_log(WARN,
           "fake-hci",
           "packet filter index (%d) doesn't exist",
           filter_index);
    RespondWithCommandComplete(
        pwemb::OpCode::ANDROID_APCF,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  PacketFilter* filter = &packet_filter_state_.filters[filter_index];

  filter->manufacturer_data = std::vector<uint8_t>();
  filter->manufacturer_data->resize(params.manufacturer_data().SizeInBytes());
  std::memcpy(filter->manufacturer_data->data(),
              params.manufacturer_data().BackingStorage().data(),
              filter->manufacturer_data->size());

  filter->manufacturer_data_mask = std::vector<uint8_t>();
  filter->manufacturer_data_mask->reserve(
      params.manufacturer_data_mask().SizeInBytes());
  std::memcpy(filter->manufacturer_data_mask->data(),
              params.manufacturer_data_mask().BackingStorage().data(),
              filter->manufacturer_data_mask->size());

  uint8_t available_filters =
      packet_filter_state_.max_filters -
      packet_filter_state_.filters_manufacturer_data.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::MANUFACTURER_DATA);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfManufacturerDataCommandDelete(
    const android_emb::LEApcfManufacturerDataCommandView& params) {
  uint8_t filter_index = params.filter_index().Read();
  if (packet_filter_state_.filters.count(filter_index) == 0) {
    bt_log(WARN,
           "fake-hci",
           "packet filter index (%d) doesn't exist",
           filter_index);
    RespondWithCommandComplete(
        pwemb::OpCode::ANDROID_APCF,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  PacketFilter& filter = packet_filter_state_.filters[filter_index];
  filter.manufacturer_data.reset();
  filter.manufacturer_data_mask.reset();
  packet_filter_state_.filters_manufacturer_data.erase(filter_index);

  uint8_t available_filters =
      packet_filter_state_.max_filters -
      packet_filter_state_.filters_manufacturer_data.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::MANUFACTURER_DATA);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfManufacturerDataCommandClear(
    const android_emb::LEApcfManufacturerDataCommandView& params) {
  for (auto [_, filter] : packet_filter_state_.filters_manufacturer_data) {
    filter->manufacturer_data.reset();
    filter->manufacturer_data_mask.reset();
  }
  packet_filter_state_.filters_manufacturer_data.clear();

  uint8_t available_filters =
      packet_filter_state_.max_filters -
      packet_filter_state_.filters_manufacturer_data.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::MANUFACTURER_DATA);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfManufacturerDataCommand(
    const android_emb::LEApcfManufacturerDataCommandView& params) {
  android_emb::ApcfAction action = params.action().Read();

  switch (action) {
    case android_emb::ApcfAction::ADD:
      OnAndroidLEApcfManufacturerDataCommandAdd(params);
      break;
    case android_emb::ApcfAction::DELETE:
      OnAndroidLEApcfManufacturerDataCommandDelete(params);
      break;
    case android_emb::ApcfAction::CLEAR:
      OnAndroidLEApcfManufacturerDataCommandClear(params);
      break;
  }
}

void FakeController::OnAndroidLEApcfServiceDataCommandAdd(
    const android_emb::LEApcfServiceDataCommandView& params) {
  uint8_t filter_index = params.filter_index().Read();
  if (packet_filter_state_.filters.count(filter_index) == 0) {
    bt_log(WARN,
           "fake-hci",
           "packet filter index (%d) doesn't exist",
           filter_index);
    RespondWithCommandComplete(
        pwemb::OpCode::ANDROID_APCF,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  PacketFilter* filter = &packet_filter_state_.filters[filter_index];

  filter->service_data = std::vector<uint8_t>();
  filter->service_data->resize(params.service_data().SizeInBytes());
  std::memcpy(filter->service_data->data(),
              params.service_data().BackingStorage().data(),
              filter->service_data->size());

  filter->service_data_mask = std::vector<uint8_t>();
  filter->service_data_mask->reserve(params.service_data_mask().SizeInBytes());
  std::memcpy(filter->service_data_mask->data(),
              params.service_data_mask().BackingStorage().data(),
              filter->service_data_mask->size());

  uint8_t available_filters = packet_filter_state_.max_filters -
                              packet_filter_state_.filters_service_data.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::SERVICE_DATA);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfServiceDataCommandDelete(
    const android_emb::LEApcfServiceDataCommandView& params) {
  uint8_t filter_index = params.filter_index().Read();
  if (packet_filter_state_.filters.count(filter_index) == 0) {
    bt_log(WARN,
           "fake-hci",
           "packet filter index (%d) doesn't exist",
           filter_index);
    RespondWithCommandComplete(
        pwemb::OpCode::ANDROID_APCF,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  PacketFilter& filter = packet_filter_state_.filters[filter_index];
  filter.service_data.reset();
  filter.service_data_mask.reset();
  packet_filter_state_.filters_manufacturer_data.erase(filter_index);

  uint8_t available_filters = packet_filter_state_.max_filters -
                              packet_filter_state_.filters_service_data.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::SERVICE_DATA);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfServiceDataCommandClear(
    const android_emb::LEApcfServiceDataCommandView& params) {
  for (auto [_, filter] : packet_filter_state_.filters_manufacturer_data) {
    filter->service_data.reset();
    filter->service_data_mask.reset();
  }
  packet_filter_state_.filters_service_data.clear();

  uint8_t available_filters = packet_filter_state_.max_filters -
                              packet_filter_state_.filters_service_data.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::SERVICE_DATA);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfServiceDataCommand(
    const android_emb::LEApcfServiceDataCommandView& params) {
  android_emb::ApcfAction action = params.action().Read();

  switch (action) {
    case android_emb::ApcfAction::ADD:
      OnAndroidLEApcfServiceDataCommandAdd(params);
      break;
    case android_emb::ApcfAction::DELETE:
      OnAndroidLEApcfServiceDataCommandDelete(params);
      break;
    case android_emb::ApcfAction::CLEAR:
      OnAndroidLEApcfServiceDataCommandClear(params);
      break;
  }
}

void FakeController::OnAndroidLEApcfAdTypeCommandAdd(
    const android_emb::LEApcfAdTypeCommandView& params) {
  uint8_t filter_index = params.filter_index().Read();
  if (packet_filter_state_.filters.count(filter_index) == 0) {
    bt_log(WARN,
           "fake-hci",
           "packet filter index (%d) doesn't exist",
           filter_index);
    RespondWithCommandComplete(
        pwemb::OpCode::ANDROID_APCF,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  PacketFilter* filter = &packet_filter_state_.filters[filter_index];
  filter->advertising_data_type = params.ad_type().Read();

  filter->advertising_data = std::vector<uint8_t>();
  filter->advertising_data->reserve(params.ad_data().SizeInBytes());
  std::memcpy(filter->advertising_data->data(),
              params.ad_data().BackingStorage().data(),
              filter->advertising_data->size());

  filter->advertising_data_mask = std::vector<uint8_t>();
  filter->advertising_data_mask->reserve(params.ad_data_mask().SizeInBytes());
  std::memcpy(filter->advertising_data_mask->data(),
              params.ad_data_mask().BackingStorage().data(),
              filter->advertising_data_mask->size());

  uint8_t available_filters =
      packet_filter_state_.max_filters -
      packet_filter_state_.filters_advertising_data.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::AD_TYPE_FILTER);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfAdTypeCommandDelete(
    const android_emb::LEApcfAdTypeCommandView& params) {
  uint8_t filter_index = params.filter_index().Read();
  if (packet_filter_state_.filters.count(filter_index) == 0) {
    bt_log(WARN,
           "fake-hci",
           "packet filter index (%d) doesn't exist",
           filter_index);
    RespondWithCommandComplete(
        pwemb::OpCode::ANDROID_APCF,
        pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
    return;
  }

  PacketFilter& filter = packet_filter_state_.filters[filter_index];
  filter.advertising_data_type.reset();
  filter.advertising_data.reset();
  filter.advertising_data_mask.reset();
  packet_filter_state_.filters_advertising_data.erase(filter_index);

  uint8_t available_filters =
      packet_filter_state_.max_filters -
      packet_filter_state_.filters_advertising_data.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::AD_TYPE_FILTER);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfAdTypeCommandClear(
    const android_emb::LEApcfAdTypeCommandView& params) {
  for (auto [_, filter] : packet_filter_state_.filters_advertising_data) {
    filter->advertising_data_type.reset();
    filter->advertising_data.reset();
    filter->advertising_data_mask.reset();
  }
  packet_filter_state_.filters_advertising_data.clear();

  uint8_t available_filters =
      packet_filter_state_.max_filters -
      packet_filter_state_.filters_advertising_data.size();
  auto packet =
      hci::EventPacket::New<android_emb::LEApcfCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::ApcfSubOpcode::AD_TYPE_FILTER);
  view.action().Write(params.action().Read());
  view.available_spaces().Write(available_filters);
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF, &packet);
}

void FakeController::OnAndroidLEApcfAdTypeCommand(
    const android_emb::LEApcfAdTypeCommandView& params) {
  android_emb::ApcfAction action = params.action().Read();

  switch (action) {
    case android_emb::ApcfAction::ADD:
      OnAndroidLEApcfAdTypeCommandAdd(params);
      break;
    case android_emb::ApcfAction::DELETE:
      OnAndroidLEApcfAdTypeCommandDelete(params);
      break;
    case android_emb::ApcfAction::CLEAR:
      OnAndroidLEApcfAdTypeCommandClear(params);
      break;
  }
}

void FakeController::OnAndroidLEApcfCommand(
    const PacketView<hci_spec::CommandHeader>& command_packet) {
  const auto& payload = command_packet.payload_data();

  uint8_t subopcode = payload.To<uint8_t>();
  switch (subopcode) {
    case android_hci::kLEApcfEnableSubopcode: {
      auto params = android_emb::MakeLEApcfEnableCommandView(
          command_packet.data().data(), command_packet.size());
      OnAndroidLEApcfEnableCommand(params);
      break;
    }
    case android_hci::kLEApcfSetFilteringParametersSubopcode: {
      auto params = android_emb::MakeLEApcfSetFilteringParametersCommandView(
          command_packet.data().data(), command_packet.size());
      OnAndroidLEApcfSetFilteringParametersCommand(params);
      break;
    }
    case android_hci::kLEApcfBroadcastAddressSubopcode: {
      auto params = android_emb::MakeLEApcfBroadcastAddressCommandView(
          command_packet.data().data(), command_packet.size());
      OnAndroidLEApcfBroadcastAddressCommand(params);
      break;
    }
    case android_hci::kLEApcfServiceUUIDSubopcode: {
      size_t size = command_packet.size();
      if (size == android_emb::LEApcfServiceUUID16Command::MaxSizeInBytes()) {
        auto params = android_emb::MakeLEApcfServiceUUID16CommandView(
            command_packet.data().data(), command_packet.size());
        OnAndroidLEApcfServiceUUID16Command(params);
      } else if (size ==
                 android_emb::LEApcfServiceUUID32Command::MaxSizeInBytes()) {
        auto params = android_emb::MakeLEApcfServiceUUID32CommandView(
            command_packet.data().data(), command_packet.size());
        OnAndroidLEApcfServiceUUID32Command(params);
      } else if (size ==
                 android_emb::LEApcfServiceUUID128Command::MaxSizeInBytes()) {
        auto params = android_emb::MakeLEApcfServiceUUID128CommandView(
            command_packet.data().data(), command_packet.size());
        OnAndroidLEApcfServiceUUID128Command(params);
      } else {
        bt_log(
            WARN,
            "fake-hci",
            "unhandled android packet filter command (service uuid), size: %zu",
            size);
        RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF,
                                   pwemb::StatusCode::COMMAND_DISALLOWED);
      }
      break;
    }
    case android_hci::kLEApcfServiceSolicitationUUIDSubopcode: {
      size_t size = command_packet.size();
      if (size ==
          android_emb::LEApcfSolicitationUUID16Command::MaxSizeInBytes()) {
        auto params = android_emb::MakeLEApcfSolicitationUUID16CommandView(
            command_packet.data().data(), command_packet.size());
        OnAndroidLEApcfSolicitationUUID16Command(params);
      } else if (size == android_emb::LEApcfSolicitationUUID32Command::
                             MaxSizeInBytes()) {
        auto params = android_emb::MakeLEApcfSolicitationUUID32CommandView(
            command_packet.data().data(), command_packet.size());
        OnAndroidLEApcfSolicitationUUID32Command(params);
      } else if (size == android_emb::LEApcfSolicitationUUID128Command::
                             MaxSizeInBytes()) {
        auto params = android_emb::MakeLEApcfSolicitationUUID128CommandView(
            command_packet.data().data(), command_packet.size());
        OnAndroidLEApcfSolicitationUUID128Command(params);
      } else {
        bt_log(WARN,
               "fake-hci",
               "unhandled android packet filter command (solicitation uuid), "
               "size: %zu",
               size);
        RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF,
                                   pwemb::StatusCode::COMMAND_DISALLOWED);
      }
      break;
    }
    case android_hci::kLEApcfLocalNameSubopcode: {
      size_t data_length =
          command_packet.size() -
          android_emb::LEApcfLocalNameCommand::MinSizeInBytes();
      auto params = android_emb::MakeLEApcfLocalNameCommandView(
          data_length, command_packet.data().data(), command_packet.size());
      OnAndroidLEApcfLocalNameCommand(params);
      break;
    }
    case android_hci::kLEApcfManufacturerDataSubopcode: {
      size_t data_length =
          (command_packet.size() -
           android_emb::LEApcfManufacturerDataCommand::MinSizeInBytes()) /
          2;
      auto params = android_emb::MakeLEApcfManufacturerDataCommandView(
          data_length, command_packet.data().data(), command_packet.size());
      OnAndroidLEApcfManufacturerDataCommand(params);
      break;
    }
    case android_hci::kLEApcfServiceDataSubopcode: {
      size_t data_length =
          (command_packet.size() -
           android_emb::LEApcfServiceDataCommand::MinSizeInBytes()) /
          2;
      auto params = android_emb::MakeLEApcfServiceDataCommandView(
          data_length, command_packet.data().data(), command_packet.size());
      OnAndroidLEApcfServiceDataCommand(params);
      break;
    }
    case android_hci::kLEApcfAdTypeFilter: {
      auto params = android_emb::MakeLEApcfAdTypeCommandView(
          command_packet.data().data(), command_packet.size());
      OnAndroidLEApcfAdTypeCommand(params);
      break;
    }
    default: {
      bt_log(WARN,
             "fake-hci",
             "unhandled android packet filter command, subopcode: %#.4x",
             subopcode);
      RespondWithCommandComplete(pwemb::OpCode::ANDROID_APCF,
                                 pwemb::StatusCode::UNKNOWN_COMMAND);
      break;
    }
  }
}

void FakeController::OnAndroidLEBatchScanEnableCommand(
    const android_emb::LEBatchScanEnableCommandView& params) {
  auto packet =
      hci::EventPacket::New<android_emb::LEBatchScanCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::BatchScanSubOpcode::ENABLE);

  if (!EnableVendorBatchScanning()) {
    bt_log(INFO,
           "fake-hci",
           "vendor batch scanning command rejected, another scan type in use");
    view.status().Write(pwemb::StatusCode::COMMAND_DISALLOWED);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_BATCH_SCAN, &packet);
    return;
  }

  if (params.enabled().Read() == pwemb::GenericEnableParam::ENABLE) {
    le_scan_state_.batch_scan_enabled = true;
  } else {
    le_scan_state_.batch_scan_enabled = false;
  }

  le_scan_state_.batch_scan_num_peers_read = 0;
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_BATCH_SCAN, &packet);
}

void FakeController::OnAndroidLEBatchScanSetStorageParametersCommand(
    [[maybe_unused]] const android_emb::
        LEBatchScanSetStorageParametersCommandView& params) {
  auto packet =
      hci::EventPacket::New<android_emb::LEBatchScanCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(
      android_emb::BatchScanSubOpcode::SET_STORAGE_PARAMETERS);

  if (!EnableVendorBatchScanning()) {
    bt_log(INFO,
           "fake-hci",
           "vendor batch scanning command rejected, another scan type in use");
    view.status().Write(pwemb::StatusCode::COMMAND_DISALLOWED);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_BATCH_SCAN, &packet);
    return;
  }

  // For testing purposes, we don't store storage threshold configurations for
  // truncated and full format batched scan results or the storage threshold to
  // send the results back to the Host. Rather, the tests themselves can
  // simulate these conditions by calling trigger functions on the
  // FakeController itself.
  if (!le_scan_state_.batch_scan_enabled) {
    bt_log(WARN, "fake-hci", "Cannot set storage parameters without enabling");
    view.status().Write(pwemb::StatusCode::COMMAND_DISALLOWED);
  }

  le_scan_state_.batch_scan_num_peers_read = 0;
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_BATCH_SCAN, &packet);
}

void FakeController::OnAndroidLEBatchScanSetScanParametersCommand(
    const android_emb::LEBatchScanSetScanParametersCommandView& params) {
  auto packet =
      hci::EventPacket::New<android_emb::LEBatchScanCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.sub_opcode().Write(android_emb::BatchScanSubOpcode::SET_SCAN_PARAMETERS);

  if (!EnableVendorBatchScanning()) {
    bt_log(INFO,
           "fake-hci",
           "vendor batch scanning command rejected, another scan type in use");
    view.status().Write(pwemb::StatusCode::COMMAND_DISALLOWED);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_BATCH_SCAN, &packet);
    return;
  }

  if (!le_scan_state_.batch_scan_enabled) {
    bt_log(WARN, "fake-hci", "Cannot set scan parameters without enabling");
    view.status().Write(pwemb::StatusCode::COMMAND_DISALLOWED);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_BATCH_SCAN, &packet);
    return;
  }

  // Sapphire doesn't support truncated scan mode because it doesn't contain the
  // advertising data or scan response data. Disallow our code to try and enable
  // only truncated mode.
  if (params.full_mode_enabled().Read()) {
    le_scan_state_.enabled = true;
  } else {
    if (params.truncated_mode_enabled().Read()) {
      bt_log(WARN, "fake-hci", "Cannot enable only batch scan truncated mode");
      view.status().Write(pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS);
      RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_BATCH_SCAN, &packet);
      return;
    } else {
      le_scan_state_.enabled = false;
    }
  }

  le_scan_state_.scan_window = params.window().Read();
  le_scan_state_.scan_interval = params.interval().Read();
  le_scan_state_.batch_scan_num_peers_read = 0;

  if (params.own_address_type().Read() ==
      android_emb::BatchScanOwnAddressType::PUBLIC) {
    le_scan_state_.own_address_type = pwemb::LEOwnAddressType::PUBLIC;
  } else if (params.own_address_type().Read() ==
             android_emb::BatchScanOwnAddressType::RANDOM) {
    le_scan_state_.own_address_type = pwemb::LEOwnAddressType::RANDOM;
  }

  // For testing purposes, we don't store the discard rule. The tests themselves
  // can simulate these conditions by calling trigger functions on the
  // FakeController itself.
  RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_BATCH_SCAN, &packet);
}

auto FakeController::LEBatchScanReadResultNextPacketSize() const {
  struct result {
    int num_records;
    size_t packet_size;
  };

  uint8_t num_records = 0;
  size_t max_hci_packet_size = std::numeric_limits<uint8_t>::max();
  size_t packet_size =
      android_emb::LEBatchScanReadResultsCommandCompleteEvent::MinSizeInBytes();

  auto itr = peers_.cbegin();
  std::advance(itr, le_scan_state_.batch_scan_num_peers_read);
  for (; itr != peers_.cend(); ++itr) {
    const std::unique_ptr<FakePeer>& peer = itr->second;
    size_t full_result_size =
        android_emb::LEBatchScanFullResult::MinSizeInBytes() +
        peer->advertising_data().size() + peer->scan_response().size();

    if (packet_size + full_result_size > max_hci_packet_size) {
      break;
    }

    num_records++;
    packet_size += full_result_size;
  }

  return result{num_records, packet_size};
}

void FakeController::LEBatchScanFillFullResult(
    android_emb::LEBatchScanFullResultWriter& full_result,
    const std::unique_ptr<FakePeer>& peer) const {
  full_result.peer_address().bd_addr().CopyFrom(
      peer->address().value().view().bd_addr());

  if (peer->address().type() == DeviceAddress::Type::kLERandom) {
    full_result.peer_address_type().Write(pwemb::LEAddressType::RANDOM);
  } else {
    full_result.peer_address_type().Write(pwemb::LEAddressType::PUBLIC);
  }

  full_result.tx_power().Write(peer->tx_power());
  full_result.rssi().Write(peer->rssi());

  full_result.advertising_data_length().Write(peer->advertising_data().size());
  if (!peer->advertising_data().empty()) {
    std::memcpy(full_result.advertising_data().BackingStorage().begin(),
                peer->advertising_data().data(),
                peer->advertising_data().size());
  }

  full_result.scan_response_length().Write(peer->scan_response().size());
  if (!peer->scan_response().empty()) {
    std::memcpy(full_result.scan_response_data().BackingStorage().begin(),
                peer->scan_response().data(),
                peer->scan_response().size());
  }
}

void FakeController::OnAndroidLEBatchScanReadResultsCommand(
    const android_emb::LEBatchScanReadResultsCommandView& params) {
  auto regular_packet =
      hci::EventPacket::New<android_emb::LEBatchScanCommandCompleteEventWriter>(
          hci_spec::kCommandCompleteEventCode);
  auto regular_view = regular_packet.view_t();
  regular_view.sub_opcode().Write(
      android_emb::BatchScanSubOpcode::READ_RESULT_PARAMETERS);

  if (!EnableVendorBatchScanning()) {
    bt_log(INFO,
           "fake-hci",
           "vendor batch scanning command rejected, another scan type in use");
    regular_view.status().Write(pwemb::StatusCode::COMMAND_DISALLOWED);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_BATCH_SCAN,
                               &regular_packet);
    return;
  }

  if (!le_scan_state_.batch_scan_enabled) {
    bt_log(WARN, "fake-hci", "Cannot read batch scan results without enabling");
    regular_view.status().Write(pwemb::StatusCode::COMMAND_DISALLOWED);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_BATCH_SCAN,
                               &regular_packet);
    return;
  }

  // Sapphire doesn't support truncated scan mode because it doesn't contain
  // the advertising data or scan response data. Disallow our code to try and
  // enable only truncated mode.
  if (params.read_mode().Read() == android_emb::BatchScanReadMode::TRUNCATED) {
    bt_log(WARN, "fake-hci", "Cannot read truncated batch scan results");
    regular_view.status().Write(pwemb::StatusCode::COMMAND_DISALLOWED);
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_BATCH_SCAN,
                               &regular_packet);
    return;
  }

  auto [num_records, packet_size] = LEBatchScanReadResultNextPacketSize();

  auto packet = hci::EventPacket::New<
      android_emb::LEBatchScanReadResultsCommandCompleteEventWriter>(
      hci_spec::kCommandCompleteEventCode, packet_size);
  auto view = packet.view_t();
  view.status().Write(pwemb::StatusCode::SUCCESS);
  view.sub_opcode().Write(
      android_emb::BatchScanSubOpcode::READ_RESULT_PARAMETERS);
  view.read_mode().Write(android_emb::BatchScanReadMode::FULL);
  view.num_records().Write(num_records);

  if (num_records == 0) {
    RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_BATCH_SCAN, &packet);
    return;
  }

  size_t write_offset = 0;
  int start_num_peers_read = le_scan_state_.batch_scan_num_peers_read;
  auto itr = peers_.cbegin();
  for (; itr != peers_.cend(); ++itr) {
    if (start_num_peers_read + num_records <=
        le_scan_state_.batch_scan_num_peers_read) {
      break;
    }

    le_scan_state_.batch_scan_num_peers_read++;

    const std::unique_ptr<FakePeer>& peer = itr->second;
    size_t full_result_size =
        android_emb::LEBatchScanFullResult::MinSizeInBytes() +
        peer->advertising_data().size() + peer->scan_response().size();

    android_emb::LEBatchScanFullResultWriter full_result(
        view.full_results().BackingStorage().begin() + write_offset,
        full_result_size);

    LEBatchScanFillFullResult(full_result, peer);
    write_offset += full_result_size;
  }

  RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_BATCH_SCAN, &packet);
}

void FakeController::OnAndroidLEBatchScanCommand(
    const PacketView<hci_spec::CommandHeader>& command_packet) {
  const auto& payload = command_packet.payload_data();

  uint8_t subopcode = payload.To<uint8_t>();
  switch (subopcode) {
    case android_hci::kLEBatchScanEnableSubopcode: {
      auto params = android_emb::MakeLEBatchScanEnableCommandView(
          command_packet.data().data(), command_packet.size());
      OnAndroidLEBatchScanEnableCommand(params);
      break;
    }
    case android_hci::kLEBatchScanSetStorageParametersSubopcode: {
      auto params = android_emb::MakeLEBatchScanSetStorageParametersCommandView(
          command_packet.data().data(), command_packet.size());
      OnAndroidLEBatchScanSetStorageParametersCommand(params);
      break;
    }
    case android_hci::kLEBatchScanSetScanParametersSubopcode: {
      auto params = android_emb::MakeLEBatchScanSetScanParametersCommandView(
          command_packet.data().data(), command_packet.size());
      OnAndroidLEBatchScanSetScanParametersCommand(params);
      break;
    }
    case android_hci::kLEBatchScanReadResultParametersSubopcode: {
      auto params = android_emb::MakeLEBatchScanReadResultsCommandView(
          command_packet.data().data(), command_packet.size());
      OnAndroidLEBatchScanReadResultsCommand(params);
      break;
    }
    default: {
      bt_log(WARN,
             "fake-hci",
             "unhandled android batch scan command, subopcode: %#.4x",
             subopcode);
      RespondWithCommandComplete(pwemb::OpCode::ANDROID_LE_BATCH_SCAN,
                                 pwemb::StatusCode::UNKNOWN_COMMAND);
      break;
    }
  }
}

void FakeController::OnVendorCommand(
    const PacketView<hci_spec::CommandHeader>& command_packet) {
  auto opcode = pw::bytes::ConvertOrderFrom(cpp20::endian::little,
                                            command_packet.header().opcode);

  switch (opcode) {
    case android_hci::kLEGetVendorCapabilities:
      OnAndroidLEGetVendorCapabilities();
      break;
    case android_hci::kA2dpOffloadCommand:
      OnAndroidA2dpOffloadCommand(command_packet);
      break;
    case android_hci::kLEMultiAdvt:
      OnAndroidLEMultiAdvt(command_packet);
      break;
    case android_hci::kLEApcf:
      OnAndroidLEApcfCommand(command_packet);
      break;
    case android_hci::kLEBatchScan:
      OnAndroidLEBatchScanCommand(command_packet);
      break;
    default:
      bt_log(WARN,
             "fake-hci",
             "received unhandled vendor command with opcode: %#.4x",
             opcode);
      RespondWithCommandComplete(static_cast<pwemb::OpCode>(opcode),
                                 pwemb::StatusCode::UNKNOWN_COMMAND);
      break;
  }
}

void FakeController::OnACLDataPacketReceived(
    const ByteBuffer& acl_data_packet) {
  if (acl_data_callback_) {
    PW_DCHECK(data_dispatcher_);
    DynamicByteBuffer packet_copy(acl_data_packet);
    (void)data_dispatcher_->Post(
        [packet_copy = std::move(packet_copy), cb = acl_data_callback_.share()](
            pw::async::Context /*ctx*/, pw::Status status) mutable {
          if (status.ok()) {
            cb(packet_copy);
          }
        });
  }

  if (acl_data_packet.size() < sizeof(hci_spec::ACLDataHeader)) {
    bt_log(WARN, "fake-hci", "malformed ACL packet!");
    return;
  }

  const auto& header = acl_data_packet.To<hci_spec::ACLDataHeader>();
  hci_spec::ConnectionHandle handle =
      pw::bytes::ConvertOrderFrom(cpp20::endian::little,
                                  header.handle_and_flags) &
      0x0FFFF;
  FakePeer* peer = FindByConnHandle(handle);
  if (!peer) {
    bt_log(WARN, "fake-hci", "ACL data received for unknown handle!");
    return;
  }

  if (auto_completed_packets_event_enabled_) {
    SendNumberOfCompletedPacketsEvent(handle, 1);
  }
  peer->OnRxL2CAP(handle,
                  acl_data_packet.view(sizeof(hci_spec::ACLDataHeader)));
}

void FakeController::OnScoDataPacketReceived(
    const ByteBuffer& sco_data_packet) {
  if (sco_data_callback_) {
    sco_data_callback_(sco_data_packet);
  }

  if (sco_data_packet.size() < sizeof(hci_spec::SynchronousDataHeader)) {
    bt_log(WARN, "fake-hci", "malformed SCO packet!");
    return;
  }

  const auto& header = sco_data_packet.To<hci_spec::SynchronousDataHeader>();
  hci_spec::ConnectionHandle handle =
      pw::bytes::ConvertOrderFrom(cpp20::endian::little,
                                  header.handle_and_flags) &
      0x0FFFF;
  FakePeer* peer = FindByConnHandle(handle);
  if (!peer) {
    bt_log(WARN, "fake-hci", "SCO data received for unknown handle!");
    return;
  }

  if (auto_completed_packets_event_enabled_) {
    SendNumberOfCompletedPacketsEvent(handle, 1);
  }
}

void FakeController::OnIsoDataPacketReceived(
    const ByteBuffer& iso_data_packet) {
  if (iso_data_callback_) {
    iso_data_callback_(iso_data_packet);
  }

  if (iso_data_packet.size() < pwemb::IsoDataFrameHeader::MinSizeInBytes()) {
    bt_log(WARN, "fake-hci", "malformed ISO packet!");
    return;
  }

  auto iso_header_view = pwemb::MakeIsoDataFrameHeaderView(
      iso_data_packet.data(), iso_data_packet.size());
  hci_spec::ConnectionHandle handle =
      iso_header_view.connection_handle().Read();

  if (auto_completed_packets_event_enabled_) {
    SendNumberOfCompletedPacketsEvent(handle, 1);
  }
}

void FakeController::SetDataCallback(DataCallback callback,
                                     pw::async::Dispatcher& pw_dispatcher) {
  PW_DCHECK(callback);
  PW_DCHECK(!acl_data_callback_);
  PW_DCHECK(!data_dispatcher_);

  acl_data_callback_ = std::move(callback);
  data_dispatcher_.emplace(pw_dispatcher);
}

void FakeController::ClearDataCallback() {
  // Leave dispatcher set (if already set) to preserve its write-once-ness
  // (this catches bugs with setting multiple data callbacks in class
  // hierarchies).
  acl_data_callback_ = nullptr;
}

bool FakeController::LEAdvertisingState::IsDirectedAdvertising() const {
  return properties.directed || properties.high_duty_cycle_directed_connectable;
}

bool FakeController::EnableLegacyAdvertising() {
  if (advertising_procedure() != ExtendedOperationType::kLegacy &&
      advertising_procedure() != ExtendedOperationType::kUnknown) {
    return false;
  }

  advertising_procedure_ = ExtendedOperationType::kLegacy;
  return true;
}

bool FakeController::EnableExtendedAdvertising() {
  if (advertising_procedure() != ExtendedOperationType::kExtended &&
      advertising_procedure() != ExtendedOperationType::kUnknown) {
    return false;
  }

  advertising_procedure_ = ExtendedOperationType::kExtended;
  return true;
}

bool FakeController::EnableVendorAdvertising() {
  if (advertising_procedure() != ExtendedOperationType::kVendor &&
      advertising_procedure() != ExtendedOperationType::kUnknown) {
    return false;
  }

  advertising_procedure_ = ExtendedOperationType::kVendor;
  return true;
}

bool FakeController::EnableLegacyScanning() {
  if (scan_procedure() != ExtendedOperationType::kLegacy &&
      scan_procedure() != ExtendedOperationType::kUnknown) {
    return false;
  }

  scan_procedure_ = ExtendedOperationType::kLegacy;
  return true;
}

bool FakeController::EnableExtendedScanning() {
  if (scan_procedure() != ExtendedOperationType::kExtended &&
      scan_procedure() != ExtendedOperationType::kUnknown) {
    return false;
  }

  scan_procedure_ = ExtendedOperationType::kExtended;
  return true;
}

bool FakeController::EnableVendorBatchScanning() {
  if (scan_procedure() != ExtendedOperationType::kVendor &&
      scan_procedure() != ExtendedOperationType::kUnknown) {
    return false;
  }

  scan_procedure_ = ExtendedOperationType::kVendor;
  return true;
}

void FakeController::HandleReceivedCommandPacket(
    const PacketView<hci_spec::CommandHeader>& command_packet) {
  hci_spec::OpCode opcode = pw::bytes::ConvertOrderFrom(
      cpp20::endian::little, command_packet.header().opcode);

  if (MaybeRespondWithDefaultCommandStatus(opcode)) {
    return;
  }

  if (MaybeRespondWithDefaultStatus(opcode)) {
    return;
  }

  auto ogf = hci_spec::GetOGF(opcode);
  if (ogf == hci_spec::kVendorOGF) {
    OnVendorCommand(command_packet);
    return;
  }

  // TODO(fxbug.dev/42175513): Validate size of payload to be the correct
  // length below.
  switch (opcode) {
    case hci_spec::kReadLocalVersionInfo: {
      OnReadLocalVersionInfo();
      break;
    }
    case hci_spec::kReadLocalSupportedCommands: {
      OnReadLocalSupportedCommands();
      break;
    }
    case hci_spec::kReadLocalSupportedFeatures: {
      OnReadLocalSupportedFeatures();
      break;
    }
    case hci_spec::kReadBDADDR: {
      OnReadBRADDR();
      break;
    }
    case hci_spec::kReadBufferSize: {
      OnReadBufferSize();
      break;
    }
    case hci_spec::kCreateConnectionCancel: {
      OnCreateConnectionCancel();
      break;
    }
    case hci_spec::kReadLocalName: {
      OnReadLocalName();
      break;
    }
    case hci_spec::kReadScanEnable: {
      OnReadScanEnable();
      break;
    }
    case hci_spec::kReadPageScanActivity: {
      OnReadPageScanActivity();
      break;
    }
    case hci_spec::kReadInquiryMode: {
      OnReadInquiryMode();
      break;
    }
    case hci_spec::kReadPageScanType: {
      OnReadPageScanType();
      break;
    }
    case hci_spec::kReadSimplePairingMode: {
      OnReadSimplePairingMode();
      break;
    }
    case hci_spec::kLECreateConnectionCancel: {
      OnLECreateConnectionCancel();
      break;
    }
    case hci_spec::kLEReadLocalSupportedFeatures: {
      OnLEReadLocalSupportedFeatures();
      break;
    }
    case hci_spec::kLEReadSupportedStates: {
      OnLEReadSupportedStates();
      break;
    }
    case hci_spec::kLEReadBufferSizeV1: {
      OnLEReadBufferSizeV1();
      break;
    }
    case hci_spec::kLEReadBufferSizeV2: {
      OnLEReadBufferSizeV2();
      break;
    }
    case hci_spec::kReset: {
      OnReset();
      break;
    }
    case hci_spec::kLinkKeyRequestReply: {
      const auto& params =
          command_packet.payload<pwemb::LinkKeyRequestReplyCommandView>();
      OnLinkKeyRequestReplyCommandReceived(params);
      break;
    }
    case hci_spec::kLEReadAdvertisingChannelTxPower: {
      OnLEReadAdvertisingChannelTxPower();
      break;
    }
    case hci_spec::kAuthenticationRequested:
    case hci_spec::kCreateConnection:
    case hci_spec::kDisconnect:
    case hci_spec::kEnhancedAcceptSynchronousConnectionRequest:
    case hci_spec::kEnhancedSetupSynchronousConnection:
    case hci_spec::kIOCapabilityRequestReply:
    case hci_spec::kInquiry:
    case hci_spec::kLEClearAdvertisingSets:
    case hci_spec::kLEConnectionUpdate:
    case hci_spec::kLECreateConnection:
    case hci_spec::kLEExtendedCreateConnection:
    case static_cast<uint16_t>(
        pwemb::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC):
    case static_cast<uint16_t>(
        pwemb::OpCode::LE_PERIODIC_ADVERTISING_TERMINATE_SYNC):
    case static_cast<uint16_t>(
        pwemb::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST):
    case static_cast<uint16_t>(
        pwemb::OpCode::LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST):
    case hci_spec::kLEReadMaximumAdvertisingDataLength:
    case hci_spec::kLEReadNumSupportedAdvertisingSets:
    case hci_spec::kLEReadRemoteFeatures:
    case hci_spec::kLERemoveAdvertisingSet:
    case hci_spec::kLESetAdvertisingData:
    case hci_spec::kLESetAdvertisingEnable:
    case hci_spec::kLESetAdvertisingParameters:
    case hci_spec::kLESetAdvertisingSetRandomAddress:
    case hci_spec::kLESetEventMask:
    case hci_spec::kLESetExtendedAdvertisingData:
    case hci_spec::kLESetExtendedAdvertisingEnable:
    case hci_spec::kLESetExtendedAdvertisingParameters:
    case hci_spec::kLESetExtendedScanEnable:
    case hci_spec::kLESetExtendedScanParameters:
    case hci_spec::kLESetExtendedScanResponseData:
    case hci_spec::kLESetHostFeature:
    case hci_spec::kLESetRandomAddress:
    case hci_spec::kLESetScanEnable:
    case hci_spec::kLESetScanParameters:
    case hci_spec::kLESetScanResponseData:
    case hci_spec::kLEStartEncryption:
    case hci_spec::kLERejectCISRequest:
    case hci_spec::kLinkKeyRequestNegativeReply:
    case hci_spec::kReadEncryptionKeySize:
    case hci_spec::kReadLocalExtendedFeatures:
    case hci_spec::kReadLocalSupportedControllerDelay:
    case hci_spec::kReadRemoteExtendedFeatures:
    case hci_spec::kReadRemoteSupportedFeatures:
    case hci_spec::kReadRemoteVersionInfo:
    case hci_spec::kRemoteNameRequest:
    case hci_spec::kSetConnectionEncryption:
    case hci_spec::kSetEventMask:
    case hci_spec::kUserConfirmationRequestNegativeReply:
    case hci_spec::kUserConfirmationRequestReply:
    case hci_spec::kWriteClassOfDevice:
    case hci_spec::kWriteExtendedInquiryResponse:
    case hci_spec::kWriteInquiryMode:
    case hci_spec::kWriteLEHostSupport:
    case hci_spec::kWriteLocalName:
    case hci_spec::kWritePageScanActivity:
    case hci_spec::kWritePageScanType:
    case hci_spec::kWriteScanEnable:
    case hci_spec::kWriteSecureConnectionsHostSupport:
    case hci_spec::kWriteSimplePairingMode:
    case hci_spec::kWriteSynchronousFlowControlEnable: {
      // This case is for packet types that have been migrated to the new
      // Emboss architecture. Their old version can be still be assembled from
      // the HciEmulator channel, so here we repackage and forward them as
      // Emboss packets.
      auto emboss_packet =
          bt::hci::CommandPacket::New<pwemb::CommandHeaderView>(
              opcode, command_packet.size());
      bt::MutableBufferView dest = emboss_packet.mutable_data();
      command_packet.data().view().Copy(&dest);
      HandleReceivedCommandPacket(emboss_packet);
      break;
    }
    default: {
      bt_log(WARN,
             "fake-hci",
             "received unhandled command with opcode: %#.4x",
             opcode);
      RespondWithCommandComplete(static_cast<pwemb::OpCode>(opcode),
                                 pwemb::StatusCode::UNKNOWN_COMMAND);
      break;
    }
  }
}

void FakeController::HandleReceivedCommandPacket(
    const hci::CommandPacket& command_packet) {
  hci_spec::OpCode opcode = command_packet.opcode();

  if (MaybeRespondWithDefaultCommandStatus(opcode)) {
    return;
  }

  if (MaybeRespondWithDefaultStatus(opcode)) {
    return;
  }

  auto ogf = command_packet.ogf();
  if (ogf == hci_spec::kVendorOGF) {
    bt_log(WARN,
           "fake-hci",
           "vendor commands not yet migrated to Emboss; received Emboss vendor "
           "command with "
           "opcode: %#.4x",
           opcode);
    RespondWithCommandComplete(static_cast<pwemb::OpCode>(opcode),
                               pwemb::StatusCode::UNKNOWN_COMMAND);
    return;
  }

  switch (opcode) {
    case hci_spec::kInquiry: {
      auto params = command_packet.view<pwemb::InquiryCommandView>();
      OnInquiry(params);
      break;
    }
    case hci_spec::kEnhancedAcceptSynchronousConnectionRequest: {
      auto params = command_packet.view<
          pwemb::EnhancedAcceptSynchronousConnectionRequestCommandView>();
      OnEnhancedAcceptSynchronousConnectionRequestCommand(params);
      break;
    }
    case hci_spec::kEnhancedSetupSynchronousConnection: {
      auto params =
          command_packet
              .view<pwemb::EnhancedSetupSynchronousConnectionCommandView>();
      OnEnhancedSetupSynchronousConnectionCommand(params);
      break;
    }
    case hci_spec::kCreateConnection: {
      const auto params =
          command_packet.view<pwemb::CreateConnectionCommandView>();
      OnCreateConnectionCommandReceived(params);
      break;
    }
    case hci_spec::kDisconnect: {
      const auto params = command_packet.view<pwemb::DisconnectCommandView>();
      OnDisconnectCommandReceived(params);
      break;
    }
    case hci_spec::kLESetAdvertisingEnable: {
      const auto params =
          command_packet.view<pwemb::LESetAdvertisingEnableCommandView>();
      OnLESetAdvertisingEnable(params);
      break;
    }
    case hci_spec::kLESetExtendedAdvertisingEnable: {
      const auto params =
          command_packet
              .view<pwemb::LESetExtendedAdvertisingEnableCommandView>();
      OnLESetExtendedAdvertisingEnable(params);
      break;
    }
    case hci_spec::kLERemoveAdvertisingSet: {
      const auto params =
          command_packet.view<pwemb::LERemoveAdvertisingSetCommandView>();
      OnLERemoveAdvertisingSet(params);
      break;
    }
    case hci_spec::kLinkKeyRequestNegativeReply: {
      const auto params =
          command_packet.view<pwemb::LinkKeyRequestNegativeReplyCommandView>();
      OnLinkKeyRequestNegativeReplyCommandReceived(params);
      break;
    }
    case hci_spec::kAuthenticationRequested: {
      const auto params =
          command_packet.view<pwemb::AuthenticationRequestedCommandView>();
      OnAuthenticationRequestedCommandReceived(params);
      break;
    }
    case hci_spec::kSetConnectionEncryption: {
      const auto params =
          command_packet.view<pwemb::SetConnectionEncryptionCommandView>();
      OnSetConnectionEncryptionCommand(params);
      break;
    }
    case hci_spec::kRemoteNameRequest: {
      const auto params =
          command_packet.view<pwemb::RemoteNameRequestCommandView>();
      OnReadRemoteNameRequestCommandReceived(params);
      break;
    }
    case hci_spec::kReadRemoteSupportedFeatures: {
      const auto params =
          command_packet.view<pwemb::ReadRemoteSupportedFeaturesCommandView>();
      OnReadRemoteSupportedFeaturesCommandReceived(params);
      break;
    }
    case hci_spec::kReadRemoteExtendedFeatures: {
      const auto params =
          command_packet.view<pwemb::ReadRemoteExtendedFeaturesCommandView>();
      OnReadRemoteExtendedFeaturesCommandReceived(params);
      break;
    }
    case hci_spec::kReadRemoteVersionInfo: {
      const auto params =
          command_packet.view<pwemb::ReadRemoteVersionInfoCommandView>();
      OnReadRemoteVersionInfoCommandReceived(params);
      break;
    }
    case hci_spec::kIOCapabilityRequestReply: {
      const auto params =
          command_packet.view<pwemb::IoCapabilityRequestReplyCommandView>();
      OnIOCapabilityRequestReplyCommand(params);
      break;
    }
    case hci_spec::kSetEventMask: {
      const auto params = command_packet.view<pwemb::SetEventMaskCommandView>();
      OnSetEventMask(params);
      break;
    }
    case hci_spec::kWriteLocalName: {
      const auto params =
          command_packet.view<pwemb::WriteLocalNameCommandView>();
      OnWriteLocalName(params);
      break;
    }
    case hci_spec::kWriteScanEnable: {
      const auto& params =
          command_packet.view<pwemb::WriteScanEnableCommandView>();
      OnWriteScanEnable(params);
      break;
    }
    case hci_spec::kWritePageScanActivity: {
      const auto& params =
          command_packet.view<pwemb::WritePageScanActivityCommandView>();
      OnWritePageScanActivity(params);
      break;
    }
    case hci_spec::kUserConfirmationRequestReply: {
      const auto& params =
          command_packet.view<pwemb::UserConfirmationRequestReplyCommandView>();
      OnUserConfirmationRequestReplyCommand(params);
      break;
    }
    case hci_spec::kUserConfirmationRequestNegativeReply: {
      const auto& params =
          command_packet
              .view<pwemb::UserConfirmationRequestNegativeReplyCommandView>();
      OnUserConfirmationRequestNegativeReplyCommand(params);
      break;
    }
    case hci_spec::kWriteSynchronousFlowControlEnable: {
      const auto& params =
          command_packet
              .view<pwemb::WriteSynchronousFlowControlEnableCommandView>();
      OnWriteSynchronousFlowControlEnableCommand(params);
      break;
    }
    case hci_spec::kWriteExtendedInquiryResponse: {
      const auto& params =
          command_packet.view<pwemb::WriteExtendedInquiryResponseCommandView>();
      OnWriteExtendedInquiryResponse(params);
      break;
    }
    case hci_spec::kWriteSimplePairingMode: {
      const auto& params =
          command_packet.view<pwemb::WriteSimplePairingModeCommandView>();
      OnWriteSimplePairingMode(params);
      break;
    }
    case hci_spec::kWriteClassOfDevice: {
      const auto& params =
          command_packet.view<pwemb::WriteClassOfDeviceCommandView>();
      OnWriteClassOfDevice(params);
      break;
    }
    case hci_spec::kWriteInquiryMode: {
      const auto& params =
          command_packet.view<pwemb::WriteInquiryModeCommandView>();
      OnWriteInquiryMode(params);
      break;
    };
    case hci_spec::kWritePageScanType: {
      const auto& params =
          command_packet.view<pwemb::WritePageScanTypeCommandView>();
      OnWritePageScanType(params);
      break;
    }
    case hci_spec::kWriteLEHostSupport: {
      const auto& params =
          command_packet.view<pwemb::WriteLEHostSupportCommandView>();
      OnWriteLEHostSupportCommandReceived(params);
      break;
    }
    case hci_spec::kWriteSecureConnectionsHostSupport: {
      const auto& params =
          command_packet
              .view<pwemb::WriteSecureConnectionsHostSupportCommandView>();
      OnWriteSecureConnectionsHostSupport(params);
      break;
    }
    case hci_spec::kReadEncryptionKeySize: {
      const auto& params =
          command_packet.view<pwemb::ReadEncryptionKeySizeCommandView>();
      OnReadEncryptionKeySizeCommand(params);
      break;
    }
    case hci_spec::kLEReadRemoteFeatures: {
      const auto& params =
          command_packet.view<pwemb::LEReadRemoteFeaturesCommandView>();
      OnLEReadRemoteFeaturesCommand(params);
      break;
    }
    case hci_spec::kLESetEventMask: {
      const auto& params =
          command_packet.view<pwemb::LESetEventMaskCommandView>();
      OnLESetEventMask(params);
      break;
    }
    case hci_spec::kLESetRandomAddress: {
      const auto& params =
          command_packet.view<pwemb::LESetRandomAddressCommandView>();
      OnLESetRandomAddress(params);
      break;
    }
    case hci_spec::kLESetAdvertisingData: {
      const auto& params =
          command_packet.view<pwemb::LESetAdvertisingDataCommandView>();
      OnLESetAdvertisingData(params);
      break;
    }
    case hci_spec::kLESetScanResponseData: {
      const auto& params =
          command_packet.view<pwemb::LESetScanResponseDataCommandView>();
      OnLESetScanResponseData(params);
      break;
    }
    case hci_spec::kLESetScanParameters: {
      const auto& params =
          command_packet.view<pwemb::LESetScanParametersCommandView>();
      OnLESetScanParameters(params);
      break;
    }
    case hci_spec::kLESetExtendedScanParameters: {
      const auto& params =
          command_packet.view<pwemb::LESetExtendedScanParametersCommandView>();
      OnLESetExtendedScanParameters(params);
      break;
    }
    case hci_spec::kLESetScanEnable: {
      const auto& params =
          command_packet.view<pwemb::LESetScanEnableCommandView>();
      OnLESetScanEnable(params);
      break;
    }
    case hci_spec::kLESetExtendedScanEnable: {
      const auto& params =
          command_packet.view<pwemb::LESetExtendedScanEnableCommandView>();
      OnLESetExtendedScanEnable(params);
      break;
    }
    case hci_spec::kLECreateConnection: {
      const auto& params =
          command_packet.view<pwemb::LECreateConnectionCommandView>();
      OnLECreateConnectionCommandReceived(params);
      break;
    }
    case hci_spec::kLEExtendedCreateConnection: {
      const auto& params =
          command_packet.view<pwemb::LEExtendedCreateConnectionCommandV1View>();
      OnLEExtendedCreateConnectionCommandReceived(params);
      break;
    }
    case static_cast<uint16_t>(
        pwemb::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC): {
      const auto& params =
          command_packet
              .view<pwemb::LEPeriodicAdvertisingCreateSyncCommandView>();
      OnLEPeriodicAdvertisingCreateSyncCommandReceived(params);
      break;
    }
    case static_cast<uint16_t>(
        pwemb::OpCode::LE_PERIODIC_ADVERTISING_TERMINATE_SYNC): {
      const auto& params =
          command_packet
              .view<pwemb::LEPeriodicAdvertisingTerminateSyncCommandView>();
      OnLEPeriodicAdvertisingTerminateSyncCommandReceived(params);
      break;
    }
    case static_cast<uint16_t>(
        pwemb::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST): {
      const auto& params =
          command_packet
              .view<pwemb::LEAddDeviceToPeriodicAdvertiserListCommandView>();
      OnLEAddDeviceToPeriodicAdvertiserListCommandReceived(params);
      break;
    }
    case static_cast<uint16_t>(
        pwemb::OpCode::LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST): {
      const auto& params = command_packet.view<
          pwemb::LERemoveDeviceFromPeriodicAdvertiserListCommandView>();
      OnLERemoveDeviceFromPeriodicAdvertiserListCommandReceived(params);
      break;
    }
    case hci_spec::kLEConnectionUpdate: {
      const auto& params =
          command_packet.view<pwemb::LEConnectionUpdateCommandView>();
      OnLEConnectionUpdateCommandReceived(params);
      break;
    }
    case hci_spec::kLEStartEncryption: {
      const auto& params =
          command_packet.view<pwemb::LEEnableEncryptionCommandView>();
      OnLEStartEncryptionCommand(params);
      break;
    }
    case hci_spec::kReadLocalExtendedFeatures: {
      const auto& params =
          command_packet.view<pwemb::ReadLocalExtendedFeaturesCommandView>();
      OnReadLocalExtendedFeatures(params);
      break;
    }
    case hci_spec::kLESetAdvertisingParameters: {
      const auto& params =
          command_packet.view<pwemb::LESetAdvertisingParametersCommandView>();
      OnLESetAdvertisingParameters(params);
      break;
    }
    case hci_spec::kLESetExtendedAdvertisingData: {
      const auto& params =
          command_packet.view<pwemb::LESetExtendedAdvertisingDataCommandView>();
      OnLESetExtendedAdvertisingData(params);
      break;
    }
    case hci_spec::kLESetExtendedScanResponseData: {
      const auto& params =
          command_packet
              .view<pwemb::LESetExtendedScanResponseDataCommandView>();
      OnLESetExtendedScanResponseData(params);
      break;
    }
    case hci_spec::kLESetHostFeature: {
      const auto& params =
          command_packet.view<pwemb::LESetHostFeatureCommandView>();
      OnLESetHostFeature(params);
      break;
    }
    case hci_spec::kLEReadMaximumAdvertisingDataLength: {
      OnLEReadMaximumAdvertisingDataLength();
      break;
    }
    case hci_spec::kLEReadNumSupportedAdvertisingSets: {
      OnLEReadNumberOfSupportedAdvertisingSets();
      break;
    }
    case hci_spec::kLEClearAdvertisingSets: {
      OnLEClearAdvertisingSets();
      break;
    }
    case hci_spec::kLESetAdvertisingSetRandomAddress: {
      const auto& params =
          command_packet
              .view<pwemb::LESetAdvertisingSetRandomAddressCommandView>();
      OnLESetAdvertisingSetRandomAddress(params);
      break;
    }
    case hci_spec::kLESetExtendedAdvertisingParameters: {
      const auto& params =
          command_packet
              .view<pwemb::LESetExtendedAdvertisingParametersV1CommandView>();
      OnLESetExtendedAdvertisingParameters(params);
      break;
    }
    case hci_spec::kReadLocalSupportedControllerDelay: {
      const auto& params =
          command_packet
              .view<pwemb::ReadLocalSupportedControllerDelayCommandView>();
      OnReadLocalSupportedControllerDelay(params);
      break;
    }
    case hci_spec::kLERejectCISRequest: {
      const auto& params =
          command_packet.view<pwemb::LERejectCISRequestCommandView>();
      OnLERejectCisRequestCommand(params);
      break;
    }
    default: {
      bt_log(WARN, "fake-hci", "opcode: %#.4x", opcode);
      break;
    }
  }
}
}  // namespace bt::testing
