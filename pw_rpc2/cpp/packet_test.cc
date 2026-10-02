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
#include <cstdint>
#include <iterator>
#include <string_view>
#include <utility>

#include "pw_allocator/testing.h"
#include "pw_assert/check.h"
#include "pw_bytes/array.h"
#include "pw_bytes/span.h"
#include "pw_enum/to_string.h"
#include "pw_enum/traits.h"
#include "pw_rpc2/internal/protocol_status.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

namespace {

namespace internal = ::pw::rpc2::internal;

using internal::PacketType;

using internal::CloseMode;

namespace flags = internal::flags;

// The only valid type bytes, as listed in protocol.rst.
constexpr uint8_t kValidTypes[] = {0x02,
                                   0x03,
                                   0x04,
                                   0x06,
                                   0x08,
                                   0x09,
                                   0x0a,
                                   0x0b,
                                   0x0c,
                                   0x0e,
                                   0x11,
                                   0x13,
                                   0x18,
                                   0x19};

constexpr bool IsListedAsValid(uint32_t byte) {
  for (uint8_t valid : kValidTypes) {
    if (valid == byte) {
      return true;
    }
  }
  return false;
}

constexpr bool ValidityMatchesList() {
  for (uint32_t b = 0; b < 256; ++b) {
    if (PacketType::IsValid(static_cast<uint8_t>(b)) != IsListedAsValid(b)) {
      return false;
    }
  }
  return true;
}

static_assert(ValidityMatchesList());

// Each accessor reports its own bits, so together they describe the type
// completely, and every type is addressed to exactly one endpoint.
constexpr bool AccessorsMatchBits() {
  for (uint8_t byte : kValidTypes) {
    const PacketType type = PacketType::FromValidatedBits(byte);
    const auto rebuilt = static_cast<uint8_t>(
        (type.is_server() ? static_cast<uint8_t>(flags::kServer) : uint8_t{0}) |
        (type.has_payload() ? static_cast<uint8_t>(flags::kHasPayload)
                            : uint8_t{0}) |
        (type.is_start() ? static_cast<uint8_t>(flags::kStart) : uint8_t{0}) |
        static_cast<uint8_t>(type.close_mode()));
    if (rebuilt != byte || type.bits() != byte) {
      return false;
    }
    if (type.is_for(internal::EndpointRole::kServer) ==
        type.is_for(internal::EndpointRole::kClient)) {
      return false;
    }
    if (type.is_error() != (type.close_mode() == CloseMode::kErrorTerminal)) {
      return false;
    }
  }
  return true;
}

static_assert(AccessorsMatchBits());

static_assert(PacketType::Make<flags::kServer, flags::kErrorTerminal>().is_for(
    internal::EndpointRole::kClient));
static_assert(
    PacketType::Make<flags::kStart>().is_for(internal::EndpointRole::kServer));

// The packets that the C++ client and server send have the type bytes that
// protocol.rst documents.
static_assert(internal::OutboundPacket::StartUnary(1, 2, 3).type().bits() ==
              0x0e);
static_assert(internal::OutboundPacket::StartStream(1, 2, 3).type().bits() ==
              0x04);
static_assert(internal::OutboundPacket::ClientMessage(1).type().bits() == 0x02);
static_assert(internal::OutboundPacket::ServerMessage(1).type().bits() == 0x03);
static_assert(internal::OutboundPacket::ClientStreamEnd(1).type().bits() ==
              0x08);
static_assert(internal::OutboundPacket::ServerFinish(1).type().bits() == 0x11);
static_assert(internal::OutboundPacket::Response(1).type().bits() == 0x13);
static_assert(internal::OutboundPacket::ClientError(
                  1, internal::ProtocolStatus::kCancelled)
                  .type()
                  .bits() == 0x18);
static_assert(internal::OutboundPacket::ServerError(
                  1, internal::ProtocolStatus::kCancelled)
                  .type()
                  .bits() == 0x19);

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
      internal::OutboundPacket::StartUnary(0x12345678, 0xabcdef01, 0x23456789);
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
  EXPECT_EQ(decoded.type(),
            (PacketType::
                 Make<flags::kStart, flags::kHasPayload, flags::kStreamEnd>()));
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
  size_t total_size = sizeof(internal::PacketHeader) + kPayload.size();

