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

#include "pw_containers/dynamic_vector.h"

#include <array>
#include <cstddef>
#include <utility>

#include "pw_allocator/fault_injecting_allocator.h"
#include "pw_allocator/testing.h"
#include "pw_containers/internal/test_helpers.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::containers::test::Counter;

class DynamicVectorTest : public ::testing::Test {
 protected:
  DynamicVectorTest() : allocator_(allocator_for_test_) {}

  pw::allocator::test::AllocatorForTest<1024> allocator_for_test_;
  pw::allocator::test::FaultInjectingAllocator allocator_;
};

TEST_F(DynamicVectorTest, BasicOperations) {
  pw::DynamicVector<Counter> queue(allocator_);
}

TEST_F(DynamicVectorTest, Constructor) {
  pw::DynamicVector<Counter> vec(allocator_);
  EXPECT_TRUE(vec.empty());
  EXPECT_EQ(vec.size(), 0u);
  EXPECT_GE(vec.capacity(), 0u);
}

TEST_F(DynamicVectorTest, Iterators) {
  pw::DynamicVector<Counter> vec(allocator_);
  vec.assign({10, 20, 30});

  int expected_value = 10;
  for (auto it = vec.begin(); it != vec.end(); ++it) {
    EXPECT_EQ(*it, expected_value);
    expected_value += 10;
  }
  EXPECT_EQ(expected_value, 40);  // Check all elements iterated

  expected_value = 10;
  for (auto it = vec.cbegin(); it != vec.cend(); ++it) {
    EXPECT_EQ(*it, expected_value);
    expected_value += 10;
  }

  expected_value = 30;
  for (auto it = vec.rbegin(); it != vec.rend(); ++it) {
    EXPECT_EQ(*it, expected_value);
    expected_value -= 10;
  }
  EXPECT_EQ(expected_value, 0);  // Check all elements iterated in reverse

  pw::DynamicVector<Counter>::const_iterator it =
      pw::DynamicVector<Counter>::iterator();
  EXPECT_NE(it, vec.begin());
  EXPECT_EQ(vec.begin(), vec.cbegin());
}

TEST_F(DynamicVectorTest, CapacityMethods) {
  pw::DynamicVector<Counter> vec(allocator_);

  EXPECT_TRUE(vec.empty());
  EXPECT_EQ(vec.size(), 0u);
  EXPECT_GE(vec.capacity(), 0u);

  vec.push_back(1);
  EXPECT_FALSE(vec.empty());
  EXPECT_EQ(vec.size(), 1u);
  EXPECT_GE(vec.capacity(), 1u);

  vec.reserve(10);
  EXPECT_GE(vec.capacity(), 10u);
  EXPECT_EQ(vec.size(), 1u);

  EXPECT_TRUE(vec.try_reserve(20));
  EXPECT_GE(vec.capacity(), 20u);
  const auto last_capacity = vec.capacity();

  allocator_.DisableAll();
  EXPECT_FALSE(vec.try_reserve(100));
  EXPECT_EQ(vec.capacity(), last_capacity);

  vec.push_back(2);
  vec.shrink_to_fit();
  EXPECT_EQ(vec.size(), 2u);
  EXPECT_GE(vec.capacity(), 2u);

  allocator_.EnableAll();
  EXPECT_TRUE(vec.try_reserve_exact(23));
  EXPECT_EQ(vec.capacity(), 23u);

  vec.reserve_exact(5);
  EXPECT_EQ(vec.capacity(), 23u);
}

TEST_F(DynamicVectorTest, ElementAccess) {
  pw::DynamicVector<Counter> vec(allocator_);
  vec.assign({10, 20, 30});

  EXPECT_EQ(vec[0], 10);
  EXPECT_EQ(vec[1], 20);
  EXPECT_EQ(vec[2], 30);

  EXPECT_EQ(vec.at(0), 10);
  EXPECT_EQ(vec.at(1), 20);
  EXPECT_EQ(vec.at(2), 30);

  EXPECT_EQ(vec.front(), 10);
  EXPECT_EQ(vec.back(), 30);

  *vec.data() = 5;
  EXPECT_EQ(vec[0], 5);
  EXPECT_EQ(*vec.data(), 5);

  const pw::DynamicVector<Counter>& const_vec = vec;
  EXPECT_EQ(const_vec[0], 5);
  EXPECT_EQ(const_vec.at(1), 20);
  EXPECT_EQ(const_vec.front(), 5);
  EXPECT_EQ(const_vec.back(), 30);
  EXPECT_EQ(*const_vec.data(), 5);
}

