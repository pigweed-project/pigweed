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

#include "pw_containers/forward_list.h"

#include <array>
#include <cstddef>
#include <functional>
#include <iterator>
#include <limits>
#include <type_traits>
#include <utility>

#include "pw_allocator/testing.h"
#include "pw_containers/internal/test_helpers.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::ForwardList;
using ::pw::allocator::test::AllocatorForTest;
using ::pw::containers::test::MoveOnly;

struct Point {
  int x = 0;
  int y = 0;

  Point() = default;
  Point(int x_val, int y_val) : x(x_val), y(y_val) {}

  bool operator==(const Point& other) const {
    return x == other.x && y == other.y;
  }
};

struct LifetimeItem {
  static int default_constructs;
  static int value_constructs;
  static int copy_constructs;
  static int move_constructs;
  static int destructions;

  static void Reset() {
    default_constructs = 0;
    value_constructs = 0;
    copy_constructs = 0;
    move_constructs = 0;
    destructions = 0;
  }

  static int TotalConstructs() {
    return default_constructs + value_constructs + copy_constructs +
           move_constructs;
  }

  int value = 0;

  LifetimeItem() { ++default_constructs; }
  explicit LifetimeItem(int v) : value(v) { ++value_constructs; }
  LifetimeItem(const LifetimeItem& other) : value(other.value) {
    ++copy_constructs;
  }
  LifetimeItem(LifetimeItem&& other) noexcept : value(other.value) {
    other.value = -1;
    ++move_constructs;
  }

  LifetimeItem& operator=(const LifetimeItem& other) = default;
  LifetimeItem& operator=(LifetimeItem&& other) noexcept = default;

  ~LifetimeItem() { ++destructions; }

  bool operator==(const LifetimeItem& other) const {
    return value == other.value;
  }
  bool operator<(const LifetimeItem& other) const {
    return value < other.value;
  }
};

int LifetimeItem::default_constructs = 0;
int LifetimeItem::value_constructs = 0;
int LifetimeItem::copy_constructs = 0;
int LifetimeItem::move_constructs = 0;
int LifetimeItem::destructions = 0;

template <typename T>
void ExpectElements(const ForwardList<T>& list,
                    std::initializer_list<T> expected) {
  auto it = list.begin();
  auto exp_it = expected.begin();
  while (it != list.end() && exp_it != expected.end()) {
    EXPECT_EQ(*it, *exp_it);
    ++it;
    ++exp_it;
  }
  EXPECT_EQ(it, list.end());
  EXPECT_EQ(exp_it, expected.end());
}

// -----------------------------------------------------------------------------
// Type Traits and Static Assertions
// -----------------------------------------------------------------------------

TEST(ForwardListTest, MemberTypes) {
  static_assert(std::is_same_v<ForwardList<int>::value_type, int>);
  static_assert(
      std::is_same_v<ForwardList<int>::allocator_type, pw::Allocator>);
  static_assert(std::is_same_v<ForwardList<int>::size_type, std::size_t>);
  static_assert(
      std::is_same_v<ForwardList<int>::difference_type, std::ptrdiff_t>);
  static_assert(std::is_same_v<ForwardList<int>::reference, int&>);
  static_assert(std::is_same_v<ForwardList<int>::const_reference, const int&>);
  static_assert(std::is_same_v<ForwardList<int>::pointer, int*>);
  static_assert(std::is_same_v<ForwardList<int>::const_pointer, const int*>);

  static_assert(std::is_same_v<ForwardList<int>::iterator::iterator_category,
                               std::forward_iterator_tag>);
  static_assert(
      std::is_same_v<ForwardList<int>::const_iterator::iterator_category,
                     std::forward_iterator_tag>);
}

TEST(ForwardListTest, NonCopyableMovable) {
  static_assert(!std::is_copy_constructible_v<ForwardList<int>>);
  static_assert(!std::is_copy_assignable_v<ForwardList<int>>);
  static_assert(std::is_move_constructible_v<ForwardList<int>>);
  static_assert(std::is_move_assignable_v<ForwardList<int>>);
}

// -----------------------------------------------------------------------------
// Construction, Destruction, and Assignment
// -----------------------------------------------------------------------------

TEST(ForwardListTest, ConstructEmpty) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  EXPECT_TRUE(list.empty());
  EXPECT_EQ(list.begin(), list.end());
  EXPECT_EQ(list.cbegin(), list.cend());
  EXPECT_EQ(&list.get_allocator(), &allocator);
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST(ForwardListTest, DestructorCleansUp) {
  AllocatorForTest<256> allocator;
  {
    ForwardList<int> list(allocator);
    list.push_front(1);
    list.push_front(2);
    list.push_front(3);
    EXPECT_GT(allocator.GetAllocated(), 0u);
  }
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST(ForwardListTest, MoveConstructTransfersAndRetainsAllocator) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list1(allocator);
  list1.push_front(3);
  list1.push_front(2);
  list1.push_front(1);

  ForwardList<int> list2(std::move(list1));

  // NOLINTBEGIN(bugprone-use-after-move)
  // Moved-from list is empty and retains its allocator
  EXPECT_TRUE(list1.empty());
  EXPECT_EQ(&list1.get_allocator(), &allocator);
  EXPECT_EQ(&list2.get_allocator(), &allocator);
  ExpectElements(list2, {1, 2, 3});

  // Moved-from list can still be used
  list1.push_front(10);
  ExpectElements(list1, {10});

  list1.clear();
  list2.clear();
  EXPECT_EQ(allocator.GetAllocated(), 0u);
  // NOLINTEND(bugprone-use-after-move)
}

TEST(ForwardListTest, MoveConstructEmpty) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list1(allocator);
  ForwardList<int> list2(std::move(list1));

  // NOLINTBEGIN(bugprone-use-after-move)
  EXPECT_TRUE(list1.empty());
  EXPECT_TRUE(list2.empty());
  EXPECT_EQ(&list1.get_allocator(), &allocator);
  EXPECT_EQ(&list2.get_allocator(), &allocator);
  // NOLINTEND(bugprone-use-after-move)
}

TEST(ForwardListTest, MoveAssign) {
  AllocatorForTest<256> allocator1;
  AllocatorForTest<256> allocator2;
  ForwardList<int> list1(allocator1);
  ForwardList<int> list2(allocator2);

  list1.push_front(2);
  list1.push_front(1);

  list2.push_front(5);
  list2.push_front(4);
  list2.push_front(3);

  list2 = std::move(list1);

  // NOLINTBEGIN(bugprone-use-after-move)
  EXPECT_TRUE(list1.empty());
  EXPECT_EQ(&list1.get_allocator(), &allocator1);
  EXPECT_EQ(&list2.get_allocator(), &allocator1);
  ExpectElements(list2, {1, 2});

  // Old elements of list2 on allocator2 should have been deallocated
  EXPECT_EQ(allocator2.GetAllocated(), 0u);

  // Moved-from list1 can still be used with its allocator
  list1.push_front(10);
  ExpectElements(list1, {10});

  list1.clear();
  list2.clear();
  EXPECT_EQ(allocator1.GetAllocated(), 0u);
  // NOLINTEND(bugprone-use-after-move)
}

