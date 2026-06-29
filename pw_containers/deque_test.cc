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

#include "pw_containers/deque.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>

#include "pw_allocator/null_allocator.h"
#include "pw_allocator/testing.h"
#include "pw_assert/assert.h"
#include "pw_compilation_testing/negative_compilation.h"
#include "pw_containers/internal/container_tests.h"
#include "pw_containers/internal/test_helpers.h"
#include "pw_polyfill/language_feature_macros.h"
#include "pw_polyfill/standard.h"
#include "pw_span/span.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::containers::test::CopyOnly;
using pw::containers::test::Counter;
using pw::containers::test::MoveOnly;

TEST(Deque, PushBackOverwrite) {
  pw::containers::StorageFor<int, 4> storage;
  pw::Deque<int> deque(storage);

  deque.push_back(1);
  deque.push_back(2);
  deque.push_back(3);
  deque.push_back(4);

  EXPECT_EQ(deque.size(), 4u);
  EXPECT_EQ(deque.front(), 1);
  EXPECT_EQ(deque.back(), 4);

  deque.push_back_overwrite(5);
  EXPECT_EQ(deque.size(), 4u);
  EXPECT_EQ(deque.front(), 2);
  EXPECT_EQ(deque.back(), 5);

  int val = 6;
  deque.push_back_overwrite(val);
  EXPECT_EQ(deque.size(), 4u);
  EXPECT_EQ(deque.front(), 3);
  EXPECT_EQ(deque.back(), 6);
}

TEST(Deque, EmplaceBackOverwrite) {
  pw::containers::StorageFor<Counter, 2> storage;
  pw::Deque<Counter> deque(storage);

  Counter::Reset();
  deque.emplace_back_overwrite(1);
  deque.emplace_back_overwrite(2);

  EXPECT_EQ(deque.size(), 2u);
  EXPECT_EQ(Counter::created, 2);
  EXPECT_EQ(Counter::destroyed, 0);

  deque.emplace_back_overwrite(3);
  EXPECT_EQ(deque.size(), 2u);
  EXPECT_EQ(deque.front().value, 2);
  EXPECT_EQ(deque.back().value, 3);
  EXPECT_EQ(Counter::created, 3);
  EXPECT_EQ(Counter::destroyed, 1);
}

TEST(Deque, PushFrontOverwrite) {
  pw::containers::StorageFor<int, 4> storage;
  pw::Deque<int> deque(storage);

  deque.push_front(1);
  deque.push_front(2);
  deque.push_front(3);
  deque.push_front(4);

  EXPECT_EQ(deque.size(), 4u);
  EXPECT_EQ(deque.front(), 4);
  EXPECT_EQ(deque.back(), 1);

  deque.push_front_overwrite(5);
  EXPECT_EQ(deque.size(), 4u);
  EXPECT_EQ(deque.front(), 5);
  EXPECT_EQ(deque.back(), 2);

  int val = 6;
  deque.push_front_overwrite(val);
  EXPECT_EQ(deque.size(), 4u);
  EXPECT_EQ(deque.front(), 6);
  EXPECT_EQ(deque.back(), 3);
}

TEST(Deque, EmplaceFrontOverwrite) {
  pw::containers::StorageFor<Counter, 2> storage;
  pw::Deque<Counter> deque(storage);

  Counter::Reset();
  deque.emplace_front_overwrite(1);
  deque.emplace_front_overwrite(2);

  EXPECT_EQ(deque.size(), 2u);
  EXPECT_EQ(Counter::created, 2);
  EXPECT_EQ(Counter::destroyed, 0);

  deque.emplace_front_overwrite(3);
  EXPECT_EQ(deque.size(), 2u);
  EXPECT_EQ(deque.front().value, 3);
  EXPECT_EQ(deque.back().value, 2);
  EXPECT_EQ(Counter::created, 3);
  EXPECT_EQ(Counter::destroyed, 1);
}

