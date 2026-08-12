// Copyright 2024 The Pigweed Authors
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

#include <zircon/assert.h>

#define PW_HANDLE_CRASH(fmt, ...) ZX_PANIC(fmt, ##__VA_ARGS__)

#define PW_HANDLE_ASSERT_FAILURE(x, msg, ...) \
  ZX_PANIC(                                   \
      "ASSERT FAILED at (%s:%d): %s\n" msg, __FILE__, __LINE__, #x, ##__VA_ARGS__)

#define PW_HANDLE_ASSERT_BINARY_COMPARE_FAILURE(arg_a_str,         \
                                                arg_a_val,         \
                                                comparison_op_str, \
                                                arg_b_str,         \
                                                arg_b_val,         \
                                                type_fmt,          \
                                                msg,               \
                                                ...)               \
  ZX_PANIC("ASSERT FAILED at (%s:%d): " arg_a_str " (=" type_fmt           \
           ") " comparison_op_str " " arg_b_str " (=" type_fmt ") \n" msg, \
           __FILE__,                                                       \
           __LINE__,                                                       \
           arg_a_val,                                                      \
           arg_b_val,                                                      \
           ##__VA_ARGS__)