TEST(ForwardListTest, MoveAssignEmptyToNonEmpty) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list1(allocator);
  ForwardList<int> list2(allocator);

  list2.push_front(2);
  list2.push_front(1);

  list2 = std::move(list1);

  EXPECT_TRUE(list1.empty());  // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(list2.empty());
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST(ForwardListTest, MoveAssignSelf) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);
  list.push_front(2);
  list.push_front(1);

  ForwardList<int>* list_ptr = &list;  // Use ptr to avoid self-move warnings
  list = std::move(*list_ptr);         // NOLINT(bugprone-use-after-move)

  ExpectElements(list, {1, 2});
}

// -----------------------------------------------------------------------------
// Iterators
// -----------------------------------------------------------------------------

TEST(ForwardListTest, IteratorDefaultConstructible) {
  ForwardList<int>::iterator it;
  ForwardList<int>::const_iterator cit;

  EXPECT_EQ(it, ForwardList<int>::iterator{});
  EXPECT_EQ(cit, ForwardList<int>::const_iterator{});
}

TEST(ForwardListTest, IteratorTraversal) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);
  list.push_front(3);
  list.push_front(2);
  list.push_front(1);

  auto it = list.begin();
  ASSERT_NE(it, list.end());
  EXPECT_EQ(*it, 1);

  auto post_inc = it++;
  EXPECT_EQ(*post_inc, 1);
  EXPECT_EQ(*it, 2);

  auto& pre_inc = ++it;
  EXPECT_EQ(&pre_inc, &it);
  EXPECT_EQ(*it, 3);

  ++it;
  EXPECT_EQ(it, list.end());
}

TEST(ForwardListTest, IteratorDereferenceAndArrow) {
  AllocatorForTest<256> allocator;
  ForwardList<Point> list(allocator);
  list.push_front(Point(3, 4));
  list.push_front(Point(1, 2));

  auto it = list.begin();
  EXPECT_EQ(it->x, 1);
  EXPECT_EQ(it->y, 2);
  EXPECT_EQ((*it).x, 1);

  // Mutation through non-const iterator
  it->x = 10;
  (*it).y = 20;
  EXPECT_EQ(it->x, 10);
  EXPECT_EQ(it->y, 20);
}

TEST(ForwardListTest, ConstIteratorAccess) {
  AllocatorForTest<256> allocator;
  ForwardList<Point> list(allocator);
  list.push_front(Point(3, 4));
  list.push_front(Point(1, 2));

  const auto& const_list = list;
  auto cit = const_list.cbegin();
  EXPECT_EQ(cit->x, 1);
  EXPECT_EQ(cit->y, 2);
  EXPECT_EQ((*cit).x, 1);
  EXPECT_EQ((*cit).y, 2);

  ++cit;
  EXPECT_EQ(cit->x, 3);
  EXPECT_EQ(cit->y, 4);

  ++cit;
  EXPECT_EQ(cit, const_list.cend());
  EXPECT_EQ(cit, const_list.end());
}

TEST(ForwardListTest, IteratorComparison) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);
  list.push_front(2);
  list.push_front(1);

  auto it = list.begin();
  auto cit = list.cbegin();

  EXPECT_TRUE(it == it);
  EXPECT_FALSE(it != it);
  EXPECT_TRUE(cit == cit);
  EXPECT_FALSE(cit != cit);

  EXPECT_TRUE(it == cit);
  EXPECT_TRUE(cit == it);
  EXPECT_FALSE(it != cit);
  EXPECT_FALSE(cit != it);

  ++it;
  EXPECT_FALSE(it == cit);
  EXPECT_TRUE(it != cit);
  EXPECT_FALSE(cit == it);
  EXPECT_TRUE(cit != it);
}

TEST(ForwardListTest, IteratorToConstIteratorConversion) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);
  list.push_front(2);
  list.push_front(1);

  ForwardList<int>::iterator it = list.begin();
  // Implicit conversion
  ForwardList<int>::const_iterator cit = it;
  EXPECT_EQ(cit, it);
  EXPECT_EQ(it, cit);
  EXPECT_EQ(*cit, 1);

  // Explicit conversion
  ForwardList<int>::const_iterator explicit_cit(it);
  EXPECT_EQ(explicit_cit, it);
  EXPECT_EQ(it, explicit_cit);

  // Conversion of before_begin
  ForwardList<int>::iterator b_it = list.before_begin();
  ForwardList<int>::const_iterator b_cit = b_it;
  EXPECT_EQ(b_cit, b_it);
  EXPECT_EQ(b_it, b_cit);
  EXPECT_EQ(std::next(b_cit), cit);
}

TEST(ForwardListTest, IteratorBeforeBegin) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);
  list.push_front(1);

  EXPECT_EQ(std::next(list.before_begin()), list.begin());
  EXPECT_EQ(std::next(list.cbefore_begin()), list.cbegin());

  const auto& const_list = list;
  EXPECT_EQ(std::next(const_list.before_begin()), const_list.begin());
  EXPECT_EQ(std::next(const_list.cbefore_begin()), const_list.cbegin());
}

TEST(ForwardListTest, RangeBasedForLoop) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);
  list.push_front(3);
  list.push_front(2);
  list.push_front(1);

  for (auto& val : list) {
    val *= 10;
  }
  ExpectElements(list, {10, 20, 30});

  const auto& const_list = list;
  int sum = 0;
  for (const auto& val : const_list) {
    sum += val;
  }
  EXPECT_EQ(sum, 60);
}

// -----------------------------------------------------------------------------
// Element Access and Capacity
// -----------------------------------------------------------------------------

TEST(ForwardListTest, Front) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);
  list.push_front(10);
  EXPECT_EQ(list.front(), 10);

  list.front() = 20;
  EXPECT_EQ(list.front(), 20);

  const auto& const_list = list;
  EXPECT_EQ(const_list.front(), 20);
}

TEST(ForwardListTest, Empty) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);
  EXPECT_TRUE(list.empty());

  list.push_front(1);
  EXPECT_FALSE(list.empty());

  list.pop_front();
  EXPECT_TRUE(list.empty());

  list.push_front(2);
  list.clear();
  EXPECT_TRUE(list.empty());
}

