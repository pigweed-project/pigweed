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
#include "pw_compilation_testing/negative_compilation.h"
#include "pw_enum/traits.h"
#include "pw_rpc2/internal/packet_type.h"
#include "pw_rpc2/internal/protocol_error.h"
#include "pw_rpc2/status.h"
#include "pw_unit_test/framework.h"

namespace {

namespace internal = ::pw::rpc2::internal;

using ::pw::rpc2::RpcStatus;

using internal::EndpointRole;
using internal::PacketType;
using internal::ProtocolError;

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
    if (type.is_for(type.sender())) {
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
static_assert(internal::OutboundPacket::ClientError(1,
                                                    ProtocolError::kCancelled)
                  .type()
                  .bits() == 0x18);
static_assert(internal::OutboundPacket::ServerError(1,
                                                    ProtocolError::kCancelled)
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

  pw::Buf encoded_buf = packet.Encode(std::move(buf));
  ASSERT_NE(encoded_buf, nullptr);
  EXPECT_EQ(encoded_buf.size(), total_size);

  internal::InboundPacket decoded =
      internal::InboundPacket::Decode(pw::ConstBuf(std::move(encoded_buf)));
  ASSERT_NE(decoded, nullptr);

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

    pw::Buf encoded_buf = packet.Encode(std::move(buf));
    ASSERT_NE(encoded_buf, nullptr);
    EXPECT_EQ(encoded_buf.size(), total_size);

    internal::InboundPacket decoded =
        internal::InboundPacket::Decode(pw::ConstBuf(std::move(encoded_buf)));
    ASSERT_NE(decoded, nullptr);

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

    pw::Buf encoded_buf = internal::OutboundPacket::Finish(sender, 0x12345678)
                              .Encode(std::move(buf));
    ASSERT_NE(encoded_buf, nullptr);
    EXPECT_EQ(encoded_buf.size(), total_size);

    internal::InboundPacket decoded =
        internal::InboundPacket::Decode(pw::ConstBuf(std::move(encoded_buf)));
    ASSERT_NE(decoded, nullptr);

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
    pw::Buf encoded_buf = internal::OutboundPacket::ClientError(
                              0x12345678, ProtocolError::kCancelled)
                              .Encode(std::move(buf));
    ASSERT_NE(encoded_buf, nullptr);
    EXPECT_EQ(encoded_buf.size(), total_size);

    internal::InboundPacket decoded =
        internal::InboundPacket::Decode(pw::ConstBuf(std::move(encoded_buf)));
    ASSERT_NE(decoded, nullptr);

    EXPECT_EQ(decoded.type(), PacketType::Make<flags::kErrorTerminal>());
    EXPECT_TRUE(decoded.type().is_error());
    EXPECT_EQ(decoded.call_id(), 0x12345678u);
    EXPECT_EQ(decoded.error(), ProtocolError::kCancelled);
  }

  {
    auto buf = pw::Buf::Allocate(allocator, total_size);
    pw::Buf encoded_buf = internal::OutboundPacket::ServerError(
                              0x12345678, ProtocolError::kUnknownMethod)
                              .Encode(std::move(buf));
    ASSERT_NE(encoded_buf, nullptr);
    EXPECT_EQ(encoded_buf.size(), total_size);

    internal::InboundPacket decoded =
        internal::InboundPacket::Decode(pw::ConstBuf(std::move(encoded_buf)));
    ASSERT_NE(decoded, nullptr);

    EXPECT_EQ(decoded.type(),
              (PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
    EXPECT_TRUE(decoded.type().is_error());
    EXPECT_EQ(decoded.call_id(), 0x12345678u);
    EXPECT_EQ(decoded.error(), ProtocolError::kUnknownMethod);
  }
}

TEST(PacketTest, DecodeBufferTooShortForHeader) {
  pw::allocator::test::AllocatorForTest<256> allocator;

  auto buf = pw::Buf::Allocate(allocator, sizeof(internal::PacketHeader) - 1);
  EXPECT_EQ(internal::InboundPacket::Decode(pw::ConstBuf(std::move(buf))),
            nullptr);
}

TEST(PacketTest, DecodeBufferTooShortForType) {
  pw::allocator::test::AllocatorForTest<256> allocator;

  // Long enough for the common header, but not for a request header.
  auto buf = pw::Buf::Allocate(allocator, sizeof(internal::PacketHeader));
  ASSERT_FALSE(internal::OutboundPacket::StartUnary(1, 2, 3)
                   .EncodeHeader(pw::ByteSpan(buf))
                   .has_value());
  // Write just the type byte by hand, since the header does not fit.
  buf[offsetof(internal::PacketHeader, type)] = static_cast<std::byte>(
      flags::kStart | flags::kHasPayload | flags::kStreamEnd);

  EXPECT_EQ(internal::InboundPacket::Decode(pw::ConstBuf(std::move(buf))),
            nullptr);
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

    EXPECT_EQ(internal::InboundPacket::Decode(pw::ConstBuf(std::move(buf))),
              nullptr);
  }
}

TEST(PacketTest, EncodeDecodeStartStream) {
  pw::allocator::test::AllocatorForTest<256> allocator;

  // Extra buffer space is truncated away: an open carries no payload.
  auto buf = pw::Buf::Allocate(allocator, 64);
  pw::Buf encoded_buf =
      internal::OutboundPacket::StartStream(1, 2, 3).Encode(std::move(buf));
  ASSERT_NE(encoded_buf, nullptr);
  EXPECT_EQ(encoded_buf.size(), sizeof(internal::RequestWireFormat));

  auto decode_result =
      internal::InboundPacket::Decode(pw::ConstBuf(std::move(encoded_buf)));
  ASSERT_NE(decode_result, nullptr);
  EXPECT_EQ(decode_result.type(), PacketType::Make<flags::kStart>());
  EXPECT_EQ(decode_result.call_id(), 1u);
  EXPECT_EQ(decode_result.service_id(), 2u);
  EXPECT_EQ(decode_result.method_id(), 3u);
  EXPECT_TRUE(decode_result.payload().empty());
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
    ASSERT_NE(decode_result, nullptr);
    EXPECT_EQ(decode_result.type(), type);
    EXPECT_EQ(decode_result.call_id(), 1u);
    EXPECT_EQ(decode_result.service_id(), 8u);
    EXPECT_EQ(decode_result.method_id(), 9u);
    EXPECT_TRUE(decode_result.payload().empty());
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
    EXPECT_EQ(internal::InboundPacket::Decode(pw::ConstBuf(std::move(buf))),
              nullptr);
  }
}

TEST(PacketTest, EncodeDecodeHandshakePacket) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  size_t total_size = internal::HandshakePacket::kWireSizeBytes;
  EXPECT_EQ(total_size, 8u);

