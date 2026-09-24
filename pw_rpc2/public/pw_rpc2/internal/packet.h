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
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "pw_assert/assert.h"
#include "pw_buf/buf.h"
#include "pw_bytes/endian.h"
#include "pw_bytes/span.h"
#include "pw_preprocessor/compiler.h"
#include "pw_result/result.h"
#include "pw_status/status.h"

namespace pw::rpc2::internal {

/// Identifies the role of an RPC endpoint on a connection.
enum class EndpointRole : uint8_t {
  kClient = 0,  // Sends even PacketTypes (bit 0 == 0), expects odd (1).
  kServer = 1,  // Sends odd PacketTypes (bit 0 == 1), expects even (0).
};

/// Identifies the role and wire format of a regular protocol packet in pw_rpc2.
///
/// These packets are exchanged after the initial handshake is established.
/// Encoded in the packet header to indicate how the packet is framed,
/// decoded, and processed.
///
/// Client-to-server packets use even values (`bit 0 == 0`) and server-to-client
/// packets use odd values (`bit 0 == 1`). Values are paired so that clearing
/// bit 0 maps a server packet to its client counterpart, and ordered so that
/// payload-carrying packets (`< kClientStreamEnd`) precede payload-free control
/// packets (`>= kClientStreamEnd`).
enum class PacketType : uint8_t {
  /// Initiates an RPC invocation from client to server.
  kRequest = 0x02,

  /// Completes a unary or client-streaming RPC from server to client and
  /// delivers a response payload.
  kResponse = 0x03,

  /// Carries streaming payload data from client to server.
  kClientMessage = 0x04,

  /// Carries streaming payload data from server to client.
  kServerMessage = 0x05,

  /// Signals the completion of a client stream.
  kClientStreamEnd = 0x06,

  /// Signals the completion of a server stream.
  kServerStreamEnd = 0x07,

  /// Terminates an RPC from the client with an error status.
  kClientError = 0x08,

  /// Terminates an RPC from the server with an error status.
  kServerError = 0x09,
};

/// True if `type` is a recognized `PacketType`.
constexpr bool IsValidPacketType(PacketType type) {
  return type >= PacketType::kRequest && type <= PacketType::kServerError;
}

/// True if `type` is a valid packet type addressed to `destination`.
constexpr bool IsPacketFor(PacketType type, EndpointRole destination) {
  // Rebase so valid types are 0-7, then keep bit 0 (direction) and bits 3+
  // (out of range, including values below kRequest, which wrap around). The
  // result matches only for in-range types sent by the opposite role.
  const uint8_t offset =
      static_cast<uint8_t>(type) - static_cast<uint8_t>(PacketType::kRequest);
  return (offset & ~uint8_t{0x06}) == (static_cast<uint8_t>(destination) ^ 1u);
}

/// True if `type` is `kClientError` or `kServerError`.
constexpr bool IsError(PacketType type) {
  return type == PacketType::kClientError || type == PacketType::kServerError;
}

/// True if `type` carries trailing payload bytes (`kRequest`, `kResponse`,
/// `kClientMessage`, `kServerMessage`).
constexpr bool HasPayload(PacketType type) {
  return type < PacketType::kClientStreamEnd;
}

/// Common prefix of every regular protocol packet (following the handshake).
PW_PACKED(struct) PacketHeader {
  uint32_t call_id;
  uint8_t type;
};

/// Initiates an RPC invocation. Sent by the client to request execution of
/// a specific service method, optionally followed by request payload bytes.
PW_PACKED(struct) RequestWireFormat {
  PacketHeader header;
  uint32_t service_id;
  uint32_t method_id;
};

/// Carries streaming payload data for an in-flight RPC in either direction.
PW_PACKED(struct) MessageWireFormat { PacketHeader header; };

/// Completes a unary RPC or delivers a response payload from server to client.
PW_PACKED(struct) ResponseWireFormat { PacketHeader header; };

/// Signals the end of a stream in one direction without an error.
PW_PACKED(struct) StreamEndWireFormat { PacketHeader header; };

/// Signals that an RPC has terminated abnormally with an error status code.
PW_PACKED(struct) ErrorWireFormat {
  PacketHeader header;
  uint32_t status_code;
};

/// Connection handshake packet for protocol negotiation and compatibility
/// verification.
///
/// Handshake packets are sent first before any regular protocol packets. They
/// include a magic value to verify that the remote peer is an RPC endpoint
/// before establishing the session.
PW_PACKED(struct) HandshakeWireFormat {
  uint32_t magic;
  uint8_t version;
  uint8_t type;
  uint16_t reserved;
};

static_assert(sizeof(PacketHeader) == 5);
static_assert(sizeof(RequestWireFormat) == 13);
static_assert(sizeof(MessageWireFormat) == 5);
static_assert(sizeof(ResponseWireFormat) == 5);
static_assert(sizeof(StreamEndWireFormat) == 5);
static_assert(sizeof(ErrorWireFormat) == 9);
static_assert(sizeof(HandshakeWireFormat) == 8);

/// Returns the wire format size for `type` excluding any trailing payload
/// bytes.
constexpr size_t PacketSizeWithoutPayload(PacketType type) {
  PW_DASSERT(IsValidPacketType(type));
  if (type == PacketType::kRequest) {
    return sizeof(RequestWireFormat);
  }
  if (IsError(type)) {
    return sizeof(ErrorWireFormat);
  }
  return sizeof(PacketHeader);
}

class HandshakePacket {
 public:
  enum class Type : uint8_t {
    kSyn = 1,
    kSynAck = 2,
    kAck = 3,
  };

