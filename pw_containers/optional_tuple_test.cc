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

#include "pw_containers/optional_tuple.h"

#include <cstdint>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "pw_compilation_testing/negative_compilation.h"
#include "pw_containers/internal/test_helpers.h"
#include "pw_polyfill/language_feature_macros.h"
#include "pw_polyfill/standard.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::containers::test::Counter;
using ::pw::containers::test::MoveOnly;
using ::pw::containers::test::TrivialMoveOnly;

static_assert(
    sizeof(pw::OptionalTuple<uint32_t, uint8_t, uint8_t, uint8_t>) ==
        sizeof(std::tuple<uint32_t, uint8_t, uint8_t, uint8_t, uint8_t>),
    "OptionalTuple should pack like a tuple with one extra field");

TEST(OptionalTuple, Empty) {
  pw::OptionalTuple<> tuple;
  EXPECT_TRUE(tuple.empty());
  EXPECT_EQ(tuple.count(), 0u);
  EXPECT_EQ(tuple.size(), 0u);
}

TEST(OptionalTuple, Empty_Copy) {
  pw::OptionalTuple<> tuple;
  pw::OptionalTuple<> copy_constructed(tuple);
  EXPECT_TRUE(copy_constructed.empty());
  EXPECT_EQ(copy_constructed.count(), 0u);
  EXPECT_EQ(copy_constructed.size(), 0u);
}

TEST(OptionalTuple, Empty_CopyAssign) {
  pw::OptionalTuple<> tuple;
  pw::OptionalTuple<> copy_assigned = tuple;
  EXPECT_TRUE(copy_assigned.empty());
  EXPECT_EQ(copy_assigned.count(), 0u);
  EXPECT_EQ(copy_assigned.size(), 0u);
}

TEST(OptionalTuple, Empty_Move) {
  pw::OptionalTuple<> tuple;
  pw::OptionalTuple<> move_constructed(std::move(tuple));
  EXPECT_TRUE(move_constructed.empty());
  EXPECT_EQ(move_constructed.count(), 0u);
  EXPECT_EQ(move_constructed.size(), 0u);
}

TEST(OptionalTuple, Empty_MoveAssign) {
  pw::OptionalTuple<> tuple;
  pw::OptionalTuple<> move_assigned;
  move_assigned = pw::OptionalTuple<>();
  EXPECT_TRUE(move_assigned.empty());
  EXPECT_EQ(move_assigned.count(), 0u);
  EXPECT_EQ(move_assigned.size(), 0u);
}

TEST(OptionalTuple, DefaultConstruction) {
  pw::OptionalTuple<int, bool> tuple;
  EXPECT_TRUE(tuple.empty());
  EXPECT_FALSE(tuple.has_value<0>());
  EXPECT_FALSE(tuple.has_value<int>());

  EXPECT_FALSE(tuple.has_value<1>());
  EXPECT_FALSE(tuple.has_value<bool>());

#if PW_NC_TEST(OutOfRangeAccess)
  PW_NC_EXPECT("The specified bit index is out of range");
  std::ignore = tuple.has_value<2>();
#endif  // PW_NC_TEST
}

TEST(OptionalTuple, SameTypes) {
  pw::OptionalTuple<int, int, int> tuple(pw::kTupleNull, 1, 2);
  EXPECT_FALSE(tuple.has_value<0>());

#if PW_NC_TEST(CannotAccessMultiple)
  PW_NC_EXPECT("type must appear exactly once in the OptionalTuple");
  std::ignore = tuple.has_value<int>();
#endif  // PW_NC_TEST
}

TEST(OptionalTuple, Constinit_Default) {
  PW_CONSTINIT static pw::OptionalTuple<int, TrivialMoveOnly, bool> tuple;

  EXPECT_FALSE(tuple.has_value<0>());
  EXPECT_FALSE(tuple.has_value<1>());
  EXPECT_FALSE(tuple.has_value<2>());
  EXPECT_EQ(tuple.size(), 3u);
  EXPECT_EQ(tuple.count(), 0u);

  tuple.emplace<1>(123);
  EXPECT_EQ(tuple.value<1>().value, 123);
  EXPECT_EQ(tuple.count(), 1u);

  tuple.reset<1>();
  EXPECT_FALSE(tuple.has_value<1>());
}

