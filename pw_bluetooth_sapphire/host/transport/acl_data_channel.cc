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

#include "pw_bluetooth_sapphire/internal/host/transport/acl_data_channel.h"

#include <pw_assert/check.h>
#include <pw_bluetooth/hci_commands.emb.h>
#include <pw_bluetooth/hci_data.emb.h>
#include <pw_bytes/endian.h>

#include <iterator>

#include "lib/fit/function.h"
#include "pw_bluetooth/vendor.h"
#include "pw_bluetooth_sapphire/internal/host/common/inspectable.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/util.h"
#include "pw_bluetooth_sapphire/internal/host/transport/acl_data_packet.h"
#include "pw_bluetooth_sapphire/internal/host/transport/link_type.h"
#include "pw_bluetooth_sapphire/internal/host/transport/transport.h"
#include "pw_bluetooth_sapphire/lease.h"

namespace bt::hci {

class AclDataChannelImpl final : public AclDataChannel {
 public:
  AclDataChannelImpl(
      Transport* transport,
      pw::bluetooth::Controller* hci,
      pw::async::Dispatcher& dispatcher,
      const DataBufferInfo& bredr_buffer_info,
      const DataBufferInfo& le_buffer_info,
      pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider);
  ~AclDataChannelImpl() override;

  // AclDataChannel overrides:
  void RegisterConnection(WeakPtr<ConnectionInterface> connection) override;
  void UnregisterConnection(hci_spec::ConnectionHandle handle) override;
  void OnOutboundPacketAvailable() override;
  std::optional<pw::chrono::SystemClock::time_point> GetLastPacketTime(
      hci_spec::ConnectionHandle handle) const override;
  void AttachInspect(inspect::Node& parent, const std::string& name) override;
  void SetDataRxHandler(ACLPacketHandler rx_callback) override;
  void ClearControllerPacketCount(hci_spec::ConnectionHandle handle) override;
  const DataBufferInfo& GetBufferInfo() const override;
  const DataBufferInfo& GetLeBufferInfo() const override;
  void RequestAclPriority(
      pw::bluetooth::AclPriority priority,
      hci_spec::ConnectionHandle handle,
      fit::callback<void(fit::result<fit::failed>)> callback) override;

 private:
  using ConnectionMap = std::unordered_map<hci_spec::ConnectionHandle,
                                           WeakPtr<ConnectionInterface>>;

  struct PendingPacketData {
    bt::LinkType ll_type = bt::LinkType::kACL;
    size_t count = 0;
  };

  // Handler for the HCI Number of Completed Packets Event, used for
  // packet-based data flow control.
  CommandChannel::EventCallbackResult NumberOfCompletedPacketsCallback(
      const EventPacket& event);

  // Sends next queued packets over the ACL data channel while the controller
  // has free buffer slots. If controller buffers are free and some links have
  // queued packets, we round-robin iterate through links, sending a packet from
  // each link with queued packets until the controller is full or we run out of
  // packets.
  void TrySendNextPackets();

  // Returns the number of free controller buffer slots for packets of type
  // |link_type|, taking shared buffers into account.
  size_t GetNumFreePacketsForLinkType(LinkType link_type) const;

  // Decreases the |link_type| pending packets count by |count|, taking shared
  // buffers into account.
  void DecrementPendingPacketsForLinkType(LinkType link_type, size_t count);

  // Increments the  pending packets count for links of type |link_type|, taking
  // shared buffers into account.
  void IncrementPendingPacketsForLinkType(LinkType link_type);

  // Returns true if the LE data buffer is not available
  bool IsBrEdrBufferShared() const;

  // Called when a packet is received from the controller. Validates the packet
  // and calls the client's RX callback.
  void OnRxPacket(pw::span<const std::byte> packet);

  // Increment connection iterators using round robin scheduling.
  // If the BR/EDR buffer is shared, simply increment the iterator to the next
  // connection. If the BR/EDR buffer isn't shared, increment the iterator to
  // the next connection of type |connection_type|. No-op if |conn_iter| is
  // |registered_connections_.end()|.
  void IncrementRoundRobinIterator(ConnectionMap::iterator& conn_iter,
                                   bt::LinkType connection_type);

