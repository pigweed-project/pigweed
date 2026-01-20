// Copyright 2024 The Pigweed Authors
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

#include <stddef.h>

#include <mutex>

#include "pw_async2/dispatcher.h"
#include "pw_async2/future.h"
#include "pw_chrono/virtual_clock.h"
#include "pw_containers/intrusive_list.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_sync/lock_annotations.h"
#include "pw_toolchain/no_destructor.h"

namespace pw::async2 {

namespace internal {

// A lock which guards `TimeProvider`'s linked list.
inline pw::sync::InterruptSpinLock& time_lock() {
  static pw::sync::InterruptSpinLock lock;
  return lock;
}

// `Timer` objects must not outlive their `TimeProvider`.
void AssertTimeFutureObjectsAllGone(bool empty);

}  // namespace internal

/// @submodule{pw_async2,time}

template <typename Clock>
class TimeFuture;

/// A factory for time and timers.
///
/// This extends the `VirtualClock` interface with the ability to create async
/// timers.
///
/// `TimeProvider` is designed to be dependency-injection friendly so that
/// code that uses time and timers is not bound to real wall-clock time.
/// This is particularly helpful for testing timing-sensitive code without
/// adding manual delays to tests (which often results in flakiness and
/// long-running tests).
///
/// Note that `Timer` objects must not outlive the `TimeProvider` from which
/// they were created.
template <typename Clock>
class TimeProvider : public chrono::VirtualClock<Clock> {
 public:
  ~TimeProvider() override {
    internal::AssertTimeFutureObjectsAllGone(futures_.empty());
  }

  typename Clock::time_point now() override = 0;

  /// Queues the `callback` to be invoked after `delay`.
  ///
  /// This method is thread-safe and can be invoked from `callback` but may not
  /// be interrupt-safe on all platforms.
  [[nodiscard]] TimeFuture<Clock> WaitFor(typename Clock::duration delay) {
    /// The time_point is computed based on now() plus the specified duration
    /// where a singular clock tick is added to handle partial ticks. This
    /// ensures that a duration of at least 1 tick does not result in [0,1]
    /// ticks and instead in [1,2] ticks.
    return WaitUntil(now() + delay + typename Clock::duration(1));
  }

  /// Queues the `callback` to be invoked after `timestamp`.
  ///
  /// This method is thread-safe and can be invoked from `callback` but may not
  /// be interrupt-safe on all platforms.
  [[nodiscard]] TimeFuture<Clock> WaitUntil(
      typename Clock::time_point timestamp) {
    return TimeFuture<Clock>(*this, timestamp);
  }

 protected:
  /// Run all expired timers with the current (provided) `time_point`.
  ///
  /// This method should be invoked by subclasses when `DoInvokeAt`'s timer
  /// expires.
  void RunExpired(typename Clock::time_point now)
      PW_LOCKS_EXCLUDED(internal::time_lock());

 private:
  friend class TimeFuture<Clock>;

  /// Schedule `RunExpired` to be invoked at `time_point`.
  /// Newer calls to `DoInvokeAt` supersede previous calls.
  virtual void DoInvokeAt(typename Clock::time_point)
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::time_lock()) = 0;

  /// Optimistically cancels all pending `DoInvokeAt` requests.
  virtual void DoCancel()
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::time_lock()) = 0;

  // Head of the waiting timers list.
  IntrusiveList<TimeFuture<Clock>> futures_
      PW_GUARDED_BY(internal::time_lock());
};

