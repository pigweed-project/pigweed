// Copyright 2026 The Pigweed Authors
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

#include "pw_containers/lock_free_spsc_queue.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

#include "pw_allocator/null_allocator.h"
#include "pw_allocator/testing.h"
#include "pw_assert/assert.h"
#include "pw_compilation_testing/negative_compilation.h"
#include "pw_containers/internal/test_helpers.h"
#include "pw_span/span.h"
#include "pw_unit_test/framework.h"

#if __has_include("pw_thread/test_thread_context.h") &&  \
                  __has_include("pw_thread/thread.h") && \
                                __has_include("pw_thread/yield.h")
#include "pw_thread/test_thread_context.h"
#include "pw_thread/thread.h"
#include "pw_thread/yield.h"
#endif  // __has_include("pw_thread/test_thread_context.h") &&
        // __has_include("pw_thread/thread.h") &&
        // __has_include("pw_thread/yield.h")

namespace pw {
namespace {

using ::pw::allocator::GetNullAllocator;
using ::pw::allocator::test::AllocatorForTest;
using ::pw::containers::test::CopyOnly;
using ::pw::containers::test::Counter;
using ::pw::containers::test::MoveOnly;

static_assert(!std::is_copy_constructible_v<LockFreeSpscQueue<int>>,
              "LockFreeSpscQueue should not be copy constructible");
static_assert(!std::is_copy_assignable_v<LockFreeSpscQueue<int>>,
              "LockFreeSpscQueue should not be copy assignable");
static_assert(!std::is_move_constructible_v<LockFreeSpscQueue<int>>,
              "LockFreeSpscQueue should not be move constructible");
static_assert(!std::is_move_assignable_v<LockFreeSpscQueue<int>>,
              "LockFreeSpscQueue should not be move assignable");

static_assert(!std::is_copy_constructible_v<FixedLockFreeSpscQueue<int, 4>>,
              "FixedLockFreeSpscQueue should not be copy constructible");
static_assert(!std::is_copy_assignable_v<FixedLockFreeSpscQueue<int, 4>>,
              "FixedLockFreeSpscQueue should not be copy assignable");
static_assert(std::is_move_constructible_v<FixedLockFreeSpscQueue<int, 4>>,
              "FixedLockFreeSpscQueue should be move constructible");
static_assert(std::is_move_assignable_v<FixedLockFreeSpscQueue<int, 4>>,
              "FixedLockFreeSpscQueue should be move assignable");

static_assert(!std::is_copy_constructible_v<FixedLockFreeSpscQueue<int>>,
              "FixedLockFreeSpscQueue should not be copy constructible");
static_assert(!std::is_copy_assignable_v<FixedLockFreeSpscQueue<int>>,
              "FixedLockFreeSpscQueue should not be copy assignable");
static_assert(std::is_move_constructible_v<FixedLockFreeSpscQueue<int>>,
              "FixedLockFreeSpscQueue should be move constructible");
static_assert(std::is_move_assignable_v<FixedLockFreeSpscQueue<int>>,
              "FixedLockFreeSpscQueue should be move assignable");
static_assert(noexcept(std::declval<LockFreeSpscQueue<int>&>().clear()),
              "clear() should be noexcept");

TEST(LockFreeSpscQueueTest, Construct_Buffer) {
  std::array<std::byte, 32> buffer;
  LockFreeSpscQueue<int> queue(buffer);
  EXPECT_TRUE(queue.empty());
  EXPECT_FALSE(queue.full());
  EXPECT_EQ(queue.size(), 0u);
  EXPECT_LE(queue.capacity(), buffer.size() / sizeof(int));
  EXPECT_EQ(queue.max_size(), queue.capacity());
  EXPECT_EQ(queue.remaining_capacity(), queue.capacity());
}

TEST(LockFreeSpscQueueTest, Construct_UnalignedAndSmallBuffer) {
  alignas(uint64_t) std::array<std::byte, sizeof(uint64_t) * 3> raw_buffer{};
  // Misalign by 1 byte; only 2 aligned uint64_t elements fit in the remaining
  // 23 bytes.
  LockFreeSpscQueue<uint64_t> unaligned_queue(
      span<std::byte>(raw_buffer).subspan(1));
  EXPECT_EQ(unaligned_queue.capacity(), 2u);
  EXPECT_TRUE(unaligned_queue.try_push(10u));
  EXPECT_TRUE(unaligned_queue.try_push(20u));
  EXPECT_FALSE(unaligned_queue.try_push(30u));
  EXPECT_EQ(unaligned_queue.try_pop(), 10u);
  EXPECT_EQ(unaligned_queue.try_pop(), 20u);

  // Buffer too small to hold a single element has capacity 0.
  std::array<std::byte, sizeof(uint64_t) - 1> tiny_buffer{};
  LockFreeSpscQueue<uint64_t> zero_queue(tiny_buffer);
  EXPECT_EQ(zero_queue.capacity(), 0u);
  EXPECT_EQ(zero_queue.max_size(), 0u);
  EXPECT_EQ(zero_queue.size(), 0u);
  EXPECT_EQ(zero_queue.remaining_capacity(), 0u);
  EXPECT_TRUE(zero_queue.empty());
  EXPECT_TRUE(zero_queue.full());
  EXPECT_FALSE(zero_queue.try_push(1u));
  EXPECT_EQ(zero_queue.try_peek(), nullptr);
  EXPECT_EQ(zero_queue.try_pop(), std::nullopt);
}

TEST(LockFreeSpscQueueTest, Construct_Storage) {
  containers::StorageFor<Counter, 10> storage;
  LockFreeSpscQueue<Counter> queue(storage);
  EXPECT_TRUE(queue.empty());
  EXPECT_FALSE(queue.full());
  EXPECT_EQ(queue.capacity(), 10u);
  EXPECT_EQ(queue.max_size(), 10u);
  EXPECT_EQ(queue.remaining_capacity(), 10u);
}

TEST(LockFreeSpscQueueTest, FixedLockFreeSpscQueue_StaticCapacity) {
  FixedLockFreeSpscQueue<int, 4> queue;
  EXPECT_EQ(queue.capacity(), 4u);
  EXPECT_EQ(queue.max_size(), 4u);
  EXPECT_EQ(queue.size(), 0u);
  EXPECT_EQ(queue.remaining_capacity(), 4u);
  EXPECT_TRUE(queue.empty());
  EXPECT_FALSE(queue.full());
  EXPECT_EQ(queue.deallocator(), nullptr);
}

TEST(LockFreeSpscQueueTest, FixedLockFreeSpscQueue_StaticMove) {
  Counter::Reset();
  {
    FixedLockFreeSpscQueue<Counter, 4> original;
    original.emplace(10);
    original.emplace(20);

    FixedLockFreeSpscQueue<Counter, 4> moved_constructed(std::move(original));
    EXPECT_TRUE(original.empty());  // NOLINT(bugprone-use-after-move)
    EXPECT_EQ(moved_constructed.size(), 2u);
    EXPECT_EQ(moved_constructed.front().value, 10);

    FixedLockFreeSpscQueue<Counter, 6> larger;
    larger.emplace(99);
    larger = std::move(moved_constructed);
    EXPECT_TRUE(moved_constructed.empty());  // NOLINT(bugprone-use-after-move)
    EXPECT_EQ(larger.size(), 2u);
    EXPECT_EQ(larger.try_pop()->value, 10);
    EXPECT_EQ(larger.try_pop()->value, 20);
    EXPECT_TRUE(larger.empty());
  }
  EXPECT_EQ(Counter::created + Counter::moved, Counter::destroyed);
}

TEST(LockFreeSpscQueueTest, FixedLockFreeSpscQueue_DynamicAllocator) {
  Counter::Reset();
  AllocatorForTest<256> allocator;
  {
    auto queue = FixedLockFreeSpscQueue<Counter>::Allocate(allocator, 4u);
    EXPECT_EQ(queue.capacity(), 4u);
    EXPECT_EQ(queue.max_size(), 4u);
    EXPECT_EQ(queue.deallocator(), &allocator);
    EXPECT_TRUE(queue.empty());

    queue.emplace(100);
    queue.emplace(200);
    queue.emplace(300);
    EXPECT_EQ(Counter::created, 3);
    EXPECT_EQ(Counter::destroyed, 0);

    auto first = queue.try_pop();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->value, 100);
    first.reset();
    EXPECT_EQ(Counter::created + Counter::moved - Counter::destroyed, 2);

    auto second = queue.try_pop();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->value, 200);
    second.reset();
    EXPECT_EQ(Counter::created + Counter::moved - Counter::destroyed, 1);

