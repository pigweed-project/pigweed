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

#include "pw_containers/dynamic_deque.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iterator>
#include <limits>
#include <optional>
#include <random>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "pw_allocator/fault_injecting_allocator.h"
#include "pw_allocator/null_allocator.h"
#include "pw_allocator/testing.h"
#include "pw_compilation_testing/negative_compilation.h"
#include "pw_containers/algorithm.h"
#include "pw_containers/internal/container_tests.h"
#include "pw_containers/internal/test_helpers.h"
#include "pw_polyfill/language_feature_macros.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::containers::Equal;
using pw::containers::test::CopyOnly;
using pw::containers::test::Counter;
using pw::containers::test::MoveOnly;

// Instantiate common container tests.
template <typename SizeType>
class CommonTest
    : public ::pw::containers::test::CommonTestFixture<CommonTest<SizeType>> {
 public:
  // "Container" declares an empty container usable in the test.
  template <typename T>
  class Container {
   public:
    constexpr Container(CommonTest& fixture) : container_(fixture.allocator_) {}

    pw::DynamicDeque<T, SizeType>& get() { return container_; }
    const pw::DynamicDeque<T, SizeType>& get() const { return container_; }

   private:
    pw::DynamicDeque<T, SizeType> container_;
  };

 private:
  ::pw::allocator::test::AllocatorForTest<512> allocator_;
};

using DynamicDequeCommonTestUint8 = CommonTest<uint8_t>;
using DynamicDequeCommonTestUint16 = CommonTest<uint16_t>;

PW_CONTAINERS_COMMON_DEQUE_TESTS(DynamicDequeCommonTestUint8);
PW_CONTAINERS_COMMON_DEQUE_TESTS(DynamicDequeCommonTestUint16);

PW_CONSTINIT pw::allocator::NullAllocator null_allocator;
PW_CONSTINIT const pw::DynamicDeque<int> kEmpty(null_allocator);

class DynamicDequeTest : public ::testing::Test {
 protected:
  DynamicDequeTest() : allocator_(allocator_for_test_) {}

  pw::allocator::test::AllocatorForTest<1024> allocator_for_test_;
  pw::allocator::test::FaultInjectingAllocator allocator_;
};

TEST(DynamicDeque, Constinit) {
  EXPECT_TRUE(kEmpty.empty());

  for (auto unused : kEmpty) {
    ADD_FAILURE() << unused;
  }
}

TEST_F(DynamicDequeTest, MoveConstruct) {
  pw::DynamicDeque<MoveOnly> deque(allocator_);
  deque.emplace_back(MoveOnly(1));
  deque.emplace_back(MoveOnly(2));
  deque.emplace_back(MoveOnly(3));
  deque.emplace_back(MoveOnly(4));

  pw::DynamicDeque<MoveOnly> moved_into(std::move(deque));

  EXPECT_EQ(0u, deque.size());  // NOLINT(bugprone-use-after-move)

  ASSERT_EQ(4u, moved_into.size());
  EXPECT_EQ(1, moved_into[0].value);
  EXPECT_EQ(2, moved_into[1].value);
  EXPECT_EQ(3, moved_into[2].value);
  EXPECT_EQ(4, moved_into[3].value);
}

TEST_F(DynamicDequeTest, AllocationFailure) {
  allocator_.DisableAll();
  pw::DynamicDeque<int> deque(allocator_);

  EXPECT_FALSE(deque.try_push_back(6));
  EXPECT_FALSE(deque.try_push_front(7));

  allocator_.EnableAll();

  EXPECT_TRUE(deque.try_push_back(6));
  ASSERT_TRUE(Equal(deque, std::array{6}));
}

TEST_F(DynamicDequeTest, InterspersedAllocationFailures) {
  pw::DynamicDeque<int> deque(allocator_);
  allocator_.DisableAll();

  EXPECT_FALSE(deque.try_push_back(1));

  allocator_.EnableAll();
  ASSERT_TRUE(deque.try_push_back(1));
  ASSERT_TRUE(deque.try_push_front(2));
  ASSERT_TRUE(Equal(deque, std::array{2, 1}));

  allocator_.DisableAll();

  // Fill to capacity
  for (int i = 0; deque.size() < deque.capacity(); ++i) {
    ASSERT_TRUE(deque.try_push_front(i));
  }

  EXPECT_FALSE(deque.try_push_front(100));
  EXPECT_FALSE(deque.try_push_back(100));

  allocator_.EnableAll();
  EXPECT_TRUE(deque.try_push_back(100));
}

TEST_F(DynamicDequeTest, Capacity_ResizesWhenPossible) {
  pw::DynamicDeque<int> deque(allocator_);

  ASSERT_TRUE(deque.try_push_back(1)) << "Allocate initial array";

  allocator_.DisableAllocate();

  // Fill to capacity to force resize.
  for (int i = 0; deque.size() < deque.capacity(); ++i) {
    ASSERT_TRUE(deque.try_push_back(i));
  }

  ASSERT_TRUE(deque.try_push_back(-1)) << "Must resize instead of allocate";

  deque.pop_front();  // Free a slot in the front

  // Fill again, wrap from the back to the front
  for (int i = 0; deque.size() < deque.capacity(); ++i) {
    ASSERT_TRUE(deque.try_push_back(i));
  }

  ASSERT_FALSE(deque.try_reserve_exact(deque.capacity() + 1)) << "Wrapped";

  deque.pop_back();  // remove wrapped element

  ASSERT_TRUE(deque.try_reserve_exact(deque.capacity() + 1))
      << "No longer wrapped";

  // Wrap from the front to the back
  ASSERT_TRUE(deque.try_push_front(123));
  ASSERT_TRUE(deque.try_push_front(1234));

  ASSERT_FALSE(deque.try_reserve_exact(deque.capacity() + 1)) << "Wrapped";

  deque.pop_front();  // remove wrapped element

  ASSERT_TRUE(deque.try_reserve_exact(deque.capacity() + 1))
      << "No longer wrapped";

  // Fill to capacity and wrap to the back
  ASSERT_TRUE(deque.try_push_front(123));
  ASSERT_TRUE(deque.try_push_front(1234));

  ASSERT_FALSE(deque.try_push_front(12345)) << "Wrapped, cannot resize";
  ASSERT_FALSE(deque.try_push_back(12345)) << "Wrapped, cannot resize";

  allocator_.EnableAllocate();
  ASSERT_TRUE(deque.try_push_front(12345));
  ASSERT_TRUE(deque.try_push_back(12345));
}

