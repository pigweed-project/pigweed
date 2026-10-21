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

#include "pw_containers/ptr_iterator.h"

#include <array>
#include <cstddef>
#include <iterator>
#include <type_traits>

#include "pw_containers/iterator.h"
#include "pw_unit_test/framework.h"

namespace {

// Create a fake container for use with PtrIterator.
class FakeVector {
 public:
  using value_type = int;
  using reference = int&;
  using const_reference = const int&;
  using pointer = int*;
  using const_pointer = const int*;
  using iterator = pw::containers::PtrIterator<FakeVector>;
  using const_iterator = pw::containers::ConstPtrIterator<FakeVector>;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  constexpr FakeVector(int* data, size_t length)
      : data_(data), length_(length) {}

  iterator begin() { return iterator(data_); }
  iterator end() { return iterator(data_ + length_); }
  const_iterator begin() const { return const_iterator(data_); }
  const_iterator end() const { return const_iterator(data_ + length_); }

  reverse_iterator rbegin() { return reverse_iterator(end()); }
  reverse_iterator rend() { return reverse_iterator(begin()); }
  const_reverse_iterator rbegin() const {
    return const_reverse_iterator(end());
  }
  const_reverse_iterator rend() const {
    return const_reverse_iterator(begin());
  }

 private:
  int* data_;
  size_t length_;
};

static_assert(std::is_same_v<std::iterator_traits<pw::containers::PtrIterator<
                                 FakeVector>>::iterator_category,
                             pw::containers::contiguous_iterator_tag>);
static_assert(
    std::is_same_v<std::iterator_traits<pw::containers::ConstPtrIterator<
                       FakeVector>>::iterator_category,
                   pw::containers::contiguous_iterator_tag>);

static_assert(!std::is_convertible_v<FakeVector::iterator, int*>);
static_assert(!std::is_convertible_v<int*, FakeVector::iterator>);
static_assert(
    std::is_convertible_v<FakeVector::iterator, FakeVector::const_iterator>);
static_assert(
    !std::is_convertible_v<FakeVector::const_iterator, FakeVector::iterator>);

TEST(PtrIterator, DefaultConstructor) {
  pw::containers::PtrIterator<FakeVector> it;
  pw::containers::ConstPtrIterator<FakeVector> cit;
  EXPECT_EQ(it, pw::containers::PtrIterator<FakeVector>());
  EXPECT_EQ(cit, pw::containers::ConstPtrIterator<FakeVector>());
}

TEST(PtrIterator, IterateForwards) {
  std::array<int, 5> data{1, 2, 3, 4, 5};
  FakeVector vec(data.data(), data.size());
  int expected = 1;
  for (int val : vec) {
    EXPECT_EQ(val, expected++);
  }

  std::array<int, 3> data2{10, 20, 30};
  FakeVector vec2(data2.data(), data2.size());
  expected = 10;
  for (int val : vec2) {
    EXPECT_EQ(val, expected);
    expected += 10;
  }
}

TEST(PtrIterator, ReverseIterate) {
  std::array<int, 5> data{1, 2, 3, 4, 5};
  FakeVector vec(data.data(), data.size());
  int expected = 5;
  for (FakeVector::reverse_iterator it = vec.rbegin(); it != vec.rend(); ++it) {
    EXPECT_EQ(*it, expected--);
  }

  const FakeVector& cvec = vec;
  expected = 5;
  for (FakeVector::const_reverse_iterator it = cvec.rbegin(); it != cvec.rend();
       ++it) {
    EXPECT_EQ(*it, expected--);
  }
}

TEST(PtrIterator, IterateBackwards) {
  std::array<int, 5> data{1, 2, 3, 4, 5};
  FakeVector vec(data.data(), data.size());
  FakeVector::iterator it = vec.end();
  int expected = 5;
  do {
    --it;
    EXPECT_EQ(*it, expected--);
  } while (it != vec.begin());

  std::array<int, 2> data2{100, 200};
  FakeVector vec2(data2.data(), data2.size());
  FakeVector::iterator it2 = vec2.end();
  int expected2 = 200;
  do {
    --it2;
    EXPECT_EQ(*it2, expected2);
    expected2 -= 100;
  } while (it2 != vec2.begin());
}

TEST(PtrIterator, PostIncrement) {
  std::array<int, 2> data{1, 2};
  FakeVector vec(data.data(), data.size());
  FakeVector::iterator it = vec.begin();
  EXPECT_EQ(*(it++), 1);
  EXPECT_EQ(*it, 2);
}

TEST(PtrIterator, PreIncrement) {
  std::array<int, 3> data{1, 2, 3};
  FakeVector vec(data.data(), data.size());
  FakeVector::iterator it = vec.begin();
  EXPECT_EQ(*(++it), 2);
  EXPECT_EQ(*it, 2);
}

TEST(PtrIterator, PostDecrement) {
  std::array<int, 4> data{1, 2, 3, 4};
  FakeVector vec(data.data(), data.size());
  FakeVector::iterator it = vec.end();
  --it;
  EXPECT_EQ(*(it--), 4);
  EXPECT_EQ(*it, 3);
}

TEST(PtrIterator, PreDecrement) {
  std::array<int, 5> data{1, 2, 3, 4, 5};
  FakeVector vec(data.data(), data.size());
  FakeVector::iterator it = vec.end();
  --it;
  EXPECT_EQ(*(--it), 4);
  EXPECT_EQ(*it, 4);
}

TEST(PtrIterator, Addition) {
  std::array<int, 6> data{1, 2, 3, 4, 5, 6};
  FakeVector vec(data.data(), data.size());
  FakeVector::iterator it = vec.begin();
  EXPECT_EQ(*(it + 2), 3);
  EXPECT_EQ(*(2 + it), 3);
  it += 4;
  EXPECT_EQ(*it, 5);
}

TEST(PtrIterator, Subtraction) {
  std::array<int, 7> data{1, 2, 3, 4, 5, 6, 7};
  FakeVector vec(data.data(), data.size());
  FakeVector::iterator it = vec.end();
  EXPECT_EQ(*(it - 2), 6);
  it -= 3;
  EXPECT_EQ(*it, 5);
}

TEST(PtrIterator, Difference) {
  std::array<int, 8> data{1, 2, 3, 4, 5, 6, 7, 8};
  FakeVector vec(data.data(), data.size());
  EXPECT_EQ(vec.end() - vec.begin(), 8);

  std::array<int, 1> data2{1};
  FakeVector vec2(data2.data(), data2.size());
  EXPECT_EQ(vec2.end() - vec2.begin(), 1);
}

TEST(PtrIterator, Comparison) {
  std::array<int, 5> data{1, 2, 3, 4, 5};
  FakeVector vec(data.data(), data.size());
  FakeVector::iterator it1 = vec.begin();
  FakeVector::iterator it2 = vec.begin();
  EXPECT_TRUE(it1 == it2);
  EXPECT_FALSE(it1 != it2);
  EXPECT_FALSE(it1 < it2);
  EXPECT_TRUE(it1 <= it2);
  EXPECT_FALSE(it1 > it2);
  EXPECT_TRUE(it1 >= it2);

  ++it2;
  EXPECT_FALSE(it1 == it2);
  EXPECT_TRUE(it1 != it2);
  EXPECT_TRUE(it1 < it2);
  EXPECT_TRUE(it1 <= it2);
  EXPECT_FALSE(it1 > it2);
  EXPECT_FALSE(it1 >= it2);
}

TEST(PtrIterator, ConstConversion) {
  std::array<int, 5> data{1, 2, 3, 4, 5};
  FakeVector vec(data.data(), data.size());
  FakeVector::iterator it = vec.begin();
  FakeVector::const_iterator cit = it;
  EXPECT_EQ(*it, *cit);
}

TEST(PtrIterator, ArrayAccess) {
  std::array<int, 4> data{1, 2, 3, 4};
  FakeVector vec(data.data(), data.size());
  FakeVector::iterator it = vec.begin();
  EXPECT_EQ(it[2], 3);
  EXPECT_EQ(it[0], 1);
  EXPECT_EQ(it[3], 4);
}

}  // namespace
