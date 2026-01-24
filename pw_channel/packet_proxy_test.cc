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

#include "pw_channel/packet_proxy.h"

#include <cstddef>

#include "pw_allocator/testing.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/func_task.h"
#include "pw_channel/packet_proxy_task.h"
#include "pw_channel/test_packet_channel.h"
#include "pw_unit_test/framework.h"

namespace {

class TestPacket {
 public:
  static constexpr int kRequestReset = 12983;

  explicit constexpr TestPacket(int value = -1) : value_(value) {}

  constexpr TestPacket(TestPacket&& other)
      : value_(cpp20::exchange(other.value_, kEmpty)) {}

  constexpr TestPacket& operator=(TestPacket&& other) {
    value_ = cpp20::exchange(other.value_, kEmpty);
    return *this;
  }

  int value() const { return value_; }

  bool operator==(const TestPacket& other) const {
    return value_ == other.value_;
  }
  bool operator!=(const TestPacket& other) const {
    return value_ != other.value_;
  }

 private:
  static constexpr int kEmpty = -1;

  int value_;
};

class TestPacketTask
    : public pw::channel::PacketProxyTask<TestPacketTask, TestPacket> {
 public:
  using pw::channel::PacketProxyTask<TestPacketTask,
                                     TestPacket>::PacketProxyTask;

 private:
  friend pw::channel::PacketProxyTask<TestPacketTask, TestPacket>;

  void HandlePacket(Packet&& packet) {
    if (packet.value() == TestPacket::kRequestReset) {
      RequestReset();
    } else {
      ForwardPacket(std::move(packet));
    }
  }
};

class PacketProxyTest : public ::testing::Test {
 protected:
  PacketProxyTest()
      : channel_1_(allocator),
        channel_2_(allocator),
        task_1_(channel_1_.channel(), channel_2_.channel(), queue_2),
        task_2_(channel_2_.channel(), channel_1_.channel(), queue_1),
        proxy_(allocator, task_1_, task_2_) {}

  pw::allocator::test::AllocatorForTest<512> allocator;
  pw::InlineAsyncQueue<TestPacket, 8> queue_1;
  pw::InlineAsyncQueue<TestPacket, 8> queue_2;

  pw::channel::TestPacketReaderWriter<TestPacket> channel_1_;
  pw::channel::TestPacketReaderWriter<TestPacket> channel_2_;

  TestPacketTask task_1_;
  TestPacketTask task_2_;

  pw::channel::PacketProxy<TestPacketTask> proxy_;
};

TEST_F(PacketProxyTest, ForwardPacketsBothDirections) {
  pw::async2::DispatcherForTest dispatcher;
  proxy_.Run(dispatcher);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_TRUE(channel_1_.written_packets().empty());
  EXPECT_TRUE(channel_2_.written_packets().empty());

  channel_1_.EnqueueReadPacket(TestPacket(123));
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  ASSERT_EQ(channel_2_.written_packets().size(), 1u);

  pw::async2::PendFuncTask reset(
      [this](pw::async2::Context& context) { return proxy_.Reset(context); });
  dispatcher.Post(reset);
  dispatcher.RunToCompletion();
}

TEST_F(PacketProxyTest, RequestCancellationFromPacket) {
  pw::async2::DispatcherForTest dispatcher;
  proxy_.Run(dispatcher);

  EXPECT_TRUE(dispatcher.RunUntilStalled());

  channel_1_.EnqueueReadPacket(TestPacket(TestPacket::kRequestReset));

  dispatcher.RunToCompletion();
}

}  // namespace
