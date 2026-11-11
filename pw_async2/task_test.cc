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

#include <mutex>
#include <utility>

#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/func_task.h"
#include "pw_async2/poll.h"
#include "pw_async2/waker.h"
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
    if (result_to_return_.IsReady()) {
      return result_to_return_;
    }
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
      waker_.Clear();
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

TEST(Context, IsSameDispatcher) {
  DispatcherForTest dispatcher_a;
  DispatcherForTest dispatcher_b;

  bool ran_on_a = false;
  bool ran_on_b = true;
  bool same_as_posted = false;
  bool same_as_other = true;
  bool same_as_unposted = true;

  pw::async2::FuncTask posted_on_a([](Context& cx) { return cx.Unschedule(); });
  pw::async2::FuncTask other_task([](Context& cx) { return cx.Unschedule(); });
  pw::async2::FuncTask unposted_task([](Context&) { return Ready(); });
  pw::async2::FuncTask task([&](Context& cx) {
    ran_on_a = cx.IsSameDispatcher(dispatcher_a);
    ran_on_b = cx.IsSameDispatcher(std::as_const(dispatcher_b));
    same_as_posted = cx.IsSameDispatcher(posted_on_a);
    same_as_other = cx.IsSameDispatcher(std::as_const(other_task));
    same_as_unposted = cx.IsSameDispatcher(unposted_task);
    return Ready();
  });

  dispatcher_a.Post(task);
  dispatcher_a.Post(posted_on_a);
  dispatcher_b.Post(other_task);
  dispatcher_a.RunToCompletion();
  posted_on_a.Deregister();
  other_task.Deregister();

  EXPECT_TRUE(ran_on_a);
  EXPECT_FALSE(ran_on_b);
  EXPECT_TRUE(same_as_posted);
  EXPECT_FALSE(same_as_other);
  EXPECT_FALSE(same_as_unposted);
}

class WakerTrackingTask : public Task {
 public:
  WakerTrackingTask() : Task(PW_ASYNC_TASK_NAME("WakerTrackingTask")) {}

  Poll<> DoPend(Context& cx) override {
    ++pend_count;
    if (should_complete) {
      waker1.Clear();
      waker2.Clear();
      return Ready();
    }
    if (store_waker1) {
      PW_ASYNC_STORE_WAKER(cx, waker1, "WakerTrackingTask waker1");
    }
    if (store_waker2) {
      PW_ASYNC_STORE_WAKER(cx, waker2, "WakerTrackingTask waker2");
    }
    return Pending();
  }

  int pend_count = 0;
  bool should_complete = false;
  bool store_waker1 = true;
  bool store_waker2 = false;
  Waker waker1;
  Waker waker2;
};

TEST(Task, TwoListStateMachine_WakeMovesToWokenListAndTriggersTask) {
  DispatcherForTest dispatcher;
  WakerTrackingTask task;
  dispatcher.Post(task);

  // Initial run queues and pends the task, storing waker1 into pending_wakers_.
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.pend_count, 1);
  EXPECT_FALSE(task.waker1.IsEmpty());

  // Waking waker1 moves it from pending_wakers_ to woken_wakers_ and posts
  // task.
  task.waker1.Wake();
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.pend_count, 2);

  // Complete the task and ensure wakers are cleared properly.
  task.should_complete = true;
  task.waker1.Wake();
  EXPECT_FALSE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.pend_count, 3);
  EXPECT_TRUE(task.waker1.IsEmpty());
}

TEST(Task, TwoListStateMachine_MultipleWakersSimultaneouslyAwoken) {
  DispatcherForTest dispatcher;
  WakerTrackingTask task;
  task.store_waker1 = true;
  task.store_waker2 = true;
  dispatcher.Post(task);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.pend_count, 1);
  EXPECT_FALSE(task.waker1.IsEmpty());
  EXPECT_FALSE(task.waker2.IsEmpty());

  // Wake both wakers before running dispatcher. Both move to woken_wakers_ and
  // get stashed in currently_executing_wakers during DoPend.
  task.waker1.Wake();
  task.waker2.Wake();
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.pend_count, 2);

  task.should_complete = true;
  task.waker1.Wake();
  EXPECT_FALSE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.pend_count, 3);
}

