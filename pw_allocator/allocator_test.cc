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

#include "pw_allocator/allocator.h"

#include <cstddef>

#include "pw_allocator/capability.h"
#include "pw_allocator/internal/counter.h"
#include "pw_allocator/testing.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::allocator::Capability;
using ::pw::allocator::Layout;
using ::pw::allocator::test::Counter;

class AllocatorTest : public ::pw::allocator::test::TestWithCounters {
 protected:
  using AllocatorType = ::pw::allocator::test::AllocatorForTest<256>;
  using BlockType = typename AllocatorType::BlockType;

  static_assert(sizeof(uintptr_t) == BlockType::kAlignment);

  AllocatorType allocator_;
};

TEST_F(AllocatorTest, HasFlags) {
  EXPECT_TRUE(
      allocator_.HasCapability(Capability::kImplementsGetRequestedLayout));
  EXPECT_TRUE(allocator_.HasCapability(Capability::kImplementsGetUsableLayout));
}

TEST_F(AllocatorTest, NewAndDelete) {
  auto* counter = allocator_.New<Counter>(2u);
  ASSERT_NE(counter, nullptr);
  EXPECT_EQ(counter->value(), 2u);
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 0u);
  allocator_.Delete(counter);
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 1u);
}

class VirtualCounter : public Counter {
 public:
  virtual ~VirtualCounter() { EXPECT_TRUE(must_be_set_by_derived_destructor_); }

 protected:
  explicit VirtualCounter(size_t value) : Counter(value) {}

  bool must_be_set_by_derived_destructor_ = false;
};

class DerivedVirtualCounter : public VirtualCounter {
 public:
  explicit DerivedVirtualCounter(size_t value) : VirtualCounter(value) {}

  ~DerivedVirtualCounter() override {
    must_be_set_by_derived_destructor_ = filler_bools[0];
  }

 private:
  // Add an array of bools to increase the size of the object. Use the first
  // value in the destructor so the array is used.
  bool filler_bools[12] = {true};
};

static_assert(sizeof(DerivedVirtualCounter) != sizeof(VirtualCounter),
              "Test should cover deleting an object with a different layout");

TEST_F(AllocatorTest, NewAndDeleteVirtualDestructor) {
  // Assign the DerivedVirtualCounter* to its base VirtualCounter*.
  VirtualCounter* counter = allocator_.New<DerivedVirtualCounter>(2u);
  ASSERT_NE(counter, nullptr);
  EXPECT_EQ(counter->value(), 2u);
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 0u);
  allocator_.Delete(counter);
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 1u);
}

TEST_F(AllocatorTest, NewAndDeleteBoundedArray) {
  auto* counters = allocator_.New<Counter[3]>();
  ASSERT_NE(counters, nullptr);
  allocator_.Delete<Counter[3]>(counters);
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 3u);
  EXPECT_EQ(allocator_.deallocate_size(), 3 * sizeof(Counter));
}

TEST_F(AllocatorTest, NewAndDeleteUnboundedArray) {
  auto* counters = allocator_.New<Counter[]>(5);
  ASSERT_NE(counters, nullptr);
  allocator_.Delete<Counter[]>(counters, 5);
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 5u);
  EXPECT_EQ(allocator_.deallocate_size(), 5 * sizeof(Counter));
}

TEST_F(AllocatorTest, NewAndDeleteArray) {
  auto* counters = allocator_.New<Counter[3]>();
  ASSERT_NE(counters, nullptr);
  allocator_.DeleteArray(counters, 3);
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 3u);
  EXPECT_EQ(allocator_.deallocate_size(), 3 * sizeof(Counter));
}

TEST_F(AllocatorTest, ResizeNull) {
  EXPECT_FALSE(allocator_.Resize(nullptr, sizeof(uintptr_t)));
}

TEST_F(AllocatorTest, ResizeZero) {
  constexpr Layout layout = Layout::Of<uintptr_t>();
  void* ptr = allocator_.Allocate(layout);
  ASSERT_NE(ptr, nullptr);
  EXPECT_FALSE(allocator_.Resize(ptr, 0));
}

