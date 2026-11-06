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

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "pw_compilation_testing/negative_compilation.h"
#include "pw_containers/internal/test_helpers.h"
#include "pw_result/result.h"
#include "pw_result_private/test_helpers.h"
#include "pw_status/try.h"
#include "pw_unit_test/constexpr.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::HasValue;
using ::pw::Optional;
using ::pw::containers::test::Counter;
using ::pw::containers::test::MoveOnly;
using ::pw::containers::test::TrivialMoveOnly;
using ::pw::internal_result::test::ConvertibleFromInt;
using ::pw::internal_result::test::ConvertibleToBool;
using ::pw::internal_result::test::Copyable;
using ::pw::internal_result::test::CopyAssignNotCtor;
using ::pw::internal_result::test::CopyCtorNotAssign;
using ::pw::internal_result::test::ExplicitConstructibleFromInt;
using ::pw::internal_result::test::InitListConstructible;
using ::pw::internal_result::test::MoveAssignNotCtor;
using ::pw::internal_result::test::MoveCtorNotAssign;

static_assert(sizeof(pw::Optional<uint8_t>) == sizeof(std::optional<uint8_t>));
static_assert(sizeof(pw::Optional<uint32_t>) ==
              sizeof(std::optional<uint32_t>));

static_assert(std::is_trivially_destructible_v<pw::Optional<int>>);
static_assert(std::is_trivially_copy_constructible_v<pw::Optional<int>>);
static_assert(std::is_trivially_move_constructible_v<pw::Optional<int>>);
static_assert(std::is_trivially_copy_assignable_v<pw::Optional<int>>);
static_assert(std::is_trivially_move_assignable_v<pw::Optional<int>>);
static_assert(std::is_trivially_copyable_v<pw::Optional<int>>);

static_assert(std::is_trivially_destructible_v<pw::Optional<TrivialMoveOnly>>);
static_assert(!std::is_trivially_destructible_v<pw::Optional<Counter>>);

static_assert(noexcept(Optional<int>()));
static_assert(noexcept(Optional<int>(std::nullopt)));
static_assert(noexcept(Optional<int>(5)));
static_assert(noexcept(Optional<int>(std::in_place, 5)));
static_assert(noexcept(std::declval<Optional<int>&>() = std::nullopt));
static_assert(noexcept(std::declval<Optional<int>&>() = 5));
static_assert(noexcept(std::declval<Optional<int>&>().reset()));
static_assert(
    noexcept(static_cast<bool>(std::declval<const Optional<int>&>())));
static_assert(noexcept(std::declval<const Optional<int>&>() == std::nullopt));
static_assert(noexcept(pw::make_optional(5)));
static_assert(noexcept(pw::make_optional<int>(5)));

static_assert(std::is_copy_constructible_v<Optional<CopyCtorNotAssign>>);
static_assert(std::is_move_constructible_v<Optional<CopyCtorNotAssign>>);
static_assert(!std::is_copy_assignable_v<Optional<CopyCtorNotAssign>>);
static_assert(!std::is_move_assignable_v<Optional<CopyCtorNotAssign>>);

static_assert(!std::is_copy_constructible_v<Optional<MoveCtorNotAssign>>);
static_assert(std::is_move_constructible_v<Optional<MoveCtorNotAssign>>);
static_assert(!std::is_copy_assignable_v<Optional<MoveCtorNotAssign>>);
static_assert(!std::is_move_assignable_v<Optional<MoveCtorNotAssign>>);

static_assert(!std::is_copy_constructible_v<Optional<CopyAssignNotCtor>>);
static_assert(std::is_move_constructible_v<Optional<CopyAssignNotCtor>>);
static_assert(!std::is_copy_assignable_v<Optional<CopyAssignNotCtor>>);
static_assert(std::is_move_assignable_v<Optional<CopyAssignNotCtor>>);

static_assert(!std::is_copy_constructible_v<Optional<MoveAssignNotCtor>>);
static_assert(!std::is_move_constructible_v<Optional<MoveAssignNotCtor>>);
static_assert(!std::is_copy_assignable_v<Optional<MoveAssignNotCtor>>);
static_assert(!std::is_move_assignable_v<Optional<MoveAssignNotCtor>>);