TEST(ForwardListTest, MaxSize) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);
  EXPECT_EQ(list.max_size(),
            static_cast<ForwardList<int>::size_type>(
                std::numeric_limits<ForwardList<int>::difference_type>::max()));
}

// -----------------------------------------------------------------------------
// Modifiers: Push, Emplace, Pop Front, Clear, Reset, Swap
// -----------------------------------------------------------------------------

TEST(ForwardListTest, PushFrontLvalueAndRvalue) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  const int a = 1;
  list.push_front(a);
  list.push_front(2);
  list.push_front(3);

  ExpectElements(list, {3, 2, 1});
}

TEST(ForwardListTest, TryPushFront) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  const int a = 1;
  EXPECT_TRUE(list.try_push_front(a));
  EXPECT_TRUE(list.try_push_front(2));

  ExpectElements(list, {2, 1});
}

TEST(ForwardListTest, EmplaceFront) {
  AllocatorForTest<256> allocator;
  ForwardList<Point> list(allocator);

  Point& p1 = list.emplace_front(1, 2);
  EXPECT_EQ(p1, Point(1, 2));

  Point& p2 = list.emplace_front(3, 4);
  EXPECT_EQ(p2, Point(3, 4));

  EXPECT_EQ(list.front(), Point(3, 4));
  ExpectElements(list, {Point(3, 4), Point(1, 2)});
}

TEST(ForwardListTest, TryEmplaceFront) {
  AllocatorForTest<256> allocator;
  ForwardList<Point> list(allocator);

  EXPECT_TRUE(list.try_emplace_front(1, 2));
  EXPECT_TRUE(list.try_emplace_front(3, 4));

  ExpectElements(list, {Point(3, 4), Point(1, 2)});
}

TEST(ForwardListTest, PopFront) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  list.push_front(3);
  list.push_front(2);
  list.push_front(1);

  list.pop_front();
  ExpectElements(list, {2, 3});

  list.pop_front();
  ExpectElements(list, {3});

  list.pop_front();
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST(ForwardListTest, PopFrontEmpty) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  list.pop_front();
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST(ForwardListTest, Clear) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  list.push_front(1);
  list.push_front(2);
  list.clear();
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(allocator.GetAllocated(), 0u);

  list.push_front(3);
  list.clear();
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST(ForwardListTest, Swap) {
  AllocatorForTest<256> allocator1;
  AllocatorForTest<256> allocator2;

  ForwardList<int> list1(allocator1);
  ForwardList<int> list2(allocator2);

  list1.push_front(2);
  list1.push_front(1);

  list2.push_front(5);
  list2.push_front(4);
  list2.push_front(3);

  list1.swap(list2);

  ExpectElements(list1, {3, 4, 5});
  ExpectElements(list2, {1, 2});
  EXPECT_EQ(&list1.get_allocator(), &allocator2);
  EXPECT_EQ(&list2.get_allocator(), &allocator1);

  list1.clear();
  list2.clear();

  EXPECT_EQ(allocator1.GetAllocated(), 0u);
  EXPECT_EQ(allocator2.GetAllocated(), 0u);
}

TEST(ForwardListTest, NonMemberSwap) {
  AllocatorForTest<256> allocator1;
  AllocatorForTest<256> allocator2;

  ForwardList<int> list1(allocator1);
  ForwardList<int> list2(allocator2);

  list1.push_front(2);
  list1.push_front(1);

  list2.push_front(5);
  list2.push_front(4);
  list2.push_front(3);

  using std::swap;
  swap(list1, list2);

  ExpectElements(list1, {3, 4, 5});
  ExpectElements(list2, {1, 2});
  EXPECT_EQ(&list1.get_allocator(), &allocator2);
  EXPECT_EQ(&list2.get_allocator(), &allocator1);

  list1.clear();
  list2.clear();

  EXPECT_EQ(allocator1.GetAllocated(), 0u);
  EXPECT_EQ(allocator2.GetAllocated(), 0u);
}

// -----------------------------------------------------------------------------
// Modifiers: Emplace and Insert After (accepting const_iterator)
// -----------------------------------------------------------------------------

TEST(ForwardListTest, EmplaceAfterAcceptsConstIterator) {
  AllocatorForTest<256> allocator;
  ForwardList<Point> list(allocator);

  auto it = list.emplace_after(list.cbefore_begin(), 1, 2);
  EXPECT_EQ(*it, Point(1, 2));

  it = list.emplace_after(list.cbegin(), 3, 4);
  EXPECT_EQ(*it, Point(3, 4));

  list.emplace_after(list.cbegin(), 5, 6);
  ExpectElements(list, {Point(1, 2), Point(5, 6), Point(3, 4)});
}

TEST(ForwardListTest, TryEmplaceAfter) {
  AllocatorForTest<256> allocator;
  ForwardList<Point> list(allocator);

  EXPECT_TRUE(list.try_emplace_after(list.cbefore_begin(), 1, 2));
  EXPECT_TRUE(list.try_emplace_after(list.cbegin(), 3, 4));

  ExpectElements(list, {Point(1, 2), Point(3, 4)});
}

TEST(ForwardListTest, InsertAfterValue) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  const int val1 = 1;
  auto it = list.insert_after(list.cbefore_begin(), val1);
  EXPECT_EQ(*it, 1);

  it = list.insert_after(it, 3);
  EXPECT_EQ(*it, 3);

  it = list.insert_after(list.cbegin(), 2);
  EXPECT_EQ(*it, 2);

  ExpectElements(list, {1, 2, 3});
}

TEST(ForwardListTest, TryInsertAfterValue) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  const int val1 = 1;
  EXPECT_TRUE(list.try_insert_after(list.cbefore_begin(), val1));
  EXPECT_TRUE(list.try_insert_after(list.cbegin(), 2));

  ExpectElements(list, {1, 2});
}

TEST(ForwardListTest, InsertAfterCount) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  auto it = list.insert_after(list.cbefore_begin(), 0, 99);
  EXPECT_EQ(it, list.before_begin());
  EXPECT_TRUE(list.empty());

  it = list.insert_after(list.cbefore_begin(), 3, 10);
  EXPECT_EQ(*it, 10);
  ExpectElements(list, {10, 10, 10});

  it = list.insert_after(list.cbegin(), 2, 20);
  EXPECT_EQ(*it, 20);
  ExpectElements(list, {10, 20, 20, 10, 10});
}

