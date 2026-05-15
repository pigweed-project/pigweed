// Copyright 2020 The Pigweed Authors
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

#include "pw_rpc/internal/call.h"

#include "pw_assert/check.h"
#include "pw_bytes/span.h"
#include "pw_log/log.h"
#include "pw_preprocessor/util.h"
#include "pw_rpc/channel.h"
#include "pw_rpc/internal/encoding_buffer.h"
#include "pw_rpc/internal/endpoint.h"
#include "pw_rpc/internal/method.h"
#include "pw_rpc/internal/packet.pwpb.h"
#include "pw_status/status_with_size.h"
#include "pw_status/try.h"

// If the callback timeout is enabled, count the number of iterations of the
// waiting loop and crash if it exceeds PW_RPC_CALLBACK_TIMEOUT_TICKS.
#if PW_RPC_CALLBACK_TIMEOUT_TICKS > 0
#define PW_RPC_CHECK_FOR_DEADLOCK(timeout_source, call) \
  iterations += 1;                                      \
  PW_CHECK(                                                                  \
      iterations < PW_RPC_CALLBACK_TIMEOUT_TICKS,                            \
      "A callback for RPC %u:%08x/%08x has not finished after "              \
      PW_STRINGIFY(PW_RPC_CALLBACK_TIMEOUT_TICKS)                            \
      " ticks. This may indicate that an RPC callback attempted to "         \
      timeout_source                                                         \
      " its own call object, which is not permitted. Fix this condition or " \
      "change the value of PW_RPC_CALLBACK_TIMEOUT_TICKS to avoid this "     \
      "crash. See https://pigweed.dev/pw_rpc/cpp.html"                       \
      "#destructors-moves-wait-for-callbacks-to-complete for details.",      \
      static_cast<unsigned>((call).channel_id_),                             \
      static_cast<unsigned>((call).service_id_),                             \
      static_cast<unsigned>((call).method_id_))
#else
#define PW_RPC_CHECK_FOR_DEADLOCK(timeout_source, call) \
  static_cast<void>(iterations)
#endif  // PW_RPC_CALLBACK_TIMEOUT_TICKS > 0

namespace pw::rpc::internal {

using pwpb::PacketType;

Result<EncodedPacket> EncodeCallbackToPayloadBuffer(
    const Function<StatusWithSize(ByteSpan)>& callback)
    PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
  if (callback == nullptr) {
    return Status::InvalidArgument();
  }

  EncodingBuffer encoding_buffer;
  ByteSpan payload_buffer =
      encoding_buffer.AllocatePayloadBuffer(MaxSafePayloadSize());

  PW_TRY_ASSIGN(const size_t payload_size, callback(payload_buffer));

  return EncodedPacket(std::move(encoding_buffer), payload_size);
}

// Creates an active server-side Call.
Call::Call(const LockedCallContext& context, CallProperties properties)
    : Call(context.server().ClaimLocked(),
           context.call_id(),
           context.channel_id(),
           UnwrapServiceId(context.service().service_id()),
           context.method().id(),
           properties) {}

// Creates an active client-side call, assigning it a new ID.
Call::Call(LockedEndpoint& client,
           uint32_t channel_id,
           uint32_t service_id,
           uint32_t method_id,
           CallProperties properties)
    : Call(client,
           client.NewCallId(),
           channel_id,
           service_id,
           method_id,
           properties) {}

Call::Call(LockedEndpoint& endpoint_ref,
           uint32_t call_id,
           uint32_t channel_id,
           uint32_t service_id,
           uint32_t method_id,
           CallProperties properties)
    : endpoint_(&endpoint_ref),
      channel_id_(channel_id),
      id_(call_id),
      service_id_(service_id),
      method_id_(method_id),
      // Note: Bit kActive set to 1 and kClientRequestedCompletion is set to 0.
      state_(kActive),
      awaiting_cleanup_(OkStatus().code()),
      callbacks_executing_(0),
      properties_(properties) {
  PW_CHECK_UINT_NE(channel_id,
                   Channel::kUnassignedChannelId,
                   "Calls cannot be created with channel ID 0 "
                   "(Channel::kUnassignedChannelId)");
  endpoint().RegisterCall(*this);
}

void Call::DestroyServerCall() {
  RpcLockGuard lock;
  // Any errors are logged in Channel::Send.
  CloseAndSendResponseLocked(OkStatus()).IgnoreError();
  WaitForCallbacksToComplete();
  state_ |= kHasBeenDestroyed;
}

void Call::DestroyClientCall() {
  RpcLockGuard lock;
  CloseClientCall();
  WaitForCallbacksToComplete();
  state_ |= kHasBeenDestroyed;
}

