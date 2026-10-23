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

#include "pw_allocator/typed_pool.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "pw_allocator/allocator.h"
#include "pw_allocator/unique_ptr.h"
#include "pw_bytes/alignment.h"
#include "pw_unit_test/framework.h"

namespace {

// Test fixtures.

using ::pw::allocator::TypedPool;

class TypedPoolTest : public ::testing::Test {
 protected:
  struct U32 {
    std::byte bytes[4];
  };

  static constexpr size_t kNumU32s = 4;
  TypedPool<U32>::Buffer<kNumU32s> buffer_;
};

// Unit tests.

TEST_F(TypedPoolTest, LayoutNeeded) {
  EXPECT_EQ(TypedPool<std::byte[1]>::SizeNeeded(1), sizeof(void*));
  EXPECT_EQ(TypedPool<std::byte[16]>::SizeNeeded(1), 16U);

  EXPECT_EQ(TypedPool<std::byte[1]>::SizeNeeded(10), sizeof(void*) * 10);
  EXPECT_EQ(TypedPool<std::byte[16]>::SizeNeeded(10), 160U);

  EXPECT_EQ(TypedPool<std::byte[1]>::AlignmentNeeded(), alignof(void*));

  struct HighlyAligned {
    alignas(64) std::byte data[128];
  };
  EXPECT_EQ(TypedPool<HighlyAligned>::AlignmentNeeded(), 64U);
}

TEST_F(TypedPoolTest, AllocateDeallocate) {
  TypedPool<U32> allocator(buffer_);
  void* ptr = allocator.Allocate();
  ASSERT_NE(ptr, nullptr);
  allocator.Deallocate(ptr);
}

TEST_F(TypedPoolTest, NewDelete) {}
TEST_F(TypedPoolTest, NewDeleteBoundedArray) {}
TEST_F(TypedPoolTest, NewDeleteUnboundedArray) {}

TEST_F(TypedPoolTest, MakeUnique) {
  TypedPool<U32> allocator(buffer_);

  // Should be able allocate `kNumU32s`.
  std::array<pw::UniquePtr<U32>, kNumU32s> ptrs;
  for (size_t i = 0; i < kNumU32s; ++i) {
    ptrs[i] = allocator.MakeUnique();
    ASSERT_NE(ptrs[i], nullptr);
  }

  // Should be exhausted.
  EXPECT_EQ(allocator.MakeUnique(), nullptr);

  // Should be able to drop the ptrs and allocate `kNumU32s` more.
  for (size_t i = 0; i < kNumU32s; ++i) {
    ptrs[i] = pw::UniquePtr<U32>();
  }
  for (size_t i = 0; i < kNumU32s; ++i) {
    ptrs[i] = allocator.MakeUnique();
    ASSERT_NE(ptrs[i], nullptr);
  }
}

}  // namespace
