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

#include "pw_system/internal/async_packet_io.h"

#include <cstddef>

#include "pw_allocator/testing.h"
#include "pw_channel/loopback_channel.h"
#include "pw_multibuf/simple_allocator_for_test.h"
#include "pw_rpc/server.h"
#include "pw_unit_test/framework.h"

namespace {

constexpr size_t kArbitrarySize = 2048;

TEST(AsyncPacketIo, Instantiate) {
  pw::multibuf::test::SimpleAllocatorForTest mb_alloc;
  pw::channel::LoopbackByteChannel channel(mb_alloc);
  pw::allocator::test::AllocatorForTest<kArbitrarySize> alloc;
  std::byte buffer[256];
  pw::rpc::Channel rpc_channel[1];
  pw::rpc::Server rpc_server(rpc_channel);

  pw::system::internal::PacketIO packet_io(
      channel.channel(), buffer, alloc, rpc_server);
}

TEST(RpcChannelOutputQueue, DropsOversizedDatagramAndReportsMtu) {
  pw::system::internal::RpcChannelOutputQueue queue;
  pw::rpc::ChannelOutput& output = queue;

  EXPECT_EQ(output.MaximumTransmissionUnit(),
            static_cast<size_t>(PW_SYSTEM_MAX_TRANSMISSION_UNIT));
  EXPECT_EQ(queue.dropped_packets(), 0u);

  std::byte oversized[PW_SYSTEM_MAX_TRANSMISSION_UNIT + 1]{};
  {
    pw::rpc::internal::RpcLockGuard lock;
    EXPECT_EQ(output.Send(oversized), pw::OkStatus());
  }
  EXPECT_EQ(queue.dropped_packets(), 1u);
}

}  // namespace