  for (auto [sender, expected_type] :
       {std::pair{internal::EndpointRole::kClient,
                  PacketType::Make<flags::kHasPayload>()},
        std::pair{internal::EndpointRole::kServer,
                  PacketType::Make<flags::kServer, flags::kHasPayload>()}}) {
    auto buf = pw::Buf::Allocate(allocator, total_size);

    auto packet = internal::OutboundPacket::Message(sender, 0x12345678);
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
    EXPECT_EQ(decoded.type(), expected_type);
    EXPECT_TRUE(decoded.type().has_payload());
    EXPECT_EQ(decoded.type().close_mode(), CloseMode::kOpen);
    EXPECT_EQ(decoded.payload_offset(), sizeof(internal::PacketHeader));
    EXPECT_EQ(decoded.call_id(), 0x12345678u);
    EXPECT_EQ(decoded.payload().size(), kPayload.size());
    EXPECT_EQ(AsString(decoded.payload()), kPayload);

    pw::ConstBuf payload_buf = std::move(decoded).TakePayload();
    EXPECT_EQ(payload_buf.size(), kPayload.size());
    EXPECT_EQ(AsString(payload_buf), kPayload);
  }
}

TEST(PacketTest, EncodeDecodeFinish) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  size_t total_size = sizeof(internal::PacketHeader);

  for (auto [sender, expected_type] :
       {std::pair{internal::EndpointRole::kClient,
                  PacketType::Make<flags::kStreamEnd>()},
        std::pair{internal::EndpointRole::kServer,
                  PacketType::Make<flags::kServer, flags::kOkTerminal>()}}) {
    auto buf = pw::Buf::Allocate(allocator, total_size);

    auto encode_result = internal::OutboundPacket::Finish(sender, 0x12345678)
                             .Encode(std::move(buf));
    ASSERT_EQ(encode_result.status(), pw::OkStatus());
    pw::Buf encoded_buf = std::move(encode_result.value());
    EXPECT_EQ(encoded_buf.size(), total_size);

    auto decode_result =
        internal::InboundPacket::Decode(pw::ConstBuf(std::move(encoded_buf)));
    ASSERT_EQ(decode_result.status(), pw::OkStatus());

    internal::InboundPacket decoded = std::move(decode_result.value());
    EXPECT_EQ(decoded.type(), expected_type);
    EXPECT_NE(decoded.type().close_mode(), CloseMode::kOpen);
    EXPECT_FALSE(decoded.type().has_payload());
    EXPECT_EQ(decoded.call_id(), 0x12345678u);
  }
}