TEST_F(DynamicDequeTest, Move_MovesBufferWithoutAllocation) {
  pw::DynamicDeque<int> deque_1(allocator_);
  pw::DynamicDeque<int> deque_2(allocator_);

  deque_1.assign({1});
  deque_2.assign({-1, -2, -3, -4, -5});

  const int* const deque_2_first = &deque_2.front();

  allocator_.DisableAll();

  deque_1 = std::move(deque_2);
  EXPECT_TRUE(Equal(deque_1, std::array{-1, -2, -3, -4, -5}));
  EXPECT_EQ(deque_2_first, &deque_1.front());
}

TEST_F(DynamicDequeTest, MoveAssign_DestroysOldElementsAndFreesBuffer) {
  pw::DynamicDeque<Counter> deque_1(allocator_);
  pw::DynamicDeque<Counter> deque_2(allocator_);

  ASSERT_TRUE(deque_1.try_push_back(1));
  ASSERT_TRUE(deque_1.try_push_back(2));

  ASSERT_TRUE(deque_2.try_push_back(3));
  ASSERT_TRUE(deque_2.try_push_back(4));
  ASSERT_TRUE(deque_2.try_push_back(5));

  ASSERT_EQ(deque_1.size(), 2u);
  ASSERT_EQ(deque_2.size(), 3u);

  Counter::Reset();
  const size_t initial_bytes = allocator_for_test_.GetAllocated();

  deque_1 = std::move(deque_2);

  EXPECT_EQ(deque_1.size(), 3u);
  EXPECT_EQ(Counter::destroyed, 2);
  EXPECT_LT(allocator_for_test_.GetAllocated(), initial_bytes);
}

TEST_F(DynamicDequeTest, Capacity_ReserveExactBeforeBufferIsAllocated) {
  pw::DynamicDeque<int> deque(allocator_);

  ASSERT_TRUE(deque.try_reserve_exact(3));
  allocator_.DisableAll();

  deque.push_front(1);
  deque.push_front(2);
  deque.push_front(3);

  EXPECT_FALSE(deque.try_push_back(0));
}

TEST_F(DynamicDequeTest, Capacity_ReserveExactRetriesIfAllocationFails) {
  pw::DynamicDeque<int> deque(allocator_);

  ASSERT_TRUE(deque.try_reserve_exact(3));
  allocator_.DisableAll();

  deque.push_front(1);
  deque.push_front(2);
  deque.push_front(3);

  EXPECT_FALSE(deque.try_push_back(0));
}

TEST_F(DynamicDequeTest, Capacity_ReserveIncreasesByMoreThanOne) {
  pw::DynamicDeque<int> deque(allocator_);

  ASSERT_TRUE(deque.try_reserve_exact(50));

  deque.reserve(51);

  EXPECT_GT(deque.capacity(), 51);

  const auto original_capacity = deque.capacity();
  deque.reserve(52);
  EXPECT_EQ(original_capacity, deque.capacity());
}

TEST_F(DynamicDequeTest, Capacity_ReserveSucceedsWhenCannotDouble) {
  pw::DynamicDeque<int> deque(allocator_);

  ASSERT_TRUE(deque.try_reserve_exact(200));
  ASSERT_EQ(deque.capacity(), 200u);
  ASSERT_FALSE(deque.try_reserve_exact(400));

  EXPECT_TRUE(deque.try_reserve(201));
  EXPECT_LT(deque.capacity(), 400);
  EXPECT_GE(deque.capacity(), 201);
}

TEST_F(DynamicDequeTest, Capacity_ShrinkToFitNopWhenFull) {
  pw::DynamicDeque<int> deque(allocator_);

  deque.reserve_exact(3);
  deque.assign({1, 2, 3});
  ASSERT_EQ(deque.capacity(), 3u);
  ASSERT_EQ(deque.size(), 3u);

  deque.shrink_to_fit();
  ASSERT_EQ(deque.capacity(), 3u);
}

TEST_F(DynamicDequeTest, Capacity_ShrinkToFitResizesWhenPossible) {
  pw::DynamicDeque<int> deque(allocator_);

  deque.reserve_exact(10);
  deque.push_back(1);
  ASSERT_EQ(deque.capacity(), 10u);

  allocator_.DisableAllocate();

  deque.shrink_to_fit();
  ASSERT_EQ(deque.capacity(), 1u);
}

