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

#include "pw_rpc/internal/channel_list.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "pw_rpc/channel.h"
#include "pw_rpc/internal/lock.h"
#include "pw_rpc/internal/packet.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc::internal {
namespace {

constexpr uint32_t kChannelId = 1;
constexpr uint32_t kNonExistentChannelId = 2;
constexpr Packet kPacket;

struct TestChannelOutput final : public ChannelOutput {
  bool received_data = false;

  TestChannelOutput(const char* name) : ChannelOutput(name) {}
  Status Send(span<const std::byte>) override {
    received_data = true;
    return OkStatus();
  }
};

TEST(Get, DefaultCtorListReturnsNullptr) {
  ChannelList list;

  EXPECT_EQ(list.Get(kChannelId), nullptr);
}

TEST(Get, ReturnsNullptrForNonexistentChannel) {
  std::array<Channel, 1> channels = {Channel::Create<kChannelId>(nullptr)};
  ChannelList list(channels);

  EXPECT_EQ(list.Get(kNonExistentChannelId), nullptr);
}

// If `PW_RPC_DYNAMIC_ALLOCATION` is enabled, then a copy of the channels are
// created and so it's not possible to directly compare their addresses in
// memory to make sure they are the same object. Instead, what is important is
// that any data sent through these channels goes through the same channel
// output, so the channel is checked by sending a packet and asserting the right
// channel output processed it.
TEST(Get, ReturnsCorrectChannel) {
  TestChannelOutput test_channel_output("test");
  std::array<Channel, 1> channels = {
      Channel::Create<kChannelId>(&test_channel_output)};
  ChannelList list(channels);

  ChannelBase* channel = list.Get(kChannelId);
  ASSERT_NE(channel, nullptr);
  RpcLockGuard lock_guard;
  ASSERT_EQ(channel->Send(kPacket), OkStatus());

  EXPECT_TRUE(test_channel_output.received_data);
}

// If `PW_RPC_DYNAMIC_ALLOCATION` is enabled, then a copy of the channels are
// created and so it's not possible to directly compare their addresses in
// memory to make sure they are the same object. Instead, what is important is
// that any data sent through these channels goes through the same channel
// output, so the channel is checked by sending a packet and asserting the right
// channel output processed it.
TEST(ChannelList, DefaultOutputCorrectlyConfigured) {
  TestChannelOutput default_channel_output("default");
  std::array<Channel, 1> channels = {Channel::Create<kChannelId>(nullptr)};
  ChannelList list(channels);

  RpcLockGuard lock_guard;
  ASSERT_EQ(list.SetDefaultChannelOutput(default_channel_output), OkStatus());

  ChannelBase* channel = list.Get(kNonExistentChannelId);
  ASSERT_NE(channel, nullptr);
  ASSERT_EQ(channel->Send(kPacket), OkStatus());

  EXPECT_TRUE(default_channel_output.received_data);
}

TEST(ChannelList, SetDefaultChannelOutputFailsIfAlreadySet) {
  TestChannelOutput default_channel_output("default");
  std::array<Channel, 1> channels = {Channel::Create<kChannelId>(nullptr)};
  ChannelList list(channels);

  RpcLockGuard lock_guard;
  ASSERT_EQ(list.SetDefaultChannelOutput(default_channel_output), OkStatus());

  EXPECT_EQ(list.SetDefaultChannelOutput(default_channel_output),
            Status::AlreadyExists());
}

}  // namespace
}  // namespace pw::rpc::internal