template <typename OptT>
void TestCommonOptional() {
  using T = typename OptT::value_type;

  OptT empty;
  EXPECT_FALSE(empty.has_value());
  EXPECT_FALSE(static_cast<bool>(empty));

  OptT empty_nullopt = std::nullopt;
  EXPECT_FALSE(empty_nullopt.has_value());

  OptT val(T(42));
  EXPECT_TRUE(val.has_value());
  EXPECT_TRUE(static_cast<bool>(val));
  EXPECT_EQ(val.value().value, 42);
  EXPECT_EQ((*val).value, 42);
  EXPECT_EQ(val->value, 42);

  if constexpr (std::is_copy_constructible_v<T>) {
    OptT copy = val;
    EXPECT_TRUE(copy.has_value());
    EXPECT_EQ(copy->value, 42);
  }

  OptT moved = std::move(val);
  EXPECT_TRUE(moved.has_value());
  EXPECT_EQ(moved->value, 42);

  OptT emp;
  emp.emplace(100);
  EXPECT_TRUE(emp.has_value());
  EXPECT_EQ(emp->value, 100);
  emp.reset();
  EXPECT_FALSE(emp.has_value());
}

TEST(OptionalTest, Common_StdOptional_Copyable) {
  TestCommonOptional<std::optional<Copyable>>();
}

TEST(OptionalTest, Common_PwOptional_Copyable) {
  TestCommonOptional<Optional<Copyable>>();
}

TEST(OptionalTest, Common_StdOptional_MoveOnly) {
  TestCommonOptional<std::optional<MoveOnly>>();
}

TEST(OptionalTest, Common_PwOptional_MoveOnly) {
  TestCommonOptional<Optional<MoveOnly>>();
}

TEST(OptionalTest, Common_StdOptional_CopyCtorNotAssign) {
  TestCommonOptional<std::optional<CopyCtorNotAssign>>();
}

TEST(OptionalTest, Common_PwOptional_CopyCtorNotAssign) {
  TestCommonOptional<Optional<CopyCtorNotAssign>>();
}

TEST(OptionalTest, Common_StdOptional_MoveCtorNotAssign) {
  TestCommonOptional<std::optional<MoveCtorNotAssign>>();
}

TEST(OptionalTest, Common_PwOptional_MoveCtorNotAssign) {
  TestCommonOptional<Optional<MoveCtorNotAssign>>();
}

PW_CONSTEXPR_TEST(OptionalTest, DefaultAndNulloptConstruction, {
  Optional<int> def;
  Optional<int> brace = {};
  Optional<int> from_nullopt = std::nullopt;

  PW_TEST_EXPECT_FALSE(def.has_value());
  PW_TEST_EXPECT_FALSE(def.ok());
  PW_TEST_EXPECT_FALSE(static_cast<bool>(def));
  PW_TEST_EXPECT_FALSE(brace.has_value());
  PW_TEST_EXPECT_FALSE(from_nullopt.has_value());

  PW_TEST_EXPECT_EQ(def, std::nullopt);
  PW_TEST_EXPECT_EQ(std::nullopt, def);
  PW_TEST_EXPECT_EQ(def, from_nullopt);
});

PW_CONSTEXPR_TEST(OptionalTest, BraceAssignmentResetsToNullopt, {
  Optional<int> scalar = 42;
  PW_TEST_EXPECT_TRUE(scalar.has_value());
  scalar = {};
  PW_TEST_EXPECT_FALSE(scalar.has_value());

  Optional<bool> boolean = false;
  PW_TEST_EXPECT_TRUE(boolean.has_value());
  boolean = {};
  PW_TEST_EXPECT_FALSE(boolean.has_value());

  Optional<Copyable> copyable = Copyable(7);
  PW_TEST_EXPECT_TRUE(copyable.has_value());
  copyable = {};
  PW_TEST_EXPECT_FALSE(copyable.has_value());

  Optional<TrivialMoveOnly> move_only(std::in_place, 9);
  PW_TEST_EXPECT_TRUE(move_only.has_value());
  move_only = {};
  PW_TEST_EXPECT_FALSE(move_only.has_value());

  Optional<ExplicitConstructibleFromInt> non_default(std::in_place, 11);
  PW_TEST_EXPECT_TRUE(non_default.has_value());
  non_default = {};
  PW_TEST_EXPECT_FALSE(non_default.has_value());

  Optional<HasValue> has_val = HasValue();
  PW_TEST_EXPECT_TRUE(has_val.has_value());
  has_val = {};
  PW_TEST_EXPECT_FALSE(has_val.has_value());
});

PW_CONSTEXPR_TEST(OptionalTest, OptionalBool, {
  Optional<bool> empty;
  Optional<bool> engaged_false = false;
  Optional<bool> engaged_true = true;

  PW_TEST_EXPECT_FALSE(empty.has_value());
  PW_TEST_EXPECT_FALSE(static_cast<bool>(empty));

  PW_TEST_EXPECT_TRUE(engaged_false.has_value());
  PW_TEST_EXPECT_TRUE(engaged_false.ok());
  PW_TEST_EXPECT_TRUE(static_cast<bool>(engaged_false));
  PW_TEST_EXPECT_FALSE(*engaged_false);
  PW_TEST_EXPECT_EQ(engaged_false, false);
  PW_TEST_EXPECT_NE(engaged_false, true);
  PW_TEST_EXPECT_NE(engaged_false, std::nullopt);

  PW_TEST_EXPECT_TRUE(engaged_true.has_value());
  PW_TEST_EXPECT_TRUE(static_cast<bool>(engaged_true));
  PW_TEST_EXPECT_TRUE(*engaged_true);
  PW_TEST_EXPECT_EQ(engaged_true, true);
  PW_TEST_EXPECT_NE(engaged_true, false);

  engaged_true = false;
  PW_TEST_EXPECT_TRUE(engaged_true.has_value());
  PW_TEST_EXPECT_FALSE(*engaged_true);
  engaged_true = std::nullopt;
  PW_TEST_EXPECT_FALSE(engaged_true.has_value());
});

