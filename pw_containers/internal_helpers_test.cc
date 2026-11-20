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

#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

#include "pw_compilation_testing/negative_compilation.h"
#include "pw_containers/internal/maybe.h"
#include "pw_containers/internal/pack_indexing.h"
#include "pw_containers/internal/special_members.h"
#include "pw_containers/internal/tag.h"
#include "pw_containers/internal/test_helpers.h"
#include "pw_polyfill/language_feature_macros.h"
#include "pw_unit_test/constexpr.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::containers::internal::ConstructAt;
using pw::containers::internal::ConstructFromTag;
using pw::containers::internal::Empty;
using pw::containers::internal::InvalidTypeIndex;
using pw::containers::internal::kAllTriviallyDestructible;
using pw::containers::internal::kCopyAssignTrait;
using pw::containers::internal::kCopyCtorTrait;
using pw::containers::internal::kInvalidTypeIndex;
using pw::containers::internal::kIsInPlaceTag;
using pw::containers::internal::kMoveAssignTrait;
using pw::containers::internal::kMoveCtorTrait;
using pw::containers::internal::kUniqueTypeIndex;
using pw::containers::internal::Maybe;
using pw::containers::internal::NthType;
using pw::containers::internal::NullPlaceholder;
using pw::containers::internal::SafeNthType;
using pw::containers::internal::SelectConstexprSpecialMembers;
using pw::containers::internal::SelectSpecialMembers;
using pw::containers::internal::SpecialMemberTrait;
using pw::containers::internal::UninitializedTag;
using pw::containers::test::CopyOnly;
using pw::containers::test::Counter;
using pw::containers::test::MoveOnly;
using pw::containers::test::TrivialMoveOnly;

// ============================================================================
// tag.h tests
// ============================================================================

static_assert(std::is_empty_v<Empty>);
static_assert(std::is_trivially_default_constructible_v<Empty>);
static_assert(std::is_trivially_copyable_v<Empty>);

static_assert(std::is_empty_v<UninitializedTag>);
static_assert(std::is_trivially_default_constructible_v<UninitializedTag>);

static_assert(std::is_empty_v<ConstructFromTag>);
static_assert(std::is_trivially_default_constructible_v<ConstructFromTag>);

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
// pack_indexing.h tests
// ============================================================================

template <size_t I>
struct IndexedTag {};

static_assert(std::is_same_v<NthType<0, int>, int>);
static_assert(std::is_same_v<NthType<0, int, char, float>, int>);
static_assert(std::is_same_v<NthType<2, int, char, float>, float>);

// Test across 8-element chunk boundaries (indices 7, 8, 15, 16, 17).
#define _PW_TEST_PACK_18                                                     \
  IndexedTag<0>, IndexedTag<1>, IndexedTag<2>, IndexedTag<3>, IndexedTag<4>, \
      IndexedTag<5>, IndexedTag<6>, IndexedTag<7>, IndexedTag<8>,            \
      IndexedTag<9>, IndexedTag<10>, IndexedTag<11>, IndexedTag<12>,         \
      IndexedTag<13>, IndexedTag<14>, IndexedTag<15>, IndexedTag<16>,        \
      IndexedTag<17>

static_assert(std::is_same_v<NthType<7, _PW_TEST_PACK_18>, IndexedTag<7>>);
static_assert(std::is_same_v<NthType<8, _PW_TEST_PACK_18>, IndexedTag<8>>);
static_assert(std::is_same_v<NthType<15, _PW_TEST_PACK_18>, IndexedTag<15>>);
static_assert(std::is_same_v<NthType<16, _PW_TEST_PACK_18>, IndexedTag<16>>);
static_assert(std::is_same_v<NthType<17, _PW_TEST_PACK_18>, IndexedTag<17>>);
static_assert(
    std::is_same_v<SafeNthType<18, _PW_TEST_PACK_18>, InvalidTypeIndex>);
static_assert(std::is_same_v<SafeNthType<0>, InvalidTypeIndex>);
#undef _PW_TEST_PACK_18

