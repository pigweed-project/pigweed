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

#include "pw_transfer/transfer.h"

#include <limits>

#include "pw_assert/check.h"
#include "pw_bytes/array.h"
#include "pw_containers/algorithm.h"
#include "pw_rpc/raw/test_method_context.h"
#include "pw_rpc/test_helpers.h"
#include "pw_thread/thread.h"
#include "pw_thread_stl/options.h"
#include "pw_transfer/internal/config.h"
#include "pw_transfer/transfer.pwpb.h"
#include "pw_transfer_private/chunk_testing.h"
#include "pw_unit_test/framework.h"

namespace pw::transfer::test {
namespace {

using namespace std::chrono_literals;

// TODO(frolv): Have a generic way to obtain a thread for testing on any system.
thread::Options& TransferThreadOptions() {
  static thread::stl::Options options;
  return options;
}

using internal::Chunk;

class TestMemoryReader : public stream::SeekableReader {
 public:
  constexpr TestMemoryReader(span<const std::byte> data)
      : memory_reader_(data) {}

  Status DoSeek(ptrdiff_t offset, Whence origin) override {
    if (seek_status.ok()) {
      return memory_reader_.Seek(offset, origin);
    }
    return seek_status;
  }

  StatusWithSize DoRead(ByteSpan dest) final {
    if (!read_status.ok()) {
      return StatusWithSize(read_status, 0);
    }

    auto result = memory_reader_.Read(dest);
    return result.ok() ? StatusWithSize(result->size())
                       : StatusWithSize(result.status(), 0);
  }

  Status seek_status;
  Status read_status;

 private:
  stream::MemoryReader memory_reader_;
};

class SimpleReadTransfer : public ReadOnlyHandler {
 public:
  SimpleReadTransfer(uint32_t session_id, ConstByteSpan data)
      : ReadOnlyHandler(session_id),
        prepare_read_called(false),
        finalize_read_called(false),
        finalize_read_status(Status::Unknown()),
        resource_size_(std::numeric_limits<size_t>::max()),
        reader_(data) {}

  Status PrepareRead() final {
    prepare_read_called = true;

    if (!prepare_read_return_status.ok()) {
      return prepare_read_return_status;
    }

    EXPECT_EQ(reader_.seek_status, reader_.Seek(0));
    set_reader(reader_);
    return OkStatus();
  }

  void FinalizeRead(Status status) final {
    finalize_read_called = true;
    finalize_read_status = status;
  }

  size_t ResourceSize() const final { return resource_size_; }

  void set_resource_size(size_t resource_size) {
    resource_size_ = resource_size;
  }
  void set_seek_status(Status status) { reader_.seek_status = status; }
  void set_read_status(Status status) { reader_.read_status = status; }

  bool prepare_read_called;
  bool finalize_read_called;
  Status prepare_read_return_status;
  Status finalize_read_status;
  size_t resource_size_;

 private:
  TestMemoryReader reader_;
};

constexpr auto kData = bytes::Initialized<32>([](size_t i) { return i; });
constexpr uint32_t kArbitrarySessionId = 123;

class ReadTransfer : public ::testing::Test {
 protected:
  ReadTransfer(size_t max_chunk_size_bytes = 64)
      : handler_(3, kData),
        transfer_thread_(span(data_buffer_).first(max_chunk_size_bytes),
                         encode_buffer_),
        ctx_(transfer_thread_,
             64,
             // Use a long timeout to avoid accidentally triggering timeouts.
             std::chrono::minutes(1)),
        system_thread_(TransferThreadOptions(), transfer_thread_) {
    ctx_.service().RegisterHandler(handler_);

    PW_CHECK(!handler_.prepare_read_called);
    PW_CHECK(!handler_.finalize_read_called);

    ctx_.call();  // Open the read stream
    transfer_thread_.WaitUntilEventIsProcessed();
  }

  ~ReadTransfer() override {
    transfer_thread_.Terminate();
    system_thread_.join();
  }

  SimpleReadTransfer handler_;
  Thread<1, 1> transfer_thread_;
  PW_RAW_TEST_METHOD_CONTEXT(TransferService, Read, 8) ctx_;
  pw::Thread system_thread_;
  std::array<std::byte, 64> data_buffer_;
  std::array<std::byte, 64> encode_buffer_;
};

TEST_F(ReadTransfer, SingleChunk) {
  rpc::test::WaitForPackets(ctx_.output(), 2, [this] {
    ctx_.SendClientStream(
        EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart)
                        .set_session_id(3)
                        .set_window_end_offset(64)
                        .set_offset(0)));

    transfer_thread_.WaitUntilEventIsProcessed();
  });

  EXPECT_TRUE(handler_.prepare_read_called);
  EXPECT_FALSE(handler_.finalize_read_called);

  ASSERT_EQ(ctx_.total_responses(), 2u);
  Chunk c0 = DecodeChunk(ctx_.responses()[0]);
  Chunk c1 = DecodeChunk(ctx_.responses()[1]);

  // First chunk should have all the read data.
  EXPECT_EQ(c0.session_id(), 3u);
  EXPECT_EQ(c0.offset(), 0u);
  ASSERT_EQ(c0.payload().size(), kData.size());
  EXPECT_EQ(std::memcmp(c0.payload().data(), kData.data(), c0.payload().size()),
            0);

  // Second chunk should be empty and set remaining_bytes = 0.
  EXPECT_EQ(c1.session_id(), 3u);
  EXPECT_FALSE(c1.has_payload());
  ASSERT_TRUE(c1.remaining_bytes().has_value());
  EXPECT_EQ(c1.remaining_bytes().value(), 0u);

  ctx_.SendClientStream(
      EncodeChunk(Chunk::Final(ProtocolVersion::kLegacy, 3, OkStatus())));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.finalize_read_called);
  EXPECT_EQ(handler_.finalize_read_status, OkStatus());
}

TEST_F(ReadTransfer, MultiChunk) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart)
                      .set_session_id(3)
                      .set_window_end_offset(16)
                      .set_offset(0)));

  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_read_called);
  EXPECT_FALSE(handler_.finalize_read_called);

  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk c0 = DecodeChunk(ctx_.responses().back());

  EXPECT_EQ(c0.session_id(), 3u);
  EXPECT_EQ(c0.offset(), 0u);
  ASSERT_EQ(c0.payload().size(), 16u);
  EXPECT_EQ(std::memcmp(c0.payload().data(), kData.data(), c0.payload().size()),
            0);

  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kParametersContinue)
          .set_session_id(3)
          .set_window_end_offset(32)
          .set_offset(16)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 2u);
  Chunk c1 = DecodeChunk(ctx_.responses().back());

  EXPECT_EQ(c1.session_id(), 3u);
  EXPECT_EQ(c1.offset(), 16u);
  ASSERT_EQ(c1.payload().size(), 16u);
  EXPECT_EQ(
      std::memcmp(c1.payload().data(), kData.data() + 16, c1.payload().size()),
      0);

  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kParametersContinue)
          .set_session_id(3)
          .set_window_end_offset(48)
          .set_offset(32)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 3u);
  Chunk c2 = DecodeChunk(ctx_.responses()[2]);

  EXPECT_EQ(c2.session_id(), 3u);
  EXPECT_FALSE(c2.has_payload());
  ASSERT_TRUE(c2.remaining_bytes().has_value());
  EXPECT_EQ(c2.remaining_bytes().value(), 0u);

  ctx_.SendClientStream(
      EncodeChunk(Chunk::Final(ProtocolVersion::kLegacy, 3, OkStatus())));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.finalize_read_called);
  EXPECT_EQ(handler_.finalize_read_status, OkStatus());
}

TEST_F(ReadTransfer, MultiChunk_RepeatedContinuePackets) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart)
                      .set_session_id(3)
                      .set_window_end_offset(16)
                      .set_offset(0)));

  transfer_thread_.WaitUntilEventIsProcessed();

  const auto continue_chunk = EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kParametersContinue)
          .set_session_id(3)
          .set_window_end_offset(24)
          .set_offset(16));
  ctx_.SendClientStream(continue_chunk);

  transfer_thread_.WaitUntilEventIsProcessed();

  // Resend the CONTINUE packets that don't actually advance the window.
  for (int i = 0; i < 3; ++i) {
    ctx_.SendClientStream(continue_chunk);
    transfer_thread_.WaitUntilEventIsProcessed();
  }

  ASSERT_EQ(ctx_.total_responses(), 2u);  // Only sent one packet
  Chunk c1 = DecodeChunk(ctx_.responses()[1]);

  EXPECT_EQ(c1.session_id(), 3u);
  EXPECT_EQ(c1.offset(), 16u);
  ASSERT_EQ(c1.payload().size(), 8u);
  EXPECT_EQ(
      std::memcmp(c1.payload().data(), kData.data() + 16, c1.payload().size()),
      0);
}

TEST_F(ReadTransfer, OutOfOrder_SeekingSupported) {
  rpc::test::WaitForPackets(ctx_.output(), 4, [this] {
    ctx_.SendClientStream(
        EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart)
                        .set_session_id(3)
                        .set_window_end_offset(16)
                        .set_offset(0)));

    transfer_thread_.WaitUntilEventIsProcessed();

    Chunk chunk = DecodeChunk(ctx_.responses().back());
    EXPECT_TRUE(pw::containers::Equal(span(kData).first(16), chunk.payload()));

    ctx_.SendClientStream(EncodeChunk(
        Chunk(ProtocolVersion::kLegacy, Chunk::Type::kParametersRetransmit)
            .set_session_id(3)
            .set_window_end_offset(10)
            .set_offset(2)));

    transfer_thread_.WaitUntilEventIsProcessed();

    chunk = DecodeChunk(ctx_.responses().back());
    EXPECT_TRUE(
        pw::containers::Equal(span(kData).subspan(2, 8), chunk.payload()));

    ctx_.SendClientStream(EncodeChunk(
        Chunk(ProtocolVersion::kLegacy, Chunk::Type::kParametersRetransmit)
            .set_session_id(3)
            .set_window_end_offset(64)
            .set_offset(17)));
  });

  ASSERT_EQ(ctx_.total_responses(), 4u);
  Chunk chunk = DecodeChunk(ctx_.responses()[2]);
  EXPECT_TRUE(pw::containers::Equal(
      span(&kData[17], kData.data() + kData.size()), chunk.payload()));
}

TEST_F(ReadTransfer, OutOfOrder_SeekingNotSupported_EndsWithUnimplemented) {
  handler_.set_seek_status(Status::Unimplemented());

  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart)
                      .set_session_id(3)
                      .set_window_end_offset(16)
                      .set_offset(0)));
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kParametersRetransmit)
          .set_session_id(3)
          .set_window_end_offset(10)
          .set_offset(2)));

  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 2u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), Status::Unimplemented());
}

