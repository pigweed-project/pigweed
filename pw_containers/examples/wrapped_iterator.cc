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

#include "pw_containers/wrapped_iterator.h"

#include <array>
#include <cstddef>

#include "pw_unit_test/framework.h"

namespace examples {

// DOCSTAG: [pw_containers-wrapped_iterator]

using pw::containers::WrappedIterator;

// Multiplies values in a std::array by two.
class DoubleIterator : public WrappedIterator<DoubleIterator, const int*, int> {
 public:
  constexpr DoubleIterator(const int* it) : WrappedIterator(it) {}
  int operator*() const { return value() * 2; }
};

// Returns twice the sum of the elements in a array of integers.
template <size_t kArraySize>
int DoubleSum(const std::array<int, kArraySize>& c) {
  int sum = 0;
  for (DoubleIterator it(c.data()); it != DoubleIterator(c.data() + c.size());
       ++it) {
    // The iterator yields doubles instead of the original values.
    sum += *it;
  }
  return sum;
}

// DOCSTAG: [pw_containers-wrapped_iterator]

}  // namespace examples

namespace {

TEST(WrappedIteratorExampleTest, DoubleSum) {
  constexpr std::array<int, 6> kArray{0, 1, 2, 3, 4, 5};
  EXPECT_EQ(examples::DoubleSum(kArray), 30);
}

}  // namespace
