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

#include "pw_containers/dynamic_hash_map.h"

#include <algorithm>
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

class DynamicHashMapTest : public ::testing::Test {
 protected:
  DynamicHashMapTest() : allocator_(allocator_for_test_) {}

  AllocatorForTest<2048> allocator_for_test_;
  FaultInjectingAllocator allocator_;
};

TEST_F(DynamicHashMapTest, ConstructDestruct) {
  pw::DynamicHashMap<int, int> map(allocator_);
  EXPECT_TRUE(map.empty());
  EXPECT_EQ(map.begin(), map.end());
  EXPECT_EQ(map.size(), 0u);
}

TEST_F(DynamicHashMapTest, SelfMoveAssign) {
  pw::DynamicHashMap<int, int> map(allocator_);
  map.emplace(1, 10);
  map.emplace(2, 20);

  auto& map_ref = map;
  map = std::move(map_ref);

  EXPECT_EQ(map.size(), 2u);
  EXPECT_EQ(map.at(1), 10);
  EXPECT_EQ(map.at(2), 20);
}

TEST_F(DynamicHashMapTest, VerifyDestruction) {
  Counter::Reset();
  {
    pw::DynamicHashMap<int, Counter> map(allocator_);
    map.emplace(1);
    map.emplace(2);
    EXPECT_EQ(Counter::created, 2);
  }
  EXPECT_EQ(Counter::destroyed, 2);
}