TEST_F(DynamicDequeTest, Capacity_ShrinkToFitOnlyResizesIfHeadIs0) {
  pw::DynamicDeque<int> deque(allocator_);

  // Empty slot is in front, so resize is not possible.
  deque.reserve_exact(4);
  deque.assign({1, 2, 3, 4});
  deque.pop_front();
  ASSERT_TRUE(Equal(deque, std::array{2, 3, 4}));

  // Rely on resize, which isn't possible since there's an empty slot in front.
  allocator_.DisableAllocate();

  deque.shrink_to_fit();
  EXPECT_EQ(deque.capacity(), 4u) << "shrink_to_fit() failed";
  ASSERT_TRUE(Equal(deque, std::array{2, 3, 4}));

  allocator_.EnableAllocate();  // Allow falling back to reallocate

  deque.shrink_to_fit();
  ASSERT_EQ(deque.capacity(), 3u) << "shrink_to_fit() reallocated";
  ASSERT_TRUE(Equal(deque, std::array{2, 3, 4}));
}

TEST_F(DynamicDequeTest, Capacity_ShrinkToFitEmptyFreesBuffer) {
  pw::DynamicDeque<int> deque(allocator_);

  deque.reserve_exact(4);
  ASSERT_EQ(deque.capacity(), 4u);
  deque.clear();
  ASSERT_EQ(deque.capacity(), 4u);

  deque.shrink_to_fit();

  ASSERT_EQ(deque.capacity(), 0u);
}

TEST_F(DynamicDequeTest, Reset_DestroysElementsAndFreesBuffer) {
  pw::DynamicDeque<Counter> deque(allocator_);
  deque.push_back(1);
  deque.push_back(2);
  ASSERT_EQ(deque.size(), 2u);
  Counter::Reset();

  deque.reset();

  EXPECT_EQ(deque.size(), 0u);
  EXPECT_EQ(deque.capacity(), 0u);
  EXPECT_EQ(Counter::destroyed, 2);
}

TEST_F(DynamicDequeTest, Capacity_ShrinkToFailsSilentlyIfCannotShrink) {
  pw::DynamicDeque<int> deque(allocator_);
  deque.reserve_exact(8);
  deque.assign(3u, 123);
  ASSERT_TRUE(Equal(deque, std::array{123, 123, 123}));

  allocator_.DisableAll();
  deque.shrink_to_fit();
  EXPECT_EQ(deque.capacity(), 8u);
}

constexpr size_t kHardcodedMinAllocSize = 4 * sizeof(void*);

TEST_F(DynamicDequeTest, Capacity_MinimumAllocationSizeSmallItem) {
  pw::DynamicDeque<char> deque(allocator_);
  deque.push_back('a');
  EXPECT_EQ(deque.capacity() * sizeof(char), kHardcodedMinAllocSize);
}

TEST_F(DynamicDequeTest, Capacity_MinimumAllocationSizeMediumItem) {
  struct MediumOne {
    char bytes[kHardcodedMinAllocSize / 3];
  };

  pw::DynamicDeque<MediumOne> deque(allocator_);
  deque.push_back(MediumOne{});
  EXPECT_EQ(deque.capacity(), 3u);
}

TEST_F(DynamicDequeTest, Capacity_MinimumAllocationSizeLargeItem) {
  struct BigOne {
    char whoa[128];
  };
  static_assert(sizeof(BigOne) > kHardcodedMinAllocSize);

  pw::DynamicDeque<BigOne> deque(allocator_);
  deque.push_back(BigOne{});
  EXPECT_EQ(deque.capacity(), 1u);
}

TEST_F(DynamicDequeTest, TryAssign_NoPartialAssignments) {
  static bool fail = true;

  struct FailOnCopy {
    constexpr FailOnCopy() = default;

    FailOnCopy(const FailOnCopy& other) { *this = other; }

    FailOnCopy& operator=(const FailOnCopy&) {
      if (fail) {
        ADD_FAILURE() << "Unwanted copy detected!";
      }
      return *this;
    }
  };

  std::array<FailOnCopy, 5> array{};

  pw::DynamicDeque<FailOnCopy> deque(allocator_);
  deque.reserve_exact(4);
  allocator_.DisableAll();

  EXPECT_FALSE(deque.try_assign(array.begin(), array.end()));
  EXPECT_TRUE(deque.empty());

  EXPECT_FALSE(deque.try_assign(5, FailOnCopy()));
  EXPECT_TRUE(deque.empty());

  fail = false;  // allow copies now

  EXPECT_TRUE(deque.try_assign(array.begin(), array.end() - 1));
  EXPECT_EQ(deque.size(), 4u);

  EXPECT_TRUE(deque.try_assign(2, FailOnCopy()));
  EXPECT_EQ(deque.size(), 2u);
}

TEST_F(DynamicDequeTest, MaxSize_BasedOnSizeType) {
  EXPECT_EQ((pw::DynamicDeque<int, uint8_t>(allocator_).max_size()), 255u);
  EXPECT_EQ((pw::DynamicDeque<int, uint16_t>(allocator_).max_size()), 65535u);
  EXPECT_EQ((pw::DynamicDeque<int, uint32_t>(allocator_).max_size()),
            std::numeric_limits<uint32_t>::max());
}

TEST_F(DynamicDequeTest, MaxSize_CannotExceed) {
  pw::DynamicDeque<bool, uint8_t> deque(allocator_);
  deque.assign(255, true);
  ASSERT_EQ(deque.size(), 255u);
  ASSERT_EQ(deque.capacity(), 255u);

  EXPECT_FALSE(deque.try_push_back(false));
  EXPECT_FALSE(deque.try_push_front(true));
  EXPECT_EQ(deque.try_insert(deque.begin(), {true, false, true, false}),
            std::nullopt);
}

TEST_F(DynamicDequeTest, MaxSize_CapacityClamps) {
  pw::DynamicDeque<bool, uint8_t> deque(allocator_);
  deque.assign(200, true);
  EXPECT_EQ(deque.capacity(), 200u);

  ASSERT_TRUE(deque.try_push_back(false));
  EXPECT_EQ(deque.capacity(), 255u);
}