TEST_F(ReadTransfer, MaxChunkSize_Client) {
  rpc::test::WaitForPackets(ctx_.output(), 5, [this] {
    ctx_.SendClientStream(
        EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart)
                        .set_session_id(3)
                        .set_window_end_offset(64)
                        .set_max_chunk_size_bytes(8)
                        .set_offset(0)));
  });

  EXPECT_TRUE(handler_.prepare_read_called);
  EXPECT_FALSE(handler_.finalize_read_called);

  ASSERT_EQ(ctx_.total_responses(), 5u);
  Chunk c0 = DecodeChunk(ctx_.responses()[0]);
  Chunk c1 = DecodeChunk(ctx_.responses()[1]);
  Chunk c2 = DecodeChunk(ctx_.responses()[2]);
  Chunk c3 = DecodeChunk(ctx_.responses()[3]);
  Chunk c4 = DecodeChunk(ctx_.responses()[4]);

  EXPECT_EQ(c0.session_id(), 3u);
  EXPECT_EQ(c0.offset(), 0u);
  ASSERT_EQ(c0.payload().size(), 8u);
  EXPECT_EQ(std::memcmp(c0.payload().data(), kData.data(), c0.payload().size()),
            0);

  EXPECT_EQ(c1.session_id(), 3u);
  EXPECT_EQ(c1.offset(), 8u);
  ASSERT_EQ(c1.payload().size(), 8u);
  EXPECT_EQ(
      std::memcmp(c1.payload().data(), kData.data() + 8, c1.payload().size()),
      0);

  EXPECT_EQ(c2.session_id(), 3u);
  EXPECT_EQ(c2.offset(), 16u);
  ASSERT_EQ(c2.payload().size(), 8u);
  EXPECT_EQ(
      std::memcmp(c2.payload().data(), kData.data() + 16, c2.payload().size()),
      0);

  EXPECT_EQ(c3.session_id(), 3u);
  EXPECT_EQ(c3.offset(), 24u);
  ASSERT_EQ(c3.payload().size(), 8u);
  EXPECT_EQ(
      std::memcmp(c3.payload().data(), kData.data() + 24, c3.payload().size()),
      0);

  EXPECT_EQ(c4.session_id(), 3u);
  EXPECT_EQ(c4.payload().size(), 0u);
  ASSERT_TRUE(c4.remaining_bytes().has_value());
  EXPECT_EQ(c4.remaining_bytes().value(), 0u);

  ctx_.SendClientStream(
      EncodeChunk(Chunk::Final(ProtocolVersion::kLegacy, 3, OkStatus())));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.finalize_read_called);
  EXPECT_EQ(handler_.finalize_read_status, OkStatus());
}

TEST_F(ReadTransfer, HandlerIsClearedAfterTransfer) {
  // Request an end offset smaller than the data size to prevent the server
  // from sending a final chunk.
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart)
                      .set_session_id(3)
                      .set_window_end_offset(16)
                      .set_offset(0)));
  ctx_.SendClientStream(
      EncodeChunk(Chunk::Final(ProtocolVersion::kLegacy, 3, OkStatus())));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 1u);
  ASSERT_TRUE(handler_.prepare_read_called);
  ASSERT_TRUE(handler_.finalize_read_called);
  ASSERT_EQ(OkStatus(), handler_.finalize_read_status);

  // Now, clear state and start a second transfer
  handler_.prepare_read_return_status = Status::FailedPrecondition();
  handler_.prepare_read_called = false;
  handler_.finalize_read_called = false;

  // Request an end offset smaller than the data size to prevent the server
  // from sending a final chunk.
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart)
                      .set_session_id(3)
                      .set_window_end_offset(16)
                      .set_offset(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  // Prepare failed, so the handler should not have been stored in the context,
  // and finalize should not have been called.
  ASSERT_TRUE(handler_.prepare_read_called);
  ASSERT_FALSE(handler_.finalize_read_called);
}

class ReadTransferMaxChunkSize8 : public ReadTransfer {
 protected:
  ReadTransferMaxChunkSize8() : ReadTransfer(/*max_chunk_size_bytes=*/8) {}
};

TEST_F(ReadTransferMaxChunkSize8, MaxChunkSize_Server) {
  // Client asks for max 16-byte chunks, but service places a limit of 8 bytes.
  // TODO(frolv): Fix
  rpc::test::WaitForPackets(ctx_.output(), 5, [this] {
    ctx_.SendClientStream(
        EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart)
                        .set_session_id(3)
                        .set_window_end_offset(64)
                        // .set_max_chunk_size_bytes(16)
                        .set_offset(0)));
  });

  EXPECT_TRUE(handler_.prepare_read_called);
  EXPECT_FALSE(handler_.finalize_read_called);

  ASSERT_EQ(ctx_.total_responses(), 5u);
  Chunk c0 = DecodeChunk(ctx_.responses()[0]);
  Chunk c1 = DecodeChunk(ctx_.responses()[1]);
  Chunk c2 = DecodeChunk(ctx_.responses()[2]);
  Chunk c3 = DecodeChunk(ctx_.responses()[3]);
  Chunk c4 = DecodeChunk(ctx_.responses()[4]);

  EXPECT_EQ(c0.session_id(), 3u);
  EXPECT_EQ(c0.offset(), 0u);
  ASSERT_EQ(c0.payload().size(), 8u);
  EXPECT_EQ(std::memcmp(c0.payload().data(), kData.data(), c0.payload().size()),
            0);

  EXPECT_EQ(c1.session_id(), 3u);
  EXPECT_EQ(c1.offset(), 8u);
  ASSERT_EQ(c1.payload().size(), 8u);
  EXPECT_EQ(
      std::memcmp(c1.payload().data(), kData.data() + 8, c1.payload().size()),
      0);

  EXPECT_EQ(c2.session_id(), 3u);
  EXPECT_EQ(c2.offset(), 16u);
  ASSERT_EQ(c2.payload().size(), 8u);
  EXPECT_EQ(
      std::memcmp(c2.payload().data(), kData.data() + 16, c2.payload().size()),
      0);

  EXPECT_EQ(c3.session_id(), 3u);
  EXPECT_EQ(c3.offset(), 24u);
  ASSERT_EQ(c3.payload().size(), 8u);
  EXPECT_EQ(
      std::memcmp(c3.payload().data(), kData.data() + 24, c3.payload().size()),
      0);

  EXPECT_EQ(c4.session_id(), 3u);
  EXPECT_EQ(c4.payload().size(), 0u);
  ASSERT_TRUE(c4.remaining_bytes().has_value());
  EXPECT_EQ(c4.remaining_bytes().value(), 0u);

  ctx_.SendClientStream(
      EncodeChunk(Chunk::Final(ProtocolVersion::kLegacy, 3, OkStatus())));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.finalize_read_called);
  EXPECT_EQ(handler_.finalize_read_status, OkStatus());
}

TEST_F(ReadTransfer, ClientError) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart)
                      .set_session_id(3)
                      .set_window_end_offset(16)
                      .set_offset(0)));

  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_read_called);
  EXPECT_FALSE(handler_.finalize_read_called);
  ASSERT_EQ(ctx_.total_responses(), 1u);

  // Send client error.
  ctx_.SendClientStream(EncodeChunk(
      Chunk::Final(ProtocolVersion::kLegacy, 3, Status::OutOfRange())));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 1u);
  EXPECT_TRUE(handler_.finalize_read_called);
  EXPECT_EQ(handler_.finalize_read_status, Status::OutOfRange());
}

TEST_F(ReadTransfer, UnregisteredHandler) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart)
                      .set_session_id(11)
                      .set_window_end_offset(32)
                      .set_offset(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 11u);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), Status::NotFound());
}

TEST_F(ReadTransfer, IgnoresNonPendingTransfers) {
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kParametersRetransmit)
          .set_session_id(3)
          .set_window_end_offset(32)
          .set_offset(3)));
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(3)
                      .set_payload(span(kData).first(10))
                      .set_offset(3)));
  ctx_.SendClientStream(
      EncodeChunk(Chunk::Final(ProtocolVersion::kLegacy, 3, OkStatus())));
  transfer_thread_.WaitUntilEventIsProcessed();

  // Only start transfer for an initial packet.
  EXPECT_FALSE(handler_.prepare_read_called);
  EXPECT_FALSE(handler_.finalize_read_called);
}

TEST_F(ReadTransfer, AbortAndRestartIfInitialPacketIsReceived) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart)
                      .set_session_id(3)
                      .set_window_end_offset(16)
                      .set_offset(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 1u);

  EXPECT_TRUE(handler_.prepare_read_called);
  EXPECT_FALSE(handler_.finalize_read_called);
  handler_.prepare_read_called = false;  // Reset so can check if called again.

  ctx_.SendClientStream(  // Resend starting chunk
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart)
                      .set_session_id(3)
                      .set_window_end_offset(16)
                      .set_offset(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 2u);

  EXPECT_TRUE(handler_.prepare_read_called);
  EXPECT_TRUE(handler_.finalize_read_called);
  EXPECT_EQ(handler_.finalize_read_status, Status::Aborted());
  handler_.finalize_read_called = false;  // Reset so can check later

  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kParametersRetransmit)
          .set_session_id(3)
          .set_window_end_offset(32)
          .set_offset(16)));
  ctx_.SendClientStream(
      EncodeChunk(Chunk::Final(ProtocolVersion::kLegacy, 3, OkStatus())));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 3u);
  EXPECT_TRUE(handler_.finalize_read_called);
  EXPECT_EQ(handler_.finalize_read_status, OkStatus());
}

TEST_F(ReadTransfer, ZeroPendingBytesWithRemainingData_Aborts) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart)
                      .set_session_id(3)
                      .set_window_end_offset(0)
                      .set_offset(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 1u);
  ASSERT_TRUE(handler_.finalize_read_called);
  EXPECT_EQ(handler_.finalize_read_status, Status::ResourceExhausted());

  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.status(), Status::ResourceExhausted());
}

TEST_F(ReadTransfer, ZeroPendingBytesNoRemainingData_Completes) {
  // Make the next read appear to be the end of the stream.
  handler_.set_read_status(Status::OutOfRange());

  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart)
                      .set_session_id(3)
                      .set_window_end_offset(0)
                      .set_offset(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 3u);
  EXPECT_EQ(chunk.remaining_bytes(), 0u);

  ctx_.SendClientStream(
      EncodeChunk(Chunk::Final(ProtocolVersion::kLegacy, 3, OkStatus())));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 1u);
  ASSERT_TRUE(handler_.finalize_read_called);
  EXPECT_EQ(handler_.finalize_read_status, OkStatus());
}

TEST_F(ReadTransfer, SendsErrorIfChunkIsReceivedInCompletedState) {
  rpc::test::WaitForPackets(ctx_.output(), 2, [this] {
    ctx_.SendClientStream(
        EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart)
                        .set_session_id(3)
                        .set_window_end_offset(64)
                        .set_offset(0)));
  });

  EXPECT_TRUE(handler_.prepare_read_called);
  EXPECT_FALSE(handler_.finalize_read_called);

  ASSERT_EQ(ctx_.total_responses(), 2u);
  Chunk c0 = DecodeChunk(ctx_.responses()[0]);
  Chunk c1 = DecodeChunk(ctx_.responses()[1]);

  // First chunk should have all the read data.
  EXPECT_EQ(c0.session_id(), 3u);
  EXPECT_EQ(c0.offset(), 0u);
  ASSERT_EQ(c0.payload().size(), kData.size());
  EXPECT_EQ(std::memcmp(c0.payload().data(), kData.data(), c0.payload().size()),
            0);

  // Second chunk should be empty and set remaining_bytes = 0.
  EXPECT_EQ(c1.session_id(), 3u);
  EXPECT_FALSE(c1.has_payload());
  ASSERT_TRUE(c1.remaining_bytes().has_value());
  EXPECT_EQ(c1.remaining_bytes().value(), 0u);

  ctx_.SendClientStream(
      EncodeChunk(Chunk::Final(ProtocolVersion::kLegacy, 3, OkStatus())));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.finalize_read_called);
  EXPECT_EQ(handler_.finalize_read_status, OkStatus());

  // At this point the transfer should be in a completed state. Send a
  // non-initial chunk as a continuation of the transfer.
  handler_.finalize_read_called = false;

  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kParametersRetransmit)
          .set_session_id(3)
          .set_window_end_offset(64)
          .set_offset(16)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 3u);

  Chunk c2 = DecodeChunk(ctx_.responses()[2]);
  ASSERT_TRUE(c2.status().has_value());
  EXPECT_EQ(c2.status().value(), Status::FailedPrecondition());

  // FinalizeRead should not be called again.
  EXPECT_FALSE(handler_.finalize_read_called);
}

