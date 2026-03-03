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

#include "pw_containers/dynamic_map.h"

#include <string>
#include <utility>
#include <vector>

#include "pw_allocator/fault_injecting_allocator.h"
#include "pw_allocator/testing.h"
#include "pw_containers/internal/test_helpers.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::allocator::test::AllocatorForTest;
using pw::allocator::test::FaultInjectingAllocator;
using pw::containers::test::CopyOnly;
using pw::containers::test::Counter;
using pw::containers::test::MoveOnly;

class DynamicMapTest : public ::testing::Test {
 protected:
  DynamicMapTest() : allocator_(allocator_for_test_) {}

  AllocatorForTest<1024> allocator_for_test_;
  FaultInjectingAllocator allocator_;
};

TEST_F(DynamicMapTest, ConstructDestruct) {
  pw::DynamicMap<int, int> map(allocator_);
  EXPECT_TRUE(map.empty());
  EXPECT_EQ(map.begin(), map.end());
  EXPECT_EQ(map.rbegin(), map.rend());
  EXPECT_EQ(map.size(), 0u);
  EXPECT_EQ(map.lower_bound(0), map.end());
  EXPECT_EQ(map.upper_bound(0), map.end());
}

TEST_F(DynamicMapTest, SelfMoveAssign) {
  pw::DynamicMap<int, int> map(allocator_);
  map.emplace(1, 10);
  map.emplace(2, 20);

  auto& map_ref = map;
  map = std::move(map_ref);

  EXPECT_EQ(map.size(), 2u);
  EXPECT_EQ(map.at(1), 10);
  EXPECT_EQ(map.at(2), 20);
}

TEST_F(DynamicMapTest, VerifyDestruction) {
  Counter::Reset();
  {
    pw::DynamicMap<int, Counter> map(allocator_);
    map.emplace(1);
    map.emplace(2);
    EXPECT_EQ(Counter::created, 2);
  }
  EXPECT_EQ(Counter::destroyed, 2);
}

TEST_F(DynamicMapTest, InsertAndFind) {
  pw::DynamicMap<int, std::string> map(allocator_);

  auto result = map.insert({1, "one"});
  EXPECT_TRUE(result.second);
  EXPECT_EQ(result.first->first, 1);
  EXPECT_EQ(result.first->second, "one");
  EXPECT_EQ(map.size(), 1u);

  EXPECT_TRUE(map.contains(1));
  EXPECT_FALSE(map.contains(2));

  auto it = map.find(1);
  ASSERT_NE(it, map.end());
  EXPECT_EQ(it->first, 1);
  EXPECT_EQ(it->second, "one");

  EXPECT_EQ(map.find(2), map.end());

  auto result2 = map.insert({1, "another one"});
  EXPECT_FALSE(result2.second);
  EXPECT_EQ(result2.first->first, 1);
  EXPECT_EQ(result2.first->second, "one");
  EXPECT_EQ(map.size(), 1u);
}

TEST_F(DynamicMapTest, TryInsertAllocationFailure) {
  pw::DynamicMap<int, int> map(allocator_);

  pw::DynamicMap<int, int>::value_type item1{1, 10};
  auto result = map.try_insert(item1);
  EXPECT_TRUE(result.has_value());
  EXPECT_TRUE(result->second);
  EXPECT_EQ(result->first->first, 1);
  EXPECT_EQ(result->first->second, 10);

  allocator_.DisableAll();
  pw::DynamicMap<int, int>::value_type item2{2, 20};
  auto result2 = map.try_insert(item2);
  EXPECT_FALSE(result2.has_value());
  allocator_.EnableAll();

  EXPECT_EQ(map.size(), 1u);
  EXPECT_TRUE(map.contains(1));
  EXPECT_FALSE(map.contains(2));
}

TEST_F(DynamicMapTest, InsertMove) {
  pw::DynamicMap<int, MoveOnly> map(allocator_);

  auto result = map.insert({1, MoveOnly(10)});
  EXPECT_TRUE(result.second);
  EXPECT_EQ(result.first->first, 1);
  EXPECT_EQ(result.first->second.value, 10);
  EXPECT_EQ(map.size(), 1u);
}

TEST_F(DynamicMapTest, InsertRange) {
  pw::DynamicMap<int, int> map(allocator_);
  std::vector<std::pair<int, int>> values = {{1, 10}, {2, 20}, {3, 30}};

  map.insert(values.begin(), values.end());
  EXPECT_EQ(map.size(), 3u);
  EXPECT_EQ(map.at(1), 10);
  EXPECT_EQ(map.at(2), 20);
  EXPECT_EQ(map.at(3), 30);
}

TEST_F(DynamicMapTest, Emplace) {
  pw::DynamicMap<int, std::string> map(allocator_);

  auto result = map.emplace(1, "one");
  EXPECT_TRUE(result.second);
  EXPECT_EQ(result.first->first, 1);
  EXPECT_EQ(result.first->second, "one");

  auto result2 = map.emplace(1, "another one");
  EXPECT_FALSE(result2.second);
  EXPECT_EQ(result2.first->second, "one");
}

