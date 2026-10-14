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

#include <cstddef>
#include <cstring>

#include "pw_bytes/endian.h"
#include "pw_enum/traits.h"
#include "pw_log/log.h"

namespace pw::rpc2::internal {
namespace {

void WriteUint32(ByteSpan buffer, size_t offset, uint32_t value) {
  bytes::CopyInOrder<uint32_t>(endian::little, value, buffer.data() + offset);
}

}  // namespace

std::optional<HandshakePacket> HandshakePacket::Decode(ConstByteSpan bytes) {
  // Later protocol versions may extend the handshake packet, so trailing bytes
  // (and the reserved field) are ignored rather than rejected. This lets a peer
  // that only knows this version still negotiate with a newer one.
  if (bytes.size() < sizeof(HandshakeWireFormat)) {
    PW_LOG_WARN("Invalid handshake packet: %u bytes is shorter than %u",
                static_cast<unsigned>(bytes.size()),
                static_cast<unsigned>(sizeof(HandshakeWireFormat)));
    return std::nullopt;
  }
  const uint32_t magic = bytes::ReadInOrder<uint32_t>(
      endian::little, bytes.data() + offsetof(HandshakeWireFormat, magic));
  if (magic != kMagic) {
    PW_LOG_WARN("Invalid handshake packet: magic 0x%08x is not 0x%08x",
                static_cast<unsigned>(magic),
                static_cast<unsigned>(kMagic));
    return std::nullopt;
  }
  const uint8_t version =
      static_cast<uint8_t>(bytes[offsetof(HandshakeWireFormat, version)]);
  if (version == 0) {
    PW_LOG_WARN("Invalid handshake packet: version 0");
    return std::nullopt;
  }
  const uint8_t type =
      static_cast<uint8_t>(bytes[offsetof(HandshakeWireFormat, type)]);
  if (!pw::IsValidEnum<Type>(type)) {
    PW_LOG_WARN("Invalid handshake packet: unknown type %u",
                static_cast<unsigned>(type));
    return std::nullopt;
  }
  return HandshakePacket(static_cast<Type>(type), version);
}

bool HandshakePacket::Encode(ByteSpan buffer) const {
  if (buffer.size() < sizeof(HandshakeWireFormat)) {
    return false;
  }
  std::memset(buffer.data(), 0, sizeof(HandshakeWireFormat));
  WriteUint32(buffer, offsetof(HandshakeWireFormat, magic), kMagic);
  buffer[offsetof(HandshakeWireFormat, version)] =
      static_cast<std::byte>(version_);
  buffer[offsetof(HandshakeWireFormat, type)] = static_cast<std::byte>(type_);
  return true;
}

Buf HandshakePacket::Encode(Buf buffer) const {
  if (buffer.size() > kWireSizeBytes) {
    buffer = Truncate(std::move(buffer), kWireSizeBytes);
  }
  if (!Encode(ByteSpan(buffer))) {
    return nullptr;
  }
  return buffer;
}

InboundPacket InboundPacket::Decode(ConstBuf&& buffer) {
  if (buffer.size() < sizeof(PacketHeader)) {
    PW_LOG_WARN(
        "Malformed RPC packet: %u bytes is shorter than the %u-byte header",
        static_cast<unsigned>(buffer.size()),
        static_cast<unsigned>(sizeof(PacketHeader)));
    return nullptr;
  }

  const auto type_bits =
      static_cast<uint8_t>(buffer[offsetof(PacketHeader, type)]);
  if (!PacketType::IsValid(type_bits)) {
    PW_LOG_WARN("Malformed RPC packet: invalid type 0x%02x",
                static_cast<unsigned>(type_bits));
    return nullptr;
  }
  const PacketType type = PacketType::FromValidatedBits(type_bits);

  // The payload length is not encoded on the wire. The transport frames the
  // packet, so the payload is exactly the bytes following the header. A packet
  // without `kHasPayload` has no payload, so it must end with its header.
  const size_t header_size = PacketSizeWithoutPayload(type);
  if (type.has_payload() && buffer.size() < header_size) {
    PW_LOG_WARN(
        "Malformed RPC packet: %u bytes is too short for the %u-byte header "
        "of type 0x%02x",
        static_cast<unsigned>(buffer.size()),
        static_cast<unsigned>(header_size),
        static_cast<unsigned>(type_bits));
    return nullptr;
  }
  if (!type.has_payload() && buffer.size() != header_size) {
    PW_LOG_WARN(
        "Malformed RPC packet: type 0x%02x is a %u-byte header with no "
        "payload, but the packet is %u bytes",
        static_cast<unsigned>(type_bits),
        static_cast<unsigned>(header_size),
        static_cast<unsigned>(buffer.size()));
    return nullptr;
  }

  return InboundPacket(std::move(buffer));
}

std::optional<size_t> OutboundPacket::EncodeHeader(ByteSpan buffer,
                                                   size_t payload_len) const {
  const size_t offset = payload_offset();

  // Checked this way around so that a large `payload_len` cannot overflow.
  if (buffer.size() < offset || buffer.size() - offset < payload_len) {
    return std::nullopt;
  }

  WriteUint32(buffer, offsetof(PacketHeader, call_id), call_id_);
  buffer[offsetof(PacketHeader, type)] = static_cast<std::byte>(type_.bits());

  if (type_.is_start()) {
    WriteUint32(buffer,
                offsetof(RequestWireFormat, service_id),
                fields_.request.service_id);
    WriteUint32(buffer,
                offsetof(RequestWireFormat, method_id),
                fields_.request.method_id);
  } else if (type_.is_error()) {
    buffer[offsetof(ErrorWireFormat, error)] =
        static_cast<std::byte>(fields_.error);
  }

  return offset + payload_len;
}

Buf OutboundPacket::Encode(Buf buffer, size_t payload_len) const {
  size_t offset = payload_offset();
  if (buffer.size() > offset + payload_len) {
    buffer = Truncate(std::move(buffer), offset + payload_len);
  }
  if (!EncodeHeader(buffer, payload_len).has_value()) {
    return nullptr;
  }
  return buffer;
}

Buf OutboundPacket::Encode(Buf buffer) const {
  size_t payload_len = 0;
  if (type_.has_payload()) {
    const size_t offset = payload_offset();
    if (buffer.size() > offset) {
      payload_len = buffer.size() - offset;
    }
  }
  return Encode(std::move(buffer), payload_len);
}

}  // namespace pw::rpc2::internal