TEST(Deque, ZeroCapacity) {
  Counter::Reset();
  {
    pw::Deque<Counter> container({});
    EXPECT_EQ(container.size(), 0u);
    EXPECT_TRUE((pw::Deque<Counter>({})).full());
  }
  EXPECT_EQ(Counter::created, 0);
  EXPECT_EQ(Counter::destroyed, 0);
}

TEST(Deque, Constinit_Bytes) {
  PW_CONSTINIT static std::array<std::byte, 32> buffer = {};
  PW_CONSTINIT static pw::Deque<char> deque(buffer);

  EXPECT_TRUE(deque.empty());
  EXPECT_EQ(deque.capacity(), sizeof(buffer));
}

TEST(Deque, Constinit_Storage) {
  PW_CONSTINIT static pw::containers::StorageFor<MoveOnly, 10> storage;
  PW_CONSTINIT static pw::Deque<MoveOnly> deque(storage);

  EXPECT_TRUE(deque.empty());
  EXPECT_EQ(deque.capacity(), 10u);
}

TEST(Deque, Storage_DifferentAlignment) {
  pw::containers::Storage<128, 7> larger_alignment_storage;
  pw::Deque<uint32_t> deque(larger_alignment_storage);

  EXPECT_EQ(deque.capacity(), 1u);

#if PW_NC_TEST(Deque_InvalidAlignment)
  PW_NC_EXPECT("kAlignment >= alignof\(value_type\)");
  pw::containers::Storage<2, 2> smaller_alignment_storage;
  [[maybe_unused]] pw::Deque<uint32_t> bad_deque(smaller_alignment_storage);
#endif  // PW_NC_TEST
}

TEST(Deque, UnalignedBuffer) {
  alignas(uint64_t) std::array<std::byte, sizeof(uint64_t) * 8> buffer = {};

  for (size_t i = 0; i <= sizeof(uint64_t); ++i) {
    pw::Deque<uint64_t> deque(pw::span<std::byte>(buffer).subspan(i));
    EXPECT_EQ(deque.capacity(), (i == 0) ? 8u : 7u);

    deque.push_back(600613u);

    void* first_item = &deque.front();
    EXPECT_EQ(reinterpret_cast<uintptr_t>(first_item) % alignof(uint64_t), 0u)
        << "Deque items should be correctly aligned";
    EXPECT_EQ(deque.front(), 600613u);
  }
}

TEST(Deque, UnalignedBuffer_EmptyDueToAlignment) {
  alignas(uint64_t) std::array<std::byte, sizeof(uint64_t) + 1> buffer = {};
  auto unaligned = pw::span(buffer).subspan(1);
  ASSERT_EQ(unaligned.size(), sizeof(uint64_t));

  pw::Deque<uint64_t> deque(unaligned);
  EXPECT_EQ(deque.capacity(), 0u);
}

TEST(Deque, MoveNotSupported) {
  pw::containers::StorageFor<int, 5> storage_1;
  [[maybe_unused]] pw::Deque<int> deque_1(storage_1);

  pw::containers::StorageFor<int, 5> storage_2;
  [[maybe_unused]] pw::Deque<int> deque_2(storage_2);

#if PW_NC_TEST(Deque_MoveNotSupported)
  PW_NC_EXPECT("delete");
  deque_1 = std::move(deque_2);
#endif  // PW_NC_TEST
}

TEST(FixedDeque, Construct_Span) {
  pw::containers::StorageFor<MoveOnly, 4> storage;
  auto deque =
      pw::FixedDeque<MoveOnly>(pw::span(storage.data(), storage.size()));

  EXPECT_EQ(deque.capacity(), 4u);
  EXPECT_EQ(deque.deallocator(), nullptr);
}

TEST(FixedDeque, Construct_Storage) {
  pw::containers::StorageFor<MoveOnly, 4> storage;
  auto deque = pw::FixedDeque<MoveOnly>(storage);

  EXPECT_EQ(deque.capacity(), 4u);
  EXPECT_EQ(deque.deallocator(), nullptr);
}

