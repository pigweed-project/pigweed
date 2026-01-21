// Copyright 2025 The Pigweed Authors
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

// We will only override if asserts are enabled, otherwise, we will leave the
// zephyr asserts as default.
#ifdef __ASSERT_ON

#include <pw_assert/check.h>

#undef __ASSERT
#undef __ASSERT_EVAL
#undef __ASSERT_NO_MSG

#define __ASSERT_NO_MSG(condition) PW_CHECK(condition)

// TODO(b/235149326): Current limitations include issues if '%' is in the
// condition statement due to format string. However, this should be okay with
// pw_assert_tokenized as it doesn't use format string.
#define __ASSERT(condition, format, ...) \
  PW_CHECK(condition, format, ##__VA_ARGS__)

#define __ASSERT_EVAL(expr1, expr2, condition, format, ...) \
  do {                                                      \
    expr2;                                                  \
    __ASSERT(condition, format, ##__VA_ARGS__);             \
  } while (false)

#endif
