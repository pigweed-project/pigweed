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

#include "pw_allocator/testing.h"
#include "pw_async2/coro.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/future_timeout.h"
#include "pw_async2/join.h"
#include "pw_async2/try.h"
#include "pw_async2/value_future.h"
#include "pw_log/log.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

using ::pw::async2::JoinFuture;
using ::pw::async2::Timeout;
using ::pw::async2::TimeoutOr;
using ::pw::async2::ValueFuture;
using ::std::chrono_literals::operator""ms;

// DOCSTAG: [pw_async2-examples-futures-number-generator]
class NumberGenerator {
 public:
  ValueFuture<int> GetNextNumber();
};
// DOCSTAG: [pw_async2-examples-futures-number-generator]

// DOCSTAG: [pw_async2-examples-futures-my-task]
class MyTask : public pw::async2::Task {
 private:
  pw::async2::Poll<> DoPend(pw::async2::Context& cx) override {
    // The future begins in a default-constructed state, which is not
    // pendable. Initialize it on the task's first run.
    if (!future_.is_pendable()) {
      future_ = generator_.GetNextNumber();
    }

    PW_TRY_READY_ASSIGN(int number, future_.Pend(cx));
    PW_LOG_INFO("Received number: %d", number);

    return pw::async2::Ready();
  }

  NumberGenerator generator_;
  ValueFuture<int> future_;
};
// DOCSTAG: [pw_async2-examples-futures-my-task]

// DOCSTAG: [pw_async2-examples-futures-my-coro]
pw::async2::Coro<void> MyCoroutineFunction(pw::async2::CoroContext,
                                           NumberGenerator& generator) {
  // Pigweed's coroutine integration allows futures to be awaited directly.
  int number = co_await generator.GetNextNumber();
  PW_LOG_INFO("Received number: %d", number);
}
// DOCSTAG: [pw_async2-examples-futures-my-coro]

// DOCSTAG: [pw_async2-examples-futures-join-function]
ValueFuture<pw::Status> DoWork(int id);
// DOCSTAG: [pw_async2-examples-futures-join-function]

// DOCSTAG: [pw_async2-examples-futures-join-task]
class JoinTask : public pw::async2::Task {
 private:
  pw::async2::Poll<> DoPend(pw::async2::Context& cx) override {
    if (!future_.is_pendable()) {
      // Start three futures concurrently and wait for all of them
      // to complete.
      future_ = pw::async2::Join(DoWork(1), DoWork(2), DoWork(3));
    }

    PW_TRY_READY_ASSIGN(auto results, future_.Pend(cx));
    auto [status1, status2, status3] = std::move(results);

    if (!status1.ok() || !status2.ok() || !status3.ok()) {
      PW_LOG_ERROR("Operation failed");
    } else {
      PW_LOG_INFO("All operations succeeded");
    }

    return pw::async2::Ready();
  }

  JoinFuture<ValueFuture<pw::Status>,
             ValueFuture<pw::Status>,
             ValueFuture<pw::Status>>
      future_;
};
// DOCSTAG: [pw_async2-examples-futures-join-task]

// DOCSTAG: [pw_async2-examples-futures-join-coro]
pw::async2::Coro<pw::Status> JoinExample(pw::async2::CoroContext) {
  // Start three futures concurrently and wait for all of them to complete.
  auto [status1, status2, status3] =
      co_await pw::async2::Join(DoWork(1), DoWork(2), DoWork(3));

  if (!status1.ok() || !status2.ok() || !status3.ok()) {
    PW_LOG_ERROR("Operation failed");
    co_return pw::Status::Internal();
  }
  PW_LOG_INFO("All operations succeeded");
  co_return pw::OkStatus();
}
// DOCSTAG: [pw_async2-examples-futures-join-coro]

// DOCSTAG: [pw_async2-examples-futures-select-functions]
#include "pw_async2/select.h"

ValueFuture<int> DoWork();
ValueFuture<int> DoOtherWork();
// DOCSTAG: [pw_async2-examples-futures-select-functions]

// DOCSTAG: [pw_async2-examples-futures-select-task]
class SelectTask : public pw::async2::Task {
 private:
  pw::async2::Poll<> DoPend(pw::async2::Context& cx) override {
    if (!future_.is_pendable()) {
      // Race two futures and wait for the first one to complete.
      future_ = pw::async2::Select(DoWork(), DoOtherWork());
    }

    PW_TRY_READY_ASSIGN(auto results, future_.Pend(cx));

    // Check which future(s) completed.
    // In this example, we check all of them, but it's common to return
    // after the first result.
    if (results.has_value<0>()) {
      PW_LOG_INFO("DoWork completed with: %d", results.value<0>());
    }
    if (results.has_value<1>()) {
      PW_LOG_INFO("DoOtherWork completed with: %d", results.value<1>());
    }

    return pw::async2::Ready();
  }

