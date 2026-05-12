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
#include <mutex>

#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/func_task.h"
#include "pw_bytes/suffix.h"
#include "pw_memory/globals.h"
#include "pw_memory/no_destructor.h"
#include "pw_multibuf/simple_allocator_for_test.h"
#include "pw_status/status.h"
#include "pw_sync/mutex.h"
#include "pw_sync/thread_notification.h"
#include "pw_thread/test_thread_context.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::async2::Context;
using ::pw::async2::FuncTask;
using ::pw::async2::Pending;
using ::pw::async2::Poll;
using ::pw::async2::Ready;
using ::pw::multibuf::MultiBuf;
using ::pw::multibuf::test::SimpleAllocatorForTest;
using ::pw::operator""_b;

class SimplePipe {
 public:
  class PipeReader final : public pw::stream::NonSeekableReader {
   public:
    PipeReader(SimplePipe& pipe) : pipe_(pipe) {}

   private:
    pw::StatusWithSize DoRead(pw::ByteSpan dest) override {
      return pipe_.Read(dest);
    }
    SimplePipe& pipe_;
  };

  class PipeWriter final : public pw::stream::NonSeekableWriter {
   public:
    PipeWriter(SimplePipe& pipe) : pipe_(pipe) {}

   private:
    pw::Status DoWrite(pw::ConstByteSpan data) override {
      return pipe_.Write(data);
    }
    SimplePipe& pipe_;
  };

  SimplePipe() : reader_(*this), writer_(*this) {}

  PipeReader& GetReader() { return reader_; }
  PipeWriter& GetWriter() { return writer_; }

 private:
  pw::StatusWithSize Read(pw::ByteSpan dest) {
    while (true) {
      {
        std::lock_guard lock(mutex_);
        if (size_ > 0) {
          size_t to_read = std::min(dest.size(), size_);
          for (size_t i = 0; i < to_read; ++i) {
            dest[i] = buffer_[tail_];
            tail_ = (tail_ + 1) % buffer_.size();
          }
          size_ -= to_read;
          static_cast<void>(data_available_.try_acquire());
          return pw::StatusWithSize(to_read);
        }
      }
      data_available_.acquire();
    }
  }

  pw::Status Write(pw::ConstByteSpan data) {
    std::lock_guard lock(mutex_);
    if (size_ + data.size() > buffer_.size()) {
      return pw::Status::ResourceExhausted();
    }
    for (std::byte b : data) {
      buffer_[head_] = b;
      head_ = (head_ + 1) % buffer_.size();
    }
    size_ += data.size();
    data_available_.release();
    return pw::OkStatus();
  }

  PipeReader reader_;
  PipeWriter writer_;
  pw::sync::Mutex mutex_;
  pw::sync::ThreadNotification data_available_;
  std::array<std::byte, 16> buffer_{};
  size_t head_ = 0;
  size_t tail_ = 0;
  size_t size_ = 0;
};

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
  SimplePipe channel_input_pipe;
  SimplePipe channel_output_pipe;
  SimpleAllocatorForTest<> allocator;
  pw::thread::test::TestThreadContext read_thread_cx;
  pw::thread::test::TestThreadContext write_thread_cx;
};

TEST(StreamChannel, ReadsAndWritesData) {
  static pw::NoDestructor<LiveForeverTestData> test_data;
  static pw::RuntimeInitGlobal<pw::channel::StreamChannel> stream_channel(
      test_data->allocator,
      test_data->channel_input_pipe.GetReader(),
      test_data->read_thread_cx.options(),
      test_data->channel_output_pipe.GetWriter(),
      test_data->write_thread_cx.options());

  FuncTask read_task([&](Context& cx) -> Poll<> {
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
  FuncTask write_task([&](Context& cx) -> Poll<> {
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
            test_data->channel_input_pipe.GetWriter().Write(data_to_send));
  dispatcher.RunToCompletion();
}

}  // namespace