TEST(FixedDeque, Allocate_ZeroCapacity) {
  auto deque_1 =
      pw::FixedDeque<MoveOnly>::Allocate(pw::allocator::GetNullAllocator(), 0);
  EXPECT_EQ(deque_1.capacity(), 0u);
}

TEST(FixedDeque, Allocate) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  auto deque = pw::FixedDeque<MoveOnly>::Allocate(allocator, 4);

  EXPECT_EQ(deque.capacity(), 4u);
  EXPECT_EQ(deque.deallocator(), &allocator);

  deque.emplace_back(MoveOnly(1));
  deque.emplace_back(MoveOnly(2));
  deque.emplace_back(MoveOnly(3));
  deque.emplace_back(MoveOnly(4));

  EXPECT_EQ(deque.size(), 4u);
  EXPECT_EQ(deque[0].value, 1);
  EXPECT_EQ(deque[3].value, 4);
}

TEST(FixedDeque, TryAllocate_ZeroCapacityIfAllocationFails) {
  auto deque_1 = pw::FixedDeque<MoveOnly>::TryAllocate(
      pw::allocator::GetNullAllocator(), 4);
  EXPECT_EQ(deque_1.capacity(), 0u);
}

TEST(FixedDeque, TryAllocate_Success) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  auto deque = pw::FixedDeque<MoveOnly>::TryAllocate(allocator, 4);

  EXPECT_EQ(deque.capacity(), 4u);
  EXPECT_EQ(deque.deallocator(), &allocator);
}

TEST(FixedDeque, TryAllocate_Failure) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  allocator.Exhaust();
  auto deque = pw::FixedDeque<MoveOnly>::TryAllocate(allocator, 4);

  EXPECT_EQ(deque.capacity(), 0u);
  EXPECT_EQ(deque.deallocator(), nullptr);
}

TEST(FixedDeque, UniquePtr) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  constexpr size_t kCapacity = 4;
  constexpr auto kLayout = pw::allocator::Layout::Of<MoveOnly[kCapacity]>();
  auto ptr = allocator.Allocate(kLayout);
  ASSERT_NE(ptr, nullptr);

  auto unique_ptr = pw::UniquePtr<std::byte[]>(
      static_cast<std::byte*>(ptr), kLayout.size(), allocator);

  auto deque = pw::FixedDeque<MoveOnly>::WithStorage(std::move(unique_ptr));

  EXPECT_EQ(deque.capacity(), kCapacity);
  EXPECT_EQ(deque.deallocator(), &allocator);
}

TEST(FixedDeque, MoveConstruct_FixedToSameSize) {
  pw::FixedDeque<MoveOnly, 4> deque_1;
  deque_1.emplace_back(MoveOnly(1));
  deque_1.emplace_back(MoveOnly(2));
  deque_1.emplace_back(MoveOnly(3));
  deque_1.emplace_back(MoveOnly(4));

  pw::FixedDeque<MoveOnly, 4> deque_2(std::move(deque_1));

  EXPECT_EQ(0u, deque_1.size());  // NOLINT(bugprone-use-after-move)

  ASSERT_EQ(4u, deque_2.size());
  EXPECT_EQ(1, deque_2[0].value);
  EXPECT_EQ(2, deque_2[1].value);
  EXPECT_EQ(3, deque_2[2].value);
  EXPECT_EQ(4, deque_2[3].value);
}

TEST(FixedDeque, MoveConstruct_FixedToLarger) {
  pw::FixedDeque<MoveOnly, 4> deque_1;
  deque_1.emplace_back(MoveOnly(1));
  deque_1.emplace_back(MoveOnly(2));
  deque_1.emplace_back(MoveOnly(3));
  deque_1.emplace_back(MoveOnly(4));

  pw::FixedDeque<MoveOnly, 5> deque_2(std::move(deque_1));

  EXPECT_EQ(0u, deque_1.size());  // NOLINT(bugprone-use-after-move)

  ASSERT_EQ(4u, deque_2.size());
  EXPECT_EQ(1, deque_2[0].value);
  EXPECT_EQ(2, deque_2[1].value);
  EXPECT_EQ(3, deque_2[2].value);
  EXPECT_EQ(4, deque_2[3].value);
}