TEST_F(AllocatorTest, ResizeSame) {
  constexpr Layout layout = Layout::Of<uintptr_t>();
  void* ptr = allocator_.Allocate(layout);
  ASSERT_NE(ptr, nullptr);
  EXPECT_TRUE(allocator_.Resize(ptr, layout.size()));
  EXPECT_EQ(allocator_.resize_ptr(), ptr);
  EXPECT_EQ(allocator_.resize_old_size(), layout.size());
  EXPECT_EQ(allocator_.resize_new_size(), layout.size());
}

TEST_F(AllocatorTest, ReallocateNull) {
  constexpr Layout old_layout = Layout::Of<uintptr_t>();
  constexpr Layout new_layout(old_layout.size(), old_layout.alignment());
  void* new_ptr = allocator_.Reallocate(nullptr, new_layout);

  // Resize should fail and Reallocate should call Allocate.
  EXPECT_EQ(allocator_.allocate_size(), new_layout.size());

  // Deallocate should not be called.
  EXPECT_EQ(allocator_.deallocate_ptr(), nullptr);
  EXPECT_EQ(allocator_.deallocate_size(), 0U);

  // Overall, Reallocate should succeed.
  EXPECT_NE(new_ptr, nullptr);
}

TEST_F(AllocatorTest, ReallocateZeroNewSize) {
  constexpr Layout old_layout = Layout::Of<uintptr_t[3]>();
  void* ptr = allocator_.Allocate(old_layout);
  ASSERT_EQ(allocator_.allocate_size(), old_layout.size());
  ASSERT_NE(ptr, nullptr);
  allocator_.ResetParameters();

  constexpr Layout new_layout(0, old_layout.alignment());
  void* new_ptr = allocator_.Reallocate(ptr, new_layout);

  // Reallocate does not call Resize, Allocate, or Deallocate.
  EXPECT_EQ(allocator_.resize_ptr(), nullptr);
  EXPECT_EQ(allocator_.resize_old_size(), 0U);
  EXPECT_EQ(allocator_.resize_new_size(), 0U);
  EXPECT_EQ(allocator_.allocate_size(), 0U);
  EXPECT_EQ(allocator_.deallocate_ptr(), nullptr);
  EXPECT_EQ(allocator_.deallocate_size(), 0U);

  // Overall, Reallocate should fail.
  EXPECT_EQ(new_ptr, nullptr);
}

TEST_F(AllocatorTest, ReallocateSame) {
  constexpr Layout layout = Layout::Of<uintptr_t[3]>();
  void* ptr = allocator_.Allocate(layout);
  ASSERT_EQ(allocator_.allocate_size(), layout.size());
  ASSERT_NE(ptr, nullptr);
  allocator_.ResetParameters();

  void* new_ptr = allocator_.Reallocate(ptr, layout);

  // Reallocate should call Resize.
  EXPECT_EQ(allocator_.resize_ptr(), ptr);
  EXPECT_EQ(allocator_.resize_old_size(), layout.size());
  EXPECT_EQ(allocator_.resize_new_size(), layout.size());

  // DoAllocate should not be called.
  EXPECT_EQ(allocator_.allocate_size(), 0U);

  // DoDeallocate should not be called.
  EXPECT_EQ(allocator_.deallocate_ptr(), nullptr);
  EXPECT_EQ(allocator_.deallocate_size(), 0U);

  // Overall, Reallocate should succeed.
  EXPECT_EQ(new_ptr, ptr);
}