TEST_F(DynamicVectorTest, AssignCopies) {
  pw::DynamicVector<Counter> vec(allocator_);

  EXPECT_TRUE(vec.try_assign(3, 7));  // Assign 3 copies of 7
  EXPECT_EQ(vec.size(), 3u);
  EXPECT_EQ(vec[0], 7);
  EXPECT_EQ(vec[1], 7);
  EXPECT_EQ(vec[2], 7);
}

TEST_F(DynamicVectorTest, AssignInitializerList) {
  pw::DynamicVector<Counter> vec(allocator_);

  EXPECT_TRUE(vec.try_assign({10, 20}));
  EXPECT_EQ(vec.size(), 2u);
  EXPECT_EQ(vec[0], 10);
  EXPECT_EQ(vec[1], 20);
}

TEST_F(DynamicVectorTest, AssignFails) {
  pw::DynamicVector<Counter> vec(allocator_);

  allocator_.DisableAll();
  EXPECT_FALSE(vec.try_assign(5, 1));
  EXPECT_TRUE(vec.empty());
}

TEST_F(DynamicVectorTest, PushBackMethods) {
  pw::DynamicVector<Counter> vec(allocator_);

  vec.push_back(10);
  EXPECT_EQ(vec.size(), 1u);
  EXPECT_EQ(vec.back(), 10);

  vec.push_back(Counter(20));  // rvalue push
  EXPECT_EQ(vec.size(), 2u);
  EXPECT_EQ(vec.back(), 20);

  EXPECT_TRUE(vec.try_push_back(30));
  EXPECT_EQ(vec.size(), 3u);
  EXPECT_EQ(vec.back(), 30);

  vec.shrink_to_fit();
  ASSERT_EQ(vec.size(), vec.capacity());

  allocator_.DisableAll();
  EXPECT_FALSE(vec.try_push_back(40));
  EXPECT_EQ(vec.size(), 3u);
}

TEST_F(DynamicVectorTest, PopBack) {
  pw::DynamicVector<Counter> vec(allocator_);
  vec.assign({1, 2, 3});
  EXPECT_EQ(vec.size(), 3u);

  vec.pop_back();
  EXPECT_EQ(vec.size(), 2u);
  EXPECT_EQ(vec.back(), 2);

  vec.pop_back();
  EXPECT_EQ(vec.size(), 1u);
  EXPECT_EQ(vec.back(), 1);

  vec.pop_back();
  EXPECT_TRUE(vec.empty());
}

TEST_F(DynamicVectorTest, EmplaceBackMethods) {
  pw::DynamicVector<std::pair<int, char>> vec(allocator_);

  vec.emplace_back(1, 'a');
  EXPECT_EQ(vec.size(), 1u);
  EXPECT_EQ(vec.back().first, 1);
  EXPECT_EQ(vec.back().second, 'a');

  EXPECT_TRUE(vec.try_emplace_back(2, 'b'));
  EXPECT_EQ(vec.size(), 2u);
  EXPECT_EQ(vec.back().first, 2);
  EXPECT_EQ(vec.back().second, 'b');

  vec.shrink_to_fit();
  ASSERT_EQ(vec.size(), vec.capacity());

  allocator_.DisableAll();
  EXPECT_FALSE(vec.try_emplace_back(3, 'c'));
  EXPECT_EQ(vec.size(), 2u);
}