PW_CONSTEXPR_TEST_IF_CPP20(OptionalTest, OptionalBoolConvertingAndDisengaged, {
  Optional<bool> opt;
  PW_TEST_EXPECT_FALSE(opt.has_value());
  opt = false;
  PW_TEST_EXPECT_TRUE(opt.has_value());
  PW_TEST_EXPECT_FALSE(*opt);
  opt = std::nullopt;
  PW_TEST_EXPECT_FALSE(opt.has_value());
  opt = true;
  PW_TEST_EXPECT_TRUE(opt.has_value());
  PW_TEST_EXPECT_TRUE(*opt);

  Optional<ConvertibleToBool> conv_false(std::in_place,
                                         ConvertibleToBool(false));
  Optional<bool> from_conv = conv_false;
  PW_TEST_EXPECT_TRUE(from_conv.has_value());
  PW_TEST_EXPECT_FALSE(*from_conv);

  Optional<ConvertibleToBool> conv_empty;
  Optional<bool> from_empty = conv_empty;
  PW_TEST_EXPECT_FALSE(from_empty.has_value());

  Optional<bool> direct_from_empty(conv_empty);
  PW_TEST_EXPECT_FALSE(direct_from_empty.has_value());

  from_conv = conv_empty;
  PW_TEST_EXPECT_FALSE(from_conv.has_value());
  from_conv = conv_false;
  PW_TEST_EXPECT_TRUE(from_conv.has_value());
  PW_TEST_EXPECT_FALSE(*from_conv);

  // Non-Optional Result<bool, Status> can still be explicitly constructed from
  // Optional<T> via Optional's explicit operator bool().
  pw::Result<bool> res_true(Optional<int>(5));
  PW_TEST_EXPECT_TRUE(res_true.ok());
  PW_TEST_EXPECT_TRUE(*res_true);

  pw::Result<bool> res_false(Optional<int>{std::nullopt});
  PW_TEST_EXPECT_TRUE(res_false.ok());
  PW_TEST_EXPECT_FALSE(*res_false);
});

PW_CONSTEXPR_TEST(OptionalTest, OptionalHasValue, {
  Optional<HasValue> empty;
  Optional<HasValue> from_nullopt = std::nullopt;
  PW_TEST_EXPECT_FALSE(empty.has_value());
  PW_TEST_EXPECT_FALSE(from_nullopt.has_value());
  PW_TEST_EXPECT_EQ(empty, std::nullopt);
  PW_TEST_EXPECT_EQ(std::nullopt, empty);
  PW_TEST_EXPECT_NE(empty, HasValue());
  PW_TEST_EXPECT_NE(empty, HasValue(HasValue::kEmpty));

  Optional<HasValue> engaged_empty = HasValue(HasValue::kEmpty);
  PW_TEST_EXPECT_TRUE(engaged_empty.has_value());
  PW_TEST_EXPECT_EQ(*engaged_empty, HasValue(HasValue::kEmpty));
  PW_TEST_EXPECT_EQ(engaged_empty, HasValue(HasValue::kEmpty));
  PW_TEST_EXPECT_EQ(engaged_empty, HasValue::kEmpty);
  PW_TEST_EXPECT_NE(engaged_empty, std::nullopt);
  PW_TEST_EXPECT_NE(std::nullopt, engaged_empty);

  Optional<HasValue> engaged_ok = HasValue();
  PW_TEST_EXPECT_TRUE(engaged_ok.has_value());
  PW_TEST_EXPECT_EQ(*engaged_ok, HasValue());
  PW_TEST_EXPECT_EQ(engaged_ok, HasValue());
  PW_TEST_EXPECT_EQ(engaged_ok, HasValue::kHasValue);
  PW_TEST_EXPECT_NE(engaged_ok, std::nullopt);

  engaged_ok = HasValue(HasValue::kEmpty);
  PW_TEST_EXPECT_TRUE(engaged_ok.has_value());
  PW_TEST_EXPECT_EQ(*engaged_ok, HasValue(HasValue::kEmpty));

  engaged_ok = std::nullopt;
  PW_TEST_EXPECT_FALSE(engaged_ok.has_value());
  PW_TEST_EXPECT_EQ(engaged_ok, std::nullopt);
});

