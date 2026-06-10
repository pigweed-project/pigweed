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

#include "pw_bluetooth_sapphire/internal/host/transport/iso_data_channel.h"

#include <pw_assert/check.h>
#include <pw_bluetooth/hci_data.emb.h>

#include <map>

namespace bt::hci {
namespace {

constexpr size_t kFrameHeaderSize =
    pw::bluetooth::emboss::IsoDataFrameHeaderView::SizeInBytes();

}  // namespace

class IsoDataChannelImpl final : public IsoDataChannel {
 public:
  IsoDataChannelImpl(
      const DataBufferInfo& buffer_info,
      CommandChannel* command_channel,
      pw::bluetooth::Controller* hci,
      pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider);
  ~IsoDataChannelImpl() override;

  // IsoDataChannel overrides:
  bool RegisterConnection(hci_spec::ConnectionHandle handle,
                          WeakPtr<ConnectionInterface> connection) override;
  bool UnregisterConnection(hci_spec::ConnectionHandle handle) override;
  void TrySendPackets() override;
  void ClearControllerPacketCount(hci_spec::ConnectionHandle handle) override;
  const DataBufferInfo& buffer_info() const override { return buffer_info_; }

 private:
  using ConnectionMap =
      std::map<hci_spec::ConnectionHandle, WeakPtr<ConnectionInterface>>;

  void SendData(DynamicByteBuffer packet);
  void OnRxPacket(pw::span<const std::byte> buffer);

  // Handle a NumberOfCompletedPackets event.
  CommandChannel::EventCallbackResult OnNumberOfCompletedPacketsEvent(
      const EventPacket& event);

  // Increment next_connection_iter_, with wrapping.
  void IncrementConnectionIter();

  CommandChannel* command_channel_ __attribute__((unused));
  pw::bluetooth::Controller* hci_;
  DataBufferInfo buffer_info_;

  size_t available_buffers_;

  // Stores connections registered by RegisterConnection()
  ConnectionMap connections_;
  ConnectionMap::iterator next_connection_iter_ = connections_.end();

  // Stores per-connection information of unacknowledged packets sent to the
  // controller. Entries are updated/removed on the HCI Number Of Completed
  // Packets event and when ClearControllerPacketCount() is called (the
  // controller does not acknowledge packets of disconnected links).
  std::unordered_map<hci_spec::ConnectionHandle, size_t> pending_packets_;

  // Event handler ID for the NumberOfCompletedPackets event
  CommandChannel::EventHandlerId num_completed_packets_event_handler_id_ = 0;