TEST_F(DynamicVectorTest, Emplace) {
  pw::DynamicVector<std::pair<int, char>> vec(allocator_);
  vec.assign({std::make_pair(1, 'a'), std::make_pair(3, 'c')});

  auto it = vec.emplace(vec.begin() + 1, 2, 'b');
  EXPECT_EQ(it, vec.begin() + 1);
  EXPECT_EQ(vec.size(), 3u);
  EXPECT_EQ(vec[0], std::make_pair(1, 'a'));
  EXPECT_EQ(vec[1], std::make_pair(2, 'b'));
  EXPECT_EQ(vec[2], std::make_pair(3, 'c'));
}

TEST_F(DynamicVectorTest, TryEmplace) {
  pw::DynamicVector<std::pair<int, char>> vec(allocator_);
  vec.assign({std::make_pair(1, 'a'), std::make_pair(3, 'c')});

  auto it = vec.try_emplace(vec.begin() + 1, 2, 'b');
  ASSERT_TRUE(it.has_value());
  EXPECT_EQ(*it, vec.begin() + 1);
  EXPECT_EQ(vec.size(), 3u);
  EXPECT_EQ(vec[1], std::make_pair(2, 'b'));

  vec.shrink_to_fit();
  ASSERT_EQ(vec.size(), vec.capacity());

  allocator_.DisableAll();
  it = vec.try_emplace(vec.begin(), 0, 'z');
  EXPECT_FALSE(it.has_value());
  EXPECT_EQ(vec.size(), 3u);
}

TEST_F(DynamicVectorTest, InsertLValue) {
  pw::DynamicVector<Counter> vec(allocator_);
  vec.assign({1, 3});
  const Counter two(2);

  auto it = vec.insert(vec.begin() + 1, two);
  EXPECT_EQ(it, vec.begin() + 1);
  EXPECT_EQ(vec.size(), 3u);
  EXPECT_EQ(vec[0], 1);
  EXPECT_EQ(vec[1], 2);
  EXPECT_EQ(vec[2], 3);
}

TEST_F(DynamicVectorTest, InsertRValue) {
  pw::DynamicVector<Counter> vec(allocator_);
  vec.assign({1, 3});

  auto it = vec.insert(vec.begin() + 1, Counter(2));
  EXPECT_EQ(it, vec.begin() + 1);
  EXPECT_EQ(vec.size(), 3u);
  EXPECT_EQ(vec[0], 1);
  EXPECT_EQ(vec[1], 2);
  EXPECT_EQ(vec[2], 3);
}

TEST_F(DynamicVectorTest, TryInsert) {
  pw::DynamicVector<Counter> vec(allocator_);
  vec.assign({1, 3});

  auto it = vec.try_insert(vec.begin() + 1, 2);
  ASSERT_TRUE(it.has_value());
  EXPECT_EQ(*it, vec.begin() + 1);
  EXPECT_EQ(vec.size(), 3u);
  EXPECT_EQ(vec[1], 2);

  vec.shrink_to_fit();
  ASSERT_EQ(vec.size(), vec.capacity());

  allocator_.DisableAll();
  it = vec.try_insert(vec.begin(), 0);
  EXPECT_FALSE(it.has_value());
  EXPECT_EQ(vec.size(), 3u);
}

TEST_F(DynamicVectorTest, InsertMultiple) {
  pw::DynamicVector<Counter> vec(allocator_);
  vec.assign({1, 5});

  auto it = vec.insert(vec.begin() + 1, 3u, 2);
  EXPECT_EQ(it, vec.begin() + 1);
  EXPECT_EQ(vec.size(), 5u);
  EXPECT_EQ(vec[0], 1);
  EXPECT_EQ(vec[1], 2);
  EXPECT_EQ(vec[2], 2);
  EXPECT_EQ(vec[3], 2);
  EXPECT_EQ(vec[4], 5);
}

TEST_F(DynamicVectorTest, TryInsertMultiple) {
  pw::DynamicVector<Counter> vec(allocator_);
  vec.assign({1, 5});

  auto it = vec.try_insert(vec.begin() + 1, 3u, 2);
  ASSERT_TRUE(it.has_value());
  EXPECT_EQ(*it, vec.begin() + 1);
  EXPECT_EQ(vec.size(), 5u);
  EXPECT_EQ(vec[2], 2);

  vec.shrink_to_fit();
  ASSERT_EQ(vec.size(), vec.capacity());

  allocator_.DisableAll();
  it = vec.try_insert(vec.begin(), 2u, 0);
  EXPECT_FALSE(it.has_value());
  EXPECT_EQ(vec.size(), 5u);
}

