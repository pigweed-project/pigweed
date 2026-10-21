// Copyright 2024 The Pigweed Authors
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

#include "pw_containers/intrusive_map.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <tuple>
#include <utility>

#include "pw_compilation_testing/negative_compilation.h"
#include "pw_containers/intrusive_multimap.h"
#include "pw_span/span.h"
#include "pw_unit_test/framework.h"

namespace {

// Base pair.
class BaseItem {
 public:
  explicit BaseItem(const char* name) : name_(name) {}

  constexpr const char* name() const { return name_; }
  void set_name(const char* name) { name_ = name; }

 private:
  const char* name_;
};

// A basic pair that can be used in a map.
class TestPair : public ::pw::IntrusiveMap<size_t, TestPair>::Pair,
                 public BaseItem {
 private:
  using Pair = ::pw::IntrusiveMap<size_t, TestPair>::Pair;

 public:
  TestPair(size_t key, const char* name) : Pair(key), BaseItem(name) {}
};

// Test fixture.
class IntrusiveMapTest : public ::testing::Test {
 protected:
  using IntrusiveMap = ::pw::IntrusiveMap<size_t, TestPair>;
  static constexpr size_t kNumPairs = 10;

  void SetUp() override { map_.insert(pairs_.begin(), pairs_.end()); }

  void TearDown() override { map_.clear(); }

  std::array<TestPair, kNumPairs> pairs_ = {{
      {30, "a"},
      {50, "b"},
      {20, "c"},
      {40, "d"},
      {10, "e"},
      {35, "A"},
      {55, "B"},
      {25, "C"},
      {45, "D"},
      {15, "E"},
  }};

  IntrusiveMap map_;
};

// Unit tests.

TEST_F(IntrusiveMapTest, Construct_Default) {
  IntrusiveMap map;
  EXPECT_TRUE(map.empty());
  EXPECT_EQ(map.begin(), map.end());
  EXPECT_EQ(map.rbegin(), map.rend());
  EXPECT_EQ(map.size(), 0U);
  EXPECT_EQ(map.lower_bound(0), map.end());
  EXPECT_EQ(map.upper_bound(0), map.end());
}

TEST_F(IntrusiveMapTest, Construct_ObjectIterators) {
  map_.clear();
  IntrusiveMap map(pairs_.begin(), pairs_.end());
  EXPECT_FALSE(map.empty());
  EXPECT_EQ(map.size(), pairs_.size());
  map.clear();
}

TEST_F(IntrusiveMapTest, Construct_ObjectIterators_Empty) {
  IntrusiveMap map(pairs_.end(), pairs_.end());
  EXPECT_TRUE(map.empty());
  EXPECT_EQ(map.size(), 0U);
}

TEST_F(IntrusiveMapTest, Construct_PointerIterators) {
  std::array<TestPair*, 3> ptrs = {&pairs_[0], &pairs_[1], &pairs_[2]};
  map_.clear();
  IntrusiveMap map(ptrs.begin(), ptrs.end());
  EXPECT_FALSE(map.empty());
  EXPECT_EQ(map.size(), 3U);
  map.clear();
}

TEST_F(IntrusiveMapTest, Construct_PointerIterators_Empty) {
  std::array<TestPair*, 0> ptrs;
  IntrusiveMap map(ptrs.begin(), ptrs.end());
  EXPECT_TRUE(map.empty());
  EXPECT_EQ(map.size(), 0U);
  map.clear();
}

TEST_F(IntrusiveMapTest, Construct_InitializerList) {
  map_.clear();
  IntrusiveMap map({&pairs_[0], &pairs_[2], &pairs_[4]});
  auto iter = map.begin();
  EXPECT_EQ((iter++)->key(), 10U);
  EXPECT_EQ((iter++)->key(), 20U);
  EXPECT_EQ((iter++)->key(), 30U);
  map.clear();
}

TEST_F(IntrusiveMapTest, Construct_InitializerList_Empty) {
  IntrusiveMap map({});
  EXPECT_TRUE(map.empty());
  EXPECT_EQ(map.size(), 0U);
}

TEST_F(IntrusiveMapTest, Construct_CustomCompare) {
  auto greater_than = [](const size_t& lhs, const size_t& rhs) {
    return lhs > rhs;
  };
  map_.clear();
  IntrusiveMap map({&pairs_[0], &pairs_[2], &pairs_[4]},
                   std::move(greater_than));
  auto iter = map.begin();
  EXPECT_EQ((iter++)->key(), 30U);
  EXPECT_EQ((iter++)->key(), 20U);
  EXPECT_EQ((iter++)->key(), 10U);
  map.clear();
}

// A map pair that includes a key accessor method.
struct HalvedKey : public ::pw::IntrusiveMap<size_t, HalvedKey>::Item,
                   public BaseItem {
 public:
  HalvedKey(size_t half_key, const char* name)
      : BaseItem(name), half_key_(half_key) {}
  size_t key() const { return half_key_ * 2; }

 private:
  const size_t half_key_;
};

TEST_F(IntrusiveMapTest, Construct_CustomItem) {
  std::array<HalvedKey, 3> items = {{
      {50, "B"},
      {40, "D"},
      {60, "F"},
  }};
  pw::IntrusiveMap<size_t, HalvedKey> map(items.begin(), items.end());

  auto iter = map.find(80);
  ASSERT_NE(iter, map.end());
  EXPECT_STREQ(iter->name(), "D");

  iter = map.find(100);
  ASSERT_NE(iter, map.end());
  EXPECT_STREQ(iter->name(), "B");

  iter = map.find(120);
  ASSERT_NE(iter, map.end());
  EXPECT_STREQ(iter->name(), "F");

  map.clear();
}

// A map item that has no explicit key.
struct NoKey : public ::pw::IntrusiveMap<size_t, NoKey>::Item, public BaseItem {
 public:
  explicit NoKey(const char* name) : BaseItem(name) {}
};

// A functor to get an implied key from a `NoKey` item.
struct GetImpliedKey {
  size_t operator()(const NoKey& item) const {
    return std::strlen(item.name());
  }
};

TEST_F(IntrusiveMapTest, Construct_CustomGetKey) {
  std::array<NoKey, 4> items = {
      NoKey("CC"),
      NoKey("AAA"),
      NoKey("B"),
      NoKey("DDDD"),
  };
  pw::IntrusiveMap<size_t, NoKey> map((std::less<>()), GetImpliedKey());
  map.insert(items.begin(), items.end());

  auto iter = map.begin();
  EXPECT_STREQ((iter++)->name(), "B");
  EXPECT_STREQ((iter++)->name(), "CC");
  EXPECT_STREQ((iter++)->name(), "AAA");
  EXPECT_STREQ((iter++)->name(), "DDDD");
  map.clear();
}

//  A struct that is not a map pair.
class NotAnItem : public BaseItem {
 public:
  NotAnItem(const char* name, size_t key) : BaseItem(name), key_(key) {}
  size_t key() const { return key_; }