TEST(ForwardListTest, TryInsertAfterCount) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  EXPECT_TRUE(list.try_insert_after(list.cbefore_begin(), 0, 99));
  EXPECT_TRUE(list.empty());

  EXPECT_TRUE(list.try_insert_after(list.cbefore_begin(), 3, 10));
  ExpectElements(list, {10, 10, 10});
}

TEST(ForwardListTest, InsertAfterRange) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  std::array<int, 3> values = {1, 2, 3};

  auto it =
      list.insert_after(list.cbefore_begin(), values.begin(), values.begin());
  EXPECT_EQ(it, list.before_begin());
  EXPECT_TRUE(list.empty());

  it = list.insert_after(list.cbefore_begin(), values.begin(), values.end());
  EXPECT_EQ(*it, 3);
  ExpectElements(list, {1, 2, 3});

  std::array<int, 2> mid_values = {10, 20};
  it = list.insert_after(list.cbegin(), mid_values.begin(), mid_values.end());
  EXPECT_EQ(*it, 20);
  ExpectElements(list, {1, 10, 20, 2, 3});
}

TEST(ForwardListTest, TryInsertAfterRange) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  std::array<int, 3> values = {1, 2, 3};
  EXPECT_TRUE(list.try_insert_after(
      list.cbefore_begin(), values.begin(), values.end()));
  ExpectElements(list, {1, 2, 3});
}

TEST(ForwardListTest, InsertAfterInitializerList) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  auto it = list.insert_after(list.cbefore_begin(), {});
  EXPECT_EQ(it, list.before_begin());
  EXPECT_TRUE(list.empty());

  it = list.insert_after(list.cbefore_begin(), {1, 2, 3});
  EXPECT_EQ(*it, 3);
  ExpectElements(list, {1, 2, 3});

  it = list.insert_after(list.cbegin(), {10, 20});
  EXPECT_EQ(*it, 20);
  ExpectElements(list, {1, 10, 20, 2, 3});
}

TEST(ForwardListTest, TryInsertAfterInitializerList) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  EXPECT_TRUE(list.try_insert_after(list.cbefore_begin(), {1, 2, 3}));
  ExpectElements(list, {1, 2, 3});
}

// -----------------------------------------------------------------------------
// Modifiers: Assign and TryAssign
// -----------------------------------------------------------------------------

TEST(ForwardListTest, AssignCount) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  list.push_front(1);
  list.push_front(2);

  list.assign(3, 42);
  ExpectElements(list, {42, 42, 42});

  list.assign(0, 99);
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST(ForwardListTest, TryAssignCount) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  EXPECT_TRUE(list.try_assign(3, 10));
  ExpectElements(list, {10, 10, 10});
}

TEST(ForwardListTest, AssignRange) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  std::array<int, 4> values = {1, 2, 3, 4};
  list.assign(values.begin(), values.end());
  ExpectElements(list, {1, 2, 3, 4});

  list.assign(values.begin(), values.begin());
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST(ForwardListTest, TryAssignRange) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  std::array<int, 3> values = {10, 20, 30};
  EXPECT_TRUE(list.try_assign(values.begin(), values.end()));
  ExpectElements(list, {10, 20, 30});
}

TEST(ForwardListTest, AssignInitializerList) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  list.assign({5, 6, 7});
  ExpectElements(list, {5, 6, 7});

  list.assign({});
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST(ForwardListTest, TryAssignInitializerList) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  EXPECT_TRUE(list.try_assign({1, 2, 3}));
  ExpectElements(list, {1, 2, 3});
}

// -----------------------------------------------------------------------------
// Modifiers: Erase After and Resize
// -----------------------------------------------------------------------------

TEST(ForwardListTest, EraseAfterSingleAcceptsConstIterator) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  list.push_front(4);
  list.push_front(3);
  list.push_front(2);
  list.push_front(1);
  // 1, 2, 3, 4

  // Erase front element (after cbefore_begin)
  auto it = list.erase_after(list.cbefore_begin());
  EXPECT_EQ(*it, 2);
  ExpectElements(list, {2, 3, 4});

  // Erase middle element (after cbegin)
  it = list.erase_after(list.cbegin());
  EXPECT_EQ(*it, 4);
  ExpectElements(list, {2, 4});

  // Erase last element
  it = list.erase_after(list.cbegin());
  EXPECT_EQ(it, list.cend());
  ExpectElements(list, {2});
}

TEST(ForwardListTest, EraseAfterRangeAcceptsConstIterator) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  list.push_front(5);
  list.push_front(4);
  list.push_front(3);
  list.push_front(2);
  list.push_front(1);
  // 1, 2, 3, 4, 5

  // Erase empty range (first, next(first)) -> no-op
  auto it = list.erase_after(list.cbegin(), std::next(list.cbegin()));
  EXPECT_EQ(it, std::next(list.cbegin()));
  ExpectElements(list, {1, 2, 3, 4, 5});

  // Erase middle range: elements 2 and 3
  it = list.erase_after(list.cbegin(), std::next(list.cbegin(), 3));
  EXPECT_EQ(*it, 4);
  ExpectElements(list, {1, 4, 5});

  // Erase remaining entire list
  it = list.erase_after(list.cbefore_begin(), list.cend());
  EXPECT_EQ(it, list.cend());
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST(ForwardListTest, EraseAfterEmptyRangeSameIterator) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  list.push_front(3);
  list.push_front(2);
  list.push_front(1);
  // 1, 2, 3

  // (cbegin, cbegin)
  auto it = list.erase_after(list.cbegin(), list.cbegin());
  EXPECT_EQ(it, list.begin());
  ExpectElements(list, {1, 2, 3});

  // (middle, middle)
  it = list.erase_after(std::next(list.cbegin()), std::next(list.cbegin()));
  EXPECT_EQ(it, std::next(list.begin()));
  ExpectElements(list, {1, 2, 3});

  // On empty list
  ForwardList<int> empty_list(allocator);
  it = empty_list.erase_after(empty_list.cbefore_begin(),
                              empty_list.cbefore_begin());
  EXPECT_EQ(it, empty_list.before_begin());
  EXPECT_TRUE(empty_list.empty());
}

TEST(ForwardListTest, ResizeAndTryResizeGrow) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  EXPECT_TRUE(list.try_resize(3));
  ExpectElements(list, {0, 0, 0});

  list.resize(5, 42);
  ExpectElements(list, {0, 0, 0, 42, 42});
}

TEST(ForwardListTest, ResizeAndTryResizeShrink) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  list.push_front(5);
  list.push_front(4);
  list.push_front(3);
  list.push_front(2);
  list.push_front(1);

  EXPECT_TRUE(list.try_resize(3));
  ExpectElements(list, {1, 2, 3});

  list.resize(0);
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST(ForwardListTest, ResizeSameSize) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  list.push_front(2);
  list.push_front(1);

  EXPECT_TRUE(list.try_resize(2));
  ExpectElements(list, {1, 2});
}

