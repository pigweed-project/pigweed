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

#include "pw_assert/check.h"
#include "pw_async2/await.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/future.h"
#include "pw_async2/poll.h"
#include "pw_async2/simulated_time_provider.h"
#include "pw_async2/value_future.h"
#include "pw_chrono/system_clock.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::async2::Context;
using pw::async2::Poll;
using pw::async2::SimulatedTimeProvider;
using pw::async2::TimeFuture;
using pw::async2::ValueFuture;
using pw::async2::ValueProvider;
using pw::chrono::SystemClock;

// A simple mock sensor that vends a ValueFuture<pw::Result<int>>.
class MockSensor {
 public:
  ValueFuture<pw::Result<int>> Read() {
    provider_ = ValueProvider<pw::Result<int>>();
    return provider_.Get();
  }

  void ResolveWithResult(pw::Result<int> result) { provider_.Resolve(result); }

 private:
  ValueProvider<pw::Result<int>> provider_;
};

// DOCSTAG: [pw_async2-examples-composite-future]
/// A composite future that reads a sensor with retry on failure.
///
/// This future exists in the middle of an async execution graph: the top level
/// contains `Task` implementations posted directly to the `Dispatcher`, while
/// the leaves are futures that asynchronously wait on external signals, like
/// hardware interrupts or timers. This future sits between those, combining
/// several other asynchronous operations into a logical unit.
///
/// Unlike leaf futures, this does not use `FutureCore`. It has no wakers, and
/// does not exist in a linked list. It is owned entirely by its caller, with
/// nothing else in the system maintaining any references to it. These types
/// of composite futures allow bundling and encapsulating multi-step async
/// logic in a composable and reusable way.
class ReadSensorWithRetryFuture {
 public:
  // Future concept requirement: define the result value type.
  using value_type = pw::Result<int>;

  // Futures must be default constructible and movable.
  ReadSensorWithRetryFuture() = default;
  ReadSensorWithRetryFuture(ReadSensorWithRetryFuture&&) = default;
  ReadSensorWithRetryFuture& operator=(ReadSensorWithRetryFuture&&) = default;

  // Future concept requirement: check if the operation can be pended.
  bool is_pendable() const {
    return state_ != State::kUninitialized && state_ != State::kDone;
  }

  // Future concept requirement: check if the operation has completed.
  bool is_complete() const { return state_ == State::kDone; }

  // Drives the composite state machine forward.
  Poll<pw::Result<int>> Pend(Context& cx) {
    while (true) {
      switch (state_) {
        case State::kUninitialized:
          PW_CRASH("Polled an uninitialized ReadSensorWithRetryFuture");

        case State::kInitializing: {
          if (!immediate_error_.IsUnknown()) {
            state_ = State::kDone;
            return pw::async2::Ready(immediate_error_);
          }

          read_future_ = sensor_->Read();
          state_ = State::kReading;
          break;
        }

        case State::kReading: {
          // Pend the child sensor future, passing `cx` down.
          // The leaf future will handle registering wakers if it returns
          // `Pending`.
          PW_AWAIT(pw::Result<int> res, read_future_, cx);

          // If the read succeeded or we have no retries left, complete the
          // future.
          if (res.ok() || retries_left_ == 0) {
            state_ = State::kDone;
            return pw::async2::Ready(res);
          }

          // Read failed: prepare for retry timer.
          retries_left_--;
          timer_future_ =
              time_provider_->WaitFor(std::chrono::milliseconds(50));
          state_ = State::kWaitingToRetry;
          break;  // Loop immediately to ensure the timer is pended.
        }

        case State::kWaitingToRetry: {
          // Pend the child time future.
          Poll<SystemClock::time_point> timer_res = timer_future_.Pend(cx);
          if (timer_res.IsPending()) {
            return pw::async2::Pending();
          }

          // Delay finished. Start a new sensor read and loop back to kReading.
          read_future_ = sensor_->Read();
          state_ = State::kReading;
          break;  // Loop immediately to pend the new sensor read.
        }

        case State::kDone:
          PW_CRASH("Polled a completed ReadSensorWithRetryFuture");
      }
    }
  }

 private:
  friend ReadSensorWithRetryFuture ReadSensorWithRetry(
      MockSensor& sensor,
      SimulatedTimeProvider<SystemClock>& time_provider,
      int max_retries);

  ReadSensorWithRetryFuture(MockSensor& sensor,
                            SimulatedTimeProvider<SystemClock>& time_provider,
                            int max_retries)
      : state_(State::kInitializing),
        sensor_(&sensor),
        time_provider_(&time_provider),
        retries_left_(max_retries) {}

  // Constructs a future that immediately fails with the specified status.
  explicit ReadSensorWithRetryFuture(pw::Status status)
      : state_(State::kInitializing), immediate_error_(status) {
    PW_ASSERT(!status.ok() && !status.IsUnknown());
  }

  enum class State {
    kUninitialized,
    kInitializing,
    kReading,
    kWaitingToRetry,
    kDone
  };
  State state_ = State::kUninitialized;

  MockSensor* sensor_ = nullptr;
  SimulatedTimeProvider<SystemClock>* time_provider_ = nullptr;
  int retries_left_ = 0;
  pw::Status immediate_error_ = pw::Status::Unknown();

  // Owns the child futures inline.
  ValueFuture<pw::Result<int>> read_future_;
  TimeFuture<SystemClock> timer_future_;
};

// Verify that ReadSensorWithRetryFuture satisfies the Future concept.
static_assert(pw::async2::Future<ReadSensorWithRetryFuture>);

