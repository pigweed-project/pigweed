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

#include "pw_containers/intrusive_multiset.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <limits>
#include <tuple>
#include <utility>

#include "pw_compilation_testing/negative_compilation.h"
#include "pw_containers/intrusive_set.h"
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

  constexpr bool operator<(const BaseItem& rhs) const {
    return key() < rhs.key();
  }

 private:
  size_t key_;
  const char* name_;
};

// A basic item that can be used in a set.
struct TestItem : public ::pw::IntrusiveMultiSet<TestItem>::Item,
                  public BaseItem {
  TestItem(size_t key, const char* name) : BaseItem(key, name) {}
};

// Test fixture.
class IntrusiveMultiSetTest : public ::testing::Test {
 protected:
  using IntrusiveMultiSet = ::pw::IntrusiveMultiSet<TestItem>;
  static constexpr size_t kNumItems = 10;

  void SetUp() override { multiset_.insert(items_.begin(), items_.end()); }

  void TearDown() override { multiset_.clear(); }

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

  IntrusiveMultiSet multiset_;
};

// Unit tests.

TEST_F(IntrusiveMultiSetTest, Construct_Default) {
  IntrusiveMultiSet multiset;
  EXPECT_TRUE(multiset.empty());
  EXPECT_EQ(multiset.begin(), multiset.end());
  EXPECT_EQ(multiset.rbegin(), multiset.rend());
  EXPECT_EQ(multiset.size(), 0U);
  EXPECT_EQ(multiset.lower_bound({0, "."}), multiset.end());
  EXPECT_EQ(multiset.upper_bound({0, "."}), multiset.end());
}

TEST_F(IntrusiveMultiSetTest, Construct_ObjectIterators) {
  multiset_.clear();
  IntrusiveMultiSet multiset(items_.begin(), items_.end());
  EXPECT_FALSE(multiset.empty());
  EXPECT_EQ(multiset.size(), items_.size());
  multiset.clear();
}

TEST_F(IntrusiveMultiSetTest, Construct_ObjectIterators_Empty) {
  IntrusiveMultiSet multiset(items_.end(), items_.end());
  EXPECT_TRUE(multiset.empty());
  EXPECT_EQ(multiset.size(), 0U);
}

TEST_F(IntrusiveMultiSetTest, Construct_PointerIterators) {
  std::array<TestItem*, 3> ptrs = {&items_[0], &items_[1], &items_[2]};
  multiset_.clear();
  IntrusiveMultiSet multiset(ptrs.begin(), ptrs.end());
  EXPECT_FALSE(multiset.empty());
  EXPECT_EQ(multiset.size(), 3U);
  multiset.clear();
}

TEST_F(IntrusiveMultiSetTest, Construct_PointerIterators_Empty) {
  std::array<TestItem*, 0> ptrs;
  IntrusiveMultiSet multiset(ptrs.begin(), ptrs.end());
  EXPECT_TRUE(multiset.empty());
  EXPECT_EQ(multiset.size(), 0U);
  multiset.clear();
}

TEST_F(IntrusiveMultiSetTest, Construct_InitializerList) {
  multiset_.clear();
  IntrusiveMultiSet multiset({&items_[0], &items_[2], &items_[4]});
  auto iter = multiset.begin();
  EXPECT_EQ((iter++)->key(), 10U);
  EXPECT_EQ((iter++)->key(), 20U);
  EXPECT_EQ((iter++)->key(), 30U);
  EXPECT_EQ(iter, multiset.end());
  multiset.clear();
}

TEST_F(IntrusiveMultiSetTest, Construct_InitializerList_Empty) {
  IntrusiveMultiSet multiset({});
  EXPECT_TRUE(multiset.empty());
  EXPECT_EQ(multiset.size(), 0U);
}

TEST_F(IntrusiveMultiSetTest, Construct_CustomCompare) {
  auto greater_than = [](const BaseItem& lhs, const BaseItem& rhs) {
    return lhs.key() > rhs.key();
  };
  multiset_.clear();
  IntrusiveMultiSet multiset({&items_[0], &items_[2], &items_[4]},
                             std::move(greater_than));
  auto iter = multiset.begin();
  EXPECT_EQ((iter++)->key(), 30U);
  EXPECT_EQ((iter++)->key(), 20U);
  EXPECT_EQ((iter++)->key(), 10U);
  EXPECT_EQ(iter, multiset.end());
  multiset.clear();
}

//  A struct that is not a multiset item.
struct NotAnItem : public BaseItem {
  NotAnItem(size_t key, const char* name) : BaseItem(key, name) {}
};

#if PW_NC_TEST(IncompatibleItem)
PW_NC_EXPECT(
    "IntrusiveMultiSet items must be derived from IntrusiveMultiSet<T>::Item");

class BadItem : public ::pw::IntrusiveMultiSet<NotAnItem>::Item {
 public:
  constexpr bool operator<(const BadItem& rhs) const { return this < &rhs; }
};

[[maybe_unused]] ::pw::IntrusiveMultiSet<BadItem> bad_multiset1;

