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

#include "pw_rpc2/internal/packet.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

#include "pw_allocator/testing.h"
#include "pw_assert/check.h"
#include "pw_bytes/array.h"
#include "pw_bytes/span.h"
#include "pw_unit_test/framework.h"

namespace {

namespace internal = ::pw::rpc2::internal;

std::string_view AsString(pw::ConstByteSpan bytes) {
  return std::string_view(reinterpret_cast<const char*>(bytes.data()),
                          bytes.size());
}

TEST(PacketTest, EncodeDecodeRequest) {
  pw::allocator::test::AllocatorForTest<256> allocator;

  constexpr std::string_view kPayload = "hello request";
  size_t total_size = sizeof(internal::RequestWireFormat) + kPayload.size();

  auto buf = pw::Buf::Allocate(allocator, total_size);

  auto packet =
      internal::OutboundPacket::Request(0x12345678, 0xabcdef01, 0x23456789);
  pw::ConstByteSpan payload_bytes = pw::as_bytes(pw::span(kPayload));
  std::copy(payload_bytes.begin(),
            payload_bytes.end(),
            buf.data() + packet.payload_offset());

  auto encode_result = packet.Encode(std::move(buf));
  ASSERT_EQ(encode_result.status(), pw::OkStatus());
  pw::Buf encoded_buf = std::move(encode_result.value());
  EXPECT_EQ(encoded_buf.size(), total_size);

  auto decode_result =
      internal::InboundPacket::Decode(pw::ConstBuf(std::move(encoded_buf)));
  ASSERT_EQ(decode_result.status(), pw::OkStatus());

  internal::InboundPacket decoded = std::move(decode_result.value());
  EXPECT_EQ(decoded.type(), internal::PacketType::kRequest);
  EXPECT_EQ(decoded.payload_offset(), sizeof(internal::RequestWireFormat));
  EXPECT_EQ(decoded.call_id(), 0x12345678u);
  EXPECT_EQ(decoded.service_id(), 0xabcdef01u);
  EXPECT_EQ(decoded.method_id(), 0x23456789u);
  EXPECT_EQ(decoded.payload().size(), kPayload.size());
  EXPECT_EQ(AsString(decoded.payload()), kPayload);

  pw::ConstBuf payload_buf = std::move(decoded).TakePayload();
  EXPECT_EQ(payload_buf.size(), kPayload.size());
  EXPECT_EQ(AsString(payload_buf), kPayload);
}

TEST(PacketTest, EncodeDecodeMessage) {
  pw::allocator::test::AllocatorForTest<256> allocator;

  constexpr std::string_view kPayload = "hello msg";
  size_t total_size = sizeof(internal::MessageWireFormat) + kPayload.size();

  auto buf = pw::Buf::Allocate(allocator, total_size);

  auto packet = internal::OutboundPacket::Message(0x12345678);
  pw::ConstByteSpan payload_bytes = pw::as_bytes(pw::span(kPayload));
  std::copy(payload_bytes.begin(),
            payload_bytes.end(),
            buf.data() + packet.payload_offset());

  auto encode_result = packet.Encode(std::move(buf));
  ASSERT_EQ(encode_result.status(), pw::OkStatus());
  pw::Buf encoded_buf = std::move(encode_result.value());
  EXPECT_EQ(encoded_buf.size(), total_size);

  auto decode_result =
      internal::InboundPacket::Decode(pw::ConstBuf(std::move(encoded_buf)));
  ASSERT_EQ(decode_result.status(), pw::OkStatus());

  internal::InboundPacket decoded = std::move(decode_result.value());
  EXPECT_EQ(decoded.type(), internal::PacketType::kMessage);
  EXPECT_EQ(decoded.payload_offset(), sizeof(internal::MessageWireFormat));
  EXPECT_EQ(decoded.call_id(), 0x12345678u);
  EXPECT_EQ(decoded.payload().size(), kPayload.size());
  EXPECT_EQ(AsString(decoded.payload()), kPayload);

  pw::ConstBuf payload_buf = std::move(decoded).TakePayload();
  EXPECT_EQ(payload_buf.size(), kPayload.size());
  EXPECT_EQ(AsString(payload_buf), kPayload);
}

TEST(PacketTest, EncodeDecodeStreamEnd) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  size_t total_size = sizeof(internal::StreamEndWireFormat);

  auto buf = pw::Buf::Allocate(allocator, total_size);

  auto encode_result =
      internal::OutboundPacket::StreamEnd(0x12345678).Encode(std::move(buf));
  ASSERT_EQ(encode_result.status(), pw::OkStatus());
  pw::Buf encoded_buf = std::move(encode_result.value());
  EXPECT_EQ(encoded_buf.size(), total_size);

