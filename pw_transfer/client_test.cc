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

#include "pw_transfer/client.h"

#include <cstring>

#include "pw_assert/check.h"
#include "pw_bytes/array.h"
#include "pw_rpc/raw/client_testing.h"
#include "pw_rpc/test_helpers.h"
#include "pw_status/status.h"
#include "pw_thread/thread.h"
#include "pw_thread_stl/options.h"
#include "pw_transfer/internal/config.h"
#include "pw_transfer_private/chunk_testing.h"
#include "pw_unit_test/framework.h"

namespace pw::transfer::test {
namespace {

using internal::Chunk;
using pw_rpc::raw::Transfer;

using namespace std::chrono_literals;

thread::Options& TransferThreadOptions() {
  static thread::stl::Options options;
  return options;
}

class ReadTransfer : public ::testing::Test {
 protected:
  ReadTransfer(size_t max_bytes_to_receive = 0)
      : transfer_thread_(chunk_buffer_, encode_buffer_),
        legacy_client_(context_.client(),
                       context_.channel().id(),
                       transfer_thread_,
                       max_bytes_to_receive > 0
                           ? max_bytes_to_receive
                           : transfer_thread_.max_chunk_size()),
        client_(context_.client(),
                context_.channel().id(),
                transfer_thread_,
                max_bytes_to_receive > 0 ? max_bytes_to_receive
                                         : transfer_thread_.max_chunk_size()),
        system_thread_(TransferThreadOptions(), transfer_thread_) {
    legacy_client_.set_protocol_version(ProtocolVersion::kLegacy);
  }

  ~ReadTransfer() override {
    transfer_thread_.Terminate();
    system_thread_.join();
  }

  rpc::RawClientTestContext<> context_;

  Thread<1, 1> transfer_thread_;
  Client legacy_client_;
  Client client_;

  std::array<std::byte, 64> chunk_buffer_;
  std::array<std::byte, 64> encode_buffer_;

  pw::Thread system_thread_;
};

constexpr auto kData32 = bytes::Initialized<32>([](size_t i) { return i; });
constexpr auto kData64 = bytes::Initialized<64>([](size_t i) { return i; });
constexpr auto kData256 = bytes::Initialized<256>([](size_t i) { return i; });

TEST_F(ReadTransfer, SingleChunk) {
  stream::MemoryWriterBuffer<64> writer;
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(
      OkStatus(),
      legacy_client_
          .Read(3,
                writer,
                [&transfer_status](Status status) { transfer_status = status; })
          .status());

  transfer_thread_.WaitUntilEventIsProcessed();

  // First transfer parameters chunk is sent.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Read>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c0 = DecodeChunk(payloads[0]);
  EXPECT_EQ(c0.session_id(), 3u);
  EXPECT_EQ(c0.resource_id(), 3u);
  EXPECT_EQ(c0.offset(), 0u);
  EXPECT_EQ(c0.window_end_offset(), 37u);
  EXPECT_EQ(c0.type(), Chunk::Type::kStart);

  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(3)
                      .set_offset(0)
                      .set_payload(kData32)
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 2u);

  Chunk c1 = DecodeChunk(payloads.back());
  EXPECT_EQ(c1.session_id(), 3u);
  ASSERT_TRUE(c1.status().has_value());
  EXPECT_EQ(c1.status().value(), OkStatus());

  EXPECT_EQ(transfer_status, OkStatus());
  EXPECT_EQ(std::memcmp(writer.data(), kData32.data(), writer.bytes_written()),
            0);
}

TEST_F(ReadTransfer, MultiChunk) {
  stream::MemoryWriterBuffer<64> writer;
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(
      OkStatus(),
      legacy_client_
          .Read(4,
                writer,
                [&transfer_status](Status status) { transfer_status = status; })
          .status());

  transfer_thread_.WaitUntilEventIsProcessed();

  // First transfer parameters chunk is sent.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Read>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c0 = DecodeChunk(payloads[0]);
  EXPECT_EQ(c0.session_id(), 4u);
  EXPECT_EQ(c0.resource_id(), 4u);
  EXPECT_EQ(c0.offset(), 0u);
  EXPECT_EQ(c0.window_end_offset(), 37u);
  EXPECT_EQ(c0.type(), Chunk::Type::kStart);

  constexpr ConstByteSpan data(kData32);
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(4)
                      .set_offset(0)
                      .set_payload(data.first(16))));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 1u);

  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(4)
                      .set_offset(16)
                      .set_payload(data.subspan(16))
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 2u);

  Chunk c1 = DecodeChunk(payloads[1]);
  EXPECT_EQ(c1.session_id(), 4u);
  ASSERT_TRUE(c1.status().has_value());
  EXPECT_EQ(c1.status().value(), OkStatus());

  EXPECT_EQ(transfer_status, OkStatus());
  EXPECT_EQ(std::memcmp(writer.data(), kData32.data(), writer.bytes_written()),
            0);
}

TEST_F(ReadTransfer, MultipleTransfers) {
  stream::MemoryWriterBuffer<64> writer;
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(
      OkStatus(),
      legacy_client_
          .Read(3,
                writer,
                [&transfer_status](Status status) { transfer_status = status; })
          .status());
  transfer_thread_.WaitUntilEventIsProcessed();

  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(3)
                      .set_offset(0)
                      .set_payload(kData32)
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(transfer_status, OkStatus());
  transfer_status = Status::Unknown();

  ASSERT_EQ(
      OkStatus(),
      legacy_client_
          .Read(3,
                writer,
                [&transfer_status](Status status) { transfer_status = status; })
          .status());
  transfer_thread_.WaitUntilEventIsProcessed();

  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(3)
                      .set_offset(0)
                      .set_payload(kData32)
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_EQ(transfer_status, OkStatus());
}

class ReadTransferMaxBytes32 : public ReadTransfer {
 protected:
  ReadTransferMaxBytes32() : ReadTransfer(/*max_bytes_to_receive=*/32) {}
};

TEST_F(ReadTransferMaxBytes32, SetsPendingBytesFromConstructorArg) {
  stream::MemoryWriterBuffer<64> writer;
  EXPECT_EQ(OkStatus(), legacy_client_.Read(5, writer, [](Status) {}).status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // First transfer parameters chunk is sent.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Read>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);

  Chunk c0 = DecodeChunk(payloads[0]);
  EXPECT_EQ(c0.session_id(), 5u);
  EXPECT_EQ(c0.resource_id(), 5u);
  EXPECT_EQ(c0.offset(), 0u);
  EXPECT_EQ(c0.window_end_offset(), 32u);
  EXPECT_EQ(c0.type(), Chunk::Type::kStart);
}

TEST_F(ReadTransferMaxBytes32, SetsPendingBytesFromWriterLimit) {
  stream::MemoryWriterBuffer<16> small_writer;
  EXPECT_EQ(OkStatus(),
            legacy_client_.Read(5, small_writer, [](Status) {}).status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // First transfer parameters chunk is sent.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Read>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);

  Chunk c0 = DecodeChunk(payloads[0]);
  EXPECT_EQ(c0.session_id(), 5u);
  EXPECT_EQ(c0.resource_id(), 5u);
  EXPECT_EQ(c0.offset(), 0u);
  EXPECT_EQ(c0.window_end_offset(), 16u);
  EXPECT_EQ(c0.type(), Chunk::Type::kStart);
}

TEST_F(ReadTransferMaxBytes32, MultiParameters) {
  stream::MemoryWriterBuffer<64> writer;
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(
      OkStatus(),
      legacy_client_
          .Read(6,
                writer,
                [&transfer_status](Status status) { transfer_status = status; })
          .status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // First transfer parameters chunk is sent.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Read>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c0 = DecodeChunk(payloads[0]);
  EXPECT_EQ(c0.session_id(), 6u);
  EXPECT_EQ(c0.resource_id(), 6u);
  EXPECT_EQ(c0.offset(), 0u);
  ASSERT_EQ(c0.window_end_offset(), 32u);

  constexpr ConstByteSpan data(kData64);
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(6)
                      .set_offset(0)
                      .set_payload(data.first(32))));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 2u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  // Second parameters chunk.
  Chunk c1 = DecodeChunk(payloads[1]);
  EXPECT_EQ(c1.session_id(), 6u);
  EXPECT_EQ(c1.offset(), 32u);
  ASSERT_EQ(c1.window_end_offset(), 64u);

  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(6)
                      .set_offset(32)
                      .set_payload(data.subspan(32))
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 3u);

  Chunk c2 = DecodeChunk(payloads[2]);
  EXPECT_EQ(c2.session_id(), 6u);
  ASSERT_TRUE(c2.status().has_value());
  EXPECT_EQ(c2.status().value(), OkStatus());

  EXPECT_EQ(transfer_status, OkStatus());
  EXPECT_EQ(std::memcmp(writer.data(), data.data(), writer.bytes_written()), 0);
}

TEST_F(ReadTransfer, UnexpectedOffset) {
  stream::MemoryWriterBuffer<64> writer;
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(
      OkStatus(),
      legacy_client_
          .Read(7,
                writer,
                [&transfer_status](Status status) { transfer_status = status; })
          .status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // First transfer parameters chunk is sent.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Read>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c0 = DecodeChunk(payloads[0]);
  EXPECT_EQ(c0.session_id(), 7u);
  EXPECT_EQ(c0.resource_id(), 7u);
  EXPECT_EQ(c0.offset(), 0u);
  EXPECT_EQ(c0.window_end_offset(), 37u);

  constexpr ConstByteSpan data(kData32);
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(0)
                      .set_payload(data.first(16))));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  // Send a chunk with an incorrect offset. The client should resend parameters.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(8)  // wrong!
                      .set_payload(data.subspan(16))
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 2u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c1 = DecodeChunk(payloads[1]);
  EXPECT_EQ(c1.session_id(), 7u);
  EXPECT_EQ(c1.offset(), 16u);
  EXPECT_EQ(c1.window_end_offset(), 53u);

  // Send the correct chunk, completing the transfer.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(7)
                      .set_offset(16)
                      .set_payload(data.subspan(16))
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 3u);

  Chunk c2 = DecodeChunk(payloads[2]);
  EXPECT_EQ(c2.session_id(), 7u);
  ASSERT_TRUE(c2.status().has_value());
  EXPECT_EQ(c2.status().value(), OkStatus());

  EXPECT_EQ(transfer_status, OkStatus());
  EXPECT_EQ(std::memcmp(writer.data(), kData32.data(), writer.bytes_written()),
            0);
}

TEST_F(ReadTransferMaxBytes32, TooMuchData_EntersRecovery) {
  stream::MemoryWriterBuffer<32> writer;
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(
      OkStatus(),
      legacy_client_
          .Read(8,
                writer,
                [&transfer_status](Status status) { transfer_status = status; })
          .status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // First transfer parameters chunk is sent.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Read>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c0 = DecodeChunk(payloads[0]);
  EXPECT_EQ(c0.session_id(), 8u);
  EXPECT_EQ(c0.resource_id(), 8u);
  EXPECT_EQ(c0.offset(), 0u);
  ASSERT_EQ(c0.window_end_offset(), 32u);

  constexpr ConstByteSpan data(kData64);

  // pending_bytes == 32
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(8)
                      .set_offset(0)
                      .set_payload(data.first(16))));

  // pending_bytes == 16
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(8)
                      .set_offset(16)
                      .set_payload(data.subspan(16, 8))));

  // pending_bytes == 8, send 16 instead.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(8)
                      .set_offset(24)
                      .set_payload(data.subspan(24, 16))));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 4u);

  // The device should resend a parameters chunk.
  Chunk c1 = DecodeChunk(payloads[3]);
  EXPECT_EQ(c1.session_id(), 8u);
  EXPECT_EQ(c1.type(), Chunk::Type::kParametersRetransmit);
  EXPECT_EQ(c1.offset(), 24u);
  EXPECT_EQ(c1.window_end_offset(), 32u);

  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(8)
                      .set_offset(24)
                      .set_payload(data.subspan(24, 8))
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_EQ(transfer_status, OkStatus());
}

TEST_F(ReadTransferMaxBytes32, TooMuchData_HitsLifetimeRetries) {
  stream::MemoryWriterBuffer<32> writer;
  Status transfer_status = Status::Unknown();

  constexpr int kLowMaxLifetimeRetries = 3;
  legacy_client_.set_max_lifetime_retries(kLowMaxLifetimeRetries).IgnoreError();

  ASSERT_EQ(
      OkStatus(),
      legacy_client_
          .Read(8,
                writer,
                [&transfer_status](Status status) { transfer_status = status; })
          .status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // First transfer parameters chunk is sent.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Read>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c0 = DecodeChunk(payloads[0]);
  EXPECT_EQ(c0.session_id(), 8u);
  EXPECT_EQ(c0.resource_id(), 8u);
  EXPECT_EQ(c0.offset(), 0u);
  ASSERT_EQ(c0.window_end_offset(), 32u);

  constexpr ConstByteSpan data(kData64);

  // pending_bytes == 32
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(8)
                      .set_offset(0)
                      .set_payload(data.first(16))));

  // pending_bytes == 16
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(8)
                      .set_offset(16)
                      .set_payload(data.subspan(16, 8))));

  // pending_bytes == 8, but send 16 several times.
  for (int i = 0; i < kLowMaxLifetimeRetries; ++i) {
    context_.server().SendServerStream<Transfer::Read>(
        EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                        .set_session_id(8)
                        .set_offset(24)
                        .set_payload(data.subspan(24, 16))));
    transfer_thread_.WaitUntilEventIsProcessed();

    ASSERT_EQ(payloads.size(), 4u + i);

    // The device should resend a parameters chunk.
    Chunk c = DecodeChunk(payloads.back());
    EXPECT_EQ(c.session_id(), 8u);
    EXPECT_EQ(c.type(), Chunk::Type::kParametersRetransmit);
  }
  EXPECT_EQ(transfer_status, Status::Unknown());

  // Send one more incorrectly-sized chunk. The transfer should fail.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(8)
                      .set_offset(24)
                      .set_payload(data.subspan(24, 16))));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 7u);
  Chunk error = DecodeChunk(payloads.back());
  EXPECT_EQ(error.session_id(), 8u);
  EXPECT_EQ(error.type(), Chunk::Type::kCompletion);
  EXPECT_EQ(error.status(), Status::Internal());

  EXPECT_EQ(transfer_status, Status::Internal());
}