void Call::WaitForCallbacksToComplete() {
  do {
    int iterations = 0;
    while (CallbacksAreRunning()) {
      PW_RPC_CHECK_FOR_DEADLOCK("destroy", *this);
      YieldRpcLock();
    }

  } while (CleanUpIfRequired());
}

void Call::MoveFrom(Call& other) {
  PW_DCHECK(!active_locked());
  PW_DCHECK(!awaiting_cleanup() && !other.awaiting_cleanup());

  // An active call with an executing callback cannot be moved. Derived call
  // classes must wait for callbacks to finish before calling MoveFrom.
  PW_DCHECK(!other.active_locked() || !other.CallbacksAreRunning());

  // Copy all members from the other call.
  endpoint_ = other.endpoint_;
  channel_id_ = other.channel_id_;
  id_ = other.id_;
  service_id_ = other.service_id_;
  method_id_ = other.method_id_;

  state_ = other.state_;

  // No need to move awaiting_cleanup_, since it is 0 in both calls here.

  properties_ = other.properties_;

  // callbacks_executing_ is not moved since it is associated with the object in
  // memory, not the call.

  on_error_ = std::move(other.on_error_);
  on_next_ = std::move(other.on_next_);

  if (other.active_locked()) {
    // Mark the other call inactive, unregister it, and register this one.
    other.MarkClosed();
    endpoint().UnregisterCall(other);
    endpoint().RegisterUniqueCall(*this);
  }
}

size_t Call::MaxWriteSizeBytes() const {
  RpcLockGuard lock;
  if (!active_locked()) {
    return 0u;
  }

  ChannelBase* channel = endpoint_->GetInternalChannel(channel_id_);
  if (channel == nullptr) {
    return 0u;
  }
  return channel->MaxWriteSizeBytes();
}

void Call::WaitUntilReadyForMove(Call& destination, Call& source) {
  do {
    // Wait for the source's callbacks to finish if it is active.
    int iterations = 0;
    while (source.active_locked() && source.CallbacksAreRunning()) {
      PW_RPC_CHECK_FOR_DEADLOCK("move", source);
      YieldRpcLock();
    }

    // At this point, no callbacks are running in the source call. If cleanup
    // is required for the destination call, perform it and retry since
    // cleanup releases and reacquires the RPC lock.
  } while (source.CleanUpIfRequired() || destination.CleanUpIfRequired());
}

void Call::CallOnError(Status error) {
  auto on_error_local = std::move(on_error_);

  CallbackStarted();

  rpc_lock().unlock();
  if (on_error_local) {
    on_error_local(error);
  }

  // This mutex lock could be avoided by making callbacks_executing_ atomic.
  RpcLockGuard lock;
  CallbackFinished();
}

bool Call::CleanUpIfRequired() PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
  if (!awaiting_cleanup()) {
    return false;
  }
  endpoint_->CleanUpCall(*this);
  rpc_lock().lock();
  return true;
}

Status Call::SendPacket(PacketType type, ConstByteSpan payload, Status status) {
  if (!active_locked()) {
    return Status::FailedPrecondition();
  }

  ChannelBase* channel = endpoint_->GetInternalChannel(channel_id_);
  if (channel == nullptr) {
    return Status::Unavailable();
  }
  return channel->Send(MakePacket(type, payload, status));
}

Status Call::CloseAndSendResponseCallbackLocked(
    const Function<StatusWithSize(ByteSpan)>& callback, Status status) {
  PW_TRY_ASSIGN(auto result, EncodeCallbackToPayloadBuffer(callback));
  return CloseAndSendFinalPacketLocked(
      pwpb::PacketType::RESPONSE, result.payload(), status);
}

Status Call::TryCloseAndSendResponseCallbackLocked(
    const Function<StatusWithSize(ByteSpan)>& callback, Status status) {
  PW_TRY_ASSIGN(auto result, EncodeCallbackToPayloadBuffer(callback));
  return TryCloseAndSendFinalPacketLocked(
      pwpb::PacketType::RESPONSE, result.payload(), status);
}

Status Call::CloseAndSendFinalPacketLocked(PacketType type,
                                           ConstByteSpan response,
                                           Status status) {
  const Status send_status = SendPacket(type, response, status);
  UnregisterAndMarkClosed();
  return send_status;
}

Status Call::TryCloseAndSendFinalPacketLocked(PacketType type,
                                              ConstByteSpan response,
                                              Status status) {
  const Status send_status = SendPacket(type, response, status);
  // Only close the call if the final packet gets sent out successfully.
  if (send_status.ok()) {
    UnregisterAndMarkClosed();
  }
  return send_status;
}

