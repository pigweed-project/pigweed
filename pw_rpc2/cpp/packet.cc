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

void WriteUint16(ByteSpan buffer, size_t offset, uint16_t value) {
  bytes::CopyInOrder<uint16_t>(endian::little, value, buffer.data() + offset);
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
  if (buffer.size() > kWireSizeBytes) {
    buffer = Truncate(std::move(buffer), kWireSizeBytes);
  }
  PW_TRY(Encode(ByteSpan(buffer)));
  return buffer;
}

// State PacketType value assumptions used to validate and classify the type
// byte.
static_assert(static_cast<uint8_t>(PacketType::kRequest) % 2 == 0);
static_assert(static_cast<uint8_t>(PacketType::kResponse) ==
              static_cast<uint8_t>(PacketType::kRequest) + 1);
static_assert(static_cast<uint8_t>(PacketType::kClientMessage) ==
              static_cast<uint8_t>(PacketType::kRequest) + 2);
static_assert(static_cast<uint8_t>(PacketType::kServerMessage) ==
              static_cast<uint8_t>(PacketType::kClientMessage) + 1);
static_assert(static_cast<uint8_t>(PacketType::kClientStreamEnd) ==
              static_cast<uint8_t>(PacketType::kClientMessage) + 2);
static_assert(static_cast<uint8_t>(PacketType::kServerStreamEnd) ==
              static_cast<uint8_t>(PacketType::kClientStreamEnd) + 1);
static_assert(static_cast<uint8_t>(PacketType::kClientError) ==
              static_cast<uint8_t>(PacketType::kClientStreamEnd) + 2);
static_assert(static_cast<uint8_t>(PacketType::kServerError) ==
              static_cast<uint8_t>(PacketType::kClientError) + 1);

// DecodeErrorCode returns these codes for both ServerError and ClientError.
static_assert(static_cast<uint16_t>(ServerError::kInternal) ==
              static_cast<uint16_t>(ClientError::kInternal));
static_assert(static_cast<uint16_t>(ServerError::kUnknown) ==
              static_cast<uint16_t>(ClientError::kUnknown));

uint16_t InboundPacket::DecodeErrorCode(uint16_t max_code) const {
  PW_DASSERT(IsError(type()));
  const uint16_t code = bytes::ReadInOrder<uint16_t>(
      endian::little, buffer_.data() + offsetof(ErrorWireFormat, error));
  if (code == 0) {
    return static_cast<uint16_t>(ServerError::kInternal);
  }
  if (code > max_code) {
    return static_cast<uint16_t>(ServerError::kUnknown);
  }
  return code;
}

Result<InboundPacket> InboundPacket::Decode(ConstBuf&& buffer) {
  if (buffer.size() < sizeof(PacketHeader)) {
    return Status::DataLoss();  // Too short for header
  }

  const auto type =
      static_cast<PacketType>(buffer[offsetof(PacketHeader, type)]);
  if (!IsValidPacketType(type)) {
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

  if (type_ == PacketType::kRequest) {
    WriteUint32(buffer,
                offsetof(RequestWireFormat, service_id),
                fields_.request.service_id);
    WriteUint32(buffer,
                offsetof(RequestWireFormat, method_id),
                fields_.request.method_id);
  } else if (IsError(type_)) {
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
  if (HasPayload(type_)) {
    const size_t offset = payload_offset();
    if (buffer.size() > offset) {
      payload_len = buffer.size() - offset;
    }
  }
  return Encode(std::move(buffer), payload_len);
}

}  // namespace pw::rpc2::internal