TEST_F(ReadTransfer, ServerError) {
  stream::MemoryWriterBuffer<64> writer;
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(
      OkStatus(),
      legacy_client_
          .Read(9,
                writer,
                [&transfer_status](Status status) { transfer_status = status; })
          .status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // First transfer parameters chunk is sent.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Read>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c0 = DecodeChunk(payloads[0]);
  EXPECT_EQ(c0.session_id(), 9u);
  EXPECT_EQ(c0.resource_id(), 9u);
  EXPECT_EQ(c0.offset(), 0u);
  ASSERT_EQ(c0.window_end_offset(), 37u);

  // Server sends an error. Client should not respond and terminate the
  // transfer.
  context_.server().SendServerStream<Transfer::Read>(EncodeChunk(
      Chunk::Final(ProtocolVersion::kLegacy, 9, Status::NotFound())));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::NotFound());
}

TEST_F(ReadTransfer, OnlySendsParametersOnceAfterDrop) {
  stream::MemoryWriterBuffer<64> writer;
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(
      OkStatus(),
      legacy_client_
          .Read(10,
                writer,
                [&transfer_status](Status status) { transfer_status = status; })
          .status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // First transfer parameters chunk is sent.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Read>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c0 = DecodeChunk(payloads[0]);
  EXPECT_EQ(c0.session_id(), 10u);
  EXPECT_EQ(c0.resource_id(), 10u);
  EXPECT_EQ(c0.offset(), 0u);
  ASSERT_EQ(c0.window_end_offset(), 37u);

  constexpr ConstByteSpan data(kData32);

  // Send the first 8 bytes of the transfer.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(10)
                      .set_offset(0)
                      .set_payload(data.first(8))));

  // Skip offset 8, send the rest starting from 16.
  for (uint32_t offset = 16; offset < data.size(); offset += 8) {
    context_.server().SendServerStream<Transfer::Read>(
        EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                        .set_session_id(10)
                        .set_offset(offset)
                        .set_payload(data.subspan(offset, 8))));
  }
  transfer_thread_.WaitUntilEventIsProcessed();

  // Only one parameters update should be sent, with the offset of the initial
  // dropped packet.
  ASSERT_EQ(payloads.size(), 2u);

  Chunk c1 = DecodeChunk(payloads[1]);
  EXPECT_EQ(c1.session_id(), 10u);
  EXPECT_EQ(c1.offset(), 8u);
  ASSERT_EQ(c1.window_end_offset(), 45u);

  // Send the remaining data to complete the transfer.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(10)
                      .set_offset(8)
                      .set_payload(data.subspan(8))
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 3u);

  Chunk c2 = DecodeChunk(payloads[2]);
  EXPECT_EQ(c2.session_id(), 10u);
  ASSERT_TRUE(c2.status().has_value());
  EXPECT_EQ(c2.status().value(), OkStatus());

  EXPECT_EQ(transfer_status, OkStatus());
}

TEST_F(ReadTransfer, ResendsParametersIfSentRepeatedChunkDuringRecovery) {
  stream::MemoryWriterBuffer<64> writer;
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(
      OkStatus(),
      legacy_client_
          .Read(11,
                writer,
                [&transfer_status](Status status) { transfer_status = status; })
          .status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // First transfer parameters chunk is sent.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Read>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c0 = DecodeChunk(payloads[0]);
  EXPECT_EQ(c0.session_id(), 11u);
  EXPECT_EQ(c0.resource_id(), 11u);
  EXPECT_EQ(c0.offset(), 0u);
  ASSERT_EQ(c0.window_end_offset(), 37u);

  constexpr ConstByteSpan data(kData32);

  // Send the first 8 bytes of the transfer.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(11)
                      .set_offset(0)
                      .set_payload(data.first(8))));

  // Skip offset 8, send the rest starting from 16.
  for (uint32_t offset = 16; offset < data.size(); offset += 8) {
    context_.server().SendServerStream<Transfer::Read>(
        EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                        .set_session_id(11)
                        .set_offset(offset)
                        .set_payload(data.subspan(offset, 8))));
  }
  transfer_thread_.WaitUntilEventIsProcessed();

  // Only one parameters update should be sent, with the offset of the initial
  // dropped packet.
  ASSERT_EQ(payloads.size(), 2u);

  const Chunk last_chunk = Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                               .set_session_id(11)
                               .set_offset(24)
                               .set_payload(data.subspan(24));

  // Re-send the final chunk of the block.
  context_.server().SendServerStream<Transfer::Read>(EncodeChunk(last_chunk));
  transfer_thread_.WaitUntilEventIsProcessed();

  // The original drop parameters should be re-sent.
  ASSERT_EQ(payloads.size(), 3u);
  Chunk c2 = DecodeChunk(payloads[2]);
  EXPECT_EQ(c2.session_id(), 11u);
  EXPECT_EQ(c2.offset(), 8u);
  ASSERT_EQ(c2.window_end_offset(), 45u);

  // Do it again.
  context_.server().SendServerStream<Transfer::Read>(EncodeChunk(last_chunk));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 4u);
  Chunk c3 = DecodeChunk(payloads[3]);
  EXPECT_EQ(c3.session_id(), 11u);
  EXPECT_EQ(c3.offset(), 8u);
  ASSERT_EQ(c3.window_end_offset(), 45u);

  // Finish the transfer normally.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(11)
                      .set_offset(8)
                      .set_payload(data.subspan(8))
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 5u);

  Chunk c4 = DecodeChunk(payloads[4]);
  EXPECT_EQ(c4.session_id(), 11u);
  ASSERT_TRUE(c4.status().has_value());
  EXPECT_EQ(c4.status().value(), OkStatus());

  EXPECT_EQ(transfer_status, OkStatus());
}

// Use a long timeout to avoid accidentally triggering timeouts.
constexpr chrono::SystemClock::duration kTestTimeout = std::chrono::seconds(30);
constexpr uint8_t kTestRetries = 3;

TEST_F(ReadTransfer, Timeout_ResendsCurrentParameters) {
  stream::MemoryWriterBuffer<64> writer;
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(
      OkStatus(),
      legacy_client_
          .Read(
              12,
              writer,
              [&transfer_status](Status status) { transfer_status = status; },
              kTestTimeout,
              kTestTimeout)
          .status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // First transfer parameters chunk is sent.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Read>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c0 = DecodeChunk(payloads.back());
  EXPECT_EQ(c0.session_id(), 12u);
  EXPECT_EQ(c0.resource_id(), 12u);
  EXPECT_EQ(c0.offset(), 0u);
  EXPECT_EQ(c0.window_end_offset(), 37u);
  EXPECT_EQ(c0.type(), Chunk::Type::kStart);

  // Wait for the timeout to expire without doing anything. The client should
  // resend its initial parameters chunk.
  transfer_thread_.SimulateClientTimeout(12);
  ASSERT_EQ(payloads.size(), 2u);

  Chunk c = DecodeChunk(payloads.back());
  EXPECT_EQ(c.session_id(), 12u);
  EXPECT_EQ(c.offset(), 0u);
  EXPECT_EQ(c.window_end_offset(), 37u);
  EXPECT_EQ(c0.type(), Chunk::Type::kStart);

  // Transfer has not yet completed.
  EXPECT_EQ(transfer_status, Status::Unknown());

  // Finish the transfer following the timeout.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(12)
                      .set_offset(0)
                      .set_payload(kData32)
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 3u);

  Chunk c4 = DecodeChunk(payloads.back());
  EXPECT_EQ(c4.session_id(), 12u);
  ASSERT_TRUE(c4.status().has_value());
  EXPECT_EQ(c4.status().value(), OkStatus());

  EXPECT_EQ(transfer_status, OkStatus());
}

TEST_F(ReadTransfer, Timeout_ResendsUpdatedParameters) {
  stream::MemoryWriterBuffer<64> writer;
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(
      OkStatus(),
      legacy_client_
          .Read(
              13,
              writer,
              [&transfer_status](Status status) { transfer_status = status; },
              kTestTimeout)
          .status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // First transfer parameters chunk is sent.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Read>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c0 = DecodeChunk(payloads.back());
  EXPECT_EQ(c0.session_id(), 13u);
  EXPECT_EQ(c0.resource_id(), 13u);
  EXPECT_EQ(c0.offset(), 0u);
  EXPECT_EQ(c0.window_end_offset(), 37u);
  EXPECT_EQ(c0.type(), Chunk::Type::kStart);

  constexpr ConstByteSpan data(kData32);

  // Send some data, but not everything.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(13)
                      .set_offset(0)
                      .set_payload(data.first(16))));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 1u);

  // Wait for the timeout to expire without sending more data. The client should
  // send an updated parameters chunk, accounting for the data already received.
  transfer_thread_.SimulateClientTimeout(13);
  ASSERT_EQ(payloads.size(), 2u);

  Chunk c = DecodeChunk(payloads.back());
  EXPECT_EQ(c.session_id(), 13u);
  EXPECT_EQ(c.offset(), 16u);
  EXPECT_EQ(c.window_end_offset(), 53u);
  EXPECT_EQ(c.type(), Chunk::Type::kParametersRetransmit);

  // Transfer has not yet completed.
  EXPECT_EQ(transfer_status, Status::Unknown());

  // Send the rest of the data, finishing the transfer.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(13)
                      .set_offset(16)
                      .set_payload(data.subspan(16))
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 3u);

  Chunk c4 = DecodeChunk(payloads.back());
  EXPECT_EQ(c4.session_id(), 13u);
  ASSERT_TRUE(c4.status().has_value());
  EXPECT_EQ(c4.status().value(), OkStatus());

  EXPECT_EQ(transfer_status, OkStatus());
}

TEST_F(ReadTransfer, Timeout_EndsTransferAfterMaxRetries) {
  stream::MemoryWriterBuffer<64> writer;
  Status transfer_status = Status::Unknown();

  Result<Client::Handle> handle = legacy_client_.Read(
      14,
      writer,
      [&transfer_status](Status status) { transfer_status = status; },
      kTestTimeout,
      kTestTimeout);
  ASSERT_EQ(OkStatus(), handle.status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // First transfer parameters chunk is sent.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Read>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c0 = DecodeChunk(payloads.back());
  EXPECT_EQ(c0.session_id(), 14u);
  EXPECT_EQ(c0.resource_id(), 14u);
  EXPECT_EQ(c0.offset(), 0u);
  EXPECT_EQ(c0.window_end_offset(), 37u);
  EXPECT_EQ(c0.type(), Chunk::Type::kStart);

  for (unsigned retry = 1; retry <= kTestRetries; ++retry) {
    // Wait for the timeout to expire without doing anything. The client should
    // resend its parameters chunk.
    transfer_thread_.SimulateClientTimeout(14);
    ASSERT_EQ(payloads.size(), retry + 1);

    Chunk c = DecodeChunk(payloads.back());
    EXPECT_EQ(c.session_id(), 14u);
    EXPECT_EQ(c.offset(), 0u);
    EXPECT_EQ(c.window_end_offset(), 37u);

    // Transfer has not yet completed.
    EXPECT_EQ(transfer_status, Status::Unknown());
  }

  // Time out one more time after the final retry. The client should cancel the
  // transfer at this point. As no packets were received from the server, no
  // final status chunk should be sent.
  transfer_thread_.SimulateClientTimeout(14);
  ASSERT_EQ(payloads.size(), 4u);

  EXPECT_EQ(transfer_status, Status::DeadlineExceeded());

  // After finishing the transfer, nothing else should be sent.
  transfer_thread_.SimulateClientTimeout(14);
  transfer_thread_.SimulateClientTimeout(14);
  transfer_thread_.SimulateClientTimeout(14);
  ASSERT_EQ(payloads.size(), 4u);
}

TEST_F(ReadTransfer, Timeout_ReceivingDataResetsRetryCount) {
  stream::MemoryWriterBuffer<64> writer;
  Status transfer_status = Status::Unknown();

  constexpr ConstByteSpan data(kData32);

  Result<Client::Handle> handle = legacy_client_.Read(
      14,
      writer,
      [&transfer_status](Status status) { transfer_status = status; },
      kTestTimeout,
      kTestTimeout);
  ASSERT_EQ(OkStatus(), handle.status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // First transfer parameters chunk is sent.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Read>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c0 = DecodeChunk(payloads.back());
  EXPECT_EQ(c0.session_id(), 14u);
  EXPECT_EQ(c0.resource_id(), 14u);
  EXPECT_EQ(c0.offset(), 0u);
  EXPECT_EQ(c0.window_end_offset(), 37u);

  // Simulate one less timeout than the maximum amount of retries.
  for (unsigned retry = 1; retry <= kTestRetries - 1; ++retry) {
    transfer_thread_.SimulateClientTimeout(14);
    ASSERT_EQ(payloads.size(), retry + 1);

    Chunk c = DecodeChunk(payloads.back());
    EXPECT_EQ(c.session_id(), 14u);
    EXPECT_EQ(c.offset(), 0u);
    EXPECT_EQ(c.window_end_offset(), 37u);

    // Transfer has not yet completed.
    EXPECT_EQ(transfer_status, Status::Unknown());
  }

  // Send some data.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(14)
                      .set_offset(0)
                      .set_payload(data.first(16))));
  transfer_thread_.WaitUntilEventIsProcessed();
  ASSERT_EQ(payloads.size(), 3u);

  // Time out a couple more times. The context's retry count should have been
  // reset, so it should go through the standard retry flow instead of
  // terminating the transfer.
  transfer_thread_.SimulateClientTimeout(14);
  ASSERT_EQ(payloads.size(), 4u);

  Chunk c = DecodeChunk(payloads.back());
  EXPECT_FALSE(c.status().has_value());
  EXPECT_EQ(c.session_id(), 14u);
  EXPECT_EQ(c.offset(), 16u);
  EXPECT_EQ(c.window_end_offset(), 53u);

  transfer_thread_.SimulateClientTimeout(14);
  ASSERT_EQ(payloads.size(), 5u);

  c = DecodeChunk(payloads.back());
  EXPECT_FALSE(c.status().has_value());
  EXPECT_EQ(c.session_id(), 14u);
  EXPECT_EQ(c.offset(), 16u);
  EXPECT_EQ(c.window_end_offset(), 53u);

  // Ensure we don't leave a dangling reference to transfer_status.
  handle->Cancel();
  transfer_thread_.WaitUntilEventIsProcessed();
}

