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
#pragma once

#include <algorithm>  // std::min, std::max_element
#include <array>
#include <initializer_list>
#include <iterator>     // std::input_iterator_tag
#include <type_traits>  // std::is_base_of_v, std::is_same_v, etc.

#include "lib/stdcompat/type_traits.h"
#include "pw_containers/algorithm.h"
#include "pw_containers/internal/test_helpers.h"
#include "pw_unit_test/framework.h"

// Common tests for containers. The container type is injected via a test
// fixture created in the test source. The test fixture must provide a
// default-initializable `Container<T>` type that can be instantiated in the
// test and inherit from CommonTestFixture<DerivedFixture>. This is necessary
// because tests need to declare container variables, but containers have
// differing template and constructor parameters.

// Instantiates a set of tests for deques.
#define PW_CONTAINERS_COMMON_DEQUE_TESTS(f)                                    \
  TEST_F(f, Move_BothEmpty) { Move_BothEmpty(); }                              \
  TEST_F(f, Move_EmptyToNonEmpty) { Move_EmptyToNonEmpty(); }                  \
  TEST_F(f, Move_NonEmptyToEmpty) { Move_NonEmptyToEmpty(); }                  \
  TEST_F(f, Move_BothNonEmpty) { Move_BothNonEmpty(); }                        \
                                                                               \
  TEST_F(f, Destructor_Empty) { Destructor_Empty(); }                          \
  TEST_F(f, Destructor_NonEmpty) { Destructor_NonEmpty(); }                    \
                                                                               \
  TEST_F(f, Assign_ZeroCopies) { Assign_ZeroCopies(); }                        \
  TEST_F(f, Assign_MultipleCopies) { Assign_MultipleCopies(); }                \
  TEST_F(f, Assign_ForwardIterator) { Assign_ForwardIterator(); }              \
  TEST_F(f, Assign_InputIterator) { Assign_InputIterator(); }                  \
  TEST_F(f, Assign_InitializerList) { Assign_InitializerList(); }              \
                                                                               \
  TEST_F(f, Access_Iterator) { Access_Iterator(); }                            \
  TEST_F(f, Access_ConstIterator) { Access_ConstIterator(); }                  \
  TEST_F(f, Access_Empty) { Access_Empty(); }                                  \
  TEST_F(f, Access_DequeContiguousData) { Access_DequeContiguousData(); }      \
  TEST_F(f, Access_DequeConstContiguousData) {                                 \
    Access_DequeConstContiguousData();                                         \
  }                                                                            \
                                                                               \
  TEST_F(f, Modify_ClearNonEmpty) { Modify_ClearNonEmpty(); }                  \
  TEST_F(f, Modify_PushBackCopy) { Modify_PushBackCopy(); }                    \
  TEST_F(f, Modify_PushBackMove) { Modify_PushBackMove(); }                    \
  TEST_F(f, Modify_EmplaceBack) { Modify_EmplaceBack(); }                      \
  TEST_F(f, Modify_DequeWrapForwards) { Modify_DequeWrapForwards(); }          \
  TEST_F(f, Modify_DequeWrapBackwards) { Modify_DequeWrapBackwards(); }        \
  TEST_F(f, Modify_PushFrontCopy) { Modify_PushFrontCopy(); }                  \
  TEST_F(f, Modify_PushFrontMove) { Modify_PushFrontMove(); }                  \
  TEST_F(f, Modify_EmplaceFront) { Modify_EmplaceFront(); }                    \
  TEST_F(f, Modify_PopBack) { Modify_PopBack(); }                              \
  TEST_F(f, Modify_PopFront) { Modify_PopFront(); }                            \
  TEST_F(f, Modify_ResizeLarger) { Modify_ResizeLarger(); }                    \
  TEST_F(f, Modify_ResizeSmaller) { Modify_ResizeSmaller(); }                  \
  TEST_F(f, Modify_ResizeZero) { Modify_ResizeZero(); }                        \
  TEST_F(f, Modify_Erase_FirstElement) { Modify_Erase_FirstElement(); }        \
  TEST_F(f, Modify_Erase_LastElement) { Modify_Erase_LastElement(); }          \
  TEST_F(f, Modify_Erase_MiddleElement) { Modify_Erase_MiddleElement(); }      \
  TEST_F(f, Modify_Erase_OnlyElement) { Modify_Erase_OnlyElement(); }          \
  TEST_F(f, Modify_Erase_AfterPopFront) { Modify_Erase_AfterPopFront(); }      \
  TEST_F(f, Modify_Erase_MoveOnly) { Modify_Erase_MoveOnly(); }                \
  TEST_F(f, Modify_EraseRange_ZeroElements) {                                  \
    Modify_EraseRange_ZeroElements();                                          \
  }                                                                            \
  TEST_F(f, Modify_EraseRange_OneElement) { Modify_EraseRange_OneElement(); }  \
  TEST_F(f, Modify_EraseRange_ToTheBeginning) {                                \
    Modify_EraseRange_ToTheBeginning();                                        \
  }                                                                            \
  TEST_F(f, Modify_EraseRange_ToTheEnd) { Modify_EraseRange_ToTheEnd(); }      \
  TEST_F(f, Modify_EraseRange_Everything) { Modify_EraseRange_Everything(); }  \
  TEST_F(f, Modify_EraseRange_AfterPopFront) {                                 \
    Modify_EraseRange_AfterPopFront();                                         \
  }                                                                            \
                                                                               \
  TEST_F(f, Modify_Emplace_Empty) { Modify_Emplace_Empty(); }                  \
  TEST_F(f, Modify_Emplace_Front) { Modify_Emplace_Front(); }                  \
  TEST_F(f, Modify_Emplace_Back) { Modify_Emplace_Back(); }                    \
  TEST_F(f, Modify_Emplace_Middle) { Modify_Emplace_Middle(); }                \
  TEST_F(f, Modify_Emplace_BeginPlusOne) { Modify_Emplace_BeginPlusOne(); }    \
  TEST_F(f, Modify_Emplace_EndMinusOne) { Modify_Emplace_EndMinusOne(); }      \
                                                                               \
  TEST_F(f, Modify_InsertCopy) { Modify_InsertCopy(); }                        \
  TEST_F(f, Modify_InsertMove) { Modify_InsertMove(); }                        \
  TEST_F(f, Modify_InsertCopies_NearBegin_FewerThanBefore) {                   \
    Modify_InsertCopies_NearBegin_FewerThanBefore();                           \
  }                                                                            \
  TEST_F(f, Modify_InsertCopies_NearBegin_SameAsBefore) {                      \
    Modify_InsertCopies_NearBegin_SameAsBefore();                              \
  }                                                                            \
  TEST_F(f, Modify_InsertCopies_NearBegin_MoreThanBefore) {                    \
    Modify_InsertCopies_NearBegin_MoreThanBefore();                            \
  }                                                                            \
  TEST_F(f, Modify_InsertCopies_NearEnd_FewerThanAfter) {                      \
    Modify_InsertCopies_NearEnd_FewerThanAfter();                              \
  }                                                                            \
  TEST_F(f, Modify_InsertCopies_NearEnd_SameAsAfter) {                         \
    Modify_InsertCopies_NearEnd_SameAsAfter();                                 \
  }                                                                            \
  TEST_F(f, Modify_InsertCopies_NearEnd_MoreThanAfter) {                       \
    Modify_InsertCopies_NearEnd_MoreThanAfter();                               \
  }                                                                            \
  TEST_F(f, Modify_InsertCopies_AtBegin_1) {                                   \
    Modify_InsertCopies_AtBegin_1();                                           \
  }                                                                            \
  TEST_F(f, Modify_InsertCopies_AtBegin_2) {                                   \
    Modify_InsertCopies_AtBegin_2();                                           \
  }                                                                            \
  TEST_F(f, Modify_InsertCopies_AtEnd_1) { Modify_InsertCopies_AtEnd_1(); }    \
  TEST_F(f, Modify_InsertCopies_AtEnd_2) { Modify_InsertCopies_AtEnd_2(); }    \
  TEST_F(f, Modify_InsertIterators_NearBegin_FewerThanBefore) {                \
    Modify_InsertIterators_NearBegin_FewerThanBefore();                        \
  }                                                                            \
  TEST_F(f, Modify_InsertIterators_NearBegin_SameAsBefore) {                   \
    Modify_InsertIterators_NearBegin_SameAsBefore();                           \
  }                                                                            \
  TEST_F(f, Modify_InsertIterators_NearBegin_MoreThanBefore) {                 \
    Modify_InsertIterators_NearBegin_MoreThanBefore();                         \
  }                                                                            \
  TEST_F(f, Modify_InsertIterators_NearEnd_FewerThanAfter) {                   \
    Modify_InsertIterators_NearEnd_FewerThanAfter();                           \
  }                                                                            \
  TEST_F(f, Modify_InsertIterators_NearEnd_SameAsAfter) {                      \
    Modify_InsertIterators_NearEnd_SameAsAfter();                              \
  }                                                                            \
  TEST_F(f, Modify_InsertIterators_NearEnd_MoreThanAfter) {                    \
    Modify_InsertIterators_NearEnd_MoreThanAfter();                            \
  }                                                                            \
  TEST_F(f, Modify_InsertIterators_AtBegin_1) {                                \
    Modify_InsertIterators_AtBegin_1();                                        \
  }                                                                            \
  TEST_F(f, Modify_InsertIterators_AtBegin_2) {                                \
    Modify_InsertIterators_AtBegin_2();                                        \
  }                                                                            \
  TEST_F(f, Modify_InsertIterators_AtEnd_1) {                                  \
    Modify_InsertIterators_AtEnd_1();                                          \
  }                                                                            \
  TEST_F(f, Modify_InsertIterators_AtEnd_2) {                                  \
    Modify_InsertIterators_AtEnd_2();                                          \
  }                                                                            \
  TEST_F(f, Modify_InsertInitializerList) { Modify_InsertInitializerList(); }  \
  TEST_F(f, Modify_InsertInputIterator) { Modify_InsertInputIterator(); }      \
                                                                               \
  TEST_F(f, Algorithm_StdMaxElement) { Algorithm_StdMaxElement(); }            \
  TEST_F(f, Algorithm_StdMaxElementConst) { Algorithm_StdMaxElementConst(); }  \
                                                                               \
  TEST_F(f, Iterator_OperatorPlus) { Iterator_OperatorPlus(); }                \
  TEST_F(f, Iterator_OperatorPlusPlus) { Iterator_OperatorPlusPlus(); }        \
  TEST_F(f, Iterator_OperatorPlusEquals) { Iterator_OperatorPlusEquals(); }    \
  TEST_F(f, Iterator_OperatorMinus) { Iterator_OperatorMinus(); }              \
  TEST_F(f, Iterator_OperatorMinusMinus) { Iterator_OperatorMinusMinus(); }    \
  TEST_F(f, Iterator_OperatorMinusEquals) { Iterator_OperatorMinusEquals(); }  \
  TEST_F(f, Iterator_OperatorSquareBracket) {                                  \
    Iterator_OperatorSquareBracket();                                          \
  }                                                                            \
  TEST_F(f, Iterator_OperatorLessThan) { Iterator_OperatorLessThan(); }        \
  TEST_F(f, Iterator_OperatorLessThanEqual) {                                  \
    Iterator_OperatorLessThanEqual();                                          \
  }                                                                            \
  TEST_F(f, Iterator_OperatorGreater) { Iterator_OperatorGreater(); }          \
  TEST_F(f, Iterator_OperatorGreaterThanEqual) {                               \
    Iterator_OperatorGreaterThanEqual();                                       \
  }                                                                            \
  TEST_F(f, Iterator_OperatorDereference) { Iterator_OperatorDereference(); }  \
                                                                               \
  static_assert(                                                               \
      std::is_integral_v<                                                      \
          typename ::pw::containers::test::Container<f, int>::size_type>);     \
  static_assert(                                                               \
      std::is_same_v<                                                          \
          typename ::pw::containers::test::Container<f, int>::value_type,      \
          int>);                                                               \
  static_assert(std::is_integral_v<typename ::pw::containers::test::           \
                                       Container<f, int>::difference_type>);   \
  static_assert(std::is_same_v<                                                \
                typename ::pw::containers::test::Container<f, int>::reference, \
                int&>);                                                        \
  static_assert(                                                               \
      std::is_same_v<                                                          \
          typename ::pw::containers::test::Container<f, int>::const_reference, \
          const int&>);                                                        \
  static_assert(std::is_same_v<                                                \
                typename ::pw::containers::test::Container<f, int>::pointer,   \
                int*>);                                                        \
  static_assert(                                                               \
      std::is_same_v<                                                          \
          typename ::pw::containers::test::Container<f, int>::const_pointer,   \
          const int*>)