TEST_F(DynamicDequeTest, Erase_Wrapped) {
  pw::DynamicDeque<int> deque(allocator_);
  deque.reserve_exact(5);
  deque.assign({1, 2, 3, 4, 5});
  deque.pop_front();
  deque.pop_front();
  deque.pop_front();
  deque.push_back(6);
  deque.push_back(7);
  deque.push_back(8);

  ASSERT_TRUE(Equal(deque, std::array{4, 5, 6, 7, 8}));

  ASSERT_LT(&deque.back(), &deque.front()) << "Must be wrapped";

  pw::DynamicDeque<int>::iterator it = deque.erase(deque.begin() + 2);
  EXPECT_EQ(*it, 7);
  ASSERT_TRUE(Equal(deque, std::array{4, 5, 7, 8}));

  it = deque.erase(deque.begin() + 1, deque.begin() + 3);
  EXPECT_EQ(*it, 8);
  ASSERT_TRUE(Equal(deque, std::array{4, 8}));

  it = deque.erase(deque.begin() + 1, deque.end());
  EXPECT_EQ(it, deque.end());
  ASSERT_TRUE(Equal(deque, std::array{4}));
}

TEST_F(DynamicDequeTest, Erase_Wrapped_RangeAcrossWrap) {
  pw::DynamicDeque<int> deque(allocator_);
  deque.reserve_exact(5);
  deque.assign({1, 2, 3, 4, 5});
  deque.pop_front();
  deque.pop_front();
  deque.pop_front();
  deque.push_back(6);
  deque.push_back(7);
  deque.push_back(8);

  ASSERT_TRUE(Equal(deque, std::array{4, 5, 6, 7, 8}));

  ASSERT_LT(&deque.back(), &deque.front()) << "Must be wrapped";

  auto it = deque.erase(deque.begin() + 1, deque.begin() + 4);
  EXPECT_EQ(*it, 8);
  ASSERT_TRUE(Equal(deque, std::array{4, 8}));
}

TEST_F(DynamicDequeTest, Erase_Wrapped_All) {
  pw::DynamicDeque<int> deque(allocator_);
  deque.reserve_exact(5);
  deque.assign({1, 2, 3, 4, 5});
  deque.pop_front();
  deque.pop_front();
  deque.pop_front();
  deque.push_back(6);
  deque.push_back(7);
  deque.push_back(8);

  ASSERT_TRUE(Equal(deque, std::array{4, 5, 6, 7, 8}));

  ASSERT_LT(&deque.back(), &deque.front()) << "Must be wrapped";

  pw::DynamicDeque<int>::iterator it = deque.erase(deque.begin(), deque.end());
  EXPECT_EQ(it, deque.end());
  ASSERT_TRUE(deque.empty());
}

class DynamicDequeWrappedTest : public DynamicDequeTest {
 protected:
  DynamicDequeWrappedTest() : deque_(allocator_) {
    deque_.reserve_exact(10);
    for (int i = 0; i < 10; ++i) {
      deque_.push_back(i);
    }
    for (int i = 0; i < 5; ++i) {
      deque_.pop_front();
      deque_.push_back(i + 10);
    }
    EXPECT_TRUE(Equal(deque_, std::array{5, 6, 7, 8, 9, 10, 11, 12, 13, 14}));
    EXPECT_LT(&deque_.back(), &deque_.front()) << "Deque must be wrapped";
  }

  pw::DynamicDeque<int> deque_;
};

TEST_F(DynamicDequeWrappedTest, Insert_NearBegin_FewerThanBefore) {
  std::array<int, 1> to_insert = {99};
  auto it =
      deque_.insert(deque_.begin() + 2, to_insert.begin(), to_insert.end());
  EXPECT_EQ(*it, 99);
  ASSERT_TRUE(Equal(deque_, std::array{5, 6, 99, 7, 8, 9, 10, 11, 12, 13, 14}));
}

TEST_F(DynamicDequeWrappedTest, Insert_NearBegin_SameAsBefore) {
  std::array<int, 2> to_insert = {98, 99};
  auto it =
      deque_.insert(deque_.begin() + 2, to_insert.begin(), to_insert.end());
  EXPECT_EQ(*it, 98);
  ASSERT_TRUE(
      Equal(deque_, std::array{5, 6, 98, 99, 7, 8, 9, 10, 11, 12, 13, 14}));
}

TEST_F(DynamicDequeWrappedTest, Insert_NearBegin_MoreThanBefore) {
  std::array<int, 3> to_insert = {97, 98, 99};
  auto it =
      deque_.insert(deque_.begin() + 2, to_insert.begin(), to_insert.end());
  EXPECT_EQ(*it, 97);
  ASSERT_TRUE(
      Equal(deque_, std::array{5, 6, 97, 98, 99, 7, 8, 9, 10, 11, 12, 13, 14}));
}

TEST_F(DynamicDequeWrappedTest, Insert_NearEnd_FewerThanAfter) {
  std::array<int, 1> to_insert = {99};
  auto it = deque_.insert(deque_.end() - 2, to_insert.begin(), to_insert.end());
  EXPECT_EQ(*it, 99);
  ASSERT_TRUE(Equal(deque_, std::array{5, 6, 7, 8, 9, 10, 11, 12, 99, 13, 14}));
}

TEST_F(DynamicDequeWrappedTest, Insert_NearEnd_SameAsAfter) {
  std::array<int, 2> to_insert = {98, 99};
  auto it = deque_.insert(deque_.end() - 2, to_insert.begin(), to_insert.end());
  EXPECT_EQ(*it, 98);
  ASSERT_TRUE(
      Equal(deque_, std::array{5, 6, 7, 8, 9, 10, 11, 12, 98, 99, 13, 14}));
}

