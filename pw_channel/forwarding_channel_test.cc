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

#include "pw_channel/forwarding_channel.h"

#include <algorithm>
#include <array>

#include "pw_allocator/testing.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_multibuf/simple_allocator.h"
#include "pw_multibuf/v1/header_chunk_region_tracker.h"
#include "pw_string/string.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::async2::Context;
using ::pw::async2::Pending;
using ::pw::async2::Poll;
using ::pw::async2::PollResult;
using ::pw::async2::Ready;
using ::pw::async2::Task;
using ::pw::async2::Waker;
using ::pw::channel::ByteReader;
using ::pw::channel::DatagramReader;
using ::pw::multibuf::MultiBuf;

// Creates and initializes a MultiBuf to the specified value.
class InitializedMultiBuf {
 public:
  InitializedMultiBuf(std::string_view contents) {
    std::optional<pw::multibuf::OwnedChunk> chunk =
        pw::multibuf::v1::HeaderChunkRegionTracker::AllocateRegionAsChunk(
            allocator_, contents.size());
    std::memcpy(chunk.value().data(), contents.data(), contents.size());
    buf_.PushFrontChunk(std::move(*chunk));
  }

  pw::multibuf::MultiBuf Take() { return std::move(buf_); }

 private:
  pw::allocator::test::AllocatorForTest<2048> allocator_;
  pw::multibuf::MultiBuf buf_;
};

pw::InlineString<128> CopyToString(const pw::multibuf::MultiBuf& mb) {
  pw::InlineString<128> contents(mb.size(), '\0');
  std::copy(
      mb.begin(), mb.end(), reinterpret_cast<std::byte*>(contents.data()));
  return contents;
}

template <pw::channel::DataType kType,
          size_t kDataSize = 128,
          size_t kMetaSize = 128>
class TestChannelPair {
 public:
  TestChannelPair()
      : first_out_alloc_(first_out_data_area_, meta_alloc_),
        second_out_alloc_(second_out_data_area_, meta_alloc_),
        pair_(first_out_alloc_, second_out_alloc_) {}

  pw::channel::ForwardingChannelPair<kType>* operator->() { return &pair_; }
  pw::channel::ForwardingChannelPair<kType>& operator*() { return pair_; }

 private:
  std::array<std::byte, kDataSize> first_out_data_area_;
  std::array<std::byte, kDataSize> second_out_data_area_;
  pw::allocator::test::AllocatorForTest<kMetaSize> meta_alloc_;
  pw::multibuf::SimpleAllocator first_out_alloc_;
  pw::multibuf::SimpleAllocator second_out_alloc_;

  pw::channel::ForwardingChannelPair<kType> pair_;
};

// TODO: b/330788671 - Have the test tasks run in multiple stages to ensure that
//     wakers are stored / woken properly by ForwardingChannel.
TEST(ForwardingDatagramChannel, ForwardsEmptyDatagrams) {
  pw::async2::DispatcherForTest dispatcher;

  class : public pw::async2::Task {
   public:
    TestChannelPair<pw::channel::DataType::kDatagram> pair;

    int test_completed = 0;

   private:
    pw::async2::Poll<> DoPend(pw::async2::Context& cx) override {
      // No data yet
      EXPECT_EQ(pw::async2::Pending(), pair->first().PendRead(cx));
      EXPECT_EQ(pw::async2::Pending(), pair->second().PendRead(cx));

      // Send datagram first->second
      EXPECT_EQ(pw::async2::Ready(pw::OkStatus()),
                pair->first().PendReadyToWrite(cx));
      PW_TEST_EXPECT_OK(pair->first().StageWrite({}));  // Write empty datagram

      EXPECT_EQ(pw::async2::Pending(), pair->first().PendReadyToWrite(cx));
      EXPECT_EQ(pw::async2::Pending(), pair->first().PendRead(cx));

      auto empty_chunk_result = pair->second().PendRead(cx);
      EXPECT_TRUE(empty_chunk_result.IsReady());
      EXPECT_TRUE(empty_chunk_result->ok());
      EXPECT_EQ((*empty_chunk_result)->size(), 0u);

      EXPECT_EQ(pw::async2::Pending(), pair->second().PendRead(cx));

      // Send datagram second->first
      EXPECT_EQ(pw::async2::Ready(pw::OkStatus()),
                pair->second().PendReadyToWrite(cx));
      PW_TEST_EXPECT_OK(pair->second().StageWrite({}));  // Write empty datagram

      EXPECT_EQ(pw::async2::Pending(), pair->second().PendReadyToWrite(cx));
      EXPECT_EQ(pw::async2::Pending(), pair->second().PendRead(cx));

      empty_chunk_result = pair->first().PendRead(cx);
      EXPECT_TRUE(empty_chunk_result.IsReady());
      EXPECT_TRUE(empty_chunk_result->ok());
      EXPECT_EQ((*empty_chunk_result)->size(), 0u);

      EXPECT_EQ(pw::async2::Pending(), pair->first().PendRead(cx));

      test_completed += 1;
      return pw::async2::Ready();
    }
  } test_task;

  dispatcher.Post(test_task);

  dispatcher.RunToCompletion();
  EXPECT_EQ(test_task.test_completed, 1);
}