#elif PW_NC_TEST(DoesNotInheritFromItem)
PW_NC_EXPECT(
    "IntrusiveMultiSet items must be derived from IntrusiveMultiSet<T>::Item");

[[maybe_unused]] ::pw::IntrusiveMultiSet<NotAnItem> bad_multiset2;

#endif  // PW_NC_TEST

// Iterators

TEST_F(IntrusiveMultiSetTest, Iterator) {
  const IntrusiveMultiSet& multiset = multiset_;
  auto iter = multiset.begin();
  size_t key = 10;
  for (size_t i = 0; i < kNumItems; ++i) {
    auto& item = *iter++;
    EXPECT_EQ(item.key(), key);
    key += 5;
  }
  EXPECT_EQ(key, 60U);
  EXPECT_EQ(iter, multiset.end());
  EXPECT_EQ(iter, multiset.cend());
  for (size_t i = 0; i < kNumItems; ++i) {
    key -= 5;
    EXPECT_EQ((--iter)->key(), key);
  }
  EXPECT_EQ(key, 10U);
  EXPECT_EQ(iter, multiset.begin());
  EXPECT_EQ(iter, multiset.cbegin());
}

TEST_F(IntrusiveMultiSetTest, ReverseIterator) {
  const IntrusiveMultiSet& multiset = multiset_;
  auto iter = multiset.rbegin();
  size_t key = 55;
  for (size_t i = 0; i < kNumItems; ++i) {
    auto& item = *iter++;
    EXPECT_EQ(item.key(), key);
    key -= 5;
  }
  EXPECT_EQ(key, 5U);
  EXPECT_EQ(iter, multiset.rend());
  EXPECT_EQ(iter, multiset.crend());
  for (size_t i = 0; i < kNumItems; ++i) {
    key += 5;
    EXPECT_EQ((--iter)->key(), key);
  }
  EXPECT_EQ(key, 55U);
  EXPECT_EQ(iter, multiset.rbegin());
  EXPECT_EQ(iter, multiset.crbegin());
}

TEST_F(IntrusiveMultiSetTest, IteratorIsDefaultConstructible) {
  IntrusiveMultiSet::iterator iter;
  EXPECT_NE(iter, multiset_.begin());
  EXPECT_NE(iter, multiset_.begin());
  EXPECT_EQ(iter, IntrusiveMultiSet::iterator());
}

TEST_F(IntrusiveMultiSetTest, IteratorIsCopyConstructible) {
  IntrusiveMultiSet::iterator iter1 = multiset_.begin();
  IntrusiveMultiSet::iterator iter2(iter1);
  EXPECT_EQ(iter2, multiset_.begin());
}

TEST_F(IntrusiveMultiSetTest, IteratorCopyAssignable) {
  IntrusiveMultiSet::iterator iter1 = multiset_.begin();
  IntrusiveMultiSet::iterator iter2 = iter1;
  EXPECT_EQ(iter2, multiset_.begin());
}

TEST_F(IntrusiveMultiSetTest, IteratorisMoveConstructible) {
  IntrusiveMultiSet::iterator iter1 = multiset_.begin();
  IntrusiveMultiSet::iterator iter2(std::move(iter1));
  EXPECT_EQ(iter2, multiset_.begin());
}

TEST_F(IntrusiveMultiSetTest, IteratorMoveAssignable) {
  IntrusiveMultiSet::iterator iter1 = multiset_.begin();
  IntrusiveMultiSet::iterator iter2 = std::move(iter1);
  EXPECT_EQ(iter2, multiset_.begin());
}

TEST_F(IntrusiveMultiSetTest, ConstIterator_CompareNonConst) {
  EXPECT_EQ(multiset_.end(), multiset_.cend());
}

// A multiset item that is distinct from TestItem
struct OtherItem : public ::pw::IntrusiveMultiSet<OtherItem>::Item,
                   public BaseItem {
  OtherItem(size_t key, const char* name) : BaseItem(key, name) {}
};

TEST_F(IntrusiveMultiSetTest, ConstIterator_CompareNonConst_CompilationFails) {
  ::pw::IntrusiveMultiSet<OtherItem> multiset;
#if PW_NC_TEST(CannotCompareIncompatibleIteratorsEqual)
  PW_NC_EXPECT("multiset_\.end\(\) == multiset\.end\(\)");
  static_cast<void>(multiset_.end() == multiset.end());
#elif PW_NC_TEST(CannotCompareIncompatibleIteratorsInequal)
  PW_NC_EXPECT("multiset_\.end\(\) != multiset\.end\(\)");
  static_cast<void>(multiset_.end() != multiset.end());
#endif  // PW_NC_TEST
}

#if PW_NC_TEST(CannotModifyThroughConstIterator)
PW_NC_EXPECT("function is not marked const|discards qualifiers");

TEST_F(IntrusiveMultiSetTest, ConstIterator_Modify) {
  const IntrusiveMultiSet& multiset = multiset_;
  auto iter = multiset.begin();
  iter->set_name("nope");
}

#endif  // PW_NC_TEST

// Capacity

