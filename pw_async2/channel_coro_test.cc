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

#include "pw_allocator/testing.h"
#include "pw_async2/channel.h"
#include "pw_async2/coro.h"
#include "pw_async2/coro_task.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/try.h"
#include "pw_containers/vector.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::async2::ChannelStorage;
using pw::async2::Coro;
using pw::async2::CoroContext;
using pw::async2::CoroTask;
using pw::async2::CreateMpscChannel;
using pw::async2::CreateSpscChannel;
using pw::async2::Receiver;
using pw::async2::Sender;

Coro<pw::Status> Producer(CoroContext, Sender<int> sender, int start, int end) {
  for (int i = start; i <= end; ++i) {
    if (!co_await sender.Send(i)) {
      co_return pw::Status::Cancelled();
    }
  }
  co_return pw::OkStatus();
}

Coro<pw::Status> Consumer(CoroContext,
                          Receiver<int> receiver,
                          pw::Vector<int>& out) {
  while (true) {
    std::optional<int> value = co_await receiver.Receive();
    if (!value.has_value()) {
      break;
    }
    out.push_back(*value);
  }
  co_return pw::OkStatus();
}

Coro<pw::Status> DisconnectingConsumer(CoroContext,
                                       Receiver<int> receiver,
                                       size_t disconnect_after) {
  for (size_t i = 0; i < disconnect_after; ++i) {
    std::optional<int> value = co_await receiver.Receive();
    if (!value.has_value()) {
      break;
    }
  }
  receiver.Disconnect();
  co_return pw::OkStatus();
}

TEST(DynamicChannel, SingleProducerSingleConsumer) {
  pw::allocator::test::AllocatorForTest<1024> alloc;
  pw::async2::DispatcherForTest dispatcher;

  auto result = CreateSpscChannel<int>(alloc, 3);
  ASSERT_TRUE(result.has_value());
  auto&& [channel, sender, receiver] = *result;
  channel.Release();

  pw::Vector<int, 10> out;

  auto producer = CoroTask(Producer(alloc, std::move(sender), 1, 6));
  auto consumer = CoroTask(Consumer(alloc, std::move(receiver), out));

  dispatcher.Post(producer);
  dispatcher.Post(consumer);

  dispatcher.RunToCompletion();

  ASSERT_EQ(out.size(), 6u);
  EXPECT_EQ(out[0], 1);
  EXPECT_EQ(out[1], 2);
  EXPECT_EQ(out[2], 3);
  EXPECT_EQ(out[3], 4);
  EXPECT_EQ(out[4], 5);
  EXPECT_EQ(out[5], 6);
}

TEST(DynamicChannel, MultiProducerSingleConsumer) {
  pw::allocator::test::AllocatorForTest<1024> alloc;
  pw::async2::DispatcherForTest dispatcher;

  auto result = CreateMpscChannel<int>(alloc, 3);
  ASSERT_TRUE(result.has_value());
  auto&& [channel, receiver] = *result;

  pw::Vector<int, 10> out;

  auto producer_1 = CoroTask(Producer(alloc, channel.CreateSender(), 1, 3));
  auto producer_2 = CoroTask(Producer(alloc, channel.CreateSender(), 4, 6));
  auto consumer = CoroTask(Consumer(alloc, std::move(receiver), out));

  channel.Release();

  dispatcher.Post(producer_1);
  dispatcher.Post(producer_2);
  dispatcher.Post(consumer);

  dispatcher.RunToCompletion();

  ASSERT_EQ(out.size(), 6u);
  std::stable_partition(out.begin(), out.end(), [](int x) { return x < 4; });
  for (size_t i = 0; i < 6; ++i) {
    EXPECT_EQ(out[i], static_cast<int>(i + 1));
  }
}

TEST(DynamicChannel, ReceiverDisconnects) {
  pw::allocator::test::AllocatorForTest<1024> alloc;
  pw::async2::DispatcherForTest dispatcher;

  auto result = CreateSpscChannel<int>(alloc, 3);
  ASSERT_TRUE(result.has_value());
  auto&& [channel, sender, receiver] = *result;
  channel.Release();

  auto producer = CoroTask(Producer(alloc, std::move(sender), 1, 10));
  auto consumer =
      CoroTask(DisconnectingConsumer(alloc, std::move(receiver), 3));

  dispatcher.Post(producer);
  dispatcher.Post(consumer);

  dispatcher.RunToCompletion();

  EXPECT_EQ(producer.Wait(), pw::Status::Cancelled());
}

TEST(StaticChannel, SingleProducerSingleConsumer) {
  pw::allocator::test::AllocatorForTest<1024> alloc;
  pw::async2::DispatcherForTest dispatcher;

  ChannelStorage<int, 3> storage;
  auto [channel, sender, receiver] = CreateSpscChannel<int>(storage);
  channel.Release();
  pw::Vector<int, 10> out;

  auto producer = CoroTask(Producer(alloc, std::move(sender), 1, 6));
  auto consumer = CoroTask(Consumer(alloc, std::move(receiver), out));

  dispatcher.Post(producer);
  dispatcher.Post(consumer);

  dispatcher.RunToCompletion();

  ASSERT_EQ(out.size(), 6u);
  EXPECT_EQ(out[0], 1);
  EXPECT_EQ(out[1], 2);
  EXPECT_EQ(out[2], 3);
  EXPECT_EQ(out[3], 4);
  EXPECT_EQ(out[4], 5);
  EXPECT_EQ(out[5], 6);
}

}  // namespace
