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

#include "pw_channel/loopback_channel.h"

#include "pw_allocator/testing.h"
#include "pw_assert/check.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_bytes/suffix.h"
#include "pw_channel/channel.h"
#include "pw_multibuf/simple_allocator_for_test.h"
#include "pw_status/status.h"

namespace {

using ::pw::async2::Context;
using ::pw::async2::DispatcherForTest;

using ::pw::async2::Pending;
using ::pw::async2::Poll;
using ::pw::async2::Ready;
using ::pw::async2::Task;
using ::pw::operator""_b;
using ::pw::channel::ByteReaderWriter;
using ::pw::channel::LoopbackByteChannel;
using ::pw::multibuf::test::SimpleAllocatorForTest;

template <typename ChannelKind>
class ReaderTask : public Task {
 public:
  ReaderTask(ChannelKind& channel) : channel_(channel) {}

  int poll_count = 0;
  int read_count = 0;
  int bytes_read_count = 0;

 private:
  Poll<> DoPend(Context& cx) final {
    ++poll_count;
    while (true) {
      auto result = channel_.PendRead(cx);
      if (result.IsPending()) {
        return Pending();
      }
      if (!result->ok()) {
        // We hit an error-- call it quits.
        return Ready();
      }
      ++read_count;
      bytes_read_count += (**result).size();
    }
  }

  ChannelKind& channel_;
};

TEST(LoopbackByteChannel, IgnoresEmptyWrites) {
  SimpleAllocatorForTest alloc;
  LoopbackByteChannel channel(alloc);
  ReaderTask<ByteReaderWriter> read_task(channel.channel());

  DispatcherForTest dispatcher;
  dispatcher.Post(read_task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(read_task.poll_count, 1);
  EXPECT_EQ(read_task.read_count, 0);
  EXPECT_EQ(read_task.bytes_read_count, 0);

  PW_TEST_EXPECT_OK(channel.StageWrite(alloc.BufWith({})));

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(read_task.poll_count, 1);
  EXPECT_EQ(read_task.read_count, 0);
  EXPECT_EQ(read_task.bytes_read_count, 0);
}

TEST(LoopbackByteChannel, LoopsData) {
  SimpleAllocatorForTest alloc;
  LoopbackByteChannel channel(alloc);
  ReaderTask<ByteReaderWriter> read_task(channel.channel());

  DispatcherForTest dispatcher;
  dispatcher.Post(read_task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(read_task.poll_count, 1);
  EXPECT_EQ(read_task.read_count, 0);
  EXPECT_EQ(read_task.bytes_read_count, 0);

  PW_TEST_EXPECT_OK(channel.StageWrite(alloc.BufWith({1_b, 2_b, 3_b})));

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(read_task.poll_count, 2);
  EXPECT_EQ(read_task.read_count, 1);
  EXPECT_EQ(read_task.bytes_read_count, 3);
}

}  // namespace