TEST_F(ReadTransfer, InitialPacketFails_OnCompletedCalledWithDataLoss) {
  stream::MemoryWriterBuffer<64> writer;
  Status transfer_status = Status::Unknown();

  context_.output().set_send_status(Status::Unauthenticated());

  const auto on_completed = [&transfer_status](Status status) {
    ASSERT_EQ(transfer_status,
              Status::Unknown());  // Must only call once
    transfer_status = status;
  };

  ASSERT_EQ(
      OkStatus(),
      legacy_client_.Read(14, writer, on_completed, kTestTimeout).status());
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_EQ(transfer_status, Status::Internal());
}

TEST_F(ReadTransfer, HijackedStreamIsCancelled) {
  stream::MemoryWriterBuffer<64> writer1;
  Status transfer_status1 = Status::Unknown();

  ASSERT_EQ(OkStatus(),
            legacy_client_
                .Read(3,
                      writer1,
                      [&transfer_status1](Status status) {
                        transfer_status1 = status;
                      })
                .status());
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(legacy_client_.has_read_stream());

  // A new client (client_) attempts a read transfer using the same thread.
  // This will steal the read stream from legacy_client_.
  stream::MemoryWriterBuffer<64> writer2;
  Status transfer_status2 = Status::Unknown();

  Result<Client::Handle> handle =
      client_.Read(4, writer2, [&transfer_status2](Status status) {
        transfer_status2 = status;
      });
  ASSERT_EQ(OkStatus(), handle.status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // legacy_client_ should have its read stream cancelled and marked false.
  EXPECT_FALSE(legacy_client_.has_read_stream());
  EXPECT_TRUE(client_.has_read_stream());
  EXPECT_EQ(transfer_status1, Status::Aborted());

  handle->Cancel();
  transfer_thread_.WaitUntilEventIsProcessed();
}

class WriteTransfer : public ::testing::Test {
 protected:
  WriteTransfer()
      : transfer_thread_(chunk_buffer_, encode_buffer_),
        legacy_client_(context_.client(),
                       context_.channel().id(),
                       transfer_thread_,
                       transfer_thread_.max_chunk_size()),
        client_(context_.client(),
                context_.channel().id(),
                transfer_thread_,
                transfer_thread_.max_chunk_size()),
        system_thread_(TransferThreadOptions(), transfer_thread_) {
    legacy_client_.set_protocol_version(ProtocolVersion::kLegacy);
  }

  ~WriteTransfer() override {
    transfer_thread_.Terminate();
    system_thread_.join();
  }

  rpc::RawClientTestContext<> context_;

  Thread<1, 1> transfer_thread_;
  Client legacy_client_;
  Client client_;

  std::array<std::byte, 64> chunk_buffer_;
  std::array<std::byte, 64> encode_buffer_;

  pw::Thread system_thread_;
};

TEST_F(WriteTransfer, SingleChunk) {
  stream::MemoryReader reader(kData32);
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(OkStatus(),
            legacy_client_
                .Write(3,
                       reader,
                       [&transfer_status](Status status) {
                         transfer_status = status;
                       })
                .status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // The client begins by sending the ID of the resource to transfer.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Write>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c0 = DecodeChunk(payloads[0]);
  EXPECT_EQ(c0.session_id(), 3u);
  EXPECT_EQ(c0.resource_id(), 3u);
  EXPECT_EQ(c0.type(), Chunk::Type::kStart);

  // Send transfer parameters. Client should send a data chunk and the final
  // chunk.
  rpc::test::WaitForPackets(context_.output(), 2, [this] {
    context_.server().SendServerStream<Transfer::Write>(EncodeChunk(
        Chunk(ProtocolVersion::kLegacy, Chunk::Type::kParametersRetransmit)
            .set_session_id(3)
            .set_offset(0)
            .set_window_end_offset(64)
            .set_max_chunk_size_bytes(32)));
  });

  ASSERT_EQ(payloads.size(), 3u);

  Chunk c1 = DecodeChunk(payloads[1]);
  EXPECT_EQ(c1.session_id(), 3u);
  EXPECT_EQ(c1.offset(), 0u);
  EXPECT_TRUE(c1.has_payload());
  EXPECT_EQ(
      std::memcmp(c1.payload().data(), kData32.data(), c1.payload().size()), 0);

  Chunk c2 = DecodeChunk(payloads[2]);
  EXPECT_EQ(c2.session_id(), 3u);
  ASSERT_TRUE(c2.remaining_bytes().has_value());
  EXPECT_EQ(c2.remaining_bytes().value(), 0u);

  EXPECT_EQ(transfer_status, Status::Unknown());

  // Send the final status chunk to complete the transfer.
  context_.server().SendServerStream<Transfer::Write>(
      EncodeChunk(Chunk::Final(ProtocolVersion::kLegacy, 3, OkStatus())));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_EQ(payloads.size(), 3u);
  EXPECT_EQ(transfer_status, OkStatus());
}

TEST_F(WriteTransfer, MultiChunk) {
  stream::MemoryReader reader(kData32);
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(OkStatus(),
            legacy_client_
                .Write(4,
                       reader,
                       [&transfer_status](Status status) {
                         transfer_status = status;
                       })
                .status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // The client begins by sending the ID of the resource to transfer.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Write>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c0 = DecodeChunk(payloads[0]);
  EXPECT_EQ(c0.session_id(), 4u);
  EXPECT_EQ(c0.resource_id(), 4u);
  EXPECT_EQ(c0.type(), Chunk::Type::kStart);

  // Send transfer parameters with a chunk size smaller than the data.

  // Client should send two data chunks and the final chunk.
  rpc::test::WaitForPackets(context_.output(), 3, [this] {
    context_.server().SendServerStream<Transfer::Write>(EncodeChunk(
        Chunk(ProtocolVersion::kLegacy, Chunk::Type::kParametersRetransmit)
            .set_session_id(4)
            .set_offset(0)
            .set_window_end_offset(64)
            .set_max_chunk_size_bytes(16)));
  });

  ASSERT_EQ(payloads.size(), 4u);

  Chunk c1 = DecodeChunk(payloads[1]);
  EXPECT_EQ(c1.session_id(), 4u);
  EXPECT_EQ(c1.offset(), 0u);
  EXPECT_TRUE(c1.has_payload());
  EXPECT_EQ(
      std::memcmp(c1.payload().data(), kData32.data(), c1.payload().size()), 0);

  Chunk c2 = DecodeChunk(payloads[2]);
  EXPECT_EQ(c2.session_id(), 4u);
  EXPECT_EQ(c2.offset(), 16u);
  EXPECT_TRUE(c2.has_payload());
  EXPECT_EQ(std::memcmp(c2.payload().data(),
                        kData32.data() + c2.offset(),
                        c2.payload().size()),
            0);

  Chunk c3 = DecodeChunk(payloads[3]);
  EXPECT_EQ(c3.session_id(), 4u);
  ASSERT_TRUE(c3.remaining_bytes().has_value());
  EXPECT_EQ(c3.remaining_bytes().value(), 0u);

  EXPECT_EQ(transfer_status, Status::Unknown());

  // Send the final status chunk to complete the transfer.
  context_.server().SendServerStream<Transfer::Write>(
      EncodeChunk(Chunk::Final(ProtocolVersion::kLegacy, 4, OkStatus())));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_EQ(payloads.size(), 4u);
  EXPECT_EQ(transfer_status, OkStatus());
}

TEST_F(WriteTransfer, OutOfOrder_SeekSupported) {
  stream::MemoryReader reader(kData32);
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(OkStatus(),
            legacy_client_
                .Write(5,
                       reader,
                       [&transfer_status](Status status) {
                         transfer_status = status;
                       })
                .status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // The client begins by sending the ID of the resource to transfer.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Write>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c0 = DecodeChunk(payloads[0]);
  EXPECT_EQ(c0.session_id(), 5u);
  EXPECT_EQ(c0.resource_id(), 5u);
  EXPECT_EQ(c0.type(), Chunk::Type::kStart);

  // Send transfer parameters with a nonzero offset, requesting a seek.
  // Client should send a data chunk and the final chunk.
  rpc::test::WaitForPackets(context_.output(), 2, [this] {
    context_.server().SendServerStream<Transfer::Write>(EncodeChunk(
        Chunk(ProtocolVersion::kLegacy, Chunk::Type::kParametersRetransmit)
            .set_session_id(5)
            .set_offset(16)
            .set_window_end_offset(64)
            .set_max_chunk_size_bytes(32)));
  });

  ASSERT_EQ(payloads.size(), 3u);

  Chunk c1 = DecodeChunk(payloads[1]);
  EXPECT_EQ(c1.session_id(), 5u);
  EXPECT_EQ(c1.offset(), 16u);
  EXPECT_TRUE(c1.has_payload());
  EXPECT_EQ(std::memcmp(c1.payload().data(),
                        kData32.data() + c1.offset(),
                        c1.payload().size()),
            0);

  Chunk c2 = DecodeChunk(payloads[2]);
  EXPECT_EQ(c2.session_id(), 5u);
  ASSERT_TRUE(c2.remaining_bytes().has_value());
  EXPECT_EQ(c2.remaining_bytes().value(), 0u);

  EXPECT_EQ(transfer_status, Status::Unknown());

  // Send the final status chunk to complete the transfer.
  context_.server().SendServerStream<Transfer::Write>(
      EncodeChunk(Chunk::Final(ProtocolVersion::kLegacy, 5, OkStatus())));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_EQ(payloads.size(), 3u);
  EXPECT_EQ(transfer_status, OkStatus());
}

class FakeNonSeekableReader final : public stream::NonSeekableReader {
 public:
  FakeNonSeekableReader(ConstByteSpan data) : data_(data), position_(0) {}

 private:
  StatusWithSize DoRead(ByteSpan out) final {
    if (position_ == data_.size()) {
      return StatusWithSize::OutOfRange();
    }

    size_t to_copy = std::min(out.size(), data_.size() - position_);
    std::memcpy(out.data(), data_.data() + position_, to_copy);
    position_ += to_copy;

    return StatusWithSize(to_copy);
  }

  ConstByteSpan data_;
  size_t position_;
};

TEST_F(WriteTransfer, OutOfOrder_SeekNotSupported) {
  FakeNonSeekableReader reader(kData32);
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(OkStatus(),
            legacy_client_
                .Write(6,
                       reader,
                       [&transfer_status](Status status) {
                         transfer_status = status;
                       })
                .status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // The client begins by sending the ID of the resource to transfer.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Write>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c0 = DecodeChunk(payloads[0]);
  EXPECT_EQ(c0.session_id(), 6u);
  EXPECT_EQ(c0.resource_id(), 6u);
  EXPECT_EQ(c0.type(), Chunk::Type::kStart);

  // Send transfer parameters with a nonzero offset, requesting a seek.
  context_.server().SendServerStream<Transfer::Write>(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kParametersRetransmit)
          .set_session_id(6)
          .set_offset(16)
          .set_window_end_offset(64)
          .set_max_chunk_size_bytes(32)));
  transfer_thread_.WaitUntilEventIsProcessed();

  // Client should send a status chunk and end the transfer.
  ASSERT_EQ(payloads.size(), 2u);

  Chunk c1 = DecodeChunk(payloads[1]);
  EXPECT_EQ(c1.session_id(), 6u);
  EXPECT_EQ(c1.type(), Chunk::Type::kCompletion);
  ASSERT_TRUE(c1.status().has_value());
  EXPECT_EQ(c1.status().value(), Status::Unimplemented());

  EXPECT_EQ(transfer_status, Status::Unimplemented());
}

TEST_F(WriteTransfer, ServerError) {
  stream::MemoryReader reader(kData32);
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(OkStatus(),
            legacy_client_
                .Write(7,
                       reader,
                       [&transfer_status](Status status) {
                         transfer_status = status;
                       })
                .status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // The client begins by sending the ID of the resource to transfer.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Write>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c0 = DecodeChunk(payloads[0]);
  EXPECT_EQ(c0.session_id(), 7u);
  EXPECT_EQ(c0.resource_id(), 7u);
  EXPECT_EQ(c0.type(), Chunk::Type::kStart);

  // Send an error from the server.
  context_.server().SendServerStream<Transfer::Write>(EncodeChunk(
      Chunk::Final(ProtocolVersion::kLegacy, 7, Status::NotFound())));
  transfer_thread_.WaitUntilEventIsProcessed();

  // Client should not respond and terminate the transfer.
  EXPECT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::NotFound());
}

TEST_F(WriteTransfer, AbortIfZeroBytesAreRequested) {
  stream::MemoryReader reader(kData32);
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(OkStatus(),
            legacy_client_
                .Write(9,
                       reader,
                       [&transfer_status](Status status) {
                         transfer_status = status;
                       })
                .status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // The client begins by sending the ID of the resource to transfer.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Write>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c0 = DecodeChunk(payloads[0]);
  EXPECT_EQ(c0.session_id(), 9u);
  EXPECT_EQ(c0.resource_id(), 9u);
  EXPECT_EQ(c0.type(), Chunk::Type::kStart);

  // Send an invalid transfer parameters chunk with 0 pending bytes.
  context_.server().SendServerStream<Transfer::Write>(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kParametersRetransmit)
          .set_session_id(9)
          .set_offset(0)
          .set_window_end_offset(0)
          .set_max_chunk_size_bytes(32)));
  transfer_thread_.WaitUntilEventIsProcessed();

  // Client should send a status chunk and end the transfer.
  ASSERT_EQ(payloads.size(), 2u);

  Chunk c1 = DecodeChunk(payloads[1]);
  EXPECT_EQ(c1.session_id(), 9u);
  ASSERT_TRUE(c1.status().has_value());
  EXPECT_EQ(c1.status().value(), Status::ResourceExhausted());

  EXPECT_EQ(transfer_status, Status::ResourceExhausted());
}

TEST_F(WriteTransfer, IgnoresEarlierWindowEndOffsetInContinueParameters) {
  stream::MemoryReader reader(kData32);
  Status transfer_status = Status::Unknown();

  Result<Client::Handle> handle =
      legacy_client_.Write(9, reader, [&transfer_status](Status status) {
        transfer_status = status;
      });
  ASSERT_EQ(handle.status(), OkStatus());
  transfer_thread_.WaitUntilEventIsProcessed();

  // The client begins by sending the ID of the resource to transfer.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Write>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c0 = DecodeChunk(payloads[0]);
  EXPECT_EQ(c0.session_id(), 9u);
  EXPECT_EQ(c0.resource_id(), 9u);
  EXPECT_EQ(c0.type(), Chunk::Type::kStart);

  context_.server().SendServerStream<Transfer::Write>(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kParametersRetransmit)
          .set_session_id(9)
          .set_offset(0)
          .set_window_end_offset(16)
          .set_max_chunk_size_bytes(16)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 2u);

  Chunk chunk = DecodeChunk(payloads[1]);
  EXPECT_EQ(chunk.session_id(), 9u);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.payload().size(), 16u);

  // Rewind the window end offset to earlier than the client's offset using a
  // CONTINUE chunk.
  context_.server().SendServerStream<Transfer::Write>(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kParametersContinue)
          .set_session_id(9)
          .set_offset(10)
          .set_window_end_offset(14)));
  transfer_thread_.WaitUntilEventIsProcessed();

  // The client should ignore it.
  ASSERT_EQ(payloads.size(), 2u);

  // Retry the same chunk as a RETRANSMIT.
  context_.server().SendServerStream<Transfer::Write>(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kParametersRetransmit)
          .set_session_id(9)
          .set_offset(10)
          .set_window_end_offset(14)));
  transfer_thread_.WaitUntilEventIsProcessed();

  // The client should respond correctly.
  ASSERT_EQ(payloads.size(), 3u);
  chunk = DecodeChunk(payloads[2]);
  EXPECT_EQ(chunk.session_id(), 9u);
  EXPECT_EQ(chunk.offset(), 10u);
  EXPECT_EQ(chunk.payload().size(), 4u);

  // Ensure we don't leave a dangling reference to transfer_status.
  handle->Cancel();
  transfer_thread_.WaitUntilEventIsProcessed();
}

