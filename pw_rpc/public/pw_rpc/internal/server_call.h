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
#pragma once

#include "pw_function/function.h"
#include "pw_rpc/internal/call.h"
#include "pw_rpc/internal/config.h"
#include "pw_rpc/internal/lock.h"

namespace pw::rpc::internal {

// A Call object, as used by an RPC server.
class ServerCall : public Call {
 public:
  void HandleClientRequestedCompletion() PW_UNLOCK_FUNCTION(rpc_lock());

#if PW_RPC_COMPLETION_REQUEST_CALLBACK
  // Moves out the completion request callback so that it can be destroyed
  // after rpc_lock() is released.
  Function<void()> TakeOnCompletionRequestedLocked()
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
    return std::move(on_client_requested_completion_);
  }
#endif  // PW_RPC_COMPLETION_REQUEST_CALLBACK

 protected:
  constexpr ServerCall() = default;

  ServerCall(ServerCall&& other) { *this = std::move(other); }

  ~ServerCall() { DestroyServerCall(); }

  // Version of operator= used by the raw call classes.
  ServerCall& operator=(ServerCall&& other) PW_LOCKS_EXCLUDED(rpc_lock()) {
    RpcLockGuard lock;
    MoveServerCallFrom(other);
    return *this;
  }

  void MoveServerCallFrom(ServerCall& other)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock());

  ServerCall(const LockedCallContext& context, CallProperties properties)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock())
      : Call(context, properties) {}

  // set_on_completion_requested is templated so that it can be
  // conditionally disabled with a helpful static_assert message.
  template <typename UnusedType = void>
  void set_on_completion_requested(
      [[maybe_unused]] Function<void()> on_client_requested_completion)
      PW_LOCKS_EXCLUDED(rpc_lock()) {
    static_assert(cfg::kClientStreamEndCallbackEnabled<UnusedType>,
                  "The client stream end callback is disabled, so "
                  "set_on_completion_requested cannot be called. To "
                  "enable the client end "
                  "callback, set PW_RPC_COMPLETION_REQUEST_CALLBACK to 1.");
#if PW_RPC_COMPLETION_REQUEST_CALLBACK
    RpcLockGuard lock;
    on_client_requested_completion_ = std::move(on_client_requested_completion);
#endif  // PW_RPC_COMPLETION_REQUEST_CALLBACK
  }

  // Sets the provided on_client_requested_completion callback if
  // PW_RPC_COMPLETION_REQUEST_CALLBACK is defined. Unlike
  // set_on_completion_requested this API will not raise a static_assert
  // message at compile time even when the macro is not defined. The previous
  // callback is destroyed after rpc_lock() is released.
  void set_on_completion_requested_if_enabled(
      Function<void()> on_client_requested_completion)
      PW_LOCKS_EXCLUDED(rpc_lock()) {
#if PW_RPC_COMPLETION_REQUEST_CALLBACK
    RpcLockGuard lock;
    on_client_requested_completion_.swap(on_client_requested_completion);
#else
    on_client_requested_completion = nullptr;
#endif  // PW_RPC_COMPLETION_REQUEST_CALLBACK
  }

 private:
#if PW_RPC_COMPLETION_REQUEST_CALLBACK
  // Called when a client stream completes.
  Function<void()> on_client_requested_completion_ PW_GUARDED_BY(rpc_lock());
#endif  // PW_RPC_COMPLETION_REQUEST_CALLBACK
};

}  // namespace pw::rpc::internal
