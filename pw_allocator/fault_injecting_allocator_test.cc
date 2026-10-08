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

#include "pw_allocator/fault_injecting_allocator.h"

#include "pw_allocator/layout.h"
#include "pw_allocator/testing.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::allocator::Fragmentation;
using ::pw::allocator::Layout;

constexpr size_t kTestBufferSize = 128;

constexpr Layout kSmallLayout = Layout::Of<int>();
constexpr Layout kLargeLayout = Layout::Of<long long>();

class FaultInjectingAllocatorTest : public testing::Test {
 protected:
  // AllocatorForTest is a FaultInjectingAllocator.
  pw::allocator::test::AllocatorForTest<kTestBufferSize> allocator_;
};

TEST_F(FaultInjectingAllocatorTest, AllocateEnableDisable) {
  allocator_.DisableAllocate();

  void* ptr = allocator_.Allocate(kSmallLayout);
  EXPECT_EQ(ptr, nullptr);

  allocator_.EnableAllocate();
  ptr = allocator_.Allocate(kSmallLayout);
  EXPECT_NE(ptr, nullptr);

  allocator_.Deallocate(ptr);
}

TEST_F(FaultInjectingAllocatorTest, ResizeEnableDisable) {
  allocator_.EnableAllocate();
  void* ptr =
      allocator_.Allocate(kLargeLayout);  // Allocate a larger block initially
  ASSERT_NE(ptr, nullptr);  // Stop test if initial allocation fails

  allocator_.DisableResize();
  EXPECT_FALSE(allocator_.Resize(ptr, 1));

  allocator_.EnableResize();
  EXPECT_TRUE(allocator_.Resize(ptr, 1));

  allocator_.Deallocate(ptr);
}

TEST_F(FaultInjectingAllocatorTest, ReallocateEnableDisable) {
  allocator_.EnableAllocate();
  void* original_ptr = allocator_.Allocate(kSmallLayout);
  ASSERT_NE(original_ptr, nullptr);  // Stop test if initial allocation fails

  allocator_.DisableReallocate();

  void* reallocated_ptr = allocator_.Reallocate(original_ptr, kLargeLayout);
  EXPECT_EQ(reallocated_ptr, nullptr);

  allocator_.EnableReallocate();

  reallocated_ptr = allocator_.Reallocate(original_ptr, kLargeLayout);
  EXPECT_NE(reallocated_ptr, nullptr);

  if (reallocated_ptr != nullptr) {
    allocator_.Deallocate(reallocated_ptr);
  } else {
    allocator_.Deallocate(original_ptr);
  }
}

TEST_F(FaultInjectingAllocatorTest, MeasureFragmentationEnableDisable) {
  Fragmentation fragmentation = {
      .sum_of_squares = {.hi = 0x11223344, .lo = 0x55667788},
      .sum = 0x99AABBCC,
  };
  allocator_.SetFragmentation(fragmentation);

  allocator_.DisableMeasureFragmentation();
  EXPECT_EQ(allocator_.MeasureFragmentation(), std::nullopt);

  allocator_.EnableMeasureFragmentation();
  EXPECT_EQ(allocator_.MeasureFragmentation(), fragmentation);
}

}  // namespace
