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
#include "pw_channel/channel.h"

#include <optional>

#include "pw_allocator/testing.h"
#include "pw_assert/check.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_compilation_testing/negative_compilation.h"
#include "pw_multibuf/allocator.h"
#include "pw_multibuf/simple_allocator.h"
#include "pw_multibuf/v1/allocator_async.h"
#include "pw_preprocessor/compiler.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::allocator::test::AllocatorForTest;
using ::pw::async2::Context;
using ::pw::async2::DispatcherForTest;

using ::pw::async2::Pending;
using ::pw::async2::Poll;
using ::pw::async2::PollOptional;
using ::pw::async2::PollResult;
using ::pw::async2::Ready;
using ::pw::async2::Task;
using ::pw::async2::Waker;
using ::pw::channel::ByteChannel;
using ::pw::channel::DatagramWriter;
using ::pw::channel::kReadable;
using ::pw::channel::kReliable;
using ::pw::channel::kSeekable;
using ::pw::channel::kWritable;
using ::pw::multibuf::MultiBuf;
using ::pw::multibuf::MultiBufAllocator;
using ::pw::multibuf::SimpleAllocator;
using ::pw::multibuf::v1::MultiBufAllocationFuture;

static_assert(sizeof(::pw::channel::AnyChannel) == 2 * sizeof(void*));

static_assert((kReliable < kReadable) && (kReadable < kWritable) &&
              (kWritable < kSeekable));

class ReliableByteReaderWriterStub
    : public pw::channel::ByteChannelImpl<kReliable, kReadable, kWritable> {
 private:
  // Read functions

  PollResult<pw::multibuf::MultiBuf> DoPendRead(Context&) override {
    return Pending();
  }

  // Write functions

  // Disable maybe-uninitialized: this check fails erroniously on Windows GCC.
  PW_MODIFY_DIAGNOSTICS_PUSH();
  PW_MODIFY_DIAGNOSTIC_GCC(ignored, "-Wmaybe-uninitialized");
  Poll<pw::Status> DoPendReadyToWrite(Context&) override { return Pending(); }
  PW_MODIFY_DIAGNOSTICS_POP();

  PollOptional<MultiBuf> DoPendAllocateWriteBuffer(Context&, size_t) override {
    return std::nullopt;
  }

  pw::Status DoStageWrite(pw::multibuf::MultiBuf&&) override {
    return pw::Status::Unimplemented();
  }

  Poll<pw::Status> DoPendWrite(Context&) override {
    return Ready(pw::Status::Unimplemented());
  }

  // Common functions
  Poll<pw::Status> DoPendClose(Context&) override { return pw::OkStatus(); }
};

class ReadOnlyStub : public pw::channel::Implement<pw::channel::ByteReader> {
 public:
  constexpr ReadOnlyStub() = default;

 private:
  // Read functions
  PollResult<pw::multibuf::MultiBuf> DoPendRead(Context&) override {
    return Pending();
  }

  Poll<pw::Status> DoPendClose(Context&) override { return pw::OkStatus(); }
};

class WriteOnlyStub : public pw::channel::Implement<pw::channel::ByteWriter> {
 private:
  // Write functions

  Poll<pw::Status> DoPendReadyToWrite(Context&) override { return Pending(); }

  PollOptional<MultiBuf> DoPendAllocateWriteBuffer(Context&, size_t) override {
    return std::nullopt;
  }

  pw::Status DoStageWrite(pw::multibuf::MultiBuf&&) override {
    return pw::Status::Unimplemented();
  }

  Poll<pw::Status> DoPendWrite(Context&) override {
    return Ready(pw::Status::Unimplemented());
  }

  // Common functions
  Poll<pw::Status> DoPendClose(Context&) override { return pw::OkStatus(); }
};

