// Copyright 2021 The Pigweed Authors
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

#include "pw_rpc/internal/server_call.h"

#include "pw_log/log.h"

namespace pw::rpc::internal {

void ServerCall::HandleClientRequestedCompletion()
    PW_UNLOCK_FUNCTION(rpc_lock()) {
  MarkStreamCompleted();

#if PW_RPC_COMPLETION_REQUEST_CALLBACK
  auto on_client_requested_completion_local =
      std::move(on_client_requested_completion_);
  CallbackStarted();
  rpc_lock().unlock();

  if (on_client_requested_completion_local) {
    on_client_requested_completion_local();
  }

  rpc_lock().lock();
  CallbackFinished();
#else
  PW_LOG_WARN(
      "Received completion request packet for %u:%08x/%08x, but completion "
      "request callbacks are disabled (PW_RPC_COMPLETION_REQUEST_CALLBACK is "
      "0). The client call may be waiting for an action that the server cannot "
      "complete. The server should be compiled with completion callbacks to "
      "support services that require them.",
      static_cast<unsigned>(channel_id_locked()),
      static_cast<unsigned>(service_id()),
      static_cast<unsigned>(method_id()));
#endif  // PW_RPC_COMPLETION_REQUEST_CALLBACK
  rpc_lock().unlock();
}

void ServerCall::MoveServerCallFrom(ServerCall& other) {
  WaitUntilReadyForMove(*this, other);

  // If this call is active, finish it first.
  if (active_locked()) {
    CloseAndSendResponseLocked(OkStatus()).IgnoreError();
  }

  MoveFrom(other);

#if PW_RPC_COMPLETION_REQUEST_CALLBACK
  // Swap so that this call's previous callback is destroyed with `other`
  // rather than while rpc_lock() is held.
  on_client_requested_completion_.swap(other.on_client_requested_completion_);
#endif  // PW_RPC_COMPLETION_REQUEST_CALLBACK
}

}  // namespace pw::rpc::internal