  auto decode_result =
      internal::InboundPacket::Decode(pw::ConstBuf(std::move(encoded_buf)));
  ASSERT_EQ(decode_result.status(), pw::OkStatus());

  internal::InboundPacket decoded = std::move(decode_result.value());
  EXPECT_EQ(decoded.type(), internal::PacketType::kStreamEnd);
  EXPECT_EQ(decoded.call_id(), 0x12345678u);
}

TEST(PacketTest, EncodeDecodeError) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  size_t total_size = sizeof(internal::ErrorWireFormat);

  auto buf = pw::Buf::Allocate(allocator, total_size);

  auto encode_result = internal::OutboundPacket::Error(
                           0x12345678, pw::Status::FailedPrecondition())
                           .Encode(std::move(buf));
  ASSERT_EQ(encode_result.status(), pw::OkStatus());
  pw::Buf encoded_buf = std::move(encode_result.value());
  EXPECT_EQ(encoded_buf.size(), total_size);

  auto decode_result =
      internal::InboundPacket::Decode(pw::ConstBuf(std::move(encoded_buf)));
  ASSERT_EQ(decode_result.status(), pw::OkStatus());

  internal::InboundPacket decoded = std::move(decode_result.value());
  EXPECT_EQ(decoded.type(), internal::PacketType::kError);
  EXPECT_EQ(decoded.call_id(), 0x12345678u);
  EXPECT_EQ(decoded.status(), pw::Status::FailedPrecondition());
}

TEST(PacketTest, DecodeBufferTooShortForHeader) {
  pw::allocator::test::AllocatorForTest<256> allocator;

  auto buf = pw::Buf::Allocate(allocator, sizeof(internal::PacketHeader) - 1);
  EXPECT_EQ(
      internal::InboundPacket::Decode(pw::ConstBuf(std::move(buf))).status(),
      pw::Status::DataLoss());
}

TEST(PacketTest, DecodeBufferTooShortForType) {
  pw::allocator::test::AllocatorForTest<256> allocator;

  // Long enough for the common header, but not for a request header.
  auto buf = pw::Buf::Allocate(allocator, sizeof(internal::PacketHeader));
  ASSERT_EQ(internal::OutboundPacket::Request(1, 2, 3)
                .EncodeHeader(pw::ByteSpan(buf))
                .status(),
            pw::Status::ResourceExhausted());
  // Write just the type byte by hand, since the header does not fit.
  buf[offsetof(internal::PacketHeader, type)] =
      static_cast<std::byte>(internal::PacketType::kRequest);

  EXPECT_EQ(
      internal::InboundPacket::Decode(pw::ConstBuf(std::move(buf))).status(),
      pw::Status::DataLoss());
}

TEST(PacketTest, DecodeUnrecognizedType) {
  pw::allocator::test::AllocatorForTest<256> allocator;

  for (std::byte invalid_type :
       {std::byte{0x00}, std::byte{0x06}, std::byte{0x7f}, std::byte{0xff}}) {
    auto buf = pw::Buf::Allocate(allocator, 32);
    buf[offsetof(internal::PacketHeader, type)] = invalid_type;

    EXPECT_EQ(
        internal::InboundPacket::Decode(pw::ConstBuf(std::move(buf))).status(),
        pw::Status::InvalidArgument());
  }
}

TEST(PacketTest, EncodeDecodeHandshakePacket) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  size_t total_size = internal::HandshakePacket::kWireSizeBytes;
  EXPECT_EQ(total_size, 8u);

  auto buf = pw::Buf::Allocate(allocator, total_size);

  internal::HandshakePacket packet(internal::HandshakePacket::Type::kSyn);

  auto encode_result = packet.Encode(std::move(buf));
  ASSERT_EQ(encode_result.status(), pw::OkStatus());
  pw::Buf encoded_buf = std::move(encode_result.value());
  EXPECT_EQ(encoded_buf.size(), total_size);

  auto decode_result = internal::HandshakePacket::Decode(encoded_buf);
  ASSERT_EQ(decode_result.status(), pw::OkStatus());

  internal::HandshakePacket decoded = decode_result.value();
  EXPECT_EQ(decoded.version(), 1u);
  EXPECT_EQ(decoded.type(), internal::HandshakePacket::Type::kSyn);
}

