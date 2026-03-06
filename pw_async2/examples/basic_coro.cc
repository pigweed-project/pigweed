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

// DOCSTAG: [pw_async2-examples-basic-coro]
#include "pw_async2/channel.h"
#include "pw_async2/coro.h"
#include "pw_log/log.h"
#include "pw_result/result.h"
#include "pw_status/status.h"

namespace {

using ::pw::OkStatus;
using ::pw::Status;
using ::pw::async2::Coro;
using ::pw::async2::CoroContext;
using ::pw::async2::Receiver;
using ::pw::async2::Sender;

/// Create a coroutine which asynchronously receives a value from `receiver` and
/// forwards it to `sender`.
///
/// Note: the `CoroContext` argument is used by the `Coro<T>` internals to
/// allocate the coroutine state. If this allocation fails, the coroutine
/// aborts.
Coro<Status> ForwardingCoro(CoroContext,
                            Receiver<int> receiver,
                            Sender<int> sender) {
  std::optional<int> data = co_await receiver.Receive();
  if (!data.has_value()) {
    PW_LOG_ERROR("Receiving failed: channel has closed");
    co_return Status::Unavailable();
  }

  if (!(co_await sender.Send(*data))) {
    PW_LOG_ERROR("Sending failed: channel has closed");
    co_return Status::Unavailable();
  }

  co_return OkStatus();
}

}  // namespace
// DOCSTAG: [pw_async2-examples-basic-coro]

#include "pw_allocator/testing.h"
#include "pw_async2/coro_task.h"
#include "pw_async2/dispatcher_for_test.h"

namespace {

using ::pw::SharedPtr;
using ::pw::allocator::test::AllocatorForTest;
using ::pw::async2::ChannelStorage;
using ::pw::async2::CoroTask;
using ::pw::async2::CreateSpscChannel;

class CoroExample : public ::testing::Test {
 protected:
  CoroExample() {
    auto [handle1, s1, r1] = CreateSpscChannel(storage1_);
    auto [handle2, s2, r2] = CreateSpscChannel(storage2_);
    handle1.Release();
    handle2.Release();

    sender1_ = std::move(s1);
    receiver1_ = std::move(r1);
    sender2_ = std::move(s2);
    receiver2_ = std::move(r2);

    EXPECT_EQ(sender1_.TrySend(42), pw::OkStatus());
  }

  ChannelStorage<int, 1> storage1_;
  ChannelStorage<int, 1> storage2_;
  pw::async2::Sender<int> sender1_;
  pw::async2::Receiver<int> receiver1_;
  pw::async2::Sender<int> sender2_;
  pw::async2::Receiver<int> receiver2_;

  AllocatorForTest<512> allocator;
  pw::async2::DispatcherForTest dispatcher;
};

TEST_F(CoroExample, ImplicitCoro) {
  // DOCSTAG: [pw_async2-examples-basic-allocated]
  SharedPtr<CoroTask<Status>> task = dispatcher.Post<ForwardingCoro>(
      allocator, std::move(receiver1_), std::move(sender2_));

  // The task is automatically posted when allocated.
  dispatcher.RunToCompletion();
  // DOCSTAG: [pw_async2-examples-basic-allocated]

  EXPECT_EQ(receiver2_.TryReceive().value(), 42);
}

TEST_F(CoroExample, ExplicitCoro) {
  // DOCSTAG: [pw_async2-examples-basic-allocated-explicit]
  SharedPtr<CoroTask<Status>> task = dispatcher.Post(
      allocator,
      ForwardingCoro(allocator, std::move(receiver1_), std::move(sender2_)));

  // The task is automatically posted when allocated.
  dispatcher.RunToCompletion();
  // DOCSTAG: [pw_async2-examples-basic-allocated-explicit]

  EXPECT_EQ(receiver2_.TryReceive().value(), 42);
}

TEST_F(CoroExample, FallibleCoroTask) {
  // DOCSTAG: [pw_async2-examples-basic-allocated-fallible]
  auto task = dispatcher.Post(
      allocator,
      ForwardingCoro(allocator, std::move(receiver1_), std::move(sender2_)),
      [] { PW_LOG_ERROR("coroutine allocation failed! Aborting..."); });

  // The task is automatically posted when allocated.
  dispatcher.RunToCompletion();
  // DOCSTAG: [pw_async2-examples-basic-allocated-fallible]

  EXPECT_EQ(receiver2_.TryReceive().value(), 42);
}

TEST_F(CoroExample, CoroTask) {
  // DOCSTAG: [pw_async2-examples-basic-coro-task]
  // NOT RECOMMENDED: Manually declare a CoroTask to wrap a Coro. The coroutine
  // itself still requires dynamic allocation.
  pw::async2::CoroTask task(
      ForwardingCoro(allocator, std::move(receiver1_), std::move(sender2_)));

  dispatcher.Post(task);
  dispatcher.RunToCompletion();
  // DOCSTAG: [pw_async2-examples-basic-coro-task]

  EXPECT_EQ(receiver2_.TryReceive().value(), 42);
}

}  // namespace
