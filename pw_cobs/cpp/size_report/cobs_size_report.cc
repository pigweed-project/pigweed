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

#include <algorithm>
#include <array>
#include <cstddef>

#include "pw_bloat/bloat_this_binary.h"
#include "pw_bytes/span.h"
#include "pw_cobs/cobs.h"
#include "pw_log/log.h"
#include "pw_preprocessor/compiler.h"
#include "pw_result/result.h"
#include "pw_span/span.h"

namespace pw::size_report {

PW_NO_INLINE
Result<int> ForceResultInBloat() { return 43; }

std::byte buffer[128];
std::byte* volatile get_buffer = buffer;
volatile unsigned get_size;

PW_NO_INLINE
ByteSpan GetBufferSpan() {
  if (ForceResultInBloat().ok()) {
    PW_LOG_INFO("Got result!");
  }
  get_size = sizeof(buffer);
  std::byte* local_buffer = get_buffer;
  unsigned local_size = get_size;
  return span(local_buffer, local_size);
}

PW_NO_INLINE
void CobsSizeReport() {
  std::array<std::byte, 100> data;
  std::fill(data.begin(), data.end(), std::byte{0});
  ByteSpan span = GetBufferSpan();
  Result<ByteSpan> span_res = span;
  if (span_res.ok()) {
    get_size =
        get_size + static_cast<unsigned>((*span_res).size() + span_res->size());
  }

#ifdef ENABLE_ENCODE_DECODE
  std::array<std::byte, 128> raw_out{};
  auto enc_res = cobs::Encode(data, raw_out);
  if (enc_res.ok()) {
    auto dec_res = cobs::Decode(*enc_res, GetBufferSpan());
    if (dec_res.ok()) {
      get_size = get_size + static_cast<unsigned>(dec_res->size());
    }
  }
#endif  // ENABLE_ENCODE_DECODE

#ifdef ENABLE_IN_PLACE
  cobs::EncodeBuffer<100> encode_buf(data);
  ConstByteSpan encoded = encode_buf.Encode();
  get_size = get_size + static_cast<unsigned>(encoded.size());
#endif  // ENABLE_IN_PLACE

#ifdef ENABLE_STREAMING_DECODER
  cobs::Decoder decoder(data);
  Result<ByteSpan> frame = decoder.Process(std::byte{0x00});
  if (frame.ok()) {
    get_size = get_size + 1;
  }
  decoder.Clear();
#endif  // ENABLE_STREAMING_DECODER
}

}  // namespace pw::size_report

int main() {
  pw::bloat::BloatThisBinary();
  pw::size_report::CobsSizeReport();
  return 0;
}