TEST(Channel, MethodsShortCircuitAfterCloseReturnsReady) {
  DispatcherForTest dispatcher;

  class : public Task {
   public:
    ReliableByteReaderWriterStub channel;

   private:
    Poll<> DoPend(Context& cx) override {
      EXPECT_TRUE(channel.is_read_open());
      EXPECT_TRUE(channel.is_write_open());
      EXPECT_EQ(Ready(pw::OkStatus()), channel.PendClose(cx));
      EXPECT_FALSE(channel.is_read_open());
      EXPECT_FALSE(channel.is_write_open());

      EXPECT_EQ(pw::Status::FailedPrecondition(),
                channel.PendRead(cx)->status());
      EXPECT_EQ(Ready(pw::Status::FailedPrecondition()),
                channel.PendReadyToWrite(cx));
      EXPECT_EQ(Ready(pw::Status::FailedPrecondition()), channel.PendWrite(cx));
      EXPECT_EQ(Ready(pw::Status::FailedPrecondition()), channel.PendClose(cx));

      return Ready();
    }
  } test_task;
  dispatcher.Post(test_task);

  dispatcher.RunToCompletion();
}

TEST(Channel, ReadOnlyChannelOnlyOpenForReads) {
  ReadOnlyStub read_only;

  EXPECT_TRUE(read_only.readable());
  EXPECT_TRUE(read_only.is_read_open());
  EXPECT_FALSE(read_only.is_write_open());
}

TEST(Channel, WriteOnlyChannelOnlyOpenForWrites) {
  WriteOnlyStub write_only;

  EXPECT_FALSE(write_only.readable());
  EXPECT_FALSE(write_only.is_read_open());
  EXPECT_TRUE(write_only.is_write_open());
}

#if PW_NC_TEST(ChannelInvalidOrdering)
PW_NC_EXPECT("Properties must be specified in the following order");
bool Illegal(pw::channel::ByteChannel<kReadable, pw::channel::kReliable>& foo) {
  return foo.is_read_open();
}
#elif PW_NC_TEST(ChannelImplInvalidOrdering)
PW_NC_EXPECT("Properties must be specified in the following order");
class BadChannel
    : public pw::channel::ByteChannelImpl<kReadable, pw::channel::kReliable> {};
#elif PW_NC_TEST(ChannelNoProperties)
PW_NC_EXPECT("At least one of kReadable or kWritable must be provided");
bool Illegal(pw::channel::ByteChannel<>& foo) { return foo.is_read_open(); }
#elif PW_NC_TEST(ChannelImplNoProperties)
PW_NC_EXPECT("At least one of kReadable or kWritable must be provided");
class NoChannel : public pw::channel::ByteChannelImpl<> {};
#elif PW_NC_TEST(ChannelNoReadOrWrite)
PW_NC_EXPECT("At least one of kReadable or kWritable must be provided");
bool Illegal(pw::channel::ByteChannel<pw::channel::kReliable>& foo) {
  return foo.is_read_open();
}
#elif PW_NC_TEST(ChannelImplNoReadOrWrite)
PW_NC_EXPECT("At least one of kReadable or kWritable must be provided");
class BadChannel : public pw::channel::ByteChannelImpl<pw::channel::kReliable> {
};
#elif PW_NC_TEST(TooMany)
PW_NC_EXPECT("Too many properties given");
bool Illegal(
    pw::channel::
        ByteChannel<kReliable, kReliable, kReliable, kReadable, kWritable>&
            foo) {
  return foo.is_read_open();
}
#elif PW_NC_TEST(Duplicates)
PW_NC_EXPECT("duplicates");
bool Illegal(pw::channel::ByteChannel<kReadable, kReadable>& foo) {
  return foo.is_read_open();
}
#endif  // PW_NC_TEST

class TestByteReader
    : public pw::channel::ByteChannelImpl<kReliable, kReadable> {
 public:
  TestByteReader() {}

  void PushData(MultiBuf data) {
    bool was_empty = data_.empty();
    data_.PushSuffix(std::move(data));
    if (was_empty) {
      read_waker_.Wake();
    }
  }

 private:
  PollResult<MultiBuf> DoPendRead(Context& cx) override {
    if (data_.empty()) {
      PW_ASYNC_STORE_WAKER(
          cx, read_waker_, "TestByteReader is waiting for a call to PushData");
      return Pending();
    }
    return std::move(data_);
  }

  Poll<pw::Status> DoPendClose(Context&) override {
    return Ready(pw::OkStatus());
  }

  Waker read_waker_;
  MultiBuf data_;
};

class TestDatagramWriter : public pw::channel::Implement<DatagramWriter> {
 public:
  TestDatagramWriter(MultiBufAllocator& alloc) : alloc_fut_(alloc) {}

  const pw::multibuf::MultiBuf& last_datagram() const { return last_dgram_; }