  auto buf = pw::Buf::Allocate(allocator, total_size);

  internal::HandshakePacket packet(internal::HandshakePacket::Type::kSyn);

  pw::Buf encoded_buf = packet.Encode(std::move(buf));
  ASSERT_NE(encoded_buf, nullptr);
  EXPECT_EQ(encoded_buf.size(), total_size);

  auto decode_result = internal::HandshakePacket::Decode(encoded_buf);
  ASSERT_TRUE(decode_result.has_value());

  internal::HandshakePacket decoded = decode_result.value();
  EXPECT_EQ(decoded.version(), 1u);
  EXPECT_EQ(decoded.type(), internal::HandshakePacket::Type::kSyn);
}

TEST(PacketTest, DecodeZeroLengthPayload) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  size_t total_size = sizeof(internal::RequestWireFormat);

  auto buf = pw::Buf::Allocate(allocator, total_size);

  pw::Buf encoded_buf =
      internal::OutboundPacket::StartUnary(0x12345678, 0x100, 0x200)
          .Encode(std::move(buf));
  ASSERT_NE(encoded_buf, nullptr);
  EXPECT_EQ(encoded_buf.size(), total_size);

  internal::InboundPacket decoded =
      internal::InboundPacket::Decode(pw::ConstBuf(std::move(encoded_buf)));
  ASSERT_NE(decoded, nullptr);

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
  EXPECT_FALSE(internal::HandshakePacket::Decode(buf).has_value());
}

TEST(PacketTest, HandshakePacketDefaultsToMaxVersion) {
  const internal::HandshakePacket packet(
      internal::HandshakePacket::Type::kSynAck);
  EXPECT_EQ(packet.version(), internal::HandshakePacket::kMaxVersion);
}