static_assert(kUniqueTypeIndex<int> == kInvalidTypeIndex);
static_assert(kUniqueTypeIndex<int, int> == 0);
static_assert(kUniqueTypeIndex<char, int, char, float> == 1);
static_assert(kUniqueTypeIndex<double, int, char, float> == kInvalidTypeIndex);
static_assert(kUniqueTypeIndex<int, int, char, int> == kInvalidTypeIndex);
static_assert(kUniqueTypeIndex<float, int, char, float> == 2);

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

// ============================================================================
// special_members.h tests
// ============================================================================

struct NonConstDeletedCopy {
  int value = 0;
  NonConstDeletedCopy(NonConstDeletedCopy&) = delete;
  NonConstDeletedCopy& operator=(NonConstDeletedCopy&) = delete;
  NonConstDeletedCopy(NonConstDeletedCopy&&) = default;
  NonConstDeletedCopy& operator=(NonConstDeletedCopy&&) = default;
};

static_assert(kAllTriviallyDestructible<int, float>);
static_assert(!kAllTriviallyDestructible<int, Counter>);

static_assert(kMoveCtorTrait<int, float> == SpecialMemberTrait::kTrivial);
static_assert(kMoveCtorTrait<int, Counter> == SpecialMemberTrait::kNonTrivial);
static_assert(kMoveCtorTrait<int, CopyOnly> == SpecialMemberTrait::kDeleted);

static_assert(kCopyCtorTrait<int, float> == SpecialMemberTrait::kTrivial);
static_assert(kCopyCtorTrait<int, Counter> == SpecialMemberTrait::kNonTrivial);
static_assert(kCopyCtorTrait<int, MoveOnly> == SpecialMemberTrait::kDeleted);

static_assert(kMoveAssignTrait<int, float> == SpecialMemberTrait::kTrivial);
static_assert(kMoveAssignTrait<int, Counter> ==
              SpecialMemberTrait::kNonTrivial);
static_assert(kMoveAssignTrait<int, CopyOnly> == SpecialMemberTrait::kDeleted);

static_assert(kCopyAssignTrait<int, float> == SpecialMemberTrait::kTrivial);
static_assert(kCopyAssignTrait<int, Counter> ==
              SpecialMemberTrait::kNonTrivial);
static_assert(kCopyAssignTrait<int, MoveOnly> == SpecialMemberTrait::kDeleted);

template <typename T>
struct TestStorageBase {
  int value = 0;
  int copy_ctors = 0;
  int move_ctors = 0;
  int copy_assigns = 0;
  int move_assigns = 0;

  constexpr TestStorageBase() = default;
  constexpr explicit TestStorageBase(ConstructFromTag,
                                     const TestStorageBase& other) noexcept
      : value(other.value), copy_ctors(other.copy_ctors + 1) {}
  constexpr explicit TestStorageBase(ConstructFromTag,
                                     TestStorageBase&& other) noexcept
      : value(other.value), move_ctors(other.move_ctors + 1) {}
  constexpr explicit TestStorageBase(int v) : value(v) {}

  PW_CONSTEXPR_CPP20 void Destroy() noexcept {}

  constexpr void AssignFrom(const TestStorageBase& other) noexcept {
    value = other.value;
    ++copy_assigns;
  }

  constexpr void AssignFrom(TestStorageBase&& other) noexcept {
    value = other.value;
    ++move_assigns;
  }
};

template <typename... Ts>
using TestContainer = SelectSpecialMembers<TestStorageBase<void>, Ts...>;

static_assert(std::is_trivially_destructible_v<TestContainer<int>>);
static_assert(std::is_trivially_copy_constructible_v<TestContainer<int>>);
static_assert(std::is_trivially_move_constructible_v<TestContainer<int>>);
static_assert(std::is_trivially_copy_assignable_v<TestContainer<int>>);
static_assert(std::is_trivially_move_assignable_v<TestContainer<int>>);
static_assert(std::is_trivially_copyable_v<TestContainer<int>>);

static_assert(!std::is_trivially_destructible_v<TestContainer<Counter>>);
static_assert(!std::is_trivially_copy_constructible_v<TestContainer<Counter>>);
static_assert(std::is_copy_constructible_v<TestContainer<Counter>>);
static_assert(std::is_move_constructible_v<TestContainer<Counter>>);
static_assert(std::is_copy_assignable_v<TestContainer<Counter>>);
static_assert(std::is_move_assignable_v<TestContainer<Counter>>);

