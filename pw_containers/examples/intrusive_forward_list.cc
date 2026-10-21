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

#include "pw_unit_test/framework.h"

namespace examples {

// DOCSTAG: [pw_containers-intrusive_forward_list]

class Square : public pw::IntrusiveForwardList<Square>::Item {
 public:
  Square(size_t side_length) : side_length_(side_length) {}
  size_t Area() const { return side_length_ * side_length_; }

 private:
  size_t side_length_;
};

class SquareList {
 public:
  // These elements are not copied into the linked list, the original objects
  // are just chained together and can be accessed via `list_`.
  SquareList() : list_(squares_.begin(), squares_.end()) {}

  // It is an error for items to go out of scope while still listed, or for a
  // list to go out of scope while it still has items.
  ~SquareList() { list_.clear(); }

  // The list can be iterated over normally.
  size_t SumAreas() const {
    size_t sum = 0;
    for (const auto& square : list_) {
      sum += square.Area();
    }
    return sum;
  }

  // Like `std::forward_list`, an iterator is invalidated when the item it
  // refers to is removed. It is *NOT* safe to remove items from a list while
  // iterating over it in a range-based for loop.
  //
  // To remove items while iterating, use an iterator to the previous item. If
  // only removing items, consider using `remove_if` instead.
  size_t RemoveAndSumAreas(size_t area_to_remove) {
    size_t sum = 0;
    auto previous = list_.before_begin();
    auto current = list_.begin();
    while (current != list_.end()) {
      if (current->Area() == area_to_remove) {
        current = list_.erase_after(previous);
      } else {
        sum += current->Area();
        previous = current;
        ++current;
      }
    }
    return sum;
  }

 private:
  std::array<Square, 3> squares_ = {{{1}, {20}, {400}}};
  pw::IntrusiveForwardList<Square> list_;
};

// DOCSTAG: [pw_containers-intrusive_forward_list]

}  // namespace examples

namespace {

TEST(IntrusiveForwardListExampleTest, EnlistSquares) {
  examples::SquareList square_list;
  EXPECT_EQ(square_list.SumAreas(), 160000u + 400u + 1u);
  EXPECT_EQ(square_list.RemoveAndSumAreas(400), 160000u + 1u);
  EXPECT_EQ(square_list.SumAreas(), 160000u + 1u);
}

}  // namespace
