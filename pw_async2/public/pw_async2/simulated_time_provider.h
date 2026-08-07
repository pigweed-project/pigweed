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

#include "pw_async2/time_provider.h"
#include "pw_sync/interrupt_spin_lock.h"

namespace pw::async2 {

/// @submodule{pw_async2,time}

/// A simulated `TimeProvider` suitable for testing APIs which use `Timer`.
template <typename Clock>
class SimulatedTimeProvider final : public TimeProvider<Clock> {
 public:
  SimulatedTimeProvider(
      typename Clock::time_point timestamp =
          typename Clock::time_point(typename Clock::duration(0)))
      : now_(timestamp) {}

  /// Advances the simulated time and runs any newly-expired timers.
  void AdvanceTime(typename Clock::duration duration) {
    lock_.lock();
    SetTimeUnlockAndRun(now_ + duration);
  }

  /// Advances the simulated time until the next point at which a timer
  /// would fire.
  ///
  /// Returns whether any timers were waiting to be run.
  bool AdvanceUntilNextExpiration() {
    lock_.lock();
    if (next_wake_time_ == std::nullopt) {
      lock_.unlock();
      return false;
    }
    SetTimeUnlockAndRun(*next_wake_time_);
    return true;
  }

  /// Modifies the simulated time and runs any newly-expired timers.
  ///
  /// WARNING: Use of this function with a timestamp older than the current
  /// `now()` will violate the is_monotonic clock attribute. We don't like it
  /// when time goes backwards!
  void SetTime(typename Clock::time_point new_now) {
    lock_.lock();
    SetTimeUnlockAndRun(new_now);
  }

  /// Explicitly run expired timers.
  ///
  /// Calls to this function are not usually necessary, as `AdvanceTime` and
  /// `SetTime` will trigger expired timers to run. However, if a timer is set
  /// for a time in the past and neither `AdvanceTime` nor `SetTime` are
  /// subsequently invoked, the timer will not have a chance to run until
  /// one of `AdvanceTime`, `SetTime`, or `RunExpiredTimers` has been called.
  void RunExpiredTimers() { RunExpired(now()); }

  typename Clock::time_point now() final {
    std::lock_guard lock(lock_);
    return now_;
  }

  std::optional<typename Clock::time_point> NextExpiration() {
    std::lock_guard lock(lock_);
    return next_wake_time_;
  }

  std::optional<typename Clock::duration> TimeUntilNextExpiration() {
    std::lock_guard lock(lock_);
    if (next_wake_time_ == std::nullopt) {
      return std::nullopt;
    }
    return *next_wake_time_ - now_;
  }

 private:
  void SetTimeUnlockAndRun(typename Clock::time_point new_now)
      PW_UNLOCK_FUNCTION(lock_) {
    now_ = new_now;
    if (new_now >= next_wake_time_) {
      next_wake_time_ = std::nullopt;
      lock_.unlock();
      TimeProvider<Clock>::RunExpired(new_now);
    } else {
      lock_.unlock();
    }
  }

  void DoInvokeAt(typename Clock::time_point wake_time) final
      PW_LOCKS_EXCLUDED(internal::time_lock()) {
    std::lock_guard lock(lock_);
    next_wake_time_ = wake_time;

    // We don't need to actually schedule anything here since calls to
    // `RunExpired` are triggered directly by user calls to
    // `AdvanceTime`/`SetTime`.
    //
    // Note: we cannot run the timer here even if it was in the past because
    // we might be *inside* the current callback due to a nested invocation of
    // `InvokeAfter`/`InvokeAt`.
  }

  void DoCancel() final PW_LOCKS_EXCLUDED(internal::time_lock()) {
    std::lock_guard lock(lock_);
    next_wake_time_ = std::nullopt;

    // We don't need to do anything here since `RunExpired` itself is safe to
    // call redundantly-- it will filter out extra invocations.
  }

  mutable sync::InterruptSpinLock lock_;
  typename Clock::time_point now_ PW_GUARDED_BY(lock_);
  std::optional<typename Clock::time_point> next_wake_time_
      PW_GUARDED_BY(lock_);
};

/// @endsubmodule

}  // namespace pw::async2