TEST(FixedDeque, MoveConstruct_FixedToSmaller) {
  [[maybe_unused]] pw::FixedDeque<MoveOnly, 3> deque_1;

#if PW_NC_TEST(FixedDeque_MoveConstruct_FixedToSmaller)
  PW_NC_EXPECT("kOtherCapacity");
  pw::FixedDeque<MoveOnly, 2> deque_2(std::move(deque_1));
#endif  // PW_NC_TEST
}

TEST(FixedDeque, MoveConstruct_Dynamic) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  auto deque_1 = pw::FixedDeque<MoveOnly>::Allocate(allocator, 4);
  deque_1.emplace_back(MoveOnly(1));
  deque_1.emplace_back(MoveOnly(2));
  deque_1.emplace_back(MoveOnly(3));
  deque_1.emplace_back(MoveOnly(4));

  allocator.Exhaust();

  pw::FixedDeque<MoveOnly> deque_2(std::move(deque_1));

  EXPECT_EQ(0u, deque_1.size());  // NOLINT(bugprone-use-after-move)

  ASSERT_EQ(4u, deque_2.size());
  EXPECT_EQ(1, deque_2[0].value);
  EXPECT_EQ(2, deque_2[1].value);
  EXPECT_EQ(3, deque_2[2].value);
  EXPECT_EQ(4, deque_2[3].value);
}

TEST(FixedDeque, SwapDynamic) {
  pw::allocator::test::AllocatorForTest<256> allocator;

  auto deque_1 = pw::FixedDeque<MoveOnly>::Allocate(allocator, 4);
  auto deque_2 = pw::FixedDeque<MoveOnly>::Allocate(allocator, 10);

  deque_1.emplace_back(MoveOnly(1));
  deque_1.emplace_back(MoveOnly(2));
  deque_1.emplace_back(MoveOnly(3));

  deque_2.emplace_back(MoveOnly(99));

  allocator.Exhaust();
  deque_1.swap(deque_2);

  ASSERT_EQ(1u, deque_1.size());
  ASSERT_EQ(3u, deque_2.size());

  EXPECT_EQ(99, deque_1[0].value);

  EXPECT_EQ(1, deque_2[0].value);
  EXPECT_EQ(2, deque_2[1].value);
  EXPECT_EQ(3, deque_2[2].value);
}

TEST(FixedDeque, Swap_StaticSameSize) {
  pw::FixedDeque<MoveOnly, 4> deque_1;
  deque_1.emplace_back(MoveOnly(1));
  deque_1.emplace_back(MoveOnly(2));

  pw::FixedDeque<MoveOnly, 4> deque_2;
  deque_2.emplace_back(MoveOnly(3));
  deque_2.emplace_back(MoveOnly(4));

  deque_1.swap(deque_2);

  ASSERT_EQ(2u, deque_1.size());
  EXPECT_EQ(3, deque_1[0].value);
  EXPECT_EQ(4, deque_1[1].value);

  ASSERT_EQ(2u, deque_2.size());
  EXPECT_EQ(1, deque_2[0].value);
  EXPECT_EQ(2, deque_2[1].value);
}

TEST(FixedDeque, Swap_StaticDifferentSize) {
  pw::FixedDeque<MoveOnly, 4> deque_1;
  deque_1.emplace_back(MoveOnly(1));

  pw::FixedDeque<MoveOnly, 4> deque_2;
  deque_2.emplace_back(MoveOnly(3));
  deque_2.emplace_back(MoveOnly(4));

  deque_1.swap(deque_2);

  ASSERT_EQ(2u, deque_1.size());
  EXPECT_EQ(3, deque_1[0].value);
  EXPECT_EQ(4, deque_1[1].value);

  ASSERT_EQ(1u, deque_2.size());
  EXPECT_EQ(1, deque_2[0].value);
}

