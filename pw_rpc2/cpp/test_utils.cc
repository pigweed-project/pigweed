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

#include "pw_rpc2/internal/test_utils.h"

#include <cstddef>
#include <cstring>
#include <mutex>
#include <optional>
#include <utility>

#include "pw_assert/check.h"
#include "pw_async2/future_task.h"
#include "pw_log/log.h"
#include "pw_rpc2/internal/call.h"
#include "pw_rpc2/internal/handshake.h"
#include "pw_rpc2/internal/packet.h"
#include "pw_rpc2/internal/packet_testing.h"

namespace pw::rpc2::test {

MockConnection::MockConnection(pw::Allocator& allocator)
    : ReliableDatagramSocketImpl(allocator, 1500), alloc_(allocator) {}

async2::Poll<pw::ConstBuf> MockConnection::DoRead() {
  if (!read_queue_.empty()) {
    pw::ConstBuf buf = std::move(read_queue_.front());
    read_queue_.pop_front();
    return async2::Ready(std::move(buf));
  }
  return async2::Pending();
}

void MockConnection::SetBlockReserveWrite(bool block) {
  std::lock_guard lock(*this);
  block_reserve_write_ = block;
  if (!block_reserve_write_) {
    WakeAllWriters();
  }
}

void MockConnection::UnblockReserveWrite(size_t size) {
  (void)size;
  std::lock_guard lock(*this);
  block_reserve_write_ = false;
  WakeOneWriter();
}

std::optional<transport::WriteReservation> MockConnection::DoTryReserveWrite(
    size_t size) {
  if (block_reserve_write_) {
    return std::nullopt;
  }
  pw::Buf buf = pw::Buf::TryAllocate(alloc_, size);
  if (buf.empty()) {
    return std::nullopt;
  }
  return CreateReservation(std::move(buf));
}

void MockConnection::DoClose() { MarkClosed(); }

void MockConnection::PushNextRead(pw::Buf buf) {
  std::lock_guard lock(*this);
  PW_CHECK(!read_queue_.full(),
           "MockConnection ingress queue is full (%zu packets). The connection "
           "task has not consumed queued packets; run the dispatcher between "
           "pushes, or raise MockConnection::kMaxReadQueueCapacity.",
           kMaxReadQueueCapacity);
  read_queue_.push_back(std::move(buf));
  WakeReader();
}

void MockConnection::SetNextRead(pw::Buf buf) { PushNextRead(std::move(buf)); }

void MockConnection::TryResolveRead() {
  std::lock_guard lock(*this);
  if (!read_queue_.empty()) {
    WakeReader();
  }
}

bool MockConnection::DoCommitWrite(pw::Buf&& buffer) {
  PW_CHECK(!written_packets_.full(),
           "MockConnection has recorded its maximum of %zu written packets. "
           "Silently dropping packets would corrupt response indices; clear "
           "them with clear_written() or raise "
           "MockConnection::kMaxWrittenPackets.",
           kMaxWrittenPackets);
  commit_count_++;
  written_packets_.push_back(std::move(buffer));
  return true;
}

void MockConnection::DoCancelWrite(pw::Buf&& buffer) { (void)buffer; }

void PushSynAck(pw::Allocator& allocator, MockConnection& connection) {
  pw::Buf buffer = pw::Buf::TryAllocate(
      allocator, internal::HandshakePacket::kWireSizeBytes);
  PW_CHECK(!buffer.empty(), "Could not allocate a handshake packet");
  auto encoded =
      internal::HandshakePacket(internal::HandshakePacket::Type::kSynAck)
          .Encode(std::move(buffer));
  PW_CHECK_OK(encoded.status());
  connection.PushNextRead(std::move(*encoded));
}

Client ConnectMockClient(async2::RunnableDispatcher& dispatcher,
                         pw::Allocator& allocator,
                         transport::ReliableDatagramSocket connection,
                         MockConnection& raw_connection) {
  async2::FutureTask connect(
      Client::Connect(dispatcher, allocator, std::move(connection)));
  dispatcher.Post(connect);
  dispatcher.RunUntilStalled();

  PushSynAck(allocator, raw_connection);
  dispatcher.RunUntilStalled();

  PW_CHECK(connect.has_value(), "The client did not complete the handshake");
  PW_CHECK_OK(connect.value().status());
  return std::move(*connect.value());
}

MockPeer MakeMockPeer(async2::RunnableDispatcher& dispatcher,
                      pw::Allocator& allocator) {
  auto [connection, raw] = MakeMockConnection(allocator);
  // Constructing from an EstablishedConnection selects ConnectionTask's
  // kActive path, so no handshake packets are ever written.
  internal::EstablishedConnection established{connection,
                                              internal::HandshakeInfo{}};
  return MockPeer(internal::CallAccess::CreateClient<Client>(
                      dispatcher, allocator, std::move(established)),
                  *raw,
                  dispatcher,
                  allocator);
}

internal::InboundPacket MockPeer::DecodeWrittenPacket(size_t index) const {
  auto packet = internal::InboundPacket::Decode(
      ConstBuf::Unowned(connection_->written_packet(index)));
  PW_CHECK(packet.ok(),
           "MockPeer could not decode written packet %zu (%s); it only "
           "understands RPC packets, so the connection must come from "
           "MakeMockPeer, which skips the handshake.",
           index,
           packet.status().str());
  return std::move(*packet);
}

void MockPeer::InjectResponseBytes(uint32_t call_id, ConstByteSpan payload) {
  Inject(internal::PacketFramer::FrameResponsePacket(
      *allocator_, call_id, payload));
}

void MockPeer::Inject(pw::Result<pw::Buf>&& packet) {
  PW_CHECK(packet.ok(),
           "MockPeer could not frame a packet (%s). The allocator is likely "
           "exhausted.",
           packet.status().str());
  // PushNextRead wakes the reader; running the dispatcher lets the client
  // consume the packet and resume whatever was awaiting it.
  connection_->PushNextRead(std::move(*packet));
  dispatcher_->RunUntilStalled();
}

PairedConnection::PairedConnection(pw::Allocator& allocator)
    : ReliableDatagramSocketImpl(allocator, 1500), alloc_(allocator) {}

PairedConnection::~PairedConnection() {
  std::lock_guard lock(*this);
  if (peer_ != nullptr) {
    peer_->ClearPeer();
    peer_ = nullptr;
  }
}

void PairedConnection::SetPeer(PairedConnection* peer) {
  std::lock_guard lock(*this);
  peer_ = peer;
}

void PairedConnection::ClearPeer() {
  std::lock_guard lock(*this);
  peer_ = nullptr;
}

PairedConnection* PairedConnection::peer() const {
  std::lock_guard lock(*this);
  return peer_;
}

async2::Poll<pw::ConstBuf> PairedConnection::DoRead() {
  if (ReliableDatagramSocketImpl::is_closed()) {
    return async2::Ready(pw::ConstBuf());
  }
  if (!read_queue_.empty()) {
    pw::ConstBuf pkt = std::move(read_queue_.front());
    read_queue_.pop_front();
    return async2::Ready(std::move(pkt));
  }
  return async2::Pending();
}

void PairedConnection::SetBlockReserveWrite(bool block) {
  std::lock_guard lock(*this);
  block_reserve_write_ = block;
  if (!block_reserve_write_) {
    WakeAllWriters();
  }
}

void PairedConnection::UnblockReserveWrite(size_t size) {
  (void)size;
  std::lock_guard lock(*this);
  block_reserve_write_ = false;
  WakeOneWriter();
}

std::optional<transport::WriteReservation> PairedConnection::DoTryReserveWrite(
    size_t size) {
  if (ReliableDatagramSocketImpl::is_closed() || block_reserve_write_) {
    return std::nullopt;
  }
  pw::Buf buf = pw::Buf::TryAllocate(alloc_, size);
  if (buf.empty()) {
    return std::nullopt;
  }
  return CreateReservation(std::move(buf));
}

void PairedConnection::DoClose() {
  MarkClosed();
  read_queue_.clear();
  // WARNING: Cross-connection AB-BA deadlock hazard.
  // Calling peer_->SimulateDisconnect() while holding *this lock can lead to an
  // AB-BA deadlock if both peers close concurrently on different threads.
  if (peer_ != nullptr && !peer_->is_closed()) {
    peer_->SimulateDisconnect(OkStatus(), false);
  }
}

void PairedConnection::ReceivePacket(pw::ConstBuf packet) {
  std::lock_guard lock(*this);
  if (ReliableDatagramSocketImpl::is_closed()) {
    return;
  }
  if (!read_queue_.full()) {
    read_queue_.push_back(std::move(packet));
    WakeReader();
  } else {
    PW_LOG_WARN("PairedConnection read queue full, dropping packet");
  }
}

void PairedConnection::TryResolveRead() {
  std::lock_guard lock(*this);
  if (!read_queue_.empty()) {
    WakeReader();
  }
}

void PairedConnection::SimulateDisconnect(Status status, bool disconnect_peer) {
  (void)status;
  PairedConnection* peer_to_disconnect = nullptr;
  {
    std::lock_guard lock(*this);
    if (ReliableDatagramSocketImpl::is_closed()) {
      return;
    }
    MarkClosed();
    read_queue_.clear();
    if (disconnect_peer) {
      peer_to_disconnect = peer_;
    }
  }
  if (peer_to_disconnect != nullptr) {
    peer_to_disconnect->SimulateDisconnect(status, false);
  }
}

bool PairedConnection::DoCommitWrite(pw::Buf&& payload) {
  if (ReliableDatagramSocketImpl::is_closed()) {
    return false;
  }
  commit_count_++;
  last_written_buf_ = pw::Buf::TryAllocate(alloc_, payload.size());
  if (!last_written_buf_.empty()) {
    std::memcpy(last_written_buf_.data(), payload.data(), payload.size());
  }
  // WARNING: Cross-connection AB-BA deadlock hazard.
  // Calling peer_->ReceivePacket() while holding *this lock acquires peer_'s
  // lock while *this is locked. If both ends of a PairedConnection transmit
  // concurrently on different threads (Thread 1: locks ConnA -> attempts to
  // lock ConnB; Thread 2: locks ConnB -> attempts to lock ConnA), an AB-BA
  // deadlock will occur.
  if (peer_ != nullptr) {
    peer_->ReceivePacket(std::move(payload));
  }
  return true;
}

void PairedConnection::DoCancelWrite(pw::Buf&&) {}

}  // namespace pw::rpc2::test

