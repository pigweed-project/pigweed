// Copyright 2022 The Pigweed Authors
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

#include <stdint.h>  // IWYU pragma: export

#include "pw_assert_tokenized/handler.h"  // IWYU pragma: export
#include "pw_preprocessor/arguments.h"    // IWYU pragma: export
#include "pw_preprocessor/concat.h"       // IWYU pragma: export
#include "pw_tokenizer/tokenize.h"        // IWYU pragma: export

// In C and C++23 (where static constexpr variables are permitted inside
// constexpr functions per P2647R1), emit the tokenized __FILE__ entry directly
// into the .pw_tokenizer.entries ELF section so no offline file list generation
// is needed. Fall back to PW_TOKENIZER_STRING_TOKEN(__FILE__) in pre-C++23 C++
// or when `static` is macro-defined to nothing (e.g. via
// PW_THIRD_PARTY_FREERTOS_NO_STATICS or FreeRTOS's portREMOVE_STATIC_QUALIFIER,
// which defines `static` after including headers).
#if !defined(__cplusplus) || __cpp_constexpr >= 202211L
#define _PW_ASSERT_TOKENIZED_FILE_TOKEN \
  PW_CONCAT(_PW_ASSERT_TOKENIZED_FILE_TOKEN_, PW_EMPTY_ARGS(static))(__FILE__)
#define _PW_ASSERT_TOKENIZED_FILE_TOKEN_0 PW_TOKENIZE_STRING
#define _PW_ASSERT_TOKENIZED_FILE_TOKEN_1 PW_TOKENIZER_STRING_TOKEN
#else
#define _PW_ASSERT_TOKENIZED_FILE_TOKEN PW_TOKENIZER_STRING_TOKEN(__FILE__)
#endif

#define PW_ASSERT_HANDLE_FAILURE(expression)                                  \
  do {                                                                        \
    _PW_TOKENIZER_CONST uint32_t _pw_assert_file_token =                      \
        _PW_ASSERT_TOKENIZED_FILE_TOKEN;                                      \
    pw_assert_tokenized_HandleAssertFailure(_pw_assert_file_token, __LINE__); \
  } while (0)