PW_CONSTEXPR_TEST(OptionalTest, NestedStdOptionalNulloptDisambiguation, {
  Optional<std::optional<int>> outer_empty = std::nullopt;
  PW_TEST_EXPECT_FALSE(outer_empty.has_value());
  PW_TEST_EXPECT_EQ(outer_empty, std::nullopt);
  PW_TEST_EXPECT_EQ(std::nullopt, outer_empty);

  Optional<std::optional<int>> outer_with_empty_inner(std::in_place,
                                                      std::nullopt);
  PW_TEST_EXPECT_TRUE(outer_with_empty_inner.has_value());
  PW_TEST_EXPECT_FALSE(outer_with_empty_inner->has_value());
  PW_TEST_EXPECT_NE(outer_with_empty_inner, std::nullopt);

  outer_with_empty_inner = std::nullopt;
  PW_TEST_EXPECT_FALSE(outer_with_empty_inner.has_value());
  PW_TEST_EXPECT_EQ(outer_with_empty_inner, std::nullopt);
});

PW_CONSTEXPR_TEST(OptionalTest, ImplicitAndExplicitConversion, {
  Optional<ConvertibleFromInt> opt = 123;
  PW_TEST_EXPECT_TRUE(opt.has_value());
  PW_TEST_EXPECT_EQ(opt->value, 123);

  Optional<ExplicitConstructibleFromInt> explicit_opt(456);
  PW_TEST_EXPECT_TRUE(explicit_opt.has_value());
  PW_TEST_EXPECT_EQ(explicit_opt->value, 456);

  Optional<ExplicitConstructibleFromInt> in_place_opt(std::in_place, 789);
  PW_TEST_EXPECT_TRUE(in_place_opt.has_value());
  PW_TEST_EXPECT_EQ(in_place_opt->value, 789);
});

PW_CONSTEXPR_TEST(OptionalTest, CopyAndMoveAssignment, {
  Optional<Copyable> opt1 = Copyable(1);
  Optional<Copyable> opt2 = Copyable(2);
  opt1 = opt2;
  PW_TEST_EXPECT_EQ(opt1->value, 2);
  opt1 = std::nullopt;
  PW_TEST_EXPECT_FALSE(opt1.has_value());

  Optional<TrivialMoveOnly> mv1(std::in_place, 1);
  Optional<TrivialMoveOnly> mv2(std::in_place, 2);
  mv1 = std::move(mv2);
  PW_TEST_EXPECT_EQ(mv1->value, 2);
  mv1.reset();
  PW_TEST_EXPECT_FALSE(mv1.has_value());
});

TEST(OptionalTest, Destructor_CalledOnResetAndScopeExit) {
  Counter::Reset();
  {
    Optional<Counter> opt(std::in_place, 5);
    EXPECT_TRUE(opt.has_value());
    opt.reset();
    EXPECT_FALSE(opt.has_value());
    EXPECT_EQ(Counter::created, 1);
    EXPECT_EQ(Counter::destroyed, 1);

    opt.emplace(10);
    EXPECT_TRUE(opt.has_value());
    opt = std::nullopt;
    EXPECT_FALSE(opt.has_value());
    EXPECT_EQ(Counter::created, 2);
    EXPECT_EQ(Counter::destroyed, 2);

    opt.emplace(15);
    EXPECT_TRUE(opt.has_value());
    opt = {};
    EXPECT_FALSE(opt.has_value());
    EXPECT_EQ(Counter::created, 3);
    EXPECT_EQ(Counter::destroyed, 3);

    opt.emplace(20);
  }
  EXPECT_EQ(Counter::created, 4);
  EXPECT_EQ(Counter::destroyed, 4);
}

TEST(OptionalTest, ConvertingConstructorsAndAssignment) {
  Optional<int> source(42);
  Optional<long> dest = source;
  EXPECT_TRUE(dest.has_value());
  EXPECT_EQ(*dest, 42L);

  dest = Optional<int>(99);
  EXPECT_TRUE(dest.has_value());
  EXPECT_EQ(*dest, 99L);

  Optional<int> empty_source;
  dest = empty_source;
  EXPECT_FALSE(dest.has_value());

  dest = Optional<int>(123);
  EXPECT_TRUE(dest.has_value());
  EXPECT_EQ(*dest, 123L);
}