class SimpleWriteTransfer final : public WriteOnlyHandler {
 public:
  SimpleWriteTransfer(uint32_t session_id, ByteSpan data)
      : WriteOnlyHandler(session_id),
        prepare_write_called(false),
        finalize_write_called(false),
        finalize_write_status(Status::Unknown()),
        writer_(data) {}

  Status PrepareWrite() final {
    EXPECT_EQ(OkStatus(), writer_.Seek(0));
    set_writer(writer_);
    prepare_write_called = true;
    return OkStatus();
  }

  Status FinalizeWrite(Status status) final {
    finalize_write_called = true;
    finalize_write_status = status;
    return finalize_write_return_status_;
  }

  void set_finalize_write_return(Status status) {
    finalize_write_return_status_ = status;
  }

  bool prepare_write_called;
  bool finalize_write_called;
  Status finalize_write_status;

 private:
  Status finalize_write_return_status_;
  stream::MemoryWriter writer_;
};

class WriteTransfer : public ::testing::Test {
 protected:
  WriteTransfer(size_t max_bytes_to_receive = 64)
      : buffer{},
        handler_(7, buffer),
        transfer_thread_(data_buffer_, encode_buffer_),
        system_thread_(TransferThreadOptions(), transfer_thread_),
        ctx_(transfer_thread_,
             max_bytes_to_receive,
             // Use a long timeout to avoid accidentally triggering timeouts.
             std::chrono::minutes(1),
             /*max_retries=*/3) {
    ctx_.service().RegisterHandler(handler_);

    PW_CHECK(!handler_.prepare_write_called);
    PW_CHECK(!handler_.finalize_write_called);

    ctx_.call();  // Open the write stream
    transfer_thread_.WaitUntilEventIsProcessed();
  }

  ~WriteTransfer() override {
    transfer_thread_.Terminate();
    system_thread_.join();
  }

  std::array<std::byte, kData.size()> buffer;
  SimpleWriteTransfer handler_;

  Thread<1, 1> transfer_thread_;
  pw::Thread system_thread_;
  std::array<std::byte, 64> data_buffer_;
  std::array<std::byte, 64> encode_buffer_;
  PW_RAW_TEST_METHOD_CONTEXT(TransferService, Write) ctx_;
};

TEST_F(WriteTransfer, SingleChunk) {
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart).set_session_id(7)));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_write_called);
  EXPECT_FALSE(handler_.finalize_write_called);

  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  EXPECT_EQ(chunk.window_end_offset(), 32u);
  ASSERT_TRUE(chunk.max_chunk_size_bytes().has_value());
  EXPECT_EQ(chunk.max_chunk_size_bytes().value(), 37u);

  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(0)
                      .set_payload(kData)
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 2u);
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), OkStatus());

  EXPECT_TRUE(handler_.finalize_write_called);
  EXPECT_EQ(handler_.finalize_write_status, OkStatus());
  EXPECT_EQ(std::memcmp(buffer.data(), kData.data(), kData.size()), 0);
}

TEST_F(WriteTransfer, FinalizeFails) {
  // Return an error when FinalizeWrite is called.
  handler_.set_finalize_write_return(Status::FailedPrecondition());

  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart).set_session_id(7)));
  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(0)
                      .set_payload(kData)
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 2u);
  Chunk chunk = DecodeChunk(ctx_.responses()[1]);
  EXPECT_EQ(chunk.session_id(), 7u);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), Status::DataLoss());

  EXPECT_TRUE(handler_.finalize_write_called);
  EXPECT_EQ(handler_.finalize_write_status, OkStatus());
}

TEST_F(WriteTransfer, SendingFinalPacketFails) {
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart).set_session_id(7)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ctx_.output().set_send_status(Status::Unknown());

  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(0)
                      .set_payload(kData)
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  // Should only have sent the transfer parameters.
  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses()[0]);
  EXPECT_EQ(chunk.session_id(), 7u);
  EXPECT_EQ(chunk.window_end_offset(), 32u);
  ASSERT_TRUE(chunk.max_chunk_size_bytes().has_value());
  EXPECT_EQ(chunk.max_chunk_size_bytes().value(), 37u);

  // When FinalizeWrite() was called, the transfer was considered successful.
  EXPECT_TRUE(handler_.finalize_write_called);
  EXPECT_EQ(handler_.finalize_write_status, OkStatus());
}

TEST_F(WriteTransfer, MultiChunk) {
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart).set_session_id(7)));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_write_called);
  EXPECT_FALSE(handler_.finalize_write_called);

  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses()[0]);
  EXPECT_EQ(chunk.session_id(), 7u);
  EXPECT_EQ(chunk.window_end_offset(), 32u);

  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(0)
                      .set_payload(span(kData).first(8))));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 1u);

  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(8)
                      .set_payload(span(kData).subspan(8))
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 2u);
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), OkStatus());

  EXPECT_TRUE(handler_.finalize_write_called);
  EXPECT_EQ(handler_.finalize_write_status, OkStatus());
  EXPECT_EQ(std::memcmp(buffer.data(), kData.data(), kData.size()), 0);
}

TEST_F(WriteTransfer, WriteFailsOnRetry) {
  // Skip one packet to fail on a retry.
  ctx_.output().set_send_status(Status::FailedPrecondition(), 1);

  // Wait for 3 packets: initial params, retry attempt, final error
  rpc::test::WaitForPackets(ctx_.output(), 3, [this] {
    // Send only one client packet so the service times out.
    ctx_.SendClientStream(
        EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart)
                        .set_session_id(7)));
    transfer_thread_.SimulateServerTimeout(7);  // Time out to trigger retry
  });

  // Attempted to send 3 packets, but the 2nd packet was dropped.
  // Check that the last packet is an INTERNAL error from the RPC write failure.
  ASSERT_EQ(ctx_.total_responses(), 2u);
  Chunk chunk = DecodeChunk(ctx_.responses()[1]);
  EXPECT_EQ(chunk.session_id(), 7u);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), Status::Internal());
}

TEST_F(WriteTransfer, TimeoutInRecoveryState) {
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart).set_session_id(7)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), 32u);

  constexpr span data(kData);

  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(0)
                      .set_payload(data.first(8))));

  // Skip offset 8 to enter a recovery state.
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(12)
                      .set_payload(data.subspan(12, 4))));
  transfer_thread_.WaitUntilEventIsProcessed();

  // Recovery parameters should be sent for offset 8.
  ASSERT_EQ(ctx_.total_responses(), 2u);
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  EXPECT_EQ(chunk.offset(), 8u);
  EXPECT_EQ(chunk.window_end_offset(), 32u);

  // Timeout while in the recovery state.
  transfer_thread_.SimulateServerTimeout(7);
  transfer_thread_.WaitUntilEventIsProcessed();

  // Same recovery parameters should be re-sent.
  ASSERT_EQ(ctx_.total_responses(), 3u);
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  EXPECT_EQ(chunk.offset(), 8u);
  EXPECT_EQ(chunk.window_end_offset(), 32u);
}

TEST_F(WriteTransfer, ExtendWindow) {
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart).set_session_id(7)));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_write_called);
  EXPECT_FALSE(handler_.finalize_write_called);

  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  EXPECT_EQ(chunk.window_end_offset(), 32u);

  // Window starts at 32 bytes and should extend when half of that is sent.
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(0)
                      .set_payload(span(kData).first(4))));
  transfer_thread_.WaitUntilEventIsProcessed();
  ASSERT_EQ(ctx_.total_responses(), 1u);

  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(4)
                      .set_payload(span(kData).subspan(4, 4))));
  transfer_thread_.WaitUntilEventIsProcessed();
  ASSERT_EQ(ctx_.total_responses(), 1u);

  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(8)
                      .set_payload(span(kData).subspan(8, 4))));
  transfer_thread_.WaitUntilEventIsProcessed();
  ASSERT_EQ(ctx_.total_responses(), 1u);

  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(12)
                      .set_payload(span(kData).subspan(12, 4))));
  transfer_thread_.WaitUntilEventIsProcessed();
  ASSERT_EQ(ctx_.total_responses(), 2u);

  // Extend parameters chunk.
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  EXPECT_EQ(chunk.window_end_offset(), 32u);
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersContinue);

  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(16)
                      .set_payload(span(kData).subspan(16))
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 3u);
  chunk = DecodeChunk(ctx_.responses()[2]);
  EXPECT_EQ(chunk.session_id(), 7u);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), OkStatus());

  EXPECT_TRUE(handler_.finalize_write_called);
  EXPECT_EQ(handler_.finalize_write_status, OkStatus());
  EXPECT_EQ(std::memcmp(buffer.data(), kData.data(), kData.size()), 0);
}

class WriteTransferMaxBytes16 : public WriteTransfer {
 protected:
  WriteTransferMaxBytes16() : WriteTransfer(/*max_bytes_to_receive=*/16) {}
};

TEST_F(WriteTransfer, TransmitterReducesWindow) {
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart).set_session_id(7)));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_write_called);
  EXPECT_FALSE(handler_.finalize_write_called);

  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  EXPECT_EQ(chunk.window_end_offset(), 32u);

  // Send only 12 bytes and set that as the new end offset.
  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(0)
                      .set_window_end_offset(12)
                      .set_payload(span(kData).first(12))));
  transfer_thread_.WaitUntilEventIsProcessed();
  ASSERT_EQ(ctx_.total_responses(), 2u);

  // Receiver should respond immediately with a continue chunk as the end of
  // the window has been reached.
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  EXPECT_EQ(chunk.offset(), 12u);
  EXPECT_EQ(chunk.window_end_offset(), 32u);
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersContinue);
}

TEST_F(WriteTransfer, TransmitterExtendsWindow_TerminatesWithInvalid) {
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart).set_session_id(7)));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_write_called);
  EXPECT_FALSE(handler_.finalize_write_called);

  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  EXPECT_EQ(chunk.window_end_offset(), 32u);

  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(0)
                      // Larger window end offset than the receiver's.
                      .set_window_end_offset(48)
                      .set_payload(span(kData).first(16))));
  transfer_thread_.WaitUntilEventIsProcessed();
  ASSERT_EQ(ctx_.total_responses(), 2u);

  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), Status::Internal());
}

TEST_F(WriteTransferMaxBytes16, MultipleParameters) {
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart).set_session_id(7)));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_write_called);
  EXPECT_FALSE(handler_.finalize_write_called);

  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  EXPECT_EQ(chunk.window_end_offset(), 16u);

  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(0)
                      .set_payload(span(kData).first(8))));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 2u);
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  EXPECT_EQ(chunk.offset(), 8u);
  EXPECT_EQ(chunk.window_end_offset(), 24u);

  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(8)
                      .set_payload(span(kData).subspan(8, 8))));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 3u);
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  EXPECT_EQ(chunk.offset(), 16u);
  EXPECT_EQ(chunk.window_end_offset(), 32u);

  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(16)
                      .set_payload(span(kData).subspan(16, 8))));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 4u);
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  EXPECT_EQ(chunk.offset(), 24u);
  EXPECT_EQ(chunk.window_end_offset(), 32u);

  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(24)
                      .set_payload(span(kData).subspan(24))
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 5u);
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), OkStatus());

  EXPECT_TRUE(handler_.finalize_write_called);
  EXPECT_EQ(handler_.finalize_write_status, OkStatus());
  EXPECT_EQ(std::memcmp(buffer.data(), kData.data(), kData.size()), 0);
}

