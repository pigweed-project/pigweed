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

#include <optional>
#include <type_traits>
#include <utility>

#include "pw_compilation_testing/negative_compilation.h"
#include "pw_containers/internal/maybe.h"
#include "pw_containers/internal/tag.h"
#include "pw_polyfill/language_feature_macros.h"
#include "pw_unit_test/constexpr.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::containers::internal::ConstructAt;
using pw::containers::internal::Empty;
using pw::containers::internal::kIsInPlaceTag;
using pw::containers::internal::Maybe;
using pw::containers::internal::NullPlaceholder;
using pw::containers::internal::UninitializedTag;

// ============================================================================
// tag.h tests
// ============================================================================

static_assert(std::is_empty_v<Empty>);
static_assert(std::is_trivially_default_constructible_v<Empty>);
static_assert(std::is_trivially_copyable_v<Empty>);

static_assert(std::is_empty_v<UninitializedTag>);
static_assert(std::is_trivially_default_constructible_v<UninitializedTag>);

constexpr auto kNullPlaceholder = NullPlaceholder::InternalUseOnly();
static_assert(
    std::is_same_v<decltype(kNullPlaceholder), const NullPlaceholder>);

static_assert(kIsInPlaceTag<std::in_place_t>);
static_assert(kIsInPlaceTag<std::in_place_type_t<int>>);
static_assert(kIsInPlaceTag<std::in_place_index_t<0>>);
static_assert(!kIsInPlaceTag<int>);
static_assert(!kIsInPlaceTag<Empty>);
static_assert(!kIsInPlaceTag<const std::in_place_t>);
static_assert(!kIsInPlaceTag<const std::in_place_t&>);

// ============================================================================
// maybe.h tests
// ============================================================================

struct NonDefaultTrivial {
  constexpr explicit NonDefaultTrivial(int v) : value(v) {}
  int value;
};

struct NonTrivialDtor {
  constexpr explicit NonTrivialDtor(int v) : value(v) {}
  PW_CONSTEXPR_CPP20 ~NonTrivialDtor() {}
  int value;
};

struct EvilAddressOf {
  int value;
  constexpr explicit EvilAddressOf(int v) : value(v) {}
  constexpr void operator&() const = delete;  // NOLINT(google-runtime-operator)
};

static_assert(std::is_trivially_destructible_v<Maybe<int>>);
static_assert(std::is_trivially_copy_constructible_v<Maybe<int>>);
static_assert(std::is_trivially_move_constructible_v<Maybe<int>>);
static_assert(std::is_trivially_copy_assignable_v<Maybe<int>>);
static_assert(std::is_trivially_move_assignable_v<Maybe<int>>);
static_assert(std::is_trivially_copyable_v<Maybe<int>>);

static_assert(std::is_trivially_destructible_v<Maybe<NonDefaultTrivial>>);
static_assert(std::is_trivially_copyable_v<Maybe<NonDefaultTrivial>>);

static_assert(!std::is_trivially_destructible_v<Maybe<NonTrivialDtor>>);

PW_CONSTEXPR_TEST(MaybeTest, ConstructionAndCopyMove, {
  Maybe<int> m1(42);
  PW_TEST_EXPECT_EQ(m1.value, 42);

  Maybe<int> m2(m1);
  PW_TEST_EXPECT_EQ(m2.value, 42);

  Maybe<NonDefaultTrivial> m3(NullPlaceholder::InternalUseOnly());
  Maybe<NonDefaultTrivial> m4(77);
  PW_TEST_EXPECT_EQ(m4.value.value, 77);
  std::ignore = m3;
});

PW_CONSTEXPR_TEST(MaybeTest, ForwardsInPlaceToValue, {
  // Verify `std::in_place` is forwarded to `T` rather than swallowed by
  // `MaybeImpl`.
  Maybe<std::optional<int>> m_default(std::in_place);
  PW_TEST_EXPECT_TRUE(m_default.value.has_value());
  PW_TEST_EXPECT_EQ(*m_default.value, 0);

  Maybe<std::optional<int>> m_arg(std::in_place, 123);
  PW_TEST_EXPECT_TRUE(m_arg.value.has_value());
  PW_TEST_EXPECT_EQ(*m_arg.value, 123);
});

TEST(MaybeTest, ConstructAtAndInPlaceSwitch) {
  Maybe<NonDefaultTrivial> m(NullPlaceholder::InternalUseOnly());
  m.Construct(99);
  EXPECT_EQ(m.value.value, 99);

  EvilAddressOf obj(1);
  ConstructAt(obj, 42);
  EXPECT_EQ(obj.value, 42);

#if PW_NC_TEST(NullPlaceholderPrivateCtor)
  PW_NC_EXPECT_CLANG("calling a private constructor");
  PW_NC_EXPECT_GCC("is private within this context");
  NullPlaceholder tag{};
  std::ignore = tag;
#endif  // PW_NC_TEST
}

PW_CONSTEXPR_TEST_IF_CPP20(MaybeTest, ConstexprConstructInPlaceSwitch, {
  Maybe<NonDefaultTrivial> m1(NullPlaceholder::InternalUseOnly());
  m1.Construct(99);
  PW_TEST_EXPECT_EQ(m1.value.value, 99);

  Maybe<NonTrivialDtor> m2(NullPlaceholder::InternalUseOnly());
  m2.Construct(77);
  PW_TEST_EXPECT_EQ(m2.value.value, 77);
  m2.value.~NonTrivialDtor();
});

}  // namespace