PW_CONSTEXPR_TEST(OptionalTest, HeterogeneousEquality, {
  Optional<int> a = 42;
  Optional<long> b = 42L;
  Optional<long> c = 99L;
  Optional<long> empty;

  PW_TEST_EXPECT_EQ(a, b);
  PW_TEST_EXPECT_NE(a, c);
  PW_TEST_EXPECT_NE(a, empty);
  PW_TEST_EXPECT_EQ(Optional<int>(), empty);

  Optional<std::string_view> sv = "hello";
  Optional<const char*> cstr = "hello";
  PW_TEST_EXPECT_EQ(sv, cstr);
});

struct CustomNotEqualOnly {
  int value;
  friend constexpr bool operator!=(const CustomNotEqualOnly& lhs,
                                   const CustomNotEqualOnly& rhs) noexcept {
    return lhs.value != rhs.value;
  }
  friend constexpr bool operator!=(const CustomNotEqualOnly& lhs,
                                   int rhs) noexcept {
    return lhs.value != rhs;
  }
  friend constexpr bool operator!=(int lhs,
                                   const CustomNotEqualOnly& rhs) noexcept {
    return lhs != rhs.value;
  }
};

PW_CONSTEXPR_TEST(OptionalTest, RelationalComparisons, {
  Optional<int> empty1;
  Optional<int> empty2;
  Optional<int> one = 1;
  Optional<int> two = 2;
  Optional<long> two_long = 2L;
  Optional<long> three_long = 3L;
  Optional<long> empty_long;

  // Optional vs Optional (homogeneous and heterogeneous)
  PW_TEST_EXPECT_FALSE(empty1 < empty2);
  PW_TEST_EXPECT_TRUE(empty1 <= empty2);
  PW_TEST_EXPECT_FALSE(empty1 > empty2);
  PW_TEST_EXPECT_TRUE(empty1 >= empty2);

  PW_TEST_EXPECT_TRUE(empty1 < one);
  PW_TEST_EXPECT_TRUE(empty1 <= one);
  PW_TEST_EXPECT_FALSE(empty1 > one);
  PW_TEST_EXPECT_FALSE(empty1 >= one);

  PW_TEST_EXPECT_FALSE(one < empty1);
  PW_TEST_EXPECT_FALSE(one <= empty1);
  PW_TEST_EXPECT_TRUE(one > empty1);
  PW_TEST_EXPECT_TRUE(one >= empty1);

  PW_TEST_EXPECT_TRUE(one < two);
  PW_TEST_EXPECT_TRUE(one <= two);
  PW_TEST_EXPECT_FALSE(one > two);
  PW_TEST_EXPECT_FALSE(one >= two);

  PW_TEST_EXPECT_FALSE(two < two_long);
  PW_TEST_EXPECT_TRUE(two <= two_long);
  PW_TEST_EXPECT_FALSE(two > two_long);
  PW_TEST_EXPECT_TRUE(two >= two_long);

  PW_TEST_EXPECT_TRUE(two < three_long);
  PW_TEST_EXPECT_TRUE(two <= three_long);
  PW_TEST_EXPECT_FALSE(two > three_long);
  PW_TEST_EXPECT_FALSE(two >= three_long);

  PW_TEST_EXPECT_FALSE(two < empty_long);
  PW_TEST_EXPECT_TRUE(two > empty_long);
  PW_TEST_EXPECT_TRUE(empty_long < two);
  PW_TEST_EXPECT_FALSE(empty_long > two);

  // Optional vs std::nullopt
  PW_TEST_EXPECT_FALSE(empty1 < std::nullopt);
  PW_TEST_EXPECT_FALSE(std::nullopt < empty1);
  PW_TEST_EXPECT_TRUE(empty1 <= std::nullopt);
  PW_TEST_EXPECT_TRUE(std::nullopt <= empty1);
  PW_TEST_EXPECT_FALSE(empty1 > std::nullopt);
  PW_TEST_EXPECT_FALSE(std::nullopt > empty1);
  PW_TEST_EXPECT_TRUE(empty1 >= std::nullopt);
  PW_TEST_EXPECT_TRUE(std::nullopt >= empty1);

  PW_TEST_EXPECT_FALSE(one < std::nullopt);
  PW_TEST_EXPECT_TRUE(std::nullopt < one);
  PW_TEST_EXPECT_FALSE(one <= std::nullopt);
  PW_TEST_EXPECT_TRUE(std::nullopt <= one);
  PW_TEST_EXPECT_TRUE(one > std::nullopt);
  PW_TEST_EXPECT_FALSE(std::nullopt > one);
  PW_TEST_EXPECT_TRUE(one >= std::nullopt);
  PW_TEST_EXPECT_FALSE(std::nullopt >= one);

  // Optional<HasValue> vs std::nullopt
  Optional<HasValue> hv_empty;
  Optional<HasValue> hv_engaged = HasValue(HasValue::kEmpty);
  PW_TEST_EXPECT_FALSE(hv_empty < std::nullopt);
  PW_TEST_EXPECT_FALSE(std::nullopt < hv_empty);
  PW_TEST_EXPECT_TRUE(hv_empty <= std::nullopt);
  PW_TEST_EXPECT_TRUE(std::nullopt <= hv_empty);
  PW_TEST_EXPECT_TRUE(std::nullopt < hv_engaged);
  PW_TEST_EXPECT_TRUE(hv_engaged > std::nullopt);
  PW_TEST_EXPECT_FALSE(hv_engaged <= std::nullopt);
  PW_TEST_EXPECT_TRUE(hv_engaged >= std::nullopt);

  // Optional vs value U (both operand orders)
  PW_TEST_EXPECT_TRUE(empty1 < 0);
  PW_TEST_EXPECT_TRUE(empty1 <= 0);
  PW_TEST_EXPECT_FALSE(empty1 > 0);
  PW_TEST_EXPECT_FALSE(empty1 >= 0);
  PW_TEST_EXPECT_FALSE(0 < empty1);
  PW_TEST_EXPECT_FALSE(0 <= empty1);
  PW_TEST_EXPECT_TRUE(0 > empty1);
  PW_TEST_EXPECT_TRUE(0 >= empty1);

  PW_TEST_EXPECT_TRUE(one < 2);
  PW_TEST_EXPECT_TRUE(one <= 2);
  PW_TEST_EXPECT_FALSE(one > 2);
  PW_TEST_EXPECT_FALSE(one >= 2);
  PW_TEST_EXPECT_FALSE(2 < one);
  PW_TEST_EXPECT_FALSE(2 <= one);
  PW_TEST_EXPECT_TRUE(2 > one);
  PW_TEST_EXPECT_TRUE(2 >= one);

  PW_TEST_EXPECT_FALSE(one < 1L);
  PW_TEST_EXPECT_TRUE(one <= 1L);
  PW_TEST_EXPECT_FALSE(one > 1L);
  PW_TEST_EXPECT_TRUE(one >= 1L);
  PW_TEST_EXPECT_FALSE(1L < one);
  PW_TEST_EXPECT_TRUE(1L <= one);
  PW_TEST_EXPECT_FALSE(1L > one);
  PW_TEST_EXPECT_TRUE(1L >= one);

  // Separate operator!= support
  Optional<CustomNotEqualOnly> ne_a(std::in_place, CustomNotEqualOnly{1});
  Optional<CustomNotEqualOnly> ne_b(std::in_place, CustomNotEqualOnly{2});
  Optional<CustomNotEqualOnly> ne_empty;
  PW_TEST_EXPECT_TRUE(ne_a != ne_b);
  PW_TEST_EXPECT_FALSE(ne_a != CustomNotEqualOnly{1});
  PW_TEST_EXPECT_TRUE(ne_a != CustomNotEqualOnly{2});
  PW_TEST_EXPECT_TRUE(ne_a != 2);
  PW_TEST_EXPECT_TRUE(2 != ne_a);
  PW_TEST_EXPECT_FALSE(1 != ne_a);
  PW_TEST_EXPECT_TRUE(ne_empty != 1);
  PW_TEST_EXPECT_TRUE(1 != ne_empty);
});