TEST_F(DynamicMapTest, EmplacePiecewise) {
  struct CustomType {
    CustomType(int a, int b) : sum(a + b) {}
    CustomType(const CustomType&) = delete;
    int sum;
  };
  pw::DynamicMap<int, CustomType> map(allocator_);

  EXPECT_TRUE(map.try_emplace(1, 10, 20).has_value());
  EXPECT_EQ(map.at(1).sum, 30);
}

TEST_F(DynamicMapTest, TryEmplaceAllocationFailure) {
  pw::DynamicMap<int, int> map(allocator_);

  auto result = map.try_emplace(1, 10);
  EXPECT_TRUE(result.has_value());
  EXPECT_TRUE(result->second);
  EXPECT_EQ(result->first->first, 1);
  EXPECT_EQ(result->first->second, 10);

  allocator_.DisableAll();
  auto result2 = map.try_emplace(2, 20);
  EXPECT_FALSE(result2.has_value());
  allocator_.EnableAll();

  EXPECT_EQ(map.size(), 1u);
}

TEST_F(DynamicMapTest, At) {
  pw::DynamicMap<int, std::string> map(allocator_);
  map.insert({1, "one"});

  EXPECT_EQ(map.at(1), "one");
  map.at(1) = "new one";
  EXPECT_EQ(map.at(1), "new one");

  const auto& const_map = map;
  EXPECT_EQ(const_map.at(1), "new one");

  EXPECT_DEATH_IF_SUPPORTED(map.at(2), "");
}

TEST_F(DynamicMapTest, OperatorBrackets) {
  pw::DynamicMap<int, std::string> map(allocator_);

  map[1] = "one";
  EXPECT_EQ(map.size(), 1u);
  EXPECT_EQ(map[1], "one");

  map[1] = "new one";
  EXPECT_EQ(map.size(), 1u);
  EXPECT_EQ(map[1], "new one");

  EXPECT_EQ(map[2], "");
  EXPECT_EQ(map.size(), 2u);
  EXPECT_TRUE(map.contains(2));
}

TEST_F(DynamicMapTest, OperatorBracketsAllocationFailure) {
  pw::DynamicMap<int, int> map(allocator_);

  map[1] = 10;
  EXPECT_EQ(map.size(), 1u);
  EXPECT_EQ(map[1], 10);

  allocator_.DisableAll();
  EXPECT_DEATH_IF_SUPPORTED(map[2] = 20, "");
  allocator_.EnableAll();
  EXPECT_EQ(map.size(), 1u);
}

TEST_F(DynamicMapTest, Erase) {
  pw::DynamicMap<int, int> map(allocator_);
  map.insert({1, 10});
  map.insert({2, 20});
  map.insert({3, 30});

  EXPECT_EQ(map.size(), 3u);
  EXPECT_EQ(map.erase(2), 1u);
  EXPECT_EQ(map.size(), 2u);
  EXPECT_FALSE(map.contains(2));
  EXPECT_EQ(map.erase(2), 0u);

  auto it = map.find(1);
  ASSERT_NE(it, map.end());
  auto next_it = map.erase(it);
  EXPECT_EQ(map.size(), 1u);
  EXPECT_FALSE(map.contains(1));
  ASSERT_NE(next_it, map.end());
  EXPECT_EQ(next_it->first, 3);

  map.erase(map.begin(), map.end());
  EXPECT_TRUE(map.empty());
}

TEST_F(DynamicMapTest, EraseConstIterator) {
  pw::DynamicMap<int, int> map(allocator_);
  map.insert({1, 10});
  map.insert({2, 20});
  map.insert({3, 30});

  const auto& const_map = map;
  pw::DynamicMap<int, int>::const_iterator cit = const_map.find(2);
  ASSERT_NE(cit, const_map.end());

  auto next_it = map.erase(cit);

  EXPECT_EQ(map.size(), 2u);
  EXPECT_FALSE(map.contains(2));

  ASSERT_NE(next_it, map.end());
  EXPECT_EQ(next_it->first, 3);
}

TEST_F(DynamicMapTest, Clear) {
  pw::DynamicMap<int, int> map(allocator_);
  map.insert({1, 10});
  map.insert({2, 20});

  EXPECT_FALSE(map.empty());
  map.clear();
  EXPECT_TRUE(map.empty());
  EXPECT_EQ(map.size(), 0u);
}

