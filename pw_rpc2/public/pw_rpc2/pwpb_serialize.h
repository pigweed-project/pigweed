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

#include "pw_bytes/span.h"
#include "pw_protobuf/encoder.h"
#include "pw_protobuf/internal/codegen.h"
#include "pw_protobuf/stream_decoder.h"
#include "pw_result/result.h"
#include "pw_rpc2/serialize.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"
#include "pw_status/try.h"
#include "pw_stream/memory_stream.h"
#include "pw_stream/null_stream.h"

namespace pw::rpc2 {

using PwpbMessageDescriptor =
    const span<const protobuf::internal::MessageField>*;

namespace internal {

constexpr bool HasNoCallbacks(PwpbMessageDescriptor table) {
  for (const auto& field : *table) {
    if (field.callback_type() != protobuf::internal::CallbackType::kNone) {
      return false;
    }
    if (field.nested_message_fields() != nullptr &&
        !HasNoCallbacks(field.nested_message_fields())) {
      return false;
    }
  }
  return true;
}

class PwpbEncoder : public protobuf::MemoryEncoder {
 public:
  constexpr PwpbEncoder(span<std::byte> buffer)
      : protobuf::MemoryEncoder(buffer) {}

  StatusWithSize Write(span<const std::byte> message,
                       PwpbMessageDescriptor table) {
    const Status status = protobuf::MemoryEncoder::Write(message, *table);
    return StatusWithSize(status, size());
  }
};

class PwpbStreamEncoder : public protobuf::StreamEncoder {
 public:
  PwpbStreamEncoder(stream::CountingNullStream& counting_stream)
      : protobuf::StreamEncoder(counting_stream) {}

  using protobuf::StreamEncoder::Write;
};

/// Decodes a pwpb message struct from a buffer using its generated field table.
///
/// This is a thin wrapper around `protobuf::StreamDecoder`, which is the
/// same table-driven decoder the rest of Pigweed uses. In particular, it
/// reports `RESOURCE_EXHAUSTED` when a repeated or string field does not fit
/// in the message struct's fixed capacity, rather than silently truncating.
class PwpbDecoder : public protobuf::StreamDecoder {
 public:
  // `StreamDecoder` only stores the reference, so passing `reader_` before it
  // is constructed is safe. This mirrors `rpc::internal::PwpbSerde`.
  constexpr PwpbDecoder(span<const std::byte> buffer)
      : protobuf::StreamDecoder(reader_), reader_(buffer) {}

  Status Read(span<std::byte> message, PwpbMessageDescriptor table) {
    return protobuf::StreamDecoder::Read(message, *table);
  }

 private:
  stream::MemoryReader reader_;
};

}  // namespace internal

/// Low-level serializer bound to a specific PWPB message descriptor table.
template <PwpbMessageDescriptor kTable>
struct PwpbSerde {
  template <typename Message>
  static StatusWithSize EncodedSizeBytes(const Message& value) {
    stream::CountingNullStream output;
    internal::PwpbStreamEncoder encoder(output);
    const Status status = encoder.Write(as_bytes(span(&value, 1)), *kTable);
    // TODO: b/269633514 - Add 16 to the encoded size because pw_protobuf
    // sometimes fails to encode to buffers that exactly fit the output.
    return StatusWithSize(status, output.bytes_written() + 16);
  }

  template <typename Message>
  static StatusWithSize Serialize(const Message& value,
                                  span<std::byte> destination) {
    internal::PwpbEncoder encoder(destination);
    return encoder.Write(as_bytes(span(&value, 1)), kTable);
  }

  template <typename Message>
  static Result<Message> Deserialize(span<const std::byte> source) {
    Message value{};
    internal::PwpbDecoder decoder(source);
    PW_TRY(decoder.Read(as_writable_bytes(span(&value, 1)), kTable));
    return value;
  }
};

/// Primary PwpbSerializer for a PWPB message type.
template <PwpbMessageDescriptor kTable, size_t kMaxSizeBytes>
struct PwpbSerializer {
  template <typename MessageType>
  static constexpr size_t MaxEncodedSize(const MessageType& value) {
    if constexpr (internal::HasNoCallbacks(kTable)) {
      return kMaxSizeBytes;
    } else {
      const StatusWithSize result = PwpbSerde<kTable>::EncodedSizeBytes(value);
      return result.ok() ? result.size() : kMaxSizeBytes;
    }
  }

  template <typename MessageType>
  static StatusWithSize Serialize(const MessageType& value,
                                  span<std::byte> destination) {
    return PwpbSerde<kTable>::Serialize(value, destination);
  }

  template <typename MessageType>
  static Result<MessageType> Deserialize(span<const std::byte> source) {
    return PwpbSerde<kTable>::template Deserialize<MessageType>(source);
  }
};

}  // namespace pw::rpc2