TEST(PacketTest, EncodeDecodeHandshakePacketVersion) {
  std::byte buffer[internal::HandshakePacket::kWireSizeBytes];
  ASSERT_TRUE(
      internal::HandshakePacket(internal::HandshakePacket::Type::kAck, 7)
          .Encode(buffer));
  EXPECT_EQ(buffer[offsetof(internal::HandshakeWireFormat, version)],
            std::byte{7});

  auto decoded = internal::HandshakePacket::Decode(buffer);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->type(), internal::HandshakePacket::Type::kAck);
  EXPECT_EQ(decoded->version(), 7u);
}

TEST(PacketTest, DecodeHandshakeIgnoresTrailingAndReservedBytes) {
  // A later protocol version may send a longer handshake packet and use the
  // reserved field. Both must be ignored so that negotiation still works.
  std::byte buffer[internal::HandshakePacket::kWireSizeBytes + 5];
  std::fill(std::begin(buffer), std::end(buffer), std::byte{0xee});
  ASSERT_TRUE(
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
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->type(), internal::HandshakePacket::Type::kSyn);
    EXPECT_EQ(decoded->version(), 2u);
  }
}

TEST(PacketTest, DecodeHandshakeRejectsTruncatedPacket) {
  std::byte buffer[internal::HandshakePacket::kWireSizeBytes];
  ASSERT_TRUE(internal::HandshakePacket(internal::HandshakePacket::Type::kSyn)
                  .Encode(buffer));
  EXPECT_FALSE(internal::HandshakePacket::Decode(
                   pw::ConstByteSpan(buffer).first(sizeof(buffer) - 1))
                   .has_value());
}

TEST(PacketTest, DecodeHandshakeRejectsVersionZero) {
  std::byte buffer[internal::HandshakePacket::kWireSizeBytes];
  ASSERT_TRUE(
      internal::HandshakePacket(internal::HandshakePacket::Type::kSyn, 0)
          .Encode(buffer));
  EXPECT_FALSE(internal::HandshakePacket::Decode(buffer).has_value());
}

TEST(PacketTest, DecodeHandshakeRejectsUnknownType) {
  std::byte buffer[internal::HandshakePacket::kWireSizeBytes];
  ASSERT_TRUE(internal::HandshakePacket(internal::HandshakePacket::Type::kSyn)
                  .Encode(buffer));
  for (std::byte type : {std::byte{0}, std::byte{4}, std::byte{0xff}}) {
    buffer[offsetof(internal::HandshakeWireFormat, type)] = type;
    EXPECT_FALSE(internal::HandshakePacket::Decode(buffer).has_value());
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

  pw::Buf encoded_buf = packet.Encode(std::move(buf));
  ASSERT_NE(encoded_buf, nullptr);
  EXPECT_EQ(encoded_buf.size(), total_size);

  internal::InboundPacket decoded =
      internal::InboundPacket::Decode(pw::ConstBuf(std::move(encoded_buf)));
  ASSERT_NE(decoded, nullptr);

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
  pw::Buf encoded_buf =
      internal::OutboundPacket::ServerFinish(0x12345678).Encode(std::move(buf));
  ASSERT_NE(encoded_buf, nullptr);
  EXPECT_EQ(encoded_buf.size(), sizeof(internal::PacketHeader));
}

TEST(PacketTest, EncodeErrorTruncatesExtraBuffer) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  auto buf = pw::Buf::Allocate(allocator, 64);
  pw::Buf encoded_buf = internal::OutboundPacket::ServerError(
                            0x12345678, ProtocolError::kUnknownMethod)
                            .Encode(std::move(buf));
  ASSERT_NE(encoded_buf, nullptr);
  EXPECT_EQ(encoded_buf.size(), sizeof(internal::ErrorWireFormat));
}

TEST(PacketTest, EncodeHandshakeTruncatesExtraBuffer) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  auto buf = pw::Buf::Allocate(allocator, 64);
  pw::Buf encoded_buf =
      internal::HandshakePacket(internal::HandshakePacket::Type::kSyn)
          .Encode(std::move(buf));
  ASSERT_NE(encoded_buf, nullptr);
  EXPECT_EQ(encoded_buf.size(), internal::HandshakePacket::kWireSizeBytes);

  auto decode_result = internal::HandshakePacket::Decode(encoded_buf);
  ASSERT_TRUE(decode_result.has_value());
  EXPECT_EQ(decode_result->type(), internal::HandshakePacket::Type::kSyn);
}

