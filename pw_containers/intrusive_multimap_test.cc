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

#include "pw_containers/intrusive_multimap.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <tuple>
#include <utility>

#include "pw_compilation_testing/negative_compilation.h"
#include "pw_containers/intrusive_map.h"
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
class TestPair : public ::pw::IntrusiveMultiMap<size_t, TestPair>::Pair,
                 public BaseItem {
 private:
  using Pair = ::pw::IntrusiveMultiMap<size_t, TestPair>::Pair;

 public:
  TestPair(size_t key, const char* name) : Pair(key), BaseItem(name) {}
};

// Test fixture.
class IntrusiveMultiMapTest : public ::testing::Test {
 protected:
  using IntrusiveMultiMap = ::pw::IntrusiveMultiMap<size_t, TestPair>;
  static constexpr size_t kNumPairs = 10;

  void SetUp() override { multimap_.insert(pairs_.begin(), pairs_.end()); }

  void TearDown() override { multimap_.clear(); }

  std::array<TestPair, kNumPairs> pairs_ = {{
      {30, "a"},
      {50, "b"},
      {20, "c"},
      {40, "d"},
      {10, "e"},
      {30, "A"},
      {50, "B"},
      {20, "C"},
      {40, "D"},
      {10, "E"},
  }};

  IntrusiveMultiMap multimap_;
};

// Unit tests.

TEST_F(IntrusiveMultiMapTest, Construct_Default) {
  IntrusiveMultiMap multimap;
  EXPECT_TRUE(multimap.empty());
  EXPECT_EQ(multimap.begin(), multimap.end());
  EXPECT_EQ(multimap.rbegin(), multimap.rend());
  EXPECT_EQ(multimap.size(), 0U);
  EXPECT_EQ(multimap.lower_bound(0), multimap.end());
  EXPECT_EQ(multimap.upper_bound(0), multimap.end());
}

TEST_F(IntrusiveMultiMapTest, Construct_ObjectIterators) {
  multimap_.clear();
  IntrusiveMultiMap multimap(pairs_.begin(), pairs_.end());
  EXPECT_FALSE(multimap.empty());
  EXPECT_EQ(multimap.size(), pairs_.size());
  multimap.clear();
}

TEST_F(IntrusiveMultiMapTest, Construct_ObjectIterators_Empty) {
  IntrusiveMultiMap multimap(pairs_.end(), pairs_.end());
  EXPECT_TRUE(multimap.empty());
  EXPECT_EQ(multimap.size(), 0U);
}

TEST_F(IntrusiveMultiMapTest, Construct_PointerIterators) {
  std::array<TestPair*, 3> ptrs = {&pairs_[0], &pairs_[1], &pairs_[2]};
  multimap_.clear();
  IntrusiveMultiMap multimap(ptrs.begin(), ptrs.end());
  EXPECT_FALSE(multimap.empty());
  EXPECT_EQ(multimap.size(), 3U);
  multimap.clear();
}

TEST_F(IntrusiveMultiMapTest, Construct_PointerIterators_Empty) {
  std::array<TestPair*, 0> ptrs;
  IntrusiveMultiMap multimap(ptrs.begin(), ptrs.end());
  EXPECT_TRUE(multimap.empty());
  EXPECT_EQ(multimap.size(), 0U);
  multimap.clear();
}

TEST_F(IntrusiveMultiMapTest, Construct_InitializerList) {
  multimap_.clear();
  IntrusiveMultiMap multimap({&pairs_[0], &pairs_[2], &pairs_[4]});
  auto iter = multimap.begin();
  EXPECT_EQ((iter++)->key(), 10U);
  EXPECT_EQ((iter++)->key(), 20U);
  EXPECT_EQ((iter++)->key(), 30U);
  EXPECT_EQ(iter, multimap.end());
  multimap.clear();
}

TEST_F(IntrusiveMultiMapTest, Construct_InitializerList_Empty) {
  IntrusiveMultiMap multimap({});
  EXPECT_TRUE(multimap.empty());
  EXPECT_EQ(multimap.size(), 0U);
}

TEST_F(IntrusiveMultiMapTest, Construct_CustomCompare) {
  multimap_.clear();
  IntrusiveMultiMap multimap({&pairs_[0], &pairs_[2], &pairs_[4]},
                             std::greater<>());
  auto iter = multimap.begin();
  EXPECT_EQ((iter++)->key(), 30U);
  EXPECT_EQ((iter++)->key(), 20U);
  EXPECT_EQ((iter++)->key(), 10U);
  EXPECT_EQ(iter, multimap.end());
  multimap.clear();
}

// A map pair that includes a key accessor method.
struct HalvedKey : public ::pw::IntrusiveMultiMap<size_t, HalvedKey>::Item,
                   public BaseItem {
 private:
  using MapItem = ::pw::IntrusiveMultiMap<size_t, HalvedKey>::Item;

 public:
  HalvedKey(size_t half_key, const char* name)
      : BaseItem(name), half_key_(half_key) {}
  size_t key() const { return half_key_ * 2; }

 private:
  const size_t half_key_;
};

TEST_F(IntrusiveMultiMapTest, Construct_CustomItem) {
  std::array<HalvedKey, 3> items = {{
      {50, "B"},
      {40, "D"},
      {60, "F"},
  }};
  pw::IntrusiveMultiMap<size_t, HalvedKey> multimap(items.begin(), items.end());

  auto iter = multimap.find(80);
  ASSERT_NE(iter, multimap.end());
  EXPECT_STREQ(iter->name(), "D");

  iter = multimap.find(100);
  ASSERT_NE(iter, multimap.end());
  EXPECT_STREQ(iter->name(), "B");

  iter = multimap.find(120);
  ASSERT_NE(iter, multimap.end());
  EXPECT_STREQ(iter->name(), "F");

  multimap.clear();
}

