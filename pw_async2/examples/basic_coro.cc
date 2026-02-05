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

#include "pw_allocator/allocator.h"
#include "pw_async2/coro_or_else_task.h"
#include "pw_async2/dispatcher.h"
#include "pw_status/status.h"

// DOCSTAG: [pw_async2-examples-basic-coro]
#include "pw_allocator/allocator.h"
#include "pw_async2/channel.h"
#include "pw_async2/coro.h"
#include "pw_log/log.h"
#include "pw_result/result.h"

namespace {

using ::pw::OkStatus;
using ::pw::Status;
using ::pw::async2::Coro;
using ::pw::async2::CoroContext;
using ::pw::async2::Receiver;
using ::pw::async2::Sender;

/// Create a coroutine which asynchronously receives a value from
/// ``receiver`` and forwards it to ``sender``.
///
/// Note: the ``CoroContext`` argument is used by the ``Coro<T>`` internals to
/// allocate the coroutine state. If this allocation fails, ``Coro<Status>``
/// will return ``Status::Internal()``.
Coro<Status> ForwardingCoro(CoroContext&,
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
#include "pw_async2/dispatcher_for_test.h"

namespace {

using ::pw::allocator::test::AllocatorForTest;
using ::pw::async2::ChannelStorage;
using ::pw::async2::CoroContext;
using ::pw::async2::CoroOrElseTask;
using ::pw::async2::CreateSpscChannel;

TEST(CoroExample, ReturnsOk) {
  ChannelStorage<int, 1> storage1;
  auto [handle1, sender1, receiver1] = CreateSpscChannel(storage1);
  ChannelStorage<int, 1> storage2;
  auto [handle2, sender2, receiver2] = CreateSpscChannel(storage2);
  handle1.Release();
  handle2.Release();

  AllocatorForTest<512> alloc;
  CoroContext coro_cx(alloc);
  CoroOrElseTask task(
      ForwardingCoro(coro_cx, std::move(receiver1), std::move(sender2)),
      [](Status) { FAIL(); });

  pw::async2::DispatcherForTest dispatcher;
  EXPECT_EQ(sender1.TrySend(42), pw::OkStatus());
  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  EXPECT_EQ(receiver2.TryReceive().value(), 42);
}

}  // namespace
