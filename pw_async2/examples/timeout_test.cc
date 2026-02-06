// Copyright 2025 The Pigweed Authors
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

#define PW_LOG_MODULE_NAME "EXAMPLES_TIMEOUT"

#include <array>
#include <atomic>
#include <chrono>  // IWYU pragma: keep
#include <optional>
#include <thread>
#include <tuple>
#include <utility>

#include "pw_allocator/libc_allocator.h"
#include "pw_assert/check.h"
#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/context.h"
#include "pw_async2/coro.h"
#include "pw_async2/coro_or_else_task.h"
#include "pw_async2/func_task.h"
#include "pw_async2/future_timeout.h"
#include "pw_async2/poll.h"
#include "pw_async2/select.h"
#include "pw_async2/system_time_provider.h"
#include "pw_async2/task.h"
#include "pw_async2/try.h"
#include "pw_async2/value_future.h"
#include "pw_chrono/system_clock.h"
#include "pw_containers/vector.h"
#include "pw_log/log.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_string/string_builder.h"
#include "pw_unit_test/framework.h"

namespace examples {

using namespace std::chrono_literals;

using ::pw::Result;
using ::pw::Status;
using ::pw::StringBuilder;
using ::pw::Vector;
using ::pw::allocator::LibCAllocator;
using ::pw::async2::BasicDispatcher;
using ::pw::async2::Context;
using ::pw::async2::Coro;
using ::pw::async2::CoroContext;
using ::pw::async2::CoroOrElseTask;
using ::pw::async2::FuncTask;
using ::pw::async2::GetSystemTimeProvider;
using ::pw::async2::Poll;
using ::pw::async2::Ready;
using ::pw::async2::SelectFuture;
using ::pw::async2::Task;
using ::pw::async2::ValueFuture;
using ::pw::async2::ValueFutureWithTimeout;
using ::pw::async2::ValueProvider;

namespace {

constexpr auto kSimulatedSensorDelayNormal = 1ms;
constexpr auto kSimulatedSensorDelayAbnormal = 1000ms;

constexpr auto kSensorReadTimeout = 100ms;

}  // namespace

template <typename T>
Result<T> RunFutureToCompletionWithTimeout(
    auto& future, pw::chrono::SystemClock::duration delay) {
  auto timeout = GetSystemTimeProvider().WaitFor(delay);
  auto select = SelectFuture(std::move(future), std::move(timeout));
  typename decltype(select)::value_type select_result;

  BasicDispatcher dispatcher;
  auto task = FuncTask([&](Context cx) -> Poll<> {
    PW_TRY_READY_ASSIGN(select_result, select.Pend(cx));
    return Ready();
  });
  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  if (select_result.template has_value<0>()) {
    return select_result.template value<0>();
  }
  PW_CHECK(select_result.template has_value<1>());
  return Status::DeadlineExceeded();
}

class FakeVoltageSensor {
 public:
  static std::atomic<bool> force_timeout;

  Result<float> ReadWithTimeout(pw::chrono::SystemClock::duration delay) {
    auto read = ReadFuture();
    return RunFutureToCompletionWithTimeout<float>(read, delay);
  }

  [[nodiscard]] ValueFuture<float> ReadFuture() {
    std::ignore = this;
    return SpawnFutureResolvingThread();
  }

 private:
  [[nodiscard]] static ValueFuture<float> SpawnFutureResolvingThread() {
    static LibCAllocator alloc;
    auto provider = alloc.MakeShared<ValueProvider<float>>();
    ValueFuture<float> future = provider->Get();

    std::thread interrupt_thread([provider] {
      if (force_timeout) {
        std::this_thread::sleep_for(kSimulatedSensorDelayAbnormal);
      } else {
        std::this_thread::sleep_for(kSimulatedSensorDelayNormal);
      }

      constexpr float kVoltageReading = 3.3F;
      provider->Resolve(kVoltageReading);
    });
    interrupt_thread.detach();

    return future;
  }
};

std::atomic<bool> FakeVoltageSensor::force_timeout = true;

void LogSampledVoltages(const char* name,
                        const Result<Vector<float, 10>>& voltages) {
  std::array<char, 100> dump_buffer{};
  StringBuilder dump(dump_buffer);

  if (voltages.ok()) {
    dump.append("voltages [");
    for (const float value : *voltages) {
      dump.Format(" %3.1f", value);
    }
    dump.append(" ]");
  } else {
    dump.append("error ");
    dump.append(voltages.status().str());
  }
  const char* timeout =
      FakeVoltageSensor::force_timeout ? "timeout" : "no timeout";
  PW_LOG_INFO("%s (%s): %s", name, timeout, dump.c_str());
}

Result<Vector<float, 10>> SampleVoltageBlocking() {
  Vector<float, 10> voltages;
  FakeVoltageSensor sensor;

  while (!voltages.full()) {
    Result<float> result = sensor.ReadWithTimeout(kSensorReadTimeout);
    if (!result.ok()) {
      return Status::DeadlineExceeded();
    }
    voltages.push_back(*result);
  }
  return voltages;
}

Coro<Status> SampleVoltageCoro(CoroContext& cx,
                               Result<Vector<float, 10>>& output) {
  Vector<float, 10> voltages;
  FakeVoltageSensor sensor;

  while (!voltages.full()) {
    const Result<float> result =
        co_await Timeout(sensor.ReadFuture(), kSensorReadTimeout);
    if (!result.ok()) {
      output = result.status();
      co_return result.status();
    }
    voltages.push_back(*result);
  }

  output = voltages;
  co_return Status{};
}

class SampleVoltageTask final : public Task {
 public:
  Result<Vector<float, 10>> result() {
    if (!status_.ok()) {
      return status_;
    }
    return voltages_;
  }