/// A timer which can asynchronously wait for time to pass.
///
/// This timer uses a `TimeProvider` to control its execution and so can be
/// used with any `TimeProvider` with a compatible `Clock` type.
template <typename Clock>
class [[nodiscard]] TimeFuture
    : public IntrusiveForwardList<TimeFuture<Clock>>::Item {
 public:
  using value_type = typename Clock::time_point;

  constexpr TimeFuture() : provider_(nullptr) {}

  TimeFuture(const TimeFuture&) = delete;
  TimeFuture& operator=(const TimeFuture&) = delete;

  TimeFuture(TimeFuture&& other) : provider_(nullptr) {
    *this = std::move(other);
  }

  TimeFuture& operator=(TimeFuture&& other) {
    std::lock_guard lock(internal::time_lock());
    UnlistLocked();

    provider_ = other.provider_;
    expiration_ = other.expiration_;

    // Replace the entry of `other_` in the list.
    if (!other.unlisted()) {
      auto previous = provider_->futures_.before_begin();
      while (&*std::next(previous) != &other) {
        previous++;
      }

      // NOTE: this will leave `other` reporting (falsely) that it has expired.
      // However, `other` should not be used post-`move`.
      other.unlist(&*previous);
      provider_->futures_.insert_after(previous, *this);
    }

    return *this;
  }

  /// Destructs `timer`.
  ///
  /// Destruction is thread-safe, but not necessarily interrupt-safe.
  ~TimeFuture() { Unlist(); }

  [[nodiscard]] constexpr bool is_pendable() const PW_NO_LOCK_SAFETY_ANALYSIS {
    return expiration_ != value_type{} && provider_ != nullptr;
  }

  [[nodiscard]] constexpr bool is_complete() const PW_NO_LOCK_SAFETY_ANALYSIS {
    return expiration_ != value_type{} && provider_ == nullptr;
  }

  Poll<typename Clock::time_point> Pend(Context& cx)
      PW_LOCKS_EXCLUDED(internal::time_lock()) {
    PW_ASSERT(is_pendable());

    std::lock_guard lock(internal::time_lock());
    if (this->unlisted()) {
      provider_ = nullptr;
      return Ready(expiration_);
    }
    // NOTE: this is done under the lock in order to ensure that `provider_` is
    // not set to unlisted between it being initially read and `waker_` being
    // set.
    PW_ASYNC_STORE_WAKER(cx, waker_, "TimeFuture is waiting for a time_point");
    return Pending();
  }

  // Returns the provider associated with this timer.
  //
  // NOTE: this method must not be called before initializing the timer.
  TimeProvider<Clock>& provider() PW_NO_LOCK_SAFETY_ANALYSIS {
    // A lock is not required because this value is only mutated in
    // constructors.
    return *provider_;
  }

  // Returns the provider associated with this timer.
  //
  // NOTE: this method must not be called before initializing the timer.
  typename Clock::time_point expiration() PW_NO_LOCK_SAFETY_ANALYSIS {
    // A lock is not required because this is only mutated in  constructors.
    return expiration_;
  }

 private:
  friend class TimeProvider<Clock>;

  /// Constructs a `Timer` from a `TimeProvider` and a `time_point`.
  ///
  /// @pre `expiration` cannot be the epoch (which will always have expired).
  TimeFuture(TimeProvider<Clock>& provider,
             typename Clock::time_point expiration)
      : waker_(), provider_(&provider), expiration_(expiration) {
    PW_ASSERT(expiration != value_type{});

    std::lock_guard lock(internal::time_lock());
    EnlistLocked();
  }

  void EnlistLocked() PW_EXCLUSIVE_LOCKS_REQUIRED(internal::time_lock()) {
    // Skip enlisting if the expiration of the timer is in the past.
    // NOTE: this *does not* trigger a waker since `Poll` has not yet been
    // invoked, so none has been registered.
    if (provider_->now() >= expiration_) {
      return;
    }

    if (provider_->futures_.empty() ||
        provider_->futures_.front().expiration_ > expiration_) {
      provider_->futures_.push_front(*this);
      provider_->DoInvokeAt(expiration_);
      return;
    }
    auto current = provider_->futures_.begin();
    while (std::next(current) != provider_->futures_.end() &&
           std::next(current)->expiration_ < expiration_) {
      current++;
    }
    provider_->futures_.insert_after(current, *this);
  }

  void Unlist() PW_LOCKS_EXCLUDED(internal::time_lock()) {
    std::lock_guard lock(internal::time_lock());
    UnlistLocked();
  }

  // Removes this timer from the `TimeProvider`'s list (if listed).
  //
  // If this timer was previously the `head` element of the `TimeProvider`'s
  // list, the `TimeProvider` will be rescheduled to wake up based on the
  // new `head`'s expiration time.
  void UnlistLocked() PW_EXCLUSIVE_LOCKS_REQUIRED(internal::time_lock()) {
    if (this->unlisted()) {
      return;
    }
    if (&provider_->futures_.front() == this) {
      provider_->futures_.pop_front();
      if (provider_->futures_.empty()) {
        provider_->DoCancel();
      } else {
        provider_->DoInvokeAt(provider_->futures_.front().expiration_);
      }
      return;
    }

    provider_->futures_.remove(*this);
  }

  Waker waker_;
  // NOTE: the lock is only required when
  //  (1) modifying these fields or
  //  (2) reading these fields via the TimeProvider.
  //
  // Reading these fields when nonmodification is guaranteed (such as in an
  // accessor like ``provider`` or ``expiration`` above) does not require
  // holding the lock.
  TimeProvider<Clock>* provider_ PW_GUARDED_BY(internal::time_lock());
  typename Clock::time_point expiration_ PW_GUARDED_BY(internal::time_lock());
};

template <typename Clock>
void TimeProvider<Clock>::RunExpired(typename Clock::time_point now) {
  std::lock_guard lock(internal::time_lock());
  while (!futures_.empty()) {
    if (futures_.front().expiration_ > now) {
      DoInvokeAt(futures_.front().expiration_);
      return;
    }
    futures_.front().waker_.Wake();
    futures_.pop_front();
  }
}

/// @endsubmodule

}  // namespace pw::async2