TEST_F(WriteTransferMaxBytes16, SetsDefaultWindowEndOffset) {
  // Default max bytes is smaller than buffer.
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart).set_session_id(7)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  EXPECT_EQ(chunk.window_end_offset(), 16u);
}

TEST_F(WriteTransfer, SetsWriterWindowEndOffset) {
  // Buffer is smaller than constructor's default max bytes.
  std::array<std::byte, 8> small_buffer = {};

  SimpleWriteTransfer handler_(987, small_buffer);
  ctx_.service().RegisterHandler(handler_);

  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart)
                      .set_session_id(987)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 987u);
  EXPECT_EQ(chunk.window_end_offset(), 8u);

  ctx_.service().UnregisterHandler(handler_);
}

TEST_F(WriteTransfer, UnexpectedOffset) {
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart).set_session_id(7)));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_write_called);
  EXPECT_FALSE(handler_.finalize_write_called);

  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), 32u);

  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(0)
                      .set_payload(span(kData).first(8))));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 1u);

  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(4)  // incorrect
                      .set_payload(span(kData).subspan(8))
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 2u);
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  EXPECT_EQ(chunk.offset(), 8u);
  EXPECT_EQ(chunk.window_end_offset(), 32u);

  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(8)  // correct
                      .set_payload(span(kData).subspan(8))
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 3u);
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), OkStatus());

  EXPECT_TRUE(handler_.finalize_write_called);
  EXPECT_EQ(handler_.finalize_write_status, OkStatus());
  EXPECT_EQ(std::memcmp(buffer.data(), kData.data(), kData.size()), 0);
}

TEST_F(WriteTransferMaxBytes16, TooMuchData_EntersRecovery) {
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart).set_session_id(7)));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_write_called);
  EXPECT_FALSE(handler_.finalize_write_called);

  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  EXPECT_EQ(chunk.window_end_offset(), 16u);

  // window_end_offset = 16, but send 24 bytes of data.
  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(0)
                      .set_payload(span(kData).first(24))));
  transfer_thread_.WaitUntilEventIsProcessed();

  // Transfer should resend a parameters chunk.
  ASSERT_EQ(ctx_.total_responses(), 2u);
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersRetransmit);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), 16u);
}

TEST_F(WriteTransfer, UnregisteredHandler) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart)
                      .set_session_id(999)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 999u);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), Status::NotFound());
}

TEST_F(WriteTransfer, ClientError) {
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart).set_session_id(7)));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_write_called);
  EXPECT_FALSE(handler_.finalize_write_called);

  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  EXPECT_EQ(chunk.window_end_offset(), 32u);

  ctx_.SendClientStream<64>(EncodeChunk(
      Chunk::Final(ProtocolVersion::kLegacy, 7, Status::DataLoss())));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_EQ(ctx_.total_responses(), 1u);

  EXPECT_TRUE(handler_.finalize_write_called);
  EXPECT_EQ(handler_.finalize_write_status, Status::DataLoss());
}

TEST_F(WriteTransfer, OnlySendParametersUpdateOnceAfterDrop) {
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart).set_session_id(7)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 1u);

  constexpr span data(kData);
  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(0)
                      .set_payload(data.first(1))));

  // Drop offset 1, then send the rest of the data.
  for (uint32_t i = 2; i < kData.size(); ++i) {
    ctx_.SendClientStream<64>(
        EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                        .set_session_id(7)
                        .set_offset(i)
                        .set_payload(data.subspan(i, 1))));
  }

  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 2u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  EXPECT_EQ(chunk.offset(), 1u);

  // Send the remaining data.
  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(1)
                      .set_payload(data.subspan(1, 31))
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.finalize_write_called);
  EXPECT_EQ(handler_.finalize_write_status, OkStatus());
}

TEST_F(WriteTransfer, ResendParametersIfSentRepeatedChunkDuringRecovery) {
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart).set_session_id(7)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 1u);

  constexpr span data(kData);

  // Skip offset 0, then send the rest of the data.
  for (uint32_t i = 1; i < kData.size(); ++i) {
    ctx_.SendClientStream<64>(
        EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                        .set_session_id(7)
                        .set_offset(i)
                        .set_payload(data.subspan(i, 1))));
  }

  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 2u);  // Resent transfer parameters once.

  const auto last_chunk =
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(kData.size() - 1)
                      .set_payload(data.last(1)));
  ctx_.SendClientStream<64>(last_chunk);
  transfer_thread_.WaitUntilEventIsProcessed();

  // Resent transfer parameters since the packet is repeated
  ASSERT_EQ(ctx_.total_responses(), 3u);

  ctx_.SendClientStream<64>(last_chunk);
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 4u);

  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), 32u);

  // Resumes normal operation when correct offset is sent.
  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(0)
                      .set_payload(kData)
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.finalize_write_called);
  EXPECT_EQ(handler_.finalize_write_status, OkStatus());
}

TEST_F(WriteTransfer, ResendsStatusIfClientRetriesAfterStatusChunk) {
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart).set_session_id(7)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 1u);

  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(0)
                      .set_payload(kData)
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 2u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), OkStatus());

  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(0)
                      .set_payload(kData)
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 3u);
  chunk = DecodeChunk(ctx_.responses().back());
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), OkStatus());
}

TEST_F(WriteTransfer, IgnoresNonPendingTransfers) {
  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(3)));
  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(0)
                      .set_payload(span(kData).first(10))
                      .set_remaining_bytes(0)));

  transfer_thread_.WaitUntilEventIsProcessed();

  // Only start transfer for initial packet.
  EXPECT_FALSE(handler_.prepare_write_called);
  EXPECT_FALSE(handler_.finalize_write_called);
}

TEST_F(WriteTransfer, AbortAndRestartIfInitialPacketIsReceived) {
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart).set_session_id(7)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 1u);

  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(0)
                      .set_payload(span(kData).first(8))));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 1u);

  ASSERT_TRUE(handler_.prepare_write_called);
  ASSERT_FALSE(handler_.finalize_write_called);
  handler_.prepare_write_called = false;  // Reset to check it's called again.

  // Simulate client disappearing then restarting the transfer.
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart).set_session_id(7)));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_write_called);
  EXPECT_TRUE(handler_.finalize_write_called);
  EXPECT_EQ(handler_.finalize_write_status, Status::Aborted());

  handler_.finalize_write_called = false;  // Reset to check it's called again.

  ASSERT_EQ(ctx_.total_responses(), 2u);

  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(0)
                      .set_payload(kData)
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 3u);

  EXPECT_TRUE(handler_.finalize_write_called);
  EXPECT_EQ(handler_.finalize_write_status, OkStatus());
  EXPECT_EQ(std::memcmp(buffer.data(), kData.data(), kData.size()), 0);
}

class SometimesUnavailableReadHandler final : public ReadOnlyHandler {
 public:
  SometimesUnavailableReadHandler(uint32_t session_id, ConstByteSpan data)
      : ReadOnlyHandler(session_id), reader_(data), call_count_(0) {}

  Status PrepareRead() final {
    if ((call_count_++ % 2) == 0) {
      return Status::Unavailable();
    }

    set_reader(reader_);
    return OkStatus();
  }

 private:
  stream::MemoryReader reader_;
  int call_count_;
};

TEST_F(ReadTransfer, PrepareError) {
  SometimesUnavailableReadHandler unavailable_handler(88, kData);
  ctx_.service().RegisterHandler(unavailable_handler);

  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart)
                      .set_session_id(88)
                      .set_window_end_offset(128)
                      .set_offset(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 88u);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), Status::DataLoss());

  // Try starting the transfer again. It should work this time.
  // TODO(frolv): This won't work until completion ACKs are supported.
  if (false) {
    ctx_.SendClientStream(
        EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart)
                        .set_session_id(88)
                        .set_window_end_offset(128)
                        .set_offset(0)));
    transfer_thread_.WaitUntilEventIsProcessed();

    ASSERT_EQ(ctx_.total_responses(), 2u);
    chunk = DecodeChunk(ctx_.responses().back());
    EXPECT_EQ(chunk.session_id(), 88u);
    ASSERT_EQ(chunk.payload().size(), kData.size());
    EXPECT_EQ(std::memcmp(
                  chunk.payload().data(), kData.data(), chunk.payload().size()),
              0);
  }

  ctx_.service().UnregisterHandler(unavailable_handler);
}

TEST_F(WriteTransferMaxBytes16, Service_SetMaxPendingBytes) {
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart).set_session_id(7)));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_write_called);
  EXPECT_FALSE(handler_.finalize_write_called);

  // First parameters chunk has the default window end offset of 16.
  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  EXPECT_EQ(chunk.window_end_offset(), 16u);

  // Update the max window size.
  ctx_.service().set_max_window_size_bytes(12);

  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(0)
                      .set_payload(span(kData).first(8))));
  transfer_thread_.WaitUntilEventIsProcessed();

  // Second parameters chunk should use the new max pending bytes.
  ASSERT_EQ(ctx_.total_responses(), 2u);
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 7u);
  EXPECT_EQ(chunk.offset(), 8u);
  EXPECT_EQ(chunk.window_end_offset(), 8u + 12u);
}

TEST_F(ReadTransfer, Version2_SimpleTransfer) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStart)
                      .set_desired_session_id(kArbitrarySessionId)
                      .set_resource_id(3)));

  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_read_called);
  EXPECT_FALSE(handler_.finalize_read_called);

  // First, the server responds with a START_ACK, accepting the session ID and
  // confirming the protocol version.
  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAck);
  EXPECT_FALSE(chunk.desired_session_id().has_value());
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.resource_id(), 3u);

  // Complete the handshake by confirming the server's ACK and sending the first
  // read transfer parameters.
  rpc::test::WaitForPackets(ctx_.output(), 2, [this] {
    ctx_.SendClientStream(EncodeChunk(
        Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAckConfirmation)
            .set_session_id(kArbitrarySessionId)
            .set_window_end_offset(64)
            .set_offset(0)));

    transfer_thread_.WaitUntilEventIsProcessed();
  });

  // Server should respond by starting the data transfer, sending its sole data
  // chunk and a remaining_bytes 0 chunk.
  ASSERT_EQ(ctx_.total_responses(), 3u);

  Chunk c1 = DecodeChunk(ctx_.responses()[1]);
  EXPECT_EQ(c1.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(c1.type(), Chunk::Type::kData);
  EXPECT_EQ(c1.session_id(), kArbitrarySessionId);
  EXPECT_EQ(c1.offset(), 0u);
  ASSERT_TRUE(c1.has_payload());
  ASSERT_EQ(c1.payload().size(), kData.size());
  EXPECT_EQ(std::memcmp(c1.payload().data(), kData.data(), c1.payload().size()),
            0);

  Chunk c2 = DecodeChunk(ctx_.responses()[2]);
  EXPECT_EQ(c2.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(c2.type(), Chunk::Type::kData);
  EXPECT_EQ(c2.session_id(), kArbitrarySessionId);
  EXPECT_FALSE(c2.has_payload());
  EXPECT_EQ(c2.remaining_bytes(), 0u);

  ctx_.SendClientStream(EncodeChunk(Chunk::Final(
      ProtocolVersion::kVersionTwo, kArbitrarySessionId, OkStatus())));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.finalize_read_called);
  EXPECT_EQ(handler_.finalize_read_status, OkStatus());
}