TEST_F(WriteTransfer, Timeout_RetriesWithInitialChunk) {
  stream::MemoryReader reader(kData32);
  Status transfer_status = Status::Unknown();

  Result<Client::Handle> handle = legacy_client_.Write(
      10,
      reader,
      [&transfer_status](Status status) { transfer_status = status; },
      kTestTimeout,
      kTestTimeout);
  ASSERT_EQ(OkStatus(), handle.status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // The client begins by sending the ID of the resource to transfer.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Write>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c0 = DecodeChunk(payloads.back());
  EXPECT_EQ(c0.session_id(), 10u);
  EXPECT_EQ(c0.resource_id(), 10u);
  EXPECT_EQ(c0.type(), Chunk::Type::kStart);

  // Wait for the timeout to expire without doing anything. The client should
  // resend the initial transmit chunk.
  transfer_thread_.SimulateClientTimeout(10);
  ASSERT_EQ(payloads.size(), 2u);

  Chunk c = DecodeChunk(payloads.back());
  EXPECT_EQ(c.session_id(), 10u);
  EXPECT_EQ(c.resource_id(), 10u);
  EXPECT_EQ(c.type(), Chunk::Type::kStart);

  // Transfer has not yet completed.
  EXPECT_EQ(transfer_status, Status::Unknown());

  // Ensure we don't leave a dangling reference to transfer_status.
  handle->Cancel();
  transfer_thread_.WaitUntilEventIsProcessed();
}

TEST_F(WriteTransfer, Timeout_RetriesWithMostRecentChunk) {
  stream::MemoryReader reader(kData32);
  Status transfer_status = Status::Unknown();

  Result<Client::Handle> handle = legacy_client_.Write(
      11,
      reader,
      [&transfer_status](Status status) { transfer_status = status; },
      kTestTimeout);
  ASSERT_EQ(OkStatus(), handle.status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // The client begins by sending the ID of the resource to transfer.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Write>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c0 = DecodeChunk(payloads.back());
  EXPECT_EQ(c0.session_id(), 11u);
  EXPECT_EQ(c0.resource_id(), 11u);
  EXPECT_EQ(c0.type(), Chunk::Type::kStart);

  // Send the first parameters chunk.
  rpc::test::WaitForPackets(context_.output(), 2, [this] {
    context_.server().SendServerStream<Transfer::Write>(EncodeChunk(
        Chunk(ProtocolVersion::kLegacy, Chunk::Type::kParametersRetransmit)
            .set_session_id(11)
            .set_offset(0)
            .set_window_end_offset(16)
            .set_max_chunk_size_bytes(8)));
  });
  ASSERT_EQ(payloads.size(), 3u);

  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c1 = DecodeChunk(payloads[1]);
  EXPECT_EQ(c1.session_id(), 11u);
  EXPECT_EQ(c1.offset(), 0u);
  EXPECT_EQ(c1.payload().size(), 8u);
  EXPECT_EQ(
      std::memcmp(c1.payload().data(), kData32.data(), c1.payload().size()), 0);

  Chunk c2 = DecodeChunk(payloads[2]);
  EXPECT_EQ(c2.session_id(), 11u);
  EXPECT_EQ(c2.offset(), 8u);
  EXPECT_EQ(c2.payload().size(), 8u);
  EXPECT_EQ(std::memcmp(c2.payload().data(),
                        kData32.data() + c2.offset(),
                        c2.payload().size()),
            0);

  // Wait for the timeout to expire without doing anything. The client should
  // resend the most recently sent chunk.
  transfer_thread_.SimulateClientTimeout(11);
  ASSERT_EQ(payloads.size(), 4u);

  Chunk c3 = DecodeChunk(payloads[3]);
  EXPECT_EQ(c3.session_id(), c2.session_id());
  EXPECT_EQ(c3.offset(), c2.offset());
  EXPECT_EQ(c3.payload().size(), c2.payload().size());
  EXPECT_EQ(std::memcmp(
                c3.payload().data(), c2.payload().data(), c3.payload().size()),
            0);

  // Transfer has not yet completed.
  EXPECT_EQ(transfer_status, Status::Unknown());

  // Ensure we don't leave a dangling reference to transfer_status.
  handle->Cancel();
  transfer_thread_.WaitUntilEventIsProcessed();
}

TEST_F(WriteTransfer, Timeout_RetriesWithSingleChunkTransfer) {
  stream::MemoryReader reader(kData32);
  Status transfer_status = Status::Unknown();

  Result<Client::Handle> handle = legacy_client_.Write(
      12,
      reader,
      [&transfer_status](Status status) { transfer_status = status; },
      kTestTimeout);
  ASSERT_EQ(OkStatus(), handle.status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // The client begins by sending the ID of the resource to transfer.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Write>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c0 = DecodeChunk(payloads.back());
  EXPECT_EQ(c0.session_id(), 12u);
  EXPECT_EQ(c0.resource_id(), 12u);
  EXPECT_EQ(c0.type(), Chunk::Type::kStart);

  // Send the first parameters chunk, requesting all the data. The client should
  // respond with one data chunk and a remaining_bytes = 0 chunk.
  rpc::test::WaitForPackets(context_.output(), 2, [this] {
    context_.server().SendServerStream<Transfer::Write>(EncodeChunk(
        Chunk(ProtocolVersion::kLegacy, Chunk::Type::kParametersRetransmit)
            .set_session_id(12)
            .set_offset(0)
            .set_window_end_offset(64)
            .set_max_chunk_size_bytes(64)));
  });
  ASSERT_EQ(payloads.size(), 3u);

  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c1 = DecodeChunk(payloads[1]);
  EXPECT_EQ(c1.session_id(), 12u);
  EXPECT_EQ(c1.offset(), 0u);
  EXPECT_EQ(c1.payload().size(), 32u);
  EXPECT_EQ(
      std::memcmp(c1.payload().data(), kData32.data(), c1.payload().size()), 0);

  Chunk c2 = DecodeChunk(payloads[2]);
  EXPECT_EQ(c2.session_id(), 12u);
  ASSERT_TRUE(c2.remaining_bytes().has_value());
  EXPECT_EQ(c2.remaining_bytes().value(), 0u);

  // Wait for the timeout to expire without doing anything. The client should
  // resend the data chunk.
  transfer_thread_.SimulateClientTimeout(12);
  ASSERT_EQ(payloads.size(), 4u);

  Chunk c3 = DecodeChunk(payloads[3]);
  EXPECT_EQ(c3.session_id(), c1.session_id());
  EXPECT_EQ(c3.offset(), c1.offset());
  EXPECT_EQ(c3.payload().size(), c1.payload().size());
  EXPECT_EQ(std::memcmp(
                c3.payload().data(), c1.payload().data(), c3.payload().size()),
            0);

  // The remaining_bytes = 0 chunk should be resent on the next parameters.
  context_.server().SendServerStream<Transfer::Write>(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kParametersRetransmit)
          .set_session_id(12)
          .set_offset(32)
          .set_window_end_offset(64)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 5u);

  Chunk c4 = DecodeChunk(payloads[4]);
  EXPECT_EQ(c4.session_id(), 12u);
  ASSERT_TRUE(c4.remaining_bytes().has_value());
  EXPECT_EQ(c4.remaining_bytes().value(), 0u);

  context_.server().SendServerStream<Transfer::Write>(
      EncodeChunk(Chunk::Final(ProtocolVersion::kLegacy, 12, OkStatus())));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_EQ(transfer_status, OkStatus());
}

TEST_F(WriteTransfer, Timeout_EndsTransferAfterMaxRetries) {
  stream::MemoryReader reader(kData32);
  Status transfer_status = Status::Unknown();

  Result<Client::Handle> handle = legacy_client_.Write(
      13,
      reader,
      [&transfer_status](Status status) { transfer_status = status; },
      kTestTimeout,
      kTestTimeout);
  ASSERT_EQ(OkStatus(), handle.status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // The client begins by sending the ID of the resource to transfer.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Write>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c0 = DecodeChunk(payloads.back());
  EXPECT_EQ(c0.session_id(), 13u);
  EXPECT_EQ(c0.resource_id(), 13u);
  EXPECT_EQ(c0.type(), Chunk::Type::kStart);

  for (unsigned retry = 1; retry <= kTestRetries; ++retry) {
    // Wait for the timeout to expire without doing anything. The client should
    // resend the initial transmit chunk.
    transfer_thread_.SimulateClientTimeout(13);
    ASSERT_EQ(payloads.size(), retry + 1);

    Chunk c = DecodeChunk(payloads.back());
    EXPECT_EQ(c.session_id(), 13u);
    EXPECT_EQ(c.resource_id(), 13u);
    EXPECT_EQ(c.type(), Chunk::Type::kStart);

    // Transfer has not yet completed.
    EXPECT_EQ(transfer_status, Status::Unknown());
  }

  // Time out one more time after the final retry. The client should cancel the
  // transfer at this point. As no packets were received from the server, no
  // final status chunk should be sent.
  transfer_thread_.SimulateClientTimeout(13);
  ASSERT_EQ(payloads.size(), 4u);

  EXPECT_EQ(transfer_status, Status::DeadlineExceeded());

  // After finishing the transfer, nothing else should be sent.
  transfer_thread_.SimulateClientTimeout(13);
  transfer_thread_.SimulateClientTimeout(13);
  transfer_thread_.SimulateClientTimeout(13);
  ASSERT_EQ(payloads.size(), 4u);

  // Ensure we don't leave a dangling reference to transfer_status.
  handle->Cancel();
  transfer_thread_.WaitUntilEventIsProcessed();
}

TEST_F(WriteTransfer, Timeout_NonSeekableReaderEndsTransfer) {
  FakeNonSeekableReader reader(kData32);
  Status transfer_status = Status::Unknown();

  Result<Client::Handle> handle = legacy_client_.Write(
      14,
      reader,
      [&transfer_status](Status status) { transfer_status = status; },
      kTestTimeout);
  ASSERT_EQ(OkStatus(), handle.status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // The client begins by sending the ID of the resource to transfer.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Write>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c0 = DecodeChunk(payloads.back());
  EXPECT_EQ(c0.session_id(), 14u);
  EXPECT_EQ(c0.resource_id(), 14u);
  EXPECT_EQ(c0.type(), Chunk::Type::kStart);

  // Send the first parameters chunk.
  rpc::test::WaitForPackets(context_.output(), 2, [this] {
    context_.server().SendServerStream<Transfer::Write>(EncodeChunk(
        Chunk(ProtocolVersion::kLegacy, Chunk::Type::kParametersRetransmit)
            .set_session_id(14)
            .set_offset(0)
            .set_window_end_offset(16)
            .set_max_chunk_size_bytes(8)));
  });
  ASSERT_EQ(payloads.size(), 3u);

  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk c1 = DecodeChunk(payloads[1]);
  EXPECT_EQ(c1.session_id(), 14u);
  EXPECT_EQ(c1.offset(), 0u);
  EXPECT_TRUE(c1.has_payload());
  EXPECT_EQ(c1.payload().size(), 8u);
  EXPECT_EQ(
      std::memcmp(c1.payload().data(), kData32.data(), c1.payload().size()), 0);

  Chunk c2 = DecodeChunk(payloads[2]);
  EXPECT_EQ(c2.session_id(), 14u);
  EXPECT_EQ(c2.offset(), 8u);
  EXPECT_TRUE(c2.has_payload());
  EXPECT_EQ(c2.payload().size(), 8u);
  EXPECT_EQ(std::memcmp(c2.payload().data(),
                        kData32.data() + c2.offset(),
                        c2.payload().size()),
            0);

  // Wait for the timeout to expire without doing anything. The client should
  // fail to seek back and end the transfer.
  transfer_thread_.SimulateClientTimeout(14);
  ASSERT_EQ(payloads.size(), 4u);

  Chunk c3 = DecodeChunk(payloads[3]);
  EXPECT_EQ(c3.protocol_version(), ProtocolVersion::kLegacy);
  EXPECT_EQ(c3.session_id(), 14u);
  ASSERT_TRUE(c3.status().has_value());
  EXPECT_EQ(c3.status().value(), Status::DeadlineExceeded());

  EXPECT_EQ(transfer_status, Status::DeadlineExceeded());

  // Ensure we don't leave a dangling reference to transfer_status.
  handle->Cancel();
  transfer_thread_.WaitUntilEventIsProcessed();
}

TEST_F(WriteTransfer, ManualCancel) {
  stream::MemoryReader reader(kData32);
  Status transfer_status = Status::Unknown();

  Result<Client::Handle> handle = legacy_client_.Write(
      15,
      reader,
      [&transfer_status](Status status) { transfer_status = status; },
      kTestTimeout);
  ASSERT_EQ(OkStatus(), handle.status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // The client begins by sending the ID of the resource to transfer.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Write>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.session_id(), 15u);
  EXPECT_EQ(chunk.resource_id(), 15u);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStart);

  // Get a response from the server, then cancel the transfer.
  // This must request a smaller chunk than the entire available write data to
  // prevent the client from trying to send an additional finish chunk.
  context_.server().SendServerStream<Transfer::Write>(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kParametersRetransmit)
          .set_session_id(15)
          .set_offset(0)
          .set_window_end_offset(16)
          .set_max_chunk_size_bytes(16)));
  transfer_thread_.WaitUntilEventIsProcessed();
  ASSERT_EQ(payloads.size(), 2u);

  handle->Cancel();
  transfer_thread_.WaitUntilEventIsProcessed();

  // Client should send a cancellation chunk to the server.
  ASSERT_EQ(payloads.size(), 3u);
  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.session_id(), 15u);
  ASSERT_EQ(chunk.type(), Chunk::Type::kCompletion);
  EXPECT_EQ(chunk.status().value(), Status::Cancelled());

  EXPECT_EQ(transfer_status, Status::Cancelled());
}

TEST_F(WriteTransfer, ManualCancel_NoContact) {
  stream::MemoryReader reader(kData32);
  Status transfer_status = Status::Unknown();

  Result<Client::Handle> handle = legacy_client_.Write(
      15,
      reader,
      [&transfer_status](Status status) { transfer_status = status; },
      kTestTimeout,
      kTestTimeout);
  ASSERT_EQ(handle.status(), OkStatus());
  transfer_thread_.WaitUntilEventIsProcessed();

  // The client begins by sending the ID of the resource to transfer.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Write>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.session_id(), 15u);
  EXPECT_EQ(chunk.resource_id(), 15u);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStart);

  // Cancel transfer without a server response. No final chunk should be sent.
  handle->Cancel();
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 1u);

  EXPECT_EQ(transfer_status, Status::Cancelled());
}

