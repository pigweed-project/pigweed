// Copyright 2024 The Pigweed Authors
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
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/poll.h"
#include "pw_async2/value_future.h"
#include "pw_function/function.h"
#include "pw_status/status.h"

namespace {

using ::pw::allocator::test::AllocatorForTest;
using ::pw::async2::Context;
using ::pw::async2::DispatcherForTest;
using ::pw::async2::Pending;
using ::pw::async2::Poll;
using ::pw::async2::Ready;
using ::pw::async2::Task;
using ::pw::async2::ValueProvider;
using ::pw::async2::Waker;

struct TaskFunctorState {
  Waker last_waker = {};
  bool should_finish = false;
  int created = 0;
  int polled = 0;
  int destroyed = 0;
};

class TaskFunctor {
 public:
  TaskFunctor(TaskFunctorState& state) : state_(&state) { ++state_->created; }

  TaskFunctor() = delete;

  TaskFunctor(TaskFunctor&& other) : state_(other.state_) {
    other.state_ = nullptr;
  }
  TaskFunctor& operator=(TaskFunctor&& other) {
    Reset();
    state_ = other.state_;
    other.state_ = nullptr;
    return *this;
  }

  ~TaskFunctor() { Reset(); }
  Poll<> operator()(Context& cx) {
    if (state_ == nullptr) {
      return Pending();
    }
    PW_ASYNC_STORE_WAKER(
        cx, state_->last_waker, "TaskFunctor is waiting for last_waker");
    ++state_->polled;
    if (state_->should_finish) {
      return Ready();
    }
    return Pending();
  }

 private:
  void Reset() {
    if (state_ != nullptr) {
      ++state_->destroyed;
    }
  }
  TaskFunctorState* state_;
};

class AllocateTaskTest : public ::testing::Test {
 protected:
  AllocatorForTest<256> alloc_;
  DispatcherForTest dispatcher_;
};

TEST_F(AllocateTaskTest, AllocatesWithRvalue) {
  TaskFunctorState state = {.should_finish = true};
  TaskFunctor func(state);

  ASSERT_NE(dispatcher_.Post(alloc_, std::move(func)), nullptr);
  dispatcher_.RunToCompletion();
}

TEST_F(AllocateTaskTest, AllocatesWithArgs) {
  TaskFunctorState state = {.should_finish = true};

  ASSERT_NE(dispatcher_.Post<TaskFunctor>(alloc_, state), nullptr);
  dispatcher_.RunToCompletion();
}

TEST_F(AllocateTaskTest, DestroysOnceAfterPendReturnsReady) {
  TaskFunctorState state = {};
  ASSERT_NE(dispatcher_.Post<TaskFunctor>(alloc_, state), nullptr);

  EXPECT_TRUE(dispatcher_.RunUntilStalled());
  EXPECT_EQ(state.polled, 1);
  EXPECT_EQ(state.destroyed, 0);

  state.last_waker.Wake();
  state.should_finish = true;

  dispatcher_.RunToCompletion();
  EXPECT_EQ(state.polled, 2);
  EXPECT_EQ(state.destroyed, 1);

  // Ensure that the allocated task is not polled or destroyed again after being
  // deallocated.
  state.last_waker.Wake();
  dispatcher_.RunToCompletion();
  EXPECT_EQ(state.polled, 2);
  EXPECT_EQ(state.destroyed, 1);
}

class RepeatedTask final : public Task {
 public:
  explicit RepeatedTask(int runs, int& count) : runs_{runs}, count_(count) {}

  int runs() const { return runs_; }

 private:
  Poll<> DoPend(Context& cx) override {
    if (runs_ == 0) {
      return Ready();
    }
    runs_ -= 1;
    count_ += 1;
    cx.ReEnqueue();
    return Pending();
  }

  int runs_;
  int& count_;
};

TEST_F(AllocateTaskTest, TaskInstance) {
  int count = 0;
  auto task = dispatcher_.Post<RepeatedTask>(alloc_, 3, count);
  ASSERT_NE(task, nullptr);

  dispatcher_.RunToCompletion();
  EXPECT_EQ(count, 3);
  EXPECT_EQ(task->runs(), 0);
}

class TrackedTask : public Task {
 public:
  TrackedTask(int& executed, int& deleted)
      : executed_(executed), deleted_(deleted) {}

  ~TrackedTask() override { deleted_ += 1; }

 private:
  Poll<> DoPend(Context&) override {
    executed_ += 1;
    return Ready();
  }