TEST_F(ReadTransfer, Version2_MultiChunk) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStart)
                      .set_desired_session_id(kArbitrarySessionId)
                      .set_resource_id(3)));

  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_read_called);
  EXPECT_FALSE(handler_.finalize_read_called);

  // First, the server responds with a START_ACK, accepting the session ID and
  // confirming the protocol version.
  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAck);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.resource_id(), 3u);

  // Complete the handshake by confirming the server's ACK and sending the first
  // read transfer parameters.
  rpc::test::WaitForPackets(ctx_.output(), 3, [this] {
    ctx_.SendClientStream(EncodeChunk(
        Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAckConfirmation)
            .set_session_id(kArbitrarySessionId)
            .set_window_end_offset(64)
            .set_max_chunk_size_bytes(16)
            .set_offset(0)));

    transfer_thread_.WaitUntilEventIsProcessed();
  });

  ASSERT_EQ(ctx_.total_responses(), 4u);

  Chunk c1 = DecodeChunk(ctx_.responses()[1]);
  EXPECT_EQ(c1.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(c1.type(), Chunk::Type::kData);
  EXPECT_EQ(c1.session_id(), kArbitrarySessionId);
  EXPECT_EQ(c1.offset(), 0u);
  ASSERT_TRUE(c1.has_payload());
  ASSERT_EQ(c1.payload().size(), 16u);
  EXPECT_EQ(std::memcmp(c1.payload().data(), kData.data(), c1.payload().size()),
            0);

  Chunk c2 = DecodeChunk(ctx_.responses()[2]);
  EXPECT_EQ(c2.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(c2.type(), Chunk::Type::kData);
  EXPECT_EQ(c2.session_id(), kArbitrarySessionId);
  EXPECT_EQ(c2.offset(), 16u);
  ASSERT_TRUE(c2.has_payload());
  ASSERT_EQ(c2.payload().size(), 16u);
  EXPECT_EQ(
      std::memcmp(
          c2.payload().data(), kData.data() + c2.offset(), c2.payload().size()),
      0);

  Chunk c3 = DecodeChunk(ctx_.responses()[3]);
  EXPECT_EQ(c3.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(c3.type(), Chunk::Type::kData);
  EXPECT_EQ(c3.session_id(), kArbitrarySessionId);
  EXPECT_FALSE(c3.has_payload());
  EXPECT_EQ(c3.remaining_bytes(), 0u);

  ctx_.SendClientStream(EncodeChunk(Chunk::Final(
      ProtocolVersion::kVersionTwo, kArbitrarySessionId, OkStatus())));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.finalize_read_called);
  EXPECT_EQ(handler_.finalize_read_status, OkStatus());
}

TEST_F(ReadTransfer, Version2_MultiParameters) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStart)
                      .set_desired_session_id(kArbitrarySessionId)
                      .set_resource_id(3)));

  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_read_called);
  EXPECT_FALSE(handler_.finalize_read_called);

  // First, the server responds with a START_ACK, accepting the session ID and
  // confirming the protocol version.
  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAck);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.resource_id(), 3u);

  // Complete the handshake by confirming the server's ACK and sending the first
  // read transfer parameters.
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAckConfirmation)
          .set_session_id(kArbitrarySessionId)
          .set_window_end_offset(16)
          .set_offset(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 2u);

  Chunk c1 = DecodeChunk(ctx_.responses()[1]);
  EXPECT_EQ(c1.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(c1.type(), Chunk::Type::kData);
  EXPECT_EQ(c1.session_id(), kArbitrarySessionId);
  EXPECT_EQ(c1.offset(), 0u);
  ASSERT_TRUE(c1.has_payload());
  ASSERT_EQ(c1.payload().size(), 16u);
  EXPECT_EQ(std::memcmp(c1.payload().data(), kData.data(), c1.payload().size()),
            0);

  rpc::test::WaitForPackets(ctx_.output(), 2, [this] {
    ctx_.SendClientStream(EncodeChunk(
        Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kParametersContinue)
            .set_session_id(kArbitrarySessionId)
            .set_window_end_offset(64)
            .set_offset(16)));
    transfer_thread_.WaitUntilEventIsProcessed();
  });

  ASSERT_EQ(ctx_.total_responses(), 4u);

  Chunk c2 = DecodeChunk(ctx_.responses()[2]);
  EXPECT_EQ(c2.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(c2.type(), Chunk::Type::kData);
  EXPECT_EQ(c2.session_id(), kArbitrarySessionId);
  EXPECT_EQ(c2.offset(), 16u);
  ASSERT_TRUE(c2.has_payload());
  ASSERT_EQ(c2.payload().size(), 16u);
  EXPECT_EQ(
      std::memcmp(
          c2.payload().data(), kData.data() + c2.offset(), c2.payload().size()),
      0);

  Chunk c3 = DecodeChunk(ctx_.responses()[3]);
  EXPECT_EQ(c3.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(c3.type(), Chunk::Type::kData);
  EXPECT_EQ(c3.session_id(), kArbitrarySessionId);
  EXPECT_FALSE(c3.has_payload());
  EXPECT_EQ(c3.remaining_bytes(), 0u);

  ctx_.SendClientStream(EncodeChunk(Chunk::Final(
      ProtocolVersion::kVersionTwo, kArbitrarySessionId, OkStatus())));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.finalize_read_called);
  EXPECT_EQ(handler_.finalize_read_status, OkStatus());
}

TEST_F(ReadTransfer, Version2_ClientTerminatesDuringHandshake) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStart)
                      .set_desired_session_id(kArbitrarySessionId)
                      .set_resource_id(3)));

  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_read_called);
  EXPECT_FALSE(handler_.finalize_read_called);

  // First, the server responds with a START_ACK, accepting the session ID and
  // confirming the protocol version.
  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAck);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.resource_id(), 3u);

  // Send a terminating chunk instead of the third part of the handshake.
  ctx_.SendClientStream(EncodeChunk(Chunk::Final(ProtocolVersion::kVersionTwo,
                                                 kArbitrarySessionId,
                                                 Status::ResourceExhausted())));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.finalize_read_called);
  EXPECT_EQ(handler_.finalize_read_status, Status::ResourceExhausted());
}

TEST_F(ReadTransfer, Version2_ClientSendsWrongProtocolVersion) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStart)
                      .set_desired_session_id(kArbitrarySessionId)
                      .set_resource_id(3)));

  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_read_called);
  EXPECT_FALSE(handler_.finalize_read_called);

  // First, the server responds with a START_ACK, accepting the session ID and
  // confirming the protocol version.
  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAck);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.resource_id(), 3u);

  // Complete the handshake by confirming the server's ACK and sending the first
  // read transfer parameters.
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAckConfirmation)
          .set_session_id(kArbitrarySessionId)
          .set_window_end_offset(16)
          .set_offset(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 2u);

  Chunk c1 = DecodeChunk(ctx_.responses()[1]);
  EXPECT_EQ(c1.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(c1.type(), Chunk::Type::kData);
  EXPECT_EQ(c1.session_id(), kArbitrarySessionId);
  EXPECT_EQ(c1.offset(), 0u);
  ASSERT_TRUE(c1.has_payload());
  ASSERT_EQ(c1.payload().size(), 16u);
  EXPECT_EQ(std::memcmp(c1.payload().data(), kData.data(), c1.payload().size()),
            0);

  // Send a parameters update, but with the incorrect protocol version. The
  // server should terminate the transfer.
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kParametersContinue)
          .set_session_id(3)
          .set_window_end_offset(64)
          .set_offset(16)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 3u);

  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletion);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), Status::Internal());

  EXPECT_TRUE(handler_.finalize_read_called);
  EXPECT_EQ(handler_.finalize_read_status, Status::Internal());
}

TEST_F(ReadTransfer, Version2_BadParametersInHandshake) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStart)
                      .set_desired_session_id(kArbitrarySessionId)
                      .set_resource_id(3)));

  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_read_called);
  EXPECT_FALSE(handler_.finalize_read_called);

  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAck);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.resource_id(), 3u);

  // Complete the handshake, but send an invalid parameters chunk. The server
  // should terminate the transfer.
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAckConfirmation)
          .set_session_id(kArbitrarySessionId)
          .set_window_end_offset(0)
          .set_offset(0)));

  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 2u);

  Chunk c1 = DecodeChunk(ctx_.responses()[1]);
  EXPECT_EQ(c1.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(c1.type(), Chunk::Type::kCompletion);
  EXPECT_EQ(c1.session_id(), kArbitrarySessionId);
  ASSERT_TRUE(c1.status().has_value());
  EXPECT_EQ(c1.status().value(), Status::ResourceExhausted());
}

TEST_F(ReadTransfer, Version2_InvalidResourceId) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStart)
                      .set_desired_session_id(kArbitrarySessionId)
                      .set_resource_id(99)));

  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 1u);

  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletion);
  EXPECT_EQ(chunk.status().value(), Status::NotFound());
}

TEST_F(ReadTransfer, Version2_PrepareError) {
  SometimesUnavailableReadHandler unavailable_handler(99, kData);
  ctx_.service().RegisterHandler(unavailable_handler);

  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStart)
                      .set_desired_session_id(kArbitrarySessionId)
                      .set_resource_id(99)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletion);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.resource_id(), 99u);
  EXPECT_EQ(chunk.status().value(), Status::DataLoss());

  ctx_.service().UnregisterHandler(unavailable_handler);
}

