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
#include "pw_assert/check.h"
#include "pw_bluetooth/emboss_util.h"
#include "pw_bluetooth_proxy/l2cap_channel_common.h"
#include "pw_bluetooth_proxy/rfcomm/rfcomm_common.h"
#include "pw_containers/vector.h"
#include "pw_log/log.h"
#include "pw_multibuf/multibuf.h"

namespace pw::bluetooth::proxy::rfcomm {

RfcommManager::ConnectionState::ConnectionState(ConnectionHandle handle,
                                                uint16_t local_cid_arg,
                                                uint16_t remote_cid_arg,
                                                Allocator& allocator)
    : connection_handle(handle),
      local_cid(local_cid_arg),
      remote_cid(remote_cid_arg),
      channels(allocator) {}

RfcommManager::RfcommManager(
    L2capChannelManagerInterface& l2cap_channel_manager, Allocator& allocator)
    : l2cap_channel_manager_(l2cap_channel_manager),
      allocator_(allocator),
      connections_(allocator) {
  PW_LOG_INFO("RFCOMM manager created");
}

RfcommManager::~RfcommManager() {
  PW_LOG_INFO("RFCOMM manager destroyed");
  DeregisterAndCloseChannels(RfcommEvent::kChannelClosedByOther);
}

Result<RfcommChannel> RfcommManager::DoAcquireRfcommChannel(
    multibuf::MultiBufAllocator& rx_multibuf_allocator,
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
        [this](multibuf::MultiBuf&& payload,
               ConnectionHandle handle,
               uint16_t local_cid,
               uint16_t remote_cid) -> std::optional<multibuf::MultiBuf> {
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

    auto emplace_result = connections_.try_emplace(connection_handle,
                                                   connection_handle,
                                                   rx_config.cid,
                                                   tx_config.cid,
                                                   allocator_);
    if (!emplace_result.has_value()) {
      return Status::ResourceExhausted();
    }
    it = emplace_result.value().first;
    it->second.l2cap_channel_proxy = std::move(proxy_result).value();
    PW_LOG_INFO("New RFCOMM connection state created for connection handle %hu",
                static_cast<uint16_t>(connection_handle));
  } else {
    // If the connection already exists, verify that the CIDs match.
    if (it->second.local_cid != rx_config.cid ||
        it->second.remote_cid != tx_config.cid) {
      PW_LOG_ERROR(
          "RFCOMM CIDs do not match existing connection for handle %hu",
          static_cast<uint16_t>(connection_handle));
      return Status::InvalidArgument();
    }
  }

  auto& conn_state = it->second;
  auto channel_it =
      conn_state.channels.find(MakeDlci(channel_number, direction));
  // If the channel already exists, return an error.
  if (channel_it != conn_state.channels.end()) {
    return Status::AlreadyExists();
  }

  // Insert the new channel into the connection state.
  auto emplace_result =
      conn_state.channels.try_emplace(MakeDlci(channel_number, direction),
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
  if (!emplace_result.has_value()) {
    PW_LOG_ERROR("Failed to insert RFCOMM channel");
    return Status::ResourceExhausted();
  }

  return RfcommChannel(connection_handle, channel_number, direction, this);
}

StatusWithMultiBuf RfcommManager::DoWrite(ConnectionHandle connection_handle,
                                          uint8_t channel_number,
                                          RfcommDirection direction,
                                          multibuf::MultiBuf&& payload) {
  std::lock_guard lock(connections_mutex_);
  auto it = connections_.find(connection_handle);
  if (it == connections_.end()) {
    return {Status::NotFound(), std::move(payload)};
  }
  auto& conn_state = it->second;
  auto channel_it =
      conn_state.channels.find(MakeDlci(channel_number, direction));
  if (channel_it == conn_state.channels.end()) {
    return {Status::NotFound(), std::move(payload)};
  }
  return channel_it->second.Write(std::move(payload));
}

void RfcommManager::DeregisterAndCloseChannels(RfcommEvent event) {
  PW_LOG_INFO("Deregistering and closing RFCOMM channels with event: %d",
              static_cast<int>(event));
  ConnectionMap connections_to_close(allocator_);
  {
    std::lock_guard lock(connections_mutex_);
    connections_to_close.swap(connections_);
  }

  while (!connections_to_close.empty()) {
    CloseConnectionState(
        connections_to_close.take(connections_to_close.begin()), event);
  }
}

Status RfcommManager::DoReleaseRfcommChannel(ConnectionHandle connection_handle,
                                             uint8_t channel_number,
                                             RfcommDirection direction) {
  UniquePtr<ConnectionMap::node_type> conn_state_to_delete;
  UniquePtr<ChannelMap::node_type> channel_to_close;
  {
    std::lock_guard lock(connections_mutex_);
    auto it = connections_.find(connection_handle);
    if (it == connections_.end()) {
      return Status::NotFound();
    }

    auto& conn_state = it->second;
    auto channel_it =
        conn_state.channels.find(MakeDlci(channel_number, direction));
    // If the channel is found, remove it from the map.
    if (channel_it != conn_state.channels.end()) {
      channel_to_close = conn_state.channels.take(channel_it);
    }

    // If this is the last RFCOMM channel for the connection, close the
    // connection.
    if (conn_state.channels.empty()) {
      PW_LOG_INFO(
          "Last RFCOMM channel closed for connection handle %hu. "
          "Closing connection.",
          static_cast<uint16_t>(conn_state.connection_handle));
      conn_state_to_delete = connections_.take(it);
    }
  }

  // Close the channel outside the lock.
  if (channel_to_close != nullptr) {
    channel_to_close->mapped().Close(RfcommEvent::kChannelClosedByOther);
  }

  return OkStatus();
}

Status RfcommManager::DoSendAdditionalRxCredits(
    ConnectionHandle connection_handle,
    uint8_t channel_number,
    RfcommDirection direction,
    uint8_t credits) {
  std::lock_guard lock(connections_mutex_);
  auto it = connections_.find(connection_handle);
  if (it == connections_.end()) {
    PW_LOG_WARN("Connection handle %hu not found",
                static_cast<uint16_t>(connection_handle));
    return Status::NotFound();
  }
  auto& conn_state = it->second;
  auto channel_it =
      conn_state.channels.find(MakeDlci(channel_number, direction));
  if (channel_it == conn_state.channels.end()) {
    PW_LOG_WARN("Channel %d (direction: %hhu) not found",
                channel_number,
                static_cast<uint8_t>(direction));
    return Status::NotFound();
  }
  return channel_it->second.SendAdditionalRxCredits(credits);
}

void RfcommManager::CloseAllChannelsForConnection(
    ConnectionHandle connection_handle, RfcommEvent event) {
  UniquePtr<ConnectionMap::node_type> conn_state_to_close;
  {
    std::lock_guard lock(connections_mutex_);
    auto it = connections_.find(connection_handle);
    if (it != connections_.end()) {
      conn_state_to_close = connections_.take(it);
    }
  }

  if (conn_state_to_close != nullptr) {
    CloseConnectionState(std::move(conn_state_to_close), event);
  }
}

void RfcommManager::CloseConnectionState(
    UniquePtr<ConnectionMap::node_type>&& conn_state_node, RfcommEvent event) {
  if (conn_state_node == nullptr) {
    return;
  }
  ChannelMap channels_to_close(allocator_);
  channels_to_close.swap(conn_state_node->mapped().channels);
  for (auto& [_, channel] : channels_to_close) {
    channel.Close(event);
  }
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

std::optional<multibuf::MultiBuf> RfcommManager::HandlePduFromController(
    multibuf::MultiBuf&& pdu,
    ConnectionHandle connection_handle,
    uint16_t local_cid,
    uint16_t remote_cid) {
  UniquePtr<ConnectionMap::node_type> conn_state_to_delete;
  UniquePtr<ChannelMap::node_type> channel_to_close;
  uint8_t uih_credits = 0;
  span<const uint8_t> uih_info_bytes;
  std::optional<internal::BorrowedRfcommChannel> borrowed_channel;

  {
    std::lock_guard lock(connections_mutex_);
    auto it = connections_.find(connection_handle);
    if (it == connections_.end()) {
      return std::move(pdu);
    }
    if (it->second.local_cid != local_cid ||
        it->second.remote_cid != remote_cid) {
      PW_LOG_ERROR("Received L2CAP PDU with mismatched CIDs for handle %hu",
                   static_cast<uint16_t>(connection_handle));
      return std::move(pdu);
    }

    auto& conn_state = it->second;

    auto contiguous_span = pdu.ContiguousSpan();
    PW_CHECK(contiguous_span.has_value(),
             "Received fragmented L2CAP PDU for handle %hu, which is not yet "
             "supported",
             static_cast<uint16_t>(connection_handle));
    ConstByteSpan pdu_bytes = as_bytes(*contiguous_span);

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
          "Received RFCOMM PDU for unknown DLCI: (channel %u, direction %u, "
          "size %zu), "
          "forwarding to host.",
          channel_number,
          static_cast<uint8_t>(direction),
          pdu_bytes.size());
      return std::move(pdu);
    }

    internal::RfcommChannelInternal* channel = &channel_it->second;

    // Handle the RFCOMM frame type.
    emboss::RfcommFrameType frame_type = parsed_frame.control().Read();
    if (parsed_frame.uih().Read()) {
      borrowed_channel.emplace(*channel);
      uih_credits = parsed_frame.credits().IsComplete()
                        ? parsed_frame.credits().Read()
                        : 0;
      uih_info_bytes = span<const uint8_t>(
          parsed_frame.information().BackingStorage().data(),
          parsed_frame.information().SizeInBytes());
    } else if (frame_type == emboss::RfcommFrameType::DISCONNECT_MODE ||
               frame_type ==
                   emboss::RfcommFrameType::DISCONNECT_MODE_AND_POLL_FINAL ||
               frame_type == emboss::RfcommFrameType::DISCONNECT ||
               frame_type ==
                   emboss::RfcommFrameType::DISCONNECT_AND_POLL_FINAL) {
      PW_LOG_INFO("Channel number %u closed by remote", channel_number);
      channel_to_close = conn_state.channels.take(channel_it);
      if (conn_state.channels.empty()) {
        PW_LOG_INFO(
            "Last RFCOMM channel closed for connection handle %hu. "
            "Closing connection.",
            static_cast<uint16_t>(conn_state.connection_handle));
        conn_state_to_delete = connections_.take(it);
      }
    }
  }

  if (borrowed_channel.has_value()) {
    if (!borrowed_channel.value()->HandlePduFromController(
            uih_credits, as_bytes(uih_info_bytes))) {
      return std::nullopt;
    }
  }

  if (channel_to_close != nullptr) {
    channel_to_close->mapped().Close(RfcommEvent::kChannelClosedByRemote);
  }
  return std::move(pdu);
}

void RfcommManager::HandleL2capEvent(L2capChannelEvent event,
                                     ConnectionHandle connection_handle) {
  UniquePtr<ConnectionMap::node_type> conn_state_to_close;
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
      conn_state_to_close = connections_.take(it);
    }
  }

  if (conn_state_to_close != nullptr) {
    RfcommEvent rfcomm_event = event == L2capChannelEvent::kReset
                                   ? RfcommEvent::kReset
                                   : RfcommEvent::kChannelClosedByOther;
    CloseConnectionState(std::move(conn_state_to_close), rfcomm_event);
  }
}

}  // namespace pw::bluetooth::proxy::rfcomm
