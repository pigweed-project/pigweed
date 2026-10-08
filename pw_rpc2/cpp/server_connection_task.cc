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

#include "pw_rpc2/internal/server_connection_task.h"

#include <utility>

#include "pw_assert/check.h"
#include "pw_log/log.h"
#include "pw_rpc2/internal/call.h"
#include "pw_rpc2/internal/packet.h"
#include "pw_rpc2/internal/server_call.h"
#include "pw_rpc2/internal/server_task.h"
#include "pw_rpc2/service.h"

namespace pw::rpc2::internal {
namespace {

// Returns the payload for the method that `packet` starts. Client-stream
// methods read requests from the call, so they get an empty payload.
ConstBuf TakeInitialPayload(Call& call,
                            bool is_client_stream_method,
                            InboundPacket&& packet) {
  const PacketType packet_type = packet.type();
  if (!is_client_stream_method) {
    return std::move(packet).TakePayload();
  }
  if (packet_type.has_payload()) {
    call.DeliverFirstPayloadForStream(std::move(packet).TakePayload());
  }
  if (packet_type.close_mode() == CloseMode::kStreamEnd) {
    call.OnPeerStreamEnd();
  }
  return {};
}

}  // namespace

ServerConnectionTask::ServerConnectionTask(
    transport::ReliableDatagramSocket connection,
    Allocator& allocator,
    ServerTask& server_task)
    : ConnectionTask(std::move(connection), allocator, EndpointRole::kServer),
      server_task_(server_task),
      handshake_future_(this->connection()) {}

ServerConnectionTask::ServerConnectionTask(
    EstablishedConnection established_connection,
    Allocator& allocator,
    ServerTask& server_task)
    : ConnectionTask(
          std::move(established_connection), allocator, EndpointRole::kServer),
      server_task_(server_task) {}

ServerConnectionTask::~ServerConnectionTask() {
  Teardown();
  ForceRetireAllCalls();
  server_task_.RemoveConnection(*this);
}

void ServerConnectionTask::CleanUpCalls() {
  for (auto prev = calls().before_begin(), it = calls().begin();
       it != calls().end();) {
    ServerCall& call = static_cast<ServerCall&>(*it);
    if (call.is_retirable()) {
      it = calls().erase_after(prev);
      call.Retire();
    } else {
      prev = it++;
    }
  }
}

void ServerConnectionTask::ForceRetireAllCalls() {
  while (!calls().empty()) {
    ServerCall& call = static_cast<ServerCall&>(calls().front());
    calls().pop_front();
    call.Retire();
  }
}

async2::Poll<> ServerConnectionTask::DoPend(async2::Context& cx) {
  if (PollConnection(cx)) {
    cx.ReEnqueue();
  }
  if (is_closed()) {
    // Retire all server calls before unregistering from the server so that no
    // call's user future outlives this connection's place in the server.
    ForceRetireAllCalls();
    server_task_.RemoveConnection(*this);
    return async2::Ready();
  }
  return async2::Pending();
}

bool ServerConnectionTask::PollConnection(async2::Context& cx) {
  if (is_closed()) {
    return false;
  }
  StoreWaker(cx);

  // Stage 1: Complete responder handshake before accepting packets.
  if (state() == State::kHandshaking && !PollHandshake(cx)) {
    return false;
  }

  // Stage 2: Drain outgoing control packets and read/dispatch up to
  // `kMaxPacketsPerPoll` incoming packets.
  //
  // Note: `progressed` is intentionally overwritten on each iteration rather
  // than OR-accumulated. If `PendPacket()` returns false on iteration `i <
  // kMaxPacketsPerPoll`, it has already polled the underlying transport futures
  // to `Pending()` and registered wakers with `cx`, so re-enqueuing the task
  // would only cause a redundant poll. `progressed` remains true after the loop
  // only when all `kMaxPacketsPerPoll` iterations succeeded, meaning more work
  // may still be ready in the transport and the task must yield via
  // `cx.ReEnqueue()`.
  bool progressed = false;
  for (int i = 0; i < kMaxPacketsPerPoll; ++i) {
    InboundPacket request;
    progressed = PendPacket(cx, &request);
    if (request != nullptr) {
      HandleIncomingRequest(std::move(request));
    }
    if (is_closed()) {
      return false;
    }
    if (!progressed) {
      break;
    }
  }

  // Stage 3: Retire the calls that are done.
  CleanUpCalls();
  return progressed;
}

bool ServerConnectionTask::PollHandshake(async2::Context& cx) {
  auto poll = handshake_future_.Pend(cx);
  if (!poll.IsReady()) {
    return false;
  }
  if (!poll->ok()) {
    PW_LOG_WARN("Handshake failed with status %s, closing connection",
                poll->status().str());
    CloseConnection(poll->status());
    return false;
  }
  FinishHandshake(std::move(poll->value()));
  handshake_future_ = ResponderHandshakeFuture();
  return true;
}

void ServerConnectionTask::HandleIncomingRequest(InboundPacket&& packet) {
  const PacketType packet_type = packet.type();
  PW_DCHECK(packet_type.is_start());

  // Runs on the dispatcher thread, which also owns the registry, so the
  // service resolved here cannot be unregistered underneath the dispatch.
  Service* target_service = server_task_.FindService(packet.service_id());
  if (target_service == nullptr) {
    PW_LOG_WARN("Call %u: request for unknown service 0x%08x",
                static_cast<unsigned>(packet.call_id()),
                static_cast<unsigned>(packet.service_id()));
    QueueError(packet.call_id(), ProtocolStatus::kUnknownService);
    return;
  }

  const uint32_t method_id = packet.method_id();
  const Method* method = ServiceAccess::FindMethod(*target_service, method_id);
  if (method == nullptr) {
    PW_LOG_WARN("Call %u: request for unknown method 0x%08x in service 0x%08x",
                static_cast<unsigned>(packet.call_id()),
                static_cast<unsigned>(method_id),
                static_cast<unsigned>(packet.service_id()));
    QueueError(packet.call_id(), ProtocolStatus::kUnknownMethod);
    return;
  }

  // A unary or server-streaming method takes exactly one request, so it must
  // be started by the packet that carries that request and closes the client's
  // stream. Anything else means the client disagrees about the method's type.
  const bool is_client_stream_method = HasClientStream(method->type());
  const bool is_single_request =
      packet_type.has_payload() &&
      packet_type.close_mode() == CloseMode::kStreamEnd;
  if (!is_client_stream_method && !is_single_request) {
    PW_LOG_WARN(
        "Call %u: start packet type 0x%02x does not match the type of method "
        "0x%08x in service 0x%08x",
        static_cast<unsigned>(packet.call_id()),
        static_cast<unsigned>(packet_type.bits()),
        static_cast<unsigned>(method_id),
        static_cast<unsigned>(packet.service_id()));
    QueueError(packet.call_id(), ProtocolStatus::kMethodTypeMismatch);
    return;
  }

  Result<ServerCall*> call_res =
      ServerCall::Allocate(*this, packet.call_id(), *method, allocator());
  if (!call_res.ok()) {
    PW_LOG_ERROR("Call %u: failed to allocate ServerCall: %s",
                 static_cast<unsigned>(packet.call_id()),
                 call_res.status().str());
    QueueError(packet.call_id(), ProtocolStatus::kFailedToAllocateCall);
    return;
  }
  ServerCall& call = **call_res;

  const ProtocolStatus invocation_error = method->Invoke(
      *target_service,
      call,
      TakeInitialPayload(call, is_client_stream_method, std::move(packet)));
  if (invocation_error != ProtocolStatus::kOk) {
    PW_LOG_WARN("Call %u: method invocation failed with protocol error: %s",
                static_cast<unsigned>(call.call_id()),
                pw::EnumToString(invocation_error));
    if (!call.is_write_closed()) {
      QueueError(call.call_id(), invocation_error);
      call.CloseWrite();
    }
    calls().pop_front();
    call.Retire();
    return;
  }

  // The method is now running. Give it its own task on the server's
  // dispatcher --- the one polling this connection --- so that the wakers it
  // stores wake this call alone.
  server_task_.dispatcher().Post(call);
}

}  // namespace pw::rpc2::internal
