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
#include "pw_async_basic/dispatcher.h"

#include <vector>

#include "pw_chrono/system_clock.h"
#include "pw_log/log.h"
#include "pw_span/span.h"
#include "pw_sync/thread_notification.h"
#include "pw_thread/thread.h"
#include "pw_thread_stl/options.h"
#include "pw_unit_test/framework.h"

#define ASSERT_CANCELLED(status) ASSERT_EQ(Status::Cancelled(), status)

using namespace std::chrono_literals;

static_assert(pw::async::kAtomicRepostingSupported);

namespace pw::async {
namespace {

// Lambdas can only capture one ptr worth of memory without allocating, so we
// group the data we want to share between tasks and their containing tests
// inside one struct.
struct TestPrimitives {
  int count = 0;
  sync::ThreadNotification notification;
};

TEST(DispatcherBasic, PostTasks) {
  BasicDispatcher dispatcher;
  Thread work_thread(thread::stl::Options(), dispatcher);

  TestPrimitives tp;
  auto inc_count = [&tp]([[maybe_unused]] Context& c, Status status) {
    PW_TEST_ASSERT_OK(status);
    ++tp.count;
  };

  Task task(inc_count);
  dispatcher.Post(task);

  Task task2(inc_count);
  dispatcher.Post(task2);

  Task task3([&tp]([[maybe_unused]] Context& c, Status status) {
    PW_TEST_ASSERT_OK(status);
    ++tp.count;
    tp.notification.release();
  });
  dispatcher.Post(task3);

  tp.notification.acquire();
  dispatcher.RequestStop();
  work_thread.join();
  ASSERT_EQ(tp.count, 3);
}

TEST(DispatcherBasic, ChainedTasks) {
  BasicDispatcher dispatcher;
  Thread work_thread(thread::stl::Options(), dispatcher);

  sync::ThreadNotification notification;
  Task task1([&notification]([[maybe_unused]] Context& c, Status status) {
    PW_TEST_ASSERT_OK(status);
    notification.release();
  });

  Task task2([&task1](Context& c, Status status) {
    PW_TEST_ASSERT_OK(status);
    c.dispatcher->Post(task1);
  });

  Task task3([&task2](Context& c, Status status) {
    PW_TEST_ASSERT_OK(status);
    c.dispatcher->Post(task2);
  });
  dispatcher.Post(task3);

  notification.acquire();
  dispatcher.RequestStop();
  work_thread.join();
}

TEST(DispatcherBasic, TaskOrdering) {
  struct TestState {
    std::vector<int> tasks;
    sync::ThreadNotification notification;
  };

  BasicDispatcher dispatcher;
  Thread work_thread(thread::stl::Options(), dispatcher);
  TestState state;

  Task task1([&state](Context&, Status status) {
    PW_TEST_ASSERT_OK(status);
    state.tasks.push_back(1);
  });

  Task task2([&state](Context&, Status status) {
    PW_TEST_ASSERT_OK(status);
    state.tasks.push_back(2);
    state.notification.release();
  });

  // Task posted at same time should be ordered FIFO.
  auto due_time = chrono::SystemClock::now();
  dispatcher.PostAt(task1, due_time);
  dispatcher.PostAt(task2, due_time);

  state.notification.acquire();
  dispatcher.RequestStop();
  work_thread.join();

  ASSERT_EQ(state.tasks.size(), 2U);
  EXPECT_EQ(state.tasks[0], 1);
  EXPECT_EQ(state.tasks[1], 2);
}

// Test RequestStop() from inside task.
TEST(DispatcherBasic, RequestStopInsideTask) {
  BasicDispatcher dispatcher;
  Thread work_thread(thread::stl::Options(), dispatcher);

  int count = 0;
  auto inc_count = [&count]([[maybe_unused]] Context& c, Status status) {
    ASSERT_CANCELLED(status);
    ++count;
  };

  // These tasks are never executed and cleaned up in RequestStop().
  Task task0(inc_count), task1(inc_count);
  dispatcher.PostAfter(task0, 20ms);
  dispatcher.PostAfter(task1, 21ms);

  Task stop_task([&count]([[maybe_unused]] Context& c, Status status) {
    PW_TEST_ASSERT_OK(status);
    ++count;
    static_cast<BasicDispatcher*>(c.dispatcher)->RequestStop();
  });
  dispatcher.Post(stop_task);

  work_thread.join();
  ASSERT_EQ(count, 3);
}

TEST(DispatcherBasic, TasksCancelledByRequestStopInDifferentThread) {
  BasicDispatcher dispatcher;
  Thread work_thread(thread::stl::Options(), dispatcher);

  int count = 0;
  auto inc_count = [&count]([[maybe_unused]] Context& c, Status status) {
    ASSERT_CANCELLED(status);
    ++count;
  };

  Task task0(inc_count), task1(inc_count), task2(inc_count);
  dispatcher.PostAfter(task0, 10s);
  dispatcher.PostAfter(task1, 10s);
  dispatcher.PostAfter(task2, 10s);

  dispatcher.RequestStop();
  work_thread.join();
  ASSERT_EQ(count, 3);
}

TEST(DispatcherBasic, TasksCancelledByDispatcherDestructor) {
  int count = 0;
  auto inc_count = [&count]([[maybe_unused]] Context& c, Status status) {
    ASSERT_CANCELLED(status);
    ++count;
  };
  Task task0(inc_count), task1(inc_count), task2(inc_count);

  {
    BasicDispatcher dispatcher;
    dispatcher.PostAfter(task0, 10s);
    dispatcher.PostAfter(task1, 10s);
    dispatcher.PostAfter(task2, 10s);
  }

  ASSERT_EQ(count, 3);
}

TEST(DispatcherBasic, TasksCancelledByRunUntilIdle) {
  int count = 0;
  auto inc_count = [&count]([[maybe_unused]] Context& c, Status status) {
    ASSERT_CANCELLED(status);
    ++count;
  };
  Task task0(inc_count), task1(inc_count), task2(inc_count);

  BasicDispatcher dispatcher;
  dispatcher.PostAfter(task0, 10s);
  dispatcher.PostAfter(task1, 10s);
  dispatcher.PostAfter(task2, 10s);

  dispatcher.RequestStop();
  dispatcher.RunUntilIdle();
  ASSERT_EQ(count, 3);
}

TEST(DispatcherBasic, TasksCancelledByRunFor) {
  int count = 0;
  auto inc_count = [&count]([[maybe_unused]] Context& c, Status status) {
    ASSERT_CANCELLED(status);
    ++count;
  };
  Task task0(inc_count), task1(inc_count), task2(inc_count);

  BasicDispatcher dispatcher;
  dispatcher.PostAfter(task0, 10s);
  dispatcher.PostAfter(task1, 10s);
  dispatcher.PostAfter(task2, 10s);

  dispatcher.RequestStop();
  dispatcher.RunFor(5s);
  ASSERT_EQ(count, 3);
}

class BasicDispatcherExecuteTask final : public BasicDispatcher {
 public:
  BasicDispatcherExecuteTask(pw::span<Task*> expected_tasks,
                             Status expected_status)
      : expected_tasks_(expected_tasks), expected_status_(expected_status) {}

