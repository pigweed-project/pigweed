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
#pragma once

#include <stdint.h>

#include "pw_assert_tokenized/handler.h"
#include "pw_log_tokenized/config.h"
#include "pw_polyfill/static_assert.h"
#include "pw_preprocessor/arguments.h"
#include "pw_tokenizer/tokenize.h"

// In C and C++23 (where static variables are permitted inside unreached
// branches of constexpr functions per P2242R3), tokenize the assert expression
// and __FILE__ directly into the .pw_tokenizer.entries ELF section and forward
// the token to pw_assert_tokenized_HandleCheckFailure().
#define _PW_ASSERT_TOKENIZED_FORMAT_STRING(expression) \
  PW_LOG_TOKENIZED_FORMAT_STRING("PW_ASSERT", "Assert failure: " expression)

#ifdef __cplusplus
#define _PW_ASSERT_TOKENIZED_CHECK_LANGUAGE()                             \
  static_assert(                                                          \
      __cpp_constexpr >= 202110L,                                         \
      "pw_assert_tokenized:assert_with_condition_backend requires C++23 " \
      "or newer (__cpp_constexpr >= 202110L)")
#else
#define _PW_ASSERT_TOKENIZED_CHECK_LANGUAGE() \
  do {                                        \
  } while (0)
#endif  // __cplusplus

#define PW_ASSERT_HANDLE_FAILURE(expression)                                \
  do {                                                                      \
    _PW_ASSERT_TOKENIZED_CHECK_LANGUAGE();                                  \
    static_assert(!PW_EMPTY_ARGS(static),                                   \
                  "pw_assert_tokenized:assert_with_condition_backend "      \
                  "cannot be used when `static` is defined to nothing");    \
    _PW_TOKENIZER_CONST uint32_t _pw_assert_token =                         \
        PW_TOKENIZE_STRING(_PW_ASSERT_TOKENIZED_FORMAT_STRING(expression)); \
    pw_assert_tokenized_HandleCheckFailure(_pw_assert_token, __LINE__);     \
  } while (0)
