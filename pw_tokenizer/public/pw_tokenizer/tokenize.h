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

#ifdef __cplusplus

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "pw_bytes/endian.h"

#else

#include <stddef.h>
#include <stdint.h>

#endif  // __cplusplus

#include "pw_polyfill/static_assert.h"
#include "pw_preprocessor/arguments.h"
#include "pw_preprocessor/compiler.h"
#include "pw_preprocessor/concat.h"
#include "pw_preprocessor/util.h"
#include "pw_tokenizer/internal/argument_types.h"
#include "pw_tokenizer/internal/tokenize_string.h"

/// @submodule{pw_tokenizer,tokenize}

/// The type of the 32-bit token used in place of a string. Also available as
/// `pw::tokenizer::Token`.
typedef uint32_t pw_tokenizer_Token;

/// Strings may optionally be tokenized to a domain. Strings in different
/// domains can be processed separately by the token database tools. Each domain
/// in use must have a corresponding section declared in the linker script. See
/// `pw_tokenizer_linker_sections.ld` for more details.
///
/// The default domain is an empty string.
#define PW_TOKENIZER_DEFAULT_DOMAIN ""

/// Converts a string literal to a `pw_tokenizer_Token` (`uint32_t`) token in a
/// standalone statement. C and C++ compatible. In C++, the string may be a
/// literal or a constexpr char array, including function variables like
/// `__func__`. In C, the argument must be a string literal. In either case, the
/// string must be null terminated, but may contain any characters (including
/// '\0').
///
/// @code{cpp}
///
///   // Tokenizes a string literal in the given domain.
///   constexpr uint32_t token = PW_TOKENIZE_STRING("domain", "string literal");
///
///   // Tokenizes a string literal in the default domain ("").
///   constexpr uint32_t other = PW_TOKENIZE_STRING("string literal");
///
/// @endcode
#define PW_TOKENIZE_STRING(...) \
  PW_DELEGATE_BY_ARG_COUNT(_PW_TOKENIZE_STRING_, __VA_ARGS__)

/// @}

#define _PW_TOKENIZE_STRING_1(string_literal) \
  PW_TOKENIZE_STRING_DOMAIN(PW_TOKENIZER_DEFAULT_DOMAIN, string_literal)

#define _PW_TOKENIZE_STRING_2(domain, string_literal) \
  PW_TOKENIZE_STRING_DOMAIN(domain, string_literal)

/// @submodule{pw_tokenizer,tokenize}

/// Converts a string literal to a ``uint32_t`` token within an expression.
/// Requires C++.
///
/// @code{cpp}
///
///   // Tokenizes a string literal in the given domain within an expression.
///   DoSomethingWithToken(PW_TOKENIZE_STRING_EXPR("domain", "Succeed"));
///
///   // Tokenizes a string literal in the default domain within an expression.
///   DoSomethingWithToken(PW_TOKENIZE_STRING_EXPR("Succeed"));
///
/// @endcode
#define PW_TOKENIZE_STRING_EXPR(...) \
  PW_DELEGATE_BY_ARG_COUNT(_PW_TOKENIZE_STRING_EXPR_, __VA_ARGS__)

/// @}

#define _PW_TOKENIZE_STRING_EXPR_1(string_literal) \
  _PW_TOKENIZE_STRING_EXPR_2(PW_TOKENIZER_DEFAULT_DOMAIN, string_literal)

#define _PW_TOKENIZE_STRING_EXPR_2(domain, string_literal) \
  [&] {                                                    \
    constexpr uint32_t lambda_ret_token =                  \
        PW_TOKENIZE_STRING_DOMAIN(domain, string_literal); \
    return lambda_ret_token;                               \
  }()

/// @submodule{pw_tokenizer,tokenize}

// clang-format off
/// Tokenizes a string literal in a standalone statement using the specified
/// [domain](../../pw_tokenizer/tokenization.html#keep-tokens-from-different-sources-separate-with-domains).
/// C and C++ compatible.
// clang-format on
#define PW_TOKENIZE_STRING_DOMAIN(domain, string_literal) \
  PW_TOKENIZE_STRING_MASK(domain, UINT32_MAX, string_literal)