TEST_F(WriteTransfer, ManualCancel_Duplicate) {
  stream::MemoryReader reader(kData32);
  Status transfer_status = Status::Unknown();

  Result<Client::Handle> handle = legacy_client_.Write(
      16,
      reader,
      [&transfer_status](Status status) { transfer_status = status; },
      kTestTimeout);
  ASSERT_EQ(OkStatus(), handle.status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // The client begins by sending the ID of the resource to transfer.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Write>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.session_id(), 16u);
  EXPECT_EQ(chunk.resource_id(), 16u);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStart);

  // Get a response from the server, then cancel the transfer.
  context_.server().SendServerStream<Transfer::Write>(EncodeChunk(
      Chunk(ProtocolVersion::kLegacy, Chunk::Type::kParametersRetransmit)
          .set_session_id(16)
          .set_offset(0)
          .set_window_end_offset(16)  // Request only a single chunk.
          .set_max_chunk_size_bytes(16)));
  transfer_thread_.WaitUntilEventIsProcessed();
  ASSERT_EQ(payloads.size(), 2u);

  handle->Cancel();
  transfer_thread_.WaitUntilEventIsProcessed();

  // Client should send a cancellation chunk to the server.
  ASSERT_EQ(payloads.size(), 3u);
  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.session_id(), 16u);
  ASSERT_EQ(chunk.type(), Chunk::Type::kCompletion);
  EXPECT_EQ(chunk.status().value(), Status::Cancelled());

  EXPECT_EQ(transfer_status, Status::Cancelled());

  // Attempt to cancel the transfer again.
  transfer_status = Status::Unknown();
  handle->Cancel();
  transfer_thread_.WaitUntilEventIsProcessed();

  // No further chunks should be sent.
  EXPECT_EQ(payloads.size(), 3u);
  EXPECT_EQ(transfer_status, Status::Unknown());
}

TEST_F(ReadTransfer, Version2_SingleChunk) {
  stream::MemoryWriterBuffer<64> writer;
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(
      OkStatus(),
      client_
          .Read(
              3,
              writer,
              [&transfer_status](Status status) { transfer_status = status; },
              cfg::kDefaultClientTimeout,
              cfg::kDefaultClientTimeout)
          .status());

  transfer_thread_.WaitUntilEventIsProcessed();

  // Initial chunk of the transfer is sent. This chunk should contain all the
  // fields from both legacy and version 2 protocols for backwards
  // compatibility.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Read>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk chunk = DecodeChunk(payloads[0]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStart);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.desired_session_id(), 1u);
  EXPECT_EQ(chunk.resource_id(), 3u);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), 37u);
  EXPECT_EQ(chunk.max_chunk_size_bytes(), 37u);

  // The server responds with a START_ACK, continuing the version 2 handshake.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAck)
                      .set_session_id(1)
                      .set_resource_id(3)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 2u);

  // Client should accept the session_id with a START_ACK_CONFIRMATION,
  // additionally containing the initial parameters for the read transfer.
  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAckConfirmation);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_FALSE(chunk.desired_session_id().has_value());
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_FALSE(chunk.resource_id().has_value());
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), 37u);
  EXPECT_EQ(chunk.max_chunk_size_bytes(), 37u);

  // Send all the transfer data. Client should accept it and complete the
  // transfer.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
                      .set_session_id(1)
                      .set_offset(0)
                      .set_payload(kData32)
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 3u);

  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletion);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), OkStatus());

  EXPECT_EQ(transfer_status, OkStatus());
  EXPECT_EQ(std::memcmp(writer.data(), kData32.data(), writer.bytes_written()),
            0);

  context_.server().SendServerStream<Transfer::Read>(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kCompletionAck)
          .set_session_id(1)));
}

TEST_F(ReadTransfer, Version2_ServerRunsLegacy) {
  stream::MemoryWriterBuffer<64> writer;
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(
      OkStatus(),
      client_
          .Read(
              3,
              writer,
              [&transfer_status](Status status) { transfer_status = status; },
              cfg::kDefaultClientTimeout,
              cfg::kDefaultClientTimeout)
          .status());

  transfer_thread_.WaitUntilEventIsProcessed();

  // Initial chunk of the transfer is sent. This chunk should contain all the
  // fields from both legacy and version 2 protocols for backwards
  // compatibility.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Read>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk chunk = DecodeChunk(payloads[0]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStart);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.desired_session_id(), 1u);
  EXPECT_EQ(chunk.resource_id(), 3u);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), 37u);
  EXPECT_EQ(chunk.max_chunk_size_bytes(), 37u);

  // Instead of a START_ACK to continue the handshake, the server responds with
  // an immediate data chunk, indicating that it is running the legacy protocol
  // version. Client should revert to legacy, using the resource_id of 3 as the
  // session_id, and complete the transfer.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kLegacy, Chunk::Type::kData)
                      .set_session_id(3)
                      .set_offset(0)
                      .set_payload(kData32)
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 2u);

  chunk = DecodeChunk(payloads.back());
  EXPECT_FALSE(chunk.desired_session_id().has_value());
  EXPECT_EQ(chunk.session_id(), 3u);
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletion);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kLegacy);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), OkStatus());

  EXPECT_EQ(transfer_status, OkStatus());
  EXPECT_EQ(std::memcmp(writer.data(), kData32.data(), writer.bytes_written()),
            0);
}

TEST_F(ReadTransfer, Version2_TimeoutDuringHandshake) {
  stream::MemoryWriterBuffer<64> writer;
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(
      OkStatus(),
      client_
          .Read(
              3,
              writer,
              [&transfer_status](Status status) { transfer_status = status; },
              cfg::kDefaultClientTimeout,
              cfg::kDefaultClientTimeout)
          .status());

  transfer_thread_.WaitUntilEventIsProcessed();

  // Initial chunk of the transfer is sent. This chunk should contain all the
  // fields from both legacy and version 2 protocols for backwards
  // compatibility.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Read>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStart);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.desired_session_id(), 1u);
  EXPECT_EQ(chunk.resource_id(), 3u);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), 37u);
  EXPECT_EQ(chunk.max_chunk_size_bytes(), 37u);

  // Wait for the timeout to expire without doing anything. The client should
  // resend the initial chunk.
  transfer_thread_.SimulateClientTimeout(1);
  ASSERT_EQ(payloads.size(), 2u);

  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStart);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_EQ(chunk.resource_id(), 3u);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), 37u);
  EXPECT_EQ(chunk.max_chunk_size_bytes(), 37u);

  // This time, the server responds, continuing the handshake and transfer.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAck)
                      .set_session_id(1)
                      .set_resource_id(3)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 3u);

  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAckConfirmation);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_FALSE(chunk.resource_id().has_value());
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), 37u);
  EXPECT_EQ(chunk.max_chunk_size_bytes(), 37u);

  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
                      .set_session_id(1)
                      .set_offset(0)
                      .set_payload(kData32)
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 4u);

  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletion);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), OkStatus());

  EXPECT_EQ(transfer_status, OkStatus());
  EXPECT_EQ(std::memcmp(writer.data(), kData32.data(), writer.bytes_written()),
            0);

  context_.server().SendServerStream<Transfer::Read>(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kCompletionAck)
          .set_session_id(1)));
}

TEST_F(ReadTransfer, Version2_TimeoutAfterHandshake) {
  stream::MemoryWriterBuffer<64> writer;
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(
      OkStatus(),
      client_
          .Read(
              3,
              writer,
              [&transfer_status](Status status) { transfer_status = status; },
              cfg::kDefaultClientTimeout,
              cfg::kDefaultClientTimeout)
          .status());

  transfer_thread_.WaitUntilEventIsProcessed();

  // Initial chunk of the transfer is sent. This chunk should contain all the
  // fields from both legacy and version 2 protocols for backwards
  // compatibility.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Read>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStart);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.desired_session_id(), 1u);
  EXPECT_EQ(chunk.resource_id(), 3u);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), 37u);
  EXPECT_EQ(chunk.max_chunk_size_bytes(), 37u);

  // The server responds with a START_ACK, continuing the version 2 handshake
  // and assigning a session_id to the transfer.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAck)
                      .set_session_id(1)
                      .set_resource_id(3)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 2u);

  // Client should accept the session_id with a START_ACK_CONFIRMATION,
  // additionally containing the initial parameters for the read transfer.
  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAckConfirmation);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_FALSE(chunk.resource_id().has_value());
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), 37u);
  EXPECT_EQ(chunk.max_chunk_size_bytes(), 37u);

  // Wait for the timeout to expire without doing anything. The client should
  // resend the confirmation chunk.
  transfer_thread_.SimulateClientTimeout(1);
  ASSERT_EQ(payloads.size(), 3u);

  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAckConfirmation);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_FALSE(chunk.resource_id().has_value());
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), 37u);
  EXPECT_EQ(chunk.max_chunk_size_bytes(), 37u);

  // The server responds and the transfer should continue normally.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
                      .set_session_id(1)
                      .set_offset(0)
                      .set_payload(kData32)
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 4u);

  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletion);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), OkStatus());

  EXPECT_EQ(transfer_status, OkStatus());
  EXPECT_EQ(std::memcmp(writer.data(), kData32.data(), writer.bytes_written()),
            0);

  context_.server().SendServerStream<Transfer::Read>(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kCompletionAck)
          .set_session_id(1)));
}