  // Increments count of pending packets that have been sent to the controller
  // on |connection|.
  void IncrementPendingPacketsForLink(WeakPtr<ConnectionInterface>& connection);

  // Sends queued packets from links in a round-robin fashion, starting with
  // |current_link|. |current_link| will be incremented to the next link that
  // should send packets (according to the round-robin policy).
  void SendPackets(ConnectionMap::iterator& current_link);

  // Handler for HCI_Buffer_Overflow_event.
  CommandChannel::EventCallbackResult DataBufferOverflowCallback(
      const EventPacket& event);

  void ResetRoundRobinIterators();

  // Links this node to the inspect tree. Initialized as needed by
  // AttachInspect.
  inspect::Node node_;

  // Contents of |node_|. Retained as members so that they last as long as a
  // class instance.
  inspect::Node le_subnode_;
  inspect::BoolProperty le_subnode_shared_with_bredr_property_;
  inspect::Node bredr_subnode_;

  // The Transport object that owns this instance.
  Transport* const transport_;  // weak;

  // Controller is owned by Transport and will outlive this object.
  pw::bluetooth::Controller* const hci_;

  // Dispatcher handling task queue, currently gives us the current time
  pw::async::Dispatcher& dispatcher_;

  // The event handler ID for the Number Of Completed Packets event.
  CommandChannel::EventHandlerId num_completed_packets_event_handler_id_ = 0;

  // The event handler ID for the Data Buffer Overflow event.
  CommandChannel::EventHandlerId data_buffer_overflow_event_handler_id_ = 0;

  // The current handler for incoming data.
  ACLPacketHandler rx_callback_;

  // BR/EDR data buffer information. This buffer will not be available on
  // LE-only controllers.
  const DataBufferInfo bredr_buffer_info_;

  // LE data buffer information. This buffer will not be available on
  // BR/EDR-only controllers (which we do not support) and MAY be available on
  // dual-mode controllers. We maintain that if this buffer is not available,
  // then the BR/EDR buffer MUST be available.
  const DataBufferInfo le_buffer_info_;

  // The current count of the number of ACL data packets that have been sent to
  // the controller. |num_pending_le_packets_| is ignored if the controller uses
  // one buffer for LE and BR/EDR.
  UintInspectable<size_t> num_pending_bredr_packets_;
  UintInspectable<size_t> num_pending_le_packets_;

  // Stores per-connection information of unacknowledged packets sent to the
  // controller. Entries are updated/removed on the HCI Number Of Completed
  // Packets event and when a connection is unregistered (the controller does
  // not acknowledge packets of disconnected links).
  std::unordered_map<hci_spec::ConnectionHandle, PendingPacketData>
      pending_links_;

  // Stores connections registered by RegisterConnection().
  ConnectionMap registered_connections_;

  // Iterators used to round-robin through links for sending packets. When the
  // BR/EDR buffer is shared with LE, |current_le_link_| is ignored
  ConnectionMap::iterator current_bredr_link_ = registered_connections_.end();
  ConnectionMap::iterator current_le_link_ = registered_connections_.end();

  std::unordered_map<hci_spec::ConnectionHandle,
                     pw::chrono::SystemClock::time_point>
      last_packet_times_;

  pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider_;
  std::optional<pw::bluetooth_sapphire::Lease> wake_lease_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(AclDataChannelImpl);
};

std::unique_ptr<AclDataChannel> AclDataChannel::Create(
    Transport* transport,
    pw::bluetooth::Controller* hci,
    pw::async::Dispatcher& dispatcher,
    const DataBufferInfo& bredr_buffer_info,
    const DataBufferInfo& le_buffer_info,
    pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider) {
  return std::make_unique<AclDataChannelImpl>(transport,
                                              hci,
                                              dispatcher,
                                              bredr_buffer_info,
                                              le_buffer_info,
                                              wake_lease_provider);
}

AclDataChannelImpl::AclDataChannelImpl(
    Transport* transport,
    pw::bluetooth::Controller* hci,
    pw::async::Dispatcher& dispatcher,
    const DataBufferInfo& bredr_buffer_info,
    const DataBufferInfo& le_buffer_info,
    pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider)
    : transport_(transport),
      hci_(hci),
      dispatcher_(dispatcher),
      bredr_buffer_info_(bredr_buffer_info),
      le_buffer_info_(le_buffer_info),
      wake_lease_provider_(wake_lease_provider) {
  PW_DCHECK(transport_);
  PW_CHECK(hci_);

  PW_DCHECK(bredr_buffer_info.IsAvailable() || le_buffer_info.IsAvailable());

  num_completed_packets_event_handler_id_ =
      transport_->command_channel()->AddEventHandler(
          hci_spec::kNumberOfCompletedPacketsEventCode,
          fit::bind_member<
              &AclDataChannelImpl::NumberOfCompletedPacketsCallback>(this));
  PW_DCHECK(num_completed_packets_event_handler_id_);

  data_buffer_overflow_event_handler_id_ =
      transport_->command_channel()->AddEventHandler(
          hci_spec::kDataBufferOverflowEventCode,
          fit::bind_member<&AclDataChannelImpl::DataBufferOverflowCallback>(
              this));
  PW_DCHECK(data_buffer_overflow_event_handler_id_);

  bt_log(DEBUG, "hci", "AclDataChannel initialized");
}

AclDataChannelImpl::~AclDataChannelImpl() {
  bt_log(INFO, "hci", "AclDataChannel shutting down");

  transport_->command_channel()->RemoveEventHandler(
      num_completed_packets_event_handler_id_);
  transport_->command_channel()->RemoveEventHandler(
      data_buffer_overflow_event_handler_id_);

  hci_->SetReceiveAclFunction(nullptr);
}

void AclDataChannelImpl::RegisterConnection(
    WeakPtr<ConnectionInterface> connection) {
  bt_log(DEBUG,
         "hci",
         "ACL register connection (handle: %#.4x)",
         connection->handle());
  auto [_, inserted] =
      registered_connections_.emplace(connection->handle(), connection);
  PW_CHECK(inserted,
           "connection with handle %#.4x already registered",
           connection->handle());

  // Reset the round-robin iterators because they have been invalidated.
  ResetRoundRobinIterators();
}

void AclDataChannelImpl::UnregisterConnection(
    hci_spec::ConnectionHandle handle) {
  bt_log(DEBUG, "hci", "ACL unregister link (handle: %#.4x)", handle);
  auto iter = registered_connections_.find(handle);
  if (iter == registered_connections_.end()) {
    bt_log(WARN,
           "hci",
           "attempt to unregister link that is not registered (handle: %#.4x)",
           handle);
    return;
  }
  registered_connections_.erase(iter);

  // Reset the round-robin iterators because they have been invalidated.
  ResetRoundRobinIterators();
}

bool AclDataChannelImpl::IsBrEdrBufferShared() const {
  return !le_buffer_info_.IsAvailable();
}

void AclDataChannelImpl::IncrementRoundRobinIterator(
    ConnectionMap::iterator& conn_iter, bt::LinkType connection_type) {
  // Only update iterator if |registered_connections_| is non-empty
  if (conn_iter == registered_connections_.end()) {
    bt_log(
        DEBUG, "hci", "no registered connections, cannot increment iterator");
    return;
  }

  // Prevent infinite looping by tracking |original_conn_iter|
  const ConnectionMap::iterator original_conn_iter = conn_iter;
  do {
    conn_iter++;
    if (conn_iter == registered_connections_.end()) {
      conn_iter = registered_connections_.begin();
    }
  } while (!IsBrEdrBufferShared() &&
           conn_iter->second->type() != connection_type &&
           conn_iter != original_conn_iter);

  // When buffer isn't shared, we must ensure |conn_iter| is assigned to a link
  // of the same type.
  if (!IsBrEdrBufferShared() && conn_iter->second->type() != connection_type) {
    // There are no connections of |connection_type| in
    // |registered_connections_|.
    conn_iter = registered_connections_.end();
  }
}

void AclDataChannelImpl::IncrementPendingPacketsForLink(
    WeakPtr<ConnectionInterface>& connection) {
  auto [iter, _] = pending_links_.try_emplace(
      connection->handle(), PendingPacketData{connection->type()});
  iter->second.count++;
  IncrementPendingPacketsForLinkType(connection->type());
}

void AclDataChannelImpl::SendPackets(ConnectionMap::iterator& current_link) {
  PW_DCHECK(current_link != registered_connections_.end());
  const ConnectionMap::iterator original_link = current_link;
  const LinkType link_type = original_link->second->type();
  size_t free_buffer_packets = GetNumFreePacketsForLinkType(link_type);
  bool is_packet_queued = true;

  // Send packets as long as a link may have a packet queued and buffer space is
  // available.
  for (; free_buffer_packets != 0;
       IncrementRoundRobinIterator(current_link, link_type)) {
    if (current_link == original_link) {
      if (!is_packet_queued) {
        // All links are empty
        break;
      }
      is_packet_queued = false;
    }

    if (!current_link->second->HasAvailablePacket()) {
      continue;
    }

    // Acquire a wake lease because we may be taking the last queued packet from
    // upper layers, causing them to drop their wake leases.
    pw::Result<pw::bluetooth_sapphire::Lease> lease = PW_SAPPHIRE_ACQUIRE_LEASE(
        wake_lease_provider_, "AclDataChannelImpl::SendPackets");

    // If there is an available packet, send and update packet counts
    ACLDataPacketPtr packet = current_link->second->GetNextOutboundPacket();
    PW_DCHECK(packet);
    last_packet_times_[packet->connection_handle()] = dispatcher_.now();
    hci_->SendAclData(packet->view().data().subspan());

    is_packet_queued = true;
    free_buffer_packets--;
    IncrementPendingPacketsForLink(current_link->second);
  }
}

void AclDataChannelImpl::TrySendNextPackets() {
  if (current_bredr_link_ != registered_connections_.end()) {
    // If the BR/EDR buffer is shared, this will also send LE packets.
    SendPackets(current_bredr_link_);
  }

  if (!IsBrEdrBufferShared() &&
      current_le_link_ != registered_connections_.end()) {
    SendPackets(current_le_link_);
  }
}

void AclDataChannelImpl::OnOutboundPacketAvailable() { TrySendNextPackets(); }

std::optional<pw::chrono::SystemClock::time_point>
AclDataChannelImpl::GetLastPacketTime(hci_spec::ConnectionHandle handle) const {
  auto iter = last_packet_times_.find(handle);
  if (iter == last_packet_times_.end()) {
    return std::nullopt;
  }
  return iter->second;
}

void AclDataChannelImpl::AttachInspect(inspect::Node& parent,
                                       const std::string& name) {
  node_ = parent.CreateChild(std::move(name));

  bredr_subnode_ = node_.CreateChild("bredr");
  num_pending_bredr_packets_.AttachInspect(bredr_subnode_, "num_sent_packets");

  le_subnode_ = node_.CreateChild("le");
  num_pending_le_packets_.AttachInspect(le_subnode_, "num_sent_packets");
  le_subnode_shared_with_bredr_property_ =
      le_subnode_.CreateBool("independent_from_bredr", !IsBrEdrBufferShared());
}

void AclDataChannelImpl::SetDataRxHandler(ACLPacketHandler rx_callback) {
  PW_CHECK(rx_callback);
  rx_callback_ = std::move(rx_callback);
  hci_->SetReceiveAclFunction(
      fit::bind_member<&AclDataChannelImpl::OnRxPacket>(this));
}

void AclDataChannelImpl::ClearControllerPacketCount(
    hci_spec::ConnectionHandle handle) {
  // Ensure link has already been unregistered. Otherwise, queued packets for
  // this handle could be sent after clearing packet count, and the packet count
  // could become corrupted.
  PW_CHECK(registered_connections_.find(handle) ==
           registered_connections_.end());

  bt_log(DEBUG, "hci", "clearing pending packets (handle: %#.4x)", handle);

  // subtract removed packets from sent packet counts, because controller does
  // not send HCI Number of Completed Packets event for disconnected link
  auto iter = pending_links_.find(handle);
  if (iter == pending_links_.end()) {
    bt_log(DEBUG,
           "hci",
           "no pending packets on connection (handle: %#.4x)",
           handle);
    return;
  }

  const PendingPacketData& data = iter->second;
  DecrementPendingPacketsForLinkType(data.ll_type, data.count);

  pending_links_.erase(iter);
  last_packet_times_.erase(handle);

  // Try sending the next batch of packets in case buffer space opened up.
  TrySendNextPackets();
}

const DataBufferInfo& AclDataChannelImpl::GetBufferInfo() const {
  return bredr_buffer_info_;
}

const DataBufferInfo& AclDataChannelImpl::GetLeBufferInfo() const {
  return !IsBrEdrBufferShared() ? le_buffer_info_ : bredr_buffer_info_;
}

void AclDataChannelImpl::RequestAclPriority(
    pw::bluetooth::AclPriority priority,
    hci_spec::ConnectionHandle handle,
    fit::callback<void(fit::result<fit::failed>)> callback) {
  bt_log(TRACE, "hci", "sending ACL priority command");

  hci_->EncodeVendorCommand(
      pw::bluetooth::SetAclPriorityCommandParameters{
          .connection_handle = handle, .priority = priority},
      [this, priority, request_cb = std::move(callback)](
          pw::Result<pw::span<const std::byte>> encode_result) mutable {
        if (!encode_result.ok()) {
          bt_log(TRACE, "hci", "encoding ACL priority command failed");
          request_cb(fit::failed());
          return;
        }

        DynamicByteBuffer encoded(
            BufferView(encode_result->data(), encode_result->size()));
        if (encoded.size() < sizeof(hci_spec::CommandHeader)) {
          bt_log(TRACE,
                 "hci",
                 "encoded ACL priority command too small (size: %zu)",
                 encoded.size());
          request_cb(fit::failed());
          return;
        }

        hci_spec::OpCode op_code = pw::bytes::ConvertOrderFrom(
            cpp20::endian::little,
            encoded.ReadMember<&hci_spec::CommandHeader::opcode>());
        auto packet =
            CommandPacket::New<pw::bluetooth::emboss::GenericHciCommandWriter>(
                op_code, encoded.size());
        auto packet_data = packet.mutable_data();
        encoded.Copy(&packet_data);

        transport_->command_channel()
            ->SendCommand(
                std::move(packet),
                [cb = std::move(request_cb), priority](
                    auto, const hci::EventPacket& event) mutable {
                  if (HCI_IS_ERROR(event, WARN, "hci", "acl priority failed")) {
                    cb(fit::failed());
                    return;
                  }

                  bt_log(DEBUG,
                         "hci",
                         "acl priority updated (priority: %#.8x)",
                         static_cast<uint32_t>(priority));
                  cb(fit::ok());
                })
            .IgnoreError();
      });
}

CommandChannel::EventCallbackResult
AclDataChannelImpl::NumberOfCompletedPacketsCallback(const EventPacket& event) {
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
    bt_log(WARN,
           "hci",
           "packets handle count (%d) doesn't match params size (%zu)",
           expected_number_of_handles,
           handles_in_packet);
  }