PW_CONSTEXPR_TEST(OptionalTest, ValueOrAndMakeOptional, {
  auto opt = pw::make_optional(42);
  PW_TEST_EXPECT_TRUE(opt.has_value());
  PW_TEST_EXPECT_EQ(opt.value_or(0), 42);
  PW_TEST_EXPECT_EQ(opt.value_or({}), 42);

  Optional<int> empty;
  PW_TEST_EXPECT_EQ(empty.value_or(99), 99);
  PW_TEST_EXPECT_EQ(empty.value_or({}), 0);

  auto in_place_opt = pw::make_optional<ExplicitConstructibleFromInt>(77);
  PW_TEST_EXPECT_TRUE(in_place_opt.has_value());
  PW_TEST_EXPECT_EQ(in_place_opt->value, 77);

  auto ilist_opt = pw::make_optional<InitListConstructible>({1, 2, 3}, 10);
  PW_TEST_EXPECT_TRUE(ilist_opt.has_value());
  PW_TEST_EXPECT_EQ(ilist_opt->sum, 60);
});

PW_CONSTEXPR_TEST_IF_CPP20(OptionalTest, Swap, {
  Optional<int> a = 10;
  Optional<int> b = 20;
  a.swap(b);
  PW_TEST_EXPECT_EQ(*a, 20);
  PW_TEST_EXPECT_EQ(*b, 10);

  Optional<int> empty;
  swap(a, empty);
  PW_TEST_EXPECT_FALSE(a.has_value());
  PW_TEST_EXPECT_TRUE(empty.has_value());
  PW_TEST_EXPECT_EQ(*empty, 20);
});

