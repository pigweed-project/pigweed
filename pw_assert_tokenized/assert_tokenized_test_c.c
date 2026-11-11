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

#ifdef __cplusplus
#error "This file must be compiled as plain C to verify C compilation works."
#endif  // __cplusplus

#include "pw_assert_tokenized/assert_tokenized.h"

PW_NO_RETURN void pw_assert_tokenized_TestHandleAssertFailure(
    uint32_t tokenized_file_name, int line_number);
PW_NO_RETURN void pw_assert_tokenized_TestHandleCheckFailure(
    uint32_t tokenized_message, int line_number);

#define pw_assert_tokenized_HandleAssertFailure \
  pw_assert_tokenized_TestHandleAssertFailure
#define pw_assert_tokenized_HandleCheckFailure \
  pw_assert_tokenized_TestHandleCheckFailure

void pw_assert_tokenized_TestAssertBackendC(uint32_t* expected_token,
                                            int* expected_line);
void pw_assert_tokenized_TestAssertBackendCNoStatics(uint32_t* expected_token,
                                                     int* expected_line);
void pw_assert_tokenized_TestAssertWithConditionBackendC(
    uint32_t* expected_token, int* expected_line);

void pw_assert_tokenized_TestAssertBackendC(uint32_t* expected_token,
                                            int* expected_line) {
  *expected_token = PW_TOKENIZER_STRING_TOKEN(__FILE__);
  *expected_line = __LINE__ + 1;
  PW_ASSERT_HANDLE_FAILURE("x > 0");
}

// Simulate FreeRTOS tasks.c defining `static` to nothing after including
// headers (via portREMOVE_STATIC_QUALIFIER).
#define static

void pw_assert_tokenized_TestAssertBackendCNoStatics(uint32_t* expected_token,
                                                     int* expected_line) {
  *expected_token = PW_TOKENIZER_STRING_TOKEN(__FILE__);
  *expected_line = __LINE__ + 1;
  PW_ASSERT_HANDLE_FAILURE("x > 0");
}

#undef static

#undef PW_ASSERT_HANDLE_FAILURE
#include "pw_assert_tokenized/assert_with_condition.h"

#if defined(PW_LOG_MODULE_NAME) || defined(PW_LOG_LEVEL) || \
    defined(PW_LOG_FLAGS)
#error \
    "assert_with_condition.h must not define PW_LOG_MODULE_NAME, PW_LOG_LEVEL, or PW_LOG_FLAGS."
#endif  // defined(PW_LOG_MODULE_NAME) || defined(PW_LOG_LEVEL) ||
        // defined(PW_LOG_FLAGS)

#define PW_LOG_MODULE_NAME "TEST"
#define PW_LOG_LEVEL 1
#define PW_LOG_FLAGS 0

void pw_assert_tokenized_TestAssertWithConditionBackendC(
    uint32_t* expected_token, int* expected_line) {
  *expected_token = PW_TOKENIZER_STRING_TOKEN(
      "■msg♦Assert failure: x > 0■module♦PW_ASSERT■file♦" __FILE__);
  *expected_line = __LINE__ + 1;
  PW_ASSERT_HANDLE_FAILURE("x > 0");
}
