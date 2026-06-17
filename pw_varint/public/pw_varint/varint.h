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
#pragma once

/// @file pw_varint/varint.h
///
/// The `pw_varint` module provides functions for encoding and decoding variable
/// length integers or varints. For smaller values, varints require less memory
/// than a fixed-size encoding. For example, a 32-bit (4-byte) integer requires
/// 1–5 bytes when varint-encoded.
///
/// `pw_varint` supports custom variable-length encodings with different
/// terminator bit values and positions (@cpp_enum{pw::varint::Format}).
/// The basic encoding for unsigned integers is Little Endian Base 128 (LEB128).
/// ZigZag encoding is also supported, which maps negative integers to positive
/// integers to improve encoding density for LEB128.
///
/// <a
/// href=https://developers.google.com/protocol-buffers/docs/encoding#varints>
/// Protocol Buffers</a> and @rstref{HDLC <module-pw_hdlc>} use variable-length
/// integer encodings for integers.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus

#include <limits>
#include <type_traits>

#include "lib/stdcompat/bit.h"
#include "pw_bytes/span.h"
#include "pw_preprocessor/compiler.h"

extern "C" {
#endif  // __cplusplus

// C++ API.

/// Maximum size of an LEB128-encoded `uint32_t`.
#define PW_VARINT_MAX_INT32_SIZE_BYTES 5

/// Maximum size of an LEB128-encoded `uint64_t`.
#define PW_VARINT_MAX_INT64_SIZE_BYTES 10

/// Describes a custom varint format.
typedef enum {
  PW_VARINT_ZERO_TERMINATED_LEAST_SIGNIFICANT = 0,
  PW_VARINT_ZERO_TERMINATED_MOST_SIGNIFICANT = 1,
  PW_VARINT_ONE_TERMINATED_LEAST_SIGNIFICANT = 2,
  PW_VARINT_ONE_TERMINATED_MOST_SIGNIFICANT = 3,
} pw_varint_Format;

#ifdef __cplusplus
}  // extern "C"
namespace pw::varint {

/// @module{pw_varint}
/// Variable-length integer encoding and decoding library

/// Maximum size of a varint (LEB128) encoded `uint32_t`.
constexpr size_t kMaxVarint32SizeBytes = PW_VARINT_MAX_INT32_SIZE_BYTES;

/// Maximum size of a varint (LEB128) encoded `uint64_t`.
constexpr size_t kMaxVarint64SizeBytes = PW_VARINT_MAX_INT64_SIZE_BYTES;

/// Describes a custom varint format.
enum class Format {
  kZeroTerminatedLeastSignificant = PW_VARINT_ZERO_TERMINATED_LEAST_SIGNIFICANT,
  kZeroTerminatedMostSignificant = PW_VARINT_ZERO_TERMINATED_MOST_SIGNIFICANT,
  kOneTerminatedLeastSignificant = PW_VARINT_ONE_TERMINATED_LEAST_SIGNIFICANT,
  kOneTerminatedMostSignificant = PW_VARINT_ONE_TERMINATED_MOST_SIGNIFICANT,
};

/// @brief Returns the maximum (max) integer value that can be encoded as a
/// varint into the specified number of bytes.
///
/// The following table lists the max value for each byte size:
///
/// | Bytes | Max value                 |
/// | ----- | ------------------------- |
/// | 1     |                       127 |
/// | 2     |                    16,383 |
/// | 3     |                 2,097,151 |
/// | 4     |               268,435,455 |
/// | 5     |            34,359,738,367 |
/// | 6     |         4,398,046,511,103 |
/// | 7     |       562,949,953,421,311 |
/// | 8     |    72,057,594,037,927,935 |
/// | 9     | 9,223,372,036,854,775,807 |
/// | 10    |        (uint64 max value) |
///
/// @param bytes The size of the varint, in bytes. 5 bytes are needed for the
/// max `uint32` value. 10 bytes are needed for the max `uint64` value.
///
/// @return The max integer value for a varint of size `bytes`.
constexpr uint64_t MaxValueInBytes(size_t bytes) {
  return bytes >= kMaxVarint64SizeBytes ? std::numeric_limits<uint64_t>::max()
                                        : (uint64_t(1) << (7 * bytes)) - 1;
}

/// @brief Computes the size of an integer when encoded as a varint.
///
/// @param value The integer whose encoded size is to be computed. `value`
/// can be signed or unsigned.
///
/// @returns The size of `value` when encoded as a varint.
template <typename T,
          typename = std::enable_if_t<std::is_integral<T>::value ||
                                      std::is_convertible<T, uint64_t>::value>>
constexpr size_t EncodedSize(T value) {
  return value == 0
             ? 1u
             : static_cast<size_t>(
                   (64 - cpp20::countl_zero(static_cast<uint64_t>(value)) + 6) /
                   7);
}

/// @name Encode
/// Encodes the provided integer using a variable-length encoding and returns
/// the number of bytes written.
///
/// The encoding is the same as used in protocol buffers. Signed integers are
/// ZigZag encoded to remove leading 1s from small negative numbers, then the
/// resulting number is encoded as Little Endian Base 128 (LEB128). Unsigned
/// integers are encoded directly as LEB128.
///
/// Returns the number of bytes written or 0 if the result didn't fit in the
/// encoding buffer.
/// @{
template <typename T>
constexpr size_t Encode(T value, ByteSpan out_encoded);
size_t Encode(uint64_t value, ByteSpan out_encoded, Format format);
/// @}

/// Encodes a `uint64_t` with Little-Endian Base 128 (LEB128) encoding.
/// @returns the number of bytes written; 0 if the buffer is too small
constexpr size_t EncodeLittleEndianBase128(uint64_t value,
                                           ByteSpan out_encoded);

/// Extracts and encodes 7 bits from the integer. Sets the top bit to indicate
/// more data is coming, which must be cleared if this was the last byte.
template <typename T>
constexpr std::byte EncodeOneByte(T* value);

/// ZigZag encodes a signed integer. This maps small negative numbers to small,
/// unsigned positive numbers, which improves their density for LEB128 encoding.
///
/// ZigZag encoding works by moving the sign bit from the most-significant bit
/// to the least-significant bit. For the signed `k`-bit integer `n`, the
/// formula is:
///
/// @code{.cpp}
///   (n << 1) ^ (n >> (k - 1))
/// @endcode
///
/// See the following for a description of ZigZag encoding:
///   https://protobuf.dev/programming-guides/encoding/#signed-ints
template <typename T>
constexpr std::make_unsigned_t<T> ZigZagEncode(T value);

/// @name Decode
/// Decodes a varint-encoded value. If reading into a signed integer, the value
/// is ZigZag decoded.
///
/// Returns the number of bytes read from the input if successful. Returns zero
/// if the result does not fit in a `int64_t`/ `uint64_t` or if the input is
/// exhausted before the number terminates. Reads a maximum of 10 bytes.
///
/// The following example decodes multiple varints from a buffer:
///
/// @code{.cpp}
///
///   while (!data.empty()) {
///     int64_t value;
///     size_t bytes = Decode(data, &value);
///
///     if (bytes == 0u) {
///       return Status::DataLoss();
///     }
///     results.push_back(value);
///     data = data.subspan(bytes)
///   }
///
/// @endcode
/// @{
template <typename T>
constexpr size_t Decode(ConstByteSpan encoded, T* out_value);
size_t Decode(ConstByteSpan encoded, uint64_t* out_value, Format format);
/// @}

/// Decodes one byte of an LEB128-encoded integer to a `uint32_t`.
/// @returns true if there is more data to decode (top bit is set).
template <typename T>
[[nodiscard]] constexpr bool DecodeOneByte(std::byte encoded,
                                           size_t count,
                                           T* out_value);

/// ZigZag decodes a signed integer.
///
/// The calculation is done modulo `std::numeric_limits<T>::max()+1`, so the
/// unsigned integer overflows are intentional.
template <typename T>
constexpr std::make_signed_t<T> ZigZagDecode(T encoded);

}  // namespace pw::varint
extern "C" {
#endif  // __cplusplus

// C API.

/// @copydoc pw::varint::EncodedSize
size_t pw_varint_EncodedSizeBytes(uint64_t value);

/// @copydoc pw::varint::Encode
size_t pw_varint_Encode32(uint32_t value,
                          void* out_encoded,
                          size_t out_encoded_size);

/// @copydoc pw::varint::Encode
size_t pw_varint_Encode64(uint64_t value,
                          void* out_encoded,
                          size_t out_encoded_size);

/// @copydoc pw::varint::Encode
size_t pw_varint_EncodeCustom(uint64_t value,
                              void* out_encoded,
                              size_t out_encoded_size,
                              pw_varint_Format format);

/// @copydoc pw::varint::EncodeOneByte
uint8_t pw_varint_EncodeOneByte32(uint32_t* value);

/// @copydoc pw::varint::EncodeOneByte
uint8_t pw_varint_EncodeOneByte64(uint64_t* value);

/// @copydoc pw::varint::ZigZagEncode
uint32_t pw_varint_ZigZagEncode32(int32_t value);

/// @copydoc pw::varint::ZigZagEncode
uint64_t pw_varint_ZigZagEncode64(int64_t value);

/// @copydoc pw::varint::Decode
size_t pw_varint_Decode32(const void* encoded,
                          size_t encoded_size,
                          uint32_t* out_value);

/// @copydoc pw::varint::Decode
size_t pw_varint_Decode64(const void* encoded,
                          size_t encoded_size,
                          uint64_t* out_value);

/// @copydoc pw::varint::Decode
size_t pw_varint_DecodeCustom(const void* encoded,
                              size_t encoded_size,
                              uint64_t* out_value,
                              pw_varint_Format format);

/// @copydoc pw::varint::DecodeOneByte
bool pw_varint_DecodeOneByte32(uint8_t encoded,
                               size_t count,
                               uint32_t* out_value);

/// @copydoc pw::varint::DecodeOneByte
bool pw_varint_DecodeOneByte64(uint8_t encoded,
                               size_t count,
                               uint64_t* out_value);

/// @copydoc pw::varint::ZigZagDecode
int32_t pw_varint_ZigZagDecode32(uint32_t encoded);

/// @copydoc pw::varint::ZigZagDecode
int64_t pw_varint_ZigZagDecode64(uint64_t encoded);

/// Macro that returns the encoded size of up to a 64-bit integer. This is
/// inefficient, but is a constant expression if the input is a constant. Use
/// `pw_varint_EncodedSizeBytes` for runtime encoded size calculation.
#define PW_VARINT_ENCODED_SIZE_BYTES(value)        \
  ((unsigned long long)value < (1u << 7)      ? 1u \
   : (unsigned long long)value < (1u << 14)   ? 2u \
   : (unsigned long long)value < (1u << 21)   ? 3u \
   : (unsigned long long)value < (1u << 28)   ? 4u \
   : (unsigned long long)value < (1llu << 35) ? 5u \
   : (unsigned long long)value < (1llu << 42) ? 6u \
   : (unsigned long long)value < (1llu << 49) ? 7u \
   : (unsigned long long)value < (1llu << 56) ? 8u \
   : (unsigned long long)value < (1llu << 63) ? 9u \
                                              : 10u)

#ifdef __cplusplus
}  // extern "C"
namespace pw::varint {

////////////////////////////////////////////////////////////////////////////////
// Template function implementations.

namespace internal {

template <typename U>
constexpr size_t DecodeUnsigned(ConstByteSpan encoded,
                                U* out_uvalue,
                                size_t max_count) {
  max_count = std::min(encoded.size(), max_count);
  size_t count = 0;
  bool keep_going = true;
  while (keep_going) {
    if (count >= max_count) {
      *out_uvalue = U(0);
      return 0;
    }
    keep_going = DecodeOneByte(encoded[count], count, out_uvalue);
    ++count;
  }
  return count;
}

template <typename U>
constexpr size_t EncodeUnsignedFallback(U uvalue, ByteSpan out_encoded) {
  size_t written = 0;
  do {
    if (written >= out_encoded.size()) {
      return 0u;
    }
    out_encoded[written++] = EncodeOneByte(&uvalue);
  } while (uvalue != 0u);

  out_encoded[written - 1] &= static_cast<std::byte>(0x7f);
  return written;
}

template <typename U>
constexpr size_t EncodeUnsigned(U uvalue, ByteSpan out_encoded) {
  if (uvalue < 128) {
    if (out_encoded.empty()) {
      return 0u;
    }
    out_encoded[0] = static_cast<std::byte>(uvalue);
    return 1u;
  }
  return EncodeUnsignedFallback(uvalue, out_encoded);
}

}  // namespace internal

template <typename T>
constexpr size_t Encode(T value, ByteSpan out_encoded) {
  using U = std::conditional_t<sizeof(T) <= sizeof(uint32_t),
                               uint_fast32_t,
                               uint_fast64_t>;
  if constexpr (std::is_signed<T>()) {
    return internal::EncodeUnsigned<U>(ZigZagEncode(value), out_encoded);
  } else {
    return internal::EncodeUnsigned<U>(value, out_encoded);
  }
}

template <typename T>
constexpr std::byte EncodeOneByte(T* value) {
  const auto bits = static_cast<std::byte>((*value & 0x7Fu) | 0x80u);
  *value >>= 7;
  return bits;
}

template <typename T>
constexpr std::make_unsigned_t<T> ZigZagEncode(T value) {
  static_assert(std::is_signed<T>(), "Zig-zag encoding is for signed integers");
  using U = std::make_unsigned_t<T>;
  return static_cast<U>(static_cast<U>(value) << 1) ^
         static_cast<U>(value >> (sizeof(T) * 8 - 1));
}

constexpr size_t EncodeLittleEndianBase128(uint64_t value,
                                           ByteSpan out_encoded) {
  return Encode(value, out_encoded);
}

template <typename T>
constexpr size_t Decode(ConstByteSpan encoded, T* out_value) {
  static_assert(sizeof(T) >= sizeof(uint32_t));
  using U = std::conditional_t<sizeof(T) == sizeof(uint32_t),
                               uint_fast32_t,
                               uint_fast64_t>;
  U uvalue = 0u;
  size_t max_count = sizeof(T) <= sizeof(uint32_t) ? kMaxVarint32SizeBytes
                                                   : kMaxVarint64SizeBytes;
  size_t count = internal::DecodeUnsigned(encoded, &uvalue, max_count);
  if constexpr (std::is_signed_v<T>) {
    *out_value = static_cast<T>(ZigZagDecode(uvalue));
  } else {
    *out_value = static_cast<T>(uvalue);
  }
  return count;
}

template <typename T>
[[nodiscard]] constexpr bool DecodeOneByte(std::byte encoded,
                                           size_t count,
                                           T* out_value) {
  auto u8 = static_cast<uint_fast8_t>(encoded);
  *out_value |= static_cast<T>(u8 & 0x7fu) << (count * 7);
  return (u8 & 0x80u) != 0u;
}

template <typename T>
constexpr std::make_signed_t<T> ZigZagDecode(T encoded)
    PW_NO_SANITIZE("unsigned-integer-overflow") {
  static_assert(std::is_unsigned<T>(),
                "Zig-zag decoding is for unsigned integers");
  return static_cast<std::make_signed_t<T>>((encoded >> 1) ^
                                            (~(encoded & 1) + 1));
}

}  // namespace pw::varint
#endif  // __cplusplus