TEST_F(ReadTransfer, Version2_ServerErrorDuringHandshake) {
  stream::MemoryWriterBuffer<64> writer;
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(
      OkStatus(),
      client_
          .Read(
              3,
              writer,
              [&transfer_status](Status status) { transfer_status = status; },
              cfg::kDefaultClientTimeout,
              cfg::kDefaultClientTimeout)
          .status());

  transfer_thread_.WaitUntilEventIsProcessed();

  // Initial chunk of the transfer is sent. This chunk should contain all the
  // fields from both legacy and version 2 protocols for backwards
  // compatibility.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Read>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStart);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.desired_session_id(), 1u);
  EXPECT_EQ(chunk.resource_id(), 3u);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), 37u);
  EXPECT_EQ(chunk.max_chunk_size_bytes(), 37u);

  // The server responds to the start request with an error.
  context_.server().SendServerStream<Transfer::Read>(EncodeChunk(Chunk::Final(
      ProtocolVersion::kVersionTwo, 1, Status::Unauthenticated())));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unauthenticated());
}

TEST_F(ReadTransfer, Version2_TimeoutWaitingForCompletionAckRetries) {
  stream::MemoryWriterBuffer<64> writer;
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(
      OkStatus(),
      client_
          .Read(
              3,
              writer,
              [&transfer_status](Status status) { transfer_status = status; },
              cfg::kDefaultClientTimeout,
              cfg::kDefaultClientTimeout)
          .status());

  transfer_thread_.WaitUntilEventIsProcessed();

  // Initial chunk of the transfer is sent. This chunk should contain all the
  // fields from both legacy and version 2 protocols for backwards
  // compatibility.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Read>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk chunk = DecodeChunk(payloads[0]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStart);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.desired_session_id(), 1u);
  EXPECT_EQ(chunk.resource_id(), 3u);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), 37u);
  EXPECT_EQ(chunk.max_chunk_size_bytes(), 37u);

  // The server responds with a START_ACK, continuing the version 2 handshake.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAck)
                      .set_session_id(1)
                      .set_resource_id(3)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 2u);

  // Client should accept the session_id with a START_ACK_CONFIRMATION,
  // additionally containing the initial parameters for the read transfer.
  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAckConfirmation);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_FALSE(chunk.resource_id().has_value());
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), 37u);
  EXPECT_EQ(chunk.max_chunk_size_bytes(), 37u);

  // Send all the transfer data. Client should accept it and complete the
  // transfer.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
                      .set_session_id(1)
                      .set_offset(0)
                      .set_payload(kData32)
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 3u);

  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletion);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), OkStatus());

  EXPECT_EQ(transfer_status, OkStatus());
  EXPECT_EQ(std::memcmp(writer.data(), kData32.data(), writer.bytes_written()),
            0);

  // Time out instead of sending a completion ACK. THe transfer should resend
  // its completion chunk.
  transfer_thread_.SimulateClientTimeout(1);
  ASSERT_EQ(payloads.size(), 4u);

  // Reset transfer_status to check whether the handler is called again.
  transfer_status = Status::Unknown();

  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletion);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), OkStatus());

  // Transfer handler should not be called a second time in response to the
  // re-sent completion chunk.
  EXPECT_EQ(transfer_status, Status::Unknown());

  // Send a completion ACK to end the transfer.
  context_.server().SendServerStream<Transfer::Read>(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kCompletionAck)
          .set_session_id(1)));
  transfer_thread_.WaitUntilEventIsProcessed();

  // No further chunks should be sent following the ACK.
  transfer_thread_.SimulateClientTimeout(1);
  ASSERT_EQ(payloads.size(), 4u);
}

TEST_F(ReadTransfer,
       Version2_TimeoutWaitingForCompletionAckEndsTransferAfterRetries) {
  stream::MemoryWriterBuffer<64> writer;
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(
      OkStatus(),
      client_
          .Read(
              3,
              writer,
              [&transfer_status](Status status) { transfer_status = status; },
              cfg::kDefaultClientTimeout,
              cfg::kDefaultClientTimeout)
          .status());

  transfer_thread_.WaitUntilEventIsProcessed();

  // Initial chunk of the transfer is sent. This chunk should contain all the
  // fields from both legacy and version 2 protocols for backwards
  // compatibility.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Read>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk chunk = DecodeChunk(payloads[0]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStart);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.desired_session_id(), 1u);
  EXPECT_EQ(chunk.resource_id(), 3u);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), 37u);
  EXPECT_EQ(chunk.max_chunk_size_bytes(), 37u);

  // The server responds with a START_ACK, continuing the version 2 handshake
  // and assigning a session_id to the transfer.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAck)
                      .set_session_id(1)
                      .set_resource_id(3)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 2u);

  // Client should accept the session_id with a START_ACK_CONFIRMATION,
  // additionally containing the initial parameters for the read transfer.
  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAckConfirmation);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_FALSE(chunk.resource_id().has_value());
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), 37u);
  EXPECT_EQ(chunk.max_chunk_size_bytes(), 37u);

  // Send all the transfer data. Client should accept it and complete the
  // transfer.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
                      .set_session_id(1)
                      .set_offset(0)
                      .set_payload(kData32)
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 3u);

  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletion);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), OkStatus());

  EXPECT_EQ(transfer_status, OkStatus());
  EXPECT_EQ(std::memcmp(writer.data(), kData32.data(), writer.bytes_written()),
            0);

  // Time out instead of sending a completion ACK. THe transfer should resend
  // its completion chunk at first, then terminate after the maximum number of
  // retries.
  transfer_thread_.SimulateClientTimeout(1);
  ASSERT_EQ(payloads.size(), 4u);  // Retry 1.

  transfer_thread_.SimulateClientTimeout(1);
  ASSERT_EQ(payloads.size(), 5u);  // Retry 2.

  transfer_thread_.SimulateClientTimeout(1);
  ASSERT_EQ(payloads.size(), 6u);  // Retry 3.

  transfer_thread_.SimulateClientTimeout(1);
  ASSERT_EQ(payloads.size(), 6u);  // No more retries; transfer has ended.
}

TEST_F(WriteTransfer, Version2_SingleChunk) {
  stream::MemoryReader reader(kData32);
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(
      OkStatus(),
      client_
          .Write(
              3,
              reader,
              [&transfer_status](Status status) { transfer_status = status; },
              cfg::kDefaultClientTimeout,
              cfg::kDefaultClientTimeout)
          .status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // The client begins by sending the ID of the resource to transfer.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Write>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStart);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.desired_session_id(), 1u);
  EXPECT_EQ(chunk.resource_id(), 3u);

  // The server responds with a START_ACK, continuing the version 2 handshake.
  context_.server().SendServerStream<Transfer::Write>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAck)
                      .set_session_id(1)
                      .set_resource_id(3)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 2u);

  // Client should accept the session_id with a START_ACK_CONFIRMATION.
  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAckConfirmation);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_FALSE(chunk.resource_id().has_value());

  // The server can then begin the data transfer by sending its transfer
  // parameters. Client should respond with a data chunk and the final chunk.
  rpc::test::WaitForPackets(context_.output(), 2, [this] {
    context_.server().SendServerStream<Transfer::Write>(EncodeChunk(
        Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kParametersRetransmit)
            .set_session_id(1)
            .set_offset(0)
            .set_window_end_offset(64)
            .set_max_chunk_size_bytes(32)));
  });

  ASSERT_EQ(payloads.size(), 4u);

  chunk = DecodeChunk(payloads[2]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kData);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_TRUE(chunk.has_payload());
  EXPECT_EQ(std::memcmp(
                chunk.payload().data(), kData32.data(), chunk.payload().size()),
            0);

  chunk = DecodeChunk(payloads[3]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kData);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);
  ASSERT_TRUE(chunk.remaining_bytes().has_value());
  EXPECT_EQ(chunk.remaining_bytes().value(), 0u);

  EXPECT_EQ(transfer_status, Status::Unknown());

  // Send the final status chunk to complete the transfer.
  context_.server().SendServerStream<Transfer::Write>(
      EncodeChunk(Chunk::Final(ProtocolVersion::kVersionTwo, 1, OkStatus())));
  transfer_thread_.WaitUntilEventIsProcessed();

  // Client should acknowledge the completion of the transfer.
  EXPECT_EQ(payloads.size(), 5u);

  chunk = DecodeChunk(payloads[4]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletionAck);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);

  EXPECT_EQ(transfer_status, OkStatus());
}

TEST_F(WriteTransfer, Version2_ServerRunsLegacy) {
  stream::MemoryReader reader(kData32);
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(
      OkStatus(),
      client_
          .Write(
              3,
              reader,
              [&transfer_status](Status status) { transfer_status = status; },
              cfg::kDefaultClientTimeout,
              cfg::kDefaultClientTimeout)
          .status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // The client begins by sending the ID of the resource to transfer.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Write>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStart);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.desired_session_id(), 1u);
  EXPECT_EQ(chunk.resource_id(), 3u);

  // Instead of continuing the handshake with a START_ACK, the server
  // immediately sends parameters, indicating that it only supports the legacy
  // protocol. Client should switch over to legacy and continue the transfer.
  rpc::test::WaitForPackets(context_.output(), 2, [this] {
    context_.server().SendServerStream<Transfer::Write>(EncodeChunk(
        Chunk(ProtocolVersion::kLegacy, Chunk::Type::kParametersRetransmit)
            .set_session_id(3)
            .set_offset(0)
            .set_window_end_offset(64)
            .set_max_chunk_size_bytes(32)));
  });

  ASSERT_EQ(payloads.size(), 3u);

  chunk = DecodeChunk(payloads[1]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kData);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kLegacy);
  EXPECT_EQ(chunk.session_id(), 3u);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_TRUE(chunk.has_payload());
  EXPECT_EQ(std::memcmp(
                chunk.payload().data(), kData32.data(), chunk.payload().size()),
            0);

  chunk = DecodeChunk(payloads[2]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kData);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kLegacy);
  EXPECT_EQ(chunk.session_id(), 3u);
  ASSERT_TRUE(chunk.remaining_bytes().has_value());
  EXPECT_EQ(chunk.remaining_bytes().value(), 0u);

  EXPECT_EQ(transfer_status, Status::Unknown());

  // Send the final status chunk to complete the transfer.
  context_.server().SendServerStream<Transfer::Write>(
      EncodeChunk(Chunk::Final(ProtocolVersion::kLegacy, 3, OkStatus())));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_EQ(payloads.size(), 3u);
  EXPECT_EQ(transfer_status, OkStatus());
}

TEST_F(WriteTransfer, Version2_RetryDuringHandshake) {
  stream::MemoryReader reader(kData32);
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(
      OkStatus(),
      client_
          .Write(
              3,
              reader,
              [&transfer_status](Status status) { transfer_status = status; },
              cfg::kDefaultClientTimeout,
              cfg::kDefaultClientTimeout)
          .status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // The client begins by sending the ID of the resource to transfer.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Write>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStart);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.desired_session_id(), 1u);
  EXPECT_EQ(chunk.resource_id(), 3u);

  // Time out waiting for a server response. The client should resend the
  // initial packet.
  transfer_thread_.SimulateClientTimeout(1);
  ASSERT_EQ(payloads.size(), 2u);

  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStart);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.desired_session_id(), 1u);
  EXPECT_EQ(chunk.resource_id(), 3u);

  // This time, respond with the correct continuation packet. The transfer
  // should resume and complete normally.
  context_.server().SendServerStream<Transfer::Write>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAck)
                      .set_session_id(1)
                      .set_resource_id(3)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 3u);

  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAckConfirmation);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_FALSE(chunk.resource_id().has_value());

  rpc::test::WaitForPackets(context_.output(), 2, [this] {
    context_.server().SendServerStream<Transfer::Write>(EncodeChunk(
        Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kParametersRetransmit)
            .set_session_id(1)
            .set_offset(0)
            .set_window_end_offset(64)
            .set_max_chunk_size_bytes(32)));
  });

  ASSERT_EQ(payloads.size(), 5u);

  chunk = DecodeChunk(payloads[3]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kData);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_TRUE(chunk.has_payload());
  EXPECT_EQ(std::memcmp(
                chunk.payload().data(), kData32.data(), chunk.payload().size()),
            0);

  chunk = DecodeChunk(payloads[4]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kData);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);
  ASSERT_TRUE(chunk.remaining_bytes().has_value());
  EXPECT_EQ(chunk.remaining_bytes().value(), 0u);

  EXPECT_EQ(transfer_status, Status::Unknown());

  context_.server().SendServerStream<Transfer::Write>(
      EncodeChunk(Chunk::Final(ProtocolVersion::kVersionTwo, 1, OkStatus())));
  transfer_thread_.WaitUntilEventIsProcessed();

  // Client should acknowledge the completion of the transfer.
  EXPECT_EQ(payloads.size(), 6u);

  chunk = DecodeChunk(payloads[5]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletionAck);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);

  EXPECT_EQ(transfer_status, OkStatus());
}

