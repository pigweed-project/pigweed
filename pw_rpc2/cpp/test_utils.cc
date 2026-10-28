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
#include "pw_rpc2/internal/call.h"
#include "pw_rpc2/internal/handshake.h"
#include "pw_rpc2/internal/method.h"
#include "pw_rpc2/internal/packet.h"
#include "pw_rpc2/internal/packet_testing.h"
#include "pw_rpc2/internal/server_call.h"

namespace pw::rpc2::test {

MockConnection::MockConnection(pw::Allocator& allocator)
    : ReliableSocketImpl(allocator, 1500), alloc_(allocator) {}

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
  if (buf == nullptr) {
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
  pw::Buf buffer =
      pw::Buf::Allocate(allocator, internal::HandshakePacket::kWireSizeBytes);
  pw::Buf encoded =
      internal::HandshakePacket(internal::HandshakePacket::Type::kSynAck)
          .Encode(std::move(buffer));
  PW_CHECK(encoded != nullptr, "Could not encode a handshake packet");
  connection.PushNextRead(std::move(encoded));
}

Client ConnectMockClient(async2::RunnableDispatcher& dispatcher,
                         pw::Allocator& allocator,
                         transport::ReliableSocket connection,
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

IntrusivePtr<internal::Call> StartServerCall(
    internal::ServerConnectionTask& connection_task, uint32_t call_id) {
  static constexpr internal::Method kMethod(
      1, MethodType::kBidirectionalStreaming, 0, nullptr);

  internal::ServerCall* const call =
      internal::ServerCall::Allocate(connection_task, call_id, kMethod);
  PW_CHECK_NOTNULL(call);
  // Finish as if the method had returned, leaving its handles to the test.
  call->FinishInvocation();
  return call->shared_call();
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
  PW_CHECK(packet != nullptr,
           "MockPeer could not decode written packet %zu; it only "
           "understands RPC packets, so the connection must come from "
           "MakeMockPeer, which skips the handshake.",
           index);
  return packet;
}

void MockPeer::InjectResponseBytes(uint32_t call_id, ConstByteSpan payload) {
  Inject(internal::PacketFramer::FrameResponsePacket(
      *allocator_, call_id, payload));
}

void MockPeer::Inject(pw::Buf&& packet) {
  PW_CHECK(packet != nullptr,
           "MockPeer could not frame a packet. The allocator is likely "
           "exhausted.");
  // PushNextRead wakes the reader; running the dispatcher lets the client
  // consume the packet and resume whatever was awaiting it.
  connection_->PushNextRead(std::move(packet));
  dispatcher_->RunUntilStalled();
}

void MockPeer::InjectStreamMessageBytes(uint32_t call_id,
                                        ConstByteSpan payload) {
  Inject(internal::PacketFramer::FrameServerMessagePacket(
      *allocator_, call_id, payload));
}

void MockPeer::InjectStreamFinish(uint32_t call_id) {
  Inject(internal::PacketFramer::FrameServerFinishPacket(*allocator_, call_id));
}

namespace {

// True if `packet` is a stream message the client wrote on `call_id`. Start
// packets are excluded: the only start packets with a payload carry a unary or
// server-streaming request, which `Invocation::request()` decodes.
bool IsClientStreamMessage(const internal::InboundPacket& packet,
                           uint32_t call_id) {
  return packet.call_id() == call_id && !packet.type().is_server() &&
         !packet.type().is_start() && packet.type().has_payload();
}

}  // namespace

size_t MockPeer::ClientStreamMessageCount(uint32_t call_id) const {
  size_t count = 0;
  for (size_t i = 0; i < connection_->written_packet_count(); ++i) {
    if (IsClientStreamMessage(DecodeWrittenPacket(i), call_id)) {
      ++count;
    }
  }
  return count;
}

RpcResult<ConstByteSpan> MockPeer::ClientStreamMessage(uint32_t call_id,
                                                       size_t index) const {
  size_t seen = 0;
  for (size_t i = 0; i < connection_->written_packet_count(); ++i) {
    const internal::InboundPacket packet = DecodeWrittenPacket(i);
    if (!IsClientStreamMessage(packet, call_id)) {
      continue;
    }
    if (seen == index) {
      // The packet is an unowned view of the recorded buffer, so the payload
      // outlives `packet`.
      return packet.payload();
    }
    ++seen;
  }
  return RpcStatus::EndOfStream();
}

bool MockPeer::ClientStreamFinished(uint32_t call_id) const {
  for (size_t i = 0; i < connection_->written_packet_count(); ++i) {
    const internal::InboundPacket packet = DecodeWrittenPacket(i);
    if (packet.call_id() == call_id && !packet.type().is_server() &&
        !packet.type().is_start() &&
        packet.type().close_mode() == internal::CloseMode::kStreamEnd) {
      return true;
    }
  }
  return false;
}

}  // namespace pw::rpc2::test

namespace pw::rpc2::internal {

pw::Buf PacketFramer::FramePacket(pw::Allocator& allocator,
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

pw::Buf PacketFramer::FrameStartPacket(pw::Allocator& allocator,
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

pw::Buf PacketFramer::Frame(pw::Allocator& allocator,
                            const OutboundPacket& packet,
                            pw::ConstByteSpan payload) {
  PW_CHECK(packet.type().has_payload() || payload.empty(),
           "A packet of type 0x%02x cannot carry a payload",
           static_cast<unsigned>(packet.type().bits()));
  const size_t packet_size = packet.payload_offset() + payload.size();
  pw::Buf buffer = pw::Buf::TryAllocate(allocator, packet_size);
  if (buffer == nullptr) {
    return nullptr;
  }
  if (!payload.empty()) {
    std::memcpy(buffer.data() + packet.payload_offset(),
                payload.data(),
                payload.size());
  }
  return packet.Encode(std::move(buffer), payload.size());
}

}  // namespace pw::rpc2::internal
