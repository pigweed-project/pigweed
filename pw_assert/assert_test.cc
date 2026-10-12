// Copyright 2020 The Pigweed Authors
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

#include "pw_assert/assert.h"

#include <memory>
#include <utility>

#include "pw_allocator/testing.h"
#include "pw_allocator/unique_ptr.h"
#include "pw_assert_test/fake_backend.h"
#include "pw_compilation_testing/negative_compilation.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

#undef PW_ASSERT_HANDLE_FAILURE
#define PW_ASSERT_HANDLE_FAILURE(condition_string) \
  PW_HANDLE_ASSERT_FAILURE(condition_string, "")

namespace {

// PW_ASSERT() should always be enabled, and always evaluate the expression.
TEST(Assert, AssertTrue) {
  int evaluated = 1;
  PW_ASSERT(++evaluated);
  EXPECT_EQ(evaluated, 2);
}

// PW_DASSERT() might be disabled sometimes.
TEST(Assert, DebugAssertTrue) {
  int evaluated = 1;
  PW_DASSERT(++evaluated);
  if (PW_ASSERT_ENABLE_DEBUG == 1) {
    EXPECT_EQ(evaluated, 2);
  } else {
    EXPECT_EQ(evaluated, 1);
  }
}

TEST(Assert, AssertOkEvaluatesExpressionAndDoesNotCrashOnOk) {
  int evaluated = 1;
  PW_ASSERT_OK(([&]() {
    ++evaluated;
    return pw::OkStatus();
  })());
  EXPECT_EQ(evaluated, 2);
}

TEST(Assert, AssertNotNullConstexpr) {
  static constexpr int kValue = 42;
  constexpr const int* not_null = PW_ASSERT_NOTNULL(&kValue);
  static_assert(*not_null == 42);
}

TEST(Assert, AssertNotNullRuntime) {
  pw_captured_assert.triggered = 0;

  int value = 42;
  int* not_null = PW_ASSERT_NOTNULL(&value);
  EXPECT_EQ(*not_null, 42);
  EXPECT_EQ(pw_captured_assert.triggered, 0);
}

TEST(Assert, AssertNotNullStlSmartPointer) {
  pw_captured_assert.triggered = 0;

  auto unique = std::make_unique<int>(42);
  std::unique_ptr<int>& not_null = PW_ASSERT_NOTNULL(unique);
  EXPECT_EQ(*not_null, 42);
  EXPECT_EQ(pw_captured_assert.triggered, 0);
}

TEST(Assert, AssertNotNullPigweedSmartPointer) {
  pw_captured_assert.triggered = 0;
  pw::allocator::test::AllocatorForTest<256> allocator;

  pw::UniquePtr<int> unique = allocator.MakeUnique<int>(42);
  pw::UniquePtr<int>& not_null = PW_ASSERT_NOTNULL(unique);
  EXPECT_EQ(*not_null, 42);
  EXPECT_EQ(pw_captured_assert.triggered, 0);
}

TEST(Assert, AssertNotNullMoveSmartPointers) {
  pw_captured_assert.triggered = 0;

  auto stl_unique = std::make_unique<int>(42);
  std::unique_ptr<int> moved_stl = PW_ASSERT_NOTNULL(std::move(stl_unique));
  EXPECT_EQ(*moved_stl, 42);

  pw::allocator::test::AllocatorForTest<256> allocator;
  pw::UniquePtr<int> pw_unique = allocator.MakeUnique<int>(42);
  pw::UniquePtr<int> moved_pw = PW_ASSERT_NOTNULL(std::move(pw_unique));
  EXPECT_EQ(*moved_pw, 42);

  EXPECT_EQ(pw_captured_assert.triggered, 0);
}

TEST(Assert, AssertNotNullRejectsNull) {
  pw_captured_assert.triggered = 0;
  int* null_ptr = nullptr;
  PW_ASSERT_NOTNULL(null_ptr);
  EXPECT_EQ(pw_captured_assert.triggered, 1);

  pw_captured_assert.triggered = 0;
  std::unique_ptr<int> null_stl_ptr;
  PW_ASSERT_NOTNULL(null_stl_ptr);
  EXPECT_EQ(pw_captured_assert.triggered, 1);

  pw_captured_assert.triggered = 0;
  pw::UniquePtr<int> null_pw_ptr;
  PW_ASSERT_NOTNULL(null_pw_ptr);
  EXPECT_EQ(pw_captured_assert.triggered, 1);
}

// Unfortunately, we don't have the infrastructure to test failure handling
// automatically, since the harness crashes in the process of running this
// test. The unsatisfying alternative is to test the functionality manually,
// then disable the test.

TEST(Assert, AssertFalse) {
  if (false) {
    PW_ASSERT(false);
  }
}

TEST(Assert, DebugAssertFalse) {
  if (false) {
    PW_DASSERT(false);
  }
}

#if PW_NC_TEST(ConstexprAssert)
PW_NC_EXPECT("PW_ASSERT_failed_in_constant_expression");

#line 1 "example.cc"  // DOCSTAG: [pw_assert-constexpr-example]
constexpr int DivideEvenNumberBy2(int value) {
  PW_ASSERT(value % 2 == 0);  // value must be even!
  return value / 2;
}

constexpr int kResult = DivideEvenNumberBy2(11);  // This fails the PW_ASSERT!
// DOCSTAG: [pw_assert-constexpr-example]

#endif  // PW_NC_TEST

}  // namespace