TEST(ForwardingDatagramChannel, ForwardsNonEmptyDatagrams) {
  pw::async2::DispatcherForTest dispatcher;

  class : public pw::async2::Task {
   public:
    TestChannelPair<pw::channel::DataType::kDatagram> pair;

    int test_completed = 0;

   private:
    pw::async2::Poll<> DoPend(pw::async2::Context& cx) override {
      InitializedMultiBuf b1("Hello");
      InitializedMultiBuf b2("world!");

      // Send datagram first->second
      EXPECT_EQ(pw::async2::Ready(pw::OkStatus()),
                pair->first().PendReadyToWrite(cx));
      PW_TEST_EXPECT_OK(pair->first().StageWrite(b1.Take()));

      EXPECT_EQ(pw::async2::Pending(), pair->first().PendReadyToWrite(cx));

      EXPECT_EQ(CopyToString(pair->second().PendRead(cx).value().value()),
                "Hello");

      EXPECT_EQ(pw::async2::Ready(pw::OkStatus()),
                pair->first().PendReadyToWrite(cx));
      EXPECT_EQ(pw::async2::Pending(), pair->second().PendRead(cx));

      PW_TEST_EXPECT_OK(pair->first().StageWrite(b2.Take()));
      EXPECT_EQ(CopyToString(pair->second().PendRead(cx).value().value()),
                "world!");

      EXPECT_EQ(pw::async2::Pending(), pair->second().PendRead(cx));
      EXPECT_EQ(pw::async2::Ready(pw::OkStatus()),
                pair->first().PendReadyToWrite(cx));

      test_completed += 1;
      return pw::async2::Ready();
    }
  } test_task;

  dispatcher.Post(test_task);

  dispatcher.RunToCompletion();
  EXPECT_EQ(test_task.test_completed, 1);
}