TEST_F(ReadTransfer, Version2_HandlerSetsTransferSize) {
  handler_.set_resource_size(kData.size());

  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStart)
                      .set_desired_session_id(kArbitrarySessionId)
                      .set_resource_id(3)));

  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_read_called);
  EXPECT_FALSE(handler_.finalize_read_called);

  // First, the server responds with a START_ACK, accepting the session ID and
  // confirming the protocol version.
  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAck);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.resource_id(), 3u);

  // Complete the handshake by confirming the server's ACK and sending the first
  // read transfer parameters.
  rpc::test::WaitForPackets(ctx_.output(), 4, [this] {
    ctx_.SendClientStream(EncodeChunk(
        Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAckConfirmation)
            .set_session_id(kArbitrarySessionId)
            .set_window_end_offset(64)
            .set_max_chunk_size_bytes(8)
            .set_offset(0)));

    transfer_thread_.WaitUntilEventIsProcessed();
  });

  ASSERT_EQ(ctx_.total_responses(), 5u);

  // Each of the sent chunks should have a remaining_bytes value set.
  Chunk c1 = DecodeChunk(ctx_.responses()[1]);
  EXPECT_EQ(c1.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(c1.type(), Chunk::Type::kData);
  EXPECT_EQ(c1.session_id(), kArbitrarySessionId);
  EXPECT_EQ(c1.offset(), 0u);
  ASSERT_TRUE(c1.remaining_bytes().has_value());
  EXPECT_EQ(c1.remaining_bytes().value(), 24u);

  Chunk c2 = DecodeChunk(ctx_.responses()[2]);
  EXPECT_EQ(c2.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(c2.type(), Chunk::Type::kData);
  EXPECT_EQ(c2.session_id(), kArbitrarySessionId);
  EXPECT_EQ(c2.offset(), 8u);
  ASSERT_TRUE(c2.remaining_bytes().has_value());
  EXPECT_EQ(c2.remaining_bytes().value(), 16u);

  Chunk c3 = DecodeChunk(ctx_.responses()[3]);
  EXPECT_EQ(c3.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(c3.type(), Chunk::Type::kData);
  EXPECT_EQ(c3.session_id(), kArbitrarySessionId);
  EXPECT_EQ(c3.offset(), 16u);
  ASSERT_TRUE(c3.remaining_bytes().has_value());
  EXPECT_EQ(c3.remaining_bytes().value(), 8u);

  Chunk c4 = DecodeChunk(ctx_.responses()[4]);
  EXPECT_EQ(c4.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(c4.type(), Chunk::Type::kData);
  EXPECT_EQ(c4.session_id(), kArbitrarySessionId);
  EXPECT_EQ(c4.offset(), 24u);
  ASSERT_TRUE(c4.remaining_bytes().has_value());
  EXPECT_EQ(c4.remaining_bytes().value(), 0u);

  ctx_.SendClientStream(EncodeChunk(Chunk::Final(
      ProtocolVersion::kVersionTwo, kArbitrarySessionId, OkStatus())));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.finalize_read_called);
  EXPECT_EQ(handler_.finalize_read_status, OkStatus());
}

TEST_F(WriteTransfer, Version2_SimpleTransfer) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStart)
                      .set_desired_session_id(kArbitrarySessionId)
                      .set_resource_id(7)));

  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_write_called);
  EXPECT_FALSE(handler_.finalize_write_called);

  // First, the server responds with a START_ACK, accepting the session ID and
  // confirming the protocol version.
  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAck);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.resource_id(), 7u);

  // Complete the handshake by confirming the server's ACK.
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAckConfirmation)
          .set_session_id(kArbitrarySessionId)));
  transfer_thread_.WaitUntilEventIsProcessed();

  // Server should respond by sending its initial transfer parameters.
  ASSERT_EQ(ctx_.total_responses(), 2u);

  chunk = DecodeChunk(ctx_.responses()[1]);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersRetransmit);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), 32u);
  ASSERT_TRUE(chunk.max_chunk_size_bytes().has_value());
  EXPECT_EQ(chunk.max_chunk_size_bytes().value(), 37u);

  // Send all of our data.
  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
                      .set_session_id(kArbitrarySessionId)
                      .set_offset(0)
                      .set_payload(kData)
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 3u);

  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletion);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), OkStatus());

  // Send the completion acknowledgement.
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kCompletionAck)
          .set_session_id(kArbitrarySessionId)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 3u);

  EXPECT_TRUE(handler_.finalize_write_called);
  EXPECT_EQ(handler_.finalize_write_status, OkStatus());
  EXPECT_EQ(std::memcmp(buffer.data(), kData.data(), kData.size()), 0);
}

TEST_F(WriteTransfer, Version2_Multichunk) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStart)
                      .set_desired_session_id(kArbitrarySessionId)
                      .set_resource_id(7)));

  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_write_called);
  EXPECT_FALSE(handler_.finalize_write_called);

  // First, the server responds with a START_ACK, accepting the session ID and
  // confirming the protocol version.
  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAck);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.resource_id(), 7u);

  // Complete the handshake by confirming the server's ACK.
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAckConfirmation)
          .set_session_id(kArbitrarySessionId)));
  transfer_thread_.WaitUntilEventIsProcessed();

  // Server should respond by sending its initial transfer parameters.
  ASSERT_EQ(ctx_.total_responses(), 2u);

  chunk = DecodeChunk(ctx_.responses()[1]);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersRetransmit);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), 32u);
  ASSERT_TRUE(chunk.max_chunk_size_bytes().has_value());
  EXPECT_EQ(chunk.max_chunk_size_bytes().value(), 37u);

  // Send all of our data across two chunks.
  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
                      .set_session_id(kArbitrarySessionId)
                      .set_offset(0)
                      .set_payload(span(kData).first(8))));
  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
                      .set_session_id(kArbitrarySessionId)
                      .set_offset(8)
                      .set_payload(span(kData).subspan(8))
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 3u);

  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletion);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), OkStatus());

  // Send the completion acknowledgement.
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kCompletionAck)
          .set_session_id(kArbitrarySessionId)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 3u);

  EXPECT_TRUE(handler_.finalize_write_called);
  EXPECT_EQ(handler_.finalize_write_status, OkStatus());
  EXPECT_EQ(std::memcmp(buffer.data(), kData.data(), kData.size()), 0);
}

TEST_F(WriteTransfer, Version2_ContinueParameters) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStart)
                      .set_desired_session_id(kArbitrarySessionId)
                      .set_resource_id(7)));

  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_write_called);
  EXPECT_FALSE(handler_.finalize_write_called);

  // First, the server responds with a START_ACK, accepting the session ID and
  // confirming the protocol version.
  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAck);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.resource_id(), 7u);

  // Complete the handshake by confirming the server's ACK.
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAckConfirmation)
          .set_session_id(kArbitrarySessionId)));
  transfer_thread_.WaitUntilEventIsProcessed();

  // Server should respond by sending its initial transfer parameters.
  ASSERT_EQ(ctx_.total_responses(), 2u);

  chunk = DecodeChunk(ctx_.responses()[1]);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersRetransmit);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), 32u);
  ASSERT_TRUE(chunk.max_chunk_size_bytes().has_value());
  EXPECT_EQ(chunk.max_chunk_size_bytes().value(), 37u);

  // Send all of our data across several chunks.
  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
                      .set_session_id(kArbitrarySessionId)
                      .set_offset(0)
                      .set_payload(span(kData).first(8))));

  transfer_thread_.WaitUntilEventIsProcessed();
  ASSERT_EQ(ctx_.total_responses(), 2u);

  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
                      .set_session_id(kArbitrarySessionId)
                      .set_offset(8)
                      .set_payload(span(kData).subspan(8, 8))));

  transfer_thread_.WaitUntilEventIsProcessed();
  ASSERT_EQ(ctx_.total_responses(), 3u);

  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersContinue);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.offset(), 16u);
  EXPECT_EQ(chunk.window_end_offset(), 32u);

  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
                      .set_session_id(kArbitrarySessionId)
                      .set_offset(16)
                      .set_payload(span(kData).subspan(16, 8))));

  transfer_thread_.WaitUntilEventIsProcessed();
  ASSERT_EQ(ctx_.total_responses(), 4u);

  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersContinue);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.offset(), 24u);
  EXPECT_EQ(chunk.window_end_offset(), 32u);

  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
                      .set_session_id(kArbitrarySessionId)
                      .set_offset(24)
                      .set_payload(span(kData).subspan(24))
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 5u);

  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletion);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), OkStatus());

  // Send the completion acknowledgement.
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kCompletionAck)
          .set_session_id(kArbitrarySessionId)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 5u);

  EXPECT_TRUE(handler_.finalize_write_called);
  EXPECT_EQ(handler_.finalize_write_status, OkStatus());
  EXPECT_EQ(std::memcmp(buffer.data(), kData.data(), kData.size()), 0);
}

TEST_F(WriteTransfer, Version2_ClientTerminatesDuringHandshake) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStart)
                      .set_desired_session_id(kArbitrarySessionId)
                      .set_resource_id(7)));

  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_write_called);
  EXPECT_FALSE(handler_.finalize_write_called);

  // First, the server responds with a START_ACK, accepting the session ID and
  // confirming the protocol version.
  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAck);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.resource_id(), 7u);

  // Send an error chunk instead of completing the handshake.
  ctx_.SendClientStream(
      EncodeChunk(Chunk::Final(ProtocolVersion::kVersionTwo,
                               kArbitrarySessionId,
                               Status::FailedPrecondition())));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.finalize_write_called);
  EXPECT_EQ(handler_.finalize_write_status, Status::FailedPrecondition());
}

TEST_F(WriteTransfer, Version2_ClientSendsWrongProtocolVersion) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStart)
                      .set_desired_session_id(kArbitrarySessionId)
                      .set_resource_id(7)));

  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_write_called);
  EXPECT_FALSE(handler_.finalize_write_called);

  // First, the server responds with a START_ACK, accepting the session ID and
  // confirming the protocol version.
  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAck);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.resource_id(), 7u);

  // Complete the handshake by confirming the server's ACK.
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAckConfirmation)
          .set_session_id(kArbitrarySessionId)));
  transfer_thread_.WaitUntilEventIsProcessed();

  // Server should respond by sending its initial transfer parameters.
  ASSERT_EQ(ctx_.total_responses(), 2u);

  chunk = DecodeChunk(ctx_.responses()[1]);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersRetransmit);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), 32u);
  ASSERT_TRUE(chunk.max_chunk_size_bytes().has_value());
  EXPECT_EQ(chunk.max_chunk_size_bytes().value(), 37u);

  // The transfer was configured to use protocol version 2. Send a legacy chunk
  // instead.
  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(0)
                      .set_payload(kData)
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  // Server should terminate the transfer.
  ASSERT_EQ(ctx_.total_responses(), 3u);

  chunk = DecodeChunk(ctx_.responses()[2]);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletion);
  EXPECT_EQ(chunk.status().value(), Status::Internal());

  // Send the completion acknowledgement.
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kCompletionAck)
          .set_session_id(kArbitrarySessionId)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 3u);
}

TEST_F(WriteTransfer, Version2_InvalidResourceId) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStart)
                      .set_desired_session_id(kArbitrarySessionId)
                      .set_resource_id(99)));

  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 1u);

  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_FALSE(chunk.resource_id().has_value());
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletion);
  EXPECT_EQ(chunk.status().value(), Status::NotFound());
}

class ReadTransferLowMaxRetries : public ::testing::Test {
 protected:
  static constexpr uint32_t kMaxRetries = 3;
  static constexpr uint32_t kMaxLifetimeRetries = 4;

  ReadTransferLowMaxRetries()
      : handler_(9, kData),
        transfer_thread_(data_buffer_, encode_buffer_),
        ctx_(transfer_thread_,
             64,
             // Use a long timeout to avoid accidentally triggering timeouts.
             std::chrono::minutes(1),
             kMaxRetries,
             cfg::kDefaultExtendWindowDivisor,
             kMaxLifetimeRetries),
        system_thread_(TransferThreadOptions(), transfer_thread_) {
    ctx_.service().RegisterHandler(handler_);

    PW_CHECK(!handler_.prepare_read_called);
    PW_CHECK(!handler_.finalize_read_called);

    ctx_.call();  // Open the read stream
    transfer_thread_.WaitUntilEventIsProcessed();
  }

  ~ReadTransferLowMaxRetries() override {
    transfer_thread_.Terminate();
    system_thread_.join();
  }

  SimpleReadTransfer handler_;
  Thread<1, 1> transfer_thread_;
  PW_RAW_TEST_METHOD_CONTEXT(TransferService, Read, 10) ctx_;
  pw::Thread system_thread_;
  std::array<std::byte, 64> data_buffer_;
  std::array<std::byte, 64> encode_buffer_;
};