PW_CONSTEXPR_TEST(OptionalTest, MonadicOperations, {
  Optional<int> val = 21;
  auto doubled = val.transform([](int x) { return x * 2; });
  PW_TEST_EXPECT_TRUE(doubled.has_value());
  PW_TEST_EXPECT_EQ(*doubled, 42);

  auto chained = doubled.and_then([](int x) -> Optional<int> {
    return x > 0 ? Optional<int>(x + 1) : std::nullopt;
  });
  PW_TEST_EXPECT_TRUE(chained.has_value());
  PW_TEST_EXPECT_EQ(*chained, 43);

  Optional<int> empty;
  auto fallback = empty.or_else([]() -> Optional<int> { return 100; });
  PW_TEST_EXPECT_TRUE(fallback.has_value());
  PW_TEST_EXPECT_EQ(*fallback, 100);
});

PW_CONSTEXPR_TEST(OptionalTest, PwTryIntegrationConstexpr, {
  constexpr auto get_val = [](bool ok, int v) -> Optional<int> {
    if (!ok) {
      return std::nullopt;
    }
    return v;
  };
  constexpr auto compute = [get_val](bool ok_a, bool ok_b) -> Optional<int> {
    PW_TRY_ASSIGN(int x, get_val(ok_a, 10));
    PW_TRY_ASSIGN(int y, get_val(ok_b, 20));
    return x + y;
  };

  PW_TEST_EXPECT_EQ(compute(true, true), 30);
  PW_TEST_EXPECT_EQ(compute(true, false), std::nullopt);
  PW_TEST_EXPECT_EQ(compute(false, true), std::nullopt);
});

TEST(OptionalTest, PwTryIntegration) {
  auto compute = [](Optional<int> a, Optional<int> b) -> Optional<int> {
    PW_TRY_ASSIGN(int x, a);
    PW_TRY_ASSIGN(int y, b);
    return x + y;
  };

  EXPECT_EQ(compute(10, 20), 30);
  EXPECT_EQ(compute(10, std::nullopt), std::nullopt);
  EXPECT_EQ(compute(std::nullopt, 20), std::nullopt);
}

PW_CONSTEXPR_TEST(OptionalTest, ResultWrappingOptionalWithNullopt, {
  // Constructing and assigning Result<Optional<int>> (where StatusType is
  // pw::Status) from std::nullopt initializes an OK Result holding an empty
  // inner Optional.
  pw::Result<Optional<int>> pw_opt_res = std::nullopt;
  PW_TEST_EXPECT_TRUE(pw_opt_res.ok());
  PW_TEST_EXPECT_FALSE(pw_opt_res->has_value());
  PW_TEST_EXPECT_TRUE(pw_opt_res == std::nullopt);
  PW_TEST_EXPECT_TRUE(std::nullopt == pw_opt_res);
  PW_TEST_EXPECT_FALSE(pw_opt_res != std::nullopt);
  PW_TEST_EXPECT_FALSE(std::nullopt != pw_opt_res);

  pw_opt_res = Optional<int>(5);
  PW_TEST_EXPECT_TRUE(pw_opt_res.ok());
  PW_TEST_EXPECT_TRUE(pw_opt_res != std::nullopt);
  pw_opt_res = std::nullopt;
  PW_TEST_EXPECT_TRUE(pw_opt_res.ok());
  PW_TEST_EXPECT_FALSE(pw_opt_res->has_value());

  // Conversely, Optional<Optional<int>> (where StatusType is HasValue) treats
  // std::nullopt as the outer Optional's empty state.
  Optional<Optional<int>> nested_opt = std::nullopt;
  PW_TEST_EXPECT_FALSE(nested_opt.has_value());
  PW_TEST_EXPECT_TRUE(nested_opt == std::nullopt);

  Optional<Optional<int>> engaged_nested = Optional<int>(5);
  PW_TEST_EXPECT_TRUE(engaged_nested.has_value());
  PW_TEST_EXPECT_TRUE(engaged_nested->has_value());
  engaged_nested = std::nullopt;
  PW_TEST_EXPECT_FALSE(engaged_nested.has_value());
});

TEST(OptionalTest, ResultWrappingStdOptionalWithNullopt) {
  // std::optional::operator=(std::nullopt_t) is not constexpr in C++17.
  pw::Result<std::optional<int>> std_opt_res = std::nullopt;
  EXPECT_TRUE(std_opt_res.ok());
  EXPECT_FALSE(std_opt_res->has_value());
  EXPECT_TRUE(std_opt_res == std::nullopt);
  EXPECT_TRUE(std::nullopt == std_opt_res);
  EXPECT_FALSE(std_opt_res != std::nullopt);
  EXPECT_FALSE(std::nullopt != std_opt_res);

  std_opt_res = std::optional<int>(5);
  EXPECT_TRUE(std_opt_res.ok());
  EXPECT_TRUE(std_opt_res != std::nullopt);
  std_opt_res = std::nullopt;
  EXPECT_TRUE(std_opt_res.ok());
  EXPECT_FALSE(std_opt_res->has_value());

  Optional<Optional<int>> nested_opt = std::nullopt;
  EXPECT_FALSE(nested_opt.has_value());
  nested_opt = Optional<int>(5);
  EXPECT_TRUE(nested_opt.has_value());
  EXPECT_TRUE(nested_opt->has_value());
  nested_opt = std::nullopt;
  EXPECT_FALSE(nested_opt.has_value());
}

