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

#include "pw_bluetooth_proxy/internal/acl_data_channel.h"

#include <cstdint>
#include <mutex>
#include <optional>

#include "lib/stdcompat/utility.h"
#include "pw_assert/check.h"
#include "pw_bluetooth/emboss_util.h"
#include "pw_bluetooth/hci_data.emb.h"
#include "pw_containers/algorithm.h"  // IWYU pragma: keep
#include "pw_log/log.h"
#include "pw_multibuf/multibuf.h"
#include "pw_span/cast.h"
#include "pw_status/status.h"

namespace pw::bluetooth::proxy {

void AclDataChannel::HandleAclFromController(H4PacketWithHci&& h4_packet) {
  if (!HandleAclData(Direction::kFromController, h4_packet.GetHciSpan())) {
    hci_transport_.SendToHost(std::move(h4_packet));
  }
}

void AclDataChannel::HandleAclFromHost(H4PacketWithH4&& h4_packet) {
  if (!HandleAclData(Direction::kFromHost, h4_packet.GetHciSpan())) {
    Result<emboss::AclDataFrameHeaderView> acl_view =
        MakeEmbossView<emboss::AclDataFrameHeaderView>(h4_packet.GetHciSpan());
    if (!acl_view.ok()) {
      PW_LOG_ERROR("Buffer is too small for ACL header in HandleAclFromHost");
      hci_transport_.SendToController(std::move(h4_packet));
      return;
    }
    uint16_t handle = acl_view->handle().Read();

    std::optional<AclTransportType> transport;
    std::optional<H4PacketWithH4> packet_to_send_directly;

    {
      std::lock_guard lock(connection_mutex_);
      AclConnection* connection_ptr = FindAclConnection(handle);
      if (!connection_ptr) {
        packet_to_send_directly = std::move(h4_packet);
      } else {
        std::lock_guard credit_lock(credit_mutex_);
        Credits& credits = LookupCredits(connection_ptr->transport());

        if (!credits.dynamic_sharing()) {
          connection_ptr->RecordPacket(PacketSource::kHost);
          packet_to_send_directly = std::move(h4_packet);
        } else {
          transport = connection_ptr->transport();
          if (h4_packet.HasReleaseFn()) {
            if (connection_ptr->queue().try_push(std::move(h4_packet))) {
              credits.IncrementTotalQueuedPackets();
            } else {
              PW_LOG_ERROR("Dropping H4 packet from host: unable to queue");
            }
          } else {
            Result<H4PacketWithH4> owned_h4_packet_result =
                H4PacketWithH4::CopyFrom(allocator_, h4_packet);
            if (!owned_h4_packet_result.ok()) {
              PW_LOG_ERROR(
                  "Dropping H4 packet from host: unable to allocate storage to "
                  "queue");
            } else {
              if (connection_ptr->queue().try_push(
                      std::move(owned_h4_packet_result.value()))) {
                credits.IncrementTotalQueuedPackets();
              } else {
                PW_LOG_ERROR("Dropping H4 packet from host: unable to queue");
              }
            }
          }
        }
      }
    }

    if (packet_to_send_directly.has_value()) {
      hci_transport_.SendToController(
          std::move(packet_to_send_directly.value()));
    }

    if (transport.has_value()) {
      DrainDynamicQuota(*transport);
    }
  }
}

AclDataChannel::AclConnection::AclConnection(AclTransportType transport,
                                             uint16_t connection_handle,
                                             pw::Allocator& allocator)
    : transport_(transport),
      connection_handle_(connection_handle),
      num_proxy_pending_packets_(0),
      num_host_pending_packets_(0),
      queue_(allocator) {
  PW_LOG_INFO(
      "btproxy: AclConnection ctor. transport_: %u, connection_handle_: %#x",
      cpp23::to_underlying(transport_),
      connection_handle_);
}

void AclDataChannel::AclConnection::RecordPacket(
    AclDataChannel::PacketSource source) {
  if (source == PacketSource::kProxy) {
    ++num_proxy_pending_packets_;
  } else {
    ++num_host_pending_packets_;
  }
}

AclDataChannel::PacketSource AclDataChannel::AclConnection::ReclaimCredit() {
  if (num_proxy_pending_packets_ > 0 && num_host_pending_packets_ > 0) {
    if (last_reclaimed_ == PacketSource::kHost) {
      last_reclaimed_ = PacketSource::kProxy;
      --num_proxy_pending_packets_;
      return AclDataChannel::PacketSource::kProxy;
    } else {
      last_reclaimed_ = AclDataChannel::PacketSource::kHost;
      --num_host_pending_packets_;
      return AclDataChannel::PacketSource::kHost;
    }
  }

  if (num_proxy_pending_packets_ > 0) {
    last_reclaimed_ = AclDataChannel::PacketSource::kProxy;
    --num_proxy_pending_packets_;
    return AclDataChannel::PacketSource::kProxy;
  }

  if (num_host_pending_packets_ > 0) {
    last_reclaimed_ = AclDataChannel::PacketSource::kHost;
    --num_host_pending_packets_;
    return AclDataChannel::PacketSource::kHost;
  }

  // If we have no record of pending packets, default to host.
  last_reclaimed_ = AclDataChannel::PacketSource::kHost;
  return AclDataChannel::PacketSource::kHost;
}

AclDataChannel::SendCredit::SendCredit(SendCredit&& other) {
  *this = std::move(other);
}

AclDataChannel::SendCredit& AclDataChannel::SendCredit::operator=(
    SendCredit&& other) {
  if (this != &other) {
    transport_ = other.transport_;
    relinquish_fn_ = std::move(other.relinquish_fn_);
    other.relinquish_fn_ = nullptr;
  }
  return *this;
}

AclDataChannel::SendCredit::~SendCredit() {
  if (relinquish_fn_) {
    relinquish_fn_(transport_);
  }
}

AclDataChannel::SendCredit::SendCredit(
    AclTransportType transport,
    Function<void(AclTransportType transport)>&& relinquish_fn)
    : transport_(transport), relinquish_fn_(std::move(relinquish_fn)) {}

void AclDataChannel::SendCredit::MarkUsed() {
  PW_CHECK(relinquish_fn_);
  relinquish_fn_ = nullptr;
}

void AclDataChannel::Reset() {
  {
    std::lock_guard lock(credit_mutex_);
    // Reset credits first so no packets queued in signaling channels can be
    // sent.
    le_credits_.Reset();
    br_edr_credits_.Reset();
  }
  {
    std::lock_guard lock(connection_mutex_);
    acl_connections_.clear();
  }
  {
    std::lock_guard lock(delegates_mutex_);
    connection_delegates_.clear();
  }
}

Status AclDataChannel::RegisterConnection(uint16_t handle,
                                          ConnectionDelegate& delegate) {
  std::lock_guard lock(delegates_mutex_);
  auto result = connection_delegates_.try_emplace(handle, &delegate);
  if (!result.has_value()) {
    return Status::ResourceExhausted();
  }
  if (!result->second) {
    return Status::AlreadyExists();
  }
  return OkStatus();
}

Status AclDataChannel::UnregisterConnection(uint16_t handle) {
  std::lock_guard lock(delegates_mutex_);
  auto iter = connection_delegates_.find(handle);
  if (iter == connection_delegates_.end()) {
    return Status::NotFound();
  }
  connection_delegates_.erase(iter);
  return OkStatus();
}

void AclDataChannel::Credits::Reset() {
  if (auto* static_credits = std::get_if<Static>(&data_)) {
    static_credits->proxy_max = 0;
  }
  pending_ = 0;
  controller_max_packets_ = 0;
  if (auto* dynamic_credits = std::get_if<Dynamic>(&data_)) {
    dynamic_credits->total_queued_packets = 0;
  }
}

uint16_t AclDataChannel::Credits::Reserve(uint16_t controller_max) {
  if (Initialized()) {
    uint16_t proxy_max = 0;
    if (auto* static_credits = std::get_if<Static>(&data_)) {
      proxy_max = static_credits->proxy_max;
    }
    PW_LOG_WARN(
        "AclDataChannel already initialized (proxy_max=%u); "
        "ignoring duplicate buffer-size event",
        proxy_max);
    // Return the previously-computed host share so the rewritten event sent on
    // to the host stays consistent.
    return controller_max > proxy_max ? controller_max - proxy_max : 0;
  }

  controller_max_packets_ = controller_max;

  if (auto* static_credits = std::get_if<Static>(&data_)) {
    static_credits->proxy_max =
        std::min(controller_max, static_credits->to_reserve);
    const uint16_t host_max = controller_max - static_credits->proxy_max;

    PW_LOG_INFO(
        "Bluetooth Proxy reserved %d ACL data credits. Passed %d on to host.",
        static_credits->proxy_max,
        host_max);

    if (static_credits->proxy_max < static_credits->to_reserve) {
      PW_LOG_ERROR(
          "Only was able to reserve %d acl data credits rather than the "
          "configured %d from the controller provided's data credits of %d. ",
          static_credits->proxy_max,
          static_credits->to_reserve,
          controller_max);
    }

    return host_max;
  }

  if (std::holds_alternative<Dynamic>(data_)) {
    // Dynamic sharing
    PW_LOG_INFO(
        "Bluetooth Proxy reserved 0 ACL data credits. Passed %d on to host.",
        controller_max);
  }

  return controller_max;
}

Status AclDataChannel::Credits::MarkPending(uint16_t num_credits,
                                            PacketSource source) {
  if (num_credits > Available()) {
    return Status::ResourceExhausted();
  }

  pending_ += num_credits;

  if (auto* dynamic = std::get_if<Dynamic>(&data_)) {
    dynamic->drain_turn = (source == PacketSource::kProxy)
                              ? PacketSource::kHost
                              : PacketSource::kProxy;
  }

  return OkStatus();
}

void AclDataChannel::Credits::MarkCompleted(uint16_t num_credits) {
  if (num_credits > pending_) {
    PW_LOG_ERROR("Tried to mark completed more packets than were pending.");
    pending_ = 0;
  } else {
    pending_ -= num_credits;
  }
}

AclDataChannel::Credits& AclDataChannel::LookupCredits(
    AclTransportType transport) {
  switch (transport) {
    case AclTransportType::kBrEdr:
      return br_edr_credits_;
    case AclTransportType::kLe:
      return le_credits_;
    default:
      PW_CHECK(false, "Invalid transport type");
  }
}

const AclDataChannel::Credits& AclDataChannel::LookupCredits(
    AclTransportType transport) const {
  switch (transport) {
    case AclTransportType::kBrEdr:
      return br_edr_credits_;
    case AclTransportType::kLe:
      return le_credits_;
    default:
      PW_CHECK(false, "Invalid transport type");
  }
}

void AclDataChannel::ProcessReadBufferSizeCommandCompleteEvent(
    emboss::ReadBufferSizeCommandCompleteEventWriter read_buffer_event) {
  {
    std::lock_guard lock(credit_mutex_);
    const uint16_t controller_max =
        read_buffer_event.total_num_acl_data_packets().Read();
    const uint16_t host_max = br_edr_credits_.Reserve(controller_max);
    read_buffer_event.total_num_acl_data_packets().Write(host_max);
    max_acl_data_packet_length_ =
        read_buffer_event.acl_data_packet_length().Read();
  }

  on_tx_credits_fn_();
}

template <class EventT>
void AclDataChannel::ProcessSpecificLEReadBufferSizeCommandCompleteEvent(
    EventT read_buffer_event) {
  {
    std::lock_guard lock(credit_mutex_);
    const uint16_t controller_max =
        read_buffer_event.total_num_le_acl_data_packets().Read();
    // TODO: https://pwbug.dev/380316252 - Support shared buffers.
    const uint16_t host_max = le_credits_.Reserve(controller_max);
    read_buffer_event.total_num_le_acl_data_packets().Write(host_max);
    max_le_acl_data_packet_length_ =
        read_buffer_event.le_acl_data_packet_length().Read();
  }

  const uint16_t le_acl_data_packet_length =
      read_buffer_event.le_acl_data_packet_length().Read();

  // Core Spec v6.0 Vol 4, Part E, Section 7.8.2:  A value of 0 means "No
  // dedicated LE Buffer exists".
  // TODO: https://pwbug.dev/380316252 - Support shared buffers.
  if (le_acl_data_packet_length == 0) {
    PW_LOG_ERROR(
        "Controller shares data buffers between BR/EDR and LE transport, which "
        "is not yet supported. So channels on LE transport will not be "
        "functional.");
  }

  // Send packets that may have queued before we acquired any LE ACL credits.
  on_tx_credits_fn_();
}

template void
AclDataChannel::ProcessSpecificLEReadBufferSizeCommandCompleteEvent<
    emboss::LEReadBufferSizeV1CommandCompleteEventWriter>(
    emboss::LEReadBufferSizeV1CommandCompleteEventWriter read_buffer_event);

template void
AclDataChannel::ProcessSpecificLEReadBufferSizeCommandCompleteEvent<
    emboss::LEReadBufferSizeV2CommandCompleteEventWriter>(
    emboss::LEReadBufferSizeV2CommandCompleteEventWriter read_buffer_event);

bool AclDataChannel::HandleNumberOfCompletedPacketsEvent(
    H4PacketWithHci& h4_packet) {
  Result<emboss::NumberOfCompletedPacketsEventWriter> nocp_event =
      MakeEmbossWriter<emboss::NumberOfCompletedPacketsEventWriter>(
          h4_packet.GetHciSpan());
  if (!nocp_event.ok()) {
    PW_LOG_ERROR(
        "Buffer is too small for NUMBER_OF_COMPLETED_PACKETS event. So "
        "will not process.");
    return true;
  }

  bool should_send_to_host = false;
  bool credits_reclaimed = false;
  bool static_proxy_credits_reclaimed = false;
  {
    std::lock_guard lock(connection_mutex_);
    for (uint8_t i = 0; i < nocp_event->num_handles().Read(); ++i) {
      uint16_t handle = nocp_event->nocp_data()[i].connection_handle().Read();
      uint16_t num_completed_packets =
          nocp_event->nocp_data()[i].num_completed_packets().Read();

      if (num_completed_packets == 0) {
        continue;
      }

      credits_reclaimed = true;

      AclConnection* connection_ptr = FindAclConnection(handle);
      if (!connection_ptr) {
        // Credits for connection we are not tracking or closed connection, so
        // should pass event on to host.
        should_send_to_host = true;
        continue;
      }

      // Handover credits to the sources that sent them.
      uint16_t proxy_reclaimed = 0;
      uint16_t host_reclaimed = 0;

      for (uint16_t j = 0; j < num_completed_packets; ++j) {
        if (connection_ptr->ReclaimCredit() == PacketSource::kProxy) {
          ++proxy_reclaimed;
        } else {
          ++host_reclaimed;
        }
      }

      if (proxy_reclaimed > 0) {
        std::lock_guard credit_lock(credit_mutex_);
        Credits& credits = LookupCredits(connection_ptr->transport());
        credits.MarkCompleted(proxy_reclaimed);
        if (!credits.dynamic_sharing()) {
          static_proxy_credits_reclaimed = true;
        }
      }

      nocp_event->nocp_data()[i].num_completed_packets().Write(host_reclaimed);
      if (host_reclaimed > 0) {
        std::lock_guard credit_lock(credit_mutex_);
        Credits& credits = LookupCredits(connection_ptr->transport());
        if (credits.dynamic_sharing()) {
          credits.MarkCompleted(host_reclaimed);
        }
        // Connection has credits remaining for host, so should pass event on to
        // host.
        should_send_to_host = true;
      }
    }
  }

  if (static_proxy_credits_reclaimed) {
    on_tx_credits_fn_();
  }

  if (credits_reclaimed) {
    for (auto transport : {AclTransportType::kLe, AclTransportType::kBrEdr}) {
      DrainDynamicQuota(transport);
    }
  }

  return should_send_to_host;
}

void AclDataChannel::DrainDynamicQuota(AclTransportType transport) {
  std::lock_guard drain_lock(draining_mutex_);

  bool proxy_tried_and_empty = false;
  bool host_tried_and_empty = false;
  bool should_run_proxy_turn = false;

  while (true) {
    std::optional<H4PacketWithH4> packet_to_send;
    {
      std::lock_guard lock(connection_mutex_);
      std::lock_guard credit_lock(credit_mutex_);
      Credits& credits = LookupCredits(transport);

      if (!credits.dynamic_sharing() || !credits.Available() ||
          (proxy_tried_and_empty && host_tried_and_empty)) {
        break;
      }

      if (credits.drain_turn() == PacketSource::kProxy) {
        should_run_proxy_turn = true;
        proxy_tried_and_empty = true;
        credits.PassTurn();
      } else {
        AclConnection* connection_to_drain = FindConnectionToDrain(transport);
        if (connection_to_drain) {
          packet_to_send = DequeueHostPacket(connection_to_drain, credits);
          host_tried_and_empty = false;
          proxy_tried_and_empty = false;
        } else {
          host_tried_and_empty = true;
          credits.PassTurn();
        }
      }
    }

    if (packet_to_send.has_value()) {
      hci_transport_.SendToController(std::move(packet_to_send.value()));
    }
  }
  if (should_run_proxy_turn) {
    on_tx_credits_fn_();
  }
}

void AclDataChannel::HandleConnectionCompleteEvent(uint16_t connection_handle,
                                                   AclTransportType transport) {
  if (CreateAclConnection(connection_handle, transport) ==
      Status::ResourceExhausted()) {
    PW_LOG_ERROR(
        "Could not track connection like requested. Max connections "
        "reached.");
  }
}

void AclDataChannel::ProcessDisconnectionCompleteEvent(
    uint16_t connection_handle, emboss::StatusCode reason) {
  pw::DynamicQueue<H4PacketWithH4> packets_to_drop(allocator_);
  {
    std::lock_guard lock(connection_mutex_);

    AclConnection* connection_ptr = FindAclConnection(connection_handle);
    if (!connection_ptr) {
      PW_LOG_INFO(
          "btproxy: Viewed disconnect (reason: %#.2hhx) for unacquired "
          "connection %#x.",
          cpp23::to_underlying(reason),
          connection_handle);
      return;
    }
    PW_LOG_INFO("Proxy viewed disconnect (reason: %#.2hhx) for connection %#x.",
                cpp23::to_underlying(reason),
                connection_handle);

    if (connection_ptr->num_proxy_pending_packets() > 0 ||
        connection_ptr->num_host_pending_packets() > 0) {
      PW_LOG_WARN(
          "Connection %#x is disconnecting with packets in flight. Releasing "
          "associated credits.",
          connection_handle);
      std::lock_guard credit_lock(credit_mutex_);
      LookupCredits(connection_ptr->transport())
          .MarkCompleted(connection_ptr->num_proxy_pending_packets());
      LookupCredits(connection_ptr->transport())
          .MarkCompleted(connection_ptr->num_host_pending_packets());
    }

    {
      std::lock_guard credit_lock(credit_mutex_);
      Credits& credits = LookupCredits(connection_ptr->transport());
      if (credits.dynamic_sharing()) {
        credits.DecrementTotalQueuedPackets(connection_ptr->queue().size());
      }
    }

    packets_to_drop.swap(connection_ptr->queue());

    acl_connections_.erase(connection_ptr);
  }

  // Important to drop outside of connection lock since freeing packets will
  // result in calls into proxy again.
  packets_to_drop.clear();
}

bool AclDataChannel::HasSendAclCapability(AclTransportType transport) const {
  std::lock_guard lock(credit_mutex_);
  return LookupCredits(transport).HasSendCapability();
}

uint16_t AclDataChannel::GetNumFreeAclPackets(
    AclTransportType transport) const {
  std::lock_guard lock(credit_mutex_);
  return LookupCredits(transport).Remaining();
}

std::optional<AclDataChannel::SendCredit> AclDataChannel::ReserveSendCredit(
    AclTransportType transport) {
  std::lock_guard lock(credit_mutex_);
  Credits& credits = LookupCredits(transport);

  // In dynamic sharing mode, if it is the host's turn to drain and the host has
  // packets queued, the proxy must yield. This prevents the proxy from greedily
  // consuming all available credits during its `on_tx_credits_fn_` callback
  // when it is supposed to alternate with the host queue.
  if (credits.dynamic_sharing() &&
      credits.drain_turn() == PacketSource::kHost &&
      credits.total_queued_packets() > 0) {
    return std::nullopt;
  }

  if (const auto status = credits.MarkPending(1, PacketSource::kProxy);
      !status.ok()) {
    return std::nullopt;
  }

  return SendCredit(transport, [this](AclTransportType t) {
    std::lock_guard fn_lock(credit_mutex_);
    LookupCredits(t).MarkCompleted(1);
  });
}

pw::Status AclDataChannel::SendAcl(H4PacketWithH4&& h4_packet,
                                   SendCredit&& credit) {
  std::lock_guard lock(connection_mutex_);
  Result<emboss::AclDataFrameHeaderView> acl_view =
      MakeEmbossView<emboss::AclDataFrameHeaderView>(h4_packet.GetHciSpan());
  if (!acl_view.ok()) {
    PW_LOG_ERROR("An invalid ACL packet was provided. So will not send.");
    return pw::Status::InvalidArgument();
  }
  uint16_t handle = acl_view->handle().Read();

  AclConnection* connection_ptr = FindAclConnection(handle);
  if (!connection_ptr) {
    PW_LOG_ERROR("Tried to send ACL packet on unregistered connection.");
    return pw::Status::NotFound();
  }

  if (connection_ptr->transport() != credit.transport_) {
    PW_LOG_WARN("Provided credit for wrong transport. So will not send.");
    return pw::Status::InvalidArgument();
  }

  credit.MarkUsed();

  connection_ptr->RecordPacket(PacketSource::kProxy);

  hci_transport_.SendToController(std::move(h4_packet));
  return pw::OkStatus();
}

Status AclDataChannel::CreateAclConnection(uint16_t connection_handle,
                                           AclTransportType transport) {
  std::lock_guard lock(connection_mutex_);
  AclConnection* connection_it = FindAclConnection(connection_handle);
  if (connection_it) {
    return Status::AlreadyExists();
  }
  if (acl_connections_.full()) {
    PW_LOG_ERROR(
        "btproxy: Attempt to create new AclConnection when acl_connections_ is"
        "already full. connection_handle: %#x",
        connection_handle);
    return Status::ResourceExhausted();
  }

  uint16_t max_packets = 0;
  {
    std::lock_guard credit_lock(credit_mutex_);
    max_packets = LookupCredits(transport).controller_max_packets();
  }

  acl_connections_.emplace_back(transport,
                                /*connection_handle=*/connection_handle,
                                allocator_);

  if (max_packets > 0) {
    if (!acl_connections_.back().queue().try_reserve(max_packets)) {
      PW_LOG_ERROR("Failed to reserve capacity for ACL connection %#x queue.",
                   connection_handle);
    }
  }

  return OkStatus();
}

AclDataChannel::AclConnection* AclDataChannel::FindConnectionToDrain(
    AclTransportType transport) {
  for (auto& connection : acl_connections_) {
    if (connection.transport() == transport && !connection.queue().empty()) {
      return &connection;
    }
  }
  return nullptr;
}

std::optional<H4PacketWithH4> AclDataChannel::DequeueHostPacket(
    AclConnection* connection, Credits& credits) {
  if (connection->queue().empty()) {
    return std::nullopt;
  }
  H4PacketWithH4 queued = std::move(connection->queue().front());
  connection->queue().pop();
  credits.DecrementTotalQueuedPackets(1);

  credits.MarkPending(1, PacketSource::kHost).IgnoreError();
  connection->RecordPacket(PacketSource::kHost);
  return queued;
}

AclDataChannel::AclConnection* AclDataChannel::FindAclConnection(
    uint16_t connection_handle) {
  AclConnection* connection_it = containers::FindIf(
      acl_connections_, [connection_handle](const AclConnection& connection) {
        return connection.connection_handle() == connection_handle;
      });
  return connection_it == acl_connections_.end() ? nullptr : connection_it;
}

bool AclDataChannel::HandleAclData(Direction direction,
                                   pw::span<uint8_t> buffer) {
  // This function returns whether or not the frame was handled here.
  // * Return true if the frame was handled by the proxy and should _not_ be
  //   passed on to the other side (Host/Controller).
  // * Return false if the frame was _not_ handled by the proxy and should be
  //   passed on to the other side (Host/Controller).
  static constexpr bool kHandled = true;
  static constexpr bool kUnhandled = false;

  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(buffer);
  if (!acl.ok()) {
    PW_LOG_ERROR("Buffer is too small for ACL header");
    return kUnhandled;
  }

  const uint16_t handle = acl->header().handle().Read();

  ConnectionDelegate::HandleAclDataReturn result{};
  {
    std::lock_guard lock(delegates_mutex_);
    auto iter = connection_delegates_.find(handle);
    if (iter != connection_delegates_.end()) {
      result = iter->second->HandleAclData(direction, *acl);
    }
  }

  if (result.recombined_buffer.has_value()) {
    auto contiguous = result.recombined_buffer->ContiguousSpan().value();
    pw::span<uint8_t> h4_span = span_cast<uint8_t>(contiguous);
    // Send onward to its final destination.
    switch (direction) {
      case Direction::kFromController: {
        H4PacketWithHci h4_packet{h4_span};
        hci_transport_.SendToHost(std::move(h4_packet));
        break;
      }
      case Direction::kFromHost: {
        H4PacketWithH4 h4_packet{h4_span};
        {
          std::lock_guard lock(connection_mutex_);
          AclConnection* connection_ptr = FindAclConnection(handle);
          if (connection_ptr) {
            connection_ptr->RecordPacket(PacketSource::kHost);
          }
        }
        hci_transport_.SendToController(std::move(h4_packet));
        break;
      }
    }

    // We still return kHandled here since the last fragment packet was already
    // passed on to the host as part of the recombined H4 packet.
    return kHandled;
  }

  return result.handled;
}

std::optional<uint16_t> AclDataChannel::MaxDataPacketLengthForTransport(
    AclTransportType transport) const {
  std::lock_guard lock(credit_mutex_);
  switch (transport) {
    case AclTransportType::kBrEdr:
      return max_acl_data_packet_length_;
    case AclTransportType::kLe:
      return max_le_acl_data_packet_length_;
    default:
      return std::nullopt;
  }
}

bool AclDataChannel::HasAclConnection(uint16_t connection_handle) {
  std::lock_guard lock(connection_mutex_);
  return FindAclConnection(connection_handle) != nullptr;
}

}  // namespace pw::bluetooth::proxy
