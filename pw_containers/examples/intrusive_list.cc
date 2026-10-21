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

#include "pw_containers/intrusive_list.h"

#include <array>

#include "pw_unit_test/framework.h"

namespace examples {

// DOCSTAG: [pw_containers-intrusive_list]

using pw::containers::future::IntrusiveList;

class IntWrapper : public IntrusiveList<IntWrapper>::Item {
 public:
  IntWrapper(int value) : value_(value) {}
  int value() const { return value_; }

 private:
  const int value_;
};

// This example is adapted from https://en.cppreference.com/w/cpp/container/list
int CreateAndSum() {
  // Create a list containing integers
  std::array<IntWrapper, 4> wrappers = {{{6}, {7}, {3}, {0}}};
  IntrusiveList<IntWrapper> list(wrappers.begin(), wrappers.end());

  // Add an integer to the front of the list
  IntWrapper eight(8);
  list.push_front(eight);

  // Add an integer to the back of the list
  IntWrapper nine(9);
  list.push_back(nine);

  // Insert an integer before 3 by searching
  IntWrapper five(5);
  auto it = list.begin();
  while (it != list.end()) {
    if (it->value() == 3) {
      list.insert(it, five);
      break;
    }
    ++it;
  }

  // Sum the list
  int sum = 0;
  for (const auto& wrapper : list) {
    sum += wrapper.value();
  }

  // It is an error for items to go out of scope while still listed, or for a
  // list to go out of scope while it still has items.
  list.clear();

  return sum;
}

// DOCSTAG: [pw_containers-intrusive_list]

}  // namespace examples

namespace {

TEST(IntrusiveListExampleTest, CreateAndSum) {
  EXPECT_EQ(examples::CreateAndSum(), 8 + 6 + 7 + 5 + 3 + 0 + 9);
}

}  // namespace
