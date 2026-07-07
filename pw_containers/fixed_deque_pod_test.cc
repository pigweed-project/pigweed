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

#include <array>
#include <cstddef>

#include "pw_containers/deque.h"
#include "pw_containers/pod_deque.h"
#include "pw_unit_test/framework.h"

namespace pw::containers {
namespace {

TEST(PodDeque, PushBack) {
  FixedDeque<int, 3> deque;
  EXPECT_TRUE(deque.empty());
  EXPECT_FALSE(deque.full());
  EXPECT_EQ(deque.size(), 0u);

  deque.push_back(5);
  EXPECT_EQ(deque.size(), 1u);
  EXPECT_EQ(deque.front(), 5);
  EXPECT_EQ(deque.back(), 5);

  deque.push_back(2);
  EXPECT_EQ(deque.size(), 2u);
  EXPECT_EQ(deque.back(), 2);
}

TEST(PodDeque, PopBack) {
  FixedDeque<int, 3> deque;
  deque.push_back(45);
  deque.push_back(2);

  deque.pop_back();
  EXPECT_EQ(deque.size(), 1u);
  EXPECT_EQ(deque.front(), 45);
}

TEST(PodDeque, PushFront) {
  FixedDeque<int, 3> deque;
  deque.push_front(3);
  EXPECT_EQ(deque.size(), 1u);
  EXPECT_EQ(deque.front(), 3);
  EXPECT_EQ(deque.back(), 3);

  deque.push_front(2);
  EXPECT_EQ(deque.size(), 2u);
  EXPECT_EQ(deque.front(), 2);
}

TEST(PodDeque, PopFront) {
  FixedDeque<int, 3> deque;
  deque.push_back(1);
  deque.push_back(2);

  deque.pop_front();
  EXPECT_EQ(deque.size(), 1u);
  EXPECT_EQ(deque.front(), 2);
}

TEST(PodDeque, InheritanceBehavior) {
  // For POD types, FixedDeque should inherit from PodDeque
  using PodFixedDeque = FixedDeque<int, 10>;
  using ExpectedPodBase = PodDeque<int, uint16_t>;

  static_assert(std::is_base_of_v<ExpectedPodBase, PodFixedDeque>,
                "FixedDeque with POD type should inherit from PodDeque");

  // For Non-POD types, FixedDeque should inherit from Deque
  struct NonPod {
    int id_;
    int value_;
    NonPod(int id, int value) : id_(id), value_(value) {}
    ~NonPod() {}
  };

  using NonPodFixedDeque = FixedDeque<NonPod, 10>;
  using ExpectedNonPodBase = Deque<NonPod, uint16_t>;

  static_assert(std::is_base_of_v<ExpectedNonPodBase, NonPodFixedDeque>,
                "FixedDeque with Non-POD type should inherit from Deque");
}

TEST(PodDeque, ComplexPod) {
  struct ComplexPod {
    int id;
    int value;
  };

  using PodFixedDeque = FixedDeque<ComplexPod, 3>;
  using ExpectedPodBase = PodDeque<ComplexPod, uint16_t>;

  static_assert(std::is_base_of_v<ExpectedPodBase, PodFixedDeque>,
                "FixedDeque with ComplexPod should inherit from PodDeque");

  FixedDeque<ComplexPod, 3> deque;
  deque.push_back({1, 10});
  deque.push_back({2, 20});

  EXPECT_EQ(deque.size(), 2u);
  EXPECT_EQ(deque.front().id, 1);
  EXPECT_EQ(deque.front().value, 10);
  EXPECT_EQ(deque.back().id, 2);
  EXPECT_EQ(deque.back().value, 20);

  deque.pop_front();
  EXPECT_EQ(deque.size(), 1u);
  EXPECT_EQ(deque.front().id, 2);
  EXPECT_EQ(deque.front().value, 20);
}

TEST(PodDeque, MaxSize) {
  FixedDeque<int, 7> deque;
  EXPECT_EQ(deque.max_size(), 7u);
  EXPECT_EQ(deque.capacity(), 7u);
}

TEST(PodDeque, Assign) {
  FixedDeque<int, 5> deque;
  deque.push_back(99);
  deque.push_back(100);

  std::array<int, 3> new_data = {5, 10, 15};
  deque.assign(new_data.begin(), new_data.end());

  EXPECT_EQ(deque.size(), 3u);
  EXPECT_EQ(deque.front(), 5);
  EXPECT_EQ(deque.back(), 15);
}

TEST(PodDeque, Clear) {
  FixedDeque<int, 4> deque;
  deque.push_back(1);
  deque.push_back(2);
  EXPECT_FALSE(deque.empty());

  deque.clear();
  EXPECT_TRUE(deque.empty());
  EXPECT_EQ(deque.size(), 0u);
}

TEST(PodDeque, Resize) {
  FixedDeque<int, 5> deque;
  deque.push_back(42);

  deque.resize(3);
  EXPECT_EQ(deque.size(), 3u);
  EXPECT_EQ(deque.front(), 42);
  EXPECT_EQ(deque.back(), 0);

  deque.resize(1);
  EXPECT_EQ(deque.size(), 1u);
  EXPECT_EQ(deque.front(), 42);
}

TEST(PodDeque, NonPodAssignAndResize) {
  struct TestNonPod {
    int val_;
    TestNonPod() : val_(0) {}
    TestNonPod(int v) : val_(v) {}
    ~TestNonPod() {}
  };

  FixedDeque<TestNonPod, 5> deque;
  deque.push_back(10);
  deque.push_back(20);

  std::array<TestNonPod, 2> source = {5, 15};
  deque.assign(source.begin(), source.end());

  EXPECT_EQ(deque.size(), 2u);
  EXPECT_EQ(deque.front().val_, 5);
  EXPECT_EQ(deque.back().val_, 15);

  deque.resize(4, TestNonPod(99));
  EXPECT_EQ(deque.size(), 4u);
  EXPECT_EQ(deque.back().val_, 99);
}

TEST(PodDeque, MoveConstructorSameCapacity) {
  FixedDeque<int, 5> deque1;
  for (int i = 1; i <= 5; ++i)
    deque1.push_back(i);
  deque1.pop_front();
  deque1.pop_front();
  deque1.push_back(6);  // [6, (empty), 3, 4, 5] (head=2, tail=1, count=4)

  FixedDeque<int, 5> dest(std::move(deque1));

  EXPECT_EQ(dest.size(), 4u);
  EXPECT_EQ(dest.front(), 3);
  EXPECT_EQ(dest.back(), 6);
  EXPECT_TRUE(deque1.empty());  // NOLINT(bugprone-use-after-move)
}

TEST(PodDeque, MoveConstructorDifferentCapacity) {
  FixedDeque<int, 5> deque1;
  for (int i = 1; i <= 5; ++i)
    deque1.push_back(i);
  deque1.pop_front();
  deque1.pop_front();
  deque1.push_back(6);  // wrapped!

  FixedDeque<int, 10> dest(std::move(deque1));

  EXPECT_EQ(dest.size(), 4u);
  EXPECT_EQ(dest.front(), 3);
  EXPECT_EQ(dest.back(), 6);
  EXPECT_TRUE(deque1.empty());  // NOLINT(bugprone-use-after-move)
}

TEST(PodDeque, MoveAssignmentSameCapacity) {
  FixedDeque<int, 5> deque1;
  for (int i = 1; i <= 5; ++i)
    deque1.push_back(i);
  deque1.pop_front();
  deque1.pop_front();
  deque1.push_back(6);  // wrapped!

  FixedDeque<int, 5> dest;
  dest.push_back(99);
  dest = std::move(deque1);

  EXPECT_EQ(dest.size(), 4u);
  EXPECT_EQ(dest.front(), 3);
  EXPECT_EQ(dest.back(), 6);
  EXPECT_TRUE(deque1.empty());  // NOLINT(bugprone-use-after-move)
}

TEST(PodDeque, MoveAssignmentDifferentCapacity) {
  FixedDeque<int, 5> deque1;
  for (int i = 1; i <= 5; ++i)
    deque1.push_back(i);
  deque1.pop_front();
  deque1.pop_front();
  deque1.push_back(6);  // wrapped!

  FixedDeque<int, 10> dest;
  dest.push_back(99);
  dest = std::move(deque1);

  EXPECT_EQ(dest.size(), 4u);
  EXPECT_EQ(dest.front(), 3);
  EXPECT_EQ(dest.back(), 6);
  EXPECT_TRUE(deque1.empty());  // NOLINT(bugprone-use-after-move)
}

TEST(PodDeque, SwapSameCapacity) {
  FixedDeque<int, 5> deque1;
  deque1.push_back(1);
  deque1.push_back(2);

  FixedDeque<int, 5> deque2;
  deque2.push_back(10);
  deque2.push_back(20);
  deque2.push_back(30);

  deque1.swap(deque2);

  EXPECT_EQ(deque1.size(), 3u);
  EXPECT_EQ(deque1.front(), 10);
  EXPECT_EQ(deque1.back(), 30);

  EXPECT_EQ(deque2.size(), 2u);
  EXPECT_EQ(deque2.front(), 1);
  EXPECT_EQ(deque2.back(), 2);
}

TEST(PodDeque, SwapDifferentCapacity) {
  FixedDeque<int, 15> deque1;
  deque1.push_back(1);
  deque1.push_back(2);

  FixedDeque<int, 10> deque2;
  deque2.push_back(10);
  deque2.push_back(20);
  deque2.push_back(30);

  EXPECT_EQ(deque1.capacity(), 15u);
  EXPECT_EQ(deque2.capacity(), 10u);

  deque1.swap(deque2);

  EXPECT_EQ(deque1.capacity(), 15u);
  EXPECT_EQ(deque2.capacity(), 10u);

  EXPECT_EQ(deque1.size(), 3u);
  EXPECT_EQ(deque1.front(), 10);
  EXPECT_EQ(deque1.back(), 30);

  EXPECT_EQ(deque2.size(), 2u);
  EXPECT_EQ(deque2.front(), 1);
  EXPECT_EQ(deque2.back(), 2);
}

TEST(PodDeque, UnalignedBuffer) {
  alignas(uint64_t) std::array<std::byte, sizeof(uint64_t) * 8> buffer = {};

  for (size_t i = 0; i <= sizeof(uint64_t); ++i) {
    PodDeque<uint64_t> deque(pw::span<std::byte>(buffer).subspan(i));
    EXPECT_EQ(deque.capacity(), (i == 0) ? 8u : 7u);

    deque.push_back(600613u);

    void* first_item = &deque.front();
    EXPECT_EQ(reinterpret_cast<uintptr_t>(first_item) % alignof(uint64_t), 0u)
        << "Deque items should be correctly aligned";
    EXPECT_EQ(deque.front(), 600613u);
  }
}

TEST(PodDeque, UnalignedBuffer_EmptyDueToAlignment) {
  alignas(uint64_t) std::array<std::byte, sizeof(uint64_t) + 1> buffer = {};
  auto unaligned = pw::span(buffer).subspan(1);
  ASSERT_EQ(unaligned.size(), sizeof(uint64_t));

  PodDeque<uint64_t> deque(unaligned);
  EXPECT_EQ(deque.capacity(), 0u);
}

}  // namespace
}  // namespace pw::containers