TEST_F(DynamicDequeWrappedTest, Insert_NearEnd_MoreThanAfter) {
  std::array<int, 3> to_insert = {97, 98, 99};
  auto it = deque_.insert(deque_.end() - 2, to_insert.begin(), to_insert.end());
  EXPECT_EQ(*it, 97);
  ASSERT_TRUE(
      Equal(deque_, std::array{5, 6, 7, 8, 9, 10, 11, 12, 97, 98, 99, 13, 14}));
}

TEST_F(DynamicDequeWrappedTest, Insert_CopiesNearBegin_FewerThanBefore) {
  auto it = deque_.insert(deque_.begin() + 2, 1, 99);
  EXPECT_EQ(*it, 99);
  ASSERT_TRUE(Equal(deque_, std::array{5, 6, 99, 7, 8, 9, 10, 11, 12, 13, 14}));
}

TEST_F(DynamicDequeWrappedTest, Insert_CopiesNearBegin_SameAsBefore) {
  auto it = deque_.insert(deque_.begin() + 2, 2, 99);
  EXPECT_EQ(*it, 99);
  ASSERT_TRUE(
      Equal(deque_, std::array{5, 6, 99, 99, 7, 8, 9, 10, 11, 12, 13, 14}));
}

TEST_F(DynamicDequeWrappedTest, Insert_CopiesNearBegin_MoreThanBefore) {
  auto it = deque_.insert(deque_.begin() + 2, 3, 99);
  EXPECT_EQ(*it, 99);
  ASSERT_TRUE(
      Equal(deque_, std::array{5, 6, 99, 99, 99, 7, 8, 9, 10, 11, 12, 13, 14}));
}

TEST_F(DynamicDequeWrappedTest, Insert_CopiesNearEnd_FewerThanAfter) {
  auto it = deque_.insert(deque_.end() - 2, 1, 99);
  EXPECT_EQ(*it, 99);
  ASSERT_TRUE(Equal(deque_, std::array{5, 6, 7, 8, 9, 10, 11, 12, 99, 13, 14}));
}

TEST_F(DynamicDequeWrappedTest, Insert_CopiesNearEnd_SameAsAfter) {
  auto it = deque_.insert(deque_.end() - 2, 2, 99);
  EXPECT_EQ(*it, 99);
  ASSERT_TRUE(
      Equal(deque_, std::array{5, 6, 7, 8, 9, 10, 11, 12, 99, 99, 13, 14}));
}

TEST_F(DynamicDequeWrappedTest, Insert_CopiesNearEnd_MoreThanAfter) {
  auto it = deque_.insert(deque_.end() - 2, 3, 99);
  EXPECT_EQ(*it, 99);
  ASSERT_TRUE(
      Equal(deque_, std::array{5, 6, 7, 8, 9, 10, 11, 12, 99, 99, 99, 13, 14}));
}

TEST_F(DynamicDequeWrappedTest, Insert_InputIterator_NearBegin) {
  using pw::containers::test::InputIt;
  auto it = deque_.insert(deque_.begin() + 2, InputIt(97), InputIt(100));
  EXPECT_EQ(*it, 97);
  ASSERT_TRUE(
      Equal(deque_, std::array{5, 6, 97, 98, 99, 7, 8, 9, 10, 11, 12, 13, 14}));
}

TEST_F(DynamicDequeWrappedTest, Insert_InputIterator_NearEnd) {
  using pw::containers::test::InputIt;
  auto it = deque_.insert(deque_.end() - 2, InputIt(97), InputIt(100));
  EXPECT_EQ(*it, 97);
  ASSERT_TRUE(
      Equal(deque_, std::array{5, 6, 7, 8, 9, 10, 11, 12, 97, 98, 99, 13, 14}));
}

TEST_F(DynamicDequeWrappedTest, Insert_InputIterator_MoreThanSize) {
  using pw::containers::test::InputIt;
  deque_.assign({1, 2});
  auto it = deque_.insert(deque_.begin() + 1, InputIt(3), InputIt(15));
  EXPECT_EQ(*it, 3);
  ASSERT_TRUE(
      Equal(deque_, std::array{1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 2}));
}

TEST_F(DynamicDequeTest, Swap_BothEmpty) {
  pw::DynamicDeque<Counter> container_1(allocator_);
  pw::DynamicDeque<Counter> container_2(null_allocator);

  container_1.swap(container_2);

  EXPECT_TRUE(container_1.empty());
  EXPECT_TRUE(container_2.empty());

  EXPECT_EQ(&container_1.get_allocator(), &null_allocator);
  EXPECT_EQ(&container_2.get_allocator(), &allocator_);
}

TEST_F(DynamicDequeTest, Swap_EmptyToNonEmpty) {
  pw::DynamicDeque<Counter> container_1(allocator_);
  container_1.assign({1, 2});

  pw::DynamicDeque<Counter> container_2(null_allocator);

  container_1.swap(container_2);

  EXPECT_TRUE(container_1.empty());
  EXPECT_TRUE(Equal(container_2, std::array{1, 2}));

  EXPECT_EQ(&container_1.get_allocator(), &null_allocator);
  EXPECT_EQ(&container_2.get_allocator(), &allocator_);
}

TEST_F(DynamicDequeTest, Swap_NonEmptyToEmpty) {
  pw::DynamicDeque<Counter> container_1(allocator_);

  pw::DynamicDeque<Counter> container_2(allocator_);
  container_2.assign({-1, -2, -3, -4});
  container_2.pop_front();
  container_2.pop_front();
  container_2.push_back(-5);

  container_1.swap(container_2);

  EXPECT_TRUE(Equal(container_1, std::array{-3, -4, -5}));
  EXPECT_TRUE(container_2.empty());
}