TEST_F(DynamicMapTest, Iterators) {
  pw::DynamicMap<int, int> map(allocator_);
  map.insert({3, 30});
  map.insert({1, 10});
  map.insert({2, 20});

  std::vector<std::pair<int, int>> expected = {{1, 10}, {2, 20}, {3, 30}};
  std::vector<std::pair<int, int>> actual;
  for (const auto& pair : map) {
    actual.emplace_back(pair);
  }
  EXPECT_EQ(actual, expected);

  actual.clear();
  for (auto it = map.cbegin(); it != map.cend(); ++it) {
    actual.emplace_back(*it);
  }
  EXPECT_EQ(actual, expected);
}

TEST_F(DynamicMapTest, Bounds) {
  pw::DynamicMap<int, int> map(allocator_);
  map.insert({1, 10});
  map.insert({3, 30});
  map.insert({5, 50});

  EXPECT_EQ(map.lower_bound(0)->first, 1);
  EXPECT_EQ(map.lower_bound(1)->first, 1);
  EXPECT_EQ(map.lower_bound(2)->first, 3);
  EXPECT_EQ(map.lower_bound(3)->first, 3);
  EXPECT_EQ(map.lower_bound(6), map.end());

  EXPECT_EQ(map.upper_bound(0)->first, 1);
  EXPECT_EQ(map.upper_bound(1)->first, 3);
  EXPECT_EQ(map.upper_bound(2)->first, 3);
  EXPECT_EQ(map.upper_bound(3)->first, 5);
  EXPECT_EQ(map.upper_bound(5), map.end());
}

TEST_F(DynamicMapTest, Swap) {
  AllocatorForTest<1024> allocator2_for_test;
  FaultInjectingAllocator allocator2(allocator2_for_test);

  pw::DynamicMap<int, int> map1(allocator_);
  map1.insert({1, 10});

  pw::DynamicMap<int, int> map2(allocator2);
  map2.insert({2, 20});
  map2.insert({3, 30});

  map1.swap(map2);

  EXPECT_EQ(map1.size(), 2u);
  EXPECT_TRUE(map1.contains(2));
  EXPECT_TRUE(map1.contains(3));
  EXPECT_FALSE(map1.contains(1));

  EXPECT_EQ(map2.size(), 1u);     // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(map2.contains(1));  // NOLINT(bugprone-use-after-move)
}

TEST_F(DynamicMapTest, Merge) {
  pw::DynamicMap<int, int> map1(allocator_);
  map1.insert({1, 10});
  map1.insert({2, 20});

  AllocatorForTest<1024> allocator2_for_test;
  FaultInjectingAllocator allocator2(allocator2_for_test);
  pw::DynamicMap<int, int> map2(allocator2);
  map2.insert({2, 200});
  map2.insert({3, 30});

  map1.merge(std::move(map2));

  EXPECT_EQ(map1.size(), 3u);
  EXPECT_EQ(map1.at(1), 10);
  EXPECT_EQ(map1.at(2), 20);
  EXPECT_EQ(map1.at(3), 30);

  EXPECT_EQ(map2.size(), 1u);     // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(map2.contains(2));  // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(map2.at(2), 200);     // NOLINT(bugprone-use-after-move)
}

TEST_F(DynamicMapTest, MoveConstruct) {
  pw::DynamicMap<int, MoveOnly> map(allocator_);
  map.emplace(1, MoveOnly(1));
  map.emplace(2, MoveOnly(2));

  pw::DynamicMap<int, MoveOnly> moved_into(std::move(map));

  EXPECT_EQ(map.size(), 0u);  // NOLINT(bugprone-use-after-move)
  ASSERT_EQ(moved_into.size(), 2u);
  EXPECT_EQ(moved_into.at(1).value, 1);
  EXPECT_EQ(moved_into.at(2).value, 2);
}

TEST_F(DynamicMapTest, MoveAssign) {
  pw::DynamicMap<int, MoveOnly> map(allocator_);
  map.emplace(1, MoveOnly(1));
  map.emplace(2, MoveOnly(2));

  pw::DynamicMap<int, MoveOnly> moved_into(allocator_);
  moved_into.emplace(3, MoveOnly(3));

  moved_into = std::move(map);

  EXPECT_EQ(map.size(), 0u);  // NOLINT(bugprone-use-after-move)
  ASSERT_EQ(moved_into.size(), 2u);
  EXPECT_EQ(moved_into.at(1).value, 1);
  EXPECT_EQ(moved_into.at(2).value, 2);
  EXPECT_FALSE(moved_into.contains(3));
}

// Test that DynamicMap<K, T> is NOT copy constructible
static_assert(!std::is_copy_constructible_v<pw::DynamicMap<int, int>>);

// Test that DynamicMap<K, T> is move constructible
static_assert(std::is_move_constructible_v<pw::DynamicMap<int, MoveOnly>>);

// Test that DynamicMap<K, T> is NOT copy assignable
static_assert(!std::is_copy_assignable_v<pw::DynamicMap<int, CopyOnly>>);

// Test that DynamicMap<K, T> is move assignable
static_assert(std::is_move_assignable_v<pw::DynamicMap<int, MoveOnly>>);

}  // namespace
