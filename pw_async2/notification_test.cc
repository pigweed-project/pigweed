// Copyright 2026 The Pigweed Authors
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

#include "pw_async2/notification.h"

#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/func_task.h"
#include "pw_async2/try.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::async2::Context;
using ::pw::async2::DispatcherForTest;
using ::pw::async2::FuncTask;
using ::pw::async2::Notification;
using ::pw::async2::Poll;
using ::pw::async2::Ready;
using ::pw::async2::VoidFuture;

TEST(Notification, WaitAndNotify) {
  Notification notification;
  DispatcherForTest dispatcher;

  bool woken = false;
  auto future = notification.Wait();
  FuncTask task([&future, &woken](Context& cx) -> Poll<> {
    PW_TRY_READY(future.Pend(cx));
    woken = true;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_FALSE(woken);

  notification.Notify();
  dispatcher.RunToCompletion();
  EXPECT_TRUE(woken);
}

TEST(Notification, MultipleWaiters) {
  Notification notification;
  DispatcherForTest dispatcher;

  int woke_count = 0;

  auto future1 = notification.Wait();
  FuncTask task1([&](Context& cx) -> Poll<> {
    PW_TRY_READY(future1.Pend(cx));
    ++woke_count;
    return Ready();
  });

  auto future2 = notification.Wait();
  FuncTask task2([&](Context& cx) -> Poll<> {
    PW_TRY_READY(future2.Pend(cx));
    ++woke_count;
    return Ready();
  });

  auto future3 = notification.Wait();
  FuncTask task3([&](Context& cx) -> Poll<> {
    PW_TRY_READY(future3.Pend(cx));
    ++woke_count;
    return Ready();
  });

  dispatcher.Post(task1);
  dispatcher.Post(task2);
  dispatcher.Post(task3);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(woke_count, 0);

  notification.Notify();
  dispatcher.RunToCompletion();
  EXPECT_EQ(woke_count, 3);
}

TEST(Notification, ReacquireAfterWait) {
  Notification notification;
  DispatcherForTest dispatcher;
  int count = 0;

  VoidFuture current_future;

  FuncTask task([&](Context& cx) -> Poll<> {
    while (count < 3) {
      if (!current_future.is_pendable()) {
        current_future = notification.Wait();
      }
      PW_TRY_READY(current_future.Pend(cx));
      count++;
    }
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(count, 0);

  notification.Notify();
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(count, 1);

  notification.Notify();
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(count, 2);

  notification.Notify();
  dispatcher.RunToCompletion();
  EXPECT_EQ(count, 3);
}

}  // namespace