TEST_F(AllocatorTest, ReallocateSmaller) {
  constexpr Layout old_layout = Layout::Of<uintptr_t[3]>();
  void* ptr = allocator_.Allocate(old_layout);
  ASSERT_EQ(allocator_.allocate_size(), old_layout.size());
  ASSERT_NE(ptr, nullptr);
  allocator_.ResetParameters();

  constexpr Layout new_layout(sizeof(uintptr_t), old_layout.alignment());
  void* new_ptr = allocator_.Reallocate(ptr, new_layout);

  // Reallocate should call Resize.
  EXPECT_EQ(allocator_.resize_ptr(), ptr);
  EXPECT_EQ(allocator_.resize_old_size(), old_layout.size());
  EXPECT_EQ(allocator_.resize_new_size(), new_layout.size());

  // Allocate should not be called.
  EXPECT_EQ(allocator_.allocate_size(), 0U);

  // Deallocate should not be called.
  EXPECT_EQ(allocator_.deallocate_ptr(), nullptr);
  EXPECT_EQ(allocator_.deallocate_size(), 0U);

  // Overall, Reallocate should succeed.
  EXPECT_EQ(new_ptr, ptr);
}

TEST_F(AllocatorTest, ReallocateLarger) {
  constexpr Layout old_layout = Layout::Of<uintptr_t>();
  void* ptr = allocator_.Allocate(old_layout);
  ASSERT_EQ(allocator_.allocate_size(), old_layout.size());
  ASSERT_NE(ptr, nullptr);

  // The abstraction is a bit leaky here: This tests relies on the details of
  // `Resize` in order to get it to fail and fallback to
  // allocate/copy/deallocate. Allocate a second block, which should prevent the
  // first one from being able to grow.
  void* next = allocator_.Allocate(old_layout);
  ASSERT_EQ(allocator_.allocate_size(), old_layout.size());
  ASSERT_NE(next, nullptr);
  allocator_.ResetParameters();

  constexpr Layout new_layout(sizeof(uintptr_t[3]), old_layout.alignment());
  void* new_ptr = allocator_.Reallocate(ptr, new_layout);

  // Reallocate should call Resize.
  EXPECT_EQ(allocator_.resize_ptr(), ptr);
  EXPECT_EQ(allocator_.resize_old_size(), old_layout.size());
  EXPECT_EQ(allocator_.resize_new_size(), new_layout.size());

  // Resize should fail and Reallocate should call Allocate.
  EXPECT_EQ(allocator_.allocate_size(), new_layout.size());

  // Deallocate should also be called.
  EXPECT_EQ(allocator_.deallocate_ptr(), ptr);
  EXPECT_EQ(allocator_.deallocate_size(), old_layout.size());

  // Overall, Reallocate should succeed.
  EXPECT_NE(new_ptr, nullptr);
  EXPECT_NE(new_ptr, ptr);
}

// Test fixture for IsEqual tests.
class BaseAllocator : public pw::Allocator {
 public:
  BaseAllocator(void* ptr) : pw::Allocator(Capabilities()), ptr_(ptr) {}

 private:
  void* DoAllocate(Layout) override {
    void* ptr = ptr_;
    ptr_ = nullptr;
    return ptr;
  }

  void DoDeallocate(void*) override {}

  bool DoResize(void*, size_t) override { return false; }

  void* ptr_;
};

// Test fixture for IsEqual tests.
class DerivedAllocator : public BaseAllocator {
 public:
  DerivedAllocator(size_t value, void* ptr)
      : BaseAllocator(ptr), value_(value) {}
  size_t value() const { return value_; }

 private:
  size_t value_;
};

TEST_F(AllocatorTest, IsEqualFailsWithDifferentObjects) {
  std::array<std::byte, 8> buffer;
  DerivedAllocator derived1(1, buffer.data());
  DerivedAllocator derived2(2, buffer.data());
  EXPECT_FALSE(derived1.IsEqual(derived2));
  EXPECT_FALSE(derived2.IsEqual(derived1));
}

TEST_F(AllocatorTest, IsEqualSucceedsWithSameObject) {
  std::array<std::byte, 8> buffer;
  DerivedAllocator derived(1, buffer.data());
  BaseAllocator* base = &derived;
  EXPECT_TRUE(derived.IsEqual(*base));
  EXPECT_TRUE(base->IsEqual(derived));
}

}  // namespace
