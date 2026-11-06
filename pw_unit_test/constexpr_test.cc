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

#include <limits>

#include "pw_compilation_testing/negative_compilation.h"

// Start with an example test for the docs, which includes the #includes.
// DOCSTAG[pw_unit_test-constexpr]
#include "pw_unit_test/constexpr.h"
#include "pw_unit_test/framework.h"

namespace {

constexpr int ComputeSum(int lhs, int rhs) { return lhs + rhs; }

PW_CONSTEXPR_TEST(PwConstexprTestExample, AddNumbersOverflow, {
  // Use PW_TEST_EXPECT/ASSERT macros like regular GoogleTest macros.
  PW_TEST_EXPECT_EQ(ComputeSum(1, -2), -1);
  PW_TEST_EXPECT_LT(ComputeSum(1, 1), ComputeSum(2, 2));

  PW_TEST_ASSERT_EQ(ComputeSum(0, 0), 0);
  PW_TEST_EXPECT_EQ(ComputeSum(-123, 0), -123) << "Additive identity";
});

}  // namespace
// DOCSTAG[pw_unit_test-constexpr]

namespace {

PW_CONSTEXPR_TEST(PwConstexprTest, AllMacros, {
  PW_TEST_EXPECT_TRUE(true) << "";
  PW_TEST_EXPECT_FALSE(false) << "";

  PW_TEST_EXPECT_EQ(0, 0) << "";
  PW_TEST_EXPECT_NE(0, 1) << "";
  PW_TEST_EXPECT_GT(1, 0) << "";
  PW_TEST_EXPECT_GE(0, 0) << "";
  PW_TEST_EXPECT_LT(-1, 0) << "";
  PW_TEST_EXPECT_LE(0, 0) << "";

  PW_TEST_EXPECT_NEAR(0, 0, 1) << "";
  PW_TEST_EXPECT_FLOAT_EQ(0.0f, 0.0f) << "";
  PW_TEST_EXPECT_DOUBLE_EQ(0.0, 0.0) << "";

  PW_TEST_ASSERT_STREQ("", "") << "";
  PW_TEST_ASSERT_STRNE("", "a") << "";

  PW_TEST_ASSERT_TRUE(true) << "";
  PW_TEST_ASSERT_FALSE(false) << "";

  PW_TEST_ASSERT_EQ(0, 0) << "";
  PW_TEST_ASSERT_NE(0, 1) << "";
  PW_TEST_ASSERT_GT(1, 0) << "";
  PW_TEST_ASSERT_GE(0, 0) << "";
  PW_TEST_ASSERT_LT(-1, 0) << "";
  PW_TEST_ASSERT_LE(0, 0) << "";

  PW_TEST_ASSERT_NEAR(0, 0, 1) << "";
  PW_TEST_ASSERT_FLOAT_EQ(0.0f, 0.0f) << "";
  PW_TEST_ASSERT_DOUBLE_EQ(0.0, 0.0) << "";

  PW_TEST_ASSERT_STREQ("", "") << "";
  PW_TEST_ASSERT_STRNE("", "a") << "";
});

PW_CONSTEXPR_TEST(PwConstexprTest, CommasOutsideMacrosExpandCorrectly, {
  int a = 1, b = 2, c = 3;
  PW_TEST_EXPECT_LT(a, b);
  PW_TEST_EXPECT_EQ(b + 1, c);

  int sum = ComputeSum(a, b);
  PW_TEST_EXPECT_EQ(sum, c);
});

PW_CONSTEXPR_TEST_IF_CPP20(PwConstexprTest, ConstexprIfCpp20, {
  PW_TEST_EXPECT_EQ(ComputeSum(2, 3), 5);
});

// block-submission: disable
// DOCSTAG[pw_unit_test-constexpr-skip]
// Subsequent PW_CONSTEXPR_TESTs will skip the constexpr portion of the test.
// This allows you to use the richer GoogleTest-style output to debug failures.
#define SKIP_CONSTEXPR_TESTS_DONT_SUBMIT

void NotConstexpr() {}

PW_CONSTEXPR_TEST(PwConstexprTest, NotConstexprButDisabledByMacro, {
  // This test is not constexpr, but the constexpr test is skipped because the
  // SKIP_CONSTEXPR_TESTS macro is defined.
  NotConstexpr();
  PW_TEST_EXPECT_TRUE(true);
});

// Now, constexpr tests will no longer be skipped, and the same test will fail.
#undef SKIP_CONSTEXPR_TESTS_DONT_SUBMIT
// DOCSTAG[pw_unit_test-constexpr-skip]
// block-submission: enable

#if PW_NC_TEST(NonConstexprFailsToCompile)
PW_NC_EXPECT("NotConstexpr");

PW_CONSTEXPR_TEST(PwConstexprTest, NotConstexpr, {
  NotConstexpr();
  PW_TEST_EXPECT_TRUE(true);
});
#endif  // PW_NC_TEST

#if PW_NC_TEST(FailingTestFailsToCompile)
PW_NC_EXPECT("EXPECT_TRUE_FAILED");

PW_CONSTEXPR_TEST(PwConstexprTest, FailingTest, {
  PW_TEST_EXPECT_TRUE(false);
});
#endif  // PW_NC_TEST

#if PW_NC_TEST(NonConstexprFailsToCompileWithClang)
PW_NC_EXPECT_CLANG("non-constexpr function");
PW_NC_EXPECT_GCC("never defined");

#if defined(__clang__)
bool UsingClang() { return true; }
#elif defined(__GNUC__)
constexpr bool UsingClang();
#endif  // defined(__clang__)

PW_CONSTEXPR_TEST_IF_CLANG(PwConstexprTest, FailsToCompileWithClang, {
  PW_TEST_EXPECT_TRUE(UsingClang());
});
#endif  // PW_NC_TEST

#if PW_NC_TEST(NonConstexprFailsToCompileWithGCC)
PW_NC_EXPECT_CLANG("not defined");
PW_NC_EXPECT_GCC("non-constant condition");

#if defined(__clang__)
constexpr bool UsingGcc();
#elif defined(__GNUC__)
bool UsingGcc() { return true; }
#endif  // defined(__clang__)

PW_CONSTEXPR_TEST_IF_GCC(PwConstexprTest, FailsToCompileWithGCC, {
  PW_TEST_EXPECT_TRUE(UsingGcc());
});
#endif  // PW_NC_TEST

}  // namespace