TEST(PacketTest, EncodeDecodeError) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  size_t total_size = sizeof(internal::ErrorWireFormat);

  {
    auto buf = pw::Buf::Allocate(allocator, total_size);
    auto encode_result = internal::OutboundPacket::ClientError(
                             0x12345678, internal::ProtocolStatus::kCancelled)
                             .Encode(std::move(buf));
    ASSERT_EQ(encode_result.status(), pw::OkStatus());
    pw::Buf encoded_buf = std::move(encode_result.value());
    EXPECT_EQ(encoded_buf.size(), total_size);

    auto decode_result =
        internal::InboundPacket::Decode(pw::ConstBuf(std::move(encoded_buf)));
    ASSERT_EQ(decode_result.status(), pw::OkStatus());

    internal::InboundPacket decoded = std::move(decode_result.value());
    EXPECT_EQ(decoded.type(), PacketType::Make<flags::kErrorTerminal>());
    EXPECT_TRUE(decoded.type().is_error());
    EXPECT_EQ(decoded.call_id(), 0x12345678u);
    EXPECT_EQ(decoded.error(), internal::ProtocolStatus::kCancelled);
  }

  {
    auto buf = pw::Buf::Allocate(allocator, total_size);
    auto encode_result =
        internal::OutboundPacket::ServerError(
            0x12345678, internal::ProtocolStatus::kUnknownMethod)
            .Encode(std::move(buf));
    ASSERT_EQ(encode_result.status(), pw::OkStatus());
    pw::Buf encoded_buf = std::move(encode_result.value());
    EXPECT_EQ(encoded_buf.size(), total_size);

    auto decode_result =
        internal::InboundPacket::Decode(pw::ConstBuf(std::move(encoded_buf)));
    ASSERT_EQ(decode_result.status(), pw::OkStatus());

    internal::InboundPacket decoded = std::move(decode_result.value());
    EXPECT_EQ(decoded.type(),
              (PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
    EXPECT_TRUE(decoded.type().is_error());
    EXPECT_EQ(decoded.call_id(), 0x12345678u);
    EXPECT_EQ(decoded.error(), internal::ProtocolStatus::kUnknownMethod);
  }
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
  ASSERT_EQ(internal::OutboundPacket::StartUnary(1, 2, 3)
                .EncodeHeader(pw::ByteSpan(buf))
                .status(),
            pw::Status::ResourceExhausted());
  // Write just the type byte by hand, since the header does not fit.
  buf[offsetof(internal::PacketHeader, type)] = static_cast<std::byte>(
      flags::kStart | flags::kHasPayload | flags::kStreamEnd);

  EXPECT_EQ(
      internal::InboundPacket::Decode(pw::ConstBuf(std::move(buf))).status(),
      pw::Status::DataLoss());
}

TEST(PacketTest, DecodeUnrecognizedType) {
  pw::allocator::test::AllocatorForTest<256> allocator;

  for (std::byte invalid_type : {std::byte{0x00},  // Empty no-op
                                 std::byte{0x01},  // Empty no-op
                                 std::byte{0x05},  // Server start
                                 std::byte{0x10},  // Client OK terminal
                                 std::byte{0x14},  // Start + OK terminal
                                 std::byte{0x1a},  // Error with payload
                                 std::byte{0x1c},  // Start + error
                                 std::byte{0x20},  // Reserved bit
                                 std::byte{0x7f},
                                 std::byte{0xff}}) {
    auto buf = pw::Buf::Allocate(allocator, 32);
    buf[offsetof(internal::PacketHeader, type)] = invalid_type;

    EXPECT_EQ(
        internal::InboundPacket::Decode(pw::ConstBuf(std::move(buf))).status(),
        pw::Status::InvalidArgument());
  }
}

TEST(PacketTest, EncodeDecodeStartStream) {
  pw::allocator::test::AllocatorForTest<256> allocator;

  // Extra buffer space is truncated away: an open carries no payload.
  auto buf = pw::Buf::Allocate(allocator, 64);
  auto encode_result =
      internal::OutboundPacket::StartStream(1, 2, 3).Encode(std::move(buf));
  ASSERT_EQ(encode_result.status(), pw::OkStatus());
  EXPECT_EQ(encode_result->size(), sizeof(internal::RequestWireFormat));

  auto decode_result = internal::InboundPacket::Decode(
      pw::ConstBuf(std::move(encode_result.value())));
  ASSERT_EQ(decode_result.status(), pw::OkStatus());
  EXPECT_EQ(decode_result->type(), PacketType::Make<flags::kStart>());
  EXPECT_EQ(decode_result->call_id(), 1u);
  EXPECT_EQ(decode_result->service_id(), 2u);
  EXPECT_EQ(decode_result->method_id(), 3u);
  EXPECT_TRUE(decode_result->payload().empty());
}

TEST(PacketTest, DecodeAllStartTypes) {
  pw::allocator::test::AllocatorForTest<256> allocator;

  for (PacketType type : {PacketType::Make<flags::kStart>(),
                          PacketType::Make<flags::kStart, flags::kHasPayload>(),
                          PacketType::Make<flags::kStart, flags::kStreamEnd>(),
                          PacketType::Make<flags::kStart,
                                           flags::kHasPayload,
                                           flags::kStreamEnd>()}) {
    // call_id 1, the type byte, service_id 8, method_id 9.
    const std::array<uint8_t, sizeof(internal::RequestWireFormat)> kWire = {
        0x01, 0, 0, 0, type.bits(), 0x08, 0, 0, 0, 0x09, 0, 0, 0};
    auto buf = pw::Buf::Allocate(allocator, kWire.size());
    for (size_t i = 0; i < kWire.size(); ++i) {
      buf[i] = static_cast<std::byte>(kWire[i]);
    }

    auto decode_result =
        internal::InboundPacket::Decode(pw::ConstBuf(std::move(buf)));
    ASSERT_EQ(decode_result.status(), pw::OkStatus());
    EXPECT_EQ(decode_result->type(), type);
    EXPECT_EQ(decode_result->call_id(), 1u);
    EXPECT_EQ(decode_result->service_id(), 8u);
    EXPECT_EQ(decode_result->method_id(), 9u);
    EXPECT_TRUE(decode_result->payload().empty());
  }
}

TEST(PacketTest, DecodeRejectsTrailingBytesOnControlPacket) {
  pw::allocator::test::AllocatorForTest<256> allocator;

  for (PacketType type :
       {PacketType::Make<flags::kStart>(),
        PacketType::Make<flags::kStreamEnd>(),
        PacketType::Make<flags::kServer, flags::kStreamEnd>(),
        PacketType::Make<flags::kStart, flags::kStreamEnd>(),
        PacketType::Make<flags::kServer, flags::kOkTerminal>(),
        PacketType::Make<flags::kErrorTerminal>(),
        PacketType::Make<flags::kServer, flags::kErrorTerminal>()}) {
    const size_t header_size = internal::PacketSizeWithoutPayload(type);
    auto buf = pw::Buf::Allocate(allocator, header_size + 1);
    buf[offsetof(internal::PacketHeader, type)] =
        static_cast<std::byte>(type.bits());
    EXPECT_EQ(
        internal::InboundPacket::Decode(pw::ConstBuf(std::move(buf))).status(),
        pw::Status::DataLoss());
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
      internal::OutboundPacket::StartUnary(0x12345678, 0x100, 0x200)
          .Encode(std::move(buf));
  ASSERT_EQ(encode_result.status(), pw::OkStatus());
  pw::Buf encoded_buf = std::move(encode_result.value());
  EXPECT_EQ(encoded_buf.size(), total_size);

  auto decode_result =
      internal::InboundPacket::Decode(pw::ConstBuf(std::move(encoded_buf)));
  ASSERT_EQ(decode_result.status(), pw::OkStatus());

  internal::InboundPacket decoded = std::move(decode_result.value());
  EXPECT_EQ(decoded.type(),
            (PacketType::
                 Make<flags::kStart, flags::kHasPayload, flags::kStreamEnd>()));
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

TEST(PacketTest, HandshakePacketDefaultsToMaxVersion) {
  const internal::HandshakePacket packet(
      internal::HandshakePacket::Type::kSynAck);
  EXPECT_EQ(packet.version(), internal::HandshakePacket::kMaxVersion);
}

TEST(PacketTest, EncodeDecodeHandshakePacketVersion) {
  std::byte buffer[internal::HandshakePacket::kWireSizeBytes];
  PW_TEST_ASSERT_OK(
      internal::HandshakePacket(internal::HandshakePacket::Type::kAck, 7)
          .Encode(buffer));
  EXPECT_EQ(buffer[offsetof(internal::HandshakeWireFormat, version)],
            std::byte{7});

  auto decoded = internal::HandshakePacket::Decode(buffer);
  PW_TEST_ASSERT_OK(decoded.status());
  EXPECT_EQ(decoded->type(), internal::HandshakePacket::Type::kAck);
  EXPECT_EQ(decoded->version(), 7u);
}

TEST(PacketTest, DecodeHandshakeIgnoresTrailingAndReservedBytes) {
  // A later protocol version may send a longer handshake packet and use the
  // reserved field. Both must be ignored so that negotiation still works.
  std::byte buffer[internal::HandshakePacket::kWireSizeBytes + 5];
  std::fill(std::begin(buffer), std::end(buffer), std::byte{0xee});
  PW_TEST_ASSERT_OK(
      internal::HandshakePacket(internal::HandshakePacket::Type::kSyn, 2)
          .Encode(buffer));
  buffer[offsetof(internal::HandshakeWireFormat, reserved)] = std::byte{0xa5};
  buffer[offsetof(internal::HandshakeWireFormat, reserved) + 1] =
      std::byte{0x5a};

  for (size_t size = internal::HandshakePacket::kWireSizeBytes;
       size <= sizeof(buffer);
       ++size) {
    auto decoded =
        internal::HandshakePacket::Decode(pw::ConstByteSpan(buffer, size));
    PW_TEST_ASSERT_OK(decoded.status());
    EXPECT_EQ(decoded->type(), internal::HandshakePacket::Type::kSyn);
    EXPECT_EQ(decoded->version(), 2u);
  }
}

TEST(PacketTest, DecodeHandshakeRejectsTruncatedPacket) {
  std::byte buffer[internal::HandshakePacket::kWireSizeBytes];
  PW_TEST_ASSERT_OK(
      internal::HandshakePacket(internal::HandshakePacket::Type::kSyn)
          .Encode(buffer));
  EXPECT_EQ(internal::HandshakePacket::Decode(
                pw::ConstByteSpan(buffer).first(sizeof(buffer) - 1))
                .status(),
            pw::Status::DataLoss());
}

TEST(PacketTest, DecodeHandshakeRejectsVersionZero) {
  std::byte buffer[internal::HandshakePacket::kWireSizeBytes];
  PW_TEST_ASSERT_OK(
      internal::HandshakePacket(internal::HandshakePacket::Type::kSyn, 0)
          .Encode(buffer));
  EXPECT_EQ(internal::HandshakePacket::Decode(buffer).status(),
            pw::Status::DataLoss());
}

TEST(PacketTest, DecodeHandshakeRejectsUnknownType) {
  std::byte buffer[internal::HandshakePacket::kWireSizeBytes];
  PW_TEST_ASSERT_OK(
      internal::HandshakePacket(internal::HandshakePacket::Type::kSyn)
          .Encode(buffer));
  for (std::byte type : {std::byte{0}, std::byte{4}, std::byte{0xff}}) {
    buffer[offsetof(internal::HandshakeWireFormat, type)] = type;
    EXPECT_EQ(internal::HandshakePacket::Decode(buffer).status(),
              pw::Status::DataLoss());
  }
}

TEST(PacketTest, EncodeDecodeResponse) {
  pw::allocator::test::AllocatorForTest<256> allocator;

  constexpr std::string_view kPayload = "hello response";
  size_t total_size = sizeof(internal::PacketHeader) + kPayload.size();

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
  EXPECT_EQ(decoded.type(),
            (PacketType::Make<flags::kServer,
                              flags::kHasPayload,
                              flags::kOkTerminal>()));
  EXPECT_EQ(decoded.payload_offset(), sizeof(internal::PacketHeader));
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
      internal::OutboundPacket::ServerFinish(0x12345678).Encode(std::move(buf));
  ASSERT_EQ(encode_result.status(), pw::OkStatus());
  EXPECT_EQ(encode_result->size(), sizeof(internal::PacketHeader));
}

TEST(PacketTest, EncodeErrorTruncatesExtraBuffer) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  auto buf = pw::Buf::Allocate(allocator, 64);
  auto encode_result = internal::OutboundPacket::ServerError(
                           0x12345678, internal::ProtocolStatus::kUnknownMethod)
                           .Encode(std::move(buf));
  ASSERT_EQ(encode_result.status(), pw::OkStatus());
  EXPECT_EQ(encode_result->size(), sizeof(internal::ErrorWireFormat));
}

TEST(PacketTest, EncodeHandshakeTruncatesExtraBuffer) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  auto buf = pw::Buf::Allocate(allocator, 64);
  auto encode_result =
      internal::HandshakePacket(internal::HandshakePacket::Type::kSyn)
          .Encode(std::move(buf));
  ASSERT_EQ(encode_result.status(), pw::OkStatus());
  EXPECT_EQ(encode_result->size(), internal::HandshakePacket::kWireSizeBytes);

  auto decode_result = internal::HandshakePacket::Decode(*encode_result);
  ASSERT_EQ(decode_result.status(), pw::OkStatus());
  EXPECT_EQ(decode_result->type(), internal::HandshakePacket::Type::kSyn);
}

TEST(PacketTest, EncodeRejectsBufferSmallerThanHeader) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  auto buf =
      pw::Buf::Allocate(allocator, sizeof(internal::RequestWireFormat) - 1);
  EXPECT_EQ(internal::OutboundPacket::StartUnary(1, 2, 3)
                .Encode(std::move(buf))
                .status(),
            pw::Status::ResourceExhausted());
}