 private:
  const size_t key_;
};

#if PW_NC_TEST(IncompatibleItem)
PW_NC_EXPECT(
    "IntrusiveMap items must be derived from IntrusiveMap<Key, T>::Item");

class BadItem : public ::pw::IntrusiveMap<size_t, NotAnItem>::Item {
 public:
  constexpr explicit BadItem(size_t key) : key_(key) {}
  constexpr size_t key() const { return key_; }
  constexpr bool operator<(const Pair& rhs) { return key_ < rhs.key(); }

 private:
  const size_t key_;
};

using Compare = Function<bool(Key, Key)>;
using GetKey = Function<Key(const V&)>;

[[maybe_unused]] ::pw::IntrusiveMap<size_t, BadItem> bad_map1;

#elif PW_NC_TEST(DoesNotInheritFromItem)
PW_NC_EXPECT(
    "IntrusiveMap items must be derived from IntrusiveMap<Key, T>::Item");

[[maybe_unused]] ::pw::IntrusiveMap<size_t, NotAnItem> bad_map2;

#endif  // PW_NC_TEST

// Element access

TEST_F(IntrusiveMapTest, At) {
  const IntrusiveMap& map = map_;
  for (const auto& pair : pairs_) {
    EXPECT_EQ(&(map.at(pair.key())), &pair);
  }
}

// Iterators

TEST_F(IntrusiveMapTest, Iterator) {
  const IntrusiveMap& map = map_;
  auto iter = map.begin();
  size_t key = 10;
  for (size_t i = 0; i < kNumPairs; ++i) {
    auto& pair = *iter++;
    EXPECT_EQ(pair.key(), key);
    key += 5;
  }
  EXPECT_EQ(key, 60U);
  EXPECT_EQ(iter, map.end());
  EXPECT_EQ(iter, map.cend());
  for (size_t i = 0; i < kNumPairs; ++i) {
    key -= 5;
    EXPECT_EQ((--iter)->key(), key);
  }
  EXPECT_EQ(key, 10U);
  EXPECT_EQ(iter, map.begin());
  EXPECT_EQ(iter, map.cbegin());
}

TEST_F(IntrusiveMapTest, ReverseIterator) {
  const IntrusiveMap& map = map_;
  auto iter = map.rbegin();
  size_t key = 55;
  for (size_t i = 0; i < kNumPairs; ++i) {
    auto& pair = *iter++;
    EXPECT_EQ(pair.key(), key);
    key -= 5;
  }
  EXPECT_EQ(key, 5U);
  EXPECT_EQ(iter, map.rend());
  EXPECT_EQ(iter, map.crend());
  for (size_t i = 0; i < kNumPairs; ++i) {
    key += 5;
    EXPECT_EQ((--iter)->key(), key);
  }
  EXPECT_EQ(key, 55U);
  EXPECT_EQ(iter, map.rbegin());
  EXPECT_EQ(iter, map.crbegin());
}

TEST_F(IntrusiveMapTest, IteratorIsDefaultConstructible) {
  IntrusiveMap::iterator iter;
  EXPECT_NE(iter, map_.begin());
  EXPECT_NE(iter, map_.begin());
  EXPECT_EQ(iter, IntrusiveMap::iterator());
}

TEST_F(IntrusiveMapTest, IteratorIsCopyConstructible) {
  IntrusiveMap::iterator iter1 = map_.begin();
  IntrusiveMap::iterator iter2(iter1);
  EXPECT_EQ(iter2, map_.begin());
}

TEST_F(IntrusiveMapTest, IteratorCopyAssignable) {
  IntrusiveMap::iterator iter1 = map_.begin();
  IntrusiveMap::iterator iter2 = iter1;
  EXPECT_EQ(iter2, map_.begin());
}

TEST_F(IntrusiveMapTest, IteratorisMoveConstructible) {
  IntrusiveMap::iterator iter1 = map_.begin();
  IntrusiveMap::iterator iter2(std::move(iter1));
  EXPECT_EQ(iter2, map_.begin());
}

TEST_F(IntrusiveMapTest, IteratorMoveAssignable) {
  IntrusiveMap::iterator iter1 = map_.begin();
  IntrusiveMap::iterator iter2 = std::move(iter1);
  EXPECT_EQ(iter2, map_.begin());
}

TEST_F(IntrusiveMapTest, ConstIterator_CompareNonConst) {
  EXPECT_EQ(map_.end(), map_.cend());
}

// A map pair that is distinct from TestPair
struct OtherPair : public ::pw::IntrusiveMap<size_t, OtherPair>::Pair,
                   public BaseItem {
 private:
  using Pair = ::pw::IntrusiveMap<size_t, OtherPair>::Pair;

 public:
  OtherPair(size_t key, const char* name) : Pair(key), BaseItem(name) {}
};

TEST_F(IntrusiveMapTest, ConstIterator_CompareNonConst_CompilationFails) {
  ::pw::IntrusiveMap<size_t, OtherPair> map;
#if PW_NC_TEST(CannotCompareIncompatibleIteratorsEqual)
  PW_NC_EXPECT("map_\.end\(\) == map\.end\(\)");
  static_cast<void>(map_.end() == map.end());
#elif PW_NC_TEST(CannotCompareIncompatibleIteratorsInequal)
  PW_NC_EXPECT("map_\.end\(\) != map\.end\(\)");
  static_cast<void>(map_.end() != map.end());
#endif  // PW_NC_TEST
}

#if PW_NC_TEST(CannotModifyThroughConstIterator)
PW_NC_EXPECT("function is not marked const|discards qualifiers");

TEST_F(IntrusiveMapTest, ConstIterator_Modify) {
  const IntrusiveMap& map = map_;
  auto iter = map.begin();
  iter->set_name("nope");
}

#endif  // PW_NC_TEST

// Capacity

TEST_F(IntrusiveMapTest, IsEmpty) {
  const IntrusiveMap& map = map_;
  EXPECT_FALSE(map.empty());
  map_.clear();
  EXPECT_TRUE(map.empty());
}

TEST_F(IntrusiveMapTest, GetSize) {
  const IntrusiveMap& map = map_;
  EXPECT_EQ(map.size(), kNumPairs);
  map_.clear();
  EXPECT_EQ(map.size(), 0U);
}

TEST_F(IntrusiveMapTest, GetMaxSize) {
  const IntrusiveMap& map = map_;
  EXPECT_EQ(map.max_size(), size_t(std::numeric_limits<ptrdiff_t>::max()));
}

// Modifiers

// This functions allows tests to use `std::is_sorted` without specializing
// `std::less<TestPair>`. Since `std::less` is the default value for the
// `Compare` template parameter, leaving it untouched avoids accidentally
// masking type-handling errors.
constexpr bool LessThan(const TestPair& lhs, const TestPair& rhs) {
  return lhs.key() < rhs.key();
}

TEST_F(IntrusiveMapTest, Insert) {
  map_.clear();
  bool sorted = true;
  size_t prev_key = 0;
  for (auto& pair : pairs_) {
    sorted &= prev_key < pair.key();

    // Use the "hinted" version of insert.
    map_.insert(map_.end(), pair);
    prev_key = pair.key();
  }
  EXPECT_FALSE(sorted);

  EXPECT_EQ(map_.size(), kNumPairs);
  EXPECT_TRUE(std::is_sorted(map_.begin(), map_.end(), LessThan));
}

TEST_F(IntrusiveMapTest, Insert_Duplicate) {
  TestPair pair1(60, "1");
  TestPair pair2(60, "2");

  auto result = map_.insert(pair1);
  EXPECT_STREQ(result.first->name(), "1");
  EXPECT_TRUE(result.second);

  result = map_.insert(pair2);
  EXPECT_STREQ(result.first->name(), "1");
  EXPECT_FALSE(result.second);

  EXPECT_EQ(map_.size(), kNumPairs + 1);
  EXPECT_TRUE(std::is_sorted(map_.begin(), map_.end(), LessThan));

  // Explicitly clear the map before pair 1 goes out of scope.
  map_.clear();
}

TEST_F(IntrusiveMapTest, Insert_ObjectIterators) {
  map_.clear();
  map_.insert(pairs_.begin(), pairs_.end());
  EXPECT_EQ(map_.size(), kNumPairs);
  EXPECT_TRUE(std::is_sorted(map_.begin(), map_.end(), LessThan));
}

TEST_F(IntrusiveMapTest, Insert_ObjectIterators_Empty) {
  map_.insert(pairs_.end(), pairs_.end());
  EXPECT_EQ(map_.size(), kNumPairs);
  EXPECT_TRUE(std::is_sorted(map_.begin(), map_.end(), LessThan));
}

TEST_F(IntrusiveMapTest, Insert_ObjectIterators_WithDuplicates) {
  std::array<TestPair, 3> pairs = {{
      {50, "B"},
      {40, "D"},
      {60, "F"},
  }};

  map_.insert(pairs.begin(), pairs.end());
  EXPECT_EQ(map_.size(), kNumPairs + 1);
  EXPECT_TRUE(std::is_sorted(map_.begin(), map_.end(), LessThan));
  EXPECT_STREQ(map_.at(40).name(), "d");
  EXPECT_STREQ(map_.at(50).name(), "b");
  EXPECT_STREQ(map_.at(60).name(), "F");

  // Explicitly clear the map before pairs goes out of scope.
  map_.clear();
}

TEST_F(IntrusiveMapTest, Insert_PointerIterators) {
  map_.clear();
  std::array<TestPair*, 3> ptrs = {&pairs_[0], &pairs_[1], &pairs_[2]};

  map_.insert(ptrs.begin(), ptrs.end());
  EXPECT_EQ(map_.size(), 3U);
  EXPECT_TRUE(std::is_sorted(map_.begin(), map_.end(), LessThan));
}

TEST_F(IntrusiveMapTest, Insert_PointerIterators_Empty) {
  std::array<TestPair*, 0> ptrs;

  map_.insert(ptrs.begin(), ptrs.end());
  EXPECT_EQ(map_.size(), kNumPairs);
  EXPECT_TRUE(std::is_sorted(map_.begin(), map_.end(), LessThan));
}

TEST_F(IntrusiveMapTest, Insert_PointerIterators_WithDuplicates) {
  TestPair pair1(50, "B");
  TestPair pair2(40, "D");
  TestPair pair3(60, "F");
  std::array<TestPair*, 3> ptrs = {&pair1, &pair2, &pair3};

  map_.insert(ptrs.begin(), ptrs.end());
  EXPECT_EQ(map_.size(), kNumPairs + 1);
  EXPECT_TRUE(std::is_sorted(map_.begin(), map_.end(), LessThan));
  EXPECT_STREQ(map_.at(40).name(), "d");
  EXPECT_STREQ(map_.at(50).name(), "b");
  EXPECT_STREQ(map_.at(60).name(), "F");

  // Explicitly clear the map before pairs goes out of scope.
  map_.clear();
}

TEST_F(IntrusiveMapTest, Insert_InitializerList) {
  map_.clear();
  map_.insert({&pairs_[0], &pairs_[2], &pairs_[4]});
  EXPECT_EQ(map_.size(), 3U);
  EXPECT_TRUE(std::is_sorted(map_.begin(), map_.end(), LessThan));
}

TEST_F(IntrusiveMapTest, Insert_InitializerList_Empty) {
  map_.insert({});
  EXPECT_EQ(map_.size(), kNumPairs);
  EXPECT_TRUE(std::is_sorted(map_.begin(), map_.end(), LessThan));
}

TEST_F(IntrusiveMapTest, Insert_InitializerList_WithDuplicates) {
  TestPair pair1(50, "B");
  TestPair pair2(40, "D");
  TestPair pair3(60, "F");

  map_.insert({&pair1, &pair2, &pair3});
  EXPECT_EQ(map_.size(), kNumPairs + 1);
  EXPECT_TRUE(std::is_sorted(map_.begin(), map_.end(), LessThan));
  EXPECT_STREQ(map_.at(40).name(), "d");
  EXPECT_STREQ(map_.at(50).name(), "b");
  EXPECT_STREQ(map_.at(60).name(), "F");

  // Explicitly clear the map before pairs goes out of scope.
  map_.clear();
}

// A pair derived from TestPair.
struct DerivedPair : public TestPair {
  DerivedPair(size_t n, const char* name) : TestPair(n * 10, name) {}
};

TEST_F(IntrusiveMapTest, Insert_DerivedPairs) {
  DerivedPair pair1(6, "f");
  map_.insert(pair1);

  DerivedPair pair2(7, "g");
  map_.insert(pair2);

  EXPECT_EQ(map_.size(), kNumPairs + 2);
  EXPECT_TRUE(std::is_sorted(map_.begin(), map_.end(), LessThan));

  // Explicitly clear the map before pairs goes out of scope.
  map_.clear();
}

TEST_F(IntrusiveMapTest, Insert_DerivedPairs_CompilationFails) {
  ::pw::IntrusiveMap<size_t, DerivedPair> derived_from_compatible_pair_type;

  DerivedPair pair1(6, "f");
  derived_from_compatible_pair_type.insert(pair1);

  EXPECT_EQ(derived_from_compatible_pair_type.size(), 1U);

#if PW_NC_TEST(CannotAddBaseClassToDerivedClassMap)
  PW_NC_EXPECT("derived_from_compatible_pair_type\.insert\(pair2\)");

  TestPair pair2(70, "g");
  derived_from_compatible_pair_type.insert(pair2);
#endif
  derived_from_compatible_pair_type.clear();
}

TEST_F(IntrusiveMapTest, Erase_One_ByItem) {
  for (size_t i = 0; i < kNumPairs; ++i) {
    EXPECT_EQ(map_.size(), kNumPairs);
    auto iter = map_.erase(pairs_[i]);
    if (iter != map_.end()) {
      EXPECT_GT(iter->key(), pairs_[i].key());
    }
    EXPECT_EQ(map_.size(), kNumPairs - 1);
    EXPECT_EQ(map_.find(pairs_[i].key()), map_.end());
    map_.insert(pairs_[i]);
  }
}

TEST_F(IntrusiveMapTest, Erase_One_ByItem_IteratorCopy) {
  // This test explicitly checks that the iterator returned by erase(T&) can be
  // copy constructed and assigned. This is to ensure that the fix for a static
  // analysis warning (which was caused by a delegating copy constructor) works
  // as intended and the returned iterator is valid.
  auto iter = map_.erase(pairs_[0]);

  // Copy construct
  IntrusiveMap::iterator iter_copy(iter);
  EXPECT_EQ(iter_copy, iter);

  // Copy assign
  IntrusiveMap::iterator iter_assign;
  iter_assign = iter;
  EXPECT_EQ(iter_assign, iter);

  // Verify it points to the next item.
  // The map is sorted by key. The keys in `pairs_` are:
  // 10, 15, 20, 25, 30, 35, 40, 45, 50, 55.
  //
  // `pairs_[0]` has key 30.
  // The next key in the sorted sequence is 35.

  EXPECT_EQ(iter->key(), 35U);
  EXPECT_EQ(iter_copy->key(), 35U);
  EXPECT_EQ(iter_assign->key(), 35U);

  map_.insert(pairs_[0]);
}

TEST_F(IntrusiveMapTest, Erase_One_ByKey) {
  for (size_t i = 0; i < kNumPairs; ++i) {
    EXPECT_EQ(map_.size(), kNumPairs);
    EXPECT_EQ(map_.erase(pairs_[i].key()), 1U);
    EXPECT_EQ(map_.size(), kNumPairs - 1);
    auto iter = map_.find(pairs_[i].key());
    EXPECT_EQ(iter, map_.end());
    map_.insert(pairs_[i]);
  }
}

TEST_F(IntrusiveMapTest, Erase_OnlyItem) {
  map_.clear();
  map_.insert(pairs_[0]);
  EXPECT_EQ(map_.size(), 1U);

  EXPECT_EQ(map_.erase(pairs_[0].key()), 1U);
  EXPECT_EQ(map_.size(), 0U);
}

TEST_F(IntrusiveMapTest, Erase_AllOnebyOne) {
  auto iter = map_.begin();
  for (size_t n = kNumPairs; n != 0; --n) {
    ASSERT_NE(iter, map_.end());
    iter = map_.erase(iter);
  }
  EXPECT_EQ(iter, map_.end());
  EXPECT_EQ(map_.size(), 0U);
}

TEST_F(IntrusiveMapTest, Erase_Range) {
  auto first = map_.begin();
  auto last = map_.end();
  ++first;
  --last;
  auto iter = map_.erase(first, last);
  EXPECT_EQ(map_.size(), 2U);
  EXPECT_TRUE(std::is_sorted(map_.begin(), map_.end(), LessThan));
  EXPECT_EQ(iter->key(), 55U);
}

TEST_F(IntrusiveMapTest, Erase_AllRange) {
  auto first = map_.begin();
  auto last = map_.end();
  auto iter = map_.erase(first, last);
  EXPECT_TRUE(map_.empty());
  EXPECT_EQ(iter, map_.end());
}

TEST_F(IntrusiveMapTest, Erase_MissingItem) { EXPECT_EQ(map_.erase(100), 0U); }

TEST_F(IntrusiveMapTest, Erase_Reinsert) {
  EXPECT_EQ(map_.size(), pairs_.size());

  EXPECT_EQ(map_.erase(pairs_[0].key()), 1U);
  EXPECT_EQ(map_.find(pairs_[0].key()), map_.end());

  EXPECT_EQ(map_.erase(pairs_[2].key()), 1U);
  EXPECT_EQ(map_.find(pairs_[2].key()), map_.end());

  EXPECT_EQ(map_.erase(pairs_[4].key()), 1U);
  EXPECT_EQ(map_.find(pairs_[4].key()), map_.end());

  EXPECT_EQ(map_.size(), pairs_.size() - 3);

  map_.insert(pairs_[4]);
  auto iter = map_.find(pairs_[4].key());
  EXPECT_NE(iter, map_.end());

  map_.insert(pairs_[0]);
  iter = map_.find(pairs_[0].key());
  EXPECT_NE(iter, map_.end());

  map_.insert(pairs_[2]);
  iter = map_.find(pairs_[2].key());
  EXPECT_NE(iter, map_.end());

  EXPECT_EQ(map_.size(), pairs_.size());
}

TEST_F(IntrusiveMapTest, Swap) {
  std::array<TestPair, 3> pairs = {{
      {50, "B"},
      {40, "D"},
      {60, "F"},
  }};
  IntrusiveMap map(pairs.begin(), pairs.end());

  map_.swap(map);
  EXPECT_EQ(map.size(), kNumPairs);
  EXPECT_TRUE(std::is_sorted(map.begin(), map.end(), LessThan));
  EXPECT_EQ(map.at(30).name(), "a");
  EXPECT_EQ(map.at(50).name(), "b");
  EXPECT_EQ(map.at(20).name(), "c");
  EXPECT_EQ(map.at(40).name(), "d");
  EXPECT_EQ(map.at(10).name(), "e");
  map.clear();

  EXPECT_EQ(map_.size(), 3U);
  EXPECT_TRUE(std::is_sorted(map_.begin(), map_.end(), LessThan));
  EXPECT_STREQ(map_.at(50).name(), "B");
  EXPECT_STREQ(map_.at(40).name(), "D");
  EXPECT_STREQ(map_.at(60).name(), "F");

  // Explicitly clear the map before pairs goes out of scope.
  map_.clear();
}

TEST_F(IntrusiveMapTest, Swap_Empty) {
  IntrusiveMap map;

  map_.swap(map);
  EXPECT_EQ(map.size(), kNumPairs);
  EXPECT_TRUE(std::is_sorted(map.begin(), map.end(), LessThan));
  EXPECT_EQ(map.at(30).name(), "a");
  EXPECT_EQ(map.at(50).name(), "b");
  EXPECT_EQ(map.at(20).name(), "c");
  EXPECT_EQ(map.at(40).name(), "d");
  EXPECT_EQ(map.at(10).name(), "e");
  map.clear();

  EXPECT_EQ(map_.size(), 0U);
}

TEST_F(IntrusiveMapTest, Merge) {
  std::array<TestPair, 3> pairs = {{
      {5, "f"},
      {75, "g"},
      {85, "h"},
  }};
  IntrusiveMap map(pairs.begin(), pairs.end());

  map_.merge(map);
  EXPECT_TRUE(map.empty());
  EXPECT_EQ(map_.size(), kNumPairs + 3);
  EXPECT_TRUE(std::is_sorted(map_.begin(), map_.end(), LessThan));
  EXPECT_STREQ(map_.at(30).name(), "a");
  EXPECT_STREQ(map_.at(35).name(), "A");
  EXPECT_STREQ(map_.at(50).name(), "b");
  EXPECT_STREQ(map_.at(55).name(), "B");
  EXPECT_STREQ(map_.at(20).name(), "c");
  EXPECT_STREQ(map_.at(25).name(), "C");
  EXPECT_STREQ(map_.at(40).name(), "d");
  EXPECT_STREQ(map_.at(45).name(), "D");
  EXPECT_STREQ(map_.at(10).name(), "e");
  EXPECT_STREQ(map_.at(15).name(), "E");
  EXPECT_STREQ(map_.at(5).name(), "f");
  EXPECT_STREQ(map_.at(75).name(), "g");
  EXPECT_STREQ(map_.at(85).name(), "h");

  // Explicitly clear the map before pairs goes out of scope.
  map_.clear();
}

TEST_F(IntrusiveMapTest, Merge_Empty) {
  IntrusiveMap map;

  map_.merge(map);
  EXPECT_EQ(map_.size(), kNumPairs);
  EXPECT_TRUE(std::is_sorted(map_.begin(), map_.end(), LessThan));

  map.merge(map_);
  EXPECT_TRUE(map_.empty());
  EXPECT_EQ(map.size(), kNumPairs);
  EXPECT_TRUE(std::is_sorted(map.begin(), map.end(), LessThan));

  map.clear();
}

TEST_F(IntrusiveMapTest, Merge_WithDuplicates) {
  std::array<TestPair, 3> pairs = {{
      {50, "B"},
      {40, "D"},
      {60, "F"},
  }};
  IntrusiveMap map(pairs.begin(), pairs.end());

  map_.merge(map);
  EXPECT_TRUE(map.empty());
  EXPECT_EQ(map_.size(), kNumPairs + 1);
  EXPECT_TRUE(std::is_sorted(map_.begin(), map_.end(), LessThan));
  EXPECT_STREQ(map_.at(30).name(), "a");
  EXPECT_STREQ(map_.at(50).name(), "b");
  EXPECT_STREQ(map_.at(20).name(), "c");
  EXPECT_STREQ(map_.at(40).name(), "d");
  EXPECT_STREQ(map_.at(10).name(), "e");
  EXPECT_STREQ(map_.at(60).name(), "F");

  // Explicitly clear the map before pairs goes out of scope.
  map_.clear();
}

TEST_F(IntrusiveMapTest, Merge_MultiMap) {
  std::array<TestPair, 3> pairs = {{
      {50, "B"},
      {40, "D"},
      {60, "F"},
  }};
  ::pw::IntrusiveMultiMap<size_t, TestPair> multimap(pairs.begin(),
                                                     pairs.end());

  map_.merge(multimap);
  EXPECT_TRUE(multimap.empty());
  EXPECT_EQ(map_.size(), kNumPairs + 1);
  EXPECT_TRUE(std::is_sorted(map_.begin(), map_.end(), LessThan));
  EXPECT_STREQ(map_.at(30).name(), "a");
  EXPECT_STREQ(map_.at(50).name(), "b");
  EXPECT_STREQ(map_.at(20).name(), "c");
  EXPECT_STREQ(map_.at(40).name(), "d");
  EXPECT_STREQ(map_.at(10).name(), "e");
  EXPECT_STREQ(map_.at(60).name(), "F");

  // Explicitly clear the map before pairs goes out of scope.
  map_.clear();
}

TEST_F(IntrusiveMapTest, Count) {
  const IntrusiveMap& map = map_;
  EXPECT_EQ(map.count(10), 1U);
  EXPECT_EQ(map.count(20), 1U);
  EXPECT_EQ(map.count(30), 1U);
  EXPECT_EQ(map.count(40), 1U);
  EXPECT_EQ(map.count(50), 1U);
}

TEST_F(IntrusiveMapTest, Count_NoSuchKey) {
  const IntrusiveMap& map = map_;
  EXPECT_EQ(map.count(60), 0U);
}

TEST_F(IntrusiveMapTest, Find) {
  const IntrusiveMap& map = map_;
  size_t key = 10;
  for (size_t i = 0; i < kNumPairs; ++i) {
    auto iter = map.find(key);
    ASSERT_NE(iter, map.end());
    EXPECT_EQ(iter->key(), key);
    key += 5;
  }
}

TEST_F(IntrusiveMapTest, Find_NoSuchKey) {
  const IntrusiveMap& map = map_;
  auto iter = map.find(60);
  EXPECT_EQ(iter, map.end());
}

TEST_F(IntrusiveMapTest, LowerBound) {
  const IntrusiveMap& map = map_;
  auto iter = map.lower_bound(10);
  ASSERT_NE(iter, map.end());
  EXPECT_STREQ(iter->name(), "e");

  iter = map.lower_bound(20);
  ASSERT_NE(iter, map.end());
  EXPECT_STREQ(iter->name(), "c");

  iter = map.lower_bound(30);
  ASSERT_NE(iter, map.end());
  EXPECT_STREQ(iter->name(), "a");

  iter = map.lower_bound(40);
  ASSERT_NE(iter, map.end());
  EXPECT_STREQ(iter->name(), "d");

  iter = map.lower_bound(50);
  ASSERT_NE(iter, map.end());
  EXPECT_STREQ(iter->name(), "b");
}

TEST_F(IntrusiveMapTest, LowerBound_NoExactKey) {
  const IntrusiveMap& map = map_;
  auto iter = map.lower_bound(6);
  ASSERT_NE(iter, map.end());
  EXPECT_STREQ(iter->name(), "e");

  iter = map.lower_bound(16);
  ASSERT_NE(iter, map.end());
  EXPECT_STREQ(iter->name(), "c");

  iter = map.lower_bound(26);
  ASSERT_NE(iter, map.end());
  EXPECT_STREQ(iter->name(), "a");

  iter = map.lower_bound(36);
  ASSERT_NE(iter, map.end());
  EXPECT_STREQ(iter->name(), "d");

  iter = map.lower_bound(46);
  ASSERT_NE(iter, map.end());
  EXPECT_STREQ(iter->name(), "b");
}

TEST_F(IntrusiveMapTest, LowerBound_OutOfRange) {
  const IntrusiveMap& map = map_;
  EXPECT_EQ(map.lower_bound(56), map.end());
}

TEST_F(IntrusiveMapTest, UpperBound) {
  const IntrusiveMap& map = map_;
  auto iter = map.upper_bound(15);
  ASSERT_NE(iter, map.end());
  EXPECT_STREQ(iter->name(), "c");

  iter = map.upper_bound(25);
  ASSERT_NE(iter, map.end());
  EXPECT_STREQ(iter->name(), "a");

  iter = map.upper_bound(35);
  ASSERT_NE(iter, map.end());
  EXPECT_STREQ(iter->name(), "d");

  iter = map.upper_bound(45);
  ASSERT_NE(iter, map.end());
  EXPECT_STREQ(iter->name(), "b");

  EXPECT_EQ(map.upper_bound(55), map.end());
}

TEST_F(IntrusiveMapTest, UpperBound_NoExactKey) {
  const IntrusiveMap& map = map_;
  auto iter = map.upper_bound(5);
  ASSERT_NE(iter, map.end());
  EXPECT_STREQ(iter->name(), "e");

  iter = map.upper_bound(15);
  ASSERT_NE(iter, map.end());
  EXPECT_STREQ(iter->name(), "c");

  iter = map.upper_bound(25);
  ASSERT_NE(iter, map.end());
  EXPECT_STREQ(iter->name(), "a");

  iter = map.upper_bound(35);
  ASSERT_NE(iter, map.end());
  EXPECT_STREQ(iter->name(), "d");

  iter = map.upper_bound(45);
  ASSERT_NE(iter, map.end());
  EXPECT_STREQ(iter->name(), "b");
}

TEST_F(IntrusiveMapTest, UpperBound_OutOfRange) {
  const IntrusiveMap& map = map_;
  EXPECT_EQ(map.upper_bound(55), map.end());
}

TEST_F(IntrusiveMapTest, EqualRange) {
  const IntrusiveMap& map = map_;

  auto pair = map.equal_range(10);
  IntrusiveMap::const_iterator lower = pair.first;
  IntrusiveMap::const_iterator upper = pair.second;
  ASSERT_NE(lower, map.end());
  EXPECT_STREQ(lower->name(), "e");
  ASSERT_NE(upper, map.end());
  EXPECT_STREQ(upper->name(), "E");

  std::tie(lower, upper) = map.equal_range(20);
  ASSERT_NE(lower, map.end());
  EXPECT_STREQ(lower->name(), "c");
  ASSERT_NE(upper, map.end());
  EXPECT_STREQ(upper->name(), "C");

  std::tie(lower, upper) = map.equal_range(30);
  ASSERT_NE(lower, map.end());
  EXPECT_STREQ(lower->name(), "a");
  ASSERT_NE(upper, map.end());
  EXPECT_STREQ(upper->name(), "A");

  std::tie(lower, upper) = map.equal_range(40);
  ASSERT_NE(lower, map.end());
  EXPECT_STREQ(lower->name(), "d");
  ASSERT_NE(upper, map.end());
  EXPECT_STREQ(upper->name(), "D");

  std::tie(lower, upper) = map.equal_range(50);
  ASSERT_NE(lower, map.end());
  EXPECT_STREQ(lower->name(), "b");
  ASSERT_NE(upper, map.end());
  EXPECT_STREQ(upper->name(), "B");
}

TEST_F(IntrusiveMapTest, EqualRange_NoExactKey) {
  const IntrusiveMap& map = map_;

  auto pair = map.equal_range(6);
  IntrusiveMap::const_iterator lower = pair.first;
  IntrusiveMap::const_iterator upper = pair.second;
  ASSERT_NE(lower, map.end());
  EXPECT_STREQ(lower->name(), "e");
  ASSERT_NE(upper, map.end());
  EXPECT_STREQ(upper->name(), "e");

  std::tie(lower, upper) = map.equal_range(16);
  ASSERT_NE(lower, map.end());
  EXPECT_STREQ(lower->name(), "c");
  ASSERT_NE(upper, map.end());
  EXPECT_STREQ(upper->name(), "c");

  std::tie(lower, upper) = map.equal_range(26);
  ASSERT_NE(lower, map.end());
  EXPECT_STREQ(lower->name(), "a");
  ASSERT_NE(upper, map.end());
  EXPECT_STREQ(upper->name(), "a");

  std::tie(lower, upper) = map.equal_range(36);
  ASSERT_NE(lower, map.end());
  EXPECT_STREQ(lower->name(), "d");
  ASSERT_NE(upper, map.end());
  EXPECT_STREQ(upper->name(), "d");

  std::tie(lower, upper) = map.equal_range(46);
  ASSERT_NE(lower, map.end());
  EXPECT_STREQ(lower->name(), "b");
  ASSERT_NE(upper, map.end());
  EXPECT_STREQ(upper->name(), "b");
}

TEST_F(IntrusiveMapTest, EqualRange_OutOfRange) {
  const IntrusiveMap& map = map_;

  auto pair = map.equal_range(56);
  IntrusiveMap::const_iterator lower = pair.first;
  IntrusiveMap::const_iterator upper = pair.second;
  EXPECT_EQ(lower, map.end());
  EXPECT_EQ(upper, map.end());
}

}  // namespace
