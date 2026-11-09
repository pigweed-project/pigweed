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

// pw::Result is derived from absl::StatusOr, but has some small differences.
// This test covers basic pw::Result functionality and as well as the features
// supported by pw::Result that are not supported by absl::StatusOr (constexpr
// use in particular).
//
// The complete, thorough pw::Result tests are in statusor_test.cc, which is
// derived from Abseil's tests for absl::StatusOr.

#include "pw_result/result.h"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "pw_assert/assert.h"
#include "pw_compilation_testing/negative_compilation.h"
#include "pw_containers/internal/test_helpers.h"
#include "pw_result_private/test_helpers.h"
#include "pw_status/status.h"
#include "pw_status/try.h"
#include "pw_unit_test/constexpr.h"
#include "pw_unit_test/framework.h"

namespace pw {
namespace {

static_assert(std::is_same_v<decltype(Result<int>().status()), Status>);
static_assert(std::is_same_v<
              decltype(static_cast<const Result<int>&>(Result<int>()).status()),
              Status>);

TEST(Result, CreateOk) {
  Result<const char*> res("hello");
  EXPECT_TRUE(res.ok());
  EXPECT_EQ(res.status(), OkStatus());
  EXPECT_EQ(res.value(), "hello");
}

TEST(Result, CreateOkTypeDeduction) {
  auto res = Result("hello");
  static_assert(std::is_same_v<decltype(res), Result<const char*>>);
  EXPECT_TRUE(res.ok());
  EXPECT_EQ(res.status(), OkStatus());
  EXPECT_STREQ(res.value(), "hello");
}

TEST(Result, TypeDeductionFromStatus) {
  Result ok_res = OkStatus();
  static_assert(std::is_same_v<decltype(ok_res), Result<Status>>);
  EXPECT_TRUE(ok_res.ok());
  EXPECT_EQ(*ok_res, OkStatus());

  Result err_res = Status::NotFound();
  static_assert(std::is_same_v<decltype(err_res), Result<Status>>);
  EXPECT_TRUE(err_res.ok());
  EXPECT_EQ(*err_res, Status::NotFound());
  EXPECT_TRUE(err_res->IsNotFound());
}

TEST(Result, CreateNotOk) {
  Result<int> res(Status::DataLoss());
  EXPECT_FALSE(res.ok());
  EXPECT_EQ(res.status(), Status::DataLoss());
}

TEST(Result, ValueOr) {
  Result<int> good(3);
  Result<int> bad(Status::DataLoss());
  EXPECT_EQ(good.value_or(42), 3);
  EXPECT_EQ(bad.value_or(42), 42);
}

TEST(Result, Deref) {
  struct Tester {
    constexpr bool True() { return true; }
    constexpr bool False() { return false; }
  };

  auto tester = Result<Tester>(Tester());
  EXPECT_TRUE(tester.ok());
  EXPECT_TRUE(tester->True());
  EXPECT_FALSE(tester->False());
  EXPECT_TRUE((*tester).True());
  EXPECT_FALSE((*tester).False());
  EXPECT_EQ(tester.value().True(), tester->True());
  EXPECT_EQ(tester.value().False(), tester->False());
}

TEST(Result, ConstDeref) {
  struct Tester {
    constexpr bool True() const { return true; }
    constexpr bool False() const { return false; }
  };

  const auto tester = Result<Tester>(Tester());
  EXPECT_TRUE(tester.ok());
  EXPECT_TRUE(tester->True());
  EXPECT_FALSE(tester->False());
  EXPECT_TRUE((*tester).True());
  EXPECT_FALSE((*tester).False());
  EXPECT_EQ(tester.value().True(), tester->True());
  EXPECT_EQ(tester.value().False(), tester->False());
}

TEST(Result, ConstructType) {
  struct Point {
    Point(int a, int b) : x(a), y(b) {}

    int x;
    int y;
  };

  Result<Point> origin{std::in_place, 0, 0};
  ASSERT_TRUE(origin.ok());
  ASSERT_EQ(origin.value().x, 0);
  ASSERT_EQ(origin.value().y, 0);
}

Result<float> Divide(float a, float b) {
  if (b == 0) {
    return Status::InvalidArgument();
  }
  return a / b;
}

TEST(Divide, ReturnOk) {
  Result<float> res = Divide(10, 5);
  ASSERT_TRUE(res.ok());
  EXPECT_EQ(res.value(), 2.0f);
}

TEST(Divide, ReturnNotOk) {
  Result<float> res = Divide(10, 0);
  EXPECT_FALSE(res.ok());
  EXPECT_EQ(res.status(), Status::InvalidArgument());
}

Result<bool> ReturnResult(Result<bool> result) { return result; }

Status TryResultAssign(Result<bool> result) {
  PW_TRY_ASSIGN(const bool value, ReturnResult(result));

  // Any status other than OK should have already returned.
  EXPECT_EQ(result.status(), OkStatus());
  EXPECT_EQ(value, result.value());
  return result.status();
}

TEST(Result, TryAssign) {
  EXPECT_EQ(TryResultAssign(Status::Cancelled()), Status::Cancelled());
  EXPECT_EQ(TryResultAssign(Status::DataLoss()), Status::DataLoss());
  EXPECT_EQ(TryResultAssign(Status::Unimplemented()), Status::Unimplemented());
  EXPECT_EQ(TryResultAssign(false), OkStatus());
  EXPECT_EQ(TryResultAssign(true), OkStatus());
}

constexpr int kExpectedVal = 5;

class Immovable {
 public:
  Immovable() = delete;
  explicit constexpr Immovable(int val) : val_(val) {}
  Immovable(const Immovable&) = delete;
  Immovable& operator=(const Immovable&) = delete;
  Immovable(Immovable&&) = delete;
  Immovable& operator=(Immovable&&) = delete;
  constexpr int val() const { return val_; }