  for (uint8_t i = 0; i < expected_number_of_handles && i < handles_in_packet;
       ++i) {
    uint16_t connection_handle = view.nocp_data()[i].connection_handle().Read();
    uint16_t num_completed_packets =
        view.nocp_data()[i].num_completed_packets().Read();
    auto iter = pending_links_.find(connection_handle);
    if (iter == pending_links_.end()) {
      // This is expected if the completed packet is a SCO packet.
      bt_log(TRACE,
             "hci",
             "controller reported completed packets for connection handle "
             "without pending packets: "
             "%#.4x",
             connection_handle);
      continue;
    }

    if (iter->second.count < num_completed_packets) {
      // TODO(fxbug.dev/42102535): This can be caused by the controller
      // reusing the connection handle of a connection that just disconnected.
      // We should somehow avoid sending the controller packets for a connection
      // that has disconnected. AclDataChannel already dequeues such packets,
      // but this is insufficient: packets can be queued in the channel to the
      // transport driver, and possibly in the transport driver or USB/UART
      // drivers.
      bt_log(ERROR,
             "hci",
             "ACL packet tx count mismatch! (handle: %#.4x, expected: %zu, "
             "actual : %u)",
             connection_handle,
             iter->second.count,
             num_completed_packets);
      // This should eventually result in convergence with the correct pending
      // packet count. If it undercounts the true number of pending packets,
      // this branch will be reached again when the controller sends an updated
      // Number of Completed Packets event. However, AclDataChannel may overflow
      // the controller's buffer in the meantime!
      num_completed_packets = static_cast<uint16_t>(iter->second.count);
    }

    iter->second.count -= num_completed_packets;
    DecrementPendingPacketsForLinkType(iter->second.ll_type,
                                       num_completed_packets);
    if (!iter->second.count) {
      pending_links_.erase(iter);
    }
  }

