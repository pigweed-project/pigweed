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

#include "pw_allocator/freertos_allocator.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "pw_allocator/allocator.h"
#include "pw_allocator/layout.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::allocator::GetFreeRtosAllocator;
using ::pw::allocator::Layout;

TEST(FreeRtosAllocatorTest, AllocateDeallocate) {
  pw::Allocator& allocator = GetFreeRtosAllocator();
  constexpr Layout layout = Layout::Of<std::byte[64]>();
  void* ptr = allocator.Allocate(layout);
  ASSERT_NE(ptr, nullptr);
  // Check that the pointer can be dereferenced.
  memset(ptr, 0xAB, layout.size());
  allocator.Deallocate(ptr);
}

TEST(FreeRtosAllocatorTest, AllocatorHasGlobalLifetime) {
  void* ptr = nullptr;
  constexpr Layout layout = Layout::Of<std::byte[64]>();
  {
    ptr = GetFreeRtosAllocator().Allocate(layout);
    ASSERT_NE(ptr, nullptr);
  }
  // Check that the pointer can be dereferenced.
  {
    memset(ptr, 0xAB, layout.size());
    GetFreeRtosAllocator().Deallocate(ptr);
  }
}

TEST(FreeRtosAllocatorTest, AllocateLargeAlignment) {
  pw::Allocator& allocator = GetFreeRtosAllocator();
  // LibCAllocator has a maximum alignment of `std::align_max_t`.
  size_t size = 16;
  size_t alignment = alignof(std::max_align_t) * 2;
  void* ptr = allocator.Allocate(Layout(size, alignment));
  EXPECT_EQ(ptr, nullptr);
}

TEST(FreeRtosAllocatorTest, Reallocate) {
  pw::Allocator& allocator = GetFreeRtosAllocator();
  constexpr Layout old_layout = Layout::Of<uint32_t[4]>();
  void* ptr = allocator.Allocate(old_layout);
  ASSERT_NE(ptr, nullptr);
  constexpr Layout new_layout(sizeof(uint32_t[3]), old_layout.alignment());

  // Without a way to get the size of the previous allocation from the
  // allocation itself, `Rellocate` is expected to fail.
  EXPECT_EQ(allocator.Reallocate(ptr, new_layout), nullptr);
}

}  // namespace
