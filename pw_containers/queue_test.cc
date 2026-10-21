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

#include "pw_containers/queue.h"

#include <array>
#include <cstddef>
#include <type_traits>
#include <utility>

#include "pw_allocator/testing.h"
#include "pw_containers/internal/test_helpers.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::containers::test::Counter;
using pw::containers::test::MoveOnly;

//
// Tests for pw::Queue
//
TEST(Queue, Construct_Buffer) {
  std::array<std::byte, 32> buffer;
  pw::Queue<int> queue(buffer);
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(queue.size(), 0u);
  EXPECT_LE(queue.capacity(), buffer.size() / sizeof(int));
  EXPECT_EQ(queue.max_size(), queue.capacity());
}

TEST(Queue, Construct_Storage) {
  pw::containers::StorageFor<Counter, 10> storage;
  pw::Queue<Counter> queue(storage);
  EXPECT_TRUE(queue.empty());
  EXPECT_LE(queue.capacity(), 10u);
  EXPECT_EQ(queue.max_size(), queue.capacity());
}

TEST(Queue, PushPop) {
  pw::containers::StorageFor<int, 3> buffer;
  pw::Queue<int> queue(buffer);

  queue.push(1);
  EXPECT_EQ(queue.front(), 1);
  EXPECT_EQ(queue.back(), 1);

  queue.push(2);
  EXPECT_EQ(queue.front(), 1);
  EXPECT_EQ(queue.back(), 2);

  queue.push(3);
  EXPECT_EQ(queue.front(), 1);
  EXPECT_EQ(queue.back(), 3);

  EXPECT_EQ(queue.size(), 3u);
  EXPECT_TRUE(queue.full());

  queue.pop();
  EXPECT_EQ(queue.front(), 2);
  EXPECT_EQ(queue.back(), 3);
  EXPECT_EQ(queue.size(), 2u);

  queue.pop();
  EXPECT_EQ(queue.front(), 3);
  EXPECT_EQ(queue.back(), 3);
  EXPECT_EQ(queue.size(), 1u);

  queue.pop();
  EXPECT_TRUE(queue.empty());
}

TEST(Queue, Emplace) {
  pw::containers::StorageFor<MoveOnly, 2> buffer;
  pw::Queue<MoveOnly> queue(buffer);

  queue.emplace(1);
  EXPECT_EQ(queue.back().value, 1);

  queue.emplace(2);
  EXPECT_EQ(queue.back().value, 2);

  EXPECT_EQ(queue.size(), 2u);
}

TEST(Queue, Overwrite) {
  pw::containers::StorageFor<int, 2> storage;
  pw::Queue<int> queue(storage);

  queue.push(1);
  queue.push(2);
  EXPECT_TRUE(queue.full());

  queue.push_overwrite(3);
  EXPECT_EQ(queue.size(), 2u);
  EXPECT_EQ(queue.front(), 2);
  EXPECT_EQ(queue.back(), 3);

  queue.emplace_overwrite(4);
  EXPECT_EQ(queue.size(), 2u);
  EXPECT_EQ(queue.front(), 3);
  EXPECT_EQ(queue.back(), 4);
}

TEST(FixedQueueStatic, Construct) {
  pw::FixedQueue<int, 10> queue;
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(queue.size(), 0u);
  EXPECT_EQ(queue.capacity(), 10u);
  EXPECT_EQ(queue.max_size(), 10u);
}

TEST(FixedQueueStatic, PushPop) {
  pw::FixedQueue<int, 3> queue;

  queue.push(1);
  queue.push(2);
  queue.push(3);

  EXPECT_EQ(queue.size(), 3u);
  EXPECT_TRUE(queue.full());
  EXPECT_EQ(queue.front(), 1);
  EXPECT_EQ(queue.back(), 3);

  queue.pop();
  EXPECT_EQ(queue.size(), 2u);
  EXPECT_EQ(queue.front(), 2);

  queue.pop();
  EXPECT_EQ(queue.size(), 1u);
  EXPECT_EQ(queue.front(), 3);

  queue.pop();
  EXPECT_TRUE(queue.empty());
}

TEST(FixedQueueStatic, MoveConstruct) {
  pw::FixedQueue<Counter, 5> original;
  original.emplace(1);
  original.emplace(2);

  pw::FixedQueue<Counter, 5> moved(std::move(original));
  EXPECT_TRUE(original.empty());  // NOLINT(bugprone-use-after-move)

  ASSERT_EQ(moved.size(), 2u);
  EXPECT_EQ(moved.front().value, 1);
  moved.pop();
  EXPECT_EQ(moved.front().value, 2);
}

TEST(FixedQueueStatic, MoveAssign) {
  pw::FixedQueue<Counter, 5> original;
  original.emplace(1);
  original.emplace(2);

  pw::FixedQueue<Counter, 5> moved;
  moved.emplace(3);
  moved = std::move(original);

  EXPECT_TRUE(original.empty());  // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(moved.size(), 2u);
  EXPECT_EQ(moved.front().value, 1);
}

