// Copyright 2021 The Pigweed Authors
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

#include "pw_containers/filtered_view.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

#include "pw_containers/algorithm.h"
#include "pw_containers/flat_map.h"
#include "pw_containers/intrusive_list.h"
#include "pw_span/span.h"
#include "pw_unit_test/framework.h"

namespace pw::containers {
namespace {

struct Item : IntrusiveList<Item>::Item {
  constexpr Item(int x) : value(x) {}

  int value;
};

constexpr std::array<int, 6> kArray{0, 1, 2, 3, 4, 5};

TEST(FilteredView, MoveConstructor) {
  FilteredView original(kArray, [](int x) { return x == 3 || x == 5; });
  FilteredView moved(std::move(original));

  auto it = moved.begin();
  ASSERT_EQ(*it, 3);
  ++it;
  ASSERT_EQ(*it, 5);
  ++it;
  EXPECT_EQ(it, moved.end());
}

TEST(FilteredView, MoveOperator) {
  // Force lambda to function pointer with `+` so the FilteredView types match.
  FilteredView original(kArray, +[](int x) { return x == 3 || x == 5; });
  FilteredView moved(kArray, +[](int x) { return x == 4; });
  moved = std::move(original);

  auto it = moved.begin();
  ASSERT_EQ(*it, 3);
  ++it;
  ASSERT_EQ(*it, 5);
  ++it;
  EXPECT_EQ(it, moved.end());
}

TEST(FilteredView, Array_MatchSubset) {
  FilteredView view(kArray, [](int x) { return x == 3 || x == 5; });

  auto it = view.begin();
  ASSERT_EQ(*it, 3);
  ++it;
  ASSERT_EQ(*it, 5);
  ++it;
  EXPECT_EQ(it, view.end());
}

TEST(FilteredView, Array_MatchAll) {
  FilteredView view(kArray, [](int) { return true; });

  std::array<bool, 6> found = {};
  for (int value : view) {
    found[static_cast<size_t>(value)] = true;
  }
  EXPECT_TRUE(
      std::all_of(found.begin(), found.end(), [](bool b) { return b; }));
}

TEST(FilteredView, Array_MatchNone) {
  for (int unused : FilteredView(kArray, [](int) { return false; })) {
    static_cast<void>(unused);
    FAIL();
  }
}

TEST(FilteredView, EmptyContainer) {
  constexpr std::array<int, 0> nothing{};
  for (int unused : FilteredView(nothing, [](int) { return true; })) {
    static_cast<void>(unused);
    FAIL();
  }

  IntrusiveList<Item> intrusive_list;
  for (const Item& unused :
       FilteredView(intrusive_list, [](const Item&) { return true; })) {
    static_cast<void>(unused);
    FAIL();
  }
}

TEST(FilteredView, IntrusiveList_MatchSubset) {
  Item item_1{1};
  Item item_2{2};
  Item item_3{3};
  IntrusiveList<Item> intrusive_list({&item_1, &item_2, &item_3});

  FilteredView view(intrusive_list,
                    [](const Item& i) { return i.value % 2 != 0; });

  auto it = view.begin();
  ASSERT_EQ(it->value, 1);
  ++it;
  ASSERT_EQ((*it).value, 3);
  ++it;
  EXPECT_EQ(it, view.end());
}

TEST(FilteredView, IntrusiveList_MatchAll) {
  Item item_1{0};
  Item item_2{1};
  Item item_3{2};
  IntrusiveList<Item> intrusive_list({&item_1, &item_2, &item_3});

  std::array<bool, 3> found = {};

  for (const Item& item :
       FilteredView(intrusive_list, [](const Item&) { return true; })) {
    found[static_cast<size_t>(item.value)] = true;
  }
  EXPECT_TRUE(
      std::all_of(found.begin(), found.end(), [](bool b) { return b; }));
}

TEST(FilteredView, IntrusiveList_CopyPredicate) {
  Item item_1{1};
  Item item_2{2};
  Item item_3{3};
  IntrusiveList<Item> intrusive_list({&item_1, &item_2, &item_3});

  auto filter_to_copy = [](const Item& i) { return i.value % 2 != 0; };
  FilteredView view(intrusive_list, filter_to_copy);

  auto it = view.begin();
  ASSERT_EQ(it->value, 1);
  ++it;
  ASSERT_EQ((*it).value, 3);
  ++it;
  EXPECT_EQ(it, view.end());
}

TEST(FilteredView, IntrusiveList_MatchNone) {
  Item item_1{0};
  Item item_2{1};
  Item item_3{2};
  IntrusiveList<Item> intrusive_list({&item_1, &item_2, &item_3});

  for (const Item& unused :
       FilteredView(intrusive_list, [](const Item&) { return false; })) {
    static_cast<void>(unused);
    FAIL();
  }
}

TEST(FilteredView, Front_OneElement) {
  EXPECT_EQ(FilteredView(kArray, [](int x) { return x == 0; }).front(), 0);
}

TEST(FilteredView, Back_OneElement) {
  EXPECT_EQ(FilteredView(kArray, [](int x) { return x == 0; }).back(), 0);
}

TEST(FilteredView, Front_MultipleElements) {
  EXPECT_EQ(
      FilteredView(kArray, [](int x) { return x == 3 || x == 5; }).front(), 3);
}

TEST(FilteredView, Back_MultipleElements) {
  EXPECT_EQ(FilteredView(kArray, [](int x) { return x == 3 || x == 5; }).back(),
            5);
}

TEST(FilteredView, Size_Empty) {
  EXPECT_EQ(FilteredView(kArray, [](int x) { return x < 0; }).size(), 0u);

  EXPECT_TRUE(FilteredView(kArray, [](int x) { return x < 0; }).empty());

  constexpr std::array<int, 0> empty{};
  FilteredView empty_view(empty, [](const Item&) { return true; });
  EXPECT_EQ(empty_view.size(), 0u);
  EXPECT_TRUE(empty_view.empty());
}

TEST(FilteredView, Size_OneElement) {
  EXPECT_EQ(FilteredView(kArray, [](int x) { return x == 0; }).size(), 1u);
  EXPECT_EQ(FilteredView(kArray, [](int x) { return x == 3; }).size(), 1u);
  EXPECT_EQ(FilteredView(kArray, [](int x) { return x == 5; }).size(), 1u);

  EXPECT_FALSE(FilteredView(kArray, [](int x) { return x == 5; }).empty());
}

TEST(FilteredView, Size_MultipleElements) {
  EXPECT_EQ(FilteredView(kArray, [](int x) { return x <= 1; }).size(), 2u);
  EXPECT_EQ(FilteredView(kArray, [](int x) { return x > 1; }).size(), 4u);
  EXPECT_EQ(FilteredView(kArray, [](int x) { return x < 5; }).size(), 5u);

  EXPECT_FALSE(FilteredView(kArray, [](int x) { return x < 5; }).empty());
}

TEST(FilteredView, Size_AllElements) {
  EXPECT_EQ(FilteredView(kArray, [](int) { return true; }).size(), 6u);

  EXPECT_FALSE(FilteredView(kArray, [](int x) { return x < 5; }).empty());
}

TEST(FilteredView, FlatMap) {
  pw::containers::FlatMap<char, int, 3> map({{{'A', 1}, {'B', 2}, {'C', 3}}});

  auto odd_pred = [](pw::containers::Pair<char, int> p) {
    return p.second % 2 != 0;
  };

  auto odd_map = pw::containers::FilteredView(map, std::move(odd_pred));

  pw::containers::FlatMap<char, int, 2> odd_expect({{{'A', 1}, {'C', 3}}});

  ASSERT_TRUE(pw::containers::Equal(odd_map, odd_expect));
}

}  // namespace
}  // namespace pw::containers