TEST_F(DynamicHashMapTest, InsertAndFind) {
  pw::DynamicHashMap<int, std::string> map(allocator_);

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

TEST_F(DynamicHashMapTest, TryInsertAllocationFailure) {
  pw::DynamicHashMap<int, int> map(allocator_);

  const std::pair<const int, int> item1{1, 10};
  auto result = map.try_insert(item1);
  EXPECT_TRUE(result.has_value());
  EXPECT_TRUE(result->second);
  EXPECT_EQ(result->first->first, 1);
  EXPECT_EQ(result->first->second, 10);

  allocator_.DisableAll();
  const std::pair<const int, int> item2{2, 20};
  auto result2 = map.try_insert(item2);
  EXPECT_FALSE(result2.has_value());
  allocator_.EnableAll();

  EXPECT_EQ(map.size(), 1u);
  EXPECT_TRUE(map.contains(1));
  EXPECT_FALSE(map.contains(2));
}

TEST_F(DynamicHashMapTest, InsertInitializerList) {
  pw::DynamicHashMap<int, int> map(allocator_);

  map.insert({{1, 10}, {2, 20}});
  EXPECT_EQ(map.size(), 2u);
  EXPECT_EQ(map.at(1), 10);
  EXPECT_EQ(map.at(2), 20);
}

TEST_F(DynamicHashMapTest, InsertMove) {
  pw::DynamicHashMap<int, MoveOnly> map(allocator_);

  auto result = map.insert({1, MoveOnly(10)});
  EXPECT_TRUE(result.second);
  EXPECT_EQ(result.first->first, 1);
  EXPECT_EQ(result.first->second.value, 10);
  EXPECT_EQ(map.size(), 1u);
}

TEST_F(DynamicHashMapTest, InsertRange) {
  pw::DynamicHashMap<int, int> map(allocator_);
  std::vector<std::pair<int, int>> values = {{1, 10}, {2, 20}, {3, 30}};

  map.insert(values.begin(), values.end());
  EXPECT_EQ(map.size(), 3u);
  EXPECT_EQ(map.at(1), 10);
  EXPECT_EQ(map.at(2), 20);
  EXPECT_EQ(map.at(3), 30);
}

TEST_F(DynamicHashMapTest, InsertWithAutoRehash) {
  pw::DynamicHashMap<int, int> map(allocator_);
  map.max_load_factor_percent(50u);

  for (int i = 0; i < 20; ++i) {
    map.insert({i, i});
  }
  EXPECT_EQ(map.size(), 20u);

  for (int i = 0; i < 20; ++i) {
    EXPECT_TRUE(map.contains(i));
    EXPECT_EQ(map.at(i), i);
  }
}

TEST_F(DynamicHashMapTest, Emplace) {
  pw::DynamicHashMap<int, std::string> map(allocator_);

  auto result = map.emplace(1, "one");
  EXPECT_TRUE(result.second);
  EXPECT_EQ(result.first->first, 1);
  EXPECT_EQ(result.first->second, "one");

  auto result2 = map.emplace(1, "another one");
  EXPECT_FALSE(result2.second);
  EXPECT_EQ(result2.first->second, "one");
}

TEST_F(DynamicHashMapTest, EmplacePiecewise) {
  struct CustomType {
    CustomType(int a, int b) : sum(a + b) {}
    CustomType(const CustomType&) = delete;
    int sum;
  };
  pw::DynamicHashMap<int, CustomType> map(allocator_);

  EXPECT_TRUE(map.try_emplace(1, 10, 20).has_value());
  EXPECT_EQ(map.at(1).sum, 30);
}

TEST_F(DynamicHashMapTest, TryEmplaceAllocationFailure) {
  pw::DynamicHashMap<int, int> map(allocator_);

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

TEST_F(DynamicHashMapTest, At) {
  pw::DynamicHashMap<int, std::string> map(allocator_);
  map.insert({1, "one"});

  EXPECT_EQ(map.at(1), "one");
  map.at(1) = "new one";
  EXPECT_EQ(map.at(1), "new one");

  const auto& const_map = map;
  EXPECT_EQ(const_map.at(1), "new one");

  EXPECT_DEATH_IF_SUPPORTED(map.at(2), "");
}

TEST_F(DynamicHashMapTest, OperatorBrackets) {
  pw::DynamicHashMap<int, std::string> map(allocator_);

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

TEST_F(DynamicHashMapTest, OperatorBracketsAllocationFailure) {
  pw::DynamicHashMap<int, int> map(allocator_);

  map[1] = 10;
  EXPECT_EQ(map.size(), 1u);
  EXPECT_EQ(map[1], 10);

  allocator_.DisableAll();
  EXPECT_DEATH_IF_SUPPORTED(map[2] = 20, "");
  allocator_.EnableAll();
  EXPECT_EQ(map.size(), 1u);
}

TEST_F(DynamicHashMapTest, Erase) {
  pw::DynamicHashMap<int, int> map(allocator_);
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
  map.erase(it);
  EXPECT_EQ(map.size(), 1u);
  EXPECT_FALSE(map.contains(1));

  map.erase(map.begin(), map.end());
  EXPECT_TRUE(map.empty());
}

TEST_F(DynamicHashMapTest, Clear) {
  pw::DynamicHashMap<int, int> map(allocator_);
  map.insert({1, 10});
  map.insert({2, 20});

  EXPECT_FALSE(map.empty());
  map.clear();
  EXPECT_TRUE(map.empty());
  EXPECT_EQ(map.size(), 0u);
}

TEST_F(DynamicHashMapTest, Iterators) {
  pw::DynamicHashMap<int, int> map(allocator_);
  map.insert({3, 30});
  map.insert({1, 10});
  map.insert({2, 20});

  std::vector<std::pair<int, int>> expected = {{1, 10}, {2, 20}, {3, 30}};

  // Helper to collect and sort pairs because HashMap iteration order is
  // undefined
  auto collect_sorted = [](auto& map_obj) {
    std::vector<std::pair<int, int>> result;
    for (const auto& pair : map_obj) {
      result.emplace_back(pair);
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
      return a.first < b.first;
    });
    return result;
  };

  std::vector<std::pair<int, int>> actual = collect_sorted(map);
  EXPECT_EQ(actual, expected);

  // Test const iterators
  const auto& const_map = map;
  std::vector<std::pair<int, int>> const_actual = collect_sorted(const_map);
  EXPECT_EQ(const_actual, expected);
}

TEST_F(DynamicHashMapTest, EqualRange) {
  pw::DynamicHashMap<int, int> map(allocator_);
  map.insert({1, 10});
  map.insert({3, 30});
  map.insert({5, 50});

  // Key present
  auto range = map.equal_range(3);
  ASSERT_NE(range.first, map.end());
  EXPECT_EQ(range.first->first, 3);
  EXPECT_EQ(range.first->second, 30);
  EXPECT_EQ(range.second, ++(map.find(3)));

  auto range_absent = map.equal_range(4);
  EXPECT_EQ(range_absent.first, map.end());
  EXPECT_EQ(range_absent.second, map.end());
}

TEST_F(DynamicHashMapTest, Swap) {
  AllocatorForTest<1024> allocator2_for_test;
  FaultInjectingAllocator allocator2(allocator2_for_test);

  pw::DynamicHashMap<int, int> map1(allocator_);
  map1.insert({1, 10});

  pw::DynamicHashMap<int, int> map2(allocator2);
  map2.insert({2, 20});
  map2.insert({3, 30});

  map1.swap(map2);

  EXPECT_EQ(map1.size(), 2u);
  EXPECT_TRUE(map1.contains(2));
  EXPECT_TRUE(map1.contains(3));
  EXPECT_FALSE(map1.contains(1));

  EXPECT_EQ(map2.size(), 1u);
  EXPECT_TRUE(map2.contains(1));
}

TEST_F(DynamicHashMapTest, Merge) {
  pw::DynamicHashMap<int, int> map1(allocator_);
  map1.insert({1, 10});
  map1.insert({2, 20});

  AllocatorForTest<1024> allocator2_for_test;
  FaultInjectingAllocator allocator2(allocator2_for_test);
  pw::DynamicHashMap<int, int> map2(allocator2);
  map2.insert({2, 200});
  map2.insert({3, 30});

  map1.merge(map2);

  EXPECT_EQ(map1.size(), 3u);
  EXPECT_EQ(map1.at(1), 10);
  EXPECT_EQ(map1.at(2), 20);
  EXPECT_EQ(map1.at(3), 30);

  EXPECT_EQ(map2.size(), 1u);
  EXPECT_TRUE(map2.contains(2));
  EXPECT_EQ(map2.at(2), 200);
}

TEST_F(DynamicHashMapTest, Reserve) {
  pw::DynamicHashMap<int, int> map(allocator_);
  map.reserve(100);
  map.emplace(1, 1);
  EXPECT_LE(map.load_factor_percent(), 1u);
}

TEST_F(DynamicHashMapTest, MoveConstruct) {
  pw::DynamicHashMap<int, MoveOnly> map(allocator_);
  map.emplace(1, MoveOnly(1));
  map.emplace(2, MoveOnly(2));

  pw::DynamicHashMap<int, MoveOnly> moved_into(std::move(map));

  EXPECT_EQ(map.size(), 0u);  // NOLINT(bugprone-use-after-move)
  ASSERT_EQ(moved_into.size(), 2u);
  EXPECT_EQ(moved_into.at(1).value, 1);
  EXPECT_EQ(moved_into.at(2).value, 2);
}

TEST_F(DynamicHashMapTest, MoveAssign) {
  pw::DynamicHashMap<int, MoveOnly> map(allocator_);
  map.emplace(1, MoveOnly(1));
  map.emplace(2, MoveOnly(2));

  pw::DynamicHashMap<int, MoveOnly> moved_into(allocator_);
  moved_into.emplace(3, MoveOnly(3));

  moved_into = std::move(map);

  EXPECT_EQ(map.size(), 0u);  // NOLINT(bugprone-use-after-move)
  ASSERT_EQ(moved_into.size(), 2u);
  EXPECT_EQ(moved_into.at(1).value, 1);
  EXPECT_EQ(moved_into.at(2).value, 2);
  EXPECT_FALSE(moved_into.contains(3));
}

TEST_F(DynamicHashMapTest, HashCollisions) {
  struct BadHash {
    size_t operator()(int) const { return 0; }  // All keys hash to 0
  };

  pw::DynamicHashMap<int, int, BadHash> map(allocator_);

  map.insert({1, 10});
  map.insert({2, 20});
  map.insert({3, 30});

  EXPECT_EQ(map.size(), 3u);
  EXPECT_TRUE(map.contains(1));
  EXPECT_TRUE(map.contains(2));
  EXPECT_TRUE(map.contains(3));

  EXPECT_EQ(map.at(1), 10);
  EXPECT_EQ(map.at(2), 20);
  EXPECT_EQ(map.at(3), 30);

  map.erase(2);
  EXPECT_EQ(map.size(), 2u);
  EXPECT_FALSE(map.contains(2));
  EXPECT_EQ(map.at(1), 10);
  EXPECT_EQ(map.at(3), 30);
}

// Test that DynamicHashMap<K, T> is NOT copy constructible
static_assert(!std::is_copy_constructible_v<pw::DynamicHashMap<int, int>>);

// Test that DynamicHashMap<K, T> is move constructible
static_assert(std::is_move_constructible_v<pw::DynamicHashMap<int, MoveOnly>>);

// Test that DynamicHashMap<K, T> is NOT copy assignable
static_assert(!std::is_copy_assignable_v<pw::DynamicHashMap<int, CopyOnly>>);

// Test that DynamicHashMap<K, T> is move assignable
static_assert(std::is_move_assignable_v<pw::DynamicHashMap<int, MoveOnly>>);

}  // namespace
