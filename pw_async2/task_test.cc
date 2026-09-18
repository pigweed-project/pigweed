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

#include "pw_async2/task.h"

#include <utility>

#include "pw_async2/dispatcher_for_test.h"
#include "pw_sync/binary_semaphore.h"
#include "pw_sync/mutex.h"
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
  explicit BlockingTask(Poll<> result_to_return)
      : Task(PW_ASYNC_TASK_NAME("BlockingTask")),
        result_to_return_(result_to_return) {}

  Poll<> DoPend(Context& cx) override {
    ready_to_deregister_.release();
    wait_for_deregister_.acquire();
    // Store a waker in case the task completes before it is deregistered.
    PW_ASYNC_STORE_WAKER(cx, waker_, "BlockingTask");
    return result_to_return_;
  }

  void WaitUntilRunning() { ready_to_deregister_.acquire(); }

  void Unblock() { wait_for_deregister_.release(); }

 private:
  pw::sync::BinarySemaphore ready_to_deregister_;
  pw::sync::BinarySemaphore wait_for_deregister_;

  Poll<> result_to_return_ = Pending();
  Waker waker_;
};

class SleepingTask : public Task {
 public:
  SleepingTask() : Task(PW_ASYNC_TASK_NAME("SleepingTask")) {}

  Poll<> DoPend(Context& cx) override {
    std::lock_guard lock(lock_);
    if (should_complete_) {
      return Ready();
    }
    PW_ASYNC_STORE_WAKER(cx, waker_, "SleepingTask is sleeping");
    sleeping_.release();
    return Pending();
  }

  void WaitUntilSleeping() { sleeping_.acquire(); }

  void Wake() {
    std::lock_guard lock(lock_);
    should_complete_ = true;
    waker_.Wake();
  }

 private:
  pw::sync::BinarySemaphore sleeping_;
  pw::sync::Mutex lock_;
  bool should_complete_ = false;
  Waker waker_;
};

TEST(Task, IsRegistered) {
  BlockingTask task(Ready());
  EXPECT_FALSE(task.IsRegistered());

  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  EXPECT_TRUE(task.IsRegistered());
}

TEST(Task, DeregisterWhileSleeping) {
  DispatcherForTest dispatcher;
  BlockingTask task(Ready());
  dispatcher.Post(task);

  task.Deregister();
  EXPECT_FALSE(task.IsRegistered());
}

void DeregisterWhileRunning(Poll<> task_return) {
  DispatcherForTest dispatcher;

  BlockingTask task(task_return);
  dispatcher.Post(task);

  pw::thread::test::TestThreadContext context;
  pw::Thread dispatcher_thread(context.options(),
                               [&dispatcher] { dispatcher.RunToCompletion(); });

  task.WaitUntilRunning();

  // Start a thread to unblock the task. The thread sleeps for a bit, so it
  // MIGHT unblock the task after Deregister() is called, but it's not
  // guaranteed.
  pw::thread::test::TestThreadContext unblock_context;
  pw::Thread unblock_thread(unblock_context.options(), [&task]() {
    pw::this_thread::sleep_for(10ms);
    task.Unblock();
  });

  task.Deregister();

  EXPECT_FALSE(task.IsRegistered());

  unblock_thread.join();
  dispatcher_thread.join();
}

TEST(Task, DeregisterRunningTask_TaskReturnsPending) {
  DeregisterWhileRunning(Pending());
}

TEST(Task, DeregisterRunningTask_TaskReturnsReady) {
  DeregisterWhileRunning(Ready());
}

TEST(Task, BlockingJoin_RunningTask) {
  DispatcherForTest dispatcher;
  BlockingTask task(Ready());
  dispatcher.Post(task);

  pw::thread::test::TestThreadContext context;
  pw::Thread dispatcher_thread(context.options(),
                               [&dispatcher] { dispatcher.RunToCompletion(); });

  task.WaitUntilRunning();

  pw::thread::test::TestThreadContext unblock_context;
  pw::Thread unblock_thread(unblock_context.options(), [&task]() {
    pw::this_thread::sleep_for(10ms);
    task.Unblock();
  });

  task.BlockingJoin();
  EXPECT_FALSE(task.IsRegistered());

  unblock_thread.join();
  dispatcher_thread.join();
}

TEST(Task, BlockingJoin_SleepingTask) {
  DispatcherForTest dispatcher;
  dispatcher.AllowBlocking();
  SleepingTask task;
  dispatcher.Post(task);

  pw::thread::test::TestThreadContext context;
  pw::Thread dispatcher_thread(context.options(),
                               [&dispatcher] { dispatcher.RunToCompletion(); });

  task.WaitUntilSleeping();

  pw::thread::test::TestThreadContext wake_context;
  pw::Thread wake_thread(wake_context.options(), [&task]() {
    pw::this_thread::sleep_for(10ms);
    task.Wake();
  });

  task.BlockingJoin();
  EXPECT_FALSE(task.IsRegistered());

  wake_thread.join();
  dispatcher_thread.join();
}

TEST(Context, IsRunningOn) {
  DispatcherForTest dispatcher_a;
  DispatcherForTest dispatcher_b;

  bool ran_on_a = false;
  bool ran_on_b = true;

  pw::async2::FuncTask task([&](Context& cx) {
    ran_on_a = cx.IsRunningOn(dispatcher_a);
    ran_on_b = cx.IsRunningOn(std::as_const(dispatcher_b));
    return Ready();
  });

  dispatcher_a.Post(task);
  dispatcher_a.RunToCompletion();

  EXPECT_TRUE(ran_on_a);
  EXPECT_FALSE(ran_on_b);
}

}  // namespace
