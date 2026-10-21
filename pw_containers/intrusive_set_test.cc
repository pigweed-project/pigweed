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

#include "pw_containers/intrusive_set.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <tuple>
#include <utility>

#include "pw_compilation_testing/negative_compilation.h"
#include "pw_containers/intrusive_multiset.h"
#include "pw_span/span.h"
#include "pw_unit_test/framework.h"

namespace {

// Base item.
class BaseItem {
 public:
  BaseItem(size_t key, const char* name) : key_(key), name_(name) {}

  constexpr const size_t& key() const { return key_; }
  constexpr const char* name() const { return name_; }
  void set_name(const char* name) { name_ = name; }

  bool operator<(const BaseItem& rhs) const { return key() < rhs.key(); }

 private:
  size_t key_;
  const char* name_;
};

// A basic item that can be used in a set.
struct TestItem : public ::pw::IntrusiveSet<TestItem>::Item, public BaseItem {
  TestItem(size_t key, const char* name) : BaseItem(key, name) {}
};

// Test fixture.
class IntrusiveSetTest : public ::testing::Test {
 protected:
  using IntrusiveSet = ::pw::IntrusiveSet<TestItem>;
  static constexpr size_t kNumItems = 10;

  void SetUp() override { set_.insert(items_.begin(), items_.end()); }

  void TearDown() override { set_.clear(); }

