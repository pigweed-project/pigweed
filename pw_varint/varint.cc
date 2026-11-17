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

#include "pw_varint/varint.h"

#include <algorithm>
#include <cstddef>

namespace pw::varint {
namespace {

constexpr bool ZeroTerminated(Format format) {
  return (static_cast<unsigned>(format) & 0b10) == 0;
}

constexpr bool LeastSignificant(Format format) {
  return (static_cast<unsigned>(format) & 0b01) == 0;
}

}  // namespace

size_t Encode(uint64_t value, ByteSpan out_encoded, Format format) {
  size_t written = 0;
  int value_shift = LeastSignificant(format) ? 1 : 0;
  int term_shift = value_shift == 1 ? 0 : 7;

  std::byte cont, term;
  if (ZeroTerminated(format)) {
    cont = std::byte(0x01) << term_shift;
    term = std::byte(0x00) << term_shift;
  } else {
    cont = std::byte(0x00) << term_shift;
    term = std::byte(0x01) << term_shift;
  }

  do {
    if (written >= out_encoded.size()) {
      return 0;
    }

    bool last_byte = (value >> 7) == 0u;

    // Grab 7 bits and set the eighth according to the continuation bit.
    std::byte byte = (static_cast<std::byte>(value) & std::byte(0x7f))
                     << value_shift;

    if (last_byte) {
      byte |= term;
    } else {
      byte |= cont;
    }

    out_encoded[written++] = byte;
    value >>= 7;
  } while (value != 0u);

  return written;
}

size_t Decode(ConstByteSpan encoded, uint64_t* out_value, Format format) {
  uint64_t decoded_value = 0;
  uint_fast8_t count = 0;

  // The largest 64-bit ints require 10 B.
  const size_t max_count = std::min(kMaxVarint64SizeBytes, encoded.size());

  std::byte mask;
  uint32_t shift;
  if (LeastSignificant(format)) {
    mask = std::byte(0xfe);
    shift = 1;
  } else {
    mask = std::byte(0x7f);
    shift = 0;
  }

  // Determines whether a byte is the last byte of a varint.
  auto is_last_byte = [&](std::byte byte) {
    if (ZeroTerminated(format)) {
      return (byte & ~mask) == std::byte(0);
    }
    return (byte & ~mask) != std::byte(0);
  };

  while (true) {
    if (count >= max_count) {
      return 0;
    }

    // Add the bottom seven bits of the next byte to the result.
    decoded_value |= static_cast<uint64_t>((encoded[count] & mask) >> shift)
                     << (7 * count);

    // Stop decoding if the end is reached.
    if (is_last_byte(encoded[count++])) {
      break;
    }
  }

  *out_value = decoded_value;
  return count;
}

}  // namespace pw::varint

extern "C" {

size_t pw_varint_EncodedSizeBytes(uint64_t value) {
  return pw::varint::EncodedSize(value);
}

size_t pw_varint_Encode32(uint32_t value,
                          void* out_encoded,
                          size_t out_encoded_size) {
  return pw::varint::Encode(value,
                            pw::AsWritableBytes(out_encoded, out_encoded_size));
}

size_t pw_varint_Encode64(uint64_t value,
                          void* out_encoded,
                          size_t out_encoded_size) {
  return pw::varint::Encode(value,
                            pw::AsWritableBytes(out_encoded, out_encoded_size));
}

size_t pw_varint_EncodeCustom(uint64_t value,
                              void* out_encoded,
                              size_t out_encoded_size,
                              pw_varint_Format format) {
  return pw::varint::Encode(value,
                            pw::AsWritableBytes(out_encoded, out_encoded_size),
                            static_cast<pw::varint::Format>(format));
}

uint8_t pw_varint_EncodeOneByte32(uint32_t* value) {
  return static_cast<uint8_t>(pw::varint::EncodeOneByte(value));
}

uint8_t pw_varint_EncodeOneByte64(uint64_t* value) {
  return static_cast<uint8_t>(pw::varint::EncodeOneByte(value));
}

uint32_t pw_varint_ZigZagEncode32(int32_t value) {
  return pw::varint::ZigZagEncode(value);
}

uint64_t pw_varint_ZigZagEncode64(int64_t value) {
  return pw::varint::ZigZagEncode(value);
}

size_t pw_varint_Decode32(const void* encoded,
                          size_t encoded_size,
                          uint32_t* out_value) {
  return pw::varint::Decode(pw::AsBytes(encoded, encoded_size), out_value);
}

size_t pw_varint_Decode64(const void* encoded,
                          size_t encoded_size,
                          uint64_t* out_value) {
  return pw::varint::Decode(pw::AsBytes(encoded, encoded_size), out_value);
}

size_t pw_varint_DecodeCustom(const void* encoded,
                              size_t encoded_size,
                              uint64_t* out_value,
                              pw_varint_Format format) {
  return pw::varint::Decode(pw::AsBytes(encoded, encoded_size),
                            out_value,
                            static_cast<pw::varint::Format>(format));
}

bool pw_varint_DecodeOneByte32(uint8_t encoded,
                               size_t count,
                               uint32_t* out_value) {
  return pw::varint::DecodeOneByte(
      static_cast<std::byte>(encoded), count, out_value);
}

bool pw_varint_DecodeOneByte64(uint8_t encoded,
                               size_t count,
                               uint64_t* out_value) {
  return pw::varint::DecodeOneByte(
      static_cast<std::byte>(encoded), count, out_value);
}

int32_t pw_varint_ZigZagDecode32(uint32_t encoded) {
  return pw::varint::ZigZagDecode(encoded);
}

int64_t pw_varint_ZigZagDecode64(uint64_t encoded) {
  return pw::varint::ZigZagDecode(encoded);
}

}  // extern "C"
