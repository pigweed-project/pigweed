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

#include "pw_rpc2/internal/pwpb_serialize.h"

#include <cstddef>

#include "pw_protobuf/encoder.h"
#include "pw_protobuf/stream_decoder.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"
#include "pw_stream/memory_stream.h"
#include "pw_stream/null_stream.h"

namespace pw::rpc2::internal {
namespace {

class PwpbStreamEncoder : public protobuf::StreamEncoder {
 public:
  PwpbStreamEncoder(stream::CountingNullStream& counting_stream)
      : protobuf::StreamEncoder(counting_stream) {}

  using protobuf::StreamEncoder::Write;
};

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

/// Decodes a pwpb message struct from a buffer using its generated field table.
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

}  // namespace

StatusWithSize PwpbEncodedSizeBytes(span<const std::byte> message,
                                    PwpbMessageDescriptor table) {
  stream::CountingNullStream output;
  PwpbStreamEncoder encoder(output);
  const Status status = encoder.Write(message, *table);
  return StatusWithSize(status, output.bytes_written());
}

StatusWithSize PwpbEncode(span<const std::byte> message,
                          PwpbMessageDescriptor table,
                          span<std::byte> destination) {
  PwpbEncoder encoder(destination);
  return encoder.Write(message, table);
}

Status PwpbDecode(span<const std::byte> source,
                  PwpbMessageDescriptor table,
                  span<std::byte> message) {
  PwpbDecoder decoder(source);
  return decoder.Read(message, table);
}

}  // namespace pw::rpc2::internal
