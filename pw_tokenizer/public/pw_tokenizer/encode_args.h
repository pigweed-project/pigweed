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
#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "pw_polyfill/standard.h"
#include "pw_preprocessor/util.h"
#include "pw_tokenizer/internal/argument_types.h"
#include "pw_varint/varint.h"

#ifdef __cplusplus

#include <cstring>

#include "pw_polyfill/standard.h"
#include "pw_span/span.h"
#include "pw_tokenizer/config.h"
#include "pw_tokenizer/tokenize.h"

namespace pw::tokenizer {
namespace internal {

// Returns the maximum encoded size of an argument of the specified type.
template <typename T>
constexpr size_t ArgEncodedSizeBytes() {
  constexpr pw_tokenizer_ArgTypes kType = VarargsType<T>();
  if constexpr (kType == PW_TOKENIZER_ARG_TYPE_DOUBLE) {
    return sizeof(float);
  } else if constexpr (kType == PW_TOKENIZER_ARG_TYPE_STRING) {
    return 1;  // Size of the length byte only
  } else if constexpr (kType == PW_TOKENIZER_ARG_TYPE_INT64) {
    return 10;  // Max size of a varint-encoded 64-bit integer
  } else if constexpr (kType == PW_TOKENIZER_ARG_TYPE_INT) {
    return sizeof(T) + 1;  // Max size of zig-zag varint integer <= 32-bits
  } else {
    static_assert(sizeof(T) != sizeof(T), "Unsupported argument type");
  }
}

}  // namespace internal

/// @submodule{pw_tokenizer,tokenize}

/// Calculates the minimum buffer size to allocate that is guaranteed to support
/// encoding the specified arguments.
///
/// The contents of strings are NOT included in this total. The string's
/// length/status byte is guaranteed to fit, but the string contents may be
/// truncated. Encoding is considered to succeed as long as the string's
/// length/status byte is written, even if the actual string is truncated.
///
/// Examples:
///
/// - Message with no arguments:
///       `MinEncodingBufferSizeBytes() == 4`
/// - Message with an int argument
///       `MinEncodingBufferSizeBytes<int>() == 9 (4 + 5)`
template <typename... ArgTypes>
constexpr size_t MinEncodingBufferSizeBytes() {
  return (sizeof(pw_tokenizer_Token) + ... +
          internal::ArgEncodedSizeBytes<ArgTypes>());
}

/// Encodes a tokenized string's arguments to a buffer. The
/// `pw_tokenizer_ArgTypes` parameter specifies the argument types, in
/// place of a format string.
///
/// Most tokenization implementations should use the
/// `pw::tokenizer::EncodedMessage` class.
size_t EncodeArgs(pw_tokenizer_ArgTypes types,
                  va_list args,
                  span<std::byte> output);

/// Encodes a tokenized message to a fixed size buffer. This class is used to
/// encode tokenized messages passed in from tokenization macros.
///
/// To use `pw::tokenizer::EncodedMessage`, construct it with the token,
/// argument types, and `va_list` from the variadic arguments:
///
/// @code{.cpp}
///   void SendLogMessage(span<std::byte> log_data);
///
///   extern "C" void TokenizeToSendLogMessage(pw_tokenizer_Token token,
///                                            pw_tokenizer_ArgTypes types,
///                                            ...) {
///     va_list args;
///     va_start(args, types);
///     EncodedMessage encoded_message(token, types, args);
///     va_end(args);
///
///     SendLogMessage(encoded_message);  // EncodedMessage converts to span
///   }
/// @endcode
template <size_t kMaxSizeBytes>
class EncodedMessage {
 public:
  // Encodes a tokenized message to an internal buffer.
  EncodedMessage(pw_tokenizer_Token token,
                 pw_tokenizer_ArgTypes types,
                 va_list args) {
    auto token_bytes = TokenBytes(token);
    static_assert(token_bytes.size() == sizeof(pw_tokenizer_Token));
    std::memcpy(data_, token_bytes.data(), token_bytes.size());
    size_ =
        sizeof(token) +
        EncodeArgs(types, args, span<std::byte>(data_).subspan(sizeof(token)));
  }

  /// The binary-encoded tokenized message.
  const std::byte* data() const { return data_; }

  /// Returns `data()` as a pointer to `uint8_t` instead of `std::byte`.
  const uint8_t* data_as_uint8() const {
    return reinterpret_cast<const uint8_t*>(data());
  }

  /// The size of the encoded tokenized message in bytes.
  size_t size() const { return size_; }

 private:
  static_assert(kMaxSizeBytes >= sizeof(pw_tokenizer_Token),
                "The encoding buffer must be at least large enough for a token "
                "(4 bytes)");

  std::byte data_[kMaxSizeBytes];
  size_t size_;
};

/// @}

}  // namespace pw::tokenizer

#endif  // __cplusplus

PW_EXTERN_C_START

/// @submodule{pw_tokenizer,tokenize}

/// C function that encodes arguments to a tokenized buffer. Use the
/// `pw::tokenizer::EncodeArgs` function from C++.
size_t pw_tokenizer_EncodeArgs(pw_tokenizer_ArgTypes types,
                               va_list args,
                               void* output_buffer,
                               size_t output_buffer_size);

/// Encodes an `int` with the standard integer encoding: zig-zag + LEB128.
/// This function is only necessary when manually encoding tokenized messages.
static inline size_t pw_tokenizer_EncodeInt(int value,
                                            void* output,
                                            size_t output_size_bytes) {
  return pw_varint_Encode32(
      pw_varint_ZigZagEncode32(value), output, output_size_bytes);
}

/// Encodes an `int64_t` with the standard integer encoding: zig-zag + LEB128.
/// This function is only necessary when manually encoding tokenized messages.
static inline size_t pw_tokenizer_EncodeInt64(int64_t value,
                                              void* output,
                                              size_t output_size_bytes) {
  return pw_varint_Encode64(
      pw_varint_ZigZagEncode64(value), output, output_size_bytes);
}

/// @}

PW_EXTERN_C_END
