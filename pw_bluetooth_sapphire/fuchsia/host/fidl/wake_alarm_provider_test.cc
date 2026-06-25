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

#include <fidl/fuchsia.time.alarms/cpp/test_base.h>

#include "pw_bluetooth_sapphire/fake_lease_provider.h"
#include "pw_bluetooth_sapphire/internal/host/testing/loop_fixture.h"

namespace bthost {
namespace {

class FakeWakeAlarms final
    : public fidl::testing::TestBase<fuchsia_time_alarms::WakeAlarms> {
 public:
  void SetAndWait(SetAndWaitRequest& request,
                  SetAndWaitCompleter::Sync& completer) override {
    last_alarm_id_ = request.alarm_id();
    last_deadline_ = request.deadline();
    completers_.emplace(request.alarm_id(), completer.ToAsync());

    if (request.mode().Which() ==
        fuchsia_time_alarms::SetMode::Tag::kNotifySetupDone) {
      if (auto_signal_setup_) {
        request.mode().notify_setup_done()->signal(0, ZX_EVENT_SIGNALED);
      } else {
        setup_events_.emplace(
            request.alarm_id(),
            std::move(request.mode().notify_setup_done().value()));
      }
    }
  }

  void Cancel(CancelRequest& request,
              CancelCompleter::Sync& completer) override {
    auto it = completers_.find(request.alarm_id());
    if (it != completers_.end()) {
      it->second.Reply(
          fit::error(fuchsia_time_alarms::WakeAlarmsError::kDropped));
      completers_.erase(it);
    }
    setup_events_.erase(request.alarm_id());
  }

  void FireAlarm(uint64_t id) {
    auto it = completers_.find(std::to_string(id));
    if (it != completers_.end()) {
      zx::eventpair endpoint0, endpoint1;
      zx::eventpair::create(0, &endpoint0, &endpoint1);

      fuchsia_time_alarms::WakeAlarmsSetAndWaitResponse response;
      response.keep_alive() = std::move(endpoint0);

      it->second.Reply(fit::ok(std::move(response)));
      completers_.erase(it);
    }
    setup_events_.erase(std::to_string(id));
  }

  void FailAlarm(uint64_t id, fuchsia_time_alarms::WakeAlarmsError error) {
    auto it = completers_.find(std::to_string(id));
    if (it != completers_.end()) {
      it->second.Reply(fit::error(error));
      completers_.erase(it);
    }
    setup_events_.erase(std::to_string(id));
  }

  void SignalSetup(const std::string& id) {
    auto it = setup_events_.find(id);
    if (it != setup_events_.end()) {
      it->second.signal(0, ZX_EVENT_SIGNALED);
      setup_events_.erase(it);
    }
  }

  void set_auto_signal_setup(bool auto_signal) {
    auto_signal_setup_ = auto_signal;
  }

  void handle_unknown_method(
      fidl::UnknownMethodMetadata<fuchsia_time_alarms::WakeAlarms>,
      fidl::UnknownMethodCompleter::Sync&) override {}
  void NotImplemented_(const std::string&, fidl::CompleterBase&) override {}

  const std::string& last_alarm_id() const { return last_alarm_id_; }
  zx::time_boot last_deadline() const { return last_deadline_; }

 private:
  std::string last_alarm_id_;
  zx::time_boot last_deadline_;
  std::unordered_map<std::string, SetAndWaitCompleter::Async> completers_;
  bool auto_signal_setup_ = true;
  std::unordered_map<std::string, zx::event> setup_events_;
};

class WakeAlarmProviderTest : public bt::testing::TestLoopFixture {
 public:
  void SetUp() override {
    auto endpoints = fidl::CreateEndpoints<fuchsia_time_alarms::WakeAlarms>();
    ASSERT_TRUE(endpoints.is_ok());

    fake_wake_alarms_.emplace();
    binding_ = fidl::BindServer(
        dispatcher(), std::move(endpoints->server), &fake_wake_alarms_.value());

    provider_ = FuchsiaWakeAlarmProvider::Create(
        std::move(endpoints->client), lease_provider_, dispatcher());
  }

  FuchsiaWakeAlarmProvider& provider() { return *provider_; }
  FakeWakeAlarms& fake() { return fake_wake_alarms_.value(); }
  pw::bluetooth_sapphire::testing::FakeLeaseProvider& lease_provider() {
    return lease_provider_;
  }

