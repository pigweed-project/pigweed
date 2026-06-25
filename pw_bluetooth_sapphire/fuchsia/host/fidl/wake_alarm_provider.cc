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

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/wake_alarm_provider.h"

#include <lib/async/cpp/wait.h>
#include <lib/zx/clock.h>
#include <lib/zx/time.h>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"

namespace bthost {

namespace {

// Helper to convert Pigweed SystemClock deadline to Fuchsia boot time.
// This handles the potential offset between Pigweed's SystemClock (usually
// monotonic) and the Fuchsia boot clock.
zx::time_boot ToZxBootTime(pw::chrono::SystemClock::time_point deadline) {
  // Get current monotonic and boot times to calculate the offset.
  // We use raw zx_time_t because zx::time classes for different clocks do not
  // support subtraction.
  zx_time_t mono_now = zx_clock_get_monotonic();
  zx_time_t boot_now = zx_clock_get_boot();
  zx_duration_t offset = boot_now - mono_now;

  // We assume SystemClock is monotonic.
  auto deadline_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         deadline.time_since_epoch())
                         .count();
  return zx::time_boot(deadline_ns + offset);
}

}  // namespace

class FuchsiaWakeAlarmProvider::FuchsiaWakeAlarm final
    : public pw::bluetooth_sapphire::WakeAlarm {
 public:
  FuchsiaWakeAlarm(WeakSelf<FuchsiaWakeAlarmProvider>::WeakPtr provider,
                   uint64_t id)
      : WakeAlarm([provider, id]() {
          if (provider.is_alive()) {
            provider->CancelAlarm(id);
          }
        }) {}
};

std::unique_ptr<FuchsiaWakeAlarmProvider> FuchsiaWakeAlarmProvider::Create(
    fidl::ClientEnd<fuchsia_time_alarms::WakeAlarms> client_end,
    pw::bluetooth_sapphire::LeaseProvider& lease_provider,
    async_dispatcher_t* dispatcher) {
  if (!client_end.is_valid()) {
    bt_log(WARN, "fidl", "invalid wake alarms client end provided");
    return nullptr;
  }

  return std::unique_ptr<FuchsiaWakeAlarmProvider>(new FuchsiaWakeAlarmProvider(
      std::move(client_end), lease_provider, dispatcher));
}

FuchsiaWakeAlarmProvider::FuchsiaWakeAlarmProvider(
    fidl::ClientEnd<fuchsia_time_alarms::WakeAlarms> client_end,
    pw::bluetooth_sapphire::LeaseProvider& lease_provider,
    async_dispatcher_t* dispatcher)
    : lease_provider_(lease_provider), dispatcher_(dispatcher) {
  client_.emplace(std::move(client_end), dispatcher);
}

FuchsiaWakeAlarmProvider::~FuchsiaWakeAlarmProvider() {
  // Explicitly fail all pending alarms to ensure callbacks are called if the
  // provider is destroyed before FIDL responses arrive.
  for (auto& [id, alarm] : pending_alarms_) {
    bt_log(DEBUG, "fidl", "Failing pending wake alarm %lu on destruction", id);
    alarm.callback(pw::Status::Cancelled());
  }
  pending_alarms_.clear();
}