  // A wake lease is held while there are transmitted packets awaiting
  // acknowledgement from a Number Of Completed Packets event.
  std::optional<pw::bluetooth_sapphire::Lease> wake_lease_;
  pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider_;
};

IsoDataChannelImpl::IsoDataChannelImpl(
    const DataBufferInfo& buffer_info,
    CommandChannel* command_channel,
    pw::bluetooth::Controller* hci,
    pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider)
    : command_channel_(command_channel),
      hci_(hci),
      buffer_info_(buffer_info),
      available_buffers_(buffer_info.max_num_packets()),
      wake_lease_provider_(wake_lease_provider) {
  // IsoDataChannel shouldn't be used if the buffer is unavailable (implying the
  // controller doesn't support isochronous channels).
  PW_CHECK(buffer_info_.IsAvailable());

  hci_->SetReceiveIsoFunction(
      fit::bind_member<&IsoDataChannelImpl::OnRxPacket>(this));

  num_completed_packets_event_handler_id_ = command_channel_->AddEventHandler(
      hci_spec::kNumberOfCompletedPacketsEventCode,
      fit::bind_member<&IsoDataChannelImpl::OnNumberOfCompletedPacketsEvent>(
          this));
  PW_DCHECK(num_completed_packets_event_handler_id_);
}

IsoDataChannelImpl::~IsoDataChannelImpl() {
  command_channel_->RemoveEventHandler(num_completed_packets_event_handler_id_);
}

void IsoDataChannelImpl::OnRxPacket(pw::span<const std::byte> buffer) {
  const uint8_t* data = reinterpret_cast<const uint8_t*>(buffer.data());
  auto header_view =
      pw::bluetooth::emboss::MakeIsoDataFrameHeaderView(data, buffer.size());
  if (!header_view.Ok()) {
    bt_log(ERROR,
           "hci",
           "malformed ISO header (invalid fields or size - expected at least "
           "%zu bytes, got %zu)",
           kFrameHeaderSize,
           buffer.size());
    return;
  }

  hci_spec::ConnectionHandle handle = header_view.connection_handle().Read();
  if (connections_.count(handle) == 0) {
    bt_log(WARN,
           "hci",
           "ISO data packet received for unrecognized handle 0x%x",
           handle);
    return;
  }
  WeakPtr<ConnectionInterface> stream = connections_[handle];
  PW_CHECK(stream.is_alive());
  stream->ReceiveInboundPacket(buffer);
}

bool IsoDataChannelImpl::RegisterConnection(
    hci_spec::ConnectionHandle handle,
    WeakPtr<ConnectionInterface> connection) {
  bt_log(INFO, "hci", "registering ISO connection for handle 0x%x", handle);
  if (connections_.count(handle) != 0) {
    bt_log(ERROR,
           "hci",
           "Attempt to re-register connection for handle 0x%x",
           handle);
    return false;
  }
  connections_[handle] = std::move(connection);

  // Reset round robin iterator.
  next_connection_iter_ = connections_.begin();

  TrySendPackets();

  return true;
}

bool IsoDataChannelImpl::UnregisterConnection(
    hci_spec::ConnectionHandle handle) {
  bt_log(INFO, "hci", "unregistering ISO connection for handle 0x%x", handle);
  if (connections_.count(handle) == 0) {
    bt_log(ERROR,
           "hci",
           "Attempt to de-register connection for unrecognized handle 0x%x",
           handle);
    return false;
  }
  connections_.erase(handle);

  // Reset round robin iterator.
  next_connection_iter_ = connections_.begin();

  return true;
}

void IsoDataChannelImpl::ClearControllerPacketCount(
    hci_spec::ConnectionHandle handle) {
  PW_CHECK(connections_.find(handle) == connections_.end());

  bt_log(INFO, "hci", "clearing pending packets (handle: %#.4x)", handle);

  auto pending_packets_iter = pending_packets_.find(handle);
  if (pending_packets_iter == pending_packets_.end()) {
    bt_log(DEBUG,
           "hci",
           "no pending packets on connection (handle: %#.4x)",
           handle);
    return;
  }

  // Add pending packets to available buffers because controller does
  // not send HCI Number of Completed Packets events for disconnected
  // connections.
  available_buffers_ += pending_packets_iter->second;
  if (available_buffers_ == buffer_info_.max_num_packets()) {
    wake_lease_.reset();
  }
  pending_packets_.erase(pending_packets_iter);

  // Try sending the next batch of packets in case buffer space opened up.
  TrySendPackets();
}

void IsoDataChannelImpl::TrySendPackets() {
  if (connections_.empty()) {
    return;
  }

  // Ensure a lease is held while calling GetNextOutboundPdu() below, in case
  // connections drop their wake lease upon handing off their last packet.
  pw::Result<pw::bluetooth_sapphire::Lease> temp_lease =
      PW_SAPPHIRE_ACQUIRE_LEASE(wake_lease_provider_,
                                "IsoDataChannel::TrySendPackets");

  // Use Round Robin fairness algorithm to send packets from multiple streams.
  ConnectionMap::iterator start_iter = next_connection_iter_;
  // Initialize to true to ensure the connection map is looped over at least
  // once.
  bool packet_sent_this_iteration = true;
  for (; available_buffers_ > 0; IncrementConnectionIter()) {
    if (next_connection_iter_ == start_iter) {
      if (!packet_sent_this_iteration) {
        // All streams are empty.
        break;
      }
      packet_sent_this_iteration = false;
    }

    std::optional<DynamicByteBuffer> packet =
        next_connection_iter_->second->GetNextOutboundPdu();
    if (!packet) {
      continue;
    }
    packet_sent_this_iteration = true;

    if ((packet->size() - kFrameHeaderSize) > buffer_info_.max_data_length()) {
      bt_log(ERROR,
             "hci",
             "Dropping oversized ISO packet (size: %zu, max: %zu)",
             packet->size() - kFrameHeaderSize,
             buffer_info_.max_data_length());
      continue;
    }
    hci_->SendIsoData(packet->view().subspan());

    --available_buffers_;
    auto [iter, _] =
        pending_packets_.try_emplace(next_connection_iter_->first, 0);
    iter->second++;
    if (!wake_lease_) {
      wake_lease_ =
          PW_SAPPHIRE_ACQUIRE_LEASE(wake_lease_provider_, "IsoDataChannel")
              .value_or(pw::bluetooth_sapphire::Lease());
    }
  }
}

CommandChannel::EventCallbackResult
IsoDataChannelImpl::OnNumberOfCompletedPacketsEvent(const EventPacket& event) {
  if (event.size() <
      pw::bluetooth::emboss::NumberOfCompletedPacketsEvent::MinSizeInBytes()) {
    bt_log(ERROR,
           "hci",
           "Invalid HCI_Number_Of_Completed_Packets event received, ignoring");
    return CommandChannel::EventCallbackResult::kContinue;
  }
  auto view = event.unchecked_view<
      pw::bluetooth::emboss::NumberOfCompletedPacketsEventView>();
  PW_CHECK(view.header().event_code().Read() ==
           pw::bluetooth::emboss::EventCode::NUMBER_OF_COMPLETED_PACKETS);

  size_t handles_in_packet =
      (event.size() -
       pw::bluetooth::emboss::NumberOfCompletedPacketsEvent::MinSizeInBytes()) /
      pw::bluetooth::emboss::NumberOfCompletedPacketsEventData::
          IntrinsicSizeInBytes();
  uint8_t expected_number_of_handles = view.num_handles().Read();
  if (expected_number_of_handles != handles_in_packet) {
    bt_log(ERROR,
           "hci",
           "packets handle count (%d) doesn't match params size (%zu)",
           expected_number_of_handles,
           handles_in_packet);
  }

  for (uint8_t i = 0; i < expected_number_of_handles && i < handles_in_packet;
       ++i) {
    uint16_t handle = view.nocp_data()[i].connection_handle().Read();
    uint16_t num_completed_packets =
        view.nocp_data()[i].num_completed_packets().Read();
    auto pending_packets_iter = pending_packets_.find(handle);
    if (pending_packets_iter == pending_packets_.end()) {
      // This is expected if the completed packet is an ACL or SCO packet.
      bt_log(TRACE,
             "hci",
             "controller reported completed packets for connection handle "
             "without pending packets: "
             "%#.4x",
             handle);
      continue;
    }

    if (pending_packets_iter->second < num_completed_packets) {
      // TODO(fxbug.dev/42102535): This can be caused by the controller
      // reusing the connection handle of a connection that just disconnected.
      // We should somehow avoid sending the controller packets for a connection
      // that has disconnected. IsoDataChannel already dequeues such packets,
      // but this is insufficient: packets can be queued in the channel to the
      // transport driver, and possibly in the transport driver or USB/UART
      // drivers.
      bt_log(ERROR,
             "hci",
             "ISO NOCP count mismatch! (handle: %#.4x, expected: %zu, "
             "actual : %u)",
             handle,
             pending_packets_iter->second,
             num_completed_packets);
      // This should eventually result in convergence with the correct pending
      // packet count. If it undercounts the true number of pending packets,
      // this branch will be reached again when the controller sends an updated
      // Number of Completed Packets event. However, IsoDataChannel may overflow
      // the controller's buffer in the meantime!
      num_completed_packets =
          static_cast<uint16_t>(pending_packets_iter->second);
    }

    available_buffers_ += num_completed_packets;
    if (available_buffers_ == buffer_info_.max_num_packets()) {
      wake_lease_.reset();
    }
    pending_packets_iter->second -= num_completed_packets;
  }

  TrySendPackets();
  return CommandChannel::EventCallbackResult::kContinue;
}

std::unique_ptr<IsoDataChannel> IsoDataChannel::Create(
    const DataBufferInfo& buffer_info,
    CommandChannel* command_channel,
    pw::bluetooth::Controller* hci,
    pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider) {
  bt_log(DEBUG, "hci", "Creating a new IsoDataChannel");
  return std::make_unique<IsoDataChannelImpl>(
      buffer_info, command_channel, hci, wake_lease_provider);
}

void IsoDataChannelImpl::IncrementConnectionIter() {
  if (connections_.empty()) {
    next_connection_iter_ = connections_.end();
    return;
  }
  ++next_connection_iter_;
  if (next_connection_iter_ == connections_.end()) {
    next_connection_iter_ = connections_.begin();
  }
}

}  // namespace bt::hci
