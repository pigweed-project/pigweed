// Copyright 2021 The Pigweed Authors
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

#include "pw_log/levels.h"
#include "pw_log/options.h"
#include "pw_polyfill/static_assert.h"
#include "pw_tokenizer/config.h"

/// @module{pw_log_tokenized}

// The size of the stack-allocated argument encoding buffer to use by default.
// A buffer of this size is allocated and used for the 4-byte token and for
// encoding all arguments. It must be at least large enough for the token (4
// bytes).
//
// This buffer does not need to be large to accommodate a good number of
// tokenized string arguments. Integer arguments are usually encoded smaller
// than their native size (e.g. 1 or 2 bytes for smaller numbers). All floating
// point types are encoded as four bytes. Null-terminated strings are encoded
// 1:1 in size, however, and can quickly fill up this buffer.
#ifndef PW_LOG_TOKENIZED_ENCODING_BUFFER_SIZE_BYTES
#define PW_LOG_TOKENIZED_ENCODING_BUFFER_SIZE_BYTES \
  PW_TOKENIZER_CFG_ENCODING_BUFFER_SIZE_BYTES
#endif  // PW_LOG_TOKENIZED_ENCODING_BUFFER_SIZE_BYTES

#define PW_LOG_TOKENIZED_FIELD_PREFIX "■"
#define PW_LOG_TOKENIZED_KEY_VALUE_SEPARATOR "♦"

// This macro takes the PW_LOG format string and optionally transforms it. By
// default, pw_log_tokenized specifies three fields as key-value pairs.
#ifndef PW_LOG_TOKENIZED_FORMAT_STRING

#define _PW_LOG_TOKENIZED_FIELD(name, contents)                           \
  PW_LOG_TOKENIZED_FIELD_PREFIX name PW_LOG_TOKENIZED_KEY_VALUE_SEPARATOR \
      contents

/// This macro takes the `pw_log` module name and format string to produce a new
/// string that will be tokenized. Any information can be packed into this
/// string without affecting code size, since tokenization removes it from the
/// binary. By default, `pw_log_tokenized` includes three fields as key-value
/// pair: log module, message, and file path (`__FILE__`).
#define PW_LOG_TOKENIZED_FORMAT_STRING(module, message) \
  _PW_LOG_TOKENIZED_FIELD("msg", message)               \
  _PW_LOG_TOKENIZED_FIELD("module", module)             \
  _PW_LOG_TOKENIZED_FIELD("file", __FILE__)

#endif  // PW_LOG_TOKENIZED_FORMAT_STRING

// The log level, line number, flag bits, and module token are packed into the
// tokenizer's payload argument, which is typically 32 bits. These macros
// specify the number of bits to use for each field. A field with zero bits is
// excluded.

/// Bits to allocate for the log level. Defaults to `PW_LOG_LEVEL_BITS` (3).
#ifndef PW_LOG_TOKENIZED_LEVEL_BITS
#define PW_LOG_TOKENIZED_LEVEL_BITS PW_LOG_LEVEL_BITS
#endif  // PW_LOG_TOKENIZED_LEVEL_BITS

/// Bits to allocate for the line number. Defaults to 11 (up to line 2047). If
/// the line number is too large to be represented by this field, line is
/// reported as 0.
///
/// Including the line number can slightly increase code size. Without the line
/// number, the log metadata argument is the same for all logs with the same
/// level and flags. With the line number, each metadata value is unique and
/// must be encoded as a separate word in the binary. Systems with extreme space
/// constraints may exclude line numbers by setting this macro to 0.
///
/// It is possible to include line numbers in tokenized log format strings, but
/// that is discouraged because line numbers change whenever a file is edited.
/// Passing the line number with the metadata is a lightweight way to include
/// it.
#ifndef PW_LOG_TOKENIZED_LINE_BITS
#define PW_LOG_TOKENIZED_LINE_BITS 11
#endif  // PW_LOG_TOKENIZED_LINE_BITS