  int& executed_;
  int& deleted_;
};

TEST_F(AllocateTaskTest, PostSharedPtr) {
  int executed = 0;
  int deleted = 0;

  pw::SharedPtr<TrackedTask> task =
      alloc_.MakeShared<TrackedTask>(executed, deleted);
  ASSERT_NE(task, nullptr);
  dispatcher_.PostShared(task);

  dispatcher_.RunToCompletion();
  EXPECT_EQ(executed, 1);
  EXPECT_EQ(deleted, 0);

  dispatcher_.PostShared(task);
  task.reset();

  EXPECT_EQ(executed, 1);
  EXPECT_EQ(deleted, 0);

  dispatcher_.RunToCompletion();
  EXPECT_EQ(executed, 2);
  EXPECT_EQ(deleted, 1);
}

TEST_F(AllocateTaskTest, DeregisteredRemovesTaskReference) {
  int executed = 0;
  int deleted = 0;

  pw::SharedPtr<TrackedTask> task =
      dispatcher_.Post<TrackedTask>(alloc_, executed, deleted);
  ASSERT_NE(task, nullptr);

  task->Deregister();
  EXPECT_EQ(deleted, 0);

  task.reset();
  EXPECT_EQ(deleted, 1);
}

TEST_F(AllocateTaskTest, DestroyingDispatcherRemovesTaskReference) {
  int executed = 0;
  int deleted = 0;

  {
    DispatcherForTest disp;

    pw::SharedPtr<TrackedTask> task =
        disp.Post<TrackedTask>(alloc_, executed, deleted);
    ASSERT_NE(task, nullptr);

    task.reset();
    EXPECT_EQ(deleted, 0);
  }

  EXPECT_EQ(deleted, 1);
}

TEST_F(AllocateTaskTest, Lambda) {
  int count = 0;
  ASSERT_NE(dispatcher_.Post(alloc_,
                             [&count](Context&) {
                               count += 1;
                               return Ready();
                             }),
            nullptr);

  dispatcher_.RunToCompletion();
  EXPECT_EQ(count, 1);
}

template <typename T>
struct FuncTaskType;

template <typename Func>
struct FuncTaskType<pw::async2::FuncTask<Func>> {
  using type = Func;
};

TEST_F(AllocateTaskTest, NeverDeducesReference) {
  int count = 0;
  auto func = [&count](Context&) {
    count += 1;
    return Ready();
  };
  auto& func_ref = func;

  auto value = dispatcher_.Post(alloc_, func_ref);
  static_assert(!std::is_reference_v<
                FuncTaskType<typename decltype(value)::element_type>::type>);

  dispatcher_.RunToCompletion();
  EXPECT_EQ(count, 1);

  // Explicitly specify reference
  auto ref = dispatcher_.Post<decltype(func)&>(alloc_, func_ref);
  static_assert(std::is_reference_v<
                FuncTaskType<typename decltype(ref)::element_type>::type>);

  auto moved_value = dispatcher_.Post(alloc_, std::move(func));
  static_assert(
      !std::is_reference_v<
          FuncTaskType<typename decltype(moved_value)::element_type>::type>);
}

TEST_F(AllocateTaskTest, SpecifiedFunctionType) {
  int count = 0;
  ASSERT_NE(
      dispatcher_.Post<pw::Function<Poll<>(Context&)>>(alloc_,
                                                       [&count](Context&) {
                                                         count += 1;
                                                         return Ready();
                                                       }),
      nullptr);

  dispatcher_.RunToCompletion();
  EXPECT_EQ(count, 1);
}

TEST_F(AllocateTaskTest, Future) {
  ValueProvider<int> provider;

  auto task = dispatcher_.PostFuture(alloc_, provider.Get());
  ASSERT_NE(task, nullptr);

  EXPECT_TRUE(dispatcher_.RunUntilStalled());

  provider.Resolve(3);
  dispatcher_.RunToCompletion();
  EXPECT_EQ(task->Wait(), 3);
}

TEST_F(AllocateTaskTest, RunOnce) {
  int count_1 = 0;
  auto task = dispatcher_.RunOnce(alloc_, [&count_1] {
    count_1 += 1;
    return -count_1;
  });

  dispatcher_.RunToCompletion();
  EXPECT_EQ(task->Wait(), -1);
  EXPECT_EQ(count_1, 1);
}

TEST_F(AllocateTaskTest, AllocationFailureDoesNotPostTask) {
  alloc_.Exhaust();

  int count = 0;
  EXPECT_EQ(dispatcher_.Post(alloc_,
                             [&count](Context&) {
                               count += 1;
                               return Ready();
                             }),
            nullptr);

  dispatcher_.RunToCompletion();
  EXPECT_EQ(count, 0);
}

}  // namespace
