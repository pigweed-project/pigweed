// Copyright 2020 The Pigweed Authors
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
#include <cstdint>
#include <limits>
#include <utility>

#include "pw_polyfill/language_feature_macros.h"
#include "pw_unit_test/framework.h"

namespace pw::containers {
namespace {

using Single = FlatMap<int, char, 1>;

constexpr FlatMap<int, char, 5> kOddMap({{
    {-3, 'a'},
    {0, 'b'},
    {1, 'c'},
    {50, 'd'},
    {100, 'e'},
}});

}  // namespace

TEST(FlatMap, PairEquality) {
  Pair<char, int> p1{'a', 1};
  Pair<char, int> p2{'a', 1};
  Pair<char, int> p3{'b', 1};
  Pair<char, int> p4{'a', 2};

  EXPECT_EQ(p1, p2);
  EXPECT_NE(p1, p3);
  EXPECT_NE(p1, p4);
}

TEST(FlatMap, Size) { EXPECT_EQ(kOddMap.size(), static_cast<uint32_t>(5)); }

TEST(FlatMap, EmptyFlatMapSize) {
  PW_CONSTEXPR_CPP20 FlatMap<int, char, 0> kEmpty({{}});
  EXPECT_EQ(kEmpty.size(), static_cast<uint32_t>(0));
}

TEST(FlatMap, Empty) {
  PW_CONSTEXPR_CPP20 FlatMap<int, char, 0> kEmpty({{}});
  EXPECT_TRUE(kEmpty.empty());
}

TEST(FlatMap, NotEmpty) {
  PW_CONSTEXPR_CPP20 FlatMap<int, char, 1> kNotEmpty({{}});
  EXPECT_FALSE(kNotEmpty.empty());
}

TEST(FlatMap, EmptyFlatMapFind) {
  PW_CONSTEXPR_CPP20 FlatMap<int, char, 0> kEmpty({{}});
  EXPECT_EQ(kEmpty.find(0), kEmpty.end());
}

TEST(FlatMap, EmptyFlatMapLowerBound) {
  PW_CONSTEXPR_CPP20 FlatMap<int, char, 0> kEmpty({{}});
  EXPECT_EQ(kEmpty.lower_bound(0), kEmpty.end());
}

TEST(FlatMap, EmptyFlatMapUpperBound) {
  PW_CONSTEXPR_CPP20 FlatMap<int, char, 0> kEmpty({{}});
  EXPECT_EQ(kEmpty.upper_bound(0), kEmpty.end());
}

TEST(FlatMap, EmptyEqualRange) {
  PW_CONSTEXPR_CPP20 FlatMap<int, char, 0> kEmpty({{}});
  EXPECT_EQ(kEmpty.equal_range(0).first, kEmpty.end());
  EXPECT_EQ(kEmpty.equal_range(0).second, kEmpty.end());
}

TEST(FlatMap, ConstAtReturnsCorrectValues) {
  for (const auto& [key, value] : kOddMap) {
    EXPECT_EQ(value, kOddMap.at(key));
  }
}

TEST(FlatMap, MutableAtReturnsCorrectMutableValues) {
  FlatMap<int, char, 5> mutable_map({{
      {-4, 'a'},
      {-1, 'b'},
      {0, 'c'},
      {49, 'd'},
      {99, 'e'},
  }});

  for (const auto& [key, value] : mutable_map) {
    const char original_value{value};
    EXPECT_EQ(original_value, mutable_map.at(key));
    ++mutable_map.at(key);
    EXPECT_EQ(original_value + 1, mutable_map.at(key));
  }
}

TEST(FlatMap, Contains) {
  EXPECT_TRUE(kOddMap.contains(0));
  EXPECT_FALSE(kOddMap.contains(10));
}

TEST(FlatMap, Iterate) {
  char value = 'a';
  for (const auto& item : kOddMap) {
    EXPECT_EQ(value, item.second);
    EXPECT_EQ(&item, &(*kOddMap.find(item.first)));
    value += 1;
  }
}

TEST(FlatMap, ForwardsMappedValuesIterationWithDeferenceWorks) {
  FlatMap<int, char, 5> map({{
      {-4, 'a'},
      {-1, 'b'},
      {0, 'c'},
      {49, 'd'},
      {99, 'e'},
  }});

  char value = 'a';
  for (auto it = map.mapped_begin(); it != map.mapped_end(); ++it) {
    EXPECT_EQ(value, *it);
    ++value;
  }
}

TEST(FlatMap, BackwardsMappedValuesIterationWithDereferenceWorks) {
  FlatMap<int, char, 5> map({{
      {-4, 'a'},
      {-1, 'b'},
      {0, 'c'},
      {49, 'd'},
      {99, 'e'},
  }});

  char value = 'e';
  auto it = map.mapped_end();
  do {
    --it;
    EXPECT_EQ(value, *it);
    --value;
  } while (it != map.mapped_begin());
}

TEST(FlatMap, EqualRange) {
  auto pair = kOddMap.equal_range(1);
  EXPECT_EQ(1, pair.first->first);
  EXPECT_EQ(50, pair.second->first);

  pair = kOddMap.equal_range(75);
  EXPECT_EQ(100, pair.first->first);
  EXPECT_EQ(100, pair.second->first);
}

TEST(FlatMap, Find) {
  auto it = kOddMap.find(50);
  EXPECT_EQ(50, it->first);
  EXPECT_EQ('d', it->second);

  auto not_found = kOddMap.find(-1);
  EXPECT_EQ(kOddMap.cend(), not_found);
}

TEST(FlatMap, UpperBoundLessThanSmallestKey) {
  EXPECT_EQ(-3, kOddMap.upper_bound(std::numeric_limits<int>::min())->first);
  EXPECT_EQ(-3, kOddMap.upper_bound(-123)->first);
  EXPECT_EQ(-3, kOddMap.upper_bound(-4)->first);
}

TEST(FlatMap, UpperBoundBetweenTheTwoSmallestKeys) {
  EXPECT_EQ(0, kOddMap.upper_bound(-3)->first);
  EXPECT_EQ(0, kOddMap.upper_bound(-2)->first);
  EXPECT_EQ(0, kOddMap.upper_bound(-1)->first);
}

TEST(FlatMap, UpperBoundIntermediateKeys) {
  EXPECT_EQ(1, kOddMap.upper_bound(0)->first);
  EXPECT_EQ('c', kOddMap.upper_bound(0)->second);
  EXPECT_EQ(50, kOddMap.upper_bound(1)->first);
  EXPECT_EQ('d', kOddMap.upper_bound(1)->second);
  EXPECT_EQ(50, kOddMap.upper_bound(2)->first);
  EXPECT_EQ(50, kOddMap.upper_bound(49)->first);
  EXPECT_EQ(100, kOddMap.upper_bound(51)->first);
}

TEST(FlatMap, UpperBoundGreaterThanLargestKey) {
  EXPECT_EQ(kOddMap.end(), kOddMap.upper_bound(100));
  EXPECT_EQ(kOddMap.end(), kOddMap.upper_bound(2384924));
  EXPECT_EQ(kOddMap.end(),
            kOddMap.upper_bound(std::numeric_limits<int>::max()));
}

TEST(FlatMap, LowerBoundLessThanSmallestKey) {
  EXPECT_EQ(-3, kOddMap.lower_bound(std::numeric_limits<int>::min())->first);
  EXPECT_EQ(-3, kOddMap.lower_bound(-123)->first);
  EXPECT_EQ(-3, kOddMap.lower_bound(-4)->first);
}

TEST(FlatMap, LowerBoundBetweenTwoSmallestKeys) {
  EXPECT_EQ(-3, kOddMap.lower_bound(-3)->first);
  EXPECT_EQ(0, kOddMap.lower_bound(-2)->first);
  EXPECT_EQ(0, kOddMap.lower_bound(-1)->first);
}

TEST(FlatMap, LowerBoundIntermediateKeys) {
  EXPECT_EQ(0, kOddMap.lower_bound(0)->first);
  EXPECT_EQ('b', kOddMap.lower_bound(0)->second);
  EXPECT_EQ(1, kOddMap.lower_bound(1)->first);
  EXPECT_EQ('c', kOddMap.lower_bound(1)->second);
  EXPECT_EQ(50, kOddMap.lower_bound(2)->first);
  EXPECT_EQ(50, kOddMap.lower_bound(49)->first);
  EXPECT_EQ(100, kOddMap.lower_bound(51)->first);
}

TEST(FlatMap, LowerBoundGreaterThanLargestKey) {
  EXPECT_EQ(100, kOddMap.lower_bound(100)->first);
  EXPECT_EQ(kOddMap.end(), kOddMap.lower_bound(2384924));
  EXPECT_EQ(kOddMap.end(),
            kOddMap.lower_bound(std::numeric_limits<int>::max()));
}

TEST(FlatMap, ForEachIteration) {
  for (const auto& item : kOddMap) {
    EXPECT_NE(item.first, 2);
  }
}

TEST(FlatMap, MapsWithUnsortedKeys) {
  constexpr FlatMap<int, const char*, 2> bad_array({{
      {2, "hello"},
      {1, "goodbye"},
  }});

  EXPECT_EQ(bad_array.begin()->first, 1);

  constexpr FlatMap<int, const char*, 2> too_short({{
      {1, "goodbye"},
  }});
  EXPECT_EQ(too_short.begin()->first, 0);
}

TEST(FlatMap, DontDereferenceEnd) {
  constexpr FlatMap<int, const char*, 2> unsorted_array({{
      {2, "hello"},
      {1, "goodbye"},
  }});

  EXPECT_EQ(unsorted_array.contains(3), false);
}

TEST(FlatMap, NullMappedIteratorNotEqualToValidOne) {
  Single map({{{-4, 'a'}}});

  EXPECT_NE(Single::mapped_iterator(), map.mapped_begin());
}

TEST(FlatMap, CopyConstructedMapIteratorEqualToSource) {
  Single map({{{-4, 'a'}}});

  EXPECT_EQ(Single::mapped_iterator(map.mapped_begin()), map.mapped_begin());
}

TEST(FlatMap, CopyAssignedMapIteratorEqualToSource) {
  Single map({{{-4, 'a'}}});

  Single::mapped_iterator it;
  EXPECT_NE(it, map.mapped_begin());
  it = map.mapped_begin();
  EXPECT_EQ(it, map.mapped_begin());
}

TEST(FlatMap, MappedIteratorCorrectDereferenceMutation) {
  constexpr int kKey = -4;
  constexpr char kValue = 'a';
  Single mutable_map({{{kKey, kValue}}});

  ++*mutable_map.mapped_begin();
  EXPECT_EQ(kValue + 1, mutable_map.at(kKey));
}

TEST(FlatMap, MappedIteratorValueCorrectMemberAccess) {
  constexpr int kAValue{5};
  struct A {
    int a;
  };
  FlatMap<int, A, 1> map({{{-4, A{kAValue}}}});

  EXPECT_EQ(kAValue, map.mapped_begin()->a);
}

TEST(FlatMap, MappedIteratorValueCorrectMemberMutation) {
  constexpr int kAValue{5};
  struct A {
    int a;
  };
  FlatMap<int, A, 1> map({{{-4, A{kAValue}}}});

  ++map.mapped_begin()->a;
  EXPECT_EQ(kAValue + 1, map.mapped_begin()->a);
}

TEST(FlatMap, MappedIteratorPrefixIncrementCorrectReturnAndSideEffect) {
  Single map({{{-4, 'a'}}});

  auto it = map.mapped_begin();
  auto it_incremented = ++it;
  EXPECT_EQ(it, it_incremented);
  EXPECT_NE(map.mapped_begin(), it_incremented);
}

TEST(FlatMap, MappedIteratorPostfixIncrementCorrectReturnAndSideEffect) {
  Single map({{{-4, 'a'}}});

  auto it = map.mapped_begin();
  auto it_original = it++;
  EXPECT_EQ(map.mapped_begin(), it_original);
  EXPECT_NE(it, it_original);
}

TEST(FlatMap, MappedIteratorPrefixDecrementCorrectReturnAndSideEffect) {
  Single map({{{-4, 'a'}}});

  auto it = map.mapped_end();
  auto it_decremented = --it;
  EXPECT_EQ(it, it_decremented);
  EXPECT_NE(map.mapped_end(), it_decremented);
}

TEST(FlatMap, MappedIteratorPostfixDecrementCorrectReturnAndSideEffect) {
  Single map({{{-4, 'a'}}});

  auto it = map.mapped_end();
  auto it_original = it--;
  EXPECT_EQ(map.mapped_end(), it_original);
  EXPECT_NE(it, it_original);
}

TEST(FlatMap, PairsConstructionWithOneElement) {
  constexpr FlatMap kMap(Pair<int, char>{1, 'a'});

  ASSERT_EQ(kMap.size(), 1u);
  ASSERT_NE(kMap.find(1), kMap.cend());
  EXPECT_EQ(kMap.at(1), 'a');
}

TEST(FlatMap, PairsConstructionWithMoreThanOneElements) {
  using FlatMapItem = Pair<int, char>;
  constexpr FlatMap kMap = {
      FlatMapItem{-4, 'a'},
      FlatMapItem{-1, 'b'},
      FlatMapItem{0, 'c'},
      FlatMapItem{49, 'd'},
      FlatMapItem{99, 'e'},
  };

  ASSERT_EQ(kMap.size(), 5u);
  for (const auto& [key, value] : kMap) {
    EXPECT_EQ(kMap.at(key), value);
  }
}

namespace {
class MoveOnlyObject {
 public:
  MoveOnlyObject(const MoveOnlyObject&) = delete;
  MoveOnlyObject& operator=(const MoveOnlyObject&) = delete;