// A map item that has no explicit key.
struct NoKey : public ::pw::IntrusiveMultiMap<size_t, NoKey>::Item,
               public BaseItem {
 public:
  explicit NoKey(const char* name) : BaseItem(name) {}
};

// A functor to get an implied key from a `NoKey` item.
struct GetImpliedKey {
  size_t operator()(const NoKey& item) const {
    return std::strlen(item.name());
  }
};

TEST_F(IntrusiveMultiMapTest, Construct_CustomGetKey) {
  std::array<NoKey, 5> items = {
      NoKey("CC"),
      NoKey("AAA"),
      NoKey("AAA"),
      NoKey("B"),
      NoKey("DDDD"),
  };
  pw::IntrusiveMultiMap<size_t, NoKey> multimap(
      items.begin(), items.end(), std::less<>(), GetImpliedKey());

  auto iter = multimap.begin();
  EXPECT_STREQ((iter++)->name(), "B");
  EXPECT_STREQ((iter++)->name(), "CC");
  EXPECT_STREQ((iter++)->name(), "AAA");
  EXPECT_STREQ((iter++)->name(), "AAA");
  EXPECT_STREQ((iter++)->name(), "DDDD");
  multimap.clear();
}

//  A struct that is not a multimap pair.
class NotAnItem : public BaseItem {
 public:
  NotAnItem(const char* name, size_t key) : BaseItem(name), key_(key) {}
  size_t key() const { return key_; }

 private:
  const size_t key_;
};

#if PW_NC_TEST(IncompatibleItem)
PW_NC_EXPECT(
    "IntrusiveMultiMap items must be derived from IntrusiveMultiMap<Key, "
    "T>::Item");

struct BadItem : public ::pw::IntrusiveMultiMap<size_t, NotAnItem>::Item {
  constexpr explicit BadItem(size_t key) : key_(key) {}
  constexpr size_t key() const { return key_; }
  constexpr bool operator<(const Pair& rhs) { return key_ < rhs.key(); }

 private:
  const size_t key_;
};

[[maybe_unused]] ::pw::IntrusiveMultiMap<size_t, BadItem> bad_multimap1;

#elif PW_NC_TEST(DoesNotInheritFromItem)
PW_NC_EXPECT(
    "IntrusiveMultiMap items must be derived from IntrusiveMultiMap<Key, "
    "T>::Item");

[[maybe_unused]] ::pw::IntrusiveMultiMap<size_t, NotAnItem> bad_multimap2;

#endif  // PW_NC_TEST

// Iterators

TEST_F(IntrusiveMultiMapTest, Iterator) {
  const IntrusiveMultiMap& multimap = multimap_;
  auto iter = multimap.begin();
  size_t key = 10;
  for (size_t i = 0; i < kNumPairs; i += 2) {
    EXPECT_EQ((*iter++).key(), key);
    EXPECT_EQ((*iter++).key(), key);
    key += 10;
  }
  EXPECT_EQ(key, 60U);
  EXPECT_EQ(iter, multimap.end());
  EXPECT_EQ(iter, multimap.cend());
  for (size_t i = 0; i < kNumPairs; i += 2) {
    key -= 10;
    EXPECT_EQ((--iter)->key(), key);
    EXPECT_EQ((--iter)->key(), key);
  }
  EXPECT_EQ(key, 10U);
  EXPECT_EQ(iter, multimap.begin());
  EXPECT_EQ(iter, multimap.cbegin());
}

TEST_F(IntrusiveMultiMapTest, ReverseIterator) {
  const IntrusiveMultiMap& multimap = multimap_;
  auto iter = multimap.rbegin();
  size_t key = 50;
  for (size_t i = 0; i < kNumPairs; i += 2) {
    EXPECT_EQ((*iter++).key(), key);
    EXPECT_EQ((*iter++).key(), key);
    key -= 10;
  }
  EXPECT_EQ(key, 0U);
  EXPECT_EQ(iter, multimap.rend());
  EXPECT_EQ(iter, multimap.crend());
  for (size_t i = 0; i < kNumPairs; i += 2) {
    key += 10;
    EXPECT_EQ((--iter)->key(), key);
    EXPECT_EQ((--iter)->key(), key);
  }
  EXPECT_EQ(key, 50U);
  EXPECT_EQ(iter, multimap.rbegin());
  EXPECT_EQ(iter, multimap.crbegin());
}

TEST_F(IntrusiveMultiMapTest, IteratorIsDefaultConstructible) {
  IntrusiveMultiMap::iterator iter;
  EXPECT_NE(iter, multimap_.begin());
  EXPECT_NE(iter, multimap_.begin());
  EXPECT_EQ(iter, IntrusiveMultiMap::iterator());
}

TEST_F(IntrusiveMultiMapTest, IteratorIsCopyConstructible) {
  IntrusiveMultiMap::iterator iter1 = multimap_.begin();
  IntrusiveMultiMap::iterator iter2(iter1);
  EXPECT_EQ(iter2, multimap_.begin());
}

TEST_F(IntrusiveMultiMapTest, IteratorCopyAssignable) {
  IntrusiveMultiMap::iterator iter1 = multimap_.begin();
  IntrusiveMultiMap::iterator iter2 = iter1;
  EXPECT_EQ(iter2, multimap_.begin());
}

TEST_F(IntrusiveMultiMapTest, IteratorisMoveConstructible) {
  IntrusiveMultiMap::iterator iter1 = multimap_.begin();
  IntrusiveMultiMap::iterator iter2(std::move(iter1));
  EXPECT_EQ(iter2, multimap_.begin());
}