TEST(OptionalTuple, Constinit_Initialized) {
  PW_CONSTINIT static pw::OptionalTuple<const char*, TrivialMoveOnly, int>
      tuple("?", TrivialMoveOnly(42), pw::kTupleNull);

  EXPECT_EQ(tuple.count(), 2u);
  EXPECT_TRUE(tuple.has_value<0>());
  EXPECT_TRUE(tuple.has_value<1>());
  EXPECT_FALSE(tuple.has_value<2>());

  EXPECT_STREQ(tuple.value<0>(), "?");
  EXPECT_EQ(tuple.value<1>().value, 42);
}

TEST(OptionalTuple, ValueConstruction_AllSet) {
  pw::OptionalTuple<MoveOnly, bool, const char*> tuple(
      MoveOnly(100), false, "hello");
  EXPECT_FALSE(tuple.empty());
  EXPECT_EQ(tuple.value<0>().value, 100);
  EXPECT_FALSE(tuple.value<1>());
  EXPECT_STREQ(tuple.value<2>(), "hello");
}

TEST(OptionalTuple, ValueConstruction_AllSet_Copy) {
  pw::OptionalTuple<int, bool, const char*> tuple(100, false, "hello");
  EXPECT_FALSE(tuple.empty());
  EXPECT_EQ(tuple.count(), 3u);
  EXPECT_EQ(tuple.size(), 3u);

  EXPECT_EQ(tuple.value<0>(), 100);
  EXPECT_FALSE(tuple.value<1>());
  EXPECT_STREQ(tuple.value<2>(), "hello");
}

TEST(OptionalTuple, ValueConstruction_MixedSet) {
  pw::OptionalTuple<bool, int, const char*> tuple(pw::kTupleNull, 100, "hello");

  EXPECT_FALSE(tuple.empty());
  EXPECT_EQ(tuple.count(), 2u);
  EXPECT_EQ(tuple.size(), 3u);

  EXPECT_FALSE(tuple.has_value<0>());
  ASSERT_TRUE(tuple.has_value<1>());
  EXPECT_EQ(tuple.value<1>(), 100);
  ASSERT_TRUE(tuple.has_value<2>());
  EXPECT_STREQ(tuple.value<2>(), "hello");
}

TEST(OptionalTuple, Reset) {
  pw::OptionalTuple<int, bool, const char*> tuple(1, true, "foo");
  EXPECT_EQ(tuple.count(), 3u);

  tuple.reset<1>();
  EXPECT_TRUE(tuple.has_value<0>());
  EXPECT_FALSE(tuple.has_value<1>());
  EXPECT_TRUE(tuple.has_value<2>());
  EXPECT_EQ(tuple.value<0>(), 1);
  EXPECT_STREQ(tuple.value<2>(), "foo");

  tuple.reset<0>();
  EXPECT_FALSE(tuple.has_value<0>());
  EXPECT_FALSE(tuple.has_value<1>());
  EXPECT_TRUE(tuple.has_value<2>());

  tuple.reset<2>();
  EXPECT_FALSE(tuple.has_value<0>());
  EXPECT_FALSE(tuple.has_value<1>());
  EXPECT_FALSE(tuple.has_value<2>());
  EXPECT_TRUE(tuple.empty());
}

TEST(OptionalTuple, Emplace_IntoEmpty) {
  pw::OptionalTuple<int, bool, std::string_view> tuple;
  EXPECT_TRUE(tuple.empty());

#if PW_NC_TEST(CannotEmplaceWithTupleNull)
  PW_NC_EXPECT("OptionalTupleNullPlaceholder");
  tuple.emplace<0>(pw::kTupleNull);
#endif  // PW_NC_TEST

  EXPECT_EQ(tuple.emplace<0>(31), 31);
  EXPECT_TRUE(tuple.has_value<0>());
  EXPECT_EQ(tuple.value<0>(), 31);
  EXPECT_FALSE(tuple.has_value<1>());
  EXPECT_FALSE(tuple.has_value<2>());

  EXPECT_TRUE(tuple.emplace<1>(true));
  EXPECT_TRUE(tuple.has_value<0>());
  EXPECT_TRUE(tuple.has_value<1>());
  EXPECT_TRUE(tuple.value<1>());
  EXPECT_FALSE(tuple.has_value<2>());
}

TEST(OptionalTuple, Emplace_OverExisting) {
  pw::OptionalTuple<int, bool, std::string_view> tuple;

  EXPECT_EQ(tuple.emplace<0>(42), 42);
  EXPECT_TRUE(tuple.has_value<0>());
  EXPECT_EQ(tuple.value<0>(), 42);
}