namespace pw::containers::test {

class InputIt {
 public:
  using iterator_category = std::input_iterator_tag;
  using value_type = int;
  using difference_type = void;
  using pointer = const int*;
  using reference = const int&;

  explicit constexpr InputIt(value_type value) : value_(value) {}

  constexpr InputIt(const InputIt&) = default;
  constexpr InputIt& operator=(const InputIt&) = default;

  constexpr InputIt& operator++() {
    value_ += 1;
    return *this;
  }
  constexpr void operator++(int) { operator++(); }

  constexpr pointer operator->() const { return &value_; }
  constexpr reference operator*() const { return value_; }

  constexpr bool operator!=(const InputIt& other) const {
    return value_ != other.value_;
  }

 private:
  value_type value_;
};

// Checks iterator properties.
template <template <typename> typename Container>
struct IteratorProperties {
  using iterator = typename Container<int>::iterator;
  using const_iterator = typename Container<int>::const_iterator;

  // Test that Container<T> iterators are copy constructible
  static_assert(std::is_copy_constructible_v<iterator>);
  static_assert(std::is_copy_constructible_v<const_iterator>);

  static_assert(std::is_copy_assignable_v<iterator>);
  static_assert(std::is_copy_assignable_v<const_iterator>);

  static_assert(std::is_convertible_v<iterator, const_iterator>,
                "Conversions from non-const to const are supported");

  static_assert(!std::is_convertible_v<const_iterator, iterator>,
                "Cannot convert const to non-const iterator");

  static_assert(
      !std::is_convertible_v<iterator, typename Container<char>::iterator>,
      "Cannot convert between iterator types");

  static_assert(const_iterator() == const_iterator());
  static_assert(iterator() == const_iterator());
  static_assert(const_iterator() == iterator());
  static_assert(iterator() == iterator());

  static_assert(const_iterator() <= const_iterator());
  static_assert(iterator() <= const_iterator());
  static_assert(const_iterator() <= iterator());
  static_assert(iterator() <= iterator());

  static_assert(const_iterator() >= const_iterator());
  static_assert(iterator() >= const_iterator());
  static_assert(const_iterator() >= iterator());
  static_assert(iterator() >= iterator());

  static_assert(!(const_iterator() != const_iterator()));
  static_assert(!(iterator() != const_iterator()));
  static_assert(!(const_iterator() != iterator()));
  static_assert(!(iterator() != iterator()));

  static_assert(!(const_iterator() < const_iterator()));
  static_assert(!(iterator() < const_iterator()));
  static_assert(!(const_iterator() < iterator()));
  static_assert(!(iterator() < iterator()));

  static_assert(!(const_iterator() > const_iterator()));
  static_assert(!(iterator() > const_iterator()));
  static_assert(!(const_iterator() > iterator()));
  static_assert(!(iterator() > iterator()));

  static constexpr bool kPasses = true;
};

// Gets the actual container type declared by the derived test fixture.
template <typename Derived, typename T>
using Container = cpp20::remove_cvref_t<
    decltype(std::declval<typename Derived::template Container<T>>().get())>;

// Containers are declared by a wrapper class, in case they need additional
// setup. This declares the wrapper then gets a reference to the actual
// container.
#define PW_DECLARE_CONTAINER(variable, type)                              \
  typename Derived::template Container<type> test_wrapper_for_##variable( \
      fixture());                                                         \
  auto& variable = test_wrapper_for_##variable.get()

template <typename Derived>
class CommonTestFixture : public ::testing::Test {
 public:
  CommonTestFixture() { Counter::Reset(); }

  void Move_BothEmpty() {
    PW_DECLARE_CONTAINER(container_1, Counter);
    PW_DECLARE_CONTAINER(container_2, Counter);

    container_1 = std::move(container_2);

    EXPECT_TRUE(container_1.empty());
    EXPECT_TRUE(container_2.empty());  // NOLINT(bugprone-use-after-move)
  }