  void MakeReadyToWrite() {
    PW_CHECK_INT_EQ(
        state_,
        kUnavailable,
        "Can't make writable when write is pending or already writable");

    state_ = kReadyToWrite;
    waker_.Wake();
  }

  void MakeReadyToFlush() {
    PW_CHECK_INT_EQ(state_,
                    kWritePending,
                    "Can't make flushable unless a write is pending");

    state_ = kReadyToFlush;
    waker_.Wake();
  }

 private:
  Poll<pw::Status> DoPendReadyToWrite(Context& cx) override {
    if (state_ == kReadyToWrite) {
      return Ready(pw::OkStatus());
    }

    PW_ASYNC_STORE_WAKER(
        cx,
        waker_,
        "TestDatagramWriter waiting for a call to MakeReadyToWrite");
    return Pending();
  }

  pw::Status DoStageWrite(MultiBuf&& buffer) override {
    if (state_ != kReadyToWrite) {
      return pw::Status::Unavailable();
    }

    state_ = kWritePending;
    last_dgram_ = std::move(buffer);
    return pw::OkStatus();
  }

  PollOptional<MultiBuf> DoPendAllocateWriteBuffer(Context& cx,
                                                   size_t min_bytes) override {
    alloc_fut_.SetDesiredSize(min_bytes);
    return alloc_fut_.Pend(cx);
  }

  Poll<pw::Status> DoPendWrite(Context& cx) override {
    if (state_ != kReadyToFlush) {
      PW_ASYNC_STORE_WAKER(
          cx, waker_, "TestDatagramWriter is waiting for its Channel to flush");
      return Pending();
    }
    last_flush_ = last_write_;
    return pw::OkStatus();
  }

  Poll<pw::Status> DoPendClose(Context&) override {
    return Ready(pw::OkStatus());
  }

  enum {
    kUnavailable,
    kReadyToWrite,
    kWritePending,
    kReadyToFlush,
  } state_ = kUnavailable;
  Waker waker_;
  uint32_t last_write_ = 0;
  uint32_t last_flush_ = 0;
  MultiBuf last_dgram_;
  MultiBufAllocationFuture alloc_fut_;
};

TEST(Channel, TestByteReader) {
  static constexpr char kReadData[] = "hello, world";
  static constexpr size_t kReadDataSize = sizeof(kReadData);
  static constexpr size_t kArbitraryMetaSize = 512;

  DispatcherForTest dispatcher;
  std::array<std::byte, kReadDataSize> data_area;
  AllocatorForTest<kArbitraryMetaSize> meta_alloc;
  SimpleAllocator simple_allocator(data_area, meta_alloc);
  std::optional<MultiBuf> read_buf_opt =
      simple_allocator.Allocate(kReadDataSize);
  ASSERT_TRUE(read_buf_opt.has_value());
  MultiBuf& read_buf = *read_buf_opt;

  class : public Task {
   public:
    TestByteReader channel;
    int test_executed = 0;

   private:
    Poll<> DoPend(Context& cx) override {
      auto result = channel.PendRead(cx);
      if (!result.IsReady()) {
        return Pending();
      }

      auto actual_result = std::move(*result);
      EXPECT_TRUE(actual_result.ok());

      std::byte contents[kReadDataSize] = {};

      EXPECT_EQ(actual_result->size(), sizeof(kReadData));
      std::copy(actual_result->begin(), actual_result->end(), contents);
      EXPECT_STREQ(reinterpret_cast<const char*>(contents), kReadData);

      test_executed += 1;
      return Ready();
    }
  } test_task;

  dispatcher.Post(test_task);

  EXPECT_TRUE(dispatcher.RunUntilStalled());

  auto kReadDataBytes = reinterpret_cast<const std::byte*>(kReadData);
  std::copy(kReadDataBytes, kReadDataBytes + kReadDataSize, read_buf.begin());
  test_task.channel.PushData(std::move(read_buf));
  dispatcher.RunToCompletion();

  EXPECT_EQ(test_task.test_executed, 1);
}