TEST_F(WriteTransfer, HijackedStreamIsCancelled) {
  stream::MemoryReader reader1(kData32);
  Status transfer_status1 = Status::Unknown();

  ASSERT_EQ(OkStatus(),
            legacy_client_
                .Write(3,
                       reader1,
                       [&transfer_status1](Status status) {
                         transfer_status1 = status;
                       })
                .status());
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_TRUE(legacy_client_.has_write_stream());

  // A new client (client_) attempts a write transfer using the same thread.
  // This will steal the write stream from legacy_client_.
  stream::MemoryReader reader2(kData32);
  Status transfer_status2 = Status::Unknown();

  Result<Client::Handle> handle =
      client_.Write(4, reader2, [&transfer_status2](Status status) {
        transfer_status2 = status;
      });
  ASSERT_EQ(OkStatus(), handle.status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // legacy_client_ should have its write stream cancelled and marked false.
  EXPECT_FALSE(legacy_client_.has_write_stream());
  EXPECT_TRUE(client_.has_write_stream());
  EXPECT_EQ(transfer_status1, Status::Aborted());

  handle->Cancel();
  transfer_thread_.WaitUntilEventIsProcessed();
}

TEST_F(WriteTransfer, Version2_RetryAfterHandshake) {
  stream::MemoryReader reader(kData32);
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(
      OkStatus(),
      client_
          .Write(
              3,
              reader,
              [&transfer_status](Status status) { transfer_status = status; },
              cfg::kDefaultClientTimeout,
              cfg::kDefaultClientTimeout)
          .status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // The client begins by sending the ID of the resource to transfer.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Write>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStart);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.desired_session_id(), 1u);
  EXPECT_EQ(chunk.resource_id(), 3u);

  // The server responds with a START_ACK, continuing the version 2 handshake.
  context_.server().SendServerStream<Transfer::Write>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAck)
                      .set_session_id(1)
                      .set_resource_id(3)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 2u);

  // Client should accept the session_id with a START_ACK_CONFIRMATION.
  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAckConfirmation);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_FALSE(chunk.resource_id().has_value());

  // Time out waiting for a server response. The client should resend the
  // initial packet.
  transfer_thread_.SimulateClientTimeout(1);
  ASSERT_EQ(payloads.size(), 3u);

  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAckConfirmation);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_FALSE(chunk.resource_id().has_value());

  // This time, respond with the first transfer parameters chunk. The transfer
  // should resume and complete normally.
  rpc::test::WaitForPackets(context_.output(), 2, [this] {
    context_.server().SendServerStream<Transfer::Write>(EncodeChunk(
        Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kParametersRetransmit)
            .set_session_id(1)
            .set_offset(0)
            .set_window_end_offset(64)
            .set_max_chunk_size_bytes(32)));
  });

  ASSERT_EQ(payloads.size(), 5u);

  chunk = DecodeChunk(payloads[3]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kData);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_TRUE(chunk.has_payload());
  EXPECT_EQ(std::memcmp(
                chunk.payload().data(), kData32.data(), chunk.payload().size()),
            0);

  chunk = DecodeChunk(payloads[4]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kData);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);
  ASSERT_TRUE(chunk.remaining_bytes().has_value());
  EXPECT_EQ(chunk.remaining_bytes().value(), 0u);

  EXPECT_EQ(transfer_status, Status::Unknown());

  context_.server().SendServerStream<Transfer::Write>(
      EncodeChunk(Chunk::Final(ProtocolVersion::kVersionTwo, 1, OkStatus())));
  transfer_thread_.WaitUntilEventIsProcessed();

  // Client should acknowledge the completion of the transfer.
  EXPECT_EQ(payloads.size(), 6u);

  chunk = DecodeChunk(payloads[5]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletionAck);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);

  EXPECT_EQ(transfer_status, OkStatus());
}

TEST_F(WriteTransfer, Version2_ServerErrorDuringHandshake) {
  stream::MemoryReader reader(kData32);
  Status transfer_status = Status::Unknown();

  ASSERT_EQ(
      OkStatus(),
      client_
          .Write(
              3,
              reader,
              [&transfer_status](Status status) { transfer_status = status; },
              cfg::kDefaultClientTimeout,
              cfg::kDefaultClientTimeout)
          .status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // The client begins by sending the ID of the resource to transfer.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Write>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStart);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.desired_session_id(), 1u);
  EXPECT_EQ(chunk.resource_id(), 3u);

  // The server responds to the start request with an error.
  context_.server().SendServerStream<Transfer::Write>(EncodeChunk(
      Chunk::Final(ProtocolVersion::kVersionTwo, 1, Status::NotFound())));
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::NotFound());
}

class ReadTransferMaxBytes256 : public ReadTransfer {
 protected:
  ReadTransferMaxBytes256() : ReadTransfer(/*max_bytes_to_receive=*/256) {}
};

TEST_F(ReadTransferMaxBytes256, Version2_AdapativeWindow_SlowStart) {
  stream::MemoryWriterBuffer<256> writer;
  Status transfer_status = Status::Unknown();

  constexpr size_t kExpectedMaxChunkSize = 37;

  ASSERT_EQ(
      OkStatus(),
      client_
          .Read(
              3,
              writer,
              [&transfer_status](Status status) { transfer_status = status; },
              cfg::kDefaultClientTimeout,
              cfg::kDefaultClientTimeout)
          .status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // Initial chunk of the transfer is sent. This chunk should contain all the
  // fields from both legacy and version 2 protocols for backwards
  // compatibility.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Read>(context_.channel().id());

  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk chunk = DecodeChunk(payloads[0]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStart);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.desired_session_id(), 1u);
  EXPECT_EQ(chunk.resource_id(), 3u);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), kExpectedMaxChunkSize);
  EXPECT_EQ(chunk.max_chunk_size_bytes(), kExpectedMaxChunkSize);

  // The server responds with a START_ACK, continuing the version 2 handshake.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAck)
                      .set_session_id(1)
                      .set_resource_id(3)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 2u);

  // Client should accept the session_id with a START_ACK_CONFIRMATION,
  // additionally containing the initial parameters for the read transfer.
  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAckConfirmation);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_FALSE(chunk.desired_session_id().has_value());
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_FALSE(chunk.resource_id().has_value());
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), kExpectedMaxChunkSize);
  EXPECT_EQ(chunk.max_chunk_size_bytes(), kExpectedMaxChunkSize);

  context_.server().SendServerStream<Transfer::Read>(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
          .set_session_id(1)
          .set_offset(0)
          .set_payload(span(kData256).first(kExpectedMaxChunkSize))));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 3u);

  // Window size should double in response to successful receipt.
  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersContinue);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_FALSE(chunk.desired_session_id().has_value());
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_FALSE(chunk.resource_id().has_value());
  EXPECT_EQ(chunk.offset(), kExpectedMaxChunkSize);
  EXPECT_EQ(chunk.window_end_offset(),
            chunk.offset() + 2 * kExpectedMaxChunkSize);

  // Send the next chunk.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
                      .set_session_id(1)
                      .set_offset(chunk.offset())
                      .set_payload(span(kData256).subspan(
                          chunk.offset(), kExpectedMaxChunkSize))));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 4u);

  // Window size should double in response to successful receipt.
  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersContinue);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_FALSE(chunk.desired_session_id().has_value());
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_FALSE(chunk.resource_id().has_value());
  EXPECT_EQ(chunk.offset(), 2 * kExpectedMaxChunkSize);
  EXPECT_EQ(chunk.window_end_offset(),
            chunk.offset() + 4 * kExpectedMaxChunkSize);

  // Finish the transfer.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
                      .set_session_id(1)
                      .set_offset(chunk.offset())
                      .set_payload(span(kData256).subspan(
                          chunk.offset(), kExpectedMaxChunkSize))
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 5u);

  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletion);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), OkStatus());

  EXPECT_EQ(transfer_status, OkStatus());
  EXPECT_EQ(std::memcmp(writer.data(), kData256.data(), writer.bytes_written()),
            0);

  context_.server().SendServerStream<Transfer::Read>(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kCompletionAck)
          .set_session_id(1)));
}

TEST_F(ReadTransferMaxBytes256, Version2_AdapativeWindow_CongestionAvoidance) {
  stream::MemoryWriterBuffer<256> writer;
  Status transfer_status = Status::Unknown();

  constexpr size_t kExpectedMaxChunkSize = 37;

  ASSERT_EQ(
      OkStatus(),
      client_
          .Read(
              3,
              writer,
              [&transfer_status](Status status) { transfer_status = status; },
              cfg::kDefaultClientTimeout,
              cfg::kDefaultClientTimeout)
          .status());

  transfer_thread_.WaitUntilEventIsProcessed();

  // Initial chunk of the transfer is sent. This chunk should contain all the
  // fields from both legacy and version 2 protocols for backwards
  // compatibility.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Read>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk chunk = DecodeChunk(payloads[0]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStart);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.desired_session_id(), 1u);
  EXPECT_EQ(chunk.resource_id(), 3u);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), kExpectedMaxChunkSize);
  EXPECT_EQ(chunk.max_chunk_size_bytes(), kExpectedMaxChunkSize);

  // The server responds with a START_ACK, continuing the version 2 handshake.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAck)
                      .set_session_id(1)
                      .set_resource_id(3)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 2u);

  // Client should accept the session_id with a START_ACK_CONFIRMATION,
  // additionally containing the initial parameters for the read transfer.
  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAckConfirmation);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_FALSE(chunk.desired_session_id().has_value());
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_FALSE(chunk.resource_id().has_value());
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), kExpectedMaxChunkSize);
  EXPECT_EQ(chunk.max_chunk_size_bytes(), kExpectedMaxChunkSize);

  context_.server().SendServerStream<Transfer::Read>(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
          .set_session_id(1)
          .set_offset(0)
          .set_payload(span(kData256).first(kExpectedMaxChunkSize))));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 3u);

  // Window size should double in response to successful receipt.
  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersContinue);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_FALSE(chunk.desired_session_id().has_value());
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_FALSE(chunk.resource_id().has_value());
  EXPECT_EQ(chunk.offset(), kExpectedMaxChunkSize);
  EXPECT_EQ(chunk.window_end_offset(),
            chunk.offset() + 2 * kExpectedMaxChunkSize);

  // Send the next chunk.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
                      .set_session_id(1)
                      .set_offset(chunk.offset())
                      .set_payload(span(kData256).subspan(
                          chunk.offset(), kExpectedMaxChunkSize))));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 4u);

  // Window size should double in response to successful receipt.
  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersContinue);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_FALSE(chunk.desired_session_id().has_value());
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_FALSE(chunk.resource_id().has_value());
  EXPECT_EQ(chunk.offset(), 2 * kExpectedMaxChunkSize);
  EXPECT_EQ(chunk.window_end_offset(),
            chunk.offset() + 4 * kExpectedMaxChunkSize);

  // Time out instead of sending another chunk.
  transfer_thread_.SimulateClientTimeout(1);

  ASSERT_EQ(payloads.size(), 5u);

  // Window size should half following data loss.
  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersRetransmit);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_FALSE(chunk.desired_session_id().has_value());
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_FALSE(chunk.resource_id().has_value());
  EXPECT_EQ(chunk.offset(), 2 * kExpectedMaxChunkSize);
  EXPECT_EQ(chunk.window_end_offset(),
            chunk.offset() + 2 * (kExpectedMaxChunkSize - 1));

  // Send another chunk.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
                      .set_session_id(1)
                      .set_offset(chunk.offset())
                      .set_payload(span(kData256).subspan(
                          chunk.offset(), kExpectedMaxChunkSize - 1))));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 6u);

  // Window size should now only increase by 1 instead of doubling.
  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kParametersContinue);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_FALSE(chunk.desired_session_id().has_value());
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_FALSE(chunk.resource_id().has_value());
  EXPECT_EQ(chunk.offset(), 3 * kExpectedMaxChunkSize - 1);
  EXPECT_EQ(chunk.window_end_offset(),
            chunk.offset() + 3 * (kExpectedMaxChunkSize - 1));

  // Finish the transfer.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kData)
                      .set_session_id(1)
                      .set_offset(chunk.offset())
                      .set_payload(span(kData256).subspan(
                          chunk.offset(), kExpectedMaxChunkSize - 1))
                      .set_remaining_bytes(0)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 7u);

  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletion);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  ASSERT_TRUE(chunk.status().has_value());
  EXPECT_EQ(chunk.status().value(), OkStatus());

  EXPECT_EQ(transfer_status, OkStatus());
  EXPECT_EQ(std::memcmp(writer.data(), kData256.data(), writer.bytes_written()),
            0);

  context_.server().SendServerStream<Transfer::Read>(EncodeChunk(
      Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kCompletionAck)
          .set_session_id(1)));
}