 private:
  std::optional<FakeWakeAlarms> fake_wake_alarms_;
  std::optional<fidl::ServerBindingRef<fuchsia_time_alarms::WakeAlarms>>
      binding_;
  pw::bluetooth_sapphire::testing::FakeLeaseProvider lease_provider_;
  std::unique_ptr<FuchsiaWakeAlarmProvider> provider_;
};

TEST_F(WakeAlarmProviderTest, SetAlarmAndFire) {
  pw::chrono::SystemClock::time_point deadline(std::chrono::nanoseconds(12345));
  int call_count = 0;
  pw::Status last_status = pw::Status::Unknown();
  pw::Result<pw::bluetooth_sapphire::Lease> fired_lease = pw::Status::Unknown();

  auto result =
      provider().Set("SetAlarmAndFire",
                     deadline,
                     [&](pw::Result<pw::bluetooth_sapphire::Lease> lease) {
                       call_count++;
                       last_status = lease.status();
                       fired_lease = std::move(lease);
                     });
  ASSERT_TRUE(result.ok());
  auto alarm = std::move(*result);

  RunLoopUntilIdle();
  // The deadline should be 12345 + offset. We check that it's at least 12345.
  EXPECT_GE(fake().last_deadline().get(), 12345);

  EXPECT_EQ(lease_provider().lease_count(), 0u);

  fake().FireAlarm(0);
  RunLoopUntilIdle();

  EXPECT_EQ(call_count, 1);
  EXPECT_TRUE(last_status.ok());
  EXPECT_TRUE(fired_lease.ok());
  EXPECT_EQ(lease_provider().lease_count(), 1u);

  fired_lease = pw::Status::Unknown();
  EXPECT_EQ(lease_provider().lease_count(), 0u);
}

TEST_F(WakeAlarmProviderTest, CancelAlarm) {
  pw::chrono::SystemClock::time_point deadline(std::chrono::nanoseconds(12345));
  int call_count = 0;
  pw::Status last_status = pw::Status::Unknown();

  auto result =
      provider().Set("CancelAlarm",
                     deadline,
                     [&](pw::Result<pw::bluetooth_sapphire::Lease> lease) {
                       call_count++;
                       last_status = lease.status();
                     });
  ASSERT_TRUE(result.ok());
  auto alarm = std::move(*result);

  RunLoopUntilIdle();
  alarm.Cancel();
  RunLoopUntilIdle();

  EXPECT_EQ(call_count, 1);
  EXPECT_TRUE(last_status.IsCancelled());
}

TEST_F(WakeAlarmProviderTest, SetMultipleAlarms) {
  int call_count_1 = 0;
  int call_count_2 = 0;

  auto result_1 = provider().Set(
      "SetMultipleAlarms_1",
      pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(1)),
      [&](pw::Result<pw::bluetooth_sapphire::Lease>) { call_count_1++; });
  ASSERT_TRUE(result_1.ok());
  auto alarm_1 = std::move(*result_1);
  auto result_2 = provider().Set(
      "SetMultipleAlarms_2",
      pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(2)),
      [&](pw::Result<pw::bluetooth_sapphire::Lease>) { call_count_2++; });
  ASSERT_TRUE(result_2.ok());
  auto alarm_2 = std::move(*result_2);

  RunLoopUntilIdle();

  fake().FireAlarm(1);
  RunLoopUntilIdle();
  EXPECT_EQ(call_count_1, 0);
  EXPECT_EQ(call_count_2, 1);

  fake().FireAlarm(0);
  RunLoopUntilIdle();
  EXPECT_EQ(call_count_1, 1);
  EXPECT_EQ(call_count_2, 1);
}

TEST_F(WakeAlarmProviderTest, AlarmHandleDestructionCancels) {
  int call_count = 0;
  pw::Status last_status = pw::Status::Unknown();

  {
    auto result = provider().Set(
        "AlarmHandleDestructionCancels",
        pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(1)),
        [&](pw::Result<pw::bluetooth_sapphire::Lease> lease) {
          call_count++;
          last_status = lease.status();
        });
    ASSERT_TRUE(result.ok());
    auto alarm = std::move(*result);
    RunLoopUntilIdle();
  }  // alarm goes out of scope and should cancel.

  RunLoopUntilIdle();
  EXPECT_EQ(call_count, 1);
  EXPECT_TRUE(last_status.IsCancelled());
}

