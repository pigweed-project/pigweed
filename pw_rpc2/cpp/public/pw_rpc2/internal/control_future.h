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

#include <mutex>
#include <optional>
#include <utility>

#include "pw_assert/assert.h"
#include "pw_async2/context.h"
#include "pw_async2/future.h"
#include "pw_async2/poll.h"
#include "pw_polyfill/language_feature_macros.h"
#include "pw_status/status.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_sync/lock_annotations.h"

namespace pw::transport {
class ReliableDatagramListener;
}  // namespace pw::transport

namespace pw::rpc2 {
namespace internal {

class ServerTask;
class ClientConnectionTask;

/// Lock protecting every `ControlFuture` and the lists that hold them.  The
/// lock is global so that a `ControlFuture` never needs a pointer back to the
/// `Server` or `Client` that issued it.
inline sync::InterruptSpinLock& ControlLock() {
  PW_CONSTINIT static sync::InterruptSpinLock lock;
  return lock;
}

}  // namespace internal

/// Future returned by the thread-safe control operations on `Server` and
/// `Client`.
///
/// Unlike every other `pw_rpc2` object, a `ControlFuture` has no dispatcher
/// affinity: it may be polled from any dispatcher, moved across threads, or
/// dropped. Dropping it before the operation has been applied cancels it.
class [[nodiscard]] ControlFuture {
 public:
  using value_type = Status;

  constexpr ControlFuture() = default;

  ControlFuture(ControlFuture&& other) noexcept
      PW_LOCKS_EXCLUDED(internal::ControlLock()) {
    *this = std::move(other);
  }

  ControlFuture& operator=(ControlFuture&& other) noexcept
      PW_LOCKS_EXCLUDED(internal::ControlLock()) {
    if (this != &other) {
      std::lock_guard lock(internal::ControlLock());
      core_ = std::move(other.core_);
      status_ = other.status_;
      listener_ = std::exchange(other.listener_, nullptr);
    }
    return *this;
  }

  ~ControlFuture() PW_LOCKS_EXCLUDED(internal::ControlLock()) {
    std::lock_guard lock(internal::ControlLock());
    core_.Reset();
  }

  /// Creates a `ControlFuture` that is already resolved with `status`.
  static ControlFuture Resolved(Status status) {
    return ControlFuture(async2::FutureState::kReadyForCompletion, status);
  }

  async2::Poll<Status> Pend(async2::Context& cx)
      PW_LOCKS_EXCLUDED(internal::ControlLock()) {
    std::lock_guard lock(internal::ControlLock());
    PW_ASSERT(core_.is_pendable());
    if (core_.is_ready()) {
      core_.MarkComplete();
      return async2::Ready(status_);
    }
    PW_ASYNC_STORE_WAKER(cx, core_.waker(), "pw::rpc2::ControlFuture");
    return async2::Pending();
  }

  [[nodiscard]] bool is_pendable() const
      PW_LOCKS_EXCLUDED(internal::ControlLock()) {
    std::lock_guard lock(internal::ControlLock());
    return core_.is_pendable();
  }

  [[nodiscard]] bool is_complete() const
      PW_LOCKS_EXCLUDED(internal::ControlLock()) {
    std::lock_guard lock(internal::ControlLock());
    return core_.is_complete();
  }

 private:
  friend class internal::ServerTask;
  friend class internal::ClientConnectionTask;

  // Declared before `List` so that the member pointer can be formed.
  async2::FutureCore core_ PW_GUARDED_BY(internal::ControlLock());

  /// Intrusive list of pending `ControlFuture`s, held by the task that will
  /// resolve them.
  using List = async2::FutureList<&ControlFuture::core_>;

  ControlFuture(async2::FutureState::ReadyForCompletion, Status status)
      : core_(async2::FutureState::kReadyForCompletion), status_(status) {}

  /// A pending future. `listener` is set for listener registrations.
  explicit ControlFuture(transport::ReliableDatagramListener* listener)
      : core_(async2::FutureState::kPending), listener_(listener) {}

  /// Resolves the future, waking any task pending it. The future must already
  /// have been removed from any list.
  void ResolveLocked(Status status)
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::ControlLock()) {
    status_ = status;
    core_.WakeAndMarkReady();
  }

  /// If the future has already been resolved, completes it and returns its
  /// status. For `*Blocking()` callers whose request was applied synchronously
  /// on their own thread, and so have no dispatcher to run the future on.
  std::optional<Status> TryTakeResolved()
      PW_LOCKS_EXCLUDED(internal::ControlLock()) {
    std::lock_guard lock(internal::ControlLock());
    if (!core_.is_ready()) {
      return std::nullopt;
    }
    core_.MarkComplete();
    return status_;
  }

  transport::ReliableDatagramListener& listener_locked() const
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::ControlLock()) {
    PW_DASSERT(listener_ != nullptr);
    return *listener_;
  }

  Status status_ PW_GUARDED_BY(internal::ControlLock()) = Status::Unknown();
  transport::ReliableDatagramListener* listener_
      PW_GUARDED_BY(internal::ControlLock()) = nullptr;
};

static_assert(async2::Future<ControlFuture>);

}  // namespace pw::rpc2
