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

// clang-format off
#include "pw_rpc/internal/log_config.h"  // PW_LOG_* macros must be first.

#include "pw_rpc/channel.h"
// clang-format on

#include <algorithm>

#include "pw_assert/check.h"
#include "pw_bytes/span.h"
#include "pw_log/log.h"
#include "pw_protobuf/decoder.h"
#include "pw_protobuf/find.h"
#include "pw_rpc/internal/config.h"
#include "pw_rpc/internal/encoding_buffer.h"
#include "pw_rpc/internal/packet.pwpb.h"

using pw::rpc::internal::pwpb::RpcPacket::Fields;

namespace pw::rpc {
namespace internal {

Status OverwriteChannelId(ByteSpan rpc_packet, uint32_t channel_id_under_128) {
  Result<ConstByteSpan> raw_field =
      protobuf::FindRaw(rpc_packet, Fields::kChannelId);
  if (!raw_field.ok()) {
    return Status::DataLoss();  // Unexpected packet format
  }
  if (raw_field->size() != 1u) {
    return Status::OutOfRange();
  }
  const_cast<std::byte*>(raw_field->data())[0] =
      static_cast<std::byte>(channel_id_under_128);
  return OkStatus();
}

Status ChannelBase::Send(const Packet& packet) {
  static constexpr bool kLogAllOutgoingPackets = false;
  if constexpr (kLogAllOutgoingPackets) {
    PW_LOG_INFO("pw_rpc channel sending RPC packet type %u for %u:%08x/%08x",
                static_cast<unsigned>(packet.type()),
                static_cast<unsigned>(packet.channel_id()),
                static_cast<unsigned>(packet.service_id()),
                static_cast<unsigned>(packet.method_id()));
  }

  EncodingBuffer encoding_buffer;
  ByteSpan buffer = encoding_buffer.GetPacketBuffer(packet.payload().size());

  Result encoded = packet.Encode(buffer);

  if (!encoded.ok()) {
    PW_LOG_ERROR(
        "Failed to encode RPC packet type %u to channel %u buffer, status %u",
        static_cast<unsigned>(packet.type()),
        static_cast<unsigned>(id()),
        encoded.status().code());
    return Status::Internal();
  }

  PW_CHECK_NOTNULL(output_);

  Status sent = output_->Send(encoded.value());

  if (!sent.ok()) {
    PW_LOG_ERROR("Channel %u failed to send packet with status %u",
                 static_cast<unsigned>(id()),
                 sent.code());
    // Channel implementers are free to return whichever status makes sense in
    // their context, but these are always mapped to UNKNOWN so the user-facing
    // functions (e.g. Finish()) always return a fixed set of statuses.
    return Status::Unknown();
  }
  return OkStatus();
}

size_t ChannelBase::MaxWriteSizeBytes() const {
  PW_DCHECK_NOTNULL(output_);
  return rpc::MaxSafePayloadSize(std::min(output_->MaximumTransmissionUnit(),
                                          cfg::kEncodingBufferSizeBytes));
}

}  // namespace internal

Result<uint32_t> ExtractChannelId(ConstByteSpan packet) {
  protobuf::Decoder decoder(packet);

  while (decoder.Next().ok()) {
    if (static_cast<Fields>(decoder.FieldNumber()) != Fields::kChannelId) {
      continue;
    }
    uint32_t channel_id;
    PW_TRY(decoder.ReadUint32(&channel_id));
    return channel_id;
  }

  return Status::DataLoss();
}

}  // namespace pw::rpc
