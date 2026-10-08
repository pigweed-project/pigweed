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

#include "pw_allocator/fallback_allocator.h"

#include "pw_allocator/testing.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::allocator::FallbackAllocator;
using ::pw::allocator::Layout;
using ::pw::allocator::test::AllocatorForTest;

class FallbackAllocatorForTest : public FallbackAllocator {
 public:
  using FallbackAllocator::FallbackAllocator;

  using FallbackAllocator::GetAllocatedLayout;
  using FallbackAllocator::GetRequestedLayout;
  using FallbackAllocator::GetUsableLayout;
  using FallbackAllocator::Recognizes;
};

// Test fixtures.

class FallbackAllocatorTest : public ::testing::Test {
 protected:
  constexpr static size_t kCapacity = 256;
  static_assert(sizeof(uintptr_t) >= AllocatorForTest<kCapacity>::kMinSize);

  FallbackAllocatorTest() : allocator_(primary_, secondary_) {}

  AllocatorForTest<kCapacity> primary_;
  AllocatorForTest<kCapacity> secondary_;
  FallbackAllocatorForTest allocator_;
};

// Unit tests.

TEST_F(FallbackAllocatorTest, GetCapacity) {
  pw::StatusWithSize capacity = allocator_.GetCapacity();
  EXPECT_EQ(capacity.status(), pw::OkStatus());
  EXPECT_EQ(capacity.size(), 2 * kCapacity);
}

TEST_F(FallbackAllocatorTest, AllocateFromPrimary) {
  Layout layout = Layout::Of<uintptr_t>();
  void* ptr = allocator_.Allocate(layout);
  EXPECT_NE(ptr, nullptr);
  EXPECT_EQ(primary_.allocate_size(), layout.size());
  EXPECT_EQ(secondary_.allocate_size(), 0U);
}

TEST_F(FallbackAllocatorTest, AllocateFromSecondary) {
  primary_.DisableAllocate();  // Exhaust memory
  Layout layout = Layout::Of<uintptr_t>();
  void* ptr = allocator_.Allocate(layout);
  EXPECT_NE(ptr, nullptr);
  EXPECT_EQ(primary_.allocate_size(), layout.size());
  EXPECT_EQ(secondary_.allocate_size(), layout.size());
}

TEST_F(FallbackAllocatorTest, AllocateFailure) {
  Layout layout = Layout::Of<uintptr_t[0x10000]>();
  void* ptr = allocator_.Allocate(layout);
  EXPECT_EQ(ptr, nullptr);
  EXPECT_EQ(primary_.allocate_size(), layout.size());
  EXPECT_EQ(secondary_.allocate_size(), layout.size());
}

TEST_F(FallbackAllocatorTest, DeallocateUsingPrimary) {
  Layout layout = Layout::Of<uintptr_t>();
  void* ptr = allocator_.Allocate(layout);
  ASSERT_NE(ptr, nullptr);
  allocator_.Deallocate(ptr);
  EXPECT_EQ(primary_.deallocate_ptr(), ptr);
  EXPECT_EQ(primary_.deallocate_size(), layout.size());
  EXPECT_EQ(secondary_.deallocate_ptr(), nullptr);
  EXPECT_EQ(secondary_.deallocate_size(), 0U);
}

TEST_F(FallbackAllocatorTest, DeallocateUsingSecondary) {
  primary_.DisableAllocate();  // Exhaust memory
  Layout layout = Layout::Of<uintptr_t>();
  void* ptr = allocator_.Allocate(layout);
  ASSERT_NE(ptr, nullptr);
  allocator_.Deallocate(ptr);
  EXPECT_EQ(primary_.deallocate_ptr(), nullptr);
  EXPECT_EQ(primary_.deallocate_size(), 0U);
  EXPECT_EQ(secondary_.deallocate_ptr(), ptr);
  EXPECT_EQ(secondary_.deallocate_size(), layout.size());
}

TEST_F(FallbackAllocatorTest, ResizePrimary) {
  Layout old_layout = Layout::Of<uintptr_t>();
  void* ptr = allocator_.Allocate(old_layout);
  ASSERT_NE(ptr, nullptr);

  size_t new_size = sizeof(uintptr_t[3]);
  EXPECT_TRUE(allocator_.Resize(ptr, new_size));
  EXPECT_EQ(primary_.resize_ptr(), ptr);
  EXPECT_EQ(primary_.resize_old_size(), old_layout.size());
  EXPECT_EQ(primary_.resize_new_size(), new_size);

  // Secondary should not be touched.
  EXPECT_EQ(secondary_.resize_ptr(), nullptr);
  EXPECT_EQ(secondary_.resize_old_size(), 0U);
  EXPECT_EQ(secondary_.resize_new_size(), 0U);
}