  TrySendNextPackets();

  return CommandChannel::EventCallbackResult::kContinue;
}

size_t AclDataChannelImpl::GetNumFreePacketsForLinkType(
    LinkType link_type) const {
  if (link_type == LinkType::kACL || IsBrEdrBufferShared()) {
    PW_DCHECK(bredr_buffer_info_.max_num_packets() >=
              *num_pending_bredr_packets_);
    return bredr_buffer_info_.max_num_packets() - *num_pending_bredr_packets_;
  } else if (link_type == LinkType::kLE) {
    PW_DCHECK(le_buffer_info_.max_num_packets() >= *num_pending_le_packets_);
    return le_buffer_info_.max_num_packets() - *num_pending_le_packets_;
  }
  return 0;
}

void AclDataChannelImpl::DecrementPendingPacketsForLinkType(LinkType link_type,
                                                            size_t count) {
  if (link_type == LinkType::kACL || IsBrEdrBufferShared()) {
    PW_DCHECK(*num_pending_bredr_packets_ >= count);
    *num_pending_bredr_packets_.Mutable() -= count;
  } else if (link_type == LinkType::kLE) {
    PW_DCHECK(*num_pending_le_packets_ >= count);
    *num_pending_le_packets_.Mutable() -= count;
  }

  if (*num_pending_bredr_packets_ == 0 && *num_pending_le_packets_ == 0) {
    wake_lease_.reset();
  }
}