TEST_F(WakeAlarmProviderTest, FidlInternalError) {
  int call_count = 0;
  pw::Status last_status = pw::Status::Unknown();

  auto result = provider().Set(
      "FidlInternalError",
      pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(1)),
      [&](pw::Result<pw::bluetooth_sapphire::Lease> lease) {
        call_count++;
        last_status = lease.status();
      });
  ASSERT_TRUE(result.ok());
  auto alarm = std::move(*result);

  RunLoopUntilIdle();

  fake().FailAlarm(0, fuchsia_time_alarms::WakeAlarmsError::kInternal);
  RunLoopUntilIdle();

  EXPECT_EQ(call_count, 1);

  EXPECT_TRUE(last_status.IsInternal());
}

TEST_F(WakeAlarmProviderTest, ProviderDestruction) {
  int call_count = 0;
  pw::Status last_status = pw::Status::Unknown();

  auto endpoints = fidl::CreateEndpoints<fuchsia_time_alarms::WakeAlarms>();
  ASSERT_TRUE(endpoints.is_ok());

  FakeWakeAlarms local_fake;
  auto binding =
      fidl::BindServer(dispatcher(), std::move(endpoints->server), &local_fake);

  auto local_provider = FuchsiaWakeAlarmProvider::Create(
      std::move(endpoints->client), lease_provider(), dispatcher());

  auto result = local_provider->Set(
      "ProviderDestruction",
      pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(1)),
      [&](pw::Result<pw::bluetooth_sapphire::Lease> lease) {
        call_count++;
        last_status = lease.status();
      });
  ASSERT_TRUE(result.ok());
  auto alarm = std::move(*result);

  RunLoopUntilIdle();

  local_provider.reset();
  RunLoopUntilIdle();

  // The callback should have been called with Cancelled on destruction.
  EXPECT_EQ(call_count, 1);
  EXPECT_TRUE(last_status.IsCancelled());

  local_fake.FireAlarm(0);
  RunLoopUntilIdle();

  // Should not be called again.
  EXPECT_EQ(call_count, 1);
}

TEST_F(WakeAlarmProviderTest, CreateWithInvalidClientReturnsNull) {
  auto local_provider = FuchsiaWakeAlarmProvider::Create(
      fidl::ClientEnd<fuchsia_time_alarms::WakeAlarms>(),
      lease_provider(),
      dispatcher());
  EXPECT_EQ(local_provider, nullptr);
}

TEST_F(WakeAlarmProviderTest, CancelAlarmTwice) {
  pw::chrono::SystemClock::time_point deadline(std::chrono::nanoseconds(12345));
  int call_count = 0;
  pw::Status last_status = pw::Status::Unknown();

  auto result =
      provider().Set("CancelAlarmTwice",
                     deadline,
                     [&](pw::Result<pw::bluetooth_sapphire::Lease> lease) {
                       call_count++;
                       last_status = lease.status();
                     });
  ASSERT_TRUE(result.ok());
  auto alarm = std::move(*result);

  RunLoopUntilIdle();
  alarm.Cancel();
  alarm.Cancel();
  RunLoopUntilIdle();

  EXPECT_EQ(call_count, 1);
  EXPECT_TRUE(last_status.IsCancelled());
}

TEST_F(WakeAlarmProviderTest, CancelAlarmAfterFire) {
  pw::chrono::SystemClock::time_point deadline(std::chrono::nanoseconds(12345));
  int call_count = 0;
  pw::Status last_status = pw::Status::Unknown();

  auto result =
      provider().Set("CancelAlarmAfterFire",
                     deadline,
                     [&](pw::Result<pw::bluetooth_sapphire::Lease> lease) {
                       call_count++;
                       last_status = lease.status();
                     });
  ASSERT_TRUE(result.ok());
  auto alarm = std::move(*result);

  RunLoopUntilIdle();
  fake().FireAlarm(0);
  RunLoopUntilIdle();

  EXPECT_EQ(call_count, 1);
  EXPECT_TRUE(last_status.ok());

  alarm.Cancel();
  RunLoopUntilIdle();

  EXPECT_EQ(call_count, 1);
  EXPECT_TRUE(last_status.ok());
}

