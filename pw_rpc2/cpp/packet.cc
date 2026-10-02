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
#include <limits>

#include "pw_assert/check.h"
#include "pw_bytes/endian.h"
#include "pw_status/try.h"

namespace pw::rpc2::internal {
namespace {

void WriteUint32(ByteSpan buffer, size_t offset, uint32_t value) {
  bytes::CopyInOrder<uint32_t>(endian::little, value, buffer.data() + offset);
}

void WriteUint16(ByteSpan buffer, size_t offset, uint16_t value) {
  bytes::CopyInOrder<uint16_t>(endian::little, value, buffer.data() + offset);
}

}  // namespace

Result<HandshakePacket> HandshakePacket::Decode(ConstByteSpan bytes) {
  // Later protocol versions may extend the handshake packet, so trailing bytes
  // (and the reserved field) are ignored rather than rejected. This lets a peer
  // that only knows this version still negotiate with a newer one.
  if (bytes.size() < sizeof(HandshakeWireFormat)) {
    return Status::DataLoss();
  }
  uint32_t magic = bytes::ReadInOrder<uint32_t>(
      endian::little, bytes.data() + offsetof(HandshakeWireFormat, magic));
  uint8_t version =
      static_cast<uint8_t>(bytes[offsetof(HandshakeWireFormat, version)]);
  Type type = static_cast<Type>(bytes[offsetof(HandshakeWireFormat, type)]);
  if (magic != kMagic || version == 0 ||
      (type != Type::kSyn && type != Type::kSynAck && type != Type::kAck)) {
    return Status::DataLoss();
  }
  return HandshakePacket(type, version);
}

Status HandshakePacket::Encode(ByteSpan buffer) const {
  if (buffer.size() < sizeof(HandshakeWireFormat)) {
    return Status::ResourceExhausted();
  }
  std::memset(buffer.data(), 0, sizeof(HandshakeWireFormat));
  WriteUint32(buffer, offsetof(HandshakeWireFormat, magic), kMagic);
  buffer[offsetof(HandshakeWireFormat, version)] =
      static_cast<std::byte>(version_);
  buffer[offsetof(HandshakeWireFormat, type)] = static_cast<std::byte>(type_);
  return OkStatus();
}

Result<Buf> HandshakePacket::Encode(Buf buffer) const {
  if (buffer.size() > kWireSizeBytes) {
    buffer = Truncate(std::move(buffer), kWireSizeBytes);
  }
  PW_TRY(Encode(ByteSpan(buffer)));
  return buffer;
}

ProtocolStatus InboundPacket::error() const {
  PW_DASSERT(type().is_error());
  const uint16_t code = bytes::ReadInOrder<uint16_t>(
      endian::little, buffer_.data() + offsetof(ErrorWireFormat, error));
  if (code == 0) {
    return ProtocolStatus::kInternal;
  }
  if (code > std::numeric_limits<uint8_t>::max()) {
    return ProtocolStatus::kUnknown;
  }
  const auto status = static_cast<ProtocolStatus>(code);
  if (type().is_server() ? !IsServerError(status) : !IsClientError(status)) {
    return ProtocolStatus::kUnknown;
  }
  return status;
}

Result<InboundPacket> InboundPacket::Decode(ConstBuf&& buffer) {
  if (buffer.size() < sizeof(PacketHeader)) {
    return Status::DataLoss();  // Too short for header
  }

  const auto type_bits =
      static_cast<uint8_t>(buffer[offsetof(PacketHeader, type)]);
  if (!PacketType::IsValid(type_bits)) {
    return Status::InvalidArgument();
  }
  const PacketType type = PacketType::FromValidatedBits(type_bits);

  // The payload length is not encoded on the wire. The transport frames the
  // packet, so the payload is exactly the bytes following the header. A packet
  // without `kHasPayload` has no payload, so it must end with its header.
  const size_t header_size = PacketSizeWithoutPayload(type);
  if (type.has_payload() ? buffer.size() < header_size
                         : buffer.size() != header_size) {
    return Status::DataLoss();
  }

  return InboundPacket(std::move(buffer));
}

Result<size_t> OutboundPacket::EncodeHeader(ByteSpan buffer,
                                            size_t payload_len) const {
  const size_t offset = payload_offset();

  // Checked this way around so that a large `payload_len` cannot overflow.
  if (buffer.size() < offset || buffer.size() - offset < payload_len) {
    return Status::ResourceExhausted();
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
    WriteUint16(buffer, offsetof(ErrorWireFormat, error), fields_.raw_error);
  }

  return offset + payload_len;
}

Result<Buf> OutboundPacket::Encode(Buf buffer, size_t payload_len) const {
  if (buffer.empty()) {
    return Status::FailedPrecondition();
  }
  size_t offset = payload_offset();
  if (buffer.size() > offset + payload_len) {
    buffer = Truncate(std::move(buffer), offset + payload_len);
  }
  PW_TRY(EncodeHeader(buffer, payload_len));
  return buffer;
}

Result<Buf> OutboundPacket::Encode(Buf buffer) const {
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