 private:
  void ExecuteTask(backend::NativeTask& task, Status status) final {
    ASSERT_LT(executed_, expected_tasks_.size());
    ASSERT_EQ(&task, &(expected_tasks_[executed_++]->native_type()));
    ASSERT_EQ(status, expected_status_);
    BasicDispatcher::ExecuteTask(task, status);
  }

  pw::span<Task*> expected_tasks_;
  Status expected_status_;
  uint32_t executed_ = 0;
};

TEST(DispatcherBasic, ExecuteTaskOk) {
  TestPrimitives tp;
  auto inc_count = [&tp]([[maybe_unused]] Context& c, Status status) {
    PW_TEST_ASSERT_OK(status);
    ++tp.count;
  };

  Task task0(inc_count);

  Task task1(inc_count);

  Task task2([&tp]([[maybe_unused]] Context& c, Status status) {
    PW_TEST_ASSERT_OK(status);
    ++tp.count;
    tp.notification.release();
  });

  Task* tasks[] = {&task0, &task1, &task2};

  BasicDispatcherExecuteTask dispatcher(tasks, OkStatus());
  Thread work_thread(thread::stl::Options(), dispatcher);

  dispatcher.Post(task0);
  dispatcher.Post(task1);
  dispatcher.Post(task2);

  tp.notification.acquire();
  dispatcher.RequestStop();
  work_thread.join();
  ASSERT_EQ(tp.count, 3);
}

TEST(DispatcherBasic, ExecuteTaskCancelled) {
  int count = 0;
  auto inc_count = [&count]([[maybe_unused]] Context& c, Status status) {
    ASSERT_CANCELLED(status);
    ++count;
  };

  Task task0(inc_count);
  Task task1(inc_count);
  Task task2(inc_count);

  Task* tasks[] = {&task0, &task1, &task2};

  BasicDispatcherExecuteTask dispatcher(tasks, Status::Cancelled());

  dispatcher.PostAfter(task0, 10s);
  dispatcher.PostAfter(task1, 10s);
  dispatcher.PostAfter(task2, 10s);

  dispatcher.RequestStop();
  dispatcher.RunFor(5s);
  ASSERT_EQ(count, 3);
}

TEST(DispatcherBasic, TaskExecutedByRunUntil) {
  BasicDispatcher dispatcher;
  int count = 0;
  auto inc_count = [&count]([[maybe_unused]] Context& c, Status status) {
    PW_TEST_ASSERT_OK(status);
    ++count;
  };

  Task task(inc_count);
  dispatcher.Post(task);

  // The specific timeout here does not really matter as the task posted above
  // will be due immediately.
  const auto time_out_duration =
      dispatcher.now() +
      pw::chrono::SystemClock::for_at_least(std::chrono::nanoseconds(100000UL));
  dispatcher.RunUntil(time_out_duration);
  ASSERT_EQ(count, 1);
}

TEST(DispatcherBasic, TaskExecutedByRunFor) {
  BasicDispatcher dispatcher;
  int count = 0;
  auto inc_count = [&count]([[maybe_unused]] Context& c, Status status) {
    PW_TEST_ASSERT_OK(status);
    ++count;
  };

  Task task(inc_count);
  dispatcher.Post(task);

  // The specific timeout here does not really matter as the task posted above
  // will be due immediately.
  const auto time_out_duration =
      pw::chrono::SystemClock::for_at_least(std::chrono::nanoseconds(100000UL));
  dispatcher.RunFor(time_out_duration);
  ASSERT_EQ(count, 1);
}

}  // namespace
TEST(DispatcherBasic, PostFutureTaskUpgradesToImmediate) {
  BasicDispatcher dispatcher;
  Thread work_thread(thread::stl::Options(), dispatcher);

  TestPrimitives tp;
  Task task([&tp](Context&, Status status) {
    PW_TEST_ASSERT_OK(status);
    ++tp.count;
    tp.notification.release();
  });

  // Post with a long delay
  dispatcher.PostAfter(task, std::chrono::hours(1));

  // Post immediately (should upgrade the task to now)
  dispatcher.Post(task);

  // Wait for it to run
  tp.notification.acquire();

  dispatcher.RequestStop();
  work_thread.join();

  EXPECT_EQ(tp.count, 1);
}

TEST(DispatcherBasic, PostImmediateTaskPreservesPosition) {
  BasicDispatcher dispatcher;

  TestPrimitives tp;
  Task task1([&tp](Context&, Status status) {
    PW_TEST_ASSERT_OK(status);
    EXPECT_EQ(tp.count, 0);
    ++tp.count;
  });
  Task task2([&tp](Context&, Status status) {
    PW_TEST_ASSERT_OK(status);
    EXPECT_EQ(tp.count, 1);
    ++tp.count;
    tp.notification.release();
  });

  // Post task1 then task2
  dispatcher.Post(task1);
  dispatcher.Post(task2);

  // Re-post task1. It should still run before task2 because it's already
  // immediate.
  dispatcher.Post(task1);

  // Start the worker thread after posting tasks to ensure task1 is still
  // pending in the queue when re-posted, avoiding a race condition where
  // task1 executes before the re-post occurs.
  Thread work_thread(thread::stl::Options(), dispatcher);

  tp.notification.acquire();
  dispatcher.RequestStop();
  work_thread.join();

  EXPECT_EQ(tp.count, 2);
}

TEST(DispatcherBasic, PostAtMovesTaskToFuture) {
  BasicDispatcher dispatcher;

  TestPrimitives tp;
  Task task([&tp](Context&, Status status) {
    PW_TEST_ASSERT_OK(status);
    ++tp.count;
    tp.notification.release();
  });

  // Post immediately
  dispatcher.Post(task);

  // Move to future
  dispatcher.PostAfter(task, std::chrono::hours(1));

  // Since it's now in the future, it won't run immediately.
  // We can't easily wait for it not to run, but we can check it hasn't run yet.
  // For this test, we'll just cancel it and verify it was cancelled.

  EXPECT_TRUE(dispatcher.Cancel(task));

  // Start the worker thread after setup to ensure the task is still pending
  // when rescheduled and cancelled.
  Thread work_thread(thread::stl::Options(), dispatcher);
  dispatcher.RequestStop();
  work_thread.join();
  EXPECT_EQ(tp.count, 0);  // Should not have run
}

TEST(DispatcherBasic, PostImmediateToDifferentDispatcherShouldCrash) {
  BasicDispatcher dispatcher1;
  BasicDispatcher dispatcher2;

  Task task([](Context&, Status) {});

  dispatcher1.Post(task);

  EXPECT_DEATH_IF_SUPPORTED(
      dispatcher2.Post(task),
      "Attempted to post a task that is already posted to a different "
      "Dispatcher.");

  dispatcher1.Cancel(task);
}

TEST(DispatcherBasic, PostAtToDifferentDispatcherShouldCrash) {
  BasicDispatcher dispatcher1;
  BasicDispatcher dispatcher2;

  Task task([](Context&, Status) {});

  dispatcher1.Post(task);

  EXPECT_DEATH_IF_SUPPORTED(
      dispatcher2.PostAt(task, dispatcher2.now() + std::chrono::hours(1)),
      "Attempted to post a task that is already posted to a different "
      "Dispatcher.");

  dispatcher1.Cancel(task);
}

class RaceReproducerDispatcher : public BasicDispatcher {
 public:
  sync::ThreadNotification task_popped;
  sync::ThreadNotification task_reposted;
  std::atomic<bool> intercepted{false};

 private:
  void ExecuteTask(backend::NativeTask& task, Status status) override {
    if (!intercepted.exchange(true)) {
      task_popped.release();
      task_reposted.acquire();
    }
    BasicDispatcher::ExecuteTask(task, status);
  }
};

// Ensure that reposting a task while it is executing doesn't result in a race
// condition where the task's dispatcher pointer is overwritten to nullptr,
// causing subsequent posts to crash.
TEST(DispatcherBasic, RepostRaceCondition) {
  RaceReproducerDispatcher dispatcher;
  Thread work_thread(thread::stl::Options(), dispatcher);

  sync::ThreadNotification task_run;
  Task task([&task_run](Context&, Status) { task_run.release(); });

  dispatcher.Post(task);
  dispatcher.task_popped.acquire();
  dispatcher.PostAfter(task, 1h);
  dispatcher.task_reposted.release();
  task_run.acquire();

  dispatcher.Post(task);

  EXPECT_TRUE(dispatcher.Cancel(task));
  dispatcher.RequestStop();
  work_thread.join();
}

}  // namespace pw::async