TEST_F(IntrusiveMultiMapTest, IteratorMoveAssignable) {
  IntrusiveMultiMap::iterator iter1 = multimap_.begin();
  IntrusiveMultiMap::iterator iter2 = std::move(iter1);
  EXPECT_EQ(iter2, multimap_.begin());
}

TEST_F(IntrusiveMultiMapTest, ConstIterator_CompareNonConst) {
  EXPECT_EQ(multimap_.end(), multimap_.cend());
}

// A multimap pair that is distinct from TestPair
class OtherPair : public ::pw::IntrusiveMultiMap<size_t, OtherPair>::Pair,
                  public BaseItem {
 private:
  using Pair = ::pw::IntrusiveMultiMap<size_t, OtherPair>::Pair;

 public:
  OtherPair(size_t key, const char* name) : Pair(key), BaseItem(name) {}
};

TEST_F(IntrusiveMultiMapTest, ConstIterator_CompareNonConst_CompilationFails) {
  ::pw::IntrusiveMultiMap<size_t, OtherPair> multimap;
#if PW_NC_TEST(CannotCompareIncompatibleIteratorsEqual)
  PW_NC_EXPECT("multimap_\.end\(\) == multimap\.end\(\)");
  static_cast<void>(multimap_.end() == multimap.end());
#elif PW_NC_TEST(CannotCompareIncompatibleIteratorsInequal)
  PW_NC_EXPECT("multimap_\.end\(\) != multimap\.end\(\)");
  static_cast<void>(multimap_.end() != multimap.end());
#endif  // PW_NC_TEST
}

#if PW_NC_TEST(CannotModifyThroughConstIterator)
PW_NC_EXPECT("function is not marked const|discards qualifiers");

TEST_F(IntrusiveMultiMapTest, ConstIterator_Modify) {
  const IntrusiveMultiMap& multimap = multimap_;
  auto iter = multimap.begin();
  iter->set_name("nope");
}

#endif  // PW_NC_TEST

// Capacity

TEST_F(IntrusiveMultiMapTest, IsEmpty) {
  const IntrusiveMultiMap& multimap = multimap_;
  EXPECT_FALSE(multimap.empty());
  multimap_.clear();
  EXPECT_TRUE(multimap.empty());
}

TEST_F(IntrusiveMultiMapTest, GetSize) {
  const IntrusiveMultiMap& multimap = multimap_;
  EXPECT_EQ(multimap.size(), kNumPairs);
  multimap_.clear();
  EXPECT_EQ(multimap.size(), 0U);
}

TEST_F(IntrusiveMultiMapTest, GetMaxSize) {
  const IntrusiveMultiMap& multimap = multimap_;
  EXPECT_EQ(multimap.max_size(), size_t(std::numeric_limits<ptrdiff_t>::max()));
}

// Modifiers

// This functions allows tests to use `std::is_sorted` without specializing
// `std::less<TestPair>`. Since `std::less` is the default value for the
// `Compare` template parameter, leaving it untouched avoids accidentally
// masking type-handling errors.
constexpr bool LessThan(const TestPair& lhs, const TestPair& rhs) {
  return lhs.key() < rhs.key();
}

TEST_F(IntrusiveMultiMapTest, Insert) {
  multimap_.clear();
  bool sorted = true;
  size_t prev_key = 0;
  for (auto& pair : pairs_) {
    sorted &= prev_key < pair.key();

    // Use the "hinted" version of insert.
    multimap_.insert(multimap_.end(), pair);
    prev_key = pair.key();
  }
  EXPECT_FALSE(sorted);

  EXPECT_EQ(multimap_.size(), kNumPairs);
  EXPECT_TRUE(std::is_sorted(multimap_.begin(), multimap_.end(), LessThan));
}

TEST_F(IntrusiveMultiMapTest, Insert_Duplicate) {
  TestPair pair1(60, "1");
  TestPair pair2(60, "2");

  auto iter = multimap_.insert(pair1);
  EXPECT_STREQ(iter->name(), "1");

  iter = multimap_.insert(pair2);
  EXPECT_STREQ(iter->name(), "2");

  EXPECT_EQ(multimap_.size(), kNumPairs + 2);
  EXPECT_TRUE(std::is_sorted(multimap_.begin(), multimap_.end(), LessThan));

  // Explicitly clear the multimap before pair 1 goes out of scope.
  multimap_.clear();
}

TEST_F(IntrusiveMultiMapTest, Insert_ObjectIterators) {
  multimap_.clear();
  multimap_.insert(pairs_.begin(), pairs_.end());
  EXPECT_EQ(multimap_.size(), kNumPairs);
  EXPECT_TRUE(std::is_sorted(multimap_.begin(), multimap_.end(), LessThan));
}

TEST_F(IntrusiveMultiMapTest, Insert_ObjectIterators_Empty) {
  multimap_.insert(pairs_.end(), pairs_.end());
  EXPECT_EQ(multimap_.size(), kNumPairs);
  EXPECT_TRUE(std::is_sorted(multimap_.begin(), multimap_.end(), LessThan));
}

TEST_F(IntrusiveMultiMapTest, Insert_ObjectIterators_WithDuplicates) {
  std::array<TestPair, 3> pairs = {{
      {50, "B"},
      {40, "D"},
      {60, "F"},
  }};

  multimap_.insert(pairs.begin(), pairs.end());
  EXPECT_EQ(multimap_.size(), kNumPairs + 3);
  EXPECT_TRUE(std::is_sorted(multimap_.begin(), multimap_.end(), LessThan));

  auto iter = multimap_.find(40);
  ASSERT_NE(iter, multimap_.end());
  EXPECT_STREQ((iter++)->name(), "d");
  EXPECT_STREQ(iter->name(), "D");

  iter = multimap_.find(50);
  ASSERT_NE(iter, multimap_.end());
  EXPECT_STREQ((iter++)->name(), "b");
  EXPECT_STREQ(iter->name(), "B");

  iter = multimap_.find(60);
  ASSERT_NE(iter, multimap_.end());
  EXPECT_STREQ(iter->name(), "F");

  // Explicitly clear the multimap before pairs goes out of scope.
  multimap_.clear();
}

