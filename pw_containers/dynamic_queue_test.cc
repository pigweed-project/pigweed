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

#include "pw_containers/dynamic_queue.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

#include "pw_allocator/fault_injecting_allocator.h"
#include "pw_allocator/testing.h"
#include "pw_compilation_testing/negative_compilation.h"
#include "pw_containers/internal/test_helpers.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::containers::test::Counter;
using pw::containers::test::MoveOnly;

class DynamicQueueTest : public ::testing::Test {
 protected:
  DynamicQueueTest() : allocator_(allocator_for_test_) {}

  pw::allocator::test::AllocatorForTest<1024> allocator_for_test_;
  pw::allocator::test::FaultInjectingAllocator allocator_;
};

TEST_F(DynamicQueueTest, BasicOperations) {
  pw::DynamicQueue<int> queue(allocator_);

  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(queue.size(), 0u);
  EXPECT_GE(queue.capacity(), 0u);
  EXPECT_EQ(queue.max_size(), std::numeric_limits<uint16_t>::max());

  queue.push(10);
  EXPECT_FALSE(queue.empty());
  EXPECT_EQ(queue.size(), 1u);
  EXPECT_GE(queue.capacity(), 1u);
  EXPECT_EQ(queue.front(), 10);
  EXPECT_EQ(queue.back(), 10);

  queue.push(20);
  EXPECT_FALSE(queue.empty());
  EXPECT_EQ(queue.size(), 2u);
  EXPECT_GE(queue.capacity(), 2u);
  EXPECT_EQ(queue.front(), 10);
  EXPECT_EQ(queue.back(), 20);

  queue.pop();
  EXPECT_FALSE(queue.empty());
  EXPECT_EQ(queue.size(), 1u);
  EXPECT_EQ(queue.front(), 20);
  EXPECT_EQ(queue.back(), 20);

  queue.pop();
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(queue.size(), 0u);
}

TEST_F(DynamicQueueTest, PushMove) {
  pw::DynamicQueue<MoveOnly> queue(allocator_);

  MoveOnly move_only(54321);
  queue.push(std::move(move_only));
  EXPECT_EQ(queue.size(), 1u);
  EXPECT_EQ(queue.front().value, 54321);

  queue.pop();
  EXPECT_TRUE(queue.empty());
}

TEST_F(DynamicQueueTest, Emplace) {
  pw::DynamicQueue<std::pair<int, float>> queue(allocator_);

  queue.emplace(1, 1.5f);
  EXPECT_EQ(queue.size(), 1u);
  EXPECT_EQ(queue.front().first, 1);
  EXPECT_FLOAT_EQ(queue.front().second, 1.5f);

  queue.pop();
  EXPECT_TRUE(queue.empty());
}

TEST_F(DynamicQueueTest, TryPushSuccess) {
  pw::DynamicQueue<int> queue(allocator_);

  EXPECT_TRUE(queue.try_push(10));
  EXPECT_EQ(queue.size(), 1u);
  EXPECT_EQ(queue.front(), 10);
}

TEST_F(DynamicQueueTest, TryPushFailure) {
  allocator_.DisableAll();

  pw::DynamicQueue<int> queue(allocator_);

  EXPECT_FALSE(queue.try_push(10));
  EXPECT_TRUE(queue.empty());
}

TEST_F(DynamicQueueTest, TryEmplaceSuccess) {
  pw::DynamicQueue<std::pair<int, float>> queue(allocator_);

  EXPECT_TRUE(queue.try_emplace(1, 1.1f));
  EXPECT_EQ(queue.size(), 1u);
  EXPECT_EQ(queue.front().first, 1);
}

TEST_F(DynamicQueueTest, TryEmplaceFailure) {
  allocator_.DisableAll();

  pw::DynamicQueue<std::pair<int, float>> queue(allocator_);

  EXPECT_FALSE(queue.try_emplace(1, 1.1f));
  EXPECT_TRUE(queue.empty());
}

TEST_F(DynamicQueueTest, ReserveExact) {
  pw::DynamicQueue<int> queue(allocator_);

  ASSERT_EQ(queue.capacity(), 0u);
  queue.reserve_exact(7);
  EXPECT_GE(queue.capacity(), 7u);
  EXPECT_TRUE(queue.empty());
}

TEST_F(DynamicQueueTest, TryReserveExactSuccess) {
  pw::DynamicQueue<int> queue(allocator_);

  EXPECT_TRUE(queue.try_reserve_exact(5));
  EXPECT_GE(queue.capacity(), 5u);
  EXPECT_TRUE(queue.empty());
}

TEST_F(DynamicQueueTest, TryReserveExactFailure) {
  allocator_.DisableAll();

  pw::DynamicQueue<int> queue(allocator_);

  ASSERT_EQ(queue.capacity(), 0u);
  EXPECT_FALSE(queue.try_reserve_exact(5));
  EXPECT_EQ(queue.capacity(), 0u);
}