TEST(Channel, TestDatagramWriter) {
  DispatcherForTest dispatcher;
  static constexpr size_t kArbitraryDataSize = 128;
  static constexpr size_t kArbitraryMetaSize = 512;
  std::array<std::byte, kArbitraryDataSize> data_area;
  AllocatorForTest<kArbitraryMetaSize> meta_alloc;
  SimpleAllocator simple_allocator(data_area, meta_alloc);
  TestDatagramWriter write_channel(simple_allocator);

  static constexpr char kWriteData[] = "Hello there";

  class SendWriteDataAndFlush : public Task {
   public:
    explicit SendWriteDataAndFlush(DatagramWriter& channel, size_t)
        : channel_(channel) {}
    int test_executed = 0;

   private:
    Poll<> DoPend(Context& cx) override {
      switch (state_) {
        case kWaitUntilReady: {
          if (channel_.PendReadyToWrite(cx).IsPending()) {
            return Pending();
          }
          PollOptional<MultiBuf> buffer =
              channel_.PendAllocateWriteBuffer(cx, sizeof(kWriteData));
          if (buffer.IsPending()) {
            return Pending();
          }
          if (!buffer->has_value()) {
            // Allocator should have enough space for `kWriteData`.
            ADD_FAILURE();
            return Ready();
          }
          pw::ConstByteSpan str(pw::as_bytes(pw::span(kWriteData)));
          std::copy(str.begin(), str.end(), (**buffer).begin());
          pw::Status write_status = channel_.StageWrite(std::move(**buffer));
          PW_CHECK_OK(write_status);
          state_ = kFlushPacket;
          [[fallthrough]];
        }
        case kFlushPacket: {
          auto result = channel_.PendWrite(cx);
          if (result.IsPending()) {
            return Pending();
          }
          test_executed += 1;
          state_ = kWaitUntilReady;
          return Ready();
        }
        default:
          PW_CRASH("Illegal value");
      }

      // This test is INCOMPLETE.

      test_executed += 1;
      return Ready();
    }

    enum { kWaitUntilReady, kFlushPacket } state_ = kWaitUntilReady;
    DatagramWriter& channel_;
  };

  SendWriteDataAndFlush test_task(write_channel.channel(), 24601);
  dispatcher.Post(test_task);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  write_channel.MakeReadyToWrite();

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  write_channel.MakeReadyToFlush();

  dispatcher.RunToCompletion();
  EXPECT_EQ(test_task.test_executed, 1);

  std::byte contents[64] = {};
  const MultiBuf& dgram = write_channel.last_datagram();
  std::copy(dgram.begin(), dgram.end(), contents);
  EXPECT_STREQ(reinterpret_cast<const char*>(contents), kWriteData);
}

void TakesAChannel(const pw::channel::AnyChannel&) {}

const pw::channel::ByteChannel<kReadable>& TakesAReadableByteChannel(
    const pw::channel::ByteChannel<kReadable>& channel) {
  return channel;
}

void TakesAWritableByteChannel(const pw::channel::ByteChannel<kWritable>&) {}

TEST(Channel, Conversions) {
  static constexpr size_t kArbitraryDataSize = 128;
  static constexpr size_t kArbitraryMetaSize = 128;
  std::array<std::byte, kArbitraryDataSize> data_area;
  AllocatorForTest<kArbitraryMetaSize> meta_alloc;
  SimpleAllocator simple_allocator(data_area, meta_alloc);

  const TestByteReader byte_channel;
  const TestDatagramWriter datagram_channel(simple_allocator);

  TakesAReadableByteChannel(byte_channel.channel());

  TakesAReadableByteChannel(byte_channel.as<kReadable>());
  TakesAReadableByteChannel(byte_channel.channel().as<kReadable>());

  TakesAReadableByteChannel(byte_channel.as<pw::channel::ByteReader>());
  TakesAReadableByteChannel(
      byte_channel.channel().as<pw::channel::ByteReader>());

  TakesAReadableByteChannel(
      byte_channel.as<pw::channel::ByteChannel<kReliable, kReadable>>());
  TakesAReadableByteChannel(
      byte_channel.channel()
          .as<pw::channel::ByteChannel<kReliable, kReadable>>());

  TakesAChannel(byte_channel);
  // Conversions from Channel<> to AnyChannel must be explicit (with .as<>).
  // TakesAChannel(byte_channel.channel());
  TakesAChannel(byte_channel.as<pw::channel::AnyChannel>());

  TakesAWritableByteChannel(datagram_channel.IgnoreDatagramBoundaries());
  TakesAWritableByteChannel(
      datagram_channel.channel().IgnoreDatagramBoundaries());

  [[maybe_unused]] const pw::channel::AnyChannel& plain = byte_channel;

#if PW_NC_TEST(CannotImplicitlyLoseWritability)
  PW_NC_EXPECT("TakesAWritableByteChannel");
  TakesAWritableByteChannel(byte_channel.channel());
#elif PW_NC_TEST(CannotExplicitlyLoseWritability)
  PW_NC_EXPECT("Cannot use a non-writable channel as a writable channel");
  TakesAWritableByteChannel(byte_channel.as<kWritable>());
#elif PW_NC_TEST(CannotIgnoreDatagramBoundariesOnByteChannel)
  PW_NC_EXPECT("only be called to use a datagram channel to a byte channel");
  std::ignore = byte_channel.IgnoreDatagramBoundaries();
#elif PW_NC_TEST(CannotIgnoreDatagramBoundariesOnByteChannelImpl)
  PW_NC_EXPECT("only be called to use a datagram channel to a byte channel");
  std::ignore = byte_channel.channel().IgnoreDatagramBoundaries();
#endif  // PW_NC_TEST
}