TEST(PacketTest, EncodeRejectsEmptyBuffer) {
  EXPECT_EQ(
      internal::OutboundPacket::ClientMessage(1).Encode(pw::Buf()).status(),
      pw::Status::FailedPrecondition());
}

TEST(PacketTest, ProtocolStatusToStatusAndToString) {
  using internal::ProtocolStatus;

  EXPECT_EQ(ToStatus(ProtocolStatus::kUnknown), pw::Status::Unknown());
  EXPECT_EQ(ToStatus(ProtocolStatus::kInternal), pw::Status::Internal());
  EXPECT_EQ(ToStatus(ProtocolStatus::kCancelled), pw::Status::Cancelled());
  EXPECT_EQ(ToStatus(ProtocolStatus::kReceivedPacketForWrongEndpoint),
            pw::Status::Unimplemented());
  EXPECT_EQ(ToStatus(ProtocolStatus::kMethodTypeMismatch),
            pw::Status::FailedPrecondition());
  EXPECT_EQ(ToStatus(ProtocolStatus::kDroppedWithoutResponse),
            pw::Status::Cancelled());
  EXPECT_EQ(ToStatus(ProtocolStatus::kServiceUnregistered),
            pw::Status::Cancelled());
  EXPECT_EQ(ToStatus(ProtocolStatus::kUnknownService), pw::Status::NotFound());
  EXPECT_EQ(ToStatus(ProtocolStatus::kUnknownMethod), pw::Status::NotFound());
  EXPECT_EQ(ToStatus(ProtocolStatus::kInvalidRequestPayload),
            pw::Status::DataLoss());
  EXPECT_EQ(ToStatus(ProtocolStatus::kFailedToAllocateCall),
            pw::Status::ResourceExhausted());
  EXPECT_EQ(
      ToStatus(ProtocolStatus::kFailedToAllocateCallResourcesWhileRunning),
      pw::Status::ResourceExhausted());
  EXPECT_EQ(ToStatus(ProtocolStatus::kOk), pw::Status::Internal());
  EXPECT_EQ(ToStatus(static_cast<ProtocolStatus>(0xff)), pw::Status::Unknown());

  EXPECT_STREQ(pw::EnumToString(ProtocolStatus::kOk), "OK");
  EXPECT_STREQ(pw::EnumToString(ProtocolStatus::kUnknown), "UNKNOWN");
  EXPECT_STREQ(pw::EnumToString(ProtocolStatus::kInternal), "INTERNAL");
  EXPECT_STREQ(pw::EnumToString(ProtocolStatus::kCancelled), "CANCELLED");
  EXPECT_STREQ(
      pw::EnumToString(ProtocolStatus::kReceivedPacketForWrongEndpoint),
      "RECEIVED_PACKET_FOR_WRONG_ENDPOINT");
  EXPECT_STREQ(pw::EnumToString(ProtocolStatus::kMethodTypeMismatch),
               "METHOD_TYPE_MISMATCH");
  EXPECT_STREQ(pw::EnumToString(ProtocolStatus::kDroppedWithoutResponse),
               "DROPPED_WITHOUT_RESPONSE");
  EXPECT_STREQ(pw::EnumToString(ProtocolStatus::kServiceUnregistered),
               "SERVICE_UNREGISTERED");
  EXPECT_STREQ(pw::EnumToString(ProtocolStatus::kUnknownService),
               "UNKNOWN_SERVICE");
  EXPECT_STREQ(pw::EnumToString(ProtocolStatus::kUnknownMethod),
               "UNKNOWN_METHOD");
  EXPECT_STREQ(pw::EnumToString(ProtocolStatus::kInvalidRequestPayload),
               "INVALID_REQUEST_PAYLOAD");
  EXPECT_STREQ(pw::EnumToString(ProtocolStatus::kFailedToAllocateCall),
               "FAILED_TO_ALLOCATE_CALL");
  EXPECT_STREQ(pw::EnumToString(
                   ProtocolStatus::kFailedToAllocateCallResourcesWhileRunning),
               "FAILED_TO_ALLOCATE_CALL_RESOURCES_WHILE_RUNNING");
}