namespace pw::rpc2::internal {

Result<pw::Buf> PacketFramer::FramePacket(pw::Allocator& allocator,
                                          PacketType type,
                                          uint32_t call_id,
                                          pw::ConstByteSpan payload) {
  PW_CHECK(!type.is_start() && !type.is_error(),
           "Packets of type 0x%02x have extra header fields",
           static_cast<unsigned>(type.bits()));
  return Frame(allocator,
               OutboundPacket(type, call_id, OutboundPacket::Fields()),
               payload);
}

Result<pw::Buf> PacketFramer::FrameStartPacket(pw::Allocator& allocator,
                                               PacketType type,
                                               uint32_t call_id,
                                               uint32_t service_id,
                                               uint32_t method_id,
                                               pw::ConstByteSpan payload) {
  PW_CHECK(type.is_start(),
           "Packets of type 0x%02x do not start a call",
           static_cast<unsigned>(type.bits()));
  return Frame(
      allocator,
      OutboundPacket(
          type, call_id, OutboundPacket::Fields(service_id, method_id)),
      payload);
}

Result<pw::Buf> PacketFramer::Frame(pw::Allocator& allocator,
                                    const OutboundPacket& packet,
                                    pw::ConstByteSpan payload) {
  PW_CHECK(packet.type().has_payload() || payload.empty(),
           "A packet of type 0x%02x cannot carry a payload",
           static_cast<unsigned>(packet.type().bits()));
  const size_t packet_size = packet.payload_offset() + payload.size();
  pw::Buf buffer = pw::Buf::TryAllocate(allocator, packet_size);
  if (buffer.empty()) {
    return Status::ResourceExhausted();
  }
  if (!payload.empty()) {
    std::memcpy(buffer.data() + packet.payload_offset(),
                payload.data(),
                payload.size());
  }
  return packet.Encode(std::move(buffer), payload.size());
}

}  // namespace pw::rpc2::internal
