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

// This is a test of using pw_tokenizer in C99. It can be compiled outside of
// the GN build by adding a main() function that calls RunTestAndReturnPassed()
// and invoking the compiler as follows:
/*
  gcc -std=c99 -Wall -Wextra \
      -Ipw_assert/public \
      -Ipw_assert/print_and_abort_check_public_overrides \
      -Ipw_containers/public \
      -Ipw_polyfill/public \
      -Ipw_preprocessor/public \
      -Ipw_tokenizer/public \
      -Ipw_varint/public \
      pw_tokenizer/tokenize_c99_test.c \
      pw_varint/varint_c.c \
      pw_containers/inline_var_len_entry_queue.c
 */

#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#if !defined(__clang__)
// GCC does not have narrower flags like -Wno-c11-extensions or
// -Wno-gnu-zero-variadic-macro-arguments to silence pedantic warnings for
// _Static_assert and empty variadic macro arguments in -std=c99 mode.
#pragma GCC diagnostic ignored "-Wpedantic"
#endif  // !defined(__clang__)

#include "pw_containers/inline_var_len_entry_queue.h"
#include "pw_tokenizer/encode_args.h"
#include "pw_tokenizer/tokenize.h"
#include "pw_varint/varint.h"

static_assert(__STDC_VERSION__ == 199901L,
              "This test should be compiled with -std=c99.");

PW_VARIABLE_LENGTH_ENTRY_QUEUE_DECLARE(buffer, 256);

// Encodes a tokenized message with any number of int arguments.
static void TokenizeIntegersOnly(uint32_t token, int arg_count, ...) {
  va_list args;
  va_start(args, arg_count);

  // Encode the tokenized log to a temporary buffer.
  uint8_t encoded[32];
  memcpy(encoded, &token, sizeof(token));

  size_t index = sizeof(token);

  for (int i = 0; i < arg_count; ++i) {
    // Encode each int argument.
    int argument = va_arg(args, int);

    // Encode the argument to the buffer
    index += pw_tokenizer_EncodeInt(
        argument, &encoded[index], sizeof(encoded) - index);
  }

  // Write the encoded log to the ring buffer
  pw_InlineVarLenEntryQueue_PushOverwrite(buffer, encoded, (uint32_t)index);

  va_end(args);
}

// Tokenization macro that only handles int arguments.
#define TOKENIZE_INTS(format, ...)                             \
  do {                                                         \
    PW_TOKENIZE_FORMAT_STRING_ANY_ARG_COUNT(                   \
        "tokenize_c99_test", UINT32_MAX, format, __VA_ARGS__); \
    TokenizeIntegersOnly(_pw_tokenizer_token,                  \
                         PW_FUNCTION_ARG_COUNT(__VA_ARGS__)    \
                             PW_COMMA_ARGS(__VA_ARGS__));      \
  } while (0)

// C version of the ASSERT_EQ macro that returns a string version of the failing
// line.
#define ASSERT_EQ(lhs, rhs)                                     \
  if ((lhs) != (rhs)) {                                         \
    return FILE_LINE ": ASSERT_EQ(" #lhs ", " #rhs ") failed!"; \
  }

#define FILE_LINE __FILE__ ":" STRINGIFY(__LINE__)
#define STRINGIFY(x) _STRINGIFY(x)
#define _STRINGIFY(x) #x

// This test tokenize a few strings with arguments and checks the contents.
// It is called from tokenize_c99_test_entry_point.cc.
const char* RunTestAndReturnPassed(void) {
  TOKENIZE_INTS("Tokenize this with no arguments!");
  TOKENIZE_INTS("One arg, one byte: %x", -1);
  TOKENIZE_INTS("One arg, 5 bytes: %ld", (long)INT32_MAX);
  TOKENIZE_INTS("Three args, 4 bytes: %d %d %d", 1, 63, 128);

  ASSERT_EQ(pw_InlineVarLenEntryQueue_Size(buffer), 4u);

  pw_InlineVarLenEntryQueue_Iterator it =
      pw_InlineVarLenEntryQueue_Begin(buffer);
  pw_InlineVarLenEntryQueue_Entry entry =
      pw_InlineVarLenEntryQueue_GetEntry(&it);

  ASSERT_EQ(entry.size_1, sizeof(uint32_t) + 0);
  ASSERT_EQ(entry.size_2, 0u);

  pw_InlineVarLenEntryQueue_Iterator_Advance(&it);
  entry = pw_InlineVarLenEntryQueue_GetEntry(&it);
  ASSERT_EQ(entry.size_1, sizeof(uint32_t) + 1);
  ASSERT_EQ(entry.size_2, 0u);

  pw_InlineVarLenEntryQueue_Iterator_Advance(&it);
  entry = pw_InlineVarLenEntryQueue_GetEntry(&it);
  ASSERT_EQ(entry.size_1, sizeof(uint32_t) + 5);
  ASSERT_EQ(entry.size_2, 0u);

  pw_InlineVarLenEntryQueue_Iterator_Advance(&it);
  entry = pw_InlineVarLenEntryQueue_GetEntry(&it);
  ASSERT_EQ(entry.size_1, sizeof(uint32_t) + 4);
  ASSERT_EQ(entry.size_2, 0u);

  pw_InlineVarLenEntryQueue_Iterator_Advance(&it);
  pw_InlineVarLenEntryQueue_Iterator end =
      pw_InlineVarLenEntryQueue_End(buffer);
  ASSERT_EQ(pw_InlineVarLenEntryQueue_Iterator_Equal(&it, &end), true);

  return "passed";
}