// -----------------------------------------------------------------------------
// Splice Operations
// -----------------------------------------------------------------------------

TEST(ForwardListTest, SpliceAfterEntireListAcceptsConstIterator) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list1(allocator);
  ForwardList<int> list2(allocator);

  list1.push_front(1);
  list2.push_front(3);
  list2.push_front(2);

  // Splice lvalue
  list1.splice_after(list1.cbegin(), list2);
  EXPECT_TRUE(list2.empty());
  ExpectElements(list1, {1, 2, 3});

  // Splice rvalue
  list2.push_front(5);
  list2.push_front(4);
  list1.splice_after(list1.cbefore_begin(), std::move(list2));
  EXPECT_TRUE(list2.empty());  // NOLINT(bugprone-use-after-move)
  ExpectElements(list1, {4, 5, 1, 2, 3});

  // Splice empty list is a no-op
  list1.splice_after(list1.cbegin(), list2);
  ExpectElements(list1, {4, 5, 1, 2, 3});

  list1.clear();
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST(ForwardListTest, SpliceAfterSingleElementAcceptsConstIterator) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list1(allocator);
  ForwardList<int> list2(allocator);

  list1.push_front(1);
  list2.push_front(4);
  list2.push_front(3);
  list2.push_front(2);
  // list1: 1
  // list2: 2, 3, 4

  // Move element after list2.cbegin() (which is 3) to after list1.cbegin()
  list1.splice_after(list1.cbegin(), list2, list2.cbegin());
  ExpectElements(list1, {1, 3});
  ExpectElements(list2, {2, 4});

  // Rvalue overload: move element after list2.cbefore_begin() (which is 2)
  list1.splice_after(
      list1.cbefore_begin(), std::move(list2), list2.cbefore_begin());
  ExpectElements(list1, {2, 1, 3});
  ExpectElements(list2, {4});  // NOLINT(bugprone-use-after-move)

  list1.clear();
  list2.clear();
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST(ForwardListTest, SpliceAfterRangeAcceptsConstIterator) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list1(allocator);
  ForwardList<int> list2(allocator);

  list1.push_front(1);
  list2.push_front(5);
  list2.push_front(4);
  list2.push_front(3);
  list2.push_front(2);
  // list1: 1
  // list2: 2, 3, 4, 5

  // Splice range (cbegin(), next(cbegin(), 3)) from list2 -> elements 3, 4
  list1.splice_after(
      list1.cbegin(), list2, list2.cbegin(), std::next(list2.cbegin(), 3));
  ExpectElements(list1, {1, 3, 4});
  ExpectElements(list2, {2, 5});

  // Rvalue overload: splice range (cbefore_begin(), cend()) -> all elements of
  // list2
  list1.splice_after(list1.cbefore_begin(),
                     std::move(list2),
                     list2.cbefore_begin(),
                     list2.cend());
  ExpectElements(list1, {2, 5, 1, 3, 4});
  EXPECT_TRUE(list2.empty());  // NOLINT(bugprone-use-after-move)

  list1.clear();
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST(ForwardListTest, SpliceAfterSelf) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  list.push_front(4);
  list.push_front(3);
  list.push_front(2);
  list.push_front(1);
  // 1, 2, 3, 4

  // Move element after 2 (3) to after 4 (the end) -> {1, 2, 4, 3}
  list.splice_after(std::next(list.begin(), 3), list, std::next(list.begin()));
  ExpectElements(list, {1, 2, 4, 3});

  // Move element after 4 (3) to after 1 -> {1, 3, 2, 4}
  list.splice_after(list.begin(), list, std::next(list.begin(), 2));
  ExpectElements(list, {1, 3, 2, 4});

  // Splice single element with pos == it is a no-op
  list.splice_after(list.begin(), list, list.begin());
  ExpectElements(list, {1, 3, 2, 4});

  // Splice single element with pos == next(it) is a no-op
  list.splice_after(std::next(list.begin()), list, list.begin());
  ExpectElements(list, {1, 3, 2, 4});

  // Empty range within same list (first == last) is a no-op
  list.splice_after(list.begin(), list, list.begin(), list.begin());
  ExpectElements(list, {1, 3, 2, 4});

  // Entire list self-splice is a no-op
  list.splice_after(list.begin(), list);
  ExpectElements(list, {1, 3, 2, 4});

  list.clear();
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST(ForwardListTest, SpliceAfterEmptyRangeDifferentLists) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list1(allocator);
  ForwardList<int> list2(allocator);

  list1.push_front(2);
  list1.push_front(1);

  list2.push_front(4);
  list2.push_front(3);

  // first == last should be a no-op
  list1.splice_after(list1.begin(), list2, list2.begin(), list2.begin());
  ExpectElements(list1, {1, 2});
  ExpectElements(list2, {3, 4});

  list1.clear();
  list2.clear();
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

// -----------------------------------------------------------------------------
// List Operations (Algorithms)
// -----------------------------------------------------------------------------