TEST(FixedDeque, Swap_StaticMismatchedCapacity) {
  pw::FixedDeque<MoveOnly, 4> deque_1;
  deque_1.emplace_back(MoveOnly(1));
  deque_1.emplace_back(MoveOnly(2));

  pw::FixedDeque<MoveOnly, 5> deque_2;
  deque_2.emplace_back(MoveOnly(3));
  deque_2.emplace_back(MoveOnly(4));
  deque_2.emplace_back(MoveOnly(5));

  deque_1.swap(deque_2);

  ASSERT_EQ(3u, deque_1.size());
  EXPECT_EQ(3, deque_1[0].value);
  EXPECT_EQ(4, deque_1[1].value);
  EXPECT_EQ(5, deque_1[2].value);

  ASSERT_EQ(2u, deque_2.size());
  EXPECT_EQ(1, deque_2[0].value);
  EXPECT_EQ(2, deque_2[1].value);

  deque_2.swap(deque_1);

  ASSERT_EQ(3u, deque_2.size());
  EXPECT_EQ(3, deque_2[0].value);
  EXPECT_EQ(4, deque_2[1].value);
  EXPECT_EQ(5, deque_2[2].value);

  ASSERT_EQ(2u, deque_1.size());
  EXPECT_EQ(1, deque_1[0].value);
  EXPECT_EQ(2, deque_1[1].value);
}

TEST(FixedDeque, Swap_WithEmpty) {
  pw::FixedDeque<MoveOnly, 4> deque_1;
  deque_1.emplace_back(MoveOnly(1));
  deque_1.emplace_back(MoveOnly(2));

  pw::FixedDeque<MoveOnly, 4> deque_2;

  deque_1.swap(deque_2);

  ASSERT_EQ(0u, deque_1.size());

  ASSERT_EQ(2u, deque_2.size());
  EXPECT_EQ(1, deque_2[0].value);
  EXPECT_EQ(2, deque_2[1].value);
}

TEST(FixedDeque, Swap_StaticAndDynamic) {
  pw::FixedDeque<MoveOnly, 4> deque_1;
  deque_1.emplace_back(MoveOnly(1));
  deque_1.emplace_back(MoveOnly(2));

  pw::allocator::test::AllocatorForTest<256> allocator;
  auto deque_2 = pw::FixedDeque<MoveOnly>::Allocate(allocator, 5);
  deque_2.emplace_back(MoveOnly(3));
  deque_2.emplace_back(MoveOnly(4));
  deque_2.emplace_back(MoveOnly(5));

  deque_1.swap(deque_2);

  ASSERT_EQ(3u, deque_1.size());
  EXPECT_EQ(3, deque_1[0].value);
  EXPECT_EQ(4, deque_1[1].value);
  EXPECT_EQ(5, deque_1[2].value);

  ASSERT_EQ(2u, deque_2.size());
  EXPECT_EQ(1, deque_2[0].value);
  EXPECT_EQ(2, deque_2[1].value);

  deque_2.swap(deque_1);

  ASSERT_EQ(3u, deque_2.size());
  EXPECT_EQ(3, deque_2[0].value);
  EXPECT_EQ(4, deque_2[1].value);
  EXPECT_EQ(5, deque_2[2].value);

  ASSERT_EQ(2u, deque_1.size());
  EXPECT_EQ(1, deque_1[0].value);
  EXPECT_EQ(2, deque_1[1].value);
}

TEST(FixedDeque, Move_OwnedToOwned) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  auto deque1 = pw::FixedDeque<MoveOnly>::Allocate(allocator, 4);
  deque1.emplace_back(MoveOnly(1));

  auto deque2 = std::move(deque1);

  EXPECT_EQ(deque1.capacity(), 0u);  // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(deque1.deallocator(), nullptr);

  EXPECT_EQ(deque2.capacity(), 4u);
  EXPECT_EQ(deque2.deallocator(), &allocator);
  EXPECT_EQ(deque2.size(), 1u);
  EXPECT_EQ(deque2[0].value, 1);
}