TEST_F(WakeAlarmProviderTest, SchedulingLeaseAcquiredAndReleased) {
  pw::chrono::SystemClock::time_point deadline(std::chrono::nanoseconds(12345));

  EXPECT_EQ(lease_provider().lease_count(), 0u);

  auto result =
      provider().Set("SchedulingLeaseAcquiredAndReleased",
                     deadline,
                     [](pw::Result<pw::bluetooth_sapphire::Lease>) {});
  ASSERT_TRUE(result.ok());

  // Lease should be acquired immediately.
  EXPECT_EQ(lease_provider().lease_count(), 1u);

  // Run loop to let FIDL call go through and fake server signal the event.
  RunLoopUntilIdle();

  // Lease should be released now.
  EXPECT_EQ(lease_provider().lease_count(), 0u);

  // Clean up the alarm.
  fake().FireAlarm(0);
  RunLoopUntilIdle();
}

TEST_F(WakeAlarmProviderTest, SetAlarmFailsWhenLeaseAcquisitionFails) {
  pw::chrono::SystemClock::time_point deadline(std::chrono::nanoseconds(12345));

  lease_provider().set_acquire_status(pw::Status::Unavailable());

  auto result =
      provider().Set("SetAlarmFailsWhenLeaseAcquisitionFails",
                     deadline,
                     [](pw::Result<pw::bluetooth_sapphire::Lease>) {});

  EXPECT_EQ(result.status(), pw::Status::Unavailable());
}

TEST_F(WakeAlarmProviderTest, ProviderDestructionBeforeSetupSignal) {
  int call_count = 0;
  pw::Status last_status = pw::Status::Unknown();

  auto endpoints = fidl::CreateEndpoints<fuchsia_time_alarms::WakeAlarms>();
  ASSERT_TRUE(endpoints.is_ok());

  FakeWakeAlarms local_fake;
  local_fake.set_auto_signal_setup(false);  // Do not signal setup automatically

  auto binding =
      fidl::BindServer(dispatcher(), std::move(endpoints->server), &local_fake);

  auto local_provider = FuchsiaWakeAlarmProvider::Create(
      std::move(endpoints->client), lease_provider(), dispatcher());

  auto result = local_provider->Set(
      "ProviderDestructionBeforeSetupSignal",
      pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(1)),
      [&](pw::Result<pw::bluetooth_sapphire::Lease> lease) {
        call_count++;
        last_status = lease.status();
      });
  ASSERT_TRUE(result.ok());
  auto alarm = std::move(*result);

  RunLoopUntilIdle();
  // Setup is NOT done yet. Lease should still be held.
  EXPECT_EQ(lease_provider().lease_count(), 1u);

  // Now destroy the provider before the setup signal is received.
  local_provider.reset();
  RunLoopUntilIdle();

  // The callback should have been called with Cancelled on destruction.
  EXPECT_EQ(call_count, 1);
  EXPECT_TRUE(last_status.IsCancelled());

  // The lease should have been released because the provider (and its pending
  // alarms) were destroyed.
  EXPECT_EQ(lease_provider().lease_count(), 0u);
}

TEST_F(WakeAlarmProviderTest, CancelAlarmBeforeSetupSignal) {
  pw::chrono::SystemClock::time_point deadline(std::chrono::nanoseconds(12345));
  int call_count = 0;
  pw::Status last_status = pw::Status::Unknown();

  fake().set_auto_signal_setup(false);  // Do not signal setup automatically

  auto result =
      provider().Set("CancelAlarmBeforeSetupSignal",
                     deadline,
                     [&](pw::Result<pw::bluetooth_sapphire::Lease> lease) {
                       call_count++;
                       last_status = lease.status();
                     });
  ASSERT_TRUE(result.ok());
  auto alarm = std::move(*result);

  RunLoopUntilIdle();
  // Lease should still be held.
  EXPECT_EQ(lease_provider().lease_count(), 1u);

  // Cancel the alarm.
  alarm.Cancel();
  RunLoopUntilIdle();

  // Callback should be called with Cancelled (since the mock returns kDropped).
  EXPECT_EQ(call_count, 1);
  EXPECT_TRUE(last_status.IsCancelled());

  // The lease should be released.
  EXPECT_EQ(lease_provider().lease_count(), 0u);
}

}  // namespace
}  // namespace bthost
