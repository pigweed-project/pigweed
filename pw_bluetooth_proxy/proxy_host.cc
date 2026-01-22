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

#include "pw_bluetooth_proxy/proxy_host.h"

#include "pw_bluetooth/emboss_util.h"
#include "pw_bluetooth/hci_commands.emb.h"
#include "pw_bluetooth/hci_common.emb.h"
#include "pw_bluetooth/hci_h4.emb.h"
#include "pw_bluetooth_proxy/h4_packet.h"
#include "pw_bluetooth_proxy/internal/logical_transport.h"
#include "pw_bluetooth_proxy/l2cap_channel_common.h"
#include "pw_log/log.h"

namespace pw::bluetooth::proxy {

ProxyHost::ProxyHost(
    pw::Function<void(H4PacketWithHci&& packet)>&& send_to_host_fn,
    pw::Function<void(H4PacketWithH4&& packet)>&& send_to_controller_fn,
    uint16_t le_acl_credits_to_reserve,
    uint16_t br_edr_acl_credits_to_reserve,
    pw::Allocator* allocator)
    : impl_(*this),
      hci_transport_(std::move(send_to_host_fn),
                     std::move(send_to_controller_fn)),
      acl_data_channel_(hci_transport_,
                        le_acl_credits_to_reserve,
                        br_edr_acl_credits_to_reserve,
                        /*on_tx_credits_fn=*/[this]() { OnAclTxCredits(); }),
      l2cap_channel_manager_(acl_data_channel_, *allocator) {
  PW_LOG_INFO(
      "btproxy: ProxyHost ctor - le_acl_credits_to_reserve: %u, "
      "br_edr_acl_credits_to_reserve: %u",
      le_acl_credits_to_reserve,
      br_edr_acl_credits_to_reserve);
}

ProxyHost::~ProxyHost() {
  PW_LOG_INFO("btproxy: ProxyHost dtor");
  acl_data_channel_.Reset();
  l2cap_channel_manager_.DeregisterAndCloseChannels(
      L2capChannelEvent::kChannelClosedByOther);
}

void ProxyHost::DoReset() {
  // Reset AclDataChannel first, so that send credits are reset to 0 until
  // reinitialized by controller event. This way, new channels can still be
  // registered, but they cannot erroneously use invalidated send credits.
  acl_data_channel_.Reset();
  l2cap_channel_manager_.DeregisterAndCloseChannels(L2capChannelEvent::kReset);
}

void ProxyHost::DoHandleH4HciFromHost(H4PacketWithH4&& h4_packet) {
  switch (h4_packet.GetH4Type()) {
    case emboss::H4PacketType::COMMAND:
      HandleCommandFromHost(std::move(h4_packet));
      return;
    case emboss::H4PacketType::ACL_DATA:
      HandleAclFromHost(std::move(h4_packet));
      return;
    case emboss::H4PacketType::EVENT:
    case emboss::H4PacketType::UNKNOWN:
    case emboss::H4PacketType::SYNC_DATA:
    case emboss::H4PacketType::ISO_DATA:
    default:
      hci_transport_.SendToController(std::move(h4_packet));
      return;
  }
}

void ProxyHost::DoHandleH4HciFromController(H4PacketWithHci&& h4_packet) {
  switch (h4_packet.GetH4Type()) {
    case emboss::H4PacketType::EVENT:
      HandleEventFromController(std::move(h4_packet));
      return;
    case emboss::H4PacketType::ACL_DATA:
      HandleAclFromController(std::move(h4_packet));
      return;
    case emboss::H4PacketType::UNKNOWN:
    case emboss::H4PacketType::COMMAND:
    case emboss::H4PacketType::SYNC_DATA:
    case emboss::H4PacketType::ISO_DATA:
    default:
      hci_transport_.SendToHost(std::move(h4_packet));
      return;
  }
}

void ProxyHost::HandleEventFromController(H4PacketWithHci&& h4_packet) {
  pw::span<uint8_t> hci_buffer = h4_packet.GetHciSpan();
  Result<emboss::EventHeaderView> event =
      MakeEmbossView<emboss::EventHeaderView>(hci_buffer);
  if (!event.ok()) {
    PW_LOG_ERROR(
        "Buffer is too small for EventHeader. So will pass on to host without "
        "processing.");
    hci_transport_.SendToHost(std::move(h4_packet));
    return;
  }

  PW_MODIFY_DIAGNOSTICS_PUSH();
  PW_MODIFY_DIAGNOSTIC(ignored, "-Wswitch-enum");
  switch (event->event_code().Read()) {
    case emboss::EventCode::NUMBER_OF_COMPLETED_PACKETS: {
      acl_data_channel_.HandleNumberOfCompletedPacketsEvent(
          std::move(h4_packet));
      break;
    }
    case emboss::EventCode::DISCONNECTION_COMPLETE: {
      Result<emboss::DisconnectionCompleteEventView> dc_event =
          MakeEmbossView<emboss::DisconnectionCompleteEventView>(
              h4_packet.GetHciSpan());
      if (dc_event.ok() &&
          dc_event->status().Read() == emboss::StatusCode::SUCCESS) {
        uint16_t conn_handle = dc_event->connection_handle().Read();
        l2cap_channel_manager_.HandleAclDisconnectionComplete(conn_handle);
        acl_data_channel_.ProcessDisconnectionCompleteEvent(
            conn_handle, dc_event->reason().Read());
      }
      hci_transport_.SendToHost(std::move(h4_packet));
      break;
    }
    case emboss::EventCode::COMMAND_COMPLETE: {
      HandleCommandCompleteEvent(std::move(h4_packet));
      break;
    }
    case emboss::EventCode::CONNECTION_COMPLETE: {
      pw::span<uint8_t> hci_span = h4_packet.GetHciSpan();
      Result<emboss::ConnectionCompleteEventView> connection_complete_event =
          MakeEmbossView<emboss::ConnectionCompleteEventView>(hci_span);
      if (connection_complete_event.ok() &&
          connection_complete_event->status().Read() ==
              emboss::StatusCode::SUCCESS) {
        OnConnectionCompleteSuccess(
            connection_complete_event->connection_handle().Read(),
            AclTransportType::kBrEdr);
      }
      hci_transport_.SendToHost(std::move(h4_packet));
      break;
    }
    case emboss::EventCode::LE_META_EVENT: {
      HandleLeMetaEvent(std::move(h4_packet));
      break;
    }
    default: {
      hci_transport_.SendToHost(std::move(h4_packet));
      return;
    }
  }
  PW_MODIFY_DIAGNOSTICS_POP();

  l2cap_channel_manager_.DeliverPendingEvents();
}

void ProxyHost::HandleAclFromController(H4PacketWithHci&& h4_packet) {
  acl_data_channel_.HandleAclFromController(std::move(h4_packet));
  //  It's possible for a channel handling rx traffic to have queued tx traffic
  //  or events.
  l2cap_channel_manager_.DrainChannelQueuesIfNewTx();
  l2cap_channel_manager_.DeliverPendingEvents();
}

void ProxyHost::HandleLeMetaEvent(H4PacketWithHci&& h4_packet) {
  pw::span<uint8_t> hci_buffer = h4_packet.GetHciSpan();
  Result<emboss::LEMetaEventView> le_meta_event_view =
      MakeEmbossView<emboss::LEMetaEventView>(hci_buffer);
  if (!le_meta_event_view.ok()) {
    PW_LOG_ERROR(
        "Buffer is too small for LE_META_EVENT event. So will not process.");
    hci_transport_.SendToHost(std::move(h4_packet));
    return;
  }

  PW_MODIFY_DIAGNOSTICS_PUSH();
  PW_MODIFY_DIAGNOSTIC(ignored, "-Wswitch-enum");
  switch (le_meta_event_view->subevent_code_enum().Read()) {
    case emboss::LeSubEventCode::CONNECTION_COMPLETE: {
      Result<emboss::LEConnectionCompleteSubeventView> event =
          MakeEmbossView<emboss::LEConnectionCompleteSubeventView>(hci_buffer);
      if (event.ok() && event->status().Read() == emboss::StatusCode::SUCCESS) {
        OnConnectionCompleteSuccess(event->connection_handle().Read(),
                                    AclTransportType::kLe);
      }
      hci_transport_.SendToHost(std::move(h4_packet));
      return;
    }
    case emboss::LeSubEventCode::ENHANCED_CONNECTION_COMPLETE_V1: {
      Result<emboss::LEEnhancedConnectionCompleteSubeventV1View> event =
          MakeEmbossView<emboss::LEEnhancedConnectionCompleteSubeventV1View>(
              hci_buffer);
      if (event.ok() && event->status().Read() == emboss::StatusCode::SUCCESS) {
        OnConnectionCompleteSuccess(event->connection_handle().Read(),
                                    AclTransportType::kLe);
      }
      hci_transport_.SendToHost(std::move(h4_packet));
      return;
    }
    case emboss::LeSubEventCode::ENHANCED_CONNECTION_COMPLETE_V2: {
      Result<emboss::LEEnhancedConnectionCompleteSubeventV2View> event =
          MakeEmbossView<emboss::LEEnhancedConnectionCompleteSubeventV2View>(
              hci_buffer);
      if (event.ok() && event->status().Read() == emboss::StatusCode::SUCCESS) {
        OnConnectionCompleteSuccess(event->connection_handle().Read(),
                                    AclTransportType::kLe);
      }
      hci_transport_.SendToHost(std::move(h4_packet));
      return;
    }
    default:
      break;
  }
  PW_MODIFY_DIAGNOSTICS_POP();
  hci_transport_.SendToHost(std::move(h4_packet));
}

void ProxyHost::HandleCommandCompleteEvent(H4PacketWithHci&& h4_packet) {
  pw::span<uint8_t> hci_buffer = h4_packet.GetHciSpan();
  Result<emboss::CommandCompleteEventView> command_complete_event =
      MakeEmbossView<emboss::CommandCompleteEventView>(hci_buffer);
  if (!command_complete_event.ok()) {
    PW_LOG_ERROR(
        "Buffer is too small for COMMAND_COMPLETE event. So will not process.");
    hci_transport_.SendToHost(std::move(h4_packet));
    return;
  }

  PW_MODIFY_DIAGNOSTICS_PUSH();
  PW_MODIFY_DIAGNOSTIC(ignored, "-Wswitch-enum");
  switch (command_complete_event->command_opcode().Read()) {
    case emboss::OpCode::READ_BUFFER_SIZE: {
      Result<emboss::ReadBufferSizeCommandCompleteEventWriter> read_event =
          MakeEmbossWriter<emboss::ReadBufferSizeCommandCompleteEventWriter>(
              hci_buffer);
      if (!read_event.ok()) {
        PW_LOG_ERROR(
            "Buffer is too small for READ_BUFFER_SIZE command complete event. "
            "Will not process.");
        break;
      }
      acl_data_channel_.ProcessReadBufferSizeCommandCompleteEvent(*read_event);
      break;
    }
    case emboss::OpCode::LE_READ_BUFFER_SIZE_V1: {
      Result<emboss::LEReadBufferSizeV1CommandCompleteEventWriter> read_event =
          MakeEmbossWriter<
              emboss::LEReadBufferSizeV1CommandCompleteEventWriter>(hci_buffer);
      if (!read_event.ok()) {
        PW_LOG_ERROR(
            "Buffer is too small for LE_READ_BUFFER_SIZE_V1 command complete "
            "event. So will not process.");
        break;
      }
      acl_data_channel_.ProcessLEReadBufferSizeCommandCompleteEvent(
          *read_event);
      break;
    }
    case emboss::OpCode::LE_READ_BUFFER_SIZE_V2: {
      Result<emboss::LEReadBufferSizeV2CommandCompleteEventWriter> read_event =
          MakeEmbossWriter<
              emboss::LEReadBufferSizeV2CommandCompleteEventWriter>(hci_buffer);
      if (!read_event.ok()) {
        PW_LOG_ERROR(
            "Buffer is too small for LE_READ_BUFFER_SIZE_V2 command complete "
            "event. So will not process.");
        break;
      }
      acl_data_channel_.ProcessLEReadBufferSizeCommandCompleteEvent(
          *read_event);
      break;
    }
    default:
      break;
  }
  PW_MODIFY_DIAGNOSTICS_POP();
  hci_transport_.SendToHost(std::move(h4_packet));
}

void ProxyHost::HandleCommandFromHost(H4PacketWithH4&& h4_packet) {
  pw::span<uint8_t> hci_buffer = h4_packet.GetHciSpan();
  Result<emboss::GenericHciCommandView> command =
      MakeEmbossView<emboss::GenericHciCommandView>(hci_buffer);
  if (!command.ok()) {
    hci_transport_.SendToController(std::move(h4_packet));
    return;
  }

  if (command->header().opcode().Read() == emboss::OpCode::RESET) {
    PW_LOG_INFO("Resetting proxy on HCI_Reset Command from host.");
    Reset();
  }

  hci_transport_.SendToController(std::move(h4_packet));
}

void ProxyHost::HandleAclFromHost(H4PacketWithH4&& h4_packet) {
  acl_data_channel_.HandleAclFromHost(std::move(h4_packet));
  //  It's possible for a channel handling tx traffic to have queued tx traffic
  //  or events.
  l2cap_channel_manager_.DrainChannelQueuesIfNewTx();
  l2cap_channel_manager_.DeliverPendingEvents();
}

pw::Result<L2capCoc> ProxyHost::DoAcquireL2capCoc(
    MultiBufAllocator& rx_multibuf_allocator,
    uint16_t connection_handle,
    L2capCoc::CocConfig rx_config,
    L2capCoc::CocConfig tx_config,
    Function<void(FlatConstMultiBuf&& payload)>&& receive_fn,
    ChannelEventCallback&& event_fn) {
  return l2cap_channel_manager_.AcquireL2capCoc(rx_multibuf_allocator,
                                                connection_handle,
                                                rx_config,
                                                tx_config,
                                                std::move(receive_fn),
                                                std::move(event_fn));
}

pw::Result<BasicL2capChannel> ProxyHost::DoAcquireBasicL2capChannel(
    MultiBufAllocator& rx_multibuf_allocator,
    uint16_t connection_handle,
    uint16_t local_cid,
    uint16_t remote_cid,
    AclTransportType transport,
    OptionalPayloadReceiveCallback&& payload_from_controller_fn,
    OptionalPayloadReceiveCallback&& payload_from_host_fn,
    ChannelEventCallback&& event_fn) {
  return l2cap_channel_manager_.AcquireBasicL2capChannel(
      rx_multibuf_allocator,
      connection_handle,
      local_cid,
      remote_cid,
      transport,
      std::move(payload_from_controller_fn),
      std::move(payload_from_host_fn),
      std::move(event_fn));
}

pw::Result<GattNotifyChannel> ProxyHost::DoAcquireGattNotifyChannel(
    int16_t connection_handle,
    uint16_t attribute_handle,
    ChannelEventCallback&& event_fn) {
  return l2cap_channel_manager_.AcquireGattNotifyChannel(
      connection_handle, attribute_handle, std::move(event_fn));
}

bool ProxyHost::DoHasSendLeAclCapability() const {
  return acl_data_channel_.HasSendAclCapability(AclTransportType::kLe);
}

bool ProxyHost::DoHasSendBrEdrAclCapability() const {
  return acl_data_channel_.HasSendAclCapability(AclTransportType::kBrEdr);
}

uint16_t ProxyHost::DoGetNumFreeLeAclPackets() const {
  return acl_data_channel_.GetNumFreeAclPackets(AclTransportType::kLe);
}

uint16_t ProxyHost::DoGetNumFreeBrEdrAclPackets() const {
  return acl_data_channel_.GetNumFreeAclPackets(AclTransportType::kBrEdr);
}

void ProxyHost::DoRegisterL2capStatusDelegate(L2capStatusDelegate& delegate) {
  l2cap_channel_manager_.RegisterStatusDelegate(delegate);
}

void ProxyHost::DoUnregisterL2capStatusDelegate(L2capStatusDelegate& delegate) {
  l2cap_channel_manager_.UnregisterStatusDelegate(delegate);
}

void ProxyHost::OnConnectionCompleteSuccess(uint16_t connection_handle,
                                            AclTransportType transport) {
  acl_data_channel_.HandleConnectionCompleteEvent(connection_handle, transport);
  Status l2cap_status =
      l2cap_channel_manager_.AddConnection(connection_handle, transport);
  if (!l2cap_status.ok()) {
    PW_LOG_WARN("Could not add L2CAP connection for %#x: %s",
                connection_handle,
                l2cap_status.str());
  }
}

void ProxyHost::OnAclTxCredits() {
  l2cap_channel_manager_.ForceDrainChannelQueues();
}

Result<UniquePtr<ChannelProxy>> ProxyHost::InternalDoInterceptBasicModeChannel(
    ConnectionHandle connection_handle,
    uint16_t local_channel_id,
    uint16_t remote_channel_id,
    AclTransportType transport,
    BufferReceiveFunction&& payload_from_controller_fn,
    BufferReceiveFunction&& payload_from_host_fn,
    ChannelEventCallback&& event_fn) {
  return l2cap_channel_manager_.InterceptBasicModeChannel(
      connection_handle,
      local_channel_id,
      remote_channel_id,
      transport,
      std::move(payload_from_controller_fn),
      std::move(payload_from_host_fn),
      std::move(event_fn));
}

}  // namespace pw::bluetooth::proxy
