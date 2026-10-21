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

#include <memory>
#include <utility>

#include "pw_assert/check.h"
#include "pw_async2/await.h"
#include "pw_log/log.h"
#include "pw_rpc2/internal/connection_task.h"
#include "pw_rpc2/reader.h"

namespace pw::rpc2::internal {

async2::Poll<RpcResult<ConstBuf>> ReadFutureBase::PendRaw(async2::Context& cx) {
  PW_CHECK(is_pendable());
  if (call().is_attached()) {
    PW_CHECK(cx.IsSameDispatcher(call().connection_task()),
             "pw_rpc2 futures must be polled from the designated pw_rpc2 "
             "dispatcher");
  }

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
      return async2::Ready(RpcResult<ConstBuf>(RpcStatus::EndOfStream()));
    }
    if (call().is_completed() && !call().completion_status().ok()) {
      return async2::Ready(RpcResult<ConstBuf>(call().completion_status()));
    }
    return async2::Ready(RpcResult<ConstBuf>(RpcStatus::EndOfStream()));
  }

  return async2::Ready(RpcResult<ConstBuf>(std::move(*val)));
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
  if (was_open && is_attached()) {
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
  if (!is_closed() || !is_attached()) {
    return;
  }
  if (role() == EndpointRole::kClient) {
    DetachFromConnection();
  } else {
    // The server connection polls this call for retirement, so it is what has
    // to notice. It may already be awake --- `CloseConnection()` completes
    // every call and then wakes once --- but waking an awake task is free.
    connection_task_->Wake();
  }
}

Call::Call(ConnectionTask& connection_task, uint32_t call_id)
    : connection_task_(&connection_task), call_id_(call_id) {
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
    ClearConnectionTask();
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

void Call::ClearConnectionTask() {
  if (is_attached()) {
    Allocator& alloc = connection_task_->allocator();
    ClearReadFlag<kAttached>();
    allocator_ = &alloc;
  }
}

void Call::DetachFromConnection() {
  if (is_attached()) {
    connection_task_->UnregisterCall(*this);
    ClearConnectionTask();
  }
}

void Call::pw_recycle() {
  DetachFromConnection();
  Allocator& alloc = *allocator_;
  std::destroy_at(this);
  alloc.Deallocate(this);
}

// Only send error packets for calls that have started. Server calls are always
// started.
void Call::QueueError(ProtocolError error) {
  if (is_attached() && HasWriteFlag<kStarted>()) {
    connection_task_->QueueError(call_id_, error);
  }
}

void Call::QueueFinish() {
  if (is_attached() && HasWriteFlag<kStarted>()) {
    connection_task_->QueueFinish(call_id_);
  }
}

RpcResult<transport::ReserveWriteFuture> Call::ReserveWrite(
    size_t header_size, size_t max_payload_size) {
  if (is_completed()) {
    return completion_status_.ok() ? RpcStatus::WriteClosed()
                                   : completion_status_;
  }
  // A call is only detached without completing once both of its directions
  // have closed, so its write side is closed.
  if (!is_attached()) {
    return RpcStatus::WriteClosed();
  }
  if (HasWriteFlag<kClosed>() || HasWriteFlag<kTerminalWritePending>() ||
      HasWriteFlag<kWriterDropped>()) {
    return RpcStatus::WriteClosed();
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
    return RpcStatus::MessageTooLarge();
  }
  return connection.ReserveWrite(header_size + max_payload_size);
}

void Call::QueueWriterDropPacket() {
  if (HasWriteFlag<kErrorOnWriterDrop>()) {
    QueueError(ProtocolError::kDroppedWithoutResponse);
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
  if (is_attached() && HasWriteFlag<kStarted>()) {
    connection_task_->QueueCancel(call_id_);
  }
  // Complete locally too: the peer does not answer a cancel, so a local read
  // would otherwise wait for a response that will never come.
  Complete(RpcStatus::Cancelled());
}

void Call::CloseWriteOnRetire() {
  PW_DASSERT(role() == EndpointRole::kServer);
  if (is_write_closed()) {
    return;
  }
  PW_LOG_WARN(
      "Call %u: method returned without finishing; terminating the RPC with an "
      "error",
      static_cast<unsigned>(call_id_));
  QueueError(ProtocolError::kMethodReturnedWithoutFinishing);
  CloseWrite();
}

size_t Call::max_write_size_bytes() const {
  if (!is_attached()) {
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

void Call::OnError(RpcStatus status) {
  ClearReadFlag<kPeerClosed>();
  Complete(status);
}

void Call::Complete(RpcStatus status) {
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

IntrusivePtr<Call> ClientCall::Create(ConnectionTask& connection_task,
                                      uint32_t call_id) {
  return IntrusivePtr<Call>(
      connection_task.allocator().New<ClientCall>(connection_task, call_id));
}

}  // namespace pw::rpc2::internal