Status Call::WriteLocked(ConstByteSpan payload) {
  return SendPacket(properties_.call_type() == kServerCall
                        ? PacketType::SERVER_STREAM
                        : PacketType::CLIENT_STREAM,
                    payload);
}

Status Call::WriteCallbackLocked(
    const Function<StatusWithSize(ByteSpan)>& callback) {
  PW_TRY_ASSIGN(auto result, EncodeCallbackToPayloadBuffer(callback));
  return SendPacket(properties_.call_type() == kServerCall
                        ? PacketType::SERVER_STREAM
                        : PacketType::CLIENT_STREAM,
                    result.payload());
}

// This definition is in the .cc file because the Endpoint class is not defined
// in the Call header, due to circular dependencies between the two.
void Call::CloseAndMarkForCleanup(Status error) {
  endpoint_->CloseCallAndMarkForCleanup(*this, error);
}

void Call::HandlePayload(ConstByteSpan payload) {
  // pw_rpc only supports handling packets for a particular RPC one at a time.
  // Check if any callbacks are running and drop the packet if they are.
  //
  // The on_next callback cannot support multiple packets at once since it is
  // moved before it is invoked. on_error and on_completed are only called
  // after the call is closed.
  if (CallbacksAreRunning()) {
    PW_LOG_WARN(
        "Received stream packet for %u:%08x/%08x before the callback for a "
        "previous packet completed! This packet will be dropped. This can be "
        "avoided by handling packets for a particular RPC on only one thread.",
        static_cast<unsigned>(channel_id_),
        static_cast<unsigned>(service_id_),
        static_cast<unsigned>(method_id_));
    rpc_lock().unlock();
    return;
  }

  if (on_next_ == nullptr) {
    rpc_lock().unlock();
    return;
  }

  const uint32_t original_id = id();
  auto on_next_local = std::move(on_next_);
  CallbackStarted();

  if (hold_lock_while_invoking_callback_with_payload()) {
    on_next_local(payload);
  } else {
    rpc_lock().unlock();
    on_next_local(payload);
    rpc_lock().lock();
  }

  CallbackFinished();

  // Restore the original callback if the original call is still active and
  // the callback has not been replaced.
  // NOLINTNEXTLINE(bugprone-use-after-move)
  if (active_locked() && id() == original_id && on_next_ == nullptr) {
    on_next_ = std::move(on_next_local);
  }

  // The call could have been reinitialized and cleaned up already by another
  // thread that acquired the rpc_lock() while on_next_local was executing
  // without lock held.
  if (endpoint_ != nullptr) {
    // Clean up calls in case decoding failed.
    endpoint_->CleanUpCalls();
  } else {
    rpc_lock().unlock();
  }
}

void Call::CloseClientCall() {
  // When a client call is closed, for bidirectional and client streaming RPCs,
  // the server may be waiting for client stream messages, so we need to notify
  // the server that the client has requested for completion and no further
  // requests should be expected from the client. For unary and server streaming
  // RPCs, since the client is not sending messages, server does not need to be
  // notified.
  if (has_client_stream() && !client_requested_completion()) {
    RequestCompletionLocked().IgnoreError();
  }
  UnregisterAndMarkClosed();
}

void Call::UnregisterAndMarkClosed() {
  if (active_locked()) {
    endpoint().UnregisterCall(*this);
    MarkClosed();
  }
}

void Call::DebugLog() const PW_NO_LOCK_SAFETY_ANALYSIS {
  PW_LOG_INFO(
      "Call %p\n"
      "\tEndpoint: %p\n"
      "\tCall ID:  %8u\n"
      "\tChannel:  %8u\n"
      "\tService:  %08x\n"
      "\tMethod:   %08x\n"
      "\tState:    %8x\n"
      "\tCleanup:  %8s\n"
      "\tBusy CBs: %8x\n"
      "\tType:     %8d\n"
      "\tClient:   %8d\n"
      "\tWrapped:  %8d\n"
      "\ton_error: %8d\n"
      "\ton_next:  %8d\n",
      static_cast<const void*>(this),
      static_cast<const void*>(endpoint_),
      static_cast<unsigned>(id_),
      static_cast<unsigned>(channel_id_),
      static_cast<unsigned>(service_id_),
      static_cast<unsigned>(method_id_),
      static_cast<int>(state_),
      Status(static_cast<Status::Code>(awaiting_cleanup_)).str(),
      static_cast<int>(callbacks_executing_),
      static_cast<int>(properties_.method_type()),
      static_cast<int>(properties_.call_type()),
      static_cast<int>(hold_lock_while_invoking_callback_with_payload()),
      static_cast<int>(on_error_ == nullptr),
      static_cast<int>(on_next_ == nullptr));
}

}  // namespace pw::rpc::internal