  static constexpr size_t kWireSizeBytes = sizeof(HandshakeWireFormat);

  static Result<HandshakePacket> Decode(ConstByteSpan bytes);

  explicit HandshakePacket(Type type) : type_(type) {}

  uint8_t version() const { return kVersion; }
  Type type() const { return type_; }

  Status Encode(ByteSpan buffer) const;
  Result<Buf> Encode(Buf buffer) const;

 private:
  static constexpr uint32_t kMagic = 0x43505250;  // 'PRPC'
  static constexpr uint8_t kVersion = 1;

  Type type_;
};

/// A packet to be written to a connection.
///
/// This is a small value type that describes a packet header; the payload
/// bytes are written separately by whoever holds the transport reservation.
/// Copies of it are stored per pending write, so it is kept compact.
class OutboundPacket {
 public:
  static constexpr OutboundPacket Request(uint32_t call_id,
                                          uint32_t service_id,
                                          uint32_t method_id) {
    return OutboundPacket(
        PacketType::kRequest, call_id, Fields(service_id, method_id));
  }

  static constexpr OutboundPacket Message(EndpointRole sender,
                                          uint32_t call_id) {
    return OutboundPacket(sender == EndpointRole::kClient
                              ? PacketType::kClientMessage
                              : PacketType::kServerMessage,
                          call_id,
                          Fields());
  }

  static constexpr OutboundPacket ClientMessage(uint32_t call_id) {
    return Message(EndpointRole::kClient, call_id);
  }

  static constexpr OutboundPacket ServerMessage(uint32_t call_id) {
    return Message(EndpointRole::kServer, call_id);
  }

  static constexpr OutboundPacket Response(uint32_t call_id) {
    return OutboundPacket(PacketType::kResponse, call_id, Fields());
  }

  static constexpr OutboundPacket StreamEnd(EndpointRole sender,
                                            uint32_t call_id) {
    return OutboundPacket(sender == EndpointRole::kClient
                              ? PacketType::kClientStreamEnd
                              : PacketType::kServerStreamEnd,
                          call_id,
                          Fields());
  }

  static constexpr OutboundPacket ClientStreamEnd(uint32_t call_id) {
    return StreamEnd(EndpointRole::kClient, call_id);
  }

  static constexpr OutboundPacket ServerStreamEnd(uint32_t call_id) {
    return StreamEnd(EndpointRole::kServer, call_id);
  }

  static constexpr OutboundPacket Error(EndpointRole sender,
                                        uint32_t call_id,
                                        Status status) {
    return OutboundPacket(sender == EndpointRole::kClient
                              ? PacketType::kClientError
                              : PacketType::kServerError,
                          call_id,
                          Fields(status));
  }

  constexpr OutboundPacket()
      : OutboundPacket(PacketType::kClientMessage, 0, Fields()) {}

  OutboundPacket(const OutboundPacket&) = default;
  OutboundPacket& operator=(const OutboundPacket&) = default;
  OutboundPacket(OutboundPacket&&) noexcept = default;
  OutboundPacket& operator=(OutboundPacket&&) noexcept = default;

  constexpr PacketType type() const { return type_; }
  constexpr uint32_t call_id() const { return call_id_; }

  uint32_t service_id() const {
    PW_DASSERT(type_ == PacketType::kRequest);
    return fields_.request.service_id;
  }

  uint32_t method_id() const {
    PW_DASSERT(type_ == PacketType::kRequest);
    return fields_.request.method_id;
  }