pw::Result<pw::bluetooth_sapphire::WakeAlarm> FuchsiaWakeAlarmProvider::Set(
    PW_SAPPHIRE_WAKE_ALARM_TOKEN_TYPE name,
    pw::chrono::SystemClock::time_point deadline,
    pw::Function<void(pw::Result<pw::bluetooth_sapphire::Lease>)> callback) {
  if (!client_.has_value() || !client_->is_valid()) {
    return pw::Status::FailedPrecondition();
  }

  // Acquire a wake lease to keep the system awake during scheduling.
  auto scheduling_lease = PW_SAPPHIRE_ACQUIRE_LEASE(lease_provider_, name);
  if (!scheduling_lease.ok()) {
    bt_log(ERROR,
           "fidl",
           "Failed to acquire scheduling lease for alarm: %s",
           scheduling_lease.status().str());
    return scheduling_lease.status();
  }

  uint64_t id = next_alarm_id_++;
  bt_log(DEBUG, "fidl", "Setting wake alarm %lu (name: %s)", id, name);

  zx::event setup_event;
  if (zx_status_t status = zx::event::create(0, &setup_event);
      status != ZX_OK) {
    bt_log(ERROR,
           "fidl",
           "Failed to create setup event for alarm %lu: %s",
           id,
           zx_status_get_string(status));
    return pw::Status::Internal();
  }

  zx::event setup_event_copy;
  if (zx_status_t status =
          setup_event.duplicate(ZX_RIGHTS_BASIC, &setup_event_copy);
      status != ZX_OK) {
    bt_log(ERROR,
           "fidl",
           "Failed to duplicate setup event for alarm %lu: %s",
           id,
           zx_status_get_string(status));
    return pw::Status::Internal();
  }

  auto wait = std::make_unique<async::WaitOnce>(setup_event_copy.get(),
                                                ZX_EVENT_SIGNALED);
  auto wait_ptr = wait.get();
  wait_ptr->Begin(dispatcher_,
                  [event = std::move(setup_event_copy),
                   lease = std::move(*scheduling_lease),
                   id](async_dispatcher_t*,
                       async::WaitOnce*,
                       zx_status_t status,
                       const zx_packet_signal_t*) {
                    if (status != ZX_OK) {
                      bt_log(
                          ERROR,
                          "fidl",
                          "Failed to wait for wake alarm %lu setup event: %s",
                          id,
                          zx_status_get_string(status));
                    } else {
                      bt_log(DEBUG,
                             "fidl",
                             "Wake alarm %lu setup event signaled, releasing "
                             "scheduling lease",
                             id);
                    }
                    // lease goes out of scope here and is released.
                  });

  pending_alarms_.emplace(id,
                          PendingAlarm{std::move(callback), std::move(wait)});

  zx::time_boot boot_deadline = ToZxBootTime(deadline);

  fuchsia_time_alarms::SetAndWaitArgs request(
      boot_deadline,
      fuchsia_time_alarms::SetMode::WithNotifySetupDone(std::move(setup_event)),
      std::to_string(id));

  (*client_)
      ->SetAndWait(std::move(request))
      .Then([this, id, name, self = weak_self_.GetWeakPtr()](auto& result) {
        if (!self.is_alive()) {
          return;
        }

        auto it = pending_alarms_.find(id);
        if (it == pending_alarms_.end()) {
          return;
        }

        auto cb = std::move(it->second.callback);
        pending_alarms_.erase(it);

        if (result.is_error()) {
          if (result.error_value().is_domain_error() &&
              result.error_value().domain_error() ==
                  fuchsia_time_alarms::WakeAlarmsError::kDropped) {
            bt_log(DEBUG, "fidl", "Wake alarm %lu was dropped/canceled", id);
            cb(pw::Status::Cancelled());
          } else {
            bt_log(ERROR,
                   "fidl",
                   "WakeAlarms.SetAndWait failed for alarm %lu: %s",
                   id,
                   result.error_value().FormatDescription().c_str());
            cb(pw::Status::Internal());
          }
          return;
        }

        bt_log(DEBUG, "fidl", "Wake alarm %lu fired successfully", id);
        auto lease_result = PW_SAPPHIRE_ACQUIRE_LEASE(lease_provider_, name);
        if (!lease_result.ok()) {
          bt_log(ERROR,
                 "fidl",
                 "Failed to acquire lease for fired alarm %lu: %s",
                 id,
                 lease_result.status().str());
          cb(lease_result.status());
          return;
        }
        auto lease = pw::bluetooth_sapphire::Lease(
            [token = std::move(result->keep_alive()),
             provider_lease = std::move(lease_result.value())]() mutable {
              token.reset();
            });

        cb(std::move(lease));
      });

  return FuchsiaWakeAlarm(weak_self_.GetWeakPtr(), id);
}

void FuchsiaWakeAlarmProvider::CancelAlarm(uint64_t id) {
  auto it = pending_alarms_.find(id);
  if (it == pending_alarms_.end()) {
    return;
  }

  if (!client_.has_value()) {
    return;
  }

  // We don't erase it here because we want the SetAndWait response to handle
  // the callback with kDropped.
  fuchsia_time_alarms::WakeAlarmsCancelRequest request;
  request.alarm_id() = std::to_string(id);
  (void)(*client_)->Cancel(std::move(request));
}

}  // namespace bthost
