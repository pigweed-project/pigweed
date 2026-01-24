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

#include "pw_channel/stream_channel.h"

#include <algorithm>
#include <array>

#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/func_task.h"
#include "pw_bytes/suffix.h"
#include "pw_multibuf/simple_allocator_for_test.h"
#include "pw_status/status.h"
#include "pw_stream/mpsc_stream.h"
#include "pw_thread/test_thread_context.h"
#include "pw_toolchain/globals.h"
#include "pw_toolchain/no_destructor.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::async2::Context;
using ::pw::async2::PendFuncTask;
using ::pw::async2::Pending;
using ::pw::async2::Poll;
using ::pw::async2::Ready;
using ::pw::multibuf::MultiBuf;
using ::pw::multibuf::test::SimpleAllocatorForTest;
using ::pw::operator""_b;

template <typename ActualIterable, typename ExpectedIterable>
void ExpectElementsEqual(const ActualIterable& actual,
                         const ExpectedIterable& expected) {
  auto actual_iter = actual.begin();
  auto expected_iter = expected.begin();
  for (; expected_iter != expected.end(); ++actual_iter, ++expected_iter) {
    ASSERT_NE(actual_iter, actual.end());
    EXPECT_EQ(*actual_iter, *expected_iter);
  }
}

template <typename ActualIterable, typename T>
void ExpectElementsEqual(const ActualIterable& actual,
                         std::initializer_list<T> expected) {
  ExpectElementsEqual<ActualIterable, std::initializer_list<T>>(actual,
                                                                expected);
}

struct LiveForeverTestData {
  LiveForeverTestData() {
    pw::stream::CreateMpscStream(channel_input_reader, channel_input_writer);
    pw::stream::CreateMpscStream(channel_output_reader, channel_output_writer);
  }
  pw::stream::BufferedMpscReader<512> channel_input_reader;
  pw::stream::MpscWriter channel_input_writer;
  pw::stream::BufferedMpscReader<512> channel_output_reader;
  pw::stream::MpscWriter channel_output_writer;
  SimpleAllocatorForTest<> allocator;
  pw::thread::test::TestThreadContext read_thread_cx;
  pw::thread::test::TestThreadContext write_thread_cx;
};

TEST(StreamChannel, ReadsAndWritesData) {
  static pw::NoDestructor<LiveForeverTestData> test_data;
  static pw::RuntimeInitGlobal<pw::channel::StreamChannel> stream_channel(
      test_data->allocator,
      test_data->channel_input_reader,
      test_data->read_thread_cx.options(),
      test_data->channel_output_writer,
      test_data->write_thread_cx.options());

  PendFuncTask read_task([&](Context& cx) -> Poll<> {
    auto read = stream_channel->PendRead(cx);
    if (read.IsPending()) {
      return Pending();
    }
    EXPECT_EQ(read->status(), pw::OkStatus());
    if (read->ok()) {
      ExpectElementsEqual(**read, {1_b, 2_b, 3_b});
    }
    return Ready();
  });

  MultiBuf to_send = test_data->allocator.BufWith({4_b, 5_b, 6_b});
  PendFuncTask write_task([&](Context& cx) -> Poll<> {
    if (stream_channel->PendReadyToWrite(cx).IsPending()) {
      return Pending();
    }
    PW_TEST_EXPECT_OK(stream_channel->StageWrite(std::move(to_send)));
    return Ready();
  });

  pw::async2::DispatcherForTest dispatcher;
  dispatcher.AllowBlocking();
  dispatcher.Post(write_task);
  dispatcher.Post(read_task);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  std::array<const std::byte, 3> data_to_send({1_b, 2_b, 3_b});
  ASSERT_EQ(pw::OkStatus(),
            test_data->channel_input_writer.Write(data_to_send));
  dispatcher.RunToCompletion();
}

}  // namespace
