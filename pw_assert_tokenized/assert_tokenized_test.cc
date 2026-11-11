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

#include <csetjmp>
#include <cstdint>

#include "pw_compilation_testing/negative_compilation.h"
#include "pw_unit_test/framework.h"

extern "C" void pw_assert_tokenized_TestAssertBackendC(uint32_t* expected_token,
                                                       int* expected_line);
extern "C" void pw_assert_tokenized_TestAssertBackendCNoStatics(
    uint32_t* expected_token, int* expected_line);
extern "C" void pw_assert_tokenized_TestAssertWithConditionBackendC(
    uint32_t* expected_token, int* expected_line);

#ifdef _PW_ASSERT_TOKENIZED_FORMAT_STRING
#pragma push_macro("PW_ASSERT_HANDLE_FAILURE")
#endif  // _PW_ASSERT_TOKENIZED_FORMAT_STRING
#ifndef _PW_ASSERT_TOKENIZED_FILE_TOKEN
#undef PW_ASSERT_HANDLE_FAILURE
#include "pw_assert_tokenized/assert_tokenized.h"
#endif  // _PW_ASSERT_TOKENIZED_FILE_TOKEN

namespace {

enum class CapturedHandler {
  kNone,
  kAssertFailure,
  kCheckFailure,
};

struct CapturedCall {
  CapturedHandler handler = CapturedHandler::kNone;
  uint32_t token = 0;
  int line = 0;
};

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::jmp_buf g_jump_buffer;
CapturedCall g_captured;
uint32_t g_expected_token = 0;
int g_expected_line = 0;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace

extern "C" PW_NO_RETURN void pw_assert_tokenized_TestHandleAssertFailure(
    uint32_t tokenized_file_name, int line_number) {
  g_captured = {
      CapturedHandler::kAssertFailure, tokenized_file_name, line_number};
  std::longjmp(g_jump_buffer, 1);
}

extern "C" PW_NO_RETURN void pw_assert_tokenized_TestHandleCheckFailure(
    uint32_t tokenized_message, int line_number) {
  g_captured = {CapturedHandler::kCheckFailure, tokenized_message, line_number};
  std::longjmp(g_jump_buffer, 1);
}

#define pw_assert_tokenized_HandleAssertFailure \
  pw_assert_tokenized_TestHandleAssertFailure
#define pw_assert_tokenized_HandleCheckFailure \
  pw_assert_tokenized_TestHandleCheckFailure

namespace {

constexpr int CheckPositiveAssertBackendConstexpr(int value) {
  if (value <= 0) {
    PW_ASSERT_HANDLE_FAILURE("value <= 0");
  }
  return value;
}

static_assert(CheckPositiveAssertBackendConstexpr(42) == 42);

void TriggerAssertBackendFailure() {
  g_expected_line = __LINE__ + 1;
  PW_ASSERT_HANDLE_FAILURE("x > 0");
}

// Simulate defining `static` to nothing after including headers in C++.
#define static

void TriggerAssertBackendNoStaticsFailure() {
  g_expected_line = __LINE__ + 1;
  PW_ASSERT_HANDLE_FAILURE("x > 0");
}

#undef static

TEST(AssertTokenized, FileTokenMatchesStringToken) {
  constexpr uint32_t kFileToken = _PW_ASSERT_TOKENIZED_FILE_TOKEN;
  EXPECT_EQ(kFileToken, PW_TOKENIZER_STRING_TOKEN(__FILE__));
}

TEST(AssertTokenized, AssertBackendDispatchesFileTokenToAssertFailureHandler) {
  g_captured = {};
  g_expected_line = 0;
  if (setjmp(g_jump_buffer) == 0) {
    TriggerAssertBackendFailure();
  }
  constexpr uint32_t kExpectedToken = PW_TOKENIZER_STRING_TOKEN(__FILE__);
  EXPECT_EQ(g_captured.handler, CapturedHandler::kAssertFailure);
  EXPECT_EQ(g_captured.token, kExpectedToken);
  EXPECT_EQ(g_captured.line, g_expected_line);
}

TEST(AssertTokenized,
     AssertBackendNoStaticsDispatchesFileTokenToAssertFailureHandler) {
  g_captured = {};
  g_expected_line = 0;
  if (setjmp(g_jump_buffer) == 0) {
    TriggerAssertBackendNoStaticsFailure();
  }
  constexpr uint32_t kExpectedToken = PW_TOKENIZER_STRING_TOKEN(__FILE__);
  EXPECT_EQ(g_captured.handler, CapturedHandler::kAssertFailure);
  EXPECT_EQ(g_captured.token, kExpectedToken);
  EXPECT_EQ(g_captured.line, g_expected_line);
}

TEST(AssertTokenized, AssertBackendDispatchesCAssertToAssertFailureHandler) {
  g_captured = {};
  g_expected_token = 0;
  g_expected_line = 0;
  if (setjmp(g_jump_buffer) == 0) {
    pw_assert_tokenized_TestAssertBackendC(&g_expected_token, &g_expected_line);
  }
  EXPECT_EQ(g_captured.handler, CapturedHandler::kAssertFailure);
  EXPECT_EQ(g_captured.token, g_expected_token);
  EXPECT_EQ(g_captured.line, g_expected_line);
}

TEST(AssertTokenized,
     AssertBackendDispatchesCNoStaticsAssertToAssertFailureHandler) {
  g_captured = {};
  g_expected_token = 0;
  g_expected_line = 0;
  if (setjmp(g_jump_buffer) == 0) {
    pw_assert_tokenized_TestAssertBackendCNoStatics(&g_expected_token,
                                                    &g_expected_line);
  }
  EXPECT_EQ(g_captured.handler, CapturedHandler::kAssertFailure);
  EXPECT_EQ(g_captured.token, g_expected_token);
  EXPECT_EQ(g_captured.line, g_expected_line);
}

}  // namespace

#ifdef _PW_ASSERT_TOKENIZED_FORMAT_STRING
#pragma pop_macro("PW_ASSERT_HANDLE_FAILURE")
#else
#undef PW_ASSERT_HANDLE_FAILURE
#include "pw_assert_tokenized/assert_with_condition.h"
#endif  // _PW_ASSERT_TOKENIZED_FORMAT_STRING

namespace {

#if __cpp_constexpr >= 202110L
constexpr int CheckPositiveAssertWithConditionBackendConstexpr(int value) {
  if (value <= 0) {
    PW_ASSERT_HANDLE_FAILURE("value <= 0");
  }
  return value;
}

static_assert(CheckPositiveAssertWithConditionBackendConstexpr(42) == 42);

// Adjacent string literals as passed by PW_ASSERT_NOTNULL.
#define _PW_ASSERT_TEST_NOTNULL_EXPR \
  "ptr"                              \
  " != nullptr"

void TriggerAssertWithConditionBackendCppFailure() {
  g_expected_line = __LINE__ + 1;
  PW_ASSERT_HANDLE_FAILURE(_PW_ASSERT_TEST_NOTNULL_EXPR);
}
#endif  // __cpp_constexpr >= 202110L

TEST(AssertTokenized,
     AssertWithConditionBackendFormatStringIncludesExpressionAndFile) {
  constexpr uint32_t kExpectedToken = PW_TOKENIZER_STRING_TOKEN(
      "■msg♦Assert failure: x > 0■module♦PW_ASSERT■file♦" __FILE__);
  constexpr uint32_t kActualToken =
      PW_TOKENIZER_STRING_TOKEN(_PW_ASSERT_TOKENIZED_FORMAT_STRING("x > 0"));
  EXPECT_EQ(kActualToken, kExpectedToken);
}

TEST(AssertTokenized,
     AssertWithConditionBackendDispatchesCAssertToCheckFailureHandler) {
  g_captured = {};
  g_expected_token = 0;
  g_expected_line = 0;
  if (setjmp(g_jump_buffer) == 0) {
    pw_assert_tokenized_TestAssertWithConditionBackendC(&g_expected_token,
                                                        &g_expected_line);
  }
  EXPECT_EQ(g_captured.handler, CapturedHandler::kCheckFailure);
  EXPECT_EQ(g_captured.token, g_expected_token);
  EXPECT_EQ(g_captured.line, g_expected_line);
}

#if __cpp_constexpr >= 202110L
TEST(AssertTokenized,
     AssertWithConditionBackendDispatchesCppAssertToCheckFailureHandler) {
  g_captured = {};
  g_expected_line = 0;
  if (setjmp(g_jump_buffer) == 0) {
    TriggerAssertWithConditionBackendCppFailure();
  }
  constexpr uint32_t kExpectedToken = PW_TOKENIZER_STRING_TOKEN(
      "■msg♦Assert failure: ptr != nullptr■module♦PW_ASSERT■file♦" __FILE__);
  EXPECT_EQ(g_captured.handler, CapturedHandler::kCheckFailure);
  EXPECT_EQ(g_captured.token, kExpectedToken);
  EXPECT_EQ(g_captured.line, g_expected_line);
}
#endif  // __cpp_constexpr >= 202110L

#if __cpp_constexpr >= 202110L
#if PW_NC_TEST(AssertWithConditionBackendFailsWhenStaticIsDefinedToNothing)
PW_NC_EXPECT(
    "pw_assert_tokenized:assert_with_condition_backend cannot be used "
    "when `static` is defined to nothing");

#define static
void TriggerNoStaticsFailure() { PW_ASSERT_HANDLE_FAILURE("test"); }
#undef static
#endif  // PW_NC_TEST
#else
#if PW_NC_TEST(AssertWithConditionBackendRequiresCpp23)
PW_NC_EXPECT(
    "pw_assert_tokenized:assert_with_condition_backend requires C\+\+23 "
    "or newer \(__cpp_constexpr >= 202110L\)");

void TriggerPreCpp23Failure() { PW_ASSERT_HANDLE_FAILURE("test"); }
#endif  // PW_NC_TEST
#endif  // __cpp_constexpr >= 202110L

}  // namespace
