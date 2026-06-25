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

#include <fidl/fuchsia.time.alarms/cpp/fidl.h>
#include <lib/async/cpp/wait.h>

#include <optional>
#include <string>
#include <unordered_map>

#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/common/weak_self.h"
#include "pw_bluetooth_sapphire/lease.h"
#include "pw_bluetooth_sapphire/wake_alarm.h"

namespace bthost {

class FuchsiaWakeAlarmProvider final
    : public pw::bluetooth_sapphire::WakeAlarmProvider {
 public:
  static std::unique_ptr<FuchsiaWakeAlarmProvider> Create(
      fidl::ClientEnd<fuchsia_time_alarms::WakeAlarms> client_end,
      pw::bluetooth_sapphire::LeaseProvider& lease_provider,
      async_dispatcher_t* dispatcher);

  ~FuchsiaWakeAlarmProvider() override;

  // WakeAlarmProvider overrides:
  pw::Result<pw::bluetooth_sapphire::WakeAlarm> Set(
      PW_SAPPHIRE_WAKE_ALARM_TOKEN_TYPE name,
      pw::chrono::SystemClock::time_point deadline,
      pw::Function<void(pw::Result<pw::bluetooth_sapphire::Lease>)> callback)
      override;

 private:
  FuchsiaWakeAlarmProvider(
      fidl::ClientEnd<fuchsia_time_alarms::WakeAlarms> client_end,
      pw::bluetooth_sapphire::LeaseProvider& lease_provider,
      async_dispatcher_t* dispatcher);

  class FuchsiaWakeAlarm;

  void CancelAlarm(uint64_t id);

  struct PendingAlarm {
    pw::Function<void(pw::Result<pw::bluetooth_sapphire::Lease>)> callback;
    std::unique_ptr<async::WaitOnce> wait;
  };

  pw::bluetooth_sapphire::LeaseProvider& lease_provider_;
  async_dispatcher_t* dispatcher_;
  std::optional<fidl::Client<fuchsia_time_alarms::WakeAlarms>> client_;
  std::unordered_map<uint64_t, PendingAlarm> pending_alarms_;
  uint64_t next_alarm_id_ = 0;

  WeakSelf<FuchsiaWakeAlarmProvider> weak_self_{this};

  BT_DISALLOW_COPY_ASSIGN_AND_MOVE(FuchsiaWakeAlarmProvider);
};

}  // namespace bthost