TEST_F(DynamicDequeTest, Swap_BothNonEmpty) {
  pw::DynamicDeque<Counter> container_1(allocator_);
  container_1.assign({1, 2});

  pw::DynamicDeque<Counter> container_2(allocator_);
  container_2.assign({-1, -2, -3, -4});
  container_2.pop_front();

  container_1.swap(container_2);

  EXPECT_TRUE(Equal(container_1, std::array{-2, -3, -4}));
  EXPECT_TRUE(Equal(container_2, std::array{1, 2}));
}

TEST_F(DynamicDequeTest, Modify_TryEmplace_Empty) {
  pw::DynamicDeque<Counter> container(allocator_);
  Counter::Reset();

  auto it = container.try_emplace(container.cbegin(), 1);
  ASSERT_TRUE(it.has_value());
  EXPECT_EQ(**it, 1);
  EXPECT_TRUE(Equal(container, std::array{1}));
  EXPECT_EQ(Counter::created, 1);
}

TEST_F(DynamicDequeTest, Modify_TryEmplace_Front) {
  pw::DynamicDeque<Counter> container(allocator_);
  container.assign({1, 2, 3});
  Counter::Reset();

  auto it = container.try_emplace(container.cbegin(), 0);
  ASSERT_TRUE(it.has_value());
  EXPECT_EQ(**it, 0);
  EXPECT_TRUE(Equal(container, std::array{0, 1, 2, 3}));
  EXPECT_EQ(Counter::created, 1);
}

TEST_F(DynamicDequeTest, Modify_TryEmplace_Back) {
  pw::DynamicDeque<Counter> container(allocator_);
  container.assign({1, 2, 3});
  Counter::Reset();

  auto it = container.try_emplace(container.cend(), 4);
  ASSERT_TRUE(it.has_value());
  EXPECT_EQ(**it, 4);
  EXPECT_TRUE(Equal(container, std::array{1, 2, 3, 4}));
  EXPECT_EQ(Counter::created, 1);
}

TEST_F(DynamicDequeTest, Modify_TryEmplace_Middle) {
  pw::DynamicDeque<Counter> container(allocator_);
  container.assign({1, 2, 4});

  auto it = container.try_emplace(container.cbegin() + 2, 3);
  ASSERT_TRUE(it.has_value());
  EXPECT_EQ(**it, 3);
  EXPECT_TRUE(Equal(container, std::array{1, 2, 3, 4}));
}

TEST_F(DynamicDequeTest, Modify_TryInsertCopy) {
  pw::DynamicDeque<Counter> container(allocator_);
  container.assign({1, 2, 4});
  Counter value(3);

  auto it = container.try_insert(container.cbegin() + 2, value);
  ASSERT_TRUE(it.has_value());
  EXPECT_EQ(*it, container.begin() + 2);
  EXPECT_EQ(**it, 3);
  EXPECT_TRUE(Equal(container, std::array{1, 2, 3, 4}));
}

TEST_F(DynamicDequeTest, Modify_TryInsertMove) {
  pw::DynamicDeque<Counter> container(allocator_);
  container.assign({1, 2, 4});
  Counter value(3);

  auto it = container.try_insert(container.cbegin() + 2, std::move(value));
  ASSERT_TRUE(it.has_value());
  EXPECT_EQ(*it, container.begin() + 2);
  EXPECT_EQ(**it, 3);
  EXPECT_TRUE(Equal(container, std::array{1, 2, 3, 4}));
}

TEST_F(DynamicDequeTest, Modify_TryInsertMultiple) {
  pw::DynamicDeque<Counter> container(allocator_);
  container.assign({1, 2, 5});
  Counter value(3);

  auto it = container.try_insert(container.cbegin() + 2, 2, value);
  ASSERT_TRUE(it.has_value());
  EXPECT_EQ(*it, container.begin() + 2);
  EXPECT_EQ(**it, 3);
  EXPECT_TRUE(Equal(container, std::array{1, 2, 3, 3, 5}));
}

TEST_F(DynamicDequeTest, Modify_TryEmplace_Fails) {
  pw::DynamicDeque<Counter> container(allocator_);
  container.assign({1, 2, 4});
  Counter::Reset();

  allocator_.DisableAll();
  auto it = container.try_emplace(container.cbegin() + 2, 3);

  ASSERT_FALSE(it.has_value());
  EXPECT_TRUE(Equal(container, std::array{1, 2, 4}));
  EXPECT_EQ(Counter::created, 0);

  allocator_.EnableAll();
  it = container.try_emplace(container.cbegin() + 2, 3);
  ASSERT_TRUE(it.has_value());
  EXPECT_TRUE(Equal(container, std::array{1, 2, 3, 4}));
}

TEST_F(DynamicDequeTest, Modify_TryInsertCopy_Fails) {
  pw::DynamicDeque<Counter> container(allocator_);
  container.assign({1, 2, 4});
  Counter value(3);
  Counter::Reset();

  allocator_.DisableAll();
  auto it = container.try_insert(container.cbegin() + 2, value);

  ASSERT_FALSE(it.has_value());
  EXPECT_TRUE(Equal(container, std::array{1, 2, 4}));
  EXPECT_EQ(Counter::created, 0);

  allocator_.EnableAll();
  it = container.try_insert(container.cbegin() + 2, value);
  ASSERT_TRUE(it.has_value());
  EXPECT_TRUE(Equal(container, std::array{1, 2, 3, 4}));
}