 private:
  int val_;
};

Result<Immovable> MakeImmovable() { return Result<Immovable>(kExpectedVal); }

Result<int> TryResultAssignImmovable() {
  PW_TRY_ASSIGN(auto&& thingy, MakeImmovable());
  return thingy.val();
}

TEST(Result, TryAssignImmovable) {
  EXPECT_EQ(TryResultAssignImmovable(), Result<int>(kExpectedVal));
}

struct Value {
  int number;
};

TEST(Result, ConstexprOk) {
  static constexpr pw::Result<Value> kResult(Value{123});

  static_assert(kResult.status() == pw::OkStatus());
  static_assert(kResult.ok());

  static_assert((*kResult).number == 123);
  static_assert((*std::move(kResult)).number == 123);

  static_assert(kResult->number == 123);
  static_assert(std::move(kResult)->number == 123);

  static_assert(kResult.value().number == 123);
  static_assert(std::move(kResult).value().number == 123);

  static_assert(kResult.value_or(Value{99}).number == 123);
  static_assert(std::move(kResult).value_or(Value{99}).number == 123);
}

TEST(Result, ConstexprNotOk) {
  static constexpr pw::Result<Value> kResult(pw::Status::NotFound());

  static_assert(kResult.status() == pw::Status::NotFound());
  static_assert(!kResult.ok());

  static_assert(kResult.value_or(Value{99}).number == 99);
  static_assert(std::move(kResult).value_or(Value{99}).number == 99);
}

TEST(Result, ConstexprNotOkCopy) {
  static constexpr pw::Result<Value> kResult(pw::Status::NotFound());
  constexpr pw::Result<Value> kResultCopy(kResult);

  static_assert(kResultCopy.status() == pw::Status::NotFound());
  static_assert(!kResultCopy.ok());

  static_assert(kResultCopy.value_or(Value{99}).number == 99);
  static_assert(std::move(kResultCopy).value_or(Value{99}).number == 99);
}

auto multiply = [](int x) -> Result<int> { return x * 2; };
auto add_two = [](int x) -> Result<int> { return x + 2; };
auto fail_unknown = [](int) -> Result<int> { return Status::Unknown(); };

TEST(Result, AndThenNonConstLValueRefInvokeSuccess) {
  Result<int> r = 32;
  auto ret = r.and_then(multiply);
  ASSERT_TRUE(ret.ok());
  EXPECT_EQ(*ret, 64);
}

TEST(Result, AndThenNonConstLValueRefInvokeFail) {
  Result<int> r = 32;
  auto ret = r.and_then(fail_unknown);
  ASSERT_FALSE(ret.ok());
  EXPECT_EQ(ret.status(), Status::Unknown());
}

TEST(Result, AndThenNonConstLValueRefSkips) {
  Result<int> r = Status::NotFound();
  auto ret = r.and_then(multiply);
  ASSERT_FALSE(ret.ok());
  EXPECT_EQ(ret.status(), Status::NotFound());
}

TEST(Result, AndThenNonConstRvalueRefInvokeSuccess) {
  Result<int> r = 32;
  auto ret = std::move(r).and_then(multiply);
  ASSERT_TRUE(ret.ok());
  EXPECT_EQ(*ret, 64);
}

TEST(Result, AndThenNonConstRvalueRefInvokeFails) {
  Result<int> r = 64;
  auto ret = std::move(r).and_then(fail_unknown);
  ASSERT_FALSE(ret.ok());
  EXPECT_EQ(ret.status(), Status::Unknown());
}

TEST(Result, AndThenNonConstRvalueRefSkips) {
  Result<int> r = Status::NotFound();
  auto ret = std::move(r).and_then(multiply);
  ASSERT_FALSE(ret.ok());
  EXPECT_EQ(ret.status(), Status::NotFound());
}

TEST(Result, AndThenConstLValueRefInvokeSuccess) {
  const Result<int> r = 32;
  auto ret = r.and_then(multiply);
  ASSERT_TRUE(ret.ok());
  EXPECT_EQ(*ret, 64);
}

TEST(Result, AndThenConstLValueRefInvokeFail) {
  const Result<int> r = 32;
  auto ret = r.and_then(fail_unknown);
  ASSERT_FALSE(ret.ok());
  EXPECT_EQ(ret.status(), Status::Unknown());
}

TEST(Result, AndThenConstLValueRefSkips) {
  const Result<int> r = Status::NotFound();
  auto ret = r.and_then(multiply);
  ASSERT_FALSE(ret.ok());
  EXPECT_EQ(ret.status(), Status::NotFound());
}

TEST(Result, AndThenConstRValueRefInvokeSuccess) {
  const Result<int> r = 32;
  auto ret = std::move(r).and_then(multiply);
  ASSERT_TRUE(ret.ok());
  EXPECT_EQ(*ret, 64);
}

TEST(Result, AndThenConstRValueRefInvokeFail) {
  const Result<int> r = 32;
  auto ret = std::move(r).and_then(fail_unknown);
  ASSERT_FALSE(ret.ok());
  EXPECT_EQ(ret.status(), Status::Unknown());
}

TEST(Result, AndThenConstRValueRefSkips) {
  const Result<int> r = Status::NotFound();
  auto ret = std::move(r).and_then(multiply);
  ASSERT_FALSE(ret.ok());
  EXPECT_EQ(ret.status(), Status::NotFound());
}

TEST(Result, AndThenMultipleChained) {
  Result<int> r = 32;
  auto ret = r.and_then(multiply).and_then(add_two).and_then(multiply);
  ASSERT_TRUE(ret.ok());
  EXPECT_EQ(*ret, 132);
}

auto return_status = [](Status) { return Status::Unknown(); };
auto return_result = [](Status) { return Result<int>(Status::Internal()); };

TEST(Result, OrElseNonConstLValueRefSkips) {
  Result<int> r = 32;
  auto ret = r.or_else(return_status);
  ASSERT_TRUE(ret.ok());
  EXPECT_EQ(*ret, 32);
}

TEST(Result, OrElseNonConstLValueRefStatusInvokes) {
  Result<int> r = Status::NotFound();
  auto ret = r.or_else(return_status);
  ASSERT_FALSE(ret.ok());
  EXPECT_EQ(ret.status(), Status::Unknown());
}

TEST(Result, OrElseNonConstLValueRefResultInvokes) {
  Result<int> r = Status::NotFound();
  auto ret = r.or_else(return_result);
  ASSERT_FALSE(ret.ok());
  EXPECT_EQ(ret.status(), Status::Internal());
}

TEST(Result, OrElseNonConstLValueRefVoidSkips) {
  Result<int> r = 32;
  bool invoked = false;
  auto ret = r.or_else([&invoked](Status) { invoked = true; });
  EXPECT_FALSE(invoked);
  ASSERT_TRUE(ret.ok());
  EXPECT_EQ(*ret, 32);
}

TEST(Result, OrElseNonConstLValueRefVoidInvokes) {
  Result<int> r = Status::NotFound();
  bool invoked = false;
  auto ret = r.or_else([&invoked](Status) { invoked = true; });
  EXPECT_TRUE(invoked);
  ASSERT_FALSE(ret.ok());
  EXPECT_EQ(ret.status(), Status::NotFound());
}

TEST(Result, OrElseNonConstRValueRefSkips) {
  Result<int> r = 32;
  auto ret = std::move(r).or_else(return_status);
  ASSERT_TRUE(ret.ok());
  EXPECT_EQ(*ret, 32);
}

TEST(Result, OrElseNonConstRValueRefStatusInvokes) {
  Result<int> r = Status::NotFound();
  auto ret = std::move(r).or_else(return_status);
  ASSERT_FALSE(ret.ok());
  EXPECT_EQ(ret.status(), Status::Unknown());
}

TEST(Result, OrElseNonConstRValueRefResultInvokes) {
  Result<int> r = Status::NotFound();
  auto ret = std::move(r).or_else(return_result);
  ASSERT_FALSE(ret.ok());
  EXPECT_EQ(ret.status(), Status::Internal());
}

TEST(Result, OrElseNonConstRValueRefVoidSkips) {
  Result<int> r = 32;
  bool invoked = false;
  auto ret = std::move(r).or_else([&invoked](Status) { invoked = true; });
  EXPECT_FALSE(invoked);
  ASSERT_TRUE(ret.ok());
  EXPECT_EQ(*ret, 32);
}

TEST(Result, OrElseNonConstRValueRefVoidInvokes) {
  Result<int> r = Status::NotFound();
  bool invoked = false;
  auto ret = std::move(r).or_else([&invoked](Status) { invoked = true; });
  EXPECT_TRUE(invoked);
  ASSERT_FALSE(ret.ok());
  EXPECT_EQ(ret.status(), Status::NotFound());
}

TEST(Result, OrElseConstLValueRefSkips) {
  const Result<int> r = 32;
  auto ret = r.or_else(return_status);
  ASSERT_TRUE(ret.ok());
  EXPECT_EQ(*ret, 32);
}

TEST(Result, OrElseConstLValueRefStatusInvokes) {
  const Result<int> r = Status::NotFound();
  auto ret = r.or_else(return_status);
  ASSERT_FALSE(ret.ok());
  EXPECT_EQ(ret.status(), Status::Unknown());
}

TEST(Result, OrElseConstLValueRefResultInvokes) {
  const Result<int> r = Status::NotFound();
  auto ret = r.or_else(return_result);
  ASSERT_FALSE(ret.ok());
  EXPECT_EQ(ret.status(), Status::Internal());
}

TEST(Result, OrElseConstLValueRefVoidSkips) {
  const Result<int> r = 32;
  bool invoked = false;
  auto ret = r.or_else([&invoked](Status) { invoked = true; });
  EXPECT_FALSE(invoked);
  ASSERT_TRUE(ret.ok());
  EXPECT_EQ(*ret, 32);
}

TEST(Result, OrElseConstLValueRefVoidInvokes) {
  const Result<int> r = Status::NotFound();
  bool invoked = false;
  auto ret = r.or_else([&invoked](Status) { invoked = true; });
  EXPECT_TRUE(invoked);
  ASSERT_FALSE(ret.ok());
  EXPECT_EQ(ret.status(), Status::NotFound());
}

TEST(Result, OrElseConstRValueRefSkips) {
  const Result<int> r = 32;
  auto ret = std::move(r).or_else(return_status);
  ASSERT_TRUE(ret.ok());
  EXPECT_EQ(*ret, 32);
}

TEST(Result, OrElseConstRValueRefStatusInvokes) {
  const Result<int> r = Status::NotFound();
  auto ret = std::move(r).or_else(return_status);
  ASSERT_FALSE(ret.ok());
  EXPECT_EQ(ret.status(), Status::Unknown());
}

TEST(Result, OrElseConstRValueRefResultInvokes) {
  const Result<int> r = Status::NotFound();
  auto ret = std::move(r).or_else(return_result);
  ASSERT_FALSE(ret.ok());
  EXPECT_EQ(ret.status(), Status::Internal());
}

TEST(Result, OrElseConstRValueRefVoidSkips) {
  const Result<int> r = 32;
  bool invoked = false;
  auto ret = std::move(r).or_else([&invoked](Status) { invoked = true; });
  EXPECT_FALSE(invoked);
  ASSERT_TRUE(ret.ok());
  EXPECT_EQ(*ret, 32);
}

TEST(Result, OrElseConstRValueRefVoidInvokes) {
  const Result<int> r = Status::NotFound();
  bool invoked = false;
  auto ret = std::move(r).or_else([&invoked](Status) { invoked = true; });
  EXPECT_TRUE(invoked);
  ASSERT_FALSE(ret.ok());
  EXPECT_EQ(ret.status(), Status::NotFound());
}

TEST(Result, OrElseMultipleChained) {
  Result<int> r = Status::NotFound();
  bool invoked = false;
  auto ret =
      r.or_else(return_result).or_else([&invoked](Status) { invoked = true; });
  EXPECT_TRUE(invoked);
  ASSERT_FALSE(ret.ok());
  EXPECT_EQ(ret.status(), Status::Internal());
}

auto multiply_int = [](int x) { return x * 2; };
auto add_two_int = [](int x) { return x + 2; };
auto make_value = [](int x) { return Value{.number = x}; };

TEST(Result, TransformNonConstLValueRefInvokeSuccess) {
  Result<int> r = 32;
  auto ret = r.transform(multiply_int);
  ASSERT_TRUE(ret.ok());
  EXPECT_EQ(*ret, 64);
}

TEST(Result, TransformNonConstLValueRefInvokeDifferentType) {
  Result<int> r = 32;
  auto ret = r.transform(make_value);
  ASSERT_TRUE(ret.ok());
  EXPECT_EQ(ret->number, 32);
}

TEST(Result, TransformNonConstLValueRefSkips) {
  Result<int> r = Status::NotFound();
  auto ret = r.transform(multiply_int);
  ASSERT_FALSE(ret.ok());
  EXPECT_EQ(ret.status(), Status::NotFound());
}

TEST(Result, TransformNonConstRValueRefInvokeSuccess) {
  Result<int> r = 32;
  auto ret = std::move(r).transform(multiply_int);
  ASSERT_TRUE(ret.ok());
  EXPECT_EQ(*ret, 64);
}

TEST(Result, TransformNonConstRValueRefInvokeDifferentType) {
  Result<int> r = 32;
  auto ret = std::move(r).transform(make_value);
  ASSERT_TRUE(ret.ok());
  EXPECT_EQ(ret->number, 32);
}

TEST(Result, TransformNonConstRValueRefSkips) {
  Result<int> r = Status::NotFound();
  auto ret = std::move(r).transform(multiply_int);
  ASSERT_FALSE(ret.ok());
  EXPECT_EQ(ret.status(), Status::NotFound());
}

TEST(Result, TransformConstLValueRefInvokeSuccess) {
  const Result<int> r = 32;
  auto ret = r.transform(multiply_int);
  ASSERT_TRUE(ret.ok());
  EXPECT_EQ(*ret, 64);
}

TEST(Result, TransformConstLValueRefInvokeDifferentType) {
  const Result<int> r = 32;
  auto ret = r.transform(make_value);
  ASSERT_TRUE(ret.ok());
  EXPECT_EQ(ret->number, 32);
}

TEST(Result, TransformConstLValueRefSkips) {
  const Result<int> r = Status::NotFound();
  auto ret = r.transform(multiply_int);
  ASSERT_FALSE(ret.ok());
  EXPECT_EQ(ret.status(), Status::NotFound());
}

TEST(Result, TransformConstRValueRefInvokeSuccess) {
  const Result<int> r = 32;
  auto ret = std::move(r).transform(multiply_int);
  ASSERT_TRUE(ret.ok());
  EXPECT_EQ(*ret, 64);
}

TEST(Result, TransformConstRValueRefInvokeDifferentType) {
  const Result<int> r = 32;
  auto ret = std::move(r).transform(make_value);
  ASSERT_TRUE(ret.ok());
  EXPECT_EQ(ret->number, 32);
}

TEST(Result, TransformConstRValueRefSkips) {
  const Result<int> r = Status::NotFound();
  auto ret = std::move(r).transform(multiply_int);
  ASSERT_FALSE(ret.ok());
  EXPECT_EQ(ret.status(), Status::NotFound());
}

TEST(Result, TransformMultipleChained) {
  Result<int> r = 32;
  auto ret = r.transform(multiply_int)
                 .transform(add_two_int)
                 .transform(multiply_int)
                 .transform(make_value);
  ASSERT_TRUE(ret.ok());
  EXPECT_EQ(ret->number, 132);
}

using ::pw::containers::test::Counter;
using ::pw::containers::test::MoveOnly;
using ::pw::containers::test::TrivialMoveOnly;

enum class HasValue : uint8_t {
  kNo = 0,
  kYes = 1,
  kDefinitelyNot = 3,
};

using HasValueStatus = StatusOf<HasValue::kYes>;

template <typename T>
using TestResult = Result<T, HasValueStatus>;

template <typename OptT>
struct OptionalTraits;

template <typename T>
struct OptionalTraits<std::optional<T>> {
  static std::optional<T> MakeEmpty() { return std::nullopt; }
  static std::optional<T> MakeValue(T v) {
    return std::optional<T>(std::move(v));
  }
  static bool HasValue(const std::optional<T>& opt) { return opt.has_value(); }
};

template <typename T>
struct OptionalTraits<TestResult<T>> {
  static constexpr TestResult<T> MakeEmpty() {
    return TestResult<T>(HasValue::kNo);
  }
  static constexpr TestResult<T> MakeValue(T v) {
    return TestResult<T>(std::move(v));
  }
  static constexpr bool HasValue(const TestResult<T>& opt) { return opt.ok(); }
};

using internal_result::test::ConvertibleFromInt;
using internal_result::test::Copyable;
using internal_result::test::CopyAssignNotCtor;
using internal_result::test::CopyCtorNotAssign;
using internal_result::test::ExplicitConstructibleFromInt;
using internal_result::test::MoveAssignNotCtor;
using internal_result::test::MoveCtorNotAssign;

template <typename OptT>
void TestCommonOptional() {
  using T = typename OptT::value_type;
  using Traits = OptionalTraits<OptT>;

  OptT empty = Traits::MakeEmpty();
  EXPECT_FALSE(Traits::HasValue(empty));

  OptT val = Traits::MakeValue(T(42));
  EXPECT_TRUE(Traits::HasValue(val));
  EXPECT_EQ(val.value().value, 42);
  EXPECT_EQ((*val).value, 42);
  EXPECT_EQ(val->value, 42);

  if constexpr (std::is_copy_constructible_v<T>) {
    OptT copy = val;
    EXPECT_TRUE(Traits::HasValue(copy));
    EXPECT_EQ(copy->value, 42);
  }

  OptT moved = std::move(val);
  EXPECT_TRUE(Traits::HasValue(moved));
  EXPECT_EQ(moved->value, 42);

  OptT emp = Traits::MakeEmpty();
  emp.emplace(100);
  EXPECT_TRUE(Traits::HasValue(emp));
  EXPECT_EQ(emp->value, 100);
}

TEST(ResultCustomStatus, Common_StdOptional_Copyable) {
  TestCommonOptional<std::optional<Copyable>>();
}

TEST(ResultCustomStatus, Common_PwResult_Copyable) {
  TestCommonOptional<TestResult<Copyable>>();
}

TEST(ResultCustomStatus, Common_StdOptional_MoveOnly) {
  TestCommonOptional<std::optional<MoveOnly>>();
}

TEST(ResultCustomStatus, Common_PwResult_MoveOnly) {
  TestCommonOptional<TestResult<MoveOnly>>();
}

TEST(ResultCustomStatus, Common_StdOptional_CopyCtorNotAssign) {
  TestCommonOptional<std::optional<CopyCtorNotAssign>>();
}

TEST(ResultCustomStatus, Common_PwResult_CopyCtorNotAssign) {
  TestCommonOptional<TestResult<CopyCtorNotAssign>>();
}

TEST(ResultCustomStatus, Common_StdOptional_MoveCtorNotAssign) {
  TestCommonOptional<std::optional<MoveCtorNotAssign>>();
}

TEST(ResultCustomStatus, Common_PwResult_MoveCtorNotAssign) {
  TestCommonOptional<TestResult<MoveCtorNotAssign>>();
}

TEST(ResultCustomStatus, Traits_CopyCtorNotAssign) {
  using Opt = TestResult<CopyCtorNotAssign>;
  static_assert(std::is_copy_constructible_v<Opt>);
  static_assert(std::is_move_constructible_v<Opt>);
  static_assert(!std::is_copy_assignable_v<Opt>);
  static_assert(!std::is_move_assignable_v<Opt>);
}

TEST(ResultCustomStatus, Traits_MoveCtorNotAssign) {
  using Opt = TestResult<MoveCtorNotAssign>;
  static_assert(!std::is_copy_constructible_v<Opt>);
  static_assert(std::is_move_constructible_v<Opt>);
  static_assert(!std::is_copy_assignable_v<Opt>);
  static_assert(!std::is_move_assignable_v<Opt>);
}

TEST(ResultCustomStatus, Traits_CopyAssignNotCtor) {
  using Opt = TestResult<CopyAssignNotCtor>;
  static_assert(!std::is_copy_constructible_v<Opt>);
  static_assert(std::is_move_constructible_v<Opt>);
  static_assert(!std::is_copy_assignable_v<Opt>);
  static_assert(std::is_move_assignable_v<Opt>);
}

TEST(ResultCustomStatus, Traits_MoveAssignNotCtor) {
  using Opt = TestResult<MoveAssignNotCtor>;
  static_assert(!std::is_copy_constructible_v<Opt>);
  static_assert(!std::is_move_constructible_v<Opt>);
  static_assert(!std::is_copy_assignable_v<Opt>);
  static_assert(!std::is_move_assignable_v<Opt>);
}

PW_CONSTEXPR_TEST(ResultCustomStatus, ImplicitConversion, {
  TestResult<ConvertibleFromInt> opt = ConvertibleFromInt(123);
  PW_TEST_EXPECT_TRUE(opt.ok());
  PW_TEST_EXPECT_EQ(opt->value, 123);
});

PW_CONSTEXPR_TEST(ResultCustomStatus, ExplicitConstruction, {
  TestResult<ExplicitConstructibleFromInt> opt(std::in_place, 123);
  PW_TEST_EXPECT_TRUE(opt.ok());
  PW_TEST_EXPECT_EQ(opt->value, 123);
});

TEST(ResultCustomStatus, Reset) {
  TestResult<Counter> opt(std::in_place, 5);
  EXPECT_TRUE(opt.ok());
  opt.reset(HasValue::kDefinitelyNot);
  EXPECT_FALSE(opt.ok());
  EXPECT_EQ(opt.status().code(), HasValue::kDefinitelyNot);
}

PW_CONSTEXPR_TEST(ResultCustomStatus, ResetTemplate, {
  TestResult<int> opt(42);
  PW_TEST_EXPECT_TRUE(opt.ok());
  opt.reset<HasValue::kDefinitelyNot>();
  PW_TEST_EXPECT_FALSE(opt.ok());
  PW_TEST_EXPECT_EQ(opt.status().code(), HasValue::kDefinitelyNot);
});

TEST(ResultCustomStatus, StatusAccess) {
  TestResult<Counter> opt(HasValue::kNo);
  EXPECT_EQ(opt.status().code(), HasValue::kNo);
  opt.reset(HasValue::kDefinitelyNot);
  EXPECT_EQ(opt.status().code(), HasValue::kDefinitelyNot);
  opt = Counter(3);
  EXPECT_EQ(opt.status().code(), HasValue::kYes);
}

PW_CONSTEXPR_TEST(ResultCustomStatus, CopyAssignment, {
  TestResult<Copyable> opt1 = Copyable(1);
  TestResult<Copyable> opt2 = Copyable(2);
  opt1 = opt2;
  PW_TEST_EXPECT_EQ(opt1->value, 2);
});

PW_CONSTEXPR_TEST(ResultCustomStatus, MoveAssignment, {
  TestResult<TrivialMoveOnly> mv1(std::in_place, 1);
  TestResult<TrivialMoveOnly> mv2(std::in_place, 2);
  mv1 = std::move(mv2);
  PW_TEST_EXPECT_EQ(mv1->value, 2);
});

TEST(ResultCustomStatus, ValueCopyAssignment) {
  TestResult<Copyable> opt(HasValue::kNo);
  Copyable val(123);
  opt = val;
  EXPECT_EQ(opt->value, 123);
}

TEST(ResultCustomStatus, ValueMoveAssignment) {
  TestResult<MoveOnly> mv(HasValue::kNo);
  mv = MoveOnly(456);
  EXPECT_EQ(mv->value, 456);
}

TEST(ResultCustomStatus, Destructor_CalledOnReset) {
  Counter::Reset();

  TestResult<Counter> opt(std::in_place);
  opt.reset(HasValue::kNo);
  EXPECT_EQ(Counter::created, 1);
  EXPECT_EQ(Counter::destroyed, 1);
}

TEST(ResultCustomStatus, Destructor_CalledOnScopeExit) {
  Counter::Reset();
  {
    TestResult<Counter> opt(std::in_place);
  }
  EXPECT_EQ(Counter::created, 1);
  EXPECT_EQ(Counter::destroyed, 1);
}

TEST(ResultCustomStatus, Destructor_CalledOnAssignState) {
  Counter::Reset();
  {
    TestResult<Counter> opt(std::in_place);
    opt = TestResult<Counter>(HasValue::kNo);
  }
  EXPECT_EQ(Counter::created, 1);
  EXPECT_EQ(Counter::destroyed, 1);
}

PW_CONSTEXPR_TEST(ResultCustomStatus, ConvertingConstructor_Implicit, {
  TestResult<long> opt = 42;  // int to long
  PW_TEST_EXPECT_TRUE(opt.ok());
  PW_TEST_EXPECT_EQ(*opt, 42);
});

TEST(ResultCustomStatus, ConvertingConstructor_FromResult) {
  TestResult<int> source(42);
  TestResult<long> dest = source;
  EXPECT_TRUE(dest.ok());
  EXPECT_EQ(*dest, 42);
}

TEST(ResultCustomStatus, ConvertingConstructor_FromMoveResult) {
  TestResult<int> source(42);
  TestResult<long> dest = std::move(source);
  EXPECT_TRUE(dest.ok());
  EXPECT_EQ(*dest, 42);
}

struct Base {
  virtual ~Base() = default;
  int x = 1;
};
struct Derived : Base {
  Derived() { x = 2; }
};

TEST(ResultCustomStatus, ConvertingConstructor_BaseDerived) {
  TestResult<Derived> derived(std::in_place);
  TestResult<Base> base = derived;
  EXPECT_TRUE(base.ok());
  EXPECT_EQ(base->x, 2);
}

PW_CONSTEXPR_TEST(ResultCustomStatus, RefQualifiedAccessors, {
  class RefTester {
   public:
    constexpr int get() & { return 1; }
    constexpr int get() const& { return 2; }

    constexpr int get() && { return -1; }
    constexpr int get() const&& { return -2; }
  };

  TestResult<RefTester> opt(std::in_place);

  // &
  PW_TEST_EXPECT_EQ(opt.value().get(), 1);
  PW_TEST_EXPECT_EQ((*opt).get(), 1);

  // const &
  const auto& c_opt = opt;
  PW_TEST_EXPECT_EQ(c_opt.value().get(), 2);
  PW_TEST_EXPECT_EQ((*c_opt).get(), 2);

  // &&
  PW_TEST_EXPECT_EQ(std::move(opt).value().get(), -1);
  PW_TEST_EXPECT_EQ((*TestResult<RefTester>(std::in_place)).get(), -1);

  // const &&
  const auto& c_opt_ref1 = c_opt;
  PW_TEST_EXPECT_EQ(std::move(c_opt_ref1).value().get(), -2);
  const auto& c_opt_ref2 = c_opt;
  PW_TEST_EXPECT_EQ((*std::move(c_opt_ref2)).get(), -2);
});

PW_CONSTEXPR_TEST(ResultCustomStatus, RefQualifiedAccessors_MoveOnly, {
  TestResult<TrivialMoveOnly> opt(std::in_place, 123);

  TrivialMoveOnly m = std::move(opt).value();
  PW_TEST_EXPECT_EQ(m.value, 123);
});

TEST(ResultCustomStatus, PlacementNew_EmplaceArgs) {
  struct MultiArg {
    MultiArg(int a, int b) : sum(a + b) {}
    int sum;
  };
  TestResult<MultiArg> opt(HasValue::kNo);
  opt.emplace(10, 20);
  EXPECT_TRUE(opt.ok());
  EXPECT_EQ(opt->sum, 30);
}

namespace driver_test {

constexpr bool BusReady() { return true; }
constexpr uint8_t ReadRawRegister() { return 0x42; }

// DOCSTAG: [pw_result-custom-status]
enum class DriverError : uint8_t {
  kOk,
  kBusFault,
  kTimeout,
};

using DriverStatus = pw::StatusOf<DriverError::kOk>;

constexpr pw::Result<uint8_t, DriverStatus> ReadRegister() {
  if (!BusReady()) {
    return DriverStatus(DriverError::kBusFault);
  }
  return ReadRawRegister();
}
// DOCSTAG: [pw_result-custom-status]

}  // namespace driver_test

static_assert(!std::is_convertible_v<driver_test::DriverError,
                                     Result<int, driver_test::DriverStatus>>);

PW_CONSTEXPR_TEST(ResultDriverStatus, BasicOperations, {
  using driver_test::DriverError;
  using driver_test::DriverStatus;

  PW_TEST_EXPECT_EQ(driver_test::ReadRegister(), 0x42);

  Result<int, DriverStatus> ok_res(42);
  PW_TEST_EXPECT_TRUE(ok_res.ok());
  PW_TEST_EXPECT_EQ(*ok_res, 42);

  Result<int, DriverStatus> err_res(DriverError::kBusFault);
  PW_TEST_EXPECT_FALSE(err_res.ok());
  PW_TEST_EXPECT_EQ(err_res.status(), DriverError::kBusFault);
  PW_TEST_EXPECT_EQ(err_res.status().code(), DriverError::kBusFault);
});

PW_CONSTEXPR_TEST(ResultDriverStatus, PwTry, {
  using driver_test::DriverError;
  using driver_test::DriverStatus;

  constexpr auto TryFunc = [](bool fail) -> Result<int, DriverStatus> {
    auto helper = [](bool f) -> Result<int, DriverStatus> {
      if (f) {
        return DriverStatus(DriverError::kTimeout);
      }
      return 100;
    };
    PW_TRY_ASSIGN(int val, helper(fail));
    return val + 1;
  };

  auto ok = TryFunc(false);
  PW_TEST_EXPECT_OK(ok);
  PW_TEST_EXPECT_TRUE(ok.ok());
  PW_TEST_EXPECT_EQ(*ok, 101);

  PW_TEST_ASSERT_OK_AND_ASSIGN(int assigned, TryFunc(false));
  PW_TEST_EXPECT_EQ(assigned, 101);

  auto err = TryFunc(true);
  PW_TEST_EXPECT_FALSE(err.ok());
  PW_TEST_EXPECT_EQ(err.status(), DriverError::kTimeout);
});

// DOCSTAG: [pw_result-void]
constexpr pw::Result<void> DoOperation(bool succeed) {
  if (!succeed) {
    return pw::Status::Unavailable();  // ok() == false
  }
  return pw::OkStatus();  // ok() == true (or pw::Result<void>(std::in_place))
}
// DOCSTAG: [pw_result-void]

PW_CONSTEXPR_TEST(ResultCustomStatus, Void_DefaultConstruction, {
  Result<void> default_void;
  PW_TEST_EXPECT_FALSE(default_void.ok());
  PW_TEST_EXPECT_EQ(default_void.status(), Status::Unknown());

  TestResult<void> opt(HasValue::kNo);
  PW_TEST_EXPECT_FALSE(opt.ok());
  PW_TEST_EXPECT_EQ(opt.status().code(), HasValue::kNo);
});

PW_CONSTEXPR_TEST(ResultCustomStatus, Void_InPlaceConstruction, {
  TestResult<void> opt(std::in_place);
  PW_TEST_EXPECT_TRUE(opt.ok());
  PW_TEST_EXPECT_EQ(opt.status().code(), HasValue::kYes);

  Result<void> ok_void = OkStatus();
  PW_TEST_EXPECT_TRUE(ok_void.ok());
  PW_TEST_EXPECT_EQ(ok_void.status(), OkStatus());

  PW_TEST_EXPECT_TRUE(DoOperation(true).ok());
  PW_TEST_EXPECT_EQ(DoOperation(false), Status::Unavailable());
});

TEST(ResultCustomStatus, OrElseRValueRefVoidMoveOnly) {
  TestResult<MoveOnly> r(std::in_place, 32);
  bool invoked = false;
  TestResult<MoveOnly> ret =
      std::move(r).or_else([&invoked](HasValueStatus) { invoked = true; });
  EXPECT_FALSE(invoked);
  ASSERT_TRUE(ret.ok());
  EXPECT_EQ(ret->value, 32);
}

TEST(ResultCustomStatus, Void_ExpectEq) {
  // EXPECT_EQ requires pw::ToString support for Result<void>.
  Result<void> ok_void = OkStatus();
  Result<void> err_void = Status::NotFound();
  EXPECT_EQ(ok_void, Result<void>(OkStatus()));
  EXPECT_NE(ok_void, err_void);
  EXPECT_EQ(err_void, Result<void>(Status::NotFound()));
}

PW_CONSTEXPR_TEST(ResultCustomStatus, Void_Emplace, {
  TestResult<void> opt(HasValue::kNo);
  opt.emplace();
  PW_TEST_EXPECT_TRUE(opt.ok());
});

PW_CONSTEXPR_TEST(ResultCustomStatus, Void_Reset, {
  TestResult<void> opt(std::in_place);
  opt.reset(HasValue::kNo);
  PW_TEST_EXPECT_FALSE(opt.ok());

  opt.emplace();
  opt.reset<HasValue::kDefinitelyNot>();
  PW_TEST_EXPECT_FALSE(opt.ok());
  PW_TEST_EXPECT_EQ(opt.status().code(), HasValue::kDefinitelyNot);
});

PW_CONSTEXPR_TEST(ResultCustomStatus, Void_Value, {
  TestResult<void> opt(std::in_place);
  opt.value();  // Should not crash
});

PW_CONSTEXPR_TEST(ResultCustomStatus, Void_CopyAssignment, {
  TestResult<void> opt1(std::in_place);
  TestResult<void> opt2(HasValue::kNo);
  opt2 = opt1;
  PW_TEST_EXPECT_TRUE(opt2.ok());
});

PW_CONSTEXPR_TEST(ResultCustomStatus, Void_MoveAssignment, {
  TestResult<void> opt1(std::in_place);
  TestResult<void> opt2(HasValue::kNo);
  opt2 = std::move(opt1);
  PW_TEST_EXPECT_TRUE(opt2.ok());
});

PW_CONSTEXPR_TEST(ResultCustomStatus, Void_Monadic, {
  Result<void> ok_void = OkStatus();
  Result<int> transformed = ok_void.transform([]() { return 42; });
  PW_TEST_EXPECT_TRUE(transformed.ok());
  PW_TEST_EXPECT_EQ(*transformed, 42);

  Result<void> chained_void = ok_void.transform([]() {});
  PW_TEST_EXPECT_TRUE(chained_void.ok());

  Result<int> and_then_val =
      ok_void.and_then([]() -> Result<int> { return 99; });
  PW_TEST_EXPECT_TRUE(and_then_val.ok());
  PW_TEST_EXPECT_EQ(*and_then_val, 99);

  Result<void> err_void = Status::NotFound();
  Result<void> recovered = err_void.or_else([](Status s) -> Result<void> {
    if (s.IsNotFound()) {
      return OkStatus();
    }
    return s;
  });
  PW_TEST_EXPECT_TRUE(recovered.ok());

  Result<int> err_transformed = err_void.transform([]() { return 42; });
  PW_TEST_EXPECT_FALSE(err_transformed.ok());
  PW_TEST_EXPECT_EQ(err_transformed.status(), Status::NotFound());

  Result<int> err_and_then =
      err_void.and_then([]() -> Result<int> { return 99; });
  PW_TEST_EXPECT_FALSE(err_and_then.ok());
  PW_TEST_EXPECT_EQ(err_and_then.status(), Status::NotFound());
});

PW_CONSTEXPR_TEST(ResultCustomStatus, Void_OrElseRValueVoidCallback, {
  bool invoked = false;
  Result<void> ok_ret =
      Result<void>(OkStatus()).or_else([&invoked](Status) { invoked = true; });
  PW_TEST_EXPECT_FALSE(invoked);
  PW_TEST_EXPECT_TRUE(ok_ret.ok());

  Result<void> err_ret =
      Result<void>(Status::NotFound()).or_else([&invoked](Status) {
        invoked = true;
      });
  PW_TEST_EXPECT_TRUE(invoked);
  PW_TEST_EXPECT_FALSE(err_ret.ok());
  PW_TEST_EXPECT_EQ(err_ret.status(), Status::NotFound());
});

PW_CONSTEXPR_TEST(ResultCustomStatus, Void_OrElseConstRefVoidCallback, {
  const Result<void> err(Status::NotFound());
  bool invoked = false;
  Result<void> ret = err.or_else([&invoked](Status) { invoked = true; });
  PW_TEST_EXPECT_TRUE(invoked);
  PW_TEST_EXPECT_FALSE(ret.ok());
  PW_TEST_EXPECT_EQ(ret.status(), Status::NotFound());
});

PW_CONSTEXPR_TEST(ResultCustomStatus, Equality_ResultVoid, {
  PW_TEST_EXPECT_EQ(TestResult<void>(HasValue::kNo),
                    TestResult<void>(HasValue::kNo));
  PW_TEST_EXPECT_NE(TestResult<void>(HasValue::kNo),
                    TestResult<void>(HasValue::kDefinitelyNot));
  PW_TEST_EXPECT_EQ(TestResult<void>(std::in_place),
                    TestResult<void>(std::in_place));
  PW_TEST_EXPECT_NE(TestResult<void>(std::in_place),
                    TestResult<void>(HasValue::kNo));
});

PW_CONSTEXPR_TEST(ResultCustomStatus, Equality_ResultVoidStatus, {
  const Result<void> ok_void = OkStatus();
  const Result<void> err_void = Status::NotFound();

  PW_TEST_EXPECT_TRUE(ok_void == OkStatus());
  PW_TEST_EXPECT_TRUE(OkStatus() == ok_void);
  PW_TEST_EXPECT_FALSE(ok_void != OkStatus());
  PW_TEST_EXPECT_FALSE(OkStatus() != ok_void);
  PW_TEST_EXPECT_TRUE(ok_void != Status::NotFound());
  PW_TEST_EXPECT_TRUE(Status::NotFound() != ok_void);

  PW_TEST_EXPECT_TRUE(err_void == Status::NotFound());
  PW_TEST_EXPECT_TRUE(Status::NotFound() == err_void);
  PW_TEST_EXPECT_TRUE(err_void != OkStatus());
  PW_TEST_EXPECT_TRUE(OkStatus() != err_void);
  PW_TEST_EXPECT_TRUE(err_void != Status::Internal());

  // Status codes.
  PW_TEST_EXPECT_TRUE(ok_void == PW_STATUS_OK);
  PW_TEST_EXPECT_TRUE(PW_STATUS_OK == ok_void);
  PW_TEST_EXPECT_TRUE(err_void == PW_STATUS_NOT_FOUND);
  PW_TEST_EXPECT_TRUE(PW_STATUS_NOT_FOUND == err_void);
  PW_TEST_EXPECT_TRUE(err_void != PW_STATUS_OK);
  PW_TEST_EXPECT_TRUE(PW_STATUS_OK != err_void);

  // Custom status types.
  const TestResult<void> custom_ok(std::in_place);
  const TestResult<void> custom_err(HasValue::kNo);
  PW_TEST_EXPECT_TRUE(custom_ok == HasValueStatus());
  PW_TEST_EXPECT_TRUE(custom_ok == HasValue::kYes);
  PW_TEST_EXPECT_TRUE(custom_ok != HasValue::kNo);
  PW_TEST_EXPECT_TRUE(custom_err == HasValueStatus(HasValue::kNo));
  PW_TEST_EXPECT_TRUE(custom_err == HasValue::kNo);
  PW_TEST_EXPECT_TRUE(HasValue::kNo == custom_err);
  PW_TEST_EXPECT_TRUE(custom_err != HasValue::kDefinitelyNot);
});

PW_CONSTEXPR_TEST(ResultCustomStatus, Equality_ResultResult, {
  PW_TEST_EXPECT_EQ(TestResult<int>(HasValue::kNo),
                    TestResult<int>(HasValue::kNo));
  PW_TEST_EXPECT_NE(TestResult<int>(HasValue::kNo),
                    TestResult<int>(HasValue::kDefinitelyNot));
  PW_TEST_EXPECT_NE(TestResult<int>(0), TestResult<int>(HasValue::kNo));
  PW_TEST_EXPECT_NE(TestResult<int>(HasValue::kNo), TestResult<int>(0));

  PW_TEST_EXPECT_EQ(TestResult<int>(1), TestResult<int>(1));
  PW_TEST_EXPECT_NE(TestResult<int>(2), TestResult<int>(1));
});

PW_CONSTEXPR_TEST(ResultCustomStatus, Equality_ResultStatus, {
  const Result<int> value(5);
  const Result<int> error(Status::NotFound());

  // A Result holding a value compares equal to OK. This must not construct a
  // Result<int> from OkStatus(), which would assert.
  PW_TEST_EXPECT_TRUE(value == OkStatus());
  PW_TEST_EXPECT_TRUE(OkStatus() == value);
  PW_TEST_EXPECT_FALSE(value != OkStatus());
  PW_TEST_EXPECT_FALSE(OkStatus() != value);
  PW_TEST_EXPECT_FALSE(value == Status::NotFound());
  PW_TEST_EXPECT_FALSE(Status::NotFound() == value);
  PW_TEST_EXPECT_TRUE(value != Status::NotFound());
  PW_TEST_EXPECT_TRUE(Status::NotFound() != value);

  PW_TEST_EXPECT_TRUE(error == Status::NotFound());
  PW_TEST_EXPECT_TRUE(Status::NotFound() == error);
  PW_TEST_EXPECT_FALSE(error != Status::NotFound());
  PW_TEST_EXPECT_FALSE(error == OkStatus());
  PW_TEST_EXPECT_TRUE(error != OkStatus());
  PW_TEST_EXPECT_TRUE(OkStatus() != error);
  PW_TEST_EXPECT_TRUE(error != Status::Internal());

  // Status codes.
  PW_TEST_EXPECT_TRUE(value == PW_STATUS_OK);
  PW_TEST_EXPECT_TRUE(PW_STATUS_OK == value);
  PW_TEST_EXPECT_TRUE(value != PW_STATUS_NOT_FOUND);
  PW_TEST_EXPECT_TRUE(error == PW_STATUS_NOT_FOUND);
  PW_TEST_EXPECT_TRUE(PW_STATUS_NOT_FOUND == error);
  PW_TEST_EXPECT_TRUE(error != PW_STATUS_OK);
  PW_TEST_EXPECT_TRUE(PW_STATUS_OK != error);

  // Comparing against a status code never compares against the value, even if
  // T is comparable with the code's underlying type.
  const Result<int> five(5);
  PW_TEST_EXPECT_TRUE(five != static_cast<pw_Status>(5));
  PW_TEST_EXPECT_TRUE(five == PW_STATUS_OK);
});

PW_CONSTEXPR_TEST(ResultCustomStatus, Equality_CustomResultStatus, {
  const TestResult<int> value(1);
  const TestResult<int> error(HasValue::kNo);

  PW_TEST_EXPECT_TRUE(value == HasValueStatus());
  PW_TEST_EXPECT_TRUE(HasValueStatus() == value);
  PW_TEST_EXPECT_TRUE(value != HasValueStatus(HasValue::kNo));
  PW_TEST_EXPECT_TRUE(value == HasValue::kYes);
  PW_TEST_EXPECT_TRUE(HasValue::kYes == value);
  PW_TEST_EXPECT_TRUE(value != HasValue::kNo);
  PW_TEST_EXPECT_TRUE(HasValue::kNo != value);

  PW_TEST_EXPECT_TRUE(error == HasValueStatus(HasValue::kNo));
  PW_TEST_EXPECT_TRUE(HasValueStatus(HasValue::kNo) == error);
  PW_TEST_EXPECT_TRUE(error != HasValueStatus());
  PW_TEST_EXPECT_TRUE(error == HasValue::kNo);
  PW_TEST_EXPECT_TRUE(HasValue::kNo == error);
  PW_TEST_EXPECT_TRUE(error != HasValue::kDefinitelyNot);
  PW_TEST_EXPECT_TRUE(HasValue::kDefinitelyNot != error);

  // Status codes always compare against the status, never the value.
  PW_TEST_EXPECT_TRUE(TestResult<int>(1) == HasValue::kYes);
  PW_TEST_EXPECT_TRUE(TestResult<int>(0) != HasValue::kNo);
});

PW_CONSTEXPR_TEST(ResultCustomStatus, Equality_ResultValue, {
  PW_TEST_EXPECT_NE(TestResult<int>(HasValue::kNo), TestResult<int>(1));
  PW_TEST_EXPECT_NE(TestResult<int>(1), TestResult<int>(HasValue::kNo));

  PW_TEST_EXPECT_EQ(TestResult<int>(1), 1);
  PW_TEST_EXPECT_NE(TestResult<int>(1), 2);

  PW_TEST_EXPECT_EQ(2, TestResult<int>(2));
  PW_TEST_EXPECT_NE(2, TestResult<int>(1));

  // A Result without a value never equals a value.
  PW_TEST_EXPECT_FALSE(TestResult<int>(HasValue::kNo) == 0);
  PW_TEST_EXPECT_TRUE(TestResult<int>(HasValue::kNo) != 0);
  PW_TEST_EXPECT_FALSE(0 == TestResult<int>(HasValue::kNo));
  PW_TEST_EXPECT_TRUE(0 != TestResult<int>(HasValue::kNo));
  PW_TEST_EXPECT_FALSE(Result<int>(Status::NotFound()) == 0);
  PW_TEST_EXPECT_TRUE(Result<int>(Status::NotFound()) != 0);
});

PW_CONSTEXPR_TEST(ResultCustomStatus, Equality_ResultHeterogeneousValue, {
  // U need not be T; it only needs to be equality comparable with T.
  const Result<long> long_result(42L);
  PW_TEST_EXPECT_TRUE(long_result == 42);
  PW_TEST_EXPECT_TRUE(42 == long_result);
  PW_TEST_EXPECT_TRUE(long_result != 43);
  PW_TEST_EXPECT_TRUE(43 != long_result);

  const Result<std::string_view> sv_result("foo");
  PW_TEST_EXPECT_TRUE(sv_result == "foo");
  PW_TEST_EXPECT_TRUE("foo" == sv_result);
  PW_TEST_EXPECT_TRUE(sv_result != "bar");
  PW_TEST_EXPECT_TRUE("bar" != sv_result);
  PW_TEST_EXPECT_TRUE(sv_result == std::string_view("foo"));

  const Result<std::string_view> sv_error(Status::NotFound());
  PW_TEST_EXPECT_FALSE(sv_error == "foo");
  PW_TEST_EXPECT_TRUE(sv_error != "foo");

  const Result<Copyable> copyable(std::in_place, 7);
  PW_TEST_EXPECT_TRUE(copyable == Copyable(7));
  PW_TEST_EXPECT_TRUE(Copyable(7) == copyable);
  PW_TEST_EXPECT_TRUE(copyable != Copyable(8));
});

TEST(ResultCustomStatus, Equality_PointerValue) {
  int x = 0;
  const Result<int*> null_result(nullptr);
  EXPECT_TRUE(null_result == nullptr);
  EXPECT_TRUE(nullptr == null_result);
  EXPECT_TRUE(null_result != &x);

  const Result<int*> ptr_result(&x);
  EXPECT_TRUE(ptr_result == &x);
  EXPECT_TRUE(&x == ptr_result);
  EXPECT_TRUE(ptr_result != nullptr);
  EXPECT_TRUE(nullptr != ptr_result);

  const Result<int*> error(Status::NotFound());
  EXPECT_FALSE(error == nullptr);
  EXPECT_TRUE(error != nullptr);
}

TEST(ResultCustomStatus, Equality_String) {
  const Result<std::string> str_result("foo");
  EXPECT_TRUE(str_result == "foo");
  EXPECT_TRUE("foo" == str_result);
  EXPECT_TRUE(str_result == std::string("foo"));
  EXPECT_TRUE(str_result == std::string_view("foo"));
  EXPECT_TRUE(str_result != "bar");
  EXPECT_TRUE(str_result != std::string("bar"));

  const Result<std::string> str_error(Status::NotFound());
  EXPECT_FALSE(str_error == "foo");
  EXPECT_TRUE(str_error != "foo");
  EXPECT_FALSE(str_error == std::string());
}

template <typename A, typename B, typename = void>
struct CanCompare : std::false_type {};

template <typename A, typename B>
struct CanCompare<
    A,
    B,
    std::void_t<decltype(std::declval<const A&>() == std::declval<const B&>()),
                decltype(std::declval<const A&>() != std::declval<const B&>())>>
    : std::true_type {};

template <typename A, typename B>
inline constexpr bool kCanCompare = CanCompare<A, B>::value;

// Comparisons are only enabled when T is comparable with U, or U is a Result,
// status, or status code.
static_assert(kCanCompare<Result<int>, Result<int>>);
static_assert(kCanCompare<Result<int>, int>);
static_assert(kCanCompare<int, Result<int>>);
static_assert(kCanCompare<Result<int>, long>);
static_assert(kCanCompare<Result<int>, Status>);
static_assert(kCanCompare<Status, Result<int>>);
static_assert(kCanCompare<Result<int>, pw_Status>);
static_assert(kCanCompare<pw_Status, Result<int>>);
static_assert(kCanCompare<Result<std::string_view>, const char*>);
static_assert(kCanCompare<Result<std::string_view>, char[4]>);
static_assert(kCanCompare<Result<int*>, std::nullptr_t>);

static_assert(!kCanCompare<Result<int>, std::string_view>);
static_assert(!kCanCompare<std::string_view, Result<int>>);
static_assert(!kCanCompare<Result<int>, std::nullptr_t>);
static_assert(!kCanCompare<Result<int>, HasValueStatus>);
static_assert(!kCanCompare<Result<int>, HasValue>);
static_assert(!kCanCompare<Result<void>, int>);
static_assert(!kCanCompare<Result<void>, std::nullptr_t>);

static_assert(kCanCompare<TestResult<int>, TestResult<int>>);
static_assert(kCanCompare<TestResult<int>, int>);
static_assert(kCanCompare<TestResult<int>, HasValueStatus>);
static_assert(kCanCompare<HasValueStatus, TestResult<int>>);
static_assert(kCanCompare<TestResult<int>, HasValue>);
static_assert(kCanCompare<HasValue, TestResult<int>>);
static_assert(!kCanCompare<TestResult<int>, Status>);
static_assert(!kCanCompare<TestResult<int>, Result<int>>);
static_assert(!kCanCompare<Result<int>, TestResult<int>>);

static_assert(kCanCompare<Result<void>, Result<void>>);
static_assert(kCanCompare<Result<void>, Status>);
static_assert(kCanCompare<Status, Result<void>>);
static_assert(kCanCompare<Result<void>, pw_Status>);
static_assert(kCanCompare<TestResult<void>, HasValueStatus>);
static_assert(kCanCompare<TestResult<void>, HasValue>);
static_assert(!kCanCompare<TestResult<void>, Status>);
static_assert(!kCanCompare<Result<void>, Result<int>>);

struct NotComparable {
  int value;
};

static_assert(!kCanCompare<Result<NotComparable>, Result<NotComparable>>);
static_assert(!kCanCompare<Result<NotComparable>, NotComparable>);
static_assert(!kCanCompare<NotComparable, Result<NotComparable>>);
static_assert(!kCanCompare<Result<NotComparable>, int>);
static_assert(kCanCompare<Result<NotComparable>, Status>);
static_assert(kCanCompare<Result<NotComparable>, pw_Status>);

PW_CONSTEXPR_TEST(ResultCustomStatus, Equality_NotComparableValueType, {
  // Status comparisons work even when T is not equality comparable.
  const Result<NotComparable> value(std::in_place, NotComparable{1});
  const Result<NotComparable> error(Status::NotFound());
  PW_TEST_EXPECT_TRUE(value == OkStatus());
  PW_TEST_EXPECT_TRUE(value != Status::NotFound());
  PW_TEST_EXPECT_TRUE(error == Status::NotFound());
  PW_TEST_EXPECT_TRUE(error == PW_STATUS_NOT_FOUND);
});

PW_CONSTEXPR_TEST(ResultDriverStatus, PwTryStatusReturn, {
  using driver_test::DriverError;
  using driver_test::DriverStatus;

  constexpr auto helper = [](bool fail) -> Result<int, DriverStatus> {
    if (fail) {
      return DriverStatus(DriverError::kTimeout);
    }
    return 100;
  };

  // PW_TRY in a function returning the custom status type.
  constexpr auto TryStatus = [helper](bool fail) -> DriverStatus {
    PW_TRY(helper(fail));
    return DriverStatus();
  };
  PW_TEST_EXPECT_TRUE(TryStatus(false).ok());
  PW_TEST_EXPECT_EQ(TryStatus(true), DriverError::kTimeout);

  // PW_TRY in a function returning a Result<T> with the custom status type.
  constexpr auto TryResult = [helper](bool fail) -> Result<bool, DriverStatus> {
    PW_TRY(helper(fail));
    return true;
  };
  PW_TEST_EXPECT_TRUE(TryResult(false).ok());
  PW_TEST_EXPECT_EQ(TryResult(true).status(), DriverError::kTimeout);

  // PW_TRY in a function returning a Result<void> with the custom status type.
  constexpr auto TryVoid = [helper](bool fail) -> Result<void, DriverStatus> {
    PW_TRY(helper(fail));
    return DriverStatus();
  };
  PW_TEST_EXPECT_TRUE(TryVoid(false).ok());
  PW_TEST_EXPECT_EQ(TryVoid(true).status(), DriverError::kTimeout);

  // PW_TRY_ASSIGN in a function returning the custom status type.
  constexpr auto TryAssignStatus = [helper](bool fail) -> DriverStatus {
    PW_TRY_ASSIGN(int value, helper(fail));
    return value == 100 ? DriverStatus() : DriverStatus(DriverError::kBusFault);
  };
  PW_TEST_EXPECT_TRUE(TryAssignStatus(false).ok());
  PW_TEST_EXPECT_EQ(TryAssignStatus(true), DriverError::kTimeout);
});

PW_CONSTEXPR_TEST(ResultCustomStatus, Void_PwTry, {
  constexpr auto helper = [](bool fail) -> Result<void> {
    if (fail) {
      return Status::Unavailable();
    }
    return OkStatus();
  };

  constexpr auto TryStatus = [helper](bool fail) -> Status {
    PW_TRY(helper(fail));
    return OkStatus();
  };
  PW_TEST_EXPECT_EQ(TryStatus(false), OkStatus());
  PW_TEST_EXPECT_EQ(TryStatus(true), Status::Unavailable());

  constexpr auto TryResult = [helper](bool fail) -> Result<int> {
    PW_TRY(helper(fail));
    return 7;
  };
  PW_TEST_EXPECT_EQ(TryResult(false), 7);
  PW_TEST_EXPECT_EQ(TryResult(true), Status::Unavailable());

  constexpr auto TryVoid = [helper](bool fail) -> Result<void> {
    PW_TRY(helper(fail));
    return OkStatus();
  };
  PW_TEST_EXPECT_TRUE(TryVoid(false).ok());
  PW_TEST_EXPECT_EQ(TryVoid(true), Status::Unavailable());

  constexpr auto TryCustom = [](bool fail) -> TestResult<int> {
    auto void_helper = [](bool f) -> TestResult<void> {
      return f ? TestResult<void>(HasValue::kNo)
               : TestResult<void>(std::in_place);
    };
    PW_TRY(void_helper(fail));
    return 3;
  };
  PW_TEST_EXPECT_EQ(TryCustom(false), 3);
  PW_TEST_EXPECT_EQ(TryCustom(true), HasValue::kNo);
});

TEST(ResultCustomStatus, Void_AssertOkAndTestMacros) {
  const Result<void> ok_void = OkStatus();
  PW_ASSERT_OK(ok_void);
  PW_TEST_EXPECT_OK(ok_void);
  PW_TEST_ASSERT_OK(ok_void);

  const TestResult<void> custom_ok(std::in_place);
  PW_ASSERT_OK(custom_ok);
  PW_TEST_EXPECT_OK(custom_ok);
  PW_TEST_ASSERT_OK(custom_ok);

  const TestResult<int> custom_value(3);
  PW_ASSERT_OK(custom_value);
  PW_TEST_EXPECT_OK(custom_value);
  PW_TEST_ASSERT_OK_AND_ASSIGN(int value, custom_value);
  EXPECT_EQ(value, 3);
}

PW_CONSTEXPR_TEST(ResultCustomStatus, ValueOr, {
  const TestResult<int> value(5);
  PW_TEST_EXPECT_EQ(value.value_or(10), 5);
  PW_TEST_EXPECT_EQ(TestResult<int>(5).value_or(10), 5);

  const TestResult<int> error(HasValue::kNo);
  PW_TEST_EXPECT_EQ(error.value_or(10), 10);
  PW_TEST_EXPECT_EQ(TestResult<int>(HasValue::kNo).value_or(10), 10);
});

PW_CONSTEXPR_TEST(ResultCustomStatus, ValueOr_MoveOnly, {
  TrivialMoveOnly moved = TestResult<TrivialMoveOnly>(std::in_place, 5)
                              .value_or(TrivialMoveOnly(10));
  PW_TEST_EXPECT_EQ(moved.value, 5);

  TrivialMoveOnly fallback =
      TestResult<TrivialMoveOnly>(HasValue::kNo).value_or(TrivialMoveOnly(10));
  PW_TEST_EXPECT_EQ(fallback.value, 10);
});

static_assert(std::is_final_v<Result<int>>);
static_assert(std::is_final_v<Result<void>>);
static_assert(std::is_final_v<TestResult<int>>);
static_assert(std::is_final_v<TestResult<void>>);

// Pigweed always builds without exceptions, so Result's noexcept qualifiers are
// unconditional even when T omits noexcept on its move operations.
struct ThrowingMove {
  ThrowingMove(ThrowingMove&&) noexcept(false) {}
  ThrowingMove& operator=(ThrowingMove&&) noexcept(false) { return *this; }
};

static_assert(std::is_nothrow_move_constructible_v<Result<int>>);
static_assert(std::is_nothrow_move_assignable_v<Result<int>>);
static_assert(std::is_nothrow_move_constructible_v<Result<ThrowingMove>>);
static_assert(std::is_nothrow_move_assignable_v<Result<ThrowingMove>>);

// Comprehensive tests for Result<Status> and Result<StatusType, StatusType>.
static_assert(kCanCompare<Result<Status>, Result<Status>>);
static_assert(kCanCompare<Result<Status>, Status>);
static_assert(kCanCompare<Status, Result<Status>>);
static_assert(kCanCompare<Result<Status>, pw_Status>);
static_assert(kCanCompare<pw_Status, Result<Status>>);
static_assert(
    kCanCompare<TestResult<HasValueStatus>, TestResult<HasValueStatus>>);
static_assert(kCanCompare<TestResult<HasValueStatus>, HasValueStatus>);
static_assert(kCanCompare<HasValueStatus, TestResult<HasValueStatus>>);
static_assert(kCanCompare<TestResult<HasValueStatus>, HasValue>);
static_assert(kCanCompare<HasValue, TestResult<HasValueStatus>>);
static_assert(
    kCanCompare<Result<driver_test::DriverStatus>, driver_test::DriverStatus>);
static_assert(
    kCanCompare<Result<driver_test::DriverStatus>, driver_test::DriverError>);
static_assert(kCanCompare<Result<driver_test::DriverStatus>, Status>);
static_assert(kCanCompare<Result<driver_test::DriverStatus>, pw_Status>);

PW_CONSTEXPR_TEST(ResultStatusValue, ConstructionAndReset, {
  // Default construction creates an Unknown error Result<Status>.
  Result<Status> default_res;
  PW_TEST_EXPECT_FALSE(default_res.ok());
  PW_TEST_EXPECT_EQ(default_res.status(), Status::Unknown());

  // Constructing from any Status or pw_Status initializes the contained value
  // (ok() == true), even when the status is not OK.
  Result<Status> from_ok = OkStatus();
  PW_TEST_EXPECT_TRUE(from_ok.ok());
  PW_TEST_EXPECT_EQ(from_ok.status(), OkStatus());
  PW_TEST_EXPECT_EQ(*from_ok, OkStatus());
  PW_TEST_EXPECT_TRUE(from_ok->ok());

  Result<Status> from_not_found = Status::NotFound();
  PW_TEST_EXPECT_TRUE(from_not_found.ok());
  PW_TEST_EXPECT_EQ(from_not_found.status(), OkStatus());
  PW_TEST_EXPECT_EQ(*from_not_found, Status::NotFound());
  PW_TEST_EXPECT_TRUE(from_not_found->IsNotFound());

  Result<Status> from_code_ok = PW_STATUS_OK;
  PW_TEST_EXPECT_TRUE(from_code_ok.ok());
  PW_TEST_EXPECT_EQ(*from_code_ok, OkStatus());

  Result<Status> from_code_err = PW_STATUS_INVALID_ARGUMENT;
  PW_TEST_EXPECT_TRUE(from_code_err.ok());
  PW_TEST_EXPECT_EQ(*from_code_err, Status::InvalidArgument());

  Result<Status> in_place_ok(std::in_place);
  PW_TEST_EXPECT_TRUE(in_place_ok.ok());
  PW_TEST_EXPECT_EQ(*in_place_ok, OkStatus());

  Result<Status> in_place_err(std::in_place, PW_STATUS_ABORTED);
  PW_TEST_EXPECT_TRUE(in_place_err.ok());
  PW_TEST_EXPECT_EQ(*in_place_err, Status::Aborted());

  // Result<const Status> is also supported.
  Result<const Status> const_status_res = Status::PermissionDenied();
  PW_TEST_EXPECT_TRUE(const_status_res.ok());
  PW_TEST_EXPECT_EQ(*const_status_res, Status::PermissionDenied());
});

PW_CONSTEXPR_TEST(ResultStatusValue, AssignmentAndReset, {
  Result<Status> res = OkStatus();
  PW_TEST_EXPECT_TRUE(res.ok());
  PW_TEST_EXPECT_EQ(*res, OkStatus());

  // Assigning a Status or pw_Status always assigns the contained value.
  res = Status::NotFound();
  PW_TEST_EXPECT_TRUE(res.ok());
  PW_TEST_EXPECT_EQ(res.status(), OkStatus());
  PW_TEST_EXPECT_EQ(*res, Status::NotFound());

  res = PW_STATUS_PERMISSION_DENIED;
  PW_TEST_EXPECT_TRUE(res.ok());
  PW_TEST_EXPECT_EQ(*res, Status::PermissionDenied());

  // Use reset() to set the error status on Result<Status>.
  res.reset(Status::Internal());
  PW_TEST_EXPECT_FALSE(res.ok());
  PW_TEST_EXPECT_EQ(res.status(), Status::Internal());
  PW_TEST_EXPECT_EQ(res.value_or(Status::Cancelled()), Status::Cancelled());

  res.reset(PW_STATUS_DATA_LOSS);
  PW_TEST_EXPECT_FALSE(res.ok());
  PW_TEST_EXPECT_EQ(res.status(), Status::DataLoss());

  res.reset<PW_STATUS_UNAVAILABLE>();
  PW_TEST_EXPECT_FALSE(res.ok());
  PW_TEST_EXPECT_EQ(res.status(), Status::Unavailable());
});

TEST(ResultStatusValue, AssignAndEmplaceAfterReset) {
  Result<Status> res;
  EXPECT_FALSE(res.ok());
  EXPECT_EQ(res.status(), Status::Unknown());

  // Assigning a Status after default construction or reset() restores the
  // Result to holding a value.
  res = Status::AlreadyExists();
  EXPECT_TRUE(res.ok());
  EXPECT_EQ(res.status(), OkStatus());
  EXPECT_EQ(*res, Status::AlreadyExists());
  EXPECT_EQ(res.value_or(Status::Cancelled()), Status::AlreadyExists());

  res.reset<PW_STATUS_INTERNAL>();
  EXPECT_FALSE(res.ok());
  EXPECT_EQ(res.status(), Status::Internal());

  res = OkStatus();
  EXPECT_TRUE(res.ok());
  EXPECT_EQ(*res, OkStatus());

  // emplace() also sets the contained value.
  res.reset<PW_STATUS_INTERNAL>();
  res.emplace(PW_STATUS_OUT_OF_RANGE);
  EXPECT_TRUE(res.ok());
  EXPECT_EQ(*res, Status::OutOfRange());
}

TEST(ResultStatusValue, DocExample) {
  // DOCSTAG: [pw_result-status-value]
  // Constructing or assigning from Status sets the contained value:
  pw::Result<pw::Status> res = pw::Status::NotFound();
  PW_ASSERT(res.ok());                        // The Result has a value!
  PW_ASSERT(res.status() == pw::OkStatus());  // The Result's status is OK.
  PW_ASSERT(*res == pw::Status::NotFound());  // Contained value is NOT_FOUND.
  PW_ASSERT(res == pw::Status::NotFound());   // Compares the contained value.

  // Use reset() to set the Result's error status:
  res.reset<PW_STATUS_INTERNAL>();
  PW_ASSERT(!res.ok());  // No value is present.
  PW_ASSERT(res.status() == pw::Status::Internal());
  PW_ASSERT(res != pw::Status::Internal());  // No value to compare equal.
  // DOCSTAG: [pw_result-status-value]

  EXPECT_FALSE(res.ok());
  EXPECT_EQ(res.status(), Status::Internal());
}

PW_CONSTEXPR_TEST(ResultStatusValue, Equality, {
  const Result<Status> val_ok = OkStatus();
  const Result<Status> val_not_found = Status::NotFound();
  Result<Status> err_not_found = OkStatus();
  err_not_found.reset<PW_STATUS_NOT_FOUND>();
  Result<Status> err_internal = OkStatus();
  err_internal.reset<PW_STATUS_INTERNAL>();

  // Result<Status> vs Result<Status>.
  PW_TEST_EXPECT_TRUE(val_ok == Result<Status>(OkStatus()));
  PW_TEST_EXPECT_TRUE(val_not_found == Result<Status>(Status::NotFound()));
  PW_TEST_EXPECT_TRUE(val_ok != val_not_found);
  // A Result<Status> holding Status::NotFound() as a value does NOT equal a
  // Result<Status> whose error status is Status::NotFound().
  PW_TEST_EXPECT_TRUE(val_not_found != err_not_found);
  PW_TEST_EXPECT_TRUE(val_ok != err_not_found);
  PW_TEST_EXPECT_TRUE(err_not_found == err_not_found);
  PW_TEST_EXPECT_TRUE(err_not_found != err_internal);

  // Result<Status> vs Status / pw_Status compares against the contained value.
  PW_TEST_EXPECT_TRUE(val_ok == OkStatus());
  PW_TEST_EXPECT_TRUE(OkStatus() == val_ok);
  PW_TEST_EXPECT_TRUE(val_ok == PW_STATUS_OK);
  PW_TEST_EXPECT_TRUE(PW_STATUS_OK == val_ok);
  PW_TEST_EXPECT_TRUE(val_ok != Status::NotFound());
  PW_TEST_EXPECT_TRUE(val_ok != PW_STATUS_NOT_FOUND);

  PW_TEST_EXPECT_TRUE(val_not_found == Status::NotFound());
  PW_TEST_EXPECT_TRUE(Status::NotFound() == val_not_found);
  PW_TEST_EXPECT_TRUE(val_not_found == PW_STATUS_NOT_FOUND);
  PW_TEST_EXPECT_TRUE(PW_STATUS_NOT_FOUND == val_not_found);
  PW_TEST_EXPECT_TRUE(val_not_found != OkStatus());
  PW_TEST_EXPECT_TRUE(OkStatus() != val_not_found);
  PW_TEST_EXPECT_TRUE(val_not_found != PW_STATUS_OK);
  PW_TEST_EXPECT_TRUE(PW_STATUS_OK != val_not_found);

  // An error Result<Status> has no value, so it never compares equal to a
  // Status or pw_Status value; check .status() instead.
  PW_TEST_EXPECT_FALSE(err_not_found == Status::NotFound());
  PW_TEST_EXPECT_TRUE(err_not_found != Status::NotFound());
  PW_TEST_EXPECT_FALSE(Status::NotFound() == err_not_found);
  PW_TEST_EXPECT_TRUE(Status::NotFound() != err_not_found);
  PW_TEST_EXPECT_FALSE(err_not_found == PW_STATUS_NOT_FOUND);
  PW_TEST_EXPECT_TRUE(err_not_found != PW_STATUS_NOT_FOUND);
  PW_TEST_EXPECT_FALSE(err_not_found == OkStatus());
  PW_TEST_EXPECT_EQ(err_not_found.status(), Status::NotFound());
});

PW_CONSTEXPR_TEST(ResultStatusValue, MonadicOperations, {
  // Transforming Result<int> to Result<Status>.
  Result<int> ok_int = 0;
  Result<Status> transformed_ok = ok_int.transform(
      [](int x) { return x == 0 ? OkStatus() : Status::InvalidArgument(); });
  PW_TEST_EXPECT_TRUE(transformed_ok.ok());
  PW_TEST_EXPECT_EQ(*transformed_ok, OkStatus());

  Result<int> nonzero_int = 5;
  Result<Status> transformed_nonzero = nonzero_int.transform(
      [](int x) { return x == 0 ? OkStatus() : Status::InvalidArgument(); });
  PW_TEST_EXPECT_TRUE(transformed_nonzero.ok());
  PW_TEST_EXPECT_EQ(*transformed_nonzero, Status::InvalidArgument());

  // Transforming an error Result<int> to Result<Status> must propagate the
  // error status (ok() == false), NOT construct a value Result<Status>!
  Result<int> err_int = Status::Internal();
  Result<Status> err_transformed_lref =
      err_int.transform([](int&) { return OkStatus(); });
  PW_TEST_EXPECT_FALSE(err_transformed_lref.ok());
  PW_TEST_EXPECT_EQ(err_transformed_lref.status(), Status::Internal());

  const Result<int> const_err_int = Status::Internal();
  Result<Status> err_transformed_clref =
      const_err_int.transform([](const int&) { return OkStatus(); });
  PW_TEST_EXPECT_FALSE(err_transformed_clref.ok());
  PW_TEST_EXPECT_EQ(err_transformed_clref.status(), Status::Internal());

  Result<Status> err_transformed_rref =
      std::move(err_int).transform([](int&&) { return OkStatus(); });
  PW_TEST_EXPECT_FALSE(err_transformed_rref.ok());
  PW_TEST_EXPECT_EQ(err_transformed_rref.status(), Status::Internal());

  Result<Status> err_transformed_crref =
      std::move(const_err_int).transform([](const int&&) {
        return OkStatus();
      });
  PW_TEST_EXPECT_FALSE(err_transformed_crref.ok());
  PW_TEST_EXPECT_EQ(err_transformed_crref.status(), Status::Internal());

  // and_then returning Result<Status> on an error Result<int> must also
  // propagate the error status (ok() == false).
  Result<int> err_for_and_then = Status::ResourceExhausted();
  Result<Status> err_and_then_lref = err_for_and_then.and_then(
      [](int&) -> Result<Status> { return OkStatus(); });
  PW_TEST_EXPECT_FALSE(err_and_then_lref.ok());
  PW_TEST_EXPECT_EQ(err_and_then_lref.status(), Status::ResourceExhausted());

  const Result<int> const_err_for_and_then = Status::Internal();
  Result<Status> err_and_then_clref = const_err_for_and_then.and_then(
      [](const int&) -> Result<Status> { return OkStatus(); });
  PW_TEST_EXPECT_FALSE(err_and_then_clref.ok());
  PW_TEST_EXPECT_EQ(err_and_then_clref.status(), Status::Internal());

  Result<Status> err_and_then_rref =
      std::move(err_for_and_then).and_then([](int&&) -> Result<Status> {
        return OkStatus();
      });
  PW_TEST_EXPECT_FALSE(err_and_then_rref.ok());
  PW_TEST_EXPECT_EQ(err_and_then_rref.status(), Status::ResourceExhausted());

  Result<Status> err_and_then_crref =
      std::move(const_err_for_and_then)
          .and_then([](const int&&) -> Result<Status> { return OkStatus(); });
  PW_TEST_EXPECT_FALSE(err_and_then_crref.ok());
  PW_TEST_EXPECT_EQ(err_and_then_crref.status(), Status::Internal());

  // Transforming and and_then from Result<void> to Result<Status>.
  const Result<void> err_void = Status::Aborted();
  Result<Status> void_to_status_transform =
      err_void.transform([]() { return OkStatus(); });
  PW_TEST_EXPECT_FALSE(void_to_status_transform.ok());
  PW_TEST_EXPECT_EQ(void_to_status_transform.status(), Status::Aborted());

  Result<Status> void_to_status_rref_transform =
      Result<void>(Status::Aborted()).transform([]() { return OkStatus(); });
  PW_TEST_EXPECT_FALSE(void_to_status_rref_transform.ok());
  PW_TEST_EXPECT_EQ(void_to_status_rref_transform.status(), Status::Aborted());

  Result<Status> void_to_status_and_then =
      err_void.and_then([]() -> Result<Status> { return OkStatus(); });
  PW_TEST_EXPECT_FALSE(void_to_status_and_then.ok());
  PW_TEST_EXPECT_EQ(void_to_status_and_then.status(), Status::Aborted());

  Result<Status> void_to_status_rref_and_then =
      Result<void>(Status::Aborted()).and_then([]() -> Result<Status> {
        return OkStatus();
      });
  PW_TEST_EXPECT_FALSE(void_to_status_rref_and_then.ok());
  PW_TEST_EXPECT_EQ(void_to_status_rref_and_then.status(), Status::Aborted());

  // Monadic operations on Result<Status> itself.
  Result<Status> status_res = Status::NotFound();
  Result<bool> is_ok = status_res.transform([](Status s) { return s.ok(); });
  PW_TEST_EXPECT_TRUE(is_ok.ok());
  PW_TEST_EXPECT_FALSE(*is_ok);

  Result<Status> err_status_res = OkStatus();
  err_status_res.reset<PW_STATUS_DEADLINE_EXCEEDED>();
  Result<Status> chained =
      err_status_res.and_then([](Status s) -> Result<Status> { return s; });
  PW_TEST_EXPECT_FALSE(chained.ok());
  PW_TEST_EXPECT_EQ(chained.status(), Status::DeadlineExceeded());

  // or_else on Result<Status> can recover with a fallback Status value or
  // propagate an error Result<Status>.
  Result<Status> recovered = err_status_res.or_else([](Status s) { return s; });
  PW_TEST_EXPECT_TRUE(recovered.ok());
  PW_TEST_EXPECT_EQ(*recovered, Status::DeadlineExceeded());
});

PW_CONSTEXPR_TEST(ResultStatusValue, CustomStatusTypeAsValue, {
  // Result<HasValueStatus, HasValueStatus> (i.e. TestResult<HasValueStatus>).
  TestResult<HasValueStatus> val_yes = HasValueStatus(HasValue::kYes);
  PW_TEST_EXPECT_TRUE(val_yes.ok());
  PW_TEST_EXPECT_EQ(val_yes.status(), HasValue::kYes);
  PW_TEST_EXPECT_EQ(*val_yes, HasValue::kYes);

  TestResult<HasValueStatus> val_no = HasValueStatus(HasValue::kNo);
  PW_TEST_EXPECT_TRUE(val_no.ok());
  PW_TEST_EXPECT_EQ(val_no.status(), HasValue::kYes);
  PW_TEST_EXPECT_EQ(*val_no, HasValue::kNo);
  PW_TEST_EXPECT_FALSE(val_no->ok());

  // Direct initialization from the explicit Code enum constructs the value.
  TestResult<HasValueStatus> from_code(HasValue::kDefinitelyNot);
  PW_TEST_EXPECT_TRUE(from_code.ok());
  PW_TEST_EXPECT_EQ(*from_code, HasValue::kDefinitelyNot);

  // Assignment from HasValueStatus updates the value.
  val_yes = HasValueStatus(HasValue::kDefinitelyNot);
  PW_TEST_EXPECT_TRUE(val_yes.ok());
  PW_TEST_EXPECT_EQ(*val_yes, HasValue::kDefinitelyNot);

  // reset() sets the error status.
  val_yes.reset(HasValue::kNo);
  PW_TEST_EXPECT_FALSE(val_yes.ok());
  PW_TEST_EXPECT_EQ(val_yes.status(), HasValue::kNo);

  val_yes.reset<HasValue::kDefinitelyNot>();
  PW_TEST_EXPECT_FALSE(val_yes.ok());
  PW_TEST_EXPECT_EQ(val_yes.status(), HasValue::kDefinitelyNot);

  // Equality comparisons.
  PW_TEST_EXPECT_TRUE(val_no == HasValueStatus(HasValue::kNo));
  PW_TEST_EXPECT_TRUE(val_no == HasValue::kNo);
  PW_TEST_EXPECT_TRUE(val_no != HasValue::kYes);
  PW_TEST_EXPECT_FALSE(val_yes == HasValue::kDefinitelyNot);
  PW_TEST_EXPECT_TRUE(val_yes != val_no);

  // Monadic operations with TestResult<HasValueStatus>.
  TestResult<int> err_int(HasValue::kNo);
  TestResult<HasValueStatus> transformed =
      err_int.transform([](int) { return HasValueStatus(); });
  PW_TEST_EXPECT_FALSE(transformed.ok());
  PW_TEST_EXPECT_EQ(transformed.status(), HasValue::kNo);
});

PW_CONSTEXPR_TEST(ResultStatusValue, DifferentStatusTypeAsValue, {
  using driver_test::DriverError;
  using driver_test::DriverStatus;

  // Result<DriverStatus, Status>: value is DriverStatus, error is pw::Status.
  Result<DriverStatus> ok_driver = DriverStatus(DriverError::kTimeout);
  PW_TEST_EXPECT_TRUE(ok_driver.ok());
  PW_TEST_EXPECT_EQ(ok_driver.status(), OkStatus());
  PW_TEST_EXPECT_EQ(*ok_driver, DriverError::kTimeout);

  // Comparing with DriverStatus / DriverError compares the value.
  PW_TEST_EXPECT_TRUE(ok_driver == DriverStatus(DriverError::kTimeout));
  PW_TEST_EXPECT_TRUE(DriverStatus(DriverError::kTimeout) == ok_driver);
  PW_TEST_EXPECT_TRUE(ok_driver == DriverError::kTimeout);
  PW_TEST_EXPECT_TRUE(DriverError::kTimeout == ok_driver);
  PW_TEST_EXPECT_TRUE(ok_driver != DriverError::kOk);

  // Comparing with pw::Status / pw_Status compares the Result's status.
  PW_TEST_EXPECT_TRUE(ok_driver == OkStatus());
  PW_TEST_EXPECT_TRUE(ok_driver == PW_STATUS_OK);
  PW_TEST_EXPECT_TRUE(ok_driver != Status::NotFound());

  Result<DriverStatus> err_driver = Status::NotFound();
  PW_TEST_EXPECT_FALSE(err_driver.ok());
  PW_TEST_EXPECT_EQ(err_driver.status(), Status::NotFound());
  PW_TEST_EXPECT_TRUE(err_driver == Status::NotFound());
  PW_TEST_EXPECT_FALSE(err_driver == DriverError::kTimeout);
});

PW_CONSTEXPR_TEST(ResultStatusValue, StatusCodeAsValue, {
  // Result<HasValue, HasValueStatus>: value is HasValue (Code), error is
  // HasValueStatus.
  Result<HasValue, HasValueStatus> ok_code(HasValue::kNo);
  PW_TEST_EXPECT_TRUE(ok_code.ok());
  PW_TEST_EXPECT_EQ(ok_code.status(), HasValue::kYes);
  PW_TEST_EXPECT_EQ(*ok_code, HasValue::kNo);
  PW_TEST_EXPECT_TRUE(ok_code == HasValue::kNo);
  PW_TEST_EXPECT_TRUE(ok_code != HasValue::kYes);
  PW_TEST_EXPECT_TRUE(ok_code == HasValueStatus(HasValue::kYes));

  ok_code = HasValue::kDefinitelyNot;
  PW_TEST_EXPECT_TRUE(ok_code.ok());
  PW_TEST_EXPECT_EQ(*ok_code, HasValue::kDefinitelyNot);

  Result<HasValue, HasValueStatus> err_code = HasValueStatus(HasValue::kNo);
  PW_TEST_EXPECT_FALSE(err_code.ok());
  PW_TEST_EXPECT_EQ(err_code.status(), HasValue::kNo);
  PW_TEST_EXPECT_TRUE(err_code == HasValueStatus(HasValue::kNo));
  PW_TEST_EXPECT_FALSE(err_code == HasValue::kNo);
});

PW_CONSTEXPR_TEST(ResultCustomStatus, Void_ConversionOperatorPriority, {
  struct ConvertibleToVoidResultAndStatus {
    constexpr operator Result<void>() const { return Status::NotFound(); }
    constexpr operator Status() const { return Status::Internal(); }
  };

  ConvertibleToVoidResultAndStatus conv;
  Result<void> r = conv;
  PW_TEST_EXPECT_FALSE(r.ok());
  PW_TEST_EXPECT_EQ(r.status(), Status::NotFound());

  Result<void> r2 = OkStatus();
  r2 = conv;
  PW_TEST_EXPECT_FALSE(r2.ok());
  PW_TEST_EXPECT_EQ(r2.status(), Status::NotFound());
});

class [[nodiscard]] StatusWithDetail final
    : public StatusBase<StatusWithDetail, HasValue::kYes> {
 public:
  constexpr StatusWithDetail() = default;
  explicit constexpr StatusWithDetail(HasValue code, uint8_t detail = 0)
      : StatusBase(code), detail_(detail) {}

  [[nodiscard]] constexpr uint8_t detail() const { return detail_; }

 private:
  uint8_t detail_ = 0;
};

PW_CONSTEXPR_TEST(ResultCustomStatus, PreservesDerivedStatusState, {
  Result<int, StatusWithDetail> res = StatusWithDetail(HasValue::kNo, 42);
  PW_TEST_EXPECT_FALSE(res.ok());
  PW_TEST_EXPECT_EQ(res.status().code(), HasValue::kNo);
  PW_TEST_EXPECT_EQ(res.status().detail(), 42u);

  Result<long, StatusWithDetail> converted = res;
  PW_TEST_EXPECT_EQ(converted.status().detail(), 42u);

  Result<int, StatusWithDetail> transformed =
      res.transform([](int x) { return x + 1; });
  PW_TEST_EXPECT_EQ(transformed.status().detail(), 42u);

  Result<int, StatusWithDetail> chained =
      res.and_then([](int x) { return Result<int, StatusWithDetail>(x + 1); });
  PW_TEST_EXPECT_EQ(chained.status().code(), HasValue::kNo);
  PW_TEST_EXPECT_EQ(chained.status().detail(), 42u);

  res.reset(StatusWithDetail(HasValue::kDefinitelyNot, 55));
  PW_TEST_EXPECT_EQ(res.status().code(), HasValue::kDefinitelyNot);
  PW_TEST_EXPECT_EQ(res.status().detail(), 55u);

  converted = res;
  PW_TEST_EXPECT_EQ(converted.status().code(), HasValue::kDefinitelyNot);
  PW_TEST_EXPECT_EQ(converted.status().detail(), 55u);

  converted =
      Result<int, StatusWithDetail>(StatusWithDetail(HasValue::kNo, 66));
  PW_TEST_EXPECT_EQ(converted.status().code(), HasValue::kNo);
  PW_TEST_EXPECT_EQ(converted.status().detail(), 66u);

  converted = StatusWithDetail(HasValue::kDefinitelyNot, 77);
  PW_TEST_EXPECT_EQ(converted.status().code(), HasValue::kDefinitelyNot);
  PW_TEST_EXPECT_EQ(converted.status().detail(), 77u);
});

PW_CONSTEXPR_TEST(ResultCustomStatus, Void_PreservesDerivedStatusState, {
  Result<void, StatusWithDetail> void_res =
      StatusWithDetail(HasValue::kDefinitelyNot, 99);
  PW_TEST_EXPECT_FALSE(void_res.ok());
  PW_TEST_EXPECT_EQ(void_res.status().code(), HasValue::kDefinitelyNot);
  PW_TEST_EXPECT_EQ(void_res.status().detail(), 99u);

  Result<int, StatusWithDetail> void_transformed =
      void_res.transform([]() { return 1; });
  PW_TEST_EXPECT_EQ(void_transformed.status().detail(), 99u);

  Result<int, StatusWithDetail> void_chained =
      void_res.and_then([]() { return Result<int, StatusWithDetail>(1); });
  PW_TEST_EXPECT_EQ(void_chained.status().detail(), 99u);

  void_res.reset(StatusWithDetail(HasValue::kNo, 88));
  PW_TEST_EXPECT_EQ(void_res.status().code(), HasValue::kNo);
  PW_TEST_EXPECT_EQ(void_res.status().detail(), 88u);

  void_res = StatusWithDetail(HasValue::kDefinitelyNot, 77);
  PW_TEST_EXPECT_EQ(void_res.status().code(), HasValue::kDefinitelyNot);
  PW_TEST_EXPECT_EQ(void_res.status().detail(), 77u);
});

// -----------------------------------------------------------------------------
// Alignment with std::optional: triviality, C++20 constexpr, SFINAE, observers,
// value_or, swap, monadic relaxations, heterogeneous ==, and noexcept.
// -----------------------------------------------------------------------------

struct ReallyTrivialMoveOnly {
  constexpr explicit ReallyTrivialMoveOnly(int v) : value(v) {}
  ReallyTrivialMoveOnly(const ReallyTrivialMoveOnly&) = delete;
  ReallyTrivialMoveOnly& operator=(const ReallyTrivialMoveOnly&) = delete;
  constexpr ReallyTrivialMoveOnly(ReallyTrivialMoveOnly&&) = default;
  constexpr ReallyTrivialMoveOnly& operator=(ReallyTrivialMoveOnly&&) = default;
  ~ReallyTrivialMoveOnly() = default;
  int value;
};

struct NonTrivialCopyTrivialMove {
  constexpr explicit NonTrivialCopyTrivialMove(int v) : value(v) {}
  constexpr NonTrivialCopyTrivialMove(const NonTrivialCopyTrivialMove& other)
      : value(other.value + 100) {}
  constexpr NonTrivialCopyTrivialMove& operator=(
      const NonTrivialCopyTrivialMove& other) {
    value = other.value + 200;
    return *this;
  }
  constexpr NonTrivialCopyTrivialMove(NonTrivialCopyTrivialMove&&) = default;
  constexpr NonTrivialCopyTrivialMove& operator=(NonTrivialCopyTrivialMove&&) =
      default;
  ~NonTrivialCopyTrivialMove() = default;
  int value;
};

// Types with non-const deleted copy operations (e.g. `T(T&) = delete`), which
// C++17 rejects if a wrapper `= default`s a `const` copy operation directly
// over them.
struct TrivialNonConstDeletedCopy {
  constexpr explicit TrivialNonConstDeletedCopy(int v) : value(v) {}
  TrivialNonConstDeletedCopy(TrivialNonConstDeletedCopy&) = delete;
  TrivialNonConstDeletedCopy& operator=(TrivialNonConstDeletedCopy&) = delete;
  constexpr TrivialNonConstDeletedCopy(TrivialNonConstDeletedCopy&&) = default;
  constexpr TrivialNonConstDeletedCopy& operator=(
      TrivialNonConstDeletedCopy&&) = default;
  ~TrivialNonConstDeletedCopy() = default;
  int value;
};

struct NonTrivialNonConstDeletedCopy {
  explicit NonTrivialNonConstDeletedCopy(int v) : value(v) {}
  NonTrivialNonConstDeletedCopy(NonTrivialNonConstDeletedCopy&) = delete;
  NonTrivialNonConstDeletedCopy& operator=(NonTrivialNonConstDeletedCopy&) =
      delete;
  NonTrivialNonConstDeletedCopy(NonTrivialNonConstDeletedCopy&& other) noexcept
      : value(other.value) {}
  NonTrivialNonConstDeletedCopy& operator=(
      NonTrivialNonConstDeletedCopy&& other) noexcept {
    value = other.value;
    return *this;
  }
  ~NonTrivialNonConstDeletedCopy() {}
  int value;
};

struct NonConstDeletedCopyAssign {
  constexpr explicit NonConstDeletedCopyAssign(int v) : value(v) {}
  constexpr NonConstDeletedCopyAssign(const NonConstDeletedCopyAssign& other)
      : value(other.value) {}
  NonConstDeletedCopyAssign& operator=(NonConstDeletedCopyAssign&) = delete;
  constexpr NonConstDeletedCopyAssign(NonConstDeletedCopyAssign&&) = default;
  constexpr NonConstDeletedCopyAssign& operator=(NonConstDeletedCopyAssign&&) =
      default;
  int value;
};

// Triviality matrix for Result<T, StatusType> and Result<void, StatusType>.
static_assert(std::is_trivially_destructible_v<Result<int>>);
static_assert(std::is_trivially_copy_constructible_v<Result<int>>);
static_assert(std::is_trivially_move_constructible_v<Result<int>>);
static_assert(std::is_trivially_copy_assignable_v<Result<int>>);
static_assert(std::is_trivially_move_assignable_v<Result<int>>);
static_assert(std::is_trivially_copyable_v<Result<int>>);

static_assert(std::is_trivially_destructible_v<TestResult<int>>);
static_assert(std::is_trivially_copy_constructible_v<TestResult<int>>);
static_assert(std::is_trivially_move_constructible_v<TestResult<int>>);
static_assert(std::is_trivially_copy_assignable_v<TestResult<int>>);
static_assert(std::is_trivially_move_assignable_v<TestResult<int>>);
static_assert(std::is_trivially_copyable_v<TestResult<int>>);

static_assert(std::is_trivially_destructible_v<Result<const int>>);
static_assert(std::is_trivially_copy_constructible_v<Result<const int>>);
static_assert(std::is_trivially_move_constructible_v<Result<const int>>);
static_assert(!std::is_copy_assignable_v<Result<const int>>);
static_assert(!std::is_move_assignable_v<Result<const int>>);
static_assert(std::is_trivially_copyable_v<Result<const int>>);

static_assert(std::is_trivially_destructible_v<Result<ReallyTrivialMoveOnly>>);
static_assert(!std::is_copy_constructible_v<Result<ReallyTrivialMoveOnly>>);
static_assert(
    std::is_trivially_move_constructible_v<Result<ReallyTrivialMoveOnly>>);
static_assert(!std::is_copy_assignable_v<Result<ReallyTrivialMoveOnly>>);
static_assert(
    std::is_trivially_move_assignable_v<Result<ReallyTrivialMoveOnly>>);
static_assert(std::is_trivially_copyable_v<Result<ReallyTrivialMoveOnly>>);

static_assert(std::is_trivially_destructible_v<Result<TrivialMoveOnly>>);
static_assert(!std::is_copy_constructible_v<Result<TrivialMoveOnly>>);
static_assert(std::is_move_constructible_v<Result<TrivialMoveOnly>>);
static_assert(!std::is_trivially_move_constructible_v<Result<TrivialMoveOnly>>);
static_assert(!std::is_copy_assignable_v<Result<TrivialMoveOnly>>);
static_assert(std::is_move_assignable_v<Result<TrivialMoveOnly>>);
static_assert(!std::is_trivially_move_assignable_v<Result<TrivialMoveOnly>>);

static_assert(
    std::is_trivially_destructible_v<Result<NonTrivialCopyTrivialMove>>);
static_assert(std::is_copy_constructible_v<Result<NonTrivialCopyTrivialMove>>);
static_assert(
    !std::is_trivially_copy_constructible_v<Result<NonTrivialCopyTrivialMove>>);
static_assert(
    std::is_trivially_move_constructible_v<Result<NonTrivialCopyTrivialMove>>);
static_assert(std::is_copy_assignable_v<Result<NonTrivialCopyTrivialMove>>);
static_assert(
    !std::is_trivially_copy_assignable_v<Result<NonTrivialCopyTrivialMove>>);
static_assert(
    std::is_trivially_move_assignable_v<Result<NonTrivialCopyTrivialMove>>);
static_assert(!std::is_trivially_copyable_v<Result<NonTrivialCopyTrivialMove>>);

static_assert(!std::is_trivially_destructible_v<Result<Counter>>);
static_assert(std::is_copy_constructible_v<Result<Counter>>);
static_assert(!std::is_trivially_copy_constructible_v<Result<Counter>>);
static_assert(std::is_move_constructible_v<Result<Counter>>);
static_assert(!std::is_trivially_move_constructible_v<Result<Counter>>);
static_assert(std::is_copy_assignable_v<Result<Counter>>);
static_assert(!std::is_trivially_copy_assignable_v<Result<Counter>>);
static_assert(std::is_move_assignable_v<Result<Counter>>);
static_assert(!std::is_trivially_move_assignable_v<Result<Counter>>);

static_assert(std::is_trivially_destructible_v<Result<Immovable>>);
static_assert(!std::is_copy_constructible_v<Result<Immovable>>);
static_assert(!std::is_move_constructible_v<Result<Immovable>>);
static_assert(!std::is_copy_assignable_v<Result<Immovable>>);
static_assert(!std::is_move_assignable_v<Result<Immovable>>);

static_assert(!std::is_copy_constructible_v<Result<const MoveOnly>>);
static_assert(!std::is_move_constructible_v<Result<const MoveOnly>>);
static_assert(!std::is_copy_assignable_v<Result<const MoveOnly>>);
static_assert(!std::is_move_assignable_v<Result<const MoveOnly>>);

static_assert(
    std::is_trivially_destructible_v<Result<TrivialNonConstDeletedCopy>>);
static_assert(
    !std::is_copy_constructible_v<Result<TrivialNonConstDeletedCopy>>);
static_assert(
    std::is_trivially_move_constructible_v<Result<TrivialNonConstDeletedCopy>>);
static_assert(!std::is_copy_assignable_v<Result<TrivialNonConstDeletedCopy>>);
static_assert(
    std::is_trivially_move_assignable_v<Result<TrivialNonConstDeletedCopy>>);

static_assert(
    !std::is_trivially_destructible_v<Result<NonTrivialNonConstDeletedCopy>>);
static_assert(
    !std::is_copy_constructible_v<Result<NonTrivialNonConstDeletedCopy>>);
static_assert(
    std::is_move_constructible_v<Result<NonTrivialNonConstDeletedCopy>>);
static_assert(
    !std::is_copy_assignable_v<Result<NonTrivialNonConstDeletedCopy>>);
static_assert(std::is_move_assignable_v<Result<NonTrivialNonConstDeletedCopy>>);

static_assert(std::is_copy_constructible_v<Result<NonConstDeletedCopyAssign>>);
static_assert(!std::is_copy_assignable_v<Result<NonConstDeletedCopyAssign>>);
static_assert(
    std::is_trivially_move_constructible_v<Result<NonConstDeletedCopyAssign>>);
static_assert(
    std::is_trivially_move_assignable_v<Result<NonConstDeletedCopyAssign>>);

static_assert(std::is_trivially_destructible_v<Result<void>>);
static_assert(std::is_trivially_copy_constructible_v<Result<void>>);
static_assert(std::is_trivially_move_constructible_v<Result<void>>);
static_assert(std::is_trivially_copy_assignable_v<Result<void>>);
static_assert(std::is_trivially_move_assignable_v<Result<void>>);
static_assert(std::is_trivially_copyable_v<Result<void>>);

// Empty base optimization check: linear inheritance must add zero padding
// bytes.
static_assert(sizeof(Result<uint8_t, HasValueStatus>) == 2);
static_assert(sizeof(Result<int>) == 2 * sizeof(int));

PW_CONSTEXPR_TEST(ResultOptionalAlign, TrivialCopyMoveConstexpr, {
  Result<int> a = 42;
  Result<int> b = a;
  PW_TEST_EXPECT_TRUE(b.has_value());
  PW_TEST_EXPECT_EQ(*b, 42);

  Result<int> c = std::move(b);
  PW_TEST_EXPECT_EQ(*c, 42);

  Result<int> err = Status::NotFound();
  c = err;
  PW_TEST_EXPECT_FALSE(c.has_value());
  PW_TEST_EXPECT_EQ(c.status(), Status::NotFound());

  c = a;
  PW_TEST_EXPECT_TRUE(c.has_value());
  PW_TEST_EXPECT_EQ(*c, 42);

  // Trivial moves work in C++17 constant evaluation even when copies are
  // non-trivial, including in the error state.
  Result<NonTrivialCopyTrivialMove> nt1(std::in_place, 7);
  Result<NonTrivialCopyTrivialMove> nt2 = std::move(nt1);
  PW_TEST_EXPECT_EQ(nt2->value, 7);

  Result<NonTrivialCopyTrivialMove> nt_err = Status::NotFound();
  Result<NonTrivialCopyTrivialMove> nt_err2 = std::move(nt_err);
  PW_TEST_EXPECT_EQ(nt_err2.status(), Status::NotFound());
});

TEST(ResultOptionalAlign, NonTrivialCopyTrivialMoveRuntime) {
  Result<NonTrivialCopyTrivialMove> nt1(std::in_place, 7);
  Result<NonTrivialCopyTrivialMove> nt2 = std::move(nt1);
  EXPECT_EQ(nt2->value, 7);
  Result<NonTrivialCopyTrivialMove> nt3 = nt2;
  EXPECT_EQ(nt3->value, 107);

  Result<NonTrivialCopyTrivialMove> nt_err = Status::Unavailable();
  Result<NonTrivialCopyTrivialMove> nt_err2 = std::move(nt_err);
  EXPECT_EQ(nt_err2.status(), Status::Unavailable());
}

TEST(ResultOptionalAlign, NonConstDeletedCopyOperations) {
  Result<TrivialNonConstDeletedCopy> t1(std::in_place, 11);
  Result<TrivialNonConstDeletedCopy> t2 = std::move(t1);
  PW_TEST_ASSERT_OK(t2);
  EXPECT_EQ(t2->value, 11);
  t1 = Status::Aborted();
  t2 = std::move(t1);
  EXPECT_EQ(t2.status(), Status::Aborted());

  Result<NonTrivialNonConstDeletedCopy> n1(std::in_place, 22);
  Result<NonTrivialNonConstDeletedCopy> n2 = std::move(n1);
  PW_TEST_ASSERT_OK(n2);
  EXPECT_EQ(n2->value, 22);
  n1 = Status::NotFound();
  n2 = std::move(n1);
  EXPECT_EQ(n2.status(), Status::NotFound());

  Result<NonConstDeletedCopyAssign> a1(std::in_place, 33);
  Result<NonConstDeletedCopyAssign> a2 = a1;
  Result<NonConstDeletedCopyAssign> a3 = std::move(a1);
  PW_TEST_ASSERT_OK(a2);
  PW_TEST_ASSERT_OK(a3);
  EXPECT_EQ(a2->value, 33);
  EXPECT_EQ(a3->value, 33);
}

// Status type with non-trivial copy and destruction. A Result using it has
// non-trivial special members even when the value type is trivial, so this
// covers the non-trivial copy/move paths and destructor driven by the status.
class [[nodiscard]] NonTrivialStatus final
    : public StatusBase<NonTrivialStatus, HasValue::kYes> {
 public:
  NonTrivialStatus() noexcept = default;
  explicit NonTrivialStatus(HasValue code) noexcept : StatusBase(code) {}
  NonTrivialStatus(const NonTrivialStatus& other) noexcept
      : StatusBase(other) {}
  NonTrivialStatus& operator=(const NonTrivialStatus& other) noexcept {
    StatusBase::operator=(other);
    return *this;
  }
  ~NonTrivialStatus() { destroyed += 1; }

  static inline int destroyed = 0;
};

using NonTrivialStatusResult = Result<int, NonTrivialStatus>;

static_assert(std::is_copy_constructible_v<NonTrivialStatusResult>);
static_assert(!std::is_trivially_copy_constructible_v<NonTrivialStatusResult>);
static_assert(!std::is_trivially_move_constructible_v<NonTrivialStatusResult>);
static_assert(!std::is_trivially_copy_assignable_v<NonTrivialStatusResult>);
static_assert(!std::is_trivially_move_assignable_v<NonTrivialStatusResult>);
static_assert(!std::is_trivially_destructible_v<NonTrivialStatusResult>);

TEST(ResultOptionalAlign, NonTrivialStatusType) {
  const NonTrivialStatusResult ok_res(std::in_place, 7);
  const NonTrivialStatusResult err_res = NonTrivialStatus(HasValue::kNo);

  // Copy construction.
  NonTrivialStatusResult copy_ok = ok_res;
  NonTrivialStatusResult copy_err = err_res;
  PW_TEST_ASSERT_OK(copy_ok);
  EXPECT_EQ(*copy_ok, 7);
  EXPECT_EQ(copy_err.status().code(), HasValue::kNo);

  // Move construction.
  NonTrivialStatusResult moved_ok = std::move(copy_ok);
  NonTrivialStatusResult moved_err = std::move(copy_err);
  PW_TEST_ASSERT_OK(moved_ok);
  EXPECT_EQ(*moved_ok, 7);
  EXPECT_EQ(moved_err.status().code(), HasValue::kNo);

  // Copy and move assignment between OK and non-OK states.
  moved_ok = err_res;
  EXPECT_EQ(moved_ok.status().code(), HasValue::kNo);
  moved_err = ok_res;
  PW_TEST_ASSERT_OK(moved_err);
  EXPECT_EQ(*moved_err, 7);
  moved_ok = std::move(moved_err);
  PW_TEST_ASSERT_OK(moved_ok);
  EXPECT_EQ(*moved_ok, 7);

  // Converting construction.
  const Result<long, NonTrivialStatus> converted = ok_res;
  PW_TEST_ASSERT_OK(converted);
  EXPECT_EQ(*converted, 7L);

  // Swap between OK and non-OK states.
  NonTrivialStatusResult swap_ok(std::in_place, 1);
  NonTrivialStatusResult swap_err = NonTrivialStatus(HasValue::kDefinitelyNot);
  swap_ok.swap(swap_err);
  EXPECT_EQ(swap_ok.status().code(), HasValue::kDefinitelyNot);
  PW_TEST_ASSERT_OK(swap_err);
  EXPECT_EQ(*swap_err, 1);

  // Destroying the Result destroys its status.
  NonTrivialStatus::destroyed = 0;
  {
    const NonTrivialStatusResult scoped(std::in_place, 3);
  }
  EXPECT_EQ(NonTrivialStatus::destroyed, 1);
}

#if _PW_RESULT_CONSTEXPR_CONSTRUCT_AT
struct ConstexprTracked {
  int value;
  int* destroyed;

