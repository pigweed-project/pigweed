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
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "pw_async2/context.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_intrusive_ptr/intrusive_ptr.h"
#include "pw_rpc2/internal/call.h"
#include "pw_rpc2/internal/method.h"
#include "pw_rpc2/internal/method_future.h"
#include "pw_rpc2/internal/protocol_error.h"
#include "pw_rpc2/internal/server_connection_task.h"

namespace pw::rpc2::internal {

/// One server-side RPC invocation: the call state, the task that runs the
/// user's method future, and that future's storage, in a single
/// reference-counted allocation.
///
/// A server call is a task, not a future its connection polls. Each in-flight
/// RPC is an independent thread of control, so the wakers its method stores
/// name that RPC and nothing else: a connection wake advances the transport,
/// not every method running on it. The call is still *bounded* by its
/// connection --- tearing the connection down retires the call --- but it is
/// not driven by it.
///
/// @note **Ownership.** The connection holds one intrusive reference while the
/// call is active. Every handle the method is given --- `Reader`, `Writer`,
/// `UnaryWriter` --- holds another, so a handle that escapes the method
/// keeps the call alive rather than dangling. Retiring the call deregisters
/// this task, destroys the user future, and drops the connection's reference,
/// breaking the cycle between a call and the handles stored inside its own
/// future.
///
/// @note **Threading.** A `ServerCall` belongs to its connection's dispatcher,
/// exactly as its `Call` base does, and is posted to that same dispatcher.
class alignas(std::max_align_t) ServerCall final : public Call,
                                                   public async2::Task {
 public:
  /// Calculates the total allocation size for `ServerCall` plus trailing
  /// storage.
  static constexpr size_t TotalAllocationSize(size_t future_size) {
    return sizeof(ServerCall) + future_size;
  }

  /// Returns a pointer to the trailing storage buffer for the method future,
  /// located immediately after `ServerCall` at offset `sizeof(ServerCall)`.
  std::byte* future_storage() {
    return reinterpret_cast<std::byte*>(this) + sizeof(ServerCall);
  }

  /// Allocates a single contiguous memory block containing `ServerCall` and
  /// its trailing future storage from `connection_task.allocator()`,
  /// registering and adopting it into `connection_task`. Returns null if the
  /// allocation failed.
  static ServerCall* Allocate(ServerConnectionTask& connection_task,
                              uint32_t call_id,
                              const Method& method);

  /// Emplaces a `MethodFutureImpl<Fut>` directly into `future_storage()` using
  /// `factory` (with C++17 guaranteed copy elision) and finishes the
  /// invocation. Returns `ProtocolError::kNone`, or
  /// `ProtocolError::kFailedToAllocateCallResourcesWhileRunning` if a
  /// coroutine failed to allocate.
  template <typename Fut, typename Factory>
  ProtocolError EmplaceFutureFromFactory(Factory&& factory) {
    return CommitEmplacedFuture(
        BoxedMethodFuture::Emplace<Fut>(future_storage(),
                                        std::forward<Factory>(factory)),
        std::is_same_v<Fut, async2::Coro<void>>);
  }

  /// Destroys the user's method future.
  ///
  /// Called when the call is retired. The future owns the handles the method
  /// was given, so this both runs their end-of-call behavior (a `Writer`
  /// sending its stream end, for instance) and releases the references they
  /// hold to this call.
  void ClearUserFuture() { user_future_ = BoxedMethodFuture(); }

  /// Cleans up this call on retirement: halts the task running the user's
  /// method, destroys that method's future, detaches this call from its
  /// connection, and drops the connection's reference, which may free the call.
  ///
  /// If the write side is still open after the future is destroyed, a
  /// `Writer` or `UnaryWriter` (or an unfinished write) escaped the method, so
  /// nothing sent the call's terminal packet. The client is owed one, so the
  /// call is ended with an error (see `Call::CloseWriteOnRetire()`), and the
  /// escaped handle's later operations fail.
  ///
  /// @pre The caller must have already unlinked this call from its connection's
  /// `calls()` list, and must not call this from inside this call's `DoPend()`.
  void Retire();

  /// Returns an `IntrusivePtr<Call>` referring to this call.
  IntrusivePtr<Call> shared_call() { return IntrusivePtr<Call>(this); }

  /// Returns the connection this call arrived on.
  ///
  /// @pre The call must still be attached to its connection, which it is
  /// while its method is being invoked or its future is alive.
  ServerConnectionTask& server_connection_task() const {
    return static_cast<ServerConnectionTask&>(connection_task());
  }

  /// True once this call is ready to be retired by its connection: either the
  /// user's method future has returned, or the call itself has closed.
  ///
  /// The two conditions are distinct, and neither implies the other. A method
  /// that has just returned still owns the `Writer` or `UnaryWriter` handles
  /// stored inside its future, and destroying those is what sends the call's
  /// terminal packet --- so the write side is still open at this point, and
  /// `Retire()` is careful to destroy the future before closing it. Conversely,
  /// a call can be closed while its method is still parked, because the peer
  /// cancelled it or the connection dropped; the connection retires those
  /// without waiting for the method to notice.
  ///
  /// Either way the call is retired by its connection's next poll, which is
  /// also what frees it; the connection cannot do that from inside this task's
  /// own poll.
  bool is_retirable() const {
    return HasWriteFlag<kServerFinished>() || is_closed();
  }

 private:
  ServerCall(ServerConnectionTask& connection_task, uint32_t call_id);

  /// Advances the user's method future, and asks the connection to retire this
  /// call once it is done with it.
  async2::Poll<> DoPend(async2::Context& cx) override;

  /// Marks the method as done and wakes the connection, which retires the
  /// call on its next poll.
  void FinishAndRequestRetirement();

  ProtocolError CommitEmplacedFuture(BoxedMethodFuture&& future, bool is_coro);

  BoxedMethodFuture user_future_;
};

}  // namespace pw::rpc2::internal
