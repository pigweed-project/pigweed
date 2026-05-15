// Copyright 2023 The Pigweed Authors
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

// Definitions for the static and dynamic versions of the pw_rpc encoding
// buffer. Both version are compiled rot, but only one is instantiated,
// depending on the PW_RPC_DYNAMIC_ALLOCATION config option.
#pragma once

#include <array>

#include "pw_assert/assert.h"
#include "pw_bytes/span.h"
#include "pw_rpc/channel.h"
#include "pw_rpc/internal/config.h"
#include "pw_rpc/internal/lock.h"
#include "pw_rpc/internal/packet.h"
#include "pw_status/status_with_size.h"

#if PW_RPC_DYNAMIC_ALLOCATION

#include PW_RPC_DYNAMIC_CONTAINER_INCLUDE

#endif  // PW_RPC_DYNAMIC_ALLOCATION

namespace pw::rpc::internal {

// Wraps a statically allocated encoding buffer.
class StaticEncodingBuffer {
 public:
  constexpr StaticEncodingBuffer() = default;

  ByteSpan AllocatePayloadBuffer(size_t /*payload_size */)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
    return ByteSpan(buffer_).subspan(Packet::kMinEncodedSizeWithoutPayload);
  }
  ByteSpan GetPacketBuffer(size_t /* payload_size */)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
    return buffer_;
  }

 private:
  static_assert(MaxSafePayloadSize() > 0,
                "pw_rpc's encode buffer is too small to fit any data");

  static inline std::array<std::byte, cfg::kEncodingBufferSizeBytes> buffer_
      PW_GUARDED_BY(rpc_lock());
};

#if PW_RPC_DYNAMIC_ALLOCATION

// Wraps a dynamically allocated encoding buffer.
class DynamicEncodingBuffer {
 public:
  DynamicEncodingBuffer() = default;

  DynamicEncodingBuffer(DynamicEncodingBuffer&&) = default;
  DynamicEncodingBuffer& operator=(DynamicEncodingBuffer&&) = default;

  ByteSpan AllocatePayloadBuffer(size_t payload_size) {
    Allocate(payload_size);
    return ByteSpan(buffer_.data(), buffer_.size())
        .subspan(Packet::kMinEncodedSizeWithoutPayload);
  }

  void ResizeToPayload(size_t size) {
    buffer_.resize(size + Packet::kMinEncodedSizeWithoutPayload);
  }

  ConstByteSpan payload() const {
    return ConstByteSpan(buffer_.data(), buffer_.size())
        .subspan(Packet::kMinEncodedSizeWithoutPayload);
  }

  // Returns the buffer into which to encode the packet, allocating a new buffer
  // if necessary.
  ByteSpan GetPacketBuffer(size_t payload_size) {
    if (buffer_.empty()) {
      Allocate(payload_size);
    }
    return ByteSpan(buffer_.data(), buffer_.size());
  }

 private:
  void Allocate(size_t payload_size) {
    const size_t buffer_size =
        payload_size + Packet::kMinEncodedSizeWithoutPayload;
    PW_DASSERT(buffer_.empty());
    buffer_.resize(buffer_size);
  }

  PW_RPC_DYNAMIC_CONTAINER(std::byte) buffer_;
};

using EncodingBuffer = DynamicEncodingBuffer;

#else

using EncodingBuffer = StaticEncodingBuffer;

#endif  // PW_RPC_DYNAMIC_ALLOCATION

class EncodedPacket {
 public:
  EncodedPacket(EncodingBuffer&& buffer, size_t packet_size)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
#if PW_RPC_DYNAMIC_ALLOCATION
    buffer_ = std::move(buffer);
    buffer_.ResizeToPayload(packet_size);
#else
    payload_ = buffer.AllocatePayloadBuffer(packet_size).first(packet_size);
#endif
  }

  ConstByteSpan payload() const {
#if PW_RPC_DYNAMIC_ALLOCATION
    return buffer_.payload();
#else
    return payload_;
#endif
  }

 private:
#if PW_RPC_DYNAMIC_ALLOCATION
  DynamicEncodingBuffer buffer_;
#else
  ConstByteSpan payload_;
#endif
};

template <typename Proto, typename Encoder>
static Result<EncodedPacket> EncodeToPayloadBuffer(Proto& payload,
                                                   const Encoder& encoder)
    PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
  EncodingBuffer encoding_buffer;

  size_t size = 0;
  if constexpr (cfg::kDynamicAllocationEnabled<Proto>) {
    StatusWithSize payload_size = encoder.EncodedSizeBytes(payload);
    if (!payload_size.ok()) {
      return Status::Internal();
    }
    size = payload_size.size();
  } else {
    size = MaxSafePayloadSize();
  }

  ByteSpan buffer = encoding_buffer.AllocatePayloadBuffer(size);

  StatusWithSize result = encoder.Encode(payload, buffer);
  if (!result.ok()) {
    return result.status();
  }

  return EncodedPacket(std::move(encoding_buffer), result.size());
}

}  // namespace pw::rpc::internal