    // Leave the 3rd item unpopped when queue goes out of scope.
  }
  EXPECT_EQ(Counter::created + Counter::moved, Counter::destroyed);
  EXPECT_EQ(allocator.metrics().allocated_bytes.value(), 0u);
}

TEST(LockFreeSpscQueueTest, FixedLockFreeSpscQueue_TryAllocateFailures) {
  AllocatorForTest<256> allocator;

  // Zero capacity returns an empty queue with capacity 0.
  auto zero_queue = FixedLockFreeSpscQueue<int>::TryAllocate(allocator, 0u);
  EXPECT_EQ(zero_queue.capacity(), 0u);
  EXPECT_TRUE(zero_queue.empty());
  EXPECT_TRUE(zero_queue.full());
  EXPECT_FALSE(zero_queue.try_push(1));
  EXPECT_EQ(zero_queue.try_pop(), std::nullopt);
  EXPECT_EQ(allocator.metrics().allocated_bytes.value(), 0u);

  // Null allocator fails and returns capacity 0.
  auto null_queue =
      FixedLockFreeSpscQueue<int>::TryAllocate(GetNullAllocator(), 4u);
  EXPECT_EQ(null_queue.capacity(), 0u);
  EXPECT_FALSE(null_queue.try_push(1));

  // Capacity exceeding kMaxCapacity fails and returns capacity 0.
  auto overflow_queue =
      FixedLockFreeSpscQueue<int, containers::kExternalStorage, uint8_t>::
          TryAllocate(allocator, 200u);
  EXPECT_EQ(overflow_queue.capacity(), 0u);
  EXPECT_EQ(allocator.metrics().allocated_bytes.value(), 0u);
}

