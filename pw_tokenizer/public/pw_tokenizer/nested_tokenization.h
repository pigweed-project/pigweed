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