TEST(ForwardingDatagramChannel, ForwardsDatagrams) {
  pw::async2::DispatcherForTest dispatcher;

  class : public pw::async2::Task {
   public:
    TestChannelPair<pw::channel::DataType::kDatagram> pair;

    int test_completed = 0;

   private:
    pw::async2::Poll<> DoPend(pw::async2::Context& cx) override {
      // No data yet
      EXPECT_EQ(pw::async2::Pending(), pair->first().PendRead(cx));
      EXPECT_EQ(pw::async2::Pending(), pair->second().PendRead(cx));

      // Send datagram first->second
      EXPECT_EQ(pw::async2::Ready(pw::OkStatus()),
                pair->first().PendReadyToWrite(cx));
      PW_TEST_EXPECT_OK(pair->first().StageWrite({}));  // Write empty datagram

      EXPECT_EQ(pw::async2::Pending(), pair->first().PendReadyToWrite(cx));
      EXPECT_EQ(pw::async2::Pending(), pair->first().PendRead(cx));

      auto empty_chunk_result = pair->second().PendRead(cx);
      EXPECT_TRUE(empty_chunk_result.IsReady());
      EXPECT_TRUE(empty_chunk_result->ok());
      EXPECT_EQ((*empty_chunk_result)->size(), 0u);

      EXPECT_EQ(pw::async2::Pending(), pair->second().PendRead(cx));

      // Send datagram second->first
      EXPECT_EQ(pw::async2::Ready(pw::OkStatus()),
                pair->second().PendReadyToWrite(cx));
      PW_TEST_EXPECT_OK(pair->second().StageWrite({}));  // Write empty datagram

      EXPECT_EQ(pw::async2::Pending(), pair->second().PendReadyToWrite(cx));
      EXPECT_EQ(pw::async2::Pending(), pair->second().PendRead(cx));

      empty_chunk_result = pair->first().PendRead(cx);
      EXPECT_TRUE(empty_chunk_result.IsReady());
      EXPECT_TRUE(empty_chunk_result->ok());
      EXPECT_EQ((*empty_chunk_result)->size(), 0u);

      EXPECT_EQ(pw::async2::Pending(), pair->first().PendRead(cx));

      test_completed += 1;
      return pw::async2::Ready();
    }
  } test_task;

  dispatcher.Post(test_task);

  dispatcher.RunToCompletion();
  EXPECT_EQ(test_task.test_completed, 1);
}

TEST(ForwardingDatagramchannel, PendCloseAwakensAndClosesPeer) {
  class TryToReadUntilClosed : public Task {
   public:
    TryToReadUntilClosed(DatagramReader& reader) : reader_(reader) {}

    int packets_read = 0;
    Waker waker;

   private:
    pw::async2::Poll<> DoPend(Context& cx) final {
      PollResult<MultiBuf> read = reader_.PendRead(cx);
      if (read.IsPending()) {
        PW_ASYNC_STORE_WAKER(
            cx, waker, "TryToReadUntilClosed is waiting for reader");
        return Pending();
      }

      if (read->ok()) {
        packets_read += 1;
        EXPECT_TRUE(read->value().empty());
        return Pending();
      }
      EXPECT_EQ(read->status(), pw::Status::FailedPrecondition());
      return Ready();
    }

    DatagramReader& reader_;
  };

  pw::async2::DispatcherForTest dispatcher;
  TestChannelPair<pw::channel::DataType::kDatagram> pair;
  TryToReadUntilClosed read_task(pair->first());
  dispatcher.Post(read_task);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  Waker empty_waker;
  Context empty_cx(dispatcher, empty_waker);

  // Write a datagram, but close before the datagram is read.
  EXPECT_EQ(pair->second().PendReadyToWrite(empty_cx), Ready(pw::OkStatus()));
  PW_TEST_EXPECT_OK(pair->second().StageWrite({}));
  EXPECT_EQ(pair->second().PendClose(empty_cx), Ready(pw::OkStatus()));
  EXPECT_FALSE(pair->second().is_read_or_write_open());

  // Closed second, so first is closed for writes, but still open for reads.
  EXPECT_TRUE(pair->first().is_read_open());
  EXPECT_FALSE(pair->first().is_write_open());

  // First should read the packet and immediately be marked closed.
  EXPECT_EQ(read_task.packets_read, 0);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(read_task.packets_read, 1);

  EXPECT_FALSE(pair->first().is_read_or_write_open());

  read_task.waker.Wake();        // wake the task so it runs again
  dispatcher.RunToCompletion();  // runs to completion

  EXPECT_FALSE(pair->first().is_read_or_write_open());
  EXPECT_EQ(read_task.packets_read, 1);
}

