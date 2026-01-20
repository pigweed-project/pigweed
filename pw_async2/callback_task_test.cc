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

#include "pw_async2/callback_task.h"

#include <optional>

#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/future.h"
#include "pw_async2/value_future.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::async2::CallbackTask;
using ::pw::async2::DispatcherForTest;
using ::pw::async2::ValueFuture;
using ::pw::async2::ValueProvider;

TEST(CallbackTask, PendsFutureUntilReady) {
  ValueProvider<char> provider;
  char result = '\0';

  CallbackTask<ValueFuture<char>> task([&result](char c) { result = c; },
                                       provider.Get());

  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(result, '\0');

  provider.Resolve('b');

  dispatcher.RunToCompletion();
  EXPECT_EQ(result, 'b');

  EXPECT_FALSE(task.IsRegistered());
}

TEST(CallbackTask, ImmediatelyReturnsReady) {
  char result = '\0';

  CallbackTask<ValueFuture<char>> task([&result](char c) { result = c; },
                                       ValueFuture<char>::Resolved('b'));

  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  dispatcher.RunToCompletion();
  EXPECT_EQ(result, 'b');

  EXPECT_FALSE(task.IsRegistered());
}

TEST(CallbackTask, VoidFuture) {
  ValueProvider<void> provider;

  bool completed = false;

  CallbackTask<ValueFuture<void>> task([&completed]() { completed = true; },
                                       provider.Get());

  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_FALSE(completed);

  provider.Resolve();

  dispatcher.RunToCompletion();
  EXPECT_TRUE(completed);

  EXPECT_FALSE(task.IsRegistered());
}

class TestFuture {
 public:
  TestFuture() = default;

  using value_type = int;

  TestFuture(int number_one, int number_two)
      : state_(pw::async2::FutureState::kPending),
        number_one_(number_one),
        number_two_(number_two) {}

  pw::async2::Poll<value_type> Pend(pw::async2::Context&) {
    PW_ASSERT(state_.is_pendable());
    state_.MarkComplete();
    return number_one_ + number_two_;
  }

  bool is_pendable() const { return state_.is_pendable(); }

  bool is_complete() const { return state_.is_complete(); }

 private:
  pw::async2::FutureState state_;
  int number_one_;
  int number_two_;
};

TEST(CallbackTask, Emplace) {
  int result = 0;
  auto task = CallbackTask<TestFuture>::Emplace(
      [&result](int c) { result = c; }, 40, 2);

  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  dispatcher.RunToCompletion();
  EXPECT_EQ(result, 42);

  EXPECT_FALSE(task.IsRegistered());
}

}  // namespace
