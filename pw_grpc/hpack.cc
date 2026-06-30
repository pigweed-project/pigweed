// Copyright 2024 The Pigweed Authors
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

#include "pw_grpc_private/hpack.h"

#include <array>

#include "pw_assert/check.h"
#include "pw_bytes/byte_builder.h"
#include "pw_log/log.h"
#include "pw_status/status.h"
#include "pw_status/try.h"
#include "pw_string/string_builder.h"
#include "pw_string/util.h"

namespace pw::grpc {

namespace {
#include "hpack.autogen.inc"
}

// RFC 7541 §5.1
Result<uint32_t> HpackIntegerDecode(ConstByteSpan& input,
                                    uint8_t bits_in_first_byte) {
  if (input.empty()) {
    return Status::InvalidArgument();
  }

  const uint8_t n = bits_in_first_byte;
  uint32_t i = static_cast<uint32_t>(input[0]) & ((1 << n) - 1U);
  input = input.subspan(1);

  if (i < ((1 << n) - 1U)) {
    return i;
  }

  uint32_t m = 0;
  while (true) {
    if (input.empty()) {
      return Status::InvalidArgument();
    }
    uint32_t b = static_cast<uint32_t>(input[0]);
    input = input.subspan(1);
    i += (b & 127) << m;
    m += 7;
    if ((b & 128) == 0) {
      return i;
    }
    if (m >= 31) {
      // Shift overflowed.
      return Status::InvalidArgument();
    }
  }
}

// RFC 7541 §5.2
Result<InlineString<kHpackMaxStringSize>> HpackStringDecode(
    ConstByteSpan& input) {
  if (input.empty()) {
    return Status::InvalidArgument();
  }

  int first = static_cast<int>(input[0]);
  bool is_huffman = (first & 0x80) != 0;

  PW_TRY_ASSIGN(size_t length, HpackIntegerDecode(input, 7));
  if (length > input.size()) {
    return Status::InvalidArgument();
  }
  if (length > kHpackMaxStringSize) {
    return Status::OutOfRange();
  }

  auto value = input.subspan(0, length);
  input = input.subspan(length);
  if (is_huffman) {
    return HpackHuffmanDecode(value);
  }
  return InlineString<kHpackMaxStringSize>(
      reinterpret_cast<const char*>(value.data()), value.size());
}

Result<InlineString<kHpackMaxStringSize>> HpackHuffmanDecode(
    ConstByteSpan input) {
  StringBuffer<kHpackMaxStringSize> buffer;
  uint32_t table_index = 0;

  // See definition of kHuffmanDecoderTable in hpack.autogen.h.
  for (std::byte byte : input) {
    for (int k = 7; k >= 0; k--) {
      auto bit = int(byte >> k) & 0x1;
      auto cmd = kHuffmanDecoderTable[table_index][bit];
      if ((cmd & 0b1000'0000) == 0) {
        table_index = cmd;
      } else if (cmd == 0b1111'1110 || cmd == 0b1111'1111) {
        // Error: unprintable character or the decoder entered an invalid state.
        return Status::InvalidArgument();
      } else {
        if (buffer.size() == buffer.max_size()) {
          return Status::OutOfRange();
        }
        buffer.push_back(32 + (cmd & 0b0111'1111));
        table_index = 0;
      }
    }
  }

  return InlineString<kHpackMaxStringSize>(buffer.view());
}

// RFC 7541 §6
Result<InlineString<kHpackMaxStringSize>> HpackParseRequestHeaders(
    ConstByteSpan input) {
  while (!input.empty()) {
    int first = static_cast<int>(input[0]);

    // RFC 7541 §6.1
    if ((first & 0b1000'0000) != 0) {
      PW_TRY_ASSIGN(uint32_t index, HpackIntegerDecode(input, 7));
      // RFC 7541 Appendix A: these are the only static table entries for :path.
      if (index == 4) {
        return "/";
      }
      if (index == 5) {
        return "/index.html";
      }
      continue;
    }

    // RFC 7541 §6.3: dynamic table size update
    if ((first & 0b1110'0000) == 0b0010'0000) {
      // Ignore: we don't use the dynamic table.
      PW_TRY(HpackIntegerDecode(input, 5));
      continue;
    }

    // RFC 7541 §6.2
    uint32_t index;
    if ((first & 0b1100'0000) == 0b0100'0000) {
      PW_TRY_ASSIGN(index, HpackIntegerDecode(input, 6));
    } else {
      PW_CHECK((first & 0b1111'0000) == 0b0000'0000 ||
               (first & 0b1111'0000) == 0b0001'0000);
      PW_TRY_ASSIGN(index, HpackIntegerDecode(input, 4));
    }

    // Check if the name is ":path".
    bool is_path;
    if (index == 0) {
      PW_TRY_ASSIGN(auto name, HpackStringDecode(input));
      is_path = (name == ":path");
    } else {
      // RFC 7541 Appendix A: these are the only static table entries for :path.
      is_path = (index == 4 || index == 5);
    }

    // Always extract the value to advance the `input` span.
    PW_TRY_ASSIGN(auto value, HpackStringDecode(input));
    if (is_path) {
      return value;
    }
  }

  return Status::NotFound();
}

ConstByteSpan ResponseHeadersPayload() {
  return as_bytes(span{kResponseHeaderFields});
}

ConstByteSpan ResponseTrailersPayload(Status response_code) {
  PW_CHECK_UINT_LT(response_code.code(), kResponseTrailerFields.size());
  auto* payload = &kResponseTrailerFields[response_code.code()];
  return as_bytes(span{payload->bytes}.subspan(0, payload->size));
}

}  // namespace pw::grpc