  constexpr ConstexprTracked(int v, int* d) : value(v), destroyed(d) {}
  constexpr ConstexprTracked(const ConstexprTracked& other)
      : value(other.value), destroyed(other.destroyed) {}
  constexpr ConstexprTracked(ConstexprTracked&& other) noexcept
      : value(other.value), destroyed(other.destroyed) {
    other.value = -1;
  }
  constexpr ConstexprTracked& operator=(const ConstexprTracked& other) {
    value = other.value;
    destroyed = other.destroyed;
    return *this;
  }
  constexpr ConstexprTracked& operator=(ConstexprTracked&& other) noexcept {
    value = other.value;
    destroyed = other.destroyed;
    other.value = -1;
    return *this;
  }
  constexpr ~ConstexprTracked() {
    if (destroyed != nullptr) {
      ++(*destroyed);
    }
  }
};

static_assert(!std::is_trivially_destructible_v<ConstexprTracked>);

PW_CONSTEXPR_TEST(ResultOptionalAlign, Cpp20ConstexprNonTrivialLifecycle, {
  int destroyed = 0;
  {
    Result<ConstexprTracked> res = Status::Unavailable();
    PW_TEST_EXPECT_FALSE(res.has_value());

    // Empty-to-engaged via emplace (uses std::construct_at).
    ConstexprTracked& ref = res.emplace(10, &destroyed);
    PW_TEST_EXPECT_TRUE(res.has_value());
    PW_TEST_EXPECT_EQ(ref.value, 10);
    PW_TEST_EXPECT_EQ(destroyed, 0);

    // Engaged-to-engaged emplace destroys previous value first.
    res.emplace(20, &destroyed);
    PW_TEST_EXPECT_EQ(res->value, 20);
    PW_TEST_EXPECT_EQ(destroyed, 1);

    // Reset destroys value.
    res.reset(Status::Aborted());
    PW_TEST_EXPECT_FALSE(res.has_value());
    PW_TEST_EXPECT_EQ(destroyed, 2);

    // Empty-to-engaged assignment.
    res = ConstexprTracked(30, &destroyed);
    PW_TEST_EXPECT_TRUE(res.has_value());
    PW_TEST_EXPECT_EQ(res->value, 30);
    PW_TEST_EXPECT_EQ(destroyed, 3);  // temporary destroyed

    // OK-to-non-OK assignment destroys the value.
    Result<ConstexprTracked> other_err = Status::Internal();
    res = other_err;
    PW_TEST_EXPECT_FALSE(res.has_value());
    PW_TEST_EXPECT_EQ(res.status(), Status::Internal());
    PW_TEST_EXPECT_EQ(destroyed, 4);

    // Non-OK to non-OK assignment (assert-free path).
    res = Status::DataLoss();
    res = std::move(other_err);
    PW_TEST_EXPECT_EQ(res.status(), Status::Internal());
    PW_TEST_EXPECT_EQ(destroyed, 4);

    res.emplace(40, &destroyed);
  }
  // Scope exit ran constexpr destructor on engaged Result<ConstexprTracked>.
  PW_TEST_EXPECT_EQ(destroyed, 5);
});
#endif  // _PW_RESULT_CONSTEXPR_CONSTRUCT_AT

// SFINAE constraints on std::in_place constructors and emplace.
template <typename R, typename = void, typename... Args>
struct CanEmplaceImpl : std::false_type {};

template <typename R, typename... Args>
struct CanEmplaceImpl<
    R,
    std::void_t<decltype(std::declval<R&>().emplace(std::declval<Args>()...))>,
    Args...> : std::true_type {};

template <typename R, typename... Args>
inline constexpr bool kCanEmplace = CanEmplaceImpl<R, void, Args...>::value;

struct TwoIntCtor {
  constexpr TwoIntCtor(int a, int b) : sum(a + b) {}
  constexpr TwoIntCtor(std::initializer_list<int> ilist, int scale) : sum(0) {
    for (int x : ilist) {
      sum += x * scale;
    }
  }
  int sum;
};

static_assert(
    std::is_constructible_v<Result<TwoIntCtor>, std::in_place_t, int, int>);
static_assert(
    !std::is_constructible_v<Result<TwoIntCtor>, std::in_place_t, int>);
static_assert(
    !std::is_constructible_v<Result<TwoIntCtor>, std::in_place_t, const char*>);
static_assert(std::is_constructible_v<Result<TwoIntCtor>,
                                      std::in_place_t,
                                      std::initializer_list<int>,
                                      int>);
static_assert(!std::is_constructible_v<Result<TwoIntCtor>,
                                       std::in_place_t,
                                       std::initializer_list<int>>);
static_assert(!std::is_constructible_v<Result<int>,
                                       std::in_place_t,
                                       std::initializer_list<int>>);

static_assert(kCanEmplace<Result<TwoIntCtor>, int, int>);
static_assert(!kCanEmplace<Result<TwoIntCtor>, int>);
static_assert(!kCanEmplace<Result<TwoIntCtor>, const char*>);
static_assert(kCanEmplace<Result<TwoIntCtor>, std::initializer_list<int>, int>);
static_assert(!kCanEmplace<Result<TwoIntCtor>, std::initializer_list<int>>);

PW_CONSTEXPR_TEST(ResultOptionalAlign, HasValueObserver, {
  const Result<int> ok_val = 7;
  const Result<int> err_val = Status::NotFound();
  PW_TEST_EXPECT_TRUE(ok_val.has_value());
  PW_TEST_EXPECT_EQ(ok_val.has_value(), ok_val.ok());
  PW_TEST_EXPECT_FALSE(err_val.has_value());
  PW_TEST_EXPECT_EQ(err_val.has_value(), err_val.ok());

  const Result<void> ok_void = OkStatus();
  const Result<void> err_void = Status::NotFound();
  PW_TEST_EXPECT_TRUE(ok_void.has_value());
  PW_TEST_EXPECT_EQ(ok_void.has_value(), ok_void.ok());
  PW_TEST_EXPECT_FALSE(err_void.has_value());
  PW_TEST_EXPECT_EQ(err_void.has_value(), err_void.ok());
});

struct AggregatePair {
  int first = 0;
  int second = 0;
  constexpr bool operator==(const AggregatePair& other) const {
    return first == other.first && second == other.second;
  }
};

PW_CONSTEXPR_TEST(ResultOptionalAlign, ValueOrDefaultedTemplateParam, {
  const Result<AggregatePair> ok_pair(std::in_place, AggregatePair{1, 2});
  const Result<AggregatePair> err_pair = Status::NotFound();

  PW_TEST_EXPECT_EQ(ok_pair.value_or({}), (AggregatePair{1, 2}));
  PW_TEST_EXPECT_EQ(ok_pair.value_or({9, 8}), (AggregatePair{1, 2}));
  PW_TEST_EXPECT_EQ(err_pair.value_or({}), (AggregatePair{0, 0}));
  PW_TEST_EXPECT_EQ(err_pair.value_or({9, 8}), (AggregatePair{9, 8}));

  PW_TEST_EXPECT_EQ(Result<AggregatePair>(Status::NotFound()).value_or({}),
                    (AggregatePair{0, 0}));
  PW_TEST_EXPECT_EQ(Result<AggregatePair>(Status::NotFound()).value_or({3, 4}),
                    (AggregatePair{3, 4}));
});

struct ConstMoveTracker {
  int value;
  bool was_moved = false;

