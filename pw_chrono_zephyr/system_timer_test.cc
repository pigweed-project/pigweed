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

#include "pw_chrono/system_timer.h"

#include <zephyr/kernel.h>

#include <atomic>

#include "pw_chrono/system_clock.h"
#include "pw_chrono_zephyr/system_clock_constants.h"
#include "pw_function/function.h"
#include "pw_sync/timed_thread_notification.h"
#include "pw_unit_test/framework.h"

namespace pw::chrono {
namespace {

using namespace std::chrono_literals;

// A large enough timeout that a guest VM running this test should be able to
// complete work on multiple threads despite what else might be happening on
// the machine.
constexpr auto kSafeTimeout = SystemClock::for_at_least(1000ms);

// A shorter timeout for negative tests (expecting the timer to NOT fire) to
// prevent the entire test suite from exceeding execution time limits.
constexpr auto kNegativeTimeout = SystemClock::for_at_least(500ms);

// Helper class to simplify testing by encapsulating SystemTimer and its
// associated synchronization context (deadline, notification, callback count).
class SystemTimerTestContext {
 public:
  // Constructor for simple tests where the test is done after the first
  // expiration.
  SystemTimerTestContext()
      : timer_([this](SystemClock::time_point expired_deadline) {
          HandleExpiry(expired_deadline);
        }) {}

  // Constructor for more complicated tests that need to do something on
  // expiration, and where the test might continue.
  explicit SystemTimerTestContext(
      pw::Function<void(SystemTimerTestContext&, SystemClock::time_point)>&&
          custom_callback)
      : custom_callback_(std::move(custom_callback)),
        timer_([this](SystemClock::time_point expired_deadline) {
          HandleExpiry(expired_deadline);
        }) {}

  // Obtains the SystemTimer instance to use for testing.
  SystemTimer& timer() { return timer_; }

  // Count of expiration callbacks handled.
  int callback_count() const { return callback_count_; }

  // The time point of the last expired deadline.
  SystemClock::time_point last_expired_deadline() const {
    return last_expired_deadline_;
  }

  // The time point of the last callback execution.
  SystemClock::time_point last_execution_time() const {
    return last_execution_time_;
  }

  // Releases the notification, allowing the test to proceed.
  // The expiration callback should invoke this to signal completion.
  void release_notification() { notification_.release(); }

  // The test body should call this to wait on the timer expiration.
  bool try_acquire_for(SystemClock::duration timeout) {
    return notification_.try_acquire_for(timeout);
  }

 private:
  void HandleExpiry(SystemClock::time_point expired_deadline) {
    last_execution_time_ = SystemClock::now();
    last_expired_deadline_ = expired_deadline;
    callback_count_++;
    if (custom_callback_) {
      custom_callback_(*this, expired_deadline);
    } else {
      notification_.release();
    }
  }