TEST(ForwardingByteChannel, IgnoresEmptyWrites) {
  pw::async2::DispatcherForTest dispatcher;

  class : public pw::async2::Task {
   public:
    TestChannelPair<pw::channel::DataType::kByte> pair;

    int test_completed = 0;

   private:
    pw::async2::Poll<> DoPend(pw::async2::Context& cx) override {
      // No data yet
      EXPECT_EQ(pw::async2::Pending(), pair->first().PendRead(cx));
      EXPECT_EQ(pw::async2::Pending(), pair->second().PendRead(cx));

      // Send nothing first->second
      EXPECT_EQ(pw::async2::Ready(pw::OkStatus()),
                pair->first().PendReadyToWrite(cx));
      PW_TEST_EXPECT_OK(pair->first().StageWrite({}));

      // Still no data
      EXPECT_EQ(pw::async2::Pending(), pair->first().PendRead(cx));
      EXPECT_EQ(pw::async2::Pending(), pair->second().PendRead(cx));

      // Send nothing second->first
      EXPECT_EQ(pw::async2::Ready(pw::OkStatus()),
                pair->first().PendReadyToWrite(cx));
      PW_TEST_EXPECT_OK(pair->first().StageWrite({}));

      // Still no data
      EXPECT_EQ(pw::async2::Pending(), pair->first().PendRead(cx));
      EXPECT_EQ(pw::async2::Pending(), pair->second().PendRead(cx));

      test_completed += 1;
      return pw::async2::Ready();
    }
  } test_task;

  dispatcher.Post(test_task);

  dispatcher.RunToCompletion();
  EXPECT_EQ(test_task.test_completed, 1);
}