  constexpr explicit ConstMoveTracker(int v) : value(v) {}
  constexpr ConstMoveTracker(const ConstMoveTracker& other)
      : value(other.value), was_moved(false) {}
  constexpr ConstMoveTracker(ConstMoveTracker&& other) noexcept
      : value(other.value), was_moved(true) {
    other.value = -1;
  }
};

static_assert(std::is_same_v<
              decltype(std::declval<const Result<const int>&>().value_or(0)),
              int>);
static_assert(
    std::is_same_v<decltype(std::declval<Result<volatile int>&&>().value_or(0)),
                   int>);

PW_CONSTEXPR_TEST(ResultOptionalAlign, ConstValuePreservesConstOnRvalue, {
  Result<const ConstMoveTracker> const_res(std::in_place, 42);
  ConstMoveTracker out = std::move(const_res).value_or(ConstMoveTracker(0));
  PW_TEST_EXPECT_EQ(out.value, 42);
  PW_TEST_EXPECT_FALSE(out.was_moved);

  Result<ConstMoveTracker> mut_res(std::in_place, 42);
  ConstMoveTracker moved_out = std::move(mut_res).value_or(ConstMoveTracker(0));
  PW_TEST_EXPECT_EQ(moved_out.value, 42);
  PW_TEST_EXPECT_TRUE(moved_out.was_moved);
});

TEST(ResultOptionalAlign, VolatileValue) {
  Result<volatile int> res(std::in_place, 5);
  PW_TEST_ASSERT_OK(res);
  EXPECT_EQ(*res, 5);
  EXPECT_EQ(res.value_or(0), 5);

  volatile int& ref = res.emplace(10);
  PW_TEST_ASSERT_OK(res);
  EXPECT_EQ(ref, 10);
  EXPECT_EQ(*res, 10);

  res.reset(Status::NotFound());
  EXPECT_FALSE(res.has_value());
  EXPECT_EQ(res.value_or(99), 99);
  EXPECT_EQ(std::move(res).value_or(77), 77);
}

TEST(ResultOptionalAlign, ConstNonTrivialDestruction) {
  Counter::Reset();
  {
    Result<const Counter> r(std::in_place, 9);
    PW_TEST_ASSERT_OK(r);
    EXPECT_EQ(r->value, 9);
    r.reset(Status::Aborted());
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(Counter::destroyed, 1);
  }
  EXPECT_EQ(Counter::created, 1);
  EXPECT_EQ(Counter::destroyed, 1);
}

// Exercises `AssignFrom` when destroying `*this`'s value mutates or destroys
// the source `Result` (e.g., `root = std::move(*root->child)`).
struct ChildClobberOnDestroy {
  Result<ChildClobberOnDestroy, StatusWithDetail>* child = nullptr;

