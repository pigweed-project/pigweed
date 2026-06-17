// Copyright 2022 The Pigweed Authors
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

#include <array>

#include "pw_protobuf/encoder.h"
#include "pw_protobuf/internal/codegen.h"
#include "pw_protobuf/stream_decoder.h"
#include "pw_rpc/internal/config.h"
#include "pw_span/span.h"
#include "pw_stream/null_stream.h"

namespace pw::rpc {

using PwpbMessageDescriptor =
    const span<const protobuf::internal::MessageField>*;

// Serializer/deserializer for a pw_protobuf message.
class PwpbSerde {
 public:
  explicit constexpr PwpbSerde(PwpbMessageDescriptor table) : table_(table) {}

  PwpbSerde(const PwpbSerde&) = default;
  PwpbSerde& operator=(const PwpbSerde&) = default;

  // Encodes a pw_protobuf struct to the serialized wire format.
  template <typename Message>
  StatusWithSize Encode(const Message& message, ByteSpan buffer) const {
    return Encoder(buffer).Write(as_bytes(span(&message, 1)), table_);
  }

  // Calculates the encoded size of the provided protobuf struct without
  // actually encoding it.
  template <typename Message>
  StatusWithSize EncodedSizeBytes(const Message& message) const {
    stream::CountingNullStream output;
    StreamEncoder encoder(output);
    const Status result = encoder.Write(as_bytes(span(&message, 1)), *table_);

    // TODO: b/269633514 - Add 16 to the encoded size because pw_protobuf
    //     sometimes fails to encode to buffers that exactly fit the output.
    return StatusWithSize(result, output.bytes_written() + 16);
  }

  // Decodes a serialized protobuf into a pw_protobuf message struct.
  template <typename Message>
  Status Decode(ConstByteSpan buffer, Message& message) const {
    return Decoder(buffer).Read(as_writable_bytes(span(&message, 1)), table_);
  }

 private:
  class Encoder : public protobuf::MemoryEncoder {
   public:
    constexpr Encoder(ByteSpan buffer) : protobuf::MemoryEncoder(buffer) {}

    StatusWithSize Write(ConstByteSpan message, PwpbMessageDescriptor table) {
      const Status status = protobuf::MemoryEncoder::Write(message, *table);
      return StatusWithSize(status, size());
    }
  };

  class StreamEncoder : public protobuf::StreamEncoder {
   public:
    constexpr StreamEncoder(stream::Writer& writer, ByteSpan buffer)
        : protobuf::StreamEncoder(writer, buffer) {}

    StreamEncoder(stream::CountingNullStream& counting_stream)
        : protobuf::StreamEncoder(counting_stream) {}

    using protobuf::StreamEncoder::Write;  // Make this method public
  };

  class Decoder : public protobuf::StreamDecoder {
   public:
    constexpr Decoder(ConstByteSpan buffer)
        : protobuf::StreamDecoder(reader_), reader_(buffer) {}

    Status Read(ByteSpan message, PwpbMessageDescriptor table) {
      return protobuf::StreamDecoder::Read(message, *table);
    }

   private:
    stream::MemoryReader reader_;
  };

  PwpbMessageDescriptor table_;
};

// Serializer/deserializer for pw_protobuf request and response message structs
// within an RPC method.
class PwpbMethodSerde {
 public:
  constexpr PwpbMethodSerde(PwpbMessageDescriptor request_table,
                            PwpbMessageDescriptor response_table)
      : request_serde_(request_table), response_serde_(response_table) {}

  PwpbMethodSerde(const PwpbMethodSerde&) = delete;
  PwpbMethodSerde& operator=(const PwpbMethodSerde&) = delete;

  const PwpbSerde& request() const { return request_serde_; }
  const PwpbSerde& response() const { return response_serde_; }

 private:
  PwpbSerde request_serde_;
  PwpbSerde response_serde_;
};

}  // namespace pw::rpc
