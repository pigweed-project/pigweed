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

#include "pw_allocator/unique_ptr.h"

#include <cstddef>

#include "pw_allocator/allocator.h"
#include "pw_allocator/internal/counter.h"
#include "pw_allocator/testing.h"
#include "pw_compilation_testing/negative_compilation.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::allocator::test::Counter;
using pw::allocator::test::CounterSink;
using pw::allocator::test::CounterWithBuffer;

class UniquePtrTest : public pw::allocator::test::TestWithCounters {
 protected:
  pw::allocator::test::AllocatorForTest<256> allocator_;
};

TEST_F(UniquePtrTest, DefaultInitializationIsNullptr) {
  pw::UniquePtr<int> empty;
  EXPECT_EQ(empty.get(), nullptr);
}

TEST_F(UniquePtrTest, OperatorEqNullptrOnEmptyUniquePtrSucceeds) {
  pw::UniquePtr<int> empty;
  EXPECT_TRUE(empty == nullptr);
  EXPECT_FALSE(empty != nullptr);
}

TEST_F(UniquePtrTest, AdoptValueViaConstructor) {
  {
    Counter* raw_ptr = allocator_.New<Counter>(5u);
    pw::UniquePtr<Counter> ptr(raw_ptr, allocator_);
    EXPECT_EQ(ptr->value(), 5u);
    EXPECT_EQ(ptr.deallocator(), &allocator_);
  }
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 1u);
  EXPECT_EQ(allocator_.deallocate_size(), sizeof(Counter));
}

TEST_F(UniquePtrTest, AdoptBoundedArrayViaConstructor) {
  {
    Counter* raw_ptr = allocator_.New<Counter[3]>();
    pw::UniquePtr<Counter[3]> ptr(raw_ptr, allocator_);
    EXPECT_EQ(ptr.size(), 3u);
    EXPECT_EQ(ptr.deallocator(), &allocator_);
  }
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 3u);
  EXPECT_EQ(allocator_.deallocate_size(), 3 * sizeof(Counter));
}

TEST_F(UniquePtrTest, AdoptUnboundedArrayViaConstructor) {
  {
    Counter* raw_ptr = allocator_.New<Counter[]>(5);
    pw::UniquePtr<Counter[]> ptr(raw_ptr, 5, allocator_);
    EXPECT_EQ(ptr.size(), 5u);
    EXPECT_EQ(ptr.deallocator(), &allocator_);
  }
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 5u);
  EXPECT_EQ(allocator_.deallocate_size(), 5 * sizeof(Counter));
}

TEST_F(UniquePtrTest, OperatorEqNullptrAfterMakeUniqueFails) {
  auto ptr = allocator_.MakeUnique<int>(5);
  EXPECT_TRUE(ptr != nullptr);
  EXPECT_FALSE(ptr == nullptr);
}

TEST_F(UniquePtrTest, OperatorEqNullptrAfterMakeUniqueNullptrTypeFails) {
  auto ptr = allocator_.MakeUnique<std::nullptr_t>(nullptr);
  EXPECT_TRUE(ptr != nullptr);
  EXPECT_FALSE(ptr == nullptr);
  EXPECT_TRUE(*ptr == nullptr);
  EXPECT_FALSE(*ptr != nullptr);
}

TEST_F(UniquePtrTest, MakeUniqueForwardsConstructorArguments) {
  Counter counter(6);
  auto ptr = allocator_.MakeUnique<CounterSink>(std::move(counter));
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(ptr->value(), 6u);
}

TEST_F(UniquePtrTest, MoveConstructsFromSubClassAndFreesTotalSize) {
  auto ptr = allocator_.MakeUnique<CounterWithBuffer>();
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(allocator_.allocate_size(), sizeof(CounterWithBuffer));
  pw::UniquePtr<Counter> base_ptr(std::move(ptr));

  EXPECT_EQ(allocator_.deallocate_size(), 0ul);
  // The size that is deallocated here should be the size of the larger
  // subclass, not the size of the smaller base class.
  base_ptr.Reset();
  EXPECT_EQ(allocator_.deallocate_size(), sizeof(CounterWithBuffer));
}

TEST_F(UniquePtrTest, MoveAssignsFromSubClassAndFreesTotalSize) {
  auto ptr = allocator_.MakeUnique<CounterWithBuffer>();
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(allocator_.allocate_size(), sizeof(CounterWithBuffer));
  pw::UniquePtr<Counter> base_ptr = std::move(ptr);

  EXPECT_EQ(allocator_.deallocate_size(), 0ul);
  // The size that is deallocated here should be the size of the larger
  // subclass, not the size of the smaller base class.
  base_ptr.Reset();
  EXPECT_EQ(allocator_.deallocate_size(), sizeof(CounterWithBuffer));
}