  constexpr explicit ChildClobberOnDestroy(
      Result<ChildClobberOnDestroy, StatusWithDetail>* c) noexcept
      : child(c) {}
  constexpr ChildClobberOnDestroy(const ChildClobberOnDestroy& other) noexcept
      : child(other.child) {}
  constexpr ChildClobberOnDestroy(ChildClobberOnDestroy&& other) noexcept
      : child(other.child) {
    other.child = nullptr;
  }
  constexpr ChildClobberOnDestroy& operator=(
      const ChildClobberOnDestroy& other) noexcept {
    child = other.child;
    return *this;
  }
  constexpr ChildClobberOnDestroy& operator=(
      ChildClobberOnDestroy&& other) noexcept {
    child = other.child;
    other.child = nullptr;
    return *this;
  }
  PW_CONSTEXPR_CPP20 ~ChildClobberOnDestroy() {
    if (child != nullptr) {
      *child = StatusWithDetail(HasValue::kDefinitelyNot, 0xEE);
    }
  }
};

TEST(ResultOptionalAlign, TransitiveSelfAssignment) {
  {
    Result<ChildClobberOnDestroy, StatusWithDetail> child =
        StatusWithDetail(HasValue::kNo, 77);
    Result<ChildClobberOnDestroy, StatusWithDetail> root(std::in_place, &child);
    root = std::move(child);
    EXPECT_FALSE(root.has_value());
    EXPECT_EQ(root.status().code(), HasValue::kNo);
    EXPECT_EQ(root.status().detail(), 77u);
  }
  {
    Result<ChildClobberOnDestroy, StatusWithDetail> child =
        StatusWithDetail(HasValue::kNo, 88);
    Result<ChildClobberOnDestroy, StatusWithDetail> root(std::in_place, &child);
    root = child;
    EXPECT_FALSE(root.has_value());
    EXPECT_EQ(root.status().code(), HasValue::kNo);
    EXPECT_EQ(root.status().detail(), 88u);
  }
}

#if _PW_RESULT_CONSTEXPR_CONSTRUCT_AT &&      \
    defined(__cpp_constexpr_dynamic_alloc) && \
    __cpp_constexpr_dynamic_alloc >= 201907L
struct HeapNode {
  Result<HeapNode>* child = nullptr;
  constexpr HeapNode() = default;
  constexpr explicit HeapNode(Result<HeapNode>* c) : child(c) {}
  constexpr HeapNode(HeapNode&& other) noexcept : child(other.child) {
    other.child = nullptr;
  }
  constexpr HeapNode& operator=(HeapNode&& other) noexcept {
    if (this != &other) {
      delete child;
      child = other.child;
      other.child = nullptr;
    }
    return *this;
  }
  constexpr ~HeapNode() { delete child; }
};

constexpr bool TestConstexprTransitiveSelfAssign() {
  Result<HeapNode> root(std::in_place,
                        new Result<HeapNode>(Status::DataLoss()));
  root = std::move(*root->child);
  return !root.has_value() && root.status() == Status::DataLoss();
}
static_assert(TestConstexprTransitiveSelfAssign());
#endif

// Converting assignment checks lvalue assignability (`T& = ...`) and
// direct-init prefers implicit value conversion over explicit status ctor.
struct LvalueAssignNoMove {
  constexpr LvalueAssignNoMove(int v) : value(v) {}  // NOLINT
  constexpr LvalueAssignNoMove(const LvalueAssignNoMove&) = default;
  constexpr LvalueAssignNoMove& operator=(const int& v) & {
    value = v;
    return *this;
  }
  LvalueAssignNoMove& operator=(const LvalueAssignNoMove&) = delete;
  LvalueAssignNoMove& operator=(LvalueAssignNoMove&&) = delete;
  int value;
};

static_assert(
    std::is_assignable_v<Result<LvalueAssignNoMove>&, const Result<int>&>);

struct HybridToIntOrExplicitStatus {
  constexpr operator int() const { return 42; }  // NOLINT
  explicit constexpr operator Status() const { return Status::NotFound(); }
};

static_assert(std::is_convertible_v<HybridToIntOrExplicitStatus, Result<int>>);
static_assert(
    std::is_constructible_v<Result<int>, HybridToIntOrExplicitStatus>);

PW_CONSTEXPR_TEST(ResultOptionalAlign, ConvertingAssignAndHybridDirectInit, {
  Result<LvalueAssignNoMove> dst(1);
  const Result<int> src(9);
  dst = src;
  PW_TEST_EXPECT_TRUE(dst.has_value());
  PW_TEST_EXPECT_EQ(dst->value, 9);

  const Result<int> src_err = Status::Aborted();
  dst = src_err;
  PW_TEST_EXPECT_EQ(dst.status(), Status::Aborted());

  HybridToIntOrExplicitStatus hybrid;
  Result<int> copy_init = hybrid;
  Result<int> direct_init(hybrid);
  PW_TEST_EXPECT_TRUE(copy_init.has_value());
  PW_TEST_EXPECT_EQ(*copy_init, 42);
  PW_TEST_EXPECT_TRUE(direct_init.has_value());
  PW_TEST_EXPECT_EQ(*direct_init, 42);

  Result<int> from_code(PW_STATUS_NOT_FOUND);
  PW_TEST_EXPECT_FALSE(from_code.has_value());
  PW_TEST_EXPECT_EQ(from_code.status(), Status::NotFound());
});

// Swap tests and SFINAE traits.
struct CustomSwappable {
  int value;
  int* custom_swap_calls;

