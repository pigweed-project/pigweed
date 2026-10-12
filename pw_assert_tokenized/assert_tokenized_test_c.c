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

#include "pw_assert_tokenized/assert_tokenized.h"

#ifdef __cplusplus
#error "This file must be compiled as plain C to verify C compilation works."
#endif  // __cplusplus

void pw_assert_tokenized_TestC(void);
void pw_assert_tokenized_TestCNoStatics(void);

void pw_assert_tokenized_TestC(void) {
  if (0) {
    PW_ASSERT_HANDLE_FAILURE("test");
  }
}

// Simulate FreeRTOS tasks.c defining `static` to nothing after including
// headers (via portREMOVE_STATIC_QUALIFIER).
#define static

void pw_assert_tokenized_TestCNoStatics(void) {
  if (0) {
    PW_ASSERT_HANDLE_FAILURE("test");
  }
}
