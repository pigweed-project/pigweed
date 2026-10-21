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

#include "pw_containers/flat_map.h"

#include <array>

#include "pw_unit_test/framework.h"

namespace examples {

// DOCSTAG: [pw_containers-flat_map]

using pw::containers::FlatMap;
using pw::containers::Pair;

// Initialized by an initializer list.
FlatMap<int, char, 2> my_flat_map1({{
    {1, 'a'},
    {-3, 'b'},
}});

// Initialized by a std::array of Pair<K, V> objects.
std::array<Pair<int, char>, 2> my_array{{
    {1, 'a'},
    {-3, 'b'},
}};
FlatMap my_flat_map2(my_array);

// Initialized by Pair<K, V> objects.
FlatMap my_flat_map3 = {
    Pair<int, char>{1, 'a'},
    Pair<int, char>{-3, 'b'},
};

// DOCSTAG: [pw_containers-flat_map]

}  // namespace examples

namespace {

TEST(FlapMapExampleTest, CheckValues) {
  EXPECT_EQ(examples::my_flat_map1.at(1), 'a');
  EXPECT_EQ(examples::my_flat_map1.at(-3), 'b');
  EXPECT_EQ(examples::my_flat_map2.at(1), 'a');
  EXPECT_EQ(examples::my_flat_map2.at(-3), 'b');
  EXPECT_EQ(examples::my_flat_map3.at(1), 'a');
  EXPECT_EQ(examples::my_flat_map3.at(-3), 'b');
}

}  // namespace