TEST_F(IntrusiveMultiSetTest, IsEmpty) {
  const IntrusiveMultiSet& multiset = multiset_;
  EXPECT_FALSE(multiset.empty());
  multiset_.clear();
  EXPECT_TRUE(multiset.empty());
}

TEST_F(IntrusiveMultiSetTest, GetSize) {
  const IntrusiveMultiSet& multiset = multiset_;
  EXPECT_EQ(multiset.size(), kNumItems);
  multiset_.clear();
  EXPECT_EQ(multiset.size(), 0U);
}

TEST_F(IntrusiveMultiSetTest, GetMaxSize) {
  const IntrusiveMultiSet& multiset = multiset_;
  EXPECT_EQ(multiset.max_size(), size_t(std::numeric_limits<ptrdiff_t>::max()));
}

// Modifiers

// This functions allows tests to use `std::is_sorted` without specializing
// `std::less<TestItem>`. Since `std::less` is the default value for the
// `Compare` template parameter, leaving it untouched avoids accidentally
// masking type-handling errors.
constexpr bool LessThan(const TestItem& lhs, const TestItem& rhs) {
  return lhs < rhs;
}

TEST_F(IntrusiveMultiSetTest, Insert) {
  multiset_.clear();
  bool sorted = true;
  size_t prev_key = 0;
  for (auto& item : items_) {
    sorted &= prev_key < item.key();

    // Use the "hinted" version of insert.
    multiset_.insert(multiset_.end(), item);
    prev_key = item.key();
  }
  EXPECT_FALSE(sorted);

  EXPECT_EQ(multiset_.size(), kNumItems);
  EXPECT_TRUE(std::is_sorted(multiset_.begin(), multiset_.end(), LessThan));
}

TEST_F(IntrusiveMultiSetTest, Insert_Duplicate) {
  TestItem item1(60, "1");
  TestItem item2(60, "2");

  auto iter = multiset_.insert(item1);
  EXPECT_STREQ(iter->name(), "1");

  iter = multiset_.insert(item2);
  EXPECT_STREQ(iter->name(), "2");

  EXPECT_EQ(multiset_.size(), kNumItems + 2);
  EXPECT_TRUE(std::is_sorted(multiset_.begin(), multiset_.end(), LessThan));

  // Explicitly clear the multiset before item 1 goes out of scope.
  multiset_.clear();
}

TEST_F(IntrusiveMultiSetTest, Insert_ObjectIterators) {
  multiset_.clear();
  multiset_.insert(items_.begin(), items_.end());
  EXPECT_EQ(multiset_.size(), kNumItems);
  EXPECT_TRUE(std::is_sorted(multiset_.begin(), multiset_.end(), LessThan));
}

TEST_F(IntrusiveMultiSetTest, Insert_ObjectIterators_Empty) {
  multiset_.insert(items_.end(), items_.end());
  EXPECT_EQ(multiset_.size(), kNumItems);
  EXPECT_TRUE(std::is_sorted(multiset_.begin(), multiset_.end(), LessThan));
}

TEST_F(IntrusiveMultiSetTest, Insert_ObjectIterators_WithDuplicates) {
  std::array<TestItem, 3> items = {{
      {50, "B"},
      {40, "D"},
      {60, "F"},
  }};

  multiset_.insert(items.begin(), items.end());
  EXPECT_EQ(multiset_.size(), kNumItems + 3);
  EXPECT_TRUE(std::is_sorted(multiset_.begin(), multiset_.end(), LessThan));

  auto iter = multiset_.find(items[0]);
  ASSERT_NE(iter, multiset_.end());
  EXPECT_STREQ((iter++)->name(), "b");
  EXPECT_STREQ(iter->name(), "B");

  iter = multiset_.find(items[1]);
  ASSERT_NE(iter, multiset_.end());
  EXPECT_STREQ((iter++)->name(), "d");
  EXPECT_STREQ(iter->name(), "D");

  iter = multiset_.find(items[2]);
  ASSERT_NE(iter, multiset_.end());
  EXPECT_STREQ(iter->name(), "F");

  // Explicitly clear the multiset before items goes out of scope.
  multiset_.clear();
}

TEST_F(IntrusiveMultiSetTest, Insert_PointerIterators) {
  multiset_.clear();
  std::array<TestItem*, 3> ptrs = {&items_[0], &items_[1], &items_[2]};

  multiset_.insert(ptrs.begin(), ptrs.end());
  EXPECT_EQ(multiset_.size(), 3U);
  EXPECT_TRUE(std::is_sorted(multiset_.begin(), multiset_.end(), LessThan));
}

TEST_F(IntrusiveMultiSetTest, Insert_PointerIterators_Empty) {
  std::array<TestItem*, 0> ptrs;

  multiset_.insert(ptrs.begin(), ptrs.end());
  EXPECT_EQ(multiset_.size(), kNumItems);
  EXPECT_TRUE(std::is_sorted(multiset_.begin(), multiset_.end(), LessThan));
}