#if PW_NC_TEST(CannotImplicitlyUseDatagramChannelAsByteChannel)
PW_NC_EXPECT("TakesAReadableByteChannel");
void DatagramChannelNcTest(
    pw::channel::DatagramChannel<kReliable, kReadable>& dgram) {
  TakesAReadableByteChannel(dgram);
}
#elif PW_NC_TEST(CannotExplicitlyUseDatagramChannelAsByteChannel)
PW_NC_EXPECT("Datagram and byte channels are not interchangeable");
void DatagramChannelNcTest(
    pw::channel::DatagramChannel<kReliable, kReadable>& dgram) {
  TakesAReadableByteChannel(dgram.as<pw::channel::ByteChannel<kReadable>>());
}
#endif  // PW_NC_TEST

class Foo {
 public:
  Foo(pw::channel::ByteChannel<kReadable>&) {}
  Foo(const Foo&) = default;
};

// Define additional overloads to ensure the right overload is selected with the
// implicit conversion.
[[maybe_unused]] void TakesAReadableByteChannel(const Foo&) {}
[[maybe_unused]] void TakesAReadableByteChannel(int) {}
[[maybe_unused]] void TakesAReadableByteChannel(
    const pw::channel::DatagramReaderWriter&) {}

TEST(Channel, SelectsCorrectOverloadWhenRelyingOnImplicitConversion) {
  TestByteReader byte_channel;

  [[maybe_unused]] Foo selects_channel_ctor_not_copy_ctor(
      byte_channel.channel());
  EXPECT_EQ(&byte_channel.as<pw::channel::ByteChannel<kReadable>>(),
            &TakesAReadableByteChannel(byte_channel.channel()));
}

#if PW_NC_TEST(CannotCallUnsupportedWriteMethodsOnChannel)
PW_NC_EXPECT("PendReadyToWrite may only be called on writable channels");
[[maybe_unused]] void Bad(Context& cx, pw::channel::DatagramReader& c) {
  std::ignore = c.PendReadyToWrite(cx);
}
#elif PW_NC_TEST(CannotCallUnsupportedWriteMethodsOnChannelImpl)
PW_NC_EXPECT("PendReadyToWrite may only be called on writable channels");
[[maybe_unused]] void Bad(Context& cx, pw::channel::ByteReader& c) {
  std::ignore = c.PendReadyToWrite(cx);
}
#elif PW_NC_TEST(CannotCallUnsupportedReadMethodsOnChannel)
PW_NC_EXPECT("PendRead may only be called on readable channels");
[[maybe_unused]] void Bad(Context& cx, pw::channel::ByteWriter& c) {
  std::ignore = c.PendRead(cx);
}
#elif PW_NC_TEST(CannotCallUnsupportedReadMethodsOnChannelImpl)
PW_NC_EXPECT("PendRead may only be called on readable channels");
[[maybe_unused]] void Bad(Context& cx, pw::channel::DatagramWriter& c) {
  std::ignore = c.PendRead(cx);
}
#endif  // PW_NC_TEST

}  // namespace