  pw::Function<void(SystemTimerTestContext&, SystemClock::time_point)>
      custom_callback_;
  std::atomic<int> callback_count_{0};
  std::atomic<SystemClock::time_point> last_expired_deadline_{
      SystemClock::time_point()};
  std::atomic<SystemClock::time_point> last_execution_time_{
      SystemClock::time_point()};
  pw::sync::TimedThreadNotification notification_;
  SystemTimer timer_;
};

TEST(SystemTimerZephyr, MaxTimeoutIsPositive) {
  EXPECT_GT(pw::chrono::zephyr::kMaxTimeout.count(), 0);
}

TEST(SystemTimerZephyr, BasicInvokeAt) {
  SystemTimerTestContext context;

  const auto expected_at = SystemClock::now() + SystemClock::for_at_least(50ms);
  context.timer().InvokeAt(expected_at);

  EXPECT_TRUE(context.try_acquire_for(kSafeTimeout));
  EXPECT_EQ(context.callback_count(), 1U);
  EXPECT_EQ(context.last_expired_deadline(), expected_at);
}

TEST(SystemTimerZephyr, BasicInvokeAfter) {
  SystemTimerTestContext context;

  const auto expected_at = SystemClock::now() + SystemClock::for_at_least(50ms);
  context.timer().InvokeAfter(SystemClock::for_at_least(50ms));

  EXPECT_TRUE(context.try_acquire_for(kSafeTimeout));
  EXPECT_EQ(context.callback_count(), 1U);
  EXPECT_GE(context.last_expired_deadline(), expected_at);
}

TEST(SystemTimerZephyr, ScheduleAtNowDoesNotBlockForever) {
  SystemTimerTestContext context;

  // Ensure scheduling a wake up at the current time does not cause issues.
  const auto expected_at = SystemClock::now();
  context.timer().InvokeAt(expected_at);

  EXPECT_TRUE(context.try_acquire_for(kSafeTimeout));
  EXPECT_GE(context.last_expired_deadline(), expected_at);
}

TEST(SystemTimerZephyr, SchedulePastDeadlineDoesNotBlockForever) {
  SystemTimerTestContext context;

  // Ensure scheduling a wake up one tick in the past does not cause issues.
  const auto expected_at = SystemClock::now() - SystemClock::duration(1);
  context.timer().InvokeAt(expected_at);

  EXPECT_TRUE(context.try_acquire_for(kSafeTimeout));
  EXPECT_GE(context.last_expired_deadline(), expected_at);
}

TEST(SystemTimerZephyr, RescheduleToLaterDeadline) {
  SystemTimerTestContext context;

  const auto expected_at =
      SystemClock::now() + SystemClock::for_at_least(200ms);

  context.timer().InvokeAt(SystemClock::now() +
                           SystemClock::for_at_least(100ms));
  context.timer().InvokeAt(expected_at);

  EXPECT_TRUE(context.try_acquire_for(kSafeTimeout));
  EXPECT_EQ(context.callback_count(), 1);
  EXPECT_EQ(context.last_expired_deadline(), expected_at);
}

TEST(SystemTimerZephyr, RescheduleToEarlierDeadline) {
  SystemTimerTestContext context;

  const auto expected_at = SystemClock::now() + SystemClock::for_at_least(50ms);

  context.timer().InvokeAt(expected_at + SystemClock::for_at_least(100ms));
  context.timer().InvokeAt(expected_at);

  EXPECT_TRUE(context.try_acquire_for(kSafeTimeout));
  EXPECT_EQ(context.callback_count(), 1);
  EXPECT_EQ(context.last_expired_deadline(), expected_at);
}

TEST(SystemTimerZephyr, CancelScheduledTimer) {
  SystemTimerTestContext context;

  context.timer().InvokeAfter(SystemClock::for_at_least(100ms));
  context.timer().Cancel();

  EXPECT_FALSE(context.try_acquire_for(kNegativeTimeout));
  EXPECT_EQ(context.callback_count(), 0);
}

TEST(SystemTimerZephyr, RescheduleFromCallback) {
  auto expected_at = SystemClock::now();

  SystemTimerTestContext context(
      [&expected_at](SystemTimerTestContext& ctx,
                     SystemClock::time_point expired_deadline) {
        if (ctx.callback_count() == 1) {
          expected_at = SystemClock::now() + SystemClock::for_at_least(50ms);
          ctx.timer().InvokeAt(expected_at);
        } else {
          ctx.release_notification();
        }
      });

  context.timer().InvokeAfter(SystemClock::for_at_least(50ms));

  EXPECT_TRUE(context.try_acquire_for(kSafeTimeout));
  EXPECT_EQ(context.callback_count(), 2);
  EXPECT_GE(context.last_execution_time(), expected_at);
}

TEST(SystemTimerZephyr, CancelFromCallback) {
  SystemTimerTestContext context(
      [](SystemTimerTestContext& ctx, SystemClock::time_point) {
        ctx.timer().Cancel();
        ctx.release_notification();
      });

  context.timer().InvokeAfter(SystemClock::for_at_least(50ms));

  EXPECT_TRUE(context.try_acquire_for(kSafeTimeout));
  EXPECT_EQ(context.callback_count(), 1);
}

TEST(SystemTimerZephyr, DestructorCancelsTimer) {
  struct {
    pw::sync::TimedThreadNotification notification;
    std::atomic<int> callback_count = 0;
  } context;

  {
    SystemTimer local_timer([&context](SystemClock::time_point) {
      context.callback_count++;
      context.notification.release();
    });
    local_timer.InvokeAfter(SystemClock::for_at_least(200ms));
  }

  EXPECT_FALSE(context.notification.try_acquire_for(kNegativeTimeout));
  EXPECT_EQ(context.callback_count, 0);
}

TEST(SystemTimerZephyr, InvokeAtMax) {
  SystemTimerTestContext context;

  context.timer().InvokeAt(SystemClock::time_point::max());

  EXPECT_FALSE(context.try_acquire_for(kNegativeTimeout));
  EXPECT_EQ(context.callback_count(), 0);
}

TEST(SystemTimerZephyr, InvokeAfterMax) {
  SystemTimerTestContext context;

  context.timer().InvokeAfter(pw::chrono::zephyr::kMaxTimeout);

  EXPECT_FALSE(context.try_acquire_for(kNegativeTimeout));
  EXPECT_EQ(context.callback_count(), 0);
}

}  // namespace
}  // namespace pw::chrono