// clang-format off
/// Tokenizes a string literal using the specified
/// [domain](../../pw_tokenizer/tokenization.html#keep-tokens-from-different-sources-separate-with-domains)
/// within an expression. Requires C++.
// clang-format on
#define PW_TOKENIZE_STRING_DOMAIN_EXPR(domain, string_literal) \
  [&] {                                                        \
    constexpr uint32_t lambda_ret_token =                      \
        PW_TOKENIZE_STRING_DOMAIN(domain, string_literal);     \
    return lambda_ret_token;                                   \
  }()

// clang-format off
/// Tokenizes a string literal in a standalone statement using the specified
/// [domain](../../pw_tokenizer/tokenization.html#keep-tokens-from-different-sources-separate-with-domains)
/// and [bit mask](../../pw_tokenizer/tokenization.html#module-pw-tokenizer-masks).
/// C and C++ compatible.
// clang-format on
#define PW_TOKENIZE_STRING_MASK(domain, mask, string_literal)                \
  /* assign to a variable */ _PW_TOKENIZER_MASK_TOKEN(mask, string_literal); \
                                                                             \
  static_assert(0 < (mask) && (mask) <= UINT32_MAX,                          \
                "Tokenizer masks must be non-zero uint32_t values.");        \
                                                                             \
  PW_TOKENIZER_DEFINE_TOKEN(                                                 \
      _PW_TOKENIZER_MASK_TOKEN(mask, string_literal), domain, string_literal)

// clang-format off
/// Tokenizes a string literal using the specified
/// [domain](../../pw_tokenizer/tokenization.html#keep-tokens-from-different-sources-separate-with-domains)
/// and [bit mask](../../pw_tokenizer/tokenization.html#module-pw-tokenizer-masks)
/// within an expression. Requires C++.
// clang-format on
#define PW_TOKENIZE_STRING_MASK_EXPR(domain, mask, string_literal) \
  [&] {                                                            \
    constexpr uint32_t lambda_ret_token =                          \
        PW_TOKENIZE_STRING_MASK(domain, mask, string_literal);     \
    return lambda_ret_token;                                       \
  }()

/// @}

#define _PW_TOKENIZER_MASK_TOKEN(mask, string_literal) \
  ((pw_tokenizer_Token)(mask) & PW_TOKENIZER_STRING_TOKEN(string_literal))

/// @submodule{pw_tokenizer,tokenize}

/// Encodes a tokenized string and arguments to the provided buffer. The size of
/// the buffer is passed via a pointer to a `size_t`. After encoding is
/// complete, the `size_t` is set to the number of bytes written to the buffer.
///
/// The macro's arguments are equivalent to the following function signature:
///
/// @code{.cpp}
///
///   TokenizeToBuffer(void* buffer,
///                    size_t* buffer_size_pointer,
///                    const char* format,
///                    ...);  // printf-style arguments
/// @endcode
///
/// For example, the following encodes a tokenized string with a temperature to
/// a buffer. The buffer is passed to a function to send the message over a
/// UART.
///
/// @code
///
///   uint8_t buffer[32];
///   size_t size_bytes = sizeof(buffer);
///   PW_TOKENIZE_TO_BUFFER(
///       buffer, &size_bytes, "Temperature (C): %0.2f", temperature_c);
///   MyProject_EnqueueMessageForUart(buffer, size);
///
/// @endcode
///
/// While `PW_TOKENIZE_TO_BUFFER` is very flexible, it must be passed a buffer,
/// which increases its code size footprint at the call site.
#define PW_TOKENIZE_TO_BUFFER(buffer, buffer_size_pointer, format, ...) \
  PW_TOKENIZE_TO_BUFFER_DOMAIN(PW_TOKENIZER_DEFAULT_DOMAIN,             \
                               buffer,                                  \
                               buffer_size_pointer,                     \
                               format,                                  \
                               __VA_ARGS__)

// clang-format off
/// Same as `PW_TOKENIZE_TO_BUFFER`, but tokenizes to the specified
/// [domain](../../pw_tokenizer/tokenization.html#keep-tokens-from-different-sources-separate-with-domains).
// clang-format on
#define PW_TOKENIZE_TO_BUFFER_DOMAIN(                 \
    domain, buffer, buffer_size_pointer, format, ...) \
  PW_TOKENIZE_TO_BUFFER_MASK(                         \
      domain, UINT32_MAX, buffer, buffer_size_pointer, format, __VA_ARGS__)

