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

#include "pw_tokenizer/encode_args.h"

#include <algorithm>
#include <cstring>

#include "pw_bytes/span.h"
#include "pw_preprocessor/compiler.h"
#include "pw_varint/varint.h"

static_assert((PW_TOKENIZER_CFG_ARG_TYPES_SIZE_BYTES == 4) ||
                  (PW_TOKENIZER_CFG_ARG_TYPES_SIZE_BYTES == 8),
              "PW_TOKENIZER_CFG_ARG_TYPES_SIZE_BYTES must be 4 or 8");

namespace pw::tokenizer {
namespace {

// Declare the types as an enum for convenience.
enum class ArgType : uint8_t {
  kInt = PW_TOKENIZER_ARG_TYPE_INT,
  kInt64 = PW_TOKENIZER_ARG_TYPE_INT64,
  kDouble = PW_TOKENIZER_ARG_TYPE_DOUBLE,
  kString = PW_TOKENIZER_ARG_TYPE_STRING,
};

size_t EncodeInt(int value, span<std::byte> output) {
  // Use the 64-bit function to avoid instantiating both 32-bit and 64-bit.
  return pw_tokenizer_EncodeInt64(value, output.data(), output.size());
}

size_t EncodeInt64(int64_t value, span<std::byte> output) {
  return pw_tokenizer_EncodeInt64(value, output.data(), output.size());
}

size_t EncodeFloat(float value, span<std::byte> output) {
  if (output.size() < sizeof(value)) {
    return 0;
  }
  std::memcpy(output.data(), &value, sizeof(value));
  return sizeof(value);
}

size_t EncodeString(const char* string, span<std::byte> output) {
  // The top bit of the status byte indicates if the string was truncated.
  static constexpr size_t kMaxStringLength = 0x7Fu;

  if (output.empty()) {  // At least one byte is needed for the status/size.
    return 0;
  }

  if (string == nullptr) {
    string = "NULL";
  }

  // Subtract 1 to save room for the status byte.
  const size_t max_bytes =
      std::min(static_cast<size_t>(output.size()), kMaxStringLength) - 1;

  // Scan the string to find out how many bytes to copy.
  size_t bytes_to_copy = 0;
  std::byte overflow_bit = std::byte{0};

  while (string[bytes_to_copy] != '\0') {
    if (bytes_to_copy == max_bytes) {
      overflow_bit = std::byte{0x80};
      break;
    }
    bytes_to_copy += 1;
  }

  output[0] = static_cast<std::byte>(bytes_to_copy) | overflow_bit;
  std::memcpy(output.data() + 1, string, bytes_to_copy);

  return bytes_to_copy + 1;  // include the status byte in the total
}

}  // namespace

size_t EncodeArgs(pw_tokenizer_ArgTypes types,
                  va_list args,
                  span<std::byte> output) {
  size_t arg_count = types & PW_TOKENIZER_TYPE_COUNT_MASK;
  types >>= PW_TOKENIZER_TYPE_COUNT_SIZE_BITS;

  size_t encoded_bytes = 0;
  while (arg_count != 0u) {
    // How many bytes were encoded; 0 indicates that there wasn't enough space.
    size_t argument_bytes = 0;

    switch (static_cast<ArgType>(types & 0b11u)) {
      case ArgType::kInt:
        argument_bytes = EncodeInt(va_arg(args, int), output);
        break;
      case ArgType::kInt64:
        argument_bytes = EncodeInt64(va_arg(args, int64_t), output);
        break;
      case ArgType::kDouble:
        argument_bytes =
            EncodeFloat(static_cast<float>(va_arg(args, double)), output);
        break;
      case ArgType::kString:
        argument_bytes = EncodeString(va_arg(args, const char*), output);
        break;
    }

    // If zero bytes were encoded, the encoding buffer is full.
    if (argument_bytes == 0u) {
      break;
    }

    output = output.subspan(argument_bytes);
    encoded_bytes += argument_bytes;

    arg_count -= 1;
    types >>= 2;  // each argument type is encoded in two bits
  }

  return encoded_bytes;
}

extern "C" size_t pw_tokenizer_EncodeArgs(pw_tokenizer_ArgTypes types,
                                          va_list args,
                                          void* output_buffer,
                                          size_t output_buffer_size) {
  return EncodeArgs(
      types, args, AsWritableBytes(output_buffer, output_buffer_size));
}

}  // namespace pw::tokenizer