  std::array<TestItem, kNumItems> items_ = {{
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

  IntrusiveSet set_;
};

// Unit tests.

TEST_F(IntrusiveSetTest, Construct_Default) {
  IntrusiveSet set;
  EXPECT_TRUE(set.empty());
  EXPECT_EQ(set.begin(), set.end());
  EXPECT_EQ(set.rbegin(), set.rend());
  EXPECT_EQ(set.size(), 0U);
  EXPECT_EQ(set.lower_bound(items_[0]), set.end());
  EXPECT_EQ(set.upper_bound(items_[0]), set.end());
}

TEST_F(IntrusiveSetTest, Construct_ObjectIterators) {
  set_.clear();
  IntrusiveSet set(items_.begin(), items_.end());
  EXPECT_FALSE(set.empty());
  EXPECT_EQ(set.size(), items_.size());
  set.clear();
}

TEST_F(IntrusiveSetTest, Construct_ObjectIterators_Empty) {
  IntrusiveSet set(items_.end(), items_.end());
  EXPECT_TRUE(set.empty());
  EXPECT_EQ(set.size(), 0U);
}

TEST_F(IntrusiveSetTest, Construct_PointerIterators) {
  std::array<TestItem*, 3> ptrs = {&items_[0], &items_[1], &items_[2]};
  set_.clear();
  IntrusiveSet set(ptrs.begin(), ptrs.end());
  EXPECT_FALSE(set.empty());
  EXPECT_EQ(set.size(), 3U);
  set.clear();
}

TEST_F(IntrusiveSetTest, Construct_PointerIterators_Empty) {
  std::array<TestItem*, 0> ptrs;
  IntrusiveSet set(ptrs.begin(), ptrs.end());
  EXPECT_TRUE(set.empty());
  EXPECT_EQ(set.size(), 0U);
  set.clear();
}

TEST_F(IntrusiveSetTest, Construct_InitializerList) {
  set_.clear();
  IntrusiveSet set({&items_[0], &items_[2], &items_[4]});
  auto iter = set.begin();
  EXPECT_EQ((iter++)->key(), 10U);
  EXPECT_EQ((iter++)->key(), 20U);
  EXPECT_EQ((iter++)->key(), 30U);
  set.clear();
}

TEST_F(IntrusiveSetTest, Construct_InitializerList_Empty) {
  IntrusiveSet set(std::initializer_list<TestItem*>{});
  EXPECT_TRUE(set.empty());
  EXPECT_EQ(set.size(), 0U);
}

TEST_F(IntrusiveSetTest, Construct_CustomCompare) {
  auto greater_than = [](const BaseItem& lhs, const BaseItem& rhs) {
    return lhs.key() > rhs.key();
  };
  set_.clear();
  IntrusiveSet set({&items_[0], &items_[2], &items_[4]},
                   std::move(greater_than));
  auto iter = set.begin();
  EXPECT_EQ((iter++)->key(), 30U);
  EXPECT_EQ((iter++)->key(), 20U);
  EXPECT_EQ((iter++)->key(), 10U);
  set.clear();
}

//  A struct that is not a set item.
struct NotAnItem : public BaseItem {
  NotAnItem(size_t key, const char* name) : BaseItem(key, name) {}
};

#if PW_NC_TEST(IncompatibleItem)
PW_NC_EXPECT("IntrusiveSet items must be derived from IntrusiveSet<T>::Item");

struct BadItem : public ::pw::IntrusiveSet<NotAnItem>::Item {
 public:
  constexpr bool operator<(const BadItem& rhs) const { return this < &rhs; }
};

[[maybe_unused]] ::pw::IntrusiveSet<BadItem> bad_set1;

#elif PW_NC_TEST(DoesNotInheritFromItem)
PW_NC_EXPECT("IntrusiveSet items must be derived from IntrusiveSet<T>::Item");

[[maybe_unused]] ::pw::IntrusiveSet<NotAnItem> bad_set2;

#endif  // PW_NC_TEST

// Iterators

TEST_F(IntrusiveSetTest, Iterator) {
  const IntrusiveSet& set = set_;
  auto iter = set.begin();
  size_t key = 10;
  for (size_t i = 0; i < kNumItems; ++i) {
    auto& item = *iter++;
    EXPECT_EQ(item.key(), key);
    key += 5;
  }
  EXPECT_EQ(key, 60U);
  EXPECT_EQ(iter, set.end());
  EXPECT_EQ(iter, set.cend());
  for (size_t i = 0; i < kNumItems; ++i) {
    key -= 5;
    EXPECT_EQ((--iter)->key(), key);
  }
  EXPECT_EQ(key, 10U);
  EXPECT_EQ(iter, set.begin());
  EXPECT_EQ(iter, set.cbegin());
}

TEST_F(IntrusiveSetTest, ReverseIterator) {
  const IntrusiveSet& set = set_;
  auto iter = set.rbegin();
  size_t key = 55;
  for (size_t i = 0; i < kNumItems; ++i) {
    auto& item = *iter++;
    EXPECT_EQ(item.key(), key);
    key -= 5;
  }
  EXPECT_EQ(key, 5U);
  EXPECT_EQ(iter, set.rend());
  EXPECT_EQ(iter, set.crend());
  for (size_t i = 0; i < kNumItems; ++i) {
    key += 5;
    EXPECT_EQ((--iter)->key(), key);
  }
  EXPECT_EQ(key, 55U);
  EXPECT_EQ(iter, set.rbegin());
  EXPECT_EQ(iter, set.crbegin());
}

TEST_F(IntrusiveSetTest, IteratorIsDefaultConstructible) {
  IntrusiveSet::iterator iter;
  EXPECT_NE(iter, set_.begin());
  EXPECT_NE(iter, set_.begin());
  EXPECT_EQ(iter, IntrusiveSet::iterator());
}

TEST_F(IntrusiveSetTest, IteratorIsCopyConstructible) {
  IntrusiveSet::iterator iter1 = set_.begin();
  IntrusiveSet::iterator iter2(iter1);
  EXPECT_EQ(iter2, set_.begin());
}

TEST_F(IntrusiveSetTest, IteratorCopyAssignable) {
  IntrusiveSet::iterator iter1 = set_.begin();
  IntrusiveSet::iterator iter2 = iter1;
  EXPECT_EQ(iter2, set_.begin());
}

TEST_F(IntrusiveSetTest, IteratorisMoveConstructible) {
  IntrusiveSet::iterator iter1 = set_.begin();
  IntrusiveSet::iterator iter2(std::move(iter1));
  EXPECT_EQ(iter2, set_.begin());
}

TEST_F(IntrusiveSetTest, IteratorMoveAssignable) {
  IntrusiveSet::iterator iter1 = set_.begin();
  IntrusiveSet::iterator iter2 = std::move(iter1);
  EXPECT_EQ(iter2, set_.begin());
}

TEST_F(IntrusiveSetTest, ConstIterator_CompareNonConst) {
  EXPECT_EQ(set_.end(), set_.cend());
}

// A set item that is distinct from TestItem
struct OtherItem : public ::pw::IntrusiveSet<OtherItem>::Item, public BaseItem {
  OtherItem(size_t key, const char* name) : BaseItem(key, name) {}
};

TEST_F(IntrusiveSetTest, ConstIterator_CompareNonConst_CompilationFails) {
  ::pw::IntrusiveSet<OtherItem> set;
#if PW_NC_TEST(CannotCompareIncompatibleIteratorsEqual)
  PW_NC_EXPECT("set_\.end\(\) == set\.end\(\)");
  static_cast<void>(set_.end() == set.end());
#elif PW_NC_TEST(CannotCompareIncompatibleIteratorsInequal)
  PW_NC_EXPECT("set_\.end\(\) != set\.end\(\)");
  static_cast<void>(set_.end() != set.end());
#endif  // PW_NC_TEST
}

#if PW_NC_TEST(CannotModifyThroughConstIterator)
PW_NC_EXPECT("function is not marked const|discards qualifiers");

TEST_F(IntrusiveSetTest, ConstIterator_Modify) {
  const IntrusiveSet& set = set_;
  auto iter = set.begin();
  iter->set_name("nope");
}

#endif  // PW_NC_TEST

// Capacity

TEST_F(IntrusiveSetTest, IsEmpty) {
  const IntrusiveSet& set = set_;
  EXPECT_FALSE(set.empty());
  set_.clear();
  EXPECT_TRUE(set.empty());
}

TEST_F(IntrusiveSetTest, GetSize) {
  const IntrusiveSet& set = set_;
  EXPECT_EQ(set.size(), kNumItems);
  set_.clear();
  EXPECT_EQ(set.size(), 0U);
}

TEST_F(IntrusiveSetTest, GetMaxSize) {
  const IntrusiveSet& set = set_;
  EXPECT_EQ(set.max_size(), size_t(std::numeric_limits<ptrdiff_t>::max()));
}

// Modifiers

TEST_F(IntrusiveSetTest, Insert) {
  set_.clear();
  bool sorted = true;
  size_t prev_key = 0;
  for (auto& item : items_) {
    sorted &= prev_key < item.key();

    // Use the "hinted" version of insert.
    set_.insert(set_.end(), item);
    prev_key = item.key();
  }
  EXPECT_FALSE(sorted);

  EXPECT_EQ(set_.size(), kNumItems);
  EXPECT_TRUE(std::is_sorted(set_.begin(), set_.end()));
}

TEST_F(IntrusiveSetTest, Insert_Duplicate) {
  TestItem item1(60, "1");
  TestItem item2(60, "2");

  auto result = set_.insert(item1);
  EXPECT_STREQ(result.first->name(), "1");
  EXPECT_TRUE(result.second);

  result = set_.insert(item2);
  EXPECT_STREQ(result.first->name(), "1");
  EXPECT_FALSE(result.second);

  EXPECT_EQ(set_.size(), kNumItems + 1);
  EXPECT_TRUE(std::is_sorted(set_.begin(), set_.end()));

  // Explicitly clear the set before item 1 goes out of scope.
  set_.clear();
}

TEST_F(IntrusiveSetTest, Insert_ObjectIterators) {
  set_.clear();
  set_.insert(items_.begin(), items_.end());
  EXPECT_EQ(set_.size(), kNumItems);
  EXPECT_TRUE(std::is_sorted(set_.begin(), set_.end()));
}

TEST_F(IntrusiveSetTest, Insert_ObjectIterators_Empty) {
  set_.insert(items_.end(), items_.end());
  EXPECT_EQ(set_.size(), kNumItems);
  EXPECT_TRUE(std::is_sorted(set_.begin(), set_.end()));
}

TEST_F(IntrusiveSetTest, Insert_ObjectIterators_WithDuplicates) {
  std::array<TestItem, 3> items = {{
      {50, "B"},
      {40, "D"},
      {60, "F"},
  }};

  set_.insert(items.begin(), items.end());
  EXPECT_EQ(set_.size(), kNumItems + 1);
  EXPECT_TRUE(std::is_sorted(set_.begin(), set_.end()));

  auto iter = set_.find(items[0]);
  ASSERT_NE(iter, set_.end());
  EXPECT_STRNE(iter->name(), "B");

  iter = set_.find(items[1]);
  ASSERT_NE(iter, set_.end());
  EXPECT_STRNE(iter->name(), "D");

  iter = set_.find(items[2]);
  ASSERT_NE(iter, set_.end());
  EXPECT_STREQ(iter->name(), "F");

  // Explicitly clear the set before items goes out of scope.
  set_.clear();
}

TEST_F(IntrusiveSetTest, Insert_PointerIterators) {
  set_.clear();
  std::array<TestItem*, 3> ptrs = {&items_[0], &items_[1], &items_[2]};

  set_.insert(ptrs.begin(), ptrs.end());
  EXPECT_EQ(set_.size(), 3U);
  EXPECT_TRUE(std::is_sorted(set_.begin(), set_.end()));
}

TEST_F(IntrusiveSetTest, Insert_PointerIterators_Empty) {
  std::array<TestItem*, 0> ptrs;

  set_.insert(ptrs.begin(), ptrs.end());
  EXPECT_EQ(set_.size(), kNumItems);
  EXPECT_TRUE(std::is_sorted(set_.begin(), set_.end()));
}

TEST_F(IntrusiveSetTest, Insert_PointerIterators_WithDuplicates) {
  TestItem item1(50, "B");
  TestItem item2(40, "D");
  TestItem item3(60, "F");
  std::array<TestItem*, 3> ptrs = {&item1, &item2, &item3};

  set_.insert(ptrs.begin(), ptrs.end());
  EXPECT_EQ(set_.size(), kNumItems + 1);
  EXPECT_TRUE(std::is_sorted(set_.begin(), set_.end()));

  auto iter = set_.find(item1);
  ASSERT_NE(iter, set_.end());
  EXPECT_STRNE(iter->name(), "B");

  iter = set_.find(item2);
  ASSERT_NE(iter, set_.end());
  EXPECT_STRNE(iter->name(), "D");

  iter = set_.find(item3);
  ASSERT_NE(iter, set_.end());
  EXPECT_STREQ(iter->name(), "F");

  // Explicitly clear the set before items goes out of scope.
  set_.clear();
}

TEST_F(IntrusiveSetTest, Insert_InitializerList) {
  set_.clear();
  set_.insert({&items_[0], &items_[2], &items_[4]});
  EXPECT_EQ(set_.size(), 3U);
  EXPECT_TRUE(std::is_sorted(set_.begin(), set_.end()));
}

TEST_F(IntrusiveSetTest, Insert_InitializerList_Empty) {
  set_.insert({});
  EXPECT_EQ(set_.size(), kNumItems);
  EXPECT_TRUE(std::is_sorted(set_.begin(), set_.end()));
}

TEST_F(IntrusiveSetTest, Insert_InitializerList_WithDuplicates) {
  TestItem item1(50, "B");
  TestItem item2(40, "D");
  TestItem item3(60, "F");

  set_.insert({&item1, &item2, &item3});
  EXPECT_EQ(set_.size(), kNumItems + 1);
  EXPECT_TRUE(std::is_sorted(set_.begin(), set_.end()));

  auto iter = set_.find(item1);
  ASSERT_NE(iter, set_.end());
  EXPECT_STRNE(iter->name(), "B");

  iter = set_.find(item2);
  ASSERT_NE(iter, set_.end());
  EXPECT_STRNE(iter->name(), "D");

  iter = set_.find(item3);
  ASSERT_NE(iter, set_.end());
  EXPECT_STREQ(iter->name(), "F");

  // Explicitly clear the set before items goes out of scope.
  set_.clear();
}

// An item derived from TestItem.
struct DerivedItem : public TestItem {
  DerivedItem(size_t n, const char* name) : TestItem(n * 10, name) {}
};

TEST_F(IntrusiveSetTest, Insert_DerivedItems) {
  DerivedItem item1(6, "f");
  set_.insert(item1);

  DerivedItem item2(7, "g");
  set_.insert(item2);

  EXPECT_EQ(set_.size(), kNumItems + 2);
  EXPECT_TRUE(std::is_sorted(set_.begin(), set_.end()));

  // Explicitly clear the set before items goes out of scope.
  set_.clear();
}

TEST_F(IntrusiveSetTest, Insert_DerivedItems_CompilationFails) {
  ::pw::IntrusiveSet<DerivedItem> derived_from_compatible_item_type;

  DerivedItem item1(6, "f");
  derived_from_compatible_item_type.insert(item1);

  EXPECT_EQ(derived_from_compatible_item_type.size(), 1U);

#if PW_NC_TEST(CannotAddBaseClassToDerivedClassSet)
  PW_NC_EXPECT("derived_from_compatible_item_type\.insert\(item2\)");

  TestItem item2(70, "g");
  derived_from_compatible_item_type.insert(item2);
#endif
  derived_from_compatible_item_type.clear();
}

TEST_F(IntrusiveSetTest, Erase_OneItem) {
  for (size_t i = 0; i < kNumItems; ++i) {
    EXPECT_EQ(set_.size(), kNumItems);
    EXPECT_EQ(set_.erase(items_[i]), 1U);
    EXPECT_EQ(set_.size(), kNumItems - 1);
    auto iter = set_.find(items_[i]);
    EXPECT_EQ(iter, set_.end());
    set_.insert(items_[i]);
  }
}

TEST_F(IntrusiveSetTest, Erase_OnlyItem) {
  set_.clear();
  set_.insert(items_[0]);
  EXPECT_EQ(set_.size(), 1U);

  EXPECT_EQ(set_.erase(items_[0]), 1U);
  EXPECT_EQ(set_.size(), 0U);
}

TEST_F(IntrusiveSetTest, Erase_AllOnebyOne) {
  auto iter = set_.begin();
  for (size_t n = kNumItems; n != 0; --n) {
    ASSERT_NE(iter, set_.end());
    iter = set_.erase(iter);
  }
  EXPECT_EQ(iter, set_.end());
  EXPECT_EQ(set_.size(), 0U);
}

TEST_F(IntrusiveSetTest, Erase_Range) {
  auto first = set_.begin();
  auto last = set_.end();
  ++first;
  --last;
  auto iter = set_.erase(first, last);
  EXPECT_EQ(set_.size(), 2U);
  EXPECT_TRUE(std::is_sorted(set_.begin(), set_.end()));
  EXPECT_EQ(iter->key(), 55U);
}

TEST_F(IntrusiveSetTest, Erase_AllRange) {
  auto first = set_.begin();
  auto last = set_.end();
  auto iter = set_.erase(first, last);
  EXPECT_TRUE(set_.empty());
  EXPECT_EQ(iter, set_.end());
}

TEST_F(IntrusiveSetTest, Erase_MissingItem) {
  TestItem item(60, "F");
  EXPECT_EQ(set_.erase(item), 0U);
}

TEST_F(IntrusiveSetTest, Erase_Reinsert) {
  EXPECT_EQ(set_.size(), items_.size());

  EXPECT_EQ(set_.erase(items_[0]), 1U);
  EXPECT_EQ(set_.find(items_[0]), set_.end());

  EXPECT_EQ(set_.erase(items_[2]), 1U);
  EXPECT_EQ(set_.find(items_[2]), set_.end());

  EXPECT_EQ(set_.erase(items_[4]), 1U);
  EXPECT_EQ(set_.find(items_[4]), set_.end());

  EXPECT_EQ(set_.size(), items_.size() - 3);

  set_.insert(items_[4]);
  auto iter = set_.find(items_[4]);
  EXPECT_NE(iter, set_.end());

  set_.insert(items_[0]);
  iter = set_.find(items_[0]);
  EXPECT_NE(iter, set_.end());

  set_.insert(items_[2]);
  iter = set_.find(items_[2]);
  EXPECT_NE(iter, set_.end());

  EXPECT_EQ(set_.size(), items_.size());
}

TEST_F(IntrusiveSetTest, Swap) {
  std::array<TestItem, 3> items = {{
      {50, "B"},
      {40, "D"},
      {60, "F"},
  }};
  IntrusiveSet set(items.begin(), items.end());

  set_.swap(set);
  EXPECT_EQ(set.size(), kNumItems);
  EXPECT_TRUE(std::is_sorted(set.begin(), set.end()));

  auto iter = set.begin();
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
  EXPECT_EQ(iter, set.end());
  set.clear();

  EXPECT_EQ(set_.size(), 3U);
  EXPECT_TRUE(std::is_sorted(set_.begin(), set_.end()));
  iter = set_.begin();
  EXPECT_STREQ((iter++)->name(), "D");
  EXPECT_STREQ((iter++)->name(), "B");
  EXPECT_STREQ((iter++)->name(), "F");
  EXPECT_EQ(iter, set_.end());

  // Explicitly clear the set before items goes out of scope.
  set_.clear();
}

TEST_F(IntrusiveSetTest, Swap_Empty) {
  IntrusiveSet set;

  set_.swap(set);
  EXPECT_EQ(set.size(), kNumItems);
  EXPECT_TRUE(std::is_sorted(set.begin(), set.end()));

  auto iter = set.begin();
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
  EXPECT_EQ(iter, set.end());
  set.clear();

  EXPECT_EQ(set_.size(), 0U);
}

TEST_F(IntrusiveSetTest, Merge) {
  std::array<TestItem, 3> items = {{
      {5, "f"},
      {75, "g"},
      {85, "h"},
  }};
  IntrusiveSet set(items.begin(), items.end());

  set_.merge(set);
  EXPECT_TRUE(set.empty());
  EXPECT_EQ(set_.size(), kNumItems + 3);
  EXPECT_TRUE(std::is_sorted(set_.begin(), set_.end()));

  auto iter = set_.begin();
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
  EXPECT_EQ(iter, set_.end());

  // Explicitly clear the set before items goes out of scope.
  set_.clear();
}

TEST_F(IntrusiveSetTest, Merge_Empty) {
  IntrusiveSet set;

  set_.merge(set);
  EXPECT_EQ(set_.size(), kNumItems);
  EXPECT_TRUE(std::is_sorted(set_.begin(), set_.end()));

  set.merge(set_);
  EXPECT_TRUE(set_.empty());
  EXPECT_EQ(set.size(), kNumItems);
  EXPECT_TRUE(std::is_sorted(set.begin(), set.end()));

  set.clear();
}

TEST_F(IntrusiveSetTest, Merge_WithDuplicates) {
  std::array<TestItem, 3> items = {{
      {50, "B"},
      {40, "D"},
      {60, "F"},
  }};
  IntrusiveSet set(items.begin(), items.end());

  set_.merge(set);
  EXPECT_TRUE(set.empty());
  EXPECT_EQ(set_.size(), kNumItems + 1);
  EXPECT_TRUE(std::is_sorted(set_.begin(), set_.end()));

  auto iter = set_.begin();
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
  EXPECT_STREQ((iter++)->name(), "F");
  EXPECT_EQ(iter, set_.end());

  // Explicitly clear the set before items goes out of scope.
  set_.clear();
}

TEST_F(IntrusiveSetTest, Merge_MultiSet) {
  std::array<TestItem, 3> items = {{
      {50, "B"},
      {40, "D"},
      {60, "F"},
  }};
  ::pw::IntrusiveMultiSet<TestItem> multiset(items.begin(), items.end());

  set_.merge(multiset);
  EXPECT_TRUE(multiset.empty());
  EXPECT_EQ(set_.size(), kNumItems + 1);
  EXPECT_TRUE(std::is_sorted(set_.begin(), set_.end()));

  auto iter = set_.begin();
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
  EXPECT_STREQ((iter++)->name(), "F");
  EXPECT_EQ(iter, set_.end());

  // Explicitly clear the set before items goes out of scope.
  set_.clear();
}

TEST_F(IntrusiveSetTest, Count) {
  const IntrusiveSet& set = set_;
  for (const auto& item : items_) {
    EXPECT_EQ(set.count(item), 1U);
  }
}

TEST_F(IntrusiveSetTest, Count_NoSuchKey) {
  const IntrusiveSet& set = set_;
  TestItem item(60, "F");
  EXPECT_EQ(set.count(item), 0U);
}

TEST_F(IntrusiveSetTest, Find) {
  const IntrusiveSet& set = set_;
  for (const auto& item : items_) {
    auto iter = set.find(item);
    ASSERT_NE(iter, set.end());
    EXPECT_EQ(iter->key(), item.key());
  }
}

TEST_F(IntrusiveSetTest, Find_NoSuchKey) {
  const IntrusiveSet& set = set_;
  TestItem item(60, "F");
  auto iter = set.find(item);
  EXPECT_EQ(iter, set.end());
}

TEST_F(IntrusiveSetTest, LowerBound) {
  const IntrusiveSet& set = set_;
  auto iter = set.lower_bound({10, "?"});
  ASSERT_NE(iter, set.end());
  EXPECT_STREQ(iter->name(), "e");

  iter = set.lower_bound({20, "?"});
  ASSERT_NE(iter, set.end());
  EXPECT_STREQ(iter->name(), "c");

  iter = set.lower_bound({30, "?"});
  ASSERT_NE(iter, set.end());
  EXPECT_STREQ(iter->name(), "a");

  iter = set.lower_bound({40, "?"});
  ASSERT_NE(iter, set.end());
  EXPECT_STREQ(iter->name(), "d");

  iter = set.lower_bound({50, "?"});
  ASSERT_NE(iter, set.end());
  EXPECT_STREQ(iter->name(), "b");
}

TEST_F(IntrusiveSetTest, LowerBound_NoExactKey) {
  const IntrusiveSet& set = set_;
  auto iter = set.lower_bound({6, "?"});
  ASSERT_NE(iter, set.end());
  EXPECT_STREQ(iter->name(), "e");

  iter = set.lower_bound({16, "?"});
  ASSERT_NE(iter, set.end());
  EXPECT_STREQ(iter->name(), "c");

  iter = set.lower_bound({26, "?"});
  ASSERT_NE(iter, set.end());
  EXPECT_STREQ(iter->name(), "a");

  iter = set.lower_bound({36, "?"});
  ASSERT_NE(iter, set.end());
  EXPECT_STREQ(iter->name(), "d");

  iter = set.lower_bound({46, "?"});
  ASSERT_NE(iter, set.end());
  EXPECT_STREQ(iter->name(), "b");
}

TEST_F(IntrusiveSetTest, LowerBound_OutOfRange) {
  const IntrusiveSet& set = set_;
  EXPECT_EQ(set.lower_bound({56, "?"}), set.end());
}

TEST_F(IntrusiveSetTest, UpperBound) {
  const IntrusiveSet& set = set_;
  auto iter = set.upper_bound({15, "?"});
  ASSERT_NE(iter, set.end());
  EXPECT_STREQ(iter->name(), "c");

  iter = set.upper_bound({25, "?"});
  ASSERT_NE(iter, set.end());
  EXPECT_STREQ(iter->name(), "a");

  iter = set.upper_bound({35, "?"});
  ASSERT_NE(iter, set.end());
  EXPECT_STREQ(iter->name(), "d");

  iter = set.upper_bound({45, "?"});
  ASSERT_NE(iter, set.end());
  EXPECT_STREQ(iter->name(), "b");

  EXPECT_EQ(set.upper_bound({55, "?"}), set.end());
}

TEST_F(IntrusiveSetTest, UpperBound_NoExactKey) {
  const IntrusiveSet& set = set_;
  auto iter = set.upper_bound({6, "?"});
  ASSERT_NE(iter, set.end());
  EXPECT_STREQ(iter->name(), "e");

  iter = set.upper_bound({16, "?"});
  ASSERT_NE(iter, set.end());
  EXPECT_STREQ(iter->name(), "c");

  iter = set.upper_bound({26, "?"});
  ASSERT_NE(iter, set.end());
  EXPECT_STREQ(iter->name(), "a");

  iter = set.upper_bound({36, "?"});
  ASSERT_NE(iter, set.end());
  EXPECT_STREQ(iter->name(), "d");

  iter = set.upper_bound({46, "?"});
  ASSERT_NE(iter, set.end());
  EXPECT_STREQ(iter->name(), "b");
}

TEST_F(IntrusiveSetTest, UpperBound_OutOfRange) {
  const IntrusiveSet& set = set_;
  EXPECT_EQ(set.upper_bound({56, "?"}), set.end());
}

TEST_F(IntrusiveSetTest, EqualRange) {
  const IntrusiveSet& set = set_;

  auto pair = set.equal_range({10, "?"});
  IntrusiveSet::const_iterator lower = pair.first;
  IntrusiveSet::const_iterator upper = pair.second;
  ASSERT_NE(lower, set.end());
  EXPECT_STREQ(lower->name(), "e");
  ASSERT_NE(upper, set.end());
  EXPECT_STREQ(upper->name(), "E");

  std::tie(lower, upper) = set.equal_range({20, "?"});
  ASSERT_NE(lower, set.end());
  EXPECT_STREQ(lower->name(), "c");
  ASSERT_NE(upper, set.end());
  EXPECT_STREQ(upper->name(), "C");

  std::tie(lower, upper) = set.equal_range({30, "?"});
  ASSERT_NE(lower, set.end());
  EXPECT_STREQ(lower->name(), "a");
  ASSERT_NE(upper, set.end());
  EXPECT_STREQ(upper->name(), "A");

  std::tie(lower, upper) = set.equal_range({40, "?"});
  ASSERT_NE(lower, set.end());
  EXPECT_STREQ(lower->name(), "d");
  ASSERT_NE(upper, set.end());
  EXPECT_STREQ(upper->name(), "D");

  std::tie(lower, upper) = set.equal_range({50, "?"});
  ASSERT_NE(lower, set.end());
  EXPECT_STREQ(lower->name(), "b");
  ASSERT_NE(upper, set.end());
  EXPECT_STREQ(upper->name(), "B");
}

TEST_F(IntrusiveSetTest, EqualRange_NoExactKey) {
  const IntrusiveSet& set = set_;

  auto pair = set.equal_range({6, "?"});
  IntrusiveSet::const_iterator lower = pair.first;
  IntrusiveSet::const_iterator upper = pair.second;
  ASSERT_NE(lower, set.end());
  EXPECT_STREQ(lower->name(), "e");
  ASSERT_NE(upper, set.end());
  EXPECT_STREQ(upper->name(), "e");

  std::tie(lower, upper) = set.equal_range({16, "?"});
  ASSERT_NE(lower, set.end());
  EXPECT_STREQ(lower->name(), "c");
  ASSERT_NE(upper, set.end());
  EXPECT_STREQ(upper->name(), "c");

  std::tie(lower, upper) = set.equal_range({26, "?"});
  ASSERT_NE(lower, set.end());
  EXPECT_STREQ(lower->name(), "a");
  ASSERT_NE(upper, set.end());
  EXPECT_STREQ(upper->name(), "a");

  std::tie(lower, upper) = set.equal_range({36, "?"});
  ASSERT_NE(lower, set.end());
  EXPECT_STREQ(lower->name(), "d");
  ASSERT_NE(upper, set.end());
  EXPECT_STREQ(upper->name(), "d");

  std::tie(lower, upper) = set.equal_range({46, "?"});
  ASSERT_NE(lower, set.end());
  EXPECT_STREQ(lower->name(), "b");
  ASSERT_NE(upper, set.end());
  EXPECT_STREQ(upper->name(), "b");
}

TEST_F(IntrusiveSetTest, EqualRange_OutOfRange) {
  const IntrusiveSet& set = set_;

  auto pair = set.equal_range({56, "?"});
  IntrusiveSet::const_iterator lower = pair.first;
  IntrusiveSet::const_iterator upper = pair.second;
  EXPECT_EQ(lower, set.end());
  EXPECT_EQ(upper, set.end());
}

}  // namespace
