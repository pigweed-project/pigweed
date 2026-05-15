// Copyright 2022 The Pigweed Authors
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
#pragma once

#include "pw_assert/assert.h"
#include "pw_bytes/span.h"
#include "pw_protobuf/encoder.h"
#include "pw_protobuf/internal/codegen.h"
#include "pw_protobuf/stream_decoder.h"
#include "pw_rpc/internal/client_call.h"
#include "pw_rpc/internal/encoding_buffer.h"
#include "pw_rpc/internal/server_call.h"
#include "pw_rpc/pwpb/serde.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"

namespace pw::rpc::internal {

// Forward declaration to avoid circular include.
class PwpbServerCall;

// Defines per-message struct type instance of the serializer/deserializer.
template <PwpbMessageDescriptor kRequest, PwpbMessageDescriptor kResponse>
constexpr PwpbMethodSerde kPwpbMethodSerde(kRequest, kResponse);

// [Client] Encodes and sends the initial request message for the call.
// active() must be true.
template <typename Request>
void PwpbSendInitialRequest(ClientCall& call,
                            PwpbSerde serde,
                            const Request& request)
    PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
  PW_ASSERT(call.active_locked());

  auto buffer = EncodeToPayloadBuffer(request, serde);
  if (buffer.ok()) {
    call.SendInitialClientRequest(buffer.value().payload());
  } else {
    call.CloseAndMarkForCleanup(buffer.status());
  }
}

// [Client/Server] Encodes and sends a client or server stream message.
// Returns FAILED_PRECONDITION if active() is false.
template <typename Payload>
Status PwpbSendStream(Call& call,
                      const Payload& payload,
                      const PwpbMethodSerde* serde)
    PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
  if (!call.active_locked()) {
    return Status::FailedPrecondition();
  }

  auto buffer = EncodeToPayloadBuffer(
      payload,
      call.type() == kClientCall ? serde->request() : serde->response());
  PW_TRY(buffer.status());

  return call.WriteLocked(buffer.value().payload());
}

}  // namespace pw::rpc::internal