/// Bits to use for implementation-defined flags. Defaults to 2.
#ifndef PW_LOG_TOKENIZED_FLAG_BITS
#define PW_LOG_TOKENIZED_FLAG_BITS 2
#endif  // PW_LOG_TOKENIZED_FLAG_BITS

/// Bits to use for the tokenized version of `PW_LOG_MODULE_NAME`. Defaults to
/// 16, which gives a ~1% probability of a collision with 37 module names.
#ifndef PW_LOG_TOKENIZED_MODULE_BITS
#define PW_LOG_TOKENIZED_MODULE_BITS 16
#endif  // PW_LOG_TOKENIZED_MODULE_BITS

static_assert((PW_LOG_TOKENIZED_LEVEL_BITS + PW_LOG_TOKENIZED_LINE_BITS +
               PW_LOG_TOKENIZED_FLAG_BITS + PW_LOG_TOKENIZED_MODULE_BITS) == 32,
              "Log metadata fields must use 32 bits");

// If the level field is present, clamp it to the maximum value.
#if PW_LOG_TOKENIZED_LEVEL_BITS == 0
#define _PW_LOG_TOKENIZED_LEVEL(value) ((uintptr_t)0)
#else
#define _PW_LOG_TOKENIZED_LEVEL(value)                   \
  (value < ((uintptr_t)1 << PW_LOG_TOKENIZED_LEVEL_BITS) \
       ? value                                           \
       : ((uintptr_t)1 << PW_LOG_TOKENIZED_LEVEL_BITS) - 1)
#endif  // PW_LOG_TOKENIZED_LEVEL_BITS

// If the line number field is present, shift it to its position. Set it to zero
// if the line number is too large for PW_LOG_TOKENIZED_LINE_BITS.
#if PW_LOG_TOKENIZED_LINE_BITS == 0
#define _PW_LOG_TOKENIZED_LINE(line) ((uintptr_t)0)
#else
#define _PW_LOG_TOKENIZED_LINE(line)                                \
  ((uintptr_t)(line < (1 << PW_LOG_TOKENIZED_LINE_BITS) ? line : 0) \
   << PW_LOG_TOKENIZED_LEVEL_BITS)
#endif  // PW_LOG_TOKENIZED_LINE_BITS

// If the flags field is present, mask it and shift it to its position.
#if PW_LOG_TOKENIZED_FLAG_BITS == 0
#define _PW_LOG_TOKENIZED_FLAGS(value) ((uintptr_t)0)
#else
#define _PW_LOG_TOKENIZED_FLAGS(value)                                       \
  (((uintptr_t)(value) & (((uintptr_t)1 << PW_LOG_TOKENIZED_FLAG_BITS) - 1)) \
   << (PW_LOG_TOKENIZED_LEVEL_BITS + PW_LOG_TOKENIZED_LINE_BITS))
#endif  // PW_LOG_TOKENIZED_FLAG_BITS

// If the module field is present, shift it to its position.
#if PW_LOG_TOKENIZED_MODULE_BITS == 0
#define _PW_LOG_TOKENIZED_MODULE(value) ((uintptr_t)0)
#else
#define _PW_LOG_TOKENIZED_MODULE(value)                  \
  ((uintptr_t)(value) << ((PW_LOG_TOKENIZED_LEVEL_BITS + \
                           PW_LOG_TOKENIZED_LINE_BITS +  \
                           PW_LOG_TOKENIZED_FLAG_BITS)))
#endif  // PW_LOG_TOKENIZED_MODULE_BITS

#ifdef __cplusplus

#include <cstddef>

namespace pw::log_tokenized {

// C++ constant for the encoding buffer size. Use this instead of the macro.
inline constexpr size_t kEncodingBufferSizeBytes =
    PW_LOG_TOKENIZED_ENCODING_BUFFER_SIZE_BYTES;

}  // namespace pw::log_tokenized

#endif  // __cplusplus

/// @endmodule