TEST_F(ReadTransferLowMaxRetries, FailsAfterLifetimeRetryCount) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart)
                      .set_session_id(9)
                      .set_window_end_offset(16)
                      .set_offset(0)));

  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_read_called);
  EXPECT_FALSE(handler_.finalize_read_called);

  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());

  EXPECT_EQ(chunk.session_id(), 9u);
  EXPECT_EQ(chunk.offset(), 0u);
  ASSERT_EQ(chunk.payload().size(), 16u);
  EXPECT_EQ(
      std::memcmp(chunk.payload().data(), kData.data(), chunk.payload().size()),
      0);

  // Time out twice. Server should retry both times.
  transfer_thread_.SimulateServerTimeout(9);
  transfer_thread_.SimulateServerTimeout(9);
  ASSERT_EQ(ctx_.total_responses(), 3u);
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 9u);
  EXPECT_EQ(chunk.offset(), 0u);
  ASSERT_EQ(chunk.payload().size(), 16u);
  EXPECT_EQ(
      std::memcmp(chunk.payload().data(), kData.data(), chunk.payload().size()),
      0);

  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kParametersContinue)
          .set_session_id(9)
          .set_window_end_offset(32)
          .set_offset(16)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 4u);
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 9u);
  EXPECT_EQ(chunk.offset(), 16u);
  ASSERT_EQ(chunk.payload().size(), 16u);
  EXPECT_EQ(
      std::memcmp(
          chunk.payload().data(), kData.data() + 16, chunk.payload().size()),
      0);

  // Time out three more times. The transfer should terminate.
  transfer_thread_.SimulateServerTimeout(9);
  ASSERT_EQ(ctx_.total_responses(), 5u);
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 9u);
  EXPECT_EQ(chunk.offset(), 16u);
  ASSERT_EQ(chunk.payload().size(), 16u);
  EXPECT_EQ(
      std::memcmp(
          chunk.payload().data(), kData.data() + 16, chunk.payload().size()),
      0);

  transfer_thread_.SimulateServerTimeout(9);
  ASSERT_EQ(ctx_.total_responses(), 6u);
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), 9u);
  EXPECT_EQ(chunk.offset(), 16u);
  ASSERT_EQ(chunk.payload().size(), 16u);
  EXPECT_EQ(
      std::memcmp(
          chunk.payload().data(), kData.data() + 16, chunk.payload().size()),
      0);

  transfer_thread_.SimulateServerTimeout(9);
  ASSERT_EQ(ctx_.total_responses(), 7u);
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.status(), Status::DeadlineExceeded());
}

TEST_F(ReadTransferLowMaxRetries, Version2_FailsAfterLifetimeRetryCount) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStart)
                      .set_desired_session_id(kArbitrarySessionId)
                      .set_resource_id(9)));

  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_read_called);
  EXPECT_FALSE(handler_.finalize_read_called);

  // First, the server responds with a START_ACK, accepting the session ID and
  // confirming the protocol version.
  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAck);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.resource_id(), 9u);

  // Time out twice. Server should retry both times.
  transfer_thread_.SimulateServerTimeout(kArbitrarySessionId);
  transfer_thread_.SimulateServerTimeout(kArbitrarySessionId);
  ASSERT_EQ(ctx_.total_responses(), 3u);
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAck);

  // Complete the handshake, allowing the transfer to continue.
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAckConfirmation)
          .set_session_id(kArbitrarySessionId)
          .set_window_end_offset(16)
          .set_offset(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 4u);
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kData);

  // Time out three more times. The transfer should terminate.
  transfer_thread_.SimulateServerTimeout(kArbitrarySessionId);
  ASSERT_EQ(ctx_.total_responses(), 5u);
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kData);

  transfer_thread_.SimulateServerTimeout(kArbitrarySessionId);
  ASSERT_EQ(ctx_.total_responses(), 6u);
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kData);

  transfer_thread_.SimulateServerTimeout(kArbitrarySessionId);
  ASSERT_EQ(ctx_.total_responses(), 7u);
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletion);
  EXPECT_EQ(chunk.status(), Status::DeadlineExceeded());
}

TEST_F(WriteTransfer, Version2_ClientRetriesOpeningChunk) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStart)
                      .set_desired_session_id(kArbitrarySessionId)
                      .set_resource_id(7)));

  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_write_called);
  EXPECT_FALSE(handler_.finalize_write_called);

  // First, the server responds with a START_ACK, accepting the session ID and
  // confirming the protocol version.
  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAck);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.resource_id(), 7u);

  // Reset prepare_write_called to ensure it isn't called again.
  handler_.prepare_write_called = false;

  // Client re-sends the same chunk instead of finishing the handshake.
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStart)
                      .set_desired_session_id(kArbitrarySessionId)
                      .set_resource_id(7)));

  transfer_thread_.WaitUntilEventIsProcessed();

  // The server should re-send the same START_ACK without reinitializing the
  // handler.
  ASSERT_EQ(ctx_.total_responses(), 2u);
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAck);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.resource_id(), 7u);

  EXPECT_FALSE(handler_.prepare_write_called);
  EXPECT_FALSE(handler_.finalize_write_called);
}

TEST_F(WriteTransfer, Version2_RegularSessionIdInStartChunk) {
  // Client incorrectly sets session_id instead of desired_session_id in its
  // START chunk. Server should immediately respond with a protocol error.
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStart)
                      .set_session_id(kArbitrarySessionId)
                      .set_resource_id(99)));

  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_FALSE(handler_.prepare_write_called);
  EXPECT_FALSE(handler_.finalize_write_called);

  ASSERT_EQ(ctx_.total_responses(), 1u);

  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_FALSE(chunk.resource_id().has_value());
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletion);
  EXPECT_EQ(chunk.status().value(), Status::DataLoss());
}

constexpr auto kData128 = bytes::Initialized<128>([](size_t i) { return i; });

class WriteTransferLargeData : public ::testing::Test {
 protected:
  WriteTransferLargeData()
      : buffer{},
        handler_(7, buffer),
        transfer_thread_(chunk_buffer_, encode_buffer_),
        system_thread_(TransferThreadOptions(), transfer_thread_),
        ctx_(transfer_thread_,
             kData128.size(),
             // Use a long timeout to avoid accidentally triggering timeouts.
             std::chrono::minutes(1),
             /*max_retries=*/3) {
    ctx_.service().RegisterHandler(handler_);

    PW_CHECK(!handler_.prepare_write_called);
    PW_CHECK(!handler_.finalize_write_called);

    ctx_.call();  // Open the write stream
    transfer_thread_.WaitUntilEventIsProcessed();
  }

  ~WriteTransferLargeData() override {
    transfer_thread_.Terminate();
    system_thread_.join();
  }

  std::array<std::byte, kData128.size()> buffer;
  SimpleWriteTransfer handler_;

  Thread<1, 1> transfer_thread_;
  pw::Thread system_thread_;
  std::array<std::byte, 48> chunk_buffer_;
  std::array<std::byte, 64> encode_buffer_;
  PW_RAW_TEST_METHOD_CONTEXT(TransferService, Write, 10) ctx_;
};

TEST_F(WriteTransferLargeData, Version2_AdaptiveWindow_SlowStart) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStart)
                      .set_desired_session_id(kArbitrarySessionId)
                      .set_resource_id(7)));

  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_write_called);
  EXPECT_FALSE(handler_.finalize_write_called);

  // First, the server responds with a START_ACK, accepting the session ID and
  // confirming the protocol version.
  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAck);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.resource_id(), 7u);

  // Complete the handshake by confirming the server's ACK.
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAckConfirmation)
          .set_session_id(kArbitrarySessionId)));
  transfer_thread_.WaitUntilEventIsProcessed();

  // Server should respond by sending its initial transfer parameters.
  ASSERT_EQ(ctx_.total_responses(), 2u);

  constexpr size_t kExpectedMaxChunkSize = 21;

  chunk = DecodeChunk(ctx_.responses()[1]);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersRetransmit);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), kExpectedMaxChunkSize);
  ASSERT_TRUE(chunk.max_chunk_size_bytes().has_value());
  EXPECT_EQ(chunk.max_chunk_size_bytes().value(), kExpectedMaxChunkSize);

  ctx_.SendClientStream<64>(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
          .set_session_id(kArbitrarySessionId)
          .set_offset(0)
          .set_payload(span(kData128).first(kExpectedMaxChunkSize))));

  transfer_thread_.WaitUntilEventIsProcessed();
  ASSERT_EQ(ctx_.total_responses(), 3u);

  // Window doubles in response to successful receive.
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersContinue);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.offset(), kExpectedMaxChunkSize);
  EXPECT_EQ(chunk.window_end_offset(),
            chunk.offset() + 2 * kExpectedMaxChunkSize);

  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
                      .set_session_id(kArbitrarySessionId)
                      .set_offset(chunk.offset())
                      .set_payload(span(kData128).subspan(
                          chunk.offset(), kExpectedMaxChunkSize))));

  transfer_thread_.WaitUntilEventIsProcessed();
  ASSERT_EQ(ctx_.total_responses(), 4u);

  // Window doubles in response to successful receive.
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersContinue);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.offset(), 2 * kExpectedMaxChunkSize);
  EXPECT_EQ(chunk.window_end_offset(),
            chunk.offset() + 4 * kExpectedMaxChunkSize);

  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
                      .set_session_id(kArbitrarySessionId)
                      .set_offset(2 * kExpectedMaxChunkSize)
                      .set_payload(span(kData128).subspan(
                          chunk.offset(), kExpectedMaxChunkSize))
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 5u);

  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletion);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), OkStatus());

  // Send the completion acknowledgement.
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kCompletionAck)
          .set_session_id(kArbitrarySessionId)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 5u);

  EXPECT_TRUE(handler_.finalize_write_called);
  EXPECT_EQ(handler_.finalize_write_status, OkStatus());
}

