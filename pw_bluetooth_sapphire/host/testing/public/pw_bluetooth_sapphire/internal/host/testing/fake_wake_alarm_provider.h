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

#include <memory>
#include <unordered_map>
#include <utility>

#include "pw_async/dispatcher.h"
#include "pw_bluetooth_sapphire/internal/host/common/smart_task.h"
#include "pw_bluetooth_sapphire/lease.h"
#include "pw_bluetooth_sapphire/wake_alarm.h"

namespace bt::testing {

// A WakeAlarm handle implementation used for dependency injection in unit
// tests.
class TestWakeAlarm final : public pw::bluetooth_sapphire::WakeAlarm {
 public:
  explicit TestWakeAlarm(pw::Function<void()> cancel_fn)
      : WakeAlarm(std::move(cancel_fn)) {}
};

// A fake WakeAlarmProvider used for dependency injection in unit tests.
class FakeWakeAlarmProvider final
    : public pw::bluetooth_sapphire::WakeAlarmProvider {
 public:
  explicit FakeWakeAlarmProvider(pw::async::Dispatcher& dispatcher)
      : dispatcher_(dispatcher) {}
  ~FakeWakeAlarmProvider() override = default;
  FakeWakeAlarmProvider(FakeWakeAlarmProvider&&) = delete;
  FakeWakeAlarmProvider& operator=(FakeWakeAlarmProvider&&) = delete;

  struct PendingAlarm {
    explicit PendingAlarm(pw::async::Dispatcher& dispatcher)
        : task(dispatcher) {}
    bt::SmartTask task;
    pw::Function<void(pw::Result<pw::bluetooth_sapphire::Lease>)> callback;
  };

  pw::Result<pw::bluetooth_sapphire::WakeAlarm> Set(
      PW_SAPPHIRE_WAKE_ALARM_TOKEN_TYPE /*name*/,
      pw::chrono::SystemClock::time_point deadline,
      pw::Function<void(pw::Result<pw::bluetooth_sapphire::Lease>)> callback)
      override {
    uint64_t id = next_alarm_id_++;
    auto alarm = std::make_unique<PendingAlarm>(dispatcher_);
    alarm->callback = std::move(callback);

    alarm->task.set_function(
        [this, id](pw::async::Context&, pw::Status status) {
          if (status.ok()) {
            TriggerAlarm(id);
          }
        });
    alarm->task.PostAt(deadline);

    pending_alarms_[id] = std::move(alarm);

    return TestWakeAlarm([this, id]() { CancelAlarm(id); });
  }

  void TriggerAlarm(uint64_t id) {
    auto it = pending_alarms_.find(id);
    if (it != pending_alarms_.end()) {
      auto callback = std::move(it->second->callback);
      pending_alarms_.erase(it);
      active_leases_++;
      callback(pw::bluetooth_sapphire::Lease([this]() { active_leases_--; }));
    }
  }

  void CancelAlarm(uint64_t id) {
    auto it = pending_alarms_.find(id);
    if (it != pending_alarms_.end()) {
      it->second->task.Cancel();
      auto callback = std::move(it->second->callback);
      pending_alarms_.erase(it);
      callback(pw::Status::Cancelled());
    }
  }

  bool HasPendingAlarms() const { return !pending_alarms_.empty(); }
  size_t active_leases() const { return active_leases_; }

 private:
  pw::async::Dispatcher& dispatcher_;
  uint64_t next_alarm_id_ = 0;
  size_t active_leases_ = 0;
  std::unordered_map<uint64_t, std::unique_ptr<PendingAlarm>> pending_alarms_;
};

}  // namespace bt::testing