/// Same as `PW_TOKENIZE_TO_BUFFER_DOMAIN`, but applies a
/// [bit mask](../../pw_tokenizer/tokenization.html#module-pw-tokenizer-masks)
/// to the token.
#define PW_TOKENIZE_TO_BUFFER_MASK(                                          \
    domain, mask, buffer, buffer_size_pointer, format, ...)                  \
  do {                                                                       \
    PW_TOKENIZE_FORMAT_STRING(domain, mask, format, __VA_ARGS__);            \
    _pw_tokenizer_ToBuffer(buffer,                                           \
                           buffer_size_pointer,                              \
                           PW_TOKENIZER_REPLACE_FORMAT_STRING(__VA_ARGS__)); \
  } while (0)

/// @brief Low-level macro for calling functions that handle tokenized strings.
///
/// Functions that work with tokenized format strings must take the following
/// arguments:
///
/// - The 32-bit token (`pw_tokenizer_Token`)
/// - The 32- or 64-bit argument types (`pw_tokenizer_ArgTypes`)
/// - Variadic arguments, if any
///
/// This macro expands to those arguments. Custom tokenization macros should use
/// this macro to pass these arguments to a function or other macro.
///
/** @code{cpp}
 *    EncodeMyTokenizedString(uint32_t token,
 *                            pw_tokenier_ArgTypes arg_types,
 *                            ...);
 *
 *    #define CUSTOM_TOKENIZATION_MACRO(format, ...)                  \
 *      PW_TOKENIZE_FORMAT_STRING(domain, mask, format, __VA_ARGS__); \
 *      EncodeMyTokenizedString(PW_TOKENIZER_REPLACE_FORMAT_STRING(__VA_ARGS__))
 *  @endcode
 */
#define PW_TOKENIZER_REPLACE_FORMAT_STRING(...) \
  _PW_TOKENIZER_REPLACE_FORMAT_STRING(PW_EMPTY_ARGS(__VA_ARGS__), __VA_ARGS__)

/// @}

#define _PW_TOKENIZER_REPLACE_FORMAT_STRING(empty_args, ...) \
  _PW_CONCAT_2(_PW_TOKENIZER_REPLACE_FORMAT_STRING_, empty_args)(__VA_ARGS__)

#define _PW_TOKENIZER_REPLACE_FORMAT_STRING_1() _pw_tokenizer_token, 0u
#define _PW_TOKENIZER_REPLACE_FORMAT_STRING_0(...) \
  _pw_tokenizer_token, PW_TOKENIZER_ARG_TYPES(__VA_ARGS__), __VA_ARGS__

/// @submodule{pw_tokenizer,tokenize}

/// Converts a series of arguments to a compact format that replaces the format
/// string literal. Evaluates to a `pw_tokenizer_ArgTypes` value.
///
/// Depending on the size of `pw_tokenizer_ArgTypes`, the bottom 4 or 6 bits
/// store the number of arguments and the remaining bits store the types, two
/// bits per type. The arguments are not evaluated; only their types are used.
///
/// In general, `PW_TOKENIZER_ARG_TYPES` should not be used directly.
/// Instead, use `PW_TOKENIZER_REPLACE_FORMAT_STRING`.
#define PW_TOKENIZER_ARG_TYPES(...) \
  PW_DELEGATE_BY_ARG_COUNT(_PW_TOKENIZER_TYPES_, __VA_ARGS__)

/// @}

PW_EXTERN_C_START

// These functions encode the tokenized strings. These should not be called
// directly. Instead, use the corresponding PW_TOKENIZE_TO_* macros above.
void _pw_tokenizer_ToBuffer(void* buffer,
                            size_t* buffer_size_bytes,  // input and output arg
                            pw_tokenizer_Token token,
                            pw_tokenizer_ArgTypes types,
                            ...);

// This empty function allows the compiler to check the format string.
static inline void pw_tokenizer_CheckFormatString(const char* format, ...)
    PW_PRINTF_FORMAT(1, 2);