TEST(FixedQueueStatic, Swap) {
  pw::FixedQueue<int, 5> queue1;
  queue1.push(1);
  queue1.push(2);

  pw::FixedQueue<int, 5> queue2;
  queue2.push(3);

  queue1.swap(queue2);

  EXPECT_EQ(queue1.size(), 1u);
  EXPECT_EQ(queue1.front(), 3);
  EXPECT_EQ(queue2.size(), 2u);
  EXPECT_EQ(queue2.front(), 1);
}

class FixedQueueDynamic : public ::testing::Test {
 protected:
  pw::allocator::test::AllocatorForTest<256> allocator_;
};

TEST_F(FixedQueueDynamic, Construct) {
  auto queue = pw::FixedQueue<int>::Allocate(allocator_, 10);
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(queue.size(), 0u);
  EXPECT_EQ(queue.capacity(), 10u);
  EXPECT_EQ(queue.max_size(), 10u);
}

TEST_F(FixedQueueDynamic, PushPop) {
  auto queue = pw::FixedQueue<int>::Allocate(allocator_, 3);

  queue.push(1);
  queue.push(2);
  queue.push(3);

  EXPECT_EQ(queue.size(), 3u);
  EXPECT_TRUE(queue.full());
  EXPECT_EQ(queue.front(), 1);
  EXPECT_EQ(queue.back(), 3);

  queue.pop();
  EXPECT_EQ(queue.size(), 2u);
  EXPECT_EQ(queue.front(), 2);

  queue.pop();
  EXPECT_EQ(queue.size(), 1u);
  EXPECT_EQ(queue.front(), 3);

  queue.pop();
  EXPECT_TRUE(queue.empty());
}

TEST_F(FixedQueueDynamic, MoveConstruct) {
  auto original = pw::FixedQueue<Counter>::Allocate(allocator_, 5);
  original.emplace(1);
  original.emplace(2);

  pw::FixedQueue<Counter> moved(std::move(original));
  EXPECT_EQ(original.size(), 0u);      // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(original.capacity(), 0u);  // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(moved.size(), 2u);
  EXPECT_EQ(moved.front().value, 1);
}

TEST_F(FixedQueueDynamic, MoveAssign) {
  auto original = pw::FixedQueue<Counter>::Allocate(allocator_, 5);
  original.emplace(1);
  original.emplace(2);

  pw::allocator::test::AllocatorForTest<128> other_alloc;
  auto moved = pw::FixedQueue<Counter>::Allocate(other_alloc, 1);
  moved.emplace(3);
  moved = std::move(original);

  EXPECT_EQ(original.size(), 0u);      // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(original.capacity(), 0u);  // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(moved.size(), 2u);
  EXPECT_EQ(moved.front().value, 1);
}

TEST_F(FixedQueueDynamic, Swap) {
  pw::allocator::test::AllocatorForTest<128> other_alloc;
  auto queue1 = pw::FixedQueue<int>::Allocate(allocator_, 5);
  queue1.push(1);
  queue1.push(2);

  auto queue2 = pw::FixedQueue<int>::Allocate(other_alloc, 5);
  queue2.push(3);

  queue1.swap(queue2);

  EXPECT_EQ(queue1.size(), 1u);
  EXPECT_EQ(queue1.front(), 3);
  EXPECT_EQ(queue2.size(), 2u);
  EXPECT_EQ(queue2.front(), 1);
}

TEST_F(FixedQueueDynamic, SwapWithStatic) {
  auto dynamic_queue = pw::FixedQueue<int>::Allocate(allocator_, 5);
  dynamic_queue.push(1);
  dynamic_queue.push(2);

  pw::FixedQueue<int, 5> static_queue;
  static_queue.push(3);

  dynamic_queue.swap(static_queue);

  EXPECT_EQ(dynamic_queue.size(), 1u);
  EXPECT_EQ(dynamic_queue.front(), 3);
  EXPECT_EQ(static_queue.size(), 2u);
  EXPECT_EQ(static_queue.front(), 1);
}

TEST(FixedQueue, UseAsQueue) {
  pw::FixedQueue<Counter, 4> queue_1;

  ::pw::allocator::test::AllocatorForTest<256> allocator;
  auto queue_2 = pw::FixedQueue<Counter>::Allocate(allocator, 5);

  pw::Queue<Counter>& queue_a = queue_1;
  queue_a.push(123);

  pw::Queue<Counter>* queue = &queue_a;
  queue->push(999);

  pw::Queue<Counter>& queue_b = queue_2;
  queue_b.push(321);

  queue = &queue_b;
  queue->pop();

  EXPECT_EQ(queue_a.size(), 2u);
  EXPECT_EQ(queue_a.front(), 123);
  queue_a.pop();
  EXPECT_EQ(queue_a.front(), 999);

  EXPECT_TRUE(queue_b.empty());
  EXPECT_EQ(queue, &queue_b);
}

}  // namespace
