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

#include "pw_allocator/libc_allocator.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "pw_allocator/allocator.h"
#include "pw_allocator/layout.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::allocator::GetLibCAllocator;
using ::pw::allocator::Layout;

TEST(LibCAllocatorTest, AllocateDeallocate) {
  pw::Allocator& allocator = GetLibCAllocator();
  constexpr Layout layout = Layout::Of<std::byte[64]>();
  void* ptr = allocator.Allocate(layout);
  ASSERT_NE(ptr, nullptr);
  // Check that the pointer can be dereferenced.
  memset(ptr, 0xAB, layout.size());
  allocator.Deallocate(ptr);
}

TEST(LibCAllocatorTest, AllocatorHasGlobalLifetime) {
  void* ptr = nullptr;
  constexpr Layout layout = Layout::Of<std::byte[64]>();
  {
    ptr = GetLibCAllocator().Allocate(layout);
    ASSERT_NE(ptr, nullptr);
  }
  // Check that the pointer can be dereferenced.
  {
    memset(ptr, 0xAB, layout.size());
    GetLibCAllocator().Deallocate(ptr);
  }
}

TEST(LibCAllocatorTest, AllocateLargeAlignment) {
  pw::Allocator& allocator = GetLibCAllocator();
  // LibCAllocator has a maximum alignment of `std::align_max_t`.
  size_t size = 16;
  size_t alignment = alignof(std::max_align_t) * 2;
  void* ptr = allocator.Allocate(Layout(size, alignment));
  EXPECT_EQ(ptr, nullptr);
}

TEST(LibCAllocatorTest, Reallocate) {
  pw::Allocator& allocator = GetLibCAllocator();
  constexpr Layout old_layout = Layout::Of<uint32_t[4]>();
  void* ptr = allocator.Allocate(old_layout);
  ASSERT_NE(ptr, nullptr);
  constexpr Layout new_layout(sizeof(uint32_t[3]), old_layout.alignment());
  void* new_ptr = allocator.Reallocate(ptr, new_layout);
  ASSERT_NE(new_ptr, nullptr);
  allocator.Deallocate(new_ptr);
}

}  // namespace