TEST_F(UniquePtrTest, MoveAssignsToExistingDeallocates) {
  auto size1 = allocator_.MakeUnique<size_t>(1u);
  ASSERT_NE(size1, nullptr);
  EXPECT_EQ(*size1, 1U);

  auto size2 = allocator_.MakeUnique<size_t>(2u);
  ASSERT_NE(size1, nullptr);
  EXPECT_EQ(*size2, 2U);

  EXPECT_EQ(allocator_.deallocate_size(), 0U);
  size1 = std::move(size2);
  EXPECT_EQ(allocator_.deallocate_size(), sizeof(size_t));
  EXPECT_EQ(*size1, 2U);
}

TEST_F(UniquePtrTest, DestructorDestroysAndFrees) {
  auto ptr = allocator_.MakeUnique<Counter>();
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 0u);
  EXPECT_EQ(allocator_.deallocate_size(), 0ul);

  ptr.Reset();  // Reset the UniquePtr, destroying its contents.
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 1u);
  EXPECT_EQ(allocator_.deallocate_size(), sizeof(Counter));
}

TEST_F(UniquePtrTest, ArrayElementsAreConstructed) {
  constexpr static size_t kArraySize = 5;

  // TODO(b/326509341): Remove when downstream consumers migrate.
  // Use the deprecated method...
  auto ptr1 = allocator_.MakeUniqueArray<Counter>(kArraySize);
  ASSERT_NE(ptr1, nullptr);
  EXPECT_EQ(Counter::TakeNumCtorCalls(), kArraySize);
  for (size_t i = 0; i < kArraySize; ++i) {
    EXPECT_EQ(ptr1[i].value(), i);
  }

  // ...and the supported method.
  auto ptr2 = allocator_.MakeUnique<Counter[]>(kArraySize);
  EXPECT_EQ(Counter::TakeNumCtorCalls(), kArraySize);
  ASSERT_NE(ptr2, nullptr);
  for (size_t i = 0; i < kArraySize; ++i) {
    EXPECT_EQ(ptr2[i].value(), i);
  }
}

TEST_F(UniquePtrTest, ArrayElementsAreConstructedWithSpecifiedAlignment) {
  constexpr static size_t kArraySize = 5;
  constexpr static size_t kArrayAlignment = 32;

  // TODO(b/326509341): Remove when downstream consumers migrate.
  // Use the deprecated method...
  auto ptr1 = allocator_.MakeUniqueArray<Counter>(kArraySize, kArrayAlignment);
  ASSERT_NE(ptr1, nullptr);
  EXPECT_EQ(Counter::TakeNumCtorCalls(), kArraySize);

  auto addr1 = reinterpret_cast<uintptr_t>(ptr1.get());
  EXPECT_EQ(addr1 % kArrayAlignment, 0u);

  // ...and the supported method.
  auto ptr2 = allocator_.MakeUnique<Counter[]>(kArraySize, kArrayAlignment);
  ASSERT_NE(ptr2, nullptr);
  EXPECT_EQ(Counter::TakeNumCtorCalls(), kArraySize);

  auto addr2 = reinterpret_cast<uintptr_t>(ptr2.get());
  EXPECT_EQ(addr2 % kArrayAlignment, 0u);
}

TEST_F(UniquePtrTest, DestructorDestroysAndFreesArray) {
  constexpr static size_t kArraySize = 5;

  pw::UniquePtr<Counter[]> ptr;
  EXPECT_EQ(ptr.size(), 0u);

  ptr = allocator_.MakeUnique<Counter[]>(kArraySize);
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(ptr.size(), kArraySize);
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 0u);
  EXPECT_EQ(allocator_.deallocate_size(), 0ul);

  ptr.Reset();  // Reset the UniquePtr, destroying its contents.
  EXPECT_EQ(ptr.size(), 0u);
  EXPECT_EQ(Counter::TakeNumDtorCalls(), kArraySize);
  EXPECT_EQ(allocator_.deallocate_size(), sizeof(Counter) * kArraySize);
}

TEST_F(UniquePtrTest, CanRelease) {
  constexpr static size_t kArraySize = 5;

  Counter* raw = nullptr;
  {
    auto ptr = allocator_.MakeUnique<Counter[]>(kArraySize);
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(ptr.deallocator(), &allocator_);
    raw = ptr.Release();

    // Allocator pointer parameter is optional. Re-releasing returns null.
    EXPECT_EQ(ptr.Release(), nullptr);
    EXPECT_EQ(ptr, nullptr);
    EXPECT_EQ(ptr.get(), nullptr);
    EXPECT_EQ(ptr.size(), 0u);
  }

  // Deallocate should not be called, even though UniquePtr goes out of scope.
  EXPECT_EQ(allocator_.deallocate_size(), 0U);
  allocator_.Delete<Counter[]>(raw, kArraySize);
  EXPECT_EQ(allocator_.deallocate_size(), sizeof(Counter) * kArraySize);
}