TEST(PacketTest, DecodeZeroLengthPayload) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  size_t total_size = sizeof(internal::RequestWireFormat);

  auto buf = pw::Buf::Allocate(allocator, total_size);

  auto encode_result =
      internal::OutboundPacket::Request(0x12345678, 0x100, 0x200)
          .Encode(std::move(buf));
  ASSERT_EQ(encode_result.status(), pw::OkStatus());
  pw::Buf encoded_buf = std::move(encode_result.value());
  EXPECT_EQ(encoded_buf.size(), total_size);

  auto decode_result =
      internal::InboundPacket::Decode(pw::ConstBuf(std::move(encoded_buf)));
  ASSERT_EQ(decode_result.status(), pw::OkStatus());

  internal::InboundPacket decoded = std::move(decode_result.value());
  EXPECT_EQ(decoded.type(), internal::PacketType::kRequest);
  EXPECT_EQ(decoded.payload().size(), 0u);

  pw::ConstBuf payload_buf = std::move(decoded).TakePayload();
  EXPECT_TRUE(payload_buf.empty());
}

TEST(PacketTest, DecodeCorruptHandshakePacket) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  auto buf = pw::Buf::Allocate(allocator, 4);
  EXPECT_EQ(internal::HandshakePacket::Decode(buf).status(),
            pw::Status::DataLoss());
}

TEST(PacketTest, EncodeDecodeResponse) {
  pw::allocator::test::AllocatorForTest<256> allocator;

  constexpr std::string_view kPayload = "hello response";
  size_t total_size = sizeof(internal::ResponseWireFormat) + kPayload.size();

  auto buf = pw::Buf::Allocate(allocator, total_size);

  auto packet = internal::OutboundPacket::Response(0x12345678);
  pw::ConstByteSpan payload_bytes = pw::as_bytes(pw::span(kPayload));
  std::copy(payload_bytes.begin(),
            payload_bytes.end(),
            buf.data() + packet.payload_offset());

  auto encode_result = packet.Encode(std::move(buf));
  ASSERT_EQ(encode_result.status(), pw::OkStatus());
  pw::Buf encoded_buf = std::move(encode_result.value());
  EXPECT_EQ(encoded_buf.size(), total_size);

  auto decode_result =
      internal::InboundPacket::Decode(pw::ConstBuf(std::move(encoded_buf)));
  ASSERT_EQ(decode_result.status(), pw::OkStatus());

  internal::InboundPacket decoded = std::move(decode_result.value());
  EXPECT_EQ(decoded.type(), internal::PacketType::kResponse);
  EXPECT_EQ(decoded.payload_offset(), sizeof(internal::ResponseWireFormat));
  EXPECT_EQ(decoded.call_id(), 0x12345678u);
  EXPECT_EQ(decoded.payload().size(), kPayload.size());
  EXPECT_EQ(AsString(decoded.payload()), kPayload);

  pw::ConstBuf payload_buf = std::move(decoded).TakePayload();
  EXPECT_EQ(payload_buf.size(), kPayload.size());
  EXPECT_EQ(AsString(payload_buf), kPayload);
}

TEST(PacketTest, EncodeStreamEndTruncatesExtraBuffer) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  auto buf = pw::Buf::Allocate(allocator, 64);
  auto encode_result =
      internal::OutboundPacket::StreamEnd(0x12345678).Encode(std::move(buf));
  ASSERT_EQ(encode_result.status(), pw::OkStatus());
  EXPECT_EQ(encode_result->size(), sizeof(internal::StreamEndWireFormat));
}

TEST(PacketTest, EncodeErrorTruncatesExtraBuffer) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  auto buf = pw::Buf::Allocate(allocator, 64);
  auto encode_result = internal::OutboundPacket::Error(
                           0x12345678, pw::Status::FailedPrecondition())
                           .Encode(std::move(buf));
  ASSERT_EQ(encode_result.status(), pw::OkStatus());
  EXPECT_EQ(encode_result->size(), sizeof(internal::ErrorWireFormat));
}

TEST(PacketTest, EncodeRejectsBufferSmallerThanHeader) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  auto buf =
      pw::Buf::Allocate(allocator, sizeof(internal::RequestWireFormat) - 1);
  EXPECT_EQ(internal::OutboundPacket::Request(1, 2, 3)
                .Encode(std::move(buf))
                .status(),
            pw::Status::ResourceExhausted());
}

TEST(PacketTest, EncodeRejectsEmptyBuffer) {
  EXPECT_EQ(internal::OutboundPacket::Message(1).Encode(pw::Buf()).status(),
            pw::Status::FailedPrecondition());
}

static_assert(
    internal::PacketSizeWithoutPayload(internal::PacketType::kRequest) == 13u);