TEST_F(DynamicVectorTest, InsertMultipleZero) {
  pw::DynamicVector<Counter> vec(allocator_);
  vec.assign({1, 2});

  auto it = vec.insert(vec.begin() + 1, 0u, 99);
  EXPECT_EQ(it, vec.begin() + 1);
  EXPECT_EQ(vec.size(), 2u);
}

TEST_F(DynamicVectorTest, InsertIteratorRange) {
  pw::DynamicVector<Counter> vec(allocator_);
  vec.assign({1, 5});
  const std::array<Counter, 3> to_insert = {2, 3, 4};

  auto it = vec.insert(vec.begin() + 1, to_insert.begin(), to_insert.end());
  EXPECT_EQ(it, vec.begin() + 1);
  EXPECT_EQ(vec.size(), 5u);
  EXPECT_EQ(vec[0], 1);
  EXPECT_EQ(vec[1], 2);
  EXPECT_EQ(vec[2], 3);
  EXPECT_EQ(vec[3], 4);
  EXPECT_EQ(vec[4], 5);
}

TEST_F(DynamicVectorTest, TryInsertIteratorRange) {
  pw::DynamicVector<Counter> vec(allocator_);
  vec.assign({1, 5});
  const std::array<Counter, 3> to_insert = {2, 3, 4};

  auto it = vec.try_insert(vec.begin() + 1, to_insert.begin(), to_insert.end());
  ASSERT_TRUE(it.has_value());
  EXPECT_EQ(*it, vec.begin() + 1);
  EXPECT_EQ(vec.size(), 5u);
  EXPECT_EQ(vec[2], 3);

  vec.shrink_to_fit();
  ASSERT_EQ(vec.size(), vec.capacity());

  allocator_.DisableAll();
  it = vec.try_insert(vec.begin(), to_insert.begin(), to_insert.end());
  EXPECT_FALSE(it.has_value());
  EXPECT_EQ(vec.size(), 5u);
}

TEST_F(DynamicVectorTest, InsertInitializerList) {
  pw::DynamicVector<Counter> vec(allocator_);
  vec.assign({1, 5});

  auto it = vec.insert(vec.begin() + 1, {2, 3, 4});
  EXPECT_EQ(it, vec.begin() + 1);
  EXPECT_EQ(vec.size(), 5u);
  EXPECT_EQ(vec[0], 1);
  EXPECT_EQ(vec[1], 2);
  EXPECT_EQ(vec[2], 3);
  EXPECT_EQ(vec[3], 4);
  EXPECT_EQ(vec[4], 5);
}

TEST_F(DynamicVectorTest, TryInsertInitializerList) {
  pw::DynamicVector<Counter> vec(allocator_);
  vec.assign({1, 5});

  auto it = vec.try_insert(vec.begin() + 1, {2, 3, 4});
  ASSERT_TRUE(it.has_value());
  EXPECT_EQ(*it, vec.begin() + 1);
  EXPECT_EQ(vec.size(), 5u);
  EXPECT_EQ(vec[2], 3);

  vec.shrink_to_fit();
  ASSERT_EQ(vec.size(), vec.capacity());

  allocator_.DisableAll();
  it = vec.try_insert(vec.begin(), {9, 9, 9});
  EXPECT_FALSE(it.has_value());
  EXPECT_EQ(vec.size(), 5u);
}

TEST_F(DynamicVectorTest, Erase_First) {
  pw::DynamicVector<Counter> vec(allocator_);
  vec.assign({1, 2, 3, 4, 5});

  vec.erase(vec.begin());

  EXPECT_EQ(vec.size(), 4u);
  EXPECT_EQ(vec[0], 2);
  EXPECT_EQ(vec[1], 3);
  EXPECT_EQ(vec[2], 4);
  EXPECT_EQ(vec[3], 5);
}

