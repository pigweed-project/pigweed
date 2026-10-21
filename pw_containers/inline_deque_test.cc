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

#include "pw_containers/inline_deque.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "pw_compilation_testing/negative_compilation.h"
#include "pw_containers/internal/container_tests.h"
#include "pw_containers/internal/test_helpers.h"
#include "pw_unit_test/framework.h"

namespace pw::containers {
namespace {

using namespace std::literals::string_view_literals;
using test::CopyOnly;
using test::Counter;
using test::MoveOnly;

static_assert(!std::is_constructible_v<pw::InlineDeque<int>>,
              "Cannot construct generic capacity container");

// Instantiate shared deque tests.
template <size_t kCapacity>
class CommonTest
    : public ::pw::containers::test::CommonTestFixture<CommonTest<kCapacity>> {
 public:
  template <typename T>
  class Container {
   public:
    Container(CommonTest&) {}

    pw::InlineDeque<T, kCapacity>& get() { return container_; }
    const pw::InlineDeque<T, kCapacity>& get() const { return container_; }

   private:
    pw::InlineDeque<T, kCapacity> container_;
  };
};

using InlineDequeCommonTest9 = CommonTest<9>;
using InlineDequeCommonTest16 = CommonTest<16>;

PW_CONTAINERS_COMMON_DEQUE_TESTS(InlineDequeCommonTest9);
PW_CONTAINERS_COMMON_DEQUE_TESTS(InlineDequeCommonTest16);
PW_CONTAINERS_OVERWRITE_DEQUE_TESTS(InlineDequeCommonTest9)
PW_CONTAINERS_OVERWRITE_DEQUE_TESTS(InlineDequeCommonTest16)

TEST(InlineDeque, ZeroCapacity) {
  Counter::Reset();
  {
    InlineDeque<Counter, 0> container;
    EXPECT_EQ(container.size(), 0u);
    EXPECT_TRUE((InlineDeque<Counter, 0>()).full());
  }
  EXPECT_EQ(Counter::created, 0);
  EXPECT_EQ(Counter::destroyed, 0);
}

TEST(InlineDeque, Construct_Sized) {
  InlineDeque<int, 3> deque;
  EXPECT_TRUE(deque.empty());
  EXPECT_EQ(deque.size(), 0u);
  EXPECT_EQ(deque.max_size(), 3u);
}

TEST(InlineDeque, Construct_GenericSized) {
  InlineDeque<int, 3> sized_deque;
  InlineDeque<int>& deque(sized_deque);
  EXPECT_TRUE(deque.empty());
  EXPECT_EQ(deque.size(), 0u);
  EXPECT_EQ(deque.max_size(), 3u);
}

TEST(InlineDeque, Construct_ConstexprSized) {
  constexpr InlineDeque<int, 3> deque(pw::kConstexpr);
  EXPECT_TRUE(deque.empty());
  EXPECT_EQ(deque.size(), 0u);
  EXPECT_EQ(deque.max_size(), 3u);
}

TEST(InlineDeque, Construct_CopySameCapacity) {
  InlineDeque<CopyOnly, 4> deque(4, CopyOnly(123));
  const auto& deque_ref = deque;
  InlineDeque<CopyOnly, 4> copied(deque_ref);

  ASSERT_EQ(4u, deque.size());
  EXPECT_EQ(123, deque[3].value);

  ASSERT_EQ(4u, copied.size());
  EXPECT_EQ(123, copied[3].value);
}

TEST(InlineDeque, Construct_MoveSameCapacity) {
  InlineDeque<MoveOnly, 4> deque;
  deque.emplace_back(MoveOnly(1));
  deque.emplace_back(MoveOnly(2));
  deque.emplace_back(MoveOnly(3));
  deque.emplace_back(MoveOnly(4));
  InlineDeque<MoveOnly, 4> moved_into(std::move(deque));

  EXPECT_EQ(0u, deque.size());  // NOLINT(bugprone-use-after-move)

  ASSERT_EQ(4u, moved_into.size());
  EXPECT_EQ(1, moved_into[0].value);
  EXPECT_EQ(2, moved_into[1].value);
  EXPECT_EQ(3, moved_into[2].value);
  EXPECT_EQ(4, moved_into[3].value);
}

TEST(InlineDeque, Construct_CopyLargerCapacity) {
  InlineDeque<CopyOnly, 4> deque(4, CopyOnly(123));
  InlineDeque<CopyOnly, 5> copied(deque);

  ASSERT_EQ(4u, deque.size());
  EXPECT_EQ(123, deque[3].value);

  ASSERT_EQ(4u, copied.size());
  EXPECT_EQ(123, copied[3].value);
}

TEST(InlineDeque, Construct_MoveLargerCapacity) {
  InlineDeque<MoveOnly, 4> deque;
  deque.emplace_back(MoveOnly(1));
  deque.emplace_back(MoveOnly(2));
  deque.emplace_back(MoveOnly(3));
  deque.emplace_back(MoveOnly(4));
  InlineDeque<MoveOnly, 5> moved(std::move(deque));

  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(0u, deque.size());

  ASSERT_EQ(4u, moved.size());
  EXPECT_EQ(4, moved[3].value);
}

TEST(InlineDeque, Construct_CopySmallerCapacity) {
  InlineDeque<CopyOnly, 4> deque(3, CopyOnly(123));
  InlineDeque<CopyOnly, 3> copied(deque);

  ASSERT_EQ(3u, deque.size());
  EXPECT_EQ(123, deque[2].value);

  ASSERT_EQ(3u, copied.size());
  EXPECT_EQ(123, copied[2].value);
}

TEST(InlineDeque, AssignOperator_InitializerList) {
  InlineDeque<int, 4> deque = {1, 3, 5, 7};

  ASSERT_EQ(4u, deque.size());

  EXPECT_EQ(1, deque[0]);
  EXPECT_EQ(3, deque[1]);
  EXPECT_EQ(5, deque[2]);
  EXPECT_EQ(7, deque[3]);
}

TEST(InlineDeque, AssignOperator_CopySameCapacity) {
  InlineDeque<CopyOnly, 4> deque(4, CopyOnly(123));
  InlineDeque<CopyOnly, 4> copied = deque;

  ASSERT_EQ(4u, deque.size());
  EXPECT_EQ(123, deque[3].value);

  ASSERT_EQ(4u, copied.size());
  EXPECT_EQ(123, copied[3].value);
}

TEST(InlineDeque, AssignOperator_CopyLargerCapacity) {
  InlineDeque<CopyOnly, 4> deque(4, CopyOnly(123));
  InlineDeque<CopyOnly, 5> copied = deque;

  ASSERT_EQ(4u, deque.size());
  EXPECT_EQ(123, deque[3].value);

  ASSERT_EQ(4u, copied.size());
  EXPECT_EQ(123, copied[3].value);
}

TEST(InlineDeque, AssignOperator_CopySmallerCapacity) {
  InlineDeque<CopyOnly, 4> deque(3, CopyOnly(123));
  InlineDeque<CopyOnly, 3> copied = deque;

  ASSERT_EQ(3u, deque.size());
  EXPECT_EQ(123, deque[2].value);

  ASSERT_EQ(3u, copied.size());
  EXPECT_EQ(123, copied[2].value);
}

TEST(InlineDeque, AssignOperator_MoveSameCapacity) {
  InlineDeque<MoveOnly, 4> deque;
  deque.emplace_back(MoveOnly(1));
  deque.emplace_back(MoveOnly(2));
  deque.emplace_back(MoveOnly(3));
  deque.emplace_back(MoveOnly(4));
  InlineDeque<MoveOnly, 4> moved = std::move(deque);

  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(0u, deque.size());

  ASSERT_EQ(4u, moved.size());
  EXPECT_EQ(4, moved[3].value);
}

TEST(InlineDeque, AssignOperator_MoveLargerCapacity) {
  InlineDeque<MoveOnly, 4> deque;
  deque.emplace_back(MoveOnly(1));
  deque.emplace_back(MoveOnly(2));
  deque.emplace_back(MoveOnly(3));
  deque.emplace_back(MoveOnly(4));
  InlineDeque<MoveOnly, 5> moved = std::move(deque);

  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(0u, deque.size());

  ASSERT_EQ(4u, moved.size());
  EXPECT_EQ(4, moved[3].value);
}

TEST(InlineDeque, AssignOperator_MoveSmallerCapacity) {
  InlineDeque<MoveOnly, 4> deque;
  deque.emplace_back(MoveOnly(1));
  deque.emplace_back(MoveOnly(2));
  deque.emplace_back(MoveOnly(3));
  InlineDeque<MoveOnly, 3> moved = std::move(deque);

  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(0u, deque.size());

  ASSERT_EQ(3u, moved.size());
  EXPECT_EQ(3, moved[2].value);
}

TEST(InlineDeque, Generic) {
  InlineDeque<int, 10> deque;
  InlineDeque<int>& generic_deque(deque);
  generic_deque = {1, 2, 3, 4, 5};

  EXPECT_EQ(generic_deque.size(), deque.size());
  EXPECT_EQ(generic_deque.max_size(), deque.max_size());

  unsigned short i = 0;
  for (int value : deque) {
    EXPECT_EQ(value, generic_deque[i]);
    i += 1;
  }

  i = 0;
  for (int value : generic_deque) {
    EXPECT_EQ(deque[i], value);
    i += 1;
  }
}

TEST(InlineDeque, MaxSizeConstexpr) {
  InlineDeque<int, 10> deque;
  constexpr size_t kMaxSize = deque.max_size();
  EXPECT_EQ(deque.max_size(), kMaxSize);

  // Ensure the generic sized container does not have a constexpr max_size().
  [[maybe_unused]] InlineDeque<int>& generic_deque(deque);
#if PW_NC_TEST(InlineDeque_GenericMaxSize_NotConstexpr)
  PW_NC_EXPECT_CLANG(
      "kGenericMaxSize.* must be initialized by a constant expression");
  PW_NC_EXPECT_GCC("call to non-'constexpr' function .*InlineDeque.*max_size");
  [[maybe_unused]] constexpr size_t kGenericMaxSize = generic_deque.max_size();
#endif  // PW_NC_TEST
}

// max_size() is capacity()
static_assert(InlineDeque<int, 0>::max_size() == 0);
static_assert(InlineDeque<int, 1234>::max_size() == 1234);
static_assert(InlineDeque<int, 65535>::max_size() == 65535);

// Instantiate shared container and iterator tests.
template <typename T>
using InlineDeque4 = InlineDeque<T, 4>;

static_assert(test::IteratorProperties<InlineDeque4>::kPasses);
static_assert(test::IteratorProperties<InlineDeque>::kPasses);

// Test that InlineDeque<T> is trivially destructible when its type is.
static_assert(std::is_trivially_destructible_v<InlineDeque<int, 4>>);

static_assert(std::is_trivially_destructible_v<CopyOnly>);
static_assert(std::is_trivially_destructible_v<InlineDeque<CopyOnly, 99>>);

static_assert(!std::is_trivially_destructible_v<Counter>);
static_assert(!std::is_trivially_destructible_v<InlineDeque<Counter, 99>>);

// Generic-capacity deques cannot be constructed or destructed.
static_assert(!std::is_constructible_v<InlineDeque<int>>);
static_assert(!std::is_destructible_v<InlineDeque<int>>);

// Tests that InlineDeque<T> does not have any extra padding.
static_assert(sizeof(InlineDeque<uint8_t, 1>) ==
              sizeof(InlineDeque<uint8_t>::size_type) * 4 +
                  std::max(sizeof(InlineDeque<uint8_t>::size_type),
                           sizeof(uint8_t)));
static_assert(sizeof(InlineDeque<uint8_t, 2>) ==
              sizeof(InlineDeque<uint8_t>::size_type) * 4 +
                  2 * sizeof(uint8_t));
static_assert(sizeof(InlineDeque<uint16_t, 1>) ==
              sizeof(InlineDeque<uint16_t>::size_type) * 4 + sizeof(uint16_t));
static_assert(sizeof(InlineDeque<uint32_t, 1>) ==
              sizeof(InlineDeque<uint32_t>::size_type) * 4 + sizeof(uint32_t));
static_assert(sizeof(InlineDeque<uint64_t, 1>) ==
              sizeof(InlineDeque<uint64_t>::size_type) * 4 + sizeof(uint64_t));

static_assert(std::is_copy_constructible_v<InlineDeque<int, 4>>);

static_assert(std::is_move_constructible_v<InlineDeque<MoveOnly, 4>>);

static_assert(std::is_copy_assignable_v<InlineDeque<CopyOnly>>);
static_assert(std::is_copy_assignable_v<InlineDeque<CopyOnly, 4>>);

static_assert(std::is_move_assignable_v<InlineDeque<MoveOnly>>);
static_assert(std::is_move_assignable_v<InlineDeque<MoveOnly, 4>>);

}  // namespace
}  // namespace pw::containers
