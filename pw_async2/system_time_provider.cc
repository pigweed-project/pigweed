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

#include "pw_async2/system_time_provider.h"

#include <mutex>

#include "pw_async2/time_provider.h"
#include "pw_chrono/system_clock.h"
#include "pw_chrono/system_timer.h"
#include "pw_memory/no_destructor.h"
#include "pw_sync/lock_annotations.h"

namespace pw::async2 {
namespace {

using ::pw::chrono::SystemClock;

class SystemTimeProvider final : public TimeProvider<SystemClock> {
 public:
  SystemTimeProvider()
      : timer_([this](SystemClock::time_point) {
          {
            std::lock_guard lock(internal::time_lock());
            programmed_deadline_ = SystemClock::time_point::max();
          }
          RunExpired(SystemClock::now());
        }) {}

 private:
  SystemClock::time_point now() final { return SystemClock::now(); }

  void DoInvokeAt(SystemClock::time_point time_point) final
      PW_LOCKS_EXCLUDED(internal::time_lock()) {
    {
      std::lock_guard lock(internal::time_lock());
      // Only reprogram the SystemTimer to fire sooner, since reprogramming can
      // be expensive. If it fires early, RunExpired() reschedules it.
      if (time_point >= programmed_deadline_) {
        return;
      }
      programmed_deadline_ = time_point;
    }

    // The SystemTimer is programmed without holding the lock, so another
    // thread may concurrently request an earlier time, and this call could
    // override it. Repeat until the earliest requested time is programmed.
    while (true) {
      timer_.InvokeAt(time_point);

      std::lock_guard lock(internal::time_lock());
      if (programmed_deadline_ >= time_point) {
        return;
      }
      time_point = programmed_deadline_;
    }
  }

  // Leave the timer armed rather than canceling it, which can be expensive. If
  // it fires with no pending timers, RunExpired() does nothing.
  void DoCancel() final PW_LOCKS_EXCLUDED(internal::time_lock()) {}

  // The time at which the SystemTimer is set to fire, or max() if it is not
  // armed. Declared before timer_ so it is initialized before the timer is
  // created and outlives the timer on destruction.
  SystemClock::time_point programmed_deadline_
      PW_GUARDED_BY(internal::time_lock()) = SystemClock::time_point::max();

  pw::chrono::SystemTimer timer_;
};

}  // namespace

TimeProvider<SystemClock>& GetSystemTimeProvider() {
  static pw::NoDestructor<SystemTimeProvider> time_provider;
  return *time_provider;
}

}  // namespace pw::async2
