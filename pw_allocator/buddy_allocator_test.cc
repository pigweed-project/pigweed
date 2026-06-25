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

#include "pw_allocator/buddy_allocator.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "pw_allocator/fuzzing.h"
#include "pw_bytes/alignment.h"
#include "pw_unit_test/framework.h"

namespace {

// Test fixtures.

using BuddyAllocator = ::pw::allocator::BuddyAllocator<>;
using GenericBuddyAllocator = ::pw::allocator::internal::GenericBuddyAllocator;
using ::pw::allocator::Layout;

class TestBuddyAllocator : public BuddyAllocator {
 public:
  using BuddyAllocator::BuddyAllocator;
  using BuddyAllocator::GetAllocatedLayout;
};

static constexpr size_t kBufferSize = 0x400;

// Unit tests.

TEST(BuddyAllocatorTest, ExplicitlyInit) {
  std::array<std::byte, kBufferSize> buffer;
  BuddyAllocator allocator;
  allocator.Init(buffer);
}

TEST(BuddyAllocatorTest, AllocateSmall) {
  std::array<std::byte, kBufferSize> buffer;
  BuddyAllocator allocator(buffer);
  void* ptr = allocator.Allocate(Layout(BuddyAllocator::kMinOuterSize / 2, 1));
  ASSERT_NE(ptr, nullptr);
  allocator.Deallocate(ptr);
}

TEST(BuddyAllocatorTest, AllocateAllBlocks) {
  std::array<std::byte, kBufferSize> buffer;
  BuddyAllocator allocator(buffer);
  pw::Vector<void*, kBufferSize / BuddyAllocator::kMinOuterSize> ptrs;
  while (true) {
    void* ptr = allocator.Allocate(Layout(1, 1));
    if (ptr == nullptr) {
      break;
    }
    ptrs.push_back(ptr);
  }
  while (!ptrs.empty()) {
    allocator.Deallocate(ptrs.back());
    ptrs.pop_back();
  }
}

TEST(BuddyAllocatorTest, AllocateLarge) {
  std::array<std::byte, kBufferSize> buffer;
  BuddyAllocator allocator(buffer);
  void* ptr = allocator.Allocate(Layout(48, 1));
  ASSERT_NE(ptr, nullptr);
  allocator.Deallocate(ptr);
}

TEST(BuddyAllocatorTest, AllocateExcessiveSize) {
  std::array<std::byte, kBufferSize> buffer;
  BuddyAllocator allocator(buffer);
  void* ptr = allocator.Allocate(Layout(786, 1));
  EXPECT_EQ(ptr, nullptr);
}

TEST(BuddyAllocatorTest, AllocateExcessiveAlignment) {
  std::array<std::byte, kBufferSize> buffer;
  BuddyAllocator allocator(buffer);
  void* ptr = allocator.Allocate(Layout(48, 32));
  EXPECT_EQ(ptr, nullptr);
}

TEST(BuddyAllocatorTest, CoalesceLeftThenRightDeallocation) {
  std::array<std::byte, 1024> storage;
  pw::ByteSpan buffer(storage);
  buffer = pw::GetAlignedSubspan(buffer.subspan(1), 256);

  ::pw::allocator::BuddyAllocator<256, 2> buddy(buffer);

  // Allocate B1 (left) and B2 (right)
  void* p1 = buddy.Allocate(Layout(100, 8));
  void* p2 = buddy.Allocate(Layout(100, 8));

  ASSERT_NE(p1, nullptr);
  ASSERT_NE(p2, nullptr);

  // Free order: Left (p1) then Right (p2)
  buddy.Deallocate(p1);
  buddy.Deallocate(p2);

  // If they coalesced, we should be able to allocate 300 bytes (needs 512
  // block)
  void* p3 = buddy.Allocate(Layout(300, 8));
  EXPECT_NE(p3, nullptr);

  if (p3 != nullptr) {
    buddy.Deallocate(p3);
  }
}

TEST(BuddyAllocatorTest, CoalesceRightThenLeftDeallocation) {
  std::array<std::byte, 1024> storage;
  pw::ByteSpan buffer(storage);
  buffer = pw::GetAlignedSubspan(buffer.subspan(1), 256);

  ::pw::allocator::BuddyAllocator<256, 2> buddy(buffer);

  // Allocate B1 (left) and B2 (right)
  void* p1 = buddy.Allocate(Layout(100, 8));
  void* p2 = buddy.Allocate(Layout(100, 8));

  ASSERT_NE(p1, nullptr);
  ASSERT_NE(p2, nullptr);

  // Free order: Right (p2) then Left (p1)
  buddy.Deallocate(p2);
  buddy.Deallocate(p1);

  // If they coalesced, we should be able to allocate 300 bytes (needs 512
  // block)
  void* p3 = buddy.Allocate(Layout(300, 8));
  EXPECT_NE(p3, nullptr);

  if (p3 != nullptr) {
    buddy.Deallocate(p3);
  }
}

TEST(BuddyAllocatorTest, GetAllocatedLayout) {
  std::array<std::byte, kBufferSize> buffer;
  TestBuddyAllocator allocator(buffer);

  void* ptr = allocator.Allocate(Layout(14, 1));
  ASSERT_NE(ptr, nullptr);

  auto result = allocator.GetAllocatedLayout(ptr);
  EXPECT_TRUE(result.ok());

  if (result.ok()) {
    // kMinOuterSize is 16.
    // InnerSize is 16 - 1 = 15.
    EXPECT_EQ(result.value().size(), 15U);
  }

  // Test invalid pointer (outside region)
  auto invalid_result =
      allocator.GetAllocatedLayout(reinterpret_cast<void*>(0x1234));
  EXPECT_FALSE(invalid_result.ok());
  EXPECT_EQ(invalid_result.status(), pw::Status::OutOfRange());

  // Test unaligned pointer inside region
  void* unaligned_ptr = static_cast<std::byte*>(ptr) + 1;
  auto unaligned_result = allocator.GetAllocatedLayout(unaligned_ptr);
  EXPECT_FALSE(unaligned_result.ok());
  EXPECT_EQ(unaligned_result.status(), pw::Status::OutOfRange());

  allocator.Deallocate(ptr);
}

// Fuzz tests.

using ::pw::allocator::test::DefaultArbitraryRequests;
using ::pw::allocator::test::Request;
using ::pw::allocator::test::TestHarness;

void NeverCrashes(const pw::Vector<Request>& requests) {
  static std::array<std::byte, kBufferSize> buffer;
  static BuddyAllocator allocator(buffer);
  static TestHarness fuzzer(allocator);
  fuzzer.HandleRequests(requests);
}

FUZZ_TEST(BucketBlockAllocatorFuzzTest, NeverCrashes)
    .WithDomains(DefaultArbitraryRequests());

}  // namespace
