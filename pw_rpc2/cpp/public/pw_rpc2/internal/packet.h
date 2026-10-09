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
#include "pw_enum/traits.h"
#include "pw_preprocessor/compiler.h"
#include "pw_result/result.h"
#include "pw_rpc2/internal/packet_type.h"
#include "pw_rpc2/internal/protocol_status.h"
#include "pw_status/status.h"

namespace pw::rpc2::internal {

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

/// Signals that an RPC has terminated abnormally with a protocol error.
PW_PACKED(struct) ErrorWireFormat {
  PacketHeader header;
  uint8_t error;
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
static_assert(sizeof(ErrorWireFormat) == 6);
static_assert(sizeof(HandshakeWireFormat) == 8);

/// Returns the wire format size for `type` excluding any trailing payload
/// bytes.
///
/// A valid type never both starts a call and carries an error code, so at most
/// one of the optional header extensions is present.
constexpr size_t PacketSizeWithoutPayload(PacketType type) {
  if (type.is_start()) {
    return sizeof(RequestWireFormat);
  }
  if (type.is_error()) {
    return sizeof(ErrorWireFormat);
  }
  return sizeof(PacketHeader);
}

/// A decoded or to-be-encoded handshake packet.
///
/// Each handshake packet carries a protocol version. In a `kSyn` it is the
/// initiator's highest supported version; in a `kSynAck` and `kAck` it is the
/// version negotiated for the connection.
///
/// For forward compatibility, decoding accepts any non-zero version and
/// packets longer than `kWireSizeBytes`: trailing bytes and the reserved field
/// are ignored, so that later protocol versions can extend the handshake
/// without breaking negotiation with older peers.
class HandshakePacket {
 public:
  enum class Type : uint8_t {
    kSyn = 1,
    kSynAck = 2,
    kAck = 3,
  };

  /// The highest protocol version this implementation supports.
  static constexpr uint8_t kMaxVersion = 1;

  /// The size of the handshake packets this implementation sends, and the
  /// minimum size it accepts.
  static constexpr size_t kWireSizeBytes = sizeof(HandshakeWireFormat);

  /// Decodes a handshake packet.
  ///
  /// @returns
  /// * @OK: The packet was decoded. Bytes past `kWireSizeBytes` are ignored.
  /// * @DATA_LOSS: The packet is shorter than `kWireSizeBytes`, has the wrong
  ///   magic value, has version 0, or has an unknown type.
  static Result<HandshakePacket> Decode(ConstByteSpan bytes);

  explicit constexpr HandshakePacket(Type type, uint8_t version = kMaxVersion)
      : type_(type), version_(version) {}

  uint8_t version() const { return version_; }
  Type type() const { return type_; }

  Status Encode(ByteSpan buffer) const;
  Result<Buf> Encode(Buf buffer) const;

 private:
  static constexpr uint32_t kMagic = 0x43505250;  // 'PRPC'

  Type type_;
  uint8_t version_;
};

/// A packet to be written to a connection.
///
/// This is a small value type that describes a packet header; the payload
/// bytes are written separately by whoever holds the transport reservation.
/// Copies of it are stored per pending write, so it is kept compact.
class OutboundPacket {
 private:
  // These are declared before the public factories that use them so that the
  // factories can be evaluated in constant expressions.

  // Tests frame packets of arbitrary types, including ones the C++ endpoints
  // never send.
  friend class PacketFramer;

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
    constexpr explicit Fields(ProtocolStatus protocol_error)
        : error(protocol_error) {}

    RequestIds request;
    ProtocolStatus error;
  };

  // A packet that starts a call, so its header includes the method to invoke.
  template <flags::Flag... kFlags>
  static constexpr OutboundPacket Start(uint32_t call_id,
                                        uint32_t service_id,
                                        uint32_t method_id) {
    constexpr PacketType kType = PacketType::Make<kFlags...>();
    static_assert(kType.is_start());
    return OutboundPacket(kType, call_id, Fields(service_id, method_id));
  }

  // A packet for an existing call that carries no error code, so its header is
  // only the common `PacketHeader`.
  template <flags::Flag... kFlags>
  static constexpr OutboundPacket Basic(uint32_t call_id) {
    constexpr PacketType kType = PacketType::Make<kFlags...>();
    static_assert(!kType.is_start() && !kType.is_error());
    return OutboundPacket(kType, call_id, Fields());
  }

 public:
  /// Starts a unary or server-streaming call with its only request message,
  /// closing the client's stream. The message may be empty.
  static constexpr OutboundPacket StartUnary(uint32_t call_id,
                                             uint32_t service_id,
                                             uint32_t method_id) {
    return Start<flags::kStart, flags::kHasPayload, flags::kStreamEnd>(
        call_id, service_id, method_id);
  }

  /// Starts a client-streaming or bidirectional-streaming call without sending
  /// a message. The client's stream remains open.
  static constexpr OutboundPacket StartStream(uint32_t call_id,
                                              uint32_t service_id,
                                              uint32_t method_id) {
    return Start<flags::kStart>(call_id, service_id, method_id);
  }

  static constexpr OutboundPacket Message(EndpointRole sender,
                                          uint32_t call_id) {
    return sender == EndpointRole::kServer ? ServerMessage(call_id)
                                           : ClientMessage(call_id);
  }

  static constexpr OutboundPacket ClientMessage(uint32_t call_id) {
    return Basic<flags::kHasPayload>(call_id);
  }

  static constexpr OutboundPacket ServerMessage(uint32_t call_id) {
    return Basic<flags::kServer, flags::kHasPayload>(call_id);
  }

  /// Completes a unary or client-streaming call with its response.
  static constexpr OutboundPacket Response(uint32_t call_id) {
    return Basic<flags::kServer, flags::kHasPayload, flags::kOkTerminal>(
        call_id);
  }

  /// Finishes `sender`'s outbound stream normally.
  ///
  /// A client half-closes its stream (`CloseMode::kStreamEnd`). A server
  /// always ends the RPC when it finishes its stream
  /// (`CloseMode::kOkTerminal`), so that a client still streaming requests
  /// stops immediately instead of writing to a call that is gone.
  static constexpr OutboundPacket Finish(EndpointRole sender,
                                         uint32_t call_id) {
    return sender == EndpointRole::kServer ? ServerFinish(call_id)
                                           : ClientStreamEnd(call_id);
  }

  static constexpr OutboundPacket ClientStreamEnd(uint32_t call_id) {
    return Basic<flags::kStreamEnd>(call_id);
  }

  static constexpr OutboundPacket ServerFinish(uint32_t call_id) {
    return Basic<flags::kServer, flags::kOkTerminal>(call_id);
  }

  static constexpr OutboundPacket Error(EndpointRole sender,
                                        uint32_t call_id,
                                        ProtocolStatus error) {
    return sender == EndpointRole::kServer ? ServerError(call_id, error)
                                           : ClientError(call_id, error);
  }

  static constexpr OutboundPacket ClientError(uint32_t call_id,
                                              ProtocolStatus error) {
    PW_DASSERT(IsClientError(error));
    return OutboundPacket(
        PacketType::Make<flags::kErrorTerminal>(), call_id, Fields(error));
  }

  static constexpr OutboundPacket ServerError(uint32_t call_id,
                                              ProtocolStatus error) {
    PW_DASSERT(IsServerError(error));
    return OutboundPacket(
        PacketType::Make<flags::kServer, flags::kErrorTerminal>(),
        call_id,
        Fields(error));
  }

  constexpr OutboundPacket()
      : OutboundPacket(PacketType::Make<flags::kHasPayload>(), 0, Fields()) {}

  OutboundPacket(const OutboundPacket&) = default;
  OutboundPacket& operator=(const OutboundPacket&) = default;
  OutboundPacket(OutboundPacket&&) noexcept = default;
  OutboundPacket& operator=(OutboundPacket&&) noexcept = default;

  constexpr PacketType type() const { return type_; }
  constexpr uint32_t call_id() const { return call_id_; }

  /// True if this packet closes the sender's stream: a client's stream end
  /// (including a unary request), or a server's end of the RPC.
  constexpr bool closes_stream() const {
    return type_.close_mode() != CloseMode::kOpen;
  }

  uint32_t service_id() const {
    PW_DASSERT(type_.is_start());
    return fields_.request.service_id;
  }

  uint32_t method_id() const {
    PW_DASSERT(type_.is_start());
    return fields_.request.method_id;
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
    // `Decode()` validated the type byte.
    return PacketType::FromValidatedBits(
        static_cast<uint8_t>(buffer_[offsetof(PacketHeader, type)]));
  }

  uint32_t call_id() const {
    return ReadUint32(offsetof(PacketHeader, call_id));
  }

  uint32_t service_id() const {
    PW_DASSERT(type().is_start());
    return ReadUint32(offsetof(RequestWireFormat, service_id));
  }

  uint32_t method_id() const {
    PW_DASSERT(type().is_start());
    return ReadUint32(offsetof(RequestWireFormat, method_id));
  }

  /// Returns the error code of an error packet.
  ///
  /// A zero wire code (never valid in an error packet) maps to
  /// `ProtocolStatus::kInternal`. Codes that are undefined, local-only, or that
  /// the sending endpoint may not send map to `ProtocolStatus::kUnknown`.
  ProtocolStatus error() const;

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
