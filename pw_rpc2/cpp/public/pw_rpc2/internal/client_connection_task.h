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

#include <atomic>
#include <cstdint>
#include <limits>

#include "pw_allocator/allocator.h"
#include "pw_assert/assert.h"
#include "pw_async2/context.h"
#include "pw_async2/poll.h"
#include "pw_rpc2/internal/call.h"
#include "pw_rpc2/internal/connection_task.h"
#include "pw_rpc2/internal/control_future.h"
#include "pw_rpc2/internal/handshake.h"
#include "pw_status/status.h"
#include "pw_sync/lock_annotations.h"

namespace pw::rpc2::internal {

/// Connection task specialized for clients, managing client-specific state
/// such as outgoing call ID allocation and the client's one thread-safe
/// control operation, `Close()`.
///
/// A client only ever reaches this point with a connection whose handshake
/// `ClientFuture` already completed, so there is no client-role handshake
/// phase and no constructor that starts one.
class ClientConnectionTask final : public ConnectionTask {
 public:
  ClientConnectionTask(EstablishedConnection established_connection,
                       Allocator& allocator);

  ~ClientConnectionTask() override;

  /// Allocates a new `Call` with a fresh call ID on this connection.
  ///
  /// @pre Must be called from this connection's dispatcher thread.
  IntrusivePtr<Call> CreateCall() {
    const uint32_t id = NewCallId();
    return ClientCall::Create(*this, id, allocator());
  }

  /// Returns a call ID that is unused on this connection.
  ///
  /// IDs are handed out monotonically. A wrapped ID could collide with an
  /// in-flight call and misroute its packets, so the connection is closed
  /// instead, forcing the peer to re-establish call state. 2^32 calls on one
  /// connection is not reachable in practice; this exists so that the failure
  /// mode is a clean reset rather than silent cross-talk.
  ///
  /// @pre Must be called from this connection's dispatcher thread.
  uint32_t NewCallId();

  /// Closes the transport connection, completes every call on it with
  /// `CANCELLED`, and retires this task.
  ///
  /// Callable from any thread. The returned future resolves once the
  /// dispatcher has done all of that, and also if the connection is already
  /// gone, so it never hangs on a connection that failed on its own.
  ControlFuture Close();

  /// `Close()`, but blocks the calling thread until the connection is closed.
  ///
  /// @pre Must not be called from this connection's dispatcher thread.
  void CloseBlocking();

  /// Thread-safe check returning true if `Close()` / `CloseBlocking()` has not
  /// been initiated and the task has not finished.
  [[nodiscard]] bool is_open() const {
    return close_state_.load(std::memory_order_acquire) == CloseState::kOpen;
  }

  /// Registers one more `Client` handle referring to this task.
  ///
  /// The dispatcher holds its own reference to the task, so the `SharedPtr`
  /// use count cannot tell when the last user handle is gone; this count does.
  void AddUserHandle() {
    const uint16_t previous =
        user_handles_.fetch_add(1, std::memory_order_relaxed);
    PW_DASSERT(previous != std::numeric_limits<uint16_t>::max());
  }

  /// Drops one `Client` handle. When it was the last one, requests a close,
  /// since nothing can reach the connection to close it any more.
  ///
  /// Callable from any thread, including the dispatcher's.
  void ReleaseUserHandle();

 private:
  enum class CloseState : uint8_t {
    kOpen,
    kClosing,  // `Close()` was requested; the dispatcher has not applied it.
    kClosed,
  };

  async2::Poll<> DoPend(async2::Context& cx) override;
  bool PollConnection(async2::Context& cx);

  /// Moves an open connection to `kClosing` and wakes the task. If `future` is
  /// non-null, it is queued to resolve when the connection is closed, or
  /// resolved on the spot if it already is.
  void RequestClose(ControlFuture* future) PW_LOCKS_EXCLUDED(ControlLock());

  /// Publishes the closed state and resolves every pending `Close()`.
  ///
  /// Runs both when the task retires and from the destructor, because a task
  /// can be dropped by a dispatcher that is itself being destroyed, and a
  /// caller blocked in `CloseBlocking()` must be released either way.
  /// Idempotent.
  void Finish() PW_LOCKS_EXCLUDED(ControlLock());

  static constexpr uint32_t kMaxCallId = 0xffffffffu;

  uint32_t next_call_id_ = 1;

  // Number of live `Client` handles. See `AddUserHandle()`.
  std::atomic<uint16_t> user_handles_{0};

  // Written under `ControlLock()`; read without it by the dispatcher thread.
  std::atomic<CloseState> close_state_{CloseState::kOpen};

  // Resolved once the connection is closed.
  ControlFuture::List close_futures_ PW_GUARDED_BY(ControlLock());
};

}  // namespace pw::rpc2::internal