TEST_F(DynamicQueueTest, Reserve_IncreasesByMoreThanOne) {
  pw::DynamicQueue<int> queue(allocator_);

  ASSERT_TRUE(queue.try_reserve_exact(50));

  queue.reserve(51);

  EXPECT_GT(queue.capacity(), 51);

  const auto original_capacity = queue.capacity();
  queue.reserve(52);
  EXPECT_EQ(original_capacity, queue.capacity());
}

TEST_F(DynamicQueueTest, ShrinkToFit) {
  pw::DynamicQueue<int> queue(allocator_);

  for (int i = 0; i < 10; ++i) {
    queue.push(i);
  }
  EXPECT_EQ(queue.size(), 10u);
  ASSERT_GE(queue.capacity(), 10u);
  const auto original_capacity = queue.capacity();

  for (int i = 0; i < 5; ++i) {
    queue.pop();
  }
  ASSERT_EQ(queue.size(), 5u);

  allocator_.DisableAll();
  queue.shrink_to_fit();
  EXPECT_EQ(queue.capacity(), original_capacity) << "shrink_to_fit may fail";

  allocator_.EnableAll();
  queue.shrink_to_fit();
  EXPECT_EQ(queue.capacity(), 5u);

  queue.clear();

  EXPECT_EQ(queue.size(), 0u);
  EXPECT_GE(queue.capacity(), 5u);

  queue.shrink_to_fit();

  EXPECT_GE(queue.capacity(), 0u);
}

TEST_F(DynamicQueueTest, Reset) {
  pw::DynamicQueue<Counter> queue(allocator_);
  queue.push(1);
  queue.push(2);
  EXPECT_FALSE(queue.empty());
  Counter::Reset();

  queue.reset();

  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(queue.size(), 0u);
  EXPECT_EQ(queue.capacity(), 0u);
  EXPECT_EQ(Counter::destroyed, 2);
}

TEST_F(DynamicQueueTest, MoveAssign_DestroysOldElementsAndFreesBuffer) {
  pw::DynamicQueue<Counter> queue1(allocator_);
  pw::DynamicQueue<Counter> queue2(allocator_);

  queue1.push(1);
  queue1.push(2);

  queue2.push(3);
  queue2.push(4);
  queue2.push(5);

  ASSERT_EQ(queue1.size(), 2u);
  ASSERT_EQ(queue2.size(), 3u);

  Counter::Reset();
  const size_t initial_bytes = allocator_for_test_.GetAllocated();

  queue1 = std::move(queue2);

  EXPECT_EQ(queue1.size(), 3u);
  EXPECT_EQ(Counter::destroyed, 2);
  EXPECT_LT(allocator_for_test_.GetAllocated(), initial_bytes);
}

TEST_F(DynamicQueueTest, Capacity) {
  pw::DynamicQueue<int> queue(allocator_);

  EXPECT_GE(queue.capacity(), 0u);

  queue.push(10);
  EXPECT_GE(queue.capacity(), 1u);

  queue.reserve_exact(20);
  EXPECT_GE(queue.capacity(), 20u);
}

TEST_F(DynamicQueueTest, Swap) {
  pw::allocator::test::AllocatorForTest<128> other_alloc_;

  pw::DynamicQueue<Counter> queue_1(allocator_);
  pw::DynamicQueue<Counter> queue_2(other_alloc_);

  queue_1.push(1);
  queue_1.push(2);
  queue_2.push(-1);
  ASSERT_EQ(queue_1.size(), 2);
  ASSERT_EQ(queue_2.size(), 1);

  queue_1.swap(queue_2);

  EXPECT_EQ(queue_1.size(), 1);
  EXPECT_EQ(queue_1.front(), -1);

  EXPECT_EQ(queue_2.size(), 2);
  EXPECT_EQ(queue_2.front(), 1);
  EXPECT_EQ(queue_2.back(), 2);
}

static_assert(
    std::is_same_v<pw::DynamicQueue<int, uint8_t>::size_type, uint8_t>);
static_assert(
    std::is_same_v<pw::DynamicQueue<int, uint16_t>::size_type, uint16_t>);
static_assert(
    std::is_same_v<pw::DynamicQueue<int, uint32_t>::size_type, uint32_t>);

#if PW_NC_TEST(DynamicQueue_EmplaceOverwriteHidden)
PW_NC_EXPECT("(is inaccessible|is (a )?private member of)");
void Test(pw::DynamicQueue<int>& queue) { queue.emplace_overwrite(1); }
#elif PW_NC_TEST(DynamicQueue_PushOverwriteHidden)
PW_NC_EXPECT("(is inaccessible|is (a )?private member of)");
void Test(pw::DynamicQueue<int>& queue) { queue.push_overwrite(1); }
#endif

}  // namespace
