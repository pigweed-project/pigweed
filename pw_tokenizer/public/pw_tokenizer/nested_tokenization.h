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

// This file provides common utilities across all token types.
#pragma once

#include <inttypes.h>

#include "pw_preprocessor/arguments.h"
#include "pw_preprocessor/concat.h"
#include "pw_tokenizer/config.h"

/// @submodule{pw_tokenizer,tokenize}

#define PW_TOKENIZER_NESTED_PREFIX PW_TOKENIZER_NESTED_PREFIX_STR[0]

/// Format specifier for a token argument.
#define PW_TOKEN_FMT(...) PW_DELEGATE_BY_ARG_COUNT(_PW_TOKEN_FMT_, __VA_ARGS__)

/// @}

// Case: no argument (no specified domain)
#define _PW_TOKEN_FMT_0() PW_TOKENIZER_NESTED_PREFIX_STR "#%08" PRIx32

// Case: one argument (specified domain)
#define _PW_TOKEN_FMT_1(domain_value) \
  PW_TOKENIZER_NESTED_PREFIX_STR      \
  "{" domain_value                    \
  "}"                                 \
  "#%08" PRIx32

/// @submodule{pw_tokenizer,tokenize}

/// Format specifier for a doubly-nested token argument.
/// Doubly-nested token arguments are useful when the domain and/or token may
/// not be known at the time of logging. For example, if an external function
/// is required to return a domain and token for logging, `PW_NESTED_TOKEN_FMT`
/// still allows for the value to be logged as it tokenizes the domain value
/// as well. It can either take an argument of a domain value, or no argument
/// at all if there is no specified domain.
///
/// PW_NESTED_TOKEN_FMT() expands to ${$#%x}#%08x
/// PW_NESTED_TOKEN_FMT(domain_value) expands to ${${domain_value}#%x}#%08x
///
/// An example of its application could look similar to this:
///
/// @code{.cpp}
/// std::pair<PW_LOG_TOKEN_TYPE, PW_LOG_TOKEN_TYPE> GetDomainAndToken(...) {...}
///
/// const auto [domain, token] = GetDomainAndToken(...);
/// PW_LOG("Nested Token " PW_NESTED_TOKEN_FMT("enum_domain"), domain, token);
/// @endcode
#define PW_NESTED_TOKEN_FMT(...) \
  PW_DELEGATE_BY_ARG_COUNT(_PW_NESTED_TOKEN_FMT_, __VA_ARGS__)

/// @}

// Case: no argument (no specified domain)
#define _PW_NESTED_TOKEN_FMT_0()                                    \
  PW_TOKENIZER_NESTED_PREFIX_STR "{" PW_TOKENIZER_NESTED_PREFIX_STR \
                                 "#%08" PRIx32 "}#%08" PRIx32

// Case: one argument (specified domain)
#define _PW_NESTED_TOKEN_FMT_1(domain_value)                        \
  PW_TOKENIZER_NESTED_PREFIX_STR "{" PW_TOKENIZER_NESTED_PREFIX_STR \
                                 "{" domain_value "}#%08" PRIx32    \
                                 "}#%08" PRIx32

/// @submodule{pw_tokenizer,tokenize}

/// Format specifier for a bit field token argument: one `uint32_t` that holds
/// two tokens in separate bit fields, each from its own domain. The
/// detokenizer replaces the argument with the low field's string immediately
/// followed by the high field's string.
///
/// Each field's token is the argument with the other field's bits cleared. The
/// fields are not shifted, so the low field's token must fit in `low_bits`
/// bits, and the high field's token must be created with its low bits masked
/// off, such as with `PW_TOKENIZE_STRING_MASK`.
///
/// `PW_TOKEN_BIT_FIELD_FMT("low", 5, "high")` expands to `${low/5,high}#%08x`.
///
/// @code{.cpp}
///   // A 5-bit code tokenized in "codes" and a 27-bit message token.
///   const uint32_t message =
///       PW_TOKENIZE_STRING_MASK("messages", 0xFFFFFFE0, ": No such key");
///   PW_LOG_WARN("Failed: " PW_TOKEN_BIT_FIELD_FMT("codes", 5, "messages"),
///               message | (code & 0x1F));
/// @endcode
///
/// @param[in] low_domain String literal with the domain of the token in the
///     low bits, `""` for the default domain, or a token format specifier such
///     as `PW_TOKEN_FMT` when the domain is itself a runtime token argument
///     (passed before the bit field token).
/// @param[in] low_bits Number of bits in the low field. The high field uses
///     the remaining bits. If `low_bits` is 0 or 32, one field has no bits and
///     the detokenizer skips it.
/// @param[in] high_domain String literal with the domain of the token in the
///     high bits, `""` for the default domain, or a token format specifier such
///     as `PW_TOKEN_FMT` when the domain is itself a runtime token argument
///     (passed before the bit field token).
///
/// @pre `low_bits` is a decimal integer literal from 0 to 32 without leading
/// zeros (other than `0`), or a macro that expands to one. It becomes part of
/// the format string, so it cannot be an expression or have a suffix or
/// parentheses.
#define PW_TOKEN_BIT_FIELD_FMT(low_domain, low_bits, high_domain)           \
  PW_TOKENIZER_NESTED_PREFIX_STR                                            \
  "{" low_domain "/" _PW_TOKEN_BIT_FIELD_LOW_BITS(low_bits) "," high_domain \
                                                            "}#%08" PRIx32

/// @}

// Expands to the low_bits argument of PW_TOKEN_BIT_FIELD_FMT as a string
// literal. Only the valid bit counts are defined, so an invalid low_bits
// argument is a compiler error.
#define _PW_TOKEN_BIT_FIELD_LOW_BITS(low_bits) \
  PW_CONCAT(_PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_, low_bits)

#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_0 "0"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_1 "1"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_2 "2"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_3 "3"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_4 "4"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_5 "5"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_6 "6"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_7 "7"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_8 "8"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_9 "9"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_10 "10"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_11 "11"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_12 "12"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_13 "13"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_14 "14"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_15 "15"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_16 "16"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_17 "17"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_18 "18"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_19 "19"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_20 "20"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_21 "21"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_22 "22"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_23 "23"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_24 "24"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_25 "25"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_26 "26"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_27 "27"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_28 "28"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_29 "29"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_30 "30"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_31 "31"
#define _PW_TOKEN_BITS_MUST_USE_PLAIN_INT_LITERAL_32 "32"