TEST_F(WriteTransferLargeData, Version2_AdaptiveWindow_CongestionAvoidance) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStart)
                      .set_desired_session_id(kArbitrarySessionId)
                      .set_resource_id(7)));

  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_write_called);
  EXPECT_FALSE(handler_.finalize_write_called);

  // First, the server responds with a START_ACK, accepting the session ID and
  // confirming the protocol version.
  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAck);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.resource_id(), 7u);

  // Complete the handshake by confirming the server's ACK.
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAckConfirmation)
          .set_session_id(kArbitrarySessionId)));
  transfer_thread_.WaitUntilEventIsProcessed();

  // Server should respond by sending its initial transfer parameters.
  ASSERT_EQ(ctx_.total_responses(), 2u);

  constexpr size_t kExpectedMaxChunkSize = 21;

  chunk = DecodeChunk(ctx_.responses()[1]);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersRetransmit);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), kExpectedMaxChunkSize);
  ASSERT_TRUE(chunk.max_chunk_size_bytes().has_value());
  EXPECT_EQ(chunk.max_chunk_size_bytes().value(), kExpectedMaxChunkSize);

  ctx_.SendClientStream<64>(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
          .set_session_id(kArbitrarySessionId)
          .set_offset(0)
          .set_payload(span(kData128).first(kExpectedMaxChunkSize))));

  transfer_thread_.WaitUntilEventIsProcessed();
  ASSERT_EQ(ctx_.total_responses(), 3u);

  // Window doubles in response to successful receive.
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersContinue);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.offset(), kExpectedMaxChunkSize);
  EXPECT_EQ(chunk.window_end_offset(),
            chunk.offset() + 2 * kExpectedMaxChunkSize);

  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
                      .set_session_id(kArbitrarySessionId)
                      .set_offset(chunk.offset())
                      .set_payload(span(kData128).subspan(
                          chunk.offset(), kExpectedMaxChunkSize))));

  transfer_thread_.WaitUntilEventIsProcessed();
  ASSERT_EQ(ctx_.total_responses(), 4u);

  // Window doubles in response to successful receive.
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersContinue);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.offset(), 2 * kExpectedMaxChunkSize);
  EXPECT_EQ(chunk.window_end_offset(),
            chunk.offset() + 4 * kExpectedMaxChunkSize);

  // Don't send any data in response.
  transfer_thread_.SimulateServerTimeout(kArbitrarySessionId);
  ASSERT_EQ(ctx_.total_responses(), 5u);

  // Window size should now half from the previous one.
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersRetransmit);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.offset(), 2 * kExpectedMaxChunkSize);
  EXPECT_EQ(chunk.window_end_offset(),
            chunk.offset() + 2 * kExpectedMaxChunkSize);

  // Send the appropriate chunk.
  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
                      .set_session_id(kArbitrarySessionId)
                      .set_offset(2 * kExpectedMaxChunkSize)
                      .set_payload(span(kData128).subspan(
                          chunk.offset(), kExpectedMaxChunkSize))));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 6u);

  // Window size should now only increase by one chunk instead of doubling.
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersContinue);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.offset(), 3 * kExpectedMaxChunkSize);
  EXPECT_EQ(chunk.window_end_offset(),
            chunk.offset() + 3 * kExpectedMaxChunkSize);

  // Complete the transfer.
  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
                      .set_session_id(kArbitrarySessionId)
                      .set_offset(3 * kExpectedMaxChunkSize)
                      .set_payload(span(kData128).subspan(
                          chunk.offset(), kExpectedMaxChunkSize))
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 7u);

  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletion);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), OkStatus());

  // Send the completion acknowledgement.
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kCompletionAck)
          .set_session_id(kArbitrarySessionId)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 7u);

  EXPECT_TRUE(handler_.finalize_write_called);
  EXPECT_EQ(handler_.finalize_write_status, OkStatus());
}

TEST_F(WriteTransferLargeData,
       Version2_ResendPreviousData_ReceivesContinueParameters) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStart)
                      .set_desired_session_id(kArbitrarySessionId)
                      .set_resource_id(7)));

  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_write_called);
  EXPECT_FALSE(handler_.finalize_write_called);

  // First, the server responds with a START_ACK, accepting the session ID and
  // confirming the protocol version.
  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAck);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.resource_id(), 7u);

  // Complete the handshake by confirming the server's ACK.
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAckConfirmation)
          .set_session_id(kArbitrarySessionId)));
  transfer_thread_.WaitUntilEventIsProcessed();

  // Server should respond by sending its initial transfer parameters.
  ASSERT_EQ(ctx_.total_responses(), 2u);

  constexpr size_t kExpectedMaxChunkSizeBytes = 21u;

  chunk = DecodeChunk(ctx_.responses()[1]);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersRetransmit);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), kExpectedMaxChunkSizeBytes);
  ASSERT_TRUE(chunk.max_chunk_size_bytes().has_value());
  EXPECT_EQ(chunk.max_chunk_size_bytes().value(), kExpectedMaxChunkSizeBytes);

  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
                      .set_session_id(kArbitrarySessionId)
                      .set_offset(0)
                      .set_payload(span(kData).first(16))));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 3u);

  // Window size grows to 2 chunks on successful receipt.
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersContinue);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.offset(), 16u);
  EXPECT_EQ(chunk.window_end_offset(),
            chunk.offset() + 2 * kExpectedMaxChunkSizeBytes);

  // Resend data that has already been received.
  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
                      .set_session_id(kArbitrarySessionId)
                      .set_offset(8)
                      .set_payload(span(kData).subspan(8, 8))
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  // The server should respond with CONTINUE parameters rather than requesting
  // retransmission and starting a recovery cycle. However, the window size
  // should shrink in response to the retried data.
  ASSERT_EQ(ctx_.total_responses(), 4u);
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersContinue);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.offset(), 16u);
  EXPECT_EQ(chunk.window_end_offset(),
            chunk.offset() + kExpectedMaxChunkSizeBytes);

  // Send the expected chunk.
  ctx_.SendClientStream<64>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
                      .set_session_id(kArbitrarySessionId)
                      .set_offset(16)
                      .set_payload(span(kData).subspan(16))
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.total_responses(), 5u);
  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), OkStatus());

  EXPECT_TRUE(handler_.finalize_write_called);
  EXPECT_EQ(handler_.finalize_write_status, OkStatus());
  EXPECT_EQ(std::memcmp(buffer.data(), kData.data(), kData.size()), 0);

  ctx_.SendClientStream<64>(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kCompletionAck)
          .set_session_id(kArbitrarySessionId)));
  transfer_thread_.WaitUntilEventIsProcessed();
  ASSERT_EQ(ctx_.total_responses(), 5u);
}

class WriteTransferMultibyteVarintChunkSize : public ::testing::Test {
 protected:
  WriteTransferMultibyteVarintChunkSize()
      : buffer{},
        handler_(7, buffer),
        transfer_thread_(chunk_buffer_, encode_buffer_),
        system_thread_(TransferThreadOptions(), transfer_thread_),
        ctx_(transfer_thread_,
             kData256.size(),
             // Use a long timeout to avoid accidentally triggering timeouts.
             std::chrono::minutes(1),
             /*max_retries=*/3) {
    ctx_.service().RegisterHandler(handler_);

    PW_CHECK(!handler_.prepare_write_called);
    PW_CHECK(!handler_.finalize_write_called);

    ctx_.call();  // Open the write stream
    transfer_thread_.WaitUntilEventIsProcessed();
  }

  ~WriteTransferMultibyteVarintChunkSize() override {
    transfer_thread_.Terminate();
    system_thread_.join();
  }

  static constexpr auto kData256 =
      bytes::Initialized<256>([](size_t i) { return i; });

  std::array<std::byte, kData256.size()> buffer;
  SimpleWriteTransfer handler_;

  Thread<1, 1> transfer_thread_;
  pw::Thread system_thread_;
  std::array<std::byte, kData256.size()> chunk_buffer_;
  std::array<std::byte, kData256.size()> encode_buffer_;
  PW_RAW_TEST_METHOD_CONTEXT(TransferService, Write, 10) ctx_;
};

TEST_F(WriteTransferMultibyteVarintChunkSize,
       AssumesMinimumWindowSizeInFirstParameters) {
  ctx_.SendClientStream(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStart)
                      .set_desired_session_id(kArbitrarySessionId)
                      .set_resource_id(7)));

  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(handler_.prepare_write_called);
  EXPECT_FALSE(handler_.finalize_write_called);

  // First, the server responds with a START_ACK, accepting the session ID and
  // confirming the protocol version.
  ASSERT_EQ(ctx_.total_responses(), 1u);
  Chunk chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAck);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.resource_id(), 7u);

  // Complete the handshake by confirming the server's ACK.
  ctx_.SendClientStream(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAckConfirmation)
          .set_session_id(kArbitrarySessionId)));
  transfer_thread_.WaitUntilEventIsProcessed();

  // Server should respond by sending its initial transfer parameters.
  ASSERT_EQ(ctx_.total_responses(), 2u);

  constexpr uint32_t kExpectedMaxChunkSize = 226;

  chunk = DecodeChunk(ctx_.responses()[1]);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersRetransmit);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), kExpectedMaxChunkSize);
  ASSERT_TRUE(chunk.max_chunk_size_bytes().has_value());
  EXPECT_EQ(chunk.max_chunk_size_bytes().value(), kExpectedMaxChunkSize);

  // Simulate a timeout, prompting the server to recalculate its transfer
  // parameters. The max chunk size and window size should not change.
  transfer_thread_.SimulateServerTimeout(kArbitrarySessionId);
  ASSERT_EQ(ctx_.total_responses(), 3u);

  chunk = DecodeChunk(ctx_.responses().back());
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersRetransmit);
  EXPECT_EQ(chunk.session_id(), kArbitrarySessionId);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), kExpectedMaxChunkSize);
  ASSERT_TRUE(chunk.max_chunk_size_bytes().has_value());
  EXPECT_EQ(chunk.max_chunk_size_bytes().value(), kExpectedMaxChunkSize);
}

class ReadTransferWithStats : public SimpleReadTransfer {
 public:
  using SimpleReadTransfer::SimpleReadTransfer;

  Status GetStatus(uint64_t& readable_offset,
                   uint64_t& writeable_offset,
                   uint64_t& read_checksum,
                   uint64_t& write_checksum) override {
    readable_offset = 64;
    read_checksum = 0xffee;
    writeable_offset = 128;
    write_checksum = 0xeeff;
    return OkStatus();
  }
};

class GetResourceStatus : public ::testing::Test {
 protected:
  GetResourceStatus(size_t max_chunk_size_bytes = 64)
      : handler_(3, kData),
        transfer_thread_(span(data_buffer_).first(max_chunk_size_bytes),
                         encode_buffer_),
        ctx_(transfer_thread_,
             64,
             // Use a long timeout to avoid accidentally triggering timeouts.
             std::chrono::minutes(1)),
        system_thread_(TransferThreadOptions(), transfer_thread_) {
    ctx_.service().RegisterHandler(handler_);

    PW_CHECK(!handler_.prepare_read_called);
    PW_CHECK(!handler_.finalize_read_called);
  }

  ~GetResourceStatus() override {
    transfer_thread_.Terminate();
    system_thread_.join();
  }

  ReadTransferWithStats handler_;
  Thread<1, 1> transfer_thread_;
  PW_RAW_TEST_METHOD_CONTEXT(TransferService, GetResourceStatus) ctx_;
  pw::Thread system_thread_;
  std::array<std::byte, 64> data_buffer_;
  std::array<std::byte, 64> encode_buffer_;
};

TEST_F(GetResourceStatus, ValidResourceId_ReturnsStatus) {
  pwpb::ResourceStatusRequest::MemoryEncoder encoder(encode_buffer_);
  ASSERT_EQ(encoder.Write({.resource_id = 3}), OkStatus());
  ctx_.call(encoder);

  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.status(), OkStatus());

  stream::MemoryReader reader(ctx_.response());
  pwpb::ResourceStatus::StreamDecoder decoder(reader);
  pwpb::ResourceStatus::Message response;
  ASSERT_EQ(decoder.Read(response), OkStatus());

  EXPECT_EQ(response.resource_id, 3u);
  EXPECT_EQ(response.readable_offset, 64u);
  EXPECT_EQ(response.read_checksum, 0xffee);
  EXPECT_EQ(response.writeable_offset, 128u);
  EXPECT_EQ(response.write_checksum, 0xeeff);
}

TEST_F(GetResourceStatus, InvalidResourceId_ReturnsNotFound) {
  pwpb::ResourceStatusRequest::MemoryEncoder encoder(encode_buffer_);
  ASSERT_EQ(encoder.Write({.resource_id = 27}), OkStatus());
  ctx_.call(encoder);

  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(ctx_.status(), Status::NotFound());

  stream::MemoryReader reader(ctx_.response());
  pwpb::ResourceStatus::StreamDecoder decoder(reader);
  pwpb::ResourceStatus::Message response;
  ASSERT_EQ(decoder.Read(response), OkStatus());

  EXPECT_EQ(response.resource_id, 27u);
}

}  // namespace
}  // namespace pw::transfer::test
