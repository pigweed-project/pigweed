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

#include "pw_bluetooth_proxy/rfcomm/rfcomm_manager.h"

#include "pw_allocator/allocator.h"
#include "pw_bluetooth/emboss_util.h"
#include "pw_bluetooth_proxy/internal/multibuf.h"
#include "pw_bluetooth_proxy/l2cap_channel_common.h"
#include "pw_bluetooth_proxy/rfcomm/rfcomm_common.h"
#include "pw_containers/vector.h"
#include "pw_log/log.h"

namespace pw::bluetooth::proxy::rfcomm {

RfcommManager::ConnectionState::ConnectionState(ConnectionHandle handle,
                                                uint16_t local_cid_arg,
                                                uint16_t remote_cid_arg)
    : connection_handle(handle),
      local_cid(local_cid_arg),
      remote_cid(remote_cid_arg) {}

RfcommManager::RfcommManager(
    L2capChannelManagerInterface& l2cap_channel_manager, Allocator& allocator)
    : l2cap_channel_manager_(l2cap_channel_manager), allocator_(allocator) {
  PW_LOG_INFO("RFCOMM manager created");
}

RfcommManager::~RfcommManager() {
  PW_LOG_INFO("RFCOMM manager destroyed");
  DeregisterAndCloseChannels(RfcommEvent::kChannelClosedByOther);
}

Result<RfcommChannel> RfcommManager::DoAcquireRfcommChannel(
    MultiBufAllocator& rx_multibuf_allocator,
    ConnectionHandle connection_handle,
    uint8_t channel_number,
    RfcommDirection direction,
    bool mux_initiator,
    const RfcommChannelConfig& rx_config,
    const RfcommChannelConfig& tx_config,
    RfcommReceiveCallback&& receive_fn,
    RfcommEventCallback&& event_fn) {
  std::lock_guard lock(connections_mutex_);
  auto it = connections_.find(connection_handle);
  // If the connection is not found, create a new connection state and L2CAP
  // channel proxy.
  if (it == connections_.end()) {
    OptionalBufferReceiveFunction from_controller_fn =
        [this](
            FlatMultiBuf&& payload,
            ConnectionHandle handle,
            uint16_t local_cid,
            uint16_t remote_cid) -> std::optional<FlatConstMultiBufInstance> {
      return HandlePduFromController(
          std::move(payload), handle, local_cid, remote_cid);
    };

    auto proxy_result = l2cap_channel_manager_.InterceptBasicModeChannel(
        connection_handle,
        rx_config.cid,
        tx_config.cid,
        AclTransportType::kBrEdr,
        std::move(from_controller_fn),
        {},
        [this, connection_handle](L2capChannelEvent event) {
          this->HandleL2capEvent(event, connection_handle);
        });

    if (!proxy_result.ok()) {
      return proxy_result.status();
    }

    auto new_conn_state = allocator_.MakeUnique<ConnectionState>(
        connection_handle, rx_config.cid, tx_config.cid);
    if (new_conn_state == nullptr) {
      return Status::ResourceExhausted();
    }
    new_conn_state->l2cap_channel_proxy = std::move(proxy_result).value();
    connections_.insert(*new_conn_state);
    new_conn_state.Release();  // The intrusive map now manages the lifetime.
    PW_LOG_INFO("New RFCOMM connection state created for connection handle %hu",
                static_cast<uint16_t>(connection_handle));
    it = connections_.find(connection_handle);
  } else {
    // If the connection already exists, verify that the CIDs match.
    if (it->local_cid != rx_config.cid || it->remote_cid != tx_config.cid) {
      PW_LOG_ERROR(
          "RFCOMM CIDs do not match existing connection for handle %hu",
          static_cast<uint16_t>(connection_handle));
      return Status::InvalidArgument();
    }
  }

  auto& conn_state = *it;
  auto channel_it =
      conn_state.channels.find(MakeDlci(channel_number, direction));
  // If the channel already exists, return an error.
  if (channel_it != conn_state.channels.end()) {
    return Status::AlreadyExists();
  }

  // Create a new RFCOMM channel.
  auto channel = allocator_.MakeUnique<internal::RfcommChannelInternal>(
      rx_multibuf_allocator,
      *conn_state.l2cap_channel_proxy,
      connection_handle,
      channel_number,
      direction,
      mux_initiator,
      rx_config,
      tx_config,
      kRfcommCrc,
      std::move(receive_fn),
      std::move(event_fn));
  if (channel == nullptr) {
    PW_LOG_ERROR("Failed to allocate RFCOMM channel");
    return Status::ResourceExhausted();
  }

  // Insert the new channel into the connection state.
  conn_state.channels.insert(*channel);
  channel.Release();  // The intrusive map now manages the lifetime.
  return RfcommChannel(connection_handle, channel_number, direction, this);
}

StatusWithMultiBuf RfcommManager::DoWrite(ConnectionHandle connection_handle,
                                          uint8_t channel_number,
                                          RfcommDirection direction,
                                          FlatConstMultiBuf&& payload) {
  std::lock_guard lock(connections_mutex_);
  auto it = connections_.find(connection_handle);
  if (it == connections_.end()) {
    return {Status::NotFound(), std::move(payload)};
  }
  auto& conn_state = *it;
  auto channel_it =
      conn_state.channels.find(MakeDlci(channel_number, direction));
  if (channel_it == conn_state.channels.end()) {
    return {Status::NotFound(), std::move(payload)};
  }
  return channel_it->Write(std::move(payload));
}

void RfcommManager::DeregisterAndCloseChannels(RfcommEvent event) {
  PW_LOG_INFO("Deregistering and closing RFCOMM channels with event: %d",
              static_cast<int>(event));
  IntrusiveMap<ConnectionHandle, ConnectionState> connections_to_close;
  {
    std::lock_guard lock(connections_mutex_);
    connections_to_close.swap(connections_);
  }

  while (!connections_to_close.empty()) {
    ConnectionState* conn_state = &*connections_to_close.begin();
    connections_to_close.erase(connections_to_close.begin());
    CloseConnectionState(conn_state, event);
  }
}

Status RfcommManager::DoReleaseRfcommChannel(ConnectionHandle connection_handle,
                                             uint8_t channel_number,
                                             RfcommDirection direction) {
  internal::RfcommChannelInternal* channel_to_close = nullptr;
  ConnectionState* conn_state_to_delete = nullptr;
  {
    std::lock_guard lock(connections_mutex_);
    auto it = connections_.find(connection_handle);
    if (it == connections_.end()) {
      return Status::NotFound();
    }

    auto& conn_state = *it;
    auto channel_it =
        conn_state.channels.find(MakeDlci(channel_number, direction));
    // If the channel is found, remove it from the map.
    if (channel_it != conn_state.channels.end()) {
      channel_to_close = &*channel_it;
      conn_state.channels.erase(channel_it);
    }

    // If this is the last RFCOMM channel for the connection, close the
    // connection.
    if (conn_state.channels.empty()) {
      PW_LOG_INFO(
          "Last RFCOMM channel closed for connection handle %hu. "
          "Closing connection.",
          static_cast<uint16_t>(conn_state.connection_handle));
      conn_state_to_delete = &conn_state;
      connections_.erase(it);
    }
  }

  // Close the channel outside the lock.
  if (channel_to_close) {
    channel_to_close->Close(RfcommEvent::kChannelClosedByOther);
    allocator_.Delete(channel_to_close);
    // If this is the last RFCOMM channel for the connection, delete the
    // connection state.
    if (conn_state_to_delete) {
      allocator_.Delete(conn_state_to_delete);
    }
  }

  return OkStatus();
}

void RfcommManager::CloseAllChannelsForConnection(
    ConnectionHandle connection_handle, RfcommEvent event) {
  ConnectionState* conn_state_to_close = nullptr;
  {
    std::lock_guard lock(connections_mutex_);
    auto it = connections_.find(connection_handle);
    if (it != connections_.end()) {
      conn_state_to_close = &*it;
      connections_.erase(it);
    }
  }

  if (conn_state_to_close) {
    CloseConnectionState(conn_state_to_close, event);
  }
}

void RfcommManager::CloseConnectionState(ConnectionState* conn_state,
                                         RfcommEvent event) {
  IntrusiveMap<uint8_t, internal::RfcommChannelInternal> channels_to_close;
  channels_to_close.swap(conn_state->channels);
  while (!channels_to_close.empty()) {
    auto* channel = &*channels_to_close.begin();
    channels_to_close.erase(channels_to_close.begin());
    channel->Close(event);
    allocator_.Delete(channel);
  }
  allocator_.Delete(conn_state);
}

Result<emboss::RfcommFrameView> RfcommManager::ParseRfcommFrame(
    ConstByteSpan pdu) {
  // Parse the RFCOMM frame.
  auto frame_view = emboss::MakeRfcommFrameView(pdu.data(), pdu.size());
  if (!frame_view.Ok()) {
    PW_LOG_WARN("Failed to parse RFCOMM frame.");
    return Status::InvalidArgument();
  }

  // The FCS must be verified before we can trust the contents of the frame.
  const uint8_t received_fcs = frame_view.fcs().Read();

  // For UIH frames, FCS is calculated over the address and control fields.
  // Otherwise FCS is calculated over the address, control, and length fields.
  emboss::RfcommHeaderLength header_size_for_fcs =
      emboss::RfcommHeaderLength::WITHOUT_LENGTH;
  if (!frame_view.uih().Read()) {
    if (frame_view.length_extended_flag().Read() ==
        emboss::RfcommLengthExtended::EXTENDED) {
      header_size_for_fcs = emboss::RfcommHeaderLength::WITH_EXTENDED_LENGTH;
    } else {
      header_size_for_fcs = emboss::RfcommHeaderLength::WITH_LENGTH;
    }
  }
  const uint8_t calculated_fcs =
      kRfcommCrc.Calculate(pdu.first(static_cast<size_t>(header_size_for_fcs)));
  if (calculated_fcs != received_fcs) {
    PW_LOG_WARN("RFCOMM FCS mismatch (expected: %02x, got: %02x)",
                calculated_fcs,
                received_fcs);
    return Status::DataLoss();
  }

  return frame_view;
}

std::optional<FlatConstMultiBufInstance> RfcommManager::HandlePduFromController(
    FlatMultiBuf&& pdu,
    ConnectionHandle connection_handle,
    uint16_t local_cid,
    uint16_t remote_cid) {
  internal::RfcommChannelInternal* channel_to_close = nullptr;
  ConnectionState* conn_state_to_delete = nullptr;
  {
    std::lock_guard lock(connections_mutex_);
    auto it = connections_.find(connection_handle);
    if (it == connections_.end()) {
      return std::move(pdu);
    }
    if (it->local_cid != local_cid || it->remote_cid != remote_cid) {
      PW_LOG_ERROR("Received L2CAP PDU with mismatched CIDs for handle %hu",
                   static_cast<uint16_t>(connection_handle));
      return std::move(pdu);
    }

    auto& conn_state = *it;

    ConstByteSpan pdu_bytes = as_bytes(MultiBufAdapter::AsSpan(pdu));

    auto parsed_frame_result = ParseRfcommFrame(pdu_bytes);
    if (!parsed_frame_result.ok()) {
      PW_LOG_WARN("Failed to parse RFCOMM frame.");
      return std::move(pdu);
    }
    auto parsed_frame = std::move(parsed_frame_result).value();

    const uint8_t channel_number =
        static_cast<uint8_t>(parsed_frame.channel().Read());
    const RfcommDirection direction = parsed_frame.direction().Read()
                                          ? RfcommDirection::kInitiator
                                          : RfcommDirection::kResponder;
    auto channel_it =
        conn_state.channels.find(MakeDlci(channel_number, direction));
    if (channel_it == conn_state.channels.end()) {
      PW_LOG_DEBUG(
          "Received RFCOMM PDU for unknown DLCI: (channel %u, direction %u), "
          "forwarding to host.",
          channel_number,
          static_cast<uint8_t>(direction));
      return std::move(pdu);
    }

    internal::RfcommChannelInternal* channel = &*channel_it;

    // Handle the RFCOMM frame type.
    emboss::RfcommFrameType frame_type = parsed_frame.control().Read();
    if (parsed_frame.uih().Read()) {
      span<const uint8_t> info_bytes(
          parsed_frame.information().BackingStorage().data(),
          parsed_frame.information().SizeInBytes());
      if (!channel->HandlePduFromController(parsed_frame.credits().IsComplete()
                                                ? parsed_frame.credits().Read()
                                                : 0,
                                            as_bytes(info_bytes))) {
        return std::nullopt;
      }
    } else if (frame_type == emboss::RfcommFrameType::DISCONNECT_MODE ||
               frame_type ==
                   emboss::RfcommFrameType::DISCONNECT_MODE_AND_POLL_FINAL ||
               frame_type == emboss::RfcommFrameType::DISCONNECT ||
               frame_type ==
                   emboss::RfcommFrameType::DISCONNECT_AND_POLL_FINAL) {
      PW_LOG_INFO("Channel number %u closed by remote", channel_number);
      channel_to_close = channel;
      conn_state.channels.erase(channel_it);
      if (conn_state.channels.empty()) {
        PW_LOG_INFO(
            "Last RFCOMM channel closed for connection handle %hu. "
            "Closing connection.",
            static_cast<uint16_t>(conn_state.connection_handle));
        conn_state_to_delete = &conn_state;
        connections_.erase(it);
      }
    }
  }

  if (channel_to_close) {
    channel_to_close->Close(RfcommEvent::kChannelClosedByRemote);
    allocator_.Delete(channel_to_close);
    if (conn_state_to_delete) {
      allocator_.Delete(conn_state_to_delete);
    }
  }
  return std::move(pdu);
}

void RfcommManager::HandleL2capEvent(L2capChannelEvent event,
                                     ConnectionHandle connection_handle) {
  ConnectionState* conn_state_to_close = nullptr;
  {
    std::lock_guard lock(connections_mutex_);
    auto it = connections_.find(connection_handle);
    // If the connection is not found, do nothing.
    if (it == connections_.end()) {
      return;
    }
    PW_LOG_INFO(
        "RFCOMM connection received L2CAP event: %d for connection handle: %hu",
        static_cast<int>(event),
        static_cast<uint16_t>(connection_handle));

    // If the L2CAP channel is closed by the other side or the connection is
    // reset, close all RFCOMM channels for the connection.
    if (event == L2capChannelEvent::kChannelClosedByOther ||
        event == L2capChannelEvent::kReset) {
      conn_state_to_close = &*it;
      connections_.erase(it);
    }
  }

  if (conn_state_to_close) {
    RfcommEvent rfcomm_event = event == L2capChannelEvent::kReset
                                   ? RfcommEvent::kReset
                                   : RfcommEvent::kChannelClosedByOther;
    CloseConnectionState(conn_state_to_close, rfcomm_event);
  }
}

}  // namespace pw::bluetooth::proxy::rfcomm