TEST_F(DynamicVectorTest, Erase_ItemNearBegin) {
  pw::DynamicVector<Counter> vec(allocator_);
  vec.assign({1, 2, 3, 4, 5});

  vec.erase(vec.begin() + 1);

  EXPECT_EQ(vec.size(), 4u);
  EXPECT_EQ(vec[0], 1);
  EXPECT_EQ(vec[1], 3);
  EXPECT_EQ(vec[2], 4);
  EXPECT_EQ(vec[3], 5);
}

TEST_F(DynamicVectorTest, Erase_ItemNearEnd) {
  pw::DynamicVector<Counter> vec(allocator_);
  vec.assign({1, 2, 3, 4, 5});

  vec.erase(vec.end() - 2);

  EXPECT_EQ(vec.size(), 4u);
  EXPECT_EQ(vec[0], 1);
  EXPECT_EQ(vec[1], 2);
  EXPECT_EQ(vec[2], 3);
  EXPECT_EQ(vec[3], 5);
}

TEST_F(DynamicVectorTest, Erase_Last) {
  pw::DynamicVector<Counter> vec(allocator_);
  vec.assign({1, 2, 3, 4, 5});

  vec.erase(vec.end() - 1);

  EXPECT_EQ(vec.size(), 4u);
  EXPECT_EQ(vec[0], 1);
  EXPECT_EQ(vec[1], 2);
  EXPECT_EQ(vec[2], 3);
  EXPECT_EQ(vec[3], 4);
}

TEST_F(DynamicVectorTest, Erase_Middle) {
  pw::DynamicVector<Counter> vec(allocator_);
  vec.assign({1, 2, 3, 4, 5});

  auto it = vec.erase(vec.begin() + 2);
  EXPECT_EQ(it, vec.begin() + 2);
  EXPECT_EQ(*it, 4);
  EXPECT_EQ(vec.size(), 4u);
  EXPECT_EQ(vec[0], 1);
  EXPECT_EQ(vec[1], 2);
  EXPECT_EQ(vec[2], 4);
  EXPECT_EQ(vec[3], 5);
}

TEST_F(DynamicVectorTest, EraseRange_EmptyRange) {
  pw::DynamicVector<Counter> vec(allocator_);
  vec.assign({1, 2, 3, 4, 5});

  EXPECT_EQ(vec.erase(vec.begin(), vec.begin()), vec.begin());
  EXPECT_EQ(vec.erase(vec.begin() + 1, vec.begin() + 1), vec.begin() + 1);
  EXPECT_EQ(vec.erase(vec.end() - 2, vec.end() - 2), vec.end() - 2);
  EXPECT_EQ(vec.erase(vec.end() - 1, vec.end() - 1), vec.end() - 1);

  EXPECT_EQ(vec.size(), 5u);
  EXPECT_EQ(vec[0], 1);
  EXPECT_EQ(vec[1], 2);
  EXPECT_EQ(vec[2], 3);
  EXPECT_EQ(vec[3], 4);
  EXPECT_EQ(vec[4], 5);
}

TEST_F(DynamicVectorTest, EraseRange_Begin) {
  pw::DynamicVector<Counter> vec(allocator_);
  vec.assign({1, 2, 3, 4, 5});

  auto it = vec.erase(vec.begin(), vec.begin() + 2);
  EXPECT_EQ(it, vec.begin());
  EXPECT_EQ(*it, 3);
  EXPECT_EQ(vec.size(), 3u);
  EXPECT_EQ(vec[0], 3);
  EXPECT_EQ(vec[1], 4);
  EXPECT_EQ(vec[2], 5);
}

TEST_F(DynamicVectorTest, EraseRange_Middle) {
  pw::DynamicVector<Counter> vec(allocator_);
  vec.assign({1, 2, 3, 4, 5});

  auto it = vec.erase(vec.begin() + 1, vec.begin() + 4);
  EXPECT_EQ(it, vec.begin() + 1);
  EXPECT_EQ(*it, 5);
  EXPECT_EQ(vec.size(), 2u);
  EXPECT_EQ(vec[0], 1);
  EXPECT_EQ(vec[1], 5);
}