TEST(FixedDeque, Move_UnownedToOwned) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  pw::containers::StorageFor<MoveOnly, 4> storage;

  auto deque1 = pw::FixedDeque<MoveOnly>::Allocate(allocator, 4);
  deque1.emplace_back(MoveOnly(1));

  auto deque2 = pw::FixedDeque<MoveOnly>(storage);
  deque2.emplace_back(MoveOnly(2));

  deque1 = std::move(deque2);

  // deque1 should have freed its old buffer and taken over deque2's unowned
  // buffer.
  EXPECT_EQ(deque1.capacity(), 4u);
  EXPECT_EQ(deque1.deallocator(), nullptr);
  EXPECT_EQ(deque1.size(), 1u);
  EXPECT_EQ(deque1[0].value, 2);

  EXPECT_EQ(deque2.capacity(), 0u);  // NOLINT(bugprone-use-after-move)
}

TEST(FixedDeque, Move_OwnedToUnowned) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  auto deque1 = pw::FixedDeque<MoveOnly>::Allocate(allocator, 4);
  deque1.emplace_back(MoveOnly(1));

  pw::containers::StorageFor<MoveOnly, 4> storage;
  auto deque2 = pw::FixedDeque<MoveOnly>(storage);
  deque2.emplace_back(MoveOnly(2));

  deque2 = std::move(deque1);

  // deque2 should have taken over deque1's owned buffer.
  EXPECT_EQ(deque2.capacity(), 4u);
  EXPECT_EQ(deque2.deallocator(), &allocator);
  EXPECT_EQ(deque2.size(), 1u);
  EXPECT_EQ(deque2[0].value, 1);

  EXPECT_EQ(deque1.capacity(), 0u);  // NOLINT(bugprone-use-after-move)
}

TEST(FixedDeque, Swap_OwnedOwned) {
  pw::allocator::test::AllocatorForTest<128> allocator1;
  pw::allocator::test::AllocatorForTest<128> allocator2;

  auto deque1 = pw::FixedDeque<MoveOnly>::Allocate(allocator1, 1);
  deque1.emplace_back(MoveOnly(1));

  auto deque2 = pw::FixedDeque<MoveOnly>::Allocate(allocator2, 4);
  deque2.emplace_back(MoveOnly(2));

  deque1.swap(deque2);

  EXPECT_EQ(deque1.deallocator(), &allocator2);
  EXPECT_EQ(deque1.size(), 1u);
  EXPECT_EQ(deque1.capacity(), 4u);
  EXPECT_EQ(deque1[0].value, 2);

  EXPECT_EQ(deque2.deallocator(), &allocator1);
  EXPECT_EQ(deque2.size(), 1u);
  EXPECT_EQ(deque2.capacity(), 1u);
  EXPECT_EQ(deque2[0].value, 1);
}

TEST(FixedDeque, Swap_OwnedUnowned) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  pw::containers::StorageFor<MoveOnly, 4> storage;

  auto deque1 = pw::FixedDeque<MoveOnly>::Allocate(allocator, 4);
  deque1.emplace_back(MoveOnly(1));

  auto deque2 = pw::FixedDeque<MoveOnly>(storage);
  deque2.emplace_back(MoveOnly(2));

  deque1.swap(deque2);

  EXPECT_EQ(deque1.deallocator(), nullptr);
  EXPECT_EQ(deque1.size(), 1u);
  EXPECT_EQ(deque1[0].value, 2);

  EXPECT_EQ(deque2.deallocator(), &allocator);
  EXPECT_EQ(deque2.size(), 1u);
  EXPECT_EQ(deque2[0].value, 1);
}