TEST(Task, TwoListStateMachine_RepeatedWakeIsIdempotent) {
  DispatcherForTest dispatcher;
  WakerTrackingTask task;
  dispatcher.Post(task);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.pend_count, 1);
  EXPECT_FALSE(task.waker1.IsEmpty());

  // Waking waker1 multiple times before running the dispatcher.
  task.waker1.Wake();
  task.waker1.Wake();
  task.waker1.Wake();

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.pend_count, 2);

  task.should_complete = true;
  task.waker1.Wake();
  EXPECT_FALSE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.pend_count, 3);
  EXPECT_TRUE(task.waker1.IsEmpty());
}

TEST(Task, TwoListStateMachine_DeregisterClearsPendingAndWokenWakers) {
  DispatcherForTest dispatcher;
  WakerTrackingTask task;
  task.store_waker1 = true;
  task.store_waker2 = true;
  dispatcher.Post(task);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.pend_count, 1);
  EXPECT_FALSE(task.waker1.IsEmpty());
  EXPECT_FALSE(task.waker2.IsEmpty());

  // Wake waker1 (moves to woken_wakers_), keep waker2 in pending_wakers_.
  task.waker1.Wake();

  task.Deregister();
  EXPECT_TRUE(task.waker1.IsEmpty());
  EXPECT_TRUE(task.waker2.IsEmpty());
}

#if PW_ASYNC2_CONFIG_ASSERT_LEAKED_WAKES
class WakeLeakingTask : public Task {
 public:
  WakeLeakingTask() : Task(PW_ASYNC_TASK_NAME("WakeLeakingTask")) {}

  Poll<> DoPend(Context& cx) override {
    if (first_pend_) {
      first_pend_ = false;
      PW_ASYNC_STORE_WAKER(cx, waker_, "WakeLeakingTask waker");
    }
    return Pending();
  }

  bool first_pend_ = true;
  Waker waker_;
};

TEST(TaskDeathTest, UnhandledWokenWakerTriggersCheck) {
  DispatcherForTest dispatcher;
  WakeLeakingTask task;
  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_FALSE(task.waker_.IsEmpty());

  task.waker_.Wake();
  EXPECT_DEATH_IF_SUPPORTED(
      dispatcher.RunUntilStalled(),
      "Task finished executing without handling all woken wakers");

  task.Deregister();
}
#else
class WakeLeakingWithNextWakerTask : public Task {
 public:
  WakeLeakingWithNextWakerTask()
      : Task(PW_ASYNC_TASK_NAME("WakeLeakingWithNextWakerTask")) {}

  Poll<> DoPend(Context& cx) override {
    ++pend_count;
    if (pend_count == 1) {
      PW_ASYNC_STORE_WAKER(cx, waker1, "WakeLeakingWithNextWakerTask waker1");
      return Pending();
    }
    if (pend_count == 2) {
      PW_ASYNC_STORE_WAKER(cx, waker2, "WakeLeakingWithNextWakerTask waker2");
      return Pending();
    }
    waker2.Clear();
    return Ready();
  }

  int pend_count = 0;
  Waker waker1;
  Waker waker2;
};

TEST(Task, UnhandledWokenWakerAllowedWhenDisabled) {
  DispatcherForTest dispatcher;
  WakeLeakingWithNextWakerTask task;
  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.pend_count, 1);
  EXPECT_FALSE(task.waker1.IsEmpty());

  task.waker1.Wake();
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.pend_count, 2);
  EXPECT_TRUE(task.waker1.IsEmpty());
  EXPECT_FALSE(task.waker2.IsEmpty());

  task.waker2.Wake();
  EXPECT_FALSE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.pend_count, 3);
  EXPECT_TRUE(task.waker2.IsEmpty());
}
#endif  // PW_ASYNC2_CONFIG_ASSERT_LEAKED_WAKES

class PendingWithoutWakerTask : public Task {
 public:
  PendingWithoutWakerTask()
      : Task(PW_ASYNC_TASK_NAME("PendingWithoutWakerTask")) {}

  Poll<> DoPend(Context&) override { return Pending(); }
};

TEST(TaskDeathTest, PendingWithoutWakerTriggersCheck) {
  DispatcherForTest dispatcher;
  PendingWithoutWakerTask task;
  dispatcher.Post(task);
  EXPECT_DEATH_IF_SUPPORTED(
      dispatcher.RunUntilStalled(),
      "returned Pending\\(\\) without registering a waker");

  task.Deregister();
}

}  // namespace
