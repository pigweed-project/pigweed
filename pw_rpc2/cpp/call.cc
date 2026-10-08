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

#include "pw_rpc2/internal/call.h"

#include <utility>

#include "pw_assert/check.h"
#include "pw_async2/await.h"
#include "pw_log/log.h"
#include "pw_rpc2/internal/connection_task.h"
#include "pw_rpc2/reader.h"

namespace pw::rpc2::internal {

async2::Poll<Result<ConstBuf>> ReadFutureBase::PendRaw(async2::Context& cx) {
  PW_CHECK(is_pendable());

  PW_AWAIT(std::optional<ConstBuf> val, receive_fut_, cx);
  // The read is no longer outstanding, so let the next one start even if
  // this future is kept alive by its owner.
  ReleaseRead();
  mark_complete();

  // `receive_fut_` is only pendable if this future was created from a call,
  // so the call is guaranteed to be non-null here.
  if (!val.has_value()) {
    // A server's reader ends cleanly once the client half-closes its stream,
    // whatever happens to the call afterwards. A client's reader ends with the
    // RPC, since a server that half-closes still sends a terminal packet, so
    // it reports the call's final status.
    if (call().role() == EndpointRole::kServer && call().peer_ended_stream()) {
      return async2::Ready(Result<ConstBuf>(Status::OutOfRange()));
    }
    if (call().is_completed() && !call().completion_status().ok()) {
      return async2::Ready(Result<ConstBuf>(call().completion_status()));
    }
    return async2::Ready(Result<ConstBuf>(Status::OutOfRange()));
  }

  return async2::Ready(Result<ConstBuf>(std::move(*val)));
}

void ReadFutureBase::PendAndDeserialize(async2::Context& cx,
                                        void* result_out,
                                        DeserializeFn deserialize) {
  auto poll = PendRaw(cx);
  if (poll.IsPending()) {
    return;
  }
  if (poll->ok()) {
    deserialize(result_out, ConstByteSpan(**poll));
  } else {
    deserialize(result_out, poll->status());
  }
}

// Defined out of line so that the precondition can be checked with
// `PW_DCHECK`, which carries a message but may not be used in a header.
async2::ReceiveFuture<ConstBuf> Call::ClaimRead() {
  PW_DCHECK(!is_read_claimed(),
            "Only one read may be outstanding on an RPC call at a time");
  PW_DCHECK(!is_reader_dropped(),
            "Cannot read from a call after its reader has closed");
  SetReadFlag<kClaimed>();
  return receiver_.Receive();
}

void Call::ReleaseRead() {
  ClearReadFlag<kClaimed>();
  if (is_reader_dropped()) {
    CloseRead();
  }
}

void Call::CloseWrite() {
  ClearWriteFlag<kTerminalWritePending>();
  SetWriteFlag<kClosed>();
  if (role() == EndpointRole::kServer) {
    // Stop accepting messages, but leave the receiver connected so that the
    // method can still drain any message that was already queued.
    send_state_.Reset();
    sender_.Disconnect();
  }
  NotifyIfClosed();
}

void Call::CloseRead() {
  SetReadFlag<kReaderDropped>();
  const bool was_open = sender_.is_open() || receiver_.is_open();
  receiver_.Disconnect();
  sender_.Disconnect();
  send_state_.Reset();
  if (was_open && connection_task_ != nullptr) {
    connection_task_->Wake();
  }
  if (role() == EndpointRole::kClient && !is_completed()) {
    // The client stopped reading before the RPC ended, so nothing will
    // observe its outcome. Cancel it rather than leaving the server to run it
    // to completion, which for a server stream may be never.
    Cancel();
  }
  NotifyIfClosed();
}

void Call::CloseReadOnReaderDestroy() {
  SetReadFlag<kReaderDropped>();
  if (!is_read_claimed()) {
    CloseRead();
  }
}

void Call::NotifyIfClosed() {
  if (!is_closed() || HasReadFlag<kClosedNotified>()) {
    return;
  }
  SetReadFlag<kClosedNotified>();
  if (role() == EndpointRole::kClient) {
    DetachFromConnection();
  } else if (connection_task_ != nullptr) {
    // The server connection polls this call for retirement, so it is what has
    // to notice. It may already be awake --- `CloseConnection()` completes
    // every call and then wakes once --- but waking an awake task is free.
    connection_task_->Wake();
  }
}

Call::Call(ConnectionTask& connection_task,
           uint32_t call_id,
           Allocator* allocator)
    : connection_task_(&connection_task),
      allocator_(allocator),
      call_id_(call_id) {
  if (connection_task.role() == EndpointRole::kServer) {
    SetWriteFlag<kServerCall>();
    SetWriteFlag<kStarted>();
  }
  InitChannel();
}

void Call::InitChannel() {
  auto [handle, sender, receiver] = async2::CreateSpscChannel(storage_);
  sender_ = std::move(sender);
  receiver_ = std::move(receiver);
  if (connection_task_->is_closed()) {
    completion_status_ = connection_task_->close_status();
    SetReadFlag<kCompleted>();
    SetWriteFlag<kClosed>();
    connection_task_ = nullptr;
    sender_.Disconnect();
  } else {
    connection_task_->RegisterCall(*this);
  }
}

Call::~Call() {
  DetachFromConnection();
  sender_.Disconnect();
  receiver_.Disconnect();
  send_state_.Reset();
}

void Call::DetachFromConnection() {
  if (connection_task_ != nullptr) {
    connection_task_->UnregisterCall(*this);
    connection_task_ = nullptr;
  }
}

// Only send error packets for calls that have started. Server calls are always
// started.
void Call::QueueError(ProtocolStatus error) {
  if (connection_task_ != nullptr && HasWriteFlag<kStarted>()) {
    connection_task_->QueueError(call_id_, error);
  }
}

void Call::QueueFinish() {
  if (connection_task_ != nullptr && HasWriteFlag<kStarted>()) {
    connection_task_->QueueFinish(call_id_);
  }
}

Result<transport::ReserveWriteFuture> Call::ReserveWrite(
    size_t header_size, size_t max_payload_size) {
  if (is_completed()) {
    return completion_status_.ok() ? Status::FailedPrecondition()
                                   : completion_status_;
  }
  // The connection task owns the connection handle and may reassign it during
  // the handshake, so it is read here rather than cached at construction.
  if (connection_task_ == nullptr) {
    return Status::Unavailable();
  }
  if (HasWriteFlag<kClosed>() || HasWriteFlag<kTerminalWritePending>() ||
      HasWriteFlag<kWriterDropped>()) {
    return Status::FailedPrecondition();
  }
  transport::ReliableDatagramSocket& connection =
      connection_task_->connection();
  // The transport asserts on requests larger than it can write, so reject
  // them here instead. Compare against the remaining space rather than adding
  // the sizes, which could overflow.
  const size_t max_size = connection.max_write_message_size_bytes();
  if (header_size > max_size || max_payload_size > max_size - header_size) {
    PW_LOG_WARN(
        "Call %u: %u-byte header + %u-byte payload exceeds the %u-byte "
        "transport limit",
        static_cast<unsigned>(call_id_),
        static_cast<unsigned>(header_size),
        static_cast<unsigned>(max_payload_size),
        static_cast<unsigned>(max_size));
    return Status::ResourceExhausted();
  }
  return connection.ReserveWrite(header_size + max_payload_size);
}

void Call::QueueWriterDropPacket() {
  if (HasWriteFlag<kCancelOnWriterDrop>()) {
    QueueError(ProtocolStatus::kDroppedWithoutResponse);
  } else {
    QueueFinish();
  }
}

void Call::FlushDeferredWriterDrop() {
  if (HasWriteFlag<kWriterDropped>() && !HasWriteFlag<kInvoking>() &&
      !HasWriteFlag<kTerminalWritePending>() && !is_write_closed()) {
    QueueWriterDropPacket();
    CloseWrite();
  }
}

void Call::AbandonTerminalWrite() {
  ClearWriteFlag<kTerminalWritePending>();
  FlushDeferredWriterDrop();
}

void Call::Cancel() {
  if (is_completed()) {
    return;
  }
  // A server that has sent its terminal packet has ended the RPC, and the
  // client has forgotten the call. A cancel would only arrive as a stray.
  if (role() == EndpointRole::kServer && is_write_closed()) {
    return;
  }
  // A client call whose start packet was never sent is unknown to the server,
  // so there is nobody to tell.
  if (connection_task_ != nullptr && HasWriteFlag<kStarted>()) {
    connection_task_->QueueCancel(call_id_);
  }
  // Complete locally too: the peer does not answer a cancel, so a local read
  // would otherwise wait for a response that will never come.
  Complete(Status::Cancelled());
}

void Call::CloseWriteOnRetire() {
  PW_DASSERT(role() == EndpointRole::kServer);
  if (is_write_closed()) {
    return;
  }
  const bool single_response = HasWriteFlag<kCancelOnWriterDrop>();
  PW_LOG_WARN(
      "Call %u: method finished without sending a terminal packet, most "
      "likely because a %s escaped it; ending the RPC with an error",
      static_cast<unsigned>(call_id_),
      single_response ? "UnaryWriter" : "Writer");
  QueueError(single_response ? ProtocolStatus::kDroppedWithoutResponse
                             : ProtocolStatus::kCancelled);
  CloseWrite();
}

size_t Call::max_write_size_bytes() const {
  if (connection_task_ == nullptr) {
    return 0;
  }
  return connection_task_->connection().max_write_message_size_bytes();
}

void Call::CloseOnWriterDestroy() {
  SetWriteFlag<kWriterDropped>();
  FlushDeferredWriterDrop();
}

void Call::FinishInvocation() {
  ClearWriteFlag<kInvoking>();
  FlushDeferredWriterDrop();
}

bool Call::ReserveMessageSlot(async2::Context& cx) {
  if (!sender_.is_open()) {
    // Trailing messages are discarded rather than queued, so no slot is
    // needed and delivery never stalls on a closed reader.
    return true;
  }
  if (send_state_.empty()) {
    send_state_ = sender_.ReserveSend();
  }
  if (!send_state_.Advance(cx)) {
    return false;
  }
  if (!send_state_->has_value()) {
    // The reader disconnected while the slot was pending; drop the state so
    // that this call discards the payload as any other closed reader would.
    send_state_.Reset();
    sender_.Disconnect();
  }
  return true;
}

void Call::OnMessage(ConstBuf payload) {
  if (!sender_.is_open()) {
    PW_LOG_DEBUG("Call %u: received message after read stream closed, dropping",
                 static_cast<unsigned>(call_id_));
    return;
  }
  PW_DCHECK(send_state_.has_value() && send_state_->has_value(),
            "OnMessage called without an active SendReservation; call "
            "ReserveMessageSlot() first");
  auto reservation = send_state_.Take();
  reservation->Commit(std::move(payload));
}

void Call::OnPeerStreamEnd() {
  SetReadFlag<kPeerClosed>();
  if (role() == EndpointRole::kClient) {
    // A server's half-close does not end the RPC. Keep the reader open until
    // the terminal packet that must follow, so that it observes the call's
    // final status rather than a premature end of stream. The C++ server does
    // not currently half-close without ending the RPC, but the protocol
    // allows it, so clients must handle it.
    return;
  }
  send_state_.Reset();
  sender_.Disconnect();
  NotifyIfClosed();
}

void Call::OnError(Status status) {
  ClearReadFlag<kPeerClosed>();
  Complete(status);
}

void Call::Complete(Status status) {
  // The first completion wins: a call that has already failed keeps its
  // original status even if the connection later closes underneath it.
  if (!is_completed()) {
    SetReadFlag<kCompleted>();
    completion_status_ = status;
  }
  send_state_.Reset();
  sender_.Disconnect();
  CloseWrite();
}

}  // namespace pw::rpc2::internal