static_assert(!internal::IsServerError(internal::ProtocolStatus::kOk));
static_assert(!internal::IsClientError(internal::ProtocolStatus::kOk));

static_assert(internal::IsServerError(internal::ProtocolStatus::kCancelled));
static_assert(internal::IsClientError(internal::ProtocolStatus::kCancelled));

static_assert(
    internal::IsServerError(internal::ProtocolStatus::kDroppedWithoutResponse));
static_assert(!internal::IsClientError(
    internal::ProtocolStatus::kDroppedWithoutResponse));

static_assert(!internal::IsServerError(static_cast<internal::ProtocolStatus>(
    static_cast<uint8_t>(internal::ProtocolStatus::kMethodTypeMismatch) + 1)));
static_assert(!internal::IsClientError(static_cast<internal::ProtocolStatus>(
    static_cast<uint8_t>(internal::ProtocolStatus::kMethodTypeMismatch) + 1)));

static_assert(internal::PacketSizeWithoutPayload(
                  PacketType::Make<flags::kStart,
                                   flags::kHasPayload,
                                   flags::kStreamEnd>()) == 13u);
static_assert(internal::PacketSizeWithoutPayload(
                  PacketType::Make<flags::kServer,
                                   flags::kHasPayload,
                                   flags::kOkTerminal>()) == 5u);