TEST(LockFreeSpscQueueTest, FixedLockFreeSpscQueue_WithStorageAndDynamicMove) {
  Counter::Reset();
  AllocatorForTest<256> allocator;
  AllocatorForTest<128> other_allocator;
  {
    UniquePtr<std::byte[]> raw_storage = allocator.MakeUnique<std::byte[]>(
        sizeof(Counter) * 4, alignof(Counter));
    ASSERT_NE(raw_storage, nullptr);

    auto original =
        FixedLockFreeSpscQueue<Counter>::WithStorage(std::move(raw_storage));
    EXPECT_EQ(original.capacity(), 4u);
    original.emplace(1);
    original.emplace(2);

    FixedLockFreeSpscQueue<Counter> moved_constructed(std::move(original));
    EXPECT_EQ(original.size(), 0u);      // NOLINT(bugprone-use-after-move)
    EXPECT_EQ(original.capacity(), 0u);  // NOLINT(bugprone-use-after-move)
    EXPECT_EQ(moved_constructed.size(), 2u);
    EXPECT_EQ(moved_constructed.front().value, 1);

    auto assigned =
        FixedLockFreeSpscQueue<Counter>::Allocate(other_allocator, 2u);
    assigned.emplace(99);
    assigned = std::move(moved_constructed);
    EXPECT_EQ(other_allocator.metrics().allocated_bytes.value(), 0u);
    EXPECT_EQ(assigned.size(), 2u);

    // Move-assigning into a moved-from (null-buffer) queue is valid.
    moved_constructed = std::move(assigned);
    EXPECT_EQ(moved_constructed.size(), 2u);
    EXPECT_EQ(moved_constructed.try_pop()->value, 1);
    EXPECT_EQ(moved_constructed.try_pop()->value, 2);
  }
  EXPECT_EQ(Counter::created + Counter::moved, Counter::destroyed);
  EXPECT_EQ(allocator.metrics().allocated_bytes.value(), 0u);
}

TEST(LockFreeSpscQueueTest, UseAsGenericLockFreeSpscQueue) {
  FixedLockFreeSpscQueue<Counter, 4> static_queue;

  AllocatorForTest<256> allocator;
  auto dynamic_queue = FixedLockFreeSpscQueue<Counter>::Allocate(allocator, 5u);

  LockFreeSpscQueue<Counter>& queue_a = static_queue;
  queue_a.emplace(123);

  LockFreeSpscQueue<Counter>* queue = &queue_a;
  queue->emplace(999);

  LockFreeSpscQueue<Counter>& queue_b = dynamic_queue;
  queue_b.emplace(321);

  queue = &queue_b;
  queue->pop();

  EXPECT_EQ(queue_a.size(), 2u);
  EXPECT_EQ(queue_a.front().value, 123);
  queue_a.pop();
  EXPECT_EQ(queue_a.front().value, 999);
  EXPECT_TRUE(queue_b.empty());
}

TEST(LockFreeSpscQueueTest, ClearDestroysRemainingElements) {
  Counter::Reset();
  FixedLockFreeSpscQueue<Counter, 4> queue;
  queue.emplace(1);
  queue.emplace(2);
  EXPECT_EQ(queue.size(), 2u);
  EXPECT_EQ(Counter::destroyed, 0);

  queue.clear();
  EXPECT_EQ(Counter::destroyed, 2);
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(queue.remaining_capacity(), 4u);

  queue.emplace(10);
  EXPECT_EQ(queue.try_pop()->value, 10);
}

