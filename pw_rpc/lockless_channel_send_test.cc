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

#include "pw_rpc/internal/config.h"

#if PW_RPC_LOCKLESS_CHANNEL_SEND

#include "pw_rpc/channel.h"
#include "pw_rpc/internal/channel_list.h"
#include "pw_rpc/internal/lock.h"
#include "pw_rpc/internal/packet.h"
#include "pw_sync/binary_semaphore.h"
#include "pw_thread/sleep.h"
#include "pw_thread/test_thread_context.h"
#include "pw_thread/thread.h"
#include "pw_thread/yield.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc::internal {
namespace {

using namespace std::chrono_literals;

constexpr Packet kTestPacket(
    pwpb::PacketType::RESPONSE, 23, 42, 100, 0, {}, Status::NotFound());

class BlockingChannelOutput final : public ChannelOutput {
 public:
  sync::BinarySemaphore semaphore;
  sync::BinarySemaphore entered_send;

  BlockingChannelOutput() : ChannelOutput("blocking") {}

  Status Send(span<const std::byte>) override {
    entered_send.release();
    semaphore.acquire();
    return OkStatus();
  }
};

class LockAcquiringChannelOutput final : public ChannelOutput {
 public:
  LockAcquiringChannelOutput() : ChannelOutput("lock_acquiring") {}

  Status Send(span<const std::byte>) override {
    RpcLockGuard lock;
    return OkStatus();
  }
};

TEST(LocklessChannelSend, SendReleasesLock) {
  LockAcquiringChannelOutput output;
  Channel channel = Channel::Create<1>(&output);

  RpcLockGuard lock;
  // This would deadlock if the lock was not released by Send.
  EXPECT_EQ(static_cast<internal::ChannelBase&>(channel).Send(kTestPacket),
            OkStatus());
}

TEST(LocklessChannelSend, SendBlocksModification) {
  BlockingChannelOutput output1;
  ChannelList list;
  {
    RpcLockGuard lock;
    ASSERT_EQ(list.Add(1, output1), OkStatus());
  }

  sync::BinarySemaphore thread_b_done;

  // Thread A: Send 1 (blocks in output_->Send)
  thread::test::TestThreadContext context_a;
  Thread thread_a(context_a.options(), [&]() {
    RpcLockGuard lock;
    ChannelBase* c = list.Get(1);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->Send(kTestPacket), OkStatus());
  });

  // Wait for Thread A to enter Send
  output1.entered_send.acquire();

  // Thread B: Add Channel 2
  BlockingChannelOutput output2;
  output2.semaphore.release();  // Don't block

  thread::test::TestThreadContext context_b;
  Thread thread_b(context_b.options(), [&]() {
    RpcLockGuard lock;
    EXPECT_EQ(list.Add(2, output2), OkStatus());
    thread_b_done.release();
  });

  // Wait until Thread B sets the modification pending flag
  while (true) {
    {
      RpcLockGuard lock;
      if (lockless_send_state().channel_modification_pending) {
        break;
      }
    }
    this_thread::yield();
  }

  // Verify Thread B is blocked
  EXPECT_FALSE(thread_b_done.try_acquire());

  // Release Thread A (Send 1)
  output1.semaphore.release();
  thread_a.join();

  // Now Thread B (Add) should proceed
  thread_b_done.acquire();
  thread_b.join();

  // Verify Channel 2 was added
  {
    RpcLockGuard lock;
    EXPECT_NE(list.Get(2), nullptr);
  }
}

}  // namespace
}  // namespace pw::rpc::internal

#endif  // PW_RPC_LOCKLESS_CHANNEL_SEND
