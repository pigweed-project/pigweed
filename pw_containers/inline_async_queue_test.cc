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

#include "pw_containers/inline_async_queue.h"

#include "pw_async2/context.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/func_task.h"
#include "pw_async2/try.h"
#include "pw_preprocessor/compiler.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

namespace {

PW_MODIFY_DIAGNOSTIC(ignored, "-Wdeprecated-declarations");

using pw::async2::Context;
using pw::async2::DispatcherForTest;

using pw::async2::PendFuncTask;
using pw::async2::Poll;

static_assert(!std::is_constructible_v<pw::InlineAsyncQueue<int>>,
              "Cannot construct generic capacity container");

TEST(InlineAsyncQueueTest, PendHasZeroSpaceReturnsSuccessImmediately) {
  pw::InlineAsyncQueue<int, 4> queue;

  DispatcherForTest dispatcher;
  PendFuncTask task([&](Context& context) -> Poll<> {
    return queue.PendHasSpace(context, 0);
  });
  dispatcher.Post(task);
  dispatcher.RunToCompletion();
}

TEST(InlineAsyncQueueTest, PendHasSpaceWhenAvailableReturnsSuccessImmediately) {
  pw::InlineAsyncQueue<int, 4> queue;
  queue.push(1);
  queue.push(2);

  DispatcherForTest dispatcher;
  PendFuncTask task([&](Context& context) -> Poll<> {
    return queue.PendHasSpace(context, 2);
  });
  dispatcher.Post(task);
  dispatcher.RunToCompletion();
}

TEST(InlineAsyncQueueTest, PendPendHasSpaceWhenFullWaitsUntilPop) {
  pw::InlineAsyncQueue<int, 4> queue;
  queue.push(1);
  queue.push(2);
  queue.push(3);

  DispatcherForTest dispatcher;
  PendFuncTask task([&](Context& context) -> Poll<> {
    return queue.PendHasSpace(context, 3);
  });
  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  queue.pop();
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  queue.pop();
  dispatcher.RunToCompletion();
}

TEST(InlineAsyncQueueTest, PendHasSpaceWhenFullWaitsUntilClear) {
  pw::InlineAsyncQueue<int, 4> queue;
  queue.push(1);
  queue.push(2);
  queue.push(3);
  queue.push(4);

  DispatcherForTest dispatcher;
  PendFuncTask task([&](Context& context) -> Poll<> {
    return queue.PendHasSpace(context, 2);
  });
  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  queue.clear();
  dispatcher.RunToCompletion();
}

TEST(InlineAsyncQueueTest, PendHasSpaceOnGenericSizedReference) {
  pw::InlineAsyncQueue<int, 4> queue1;
  pw::InlineAsyncQueue<int>& queue2 = queue1;

  DispatcherForTest dispatcher;
  PendFuncTask task([&](Context& context) -> Poll<> {
    return queue2.PendHasSpace(context, 1);
  });
  dispatcher.Post(task);
  dispatcher.RunToCompletion();
}

TEST(InlineAsyncQueueTest, PendHasSpaceWaitsAfterReadyUntilPush) {
  pw::InlineAsyncQueue<int, 4> queue;
  DispatcherForTest dispatcher;

  PendFuncTask task1([&](Context& context) -> Poll<> {
    return queue.PendHasSpace(context, 1);
  });
  dispatcher.Post(task1);
  dispatcher.RunToCompletion();

  PendFuncTask task2([&](Context& context) -> Poll<> {
    return queue.PendHasSpace(context, 2);
  });
  dispatcher.Post(task2);

  // Even though there is room, the queue returns "Pending" until the space
  // reserved by the first task has been claimed.
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  queue.push(1);
  dispatcher.RunToCompletion();
}

TEST(InlineAsyncQueueTest, PendNotEmptyWhenNotEmptyReturnsSuccessImmediately) {
  pw::InlineAsyncQueue<int, 4> queue;
  queue.push(1);

  DispatcherForTest dispatcher;
  PendFuncTask task(
      [&](Context& context) -> Poll<> { return queue.PendNotEmpty(context); });
  dispatcher.Post(task);
  dispatcher.RunToCompletion();
}

TEST(InlineAsyncQueueTest, PendNotEmptyWhenEmptyWaitsUntilPush) {
  pw::InlineAsyncQueue<int, 4> queue;

  DispatcherForTest dispatcher;
  PendFuncTask task(
      [&](Context& context) -> Poll<> { return queue.PendNotEmpty(context); });
  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  queue.push(1);
  dispatcher.RunToCompletion();
}

TEST(InlineAsyncQueueTest, PendNotEmptyOnGenericSizedReference) {
  pw::InlineAsyncQueue<int, 4> queue1;
  pw::InlineAsyncQueue<int>& queue2 = queue1;
  queue2.push(1);

  DispatcherForTest dispatcher;
  PendFuncTask task(
      [&](Context& context) -> Poll<> { return queue2.PendNotEmpty(context); });
  dispatcher.Post(task);
  dispatcher.RunToCompletion();
}

TEST(InlineAsyncQueueTest, PendNotEmptyWaitsAfterReadyUntilPop) {
  pw::InlineAsyncQueue<int, 4> queue;
  DispatcherForTest dispatcher;
  queue.push(1);
  queue.push(2);

  PendFuncTask task1(
      [&](Context& context) -> Poll<> { return queue.PendNotEmpty(context); });
  dispatcher.Post(task1);
  dispatcher.RunToCompletion();

  PendFuncTask task2(
      [&](Context& context) -> Poll<> { return queue.PendNotEmpty(context); });
  dispatcher.Post(task2);

  // Even though there is an item, the queue returns "Pending" until the item
  // reserved by the first task has been claimed.
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  queue.pop();
  dispatcher.RunToCompletion();
}

}  // namespace