  MoveOnlyObject(MoveOnlyObject&& other) {
    val_ = other.val_;
    other.val_ = -1;
  }

  MoveOnlyObject& operator=(MoveOnlyObject&& other) {
    val_ = other.val_;
    other.val_ = -1;
    return *this;
  }

  ~MoveOnlyObject() = default;

  static MoveOnlyObject Create(int val) { return MoveOnlyObject(val); }

  int val_ = 0;

 private:
  MoveOnlyObject(int val) : val_(val) {}
};

}  // namespace

TEST(FlatMap, ConstructWithArrayWithMoveOnlyObject) {
  std::array<Pair<int, MoveOnlyObject>, 2> arr{{
      {1, MoveOnlyObject::Create(4)},
      {-3, MoveOnlyObject::Create(5)},
  }};

  FlatMap map(std::move(arr));

  EXPECT_EQ(map.begin()->first, -3);
  EXPECT_EQ(map.at(1).val_, 4);
  EXPECT_EQ(map.at(-3).val_, 5);

  // Verify objects in arr were moved from.
  // NOLINTBEGIN(bugprone-use-after-move)
  EXPECT_EQ(arr[0].second.val_, -1);
  EXPECT_EQ(arr[1].second.val_, -1);
  // NOLINTEND(bugprone-use-after-move)
}

TEST(FlatMap, ConstructDirectWithMoveOnlyObject) {
  FlatMap<int, MoveOnlyObject, 2> map({{
      {1, MoveOnlyObject::Create(4)},
      {-3, MoveOnlyObject::Create(5)},
  }});

  EXPECT_EQ(map.begin()->first, -3);
  EXPECT_EQ(map.at(1).val_, 4);
  EXPECT_EQ(map.at(-3).val_, 5);
}

}  // namespace pw::containers