  constexpr CustomSwappable(int v, int* calls = nullptr)
      : value(v), custom_swap_calls(calls) {}
  constexpr CustomSwappable(CustomSwappable&&) = default;
  constexpr CustomSwappable& operator=(CustomSwappable&&) = default;

  friend constexpr void swap(CustomSwappable& a, CustomSwappable& b) noexcept {
    int tmp = a.value;
    a.value = b.value;
    b.value = tmp;
    if (a.custom_swap_calls != nullptr) {
      ++(*a.custom_swap_calls);
    }
  }
};

struct MoveCtorOnlyWithAdlSwap {
  int value;
  constexpr explicit MoveCtorOnlyWithAdlSwap(int v) : value(v) {}
  MoveCtorOnlyWithAdlSwap(const MoveCtorOnlyWithAdlSwap&) = delete;
  MoveCtorOnlyWithAdlSwap& operator=(const MoveCtorOnlyWithAdlSwap&) = delete;
  constexpr MoveCtorOnlyWithAdlSwap(MoveCtorOnlyWithAdlSwap&&) = default;
  MoveCtorOnlyWithAdlSwap& operator=(MoveCtorOnlyWithAdlSwap&&) = delete;

  friend constexpr void swap(MoveCtorOnlyWithAdlSwap& a,
                             MoveCtorOnlyWithAdlSwap& b) noexcept {
    int tmp = a.value;
    a.value = b.value;
    b.value = tmp;
  }
};

class [[nodiscard]] NonMovableSwappableStatus final
    : public StatusBase<NonMovableSwappableStatus, HasValue::kYes> {
 public:
  constexpr NonMovableSwappableStatus() noexcept = default;
  explicit constexpr NonMovableSwappableStatus(HasValue code) noexcept
      : StatusBase(code) {}
  NonMovableSwappableStatus(const NonMovableSwappableStatus&) = delete;
  NonMovableSwappableStatus& operator=(const NonMovableSwappableStatus&) =
      delete;
  NonMovableSwappableStatus(NonMovableSwappableStatus&&) = delete;
  NonMovableSwappableStatus& operator=(NonMovableSwappableStatus&&) = delete;
  friend void swap(NonMovableSwappableStatus&,
                   NonMovableSwappableStatus&) noexcept {}
};

class [[nodiscard]] StatefulOkStatus final
    : public StatusBase<StatefulOkStatus, HasValue::kYes> {
 public:
  StatefulOkStatus() noexcept : StatusBase(HasValue::kYes), id_(next_id++) {}
  explicit constexpr StatefulOkStatus(HasValue code, uint8_t id) noexcept
      : StatusBase(code), id_(id) {}

  [[nodiscard]] constexpr uint8_t id() const noexcept { return id_; }

  inline static uint8_t next_id = 1;

 private:
  uint8_t id_ = 0;
};

static_assert(std::is_swappable_v<Result<int>>);
static_assert(std::is_nothrow_swappable_v<Result<int>>);
static_assert(std::is_swappable_v<Result<void>>);
static_assert(std::is_nothrow_swappable_v<Result<void>>);
static_assert(std::is_swappable_v<Result<MoveOnly>>);
static_assert(std::is_swappable_v<Result<MoveCtorOnlyWithAdlSwap>>);
static_assert(std::is_nothrow_swappable_v<Result<MoveCtorOnlyWithAdlSwap>>);
static_assert(!std::is_swappable_v<Result<const int>>);
static_assert(!std::is_swappable_v<Result<Immovable>>);
static_assert(!std::is_swappable_v<Result<MoveCtorNotAssign>>);
static_assert(!std::is_swappable_v<Result<int, NonMovableSwappableStatus>>);
static_assert(!std::is_swappable_v<Result<void, NonMovableSwappableStatus>>);
static_assert(std::is_nothrow_swappable_v<Result<ThrowingMove>>);

TEST(ResultOptionalAlign, SwapAllFourStates) {
  using std::swap;

  // 1. Both engaged: uses ADL swap on T.
  int swap_calls = 0;
  Result<CustomSwappable> s1(std::in_place, 10, &swap_calls);
  Result<CustomSwappable> s2(std::in_place, 20, &swap_calls);
  swap(s1, s2);
  EXPECT_EQ(swap_calls, 1);
  PW_TEST_ASSERT_OK(s1);
  PW_TEST_ASSERT_OK(s2);
  EXPECT_EQ(s1->value, 20);
  EXPECT_EQ(s2->value, 10);

  // Self-swap on engaged Result.
  swap(s1, s1);
  EXPECT_EQ(swap_calls, 2);
  PW_TEST_ASSERT_OK(s1);
  EXPECT_EQ(s1->value, 20);

  // 2. Engaged and disengaged (preserving derived status detail).
  Result<int, StatusWithDetail> ok_res(42);
  Result<int, StatusWithDetail> err_res(StatusWithDetail(HasValue::kNo, 77));
  ok_res.swap(err_res);
  EXPECT_FALSE(ok_res.has_value());
  EXPECT_EQ(ok_res.status().code(), HasValue::kNo);
  EXPECT_EQ(ok_res.status().detail(), 77u);
  PW_TEST_ASSERT_OK(err_res);
  EXPECT_EQ(*err_res, 42);

  // 3. Disengaged and engaged.
  swap(ok_res, err_res);
  PW_TEST_ASSERT_OK(ok_res);
  EXPECT_EQ(*ok_res, 42);
  EXPECT_FALSE(err_res.has_value());
  EXPECT_EQ(err_res.status().code(), HasValue::kNo);
  EXPECT_EQ(err_res.status().detail(), 77u);

  swap(ok_res, ok_res);
  PW_TEST_ASSERT_OK(ok_res);
  EXPECT_EQ(*ok_res, 42);

  // 4. Both disengaged.
  Result<int, StatusWithDetail> err2(
      StatusWithDetail(HasValue::kDefinitelyNot, 99));
  swap(err_res, err2);
  EXPECT_EQ(err_res.status().code(), HasValue::kDefinitelyNot);
  EXPECT_EQ(err_res.status().detail(), 99u);
  EXPECT_EQ(err2.status().code(), HasValue::kNo);
  EXPECT_EQ(err2.status().detail(), 77u);

  swap(err_res, err_res);
  EXPECT_EQ(err_res.status().code(), HasValue::kDefinitelyNot);
  EXPECT_EQ(err_res.status().detail(), 99u);

  // Move-constructible (non-move-assignable) type with ADL swap.
  Result<MoveCtorOnlyWithAdlSwap> m1(std::in_place, 1);
  Result<MoveCtorOnlyWithAdlSwap> m2 = Status::Aborted();
  swap(m1, m2);
  EXPECT_FALSE(m1.has_value());
  EXPECT_EQ(m1.status(), Status::Aborted());
  PW_TEST_ASSERT_OK(m2);
  EXPECT_EQ(m2->value, 1);

  Result<MoveCtorOnlyWithAdlSwap> m3(std::in_place, 2);
  swap(m2, m3);
  EXPECT_EQ(m2->value, 2);
  EXPECT_EQ(m3->value, 1);

  // Counter lifetime check across swap.
  Counter::Reset();
  {
    Result<Counter> c1(std::in_place, 5);
    Result<Counter> c2 = Status::NotFound();
    swap(c1, c2);
    EXPECT_FALSE(c1.has_value());
    PW_TEST_ASSERT_OK(c2);
    EXPECT_EQ(c2->value, 5);
  }
  EXPECT_EQ(Counter::created + Counter::moved, Counter::destroyed);

  // Result<void> swap.
  Result<void, StatusWithDetail> v1(std::in_place);
  Result<void, StatusWithDetail> v2(StatusWithDetail(HasValue::kNo, 55));
  swap(v1, v2);
  EXPECT_FALSE(v1.has_value());
  EXPECT_EQ(v1.status().detail(), 55u);
  PW_TEST_EXPECT_OK(v2);
  swap(v1, v1);
  swap(v2, v2);
  EXPECT_EQ(v1.status().detail(), 55u);
  PW_TEST_EXPECT_OK(v2);

  // Derived StatusType fields are preserved even on engaged (OK) statuses.
  StatefulOkStatus::next_id = 10;
  Result<int, StatefulOkStatus> ok_a(1);
  Result<int, StatefulOkStatus> ok_b(2);
  ASSERT_EQ(ok_a.status().id(), 10u);
  ASSERT_EQ(ok_b.status().id(), 11u);
  swap(ok_a, ok_b);
  EXPECT_EQ(*ok_a, 2);
  EXPECT_EQ(*ok_b, 1);
  EXPECT_EQ(ok_a.status().id(), 11u);
  EXPECT_EQ(ok_b.status().id(), 10u);

  Result<int, StatefulOkStatus> err_c(StatefulOkStatus(HasValue::kNo, 99));
  swap(ok_a, err_c);
  EXPECT_FALSE(ok_a.has_value());
  EXPECT_EQ(ok_a.status().id(), 99u);
  PW_TEST_ASSERT_OK(err_c);
  EXPECT_EQ(*err_c, 2);
  EXPECT_EQ(err_c.status().id(), 11u);
}

#if _PW_RESULT_CONSTEXPR_CONSTRUCT_AT
PW_CONSTEXPR_TEST(ResultOptionalAlign, ConstexprSwap, {
  using std::swap;
  Result<int> a = 1;
  Result<int> b = Status::NotFound();
  swap(a, b);
  PW_TEST_EXPECT_FALSE(a.has_value());
  PW_TEST_EXPECT_EQ(a.status(), Status::NotFound());
  PW_TEST_EXPECT_TRUE(b.has_value());
  PW_TEST_EXPECT_EQ(*b, 1);

  swap(a, a);
  swap(b, b);
  PW_TEST_EXPECT_EQ(a.status(), Status::NotFound());
  PW_TEST_EXPECT_EQ(*b, 1);

  Result<void> v1 = OkStatus();
  Result<void> v2 = Status::Internal();
  swap(v1, v2);
  PW_TEST_EXPECT_EQ(v1.status(), Status::Internal());
  PW_TEST_EXPECT_TRUE(v2.has_value());
  swap(v1, v1);
  swap(v2, v2);
  PW_TEST_EXPECT_EQ(v1.status(), Status::Internal());
  PW_TEST_EXPECT_TRUE(v2.has_value());
});
#endif  // _PW_RESULT_CONSTEXPR_CONSTRUCT_AT

// Monadic enhancements: lvalue Result -> MoveOnly / Immovable, 0-arg or_else,
// and SFINAE constraints.
template <typename R, typename Fn, typename = void>
struct CanAndThen : std::false_type {};
template <typename R, typename Fn>
struct CanAndThen<
    R,
    Fn,
    std::void_t<decltype(std::declval<R>().and_then(std::declval<Fn>()))>>
    : std::true_type {};

template <typename R, typename Fn, typename = void>
struct CanTransform : std::false_type {};
template <typename R, typename Fn>
struct CanTransform<
    R,
    Fn,
    std::void_t<decltype(std::declval<R>().transform(std::declval<Fn>()))>>
    : std::true_type {};

template <typename R, typename Fn, typename = void>
struct CanOrElse : std::false_type {};
template <typename R, typename Fn>
struct CanOrElse<
    R,
    Fn,
    std::void_t<decltype(std::declval<R>().or_else(std::declval<Fn>()))>>
    : std::true_type {};

struct ZeroArgMoveOnlyRecovery {
  Result<MoveOnly> operator()() const {
    return Result<MoveOnly>(std::in_place, 99);
  }
};
struct ZeroArgImmovableRecovery {
  Result<Immovable> operator()() const {
    return Result<Immovable>(std::in_place, 99);
  }
};
struct TakesStringOnly {
  Result<int> operator()(std::string) const { return 0; }
};

static_assert(!CanAndThen<Result<int>&, TakesStringOnly>::value);
static_assert(!CanTransform<Result<int>&, TakesStringOnly>::value);
static_assert(!CanOrElse<Result<int>&, TakesStringOnly>::value);
static_assert(
    !CanOrElse<const Result<MoveOnly>&, ZeroArgMoveOnlyRecovery>::value);
static_assert(!CanOrElse<Result<MoveOnly>&, ZeroArgMoveOnlyRecovery>::value);
static_assert(CanOrElse<Result<MoveOnly>&&, ZeroArgMoveOnlyRecovery>::value);
static_assert(!CanOrElse<Result<Immovable>&&, ZeroArgImmovableRecovery>::value);

class [[nodiscard]] MoveOnlyStatus final
    : public StatusBase<MoveOnlyStatus, HasValue::kYes> {
 public:
  constexpr MoveOnlyStatus() noexcept = default;
  explicit constexpr MoveOnlyStatus(HasValue code) noexcept
      : StatusBase(code) {}
  MoveOnlyStatus(const MoveOnlyStatus&) = delete;
  MoveOnlyStatus& operator=(const MoveOnlyStatus&) = delete;
  // Moving clears the source status to OK (`HasValue::kYes`), catching any
  // premature move from a `Result`'s status before that `Result` is returned.
  constexpr MoveOnlyStatus(MoveOnlyStatus&& other) noexcept
      : StatusBase(other.code_) {
    other.code_ = HasValue::kYes;
  }
  constexpr MoveOnlyStatus& operator=(MoveOnlyStatus&& other) noexcept {
    if (this != &other) {
      code_ = other.code_;
      other.code_ = HasValue::kYes;
    }
    return *this;
  }
};

static_assert(!CanOrElse<const Result<int, MoveOnlyStatus>&,
                         Result<int, MoveOnlyStatus> (*)()>::value);
static_assert(CanOrElse<Result<int, MoveOnlyStatus>&&,
                        Result<int, MoveOnlyStatus> (*)()>::value);
static_assert(!CanOrElse<const Result<void, MoveOnlyStatus>&,
                         Result<void, MoveOnlyStatus> (*)()>::value);
static_assert(CanOrElse<Result<void, MoveOnlyStatus>&&,
                        Result<void, MoveOnlyStatus> (*)()>::value);
// Void-returning callbacks may inspect `const Status&` or take 0 args, but must
// not consume the status by value/rvalue when `self` is returned.
static_assert(
    !CanOrElse<Result<int, MoveOnlyStatus>&&, void (*)(MoveOnlyStatus)>::value);
static_assert(!CanOrElse<Result<int>&&, void (*)(Status&&)>::value);
static_assert(CanOrElse<Result<int, MoveOnlyStatus>&&,
                        void (*)(const MoveOnlyStatus&)>::value);
static_assert(CanOrElse<Result<int>&&, void (*)(Status)>::value);

PW_CONSTEXPR_TEST(ResultOptionalAlign, MonadicMoveOnlyStatus, {
  auto ok_tx =
      Result<int, MoveOnlyStatus>(10).transform([](int x) { return x * 2; });
  PW_TEST_EXPECT_TRUE(ok_tx.has_value());
  PW_TEST_EXPECT_EQ(*ok_tx, 20);

  auto err_tx = Result<int, MoveOnlyStatus>(MoveOnlyStatus(HasValue::kNo))
                    .transform([](int x) { return x * 2; });
  PW_TEST_EXPECT_FALSE(err_tx.has_value());

  auto err_and_then =
      Result<int, MoveOnlyStatus>(MoveOnlyStatus(HasValue::kNo))
          .and_then([](int x) { return Result<int, MoveOnlyStatus>(x + 1); });
  PW_TEST_EXPECT_FALSE(err_and_then.has_value());

  auto recovered =
      Result<int, MoveOnlyStatus>(MoveOnlyStatus(HasValue::kNo))
          .or_else([](MoveOnlyStatus s) -> Result<int, MoveOnlyStatus> {
            return s.code() == HasValue::kNo ? 99 : 0;
          });
  PW_TEST_EXPECT_TRUE(recovered.has_value());
  PW_TEST_EXPECT_EQ(*recovered, 99);

  bool observed_err = false;
  auto side_effect_res =
      Result<int, MoveOnlyStatus>(MoveOnlyStatus(HasValue::kDefinitelyNot))
          .or_else([&observed_err](const MoveOnlyStatus& s) {
            observed_err = (s.code() == HasValue::kDefinitelyNot);
          });
  PW_TEST_EXPECT_TRUE(observed_err);
  PW_TEST_EXPECT_FALSE(side_effect_res.has_value());
  bool status_survived = false;
  static_cast<void>(std::move(side_effect_res)
                        .or_else([&status_survived](const MoveOnlyStatus& s) {
                          status_survived =
                              (s.code() == HasValue::kDefinitelyNot);
                        }));
  PW_TEST_EXPECT_TRUE(status_survived);

  bool zero_arg_called = false;
  auto zero_arg_res =
      Result<int, MoveOnlyStatus>(MoveOnlyStatus(HasValue::kDefinitelyNot))
          .or_else([&zero_arg_called]() { zero_arg_called = true; });
  PW_TEST_EXPECT_TRUE(zero_arg_called);
  PW_TEST_EXPECT_FALSE(zero_arg_res.has_value());
  bool zero_arg_survived = false;
  static_cast<void>(std::move(zero_arg_res)
                        .or_else([&zero_arg_survived](const MoveOnlyStatus& s) {
                          zero_arg_survived =
                              (s.code() == HasValue::kDefinitelyNot);
                        }));
  PW_TEST_EXPECT_TRUE(zero_arg_survived);

  auto void_tx = Result<void, MoveOnlyStatus>(MoveOnlyStatus(HasValue::kNo))
                     .transform([]() { return 5; });
  PW_TEST_EXPECT_FALSE(void_tx.has_value());

  auto void_recovered =
      Result<void, MoveOnlyStatus>(MoveOnlyStatus(HasValue::kNo)).or_else([]() {
        return MoveOnlyStatus();
      });
  PW_TEST_EXPECT_TRUE(void_recovered.has_value());
});

PW_CONSTEXPR_TEST(
    ResultOptionalAlign, TransformAndAndThenImmovableAndMoveOnly, {
      // Lvalue Result<int> transforming directly into Result<Immovable> and
      // Result<TrivialMoveOnly> without requiring copy/move on the return type.
      Result<int> ok_val = 21;
      Result<Immovable> imm_ok =
          ok_val.transform([](int& x) { return Immovable(x * 2); });
      PW_TEST_EXPECT_TRUE(imm_ok.has_value());
      PW_TEST_EXPECT_EQ(imm_ok->val(), 42);

      const Result<int> const_ok = 10;
      Result<Immovable> imm_const =
          const_ok.transform([](const int& x) { return Immovable(x + 5); });
      PW_TEST_EXPECT_TRUE(imm_const.has_value());
      PW_TEST_EXPECT_EQ(imm_const->val(), 15);

      Result<int> err_val = Status::PermissionDenied();
      Result<Immovable> imm_err =
          err_val.transform([](int x) { return Immovable(x); });
      PW_TEST_EXPECT_FALSE(imm_err.has_value());
      PW_TEST_EXPECT_EQ(imm_err.status(), Status::PermissionDenied());

      // Lvalue and_then returning Result<TrivialMoveOnly> and
      // Result<Immovable>.
      Result<TrivialMoveOnly> mo_ok = ok_val.and_then(
          [](int x) { return Result<TrivialMoveOnly>(std::in_place, x + 1); });
      PW_TEST_EXPECT_TRUE(mo_ok.has_value());
      PW_TEST_EXPECT_EQ(mo_ok->value, 22);

      Result<Immovable> imm_and_then = const_ok.and_then(
          [](int x) { return Result<Immovable>(std::in_place, x * 3); });
      PW_TEST_EXPECT_TRUE(imm_and_then.has_value());
      PW_TEST_EXPECT_EQ(imm_and_then->val(), 30);

      Result<Immovable> imm_and_then_err = err_val.and_then(
          [](int x) { return Result<Immovable>(std::in_place, x); });
      PW_TEST_EXPECT_FALSE(imm_and_then_err.has_value());
      PW_TEST_EXPECT_EQ(imm_and_then_err.status(), Status::PermissionDenied());

      // Result<void> transforming / and_then to Immovable.
      const Result<void> ok_void = OkStatus();
      Result<Immovable> void_imm =
          ok_void.transform([]() { return Immovable(77); });
      PW_TEST_EXPECT_TRUE(void_imm.has_value());
      PW_TEST_EXPECT_EQ(void_imm->val(), 77);

      Result<Immovable> void_and_then_imm = ok_void.and_then(
          []() { return Result<Immovable>(std::in_place, 88); });
      PW_TEST_EXPECT_TRUE(void_and_then_imm.has_value());
      PW_TEST_EXPECT_EQ(void_and_then_imm->val(), 88);

      // Result<const int> preserves const qualification when passed to
      // callbacks.
      Result<const int> const_elem = 5;
      Result<bool> was_const_lref = const_elem.transform([](auto& ref) {
        return std::is_const_v<std::remove_reference_t<decltype(ref)>>;
      });
      PW_TEST_EXPECT_TRUE(*was_const_lref);

      Result<bool> was_const_rref =
          std::move(const_elem).transform([](auto&& ref) {
            return std::is_const_v<std::remove_reference_t<decltype(ref)>>;
          });
      PW_TEST_EXPECT_TRUE(*was_const_rref);
    });

PW_CONSTEXPR_TEST(ResultOptionalAlign, OrElseZeroArgCallbacks, {
  const Result<int> ok_res = 12;
  const Result<int> err_res = Status::NotFound();

  // 0-arg returning fallback Result / value / status on const&.
  PW_TEST_EXPECT_EQ(*ok_res.or_else([]() { return 99; }), 12);
  PW_TEST_EXPECT_EQ(*err_res.or_else([]() { return 99; }), 99);
  PW_TEST_EXPECT_EQ(
      err_res.or_else([]() -> Result<int> { return Status::Aborted(); })
          .status(),
      Status::Aborted());

  // 0-arg returning void on const&.
  bool side_effect = false;
  PW_TEST_EXPECT_EQ(*ok_res.or_else([&side_effect]() { side_effect = true; }),
                    12);
  PW_TEST_EXPECT_FALSE(side_effect);
  PW_TEST_EXPECT_EQ(
      err_res.or_else([&side_effect]() { side_effect = true; }).status(),
      Status::NotFound());
  PW_TEST_EXPECT_TRUE(side_effect);

  // 0-arg on rvalue Result<TrivialMoveOnly>.
  Result<TrivialMoveOnly> mo_err = Status::Unavailable();
  Result<TrivialMoveOnly> mo_recovered = std::move(mo_err).or_else(
      []() { return Result<TrivialMoveOnly>(std::in_place, 55); });
  PW_TEST_EXPECT_TRUE(mo_recovered.has_value());
  PW_TEST_EXPECT_EQ(mo_recovered->value, 55);

  bool rval_side_effect = false;
  Result<TrivialMoveOnly> mo_err2 = Status::Unavailable();
  Result<TrivialMoveOnly> mo_still_err = std::move(mo_err2).or_else(
      [&rval_side_effect]() { rval_side_effect = true; });
  PW_TEST_EXPECT_TRUE(rval_side_effect);
  PW_TEST_EXPECT_FALSE(mo_still_err.has_value());

  // Variadic callback capable of both 1-arg and 0-arg prefers 1-arg (status).
  int arity_observed = -1;
  Result<int> var_ret = err_res.or_else([&arity_observed](auto... args) {
    arity_observed = sizeof...(args);
    return 7;
  });
  PW_TEST_EXPECT_EQ(arity_observed, 1);
  PW_TEST_EXPECT_EQ(*var_ret, 7);

  // 0-arg on Result<void>.
  const Result<void> void_err = Status::DataLoss();
  PW_TEST_EXPECT_TRUE(void_err.or_else([]() { return OkStatus(); }).ok());
  bool void_side_effect = false;
  PW_TEST_EXPECT_EQ(
      void_err.or_else([&void_side_effect]() { void_side_effect = true; })
          .status(),
      Status::DataLoss());
  PW_TEST_EXPECT_TRUE(void_side_effect);

  PW_TEST_EXPECT_TRUE(Result<void>(Status::DataLoss())
                          .or_else([]() { return OkStatus(); })
                          .ok());
});

// Heterogeneous Result<T, S> == Result<U, S> and sentinel exclusion.
static_assert(kCanCompare<Result<int>, Result<long>>);
static_assert(kCanCompare<Result<long>, Result<int>>);
static_assert(kCanCompare<Result<std::string_view>, Result<const char*>>);
static_assert(!kCanCompare<Result<int>, Result<std::string_view>>);
static_assert(!kCanCompare<Result<int>, TestResult<long>>);

struct FriendOnlyLhsEq {
  int x;
  bool operator==(int) const = delete;
  friend constexpr bool operator==(int lhs,
                                   const FriendOnlyLhsEq& rhs) noexcept {
    return lhs == rhs.x;
  }
};

#if __cplusplus < 202002L
static_assert(!kCanCompare<Result<FriendOnlyLhsEq>, int>);
#else
// C++20 rewrites `Result<FriendOnlyLhsEq> == int` to `int == Result<...>`.
static_assert(kCanCompare<Result<FriendOnlyLhsEq>, int>);
#endif
static_assert(kCanCompare<int, Result<FriendOnlyLhsEq>>);

PW_CONSTEXPR_TEST(ResultOptionalAlign, AsymmetricValueEquality, {
  const Result<FriendOnlyLhsEq> ok_val(std::in_place, FriendOnlyLhsEq{5});
  const Result<FriendOnlyLhsEq> err_val = Status::NotFound();
  PW_TEST_EXPECT_TRUE(5 == ok_val);
  PW_TEST_EXPECT_FALSE(5 != ok_val);
  PW_TEST_EXPECT_TRUE(6 != ok_val);
  PW_TEST_EXPECT_FALSE(5 == err_val);
  PW_TEST_EXPECT_TRUE(5 != err_val);
});

PW_CONSTEXPR_TEST(ResultOptionalAlign, HeterogeneousResultEquality, {
  const Result<int> ok_int = 42;
  const Result<long> ok_long_same = 42L;
  const Result<long> ok_long_diff = 43L;
  const Result<int> err_int = Status::NotFound();
  const Result<long> err_long_same = Status::NotFound();
  const Result<long> err_long_diff = Status::Internal();

  PW_TEST_EXPECT_TRUE(ok_int == ok_long_same);
  PW_TEST_EXPECT_FALSE(ok_int != ok_long_same);
  PW_TEST_EXPECT_FALSE(ok_int == ok_long_diff);
  PW_TEST_EXPECT_TRUE(ok_int != ok_long_diff);

  PW_TEST_EXPECT_FALSE(ok_int == err_long_same);
  PW_TEST_EXPECT_TRUE(ok_int != err_long_same);
  PW_TEST_EXPECT_FALSE(err_int == ok_long_same);
  PW_TEST_EXPECT_TRUE(err_int != ok_long_same);

  PW_TEST_EXPECT_TRUE(err_int == err_long_same);
  PW_TEST_EXPECT_FALSE(err_int != err_long_same);
  PW_TEST_EXPECT_FALSE(err_int == err_long_diff);
  PW_TEST_EXPECT_TRUE(err_int != err_long_diff);

  const Result<std::string_view> sv = "hello";
  const Result<const char*> cstr = "hello";
  PW_TEST_EXPECT_TRUE(sv == cstr);
  PW_TEST_EXPECT_TRUE(cstr == sv);
  PW_TEST_EXPECT_FALSE(sv != cstr);
});

struct EmptySentinelTag {};

class [[nodiscard]] StatusWithSentinel final
    : public StatusBase<StatusWithSentinel, HasValue::kYes> {
 public:
  constexpr StatusWithSentinel() noexcept = default;
  explicit constexpr StatusWithSentinel(HasValue code) noexcept
      : StatusBase(code) {}
  // Implicit conversion from EmptySentinelTag (analogous to std::nullopt_t).
  constexpr StatusWithSentinel(EmptySentinelTag) noexcept  // NOLINT
      : StatusBase(HasValue::kNo) {}
};

struct ComparableWithSentinel {
  int value;
  friend constexpr bool operator==(const ComparableWithSentinel&,
                                   EmptySentinelTag) noexcept {
    return true;
  }
  friend constexpr bool operator==(EmptySentinelTag,
                                   const ComparableWithSentinel&) noexcept {
    return true;
  }
};

PW_CONSTEXPR_TEST(ResultOptionalAlign, StatusConvertibleExcludedFromValueEq, {
  // Check that ComparableWithSentinel compares equal to EmptySentinelTag.
  PW_TEST_EXPECT_TRUE(ComparableWithSentinel{1} == EmptySentinelTag{});
  PW_TEST_EXPECT_TRUE(EmptySentinelTag{} == ComparableWithSentinel{1});

  // Even though ComparableWithSentinel == EmptySentinelTag returns true,
  // EmptySentinelTag is implicitly convertible to StatusWithSentinel, so
  // Result == EmptySentinelTag must unambiguously compare the Result's status.
  const Result<ComparableWithSentinel, StatusWithSentinel> engaged(
      ComparableWithSentinel{1});
  const Result<ComparableWithSentinel, StatusWithSentinel> disengaged(
      EmptySentinelTag{});

  PW_TEST_EXPECT_FALSE(engaged == EmptySentinelTag{});
  PW_TEST_EXPECT_TRUE(engaged != EmptySentinelTag{});
  PW_TEST_EXPECT_TRUE(disengaged == EmptySentinelTag{});
  PW_TEST_EXPECT_FALSE(disengaged != EmptySentinelTag{});
  PW_TEST_EXPECT_TRUE(EmptySentinelTag{} == disengaged);

  // Meanwhile, Result<pw_Status, Status> (where T == Status::Code) still
  // compares pw_Status against the held value.
  const Result<pw_Status, Status> held_code(PW_STATUS_NOT_FOUND);
  PW_TEST_EXPECT_TRUE(held_code.has_value());
  PW_TEST_EXPECT_TRUE(held_code == PW_STATUS_NOT_FOUND);
  PW_TEST_EXPECT_TRUE(held_code != PW_STATUS_OK);
  PW_TEST_EXPECT_TRUE(held_code == OkStatus());
});

// noexcept verification for Status, StatusBase, Result<T>, and Result<void>.
static_assert(noexcept(OkStatus()));
static_assert(noexcept(Status::NotFound()));
static_assert(noexcept(Status(PW_STATUS_INTERNAL)));
static_assert(noexcept(std::declval<const Status&>().ok()));
static_assert(noexcept(std::declval<const Status&>().code()));
static_assert(noexcept(std::declval<const Status&>().IsNotFound()));
static_assert(noexcept(std::declval<const Status&>().IgnoreError()));
static_assert(noexcept(std::declval<Status&>().Update(OkStatus())));
static_assert(noexcept(OkStatus() == Status::NotFound()));
static_assert(noexcept(OkStatus() != PW_STATUS_NOT_FOUND));
static_assert(noexcept(HasValueStatus().ok()));
static_assert(noexcept(HasValueStatus().code()));
static_assert(noexcept(HasValueStatus() == HasValue::kYes));

static_assert(noexcept(Result<int>()));
static_assert(noexcept(Result<int>(std::in_place, 5)));
static_assert(noexcept(std::declval<const Result<int>&>().ok()));
static_assert(noexcept(std::declval<const Result<int>&>().has_value()));
static_assert(noexcept(std::declval<const Result<int>&>().status()));
static_assert(noexcept(std::declval<const Result<int>&>().IgnoreError()));
static_assert(noexcept(*std::declval<Result<int>&>()));
static_assert(noexcept(*std::declval<const Result<int>&>()));
static_assert(noexcept(*std::declval<Result<int>&&>()));
static_assert(noexcept(*std::declval<const Result<int>&&>()));
static_assert(noexcept(std::declval<Result<int>&>().operator->()));
static_assert(noexcept(std::declval<const Result<int>&>().operator->()));
static_assert(noexcept(std::declval<Result<int>&>().emplace(5)));
static_assert(
    noexcept(std::declval<Result<int>&>().reset<PW_STATUS_INTERNAL>()));
static_assert(noexcept(std::declval<const Result<int>&>() == OkStatus()));
static_assert(noexcept(std::declval<const Result<int>&>() == PW_STATUS_OK));

static_assert(noexcept(Result<void>()));
static_assert(noexcept(Result<void>(std::in_place)));
static_assert(noexcept(Result<void>(OkStatus())));
static_assert(noexcept(std::declval<const Result<void>&>().ok()));
static_assert(noexcept(std::declval<const Result<void>&>().has_value()));
static_assert(noexcept(std::declval<const Result<void>&>().status()));
static_assert(noexcept(std::declval<const Result<void>&>().IgnoreError()));
static_assert(noexcept(std::declval<Result<void>&>().emplace()));
static_assert(
    noexcept(std::declval<Result<void>&>().reset<PW_STATUS_INTERNAL>()));
static_assert(noexcept(std::declval<const Result<void>&>() == Result<void>()));
static_assert(noexcept(std::declval<const Result<void>&>() == OkStatus()));
static_assert(noexcept(std::declval<const Result<void>&>() == PW_STATUS_OK));

#if PW_NC_TEST(ResultRequiresEnumStatusCode)
PW_NC_EXPECT("pw::Result only supports status types with enum codes");
class [[nodiscard]] IntCodeStatus final
    : public StatusBase<IntCodeStatus, 200> {
 public:
  constexpr IntCodeStatus() = default;
  explicit constexpr IntCodeStatus(int code) : StatusBase(code) {}
};
void ResultRequiresEnumStatusCode() {
  [[maybe_unused]] Result<int, IntCodeStatus> r{IntCodeStatus(404)};
}
#elif PW_NC_TEST(ResetRequiresCodeType)
PW_NC_EXPECT_CLANG("no matching member function for call to 'reset'");
PW_NC_EXPECT_GCC("no matching function for call to");
void ResetRequiresCodeType() {
  TestResult<int> r(1);
  r.reset(1);
}
#elif PW_NC_TEST(ResetCannotUseOkCode)
PW_NC_EXPECT("Cannot set status to an OK status; set the value instead");
void ResetCannotUseOkCode() {
  Result<Status> r = OkStatus();
  r.reset<PW_STATUS_OK>();
}
#elif PW_NC_TEST(VoidResetCannotUseOkCode)
PW_NC_EXPECT("Cannot set status to an OK status; use emplace");
void VoidResetCannotUseOkCode() {
  Result<void> r = OkStatus();
  r.reset<PW_STATUS_OK>();
}
#elif PW_NC_TEST(CustomStatusNoDefaultConstructor)
PW_NC_EXPECT_CLANG("no matching constructor for initialization");
PW_NC_EXPECT_GCC("no matching function for call to");
void CustomStatusNoDefaultConstructor() {
  [[maybe_unused]] Result<int, HasValueStatus> r;
}
#elif PW_NC_TEST(TryDisallowedForResultStatus)
PW_NC_EXPECT("PW_TRY, PW_CHECK_OK, and PW_ASSERT_OK cannot be used directly");
Status TryDisallowedForResultStatus(Result<Status> r) {
  PW_TRY(r);
  return OkStatus();
}
#elif PW_NC_TEST(TryAssignDisallowedForResultStatus)
PW_NC_EXPECT("PW_TRY, PW_CHECK_OK, and PW_ASSERT_OK cannot be used directly");
Status TryAssignDisallowedForResultStatus(Result<Status> r) {
  PW_TRY_ASSIGN([[maybe_unused]] Status s, r);
  return OkStatus();
}
#elif PW_NC_TEST(ResultRejectsReferenceType)
PW_NC_EXPECT("pw::Result does not support reference types");
void ResultRejectsReferenceType() {
  [[maybe_unused]] Result<int&>* ptr = nullptr;
  static_cast<void>(sizeof(*ptr));
}
#elif PW_NC_TEST(ResultRejectsArrayType)
PW_NC_EXPECT("pw::Result does not support array types");
void ResultRejectsArrayType() {
  [[maybe_unused]] Result<int[4]>* ptr = nullptr;
  static_cast<void>(sizeof(*ptr));
}
#elif PW_NC_TEST(ResultRejectsFunctionType)
PW_NC_EXPECT("pw::Result requires an object type");
void ResultRejectsFunctionType() {
  [[maybe_unused]] Result<int()>* ptr = nullptr;
  static_cast<void>(sizeof(*ptr));
}
#elif PW_NC_TEST(ResultRejectsInPlaceType)
PW_NC_EXPECT("pw::Result cannot wrap std::in_place_t");
void ResultRejectsInPlaceType() {
  [[maybe_unused]] Result<std::in_place_t>* ptr = nullptr;
  static_cast<void>(sizeof(*ptr));
}
#elif PW_NC_TEST(ResultRejectsConstInPlaceType)
PW_NC_EXPECT("pw::Result cannot wrap std::in_place_t");
void ResultRejectsConstInPlaceType() {
  [[maybe_unused]] Result<const std::in_place_t>* ptr = nullptr;
  static_cast<void>(sizeof(*ptr));
}
#elif PW_NC_TEST(ValueOrRequiresCopyConstructible)
PW_NC_EXPECT("requires T to be copy constructible");
int ValueOrRequiresCopyConstructible() {
  const Result<MoveOnly> r(std::in_place, 1);
  return r.value_or(MoveOnly(2)).value;
}
#elif PW_NC_TEST(AndThenRequiresResultReturn)
PW_NC_EXPECT("Fn must return a pw::Result");
void AndThenRequiresResultReturn() {
  static_cast<void>(Result<int>(1).and_then([](int x) { return x; }));
}
#elif PW_NC_TEST(AndThenRequiresMatchingStatusType)
PW_NC_EXPECT("Fn must return a pw::Result with the same status_type");
void AndThenRequiresMatchingStatusType() {
  static_cast<void>(
      Result<int>(1).and_then([](int x) { return TestResult<int>(x); }));
}
#elif PW_NC_TEST(OrElseRequiresConvertibleReturn)
PW_NC_EXPECT("Fn must be convertible to a pw::Result");
void OrElseRequiresConvertibleReturn() {
  static_cast<void>(
      Result<int>(Status::NotFound()).or_else([]() { return "bad"; }));
}
#endif  // PW_NC_TEST

}  // namespace
}  // namespace pw
