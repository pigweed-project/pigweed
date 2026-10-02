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

#include "pw_rpc2/internal/connection_task.h"

#include <cstddef>
#include <optional>
#include <utility>

#include "pw_assert/check.h"
#include "pw_async2/await.h"
#include "pw_bytes/span.h"
#include "pw_log/log.h"
#include "pw_rpc2/internal/call.h"
#include "pw_rpc2/internal/packet.h"

namespace pw::rpc2::internal {
namespace {

/// Encodes an outbound packet's header into a transport reservation and commits
/// it.
void EncodeAndCommitHeader(transport::WriteReservation& reservation,
                           const OutboundPacket& packet) {
  auto encode_res =
      packet.EncodeHeader(ByteSpan(reservation.data(), reservation.size()));
  PW_DCHECK(encode_res.ok());
  static_cast<void>(reservation.Commit(*encode_res));
}

}  // namespace

async2::Poll<Status> SendControlPacketFuture::Pend(async2::Context& cx) {
  PW_ASSERT(is_pendable());

  PW_AWAIT(auto write_res, reserve_fut_, cx);
  mark_complete();

  if (!write_res.has_value()) {
    return async2::Ready(Status::Unavailable());
  }

  EncodeAndCommitHeader(*write_res, packet_);
  return async2::Ready(OkStatus());
}

ConnectionTask::ConnectionTask(transport::ReliableDatagramSocket connection,
                               Allocator& allocator,
                               EndpointRole role)
    : connection_(std::move(connection)),
      allocator_(allocator),
      state_(State::kHandshaking),
      role_(role),
      pending_control_packets_(allocator) {
  PW_DCHECK(connection_);
}

ConnectionTask::ConnectionTask(EstablishedConnection established_connection,
                               Allocator& allocator,
                               EndpointRole role)
    : connection_(std::move(established_connection.connection)),
      allocator_(allocator),
      state_(State::kActive),
      role_(role),
      handshake_info_(established_connection.info),
      pending_control_packets_(allocator) {
  PW_DCHECK(connection_);
}

ConnectionTask::~ConnectionTask() {
  Teardown();

  while (!calls_.empty()) {
    calls_.front().DetachFromConnection();
  }
}

void ConnectionTask::QueueControlPacket(const OutboundPacket& packet) {
  if (state_ == State::kClosed) {
    return;
  }
  PW_DASSERT(connection_);

  // 1. Fast path: an immediate synchronous reservation, but only when nothing
  //    is already queued. Writing directly while the queue is non-empty would
  //    let this packet overtake one the dispatcher has not sent yet, and
  //    control packets are terminal for their calls, so their order is
  //    observable by the peer.
  if (pending_control_packets_.empty() && !send_control_future_.is_pendable()) {
    const size_t size = packet.payload_offset();
    std::optional<transport::WriteReservation> write_res =
        connection_.TryReserveWrite(size);
    if (write_res.has_value()) {
      EncodeAndCommitHeader(*write_res, packet);
      return;
    }
  }

  // 2. Slow path: queue the packet until the dispatcher can write it.
  if (pending_control_packets_.try_push_back(packet)) {
    Wake();
    return;
  }

  // 3. The allocator is exhausted. This packet is terminal for its call, so
  //    silently dropping it would leave the peer waiting forever. Close the
  //    connection immediately, which completes every call on it and forces the
  //    peer to notice.
  PW_LOG_ERROR(
      "Call %u: out of memory queueing control packet (type=0x%02x), "
      "closing connection",
      static_cast<unsigned>(packet.call_id()),
      static_cast<unsigned>(packet.type().bits()));
  CloseConnection(Status::ResourceExhausted());
}

void ConnectionTask::CloseConnection(Status status) {
  if (state_ == State::kClosed) {
    return;
  }
  PW_DASSERT(connection_);
  set_state(State::kClosed);
  close_status_ = status;
  pending_control_packets_.reset();
  connection_.Close();

  // Advance `it` before `Complete()` because completing a client call detaches
  // it from `calls_`.
  for (auto it = calls_.begin(); it != calls_.end();) {
    Call& call = *it++;
    if (!call.is_closed()) {
      call.Complete(status);
    }
  }

  // Wake this task so that it observes the closed state and retires. This has
  // to happen here rather than in the loop above: a connection with no open
  // calls would otherwise never be woken, and one with many would be woken
  // once per call.
  Wake();
}

Call* ConnectionTask::FindCallById(uint32_t call_id) {
  for (auto& call : calls_) {
    if (call.call_id() == call_id) {
      return &call;
    }
  }
  return nullptr;
}

void ConnectionTask::StoreWaker(async2::Context& cx) {
  PW_DASSERT(!is_closed());
  PW_ASYNC_STORE_WAKER(cx, waker_, "waiting for connection activity");
}

bool ConnectionTask::PendPacket(async2::Context& cx,
                                InboundPacket* incoming_request) {
  PW_DASSERT(state() == State::kActive);

  // Stage 1: Outgoing control-packet egress.
  bool progressed = ProcessOutgoingControlPackets(cx);
  if (is_closed()) {
    return true;
  }

  // Stage 2: Dispatch stashed ingress packet.
  progressed |= DispatchPendingIngressPacket(cx);

  // Stage 3: Read and dispatch incoming packet from transport.
  progressed |= ReadPacketFromConnection(cx, incoming_request);
  return progressed;
}

void ConnectionTask::FinishHandshake(EstablishedConnection&& established) {
  PW_DASSERT(state_ == State::kHandshaking);
  PW_DASSERT(established.connection);
  // The only reassignment of `connection_` after construction.
  connection_ = std::move(established.connection);
  handshake_info_ = established.info;
  set_state(State::kActive);
}

bool ConnectionTask::ProcessOutgoingControlPackets(async2::Context& cx) {
  PW_DASSERT(state() == State::kActive);
  PW_DASSERT(connection_);

  if (!send_control_future_.is_pendable()) {
    if (pending_control_packets_.empty()) {
      return false;
    }
    OutboundPacket packet = pending_control_packets_.front();
    pending_control_packets_.pop_front();
    if (pending_control_packets_.empty() &&
        pending_control_packets_.capacity() > kMaxIdleControlPacketCapacity) {
      pending_control_packets_.reset();
    }
    send_control_future_ = SendControlPacketFuture(connection_, packet);
  }

  auto poll = send_control_future_.Pend(cx);
  if (!poll.IsReady()) {
    return false;
  }

  const Status send_status = *poll;
  send_control_future_ = SendControlPacketFuture();

  if (!send_status.ok()) {
    // If sending fails, the underlying transport is closed.
    PW_LOG_WARN("Failed to send control packet (%s), closing connection",
                send_status.str());
    CloseConnection(send_status);
  }
  return true;
}

bool ConnectionTask::DispatchPendingIngressPacket(async2::Context& cx) {
  if (pending_dispatch_packet_ == nullptr) {
    return false;
  }
  if (Call* call = FindCallById(pending_dispatch_packet_.call_id());
      call != nullptr) {
    if (!DeliverToCall(*call, pending_dispatch_packet_, cx)) {
      return false;
    }
  }
  pending_dispatch_packet_ = nullptr;
  return true;
}

bool ConnectionTask::ReadPacketFromConnection(async2::Context& cx,
                                              InboundPacket* incoming_request) {
  PW_DASSERT(state() == State::kActive);
  PW_DASSERT(connection_);

  if (pending_dispatch_packet_ != nullptr) {
    return false;
  }

  if (!read_future_.is_pendable()) {
    read_future_ = connection_.Read();
  }

  auto poll = read_future_.Pend(cx);
  if (!poll.IsReady()) {
    return false;
  }

  auto& result = *poll;
  if (result == nullptr) {
    CloseConnection(Status::Cancelled());
    return true;
  }

  ConstBuf pkt_buf = std::move(result);
  read_future_ = transport::ReadFuture();

  auto decode_result = InboundPacket::Decode(std::move(pkt_buf));
  if (!decode_result.ok()) {
    PW_LOG_ERROR("Received malformed RPC packet (%s), closing connection",
                 decode_result.status().str());
    CloseConnection(Status::DataLoss());
    return true;
  }

  InboundPacket packet = std::move(decode_result.value());
  // Replies to packets that cannot be delivered tell the peer to stop sending
  // for that call. A terminal packet never gets a reply: the peer has already
  // forgotten the call, and replying to an error with an error could bounce
  // between the endpoints forever.
  if (!packet.type().is_for(role_)) {
    PW_LOG_WARN(
        "Call %u: received packet of type 0x%02x for wrong endpoint role, "
        "dropping",
        static_cast<unsigned>(packet.call_id()),
        static_cast<unsigned>(packet.type().bits()));
    if (!packet.type().is_terminal()) {
      QueueError(packet.call_id(),
                 ProtocolStatus::kReceivedPacketForWrongEndpoint);
    }
    if (Call* call = FindCallById(packet.call_id()); call != nullptr) {
      call->Complete(ToStatus(ProtocolStatus::kReceivedPacketForWrongEndpoint));
    }
    return true;
  }

  if (Call* call = FindCallById(packet.call_id()); call != nullptr) {
    if (packet.type().is_start()) {
      // Only a server receives start packets. The client reused the ID of a
      // call that is still running, so it has lost track of that call; end
      // it, and tell the client that neither call will proceed.
      PW_LOG_WARN(
          "Call %u: received start packet for already active call, cancelling",
          static_cast<unsigned>(packet.call_id()));
      QueueError(packet.call_id(), ProtocolStatus::kCancelled);
      call->Complete(Status::Cancelled());
      return true;
    }
    if (!DeliverToCall(*call, packet, cx)) {
      pending_dispatch_packet_ = std::move(packet);
    }
    return true;
  }

  // Nothing is registered for this call ID, so the packet either opens a new
  // call or is stray.
  if (!packet.type().is_start()) {
    if (packet.type().is_terminal()) {
      PW_LOG_DEBUG("Call %u: dropping terminal packet 0x%02x for closed call",
                   static_cast<unsigned>(packet.call_id()),
                   static_cast<unsigned>(packet.type().bits()));
      return true;
    }
    PW_LOG_WARN(
        "Call %u: received stray packet of type 0x%02x for unknown or closed "
        "call, cancelling it",
        static_cast<unsigned>(packet.call_id()),
        static_cast<unsigned>(packet.type().bits()));
    QueueCancel(packet.call_id());
    return true;
  }

  PW_DASSERT(incoming_request != nullptr);
  *incoming_request = std::move(packet);
  return true;
}

bool ConnectionTask::DeliverToCall(Call& call,
                                   InboundPacket& packet,
                                   async2::Context& cx) {
  const PacketType type = packet.type();
  PW_DASSERT(!type.is_start());
  const CloseMode close_mode = type.close_mode();

  // A unary or client-streaming call is answered by exactly one packet, which
  // carries the response and ends the RPC. Any other packet (except an error)
  // means the server treats the method as server or bidirectional streaming,
  // so nothing in it can be trusted to be the response.
  const bool is_single_response =
      type.has_payload() && close_mode == CloseMode::kOkTerminal;
  if (call.expects_single_response() && !is_single_response &&
      close_mode != CloseMode::kErrorTerminal) {
    PW_LOG_WARN(
        "Call %u: received unexpected packet type 0x%02x for single-response "
        "call",
        static_cast<unsigned>(packet.call_id()),
        static_cast<unsigned>(type.bits()));
    // Tell the server why the call failed, unless it already ended the RPC.
    // A server that has ended the RPC has forgotten the call, so the error
    // would only arrive as a stray packet.
    if (close_mode != CloseMode::kOkTerminal) {
      QueueError(packet.call_id(), ProtocolStatus::kMethodTypeMismatch);
    }
    call.OnError(Status::FailedPrecondition());
    return true;
  }

  if (close_mode == CloseMode::kErrorTerminal) {
    call.OnError(ToStatus(packet.error()));
    return true;
  }

  // The message is delivered before any close in the same packet is applied,
  // so the reader drains it before observing the end of the stream. If the
  // slot is not available yet, nothing has been applied and the whole packet
  // is retried later.
  if (type.has_payload()) {
    if (call.peer_ended_stream()) {
      PW_LOG_WARN(
          "Call %u: received message after the peer ended its stream, "
          "dropping",
          static_cast<unsigned>(packet.call_id()));
    } else {
      if (!call.ReserveMessageSlot(cx)) {
        return false;
      }
      call.OnMessage(std::move(packet).TakePayload());
    }
  }

  switch (close_mode) {
    case CloseMode::kOpen:
      break;
    case CloseMode::kStreamEnd:
      call.OnPeerStreamEnd();
      break;
    case CloseMode::kOkTerminal:
      call.Complete(OkStatus());
      break;
    case CloseMode::kErrorTerminal:
      break;
  }
  return true;
}

}  // namespace pw::rpc2::internal