  pw::async2::SelectFuture<ValueFuture<int>, ValueFuture<int>> future_;
};
// DOCSTAG: [pw_async2-examples-futures-select-task]

// DOCSTAG: [pw_async2-examples-futures-select-coro]
pw::async2::Coro<void> SelectExample(pw::async2::CoroContext) {
  // Race two futures and wait for the first one to complete.
  auto results = co_await pw::async2::Select(DoWork(), DoOtherWork());

  // Check which future(s) completed.
  // In this example, we check all of them, but it's common to return
  // after the first result.
  if (results.has_value<0>()) {
    int result = results.value<0>();
    PW_LOG_INFO("DoWork completed with: %d", result);
  }

  if (results.has_value<1>()) {
    int result = results.value<1>();
    PW_LOG_INFO("DoOtherWork completed with: %d", result);
  }
}
// DOCSTAG: [pw_async2-examples-futures-select-coro]

class IntValueProvider {
 public:
  ValueFuture<int> Get();
};

class ValueProvider {
 public:
  ValueFuture<int> Get();
};

void TimeoutExamples(ValueProvider& value_provider,
                     IntValueProvider& int_value_provider) {
  // DOCSTAG: [pw_async2-examples-futures-timeout]
  // Obtain a basic ValueFuture<T> or similar from some provider.
  auto value_future = value_provider.Get();
  // Construct the composite future, which will either resolve
  // to a `T`, or timeout after 15ms using the system clock.
  auto future_with_timeout_ex1 = Timeout(std::move(value_future), 15ms);

  // You can also construct one this way.
  auto future_with_timeout_ex2 = Timeout(value_provider.Get(), 15ms);

  // For a sentinel with a simple constant:
  auto future_with_timeout_ex3 = TimeoutOr(int_value_provider.Get(), 15ms, -1);

  // To use a function to obtain a sentinnel value:
  auto future_with_timeout_ex4 =
      TimeoutOr(int_value_provider.Get(), 15ms, []() { return -1; });
  // DOCSTAG: [pw_async2-examples-futures-timeout]
}

ValueFuture<int> NumberGenerator::GetNextNumber() {
  return ValueFuture<int>::Resolved(42);
}
ValueFuture<pw::Status> DoWork(int id) {
  if (id == 2) {
    return ValueFuture<pw::Status>::Resolved(pw::Status::Unknown());
  }
  return ValueFuture<pw::Status>::Resolved(pw::OkStatus());
}
ValueFuture<int> DoOtherWork() {
  static pw::async2::ValueProvider<int> _provider;
  if (!_provider.has_future()) {
    return _provider.Get();
  }
  return _provider.TryGet().value_or(ValueFuture<int>());
}
ValueFuture<int> DoWork() { return ValueFuture<int>::Resolved(100); }
ValueFuture<int> IntValueProvider::Get() {
  return ValueFuture<int>::Resolved(0);
}
ValueFuture<int> ValueProvider::Get() { return ValueFuture<int>::Resolved(0); }

TEST(Futures, MyTask) {
  pw::async2::DispatcherForTest dispatcher;
  MyTask task;
  dispatcher.Post(task);
  EXPECT_FALSE(dispatcher.RunUntilStalled());
}

TEST(Futures, MyCoroutineFunction) {
  pw::allocator::test::AllocatorForTest<512> allocator;
  pw::async2::DispatcherForTest dispatcher;
  NumberGenerator generator;
  auto task =
      dispatcher.Post(allocator, MyCoroutineFunction(allocator, generator));
  EXPECT_FALSE(dispatcher.RunUntilStalled());
}

TEST(Futures, JoinTask) {
  pw::async2::DispatcherForTest dispatcher;
  JoinTask task;
  dispatcher.Post(task);
  EXPECT_FALSE(dispatcher.RunUntilStalled());
}

TEST(Futures, JoinExample) {
  pw::allocator::test::AllocatorForTest<1024> allocator;
  pw::async2::DispatcherForTest dispatcher;
  auto task = dispatcher.Post(allocator, JoinExample(allocator));
  EXPECT_FALSE(dispatcher.RunUntilStalled());
}

TEST(Futures, SelectTask) {
  pw::async2::DispatcherForTest dispatcher;
  SelectTask task;
  dispatcher.Post(task);
  EXPECT_FALSE(dispatcher.RunUntilStalled());
}

TEST(Futures, SelectExample) {
  pw::allocator::test::AllocatorForTest<1024> allocator;
  pw::async2::DispatcherForTest dispatcher;
  auto task = dispatcher.Post(allocator, SelectExample(allocator));
  EXPECT_FALSE(dispatcher.RunUntilStalled());
}

TEST(Futures, TimeoutExamples) {
  ValueProvider value_provider;
  IntValueProvider int_value_provider;
  TimeoutExamples(value_provider, int_value_provider);
}
