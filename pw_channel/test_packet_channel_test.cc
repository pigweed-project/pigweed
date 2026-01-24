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

#include "pw_channel/test_packet_channel.h"

#include "pw_allocator/testing.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/func_task.h"
#include "pw_async2/poll.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::async2::PendFuncTask;
using ::pw::channel::PendingWrite;

class TestPacketReaderWriterTest : public ::testing::Test {
 protected:
  TestPacketReaderWriterTest() : channel_(allocator_) {}

  pw::allocator::test::AllocatorForTest<512> allocator_;
  pw::channel::TestPacketReaderWriter<const char*> channel_;
  pw::async2::DispatcherForTest dispatcher_;
};

TEST_F(TestPacketReaderWriterTest, Read) {
  int completed = 0;
  PendFuncTask task([&](pw::async2::Context& cx) -> pw::async2::Poll<> {
    PW_TRY_READY_ASSIGN(pw::Result<const char*> result, channel_.PendRead(cx));
    EXPECT_STREQ(result.value(), "hello");
    completed += 1;
    return pw::async2::Ready();
  });

  dispatcher_.Post(task);
  EXPECT_TRUE(dispatcher_.RunUntilStalled());
  channel_.EnqueueReadPacket("hello");

  dispatcher_.RunToCompletion();
  EXPECT_EQ(completed, 1);
}

TEST_F(TestPacketReaderWriterTest, Write) {
  int completed = 0;
  PendFuncTask task([&](pw::async2::Context& cx) -> pw::async2::Poll<> {
    PW_TRY_READY_ASSIGN(pw::Result<PendingWrite<const char*>> pending_write,
                        channel_.PendReadyToWrite(cx, 3));
    EXPECT_EQ(pending_write.status(), pw::OkStatus());

    for (const char* packet : {"hello", ", ", "world"}) {
      pending_write->Stage(std::move(packet));
      EXPECT_TRUE(channel_.PendReadyToWrite(cx).IsPending());
    }
    EXPECT_TRUE(channel_.PendWrite(cx).IsReady());
    EXPECT_TRUE(channel_.PendClose(cx).IsReady());
    completed += 1;
    return pw::async2::Ready();
  });

  channel_.SetAvailableWrites(0);

  dispatcher_.Post(task);
  EXPECT_TRUE(dispatcher_.RunUntilStalled());
  EXPECT_EQ(completed, 0);

  channel_.SetAvailableWrites(3);
  dispatcher_.RunToCompletion();

  ASSERT_EQ(channel_.written_packets().size(), 3u);
  EXPECT_STREQ(channel_.written_packets()[0], "hello");
  EXPECT_STREQ(channel_.written_packets()[1], ", ");
  EXPECT_STREQ(channel_.written_packets()[2], "world");
}

}  // namespace
