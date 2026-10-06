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

#include "pw_rpc2/service_client.h"

#include <cstddef>
#include <cstdint>
#include <utility>

#include "pw_assert/check.h"
#include "pw_async2/try.h"
#include "pw_rpc2/internal/call.h"
#include "pw_rpc2/internal/generated_service_client.h"
#include "pw_rpc2/internal/packet.h"
#include "pw_rpc2/method_type.h"
#include "pw_rpc2/write_reservation.h"
#include "pw_status/status.h"

namespace pw::rpc2::internal {

void UnaryFutureBase::Cancel() {
  if (req_fut_.is_pendable()) {
    // The request has not been sent, so cancelling sends nothing. The pending
    // request then fails to commit and resolves to the call's status.
    req_fut_.res_fut_.CancelCall();
    return;
  }
  response_fut_.Cancel();
}

async2::Poll<Result<ConstBuf>> UnaryFutureBase::PendRaw(async2::Context& cx,
                                                        const void* request,
                                                        SerializeFn serialize) {
  PW_CHECK(is_pendable());

  if (req_fut_.is_pendable()) {
    PW_TRY_READY_ASSIGN(Result<IntrusivePtr<Call>> call,
                        req_fut_.PendWriteAndTakeCall(cx, request, serialize));
    if (!call.ok()) {
      mark_complete();
      return async2::Ready(Result<ConstBuf>(call.status()));
    }
    response_fut_ = CallAccess::Create<RawResponseFuture>(std::move(*call));
  }
  PW_TRY_READY_ASSIGN(Result<ConstBuf> response, response_fut_.Pend(cx));
  mark_complete();
  return async2::Ready(std::move(response));
}

void UnaryFutureBase::PendAndDeserialize(async2::Context& cx,
                                         const void* request,
                                         SerializeFn serialize,
                                         void* result_out,
                                         DeserializeFn deserialize) {
  auto poll = PendRaw(cx, request, serialize);
  if (poll.IsPending()) {
    return;
  }
  if (poll->ok()) {
    deserialize(result_out, ConstByteSpan(**poll));
  } else {
    deserialize(result_out, poll->status());
  }
}

namespace {

size_t RawRequestSize(const void* max_payload_size, size_t) {
  return *static_cast<const size_t*>(max_payload_size);
}

}  // namespace

RawUnaryReserveFuture GeneratedServiceClient::CallUnaryRaw(
    uint32_t method_id, size_t max_payload_size) const {
  return RawUnaryReserveFuture(StartCall(
      method_id, MethodType::kUnary, &max_payload_size, &RawRequestSize));
}

RawServerStreamReserveFuture GeneratedServiceClient::CallServerStreamRaw(
    uint32_t method_id, size_t max_payload_size) const {
  return RawServerStreamReserveFuture(StartCall(method_id,
                                                MethodType::kServerStreaming,
                                                &max_payload_size,
                                                &RawRequestSize));
}

ReserveWriteFuture GeneratedServiceClient::StartCall(
    uint32_t method_id,
    MethodType type,
    const void* request,
    RequestSizeFn size_request) const {
  if (!client().is_open()) {
    return ReserveWriteFuture::Failed(Status::Unavailable());
  }
  IntrusivePtr<Call> call = client().CreateCall();
  if (call == nullptr) {
    return ReserveWriteFuture::Failed(Status::ResourceExhausted());
  }

  // Unary and client-streaming RPCs are answered by a single response packet.
  // Flag the call so that any other response is detected as a method type
  // mismatch rather than misread.
  if (!HasServerStream(type)) {
    call->ExpectSingleResponse();
  }

  // Unary and server-streaming calls send one packet, which carries the
  // request and closes the client stream. Client- and bidirectional-streaming
  // calls send a start packet with no payload and leave the stream open.
  const bool single_request = !HasClientStream(type);
  const size_t payload_size =
      single_request
          ? size_request(request,
                         call->max_payload_size(sizeof(RequestWireFormat)))
          : 0;
  Result<transport::ReserveWriteFuture> reservation =
      call->ReserveWrite(sizeof(RequestWireFormat), payload_size);
  if (!reservation.ok()) {
    return ReserveWriteFuture::Failed(reservation.status());
  }

  const uint32_t call_id = call->call_id();
  return single_request
             ? ReserveWriteFuture::StartUnary(std::move(reservation),
                                              call_id,
                                              service_id(),
                                              method_id,
                                              std::move(call))
             : ReserveWriteFuture::StartStream(std::move(reservation),
                                               call_id,
                                               service_id(),
                                               method_id,
                                               std::move(call));
}

}  // namespace pw::rpc2::internal
