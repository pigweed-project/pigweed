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

#include "pw_async2/dispatcher_for_test.h"
#include "pw_sync/thread_notification.h"
#include "pw_thread/sleep.h"
#include "pw_thread/test_thread_context.h"
#include "pw_thread/thread.h"
#include "pw_unit_test/framework.h"

namespace {

using namespace std::chrono_literals;

using pw::async2::Context;
using pw::async2::DispatcherForTest;
using pw::async2::Pending;
using pw::async2::Poll;
using pw::async2::Ready;
using pw::async2::Task;
using pw::async2::Waker;

class BlockingTask : public Task {
 public:
  explicit BlockingTask() : Task(PW_ASYNC_TASK_NAME("BlockingTask")) {}

  Poll<> DoPend(Context& cx) override {
    if (first_run_) {
      first_run_ = false;
      PW_ASYNC_STORE_WAKER(cx, waker_, "BlockingTask::Wake()");
      running_.release();
      return Pending();
    }
    return Ready();
  }

  void WaitUntilRunning() { running_.acquire(); }

  void Wake() { waker_.Wake(); }

 private:
  pw::sync::ThreadNotification running_;
  Waker waker_;
  bool first_run_ = true;
};

TEST(DispatcherForTest, RunToCompletionWaitsForTaskToWake) {
  DispatcherForTest dispatcher;
  BlockingTask task;
  dispatcher.Post(task);

  pw::thread::test::TestThreadContext context;
  pw::Thread thread(context.options(), [&task] {
    task.WaitUntilRunning();
    task.Wake();
  });

  dispatcher.AllowBlocking();
  dispatcher.RunToCompletion();
  EXPECT_FALSE(task.IsRegistered());

  thread.join();
}

TEST(DispatcherForTest, RunToCompletionUntilReleased) {
  DispatcherForTest dispatcher;

  pw::thread::test::TestThreadContext context;
  pw::Thread thread(context.options(), [&dispatcher] {
    pw::this_thread::sleep_for(10ms);
    dispatcher.Release();
  });

  dispatcher.RunToCompletionUntilReleased();

  thread.join();
}

TEST(DispatcherForTest, RunToCompletionUntilReleasedMultipleTimes) {
  DispatcherForTest dispatcher;

  pw::thread::test::TestThreadContext context;
  for (int i = 0; i < 3; ++i) {
    pw::Thread thread(context.options(),
                      [&dispatcher] { dispatcher.Release(); });

    dispatcher.RunToCompletionUntilReleased();

    thread.join();
  }
}

}  // namespace
