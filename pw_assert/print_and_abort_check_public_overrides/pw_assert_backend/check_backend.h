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

// IWYU pragma: private, include "pw_assert/check.h"

#include <stdio.h>  // IWYU pragma: export

#include "pw_assert/internal/print_and_abort.h"  // IWYU pragma: export

/// @module{pw_assert}

/// Triggers a system crash or halt, and if possible, delivers the specified
/// message and arguments to the user or developer.
#define PW_HANDLE_CRASH(...)                         \
  PW_ASSERT_PRINT_EXPRESSION("CRASH", "PW_CRASH()"); \
  _PW_ASSERT_PRINT_MESSAGE_AND_ABORT(__VA_ARGS__)

/// Triggers a system crash or halt, and if possible, delivers the condition
/// string (indicating what expression was false) and the message with format
/// arguments, to the user or developer.
///
/// This macro is invoked from the `PW_CHECK` facade macro if condition is
/// false.
#define PW_HANDLE_ASSERT_FAILURE(condition_string, ...)  \
  PW_ASSERT_PRINT_EXPRESSION("CHECK", condition_string); \
  _PW_ASSERT_PRINT_MESSAGE_AND_ABORT(__VA_ARGS__)

/// Triggers a system crash or halt for a failed binary comparison assert (e.g.
/// any of the `PW_CHECK_<type>_<op>` macros). The handler should combine the
/// assert components into a useful message for the user; though in some cases
/// this may not be possible.
///
/// - `arg_a_str` - Stringified first operand.
/// - `arg_a_val` - The value of the first operand.
/// - `comparison_op_str` - The string version of the operator.
/// - `arg_b_str` - Stringified second operand.
/// - `arg_b_val` - The value of the second operand.
/// - `type_fmt` - The format code for the type.
/// - `...` - A formatted message and arguments to go with the assert.
#define PW_HANDLE_ASSERT_BINARY_COMPARE_FAILURE(arg_a_str,                   \
                                                arg_a_val,                   \
                                                comparison_op_str,           \
                                                arg_b_str,                   \
                                                arg_b_val,                   \
                                                type_fmt,                    \
                                                ...)                         \
  PW_ASSERT_PRINT_EXPRESSION("CHECK",                                        \
                             arg_a_str " " comparison_op_str " " arg_b_str); \
  fprintf(stderr,                                                            \
          "  \033[33mEVALUATED CONDITION\033[0m\n\n    " arg_a_str           \
          " (=" type_fmt ") " comparison_op_str " " arg_b_str " (=" type_fmt \
          ")"                                                                \
          ".\n\n",                                                           \
          arg_a_val,                                                         \
          arg_b_val);                                                        \
  _PW_ASSERT_PRINT_MESSAGE_AND_ABORT(__VA_ARGS__)

/// @endmodule

#define _PW_ASSERT_PRINT_MESSAGE_AND_ABORT(...)                    \
  fprintf(stderr, "  \033[33mMESSAGE\033[0m\n\n    " __VA_ARGS__); \
  fprintf(stderr, "\n\n");                                         \
  fflush(stderr);                                                  \
  abort()