TEST_F(IntrusiveMultiMapTest, Insert_PointerIterators) {
  multimap_.clear();
  std::array<TestPair*, 3> ptrs = {&pairs_[0], &pairs_[1], &pairs_[2]};

  multimap_.insert(ptrs.begin(), ptrs.end());
  EXPECT_EQ(multimap_.size(), 3U);
  EXPECT_TRUE(std::is_sorted(multimap_.begin(), multimap_.end(), LessThan));
}

TEST_F(IntrusiveMultiMapTest, Insert_PointerIterators_Empty) {
  std::array<TestPair*, 0> ptrs;

  multimap_.insert(ptrs.begin(), ptrs.end());
  EXPECT_EQ(multimap_.size(), kNumPairs);
  EXPECT_TRUE(std::is_sorted(multimap_.begin(), multimap_.end(), LessThan));
}

TEST_F(IntrusiveMultiMapTest, Insert_PointerIterators_WithDuplicates) {
  TestPair pair1(50, "B");
  TestPair pair2(40, "D");
  TestPair pair3(60, "F");
  std::array<TestPair*, 3> ptrs = {&pair1, &pair2, &pair3};

  multimap_.insert(ptrs.begin(), ptrs.end());
  EXPECT_EQ(multimap_.size(), kNumPairs + 3);
  EXPECT_TRUE(std::is_sorted(multimap_.begin(), multimap_.end(), LessThan));

  auto iter = multimap_.find(40);
  ASSERT_NE(iter, multimap_.end());
  EXPECT_STREQ((iter++)->name(), "d");
  EXPECT_STREQ(iter->name(), "D");

  iter = multimap_.find(50);
  ASSERT_NE(iter, multimap_.end());
  EXPECT_STREQ((iter++)->name(), "b");
  EXPECT_STREQ(iter->name(), "B");

  iter = multimap_.find(60);
  ASSERT_NE(iter, multimap_.end());
  EXPECT_STREQ(iter->name(), "F");

  // Explicitly clear the multimap before pairs goes out of scope.
  multimap_.clear();
}

TEST_F(IntrusiveMultiMapTest, Insert_InitializerList) {
  multimap_.clear();
  multimap_.insert({&pairs_[0], &pairs_[2], &pairs_[4]});
  EXPECT_EQ(multimap_.size(), 3U);
  EXPECT_TRUE(std::is_sorted(multimap_.begin(), multimap_.end(), LessThan));
}

TEST_F(IntrusiveMultiMapTest, Insert_InitializerList_Empty) {
  multimap_.insert({});
  EXPECT_EQ(multimap_.size(), kNumPairs);
  EXPECT_TRUE(std::is_sorted(multimap_.begin(), multimap_.end(), LessThan));
}

TEST_F(IntrusiveMultiMapTest, Insert_InitializerList_WithDuplicates) {
  TestPair pair1(50, "B");
  TestPair pair2(40, "D");
  TestPair pair3(60, "F");

  multimap_.insert({&pair1, &pair2, &pair3});
  EXPECT_EQ(multimap_.size(), kNumPairs + 3);
  EXPECT_TRUE(std::is_sorted(multimap_.begin(), multimap_.end(), LessThan));

  auto iter = multimap_.find(40);
  ASSERT_NE(iter, multimap_.end());
  EXPECT_STREQ((iter++)->name(), "d");
  EXPECT_STREQ(iter->name(), "D");

  iter = multimap_.find(50);
  ASSERT_NE(iter, multimap_.end());
  EXPECT_STREQ((iter++)->name(), "b");
  EXPECT_STREQ(iter->name(), "B");

  iter = multimap_.find(60);
  ASSERT_NE(iter, multimap_.end());
  EXPECT_STREQ(iter->name(), "F");

  // Explicitly clear the multimap before pairs goes out of scope.
  multimap_.clear();
}

// A pair derived from TestPair.
struct DerivedPair : public TestPair {
  DerivedPair(size_t n, const char* name) : TestPair(n * 10, name) {}
};

TEST_F(IntrusiveMultiMapTest, Insert_DerivedPairs) {
  DerivedPair pair1(6, "f");
  multimap_.insert(pair1);

  DerivedPair pair2(7, "g");
  multimap_.insert(pair2);

  EXPECT_EQ(multimap_.size(), kNumPairs + 2);
  EXPECT_TRUE(std::is_sorted(multimap_.begin(), multimap_.end(), LessThan));

  // Explicitly clear the multimap before pairs goes out of scope.
  multimap_.clear();
}