/// An async helper function.
///
/// The function is a factory constructing composite futures and returning them
/// directly by value. There is no provider, no list or waker management. Those
/// occur within the subfutures that actually perform wakeable operations.
///
/// The function begins by synchronously validating its arguments, returning a
/// future that immediately resolves to an error if invalid.
///
/// Per async2 conventions, the function returns a future directly instead of
/// wrapping it in a `Result` / `std::optional` to allow further composition,
/// or, in the coroutine world:
///
/// @code{.cpp}
///   pw::Result<int> result =
///       co_await ReadSensorWithRetry(sensor,
///                                    GetSystemTimeProvider(),
///                                    10);
/// @endcode
inline ReadSensorWithRetryFuture ReadSensorWithRetry(
    MockSensor& sensor,
    SimulatedTimeProvider<SystemClock>& time_provider,
    int max_retries = 3) {
  if (max_retries <= 0) {
    return ReadSensorWithRetryFuture(pw::Status::InvalidArgument());
  }
  return ReadSensorWithRetryFuture(sensor, time_provider, max_retries);
}
// DOCSTAG: [pw_async2-examples-composite-future]

class SensorReaderTask : public pw::async2::Task {
 public:
  SensorReaderTask(MockSensor& sensor,
                   SimulatedTimeProvider<SystemClock>& time_provider)
      : sensor_(&sensor), time_provider_(&time_provider) {}

  pw::Result<int> result() const { return result_; }

 private:
  Poll<> DoPend(Context& cx) override {
    if (!read_retry_future_.is_pendable()) {
      read_retry_future_ = ReadSensorWithRetry(*sensor_, *time_provider_, 2);
    }
    PW_AWAIT(auto res, read_retry_future_, cx);
    result_ = res;
    return pw::async2::Ready();
  }

  MockSensor* sensor_ = nullptr;
  SimulatedTimeProvider<SystemClock>* time_provider_ = nullptr;
  ReadSensorWithRetryFuture read_retry_future_;
  pw::Result<int> result_ = pw::Status::Unknown();
};

TEST(CompositeFutureTest, SuccessfulReadOnFirstTry) {
  pw::async2::DispatcherForTest dispatcher;
  SimulatedTimeProvider<SystemClock> time_provider;
  MockSensor sensor;

  SensorReaderTask task(sensor, time_provider);
  dispatcher.Post(task);

  // Initial poll: task pends ReadSensorWithRetryFuture, which pends
  // sensor.Read().
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(task.result().status().IsUnknown());

  // Fulfill sensor read with success.
  sensor.ResolveWithResult(42);

  // Drive task to completion.
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(task.result().ok());
  EXPECT_EQ(task.result().value(), 42);
}

TEST(CompositeFutureTest, RetriesAndSucceedsOnSecondTry) {
  pw::async2::DispatcherForTest dispatcher;
  SimulatedTimeProvider<SystemClock> time_provider;
  MockSensor sensor;

  SensorReaderTask task(sensor, time_provider);
  dispatcher.Post(task);

  // 1st attempt: sensor read pending.
  dispatcher.RunUntilStalled();

  // Fulfill 1st read with error.
  sensor.ResolveWithResult(pw::Status::Unavailable());

  // Task runs, detects error, sets up 50ms timer, and returns Pending.
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(task.result().status().IsUnknown());

  // Advance time until timer expires.
  EXPECT_TRUE(time_provider.AdvanceUntilNextExpiration());

  // Task runs, timer resolves, starts 2nd sensor read, returns Pending.
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(task.result().status().IsUnknown());

  // Fulfill 2nd read with success.
  sensor.ResolveWithResult(99);

  // Drive task to completion.
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(task.result().ok());
  EXPECT_EQ(task.result().value(), 99);
}

TEST(CompositeFutureTest, FailsAfterMaxRetries) {
  pw::async2::DispatcherForTest dispatcher;
  SimulatedTimeProvider<SystemClock> time_provider;
  MockSensor sensor;

  SensorReaderTask task(sensor, time_provider);
  dispatcher.Post(task);

  // 1st attempt: fail
  dispatcher.RunUntilStalled();
  sensor.ResolveWithResult(pw::Status::Unavailable());

  // Timer 1
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(time_provider.AdvanceUntilNextExpiration());

  // 2nd attempt: fail
  dispatcher.RunUntilStalled();
  sensor.ResolveWithResult(pw::Status::Unavailable());

  // Timer 2
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(time_provider.AdvanceUntilNextExpiration());

  // 3rd attempt: fail (max retries reached)
  dispatcher.RunUntilStalled();
  sensor.ResolveWithResult(pw::Status::ResourceExhausted());

  // Task should complete with failure.
  dispatcher.RunUntilStalled();
  EXPECT_EQ(task.result().status(), pw::Status::ResourceExhausted());
}

TEST(CompositeFutureTest, SynchronousInvalidArgumentValidation) {
  pw::async2::DispatcherForTest dispatcher;
  SimulatedTimeProvider<SystemClock> time_provider;
  MockSensor sensor;

  // Passing invalid max_retries = 0 triggers synchronous validation in the
  // helper.
  auto invalid_future = ReadSensorWithRetry(sensor, time_provider, 0);

  // Future is created in a ready error state without pending any subfutures or
  // sensor reads.
  pw::async2::FuncTask task([&invalid_future](Context& cx) -> Poll<> {
    auto poll_res = invalid_future.Pend(cx);
    if (poll_res.IsPending()) {
      return pw::async2::Pending();
    }
    EXPECT_EQ(poll_res.value().status(), pw::Status::InvalidArgument());
    return pw::async2::Ready();
  });

  dispatcher.Post(task);
  dispatcher.RunUntilStalled();
}

}  // namespace