  Status status() const {
    PW_DASSERT(IsError(type_));
    return fields_.status;
  }

  constexpr size_t payload_offset() const {
    return PacketSizeWithoutPayload(type_);
  }

  /// Encodes this packet's header at the start of `buffer`.
  ///
  /// `payload_len` is not written to the wire; it is only used to check that
  /// `buffer` is large enough for the header plus payload, and is included in
  /// the returned total packet size.
  Result<size_t> EncodeHeader(ByteSpan buffer, size_t payload_len = 0) const;

  Result<Buf> Encode(Buf buffer, size_t payload_len) const;
  Result<Buf> Encode(Buf buffer) const;

 private:
  struct RequestIds {
    uint32_t service_id;
    uint32_t method_id;
  };

  // The header fields that are specific to one packet type. Which member is
  // live is determined by `type_`, and the public accessors check it before
  // reading. Overlaying them keeps an `OutboundPacket` at 16 bytes rather than
  // 20, which matters because one is stored in every pending write.
  union Fields {
    constexpr Fields() : request{0, 0} {}
    constexpr Fields(uint32_t service_id, uint32_t method_id)
        : request{service_id, method_id} {}
    constexpr explicit Fields(Status error_status) : status(error_status) {}

    RequestIds request;
    Status status;
  };

  constexpr OutboundPacket(PacketType type, uint32_t call_id, Fields fields)
      : type_(type), call_id_(call_id), fields_(fields) {}

  PacketType type_;
  uint32_t call_id_;
  Fields fields_;
};

static_assert(std::is_trivially_copyable_v<OutboundPacket>);

/// An incoming packet received from a connection.
///
/// `InboundPacket` wraps an owned buffer (`ConstBuf`) containing an encoded
/// packet, validates the header on decoding, and provides accessors to the
/// header fields and the trailing payload data.
class InboundPacket {
 public:
  static Result<InboundPacket> Decode(ConstBuf&& buffer);

  constexpr InboundPacket() = default;
  constexpr InboundPacket(std::nullptr_t) noexcept : InboundPacket() {}

  InboundPacket(InboundPacket&&) noexcept = default;
  InboundPacket& operator=(InboundPacket&&) noexcept = default;

  InboundPacket(const InboundPacket&) = delete;
  InboundPacket& operator=(const InboundPacket&) = delete;

  [[nodiscard]] friend constexpr bool operator==(const InboundPacket& lhs,
                                                 std::nullptr_t) noexcept {
    return lhs.buffer_ == nullptr;
  }
  [[nodiscard]] friend constexpr bool operator==(
      std::nullptr_t, const InboundPacket& rhs) noexcept {
    return rhs.buffer_ == nullptr;
  }
  [[nodiscard]] friend constexpr bool operator!=(const InboundPacket& lhs,
                                                 std::nullptr_t) noexcept {
    return lhs.buffer_ != nullptr;
  }
  [[nodiscard]] friend constexpr bool operator!=(
      std::nullptr_t, const InboundPacket& rhs) noexcept {
    return rhs.buffer_ != nullptr;
  }

  PacketType type() const {
    return static_cast<PacketType>(buffer_[offsetof(PacketHeader, type)]);
  }

  uint32_t call_id() const {
    return ReadUint32(offsetof(PacketHeader, call_id));
  }

  uint32_t service_id() const {
    PW_DASSERT(type() == PacketType::kRequest);
    return ReadUint32(offsetof(RequestWireFormat, service_id));
  }

  uint32_t method_id() const {
    PW_DASSERT(type() == PacketType::kRequest);
    return ReadUint32(offsetof(RequestWireFormat, method_id));
  }

  Status status() const {
    PW_DASSERT(IsError(type()));
    uint32_t status_code = ReadUint32(offsetof(ErrorWireFormat, status_code));
    return Status(static_cast<Status::Code>(status_code));
  }

  size_t payload_offset() const { return PacketSizeWithoutPayload(type()); }

  ConstByteSpan payload() const {
    return ConstByteSpan(buffer_).subspan(payload_offset());
  }

  /// Slices and returns the payload into an owned `ConstBuf`.
  [[nodiscard]] ConstBuf TakePayload() && {
    return Slice(std::move(buffer_), payload_offset());
  }

 private:
  explicit InboundPacket(ConstBuf&& buffer) : buffer_(std::move(buffer)) {}

  uint32_t ReadUint32(size_t offset) const {
    return bytes::ReadInOrder<uint32_t>(endian::little,
                                        buffer_.data() + offset);
  }

  ConstBuf buffer_;
};

}  // namespace pw::rpc2::internal