TEST(FixedDeque, UseStaticOrDynamicFixedDequeAsDeque) {
  pw::FixedDeque<Counter, 4> deque_1;

  pw::allocator::test::AllocatorForTest<256> allocator;
  auto deque_2 = pw::FixedDeque<Counter>::Allocate(allocator, 5);

  pw::Deque<Counter>& deque_a = deque_1;
  deque_a.push_back(123);

  pw::Deque<Counter>* deque = &deque_a;
  deque->push_back(999);

  pw::Deque<Counter>& deque_b = deque_2;
  deque_b.push_back(321);

  deque = &deque_b;
  deque->pop_back();

  EXPECT_EQ(deque_a.size(), 2u);
  EXPECT_EQ(deque_a.at(0), 123);
  EXPECT_EQ(deque_a.at(1), 999);

  EXPECT_TRUE(deque_b.empty());
  EXPECT_EQ(deque, &deque_b);
}

void Move(pw::FixedDeque<MoveOnly>& d1, pw::FixedDeque<MoveOnly>& d2) {
  d1 = std::move(d2);
}

TEST(FixedDeque, SelfMoveAssignIsIdempotent) {
  pw::allocator::test::AllocatorForTest<128> allocator;
  auto deque = pw::FixedDeque<MoveOnly>::Allocate(allocator, 2);
  EXPECT_EQ(deque.capacity(), 2u);
  deque.emplace_back(MoveOnly(1));
  deque.emplace_back(MoveOnly(2));

  Move(deque, deque);

  EXPECT_EQ(deque.size(), 2u);
  EXPECT_EQ(deque[0].value, 1);
  EXPECT_EQ(deque[1].value, 2);
}

TEST(FixedDeque, CapacityTooLarge) {
#if PW_NC_TEST(FixedDeque_CapacityTooLargeForSizeType8)
  PW_NC_EXPECT("capacity is too large");
  [[maybe_unused]] pw::FixedDeque<int, 256, uint8_t> too_large;
#elif PW_NC_TEST(FixedDeque_CapacityTooLargeForSizeType16)
  PW_NC_EXPECT("capacity is too large");
  [[maybe_unused]] pw::FixedDeque<int, 65536> too_large;
#endif  // PW_NC_TEST
}

#if PW_CXX_STANDARD_IS_SUPPORTED(20)

constexpr size_t TestConstexpr() {
  pw::containers::StorageFor<int, 32> storage;
  pw::Deque<int> hello(storage);
  return hello.capacity();
}
static_assert(TestConstexpr() == 32);

#endif  // PW_CXX_STANDARD_IS_SUPPORTED(20)

// Instantiate shared container and iterator tests.
static_assert(pw::containers::test::IteratorProperties<pw::Deque>::kPasses);
static_assert(
    pw::containers::test::IteratorProperties<pw::FixedDeque>::kPasses);

static_assert(!std::is_copy_constructible_v<pw::Deque<int>>);
static_assert(!std::is_copy_constructible_v<pw::FixedDeque<int>>);
static_assert(!std::is_copy_constructible_v<pw::FixedDeque<int, 1>>);

static_assert(!std::is_move_constructible_v<pw::Deque<MoveOnly>>);
static_assert(std::is_move_constructible_v<pw::FixedDeque<MoveOnly>>);
static_assert(std::is_move_constructible_v<pw::FixedDeque<MoveOnly, 1>>);

static_assert(!std::is_copy_assignable_v<pw::Deque<CopyOnly>>);
static_assert(!std::is_copy_assignable_v<pw::FixedDeque<CopyOnly>>);
static_assert(!std::is_copy_assignable_v<pw::FixedDeque<CopyOnly, 1>>);

static_assert(!std::is_move_assignable_v<pw::Deque<MoveOnly>>);
static_assert(std::is_move_assignable_v<pw::FixedDeque<MoveOnly>>);
static_assert(std::is_move_assignable_v<pw::FixedDeque<MoveOnly, 1>>);

template <typename T>
class TestDeque : public pw::Deque<T> {
 public:
  using Base = pw::Deque<T>;
  using Base::Base;
  using Base::try_insert_shift_right;
};

