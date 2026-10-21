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

#include "pw_containers/inline_queue.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "pw_compilation_testing/negative_compilation.h"
#include "pw_containers/algorithm.h"
#include "pw_containers/internal/test_helpers.h"
#include "pw_unit_test/framework.h"

namespace pw::containers {
namespace {

using namespace std::literals::string_view_literals;
using test::CopyOnly;
using test::Counter;
using test::MoveOnly;

static_assert(!std::is_constructible_v<pw::InlineQueue<int>>,
              "Cannot construct generic capacity container");

TEST(InlineQueue, Construct_Sized) {
  InlineQueue<int, 3> queue;
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(queue.size(), 0u);
  EXPECT_EQ(queue.capacity(), 3u);
  EXPECT_EQ(queue.max_size(), 3u);
}

TEST(InlineQueue, Construct_GenericSized) {
  InlineQueue<int, 3> sized_queue;
  InlineQueue<int>& queue(sized_queue);
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(queue.size(), 0u);
  EXPECT_EQ(queue.capacity(), 3u);
  EXPECT_EQ(queue.max_size(), 3u);
}

TEST(InlineQueue, Construct_ConstexprSized) {
  constexpr InlineQueue<int, 3> queue(pw::kConstexpr);
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(queue.size(), 0u);
  EXPECT_EQ(queue.capacity(), 3u);
  EXPECT_EQ(queue.max_size(), 3u);
}

TEST(InlineQueue, Construct_CopySameCapacity) {
  InlineQueue<CopyOnly, 4> queue(4, CopyOnly(123));
  InlineQueue<CopyOnly, 4> copied(queue);

  EXPECT_EQ(4u, queue.size());
  EXPECT_EQ(123, queue[3].value);

  EXPECT_EQ(4u, copied.size());
  EXPECT_EQ(123, copied[3].value);
}

TEST(InlineQueue, Construct_CopyLargerCapacity) {
  InlineQueue<CopyOnly, 4> queue(4, CopyOnly(123));
  InlineQueue<CopyOnly, 5> copied(queue);

  EXPECT_EQ(4u, queue.size());
  EXPECT_EQ(123, queue[3].value);

  EXPECT_EQ(4u, copied.size());
  EXPECT_EQ(123, copied[3].value);
}

TEST(InlineQueue, Construct_CopySmallerCapacity) {
  InlineQueue<CopyOnly, 4> queue(3, CopyOnly(123));
  InlineQueue<CopyOnly, 3> copied(queue);

  EXPECT_EQ(3u, queue.size());
  EXPECT_EQ(123, queue[2].value);

  EXPECT_EQ(3u, copied.size());
  EXPECT_EQ(123, copied[2].value);
}

TEST(InlineQueue, Construct_MoveSameCapacity) {
  InlineQueue<MoveOnly, 4> queue;
  queue.emplace(MoveOnly(1));
  queue.emplace(MoveOnly(2));
  queue.emplace(MoveOnly(3));
  queue.emplace(MoveOnly(4));
  InlineQueue<MoveOnly, 4> moved(std::move(queue));

  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(0u, queue.size());

  ASSERT_EQ(4u, moved.size());
  EXPECT_EQ(4, moved[3].value);
}

TEST(InlineQueue, Construct_MoveLargerCapacity) {
  InlineQueue<MoveOnly, 4> queue;
  queue.emplace(MoveOnly(1));
  queue.emplace(MoveOnly(2));
  queue.emplace(MoveOnly(3));
  queue.emplace(MoveOnly(4));
  InlineQueue<MoveOnly, 5> moved(std::move(queue));

  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(0u, queue.size());

  ASSERT_EQ(4u, moved.size());
  EXPECT_EQ(4, moved[3].value);
}

TEST(InlineQueue, Construct_MoveSmallerCapacity) {
  InlineQueue<MoveOnly, 4> queue;
  queue.emplace(MoveOnly(1));
  queue.emplace(MoveOnly(2));
  queue.emplace(MoveOnly(3));
  InlineQueue<MoveOnly, 3> moved(std::move(queue));

  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(0u, queue.size());

  ASSERT_EQ(3u, moved.size());
  EXPECT_EQ(3, moved[2].value);
}

TEST(InlineQueue, Destruct_ZeroLength) {
  Counter::Reset();
  {
    InlineQueue<Counter, 0> queue;
    EXPECT_EQ(queue.size(), 0u);
  }
  EXPECT_EQ(Counter::created, 0);
  EXPECT_EQ(Counter::destroyed, 0);
}

TEST(InlineQueue, Destruct_Empty) {
  Counter::Reset();

  {
    InlineQueue<Counter, 3> queue;
    EXPECT_EQ(queue.size(), 0u);
  }
  EXPECT_EQ(Counter::created, 0);
  EXPECT_EQ(Counter::destroyed, 0);
}

TEST(InlineQueue, Destruct_MultipleEntries) {
  Counter value;
  Counter::Reset();

  {
    InlineQueue<Counter, 128> queue(100, value);
  }

  EXPECT_EQ(Counter::created, 100);
  EXPECT_EQ(Counter::destroyed, 100);
}

TEST(InlineQueue, Assign_InitializerList) {
  InlineQueue<int, 4> queue = {1, 3, 5, 7};

  EXPECT_EQ(4u, queue.size());

  EXPECT_EQ(1, queue[0]);
  EXPECT_EQ(3, queue[1]);
  EXPECT_EQ(5, queue[2]);
  EXPECT_EQ(7, queue[3]);
}

TEST(InlineQueue, Assign_CopySameCapacity) {
  InlineQueue<CopyOnly, 4> queue(4, CopyOnly(123));
  InlineQueue<CopyOnly, 4> copied = queue;

  EXPECT_EQ(4u, queue.size());
  EXPECT_EQ(123, queue[3].value);

  EXPECT_EQ(4u, copied.size());
  EXPECT_EQ(123, copied[3].value);
}

TEST(InlineQueue, Assign_CopyLargerCapacity) {
  InlineQueue<CopyOnly, 4> queue(4, CopyOnly(123));
  InlineQueue<CopyOnly, 5> copied = queue;

  EXPECT_EQ(4u, queue.size());
  EXPECT_EQ(123, queue[3].value);

  EXPECT_EQ(4u, copied.size());
  EXPECT_EQ(123, copied[3].value);
}

TEST(InlineQueue, Assign_CopySmallerCapacity) {
  InlineQueue<CopyOnly, 4> queue(3, CopyOnly(123));
  InlineQueue<CopyOnly, 3> copied = queue;

  EXPECT_EQ(3u, queue.size());
  EXPECT_EQ(123, queue[2].value);

  EXPECT_EQ(3u, copied.size());
  EXPECT_EQ(123, copied[2].value);
}

TEST(InlineQueue, Access_Iterator) {
  InlineQueue<Counter, 2> queue(2);
  for (Counter& item : queue) {
    EXPECT_EQ(item.value, 0);
  }
  for (const Counter& item : queue) {
    EXPECT_EQ(item.value, 0);
  }
}

TEST(InlineQueue, Access_ConstIterator) {
  const InlineQueue<Counter, 2> queue(2);
  for (const Counter& item : queue) {
    EXPECT_EQ(item.value, 0);
  }
}

TEST(InlineQueue, Access_ZeroLength) {
  InlineQueue<Counter, 0> queue;

  EXPECT_EQ(0u, queue.size());
  EXPECT_EQ(0u, queue.capacity());
  EXPECT_EQ(0u, queue.max_size());
  EXPECT_TRUE(queue.empty());
  EXPECT_TRUE(queue.full());

  for (Counter& item : queue) {
    (void)item;
    FAIL();
  }
}

TEST(InlineQueue, Access_ContiguousData) {
  // Content = {}, Storage = [x, x]
  InlineQueue<int, 2> queue;

  {
    auto [first, second] = queue.contiguous_data();
    EXPECT_EQ(first.size(), 0u);
    EXPECT_EQ(second.size(), 0u);
  }

  // Content = {1}, Storage = [1, x]
  queue.push(1);
  {
    auto [first, second] = queue.contiguous_data();
    EXPECT_TRUE(Equal(first, std::array<int, 1>{1}));
    EXPECT_TRUE(Equal(second, std::array<int, 0>{}));
  }

  // Content = {1, 2}, Storage = [1, 2]
  queue.push(2);
  EXPECT_TRUE(queue.full());
  {
    auto [first, second] = queue.contiguous_data();
    EXPECT_TRUE(Equal(first, std::array<int, 2>{1, 2}));
    EXPECT_TRUE(Equal(second, std::array<int, 0>{}));
  }

  // Content = {2}, Storage = [x, 2]
  queue.pop();
  {
    auto [first, second] = queue.contiguous_data();
    EXPECT_TRUE(Equal(first, std::array<int, 1>{2}));
    EXPECT_TRUE(Equal(second, std::array<int, 0>{}));
  }

  // Content = {2, 1}, Storage = [1, 2]
  queue.push(1);
  {
    auto [first, second] = queue.contiguous_data();
    EXPECT_TRUE(Equal(first, std::array<int, 1>{2}));
    EXPECT_TRUE(Equal(second, std::array<int, 1>{1}));
  }

  // Content = {1}, Storage = [1, x]
  queue.pop();
  {
    auto [first, second] = queue.contiguous_data();
    EXPECT_TRUE(Equal(first, std::array<int, 1>{1}));
    EXPECT_TRUE(Equal(second, std::array<int, 0>{}));
  }

  // Content = {1, 2}, Storage = [1, 2]
  queue.push(2);
  {
    auto [first, second] = queue.contiguous_data();
    EXPECT_TRUE(Equal(first, std::array<int, 2>{1, 2}));
    EXPECT_TRUE(Equal(second, std::array<int, 0>{}));
  }
}

TEST(InlineQueue, Access_ConstContiguousData) {
  // Content = {1, 2}, Storage = [1, 2]
  const InlineQueue<int, 2> queue = {1, 2};

  {
    auto [first, second] = queue.contiguous_data();
    EXPECT_EQ(first.size(), 2u);
    EXPECT_EQ(second.size(), 0u);
  }
}

TEST(InlineQueue, Modify_Clear) {
  Counter::Reset();

  InlineQueue<Counter, 100> queue;
  queue.emplace();
  queue.emplace();
  queue.emplace();

  queue.clear();

  EXPECT_EQ(3, Counter::created);
  EXPECT_EQ(3, Counter::destroyed);
}

TEST(InlineQueue, Modify_Push_Copy) {
  Counter value(99);
  Counter::Reset();

  {
    InlineQueue<Counter, 10> queue;
    queue.push(value);

    EXPECT_EQ(queue.size(), 1u);
    EXPECT_EQ(queue.front().value, 99);
  }

  EXPECT_EQ(Counter::created, 1);
  EXPECT_EQ(Counter::destroyed, 1);
}

TEST(InlineQueue, Modify_Push_Move) {
  Counter::Reset();

  {
    Counter value(99);
    InlineQueue<Counter, 10> queue;
    queue.push(std::move(value));

    EXPECT_EQ(queue.size(), 1u);
    EXPECT_EQ(queue.front().value, 99);
    // NOLINTNEXTLINE(bugprone-use-after-move)
    EXPECT_EQ(value.value, 0);
  }

  EXPECT_EQ(Counter::created, 1);
  EXPECT_EQ(Counter::destroyed, 2);
  EXPECT_EQ(Counter::moved, 1);
}

TEST(InlineQueue, Modify_Emplace) {
  Counter::Reset();

  {
    InlineQueue<Counter, 10> queue;
    queue.emplace(314);

    EXPECT_EQ(queue.size(), 1u);
    EXPECT_EQ(queue.front().value, 314);
  }

  EXPECT_EQ(Counter::created, 1);
  EXPECT_EQ(Counter::destroyed, 1);
}

TEST(InlineQueue, Modify_Overwrite) {
  Counter::Reset();
  {
    InlineQueue<Counter, 2> queue(2);
    queue.push_overwrite(1);
    queue.emplace_overwrite(2);

    EXPECT_EQ(queue.size(), 2u);
    EXPECT_EQ(queue.front().value, 1);
    EXPECT_EQ(queue.back().value, 2);
  }
}

TEST(InlineQueue, Modify_Overwrite_Trivial) {
  InlineQueue<int, 2> queue;

  // Not full.
  queue.push_overwrite(1);
  EXPECT_EQ(queue.size(), 1u);
  EXPECT_EQ(queue.front(), 1);

  // Full.
  queue.push_overwrite(2);
  EXPECT_EQ(queue.size(), 2u);
  EXPECT_EQ(queue.front(), 1);
  EXPECT_EQ(queue.back(), 2);

  // Overwrite oldest.
  queue.push_overwrite(3);
  EXPECT_EQ(queue.size(), 2u);
  EXPECT_EQ(queue.front(), 2);
  EXPECT_EQ(queue.back(), 3);

  // Overwrite oldest again.
  queue.emplace_overwrite(4);
  EXPECT_EQ(queue.size(), 2u);
  EXPECT_EQ(queue.front(), 3);
  EXPECT_EQ(queue.back(), 4);
}

TEST(InlineQueue, Modify_Overwrite_NonTrivial_Destruction) {
  Counter::Reset();
  {
    InlineQueue<Counter, 2> queue;
    queue.emplace_overwrite(1);
    queue.emplace_overwrite(2);

    EXPECT_EQ(Counter::created, 2);
    EXPECT_EQ(Counter::destroyed, 0);

    // Overwrite front. Front (1) should be destroyed.
    queue.emplace_overwrite(3);
    EXPECT_EQ(Counter::created, 3);
    EXPECT_EQ(Counter::destroyed, 1);
    EXPECT_EQ(queue.front().value, 2);
    EXPECT_EQ(queue.back().value, 3);

    // Overwrite front again. Front (2) should be destroyed.
    queue.emplace_overwrite(4);
    EXPECT_EQ(Counter::created, 4);
    EXPECT_EQ(Counter::destroyed, 2);
    EXPECT_EQ(queue.front().value, 3);
    EXPECT_EQ(queue.back().value, 4);
  }
  // All destroyed.
  EXPECT_EQ(Counter::destroyed, 4);
}

TEST(InlineQueue, Modify_Wrap) {
  Counter::Reset();

  {
    InlineQueue<Counter, 3> queue;
    queue.emplace(1);
    queue.emplace(2);
    queue.emplace(3);

    ASSERT_EQ(queue.size(), 3u);
    EXPECT_EQ(queue[0].value, 1);
    EXPECT_EQ(queue[1].value, 2);
    EXPECT_EQ(queue[2].value, 3);

    queue.pop();
    queue.emplace(4);

    ASSERT_EQ(queue.size(), 3u);
    EXPECT_EQ(queue[0].value, 2);
    EXPECT_EQ(queue[1].value, 3);
    EXPECT_EQ(queue[2].value, 4);
  }

  EXPECT_EQ(Counter::created, 4);
  EXPECT_EQ(Counter::destroyed, 4);
}

TEST(InlineQueue, Modify_Pop) {
  Counter::Reset();

  InlineQueue<Counter, 3> queue;
  queue.emplace(0);
  queue.pop();
  queue.emplace(0);
  queue.pop();
  queue.emplace(1);  // This wraps to the other end.
  queue.emplace(2);  // This is the first entry in storage.
  queue.emplace(3);
  // Content = {1, 2, 3}, Storage = [2, 3, 1]

  ASSERT_EQ(queue.size(), 3u);
  EXPECT_EQ(queue[0].value, 1);
  EXPECT_EQ(queue[1].value, 2);
  EXPECT_EQ(queue[2].value, 3);

  // This wraps around
  queue.pop();
  // Content = {2, 3}, Storage = [2, 3, x]

  EXPECT_EQ(queue.size(), 2u);
  EXPECT_EQ(queue[0].value, 2);
  EXPECT_EQ(queue[1].value, 3);

  queue.pop();
  // Content = {3}, Storage = [x, 3, x]
  ASSERT_EQ(queue.size(), 1u);
  EXPECT_EQ(queue[0].value, 3);

  EXPECT_EQ(Counter::created, 5);
  EXPECT_EQ(Counter::destroyed, 4);
}

TEST(InlineQueue, Generic) {
  InlineQueue<int, 10> queue({1, 2, 3, 4, 5});
  InlineQueue<int>& generic_queue(queue);

  EXPECT_EQ(generic_queue.size(), queue.size());
  EXPECT_EQ(generic_queue.max_size(), queue.max_size());

  uint16_t i = 0;
  for (int value : queue) {
    EXPECT_EQ(value, generic_queue[i]);
    i += 1;
  }

  i = 0;
  for (int value : generic_queue) {
    EXPECT_EQ(queue[i], value);
    i += 1;
  }
}

TEST(InlineQueue, ConstexprMaxSize) {
  InlineQueue<int, 10> queue;
  constexpr size_t kMaxSize = queue.max_size();
  EXPECT_EQ(queue.max_size(), kMaxSize);

  // Ensure the generic sized container does not have a constexpr max_size().
  [[maybe_unused]] InlineQueue<int>& generic_queue(queue);
#if PW_NC_TEST(InlineQueue_GenericMaxSize_NotConstexpr)
  PW_NC_EXPECT_CLANG(
      "kGenericMaxSize.* must be initialized by a constant expression");
  PW_NC_EXPECT_GCC("call to non-'constexpr' function .*Queue.*max_size");
  [[maybe_unused]] constexpr size_t kGenericMaxSize = generic_queue.max_size();
#endif  // PW_NC_TEST
}

TEST(InlineQueue, StdMaxElement) {
  // Content = {1, 2, 3, 4}, Storage = [1, 2, 3, 4]
  InlineQueue<int, 4> queue = {1, 2, 3, 4};

  auto max_element_it = std::max_element(queue.begin(), queue.end());
  ASSERT_NE(max_element_it, queue.end());
  EXPECT_EQ(*max_element_it, 4);

  // Content = {2, 3, 4, 5}, Storage = [5, 2, 3, 4]
  queue.push_overwrite(5);

  max_element_it = std::max_element(queue.begin(), queue.end());
  ASSERT_NE(max_element_it, queue.end());
  EXPECT_EQ(*max_element_it, 5);

  // Content = {3, 4, 5}, Storage = [5, x, 3, 4]
  queue.pop();

  max_element_it = std::max_element(queue.begin(), queue.end());
  ASSERT_NE(max_element_it, queue.end());
  EXPECT_EQ(*max_element_it, 5);

  // Content = {}, Storage = [x, x, x, x]
  queue.clear();

  max_element_it = std::max_element(queue.begin(), queue.end());
  ASSERT_EQ(max_element_it, queue.end());
}

TEST(InlineQueue, StdMaxElementConst) {
  // Content = {1, 2, 3, 4}, Storage = [1, 2, 3, 4]
  InlineQueue<int, 4> queue = {1, 2, 3, 4};

  auto max_element_it = std::max_element(queue.cbegin(), queue.cend());
  ASSERT_NE(max_element_it, queue.cend());
  EXPECT_EQ(*max_element_it, 4);

  // Content = {2, 3, 4, 5}, Storage = [5, 2, 3, 4]
  queue.push_overwrite(5);

  max_element_it = std::max_element(queue.cbegin(), queue.cend());
  ASSERT_NE(max_element_it, queue.cend());
  EXPECT_EQ(*max_element_it, 5);

  // Content = {3, 4, 5}, Storage = [5, x, 3, 4]
  queue.pop();

  max_element_it = std::max_element(queue.cbegin(), queue.cend());
  ASSERT_NE(max_element_it, queue.cend());
  EXPECT_EQ(*max_element_it, 5);

  // Content = {}, Storage = [x, x, x, x]
  queue.clear();

  max_element_it = std::max_element(queue.cbegin(), queue.cend());
  ASSERT_EQ(max_element_it, queue.cend());
}

TEST(InlineQueue, OperatorPlus) {
  // Content = {0, 0, 1, 2}, Storage = [0, 0, 1, 2]
  InlineQueue<int, 4> queue = {0, 0, 1, 2};
  // Content = {0, 1, 2, 3}, Storage = [3, 0, 1, 2]
  queue.push_overwrite(3);
  // Content = {1, 2, 3, 4}, Storage = [3, 4, 1, 2]
  queue.push_overwrite(4);

  for (int i = 0; i < 4; i++) {
    ASSERT_EQ(*(queue.begin() + i), static_cast<int>(i + 1));
    ASSERT_EQ(*(i + queue.begin()), static_cast<int>(i + 1));
  }

  ASSERT_EQ(queue.begin() + queue.size(), queue.end());
}

TEST(InlineQueue, OperatorPlusPlus) {
  // Content = {0, 0, 1, 2}, Storage = [0, 0, 1, 2]
  InlineQueue<int, 4> queue = {0, 0, 1, 2};
  // Content = {0, 1, 2, 3}, Storage = [3, 0, 1, 2]
  queue.push_overwrite(3);
  // Content = {1, 2, 3, 4}, Storage = [3, 4, 1, 2]
  queue.push_overwrite(4);

  auto it = queue.begin();

  ASSERT_EQ(*it, 1);
  it++;
  ASSERT_EQ(*it, 2);
  it++;
  ASSERT_EQ(*it, 3);
  it++;
  ASSERT_EQ(*it, 4);
  it++;

  ASSERT_EQ(it, queue.end());
}

TEST(InlineQueue, OperatorPlusEquals) {
  // Content = {0, 0, 1, 2}, Storage = [0, 0, 1, 2]
  InlineQueue<int, 4> queue = {0, 0, 1, 2};
  // Content = {0, 1, 2, 3}, Storage = [3, 0, 1, 2]
  queue.push_overwrite(3);
  // Content = {1, 2, 3, 4}, Storage = [3, 4, 1, 2]
  queue.push_overwrite(4);

  auto it = queue.begin();

  ASSERT_EQ(*it, 1);
  it += 1;
  ASSERT_EQ(*it, 2);
  it += 1;
  ASSERT_EQ(*it, 3);
  it += 1;
  ASSERT_EQ(*it, 4);
  it += 1;
  ASSERT_EQ(it, queue.end());

  it = queue.begin();
  ASSERT_EQ(*it, 1);
  it += 2;
  ASSERT_EQ(*it, 3);
  it += 2;
  ASSERT_EQ(it, queue.end());

  it = queue.begin();
  it += queue.size();

  ASSERT_EQ(it, queue.end());
}

TEST(InlineQueue, OpeartorMinus) {
  // Content = {0, 0, 1, 2}, Storage = [0, 0, 1, 2]
  InlineQueue<int, 4> queue = {0, 0, 1, 2};
  // Content = {0, 1, 2, 3}, Storage = [3, 0, 1, 2]
  queue.push_overwrite(3);
  // Content = {1, 2, 3, 4}, Storage = [3, 4, 1, 2]
  queue.push_overwrite(4);

  for (int i = 1; i <= 4; i++) {
    ASSERT_EQ(*(queue.end() - i), static_cast<int>(5 - i));
  }

  ASSERT_EQ(queue.end() - queue.size(), queue.begin());
}
TEST(InlineQueue, OperatorMinusMinus) {
  // Content = {0, 0, 1, 2}, Storage = [0, 0, 1, 2]
  InlineQueue<int, 4> queue = {0, 0, 1, 2};
  // Content = {0, 1, 2, 3}, Storage = [3, 0, 1, 2]
  queue.push_overwrite(3);
  // Content = {1, 2, 3, 4}, Storage = [3, 4, 1, 2]
  queue.push_overwrite(4);

  auto it = queue.end();

  it--;
  ASSERT_EQ(*it, 4);
  it--;
  ASSERT_EQ(*it, 3);
  it--;
  ASSERT_EQ(*it, 2);
  it--;
  ASSERT_EQ(*it, 1);

  ASSERT_EQ(it, queue.begin());
}

TEST(InlineQueue, OperatorMinusEquals) {
  // Content = {0, 0, 1, 2}, Storage = [0, 0, 1, 2]
  InlineQueue<int, 4> queue = {0, 0, 1, 2};
  // Content = {0, 1, 2, 3}, Storage = [3, 0, 1, 2]
  queue.push_overwrite(3);
  // Content = {1, 2, 3, 4}, Storage = [3, 4, 1, 2]
  queue.push_overwrite(4);

  auto it = queue.end();

  it -= 1;
  ASSERT_EQ(*it, 4);
  it -= 1;
  ASSERT_EQ(*it, 3);
  it -= 1;
  ASSERT_EQ(*it, 2);
  it -= 1;
  ASSERT_EQ(*it, 1);

  ASSERT_EQ(it, queue.begin());

  it = queue.end();

  it -= 2;
  ASSERT_EQ(*it, 3);
  it -= 2;
  ASSERT_EQ(*it, 1);

  ASSERT_EQ(it, queue.begin());

  it = queue.end();
  it -= queue.size();

  ASSERT_EQ(it, queue.begin());
}

TEST(InlineQueue, OperatorSquareBracket) {
  // Content = {0, 0, 1, 2}, Storage = [0, 0, 1, 2]
  InlineQueue<int, 4> queue = {0, 0, 1, 2};
  // Content = {0, 1, 2, 3}, Storage = [3, 0, 1, 2]
  queue.push_overwrite(3);
  // Content = {1, 2, 3, 4}, Storage = [3, 4, 1, 2]
  queue.push_overwrite(4);

  for (unsigned short i = 0; i < queue.size(); i++) {
    ASSERT_EQ(queue.begin()[i], static_cast<int>(i + 1));
  }
}

TEST(InlineQueue, OperatorLessThan) {
  // Content = {0, 0, 1, 2}, Storage = [0, 0, 1, 2]
  InlineQueue<int, 4> queue = {0, 0, 1, 2};
  // Content = {0, 1, 2, 3}, Storage = [3, 0, 1, 2]
  queue.push_overwrite(3);
  // Content = {1, 2, 3, 4}, Storage = [3, 4, 1, 2]
  queue.push_overwrite(4);

  for (int i = 0; i < queue.size(); i++) {
    for (int j = 0; j < i; j++) {
      ASSERT_TRUE((queue.begin() + j) < (queue.begin() + i));
    }

    ASSERT_TRUE((queue.begin() + i) < queue.end());
  }
}

TEST(InlineQueue, OperatorLessThanEqual) {
  // Content = {0, 0, 1, 2}, Storage = [0, 0, 1, 2]
  InlineQueue<int, 4> queue = {0, 0, 1, 2};
  // Content = {0, 1, 2, 3}, Storage = [3, 0, 1, 2]
  queue.push_overwrite(3);
  // Content = {1, 2, 3, 4}, Storage = [3, 4, 1, 2]
  queue.push_overwrite(4);

  for (int i = 0; i < queue.size(); i++) {
    for (int j = 0; j <= i; j++) {
      ASSERT_TRUE((queue.begin() + j) <= (queue.begin() + i));
    }

    ASSERT_TRUE((queue.begin() + i) <= queue.end());
  }
}

TEST(InlineQueue, OperatorGreater) {
  // Content = {0, 0, 1, 2}, Storage = [0, 0, 1, 2]
  InlineQueue<int, 4> queue = {0, 0, 1, 2};
  // Content = {0, 1, 2, 3}, Storage = [3, 0, 1, 2]
  queue.push_overwrite(3);
  // Content = {1, 2, 3, 4}, Storage = [3, 4, 1, 2]
  queue.push_overwrite(4);

  for (int i = 0; i < queue.size(); i++) {
    for (int j = i + 1; j < queue.size(); j++) {
      ASSERT_TRUE((queue.begin() + j) > (queue.begin() + i));
    }

    ASSERT_TRUE(queue.end() > (queue.begin() + i));
  }
}

TEST(InlineQueue, OperatorGreaterThanEqual) {
  // Content = {0, 0, 1, 2}, Storage = [0, 0, 1, 2]
  InlineQueue<int, 4> queue = {0, 0, 1, 2};
  // Content = {0, 1, 2, 3}, Storage = [3, 0, 1, 2]
  queue.push_overwrite(3);
  // Content = {1, 2, 3, 4}, Storage = [3, 4, 1, 2]
  queue.push_overwrite(4);

  for (int i = 0; i < queue.size(); i++) {
    for (int j = i; j < queue.size(); j++) {
      ASSERT_TRUE((queue.begin() + j) >= (queue.begin() + i));
    }

    ASSERT_TRUE(queue.end() >= (queue.begin() + i));
  }
}

TEST(InlineQueue, DereferenceOperator) {
  // Content = {0, 0, 1, 2}, Storage = [0, 0, 1, 2]
  InlineQueue<int, 4> queue = {0, 0, 1, 2};
  // Content = {0, 1, 2, 3}, Storage = [3, 0, 1, 2]
  queue.push_overwrite(3);
  // Content = {1, 2, 3, 4}, Storage = [3, 4, 1, 2]
  queue.push_overwrite(4);

  for (int i = 0; i < queue.size(); i++) {
    const auto it = queue.begin() + i;
    ASSERT_EQ(*(it.operator->()), static_cast<int>(i + 1));
  }
}

// Test that InlineQueue<T> is trivially destructible when its type is.
static_assert(std::is_trivially_destructible_v<InlineQueue<int, 1>>);

static_assert(std::is_trivially_destructible_v<CopyOnly>);
static_assert(std::is_trivially_destructible_v<InlineQueue<CopyOnly, 99>>);

static_assert(!std::is_trivially_destructible_v<Counter>);
static_assert(!std::is_trivially_destructible_v<InlineQueue<Counter, 99>>);

// Generic-capacity queues cannot be constructed or destructed.
static_assert(!std::is_constructible_v<InlineQueue<int>>);
static_assert(!std::is_destructible_v<InlineQueue<int>>);

// Tests that InlineQueue<T> does not have any extra padding.
static_assert(sizeof(InlineQueue<uint8_t, 1>) ==
              sizeof(InlineQueue<uint8_t>::size_type) * 4 +
                  std::max(sizeof(InlineQueue<uint8_t>::size_type),
                           sizeof(uint8_t)));
static_assert(sizeof(InlineQueue<uint8_t, 2>) ==
              sizeof(InlineQueue<uint8_t>::size_type) * 4 +
                  2 * sizeof(uint8_t));
static_assert(sizeof(InlineQueue<uint16_t, 1>) ==
              sizeof(InlineQueue<uint16_t>::size_type) * 4 + sizeof(uint16_t));
static_assert(sizeof(InlineQueue<uint32_t, 1>) ==
              sizeof(InlineQueue<uint32_t>::size_type) * 4 + sizeof(uint32_t));
static_assert(sizeof(InlineQueue<uint64_t, 1>) ==
              sizeof(InlineQueue<uint64_t>::size_type) * 4 + sizeof(uint64_t));

// Test that InlineQueue<T> is copy assignable
static_assert(std::is_copy_assignable_v<InlineQueue<int>::iterator>);
static_assert(std::is_copy_assignable_v<InlineQueue<int, 4>::iterator>);

// Test that InlineQueue<T>::iterator can be converted to a const_iterator
static_assert(std::is_convertible<InlineQueue<int>::iterator,
                                  InlineQueue<int>::const_iterator>::value);
static_assert(std::is_convertible<InlineQueue<int, 4>::iterator,
                                  InlineQueue<int, 4>::const_iterator>::value);

// Test that InlineQueue<T>::const_iterator can NOT be converted to a iterator
static_assert(!std::is_convertible<InlineQueue<int>::const_iterator,
                                   InlineQueue<int>::iterator>::value);
static_assert(!std::is_convertible<InlineQueue<int, 4>::const_iterator,
                                   InlineQueue<int, 4>::iterator>::value);

}  // namespace
}  // namespace pw::containers