TEST_F(WriteTransfer, Write_UpdateTransferSize) {
  FakeNonSeekableReader reader(kData32);
  Status transfer_status = Status::Unknown();

  Result<Client::Handle> result = client_.Write(
      91,
      reader,
      [&transfer_status](Status status) { transfer_status = status; },
      kTestTimeout);
  ASSERT_EQ(OkStatus(), result.status());
  transfer_thread_.WaitUntilEventIsProcessed();

  Client::Handle handle = *result;
  handle.SetTransferSize(kData32.size());
  transfer_thread_.WaitUntilEventIsProcessed();

  // Initial chunk of the transfer is sent. This chunk should contain all the
  // fields from both legacy and version 2 protocols for backwards
  // compatibility.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Write>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk chunk = DecodeChunk(payloads[0]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStart);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.desired_session_id(), 1u);
  EXPECT_EQ(chunk.resource_id(), 91u);
  EXPECT_EQ(chunk.offset(), 0u);

  // The server responds with a START_ACK, continuing the version 2 handshake.
  context_.server().SendServerStream<Transfer::Write>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAck)
                      .set_session_id(1)
                      .set_resource_id(91)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 2u);

  // Client should accept the session_id with a START_ACK_CONFIRMATION.
  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAckConfirmation);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_FALSE(chunk.resource_id().has_value());

  // The server can then begin the data transfer by sending its transfer
  // parameters. Client should respond with data chunks.
  rpc::test::WaitForPackets(context_.output(), 4, [this] {
    context_.server().SendServerStream<Transfer::Write>(EncodeChunk(
        Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kParametersRetransmit)
            .set_session_id(1)
            .set_offset(0)
            .set_window_end_offset(64)
            .set_max_chunk_size_bytes(8)));
  });

  ASSERT_EQ(payloads.size(), 6u);

  // Each 8-byte chunk of the 32-byte transfer should have an appropriate
  // `remaining_bytes` value set.
  chunk = DecodeChunk(payloads[2]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kData);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_EQ(chunk.offset(), 0u);
  ASSERT_TRUE(chunk.remaining_bytes().has_value());
  EXPECT_EQ(chunk.remaining_bytes().value(), 24u);

  chunk = DecodeChunk(payloads[3]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kData);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_EQ(chunk.offset(), 8u);
  ASSERT_TRUE(chunk.remaining_bytes().has_value());
  EXPECT_EQ(chunk.remaining_bytes().value(), 16u);

  chunk = DecodeChunk(payloads[4]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kData);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_EQ(chunk.offset(), 16u);
  ASSERT_TRUE(chunk.remaining_bytes().has_value());
  EXPECT_EQ(chunk.remaining_bytes().value(), 8u);

  chunk = DecodeChunk(payloads[5]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kData);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_EQ(chunk.offset(), 24u);
  ASSERT_TRUE(chunk.remaining_bytes().has_value());
  EXPECT_EQ(chunk.remaining_bytes().value(), 0u);

  EXPECT_EQ(transfer_status, Status::Unknown());

  // Send the final status chunk to complete the transfer.
  context_.server().SendServerStream<Transfer::Write>(
      EncodeChunk(Chunk::Final(ProtocolVersion::kVersionTwo, 1, OkStatus())));
  transfer_thread_.WaitUntilEventIsProcessed();

  // Client should acknowledge the completion of the transfer.
  ASSERT_EQ(payloads.size(), 7u);

  chunk = DecodeChunk(payloads[6]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletionAck);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);

  EXPECT_EQ(transfer_status, OkStatus());

  // Ensure we don't leave a dangling reference to transfer_status.
  handle.Cancel();
  transfer_thread_.WaitUntilEventIsProcessed();
}

TEST_F(WriteTransfer, Write_TransferSize_Large) {
  FakeNonSeekableReader reader(kData64);
  Status transfer_status = Status::Unknown();

  Result<Client::Handle> result = client_.Write(
      91,
      reader,
      [&transfer_status](Status status) { transfer_status = status; },
      kTestTimeout);
  ASSERT_EQ(OkStatus(), result.status());
  transfer_thread_.WaitUntilEventIsProcessed();

  // Set a large transfer size that will encode to a multibyte varint.
  constexpr size_t kLargeRemainingBytes = 1u << 28;
  Client::Handle handle = *result;
  handle.SetTransferSize(kLargeRemainingBytes);
  transfer_thread_.WaitUntilEventIsProcessed();

  // Initial chunk of the transfer is sent. This chunk should contain all the
  // fields from both legacy and version 2 protocols for backwards
  // compatibility.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Write>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk chunk = DecodeChunk(payloads[0]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStart);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.desired_session_id(), 1u);
  EXPECT_EQ(chunk.resource_id(), 91u);
  EXPECT_EQ(chunk.offset(), 0u);

  // The server responds with a START_ACK, continuing the version 2 handshake.
  context_.server().SendServerStream<Transfer::Write>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAck)
                      .set_session_id(1)
                      .set_resource_id(91)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 2u);

  // Client should accept the session_id with a START_ACK_CONFIRMATION.
  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAckConfirmation);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_FALSE(chunk.resource_id().has_value());

  // The server can then begin the data transfer by sending its transfer
  // parameters. Client should respond with data chunks.
  rpc::test::WaitForPackets(context_.output(), 2, [this] {
    context_.server().SendServerStream<Transfer::Write>(EncodeChunk(
        Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kParametersRetransmit)
            .set_session_id(1)
            .set_offset(0)
            .set_window_end_offset(64)  // Only request one chunk.
            .set_max_chunk_size_bytes(64)));
  });

  ASSERT_EQ(payloads.size(), 4u);

  // The transfer should reserve appropriate space for the `remaining_bytes`
  // value and not fail to encode.
  chunk = DecodeChunk(payloads[2]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kData);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.payload().size_bytes(), 48u);
  ASSERT_TRUE(chunk.remaining_bytes().has_value());
  EXPECT_EQ(chunk.remaining_bytes().value(),
            kLargeRemainingBytes - chunk.payload().size_bytes());

  EXPECT_EQ(transfer_status, Status::Unknown());

  // Send the final status chunk to complete the transfer.
  context_.server().SendServerStream<Transfer::Write>(
      EncodeChunk(Chunk::Final(ProtocolVersion::kVersionTwo, 1, OkStatus())));
  transfer_thread_.WaitUntilEventIsProcessed();

  // Client should acknowledge the completion of the transfer.
  EXPECT_EQ(payloads.size(), 5u);

  chunk = DecodeChunk(payloads[4]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletionAck);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);

  EXPECT_EQ(transfer_status, OkStatus());

  // Ensure we don't leave a dangling reference to transfer_status.
  handle.Cancel();
  transfer_thread_.WaitUntilEventIsProcessed();
}

TEST_F(WriteTransfer, Write_TransferSize_SmallerThanResource) {
  // 64 byte data, but only set a 16 byte size.
  constexpr size_t kSmallerTransferSize = 16;
  FakeNonSeekableReader reader(kData64);
  Status transfer_status = Status::Unknown();

  Result<Client::Handle> result = client_.Write(
      92,
      reader,
      [&transfer_status](Status status) { transfer_status = status; },
      kTestTimeout);
  ASSERT_EQ(OkStatus(), result.status());
  transfer_thread_.WaitUntilEventIsProcessed();

  Client::Handle handle = *result;
  handle.SetTransferSize(kSmallerTransferSize);
  transfer_thread_.WaitUntilEventIsProcessed();

  // Initial chunk of the transfer is sent. This chunk should contain all the
  // fields from both legacy and version 2 protocols for backwards
  // compatibility.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Write>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk chunk = DecodeChunk(payloads[0]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStart);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.desired_session_id(), 1u);
  EXPECT_EQ(chunk.resource_id(), 92u);
  EXPECT_EQ(chunk.offset(), 0u);

  // The server responds with a START_ACK, continuing the version 2 handshake.
  context_.server().SendServerStream<Transfer::Write>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAck)
                      .set_session_id(1)
                      .set_resource_id(92)));
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 2u);

  // Client should accept the session_id with a START_ACK_CONFIRMATION.
  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStartAckConfirmation);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_FALSE(chunk.resource_id().has_value());

  // The server can then begin the data transfer by sending its transfer
  // parameters. Client should respond with data chunks.
  rpc::test::WaitForPackets(context_.output(), 2, [this] {
    context_.server().SendServerStream<Transfer::Write>(EncodeChunk(
        Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kParametersRetransmit)
            .set_session_id(1)
            .set_offset(0)
            .set_window_end_offset(64)
            .set_max_chunk_size_bytes(8)));
  });

  ASSERT_EQ(payloads.size(), 4u);

  // Each 8-byte chunk of the transfer should have an appropriate
  // `remaining_bytes` value set.
  chunk = DecodeChunk(payloads[2]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kData);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_EQ(chunk.offset(), 0u);
  ASSERT_TRUE(chunk.remaining_bytes().has_value());
  EXPECT_EQ(chunk.remaining_bytes().value(), 8u);

  chunk = DecodeChunk(payloads[3]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kData);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);
  EXPECT_EQ(chunk.offset(), 8u);
  ASSERT_TRUE(chunk.remaining_bytes().has_value());
  EXPECT_EQ(chunk.remaining_bytes().value(), 0u);

  EXPECT_EQ(transfer_status, Status::Unknown());

  // Send the final status chunk to complete the transfer.
  context_.server().SendServerStream<Transfer::Write>(
      EncodeChunk(Chunk::Final(ProtocolVersion::kVersionTwo, 1, OkStatus())));
  transfer_thread_.WaitUntilEventIsProcessed();

  // Client should acknowledge the completion of the transfer.
  ASSERT_EQ(payloads.size(), 5u);

  chunk = DecodeChunk(payloads[4]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletionAck);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.session_id(), 1u);

  EXPECT_EQ(transfer_status, OkStatus());

  // Ensure we don't leave a dangling reference to transfer_status.
  handle.Cancel();
  transfer_thread_.WaitUntilEventIsProcessed();
}

TEST_F(ReadTransfer, Version2_CancelBeforeServerResponse) {
  stream::MemoryWriterBuffer<64> writer;
  Status transfer_status = Status::Unknown();

  Result<Client::Handle> transfer = client_.Read(
      3,
      writer,
      [&transfer_status](Status status) { transfer_status = status; },
      cfg::kDefaultClientTimeout,
      cfg::kDefaultClientTimeout);
  ASSERT_EQ(transfer.status(), OkStatus());

  transfer_thread_.WaitUntilEventIsProcessed();

  // Initial chunk of the transfer is sent. This chunk should contain all the
  // fields from both legacy and version 2 protocols for backwards
  // compatibility.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Read>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk chunk = DecodeChunk(payloads[0]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStart);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.desired_session_id(), 1u);
  EXPECT_EQ(chunk.resource_id(), 3u);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), 37u);
  EXPECT_EQ(chunk.max_chunk_size_bytes(), 37u);

  // Cancel the transfer before the server responds. Since no contact was made,
  // no cancellation chunk should be sent.
  transfer->Cancel();
  transfer_thread_.WaitUntilEventIsProcessed();

  ASSERT_EQ(payloads.size(), 1u);

  // The server responds after the cancellation. The client should notify it
  // that the transfer is no longer active.
  context_.server().SendServerStream<Transfer::Read>(
      EncodeChunk(Chunk(ProtocolVersion::kVersionTwo, Chunk::Type::kStartAck)
                      .set_session_id(1)
                      .set_resource_id(3)));
  transfer_thread_.WaitUntilEventIsProcessed();

  chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kCompletion);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.status(), Status::Cancelled());
}

TEST_F(WriteTransfer, Version2_WriteRpcError) {
  FakeNonSeekableReader reader(kData32);
  Status transfer_status = Status::Unknown();

  Result<Client::Handle> result = client_.Write(
      3,
      reader,
      [&transfer_status](Status status) { transfer_status = status; },
      cfg::kDefaultClientTimeout,
      cfg::kDefaultClientTimeout);
  ASSERT_EQ(OkStatus(), result.status());
  transfer_thread_.WaitUntilEventIsProcessed();

  Client::Handle handle = *result;

  // The client begins by sending the ID of the resource to transfer.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Write>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk chunk = DecodeChunk(payloads.back());
  EXPECT_EQ(chunk.type(), Chunk::Type::kStart);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.desired_session_id(), 1u);
  EXPECT_EQ(chunk.resource_id(), 3u);

  // RPC server sends back an error.
  context_.server().SendServerError<Transfer::Write>(Status::Internal());
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_EQ(client_.has_write_stream(), false);
  EXPECT_EQ(transfer_status, Status::Aborted());

  // Ensure we don't leave a dangling reference to transfer_status.
  handle.Cancel();
  transfer_thread_.WaitUntilEventIsProcessed();
}

TEST_F(ReadTransfer, Version2_ReadRpcError) {
  stream::MemoryWriterBuffer<64> writer;
  Status transfer_status = Status::Unknown();

  Result<Client::Handle> result = client_.Read(
      3,
      writer,
      [&transfer_status](Status status) { transfer_status = status; },
      cfg::kDefaultClientTimeout,
      cfg::kDefaultClientTimeout);
  ASSERT_EQ(OkStatus(), result.status());
  transfer_thread_.WaitUntilEventIsProcessed();

  Client::Handle handle = *result;

  // Initial chunk of the transfer is sent. This chunk should contain all the
  // fields from both legacy and version 2 protocols for backwards
  // compatibility.
  rpc::PayloadsView payloads =
      context_.output().payloads<Transfer::Read>(context_.channel().id());
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(transfer_status, Status::Unknown());

  Chunk chunk = DecodeChunk(payloads[0]);
  EXPECT_EQ(chunk.type(), Chunk::Type::kStart);
  EXPECT_EQ(chunk.protocol_version(), ProtocolVersion::kVersionTwo);
  EXPECT_EQ(chunk.desired_session_id(), 1u);
  EXPECT_EQ(chunk.resource_id(), 3u);
  EXPECT_EQ(chunk.offset(), 0u);
  EXPECT_EQ(chunk.window_end_offset(), 37u);
  EXPECT_EQ(chunk.max_chunk_size_bytes(), 37u);

  // RPC server sends back an error.
  context_.server().SendServerError<Transfer::Read>(Status::Internal());
  transfer_thread_.WaitUntilEventIsProcessed();

  EXPECT_EQ(client_.has_read_stream(), false);
  EXPECT_EQ(transfer_status, Status::Aborted());

  // Ensure we don't leave a dangling reference to transfer_status.
  handle.Cancel();
  transfer_thread_.WaitUntilEventIsProcessed();
}

}  // namespace
}  // namespace pw::transfer::test