  void Move_EmptyToNonEmpty() {
    PW_DECLARE_CONTAINER(container_1, Counter);
    container_1.assign({1, 2});

    PW_DECLARE_CONTAINER(container_2, Counter);

    container_1 = std::move(container_2);

    EXPECT_TRUE(container_1.empty());
    EXPECT_TRUE(container_2.empty());  // NOLINT(bugprone-use-after-move)
  }

  void Move_NonEmptyToEmpty() {
    PW_DECLARE_CONTAINER(container_1, Counter);

    PW_DECLARE_CONTAINER(container_2, Counter);
    container_2.assign({-1, -2, -3, -4});
    container_2.pop_front();
    container_2.pop_front();
    container_2.push_back(-5);

    container_1 = std::move(container_2);

    EXPECT_TRUE(Equal(container_1, std::array{-3, -4, -5}));
    EXPECT_TRUE(container_2.empty());  // NOLINT(bugprone-use-after-move)
  }

  void Move_BothNonEmpty() {
    PW_DECLARE_CONTAINER(container_1, Counter);
    container_1.assign({1, 2});

    PW_DECLARE_CONTAINER(container_2, Counter);
    container_2.assign({-1, -2, -3, -4});
    container_2.pop_front();

    container_1 = std::move(container_2);

    EXPECT_TRUE(Equal(container_1, std::array{-2, -3, -4}));
    EXPECT_TRUE(container_2.empty());  // NOLINT(bugprone-use-after-move)
  }

  void Destructor_Empty() {
    {
      PW_DECLARE_CONTAINER(container, Counter);
      EXPECT_EQ(container.size(), 0u);
    }
    EXPECT_EQ(Counter::created, 0);
    EXPECT_EQ(Counter::destroyed, 0);
  }

  void Destructor_NonEmpty() {
    const Counter value(1234);
    Counter::Reset();

    typename Container<Derived, Counter>::size_type count;
    {
      PW_DECLARE_CONTAINER(container, Counter);
      count = ArbitrarySizeThatFits(container);
      container.assign(count, value);
      ASSERT_EQ(container.size(), count);
    }

    EXPECT_EQ(Counter::created, static_cast<int>(count));
    EXPECT_EQ(Counter::created + Counter::moved, Counter::destroyed);
  }

  void Assign_ZeroCopies() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign(1, Counter());