TEST_F(IntrusiveMultiSetTest, Insert_PointerIterators_WithDuplicates) {
  TestItem item1(50, "B");
  TestItem item2(40, "D");
  TestItem item3(60, "F");
  std::array<TestItem*, 3> ptrs = {&item1, &item2, &item3};

  multiset_.insert(ptrs.begin(), ptrs.end());
  EXPECT_EQ(multiset_.size(), kNumItems + 3);
  EXPECT_TRUE(std::is_sorted(multiset_.begin(), multiset_.end(), LessThan));

  auto iter = multiset_.find(item1);
  ASSERT_NE(iter, multiset_.end());
  EXPECT_STREQ((iter++)->name(), "b");
  EXPECT_STREQ(iter->name(), "B");

  iter = multiset_.find(item2);
  ASSERT_NE(iter, multiset_.end());
  EXPECT_STREQ((iter++)->name(), "d");
  EXPECT_STREQ(iter->name(), "D");

  iter = multiset_.find(item3);
  ASSERT_NE(iter, multiset_.end());
  EXPECT_STREQ(iter->name(), "F");

  // Explicitly clear the multiset before items goes out of scope.
  multiset_.clear();
}

TEST_F(IntrusiveMultiSetTest, Insert_InitializerList) {
  multiset_.clear();
  multiset_.insert({&items_[0], &items_[2], &items_[4]});
  EXPECT_EQ(multiset_.size(), 3U);
  EXPECT_TRUE(std::is_sorted(multiset_.begin(), multiset_.end(), LessThan));
}

TEST_F(IntrusiveMultiSetTest, Insert_InitializerList_Empty) {
  multiset_.insert({});
  EXPECT_EQ(multiset_.size(), kNumItems);
  EXPECT_TRUE(std::is_sorted(multiset_.begin(), multiset_.end(), LessThan));
}

TEST_F(IntrusiveMultiSetTest, Insert_InitializerList_WithDuplicates) {
  TestItem item1(50, "B");
  TestItem item2(40, "D");
  TestItem item3(60, "F");

  multiset_.insert({&item1, &item2, &item3});
  EXPECT_EQ(multiset_.size(), kNumItems + 3);
  EXPECT_TRUE(std::is_sorted(multiset_.begin(), multiset_.end(), LessThan));

  auto iter = multiset_.find(item1);
  ASSERT_NE(iter, multiset_.end());
  EXPECT_STREQ((iter++)->name(), "b");
  EXPECT_STREQ(iter->name(), "B");

  iter = multiset_.find(item2);
  ASSERT_NE(iter, multiset_.end());
  EXPECT_STREQ((iter++)->name(), "d");
  EXPECT_STREQ(iter->name(), "D");

  iter = multiset_.find(item3);
  ASSERT_NE(iter, multiset_.end());
  EXPECT_STREQ(iter->name(), "F");

  // Explicitly clear the multiset before items goes out of scope.
  multiset_.clear();
}

// An item derived from TestItem.
struct DerivedItem : public TestItem {
  DerivedItem(size_t n, const char* name) : TestItem(n * 10, name) {}
};

TEST_F(IntrusiveMultiSetTest, Insert_DerivedItems) {
  DerivedItem item1(6, "f");
  multiset_.insert(item1);

  DerivedItem item2(7, "g");
  multiset_.insert(item2);

  EXPECT_EQ(multiset_.size(), kNumItems + 2);
  EXPECT_TRUE(std::is_sorted(multiset_.begin(), multiset_.end(), LessThan));

  // Explicitly clear the multiset before items goes out of scope.
  multiset_.clear();
}

