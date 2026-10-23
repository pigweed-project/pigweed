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

#include "pw_async2/cancellable_task.h"

#include <utility>

#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_async2/waker.h"
#include "pw_function/function.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::async2::CancellableTask;
using pw::async2::Context;
using pw::async2::DispatcherForTest;
using pw::async2::Pending;
using pw::async2::Poll;
using pw::async2::Ready;
using pw::async2::Task;
using pw::async2::Waker;

class MockTask : public Task {
 public:
  bool should_complete = false;
  Waker waker;

  MockTask(pw::Closure callback)
      : Task(PW_ASYNC_TASK_NAME("MockTask")), callback_(std::move(callback)) {}

 protected:
  Poll<> DoPend(Context& cx) override {
    PW_ASYNC_STORE_WAKER(cx, waker, "MockTask is waiting for waker");
    if (should_complete) {
      callback_();
      return Ready();
    }
    return Pending();
  }

 private:
  pw::Closure callback_;
};

TEST(CancellableTask, CancelsPendingTask) {
  bool task_completed = false;

  CancellableTask<MockTask> task(
      [&task_completed]() { task_completed = true; });
  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(dispatcher.tasks_polled(), 1u);
  EXPECT_FALSE(task_completed);

  task.Cancel();
  dispatcher.RunToCompletion();
  EXPECT_EQ(dispatcher.tasks_polled(), 2u);
  EXPECT_FALSE(task.IsRegistered());
  EXPECT_FALSE(task_completed);
}

TEST(CancellableTask, DoesNothingWithCompletedTask) {
  bool task_completed = false;

  CancellableTask<MockTask> task(
      [&task_completed]() { task_completed = true; });
  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(dispatcher.tasks_polled(), 1u);
  EXPECT_FALSE(task_completed);

  task.should_complete = true;
  task.waker.Wake();
  dispatcher.RunToCompletion();
  EXPECT_EQ(dispatcher.tasks_polled(), 2u);
  EXPECT_TRUE(task_completed);

  task.Cancel();
  dispatcher.RunToCompletion();
  EXPECT_EQ(dispatcher.tasks_polled(), 2u);
}

TEST(CancellableTask, CancelsTaskBeforePosting) {
  bool task_completed = false;

  CancellableTask<MockTask> task(
      [&task_completed]() { task_completed = true; });
  task.should_complete = true;
  task.Cancel();

  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  dispatcher.RunToCompletion();
  EXPECT_EQ(dispatcher.tasks_polled(), 1u);
  EXPECT_FALSE(task_completed);
}

TEST(CancellableTask, CancelsTaskBeforeRunning) {
  bool task_completed = false;

  CancellableTask<MockTask> task(
      [&task_completed]() { task_completed = true; });
  DispatcherForTest dispatcher;
  dispatcher.Post(task);

  task.should_complete = true;
  task.Cancel();

  dispatcher.RunToCompletion();
  EXPECT_EQ(dispatcher.tasks_polled(), 1u);
  EXPECT_FALSE(task_completed);
}

}  // namespace