TEST_F(DynamicDequeTest, Modify_TryInsertMove_Fails) {
  pw::DynamicDeque<Counter> container(allocator_);
  container.assign({1, 2, 4});
  Counter value(3);
  Counter::Reset();

  allocator_.DisableAll();
  auto it = container.try_insert(container.cbegin() + 2, std::move(value));

  ASSERT_FALSE(it.has_value());
  EXPECT_TRUE(Equal(container, std::array{1, 2, 4}));
  EXPECT_EQ(Counter::created, 0);
  EXPECT_EQ(Counter::moved, 0);

  allocator_.EnableAll();
  // NOLINTNEXTLINE(bugprone-use-after-move)
  it = container.try_insert(container.cbegin() + 2, std::move(value));
  ASSERT_TRUE(it.has_value());
  EXPECT_TRUE(Equal(container, std::array{1, 2, 3, 4}));
}

TEST_F(DynamicDequeTest, Modify_TryInsertMultiple_Fails) {
  pw::DynamicDeque<Counter> container(allocator_);
  container.assign({1, 2, 5});
  Counter value(3);
  Counter::Reset();

  allocator_.DisableAll();
  auto it = container.try_insert(container.cbegin() + 2, 2, value);

  ASSERT_FALSE(it.has_value());
  EXPECT_TRUE(Equal(container, std::array{1, 2, 5}));
  EXPECT_EQ(Counter::created, 0);

  allocator_.EnableAll();
  it = container.try_insert(container.cbegin() + 2, 2, value);
  ASSERT_TRUE(it.has_value());
  EXPECT_TRUE(Equal(container, std::array{1, 2, 3, 3, 5}));
}

TEST_F(DynamicDequeTest, Modify_TryInsert_Begin) {
  pw::DynamicDeque<int> deque(allocator_);
  deque.assign({1, 2, 3});
  auto it = deque.try_insert(deque.begin(), 0);
  ASSERT_TRUE(it.has_value());
  EXPECT_EQ(**it, 0);
  ASSERT_TRUE(Equal(deque, std::array{0, 1, 2, 3}));
}

TEST_F(DynamicDequeTest, Modify_TryInsert_Begin_Fails) {
  pw::DynamicDeque<int> deque(allocator_);
  deque.reserve_exact(3);
  deque.assign({1, 2, 3});
  allocator_.DisableAll();
  ASSERT_FALSE(deque.try_insert(deque.begin(), 0).has_value());
  ASSERT_TRUE(Equal(deque, std::array{1, 2, 3}));
}

TEST_F(DynamicDequeTest, Modify_TryInsert_End) {
  pw::DynamicDeque<int> deque(allocator_);
  deque.assign({1, 2, 3});
  auto it = deque.try_insert(deque.end(), 4);
  ASSERT_TRUE(it.has_value());
  EXPECT_EQ(**it, 4);
  ASSERT_TRUE(Equal(deque, std::array{1, 2, 3, 4}));
}

TEST_F(DynamicDequeTest, Modify_TryInsert_End_Fails) {
  pw::DynamicDeque<int> deque(allocator_);
  deque.reserve_exact(3);
  deque.assign({1, 2, 3});
  allocator_.DisableAll();
  ASSERT_FALSE(deque.try_insert(deque.end(), 4).has_value());
  ASSERT_TRUE(Equal(deque, std::array{1, 2, 3}));
}

TEST_F(DynamicDequeTest, Modify_TryInsertIterators) {
  pw::DynamicDeque<Counter> container(allocator_);
  container.assign({1, 5});
  std::array<Counter, 3> values = {Counter(2), Counter(3), Counter(4)};

  auto it = container.try_insert(container.cbegin() + 1,
                                 std::make_move_iterator(values.begin()),
                                 std::make_move_iterator(values.end()));
  ASSERT_TRUE(it.has_value());
  EXPECT_EQ(*it, container.begin() + 1);
  EXPECT_TRUE(Equal(container, std::array{1, 2, 3, 4, 5}));
}

TEST_F(DynamicDequeTest, Modify_TryInsertInitializerList) {
  pw::DynamicDeque<Counter> container(allocator_);
  container.assign({1, 5});

  auto it = container.try_insert(container.cbegin() + 1, {2, 3, 4});
  ASSERT_TRUE(it.has_value());
  EXPECT_EQ(*it, container.begin() + 1);
  EXPECT_TRUE(Equal(container, std::array{1, 2, 3, 4, 5}));
}

