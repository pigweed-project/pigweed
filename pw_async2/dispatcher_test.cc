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

#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/value_future.h"
#include "pw_containers/vector.h"
#include "pw_unit_test/framework.h"

namespace pw::async2 {
namespace {

class MockTask : public Task {
 public:
  bool should_complete = false;
  bool unschedule = false;
  int polled = 0;
  Waker last_waker;

  MockTask() : Task(PW_ASYNC_TASK_NAME("MockTask")) {}

 private:
  Poll<> DoPend(Context& cx) override {
    ++polled;
    if (unschedule) {
      return cx.Unschedule();
    }
    PW_ASYNC_STORE_WAKER(cx, last_waker, "MockTask is waiting for last_waker");
    if (should_complete) {
      return Ready();
    }
    return Pending();
  }
};

TEST(DispatcherForTest, RunUntilStalledPendsPostedTask) {
  MockTask task;
  task.should_complete = true;
  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  EXPECT_TRUE(task.IsRegistered());
  dispatcher.RunToCompletion();
  EXPECT_EQ(task.polled, 1);
  EXPECT_FALSE(task.IsRegistered());
  EXPECT_EQ(dispatcher.tasks_polled(), 1u);
  EXPECT_EQ(dispatcher.tasks_completed(), 1u);
}

TEST(DispatcherForTest, RunUntilStalledReturnsOnNotReady) {
  MockTask task;
  task.should_complete = false;
  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.polled, 1);
  EXPECT_EQ(dispatcher.tasks_polled(), 1u);
  EXPECT_EQ(dispatcher.tasks_completed(), 0u);
}

TEST(DispatcherForTest, RunUntilStalledDoesNotPendSleepingTask) {
  MockTask task;
  task.should_complete = false;
  DispatcherForTest dispatcher;
  dispatcher.Post(task);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.polled, 1);
  EXPECT_EQ(dispatcher.tasks_polled(), 1u);
  EXPECT_EQ(dispatcher.tasks_completed(), 0u);

  task.should_complete = true;
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.polled, 1);
  EXPECT_EQ(dispatcher.tasks_polled(), 1u);
  EXPECT_EQ(dispatcher.tasks_completed(), 0u);

  task.last_waker.Wake();
  dispatcher.RunToCompletion();
  EXPECT_EQ(task.polled, 2);
  EXPECT_EQ(dispatcher.tasks_polled(), 2u);
  EXPECT_EQ(dispatcher.tasks_completed(), 1u);
}

TEST(DispatcherForTest, RunUntilStalledWithNoTasksReturnsReady) {
  DispatcherForTest dispatcher;
  dispatcher.RunToCompletion();
  EXPECT_EQ(dispatcher.tasks_polled(), 0u);
  EXPECT_EQ(dispatcher.tasks_completed(), 0u);
}

TEST(DispatcherForTest, RunToCompletionPendsMultipleTasks) {
  class CounterTask : public Task {
   public:
    CounterTask(pw::span<Waker> wakers,
                size_t this_waker_i,
                int* counter,
                int until)
        : counter_(counter),
          this_waker_i_(this_waker_i),
          until_(until),
          wakers_(wakers) {}
    int* counter_;
    size_t this_waker_i_;
    int until_;
    pw::span<Waker> wakers_;

   private:
    Poll<> DoPend(Context& cx) override {
      ++(*counter_);
      if (*counter_ >= until_) {
        for (auto& waker : wakers_) {
          waker.Wake();
        }
        return Ready();
      } else {
        PW_ASYNC_STORE_WAKER(cx,
                             wakers_[this_waker_i_],
                             "CounterTask is waiting for counter_ >= until_");
        return Pending();
      }
    }
  };

  int counter = 0;
  constexpr const int kNumTasks = 3;
  std::array<Waker, kNumTasks> wakers;
  CounterTask task_one(wakers, 0, &counter, kNumTasks);
  CounterTask task_two(wakers, 1, &counter, kNumTasks);
  CounterTask task_three(wakers, 2, &counter, kNumTasks);
  DispatcherForTest dispatcher;
  dispatcher.Post(task_one);
  dispatcher.Post(task_two);
  dispatcher.Post(task_three);
  dispatcher.RunToCompletion();
  // We expect to see 5 total calls to `Pend`:
  // - two which increment counter and return pending
  // - one which increments the counter, returns complete, and wakes the
  //   others
  // - two which have woken back up and complete
  EXPECT_EQ(counter, 5);
  EXPECT_EQ(dispatcher.tasks_polled(), 5u);
}

TEST(DispatcherForTest, RunInTaskUntilStalledReturnsOutputOnReady) {
  auto future = pw::async2::ValueFuture<int>::Resolved(5);

  DispatcherForTest dispatcher_for_test;
  Poll<int> result = dispatcher_for_test.RunInTaskUntilStalled(future);
  EXPECT_EQ(result, Ready(5));
}