static_assert(!std::is_copy_constructible_v<TestContainer<MoveOnly>>);
static_assert(!std::is_copy_assignable_v<TestContainer<MoveOnly>>);
static_assert(std::is_move_constructible_v<TestContainer<MoveOnly>>);
static_assert(std::is_move_assignable_v<TestContainer<MoveOnly>>);

static_assert(std::is_copy_constructible_v<TestContainer<CopyOnly>>);
static_assert(std::is_copy_assignable_v<TestContainer<CopyOnly>>);
// Move construction falls back to copy when copy is enabled and move is
// deleted, while MoveAssignLayer explicitly deletes move assignment at the
// outer layer unless wrapped by a derived class with `= default`.
static_assert(std::is_move_constructible_v<TestContainer<CopyOnly>>);
static_assert(!std::is_move_assignable_v<TestContainer<CopyOnly>>);

static_assert(!std::is_copy_constructible_v<TestContainer<CopyOnly, MoveOnly>>);
static_assert(!std::is_move_constructible_v<TestContainer<CopyOnly, MoveOnly>>);
static_assert(!std::is_copy_assignable_v<TestContainer<CopyOnly, MoveOnly>>);
static_assert(!std::is_move_assignable_v<TestContainer<CopyOnly, MoveOnly>>);

static_assert(
    !std::is_copy_constructible_v<TestContainer<NonConstDeletedCopy>>);
static_assert(
    std::is_trivially_move_constructible_v<TestContainer<NonConstDeletedCopy>>);
static_assert(!std::is_copy_assignable_v<TestContainer<NonConstDeletedCopy>>);
static_assert(
    std::is_trivially_move_assignable_v<TestContainer<NonConstDeletedCopy>>);

PW_CONSTEXPR_TEST_IF_CPP20(SpecialMembersTest, NonTrivialHooks, {
  using Box = TestContainer<Counter>;
  Box a(10);
  Box b(a);
  PW_TEST_EXPECT_EQ(b.value, 10);
  PW_TEST_EXPECT_EQ(b.copy_ctors, 1);

  Box c(std::move(b));
  PW_TEST_EXPECT_EQ(c.value, 10);
  PW_TEST_EXPECT_EQ(c.move_ctors, 1);

  Box d(20);
  d = a;
  PW_TEST_EXPECT_EQ(d.value, 10);
  PW_TEST_EXPECT_EQ(d.copy_assigns, 1);

  // Self-copy assignment is guarded by CopyAssignLayer.
  const Box& d_ref = d;
  d = d_ref;
  PW_TEST_EXPECT_EQ(d.copy_assigns, 1);

  // Self-move assignment is guarded by MoveAssignLayer.
  Box& d_mut_ref = d;
  d = std::move(d_mut_ref);
  PW_TEST_EXPECT_EQ(d.move_assigns, 0);
});

PW_CONSTEXPR_TEST(SpecialMembersTest, ConstexprCpp17NonTrivialHooks, {
  using ConstexprBox =
      SelectConstexprSpecialMembers<TestStorageBase<void>, CopyOnly>;
  ConstexprBox a(10);
  ConstexprBox b(a);
  PW_TEST_EXPECT_EQ(b.value, 10);
  PW_TEST_EXPECT_EQ(b.copy_ctors, 1);

  ConstexprBox c(20);
  c = a;
  PW_TEST_EXPECT_EQ(c.value, 10);
  PW_TEST_EXPECT_EQ(c.copy_assigns, 1);

  using ConstexprMoveBox =
      SelectConstexprSpecialMembers<TestStorageBase<void>, TrivialMoveOnly>;
  ConstexprMoveBox m1(10);
  ConstexprMoveBox m2(std::move(m1));
  PW_TEST_EXPECT_EQ(m2.value, 10);
  PW_TEST_EXPECT_EQ(m2.move_ctors, 1);

  ConstexprMoveBox m3(20);
  m3 = std::move(m2);
  PW_TEST_EXPECT_EQ(m3.value, 10);
  PW_TEST_EXPECT_EQ(m3.move_assigns, 1);
});

}  // namespace
