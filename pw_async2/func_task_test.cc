// Copyright 2023 The Pigweed Authors
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

#include "pw_async2/func_task.h"

#include <optional>
#include <utility>

#include "pw_async2/dispatcher_for_test.h"
#include "pw_function/function.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::Function;
using ::pw::async2::Context;
using ::pw::async2::DispatcherForTest;
using ::pw::async2::FuncTask;
using ::pw::async2::Pending;
using ::pw::async2::Poll;
using ::pw::async2::Ready;
using ::pw::async2::Task;
using ::pw::async2::Waker;

TEST(FuncTask, PendDelegatesToFunc) {
  DispatcherForTest dispatcher;

  Waker waker;
  int poll_count = 0;
  bool allow_completion = false;

  FuncTask func_task([&](Context& cx) -> Poll<> {
    ++poll_count;
    if (allow_completion) {
      return Ready();
    }
    PW_ASYNC_STORE_WAKER(cx, waker, "func_task is waiting for waker");
    return Pending();
  });

  dispatcher.Post(func_task);

  EXPECT_EQ(poll_count, 0);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(poll_count, 1);

  // Unwoken task is not polled.
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(poll_count, 1);

  waker.Wake();
  allow_completion = true;
  dispatcher.RunToCompletion();
  EXPECT_EQ(poll_count, 2);
}

TEST(FuncTask, HoldsCallableByDefault) {
  auto callable = [](Context&) -> Poll<> { return Ready(); };
  FuncTask func_task(std::move(callable));
  static_assert(
      std::is_same<decltype(func_task), FuncTask<decltype(callable)>>::value);
}

TEST(FuncTask, HoldsPwFunctionWithEmptyTypeList) {
  FuncTask<> func_task([](Context&) -> Poll<> { return Ready(); });
  static_assert(std::is_same<decltype(func_task),
                             FuncTask<Function<Poll<>(Context&)>>>::value);
}

Poll<> ReturnsReady(Context&) { return Ready(); }

// Simulates the size of a FuncTask for testing purposes.
template <typename Func>
struct SizeHelper : public Task {
  Func func;
};

TEST(FuncTask, TestTemplateDeductionAndSize) {
  // A FuncTask with an unspecified Func template parameter will default
  // to pw::Function. This allows the same container to hold a variety of
  // different callables, but it may either reserve extra inline storage or
  // dynamically allocate memory, depending on how pw::Function is configured.
  std::optional<FuncTask<>> a;
  a.emplace([](Context&) -> Poll<> { return Ready(); });
  a.emplace(&ReturnsReady);
  static_assert(sizeof(decltype(a)::value_type) ==
                sizeof(SizeHelper<Function<Poll<>(Context&)>>));

  // When constructing a FuncTask directly from a callable, CTAD will match
  // the Func template parameter to that of the callable. This has the
  // benefit of reducing the amount of storage needed vs that of a pw::Function.
  //
  // A lambda without any captures doesn't require any storage.
  auto b = FuncTask([](Context&) -> Poll<> { return Ready(); });
  static_assert(sizeof(decltype(b)) <= sizeof(SizeHelper<char>));

  // A lambda with captures requires storage to hold the captures.
  int scratch = 6;
  auto c = FuncTask(
      [&scratch](Context&) -> Poll<> { return scratch ? Ready() : Pending(); });
  static_assert(sizeof(decltype(c)) == sizeof(SizeHelper<int*>));

  // A raw function pointer just needs storage for the pointer value.
  auto d = FuncTask(&ReturnsReady);
  static_assert(sizeof(decltype(d)) ==
                sizeof(SizeHelper<decltype(&ReturnsReady)>));
}

TEST(FuncTask, DeregistersInDestructor) {
  DispatcherForTest dispatcher;
  {
    FuncTask task([](Context&) { return Pending(); });
    dispatcher.Post(task);
  }
  EXPECT_FALSE(dispatcher.RunUntilStalled());
}

}  // namespace