template <typename Signature>
struct CallbackWithResult {
  friend constexpr bool operator==(const CallbackWithResult&,
                                   std::nullptr_t) noexcept {
    return true;
  }
  friend constexpr bool operator==(std::nullptr_t,
                                   const CallbackWithResult&) noexcept {
    return true;
  }
  friend constexpr bool operator!=(const CallbackWithResult&,
                                   std::nullptr_t) noexcept {
    return false;
  }
  friend constexpr bool operator!=(std::nullptr_t,
                                   const CallbackWithResult&) noexcept {
    return false;
  }
};

PW_CONSTEXPR_TEST(OptionalTest, AdlDoesNotRecurseOnAssociatedResultType, {
  // Comparing a type whose template arguments mention Result<T> or Optional<T>
  // (making Result<T> an associated class for ADL) must not trigger recursive
  // constraint instantiation in Result's comparison operators.
  struct Measurement {};
  const CallbackWithResult<void(pw::Result<Measurement>)> res_cb{};
  PW_TEST_EXPECT_TRUE(res_cb == nullptr);
  PW_TEST_EXPECT_TRUE(nullptr == res_cb);
  PW_TEST_EXPECT_FALSE(res_cb != nullptr);
  PW_TEST_EXPECT_FALSE(nullptr != res_cb);

  const CallbackWithResult<void(Optional<Measurement>)> opt_cb{};
  PW_TEST_EXPECT_TRUE(opt_cb == nullptr);
  PW_TEST_EXPECT_TRUE(nullptr == opt_cb);
  PW_TEST_EXPECT_FALSE(opt_cb != nullptr);
  PW_TEST_EXPECT_FALSE(nullptr != opt_cb);
});

#if PW_NC_TEST(OptionalRejectsVoid)
PW_NC_EXPECT("pw::Optional<void> is not supported");
void OptionalRejectsVoid() {
  [[maybe_unused]] Optional<void>* ptr = nullptr;
  static_cast<void>(sizeof(*ptr));
}
#elif PW_NC_TEST(OptionalRejectsNulloptType)
PW_NC_EXPECT("pw::Result cannot wrap std::nullopt_t");
void OptionalRejectsNulloptType() {
  [[maybe_unused]] Optional<std::nullopt_t>* ptr = nullptr;
  static_cast<void>(sizeof(*ptr));
}
#elif PW_NC_TEST(OptionalRejectsConstNulloptType)
PW_NC_EXPECT("pw::Result cannot wrap std::nullopt_t");
void OptionalRejectsConstNulloptType() {
  [[maybe_unused]] Optional<const std::nullopt_t>* ptr = nullptr;
  static_cast<void>(sizeof(*ptr));
}
#elif PW_NC_TEST(ResultHasNoOperatorBool)
PW_NC_EXPECT_CLANG(
    "no matching conversion for static_cast from 'pw::Result<int>' to 'bool'");
PW_NC_EXPECT_GCC(
    "invalid 'static_cast' from type 'pw::Result<int>' to type 'bool'");
bool ResultHasNoOperatorBool() { return static_cast<bool>(pw::Result<int>(1)); }
#elif PW_NC_TEST(ResultHasNoZeroArgReset)
PW_NC_EXPECT_CLANG("no matching member function for call to 'reset'");
PW_NC_EXPECT_GCC("no matching function for call to");
void ResultHasNoZeroArgReset() {
  pw::Result<int> r(1);
  r.reset();
}
#elif PW_NC_TEST(OptionalRejectsStatusReset)
PW_NC_EXPECT_CLANG("no matching member function for call to 'reset'");
PW_NC_EXPECT_GCC("no matching function for call to");
void OptionalRejectsStatusReset() {
  Optional<int> opt(1);
  opt.reset(HasValue(HasValue::kEmpty));
}
#elif PW_NC_TEST(OptionalRejectsCodeReset)
PW_NC_EXPECT_CLANG("no matching member function for call to 'reset'");
PW_NC_EXPECT_GCC("no matching function for call to");
void OptionalRejectsCodeReset() {
  Optional<int> opt(1);
  opt.reset(HasValue::kEmpty);
}
#endif  // PW_NC_TEST

}  // namespace
