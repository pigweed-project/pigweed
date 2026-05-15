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

#include <array>
#include <cstdint>

#include "examples/named_u32.h"
#include "pw_allocator/first_fit.h"
#include "pw_unit_test/framework.h"

namespace examples {

std::array<std::byte, 0x1000> buffer;

// DOCSTAG: [pw_allocator-examples-block_allocator-poison]
pw::allocator::FirstFitAllocator<> allocator(buffer);
// DOCSTAG: [pw_allocator-examples-block_allocator-poison]

// DOCSTAG: [pw_allocator-examples-block_allocator-layout_of]
template <typename T, typename... Args>
T* my_new(Args... args) {
  void* ptr = allocator.Allocate(pw::allocator::Layout::Of<T>());
  return ptr == nullptr ? nullptr : new (ptr) T(std::forward<Args>(args)...);
}
// DOCSTAG: [pw_allocator-examples-block_allocator-layout_of]

template <typename T>
void my_delete(T* t) {
  if (t != nullptr) {
    std::destroy_at(t);
    allocator.Deallocate(t);
  }
}

// DOCSTAG: [pw_allocator-examples-block_allocator-malloc_free]
void* my_malloc(size_t size) {
  return allocator.Allocate(
      pw::allocator::Layout(size, alignof(std::max_align_t)));
}

void my_free(void* ptr) { allocator.Deallocate(ptr); }
// DOCSTAG: [pw_allocator-examples-block_allocator-malloc_free]

}  // namespace examples

namespace {

TEST(BlockAllocatorExample, NewDelete) {
  auto* named_u32 = examples::my_new<examples::NamedU32>("test", 111u);
  ASSERT_NE(named_u32, nullptr);
  EXPECT_STREQ(named_u32->name().data(), "test");
  EXPECT_EQ(named_u32->value(), 111U);
  examples::my_delete(named_u32);
}

TEST(BlockAllocatorExample, MallocFree) {
  void* ptr = examples::my_malloc(sizeof(examples::NamedU32));
  ASSERT_NE(ptr, nullptr);
  examples::my_free(ptr);
}

}  // namespace