TEST_F(FallbackAllocatorTest, ResizePrimaryFailure) {
  Layout old_layout = Layout::Of<uintptr_t>();
  void* ptr = allocator_.Allocate(old_layout);
  ASSERT_NE(ptr, nullptr);
  primary_.DisableAllocate();  // Exhaust memory

  EXPECT_FALSE(allocator_.Resize(ptr, kCapacity));
  EXPECT_EQ(primary_.resize_ptr(), ptr);
  EXPECT_EQ(primary_.resize_old_size(), old_layout.size());
  EXPECT_EQ(primary_.resize_new_size(), kCapacity);

  // Secondary should not be touched.
  EXPECT_EQ(secondary_.resize_ptr(), nullptr);
  EXPECT_EQ(secondary_.resize_old_size(), 0U);
  EXPECT_EQ(secondary_.resize_new_size(), 0U);
}

TEST_F(FallbackAllocatorTest, ResizeSecondary) {
  primary_.DisableAllocate();  // Exhaust memory
  Layout old_layout = Layout::Of<uintptr_t>();
  void* ptr = allocator_.Allocate(old_layout);
  ASSERT_NE(ptr, nullptr);

  size_t new_size = sizeof(uintptr_t[3]);
  EXPECT_TRUE(allocator_.Resize(ptr, new_size));
  EXPECT_EQ(secondary_.resize_ptr(), ptr);
  EXPECT_EQ(secondary_.resize_old_size(), old_layout.size());
  EXPECT_EQ(secondary_.resize_new_size(), new_size);

  // Primary should not be touched.
  EXPECT_EQ(primary_.resize_ptr(), nullptr);
  EXPECT_EQ(primary_.resize_old_size(), 0U);
  EXPECT_EQ(primary_.resize_new_size(), 0U);
}

TEST_F(FallbackAllocatorTest, ResizeSecondaryFailure) {
  primary_.DisableAllocate();  // Exhaust memory
  Layout old_layout = Layout::Of<uintptr_t>();
  void* ptr = allocator_.Allocate(old_layout);
  ASSERT_NE(ptr, nullptr);
  secondary_.DisableAllocate();  // Exhaust memory

  EXPECT_FALSE(allocator_.Resize(ptr, kCapacity));
  EXPECT_EQ(secondary_.resize_ptr(), ptr);
  EXPECT_EQ(secondary_.resize_old_size(), old_layout.size());
  EXPECT_EQ(secondary_.resize_new_size(), kCapacity);

  // Primary should not be touched.
  EXPECT_EQ(primary_.resize_ptr(), nullptr);
  EXPECT_EQ(primary_.resize_old_size(), 0U);
  EXPECT_EQ(primary_.resize_new_size(), 0U);
}

TEST_F(FallbackAllocatorTest, ReallocateSameAllocator) {
  Layout old_layout = Layout::Of<uintptr_t>();
  void* ptr = allocator_.Allocate(old_layout);
  ASSERT_NE(ptr, nullptr);

  primary_.DisableResize();  // Force reallocation.
  Layout new_layout = Layout::Of<uintptr_t[3]>();
  void* new_ptr = allocator_.Reallocate(ptr, new_layout);
  EXPECT_NE(new_ptr, nullptr);
  EXPECT_EQ(primary_.deallocate_ptr(), ptr);
  EXPECT_EQ(primary_.deallocate_size(), old_layout.size());
  EXPECT_EQ(primary_.allocate_size(), new_layout.size());
}

TEST_F(FallbackAllocatorTest, ReallocateDifferentAllocator) {
  Layout old_layout = Layout::Of<uintptr_t>();
  void* ptr = allocator_.Allocate(old_layout);
  primary_.DisableAllocate();  // Exhaust memory
  primary_.DisableResize();    // Force reallocation.

  Layout new_layout = Layout::Of<uintptr_t[3]>();
  void* new_ptr = allocator_.Reallocate(ptr, new_layout);
  EXPECT_NE(new_ptr, nullptr);
  EXPECT_EQ(primary_.deallocate_ptr(), ptr);
  EXPECT_EQ(primary_.deallocate_size(), old_layout.size());
  EXPECT_EQ(secondary_.allocate_size(), new_layout.size());
}

TEST_F(FallbackAllocatorTest, GetRequestedLayout_Primary) {
  Layout layout = Layout::Of<uintptr_t>();
  void* ptr = allocator_.Allocate(layout);
  ASSERT_NE(ptr, nullptr);
  auto result = allocator_.GetRequestedLayout(ptr);
  EXPECT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(result->size(), layout.size());
}

TEST_F(FallbackAllocatorTest, GetRequestedLayout_Secondary) {
  primary_.DisableAllocate();  // Exhaust memory
  Layout layout = Layout::Of<uintptr_t>();
  void* ptr = allocator_.Allocate(layout);
  ASSERT_NE(ptr, nullptr);
  auto result = allocator_.GetRequestedLayout(ptr);
  EXPECT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(result->size(), layout.size());
}

TEST_F(FallbackAllocatorTest, GetRequestedLayout_None) {
  void* ptr = &allocator_;
  auto result = allocator_.GetRequestedLayout(ptr);
  EXPECT_EQ(result.status(), pw::Status::NotFound());
}

TEST_F(FallbackAllocatorTest, GetUsableLayout_Primary) {
  Layout layout = Layout::Of<uintptr_t>();
  void* ptr = allocator_.Allocate(layout);
  ASSERT_NE(ptr, nullptr);
  auto result = allocator_.GetUsableLayout(ptr);
  EXPECT_EQ(result.status(), pw::OkStatus());
  EXPECT_GE(result->size(), layout.size());
}