TEST_F(DynamicVectorTest, EraseRange_End) {
  pw::DynamicVector<Counter> vec(allocator_);
  vec.assign({1, 2, 3, 4, 5});

  auto it = vec.erase(vec.end() - 2, vec.end());
  EXPECT_EQ(it, vec.end());
  EXPECT_EQ(vec.size(), 3u);
  EXPECT_EQ(vec[0], 1);
  EXPECT_EQ(vec[1], 2);
  EXPECT_EQ(vec[2], 3);
}

TEST_F(DynamicVectorTest, ResizeMethods) {
  pw::DynamicVector<Counter> vec(allocator_);

  vec.resize(5);
  EXPECT_EQ(vec.size(), 5u);
  EXPECT_EQ(vec[0], 0);

  vec.resize(2);
  EXPECT_EQ(vec.size(), 2u);

  vec.resize(4, 99);
  EXPECT_EQ(vec.size(), 4u);
  EXPECT_EQ(vec[0], 0);
  EXPECT_EQ(vec[1], 0);
  EXPECT_EQ(vec[2], 99);
  EXPECT_EQ(vec[3], 99);

  EXPECT_TRUE(vec.try_resize(6));
  EXPECT_EQ(vec.size(), 6u);
  EXPECT_EQ(vec[4], 0);

  vec.shrink_to_fit();
  ASSERT_EQ(vec.size(), vec.capacity());

  allocator_.DisableAll();
  EXPECT_FALSE(vec.try_resize(10));
  EXPECT_EQ(vec.size(), 6u);

  allocator_.EnableAll();
  EXPECT_TRUE(vec.try_resize(8, 100));
  EXPECT_EQ(vec.size(), 8u);
  EXPECT_EQ(vec[6], 100);
  EXPECT_EQ(vec[7], 100);
}

TEST_F(DynamicVectorTest, Clear) {
  pw::DynamicVector<Counter> vec(allocator_);
  vec.assign({1, 2, 3});
  EXPECT_FALSE(vec.empty());

  vec.clear();
  EXPECT_TRUE(vec.empty());
  EXPECT_EQ(vec.size(), 0u);
}

TEST_F(DynamicVectorTest, Reset) {
  pw::DynamicVector<Counter> vec(allocator_);
  vec.assign({1, 2, 3});
  EXPECT_FALSE(vec.empty());
  Counter::Reset();

  vec.reset();

  EXPECT_TRUE(vec.empty());
  EXPECT_EQ(vec.size(), 0u);
  EXPECT_EQ(vec.capacity(), 0u);
  EXPECT_EQ(vec.data(), nullptr);
  EXPECT_EQ(Counter::destroyed, 3);
}

TEST_F(DynamicVectorTest, MoveAssign_DestroysOldElementsAndFreesBuffer) {
  pw::DynamicVector<Counter> vec1(allocator_);
  pw::DynamicVector<Counter> vec2(allocator_);

  vec1.assign({1, 2});
  vec2.assign({3, 4, 5});

  ASSERT_EQ(vec1.size(), 2u);
  ASSERT_EQ(vec2.size(), 3u);

  Counter::Reset();
  const size_t initial_bytes = allocator_for_test_.GetAllocated();

  vec1 = std::move(vec2);

  EXPECT_EQ(vec1.size(), 3u);
  EXPECT_EQ(Counter::destroyed, 2);
  EXPECT_LT(allocator_for_test_.GetAllocated(), initial_bytes);
}

TEST_F(DynamicVectorTest, Swap) {
  pw::DynamicVector<Counter> vec1(allocator_);
  vec1.assign({1, 2});

  pw::DynamicVector<Counter> vec2(allocator_);
  vec2.assign({10, 20, 30});

  EXPECT_EQ(vec1.size(), 2u);
  EXPECT_EQ(vec1[0], 1);
  EXPECT_EQ(vec2.size(), 3u);
  EXPECT_EQ(vec2[0], 10);

  vec1.swap(vec2);

  EXPECT_EQ(vec1.size(), 3u);
  EXPECT_EQ(vec1[0], 10);
  EXPECT_EQ(vec2.size(), 2u);
  EXPECT_EQ(vec2[0], 1);
}

}  // namespace