TEST(OptionalTuple, Emplace_MultipleArgs) {
  pw::OptionalTuple<int, bool, std::string_view> tuple;

  tuple.emplace<std::string_view>("hello", 1u);
  ASSERT_TRUE(tuple.has_value<2>());
  EXPECT_EQ(tuple.value<std::string_view>(), "h");
}

TEST(OptionalTuple, AccessByType) {
  pw::OptionalTuple<int, bool, std::string_view> tuple;

  EXPECT_FALSE(tuple.has_value<int>());
  tuple.emplace<int>(123);
  EXPECT_TRUE(tuple.has_value<int>());
  EXPECT_EQ(tuple.value<int>(), 123);

  EXPECT_FALSE(tuple.has_value<bool>());
  tuple.emplace<bool>(true);
  EXPECT_TRUE(tuple.has_value<bool>());
  EXPECT_TRUE(tuple.value<bool>());

  EXPECT_FALSE(tuple.has_value<std::string_view>());
  tuple.emplace<std::string_view>("test");
  EXPECT_TRUE(tuple.has_value<std::string_view>());
  EXPECT_EQ(tuple.value<std::string_view>(), "test");

  tuple.reset<int>();
  EXPECT_FALSE(tuple.has_value<int>());
  EXPECT_TRUE(tuple.has_value<bool>());
  EXPECT_TRUE(tuple.has_value<std::string_view>());
}

TEST(OptionalTuple, MoveValue) {
  pw::OptionalTuple<MoveOnly, int> tuple(MoveOnly(42), 99);
  EXPECT_TRUE(tuple.has_value<0>());

  MoveOnly moved = std::move(tuple.value<0>());
  EXPECT_EQ(moved.value, 42);

  EXPECT_TRUE(tuple.has_value<0>()) << "moved value is still present";
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(tuple.value<0>().value, MoveOnly::kMoved);
}

TEST(OptionalTuple, RvalueValue) {
  pw::OptionalTuple<MoveOnly, int> tuple(MoveOnly(42), 99);

  EXPECT_TRUE(tuple.has_value<0>());
  MoveOnly moved = std::move(tuple).value<0>();
  EXPECT_EQ(moved.value, 42);

  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(tuple.value<0>().value, MoveOnly::kMoved);
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_TRUE(tuple.has_value<0>()) << "moved value is still present";
}

TEST(OptionalTuple, RvalueValueByType) {
  pw::OptionalTuple<int, MoveOnly> tuple(99, MoveOnly(42));

  EXPECT_TRUE(tuple.has_value<MoveOnly>());
  MoveOnly moved = std::move(tuple).value<MoveOnly>();
  EXPECT_EQ(moved.value, 42);

  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(tuple.value<MoveOnly>().value, MoveOnly::kMoved);
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_TRUE(tuple.has_value<MoveOnly>()) << "moved value is still present";
}

TEST(OptionalTuple, DestructionOnScopeExit) {
  Counter::Reset();
  EXPECT_EQ(Counter::created, 0);
  EXPECT_EQ(Counter::destroyed, 0);

  {
    pw::OptionalTuple<Counter, Counter> tuple;
    EXPECT_EQ(tuple.emplace<0>(1).value, 1);
    EXPECT_EQ(tuple.emplace<1>(2).value, 2);

    EXPECT_EQ(Counter::created, 2);
    EXPECT_EQ(Counter::destroyed, 0);
  }

  EXPECT_EQ(Counter::created, 2);
  EXPECT_EQ(Counter::destroyed, 2);
}

TEST(OptionalTuple, DestructionOnReset) {
  Counter::Reset();
  pw::OptionalTuple<Counter, int> tuple;
  tuple.emplace<0>(1);
  EXPECT_EQ(Counter::created, 1);
  EXPECT_EQ(Counter::destroyed, 0);

  tuple.reset<0>();
  EXPECT_EQ(Counter::created, 1);
  EXPECT_EQ(Counter::destroyed, 1);
  EXPECT_FALSE(tuple.has_value<0>());

  // Resetting an empty slot should do nothing.
  tuple.reset<0>();
  EXPECT_EQ(Counter::destroyed, 1);
}