TEST(Deque, TryInsertShiftRight_Value) {
  pw::containers::StorageFor<int, 6> storage;
  TestDeque<int> deque(storage);

  deque.push_back(1);
  deque.push_back(2);
  deque.push_back(3);

  auto it = deque.try_insert_shift_right(deque.begin() + 1, 2, 99);
  ASSERT_TRUE(it.has_value());
  EXPECT_EQ(*it, deque.begin() + 1);
  EXPECT_EQ(deque.size(), 5u);
  EXPECT_EQ(deque[0], 1);
  EXPECT_EQ(deque[1], 99);
  EXPECT_EQ(deque[2], 99);
  EXPECT_EQ(deque[3], 2);
  EXPECT_EQ(deque[4], 3);
}

TEST(Deque, TryInsertShiftRight_Iterator) {
  pw::containers::StorageFor<int, 6> storage;
  TestDeque<int> deque(storage);

  deque.push_back(1);
  deque.push_back(2);
  deque.push_back(3);

  std::array<int, 2> to_insert = {99, 98};

  auto it = deque.try_insert_shift_right(
      deque.begin() + 1, to_insert.begin(), to_insert.end());
  ASSERT_TRUE(it.has_value());
  EXPECT_EQ(*it, deque.begin() + 1);
  EXPECT_EQ(deque.size(), 5u);
  EXPECT_EQ(deque[0], 1);
  EXPECT_EQ(deque[1], 99);
  EXPECT_EQ(deque[2], 98);
  EXPECT_EQ(deque[3], 2);
  EXPECT_EQ(deque[4], 3);
}

TEST(Deque, TryInsertShiftRight_Wrapped) {
  pw::containers::StorageFor<int, 6> storage;
  TestDeque<int> deque(storage);

  deque.push_back(99);
  deque.push_back(99);
  deque.push_back(3);
  deque.push_back(4);
  deque.pop_front();
  deque.pop_front();
  deque.push_back(5);
  deque.push_back(6);
  deque.push_back(7);

  ASSERT_EQ(deque.size(), 5u);
  EXPECT_EQ(deque[0], 3);
  EXPECT_EQ(deque[1], 4);
  EXPECT_EQ(deque[2], 5);
  EXPECT_EQ(deque[3], 6);
  EXPECT_EQ(deque[4], 7);

  auto it = deque.try_insert_shift_right(deque.begin() + 2, 1, 99);
  ASSERT_TRUE(it.has_value());
  EXPECT_EQ(deque.size(), 6u);
  EXPECT_EQ(deque[0], 3);
  EXPECT_EQ(deque[1], 4);
  EXPECT_EQ(deque[2], 99);
  EXPECT_EQ(deque[3], 5);
  EXPECT_EQ(deque[4], 6);
  EXPECT_EQ(deque[5], 7);
}

TEST(Deque, TryInsertShiftRight_WrappedIterator) {
  pw::containers::StorageFor<int, 6> storage;
  TestDeque<int> deque(storage);

  deque.push_back(99);
  deque.push_back(99);
  deque.push_back(3);
  deque.push_back(4);
  deque.pop_front();
  deque.pop_front();
  deque.push_back(5);
  deque.push_back(6);
  deque.push_back(7);

  ASSERT_EQ(deque.size(), 5u);

  std::array<int, 1> to_insert = {99};

  auto it = deque.try_insert_shift_right(
      deque.begin() + 2, to_insert.begin(), to_insert.end());
  ASSERT_TRUE(it.has_value());
  EXPECT_EQ(deque.size(), 6u);
  EXPECT_EQ(deque[0], 3);
  EXPECT_EQ(deque[1], 4);
  EXPECT_EQ(deque[2], 99);
  EXPECT_EQ(deque[3], 5);
  EXPECT_EQ(deque[4], 6);
  EXPECT_EQ(deque[5], 7);
}

}  // namespace