TEST(ForwardListTest, Remove) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  list.push_front(1);
  list.push_front(2);
  list.push_front(1);
  list.push_front(3);
  list.push_front(1);
  // 1, 3, 1, 2, 1

  // Non-existent value
  EXPECT_EQ(list.remove(99), 0u);
  ExpectElements(list, {1, 3, 1, 2, 1});

  // Remove multiple occurrences
  EXPECT_EQ(list.remove(1), 3u);
  ExpectElements(list, {3, 2});

  // Remove remaining
  EXPECT_EQ(list.remove(3), 1u);
  EXPECT_EQ(list.remove(2), 1u);
  EXPECT_TRUE(list.empty());

  // Remove on empty list is safe
  EXPECT_EQ(list.remove(1), 0u);
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST(ForwardListTest, RemoveSelfReference) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  list.push_front(1);
  list.push_front(2);
  list.push_front(1);
  list.push_front(3);
  list.push_front(1);
  // 1, 3, 1, 2, 1

  // Passing reference to first element
  EXPECT_EQ(list.remove(list.front()), 3u);
  ExpectElements(list, {3, 2});

  // Passing reference to element in middle
  EXPECT_EQ(list.remove(*std::next(list.begin())), 1u);
  ExpectElements(list, {3});

  // Single-element list self-referencing remove
  EXPECT_EQ(list.remove(list.front()), 1u);
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST(ForwardListTest, RemoveMoveOnlySelfReference) {
  AllocatorForTest<512> allocator;
  ForwardList<MoveOnly> list(allocator);

  list.emplace_front(1);
  list.emplace_front(2);
  list.emplace_front(1);
  list.emplace_front(3);
  list.emplace_front(1);
  // 1, 3, 1, 2, 1

  EXPECT_EQ(list.remove(list.front()), 3u);
  EXPECT_EQ(list.front().value, 3);
  list.pop_front();
  EXPECT_EQ(list.front().value, 2);
  list.pop_front();
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST(ForwardListTest, RemoveIf) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  list.push_front(5);
  list.push_front(4);
  list.push_front(3);
  list.push_front(2);
  list.push_front(1);
  // 1, 2, 3, 4, 5

  // Remove even numbers
  EXPECT_EQ(list.remove_if([](int x) { return x % 2 == 0; }), 2u);
  ExpectElements(list, {1, 3, 5});

  // Remove numbers > 2
  EXPECT_EQ(list.remove_if([](int x) { return x > 2; }), 2u);
  ExpectElements(list, {1});

  // Remove all
  EXPECT_EQ(list.remove_if([](int) { return true; }), 1u);
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST(ForwardListTest, Unique) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  // Empty list
  EXPECT_EQ(list.unique(), 0u);
  EXPECT_TRUE(list.empty());

  // Single element
  list.push_front(1);
  EXPECT_EQ(list.unique(), 0u);
  ExpectElements(list, {1});

  // Consecutive duplicates
  list.clear();
  list.push_front(1);
  list.push_front(1);
  list.push_front(3);
  list.push_front(2);
  list.push_front(2);
  list.push_front(2);
  list.push_front(1);
  list.push_front(1);
  // 1, 1, 2, 2, 2, 3, 1, 1

  EXPECT_EQ(list.unique(), 4u);
  ExpectElements(list, {1, 2, 3, 1});

  list.clear();
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST(ForwardListTest, UniqueCustomPredicate) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  list.push_front(6);
  list.push_front(4);
  list.push_front(3);
  list.push_front(1);
  list.push_front(2);
  // 2, 1, 3, 4, 6

  // Deduplicate consecutive elements with the same parity
  EXPECT_EQ(list.unique([](int a, int b) { return (a % 2) == (b % 2); }), 2u);
  ExpectElements(list, {2, 1, 4});

  list.clear();
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST(ForwardListTest, Merge) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list1(allocator);
  ForwardList<int> list2(allocator);

  list1.push_front(5);
  list1.push_front(3);
  list1.push_front(1);
  // 1, 3, 5

  list2.push_front(6);
  list2.push_front(4);
  list2.push_front(2);
  // 2, 4, 6

  list1.merge(list2);
  EXPECT_TRUE(list2.empty());
  ExpectElements(list1, {1, 2, 3, 4, 5, 6});

  // Merge rvalue
  list2.push_front(7);
  list2.push_front(0);
  list1.merge(std::move(list2));
  EXPECT_TRUE(list2.empty());  // NOLINT(bugprone-use-after-move)
  ExpectElements(list1, {0, 1, 2, 3, 4, 5, 6, 7});

  // Merge empty list
  list1.merge(list2);
  ExpectElements(list1, {0, 1, 2, 3, 4, 5, 6, 7});

  list1.clear();
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST(ForwardListTest, MergeCustomComparator) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list1(allocator);
  ForwardList<int> list2(allocator);

  list1.push_front(1);
  list1.push_front(3);
  list1.push_front(5);
  // 5, 3, 1 (descending)

  list2.push_front(2);
  list2.push_front(4);
  list2.push_front(6);
  // 6, 4, 2 (descending)

  list1.merge(list2, std::greater<>());
  EXPECT_TRUE(list2.empty());
  ExpectElements(list1, {6, 5, 4, 3, 2, 1});

  // Merge rvalue with custom comparator
  list2.push_front(0);
  list2.push_front(7);
  list1.merge(std::move(list2), std::greater<>());
  EXPECT_TRUE(list2.empty());  // NOLINT(bugprone-use-after-move)
  ExpectElements(list1, {7, 6, 5, 4, 3, 2, 1, 0});

  list1.clear();
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST(ForwardListTest, MergeSelf) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  list.push_front(5);
  list.push_front(3);
  list.push_front(1);

  // Self-merge lvalue
  list.merge(list);
  ExpectElements(list, {1, 3, 5});

  // Self-merge with custom comparator
  list.merge(list, std::less<>());
  ExpectElements(list, {1, 3, 5});

  // Self-merge rvalue
  list.merge(std::move(list));
  ExpectElements(list, {1, 3, 5});  // NOLINT(bugprone-use-after-move)

  list.clear();
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST(ForwardListTest, Sort) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  // Empty list
  list.sort();
  EXPECT_TRUE(list.empty());

  // Single element
  list.push_front(1);
  list.sort();
  ExpectElements(list, {1});

  // Multiple elements with duplicates
  list.push_front(3);
  list.push_front(1);
  list.push_front(4);
  list.push_front(2);
  list.push_front(5);
  list.push_front(2);
  // 2, 5, 2, 4, 1, 3, 1

  list.sort();
  ExpectElements(list, {1, 1, 2, 2, 3, 4, 5});

  // Sort with custom comparator (descending)
  list.sort(std::greater<>());
  ExpectElements(list, {5, 4, 3, 2, 2, 1, 1});

  list.clear();
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST(ForwardListTest, Reverse) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  // Empty list
  list.reverse();
  EXPECT_TRUE(list.empty());

  // Single element
  list.push_front(1);
  list.reverse();
  ExpectElements(list, {1});

  // Two elements
  list.push_front(2);
  // 2, 1
  list.reverse();
  ExpectElements(list, {1, 2});

  // Multiple elements
  list.push_front(0);
  list.push_front(-1);
  // -1, 0, 1, 2
  list.reverse();
  ExpectElements(list, {2, 1, 0, -1});

  list.clear();
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

// -----------------------------------------------------------------------------
// Lifetime, Move-Only Types, and Fallible Allocation
// -----------------------------------------------------------------------------

TEST(ForwardListTest, LifetimeTracking) {
  LifetimeItem::Reset();
  AllocatorForTest<512> allocator;
  {
    ForwardList<LifetimeItem> list(allocator);
    list.push_front(LifetimeItem(1));
    list.emplace_front(2);
    list.insert_after(list.cbegin(), LifetimeItem(3));
    // list: 2, 3, 1

    list.pop_front();
    // list: 3, 1

    list.erase_after(list.cbegin());
    // list: 3

    list.resize(3, LifetimeItem(10));
    // list: 3, 10, 10

    list.remove(LifetimeItem(10));
    // list: 3

    EXPECT_EQ(LifetimeItem::TotalConstructs() - LifetimeItem::destructions, 1);

    list.clear();
    EXPECT_TRUE(list.empty());
    EXPECT_EQ(LifetimeItem::TotalConstructs() - LifetimeItem::destructions, 0);
  }

  EXPECT_EQ(allocator.GetAllocated(), 0u);
  EXPECT_GT(LifetimeItem::TotalConstructs(), 0);
  EXPECT_EQ(LifetimeItem::TotalConstructs(), LifetimeItem::destructions);
}

TEST(ForwardListTest, ResizeDefaultConstructionLifetime) {
  LifetimeItem::Reset();
  AllocatorForTest<512> allocator;
  {
    ForwardList<LifetimeItem> list(allocator);

    // Initial resize should default-construct 3 elements, no copies
    list.resize(3);
    EXPECT_EQ(LifetimeItem::default_constructs, 3);
    EXPECT_EQ(LifetimeItem::value_constructs, 0);
    EXPECT_EQ(LifetimeItem::copy_constructs, 0);
    EXPECT_EQ(LifetimeItem::move_constructs, 0);
    EXPECT_EQ(LifetimeItem::destructions, 0);
    EXPECT_EQ(LifetimeItem::TotalConstructs() - LifetimeItem::destructions, 3);

    // Growing should default-construct 2 more elements, no copies
    list.resize(5);
    EXPECT_EQ(LifetimeItem::default_constructs, 5);
    EXPECT_EQ(LifetimeItem::copy_constructs, 0);
    EXPECT_EQ(LifetimeItem::destructions, 0);
    EXPECT_EQ(LifetimeItem::TotalConstructs() - LifetimeItem::destructions, 5);

    // Shrinking should destruct 3 elements
    list.resize(2);
    EXPECT_EQ(LifetimeItem::destructions, 3);
    EXPECT_EQ(LifetimeItem::TotalConstructs() - LifetimeItem::destructions, 2);

    list.clear();
    EXPECT_TRUE(list.empty());
    EXPECT_EQ(LifetimeItem::destructions, 5);
    EXPECT_EQ(LifetimeItem::TotalConstructs() - LifetimeItem::destructions, 0);
  }

  EXPECT_EQ(allocator.GetAllocated(), 0u);
  EXPECT_EQ(LifetimeItem::TotalConstructs(), LifetimeItem::destructions);
}

TEST(ForwardListTest, MoveOnlyType) {
  AllocatorForTest<512> allocator;
  ForwardList<MoveOnly> list(allocator);

  list.emplace_front(1);
  list.push_front(MoveOnly(2));
  list.emplace_after(list.cbegin(), 3);
  list.insert_after(list.cbegin(), MoveOnly(4));
  // 2, 4, 3, 1

  EXPECT_EQ(list.front().value, 2);

  list.reverse();
  // 1, 3, 4, 2
  EXPECT_EQ(list.front().value, 1);

  ForwardList<MoveOnly> list2(std::move(list));
  EXPECT_TRUE(list.empty());  // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(list2.front().value, 1);

  ForwardList<MoveOnly> list3(allocator);
  list3 = std::move(list2);
  EXPECT_TRUE(list2.empty());  // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(list3.front().value, 1);

  list3.pop_front();
  list3.erase_after(list3.cbegin());
  list3.clear();
  EXPECT_TRUE(list3.empty());
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

struct MoveOnlyDefaultConstructible {
  int value = 42;
  MoveOnlyDefaultConstructible() = default;
  explicit MoveOnlyDefaultConstructible(int v) : value(v) {}
  MoveOnlyDefaultConstructible(const MoveOnlyDefaultConstructible&) = delete;
  MoveOnlyDefaultConstructible& operator=(const MoveOnlyDefaultConstructible&) =
      delete;
  MoveOnlyDefaultConstructible(MoveOnlyDefaultConstructible&&) noexcept =
      default;
  MoveOnlyDefaultConstructible& operator=(
      MoveOnlyDefaultConstructible&&) noexcept = default;

  bool operator==(const MoveOnlyDefaultConstructible& other) const {
    return value == other.value;
  }
};

TEST(ForwardListTest, MoveOnlyResize) {
  AllocatorForTest<512> allocator;
  ForwardList<MoveOnlyDefaultConstructible> list(allocator);

  EXPECT_TRUE(list.try_resize(3));
  EXPECT_EQ(list.front().value, 42);

  list.resize(1);
  EXPECT_EQ(list.front().value, 42);

  list.resize(0);
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST(ForwardListTest, TryOperationsReturnFalseOnExhaustion) {
  AllocatorForTest<256> allocator;
  ForwardList<int> list(allocator);

  list.push_front(1);
  allocator.Exhaust();

  // Try operations must return false and leave the container unchanged
  EXPECT_FALSE(list.try_emplace_front(2));
  ExpectElements(list, {1});

  EXPECT_FALSE(list.try_push_front(3));
  ExpectElements(list, {1});

  EXPECT_FALSE(list.try_emplace_after(list.cbegin(), 4));
  ExpectElements(list, {1});

  EXPECT_FALSE(list.try_insert_after(list.cbegin(), 5));
  ExpectElements(list, {1});

  EXPECT_FALSE(list.try_insert_after(list.cbegin(), 3, 10));
  ExpectElements(list, {1});

  std::array<int, 2> values = {20, 30};
  EXPECT_FALSE(
      list.try_insert_after(list.cbegin(), values.begin(), values.end()));
  ExpectElements(list, {1});

  EXPECT_FALSE(list.try_insert_after(list.cbegin(), {40, 50}));
  ExpectElements(list, {1});

  EXPECT_FALSE(list.try_resize(3, 100));
  ExpectElements(list, {1});

  EXPECT_FALSE(list.try_assign(3, 200));
  ExpectElements(list, {1});

  EXPECT_FALSE(list.try_assign(values.begin(), values.end()));
  ExpectElements(list, {1});

  EXPECT_FALSE(list.try_assign({300, 400}));
  ExpectElements(list, {1});
}

class FailingAllocator : public pw::Allocator {
 public:
  explicit FailingAllocator(pw::Allocator& delegate,
                            size_t successful_allocations)
      : Allocator(delegate.capabilities()),
        delegate_(delegate),
        remaining_allocations_(successful_allocations) {}

  void set_remaining_allocations(size_t n) { remaining_allocations_ = n; }

 private:
  void* DoAllocate(pw::allocator::Layout layout) override {
    if (remaining_allocations_ == 0) {
      return nullptr;
    }
    --remaining_allocations_;
    return delegate_.Allocate(layout);
  }

  void DoDeallocate(void* ptr) override { delegate_.Deallocate(ptr); }

  bool DoResize(void* ptr, size_t new_size) override {
    return delegate_.Resize(ptr, new_size);
  }

  pw::Allocator& delegate_;
  size_t remaining_allocations_;
};

TEST(ForwardListTest, TryResizeRollbackOnPartialAllocationFailure) {
  LifetimeItem::Reset();
  AllocatorForTest<512> backing_allocator;
  FailingAllocator failing_allocator(backing_allocator, 100);
  {
    ForwardList<LifetimeItem> list(failing_allocator);
    list.push_front(LifetimeItem(2));
    list.push_front(LifetimeItem(1));
    // list: 1, 2
    const size_t initial_bytes = backing_allocator.GetAllocated();
    EXPECT_GT(initial_bytes, 0u);

    // Allow only 2 additional allocations before failing.
    // Resizing from 2 to 5 requires 3 allocations, so the 3rd fails.
    failing_allocator.set_remaining_allocations(2);
    EXPECT_FALSE(list.try_resize(5, LifetimeItem(99)));

    // Rollback must restore original elements, free the partially allocated
    // nodes, and destruct the partially allocated items.
    ExpectElements(list, {LifetimeItem(1), LifetimeItem(2)});
    EXPECT_EQ(backing_allocator.GetAllocated(), initial_bytes);
    EXPECT_EQ(LifetimeItem::TotalConstructs() - LifetimeItem::destructions, 2);

    // Test rollback from an empty list
    ForwardList<LifetimeItem> empty_list(failing_allocator);
    failing_allocator.set_remaining_allocations(1);
    EXPECT_FALSE(empty_list.try_resize(3, LifetimeItem(99)));
    EXPECT_TRUE(empty_list.empty());
  }
  EXPECT_EQ(backing_allocator.GetAllocated(), 0u);
  EXPECT_EQ(LifetimeItem::TotalConstructs(), LifetimeItem::destructions);
}

TEST(ForwardListTest, TryInsertAfterCountRollbackOnPartialAllocationFailure) {
  LifetimeItem::Reset();
  AllocatorForTest<512> backing_allocator;
  FailingAllocator failing_allocator(backing_allocator, 100);
  {
    ForwardList<LifetimeItem> list(failing_allocator);
    list.push_front(LifetimeItem(2));
    list.push_front(LifetimeItem(1));
    // list: 1, 2
    const size_t initial_bytes = backing_allocator.GetAllocated();

    // Inserting 4 copies after begin(), but only 2 allocations succeed.
    failing_allocator.set_remaining_allocations(2);
    EXPECT_FALSE(list.try_insert_after(list.cbegin(), 4, LifetimeItem(99)));

    ExpectElements(list, {LifetimeItem(1), LifetimeItem(2)});
    EXPECT_EQ(backing_allocator.GetAllocated(), initial_bytes);
    EXPECT_EQ(LifetimeItem::TotalConstructs() - LifetimeItem::destructions, 2);

    // Inserting 3 copies after before_begin(), but only 1 allocation succeeds.
    failing_allocator.set_remaining_allocations(1);
    EXPECT_FALSE(
        list.try_insert_after(list.cbefore_begin(), 3, LifetimeItem(99)));

    ExpectElements(list, {LifetimeItem(1), LifetimeItem(2)});
    EXPECT_EQ(backing_allocator.GetAllocated(), initial_bytes);
    EXPECT_EQ(LifetimeItem::TotalConstructs() - LifetimeItem::destructions, 2);
  }
  EXPECT_EQ(backing_allocator.GetAllocated(), 0u);
  EXPECT_EQ(LifetimeItem::TotalConstructs(), LifetimeItem::destructions);
}

TEST(ForwardListTest, TryInsertAfterRangeRollbackOnPartialAllocationFailure) {
  LifetimeItem::Reset();
  AllocatorForTest<512> backing_allocator;
  FailingAllocator failing_allocator(backing_allocator, 100);
  {
    ForwardList<LifetimeItem> list(failing_allocator);
    list.push_front(LifetimeItem(2));
    list.push_front(LifetimeItem(1));
    const size_t initial_bytes = backing_allocator.GetAllocated();

    std::array<LifetimeItem, 4> values = {
        LifetimeItem(10), LifetimeItem(20), LifetimeItem(30), LifetimeItem(40)};

    // Inserting range of 4 items, but only 2 allocations succeed.
    failing_allocator.set_remaining_allocations(2);
    EXPECT_FALSE(
        list.try_insert_after(list.cbegin(), values.begin(), values.end()));

    ExpectElements(list, {LifetimeItem(1), LifetimeItem(2)});
    EXPECT_EQ(backing_allocator.GetAllocated(), initial_bytes);
  }
  EXPECT_EQ(backing_allocator.GetAllocated(), 0u);
  EXPECT_EQ(LifetimeItem::TotalConstructs(), LifetimeItem::destructions);
}

TEST(ForwardListTest, TryAssignRollbackOnPartialAllocationFailure) {
  LifetimeItem::Reset();
  AllocatorForTest<512> backing_allocator;
  FailingAllocator failing_allocator(backing_allocator, 100);
  {
    ForwardList<LifetimeItem> list(failing_allocator);
    list.push_front(LifetimeItem(3));
    list.push_front(LifetimeItem(2));
    list.push_front(LifetimeItem(1));
    const size_t initial_bytes = backing_allocator.GetAllocated();

    // Assigning 5 copies, but only 2 allocations succeed in the temp list.
    failing_allocator.set_remaining_allocations(2);
    EXPECT_FALSE(list.try_assign(5, LifetimeItem(99)));

    // Original list remains unchanged.
    ExpectElements(list, {LifetimeItem(1), LifetimeItem(2), LifetimeItem(3)});
    EXPECT_EQ(backing_allocator.GetAllocated(), initial_bytes);

    // Assigning range of 4 items, but only 2 allocations succeed.
    std::array<LifetimeItem, 4> values = {
        LifetimeItem(10), LifetimeItem(20), LifetimeItem(30), LifetimeItem(40)};
    failing_allocator.set_remaining_allocations(2);
    EXPECT_FALSE(list.try_assign(values.begin(), values.end()));

    ExpectElements(list, {LifetimeItem(1), LifetimeItem(2), LifetimeItem(3)});
    EXPECT_EQ(backing_allocator.GetAllocated(), initial_bytes);
  }
  EXPECT_EQ(backing_allocator.GetAllocated(), 0u);
  EXPECT_EQ(LifetimeItem::TotalConstructs(), LifetimeItem::destructions);
}

}  // namespace
