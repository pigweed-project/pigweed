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

#include "pw_bluetooth_proxy/comms2/proxy.h"

#include "pw_allocator/testing.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/func_task.h"
#include "pw_async2/task.h"
#include "pw_bluetooth/hci_common.emb.h"
#include "pw_bluetooth_proxy/comms2/h4_packet.h"
#include "pw_bytes/array.h"
#include "pw_bytes/endian.h"
#include "pw_channel/test_packet_channel.h"
#include "pw_multibuf/v2/multibuf.h"
#include "pw_status/try.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::bluetooth::emboss::OpCode;
using ::pw::bluetooth::proxy::Direction;
using ::pw::bluetooth::proxy::H4Packet;
using ::pw::bluetooth::proxy::Proxy;
using ::pw::multibuf::v2::MultiBuf;

class ProxyTest : public ::testing::Test {
 protected:
  ProxyTest()
      : controller_channel_(allocator_),
        host_channel_(allocator_),
        controller_task_(Direction::kFromController,
                         controller_channel_.channel(),
                         host_channel_.channel()),
        host_task_(Direction::kFromHost,
                   host_channel_.channel(),
                   controller_channel_.channel()),
        proxy_(allocator_, controller_task_, host_task_) {}

  H4Packet CreateCommand(OpCode op_code = OpCode::UNSPECIFIED) {
    auto packet = allocator_.MakeUnique<std::byte[]>(5);
    auto code = pw::bytes::CopyInOrder(pw::endian::little,
                                       static_cast<uint32_t>(op_code));
    packet[0] = static_cast<std::byte>(H4Packet::Type::COMMAND);
    std::memcpy(&packet[1], code.data(), code.size());

    MultiBuf::Instance multibuf(allocator_);
    multibuf->PushBack(std::move(packet));

    H4Packet h4_packet(allocator_);
    h4_packet.PopulateFrom(std::move(*multibuf));
    return h4_packet;
  }

  pw::allocator::test::AllocatorForTest<1024> allocator_;

  pw::channel::TestPacketReaderWriter<H4Packet> controller_channel_;
  pw::channel::TestPacketReaderWriter<H4Packet> host_channel_;

  pw::bluetooth::proxy::L2capTask controller_task_;
  pw::bluetooth::proxy::L2capTask host_task_;
  Proxy proxy_;

  pw::async2::DispatcherForTest dispatcher_;
};

OpCode GetOpCode(const H4Packet& bytes) {
  return static_cast<OpCode>(static_cast<uint16_t>(bytes[0]) |
                             (static_cast<uint16_t>(bytes[1]) << 8));
}

TEST_F(ProxyTest, Passthrough) {
  proxy_.Run(dispatcher_);
  EXPECT_TRUE(dispatcher_.RunUntilStalled());

  controller_channel_.EnqueueReadPacket(CreateCommand(OpCode::INQUIRY));
  host_channel_.EnqueueReadPacket(CreateCommand(OpCode::REMOTE_NAME_REQUEST));

  EXPECT_TRUE(dispatcher_.RunUntilStalled());

  ASSERT_EQ(host_channel_.written_packets().size(), 1u);
  EXPECT_EQ(GetOpCode(host_channel_.written_packets().front()),
            OpCode::INQUIRY);

  ASSERT_EQ(controller_channel_.written_packets().size(), 1u);
  EXPECT_EQ(GetOpCode(controller_channel_.written_packets().front()),
            OpCode::REMOTE_NAME_REQUEST);

  pw::async2::FuncTask reset(
      [this](pw::async2::Context& context) { return proxy_.Reset(context); });
  dispatcher_.Post(reset);
  dispatcher_.RunToCompletion();
}

TEST_F(ProxyTest, ResetCommand) {
  proxy_.Run(dispatcher_);
  EXPECT_TRUE(dispatcher_.RunUntilStalled());

  controller_channel_.EnqueueReadPacket(CreateCommand(OpCode::RESET));

  // Resets from controller are ignored.
  EXPECT_TRUE(dispatcher_.RunUntilStalled());

  host_channel_.EnqueueReadPacket(CreateCommand(OpCode::RESET));

  dispatcher_.RunToCompletion();

  ASSERT_EQ(host_channel_.written_packets().size(), 1u);
  EXPECT_EQ(GetOpCode(host_channel_.written_packets().front()), OpCode::RESET);

  ASSERT_EQ(controller_channel_.written_packets().size(), 0u);
}

TEST_F(ProxyTest, ResetProxy) {
  proxy_.Run(dispatcher_);
  EXPECT_TRUE(dispatcher_.RunUntilStalled());

  pw::async2::FuncTask reset(
      [this](pw::async2::Context& context) { return proxy_.Reset(context); });
  dispatcher_.Post(reset);
  dispatcher_.RunToCompletion();
}

}  // namespace