void PerformRandomOperations(int iterations, uint_fast32_t seed) {
  static std::byte buffer[2048];
  std::memset(buffer, 0, sizeof(buffer));

  pw::allocator::FirstFitAllocator<> allocator(buffer);
  pw::DynamicDeque<Counter> deque(allocator);

  std::deque<int> oracle;

  std::mt19937 random(seed);
  std::uniform_int_distribution<unsigned> distro;
  auto random_uint = [&] { return distro(random); };

  enum Op : unsigned {
    kPushBack,
    kPushFront,
    kPopBack,
    kPopFront,
    kErase,
    kReserve,
    kShrinkToFit,
    kTryEmplace,
    kTryInsertCopy,
    kTryInsertCount,
    kTryInsertIterators,
    kTotalOperations,
  };

  using diff_t = pw::DynamicDeque<Counter>::difference_type;
  bool tend_to_grow = true;

  for (int i = 0; i < iterations; ++i) {
    switch (static_cast<Op>(random_uint() % kTotalOperations)) {
      case kPushBack: {
        const int value = static_cast<int>(random_uint());
        if (deque.try_push_back(value)) {
          oracle.push_back(value);
        } else {
          tend_to_grow = false;
        }
        break;
      }
      case kPushFront: {
        const int value = static_cast<int>(random_uint());
        if (deque.try_push_front(value)) {
          oracle.push_front(value);
        } else {
          tend_to_grow = false;
        }
        break;
      }
      case kPopBack:
        if (tend_to_grow && (random_uint() % 2) == 0u) {
          continue;
        }
        if (deque.empty()) {
          tend_to_grow = true;
        } else {
          deque.pop_back();
          oracle.pop_back();
        }
        break;
      case kPopFront:
        if (tend_to_grow && (random_uint() % 2) == 0u) {
          continue;
        }
        if (deque.empty()) {
          tend_to_grow = true;
        } else {
          deque.pop_front();
          oracle.pop_front();
        }
        break;
      case kErase:
        if (tend_to_grow && (random_uint() % 2) == 0u) {
          continue;
        }
        if (deque.empty()) {
          tend_to_grow = true;
        } else {
          diff_t pos = static_cast<diff_t>(random_uint() % oracle.size());
          const auto count = static_cast<diff_t>(
              random_uint() % (oracle.size() - static_cast<size_t>(pos)) + 1);
          deque.erase(deque.begin() + pos, deque.begin() + pos + count);
          oracle.erase(oracle.begin() + pos, oracle.begin() + pos + count);
        }
        break;
      case kReserve:
        std::ignore =
            deque.try_reserve_exact(deque.size() + random_uint() % 100);
        break;
      case kShrinkToFit:
        deque.shrink_to_fit();
        break;
      case kTryEmplace: {
        diff_t pos = static_cast<diff_t>(random_uint() % (oracle.size() + 1));
        const int value = static_cast<int>(random_uint());
        if (deque.try_emplace(deque.begin() + pos, value).has_value()) {
          oracle.emplace(oracle.begin() + pos, value);
        } else {
          tend_to_grow = false;
        }
        break;
      }
      case kTryInsertCopy: {
        diff_t pos = static_cast<diff_t>(random_uint() % (oracle.size() + 1));
        const int value = static_cast<int>(random_uint());
        if (deque.try_insert(deque.begin() + pos, value).has_value()) {
          oracle.insert(oracle.begin() + pos, value);
        } else {
          tend_to_grow = false;
        }
        break;
      }
      case kTryInsertCount: {
        diff_t pos = static_cast<diff_t>(random_uint() % (oracle.size() + 1));
        const auto count = static_cast<pw::DynamicDeque<Counter>::size_type>(
            random_uint() % 100);
        const int value = static_cast<int>(random_uint());
        if (deque.try_insert(deque.begin() + pos, count, value).has_value()) {
          oracle.insert(oracle.begin() + pos, count, value);
        } else {
          tend_to_grow = false;
        }
        break;
      }
      case kTryInsertIterators: {
        diff_t pos = static_cast<diff_t>(random_uint() % (oracle.size() + 1));
        const size_t count = random_uint() % 5;
        std::vector<int> source;
        source.reserve(count);
        for (size_t k = 0; k < count; ++k) {
          source.push_back(static_cast<int>(random_uint()));
        }

        if (deque.try_insert(deque.begin() + pos, source.begin(), source.end())
                .has_value()) {
          oracle.insert(oracle.begin() + pos, source.begin(), source.end());
        } else {
          tend_to_grow = false;
        }
        break;
      }
      case kTotalOperations:
      default:
        FAIL();
    }

    ASSERT_EQ(deque.size(), oracle.size());
    for (decltype(deque)::size_type j = 0; j < deque.size(); ++j) {
      ASSERT_EQ(deque[j], oracle[j]);
    }
  }
}

TEST(DynamicDeque, RandomOperations) {
  PerformRandomOperations(10000, 1);
  PerformRandomOperations(1000, 98);
  PerformRandomOperations(1000, 5555);
}

// Instantiate shared iterator tests.
static_assert(
    pw::containers::test::IteratorProperties<pw::DynamicDeque>::kPasses);

// Test that DynamicDeque<T> is NOT copy constructible
static_assert(!std::is_copy_constructible_v<pw::DynamicDeque<int>>);

// Test that DynamicDeque<T> is move constructible
static_assert(std::is_move_constructible_v<pw::DynamicDeque<MoveOnly>>);

// Test that DynamicDeque<T> is NOT copy assignable
static_assert(!std::is_copy_assignable_v<pw::DynamicDeque<CopyOnly>>);

// Test that DynamicDeque<T> is move assignable
static_assert(std::is_move_assignable_v<pw::DynamicDeque<MoveOnly>>);

// Check padding / layout of the object.
struct Uint8Layout {
  uint8_t fields[4];
  void* allocator;
  void* buffer;
};

static_assert(sizeof(pw::DynamicDeque<int, uint8_t>) == sizeof(Uint8Layout));
static_assert(sizeof(pw::DynamicDeque<long long, uint16_t>) ==
              4 * sizeof(uint16_t) + 2 * sizeof(void*));
static_assert(sizeof(pw::DynamicDeque<int, uint32_t>) ==
              4 * sizeof(uint32_t) + 2 * sizeof(void*));

#if PW_NC_TEST(DynamicDeque_EmplaceBackOverwriteHidden)
PW_NC_EXPECT("(is inaccessible|is (a )?private member of)");
void Test(pw::DynamicDeque<int>& deque) { deque.emplace_back_overwrite(1); }
#elif PW_NC_TEST(DynamicDeque_PushBackOverwriteHidden)
PW_NC_EXPECT("(is inaccessible|is (a )?private member of)");
void Test(pw::DynamicDeque<int>& deque) { deque.push_back_overwrite(1); }
#endif

}  // namespace