static inline void pw_tokenizer_CheckFormatString(const char* format, ...) {
  (void)format;
}

PW_EXTERN_C_END

/// @submodule{pw_tokenizer,tokenize}

// clang-format off
/// Tokenizes a format string with optional arguments and sets the
/// `_pw_tokenizer_token` variable to the token. Must be used in its own scope,
/// since the same variable is used in every invocation.
///
/// The tokenized string uses the specified
/// [domain](../../pw_tokenizer/tokenization.html#keep-tokens-from-different-sources-separate-with-domains).
/// Use `PW_TOKENIZER_DEFAULT_DOMAIN` for the default. The token also may be
/// masked; use `UINT32_MAX` to keep all bits.
///
/// This macro checks that the printf-style format string matches the arguments
/// and that no more than `PW_TOKENIZER_MAX_SUPPORTED_ARGS` are
/// provided. It then stores the format string in a special section, and
/// calculates the string's token at compile time.
// clang-format on
#define PW_TOKENIZE_FORMAT_STRING(domain, mask, format, ...)                     \
  static_assert(                                                               \
      PW_FUNCTION_ARG_COUNT(__VA_ARGS__) <= PW_TOKENIZER_MAX_SUPPORTED_ARGS,   \
      "Tokenized strings cannot have more than "                               \
      PW_STRINGIFY(PW_TOKENIZER_MAX_SUPPORTED_ARGS) " arguments; "             \
      PW_STRINGIFY(PW_FUNCTION_ARG_COUNT(__VA_ARGS__))                         \
      " arguments were used for " #format " (" #__VA_ARGS__ ")"); \
  PW_TOKENIZE_FORMAT_STRING_ANY_ARG_COUNT(domain, mask, format, __VA_ARGS__)

/// Equivalent to `PW_TOKENIZE_FORMAT_STRING`, but supports any number of
/// arguments.
///
/// This is a low-level macro that should rarely be used directly. It is
/// intended for situations when `pw_tokenizer_ArgTypes` is not used.
/// There are two situations where `pw_tokenizer_ArgTypes` is
/// unnecessary:
///
/// - The exact format string argument types and count are fixed.
/// - The format string supports a variable number of arguments of only one
///   type. In this case, `PW_FUNCTION_ARG_COUNT` may be used to pass
///   the argument count to the function.
///
/// NOTE: The `%.*s` format specifier cannot be used. See
/// https://pwbug.dev/408040194 for details.
#define PW_TOKENIZE_FORMAT_STRING_ANY_ARG_COUNT(domain, mask, format, ...)     \
  if (0) { /* Do not execute to prevent double evaluation of the arguments. */ \
    pw_tokenizer_CheckFormatString(format PW_COMMA_ARGS(__VA_ARGS__));         \
  }                                                                            \
                                                                               \
  _PW_TOKENIZE_VALIDATE_FORMAT_STRING(format);                                 \
                                                                               \
  /* Tokenize the string to a pw_tokenizer_Token at compile time. */           \
  static _PW_TOKENIZER_CONST pw_tokenizer_Token _pw_tokenizer_token =          \
      _PW_TOKENIZER_MASK_TOKEN(mask, format);                                  \
                                                                               \
  PW_TOKENIZER_DEFINE_TOKEN(_pw_tokenizer_token, domain, format)

/// @}

// Creates unique names to use for tokenized string entries and linker sections.
#define _PW_TOKENIZER_UNIQUE(prefix) PW_CONCAT(prefix, __LINE__, _, __COUNTER__)

#ifdef __cplusplus

#define _PW_TOKENIZER_CONST constexpr

/// @submodule{pw_tokenizer,tokenize}

/// Records the original token, domain and string directly.
///
/// This macro is intended to be used for tokenized enum and domain support. The
/// values are stored as an entry in the ELF section. As a note for tokenized
/// enum support, the enum name should be used as the string, and the enum value
/// as the token.
#define PW_TOKENIZER_DEFINE_TOKEN(token, domain, string)                     \
  static_assert(::pw::tokenizer::internal::ValidDomain(domain),              \
                "pw_tokenizer domains may only contain alphanumeric "        \
                "characters, underscore, or colon, and cannot start with a " \
                "number; space characters are ignored");                     \
  alignas(1) static constexpr auto _PW_TOKENIZER_SECTION PW_NO_SANITIZE(     \
      "address") _PW_TOKENIZER_UNIQUE(_pw_tokenizer_string_entry_) =         \
      ::pw::tokenizer::internal::MakeEntry(token, domain, string)

/// @}

// Validates the format string provided to PW_TOKENIZE_FORMAT_STRING and
// friends.
#define _PW_TOKENIZE_VALIDATE_FORMAT_STRING(format)                     \
  do {                                                                  \
    static_assert(!::pw::tokenizer::internal::Contains(format, "%.*s"), \
                  "The %.*s specifier is not supported."                \
                  " See https://pwbug.dev/408040194");                  \
  } while (0)

namespace pw::tokenizer {

using Token = ::pw_tokenizer_Token;
inline constexpr const char* kDefaultDomain = PW_TOKENIZER_DEFAULT_DOMAIN;

/// Converts a token to an array of bytes suitable for copying into a
/// TOKENIZATION_OPTIONAL proto field.
constexpr std::array<std::byte, 4> TokenBytes(Token token) {
  // Tokens in 'bytes' fields are always encoded in little-endian order.
  return bytes::CopyInOrder(endian::little, token);
}

namespace internal {

constexpr bool Contains(const char* haystack, const char* needle) {
  std::string_view haystack_view(haystack);
  return haystack_view.find(needle) != std::string_view::npos;
}

}  // namespace internal
}  // namespace pw::tokenizer

#else

#define _PW_TOKENIZER_CONST const
#define _PW_ALIGNAS(alignment) __attribute__((aligned(alignment)))

#define PW_TOKENIZER_DEFINE_TOKEN(token, domain, string) \
  _PW_ALIGNAS(1)                                         \
  static const PW_NO_SANITIZE("address")                 \
      _PW_TOKENIZER_STRING_ENTRY(token, domain, string)

// There is no way to do this in C.
#define _PW_TOKENIZE_VALIDATE_FORMAT_STRING(format) \
  do {                                              \
  } while (0)

#endif  // __cplusplus

// _PW_TOKENIZER_SECTION places the tokenized strings in a special .pw_tokenizer
// linker section. Host-side decoding tools read the strings and tokens from
// this section to build a database of tokenized strings.
//
// This section should be declared as type INFO so that it is excluded from the
// final binary. To declare the section, as well as the .pw_tokenizer.info
// metadata section, add the following to the linker script's SECTIONS command:
//
//   .pw_tokenizer.info 0x0 (INFO) :
//   {
//     KEEP(*(.pw_tokenizer.info))
//   }
//
//   .pw_tokenizer.entries 0x0 (INFO) :
//   {
//     KEEP(*(.pw_tokenizer.entries.*))
//   }
//
// A linker script snippet that provides these sections is provided in the file
// pw_tokenizer_linker_sections.ld. This file may be directly included into
// existing linker scripts.
//
// The tokenized string sections can also be managed without linker script
// modifications, though this is not recommended. The section can be extracted
// and removed from the ELF with objcopy:
//
//   objcopy --only-section .pw_tokenizer.* <ORIGINAL_ELF> <OUTPUT_ELF>
//   objcopy --remove-section .pw_tokenizer.* <ORIGINAL_ELF>
//
// OUTPUT_ELF will be an ELF with only the tokenized strings, and the original
// ELF file will have the sections removed.
//
// Without the above linker script modifications, the section garbage collection
// option (--gc-sections) removes the tokenized string sections. To avoid
// editing the target linker script, a separate metadata ELF can be linked
// without --gc-sections to preserve the tokenized data.
//
// pw_tokenizer is intended for use with ELF files only. Mach-O files (macOS
// executables) have limited support for linker sections and are not supported
// by the pw_tokenizer tooling, so tokens are not stored when building a Mach-O.
#ifdef __APPLE__
#define _PW_TOKENIZER_SECTION PW_USED
#else
#define _PW_TOKENIZER_SECTION \
  PW_KEEP_IN_SECTION(PW_STRINGIFY(_PW_TOKENIZER_UNIQUE(.pw_tokenizer.entries.)))
#endif  // __APPLE__
