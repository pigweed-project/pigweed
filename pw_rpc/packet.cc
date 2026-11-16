// Copyright 2020 The Pigweed Authors
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

#include "pw_rpc/internal/packet.h"

#include "pw_log/log.h"
#include "pw_protobuf/decoder.h"
#include "pw_status/status.h"

namespace pw::rpc::internal {

using pwpb::PacketType;

namespace RpcPacket = pwpb::RpcPacket;

Result<Packet> Packet::FromBuffer(ConstByteSpan data) {
  Packet packet;
  Status status;
  protobuf::Decoder decoder(data);

  while ((status = decoder.Next()).ok()) {
    RpcPacket::Fields field =
        static_cast<RpcPacket::Fields>(decoder.FieldNumber());

    switch (field) {
      case RpcPacket::Fields::kType: {
        uint32_t value;
        // A decode error will propagate from Next() and terminate the loop.
        decoder.ReadUint32(&value).IgnoreError();
        packet.set_type(static_cast<PacketType>(value));
        break;
      }

      case RpcPacket::Fields::kChannelId:
        // A decode error will propagate from Next() and terminate the loop.
        decoder.ReadUint32(&packet.channel_id_).IgnoreError();
        break;

      case RpcPacket::Fields::kServiceId:
        // A decode error will propagate from Next() and terminate the loop.
        decoder.ReadFixed32(&packet.service_id_).IgnoreError();
        break;

      case RpcPacket::Fields::kMethodId:
        // A decode error will propagate from Next() and terminate the loop.
        decoder.ReadFixed32(&packet.method_id_).IgnoreError();
        break;

      case RpcPacket::Fields::kPayload:
        // A decode error will propagate from Next() and terminate the loop.
        decoder.ReadBytes(&packet.payload_).IgnoreError();
        break;

      case RpcPacket::Fields::kStatus: {
        uint32_t value;
        // A decode error will propagate from Next() and terminate the loop.
        decoder.ReadUint32(&value).IgnoreError();
        packet.set_status(StatusFromCodeOrUnknown(value));
        break;
      }

      case RpcPacket::Fields::kCallId:
        // A decode error will propagate from Next() and terminate the loop.
        decoder.ReadUint32(&packet.call_id_).IgnoreError();
        break;
    }
  }

  if (status.IsDataLoss()) {
    return status;
  }

  return packet;
}

Result<ConstByteSpan> Packet::Encode(ByteSpan buffer) const {
  RpcPacket::MemoryEncoder rpc_packet(buffer);

  // The payload is encoded first, as it may share the encode buffer.
  if (!payload_.empty()) {
    rpc_packet.WritePayload(payload_).IgnoreError();
  }

  rpc_packet.WriteType(type_).IgnoreError();
  rpc_packet.WriteChannelId(channel_id_).IgnoreError();
  rpc_packet.WriteServiceId(service_id_).IgnoreError();
  rpc_packet.WriteMethodId(method_id_).IgnoreError();

  // Status code 0 is OK. In protobufs, 0 is the default int value, so skip
  // encoding it to save two bytes in the output.
  if (status_.code() != 0) {
    rpc_packet.WriteStatus(status_.code()).IgnoreError();
  }

  if (call_id_ != 0) {
    rpc_packet.WriteCallId(call_id_).IgnoreError();
  }

  if (rpc_packet.status().ok()) {
    return ConstByteSpan(rpc_packet);
  }
  return rpc_packet.status();
}

size_t Packet::MinEncodedSizeBytes() const {
  size_t reserved_size = 0;

  reserved_size += 1;  // channel_id key
  reserved_size += varint::EncodedSize(channel_id());
  reserved_size += 1;  // call_id key
  reserved_size += varint::EncodedSize(call_id());
  reserved_size += 1 + sizeof(uint32_t);  // service_id key and fixed32
  reserved_size += 1 + sizeof(uint32_t);  // method_id key and fixed32

  // Packet type always takes two bytes to encode (varint key + varint enum).
  reserved_size += 2;

  // Status field takes up to two bytes to encode (varint key + varint status).
  reserved_size += 2;

  // Payload field takes at least two bytes to encode (varint key + length).
  reserved_size += 2;

  return reserved_size;
}

void Packet::DebugLog() const {
  PW_LOG_INFO(
      "Packet {\n"
      "  Type   : %s (%d)\n"
      "  Channel: %u\n"
      "  Service: %08x\n"
      "  Method : %08x\n"
      "  ID     : %08x\n"
      "  Payload: %u B\n"
      "  Status : %s\n"
      "}",
      PacketTypeToString(type_),
      static_cast<int>(type_),
      static_cast<unsigned>(channel_id_),
      static_cast<unsigned>(service_id_),
      static_cast<unsigned>(method_id_),
      static_cast<unsigned>(call_id_),
      static_cast<unsigned>(payload_.size()),
      status_.str());
}

}  // namespace pw::rpc::internal