TEST_F(IntrusiveMultiMapTest, Insert_DerivedPairs_CompilationFails) {
  ::pw::IntrusiveMultiMap<size_t, DerivedPair>
      derived_from_compatible_pair_type;

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

TEST_F(IntrusiveMultiMapTest, Erase_One_ByItem) {
  for (size_t i = 0; i < kNumPairs; ++i) {
    EXPECT_EQ(multimap_.size(), kNumPairs);
    auto iter = multimap_.erase(pairs_[i]);
    if (iter != multimap_.end()) {
      EXPECT_GE(iter->key(), pairs_[i].key());
    }
    EXPECT_EQ(multimap_.size(), kNumPairs - 1);
    multimap_.insert(pairs_[i]);
  }
}

TEST_F(IntrusiveMultiMapTest, Erase_Two_ByKey) {
  constexpr size_t kHalf = kNumPairs / 2;
  for (size_t i = 0; i < kHalf; ++i) {
    ASSERT_EQ(pairs_[i].key(), pairs_[i + kHalf].key());
    EXPECT_EQ(multimap_.size(), kNumPairs);
    EXPECT_EQ(multimap_.erase(pairs_[i].key()), 2U);
    EXPECT_EQ(multimap_.size(), kNumPairs - 2);
    auto iter = multimap_.find(pairs_[i].key());
    EXPECT_EQ(iter, multimap_.end());
    multimap_.insert(pairs_[i]);
    multimap_.insert(pairs_[i + kHalf]);
  }
}

TEST_F(IntrusiveMultiMapTest, Erase_OnlyItem) {
  multimap_.clear();
  multimap_.insert(pairs_[0]);
  EXPECT_EQ(multimap_.size(), 1U);

  EXPECT_EQ(multimap_.erase(pairs_[0].key()), 1U);
  EXPECT_EQ(multimap_.size(), 0U);
}

TEST_F(IntrusiveMultiMapTest, Erase_AllOnebyOne) {
  auto iter = multimap_.begin();
  for (size_t n = kNumPairs; n != 0; --n) {
    ASSERT_NE(iter, multimap_.end());
    iter = multimap_.erase(iter);
  }
  EXPECT_EQ(iter, multimap_.end());
  EXPECT_EQ(multimap_.size(), 0U);
}

TEST_F(IntrusiveMultiMapTest, Erase_Range) {
  auto first = multimap_.begin();
  auto last = multimap_.end();
  ++first;
  --last;
  auto iter = multimap_.erase(first, last);
  EXPECT_EQ(multimap_.size(), 2U);
  EXPECT_TRUE(std::is_sorted(multimap_.begin(), multimap_.end(), LessThan));
  EXPECT_EQ(iter->key(), 50U);
}

TEST_F(IntrusiveMultiMapTest, Erase_AllRange) {
  auto first = multimap_.begin();
  auto last = multimap_.end();
  auto iter = multimap_.erase(first, last);
  EXPECT_TRUE(multimap_.empty());
  EXPECT_EQ(iter, multimap_.end());
}

TEST_F(IntrusiveMultiMapTest, Erase_MissingItem) {
  EXPECT_EQ(multimap_.erase(100), 0U);
}

TEST_F(IntrusiveMultiMapTest, Erase_Reinsert) {
  constexpr size_t kHalf = kNumPairs / 2;
  EXPECT_EQ(multimap_.size(), pairs_.size());

  ASSERT_EQ(pairs_[0].key(), pairs_[0 + kHalf].key());
  EXPECT_EQ(multimap_.erase(pairs_[0].key()), 2U);
  EXPECT_EQ(multimap_.find(pairs_[0].key()), multimap_.end());

  ASSERT_EQ(pairs_[2].key(), pairs_[2 + kHalf].key());
  EXPECT_EQ(multimap_.erase(pairs_[2].key()), 2U);
  EXPECT_EQ(multimap_.find(pairs_[2].key()), multimap_.end());

  ASSERT_EQ(pairs_[4].key(), pairs_[4 + kHalf].key());
  EXPECT_EQ(multimap_.erase(pairs_[4].key()), 2U);
  EXPECT_EQ(multimap_.find(pairs_[4].key()), multimap_.end());

  EXPECT_EQ(multimap_.size(), pairs_.size() - 6);

  multimap_.insert(pairs_[4]);
  auto iter = multimap_.find(pairs_[4].key());
  EXPECT_NE(iter, multimap_.end());

  multimap_.insert(pairs_[0]);
  iter = multimap_.find(pairs_[0].key());
  EXPECT_NE(iter, multimap_.end());

  multimap_.insert(pairs_[2]);
  iter = multimap_.find(pairs_[2].key());
  EXPECT_NE(iter, multimap_.end());

  EXPECT_EQ(multimap_.size(), pairs_.size() - 3);
}

TEST_F(IntrusiveMultiMapTest, Erase_Duplicate) {
  TestPair pair1(32, "1");
  TestPair pair2(32, "2");
  TestPair pair3(32, "3");
  multimap_.insert(pair1);
  multimap_.insert(pair2);
  multimap_.insert(pair3);

  auto iter = multimap_.find(32);
  ASSERT_NE(iter, multimap_.end());
  EXPECT_STREQ(iter->name(), "1");

  iter = multimap_.erase(iter);
  ASSERT_NE(iter, multimap_.end());
  EXPECT_STREQ(iter->name(), "2");

  iter = multimap_.erase(iter);
  ASSERT_NE(iter, multimap_.end());
  EXPECT_STREQ(iter->name(), "3");

  multimap_.erase(iter);
  EXPECT_EQ(multimap_.find(32), multimap_.end());
}

TEST_F(IntrusiveMultiMapTest, Swap) {
  std::array<TestPair, 3> pairs = {{
      {50, "B"},
      {40, "D"},
      {60, "F"},
  }};
  IntrusiveMultiMap multimap(pairs.begin(), pairs.end());

  multimap_.swap(multimap);
  EXPECT_EQ(multimap.size(), kNumPairs);
  EXPECT_TRUE(std::is_sorted(multimap.begin(), multimap.end(), LessThan));
  auto iter = multimap.begin();
  EXPECT_STREQ((iter++)->name(), "e");
  EXPECT_STREQ((iter++)->name(), "E");
  EXPECT_STREQ((iter++)->name(), "c");
  EXPECT_STREQ((iter++)->name(), "C");
  EXPECT_STREQ((iter++)->name(), "a");
  EXPECT_STREQ((iter++)->name(), "A");
  EXPECT_STREQ((iter++)->name(), "d");
  EXPECT_STREQ((iter++)->name(), "D");
  EXPECT_STREQ((iter++)->name(), "b");
  EXPECT_STREQ((iter++)->name(), "B");
  EXPECT_EQ(iter, multimap.end());
  multimap.clear();

  EXPECT_EQ(multimap_.size(), 3U);
  EXPECT_TRUE(std::is_sorted(multimap_.begin(), multimap_.end(), LessThan));
  iter = multimap_.begin();
  EXPECT_STREQ((iter++)->name(), "D");
  EXPECT_STREQ((iter++)->name(), "B");
  EXPECT_STREQ((iter++)->name(), "F");
  EXPECT_EQ(iter, multimap_.end());

  // Explicitly clear the multimap before pairs goes out of scope.
  multimap_.clear();
}

TEST_F(IntrusiveMultiMapTest, Swap_Empty) {
  IntrusiveMultiMap multimap;

  multimap_.swap(multimap);
  EXPECT_EQ(multimap.size(), kNumPairs);
  EXPECT_TRUE(std::is_sorted(multimap.begin(), multimap.end(), LessThan));
  auto iter = multimap.begin();
  EXPECT_STREQ((iter++)->name(), "e");
  EXPECT_STREQ((iter++)->name(), "E");
  EXPECT_STREQ((iter++)->name(), "c");
  EXPECT_STREQ((iter++)->name(), "C");
  EXPECT_STREQ((iter++)->name(), "a");
  EXPECT_STREQ((iter++)->name(), "A");
  EXPECT_STREQ((iter++)->name(), "d");
  EXPECT_STREQ((iter++)->name(), "D");
  EXPECT_STREQ((iter++)->name(), "b");
  EXPECT_STREQ((iter++)->name(), "B");
  EXPECT_EQ(iter, multimap.end());
  multimap.clear();

  EXPECT_EQ(multimap_.size(), 0U);
}

TEST_F(IntrusiveMultiMapTest, Merge) {
  std::array<TestPair, 3> pairs = {{
      {5, "f"},
      {75, "g"},
      {85, "h"},
  }};
  IntrusiveMultiMap multimap(pairs.begin(), pairs.end());

  multimap_.merge(multimap);
  EXPECT_TRUE(multimap.empty());
  EXPECT_EQ(multimap_.size(), kNumPairs + 3);
  EXPECT_TRUE(std::is_sorted(multimap_.begin(), multimap_.end(), LessThan));
  auto iter = multimap_.begin();
  EXPECT_STREQ((iter++)->name(), "f");
  EXPECT_STREQ((iter++)->name(), "e");
  EXPECT_STREQ((iter++)->name(), "E");
  EXPECT_STREQ((iter++)->name(), "c");
  EXPECT_STREQ((iter++)->name(), "C");
  EXPECT_STREQ((iter++)->name(), "a");
  EXPECT_STREQ((iter++)->name(), "A");
  EXPECT_STREQ((iter++)->name(), "d");
  EXPECT_STREQ((iter++)->name(), "D");
  EXPECT_STREQ((iter++)->name(), "b");
  EXPECT_STREQ((iter++)->name(), "B");
  EXPECT_STREQ((iter++)->name(), "g");
  EXPECT_STREQ((iter++)->name(), "h");
  EXPECT_EQ(iter, multimap_.end());

  // Explicitly clear the multimap before pairs goes out of scope.
  multimap_.clear();
}

TEST_F(IntrusiveMultiMapTest, Merge_Empty) {
  IntrusiveMultiMap multimap;

  multimap_.merge(multimap);
  EXPECT_EQ(multimap_.size(), kNumPairs);
  EXPECT_TRUE(std::is_sorted(multimap_.begin(), multimap_.end(), LessThan));

  multimap.merge(multimap_);
  EXPECT_TRUE(multimap_.empty());
  EXPECT_EQ(multimap.size(), kNumPairs);
  EXPECT_TRUE(std::is_sorted(multimap.begin(), multimap.end(), LessThan));

  multimap.clear();
}

TEST_F(IntrusiveMultiMapTest, Merge_WithDuplicates) {
  std::array<TestPair, 3> pairs = {{
      {15, "f"},
      {45, "g"},
      {55, "h"},
  }};
  IntrusiveMultiMap multimap(pairs.begin(), pairs.end());

  multimap_.merge(multimap);
  EXPECT_TRUE(multimap.empty());
  EXPECT_EQ(multimap_.size(), kNumPairs + 3);
  EXPECT_TRUE(std::is_sorted(multimap_.begin(), multimap_.end(), LessThan));
  auto iter = multimap_.begin();
  EXPECT_STREQ((iter++)->name(), "e");
  EXPECT_STREQ((iter++)->name(), "E");
  EXPECT_STREQ((iter++)->name(), "f");
  EXPECT_STREQ((iter++)->name(), "c");
  EXPECT_STREQ((iter++)->name(), "C");
  EXPECT_STREQ((iter++)->name(), "a");
  EXPECT_STREQ((iter++)->name(), "A");
  EXPECT_STREQ((iter++)->name(), "d");
  EXPECT_STREQ((iter++)->name(), "D");
  EXPECT_STREQ((iter++)->name(), "g");
  EXPECT_STREQ((iter++)->name(), "b");
  EXPECT_STREQ((iter++)->name(), "B");
  EXPECT_STREQ((iter++)->name(), "h");
  EXPECT_EQ(iter, multimap_.end());

  // Explicitly clear the multimap before pairs goes out of scope.
  multimap_.clear();
}

TEST_F(IntrusiveMultiMapTest, Merge_Map) {
  std::array<TestPair, 3> pairs = {{
      {15, "f"},
      {45, "g"},
      {55, "h"},
  }};
  ::pw::IntrusiveMap<size_t, TestPair> map(pairs.begin(), pairs.end());

  multimap_.merge(map);
  EXPECT_TRUE(map.empty());
  EXPECT_EQ(multimap_.size(), kNumPairs + 3);
  EXPECT_TRUE(std::is_sorted(multimap_.begin(), multimap_.end(), LessThan));
  auto iter = multimap_.begin();
  EXPECT_STREQ((iter++)->name(), "e");
  EXPECT_STREQ((iter++)->name(), "E");
  EXPECT_STREQ((iter++)->name(), "f");
  EXPECT_STREQ((iter++)->name(), "c");
  EXPECT_STREQ((iter++)->name(), "C");
  EXPECT_STREQ((iter++)->name(), "a");
  EXPECT_STREQ((iter++)->name(), "A");
  EXPECT_STREQ((iter++)->name(), "d");
  EXPECT_STREQ((iter++)->name(), "D");
  EXPECT_STREQ((iter++)->name(), "g");
  EXPECT_STREQ((iter++)->name(), "b");
  EXPECT_STREQ((iter++)->name(), "B");
  EXPECT_STREQ((iter++)->name(), "h");
  EXPECT_EQ(iter, multimap_.end());

  // Explicitly clear the multimap before pairs goes out of scope.
  multimap_.clear();
}

TEST_F(IntrusiveMultiMapTest, Count) {
  std::array<TestPair, 3> pairs = {{
      {50, "B"},
      {40, "D"},
      {60, "F"},
  }};
  multimap_.insert(pairs.begin(), pairs.end());

  const IntrusiveMultiMap& multimap = multimap_;
  EXPECT_EQ(multimap.count(10), 2U);
  EXPECT_EQ(multimap.count(20), 2U);
  EXPECT_EQ(multimap.count(30), 2U);
  EXPECT_EQ(multimap.count(40), 3U);
  EXPECT_EQ(multimap.count(50), 3U);
  EXPECT_EQ(multimap.count(60), 1U);

  // Explicitly clear the multimap before pairs goes out of scope.
  multimap_.clear();
}

TEST_F(IntrusiveMultiMapTest, Count_NoSuchKey) {
  const IntrusiveMultiMap& multimap = multimap_;
  EXPECT_EQ(multimap.count(60), 0U);
}

TEST_F(IntrusiveMultiMapTest, Find) {
  const IntrusiveMultiMap& multimap = multimap_;
  size_t key = 10;
  for (size_t i = 0; i < kNumPairs; i += 2) {
    auto iter = multimap.find(key);
    ASSERT_NE(iter, multimap.end());
    EXPECT_EQ(iter->key(), key);
    key += 10;
  }
}

TEST_F(IntrusiveMultiMapTest, Find_NoSuchKey) {
  const IntrusiveMultiMap& multimap = multimap_;
  auto iter = multimap.find(60);
  EXPECT_EQ(iter, multimap.end());
}

TEST_F(IntrusiveMultiMapTest, Find_WithDuplicates) {
  std::array<TestPair, 3> pairs = {{
      {50, "B"},
      {40, "D"},
      {60, "F"},
  }};
  multimap_.insert(pairs.begin(), pairs.end());

  auto iter = multimap_.find(40);
  ASSERT_NE(iter, multimap_.end());
  EXPECT_EQ(iter->key(), 40U);
  EXPECT_EQ((iter++)->name(), "d");
  EXPECT_EQ(iter->key(), 40U);
  EXPECT_EQ(iter->name(), "D");

  iter = multimap_.find(50);
  ASSERT_NE(iter, multimap_.end());
  EXPECT_EQ(iter->key(), 50U);
  EXPECT_EQ((iter++)->name(), "b");
  EXPECT_EQ(iter->key(), 50U);
  EXPECT_EQ(iter->name(), "B");

  // Explicitly clear the multimap before pairs goes out of scope.
  multimap_.clear();
}

TEST_F(IntrusiveMultiMapTest, LowerBound) {
  const IntrusiveMultiMap& multimap = multimap_;
  auto iter = multimap.lower_bound(10);
  ASSERT_NE(iter, multimap.end());
  EXPECT_STREQ(iter->name(), "e");

  iter = multimap.lower_bound(20);
  ASSERT_NE(iter, multimap.end());
  EXPECT_STREQ(iter->name(), "c");

  iter = multimap.lower_bound(30);
  ASSERT_NE(iter, multimap.end());
  EXPECT_STREQ(iter->name(), "a");

  iter = multimap.lower_bound(40);
  ASSERT_NE(iter, multimap.end());
  EXPECT_STREQ(iter->name(), "d");

  iter = multimap.lower_bound(50);
  ASSERT_NE(iter, multimap.end());
  EXPECT_STREQ(iter->name(), "b");
}

TEST_F(IntrusiveMultiMapTest, LowerBound_NoExactKey) {
  const IntrusiveMultiMap& multimap = multimap_;
  auto iter = multimap.lower_bound(5);
  ASSERT_NE(iter, multimap.end());
  EXPECT_STREQ(iter->name(), "e");

  iter = multimap.lower_bound(15);
  ASSERT_NE(iter, multimap.end());
  EXPECT_STREQ(iter->name(), "c");

  iter = multimap.lower_bound(25);
  ASSERT_NE(iter, multimap.end());
  EXPECT_STREQ(iter->name(), "a");

  iter = multimap.lower_bound(35);
  ASSERT_NE(iter, multimap.end());
  EXPECT_STREQ(iter->name(), "d");

  iter = multimap.lower_bound(45);
  ASSERT_NE(iter, multimap.end());
  EXPECT_STREQ(iter->name(), "b");
}

TEST_F(IntrusiveMultiMapTest, LowerBound_OutOfRange) {
  const IntrusiveMultiMap& multimap = multimap_;
  EXPECT_EQ(multimap.lower_bound(55), multimap.end());
}

TEST_F(IntrusiveMultiMapTest, UpperBound) {
  const IntrusiveMultiMap& multimap = multimap_;
  auto iter = multimap.upper_bound(15);
  ASSERT_NE(iter, multimap.end());
  EXPECT_STREQ(iter->name(), "c");

  iter = multimap.upper_bound(25);
  ASSERT_NE(iter, multimap.end());
  EXPECT_STREQ(iter->name(), "a");

  iter = multimap.upper_bound(35);
  ASSERT_NE(iter, multimap.end());
  EXPECT_STREQ(iter->name(), "d");

  iter = multimap.upper_bound(45);
  ASSERT_NE(iter, multimap.end());
  EXPECT_STREQ(iter->name(), "b");

  EXPECT_EQ(multimap.upper_bound(55), multimap.end());
}

TEST_F(IntrusiveMultiMapTest, UpperBound_NoExactKey) {
  const IntrusiveMultiMap& multimap = multimap_;
  auto iter = multimap.upper_bound(5);
  ASSERT_NE(iter, multimap.end());
  EXPECT_STREQ(iter->name(), "e");

  iter = multimap.upper_bound(15);
  ASSERT_NE(iter, multimap.end());
  EXPECT_STREQ(iter->name(), "c");

  iter = multimap.upper_bound(25);
  ASSERT_NE(iter, multimap.end());
  EXPECT_STREQ(iter->name(), "a");

  iter = multimap.upper_bound(35);
  ASSERT_NE(iter, multimap.end());
  EXPECT_STREQ(iter->name(), "d");

  iter = multimap.upper_bound(45);
  ASSERT_NE(iter, multimap.end());
  EXPECT_STREQ(iter->name(), "b");
}

TEST_F(IntrusiveMultiMapTest, UpperBound_OutOfRange) {
  const IntrusiveMultiMap& multimap = multimap_;
  EXPECT_EQ(multimap.upper_bound(55), multimap.end());
}

TEST_F(IntrusiveMultiMapTest, EqualRange) {
  const IntrusiveMultiMap& multimap = multimap_;

  auto pair = multimap.equal_range(10);
  IntrusiveMultiMap::const_iterator lower = pair.first;
  IntrusiveMultiMap::const_iterator upper = pair.second;
  ASSERT_NE(lower, multimap.end());
  EXPECT_STREQ(lower->name(), "e");
  ASSERT_NE(upper, multimap.end());
  EXPECT_STREQ(upper->name(), "c");
  EXPECT_EQ(std::distance(lower, upper), 2);

  std::tie(lower, upper) = multimap.equal_range(20);
  ASSERT_NE(lower, multimap.end());
  EXPECT_STREQ(lower->name(), "c");
  ASSERT_NE(upper, multimap.end());
  EXPECT_STREQ(upper->name(), "a");
  EXPECT_EQ(std::distance(lower, upper), 2);

  std::tie(lower, upper) = multimap.equal_range(30);
  ASSERT_NE(lower, multimap.end());
  EXPECT_STREQ(lower->name(), "a");
  ASSERT_NE(upper, multimap.end());
  EXPECT_STREQ(upper->name(), "d");
  EXPECT_EQ(std::distance(lower, upper), 2);

  std::tie(lower, upper) = multimap.equal_range(40);
  ASSERT_NE(lower, multimap.end());
  EXPECT_STREQ(lower->name(), "d");
  ASSERT_NE(upper, multimap.end());
  EXPECT_STREQ(upper->name(), "b");
  EXPECT_EQ(std::distance(lower, upper), 2);

  std::tie(lower, upper) = multimap.equal_range(50);
  ASSERT_NE(lower, multimap.end());
  EXPECT_STREQ(lower->name(), "b");
  EXPECT_EQ(upper, multimap.end());
  EXPECT_EQ(std::distance(lower, upper), 2);
}

TEST_F(IntrusiveMultiMapTest, EqualRange_NoExactKey) {
  const IntrusiveMultiMap& multimap = multimap_;

  auto pair = multimap.equal_range(5);
  IntrusiveMultiMap::const_iterator lower = pair.first;
  IntrusiveMultiMap::const_iterator upper = pair.second;
  ASSERT_NE(lower, multimap.end());
  EXPECT_STREQ(lower->name(), "e");
  EXPECT_EQ(lower, upper);

  std::tie(lower, upper) = multimap.equal_range(15);
  ASSERT_NE(lower, multimap.end());
  EXPECT_STREQ(lower->name(), "c");
  EXPECT_EQ(lower, upper);

  std::tie(lower, upper) = multimap.equal_range(25);
  ASSERT_NE(lower, multimap.end());
  EXPECT_STREQ(lower->name(), "a");
  EXPECT_EQ(lower, upper);

  std::tie(lower, upper) = multimap.equal_range(35);
  ASSERT_NE(lower, multimap.end());
  EXPECT_STREQ(lower->name(), "d");
  EXPECT_EQ(lower, upper);

  std::tie(lower, upper) = multimap.equal_range(45);
  ASSERT_NE(lower, multimap.end());
  EXPECT_STREQ(lower->name(), "b");
  EXPECT_EQ(lower, upper);
}

TEST_F(IntrusiveMultiMapTest, EqualRange_OutOfRange) {
  const IntrusiveMultiMap& multimap = multimap_;

  auto pair = multimap.equal_range(60);
  IntrusiveMultiMap::const_iterator lower = pair.first;
  IntrusiveMultiMap::const_iterator upper = pair.second;
  EXPECT_EQ(lower, multimap.end());
  EXPECT_EQ(upper, multimap.end());
}

}  // namespace