static_assert(
    internal::PacketSizeWithoutPayload(internal::PacketType::kMessage) == 5u);
static_assert(
    internal::PacketSizeWithoutPayload(internal::PacketType::kResponse) == 5u);
static_assert(
    internal::PacketSizeWithoutPayload(internal::PacketType::kStreamEnd) == 5u);
static_assert(
    internal::PacketSizeWithoutPayload(internal::PacketType::kError) == 9u);

static_assert(internal::OutboundPacket::Request(1, 2, 3).payload_offset() ==
              13u);
static_assert(internal::OutboundPacket::Message(1).payload_offset() == 5u);
static_assert(internal::OutboundPacket::Response(1).payload_offset() == 5u);
static_assert(internal::OutboundPacket::StreamEnd(1).payload_offset() == 5u);
static_assert(internal::OutboundPacket::Error(1, pw::Status::Internal())
                  .payload_offset() == 9u);

TEST(PacketTest, EncodedMessageHeaderLayout) {
  std::array<std::byte, sizeof(internal::MessageWireFormat)> buffer = {};

  auto result =
      internal::OutboundPacket::Message(0x12345678).EncodeHeader(buffer);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(*result, buffer.size());

  // call_id is little endian and comes first, followed by the type byte.
  constexpr auto expected = pw::bytes::Array<0x78, 0x56, 0x34, 0x12, 0x02>();
  EXPECT_EQ(buffer, expected);
}

TEST(PacketTest, EncodedRequestHeaderLayout) {
  std::array<std::byte, sizeof(internal::RequestWireFormat)> buffer = {};

  auto result =
      internal::OutboundPacket::Request(0x12345678, 0xabcdef01, 0x23456789)
          .EncodeHeader(buffer);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(*result, buffer.size());

  constexpr auto expected = pw::bytes::Array<0x78,
                                             0x56,
                                             0x34,
                                             0x12,
                                             0x01,
                                             0x01,
                                             0xef,
                                             0xcd,
                                             0xab,
                                             0x89,
                                             0x67,
                                             0x45,
                                             0x23>();
  EXPECT_EQ(buffer, expected);
}

TEST(PacketTest, EncodeHeaderReportsTotalSizeWithoutWritingPayloadLength) {
  constexpr auto buffer_init = [](size_t i) {
    return static_cast<std::byte>(i + 1);
  };
  auto buffer = pw::bytes::Initialized<64>(buffer_init);

  auto result = internal::OutboundPacket::Response(1).EncodeHeader(buffer, 10);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(*result, sizeof(internal::ResponseWireFormat) + 10u);

  // Nothing past the header was modified.
  for (size_t i = sizeof(internal::ResponseWireFormat); i < buffer.size();
       ++i) {
    EXPECT_EQ(buffer[i], buffer_init(i));
  }
}

TEST(PacketTest, EncodeHeaderRejectsBufferSmallerThanHeaderPlusPayload) {
  std::array<std::byte, sizeof(internal::MessageWireFormat) + 4> buffer = {};

  EXPECT_EQ(
      internal::OutboundPacket::Message(1).EncodeHeader(buffer, 5).status(),
      pw::Status::ResourceExhausted());
  EXPECT_EQ(
      internal::OutboundPacket::Message(1).EncodeHeader(buffer, 4).status(),
      pw::OkStatus());
}

TEST(PacketTest, DecodedPayloadIsEverythingAfterTheHeader) {
  pw::allocator::test::AllocatorForTest<256> allocator;

  // The payload length is not on the wire, so all trailing bytes belong to the
  // payload regardless of what was passed to EncodeHeader.
  constexpr size_t kTrailingBytes = 7;
  auto buf = pw::Buf::Allocate(
      allocator, sizeof(internal::MessageWireFormat) + kTrailingBytes);

  auto encode_result =
      internal::OutboundPacket::Message(42).EncodeHeader(pw::ByteSpan(buf));
  ASSERT_EQ(encode_result.status(), pw::OkStatus());
  ASSERT_EQ(*encode_result, sizeof(internal::MessageWireFormat));

  auto decode_result =
      internal::InboundPacket::Decode(pw::ConstBuf(std::move(buf)));
  ASSERT_EQ(decode_result.status(), pw::OkStatus());

  internal::InboundPacket decoded = std::move(decode_result.value());
  EXPECT_EQ(decoded.type(), internal::PacketType::kMessage);
  EXPECT_EQ(decoded.call_id(), 42u);
  EXPECT_EQ(decoded.payload().size(), kTrailingBytes);
}

}  // namespace