void AclDataChannelImpl::IncrementPendingPacketsForLinkType(
    LinkType link_type) {
  if (link_type == LinkType::kACL || IsBrEdrBufferShared()) {
    *num_pending_bredr_packets_.Mutable() += 1;
    PW_DCHECK(*num_pending_bredr_packets_ <=
              bredr_buffer_info_.max_num_packets());
  } else if (link_type == LinkType::kLE) {
    *num_pending_le_packets_.Mutable() += 1;
    PW_DCHECK(*num_pending_le_packets_ <= le_buffer_info_.max_num_packets());
  }

  if (!wake_lease_) {
    pw::Result<pw::bluetooth_sapphire::Lease> lease =
        PW_SAPPHIRE_ACQUIRE_LEASE(wake_lease_provider_, "AclDataChannel");
    if (lease.ok()) {
      wake_lease_ = std::move(lease.value());
    }
  }
}

void AclDataChannelImpl::OnRxPacket(pw::span<const std::byte> buffer) {
  PW_CHECK(rx_callback_);

  if (buffer.size() < sizeof(hci_spec::ACLDataHeader)) {
    // TODO(fxbug.dev/42179582): Handle these types of errors by signaling
    // Transport.
    bt_log(ERROR,
           "hci",
           "malformed packet - expected at least %zu bytes, got %zu",
           sizeof(hci_spec::ACLDataHeader),
           buffer.size());
    return;
  }

  auto view = pw::bluetooth::emboss::MakeAclDataFrameHeaderView(buffer.data(),
                                                                buffer.size());
  if (!view.Ok()) {
    bt_log(ERROR, "hci", "Failed to parse ACL data header");
    return;
  }

  const uint16_t data_total_length = view.data_total_length().Read();

  if (data_total_length > hci_spec::kMaxACLPayloadSize) {
    hci_spec::ConnectionHandle handle = view.handle().Read();

    bt_log(ERROR,
           "hci",
           "ACL packet payload too large (%hu bytes > %zu), dropping and "
           "disconnecting peer",
           data_total_length,
           hci_spec::kMaxACLPayloadSize);

    auto status_cb = [](auto, const EventPacket& event) {
      PW_DCHECK(event.event_code() == hci_spec::kCommandStatusEventCode);
      HCI_IS_ERROR(event, TRACE, "hci", "ignoring disconnection failure");
    };

    auto disconn =
        CommandPacket::New<pw::bluetooth::emboss::DisconnectCommandWriter>(
            hci_spec::kDisconnect);
    auto params = disconn.view_t();
    params.connection_handle().Write(handle);
    params.reason().Write(
        pw::bluetooth::emboss::StatusCode::
            REMOTE_DEVICE_TERMINATED_CONNECTION_LOW_RESOURCES);

    transport_->command_channel()
        ->SendCommand(std::move(disconn),
                      std::move(status_cb),
                      hci_spec::kCommandStatusEventCode)
        .IgnoreError();
    return;
  }

  if (buffer.size() < sizeof(hci_spec::ACLDataHeader) + data_total_length) {
    bt_log(ERROR,
           "hci",
           "malformed packet - buffer size (%zu) too small for payload length "
           "(%hu)",
           buffer.size(),
           data_total_length);
    return;
  }

  ACLDataPacketPtr packet = ACLDataPacket::New(data_total_length);
  const size_t packet_size =
      sizeof(hci_spec::ACLDataHeader) + data_total_length;
  packet->mutable_view()->mutable_data().Write(
      reinterpret_cast<const uint8_t*>(buffer.data()), packet_size);
  packet->InitializeFromBuffer();

  last_packet_times_[packet->connection_handle()] = dispatcher_.now();

  {
    TRACE_DURATION("bluetooth", "AclDataChannelImpl->rx_callback_");
    rx_callback_(std::move(packet));
  }
}

CommandChannel::EventCallbackResult
AclDataChannelImpl::DataBufferOverflowCallback(const EventPacket& event) {
  const auto params =
      event.view<pw::bluetooth::emboss::DataBufferOverflowEventView>();

  // Internal buffer state must be invalid and no further transmissions are
  // possible.
  PW_CRASH("controller data buffer overflow event received (link type: %s)",
           hci_spec::LinkTypeToString(params.ll_type().Read()));

  return CommandChannel::EventCallbackResult::kContinue;
}

void AclDataChannelImpl::ResetRoundRobinIterators() {
  current_bredr_link_ = registered_connections_.begin();

  // If the BR/EDR buffer isn't shared, we need to do extra work to ensure
  // |current_bredr_link_| is initialized to a link of BR/EDR type. The same
  // applies for |current_le_link_|.
  if (!IsBrEdrBufferShared()) {
    current_le_link_ = registered_connections_.begin();

    IncrementRoundRobinIterator(current_bredr_link_, bt::LinkType::kACL);
    IncrementRoundRobinIterator(current_le_link_, bt::LinkType::kLE);
  }
}

}  // namespace bt::hci
