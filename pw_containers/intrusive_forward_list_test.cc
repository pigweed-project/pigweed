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

#include "pw_containers/intrusive_forward_list.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <utility>

#include "pw_compilation_testing/negative_compilation.h"
#include "pw_containers/vector.h"
#include "pw_preprocessor/util.h"
#include "pw_unit_test/framework.h"

namespace {

// Test fixtures

using ::pw::IntrusiveForwardList;
using ::pw::IntrusiveForwardListItem;

class Item : public IntrusiveForwardList<Item>::Item {
 public:
  constexpr Item() = default;
  constexpr Item(int number) : number_(number) {}

  Item(Item&& other) : Item() { *this = std::move(other); }
  Item& operator=(Item&& other) {
    number_ = other.number_;
    return *this;
  }

  int GetNumber() const { return number_; }
  void SetNumber(int num) { number_ = num; }

  // This operator ensures comparisons are done by identity rather than equality
  // for `remove`, and allows using the zero-parameter overload of `unique`.
  bool operator==(const Item& other) const { return number_ == other.number_; }

  // This operator allows using the zero-parameter overloads of `merge` and
  // `sort`.
  bool operator<(const Item& other) const { return number_ < other.number_; }

 private:
  int number_ = 0;
};

using List = IntrusiveForwardList<Item>;

// Test that a list of items derived from a different Item class can be created.
class DerivedItem : public Item {};

// TODO: b/235289499 - Tests guarded by this definition should trigger CHECK
// failures. These require using a testing version of pw_assert.
#ifndef TESTING_CHECK_FAILURES_IS_SUPPORTED
#define TESTING_CHECK_FAILURES_IS_SUPPORTED 0
#endif  // TESTING_CHECK_FAILURES_IS_SUPPORTED

// Unit tests.

TEST(IntrusiveForwardListTest, Construct_InitializerList_Empty) {
  List list({});
  EXPECT_TRUE(list.empty());
}

TEST(IntrusiveForwardListTest, Construct_InitializerList_One) {
  Item one(1);
  List list({&one});

  EXPECT_EQ(&one, &list.front());
  list.clear();
}

TEST(IntrusiveForwardListTest, Construct_InitializerList_Multiple) {
  Item one(1);
  Item two(2);
  Item thr(3);

  List list({&one, &two, &thr});
  auto it = list.begin();
  EXPECT_EQ(&one, &(*it++));
  EXPECT_EQ(&two, &(*it++));
  EXPECT_EQ(&thr, &(*it++));
  EXPECT_EQ(list.end(), it);
  list.clear();
}

TEST(IntrusiveForwardListTest, Construct_ObjectIterator_Empty) {
  std::array<Item, 0> array;
  List list(array.begin(), array.end());

  EXPECT_TRUE(list.empty());
}

TEST(IntrusiveForwardListTest, Construct_ObjectIterator_One) {
  std::array<Item, 1> array{{{1}}};
  List list(array.begin(), array.end());

  EXPECT_EQ(&array.front(), &list.front());
  list.clear();
}

TEST(IntrusiveForwardListTest, Construct_ObjectIterator_Multiple) {
  std::array<Item, 3> array{{{1}, {2}, {3}}};

  List list(array.begin(), array.end());
  auto it = list.begin();
  EXPECT_EQ(&array[0], &(*it++));
  EXPECT_EQ(&array[1], &(*it++));
  EXPECT_EQ(&array[2], &(*it++));
  EXPECT_EQ(list.end(), it);
  list.clear();
}

TEST(IntrusiveForwardListTest, Construct_PointerIterator_Empty) {
  std::array<Item*, 0> array;
  List list(array.begin(), array.end());

  EXPECT_TRUE(list.empty());
  list.clear();
}

TEST(IntrusiveForwardListTest, Construct_PointerIterator_One) {
  std::array<Item, 1> array{{{1}}};
  std::array<Item*, 1> ptrs{{&array[0]}};

  List list(ptrs.begin(), ptrs.end());

  EXPECT_EQ(ptrs[0], &list.front());
  list.clear();
}

TEST(IntrusiveForwardListTest, Construct_PointerIterator_Multiple) {
  std::array<Item, 3> array{{{1}, {2}, {3}}};
  std::array<Item*, 3> ptrs{{&array[0], &array[1], &array[2]}};

  List list(ptrs.begin(), ptrs.end());
  auto it = list.begin();
  EXPECT_EQ(ptrs[0], &(*it++));
  EXPECT_EQ(ptrs[1], &(*it++));
  EXPECT_EQ(ptrs[2], &(*it++));
  EXPECT_EQ(list.end(), it);
  list.clear();
}

#if TESTING_CHECK_FAILURES_IS_SUPPORTED
TEST(IntrusiveForwardListTest, Construct_DuplicateItems) {
  Item item(1);
  List list({&item, &item});
}
#endif  // TESTING_CHECK_FAILURES_IS_SUPPORTED

TEST(IntrusiveForwardListTest, Construct_Move) {
  std::array<Item, 4> items1{{{0}, {1}, {2}, {3}}};
  List list1(items1.begin(), items1.end());
  List list2(std::move(list1));

  auto it = list2.begin();
  EXPECT_EQ(&items1[0], &(*it++));
  EXPECT_EQ(&items1[1], &(*it++));
  EXPECT_EQ(&items1[2], &(*it++));
  EXPECT_EQ(&items1[3], &(*it++));
  EXPECT_EQ(it, list2.end());

  list2.clear();
}

TEST(IntrusiveForwardListTest, Construct_Move_Empty) {
  List list1;
  List list2(std::move(list1));

  EXPECT_TRUE(list1.empty());  // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(list2.empty());
}

TEST(IntrusiveForwardListTest, Assign_Move) {
  std::array<Item, 4> items1{{{0}, {1}, {2}, {3}}};
  std::array<Item, 2> items2{{{4}, {5}}};
  List list1(items1.begin(), items1.end());
  List list2(items2.begin(), items2.end());

  list2 = std::move(list1);

  auto it = list2.begin();
  EXPECT_EQ(&items1[0], &(*it++));
  EXPECT_EQ(&items1[1], &(*it++));
  EXPECT_EQ(&items1[2], &(*it++));
  EXPECT_EQ(&items1[3], &(*it++));
  EXPECT_EQ(it, list2.end());

  list2.clear();
}

TEST(IntrusiveForwardListTest, Assign_Move_Empty) {
  std::array<Item, 3> items1{{{0}, {1}, {2}}};
  List list1(items1.begin(), items1.end());
  List list2;

  list1 = std::move(list2);

  EXPECT_TRUE(list1.empty());
  EXPECT_TRUE(list2.empty());  // NOLINT(bugprone-use-after-move)
}

TEST(IntrusiveForwardListTest, Assign_ReplacesPriorContents) {
  std::array<Item, 3> array{{{0}, {100}, {200}}};
  List list(array.begin(), array.end());

  list.assign(array.begin() + 1, array.begin() + 2);

  auto it = list.begin();
  EXPECT_EQ(&array[1], &(*it++));
  EXPECT_EQ(list.end(), it);
  list.clear();
}

TEST(IntrusiveForwardListTest, Assign_EmptyRange) {
  std::array<Item, 3> array{{{0}, {100}, {200}}};
  List list(array.begin(), array.end());

  list.assign(array.begin() + 1, array.begin() + 1);

  EXPECT_TRUE(list.empty());
}

// Element access unit tests

TEST(IntrusiveForwardListTest, IteratorIsDefaultConstructible) {
  List list;
  List::iterator iter;
  EXPECT_NE(iter, list.begin());
  EXPECT_NE(iter, list.begin());
  EXPECT_EQ(iter, List::iterator());
}

TEST(IntrusiveForwardListTest, IteratorIsCopyConstructible) {
  List list;
  List::iterator iter1 = list.begin();
  List::iterator iter2(iter1);
  EXPECT_EQ(iter2, list.begin());
}

TEST(IntrusiveForwardListTest, IteratorCopyAssignable) {
  List list;
  List::iterator iter1 = list.begin();
  List::iterator iter2 = iter1;
  EXPECT_EQ(iter2, list.begin());
}

TEST(IntrusiveForwardListTest, IteratorisMoveConstructible) {
  List list;
  List::iterator iter1 = list.begin();
  List::iterator iter2(std::move(iter1));
  EXPECT_EQ(iter2, list.begin());
}

TEST(IntrusiveForwardListTest, IteratorMoveAssignable) {
  List list;
  List::iterator iter1 = list.begin();
  List::iterator iter2 = std::move(iter1);
  EXPECT_EQ(iter2, list.begin());
}

TEST(IntrusiveForwardListTest, ListFront) {
  Item item1(1);
  Item item2(0);
  Item item3(0xffff);

  List list;
  list.push_front(item3);
  list.push_front(item2);
  list.push_front(item1);

  EXPECT_EQ(&item1, &list.front());
  EXPECT_EQ(&item1, &(*list.begin()));
  list.clear();
}

// Iterator unit tests

TEST(IntrusiveForwardListTest, IteratorIncrement) {
  Item item_array[20];
  List list;
  for (size_t i = PW_ARRAY_SIZE(item_array); i != 0; --i) {
    item_array[i - 1].SetNumber(static_cast<int>(i - 1));
    list.push_front(item_array[i - 1]);
  }

  auto it = list.begin();
  int i = 0;
  while (it != list.end()) {
    if (i == 0) {
      // Test pre-incrementing on the first element.
      EXPECT_EQ((++it)->GetNumber(), item_array[++i].GetNumber());
    } else {
      EXPECT_EQ((it++)->GetNumber(), item_array[i++].GetNumber());
    }
  }
  list.clear();
}

TEST(IntrusiveForwardListTest, ConstIteratorRead) {
  // For this test, items are checked to be non-zero.
  Item item1(1);
  Item item2(99);
  List list;

  const List* const_list = &list;

  list.push_front(item1);
  list.push_front(item2);

  auto it = const_list->begin();
  while (it != const_list->end()) {
    EXPECT_NE(it->GetNumber(), 0);
    it++;
  }
  list.clear();
}

TEST(IntrusiveForwardListTest, CompareConstAndNonConstIterator) {
  List list;
  EXPECT_EQ(list.end(), list.cend());
}

class OtherListItem : public IntrusiveForwardList<OtherListItem>::Item {};

using OtherList = IntrusiveForwardList<OtherListItem>;

TEST(IntrusiveForwardListTest,
     CompareConstAndNonConstIterator_CompilationFails) {
  List list;
  OtherList list2;
#if PW_NC_TEST(CannotCompareIncompatibleIteratorsEqual)
  PW_NC_EXPECT("list\.end\(\) == list2\.end\(\)");
  static_cast<void>(list.end() == list2.end());
#elif PW_NC_TEST(CannotCompareIncompatibleIteratorsInequal)
  PW_NC_EXPECT("list\.end\(\) != list2\.end\(\)");
  static_cast<void>(list.end() != list2.end());
#endif  // PW_NC_TEST
}

#if PW_NC_TEST(CannotModifyThroughConstIterator)
PW_NC_EXPECT("function is not marked const|discards qualifiers");

TEST(IntrusiveForwardListTest, ConstIteratorModify) {
  Item item1(1);
  Item item2(99);
  List list;

  const List* const_list = &list;

  list.push_front(item1);
  list.push_front(item2);

  auto it = const_list->begin();
  while (it != const_list->end()) {
    it->SetNumber(0);
    it++;
  }
}

#endif  // PW_NC_TEST

// Capacity unit tests

TEST(IntrusiveForwardListTest, IsEmpty) {
  Item item1(1);

  List list;
  EXPECT_TRUE(list.empty());

  list.push_front(item1);
  EXPECT_FALSE(list.empty());
  list.clear();
}

TEST(IntrusiveForwardListTest, MaxSize) {
  List list;
  EXPECT_EQ(list.max_size(), size_t(std::numeric_limits<ptrdiff_t>::max()));
}

// Modifier unit tests

TEST(IntrusiveForwardListTest, Clear_Empty) {
  List list;
  EXPECT_TRUE(list.empty());
  list.clear();
  EXPECT_TRUE(list.empty());
}

TEST(IntrusiveForwardListTest, Clear_OneItem) {
  Item item(42);
  List list;
  list.push_front(item);
  EXPECT_FALSE(list.empty());
  list.clear();
  EXPECT_TRUE(list.empty());
}

TEST(IntrusiveForwardListTest, Clear_TwoItems) {
  Item item1(42);
  Item item2(42);
  List list;
  list.push_front(item1);
  list.push_front(item2);
  EXPECT_FALSE(list.empty());
  list.clear();
  EXPECT_TRUE(list.empty());
}

TEST(IntrusiveForwardListTest, Clear_ReinsertClearedItems) {
  std::array<Item, 20> item_array;
  List list;
  EXPECT_TRUE(list.empty());
  list.clear();
  EXPECT_TRUE(list.empty());

  // Fill the list with Item objects.
  for (size_t i = 0; i < item_array.size(); ++i) {
    item_array[i].SetNumber(0);
    list.push_front(item_array[i]);
  }

  // Remove everything.
  list.clear();
  EXPECT_TRUE(list.empty());

  // Ensure all the removed elements can still be added back to a list.
  for (size_t i = 0; i < item_array.size(); ++i) {
    item_array[i].SetNumber(0);
    list.push_front(item_array[i]);
  }
  list.clear();
}

TEST(IntrusiveForwardListTest, InsertAfter) {
  // Create a test item to insert midway through the list.
  constexpr int kMagicValue = 42;
  Item inserted_item(kMagicValue);

  // Create initial values to fill in the start/end.
  Item item_array[20];

  List list;
  // Fill the list with Item objects that have a value of zero.
  for (size_t i = 0; i < PW_ARRAY_SIZE(item_array); ++i) {
    item_array[i].SetNumber(0);
    list.push_front(item_array[i]);
  }

  // Move an iterator to the middle of the list, and then insert the magic item.
  auto it = list.begin();
  size_t expected_index = 1;  // Expected index is iterator index + 1.
  for (size_t i = 0; i < PW_ARRAY_SIZE(item_array) / 2; ++i) {
    it++;
    expected_index++;
  }
  it = list.insert_after(it, inserted_item);

  // Ensure the returned iterator from insert_after is the newly inserted
  // element.
  EXPECT_EQ(it->GetNumber(), kMagicValue);

  // Ensure the value is in the expected location (index of the iterator + 1).
  size_t i = 0;
  for (Item& item : list) {
    if (item.GetNumber() == kMagicValue) {
      EXPECT_EQ(i, expected_index);
    } else {
      EXPECT_EQ(item.GetNumber(), 0);
    }
    i++;
  }

  // Ensure the list didn't break and change sizes.
  EXPECT_EQ(i, PW_ARRAY_SIZE(item_array) + 1);
  list.clear();
}

TEST(IntrusiveForwardListTest, InsertAfter_Range) {
  // Create an array of test items to insert into the middle of the list.
  constexpr int kMagicValue = 42;
  constexpr int kNumItems = 3;
  std::array<Item, kNumItems> inserted_items;
  int n = kMagicValue;
  for (auto& item : inserted_items) {
    item.SetNumber(n++);
  }

  // Create initial values to fill in the start/end.
  Item item_array[20];

  List list;
  // Fill the list with Item objects that have a value of zero.
  for (size_t i = 0; i < PW_ARRAY_SIZE(item_array); ++i) {
    item_array[i].SetNumber(0);
    list.push_front(item_array[i]);
  }

  // Move an iterator to the middle of the list, and then insert the magic item.
  auto it = list.begin();
  size_t expected_index = 1;  // Expected index is iterator index + 1.
  for (size_t i = 0; i < PW_ARRAY_SIZE(item_array) / 2; ++i) {
    it++;
    expected_index++;
  }
  it = list.insert_after(it, inserted_items.begin(), inserted_items.end());

  // Ensure the returned iterator from insert is the last newly inserted
  // element.
  EXPECT_EQ(it->GetNumber(), kMagicValue + kNumItems - 1);

  // Ensure the value is in the expected location (index of the iterator + 1).
  size_t i = 0;
  for (Item& item : list) {
    if (i < expected_index) {
      EXPECT_EQ(item.GetNumber(), 0);
    } else if (i < expected_index + inserted_items.size()) {
      EXPECT_EQ(item.GetNumber(),
                kMagicValue + static_cast<int>(i - expected_index));
    } else {
      EXPECT_EQ(item.GetNumber(), 0);
    }
    i++;
  }

  // Ensure the list didn't break and change sizes.
  EXPECT_EQ(i, PW_ARRAY_SIZE(item_array) + inserted_items.size());
  list.clear();
}

TEST(IntrusiveForwardListTest, InsertAfter_InitializerList) {
  // Create an array of test items to insert into the middle of the list.
  constexpr int kMagicValue = 42;
  constexpr int kNumItems = 3;
  std::array<Item, kNumItems> inserted_items;
  int n = kMagicValue;
  for (auto& item : inserted_items) {
    item.SetNumber(n++);
  }

  // Create initial values to fill in the start/end.
  Item item_array[20];

  List list;
  // Fill the list with Item objects that have a value of zero.
  for (size_t i = 0; i < PW_ARRAY_SIZE(item_array); ++i) {
    item_array[i].SetNumber(0);
    list.push_front(item_array[i]);
  }

  // Move an iterator to the middle of the list, and then insert the magic item.
  auto it = list.begin();
  size_t expected_index = 1;  // Expected index is iterator index + 1.
  for (size_t i = 0; i < PW_ARRAY_SIZE(item_array) / 2; ++i) {
    it++;
    expected_index++;
  }
  it = list.insert_after(it,
                         {
                             &inserted_items[0],
                             &inserted_items[1],
                             &inserted_items[2],
                         });

  // Ensure the returned iterator from insert is the last newly inserted
  // element.
  EXPECT_EQ(it->GetNumber(), kMagicValue + kNumItems - 1);

  // Ensure the value is in the expected location (index of the iterator + 1).
  size_t i = 0;
  for (Item& item : list) {
    if (i < expected_index) {
      EXPECT_EQ(item.GetNumber(), 0);
    } else if (i < expected_index + inserted_items.size()) {
      EXPECT_EQ(item.GetNumber(),
                kMagicValue + static_cast<int>(i - expected_index));
    } else {
      EXPECT_EQ(item.GetNumber(), 0);
    }
    i++;
  }

  // Ensure the list didn't break and change sizes.
  EXPECT_EQ(i, PW_ARRAY_SIZE(item_array) + inserted_items.size());
  list.clear();
}

TEST(IntrusiveForwardListTest, InsertAfter_BeforeBegin) {
  // Create a test item to insert at the beginning of the list.
  constexpr int kMagicValue = 42;
  Item inserted_item(kMagicValue);

  // Create initial values to fill in the start/end.
  Item item_array[20];

  List list;
  // Fill the list with Item objects that have a value of zero.
  for (size_t i = 0; i < PW_ARRAY_SIZE(item_array); ++i) {
    item_array[i].SetNumber(0);
    list.push_front(item_array[i]);
  }

  auto it = list.insert_after(list.before_begin(), inserted_item);

  // Ensure the returned iterator from insert_after is the newly inserted
  // element.
  EXPECT_EQ(it->GetNumber(), kMagicValue);

  // Ensure the value is at the beginning of the list.
  size_t i = 0;
  for (Item& item : list) {
    if (item.GetNumber() == kMagicValue) {
      EXPECT_EQ(i, static_cast<size_t>(0));
    } else {
      EXPECT_EQ(item.GetNumber(), 0);
    }
    i++;
  }
  list.clear();
}

TEST(IntrusiveForwardListTest, InsertAfter_BeforeBegin_Range) {
  // Create an array of test items to insert into the middle of the list.
  constexpr int kMagicValue = 42;
  constexpr int kNumItems = 3;
  std::array<Item, kNumItems> inserted_items;
  int n = kMagicValue;
  for (auto& item : inserted_items) {
    item.SetNumber(n++);
  }

  // Create initial values to fill in the start/end.
  Item item_array[20];

  List list;
  // Fill the list with Item objects that have a value of zero.
  for (size_t i = 0; i < PW_ARRAY_SIZE(item_array); ++i) {
    item_array[i].SetNumber(0);
    list.push_front(item_array[i]);
  }

  auto it = list.insert_after(
      list.before_begin(), inserted_items.begin(), inserted_items.end());

  // Ensure the returned iterator from insert is the last newly inserted
  // element.
  EXPECT_EQ(it->GetNumber(), kMagicValue + kNumItems - 1);

  // Ensure the values are at the beginning of the list.
  size_t i = 0;
  for (Item& item : list) {
    if (i < inserted_items.size()) {
      EXPECT_EQ(item.GetNumber(), kMagicValue + static_cast<int>(i));
    } else {
      EXPECT_EQ(item.GetNumber(), 0);
    }
    i++;
  }
  list.clear();
}

TEST(IntrusiveForwardListTest, InsertAfter_BeforeBegin_InitializerList) {
  // Create an array of test items to insert into the middle of the list.
  constexpr int kMagicValue = 42;
  constexpr int kNumItems = 3;
  std::array<Item, kNumItems> inserted_items;
  int n = kMagicValue;
  for (auto& item : inserted_items) {
    item.SetNumber(n++);
  }

  // Create initial values to fill in the start/end.
  Item item_array[20];

  List list;
  // Fill the list with Item objects that have a value of zero.
  for (size_t i = 0; i < PW_ARRAY_SIZE(item_array); ++i) {
    item_array[i].SetNumber(0);
    list.push_front(item_array[i]);
  }

  auto it = list.insert_after(list.before_begin(),
                              {
                                  &inserted_items[0],
                                  &inserted_items[1],
                                  &inserted_items[2],
                              });

  // Ensure the returned iterator from insert is the last newly inserted
  // element.
  EXPECT_EQ(it->GetNumber(), kMagicValue + kNumItems - 1);

  // Ensure the values are at the beginning of the list.
  size_t i = 0;
  for (Item& item : list) {
    if (i < inserted_items.size()) {
      EXPECT_EQ(item.GetNumber(), kMagicValue + static_cast<int>(i));
    } else {
      EXPECT_EQ(item.GetNumber(), 0);
    }
    i++;
  }
  list.clear();
}

#if TESTING_CHECK_FAILURES_IS_SUPPORTED
TEST(IntrusiveForwardListTest, InsertAfter_SameItem) {
  Item item(1);
  List list({&item});

  list.insert_after(list.begin(), item);
}

TEST(IntrusiveForwardListTest, InsertAfter_SameItemAfterEnd) {
  Item item(1);
  List list({&item});

  list.insert_after(list.end(), item);
}
#endif  // TESTING_CHECK_FAILURES_IS_SUPPORTED

TEST(IntrusiveForwardListTest, EraseAfter_FirstItem) {
  std::array<Item, 3> items{{{0}, {1}, {2}}};
  List list(items.begin(), items.end());

  auto it = list.erase_after(list.before_begin());
  EXPECT_EQ(list.begin(), it);
  EXPECT_EQ(&items[1], &list.front());
  list.clear();
}

TEST(IntrusiveForwardListTest, EraseAfter_LastItem) {
  std::array<Item, 3> items{{{0}, {1}, {2}}};
  List list(items.begin(), items.end());

  auto it = list.begin();
  ++it;

  it = list.erase_after(it);
  EXPECT_EQ(list.end(), it);

  it = list.begin();
  ++it;

  EXPECT_EQ(&items[1], &(*it));
  list.clear();
}

TEST(IntrusiveForwardListTest, EraseAfter_AllItems) {
  std::array<Item, 3> items{{{0}, {1}, {2}}};
  List list(items.begin(), items.end());

  list.erase_after(list.begin());
  list.erase_after(list.begin());
  auto it = list.erase_after(list.before_begin());

  EXPECT_EQ(list.end(), it);
  EXPECT_TRUE(list.empty());
}

TEST(IntrusiveForwardListTest, EraseAfter_LeadingRange) {
  std::array<Item, 4> items{{{0}, {1}, {2}, {3}}};
  List list(items.begin(), items.end());

  auto it =
      list.erase_after(list.before_begin(), std::next(std::next(list.begin())));
  EXPECT_EQ(list.begin(), it);
  EXPECT_EQ(&items[2], &(*it++));
  EXPECT_EQ(&items[3], &(*it++));
  EXPECT_EQ(list.end(), it);
  list.clear();
}

TEST(IntrusiveForwardListTest, EraseAfter_TrailingRange) {
  std::array<Item, 4> items{{{0}, {1}, {2}, {3}}};
  List list(items.begin(), items.end());

  auto it = list.erase_after(std::next(list.begin()), list.end());
  EXPECT_EQ(list.end(), it);

  it = list.begin();
  EXPECT_EQ(&items[0], &(*it++));
  EXPECT_EQ(&items[1], &(*it++));
  EXPECT_EQ(list.end(), it);
  list.clear();
}

TEST(IntrusiveForwardListTest, EraseAfter_FullRange) {
  std::array<Item, 4> items{{{0}, {1}, {2}, {3}}};
  List list(items.begin(), items.end());

  auto it = list.erase_after(list.before_begin(), list.end());
  EXPECT_EQ(list.end(), it);
  EXPECT_TRUE(list.empty());
}

TEST(IntrusiveForwardListTest, EraseAfter_EmptyRange) {
  std::array<Item, 4> items{{{0}, {1}, {2}, {3}}};
  List list(items.begin(), items.end());

  auto it = list.erase_after(list.before_begin(), list.begin());
  EXPECT_EQ(list.begin(), it);
  EXPECT_EQ(&items[0], &list.front());
  list.clear();
}

TEST(IntrusiveForwardListTest, PushFront) {
  constexpr int kMagicValue = 42;
  Item pushed_item(kMagicValue);

  Item item_array[20];
  List list;
  // Fill the list with Item objects that have a value of zero.
  for (size_t i = 0; i < PW_ARRAY_SIZE(item_array); ++i) {
    item_array[i].SetNumber(0);
    list.push_front(item_array[i]);
  }

  // Create a test item to push to the front of the list.
  list.push_front(pushed_item);
  EXPECT_EQ(list.front().GetNumber(), kMagicValue);
  list.clear();
}

TEST(IntrusiveForwardListTest, PushFrontOne) {
  constexpr int kMagicValue = 31;
  Item item1(kMagicValue);
  List list;
  list.push_front(item1);
  EXPECT_FALSE(list.empty());
  EXPECT_EQ(list.front().GetNumber(), kMagicValue);
  list.clear();
}

TEST(IntrusiveForwardListTest, PushFrontThree) {
  Item item1(1);
  Item item2(2);
  Item item3(3);

  List list;
  list.push_front(item3);
  list.push_front(item2);
  list.push_front(item1);

  int loop_count = 0;
  for (auto& test_item : list) {
    loop_count++;
    EXPECT_EQ(loop_count, test_item.GetNumber());
  }
  EXPECT_EQ(loop_count, 3);
  list.clear();
}

#if TESTING_CHECK_FAILURES_IS_SUPPORTED
TEST(IntrusiveForwardListTest, PushFront_SameItem) {
  Item item(1);
  List list({&item});

  list.push_front(item);
}
#endif  // TESTING_CHECK_FAILURES_IS_SUPPORTED

TEST(IntrusiveForwardListTest, PopFront) {
  constexpr int kValue1 = 32;
  constexpr int kValue2 = 4083;

  Item item1(kValue1);
  Item item2(kValue2);

  List list;
  EXPECT_TRUE(list.empty());

  list.push_front(item2);
  list.push_front(item1);
  list.pop_front();
  EXPECT_EQ(list.front().GetNumber(), kValue2);
  EXPECT_FALSE(list.empty());
  list.pop_front();
  EXPECT_TRUE(list.empty());
}

TEST(IntrusiveForwardListTest, PopFrontAndReinsert) {
  constexpr int kValue1 = 32;
  constexpr int kValue2 = 4083;

  Item item1(kValue1);
  Item item2(kValue2);

  List list;
  EXPECT_TRUE(list.empty());

  list.push_front(item2);
  list.push_front(item1);
  list.pop_front();
  list.push_front(item1);
  EXPECT_EQ(list.front().GetNumber(), kValue1);
  list.clear();
}

TEST(IntrusiveForwardListTest, Swap) {
  std::array<Item, 4> items1{{{0}, {1}, {2}, {3}}};
  std::array<Item, 2> items2{{{4}, {5}}};
  List list1(items1.begin(), items1.end());
  List list2(items2.begin(), items2.end());

  list1.swap(list2);

  auto it = list1.begin();
  EXPECT_EQ(&items2[0], &(*it++));
  EXPECT_EQ(&items2[1], &(*it++));
  EXPECT_EQ(it, list1.end());

  it = list2.begin();
  EXPECT_EQ(&items1[0], &(*it++));
  EXPECT_EQ(&items1[1], &(*it++));
  EXPECT_EQ(&items1[2], &(*it++));
  EXPECT_EQ(&items1[3], &(*it++));
  EXPECT_EQ(it, list2.end());

  list1.clear();
  list2.clear();
}

TEST(IntrusiveForwardListTest, Swap_Empty) {
  std::array<Item, 3> items1{{{0}, {1}, {2}}};
  List list1(items1.begin(), items1.end());
  List list2;

  list1.swap(list2);
  EXPECT_TRUE(list1.empty());

  auto it = list2.begin();
  EXPECT_EQ(&items1[0], &(*it++));
  EXPECT_EQ(&items1[1], &(*it++));
  EXPECT_EQ(&items1[2], &(*it++));
  EXPECT_EQ(it, list2.end());

  list1.swap(list2);
  EXPECT_TRUE(list2.empty());

  it = list1.begin();
  EXPECT_EQ(&items1[0], &(*it++));
  EXPECT_EQ(&items1[1], &(*it++));
  EXPECT_EQ(&items1[2], &(*it++));
  EXPECT_EQ(it, list1.end());

  list1.clear();
}

// Operation unit tests

TEST(IntrusiveForwardListTest, Merge) {
  std::array<Item, 3> evens{{{0}, {2}, {4}}};
  std::array<Item, 3> odds{{{1}, {3}, {5}}};

  List list(evens.begin(), evens.end());
  List other(odds.begin(), odds.end());
  list.merge(other);
  EXPECT_TRUE(other.empty());

  int i = 0;
  for (const Item& item : list) {
    EXPECT_EQ(item.GetNumber(), i++);
  }
  EXPECT_EQ(i, 6);
  list.clear();
}

TEST(IntrusiveForwardListTest, Merge_Compare) {
  std::array<Item, 3> evens{{{4}, {2}, {0}}};
  std::array<Item, 3> odds{{{5}, {3}, {1}}};
  auto greater_than = [](const Item& a, const Item& b) {
    return a.GetNumber() > b.GetNumber();
  };

  List list(evens.begin(), evens.end());
  List other(odds.begin(), odds.end());
  list.merge(other, greater_than);
  EXPECT_TRUE(other.empty());

  int i = 6;
  for (const Item& item : list) {
    EXPECT_EQ(item.GetNumber(), --i);
  }
  EXPECT_EQ(i, 0);
  list.clear();
}

TEST(IntrusiveForwardListTest, Merge_Empty) {
  std::array<Item, 3> items{{{1}, {2}, {3}}};

  List list;
  List other(items.begin(), items.end());
  list.merge(other);

  EXPECT_TRUE(other.empty());
  list.merge(other);

  int i = 1;
  for (const Item& item : list) {
    EXPECT_EQ(item.GetNumber(), i++);
  }
  EXPECT_EQ(i, 4);
  list.clear();
}

TEST(IntrusiveForwardListTest, Merge_IsStable) {
  std::array<Item, 2> ends{{{0}, {2}}};
  std::array<Item, 3> mids{{{1}, {1}, {1}}};

  List list(ends.begin(), ends.end());
  List other(mids.begin(), mids.end());
  list.merge(other);
  EXPECT_TRUE(other.empty());

  auto it = list.begin();
  EXPECT_EQ(&ends[0], &(*it++));
  EXPECT_EQ(&mids[0], &(*it++));
  EXPECT_EQ(&mids[1], &(*it++));
  EXPECT_EQ(&mids[2], &(*it++));
  EXPECT_EQ(&ends[1], &(*it++));
  EXPECT_EQ(list.end(), it);
  list.clear();
}

TEST(IntrusiveForwardListTest, SpliceAfter) {
  std::array<Item, 2> items{{{1}, {5}}};
  std::array<Item, 3> other_items{{{2}, {3}, {4}}};

  List list(items.begin(), items.end());
  List other(other_items.begin(), other_items.end());
  list.splice_after(list.begin(), other);
  EXPECT_TRUE(other.empty());

  int i = 1;
  for (const Item& item : list) {
    EXPECT_EQ(item.GetNumber(), i++);
  }
  EXPECT_EQ(i, 6);
  list.clear();
}

TEST(IntrusiveForwardListTest, SpliceAfter_BeforeBegin) {
  std::array<Item, 2> items{{{4}, {5}}};
  std::array<Item, 3> other_items{{{1}, {2}, {3}}};

  List list(items.begin(), items.end());
  List other(other_items.begin(), other_items.end());
  list.splice_after(list.before_begin(), other);
  EXPECT_TRUE(other.empty());

  int i = 1;
  for (const Item& item : list) {
    EXPECT_EQ(item.GetNumber(), i++);
  }
  EXPECT_EQ(i, 6);
  list.clear();
}

TEST(IntrusiveForwardListTest, SpliceAfter_BeforeEnd) {
  std::array<Item, 2> items{{{1}, {2}}};
  std::array<Item, 3> other_items{{{3}, {4}, {5}}};

  List list(items.begin(), items.end());
  List other(other_items.begin(), other_items.end());
  auto it = list.begin();
  while (std::next(it) != list.end()) {
    ++it;
  }
  list.splice_after(it, other);
  EXPECT_TRUE(other.empty());

  int i = 1;
  for (const Item& item : list) {
    EXPECT_EQ(item.GetNumber(), i++);
  }
  EXPECT_EQ(i, 6);
  list.clear();
}

TEST(IntrusiveForwardListTest, SpliceAfter_OneItem) {
  std::array<Item, 2> items{{{1}, {3}}};
  std::array<Item, 3> other_items{{{1}, {2}, {3}}};

  List list(items.begin(), items.end());
  List other(other_items.begin(), other_items.end());
  list.splice_after(list.begin(), other, other.begin());
  EXPECT_FALSE(other.empty());

  int i = 1;
  for (const Item& item : list) {
    EXPECT_EQ(item.GetNumber(), i++);
  }
  EXPECT_EQ(i, 4);
  other.clear();
  list.clear();
}

TEST(IntrusiveForwardListTest, SpliceAfter_Range) {
  std::array<Item, 2> items{{{1}, {5}}};
  std::array<Item, 5> other_items{{{1}, {2}, {3}, {4}, {5}}};

  List list(items.begin(), items.end());
  List other(other_items.begin(), other_items.end());
  auto it = other.begin();
  while (std::next(it) != other.end()) {
    ++it;
  }
  list.splice_after(list.begin(), other, other.begin(), it);
  EXPECT_FALSE(other.empty());

  int i = 1;
  for (const Item& item : list) {
    EXPECT_EQ(item.GetNumber(), i++);
  }
  EXPECT_EQ(i, 6);
  other.clear();
  list.clear();
}

TEST(IntrusiveForwardListTest, SpliceAfter_EmptyRange) {
  std::array<Item, 3> items{{{1}, {2}, {3}}};
  std::array<Item, 3> other_items{{{4}, {4}, {4}}};

  List list(items.begin(), items.end());
  List other(other_items.begin(), other_items.end());
  list.splice_after(
      list.before_begin(), other, other.before_begin(), other.begin());
  EXPECT_FALSE(other.empty());

  int i = 1;
  for (const Item& item : list) {
    EXPECT_EQ(item.GetNumber(), i++);
  }
  EXPECT_EQ(i, 4);
  other.clear();
  list.clear();
}

TEST(IntrusiveForwardListTest, Remove_EmptyList) {
  std::array<Item, 1> items{{{3}}};
  List list(items.begin(), items.begin());  // Add nothing!

  EXPECT_TRUE(list.empty());
  EXPECT_FALSE(list.remove(items[0]));
}

TEST(IntrusiveForwardListTest, Remove_SingleItem_NotPresent) {
  std::array<Item, 1> items{{{1}}};
  List list(items.begin(), items.end());

  EXPECT_FALSE(list.remove(Item(1)));
  EXPECT_EQ(&items.front(), &list.front());
  list.clear();
}

TEST(IntrusiveForwardListTest, Remove_SingleItem_Removed) {
  std::array<Item, 1> items{{{1}}};
  List list(items.begin(), items.end());

  EXPECT_TRUE(list.remove(items[0]));
  EXPECT_TRUE(list.empty());
}

TEST(IntrusiveForwardListTest, Remove_MultipleItems_NotPresent) {
  std::array<Item, 5> items{{{1}, {1}, {2}, {3}, {4}}};
  List list(items.begin(), items.end());

  EXPECT_FALSE(list.remove(Item(1)));
  list.clear();
}

TEST(IntrusiveForwardListTest, Remove_MultipleItems_RemoveAndPushBack) {
  std::array<Item, 5> items{{{1}, {1}, {2}, {3}, {4}}};
  List list(items.begin(), items.end());

  EXPECT_TRUE(list.remove(items[0]));
  EXPECT_TRUE(list.remove(items[3]));
  list.push_front(items[0]);  // Make sure can add the item after removing it.

  auto it = list.begin();
  EXPECT_EQ(&items[0], &(*it++));
  EXPECT_EQ(&items[1], &(*it++));
  EXPECT_EQ(&items[2], &(*it++));
  EXPECT_EQ(&items[4], &(*it++));
  EXPECT_EQ(list.end(), it);
  list.clear();
}

TEST(IntrusiveForwardListTest, RemoveIf_MatchNone) {
  std::array<Item, 5> items{{{1}, {3}, {5}, {7}, {9}}};
  List list(items.begin(), items.end());
  auto equal_two = [](const Item& a) { return a.GetNumber() == 2; };

  EXPECT_EQ(list.remove_if(equal_two), 0U);

  auto it = list.begin();
  EXPECT_EQ(&items[0], &(*it++));
  EXPECT_EQ(&items[1], &(*it++));
  EXPECT_EQ(&items[2], &(*it++));
  EXPECT_EQ(&items[3], &(*it++));
  EXPECT_EQ(&items[4], &(*it++));
  EXPECT_EQ(list.end(), it);
  list.clear();
}

TEST(IntrusiveForwardListTest, RemoveIf_MatchSome) {
  std::array<Item, 5> items{{{1}, {2}, {2}, {2}, {3}}};
  List list(items.begin(), items.end());
  auto equal_two = [](const Item& a) { return a.GetNumber() == 2; };

  EXPECT_EQ(list.remove_if(equal_two), 3U);

  auto it = list.begin();
  EXPECT_EQ(&items[0], &(*it++));
  EXPECT_EQ(&items[4], &(*it++));
  EXPECT_EQ(list.end(), it);
  list.clear();
}

TEST(IntrusiveForwardListTest, RemoveIf_MatchAll) {
  std::array<Item, 5> items{{{2}, {2}, {2}, {2}, {2}}};
  List list(items.begin(), items.end());
  auto equal_two = [](const Item& a) { return a.GetNumber() == 2; };

  EXPECT_EQ(list.remove_if(equal_two), 5U);
  EXPECT_TRUE(list.empty());
}

TEST(IntrusiveForwardListTest, RemoveIf_Empty) {
  List list;
  auto equal_two = [](const Item& a) { return a.GetNumber() == 2; };

  EXPECT_EQ(list.remove_if(equal_two), 0U);
  EXPECT_TRUE(list.empty());
}

TEST(IntrusiveForwardListTest, Reverse) {
  std::array<Item, 5> items{{{0}, {1}, {2}, {3}, {4}}};
  List list(items.begin(), items.end());

  list.reverse();

  int i = 4;
  for (const Item& item : list) {
    EXPECT_EQ(item.GetNumber(), i--);
  }
  EXPECT_EQ(i, -1);
  list.clear();
}

TEST(IntrusiveForwardListTest, Reverse_Empty) {
  List list;
  list.reverse();
  EXPECT_TRUE(list.empty());
}

TEST(IntrusiveForwardListTest, Unique) {
  std::array<Item, 10> items{
      {{0}, {0}, {0}, {1}, {2}, {2}, {3}, {3}, {3}, {3}}};
  List list(items.begin(), items.end());

  EXPECT_EQ(list.unique(), 6U);

  int i = 0;
  for (const Item& item : list) {
    EXPECT_EQ(item.GetNumber(), i++);
  }
  EXPECT_EQ(i, 4);
  list.clear();
}

TEST(IntrusiveForwardListTest, Unique_Compare) {
  std::array<Item, 10> items{
      {{0}, {2}, {1}, {3}, {1}, {0}, {1}, {0}, {2}, {4}}};
  List list(items.begin(), items.end());
  auto parity = [](const Item& a, const Item& b) {
    return (a.GetNumber() % 2) == (b.GetNumber() % 2);
  };

  EXPECT_EQ(list.unique(parity), 5U);

  int i = 0;
  for (const Item& item : list) {
    EXPECT_EQ(item.GetNumber(), i);
    i = (i + 1) % 2;
  }
  list.clear();
}

TEST(IntrusiveForwardListTest, Unique_Empty) {
  List list;

  EXPECT_EQ(list.unique(), 0U);

  EXPECT_TRUE(list.empty());
}

TEST(IntrusiveForwardListTest, Unique_NoDuplicates) {
  std::array<Item, 5> items{{{0}, {1}, {2}, {3}, {4}}};
  List list(items.begin(), items.end());

  EXPECT_EQ(list.unique(), 0U);

  int i = 0;
  for (const Item& item : list) {
    EXPECT_EQ(item.GetNumber(), i++);
  }
  EXPECT_EQ(i, 5);
  list.clear();
}

TEST(IntrusiveForwardListTest, Sort) {
  std::array<Item, 5> items{{{5}, {1}, {3}, {2}, {4}}};
  List list(items.begin(), items.end());
  list.sort();

  int i = 1;
  for (const Item& item : list) {
    EXPECT_EQ(item.GetNumber(), i++);
  }
  EXPECT_EQ(i, 6);
  list.clear();
}

TEST(IntrusiveForwardListTest, Sort_Compare) {
  std::array<Item, 5> items{{{5}, {1}, {3}, {2}, {4}}};
  List list(items.begin(), items.end());
  auto greater_than = [](const Item& a, const Item& b) {
    return a.GetNumber() > b.GetNumber();
  };
  list.sort(greater_than);

  int i = 5;
  for (const Item& item : list) {
    EXPECT_EQ(item.GetNumber(), i--);
  }
  EXPECT_EQ(i, 0);
  list.clear();
}

TEST(IntrusiveForwardListTest, Sort_Empty) {
  List list;
  list.sort();
  EXPECT_TRUE(list.empty());
}

TEST(IntrusiveForwardListTest, Sort_Stable) {
  std::array<Item, 5> items{{{0}, {1}, {1}, {1}, {2}}};
  List list(items.begin(), items.end());
  list.sort();

  auto it = list.begin();
  EXPECT_EQ(&items[0], &(*it++));
  EXPECT_EQ(&items[1], &(*it++));
  EXPECT_EQ(&items[2], &(*it++));
  EXPECT_EQ(&items[3], &(*it++));
  EXPECT_EQ(&items[4], &(*it++));
  EXPECT_EQ(list.end(), it);
  list.clear();
}

// Other type-related unit tests

TEST(IntrusiveForwardListTest, AddItemsOfDerivedClassToList) {
  List list;

  DerivedItem item1;
  list.push_front(item1);

  Item item2;
  list.push_front(item2);

  EXPECT_EQ(2, std::distance(list.begin(), list.end()));
  list.clear();
}

TEST(IntrusiveForwardListTest, ListOfDerivedClassItems) {
  IntrusiveForwardList<DerivedItem> derived_from_compatible_item_type;

  DerivedItem item1;
  derived_from_compatible_item_type.push_front(item1);

  EXPECT_EQ(1,
            std::distance(derived_from_compatible_item_type.begin(),
                          derived_from_compatible_item_type.end()));

#if PW_NC_TEST(CannotAddBaseClassToDerivedClassList)
  PW_NC_EXPECT_CLANG("cannot bind to a value of unrelated type");
  PW_NC_EXPECT_GCC("cannot convert");

  Item item2;
  derived_from_compatible_item_type.push_front(item2);
#endif
  derived_from_compatible_item_type.clear();
}

#if PW_NC_TEST(IncompatibleItem)
PW_NC_EXPECT(
    "IntrusiveForwardList items must be derived from "
    "IntrusiveForwardList<T>::Item");

struct Foo {};

class BadItem : public IntrusiveForwardList<Foo>::Item {};

[[maybe_unused]] void IncompatibleItem() {
  IntrusiveForwardList<BadItem> derived_from_incompatible_item_type;
  (void)derived_from_incompatible_item_type.front();
}

#elif PW_NC_TEST(DoesNotInheritFromItem)
PW_NC_EXPECT(
    "IntrusiveForwardList items must be derived from "
    "IntrusiveForwardList<T>::Item");

struct NotAnItem {};

[[maybe_unused]] void DoesNotInheritFrom() {
  IntrusiveForwardList<NotAnItem> list;
  (void)list.front();
}

#endif  // PW_NC_TEST

TEST(IntrusiveForwardListTest, MoveItemsToVector) {
  pw::Vector<Item, 3> vec;
  vec.emplace_back(Item(1));
  vec.emplace_back(Item(2));
  vec.emplace_back(Item(3));
  List list;
  list.assign(vec.begin(), vec.end());

  auto iter = list.begin();
  for (const auto& item : vec) {
    EXPECT_NE(iter, list.end());
    if (iter == list.end()) {
      break;
    }
    EXPECT_EQ(item.GetNumber(), iter->GetNumber());
    ++iter;
  }
  list.clear();

  // TODO(b/313899658): Vector has an MSAN bug in its destructor when clearing
  // items that reference themselves. Workaround it by manually clearing.
  vec.clear();
}

TEST(IntrusiveForwardListItem, Compiles) {
  IntrusiveForwardListItem<bool> item(true);
  IntrusiveForwardList<IntrusiveForwardListItem<bool>> list;
  list.push_front(item);
  EXPECT_TRUE(list.begin()->item());
  list.clear();
}

}  // namespace
