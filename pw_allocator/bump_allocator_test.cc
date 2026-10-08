// Copyright 2023 The Pigweed Authors
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

#include "pw_allocator/bump_allocator.h"

#include <cstring>

#include "lib/stdcompat/bit.h"
#include "pw_unit_test/framework.h"

namespace {

// Test fixtures.

using ::pw::allocator::BumpAllocator;
using ::pw::allocator::Layout;

class DestroyCounter final {
 public:
  DestroyCounter(size_t* counter) : counter_(counter) {}
  ~DestroyCounter() { *counter_ += 1; }

 private:
  size_t* counter_;
};

// Unit tests.

TEST(BumpAllocatorTest, ExplicitlyInit) {
  alignas(16) std::array<std::byte, 256> buffer;
  BumpAllocator allocator;
  allocator.Init(buffer);
}

TEST(BumpAllocatorTest, AllocateValid) {
  alignas(16) std::array<std::byte, 256> buffer;
  BumpAllocator allocator(buffer);
  void* ptr = allocator.Allocate(Layout(64, 16));
  ASSERT_NE(ptr, nullptr);
}

TEST(BumpAllocatorTest, AllocateAligned) {
  alignas(16) std::array<std::byte, 256> buffer;
  BumpAllocator allocator(buffer);
  void* ptr = allocator.Allocate(Layout(1, 1));
  ASSERT_NE(ptr, nullptr);

  // Last pointer was aligned, so next won't automatically be.
  ptr = allocator.Allocate(Layout(8, 32));
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(cpp20::bit_cast<uintptr_t>(ptr) % 32, 0U);
}

TEST(BumpAllocatorTest, AllocateFailsWhenExhausted) {
  alignas(16) std::array<std::byte, 256> buffer;
  BumpAllocator allocator(buffer);
  void* ptr = allocator.Allocate(Layout(256, 16));
  ASSERT_NE(ptr, nullptr);
  ptr = allocator.Allocate(Layout(1, 1));
  EXPECT_EQ(ptr, nullptr);
}

TEST(BumpAllocatorTest, DeallocateDoesNothing) {
  alignas(16) std::array<std::byte, 256> buffer;
  BumpAllocator allocator(buffer);
  void* ptr = allocator.Allocate(Layout(256, 16));
  ASSERT_NE(ptr, nullptr);
  allocator.Deallocate(ptr);
  ptr = allocator.Allocate(Layout(1, 1));
  EXPECT_EQ(ptr, nullptr);
}

TEST(BumpAllocatorTest, NewDoesNotDestroy) {
  alignas(16) std::array<std::byte, 256> buffer;
  size_t counter = 0;
  {
    BumpAllocator allocator(buffer);
    DestroyCounter* dc1 = allocator.New<DestroyCounter>(&counter);
    EXPECT_EQ(counter, 0U);
    allocator.Delete(dc1);
  }
  EXPECT_EQ(counter, 0U);
}

TEST(BumpAllocatorTest, DeleteDoesNothing) {
  alignas(16) std::array<std::byte, 256> buffer;
  size_t counter = 0;
  BumpAllocator allocator(buffer);
  DestroyCounter* dc1 = allocator.New<DestroyCounter>(&counter);
  EXPECT_EQ(counter, 0U);
  allocator.Delete(dc1);
  EXPECT_EQ(counter, 0U);
}

TEST(BumpAllocatorTest, NewOwnedDestroys) {
  alignas(16) std::array<std::byte, 256> buffer;
  size_t counter = 0;
  {
    BumpAllocator allocator(buffer);
    allocator.NewOwned<DestroyCounter>(&counter);
    EXPECT_EQ(counter, 0U);
  }
  EXPECT_EQ(counter, 1U);
}

TEST(BumpAllocatorTest, MakeUniqueDoesNotDestroy) {
  alignas(16) std::array<std::byte, 256> buffer;
  size_t counter = 0;
  {
    BumpAllocator allocator(buffer);
    allocator.MakeUnique<DestroyCounter>(&counter).get();
    EXPECT_EQ(counter, 0U);
  }
  EXPECT_EQ(counter, 0U);
}

TEST(BumpAllocatorTest, MakeUniqueOwnedDestroys) {
  alignas(16) std::array<std::byte, 256> buffer;
  size_t counter = 0;
  {
    BumpAllocator allocator(buffer);
    allocator.MakeUniqueOwned<DestroyCounter>(&counter).get();
    EXPECT_EQ(counter, 0U);
  }
  EXPECT_EQ(counter, 1U);
}

TEST(BumpAllocatorTest, MeasureFragmentation) {
  alignas(16) std::array<std::byte, 256> buffer;
  BumpAllocator allocator(buffer);
  void* ptr = allocator.Allocate(Layout(64, 16));
  ASSERT_NE(ptr, nullptr);

  auto fragmentation = allocator.MeasureFragmentation();
  ASSERT_TRUE(fragmentation.has_value());
  EXPECT_EQ(fragmentation->sum_of_squares.hi, 0U);
  EXPECT_EQ(fragmentation->sum_of_squares.lo, 192U * 192U);
  EXPECT_EQ(fragmentation->sum, 192U);
}

}  // namespace