TEST(OptionalTuple, DestructionOnEmplace) {
  Counter::Reset();
  pw::OptionalTuple<Counter> tuple;
  tuple.emplace<0>(1);
  EXPECT_EQ(Counter::created, 1);
  EXPECT_EQ(Counter::destroyed, 0);

  tuple.emplace<0>(2);

  EXPECT_EQ(Counter::created, 2);
  EXPECT_EQ(Counter::destroyed, 1);
  EXPECT_EQ(tuple.value<0>().value, 2);
}

TEST(OptionalTuple, MoveConstructor) {
  Counter::Reset();

  pw::OptionalTuple<Counter, Counter, Counter, Counter> src(
      3, 4, pw::kTupleNull, pw::kTupleNull);

  EXPECT_EQ(Counter::created, 2);
  EXPECT_EQ(Counter::destroyed, 0);
  EXPECT_EQ(Counter::moved, 0);

  pw::OptionalTuple<Counter, Counter, Counter, Counter> dest(std::move(src));

  EXPECT_EQ(Counter::created, 2);
  EXPECT_EQ(Counter::moved, 2) << "moved constructed two Counters";
  EXPECT_EQ(Counter::destroyed, 2) << "destroyed the moved-from Counters";

  EXPECT_EQ(dest.value<0>().value, 3);
  EXPECT_EQ(dest.value<1>().value, 4);
  EXPECT_FALSE(dest.has_value<2>());
  EXPECT_FALSE(dest.has_value<3>());

  EXPECT_TRUE(src.empty());  // NOLINT(bugprone-use-after-move)
}

TEST(OptionalTuple, MoveAssignment) {
  Counter::Reset();

  pw::OptionalTuple<Counter, Counter, Counter, Counter> dest(
      1, pw::kTupleNull, 2, pw::kTupleNull);
  pw::OptionalTuple<Counter, Counter, Counter, Counter> src(
      3, 4, pw::kTupleNull, pw::kTupleNull);

  EXPECT_EQ(Counter::created, 4);
  EXPECT_EQ(Counter::destroyed, 0);
  EXPECT_EQ(Counter::moved, 0);

  dest = std::move(src);

  EXPECT_EQ(Counter::created, 4);
  EXPECT_EQ(Counter::destroyed, 4);
  EXPECT_EQ(Counter::moved, 2);

  EXPECT_EQ(dest.value<0>().value, 3);
  EXPECT_EQ(dest.value<1>().value, 4);
  EXPECT_FALSE(dest.has_value<2>());
  EXPECT_FALSE(dest.has_value<3>());

  EXPECT_TRUE(src.empty());  // NOLINT(bugprone-use-after-move)
}

TEST(OptionalTuple, CopyConstructor) {
  Counter::Reset();

  pw::OptionalTuple<Counter, Counter, Counter, Counter> src(
      3, 4, pw::kTupleNull, pw::kTupleNull);

  EXPECT_EQ(Counter::created, 2);
  EXPECT_EQ(Counter::destroyed, 0);
  EXPECT_EQ(Counter::moved, 0);

  pw::OptionalTuple<Counter, Counter, Counter, Counter> dest(src);

  EXPECT_EQ(Counter::created, 4);
  EXPECT_EQ(Counter::moved, 0);
  EXPECT_EQ(Counter::destroyed, 0);

  EXPECT_EQ(src.value<0>().value, 3);
  EXPECT_EQ(src.value<1>().value, 4);
  EXPECT_FALSE(src.has_value<2>());
  EXPECT_FALSE(src.has_value<3>());

  EXPECT_EQ(dest.value<0>().value, 3);
  EXPECT_EQ(dest.value<1>().value, 4);
  EXPECT_FALSE(dest.has_value<2>());
  EXPECT_FALSE(dest.has_value<3>());
}

TEST(OptionalTuple, CopyAssignment) {
  Counter::Reset();

  pw::OptionalTuple<Counter, Counter, Counter, Counter> dest(
      1, pw::kTupleNull, 2, pw::kTupleNull);
  pw::OptionalTuple<Counter, Counter, Counter, Counter> src(
      3, 4, pw::kTupleNull, pw::kTupleNull);

  EXPECT_EQ(Counter::created, 4);
  EXPECT_EQ(Counter::destroyed, 0);
  EXPECT_EQ(Counter::moved, 0);

  dest = src;

  EXPECT_EQ(Counter::created, 6) << "Created two new copies";
  EXPECT_EQ(Counter::destroyed, 2) << "Destroys all before copy";
  EXPECT_EQ(Counter::moved, 0);

  EXPECT_EQ(src.value<0>().value, 3);
  EXPECT_EQ(src.value<1>().value, 4);
  EXPECT_FALSE(src.has_value<2>());
  EXPECT_FALSE(src.has_value<3>());

  EXPECT_EQ(dest.value<0>().value, 3);
  EXPECT_EQ(dest.value<1>().value, 4);
  EXPECT_FALSE(dest.has_value<2>());
  EXPECT_FALSE(dest.has_value<3>());
}