TEST_F(UniquePtrTest, SizeReturnsCorrectSize) {
  auto ptr_array = allocator_.MakeUnique<int[]>(5);
  EXPECT_EQ(ptr_array.size(), 5U);
}

TEST_F(UniquePtrTest, SizeReturnsCorrectSizeWhenAligned) {
  auto ptr_array = allocator_.MakeUnique<int[]>(5, 32);
  EXPECT_EQ(ptr_array.size(), 5U);
}

TEST_F(UniquePtrTest, CanSwapWhenNeitherAreEmpty) {
  auto ptr1 = allocator_.MakeUnique<Counter>(111u);
  auto ptr2 = allocator_.MakeUnique<Counter>(222u);
  ptr1.Swap(ptr2);
  EXPECT_EQ(ptr1->value(), 222u);
  EXPECT_EQ(ptr2->value(), 111u);
}

TEST_F(UniquePtrTest, CanSwapWhenOneIsEmpty) {
  auto ptr1 = allocator_.MakeUnique<Counter>(111u);
  pw::UniquePtr<Counter> ptr2;

  // ptr2 is empty.
  ptr1.Swap(ptr2);
  EXPECT_EQ(ptr2->value(), 111u);
  EXPECT_EQ(ptr1, nullptr);

  // ptr1 is empty.
  ptr1.Swap(ptr2);
  EXPECT_EQ(ptr1->value(), 111u);
  EXPECT_EQ(ptr2, nullptr);
}

TEST_F(UniquePtrTest, CanSwapWhenBothAreEmpty) {
  pw::UniquePtr<Counter> ptr1;
  pw::UniquePtr<Counter> ptr2;
  ptr1.Swap(ptr2);
  EXPECT_EQ(ptr1, nullptr);
  EXPECT_EQ(ptr2, nullptr);
}

// Verify that the UniquePtr implementation is the size of 2 pointers for the
// non-array case. This should not contain the size_t size_ parameter.
static_assert(
    sizeof(pw::UniquePtr<int>) == 2 * sizeof(void*),
    "size_ parameter must be disabled for non-array UniquePtr instances");

TEST_F(UniquePtrTest, Conversions) {
  struct Foo {
    int foo() const { return 1; }
  };
  struct Bar : public Foo {
    int bar() const { return 2; }
  };
  struct Baz : public Bar {
    int baz() const { return 3; }
  };

  pw::UniquePtr<Bar> bar = allocator_.MakeUnique<Baz>();
  auto* ptr = bar.get();

  // Upcast.
  pw::UniquePtr<Foo> foo = std::move(bar);
  EXPECT_EQ(ptr, foo.get());

  // Downcast.
  pw::UniquePtr<Baz> baz = static_cast<pw::UniquePtr<Baz>>(std::move(foo));
  EXPECT_EQ(baz->foo(), 1);
  EXPECT_EQ(baz->bar(), 2);
  EXPECT_EQ(baz->baz(), 3);
}

#if PW_NC_TEST(CannotCopyConstruct)
PW_NC_EXPECT_CLANG("call to deleted constructor");
PW_NC_EXPECT_GCC("use of deleted function");

TEST_F(UniquePtrTest, CannotCopyConstruct) {
  auto ptr1 = allocator_.MakeUnique<std::byte[]>(10);
  ASSERT_NE(ptr1, nullptr);
  pw::UniquePtr<std::byte[]> ptr2(ptr1);
  EXPECT_NE(ptr2.get(), nullptr);
}

#elif PW_NC_TEST(CannotCopyAssign)
PW_NC_EXPECT_CLANG("call to deleted constructor");
PW_NC_EXPECT_GCC("use of deleted function");

TEST_F(UniquePtrTest, CannotCopyAssign) {
  auto ptr1 = allocator_.MakeUnique<std::byte[]>(10);
  ASSERT_NE(ptr1, nullptr);
  pw::UniquePtr<std::byte[]> ptr2 = ptr1;
  EXPECT_NE(ptr2.get(), nullptr);
}

#elif PW_NC_TEST(CannotPassByValue)
PW_NC_EXPECT_CLANG("call to deleted constructor");
PW_NC_EXPECT_GCC("use of deleted function");

std::byte Checksum(pw::UniquePtr<std::byte[]> ptr) {
  uint8_t cksum = 0;
  for (size_t i = 0; i < ptr.size(); ++i) {
    cksum ^= static_cast<uint8_t>(ptr[i]);
  }
  return std::byte(cksum);
}

TEST_F(UniquePtrTest, CannotPassByValue) {
  auto ptr = allocator_.MakeUnique<std::byte[]>(4);
  ASSERT_NE(ptr, nullptr);
  ptr[0] = std::byte(0x88);
  ptr[1] = std::byte(0x44);
  ptr[2] = std::byte(0x22);
  ptr[3] = std::byte(0x11);
  EXPECT_EQ(Checksum(ptr), std::byte(0xFF));
}

#endif  // PW_NC_TEST

}  // namespace