TEST_F(FallbackAllocatorTest, GetUsableLayout_Secondary) {
  primary_.DisableAllocate();  // Exhaust memory
  Layout layout = Layout::Of<uintptr_t>();
  void* ptr = allocator_.Allocate(layout);
  ASSERT_NE(ptr, nullptr);
  auto result = allocator_.GetUsableLayout(ptr);
  EXPECT_EQ(result.status(), pw::OkStatus());
  EXPECT_GE(result->size(), layout.size());
}

TEST_F(FallbackAllocatorTest, GetUsableLayout_None) {
  void* ptr = &allocator_;
  auto result = allocator_.GetUsableLayout(ptr);
  EXPECT_EQ(result.status(), pw::Status::NotFound());
}

TEST_F(FallbackAllocatorTest, GetAllocatedLayout_Primary) {
  Layout layout = Layout::Of<uintptr_t>();
  void* ptr = allocator_.Allocate(layout);
  ASSERT_NE(ptr, nullptr);
  auto result = allocator_.GetAllocatedLayout(ptr);
  EXPECT_EQ(result.status(), pw::OkStatus());
  EXPECT_GE(result->size(), layout.size());
}

TEST_F(FallbackAllocatorTest, GetAllocatedLayout_Secondary) {
  primary_.DisableAllocate();  // Exhaust memory
  Layout layout = Layout::Of<uintptr_t>();
  void* ptr = allocator_.Allocate(layout);
  ASSERT_NE(ptr, nullptr);
  auto result = allocator_.GetAllocatedLayout(ptr);
  EXPECT_EQ(result.status(), pw::OkStatus());
  EXPECT_GE(result->size(), layout.size());
}

TEST_F(FallbackAllocatorTest, GetAllocatedLayout_None) {
  void* ptr = &allocator_;
  auto result = allocator_.GetAllocatedLayout(ptr);
  EXPECT_EQ(result.status(), pw::Status::NotFound());
}

TEST_F(FallbackAllocatorTest, GetCapacity_ReturnsSum) {
  pw::StatusWithSize capacity = allocator_.GetCapacity();
  EXPECT_EQ(capacity.status(), pw::OkStatus());
  EXPECT_EQ(capacity.size(), 2 * kCapacity);
}

TEST_F(FallbackAllocatorTest, Recognizes_Primary) {
  Layout layout = Layout::Of<uintptr_t>();
  void* ptr = allocator_.Allocate(layout);
  ASSERT_NE(ptr, nullptr);
  EXPECT_TRUE(allocator_.Recognizes(ptr));
}

TEST_F(FallbackAllocatorTest, Recognizes_Secondary) {
  primary_.DisableAllocate();  // Exhaust memory
  Layout layout = Layout::Of<uintptr_t>();
  void* ptr = allocator_.Allocate(layout);
  ASSERT_NE(ptr, nullptr);
  EXPECT_TRUE(allocator_.Recognizes(ptr));
}

TEST_F(FallbackAllocatorTest, Recognizes_None) {
  void* ptr = &allocator_;
  EXPECT_FALSE(allocator_.Recognizes(ptr));
}

TEST_F(FallbackAllocatorTest, MeasureFragmentation_Both) {
  primary_.SetFragmentation({.sum_of_squares = {.hi = 1, .lo = 2}, .sum = 3});
  secondary_.SetFragmentation(
      {.sum_of_squares = {.hi = 4, .lo = 8}, .sum = 12});

  auto result = allocator_.MeasureFragmentation();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->sum_of_squares.hi, 5U);
  EXPECT_EQ(result->sum_of_squares.lo, 10U);
  EXPECT_EQ(result->sum, 15U);
}

TEST_F(FallbackAllocatorTest, MeasureFragmentation_OnlyPrimary) {
  primary_.SetFragmentation({.sum_of_squares = {.hi = 1, .lo = 2}, .sum = 3});
  secondary_.DisableMeasureFragmentation();

  auto result = allocator_.MeasureFragmentation();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->sum_of_squares.hi, 1U);
  EXPECT_EQ(result->sum_of_squares.lo, 2U);
  EXPECT_EQ(result->sum, 3U);
}

TEST_F(FallbackAllocatorTest, MeasureFragmentation_OnlySecondary) {
  primary_.DisableMeasureFragmentation();
  secondary_.SetFragmentation(
      {.sum_of_squares = {.hi = 4, .lo = 8}, .sum = 12});

  auto result = allocator_.MeasureFragmentation();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->sum_of_squares.hi, 4U);
  EXPECT_EQ(result->sum_of_squares.lo, 8U);
  EXPECT_EQ(result->sum, 12U);
}

TEST_F(FallbackAllocatorTest, MeasureFragmentation_None) {
  primary_.DisableMeasureFragmentation();
  secondary_.DisableMeasureFragmentation();
  EXPECT_FALSE(allocator_.MeasureFragmentation().has_value());
}

}  // namespace