TEST(PacketTest, EncodeRejectsBufferSmallerThanHeader) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  auto buf =
      pw::Buf::Allocate(allocator, sizeof(internal::RequestWireFormat) - 1);
  EXPECT_EQ(
      internal::OutboundPacket::StartUnary(1, 2, 3).Encode(std::move(buf)),
      nullptr);
}

TEST(PacketTest, EncodeRejectsEmptyBuffer) {
  EXPECT_EQ(internal::OutboundPacket::ClientMessage(1).Encode(pw::Buf()),
            nullptr);
}

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
                  PacketType::Make<flags::kErrorTerminal>()) == 6u);
static_assert(internal::PacketSizeWithoutPayload(
                  PacketType::Make<flags::kServer, flags::kErrorTerminal>()) ==
              6u);

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
static_assert(internal::OutboundPacket::ClientError(1,
                                                    ProtocolError::kCancelled)
                  .payload_offset() == 6u);
static_assert(internal::OutboundPacket::ServerError(1,
                                                    ProtocolError::kCancelled)
                  .payload_offset() == 6u);

// A unary request and a streaming open are distinct on the wire, even when the
// request message is empty.
static_assert(internal::OutboundPacket::StartUnary(1, 2, 3).type() !=
              internal::OutboundPacket::StartStream(1, 2, 3).type());

TEST(PacketTest, EncodedMessageHeaderLayout) {
  std::array<std::byte, sizeof(internal::PacketHeader)> buffer = {};

  auto result =
      internal::OutboundPacket::ClientMessage(0x12345678).EncodeHeader(buffer);
  ASSERT_TRUE(result.has_value());
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
  ASSERT_TRUE(result.has_value());
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
                    0x12345678, ProtocolError::kUnknownMethod)
                    .EncodeHeader(buffer);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, buffer.size());

  // 4-byte call_id (LE), 1-byte type (0x19), 1-byte error.
  constexpr auto expected =
      pw::bytes::Array<0x78,
                       0x56,
                       0x34,
                       0x12,
                       0x19,
                       static_cast<uint8_t>(ProtocolError::kUnknownMethod)>();
  EXPECT_EQ(buffer, expected);

  auto decoded = internal::InboundPacket::Decode(pw::ConstBuf::Unowned(buffer));
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded.error(), ProtocolError::kUnknownMethod);
}

TEST(PacketTest, DecodeErrorWithTrailingBytesIsMalformed) {
  // An error packet has no payload, so it must end after its 1-byte error.
  auto buffer =
      pw::bytes::Array<0x78,
                       0x56,
                       0x34,
                       0x12,
                       0x19,
                       static_cast<uint8_t>(ProtocolError::kUnknownMethod),
                       0x00>();
  EXPECT_EQ(internal::InboundPacket::Decode(pw::ConstBuf::Unowned(buffer)),
            nullptr);
}