 private:
  Poll<> DoPend(Context& cx) override {
    while (!voltages_.full()) {
      if (!current_future_.is_pendable()) {
        current_future_ = Timeout(sensor_.ReadFuture(), kSensorReadTimeout);
      }

      PW_TRY_READY_ASSIGN(const Result<float> result, current_future_.Pend(cx));

      if (!result.ok()) {
        status_ = result.status();
        return Ready();
      }
      voltages_.push_back(*result);
    }
    return Ready();
  }

  FakeVoltageSensor sensor_;
  Vector<float, 10> voltages_;
  Status status_;
  ValueFutureWithTimeout<float> current_future_;
};

int main() {
  // The dispatcher handles dispatching to all tasks.
  BasicDispatcher dispatcher;

  // The CoroContext needs an allocator instance. Use the libc allocator.
  LibCAllocator alloc;

  // Creating a CoroContext is required before using coroutines. It makes
  // the memory allocations needed for each coroutine at runtime when each is
  // started.
  CoroContext coro_cx(alloc);

  FakeVoltageSensor::force_timeout = true;

  {
    auto result = SampleVoltageBlocking();
    PW_CHECK(result.status().IsDeadlineExceeded());
    LogSampledVoltages("SampleVoltageBlocking", result);
  }

  {
    auto task = SampleVoltageTask();
    dispatcher.Post(task);
    dispatcher.RunToCompletion();
    auto result = task.result();
    PW_CHECK(result.status().IsDeadlineExceeded());
    LogSampledVoltages("SampleVoltageTask", result);
  }

  {
    Result<Vector<float, 10>> result;
    auto task = CoroOrElseTask(SampleVoltageCoro(coro_cx, result),
                               [&](Status status) { result = status; });
    dispatcher.Post(task);
    dispatcher.RunToCompletion();
    PW_CHECK(result.status().IsDeadlineExceeded());
    LogSampledVoltages("SampleVoltageCoro", result);
  }

  FakeVoltageSensor::force_timeout = false;

  {
    auto result = SampleVoltageBlocking();
    PW_CHECK(result.ok());
    LogSampledVoltages("SampleVoltageBlocking", result);
  }

  {
    auto task = SampleVoltageTask();
    dispatcher.Post(task);
    dispatcher.RunToCompletion();
    auto result = task.result();
    PW_CHECK(result.ok());
    LogSampledVoltages("SampleVoltageTask", result);
  }

  {
    Result<Vector<float, 10>> result;
    auto task = CoroOrElseTask(SampleVoltageCoro(coro_cx, result),
                               [&](Status status) { result = status; });
    dispatcher.Post(task);
    dispatcher.RunToCompletion();
    PW_CHECK(result.ok());
    LogSampledVoltages("SampleVoltageCoro", result);
  }

  return 0;
}

}  // namespace examples

namespace {

TEST(ExampleTests, Timeout) {
  using namespace examples;

  BasicDispatcher dispatcher;
  LibCAllocator alloc;
  CoroContext coro_cx(alloc);

  FakeVoltageSensor::force_timeout = true;

  ASSERT_TRUE(SampleVoltageBlocking().status().IsDeadlineExceeded());

  {
    auto result = SampleVoltageBlocking();
    ASSERT_TRUE(result.status().IsDeadlineExceeded());
  }

  {
    auto task = SampleVoltageTask();
    dispatcher.Post(task);
    dispatcher.RunToCompletion();
    auto result = task.result();
    ASSERT_TRUE(result.status().IsDeadlineExceeded());
  }

  {
    Result<Vector<float, 10>> result;
    auto task = CoroOrElseTask(SampleVoltageCoro(coro_cx, result),
                               [&](Status status) { result = status; });
    dispatcher.Post(task);
    dispatcher.RunToCompletion();
    ASSERT_TRUE(result.status().IsDeadlineExceeded());
  }

  FakeVoltageSensor::force_timeout = false;

  {
    auto result = SampleVoltageBlocking();
    ASSERT_TRUE(result.status().ok());
    ASSERT_TRUE(result->size() == 10);
  }

  {
    auto task = SampleVoltageTask();
    dispatcher.Post(task);
    dispatcher.RunToCompletion();
    auto result = task.result();
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result->size() == 10);
  }

  {
    Result<Vector<float, 10>> result;
    auto task = CoroOrElseTask(SampleVoltageCoro(coro_cx, result),
                               [&](Status status) { result = status; });
    dispatcher.Post(task);
    dispatcher.RunToCompletion();
    ASSERT_TRUE(result.status().ok());
    ASSERT_TRUE(result->size() == 10);
  }

  main();
}

}  // namespace