TEST(ForwardingByteChannel, WriteData) {
  class ReadTask : public pw::async2::Task {
   public:
    ReadTask(pw::channel::ForwardingByteChannelPair& pair) : pair_(pair) {}

   private:
    pw::async2::Poll<> DoPend(pw::async2::Context& cx) override {
      EXPECT_EQ(pw::async2::Pending(), pair_.first().PendRead(cx));

      auto hello_world_result = pair_.second().PendRead(cx);
      if (hello_world_result.IsPending()) {
        return pw::async2::Pending();
      }

      EXPECT_EQ(CopyToString(hello_world_result->value()), "hello world");

      return pw::async2::Ready();
    }

    pw::channel::ForwardingByteChannelPair& pair_;
  };

  class WriteTask : public pw::async2::Task {
   public:
    WriteTask(pw::channel::ForwardingByteChannelPair& pair, MultiBuf&& data)
        : pair_(pair), data_(std::move(data)) {}

   private:
    pw::async2::Poll<> DoPend(pw::async2::Context& cx) override {
      EXPECT_EQ(pw::async2::Ready(pw::OkStatus()),
                pair_.first().PendReadyToWrite(cx));
      EXPECT_EQ(pw::OkStatus(), pair_.first().StageWrite(std::move(data_)));
      return pw::async2::Ready();
    }

    pw::channel::ForwardingByteChannelPair& pair_;
    MultiBuf data_;
  };

  InitializedMultiBuf data("hello world");

  TestChannelPair<pw::channel::DataType::kByte> pair;
  ReadTask read_task(*pair);
  WriteTask write_task(*pair, data.Take());

  pw::async2::DispatcherForTest dispatcher;

  dispatcher.Post(read_task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  dispatcher.Post(write_task);
  dispatcher.RunToCompletion();
}

TEST(ForwardingByteChannel, WriteDataInMultiplePieces) {
  pw::async2::DispatcherForTest dispatcher;

  class : public pw::async2::Task {
   public:
    TestChannelPair<pw::channel::DataType::kByte> pair;

    int test_completed = 0;

   private:
    pw::async2::Poll<> DoPend(pw::async2::Context& cx) override {
      // No data yet
      EXPECT_EQ(pw::async2::Pending(), pair->first().PendRead(cx));
      EXPECT_EQ(pw::async2::Pending(), pair->second().PendRead(cx));

      InitializedMultiBuf b1("hello");
      InitializedMultiBuf b2(" ");
      InitializedMultiBuf b3("world");

      // Send "hello world" first->second
      EXPECT_EQ(pw::async2::Ready(pw::OkStatus()),
                pair->first().PendReadyToWrite(cx));
      EXPECT_EQ(pw::OkStatus(), pair->first().StageWrite(b1.Take()));
      EXPECT_EQ(pw::async2::Ready(pw::OkStatus()),
                pair->first().PendReadyToWrite(cx));
      EXPECT_EQ(pw::OkStatus(), pair->first().StageWrite(b2.Take()));
      EXPECT_EQ(pw::async2::Ready(pw::OkStatus()),
                pair->first().PendReadyToWrite(cx));
      EXPECT_EQ(pw::OkStatus(), pair->first().StageWrite(b3.Take()));

      EXPECT_EQ(pw::async2::Pending(), pair->first().PendRead(cx));

      auto hello_world_result = pair->second().PendRead(cx);
      EXPECT_TRUE(hello_world_result.IsReady());

      EXPECT_EQ(CopyToString(hello_world_result->value()), "hello world");

      // Send nothing second->first
      EXPECT_EQ(pw::async2::Ready(pw::OkStatus()),
                pair->first().PendReadyToWrite(cx));
      EXPECT_EQ(pw::OkStatus(), pair->first().StageWrite({}));

      // Still no data
      EXPECT_EQ(pw::async2::Pending(), pair->first().PendRead(cx));
      EXPECT_EQ(pw::async2::Pending(), pair->second().PendRead(cx));

      test_completed += 1;
      return pw::async2::Ready();
    }
  } test_task;

  dispatcher.Post(test_task);

  dispatcher.RunToCompletion();
  EXPECT_EQ(test_task.test_completed, 1);
}

TEST(ForwardingByteChannel, PendCloseAwakensAndClosesPeer) {
  class TryToReadUntilClosed : public Task {
   public:
    TryToReadUntilClosed(ByteReader& reader) : reader_(reader) {}

    int bytes_read = 0;
    Waker waker;

   private:
    pw::async2::Poll<> DoPend(Context& cx) final {
      PollResult<MultiBuf> read = reader_.PendRead(cx);
      if (read.IsPending()) {
        PW_ASYNC_STORE_WAKER(
            cx, waker, "TryToReadUntilClosed is waiting for reader");
        return Pending();
      }

      if (read->ok()) {
        bytes_read += read->value().size();
        EXPECT_EQ(read->value().size(), 5u);
        return Pending();
      }

      EXPECT_EQ(read->status(), pw::Status::FailedPrecondition());
      return Ready();
    }
    ByteReader& reader_;
  };

  pw::async2::DispatcherForTest dispatcher;
  TestChannelPair<pw::channel::DataType::kByte> pair;
  TryToReadUntilClosed read_task(pair->first());
  dispatcher.Post(read_task);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  Waker empty_waker;
  Context empty_cx(dispatcher, empty_waker);

  InitializedMultiBuf data("hello");

  // Write a datagram, but close before the datagram is read.
  EXPECT_EQ(pair->second().PendReadyToWrite(empty_cx), Ready(pw::OkStatus()));
  EXPECT_EQ(pair->second().StageWrite(data.Take()), pw::OkStatus());
  EXPECT_EQ(pair->second().PendClose(empty_cx), Ready(pw::OkStatus()));
  EXPECT_FALSE(pair->second().is_read_or_write_open());

  // Closed second, so first is closed for writes, but still open for reads.
  EXPECT_TRUE(pair->first().is_read_open());
  EXPECT_FALSE(pair->first().is_write_open());

  // First should read the packet and immediately be marked closed.
  EXPECT_EQ(read_task.bytes_read, 0);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(read_task.bytes_read, 5);

  EXPECT_FALSE(pair->second().is_read_or_write_open());

  read_task.waker.Wake();        // wake the task so it runs again
  dispatcher.RunToCompletion();  // runs to completion

  EXPECT_FALSE(pair->first().is_read_or_write_open());
  EXPECT_EQ(read_task.bytes_read, 5);
}

}  // namespace