static_assert(internal::PacketSizeWithoutPayload(
                  PacketType::Make<flags::kHasPayload>()) == 5u);
static_assert(internal::PacketSizeWithoutPayload(
                  PacketType::Make<flags::kServer, flags::kHasPayload>()) ==
              5u);
static_assert(internal::PacketSizeWithoutPayload(
                  PacketType::Make<flags::kStreamEnd>()) == 5u);
static_assert(internal::PacketSizeWithoutPayload(
                  PacketType::Make<flags::kServer, flags::kOkTerminal>()) ==
              5u);
static_assert(internal::PacketSizeWithoutPayload(
                  PacketType::Make<flags::kErrorTerminal>()) == 7u);
static_assert(internal::PacketSizeWithoutPayload(
                  PacketType::Make<flags::kServer, flags::kErrorTerminal>()) ==
              7u);

static_assert(internal::OutboundPacket::StartUnary(1, 2, 3).payload_offset() ==
              13u);
static_assert(internal::OutboundPacket::StartStream(1, 2, 3).payload_offset() ==
              13u);
static_assert(internal::OutboundPacket::Response(1).payload_offset() == 5u);
static_assert(internal::OutboundPacket::ClientMessage(1).payload_offset() ==
              5u);
static_assert(internal::OutboundPacket::ServerMessage(1).payload_offset() ==
              5u);
