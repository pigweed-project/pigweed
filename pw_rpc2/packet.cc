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

#include "pw_assert/check.h"
#include "pw_bytes/endian.h"
#include "pw_status/try.h"

namespace pw::rpc2::internal {
namespace {

void WriteUint32(ByteSpan buffer, size_t offset, uint32_t value) {
  bytes::CopyInOrder<uint32_t>(endian::little, value, buffer.data() + offset);
}

}  // namespace

Result<HandshakePacket> HandshakePacket::Decode(ConstByteSpan bytes) {
  if (bytes.size() != sizeof(HandshakeWireFormat)) {
    return Status::DataLoss();
  }
  uint32_t magic = bytes::ReadInOrder<uint32_t>(
      endian::little, bytes.data() + offsetof(HandshakeWireFormat, magic));
  uint8_t version =
      static_cast<uint8_t>(bytes[offsetof(HandshakeWireFormat, version)]);
  Type type = static_cast<Type>(bytes[offsetof(HandshakeWireFormat, type)]);
  if (magic != kMagic || version != kVersion ||
      (type != Type::kSyn && type != Type::kSynAck && type != Type::kAck)) {
    return Status::DataLoss();
  }
  return HandshakePacket(static_cast<Type>(type));
}

Status HandshakePacket::Encode(ByteSpan buffer) const {
  if (buffer.size() < sizeof(HandshakeWireFormat)) {
    return Status::ResourceExhausted();
  }
  std::memset(buffer.data(), 0, sizeof(HandshakeWireFormat));
  WriteUint32(buffer, offsetof(HandshakeWireFormat, magic), kMagic);
  buffer[offsetof(HandshakeWireFormat, version)] =
      static_cast<std::byte>(kVersion);
  buffer[offsetof(HandshakeWireFormat, type)] = static_cast<std::byte>(type_);
  return OkStatus();
}

Result<Buf> HandshakePacket::Encode(Buf buffer) const {
  PW_TRY(Encode(ByteSpan(buffer)));
  return buffer;
}

// State PacketType value assumptions used in Decode to validate the type byte.
static_assert(PacketType::kRequest <= PacketType::kMessage);
static_assert(PacketType::kMessage <= PacketType::kStreamEnd);
static_assert(PacketType::kStreamEnd <= PacketType::kError);
static_assert(PacketType::kError <= PacketType::kResponse);

Result<InboundPacket> InboundPacket::Decode(ConstBuf&& buffer) {
  if (buffer.size() < sizeof(PacketHeader)) {
    return Status::DataLoss();  // Too short for header
  }

  const auto type =
      static_cast<PacketType>(buffer[offsetof(PacketHeader, type)]);
  if (type < PacketType::kRequest || type > PacketType::kResponse) {
    return Status::InvalidArgument();
  }
  if (buffer.size() < PacketSizeWithoutPayload(type)) {
    return Status::DataLoss();
  }

  // The payload length is not encoded on the wire. The transport frames the
  // packet, so the payload is exactly the bytes following the header.
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
  buffer[offsetof(PacketHeader, type)] = static_cast<std::byte>(type_);

  switch (type_) {
    case PacketType::kRequest: {
      WriteUint32(buffer,
                  offsetof(RequestWireFormat, service_id),
                  fields_.request.service_id);
      WriteUint32(buffer,
                  offsetof(RequestWireFormat, method_id),
                  fields_.request.method_id);
      break;
    }

    case PacketType::kMessage:
    case PacketType::kResponse:
    case PacketType::kStreamEnd: {
      // These packet types carry no header fields beyond the common header.
      break;
    }

    case PacketType::kError: {
      WriteUint32(
          buffer, offsetof(ErrorWireFormat, status_code), status().code());
      break;
    }
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
  if (type_ != PacketType::kStreamEnd && type_ != PacketType::kError) {
    const size_t offset = payload_offset();
    if (buffer.size() > offset) {
      payload_len = buffer.size() - offset;
    }
  }
  return Encode(std::move(buffer), payload_len);
}

}  // namespace pw::rpc2::internal