TEST(DispatcherForTest, RunInTaskUntilStalledReturnsPending) {
  pw::async2::ValueProvider<int> provider;
  pw::async2::ValueFuture<int> future = provider.Get();

  DispatcherForTest dispatcher_for_test;
  Poll<int> result = dispatcher_for_test.RunInTaskUntilStalled(future);
  EXPECT_EQ(result, Pending());
}

TEST(DispatcherForTest, PostToDispatcherFromInsidePendSucceeds) {
  class TaskPoster : public Task {
   public:
    TaskPoster(Task& task_to_post) : task_to_post_(&task_to_post) {}

   private:
    Poll<> DoPend(Context& cx) override {
      cx.dispatcher().Post(*task_to_post_);
      return Ready();
    }
    Task* task_to_post_;
  };

  MockTask posted_task;
  posted_task.should_complete = true;
  TaskPoster task_poster(posted_task);

  DispatcherForTest dispatcher;
  dispatcher.Post(task_poster);
  dispatcher.RunToCompletion();
  EXPECT_EQ(posted_task.polled, 1);
  EXPECT_EQ(dispatcher.tasks_polled(), 2u);
}

TEST(DispatcherForTest, RunToCompletionPendsPostedTask) {
  MockTask task;
  task.should_complete = true;
  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  dispatcher.RunToCompletion();
  EXPECT_EQ(task.polled, 1);
  EXPECT_EQ(dispatcher.tasks_polled(), 1u);
}

TEST(DispatcherForTest, RunToCompletionIgnoresDeregisteredTask) {
  DispatcherForTest dispatcher;
  MockTask task;
  task.should_complete = false;
  dispatcher.Post(task);
  EXPECT_TRUE(task.IsRegistered());
  task.Deregister();
  EXPECT_FALSE(task.IsRegistered());
  dispatcher.RunToCompletion();
  EXPECT_EQ(task.polled, 0);
  EXPECT_EQ(dispatcher.tasks_polled(), 0u);
}

TEST(DispatcherForTest, UnscheduleAllowsRepost) {
  DispatcherForTest dispatcher;
  MockTask task;
  task.should_complete = false;
  task.unschedule = true;
  dispatcher.Post(task);
  EXPECT_TRUE(task.IsRegistered());

  // The dispatcher returns Ready() since the task has opted out of being woken,
  // so it no longer exists in the dispatcher queues.
  dispatcher.RunToCompletion();
  EXPECT_EQ(task.polled, 1);
  EXPECT_EQ(dispatcher.tasks_polled(), 1u);

  dispatcher.RunToCompletion();
  EXPECT_EQ(task.polled, 1);
  EXPECT_EQ(dispatcher.tasks_polled(), 1u);

  // The task must be re-posted to run again.
  task.should_complete = true;
  dispatcher.Post(task);
  dispatcher.RunToCompletion();
  EXPECT_EQ(task.polled, 2);
  EXPECT_EQ(dispatcher.tasks_polled(), 2u);
}

class WakeCounter final : public pw::async2::Dispatcher {
 public:
  int wake_count() const { return wake_count_; }

  using pw::async2::Dispatcher::PopAndRunAllReadyTasks;

 private:
  void DoWake() override { wake_count_ += 1; }

  int wake_count_ = 0;
};

TEST(Dispatcher, PostOnlyWakesOnce) {
  MockTask task1, task2, task3;
  WakeCounter dispatcher;
  dispatcher.Post(task1);
  dispatcher.Post(task2);
  dispatcher.Post(task3);

  EXPECT_EQ(dispatcher.wake_count(), 1);
}

TEST(Dispatcher, WakingMultipleTasksOnlyWakesOnce) {
  MockTask task1, task2, task3;
  WakeCounter dispatcher;
  dispatcher.Post(task1);
  dispatcher.Post(task2);
  dispatcher.Post(task3);

  dispatcher.PopAndRunAllReadyTasks();

  EXPECT_EQ(dispatcher.wake_count(), 1);

  task1.last_waker.Wake();
  EXPECT_EQ(dispatcher.wake_count(), 2);

  task2.last_waker.Wake();
  EXPECT_EQ(dispatcher.wake_count(), 2);

  task3.last_waker.Wake();
  EXPECT_EQ(dispatcher.wake_count(), 2);
}

TEST(Dispatcher, WakingMultipleTasksAndPostingOnlyWakesOnce) {
  MockTask task1, task2, task3;
  WakeCounter dispatcher;
  dispatcher.Post(task1);
  dispatcher.Post(task2);

  dispatcher.PopAndRunAllReadyTasks();

  EXPECT_EQ(dispatcher.wake_count(), 1);

  task1.last_waker.Wake();
  EXPECT_EQ(dispatcher.wake_count(), 2);

  task2.last_waker.Wake();
  dispatcher.Post(task3);
  EXPECT_EQ(dispatcher.wake_count(), 2);
}

}  // namespace
}  // namespace pw::async2