    container.assign(0, Counter(123));
    EXPECT_TRUE(container.empty());
  }

  void Assign_MultipleCopies() {
    PW_DECLARE_CONTAINER(container, Counter);

    container.assign(3, Counter(123));

    EXPECT_EQ(container.size(), 3u);
    for (Counter& item : container) {
      EXPECT_EQ(item.value, 123);
    }

    container.assign(5, Counter(-456));
    EXPECT_EQ(container.size(), 5u);
    for (Counter& item : container) {
      EXPECT_EQ(item.value, -456);
    }
  }

  void Assign_ForwardIterator() {
    PW_DECLARE_CONTAINER(container, int);
    container.assign(5u, -1);

    std::array<int, 5> array{0, 1, 2, 3, 4};
    container.assign(array.begin(), array.end());

    ASSERT_EQ(container.size(), 5u);
    for (unsigned char i = 0; i < container.size(); ++i) {
      EXPECT_EQ(container[i], i);
    }

    container.assign(array.begin() + 3, array.end() - 1);
    ASSERT_EQ(container.size(), 1u);
    EXPECT_EQ(container.front(), 3);
  }

  void Assign_InputIterator() {
    PW_DECLARE_CONTAINER(container, int);
    container.assign(InputIt(5), InputIt(9));

    ASSERT_EQ(4u, container.size());
    EXPECT_EQ(5, container[0]);
    EXPECT_EQ(6, container[1]);
    EXPECT_EQ(7, container[2]);
    EXPECT_EQ(8, container[3]);
  }

  void Assign_InitializerList() {
    PW_DECLARE_CONTAINER(container, int);
    container.assign({1, 3, 5, 7});

    ASSERT_EQ(4u, container.size());

    EXPECT_EQ(1, container[0]);
    EXPECT_EQ(3, container[1]);
    EXPECT_EQ(5, container[2]);
    EXPECT_EQ(7, container[3]);
  }

  void Access_Iterator() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign(2, Counter());

    for (Counter& item : container) {
      EXPECT_EQ(item.value, 0);
    }
    for (const Counter& item : container) {
      EXPECT_EQ(item.value, 0);
    }
  }

  void Access_ConstIterator() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign(2, Counter());

    for (const Counter& item :
         static_cast<const Container<Derived, Counter>&>(container)) {
      EXPECT_EQ(item.value, 0);
    }
  }

  void Access_Empty() {
    PW_DECLARE_CONTAINER(container, Counter);

    EXPECT_EQ(0u, container.size());
    EXPECT_TRUE(container.empty());

    for (Counter& item : container) {
      (void)item;
      FAIL();
    }
  }

  void Access_DequeContiguousData() {
    // Content = {}, Storage = [x, x]
    PW_DECLARE_CONTAINER(container, int);

    {
      auto [first, second] = container.contiguous_data();
      EXPECT_EQ(first.size(), 0u);
      EXPECT_EQ(second.size(), 0u);
    }

    // Content = {1}, Storage = [1, x]
    container.push_back(1);
    {
      EXPECT_TRUE(SpansContain(container.contiguous_data(), {1}));
    }

    // Content = {1, 2}, Storage = [1, 2]
    container.push_back(2);
    EXPECT_EQ(container.size(), 2u);
    {
      EXPECT_TRUE(SpansContain(container.contiguous_data(), {1, 2}));
    }

    // Content = {2}, Storage = [x, 2]
    container.pop_front();
    {
      EXPECT_TRUE(SpansContain(container.contiguous_data(), {2}));
    }

    // Content = {2, 1}, Storage = [1, 2]
    container.push_back(1);
    {
      EXPECT_TRUE(SpansContain(container.contiguous_data(), {2, 1}));
    }

    // Content = {1}, Storage = [1, x]
    container.pop_front();
    {
      EXPECT_TRUE(SpansContain(container.contiguous_data(), {1}));
    }

    // Content = {1, 2}, Storage = [1, 2]
    container.push_back(2);
    {
      EXPECT_TRUE(SpansContain(container.contiguous_data(), {1, 2}));
    }
  }

  void Access_DequeConstContiguousData() {
    // Content = {1, 2}, Storage = [1, 2]
    PW_DECLARE_CONTAINER(container, int);
    container.assign({1, 2});
    const Container<Derived, int>& const_container = container;

    {
      auto spans = const_container.contiguous_data();
      EXPECT_EQ(spans.first.size(), 2u);
      EXPECT_EQ(spans.second.size(), 0u);
      EXPECT_TRUE(SpansContain(spans, {1, 2}));
    }
  }

  void Modify_ClearNonEmpty() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.emplace_back();
    container.emplace_back();
    container.emplace_back();

    container.clear();

    EXPECT_EQ(Counter::created, 3);
    EXPECT_EQ(Counter::created + Counter::moved, Counter::destroyed);
  }

  void Modify_PushBackCopy() {
    Counter value(99);
    Counter::Reset();

    {
      PW_DECLARE_CONTAINER(container, Counter);
      container.push_back(value);

      ASSERT_EQ(container.size(), 1u);
      EXPECT_EQ(container.front().value, 99);
    }

    EXPECT_EQ(Counter::created, 1);
    EXPECT_EQ(Counter::destroyed, 1);
  }

  void Modify_PushBackMove() {
    {
      Counter value(99);
      PW_DECLARE_CONTAINER(container, Counter);
      container.push_back(std::move(value));

      EXPECT_EQ(container.size(), 1u);
      EXPECT_EQ(container.front().value, 99);
      // NOLINTNEXTLINE(bugprone-use-after-move)
      EXPECT_EQ(value.value, 0);
    }

    EXPECT_EQ(Counter::created, 1);
    EXPECT_EQ(Counter::destroyed, 2);
    EXPECT_EQ(Counter::moved, 1);
  }

  void Modify_EmplaceBack() {
    {
      PW_DECLARE_CONTAINER(container, Counter);
      container.emplace_back(314);

      ASSERT_EQ(container.size(), 1u);
      EXPECT_EQ(container.front().value, 314);
    }

    EXPECT_EQ(Counter::created, 1);
    EXPECT_EQ(Counter::destroyed, 1);
  }

  void Modify_DequeWrapForwards() {
    {
      PW_DECLARE_CONTAINER(container, Counter);
      container.emplace_back(1);
      container.emplace_back(2);
      container.emplace_back(3);

      ASSERT_EQ(container.size(), 3u);
      EXPECT_EQ(container[0].value, 1);
      EXPECT_EQ(container.front().value, 1);
      EXPECT_EQ(container[1].value, 2);
      EXPECT_EQ(container[2].value, 3);
      EXPECT_EQ(container.back().value, 3);

      container.pop_front();
      container.emplace_back(4);

      ASSERT_EQ(container.size(), 3u);
      EXPECT_EQ(container[0].value, 2);
      EXPECT_EQ(container.front().value, 2);
      EXPECT_EQ(container[1].value, 3);
      EXPECT_EQ(container[2].value, 4);
      EXPECT_EQ(container.back().value, 4);
    }

    EXPECT_EQ(Counter::created, 4);
    EXPECT_EQ(Counter::created + Counter::moved, Counter::destroyed);
  }

  void Modify_DequeWrapBackwards() {
    {
      PW_DECLARE_CONTAINER(container, Counter);
      container.emplace_front(1);
      container.emplace_front(2);
      container.emplace_front(3);

      ASSERT_EQ(container.size(), 3u);
      EXPECT_EQ(container[0].value, 3);
      EXPECT_EQ(container.front().value, 3);
      EXPECT_EQ(container[1].value, 2);
      EXPECT_EQ(container[2].value, 1);
      EXPECT_EQ(container.back().value, 1);

      container.pop_back();
      container.emplace_front(4);

      ASSERT_EQ(container.size(), 3u);
      EXPECT_EQ(container[0].value, 4);
      EXPECT_EQ(container.front().value, 4);
      EXPECT_EQ(container[1].value, 3);
      EXPECT_EQ(container[2].value, 2);
      EXPECT_EQ(container.back().value, 2);
    }

    EXPECT_EQ(Counter::created, 4);
    EXPECT_EQ(Counter::created + Counter::moved, Counter::destroyed);
  }

  void Modify_PushFrontCopy() {
    Counter value(99);
    Counter::Reset();

    {
      PW_DECLARE_CONTAINER(container, Counter);
      container.push_front(value);

      EXPECT_EQ(container.size(), 1u);
      EXPECT_EQ(container.front().value, 99);
    }

    EXPECT_EQ(Counter::created, 1);
    EXPECT_EQ(Counter::destroyed, 1);
  }

  void Modify_PushFrontMove() {
    {
      Counter value(99);
      PW_DECLARE_CONTAINER(container, Counter);
      container.push_front(std::move(value));

      EXPECT_EQ(container.size(), 1u);
      EXPECT_EQ(container.front().value, 99);
      // NOLINTNEXTLINE(bugprone-use-after-move)
      EXPECT_EQ(value.value, 0);
    }

    EXPECT_EQ(Counter::created, 1);
    EXPECT_EQ(Counter::destroyed, 2);
    EXPECT_EQ(Counter::moved, 1);
  }

  void Modify_EmplaceFront() {
    {
      PW_DECLARE_CONTAINER(container, Counter);
      container.emplace_front(314);

      EXPECT_EQ(container.size(), 1u);
      EXPECT_EQ(container.front().value, 314);
    }

    EXPECT_EQ(Counter::created, 1);
    EXPECT_EQ(Counter::destroyed, 1);
  }

  void Modify_PopBack() {
    {
      PW_DECLARE_CONTAINER(container, Counter);
      container.emplace_front(1);  // This wraps to the other end.
      container.emplace_back(2);   // This is the first entry in storage.
      container.emplace_back(3);
      // Content = {1, 2, 3}, Storage = [2, 3, 1]

      ASSERT_EQ(container.size(), 3u);
      EXPECT_EQ(container[0].value, 1);
      EXPECT_EQ(container[1].value, 2);
      EXPECT_EQ(container[2].value, 3);

      container.pop_back();
      // Content = {1, 2}, Storage = [2, x, 1]
      ASSERT_EQ(container.size(), 2u);
      EXPECT_EQ(container[0].value, 1);
      EXPECT_EQ(container[1].value, 2);

      // This wraps around.
      container.pop_back();
      // Content = {1}, Storage = [x, x, 1]

      ASSERT_EQ(container.size(), 1u);
      EXPECT_EQ(container[0].value, 1);
    }

    EXPECT_EQ(Counter::created, 3);
    EXPECT_EQ(Counter::created + Counter::moved, Counter::destroyed);
  }

  void Modify_PopFront() {
    {
      PW_DECLARE_CONTAINER(container, Counter);
      container.emplace_front(1);  // This wraps to the other end.
      container.emplace_back(2);   // This is the first entry in storage.
      container.emplace_back(3);
      // Content = {1, 2, 3}, Storage = [2, 3, 1]

      ASSERT_EQ(container.size(), 3u);
      EXPECT_EQ(container[0].value, 1);
      EXPECT_EQ(container[1].value, 2);
      EXPECT_EQ(container[2].value, 3);

      // This wraps around
      container.pop_front();
      // Content = {2, 3}, Storage = [2, 3, x]

      EXPECT_EQ(container.size(), 2u);
      EXPECT_EQ(container[0].value, 2);
      EXPECT_EQ(container[1].value, 3);

      container.pop_front();
      // Content = {3}, Storage = [x, 3, x]
      ASSERT_EQ(container.size(), 1u);
      EXPECT_EQ(container[0].value, 3);
    }

    EXPECT_EQ(Counter::created, 3);
    EXPECT_EQ(Counter::created + Counter::moved, Counter::destroyed);
  }

  void Modify_ResizeLarger() {
    PW_DECLARE_CONTAINER(container, CopyOnly);
    container.assign(1, CopyOnly(123));
    ASSERT_EQ(container.size(), 1u);

    container.resize(3, CopyOnly(123));

    EXPECT_EQ(container.size(), 3u);
    for (auto& i : container) {
      EXPECT_EQ(i.value, 123);
    }
  }

  void Modify_ResizeSmaller() {
    PW_DECLARE_CONTAINER(container, CopyOnly);

    auto count = ArbitrarySizeThatFits(container);

    container.assign(count, CopyOnly(123));
    ASSERT_EQ(container.size(), count);

    container.resize(3, CopyOnly(123));

    EXPECT_EQ(container.size(), 3u);
    for (auto& i : container) {
      EXPECT_EQ(i.value, 123);
    }
  }

  void Modify_ResizeZero() {
    PW_DECLARE_CONTAINER(container, CopyOnly);
    auto count = ArbitrarySizeThatFits(container);
    container.assign(count, CopyOnly(123));
    ASSERT_EQ(container.size(), count);

    container.resize(0, CopyOnly(123));

    EXPECT_EQ(container.size(), 0u);
    EXPECT_TRUE(container.empty());
  }

  void Modify_Erase_FirstElement() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({1, 2, 3});
    Counter::Reset();

    auto it = container.erase(container.cbegin());
    EXPECT_EQ(it, container.begin());
    EXPECT_EQ(*it, 2);
    EXPECT_TRUE(Equal(container, std::array{2, 3}));
    EXPECT_EQ(Counter::destroyed, 1);
  }

  void Modify_Erase_LastElement() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({1, 2, 3});
    Counter::Reset();

    auto it = container.erase(container.cbegin() + 2);
    EXPECT_EQ(it, container.end());
    EXPECT_TRUE(Equal(container, std::array{1, 2}));
    EXPECT_EQ(Counter::destroyed, 1);
  }

  void Modify_Erase_MiddleElement() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({1, 2, 3});
    Counter::Reset();

    auto it = container.erase(container.cbegin() + 1);
    EXPECT_EQ(it, container.begin() + 1);
    EXPECT_EQ(*it, 3);
    EXPECT_TRUE(Equal(container, std::array{1, 3}));
    EXPECT_EQ(Counter::destroyed, 1);
  }

  void Modify_Erase_OnlyElement() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({1});
    Counter::Reset();

    auto it = container.erase(container.cbegin());
    EXPECT_EQ(it, container.end());
    EXPECT_TRUE(container.empty());
    EXPECT_EQ(Counter::destroyed, 1);
  }

  void Modify_Erase_AfterPopFront() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({1, 2, 3, 4});
    container.pop_front();
    container.push_back(5);
    // container is {2, 3, 4, 5}
    Counter::Reset();

    auto it = container.erase(container.cbegin() + 1);
    EXPECT_EQ(it, container.begin() + 1);
    EXPECT_EQ(*it, 4);
    EXPECT_TRUE(Equal(container, std::array{2, 4, 5}));
    EXPECT_EQ(Counter::destroyed, 1);
  }

  void Modify_Erase_MoveOnly() {
    PW_DECLARE_CONTAINER(container, MoveOnly);
    container.push_back(1);
    container.push_back(2);
    container.push_back(3);
    container.push_back(4);
    container.push_back(5);

    auto it = container.erase(container.cbegin() + 1);
    EXPECT_EQ(it, container.begin() + 1);
    EXPECT_EQ(it->value, 3);
    EXPECT_TRUE(Equal(container, std::array{1, 3, 4, 5}));
  }

  void Modify_EraseRange_ZeroElements() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({1, 2, 3});
    Counter::Reset();

    auto it = container.erase(container.cbegin(), container.cbegin());
    EXPECT_EQ(it, container.begin());
    EXPECT_TRUE(Equal(container, std::array{1, 2, 3}));
    EXPECT_EQ(Counter::destroyed, 0);
  }

  void Modify_EraseRange_OneElement() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({1, 2, 3});
    Counter::Reset();

    auto it = container.erase(container.cbegin() + 1, container.cbegin() + 2);
    EXPECT_EQ(it, container.begin() + 1);
    EXPECT_EQ(*it, 3);
    EXPECT_TRUE(Equal(container, std::array{1, 3}));
    EXPECT_EQ(Counter::destroyed, 1);
  }

  void Modify_EraseRange_ToTheBeginning() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({1, 2, 3, 4});
    Counter::Reset();

    auto it = container.erase(container.cbegin(), container.cbegin() + 2);
    EXPECT_EQ(it, container.begin());
    EXPECT_EQ(*it, 3);
    EXPECT_TRUE(Equal(container, std::array{3, 4}));
    EXPECT_EQ(Counter::destroyed, 2);
  }

  void Modify_EraseRange_ToTheEnd() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({1, 2, 3, 4});
    Counter::Reset();

    auto it = container.erase(container.cbegin() + 2, container.cend());
    EXPECT_EQ(it, container.end());
    EXPECT_TRUE(Equal(container, std::array{1, 2}));
    EXPECT_EQ(Counter::destroyed, 2);
  }

  void Modify_EraseRange_Everything() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({1, 2, 3, 4});
    Counter::Reset();

    auto it = container.erase(container.cbegin(), container.cend());
    EXPECT_EQ(it, container.end());
    EXPECT_TRUE(container.empty());
    EXPECT_EQ(Counter::destroyed, 4);
  }

  void Modify_EraseRange_AfterPopFront() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({1, 2, 3, 4, 5});
    container.pop_front();
    container.push_back(6);
    // container is {2, 3, 4, 5, 6}
    Counter::Reset();

    auto it = container.erase(container.cbegin() + 1, container.cbegin() + 4);
    EXPECT_EQ(it, container.begin() + 1);
    EXPECT_EQ(*it, 6);
    EXPECT_TRUE(Equal(container, std::array{2, 6}));
    EXPECT_EQ(Counter::destroyed, 3);
  }

  void Modify_Emplace_Empty() {
    PW_DECLARE_CONTAINER(container, Counter);
    Counter::Reset();

    auto it = container.emplace(container.cbegin(), 1);
    EXPECT_EQ(*it, 1);
    EXPECT_TRUE(Equal(container, std::array{1}));
    EXPECT_EQ(Counter::created, 1);
  }

  void Modify_Emplace_Front() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({1, 2, 3});
    Counter::Reset();

    auto it = container.emplace(container.cbegin(), 0);
    EXPECT_EQ(*it, 0);
    EXPECT_TRUE(Equal(container, std::array{0, 1, 2, 3}));
    EXPECT_EQ(Counter::created, 1);
  }

  void Modify_Emplace_Back() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({1, 2, 3});
    Counter::Reset();

    auto it = container.emplace(container.cend(), 4);
    EXPECT_EQ(*it, 4);
    EXPECT_TRUE(Equal(container, std::array{1, 2, 3, 4}));
    EXPECT_EQ(Counter::created, 1);
  }

  void Modify_Emplace_Middle() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({0, 1, 3});

    auto it = container.emplace(container.cbegin() + 2, 2);
    EXPECT_EQ(*it, 2);
    EXPECT_TRUE(Equal(container, std::array{0, 1, 2, 3}));
  }

  void Modify_Emplace_BeginPlusOne() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({10, 30, 40});
    Counter::Reset();

    auto it = container.emplace(container.cbegin() + 1, 20);
    EXPECT_EQ(it, container.begin() + 1);
    EXPECT_EQ(*it, 20);
    EXPECT_TRUE(Equal(container, std::array{10, 20, 30, 40}));
    EXPECT_EQ(Counter::created, 1);
  }

  void Modify_Emplace_EndMinusOne() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({10, 20, 40});
    Counter::Reset();

    auto it = container.emplace(container.cend() - 1, 30);
    EXPECT_EQ(it, container.begin() + 2);
    EXPECT_EQ(*it, 30);
    EXPECT_TRUE(Equal(container, std::array{10, 20, 30, 40}));
    EXPECT_EQ(Counter::created, 1);
  }

  void Modify_InsertCopy() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({1, 2, 4});
    Counter value(3);

    auto it = container.insert(container.cbegin() + 2, value);
    EXPECT_EQ(it, container.begin() + 2);
    EXPECT_EQ(*it, 3);
    EXPECT_TRUE(Equal(container, std::array{1, 2, 3, 4}));
  }

  void Modify_InsertMove() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({1, 2, 4});
    Counter value(3);

    auto it = container.insert(container.cbegin() + 2, std::move(value));
    EXPECT_EQ(it, container.begin() + 2);
    EXPECT_EQ(*it, 3);
    EXPECT_TRUE(Equal(container, std::array{1, 2, 3, 4}));
  }

  void Modify_InsertCopies_NearBegin_FewerThanBefore() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({10, 20, 30, 40, 50, 60});
    Counter value(99);
    Counter::Reset();

    auto it = container.insert(container.cbegin() + 2, 1, value);
    EXPECT_EQ(it, container.begin() + 2);
    EXPECT_EQ(*it, 99);
    EXPECT_TRUE(Equal(container, std::array{10, 20, 99, 30, 40, 50, 60}));
    EXPECT_EQ(Counter::created, 1);
  }

  void Modify_InsertCopies_NearBegin_SameAsBefore() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({10, 20, 30, 40, 50, 60});
    Counter value(99);
    Counter::Reset();

    auto it = container.insert(container.cbegin() + 2, 2, value);
    EXPECT_EQ(it, container.begin() + 2);
    EXPECT_EQ(*it, 99);
    EXPECT_TRUE(Equal(container, std::array{10, 20, 99, 99, 30, 40, 50, 60}));
    EXPECT_EQ(Counter::created, 2);
  }

  void Modify_InsertCopies_NearBegin_MoreThanBefore() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({10, 20, 30, 40, 50, 60});
    Counter value(99);
    Counter::Reset();

    auto it = container.insert(container.cbegin() + 2, 3, value);
    EXPECT_EQ(it, container.begin() + 2);
    EXPECT_EQ(*it, 99);
    EXPECT_TRUE(
        Equal(container, std::array{10, 20, 99, 99, 99, 30, 40, 50, 60}));
    EXPECT_EQ(Counter::created, 3);
  }

  void Modify_InsertCopies_NearEnd_FewerThanAfter() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({10, 20, 30, 40, 50, 60});
    Counter value(99);
    Counter::Reset();

    auto it = container.insert(container.cbegin() + 4, 1, value);
    EXPECT_EQ(it, container.begin() + 4);
    EXPECT_EQ(*it, 99);
    EXPECT_TRUE(Equal(container, std::array{10, 20, 30, 40, 99, 50, 60}));
    EXPECT_EQ(Counter::created, 1);
  }

  void Modify_InsertCopies_NearEnd_SameAsAfter() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({10, 20, 30, 40, 50, 60});
    Counter value(99);
    Counter::Reset();

    auto it = container.insert(container.cbegin() + 4, 2, value);
    EXPECT_EQ(it, container.begin() + 4);
    EXPECT_EQ(*it, 99);
    EXPECT_TRUE(Equal(container, std::array{10, 20, 30, 40, 99, 99, 50, 60}));
    EXPECT_EQ(Counter::created, 2);
  }

  void Modify_InsertCopies_NearEnd_MoreThanAfter() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({10, 20, 30, 40, 50, 60});
    Counter value(99);
    Counter::Reset();

    auto it = container.insert(container.cbegin() + 4, 3, value);
    EXPECT_EQ(it, container.begin() + 4);
    EXPECT_EQ(*it, 99);
    EXPECT_TRUE(
        Equal(container, std::array{10, 20, 30, 40, 99, 99, 99, 50, 60}));
    EXPECT_EQ(Counter::created, 3);
  }

  void Modify_InsertCopies_AtBegin_1() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({10, 20, 30, 40, 50, 60});
    Counter value(99);
    Counter::Reset();

    auto it = container.insert(container.cbegin(), 1, value);
    EXPECT_EQ(it, container.begin());
    EXPECT_EQ(*it, 99);
    EXPECT_TRUE(Equal(container, std::array{99, 10, 20, 30, 40, 50, 60}));
    EXPECT_EQ(Counter::created, 1);
  }

  void Modify_InsertCopies_AtBegin_2() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({10, 20, 30, 40, 50, 60});
    Counter value(99);
    Counter::Reset();

    auto it = container.insert(container.cbegin(), 2, value);
    EXPECT_EQ(it, container.begin());
    EXPECT_EQ(*it, 99);
    EXPECT_TRUE(Equal(container, std::array{99, 99, 10, 20, 30, 40, 50, 60}));
    EXPECT_EQ(Counter::created, 2);
  }

  void Modify_InsertCopies_AtEnd_1() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({10, 20, 30, 40, 50, 60});
    Counter value(99);
    Counter::Reset();

    auto it = container.insert(container.cend(), 1, value);
    EXPECT_EQ(it, container.end() - 1);
    EXPECT_EQ(*it, 99);
    EXPECT_TRUE(Equal(container, std::array{10, 20, 30, 40, 50, 60, 99}));
    EXPECT_EQ(Counter::created, 1);
  }

  void Modify_InsertCopies_AtEnd_2() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({10, 20, 30, 40, 50, 60});
    Counter value(99);
    Counter::Reset();

    auto it = container.insert(container.cend(), 2, value);
    EXPECT_EQ(it, container.end() - 2);
    EXPECT_EQ(*it, 99);
    EXPECT_TRUE(Equal(container, std::array{10, 20, 30, 40, 50, 60, 99, 99}));
    EXPECT_EQ(Counter::created, 2);
  }

  void Modify_InsertIterators_NearBegin_FewerThanBefore() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({10, 20, 30, 40, 50, 60});
    std::array<Counter, 1> values{Counter(99)};
    Counter::Reset();

    auto it = container.insert(container.cbegin() + 2,
                               std::make_move_iterator(values.begin()),
                               std::make_move_iterator(values.end()));
    EXPECT_EQ(it, container.begin() + 2);
    EXPECT_EQ(*it, 99);
    EXPECT_TRUE(Equal(container, std::array{10, 20, 99, 30, 40, 50, 60}));
    EXPECT_EQ(Counter::created, 0);
  }

  void Modify_InsertIterators_NearBegin_SameAsBefore() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({10, 20, 30, 40, 50, 60});
    std::array<Counter, 2> values{Counter(99), Counter(99)};
    Counter::Reset();

    auto it = container.insert(container.cbegin() + 2,
                               std::make_move_iterator(values.begin()),
                               std::make_move_iterator(values.end()));
    EXPECT_EQ(it, container.begin() + 2);
    EXPECT_EQ(*it, 99);
    EXPECT_TRUE(Equal(container, std::array{10, 20, 99, 99, 30, 40, 50, 60}));
    EXPECT_EQ(Counter::created, 0);
  }

  void Modify_InsertIterators_NearBegin_MoreThanBefore() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({10, 20, 30, 40, 50, 60});
    std::array<Counter, 3> values{Counter(99), Counter(99), Counter(99)};
    Counter::Reset();

    auto it = container.insert(container.cbegin() + 2,
                               std::make_move_iterator(values.begin()),
                               std::make_move_iterator(values.end()));
    EXPECT_EQ(it, container.begin() + 2);
    EXPECT_EQ(*it, 99);
    EXPECT_TRUE(
        Equal(container, std::array{10, 20, 99, 99, 99, 30, 40, 50, 60}));
    EXPECT_EQ(Counter::created, 0);
  }

  void Modify_InsertIterators_NearEnd_FewerThanAfter() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({10, 20, 30, 40, 50, 60});
    std::array<Counter, 1> values{Counter(99)};
    Counter::Reset();

    auto it = container.insert(container.cbegin() + 4,
                               std::make_move_iterator(values.begin()),
                               std::make_move_iterator(values.end()));
    EXPECT_EQ(it, container.begin() + 4);
    EXPECT_EQ(*it, 99);
    EXPECT_TRUE(Equal(container, std::array{10, 20, 30, 40, 99, 50, 60}));
    EXPECT_EQ(Counter::created, 0);
  }

  void Modify_InsertIterators_NearEnd_SameAsAfter() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({10, 20, 30, 40, 50, 60});
    std::array<Counter, 2> values{Counter(99), Counter(99)};
    Counter::Reset();

    auto it = container.insert(container.cbegin() + 4,
                               std::make_move_iterator(values.begin()),
                               std::make_move_iterator(values.end()));
    EXPECT_EQ(it, container.begin() + 4);
    EXPECT_EQ(*it, 99);
    EXPECT_TRUE(Equal(container, std::array{10, 20, 30, 40, 99, 99, 50, 60}));
    EXPECT_EQ(Counter::created, 0);
  }

  void Modify_InsertIterators_NearEnd_MoreThanAfter() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({10, 20, 30, 40, 50, 60});
    std::array<Counter, 3> values{Counter(99), Counter(99), Counter(99)};
    Counter::Reset();

    auto it = container.insert(container.cbegin() + 4,
                               std::make_move_iterator(values.begin()),
                               std::make_move_iterator(values.end()));
    EXPECT_EQ(it, container.begin() + 4);
    EXPECT_EQ(*it, 99);
    EXPECT_TRUE(
        Equal(container, std::array{10, 20, 30, 40, 99, 99, 99, 50, 60}));
    EXPECT_EQ(Counter::created, 0);
  }

  void Modify_InsertIterators_AtBegin_1() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({10, 20, 30, 40, 50, 60});
    std::array<Counter, 1> values{Counter(99)};
    Counter::Reset();

    auto it = container.insert(container.cbegin(),
                               std::make_move_iterator(values.begin()),
                               std::make_move_iterator(values.end()));
    EXPECT_EQ(it, container.begin());
    EXPECT_EQ(*it, 99);
    EXPECT_TRUE(Equal(container, std::array{99, 10, 20, 30, 40, 50, 60}));
    EXPECT_EQ(Counter::created, 0);
  }

  void Modify_InsertIterators_AtBegin_2() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({10, 20, 30, 40, 50, 60});
    std::array<Counter, 2> values{Counter(99), Counter(99)};
    Counter::Reset();

    auto it = container.insert(container.cbegin(),
                               std::make_move_iterator(values.begin()),
                               std::make_move_iterator(values.end()));
    EXPECT_EQ(it, container.begin());
    EXPECT_EQ(*it, 99);
    EXPECT_TRUE(Equal(container, std::array{99, 99, 10, 20, 30, 40, 50, 60}));
    EXPECT_EQ(Counter::created, 0);
  }

  void Modify_InsertIterators_AtEnd_1() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({10, 20, 30, 40, 50, 60});
    std::array<Counter, 1> values{Counter(99)};
    Counter::Reset();

    auto it = container.insert(container.cend(),
                               std::make_move_iterator(values.begin()),
                               std::make_move_iterator(values.end()));
    EXPECT_EQ(it, container.end() - 1);
    EXPECT_EQ(*it, 99);
    EXPECT_TRUE(Equal(container, std::array{10, 20, 30, 40, 50, 60, 99}));
    EXPECT_EQ(Counter::created, 0);
  }

  void Modify_InsertIterators_AtEnd_2() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({10, 20, 30, 40, 50, 60});
    std::array<Counter, 2> values{Counter(99), Counter(99)};
    Counter::Reset();

    auto it = container.insert(container.cend(),
                               std::make_move_iterator(values.begin()),
                               std::make_move_iterator(values.end()));
    EXPECT_EQ(it, container.end() - 2);
    EXPECT_EQ(*it, 99);
    EXPECT_TRUE(Equal(container, std::array{10, 20, 30, 40, 50, 60, 99, 99}));
    EXPECT_EQ(Counter::created, 0);
  }

  void Modify_InsertInitializerList() {
    PW_DECLARE_CONTAINER(container, Counter);
    container.assign({1, 5});

    auto it = container.insert(container.cbegin() + 1, {2, 3, 4});
    EXPECT_EQ(it, container.begin() + 1);
    EXPECT_TRUE(Equal(container, std::array{1, 2, 3, 4, 5}));
  }

  void Modify_InsertInputIterator() {
    PW_DECLARE_CONTAINER(container, int);
    container.assign({1, 5});

    auto it = container.insert(container.cbegin() + 1, InputIt(2), InputIt(5));
    EXPECT_EQ(it, container.begin() + 1);
    EXPECT_TRUE(Equal(container, std::array{1, 2, 3, 4, 5}));
  }

  void Algorithm_StdMaxElement() {
    // Content = {1, 2, 3, 4}, Storage = [1, 2, 3, 4]
    PW_DECLARE_CONTAINER(container, int);
    container.assign({1, 2, 3, 4});

    auto max_element_it = std::max_element(container.begin(), container.end());
    ASSERT_NE(max_element_it, container.end());
    EXPECT_EQ(*max_element_it, 4);

    // Content = {2, 3, 4}, Storage = [x, 2, 3, 4]
    container.pop_front();

    max_element_it = std::max_element(container.begin(), container.end());
    ASSERT_NE(max_element_it, container.end());
    EXPECT_EQ(*max_element_it, 4);

    // Content = {2, 3, 4, 5}, Storage = [5, 2, 3, 4]
    container.push_back(5);
    max_element_it = std::max_element(container.begin(), container.end());
    ASSERT_NE(max_element_it, container.end());
    EXPECT_EQ(*max_element_it, 5);

    // Content = {}, Storage = [x, x, x, x]
    container.clear();

    max_element_it = std::max_element(container.begin(), container.end());
    ASSERT_EQ(max_element_it, container.end());
  }

  void Algorithm_StdMaxElementConst() {
    // Content = {1, 2, 3, 4}, Storage = [1, 2, 3, 4]
    PW_DECLARE_CONTAINER(container, int);  // mutable container
    container.assign({1, 2, 3, 4});

    auto max_element_it =
        std::max_element(container.cbegin(), container.cend());
    ASSERT_NE(max_element_it, container.cend());
    EXPECT_EQ(*max_element_it, 4);

    // Content = {2, 3, 4}, Storage = [x, 2, 3, 4]
    container.pop_front();  // modify mutable container

    max_element_it = std::max_element(container.cbegin(), container.cend());
    ASSERT_NE(max_element_it, container.cend());
    EXPECT_EQ(*max_element_it, 4);

    // Content = {2, 3, 4, 5}, Storage = [5, 2, 3, 4]
    container.push_back(5);  // modify mutable container
    max_element_it = std::max_element(container.cbegin(), container.cend());
    ASSERT_NE(max_element_it, container.cend());
    EXPECT_EQ(*max_element_it, 5);

    // Content = {}, Storage = [x, x, x, x]
    container.clear();  // modify mutable container

    max_element_it = std::max_element(container.cbegin(), container.cend());
    ASSERT_EQ(max_element_it, container.cend());
  }

  void Iterator_OperatorPlus() {
    // Content = {0, 0, 1, 2}, Storage = [0, 0, 1, 2]
    PW_DECLARE_CONTAINER(container, int);
    container.assign({0, 0, 1, 2});
    // Content = {0, 1, 2}, Storage = [x, 0, 1, 2]
    container.pop_front();
    // Content = {0, 1, 2, 3}, Storage = [3, 0, 1, 2]
    container.push_back(3);
    // Content = {1, 2, 3}, Storage = [3, x, 1, 2]
    container.pop_front();
    // Content = {1, 2, 3, 4}, Storage = [3, 4, 1, 2]
    container.push_back(4);

    for (int i = 0; i < 4; i++) {
      ASSERT_EQ(*(container.begin() + i), static_cast<int>(i + 1));
      ASSERT_EQ(*(i + container.begin()), static_cast<int>(i + 1));
    }

    ASSERT_EQ(container.begin() + container.size(), container.end());
  }

  void Iterator_OperatorPlusPlus() {
    // Content = {0, 0, 1, 2}, Storage = [0, 0, 1, 2]
    PW_DECLARE_CONTAINER(container, int);
    container.assign({0, 0, 1, 2});
    // Content = {0, 1, 2}, Storage = [x, 0, 1, 2]
    container.pop_front();
    // Content = {0, 1, 2, 3}, Storage = [3, 0, 1, 2]
    container.push_back(3);
    // Content = {1, 2, 3}, Storage = [3, x, 1, 2]
    container.pop_front();
    // Content = {1, 2, 3, 4}, Storage = [3, 4, 1, 2]
    container.push_back(4);

    auto it = container.begin();

    ASSERT_EQ(*it, 1);
    it++;
    ASSERT_EQ(*it, 2);
    it++;
    ASSERT_EQ(*it, 3);
    it++;
    ASSERT_EQ(*it, 4);
    it++;

    ASSERT_EQ(it, container.end());
  }

  void Iterator_OperatorPlusEquals() {
    // Content = {0, 0, 1, 2}, Storage = [0, 0, 1, 2]
    PW_DECLARE_CONTAINER(container, int);
    container.assign({0, 0, 1, 2});
    // Content = {0, 1, 2}, Storage = [x, 0, 1, 2]
    container.pop_front();
    // Content = {0, 1, 2, 3}, Storage = [3, 0, 1, 2]
    container.push_back(3);
    // Content = {1, 2, 3}, Storage = [3, x, 1, 2]
    container.pop_front();
    // Content = {1, 2, 3, 4}, Storage = [3, 4, 1, 2]
    container.push_back(4);

    auto it = container.begin();

    ASSERT_EQ(*it, 1);
    it += 1;
    ASSERT_EQ(*it, 2);
    it += 1;
    ASSERT_EQ(*it, 3);
    it += 1;
    ASSERT_EQ(*it, 4);
    it += 1;
    ASSERT_EQ(it, container.end());

    it = container.begin();
    ASSERT_EQ(*it, 1);
    it += 2;
    ASSERT_EQ(*it, 3);
    it += 2;
    ASSERT_EQ(it, container.end());

    it = container.begin();
    it += container.size();

    ASSERT_EQ(it, container.end());
  }

  void Iterator_OperatorMinus() {
    // Content = {0, 0, 1, 2}, Storage = [0, 0, 1, 2]
    PW_DECLARE_CONTAINER(container, int);
    container.assign({0, 0, 1, 2});
    // Content = {0, 1, 2}, Storage = [x, 0, 1, 2]
    container.pop_front();
    // Content = {0, 1, 2, 3}, Storage = [3, 0, 1, 2]
    container.push_back(3);
    // Content = {1, 2, 3}, Storage = [3, x, 1, 2]
    container.pop_front();
    // Content = {1, 2, 3, 4}, Storage = [3, 4, 1, 2]
    container.push_back(4);

    for (int i = 1; i <= 4; i++) {
      ASSERT_EQ(*(container.end() - i), static_cast<int>(5 - i));
    }

    ASSERT_EQ(container.end() - container.size(), container.begin());
  }
  void Iterator_OperatorMinusMinus() {
    // Content = {0, 0, 1, 2}, Storage = [0, 0, 1, 2]
    PW_DECLARE_CONTAINER(container, int);
    container.assign({0, 0, 1, 2});
    // Content = {0, 1, 2}, Storage = [x, 0, 1, 2]
    container.pop_front();
    // Content = {0, 1, 2, 3}, Storage = [3, 0, 1, 2]
    container.push_back(3);
    // Content = {1, 2, 3}, Storage = [3, x, 1, 2]
    container.pop_front();
    // Content = {1, 2, 3, 4}, Storage = [3, 4, 1, 2]
    container.push_back(4);

    auto it = container.end();

    it--;
    ASSERT_EQ(*it, 4);
    it--;
    ASSERT_EQ(*it, 3);
    it--;
    ASSERT_EQ(*it, 2);
    it--;
    ASSERT_EQ(*it, 1);

    ASSERT_EQ(it, container.begin());
  }

  void Iterator_OperatorMinusEquals() {
    // Content = {0, 0, 1, 2}, Storage = [0, 0, 1, 2]
    PW_DECLARE_CONTAINER(container, int);
    container.assign({0, 0, 1, 2});
    // Content = {0, 1, 2}, Storage = [x, 0, 1, 2]
    container.pop_front();
    // Content = {0, 1, 2, 3}, Storage = [3, 0, 1, 2]
    container.push_back(3);
    // Content = {1, 2, 3}, Storage = [3, x, 1, 2]
    container.pop_front();
    // Content = {1, 2, 3, 4}, Storage = [3, 4, 1, 2]
    container.push_back(4);

    auto it = container.end();

    it -= 1;
    ASSERT_EQ(*it, 4);
    it -= 1;
    ASSERT_EQ(*it, 3);
    it -= 1;
    ASSERT_EQ(*it, 2);
    it -= 1;
    ASSERT_EQ(*it, 1);

    ASSERT_EQ(it, container.begin());

    it = container.end();

    it -= 2;
    ASSERT_EQ(*it, 3);
    it -= 2;
    ASSERT_EQ(*it, 1);

    ASSERT_EQ(it, container.begin());

    it = container.end();
    it -= container.size();

    ASSERT_EQ(it, container.begin());
  }

  void Iterator_OperatorSquareBracket() {
    // Content = {0, 0, 1, 2}, Storage = [0, 0, 1, 2]
    PW_DECLARE_CONTAINER(container, int);
    container.assign({0, 0, 1, 2});
    // Content = {0, 1, 2}, Storage = [x, 0, 1, 2]
    container.pop_front();
    // Content = {0, 1, 2, 3}, Storage = [3, 0, 1, 2]
    container.push_back(3);
    // Content = {1, 2, 3}, Storage = [3, x, 1, 2]
    container.pop_front();
    // Content = {1, 2, 3, 4}, Storage = [3, 4, 1, 2]
    container.push_back(4);

    for (typename Container<Derived, int>::size_type i = 0;
         i < container.size();
         i++) {
      ASSERT_EQ(container.begin()[i], static_cast<int>(i + 1));
    }
  }

  void Iterator_OperatorLessThan() {
    // Content = {0, 0, 1, 2}, Storage = [0, 0, 1, 2]
    PW_DECLARE_CONTAINER(container, int);
    container.assign({0, 0, 1, 2});
    // Content = {0, 1, 2}, Storage = [x, 0, 1, 2]
    container.pop_front();
    // Content = {0, 1, 2, 3}, Storage = [3, 0, 1, 2]
    container.push_back(3);
    // Content = {1, 2, 3}, Storage = [3, x, 1, 2]
    container.pop_front();
    // Content = {1, 2, 3, 4}, Storage = [3, 4, 1, 2]
    container.push_back(4);

    using size_type = typename Container<Derived, int>::size_type;
    for (size_type i = 0; i < container.size(); i++) {
      for (size_type j = 0; j < i; j++) {
        ASSERT_TRUE((container.begin() + j) < (container.begin() + i));
      }

      ASSERT_TRUE((container.begin() + i) < container.end());
    }
  }

  void Iterator_OperatorLessThanEqual() {
    // Content = {0, 0, 1, 2}, Storage = [0, 0, 1, 2]
    PW_DECLARE_CONTAINER(container, int);
    container.assign({0, 0, 1, 2});
    // Content = {0, 1, 2}, Storage = [x, 0, 1, 2]
    container.pop_front();
    // Content = {0, 1, 2, 3}, Storage = [3, 0, 1, 2]
    container.push_back(3);
    // Content = {1, 2, 3}, Storage = [3, x, 1, 2]
    container.pop_front();
    // Content = {1, 2, 3, 4}, Storage = [3, 4, 1, 2]
    container.push_back(4);

    using size_type = typename Container<Derived, int>::size_type;
    for (size_type i = 0; i < container.size(); i++) {
      for (size_type j = 0; j <= i; j++) {
        ASSERT_TRUE((container.begin() + j) <= (container.begin() + i));
      }

      ASSERT_TRUE((container.begin() + i) <= container.end());
    }
  }

  void Iterator_OperatorGreater() {
    // Content = {0, 0, 1, 2}, Storage = [0, 0, 1, 2]
    PW_DECLARE_CONTAINER(container, int);
    container.assign({0, 0, 1, 2});
    // Content = {0, 1, 2}, Storage = [x, 0, 1, 2]
    container.pop_front();
    // Content = {0, 1, 2, 3}, Storage = [3, 0, 1, 2]
    container.push_back(3);
    // Content = {1, 2, 3}, Storage = [3, x, 1, 2]
    container.pop_front();
    // Content = {1, 2, 3, 4}, Storage = [3, 4, 1, 2]
    container.push_back(4);

    using size_type = typename Container<Derived, int>::size_type;
    for (size_type i = 0; i < container.size(); i++) {
      for (size_type j = i + 1; j < container.size(); j++) {
        ASSERT_TRUE((container.begin() + j) > (container.begin() + i));
      }
      ASSERT_TRUE(container.end() > (container.begin() + i));
    }
  }

  void Iterator_OperatorGreaterThanEqual() {
    // Content = {0, 0, 1, 2}, Storage = [0, 0, 1, 2]
    PW_DECLARE_CONTAINER(container, int);
    container.assign({0, 0, 1, 2});
    // Content = {0, 1, 2}, Storage = [x, 0, 1, 2]
    container.pop_front();
    // Content = {0, 1, 2, 3}, Storage = [3, 0, 1, 2]
    container.push_back(3);
    // Content = {1, 2, 3}, Storage = [3, x, 1, 2]
    container.pop_front();
    // Content = {1, 2, 3, 4}, Storage = [3, 4, 1, 2]
    container.push_back(4);

    using size_type = typename Container<Derived, int>::size_type;
    for (size_type i = 0; i < container.size(); i++) {
      for (size_type j = i; j < container.size(); j++) {
        ASSERT_TRUE((container.begin() + j) >= (container.begin() + i));
      }
      ASSERT_TRUE(container.end() >= (container.begin() + i));
    }
  }

  void Iterator_OperatorDereference() {
    // Content = {0, 0, 1, 2}, Storage = [0, 0, 1, 2]
    PW_DECLARE_CONTAINER(container, int);
    container.assign({0, 0, 1, 2});
    // Content = {0, 1, 2}, Storage = [x, 0, 1, 2]
    container.pop_front();
    // Content = {0, 1, 2, 3}, Storage = [3, 0, 1, 2]
    container.push_back(3);
    // Content = {1, 2, 3}, Storage = [3, x, 1, 2]
    container.pop_front();
    // Content = {1, 2, 3, 4}, Storage = [3, 4, 1, 2]
    container.push_back(4);

    using size_type = typename Container<Derived, int>::size_type;
    for (size_type i = 0; i < container.size(); i++) {
      const auto it = container.begin() + i;
      ASSERT_EQ(*(it.operator->()), static_cast<int>(i + 1));
    }
  }

 private:
  Derived& fixture() { return static_cast<Derived&>(*this); }
  const Derived& fixture() const { return static_cast<const Derived&>(*this); }

  // Chooses a size that the container is capable of holding.
  template <typename C>
  static auto ArbitrarySizeThatFits(const C& container) {
    return std::min(container.max_size(), typename C::size_type{10});
  }

  // Checks the expected data is in order across the two spans.
  template <typename Spans, typename T>
  static bool SpansContain(const Spans& spans,
                           std::initializer_list<T> expected_contents);
};

#undef PW_DECLARE_CONTAINER

template <typename Derived>
template <typename Spans, typename T>
bool CommonTestFixture<Derived>::SpansContain(
    const Spans& spans, std::initializer_list<T> expected_contents) {
  auto expected = expected_contents.begin();

  for (const T& actual : spans.first) {
    if (expected == expected_contents.end()) {
      return false;
    }
    if (*expected != actual) {
      EXPECT_EQ(*expected, actual);
      return false;
    }
    ++expected;
  }
  for (const T& actual : spans.second) {
    if (expected == expected_contents.end()) {
      return false;
    }
    if (*expected != actual) {
      EXPECT_EQ(*expected, actual);
      return false;
    }
    ++expected;
  }
  return expected == expected_contents.end();
}

}  // namespace pw::containers::test
