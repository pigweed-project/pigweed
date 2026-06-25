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

#include "pw_bluetooth_sapphire/config.h"
#include "pw_bluetooth_sapphire/lease.h"
#include "pw_chrono/system_clock.h"
#include "pw_function/function.h"
#include "pw_result/result.h"
#include "pw_status/status.h"

namespace pw::bluetooth_sapphire {

// A handle representing a scheduled wake alarm.
class WakeAlarm {
 public:
  WakeAlarm() = default;

  WakeAlarm(WakeAlarm&& other) noexcept
      : cancel_fn_(std::move(other.cancel_fn_)) {}

  WakeAlarm& operator=(WakeAlarm&& other) noexcept {
    if (this != &other) {
      Cancel();
      cancel_fn_ = std::move(other.cancel_fn_);
    }
    return *this;
  }

  WakeAlarm(const WakeAlarm&) = delete;
  WakeAlarm& operator=(const WakeAlarm&) = delete;

  virtual ~WakeAlarm() { Cancel(); }

  // Cancel the scheduled alarm. If the alarm has already fired or was
  // already canceled, this is a no-op.
  void Cancel() {
    if (cancel_fn_) {
      cancel_fn_();
      cancel_fn_ = nullptr;
    }
  }

 protected:
  explicit WakeAlarm(pw::Function<void()> cancel_fn)
      : cancel_fn_(std::move(cancel_fn)) {}

 private:
  pw::Function<void()> cancel_fn_;
};

// Interface for a provider of wake alarms.
class WakeAlarmProvider {
 public:
  virtual ~WakeAlarmProvider() = default;

  // Sets a wake alarm to fire at the given deadline.
  //
  // When the alarm fires, the provided callback will be invoked. The
  // callback is provided with a Status (indicating if the alarm fired
  // successfully or was canceled/errored) and a Lease that keeps the
  // system awake while it is held.
  //
  // On Fuchsia, the deadline is interpreted as a boot-timeline instant.
  //
  // @returns A pw::Result containing a WakeAlarm handle that can be used to
  // cancel the alarm, or a non-ok status on failure.
  virtual pw::Result<WakeAlarm> Set(
      PW_SAPPHIRE_WAKE_ALARM_TOKEN_TYPE name,
      pw::chrono::SystemClock::time_point deadline,
      pw::Function<void(pw::Result<Lease>)> callback) = 0;
};

}  // namespace pw::bluetooth_sapphire