TEST(PacketTest, DecodeErrorReturnsCodeExactlyAsReceived) {
  constexpr auto kServerError =
      static_cast<std::byte>(flags::kServer | flags::kErrorTerminal);
  constexpr auto kClientError = static_cast<std::byte>(flags::kErrorTerminal);
  auto buffer = pw::bytes::Array<0x78, 0x56, 0x34, 0x12, 0x19, 0x00>();

  // Unassigned codes are derived from the last code in each band.
  constexpr auto kUnassignedEither = static_cast<uint8_t>(
      static_cast<uint8_t>(ProtocolError::kReceivedPacketForUnknownCall) + 1);
  constexpr auto kUnassignedServer = static_cast<uint8_t>(
      static_cast<uint8_t>(ProtocolError::kMethodReturnedWithoutFinishing) + 1);
  static_assert(
      !pw::IsValidEnum(static_cast<ProtocolError>(kUnassignedEither)));
  static_assert(
      !pw::IsValidEnum(static_cast<ProtocolError>(kUnassignedServer)));
  constexpr auto kCancelled = static_cast<uint8_t>(ProtocolError::kCancelled);
  constexpr auto kUnknownMethod =
      static_cast<uint8_t>(ProtocolError::kUnknownMethod);

  struct Case {
    std::byte type;
    uint8_t code;
    RpcStatus expected;
  };
  const Case kCases[] = {
      // Codes the sender may send.
      {kServerError, kCancelled, RpcStatus::Cancelled()},
      {kClientError, kCancelled, RpcStatus::Cancelled()},
      {kServerError, kUnknownMethod, RpcStatus::Unimplemented()},
      // Zero is never sent.
      {kServerError, 0x00, RpcStatus::ProtocolError()},
      {kClientError, 0x00, RpcStatus::ProtocolError()},
      // A server-only code from a client.
      {kClientError, kUnknownMethod, RpcStatus::ProtocolError()},
      // Unassigned codes.
      {kServerError, kUnassignedEither, RpcStatus::ProtocolError()},
      {kClientError, kUnassignedEither, RpcStatus::ProtocolError()},
      {kServerError, kUnassignedServer, RpcStatus::ProtocolError()},
      {kServerError, 0x7f, RpcStatus::ProtocolError()},
      {kClientError, 0x80, RpcStatus::ProtocolError()},
      // Reserved codes.
      {kServerError, 0xc0, RpcStatus::ProtocolError()},
      {kClientError, 0xff, RpcStatus::ProtocolError()},
  };

  for (const Case& test : kCases) {
    buffer[4] = test.type;
    buffer[5] = std::byte{test.code};
    auto decoded =
        internal::InboundPacket::Decode(pw::ConstBuf::Unowned(buffer));
    ASSERT_NE(decoded, nullptr);

    // The code is not validated or normalized when decoded.
    EXPECT_EQ(static_cast<uint8_t>(decoded.error()), test.code);
    EXPECT_EQ(decoded.type().sender(),
              test.type == kServerError ? EndpointRole::kServer
                                        : EndpointRole::kClient);
    EXPECT_EQ(internal::PeerErrorToRpcStatus(decoded.type().sender(),
                                             decoded.error()),
              test.expected);
  }
}

TEST(PacketTest, EncodeHeaderReportsTotalSizeWithoutWritingPayloadLength) {
  constexpr auto buffer_init = [](size_t i) {
    return static_cast<std::byte>(i + 1);
  };
  auto buffer = pw::bytes::Initialized<64>(buffer_init);

  auto result = internal::OutboundPacket::Response(1).EncodeHeader(buffer, 10);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, sizeof(internal::PacketHeader) + 10u);

  // Nothing past the header was modified.
  for (size_t i = sizeof(internal::PacketHeader); i < buffer.size(); ++i) {
    EXPECT_EQ(buffer[i], buffer_init(i));
  }
}

TEST(PacketTest, EncodeHeaderRejectsBufferSmallerThanHeaderPlusPayload) {
  std::array<std::byte, sizeof(internal::PacketHeader) + 4> buffer = {};

  EXPECT_FALSE(internal::OutboundPacket::ClientMessage(1)
                   .EncodeHeader(buffer, 5)
                   .has_value());
  EXPECT_TRUE(internal::OutboundPacket::ClientMessage(1)
                  .EncodeHeader(buffer, 4)
                  .has_value());
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
  ASSERT_TRUE(encode_result.has_value());
  ASSERT_EQ(*encode_result, sizeof(internal::PacketHeader));

  internal::InboundPacket decoded =
      internal::InboundPacket::Decode(pw::ConstBuf(std::move(buf)));
  ASSERT_NE(decoded, nullptr);

  EXPECT_EQ(decoded.type(), PacketType::Make<flags::kHasPayload>());
  EXPECT_EQ(decoded.call_id(), 42u);
  EXPECT_EQ(decoded.payload().size(), kTrailingBytes);
}

#if PW_NC_TEST(MakePacketTypeWithTwoCloseModes)
PW_NC_EXPECT("Packet flags must not overlap; pass each close mode alone");
[[maybe_unused]] constexpr PacketType kBadType =
    PacketType::Make<flags::kStreamEnd, flags::kOkTerminal>();
#elif PW_NC_TEST(MakePacketTypeWithOverlappingFlags)
PW_NC_EXPECT("Packet flags must not overlap; pass each close mode alone");
[[maybe_unused]] constexpr PacketType kBadType =
    PacketType::Make<flags::kServer, flags::kServer>();
#elif PW_NC_TEST(MakePacketTypeWithInvalidFlagCombination)
PW_NC_EXPECT("Invalid combination of packet flags");
[[maybe_unused]] constexpr PacketType kBadType =
    PacketType::Make<flags::kServer, flags::kStart>();
#endif  // PW_NC_TEST

}  // namespace