TEST(LockFreeSpscQueueTest, PushAndPop_SingleElementsAndWrapAround) {
  FixedLockFreeSpscQueue<int, 3> queue;

  EXPECT_TRUE(queue.empty());
  EXPECT_FALSE(queue.full());
  EXPECT_EQ(queue.remaining_capacity(), 3u);
  EXPECT_EQ(queue.try_peek(), nullptr);
  EXPECT_EQ(std::as_const(queue).try_peek(), nullptr);

  const int kOne = 1;
  queue.push(kOne);
  queue.push(2);
  EXPECT_TRUE(queue.try_push(3));
  EXPECT_FALSE(queue.try_push(4));

  EXPECT_TRUE(queue.full());
  EXPECT_EQ(queue.size(), 3u);
  EXPECT_EQ(queue.remaining_capacity(), 0u);

  ASSERT_NE(queue.try_peek(), nullptr);
  EXPECT_EQ(*queue.try_peek(), 1);
  EXPECT_EQ(*std::as_const(queue).try_peek(), 1);
  EXPECT_EQ(queue.front(), 1);
  EXPECT_EQ(std::as_const(queue).front(), 1);
  queue.pop();
  EXPECT_EQ(queue.size(), 2u);
  EXPECT_EQ(queue.remaining_capacity(), 1u);

  // Wrap around buffer and virtual indices over many cycles.
  for (int i = 4; i <= 50; ++i) {
    ASSERT_TRUE(queue.try_emplace(i));
    EXPECT_TRUE(queue.full());
    auto popped = queue.try_pop();
    ASSERT_TRUE(popped.has_value());
    EXPECT_EQ(*popped, i - 2);
    EXPECT_EQ(queue.size(), 2u);
  }

  int out = 0;
  EXPECT_TRUE(queue.try_pop(out));
  EXPECT_EQ(out, 49);
  EXPECT_EQ(queue.try_pop(), 50);
  EXPECT_FALSE(queue.try_pop(out));
  EXPECT_EQ(queue.try_pop(), std::nullopt);
  EXPECT_TRUE(queue.empty());
}

TEST(LockFreeSpscQueueTest, CustomSizeType_Uint16WrapAround) {
  FixedLockFreeSpscQueue<int, 5, uint16_t> queue;

  for (int cycle = 0; cycle < 300; ++cycle) {
    ASSERT_TRUE(queue.try_push(cycle * 2));
    ASSERT_TRUE(queue.try_push(cycle * 2 + 1));
    EXPECT_EQ(queue.try_pop(), cycle * 2);
    EXPECT_EQ(queue.try_pop(), cycle * 2 + 1);
    EXPECT_TRUE(queue.empty());
  }
}

TEST(LockFreeSpscQueueTest, MoveOnlyElements) {
  FixedLockFreeSpscQueue<MoveOnly, 4> queue;

  queue.push(MoveOnly(1));
  EXPECT_TRUE(queue.try_push(MoveOnly(2)));
  queue.emplace(3);
  EXPECT_TRUE(queue.try_emplace(4));

  MoveOnly unmoved(5);
  EXPECT_FALSE(queue.try_push(std::move(unmoved)));
  EXPECT_EQ(unmoved.value, 5);  // NOLINT(bugprone-use-after-move)

  ASSERT_NE(queue.try_peek(), nullptr);
  EXPECT_EQ(queue.try_peek()->value, 1);
  EXPECT_EQ(queue.front().value, 1);
  queue.pop();

  auto second = queue.try_pop();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->value, 2);

  MoveOnly third(0);
  EXPECT_TRUE(queue.try_pop(third));
  EXPECT_EQ(third.value, 3);

  MoveOnly fourth(0);
  EXPECT_TRUE(queue.try_pop(fourth));
  EXPECT_EQ(fourth.value, 4);
  EXPECT_TRUE(queue.empty());
}

TEST(LockFreeSpscQueueTest, CopyOnlyElements) {
  FixedLockFreeSpscQueue<CopyOnly, 4> queue;

  CopyOnly c1(11);
  CopyOnly c2(22);
  queue.push(c1);
  EXPECT_TRUE(queue.try_push(c2));

  auto first = queue.try_pop();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->value, 11);

  CopyOnly second(0);
  EXPECT_TRUE(queue.try_pop(second));
  EXPECT_EQ(second.value, 22);
  EXPECT_TRUE(queue.empty());
}

