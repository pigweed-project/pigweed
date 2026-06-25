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

#include "pw_async_fuchsia/dispatcher.h"

#include <lib/async-testing/test_loop.h>

#include "pw_async_fuchsia/util.h"
#include "pw_unit_test/framework.h"

static_assert(pw::async::kAtomicRepostingSupported);

#define ASSERT_CANCELLED(status) ASSERT_EQ(Status::Cancelled(), status)

using namespace std::chrono_literals;

namespace pw::async_fuchsia {

class DispatcherFuchsiaTest : public ::testing::Test {
 public:
  async_dispatcher_t* dispatcher() { return loop_.dispatcher(); }
  void RunLoopUntilIdle() { loop_.RunUntilIdle(); }
  void RunLoopFor(zx::duration duration) { loop_.RunFor(duration); }

 private:
  ::async::TestLoop loop_;
};

TEST_F(DispatcherFuchsiaTest, TimeConversions) {
  zx::time time{timespec{123, 456}};
  chrono::SystemClock::time_point tp =
      pw::async_fuchsia::ZxTimeToTimepoint(time);
  EXPECT_EQ(tp.time_since_epoch(), 123s + 456ns);
  EXPECT_EQ(pw::async_fuchsia::TimepointToZxTime(tp), time);
}

TEST_F(DispatcherFuchsiaTest, Basic) {
  FuchsiaDispatcher fuchsia_dispatcher(dispatcher());

  bool set = false;
  async::Task task([&set](async::Context& ctx, Status status) {
    PW_TEST_ASSERT_OK(status);
    set = true;
  });
  fuchsia_dispatcher.Post(task);

  RunLoopUntilIdle();
  EXPECT_TRUE(set);
}

TEST_F(DispatcherFuchsiaTest, DelayedTasks) {
  FuchsiaDispatcher fuchsia_dispatcher(dispatcher());

  int c = 0;
  async::Task first([&c](async::Context& ctx, Status status) {
    PW_TEST_ASSERT_OK(status);
    c = c * 10 + 1;
  });
  async::Task second([&c](async::Context& ctx, Status status) {
    PW_TEST_ASSERT_OK(status);
    c = c * 10 + 2;
  });
  async::Task third([&c](async::Context& ctx, Status status) {
    PW_TEST_ASSERT_OK(status);
    c = c * 10 + 3;
  });

  fuchsia_dispatcher.PostAfter(third, 20ms);
  fuchsia_dispatcher.PostAfter(first, 5ms);
  fuchsia_dispatcher.PostAfter(second, 10ms);

  RunLoopFor(zx::msec(25));
  EXPECT_EQ(c, 123);
}

TEST_F(DispatcherFuchsiaTest, CancelTask) {
  FuchsiaDispatcher fuchsia_dispatcher(dispatcher());

  async::Task task([](async::Context& ctx, Status status) { FAIL(); });
  fuchsia_dispatcher.Post(task);
  EXPECT_TRUE(fuchsia_dispatcher.Cancel(task));

  RunLoopUntilIdle();
}

class DestructionChecker {
 public:
  explicit DestructionChecker(bool* flag) : flag_(flag) {}
  DestructionChecker(DestructionChecker&& other) {
    flag_ = other.flag_;
    other.flag_ = nullptr;
  }
  ~DestructionChecker() {
    if (flag_) {
      *flag_ = true;
    }
  }

 private:
  bool* flag_;
};

TEST_F(DispatcherFuchsiaTest, HeapAllocatedTasks) {
  FuchsiaDispatcher fuchsia_dispatcher(dispatcher());

  int c = 0;
  for (int i = 0; i < 3; i++) {
    Post(&fuchsia_dispatcher, [&c](async::Context& ctx, Status status) {
      PW_TEST_ASSERT_OK(status);
      c++;
    });
  }

  EXPECT_EQ(c, 0);
  RunLoopUntilIdle();
  EXPECT_EQ(c, 3);

  // Test that the lambda is destroyed after being called.
  bool flag = false;
  Post(&fuchsia_dispatcher,
       [checker = DestructionChecker(&flag)](async::Context& ctx,
                                             Status status) {});
  EXPECT_FALSE(flag);
  RunLoopUntilIdle();
  EXPECT_TRUE(flag);
}

TEST_F(DispatcherFuchsiaTest, ChainedTasks) {
  FuchsiaDispatcher fuchsia_dispatcher(dispatcher());

  int c = 0;

  Post(&fuchsia_dispatcher, [&c](async::Context& ctx, Status status) {
    PW_TEST_ASSERT_OK(status);
    c++;
    Post(ctx.dispatcher, [&c](async::Context& ctx, Status status) {
      PW_TEST_ASSERT_OK(status);
      c++;
      Post(ctx.dispatcher, [&c](async::Context& ctx, Status status) {
        PW_TEST_ASSERT_OK(status);
        c++;
      });
    });
  });

  RunLoopUntilIdle();
  EXPECT_EQ(c, 3);
}

TEST_F(DispatcherFuchsiaTest, PostImmediateToDifferentDispatcherShouldCrash) {
  FuchsiaDispatcher dispatcher1(dispatcher());

  ::async::TestLoop loop2;
  FuchsiaDispatcher dispatcher2(loop2.dispatcher());

  async::Task task([](async::Context&, Status) {});

  dispatcher1.Post(task);

  EXPECT_DEATH_IF_SUPPORTED(
      dispatcher2.Post(task),
      "Attempted to post a task that is already posted to a different "
      "Dispatcher.");

  dispatcher1.Cancel(task);
}

TEST_F(DispatcherFuchsiaTest, PostAtToDifferentDispatcherShouldCrash) {
  FuchsiaDispatcher dispatcher1(dispatcher());

  ::async::TestLoop loop2;
  FuchsiaDispatcher dispatcher2(loop2.dispatcher());

  async::Task task([](async::Context&, Status) {});

  dispatcher1.Post(task);

  EXPECT_DEATH_IF_SUPPORTED(
      dispatcher2.PostAt(task, dispatcher2.now() + 1h),
      "Attempted to post a task that is already posted to a different "
      "Dispatcher.");

  dispatcher1.Cancel(task);
}

}  // namespace pw::async_fuchsia