TEST_F(IntrusiveMultiSetTest, Insert_DerivedItems_CompilationFails) {
  ::pw::IntrusiveMultiSet<DerivedItem> derived_from_compatible_item_type;

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

TEST_F(IntrusiveMultiSetTest, Erase_OneItem) {
  for (size_t i = 0; i < kNumItems; ++i) {
    EXPECT_EQ(multiset_.size(), kNumItems);
    EXPECT_EQ(multiset_.erase(items_[i]), 1U);
    EXPECT_EQ(multiset_.size(), kNumItems - 1);
    auto iter = multiset_.find(items_[i]);
    EXPECT_EQ(iter, multiset_.end());
    multiset_.insert(items_[i]);
  }
}

TEST_F(IntrusiveMultiSetTest, Erase_OnlyItem) {
  multiset_.clear();
  multiset_.insert(items_[0]);
  EXPECT_EQ(multiset_.size(), 1U);

  EXPECT_EQ(multiset_.erase(items_[0]), 1U);
  EXPECT_EQ(multiset_.size(), 0U);
}

TEST_F(IntrusiveMultiSetTest, Erase_AllOnebyOne) {
  auto iter = multiset_.begin();
  for (size_t n = kNumItems; n != 0; --n) {
    ASSERT_NE(iter, multiset_.end());
    iter = multiset_.erase(iter);
  }
  EXPECT_EQ(iter, multiset_.end());
  EXPECT_EQ(multiset_.size(), 0U);
}

TEST_F(IntrusiveMultiSetTest, Erase_Range) {
  auto first = multiset_.begin();
  auto last = multiset_.end();
  ++first;
  --last;
  auto iter = multiset_.erase(first, last);
  EXPECT_EQ(multiset_.size(), 2U);
  EXPECT_TRUE(std::is_sorted(multiset_.begin(), multiset_.end(), LessThan));
  EXPECT_EQ(iter->key(), 55U);
}

TEST_F(IntrusiveMultiSetTest, Erase_AllRange) {
  auto first = multiset_.begin();
  auto last = multiset_.end();
  auto iter = multiset_.erase(first, last);
  EXPECT_TRUE(multiset_.empty());
  EXPECT_EQ(iter, multiset_.end());
}

TEST_F(IntrusiveMultiSetTest, Erase_MissingItem) {
  EXPECT_EQ(multiset_.erase({100, "-"}), 0U);
}

TEST_F(IntrusiveMultiSetTest, Erase_Reinsert) {
  EXPECT_EQ(multiset_.size(), items_.size());

  EXPECT_EQ(multiset_.erase(items_[0]), 1U);
  EXPECT_EQ(multiset_.find(items_[0]), multiset_.end());

  EXPECT_EQ(multiset_.erase(items_[2]), 1U);
  EXPECT_EQ(multiset_.find(items_[2]), multiset_.end());

  EXPECT_EQ(multiset_.erase(items_[4]), 1U);
  EXPECT_EQ(multiset_.find(items_[4]), multiset_.end());

  EXPECT_EQ(multiset_.size(), items_.size() - 3);

  multiset_.insert(items_[4]);
  auto iter = multiset_.find(items_[4]);
  EXPECT_NE(iter, multiset_.end());

  multiset_.insert(items_[0]);
  iter = multiset_.find(items_[0]);
  EXPECT_NE(iter, multiset_.end());

  multiset_.insert(items_[2]);
  iter = multiset_.find(items_[2]);
  EXPECT_NE(iter, multiset_.end());

  EXPECT_EQ(multiset_.size(), items_.size());
}

TEST_F(IntrusiveMultiSetTest, Erase_Duplicate) {
  TestItem item1(32, "1");
  TestItem item2(32, "2");
  TestItem item3(32, "3");
  multiset_.insert(item1);
  multiset_.insert(item2);
  multiset_.insert(item3);

  auto iter = multiset_.find({32, "?"});
  ASSERT_NE(iter, multiset_.end());
  EXPECT_STREQ(iter->name(), "1");

  iter = multiset_.erase(iter);
  ASSERT_NE(iter, multiset_.end());
  EXPECT_STREQ(iter->name(), "2");

  iter = multiset_.erase(iter);
  ASSERT_NE(iter, multiset_.end());
  EXPECT_STREQ(iter->name(), "3");

  multiset_.erase(iter);
  EXPECT_EQ(multiset_.find({32, "?"}), multiset_.end());
}

TEST_F(IntrusiveMultiSetTest, Swap) {
  std::array<TestItem, 3> items = {{
      {50, "B"},
      {40, "D"},
      {60, "F"},
  }};
  IntrusiveMultiSet multiset(items.begin(), items.end());

  multiset_.swap(multiset);
  EXPECT_EQ(multiset.size(), kNumItems);
  EXPECT_TRUE(std::is_sorted(multiset.begin(), multiset.end(), LessThan));
  auto iter = multiset.begin();
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
  EXPECT_EQ(iter, multiset.end());
  multiset.clear();

  EXPECT_EQ(multiset_.size(), 3U);
  EXPECT_TRUE(std::is_sorted(multiset_.begin(), multiset_.end(), LessThan));
  iter = multiset_.begin();
  EXPECT_STREQ((iter++)->name(), "D");
  EXPECT_STREQ((iter++)->name(), "B");
  EXPECT_STREQ((iter++)->name(), "F");
  EXPECT_EQ(iter, multiset_.end());

  // Explicitly clear the multiset before items goes out of scope.
  multiset_.clear();
}

TEST_F(IntrusiveMultiSetTest, Swap_Empty) {
  IntrusiveMultiSet multiset;

  multiset_.swap(multiset);
  EXPECT_EQ(multiset.size(), kNumItems);
  EXPECT_TRUE(std::is_sorted(multiset.begin(), multiset.end(), LessThan));
  auto iter = multiset.begin();
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
  EXPECT_EQ(iter, multiset.end());
  multiset.clear();

  EXPECT_EQ(multiset_.size(), 0U);
}

TEST_F(IntrusiveMultiSetTest, Merge) {
  std::array<TestItem, 3> items = {{
      {5, "f"},
      {75, "g"},
      {85, "h"},
  }};
  IntrusiveMultiSet multiset(items.begin(), items.end());

  multiset_.merge(multiset);
  EXPECT_TRUE(multiset.empty());
  EXPECT_EQ(multiset_.size(), kNumItems + 3);
  EXPECT_TRUE(std::is_sorted(multiset_.begin(), multiset_.end(), LessThan));
  auto iter = multiset_.begin();
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
  EXPECT_EQ(iter, multiset_.end());

  // Explicitly clear the multiset before items goes out of scope.
  multiset_.clear();
}

TEST_F(IntrusiveMultiSetTest, Merge_Empty) {
  IntrusiveMultiSet multiset;

  multiset_.merge(multiset);
  EXPECT_EQ(multiset_.size(), kNumItems);
  EXPECT_TRUE(std::is_sorted(multiset_.begin(), multiset_.end(), LessThan));

  multiset.merge(multiset_);
  EXPECT_TRUE(multiset_.empty());
  EXPECT_EQ(multiset.size(), kNumItems);
  EXPECT_TRUE(std::is_sorted(multiset.begin(), multiset.end(), LessThan));

  multiset.clear();
}

TEST_F(IntrusiveMultiSetTest, Merge_WithDuplicates) {
  std::array<TestItem, 3> items = {{
      {15, "f"},
      {45, "g"},
      {55, "h"},
  }};
  IntrusiveMultiSet multiset(items.begin(), items.end());

  multiset_.merge(multiset);
  EXPECT_TRUE(multiset.empty());
  EXPECT_EQ(multiset_.size(), kNumItems + 3);
  EXPECT_TRUE(std::is_sorted(multiset_.begin(), multiset_.end(), LessThan));
  auto iter = multiset_.begin();
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
  EXPECT_EQ(iter, multiset_.end());

  // Explicitly clear the multiset before items goes out of scope.
  multiset_.clear();
}

TEST_F(IntrusiveMultiSetTest, Merge_Set) {
  std::array<TestItem, 3> items = {{
      {15, "f"},
      {45, "g"},
      {55, "h"},
  }};
  ::pw::IntrusiveSet<TestItem> set(items.begin(), items.end());

  multiset_.merge(set);
  EXPECT_TRUE(set.empty());
  EXPECT_EQ(multiset_.size(), kNumItems + 3);
  EXPECT_TRUE(std::is_sorted(multiset_.begin(), multiset_.end(), LessThan));
  auto iter = multiset_.begin();
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
  EXPECT_EQ(iter, multiset_.end());

  // Explicitly clear the multiset before items goes out of scope.
  multiset_.clear();
}

TEST_F(IntrusiveMultiSetTest, Count) {
  const IntrusiveMultiSet& multiset = multiset_;
  EXPECT_EQ(multiset.count({10, "?"}), 1U);
  EXPECT_EQ(multiset.count({20, "?"}), 1U);
  EXPECT_EQ(multiset.count({30, "?"}), 1U);
  EXPECT_EQ(multiset.count({40, "?"}), 1U);
  EXPECT_EQ(multiset.count({50, "?"}), 1U);
}

TEST_F(IntrusiveMultiSetTest, Count_NoSuchKey) {
  const IntrusiveMultiSet& multiset = multiset_;
  EXPECT_EQ(multiset.count({60, "?"}), 0U);
}

TEST_F(IntrusiveMultiSetTest, Count_WithDuplicates) {
  std::array<TestItem, 3> items = {{
      {50, "B"},
      {40, "D"},
      {60, "F"},
  }};
  multiset_.insert(items.begin(), items.end());

  EXPECT_EQ(multiset_.count({10, "?"}), 1U);
  EXPECT_EQ(multiset_.count({20, "?"}), 1U);
  EXPECT_EQ(multiset_.count({30, "?"}), 1U);
  EXPECT_EQ(multiset_.count({40, "?"}), 2U);
  EXPECT_EQ(multiset_.count({50, "?"}), 2U);
  EXPECT_EQ(multiset_.count({60, "?"}), 1U);

  // Explicitly clear the multiset before items goes out of scope.
  multiset_.clear();
}

TEST_F(IntrusiveMultiSetTest, Find) {
  const IntrusiveMultiSet& multiset = multiset_;
  size_t key = 10;
  for (size_t i = 0; i < kNumItems; ++i) {
    auto iter = multiset.find({key, "?"});
    ASSERT_NE(iter, multiset.end());
    EXPECT_EQ(iter->key(), key);
    key += 5;
  }
}

TEST_F(IntrusiveMultiSetTest, Find_NoSuchKey) {
  const IntrusiveMultiSet& multiset = multiset_;
  auto iter = multiset.find({60, "?"});
  EXPECT_EQ(iter, multiset.end());
}

TEST_F(IntrusiveMultiSetTest, Find_WithDuplicates) {
  std::array<TestItem, 3> items = {{
      {50, "B"},
      {40, "D"},
      {60, "F"},
  }};
  multiset_.insert(items.begin(), items.end());

  auto iter = multiset_.find({40, "?"});
  ASSERT_NE(iter, multiset_.end());
  EXPECT_EQ(iter->key(), 40U);
  EXPECT_EQ((iter++)->name(), "d");
  EXPECT_EQ(iter->key(), 40U);
  EXPECT_EQ(iter->name(), "D");

  iter = multiset_.find({50, "?"});
  ASSERT_NE(iter, multiset_.end());
  EXPECT_EQ(iter->key(), 50U);
  EXPECT_EQ((iter++)->name(), "b");
  EXPECT_EQ(iter->key(), 50U);
  EXPECT_EQ(iter->name(), "B");

  // Explicitly clear the multiset before items goes out of scope.
  multiset_.clear();
}

TEST_F(IntrusiveMultiSetTest, LowerBound) {
  const IntrusiveMultiSet& multiset = multiset_;
  auto iter = multiset.lower_bound({10, "?"});
  ASSERT_NE(iter, multiset.end());
  EXPECT_STREQ(iter->name(), "e");

  iter = multiset.lower_bound({20, "?"});
  ASSERT_NE(iter, multiset.end());
  EXPECT_STREQ(iter->name(), "c");

  iter = multiset.lower_bound({30, "?"});
  ASSERT_NE(iter, multiset.end());
  EXPECT_STREQ(iter->name(), "a");

  iter = multiset.lower_bound({40, "?"});
  ASSERT_NE(iter, multiset.end());
  EXPECT_STREQ(iter->name(), "d");

  iter = multiset.lower_bound({50, "?"});
  ASSERT_NE(iter, multiset.end());
  EXPECT_STREQ(iter->name(), "b");
}

TEST_F(IntrusiveMultiSetTest, LowerBound_NoExactKey) {
  const IntrusiveMultiSet& multiset = multiset_;
  auto iter = multiset.lower_bound({6, "?"});
  ASSERT_NE(iter, multiset.end());
  EXPECT_STREQ(iter->name(), "e");

  iter = multiset.lower_bound({16, "?"});
  ASSERT_NE(iter, multiset.end());
  EXPECT_STREQ(iter->name(), "c");

  iter = multiset.lower_bound({26, "?"});
  ASSERT_NE(iter, multiset.end());
  EXPECT_STREQ(iter->name(), "a");

  iter = multiset.lower_bound({36, "?"});
  ASSERT_NE(iter, multiset.end());
  EXPECT_STREQ(iter->name(), "d");

  iter = multiset.lower_bound({46, "?"});
  ASSERT_NE(iter, multiset.end());
  EXPECT_STREQ(iter->name(), "b");
}

TEST_F(IntrusiveMultiSetTest, LowerBound_OutOfRange) {
  const IntrusiveMultiSet& multiset = multiset_;
  EXPECT_EQ(multiset.lower_bound({56, "?"}), multiset.end());
}

TEST_F(IntrusiveMultiSetTest, LowerBound_WithDuplicates) {
  TestItem item1(20, "1");
  TestItem item2(40, "1");
  TestItem item3(40, "1");
  multiset_.insert(item1);
  multiset_.insert(item2);
  multiset_.insert(item3);
  EXPECT_EQ(multiset_.size(), items_.size() + 3);

  auto iter = multiset_.lower_bound({20, "?"});
  EXPECT_LT((--iter)->key(), 20U);
  EXPECT_EQ((++iter)->key(), 20U);
  EXPECT_EQ((++iter)->key(), 20U);
  EXPECT_GT((++iter)->key(), 20U);

  iter = multiset_.lower_bound({40, "?"});
  EXPECT_LT((--iter)->key(), 40U);
  EXPECT_EQ((++iter)->key(), 40U);
  EXPECT_EQ((++iter)->key(), 40U);
  EXPECT_EQ((++iter)->key(), 40U);
  EXPECT_GT((++iter)->key(), 40U);

  // Explicitly clear the multiset before items 1-3 go out of scope.
  multiset_.clear();
}

TEST_F(IntrusiveMultiSetTest, UpperBound) {
  const IntrusiveMultiSet& multiset = multiset_;
  auto iter = multiset.upper_bound({15, "?"});
  ASSERT_NE(iter, multiset.end());
  EXPECT_STREQ(iter->name(), "c");

  iter = multiset.upper_bound({25, "?"});
  ASSERT_NE(iter, multiset.end());
  EXPECT_STREQ(iter->name(), "a");

  iter = multiset.upper_bound({35, "?"});
  ASSERT_NE(iter, multiset.end());
  EXPECT_STREQ(iter->name(), "d");

  iter = multiset.upper_bound({45, "?"});
  ASSERT_NE(iter, multiset.end());
  EXPECT_STREQ(iter->name(), "b");
}

TEST_F(IntrusiveMultiSetTest, UpperBound_NoExactKey) {
  const IntrusiveMultiSet& multiset = multiset_;
  auto iter = multiset.upper_bound({6, "?"});
  ASSERT_NE(iter, multiset.end());
  EXPECT_STREQ(iter->name(), "e");

  iter = multiset.upper_bound({16, "?"});
  ASSERT_NE(iter, multiset.end());
  EXPECT_STREQ(iter->name(), "c");

  iter = multiset.upper_bound({26, "?"});
  ASSERT_NE(iter, multiset.end());
  EXPECT_STREQ(iter->name(), "a");

  iter = multiset.upper_bound({36, "?"});
  ASSERT_NE(iter, multiset.end());
  EXPECT_STREQ(iter->name(), "d");

  iter = multiset.upper_bound({46, "?"});
  ASSERT_NE(iter, multiset.end());
  EXPECT_STREQ(iter->name(), "b");
}

TEST_F(IntrusiveMultiSetTest, UpperBound_OutOfRange) {
  const IntrusiveMultiSet& multiset = multiset_;
  EXPECT_EQ(multiset.upper_bound({56, "?"}), multiset.end());
}

TEST_F(IntrusiveMultiSetTest, UpperBound_WithDuplicates) {
  TestItem item1(20, "1");
  TestItem item2(40, "1");
  TestItem item3(40, "1");
  multiset_.insert(item1);
  multiset_.insert(item2);
  multiset_.insert(item3);
  EXPECT_EQ(multiset_.size(), items_.size() + 3);

  auto iter = multiset_.upper_bound({20, "?"});
  EXPECT_GT(iter->key(), 20U);

  iter = multiset_.upper_bound({40, "?"});
  EXPECT_GT(iter->key(), 40U);

  // Explicitly clear the multiset before items 1-3 go out of scope.
  multiset_.clear();
}

TEST_F(IntrusiveMultiSetTest, EqualRange) {
  const IntrusiveMultiSet& multiset = multiset_;

  auto pair = multiset.equal_range({10, "?"});
  IntrusiveMultiSet::const_iterator lower = pair.first;
  IntrusiveMultiSet::const_iterator upper = pair.second;
  ASSERT_NE(lower, multiset.end());
  EXPECT_STREQ(lower->name(), "e");
  ASSERT_NE(upper, multiset.end());
  EXPECT_STREQ(upper->name(), "E");

  std::tie(lower, upper) = multiset.equal_range({20, "?"});
  ASSERT_NE(lower, multiset.end());
  EXPECT_STREQ(lower->name(), "c");
  ASSERT_NE(upper, multiset.end());
  EXPECT_STREQ(upper->name(), "C");

  std::tie(lower, upper) = multiset.equal_range({30, "?"});
  ASSERT_NE(lower, multiset.end());
  EXPECT_STREQ(lower->name(), "a");
  ASSERT_NE(upper, multiset.end());
  EXPECT_STREQ(upper->name(), "A");

  std::tie(lower, upper) = multiset.equal_range({40, "?"});
  ASSERT_NE(lower, multiset.end());
  EXPECT_STREQ(lower->name(), "d");
  ASSERT_NE(upper, multiset.end());
  EXPECT_STREQ(upper->name(), "D");

  std::tie(lower, upper) = multiset.equal_range({50, "?"});
  ASSERT_NE(lower, multiset.end());
  EXPECT_STREQ(lower->name(), "b");
  ASSERT_NE(upper, multiset.end());
  EXPECT_STREQ(upper->name(), "B");
}

TEST_F(IntrusiveMultiSetTest, EqualRange_NoExactKey) {
  const IntrusiveMultiSet& multiset = multiset_;

  auto pair = multiset.equal_range({6, "?"});
  IntrusiveMultiSet::const_iterator lower = pair.first;
  IntrusiveMultiSet::const_iterator upper = pair.second;
  ASSERT_NE(lower, multiset.end());
  EXPECT_STREQ(lower->name(), "e");
  ASSERT_NE(upper, multiset.end());
  EXPECT_STREQ(upper->name(), "e");

  std::tie(lower, upper) = multiset.equal_range({16, "?"});
  ASSERT_NE(lower, multiset.end());
  EXPECT_STREQ(lower->name(), "c");
  ASSERT_NE(upper, multiset.end());
  EXPECT_STREQ(upper->name(), "c");

  std::tie(lower, upper) = multiset.equal_range({26, "?"});
  ASSERT_NE(lower, multiset.end());
  EXPECT_STREQ(lower->name(), "a");
  ASSERT_NE(upper, multiset.end());
  EXPECT_STREQ(upper->name(), "a");

  std::tie(lower, upper) = multiset.equal_range({36, "?"});
  ASSERT_NE(lower, multiset.end());
  EXPECT_STREQ(lower->name(), "d");
  ASSERT_NE(upper, multiset.end());
  EXPECT_STREQ(upper->name(), "d");

  std::tie(lower, upper) = multiset.equal_range({46, "?"});
  ASSERT_NE(lower, multiset.end());
  EXPECT_STREQ(lower->name(), "b");
  ASSERT_NE(upper, multiset.end());
  EXPECT_STREQ(upper->name(), "b");
}

TEST_F(IntrusiveMultiSetTest, EqualRange_OutOfRange) {
  const IntrusiveMultiSet& multiset = multiset_;

  auto pair = multiset.equal_range({56, "?"});
  IntrusiveMultiSet::const_iterator lower = pair.first;
  IntrusiveMultiSet::const_iterator upper = pair.second;
  EXPECT_EQ(lower, multiset.end());
  EXPECT_EQ(upper, multiset.end());
}

TEST_F(IntrusiveMultiSetTest, EqualRange_WithDuplicates) {
  TestItem item1(40, "1");
  TestItem item2(40, "2");
  TestItem item3(40, "3");
  multiset_.insert(item1);
  multiset_.insert(item2);
  multiset_.insert(item3);

  auto result = multiset_.equal_range({40, "?"});
  EXPECT_EQ(std::distance(result.first, result.second), 4);

  // Explicitly clear the multiset before items 1-3 go out of scope.
  multiset_.clear();
}

}  // namespace