#if defined(PW_THREAD_JOINING_ENABLED) && PW_THREAD_JOINING_ENABLED

struct ThreadSafeMoveOnly {
  explicit ThreadSafeMoveOnly(int val) : value(val), self(this) {}

  ThreadSafeMoveOnly(const ThreadSafeMoveOnly&) = delete;
  ThreadSafeMoveOnly& operator=(const ThreadSafeMoveOnly&) = delete;

  ThreadSafeMoveOnly(ThreadSafeMoveOnly&& other) noexcept
      : value(other.value), self(this) {
    PW_ASSERT(other.self == &other);
    other.value = -1;
  }

  ThreadSafeMoveOnly& operator=(ThreadSafeMoveOnly&& other) noexcept {
    PW_ASSERT(self == this);
    PW_ASSERT(other.self == &other);
    value = other.value;
    other.value = -1;
    return *this;
  }

  ~ThreadSafeMoveOnly() {
    PW_ASSERT(self == this);
    self = nullptr;
  }

  int value;
  ThreadSafeMoveOnly* self;
};

TEST(LockFreeSpscQueueTest, Multithreaded_SpscSingleElementStress) {
  constexpr int kNumItems = 10000;
  FixedLockFreeSpscQueue<ThreadSafeMoveOnly, 16> queue;

  thread::test::TestThreadContext producer_context;
  Thread producer_thread(producer_context.options(), [&queue]() {
    for (int i = 0; i < kNumItems; ++i) {
      EXPECT_LE(queue.size(), queue.capacity());
      while (!queue.try_emplace(i)) {
        this_thread::yield();
      }
    }
  });

  int expected = 0;
  while (expected < kNumItems) {
    EXPECT_LE(queue.size(), queue.capacity());
    auto item = queue.try_pop();
    if (!item.has_value()) {
      this_thread::yield();
      continue;
    }
    ASSERT_EQ(item->value, expected);
    ++expected;
  }

  producer_thread.join();
  EXPECT_EQ(expected, kNumItems);
  EXPECT_TRUE(queue.empty());
}

TEST(LockFreeSpscQueueTest, Multithreaded_DynamicQueueStress) {
  constexpr int kNumItems = 2000;
  AllocatorForTest<1024> allocator;
  {
    auto queue =
        FixedLockFreeSpscQueue<ThreadSafeMoveOnly>::Allocate(allocator, 32u);

    thread::test::TestThreadContext producer_context;
    Thread producer_thread(producer_context.options(), [&queue]() {
      for (int i = 0; i < kNumItems; ++i) {
        while (!queue.try_emplace(i)) {
          this_thread::yield();
        }
      }
    });

    int expected = 0;
    while (expected < kNumItems) {
      auto item = queue.try_pop();
      if (!item.has_value()) {
        this_thread::yield();
        continue;
      }
      ASSERT_EQ(item->value, expected);
      ++expected;
    }

    producer_thread.join();
    EXPECT_EQ(expected, kNumItems);
    EXPECT_TRUE(queue.empty());
  }
  EXPECT_EQ(allocator.metrics().allocated_bytes.value(), 0u);
}

#endif  // defined(PW_THREAD_JOINING_ENABLED) && PW_THREAD_JOINING_ENABLED

// Negative compilation tests.
#if PW_NC_TEST(CannotUseConstElement)
PW_NC_EXPECT("const elements are not supported");
[[maybe_unused]] FixedLockFreeSpscQueue<const int, 4> queue;
#elif PW_NC_TEST(ZeroCapacity)
PW_NC_EXPECT("Capacity must be greater than zero");
[[maybe_unused]] FixedLockFreeSpscQueue<int, 0> queue;
#elif PW_NC_TEST(TooLargeForSizeType)
PW_NC_EXPECT(
    "The capacity is too large for the size_type; use a larger size_type");
[[maybe_unused]] FixedLockFreeSpscQueue<int, 32768, uint16_t> queue;
#elif PW_NC_TEST(QueueNotCopyable)
PW_NC_EXPECT_CLANG("call to deleted constructor");
PW_NC_EXPECT_GCC("use of deleted function");
[[maybe_unused]] void CopyQueue(const FixedLockFreeSpscQueue<int, 4>& queue) {
  [[maybe_unused]] FixedLockFreeSpscQueue<int, 4> copy(queue);
}
#endif  // PW_NC_TEST

}  // namespace
}  // namespace pw