TEST(OptionalTuple, ValueOr_ByIndex) {
  pw::OptionalTuple<int, int> tuple(100, pw::kTupleNull);

  EXPECT_EQ(tuple.value_or<0>(200), 100);

  EXPECT_EQ(tuple.value_or<1>(200), 200);
  EXPECT_FALSE(tuple.has_value<1>());
}

TEST(OptionalTuple, ValueOr_ByType) {
  pw::OptionalTuple<int, long> tuple(100, pw::kTupleNull);

  EXPECT_EQ(tuple.value_or<int>(200), 100);

  EXPECT_EQ(tuple.value_or<long>(500), 500);
  EXPECT_FALSE(tuple.has_value<long>());
}

TEST(OptionalTuple, ValueOr_ByIndex_RvalueRefDefault) {
  pw::OptionalTuple<int, MoveOnly> tuple(100, pw::kTupleNull);

  MoveOnly moved_value = std::move(tuple).value_or<1>(MoveOnly(200));
  EXPECT_EQ(moved_value.value, 200);
  EXPECT_FALSE(tuple.has_value<1>());  // NOLINT(bugprone-use-after-move)
}

TEST(OptionalTuple, ValueOr_ByIndex_RvalueRefMoved) {
  pw::OptionalTuple<int, MoveOnly> tuple(100, MoveOnly(42));

  MoveOnly moved_value = std::move(tuple).value_or<1>(MoveOnly(200));
  EXPECT_EQ(moved_value.value, 42);

  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_TRUE(tuple.has_value<1>());
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(tuple.value<1>().value, MoveOnly::kMoved);
}

TEST(OptionalTuple, ValueOr_ByType_RvalueRefDefault) {
  pw::OptionalTuple<int, MoveOnly> tuple(100, pw::kTupleNull);

  MoveOnly moved_value = std::move(tuple).value_or<MoveOnly>(MoveOnly(200));
  EXPECT_EQ(moved_value.value, 200);
  EXPECT_FALSE(tuple.has_value<1>());  // NOLINT(bugprone-use-after-move)
}

TEST(OptionalTuple, ValueOr_ByType_RvalueRefMoved) {
  pw::OptionalTuple<int, MoveOnly> tuple(100, MoveOnly(42));

  MoveOnly moved_value = std::move(tuple).value_or<MoveOnly>(MoveOnly(200));
  EXPECT_EQ(moved_value.value, 42);
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_TRUE(tuple.has_value<1>());
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(tuple.value<1>().value, MoveOnly::kMoved);
}

#if PW_CXX_STANDARD_IS_SUPPORTED(20)

constexpr pw::OptionalTuple<int, bool> kTuple(123, pw::kTupleNull);

static_assert(kTuple.count() == 1);
static_assert(kTuple.size() == 2);
static_assert(kTuple.value<int>() == 123);
static_assert(!kTuple.has_value<1>());

#endif  // PW_CXX_STANDARD_IS_SUPPORTED(20)

static_assert(
    std::is_same_v<std::tuple_element_t<0, pw::OptionalTuple<long>>, long>);
static_assert(
    std::is_same_v<std::tuple_element_t<0, pw::OptionalTuple<int, bool, void*>>,
                   int>);
static_assert(
    std::is_same_v<std::tuple_element_t<1, pw::OptionalTuple<int, bool, void*>>,
                   bool>);
static_assert(
    std::is_same_v<std::tuple_element_t<2, pw::OptionalTuple<int, bool, void*>>,
                   void*>);

static_assert(std::tuple_size_v<pw::OptionalTuple<>> == 0);
static_assert(std::tuple_size_v<pw::OptionalTuple<bool>> == 1);
static_assert(std::tuple_size_v<pw::OptionalTuple<long long>> == 1);
static_assert(std::tuple_size_v<pw::OptionalTuple<bool, int>> == 2);
static_assert(std::tuple_size_v<pw::OptionalTuple<long, int, bool>> == 3);

}  // namespace