static_assert(internal::OutboundPacket::ClientStreamEnd(1).payload_offset() ==
              5u);
static_assert(internal::OutboundPacket::ServerFinish(1).payload_offset() == 5u);
static_assert(internal::OutboundPacket::ClientError(
                  1, internal::ProtocolStatus::kCancelled)
                  .payload_offset() == 7u);
static_assert(internal::OutboundPacket::ServerError(
                  1, internal::ProtocolStatus::kCancelled)
                  .payload_offset() == 7u);

// A unary request and a streaming open are distinct on the wire, even when the
// request message is empty.
static_assert(internal::OutboundPacket::StartUnary(1, 2, 3).type() !=
              internal::OutboundPacket::StartStream(1, 2, 3).type());

TEST(PacketTest, EncodedMessageHeaderLayout) {
  std::array<std::byte, sizeof(internal::PacketHeader)> buffer = {};

  auto result =
      internal::OutboundPacket::ClientMessage(0x12345678).EncodeHeader(buffer);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(*result, buffer.size());

  // call_id is little endian and comes first, followed by the type byte (0x02).
  constexpr auto expected = pw::bytes::Array<0x78, 0x56, 0x34, 0x12, 0x02>();
  EXPECT_EQ(buffer, expected);
}

TEST(PacketTest, EncodedRequestHeaderLayout) {
  std::array<std::byte, sizeof(internal::RequestWireFormat)> buffer = {};

  auto result =
      internal::OutboundPacket::StartUnary(0x12345678, 0xabcdef01, 0x23456789)
          .EncodeHeader(buffer);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(*result, buffer.size());

  constexpr auto expected = pw::bytes::Array<0x78,
                                             0x56,
                                             0x34,
                                             0x12,
                                             0x0e,
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

TEST(PacketTest, EncodedErrorHeaderLayout) {
  std::array<std::byte, sizeof(internal::ErrorWireFormat)> buffer = {};

  auto result = internal::OutboundPacket::ServerError(
                    0x12345678, internal::ProtocolStatus::kUnknownMethod)
                    .EncodeHeader(buffer);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(*result, buffer.size());

  // 4-byte call_id (LE), 1-byte type (0x19), 2-byte error (LE).
  constexpr auto expected =
      pw::bytes::Array<0x78,
                       0x56,
                       0x34,
                       0x12,
                       0x19,
                       static_cast<uint8_t>(
                           internal::ProtocolStatus::kUnknownMethod),
                       0x00>();
  EXPECT_EQ(buffer, expected);

  auto decoded = internal::InboundPacket::Decode(pw::ConstBuf::Unowned(buffer));
  ASSERT_EQ(decoded.status(), pw::OkStatus());
  EXPECT_EQ(decoded->error(), internal::ProtocolStatus::kUnknownMethod);
}

TEST(PacketTest, DecodeErrorZeroMapsToInternal) {
  // A wire error of 0 (kOk) is never valid in an error packet.
  auto buffer = pw::bytes::Array<0x78, 0x56, 0x34, 0x12, 0x19, 0x00, 0x00>();

  auto decoded = internal::InboundPacket::Decode(pw::ConstBuf::Unowned(buffer));
  ASSERT_EQ(decoded.status(), pw::OkStatus());
  EXPECT_EQ(decoded->error(), internal::ProtocolStatus::kInternal);
  EXPECT_EQ(internal::ToStatus(decoded->error()), pw::Status::Internal());

  buffer[4] = static_cast<std::byte>(flags::kErrorTerminal);
  decoded = internal::InboundPacket::Decode(pw::ConstBuf::Unowned(buffer));
  ASSERT_EQ(decoded.status(), pw::OkStatus());
  EXPECT_EQ(decoded->error(), internal::ProtocolStatus::kInternal);
  EXPECT_EQ(internal::ToStatus(decoded->error()), pw::Status::Internal());
}

TEST(PacketTest, DecodeErrorOutOfRangeOrWrongRoleMapsToUnknown) {
  auto buffer = pw::bytes::Array<0x78, 0x56, 0x34, 0x12, 0x19, 0x00, 0x01>();

  // 0x0100 is beyond the largest known code.
  auto decoded = internal::InboundPacket::Decode(pw::ConstBuf::Unowned(buffer));
  ASSERT_EQ(decoded.status(), pw::OkStatus());
  EXPECT_EQ(decoded->error(), internal::ProtocolStatus::kUnknown);
  EXPECT_EQ(internal::ToStatus(decoded->error()), pw::Status::Unknown());

  buffer[4] = static_cast<std::byte>(flags::kErrorTerminal);
  decoded = internal::InboundPacket::Decode(pw::ConstBuf::Unowned(buffer));
  ASSERT_EQ(decoded.status(), pw::OkStatus());
  EXPECT_EQ(decoded->error(), internal::ProtocolStatus::kUnknown);
  EXPECT_EQ(internal::ToStatus(decoded->error()), pw::Status::Unknown());

  // Unassigned code in the gap between common and server-only bands.
  buffer[4] = static_cast<std::byte>(flags::kServer | flags::kErrorTerminal);
  buffer[5] = static_cast<std::byte>(
      static_cast<uint8_t>(internal::ProtocolStatus::kMethodTypeMismatch) + 1);
  buffer[6] = std::byte{0x00};
  decoded = internal::InboundPacket::Decode(pw::ConstBuf::Unowned(buffer));
  ASSERT_EQ(decoded.status(), pw::OkStatus());
  EXPECT_EQ(decoded->error(), internal::ProtocolStatus::kUnknown);

  // One past the maximum defined code.
  buffer[4] = static_cast<std::byte>(flags::kServer | flags::kErrorTerminal);
  buffer[5] = static_cast<std::byte>(
      static_cast<uint8_t>(pw::EnumTraits<internal::ProtocolStatus>::kMax) + 1);
  decoded = internal::InboundPacket::Decode(pw::ConstBuf::Unowned(buffer));
  ASSERT_EQ(decoded.status(), pw::OkStatus());
  EXPECT_EQ(decoded->error(), internal::ProtocolStatus::kUnknown);

  // Server-only code in a client error packet maps to kUnknown.
  buffer[4] = static_cast<std::byte>(flags::kErrorTerminal);
  buffer[5] = static_cast<std::byte>(internal::ProtocolStatus::kUnknownMethod);
  decoded = internal::InboundPacket::Decode(pw::ConstBuf::Unowned(buffer));
  ASSERT_EQ(decoded.status(), pw::OkStatus());
  EXPECT_EQ(decoded->error(), internal::ProtocolStatus::kUnknown);
}

TEST(PacketTest, EncodeHeaderReportsTotalSizeWithoutWritingPayloadLength) {
  constexpr auto buffer_init = [](size_t i) {
    return static_cast<std::byte>(i + 1);
  };
  auto buffer = pw::bytes::Initialized<64>(buffer_init);

  auto result = internal::OutboundPacket::Response(1).EncodeHeader(buffer, 10);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(*result, sizeof(internal::PacketHeader) + 10u);

  // Nothing past the header was modified.
  for (size_t i = sizeof(internal::PacketHeader); i < buffer.size(); ++i) {
    EXPECT_EQ(buffer[i], buffer_init(i));
  }
}

TEST(PacketTest, EncodeHeaderRejectsBufferSmallerThanHeaderPlusPayload) {
  std::array<std::byte, sizeof(internal::PacketHeader) + 4> buffer = {};

  EXPECT_EQ(internal::OutboundPacket::ClientMessage(1)
                .EncodeHeader(buffer, 5)
                .status(),
            pw::Status::ResourceExhausted());
  EXPECT_EQ(internal::OutboundPacket::ClientMessage(1)
                .EncodeHeader(buffer, 4)
                .status(),
            pw::OkStatus());
}

TEST(PacketTest, DecodedPayloadIsEverythingAfterTheHeader) {
  pw::allocator::test::AllocatorForTest<256> allocator;

  // The payload length is not on the wire, so all trailing bytes belong to the
  // payload regardless of what was passed to EncodeHeader.
  constexpr size_t kTrailingBytes = 7;
  auto buf = pw::Buf::Allocate(allocator,
                               sizeof(internal::PacketHeader) + kTrailingBytes);

  auto encode_result = internal::OutboundPacket::ClientMessage(42).EncodeHeader(
      pw::ByteSpan(buf));
  ASSERT_EQ(encode_result.status(), pw::OkStatus());
  ASSERT_EQ(*encode_result, sizeof(internal::PacketHeader));

  auto decode_result =
      internal::InboundPacket::Decode(pw::ConstBuf(std::move(buf)));
  ASSERT_EQ(decode_result.status(), pw::OkStatus());

  internal::InboundPacket decoded = std::move(decode_result.value());
  EXPECT_EQ(decoded.type(), PacketType::Make<flags::kHasPayload>());
  EXPECT_EQ(decoded.call_id(), 42u);
  EXPECT_EQ(decoded.payload().size(), kTrailingBytes);
}

}  // namespace
