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

#include "pw_buf/buf.h"

#include <array>

#include "pw_allocator/null_allocator.h"
#include "pw_allocator/testing.h"
#include "pw_allocator/unique_ptr.h"
#include "pw_bytes/span.h"
#include "pw_unit_test/framework.h"

namespace pw {
namespace {

// Test fixture for shared allocator setup.
class BufTest : public ::testing::Test {
 protected:
  allocator::test::AllocatorForTest<256> test_allocator_;
};

// ConstBuf tests

TEST_F(BufTest, ConstBufDefaultConstructor) {
  ConstBuf const_buf;
  EXPECT_TRUE(const_buf.empty());
  EXPECT_EQ(const_buf.size(), 0u);
  EXPECT_EQ(const_buf.data(), nullptr);
  EXPECT_EQ(const_buf.deallocator(), nullptr);
}

TEST_F(BufTest, ConstBufMoveConstructor) {
  auto unique_data = test_allocator_.MakeUnique<std::byte[]>(10);
  ASSERT_NE(unique_data, nullptr);
  std::byte* raw_ptr = unique_data.get();

  ConstBuf original = Buf(std::move(unique_data));
  ConstBuf moved(std::move(original));

  EXPECT_EQ(moved.size(), 10u);
  EXPECT_EQ(moved.data(), raw_ptr);
  EXPECT_NE(moved.deallocator(), nullptr);

  // Original should be reset
  EXPECT_TRUE(original.empty());  // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(original.size(), 0u);
  EXPECT_EQ(original.data(), nullptr);
  EXPECT_EQ(original.deallocator(), nullptr);
}

TEST_F(BufTest, ConstBufMoveConstructorFromBuf) {
  auto unique_data = test_allocator_.MakeUnique<std::byte[]>(10);
  ASSERT_NE(unique_data, nullptr);
  std::byte* raw_ptr = unique_data.get();

  Buf original(std::move(unique_data));
  ConstBuf moved(std::move(original));

  EXPECT_EQ(moved.size(), 10u);
  EXPECT_EQ(moved.data(), raw_ptr);
  EXPECT_NE(moved.deallocator(), nullptr);

  // Original Buf should be empty
  EXPECT_TRUE(original.empty());  // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(original.size(), 0u);
}

TEST_F(BufTest, ConstBufMoveAssignment) {
  auto unique_data1 = test_allocator_.MakeUnique<std::byte[]>(10);
  auto unique_data2 = test_allocator_.MakeUnique<std::byte[]>(5);
  std::byte* raw_ptr1 = unique_data1.get();

  ConstBuf cb1 = Buf(std::move(unique_data1));
  ConstBuf cb2 = Buf(std::move(unique_data2));

  cb2 = std::move(cb1);

  EXPECT_EQ(cb2.size(), 10u);
  EXPECT_EQ(cb2.data(), raw_ptr1);
  EXPECT_TRUE(cb1.empty());  // NOLINT(bugprone-use-after-move)
}

TEST_F(BufTest, ConstBufMoveAssignmentFromBuf) {
  auto unique_data1 = test_allocator_.MakeUnique<std::byte[]>(10);
  auto unique_data2 = test_allocator_.MakeUnique<std::byte[]>(5);
  std::byte* raw_ptr1 = unique_data1.get();

  Buf buf = Buf(std::move(unique_data1));
  ConstBuf cb = Buf(std::move(unique_data2));

  cb = std::move(buf);

  EXPECT_EQ(cb.size(), 10u);
  EXPECT_EQ(cb.data(), raw_ptr1);
  EXPECT_TRUE(buf.empty());  // NOLINT(bugprone-use-after-move)
}

TEST_F(BufTest, ConstBufReset) {
  auto unique_data = test_allocator_.MakeUnique<std::byte[]>(10);
  ConstBuf const_buf = Buf(std::move(unique_data));

  EXPECT_FALSE(const_buf.empty());
  EXPECT_GT(test_allocator_.metrics().allocated_bytes.value(), 0u);

  const_buf.reset();

  EXPECT_TRUE(const_buf.empty());
  EXPECT_EQ(const_buf.size(), 0u);
  EXPECT_EQ(const_buf.data(), nullptr);
  EXPECT_EQ(const_buf.deallocator(), nullptr);
  EXPECT_EQ(test_allocator_.metrics().allocated_bytes.value(), 0u);
}

TEST_F(BufTest, ConstBufAccessAndIterators) {
  auto unique_data = test_allocator_.MakeUnique<std::byte[]>(5);
  std::byte* raw_ptr = unique_data.get();
  for (size_t i = 0; i < 5; ++i) {
    raw_ptr[i] = std::byte(i);
  }

  ConstBuf const_buf = Buf(std::move(unique_data));

  EXPECT_EQ(const_buf[0], std::byte(0));
  EXPECT_EQ(const_buf[4], std::byte(4));

  size_t index = 0;
  for (const std::byte& val : const_buf) {
    EXPECT_EQ(val, std::byte(index++));
  }
  EXPECT_EQ(index, 5u);

  index = 0;
  for (auto it = const_buf.cbegin(); it != const_buf.cend(); ++it) {
    EXPECT_EQ(*it, std::byte(index++));
  }
}

TEST_F(BufTest, ConstBufSpanConversion) {
  auto unique_data = test_allocator_.MakeUnique<std::byte[]>(5);
  std::byte* raw_ptr = unique_data.get();

  ConstBuf const_buf = Buf(std::move(unique_data));
  ConstByteSpan span = const_buf;

  EXPECT_EQ(span.size(), 5u);
  EXPECT_EQ(span.data(), raw_ptr);
}

// Buf tests

TEST_F(BufTest, BufDefaultConstructor) {
  Buf buf;
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(buf.size(), 0u);
  EXPECT_EQ(buf.data(), nullptr);
  EXPECT_EQ(buf.deallocator(), nullptr);
}

TEST_F(BufTest, BufUniquePtrConstructor) {
  auto unique_data = test_allocator_.MakeUnique<std::byte[]>(10);
  std::byte* raw_ptr = unique_data.get();

  Buf buf(std::move(unique_data));

  EXPECT_EQ(buf.size(), 10u);
  EXPECT_EQ(buf.data(), raw_ptr);
  EXPECT_NE(buf.deallocator(), nullptr);
}

TEST_F(BufTest, BufUniquePtrConstructorWithOffset) {
  auto unique_data = test_allocator_.MakeUnique<std::byte[]>(10);
  std::byte* raw_ptr = unique_data.get();

  Buf buf(std::move(unique_data), 3);

  EXPECT_EQ(buf.size(), 7u);
  EXPECT_EQ(buf.data(), raw_ptr + 3);
}

TEST_F(BufTest, BufUniquePtrConstructorWithOffsetAndSize) {
  auto unique_data = test_allocator_.MakeUnique<std::byte[]>(10);
  std::byte* raw_ptr = unique_data.get();

  Buf buf(std::move(unique_data), 2, 5);

  EXPECT_EQ(buf.size(), 5u);
  EXPECT_EQ(buf.data(), raw_ptr + 2);
}

TEST_F(BufTest, BufBasicDeallocatorConstructor) {
  auto layout = allocator::Layout::Of<std::byte[10]>();
  void* ptr = test_allocator_.Allocate(layout);
  ASSERT_NE(ptr, nullptr);
  std::byte* raw_ptr = static_cast<std::byte*>(ptr);

  {
    Buf buf(raw_ptr, 10, test_allocator_);
    EXPECT_EQ(buf.size(), 10u);
    EXPECT_EQ(buf.data(), raw_ptr);
    EXPECT_EQ(buf.deallocator(), &test_allocator_);
  }
  EXPECT_EQ(test_allocator_.metrics().allocated_bytes.value(), 0u);
}

TEST_F(BufTest, BufDeallocatorConstructorWithOffset) {
  auto layout = allocator::Layout::Of<std::byte[10]>();
  void* ptr = test_allocator_.Allocate(layout);
  ASSERT_NE(ptr, nullptr);
  std::byte* raw_ptr = static_cast<std::byte*>(ptr);

  {
    Buf buf(raw_ptr, 3, 7, test_allocator_);
    EXPECT_EQ(buf.size(), 7u);
    EXPECT_EQ(buf.data(), raw_ptr + 3);
  }
  EXPECT_EQ(test_allocator_.metrics().allocated_bytes.value(), 0u);
}

TEST_F(BufTest, BufDeallocatorConstructorWithOffsetAndSize) {
  auto layout = allocator::Layout::Of<std::byte[10]>();
  void* ptr = test_allocator_.Allocate(layout);
  ASSERT_NE(ptr, nullptr);
  std::byte* raw_ptr = static_cast<std::byte*>(ptr);

  {
    Buf buf(raw_ptr, 2, 6, test_allocator_);
    EXPECT_EQ(buf.size(), 6u);
    EXPECT_EQ(buf.data(), raw_ptr + 2);
  }
  EXPECT_EQ(test_allocator_.metrics().allocated_bytes.value(), 0u);
}

TEST_F(BufTest, BufUnownedFromSpan) {
  std::array<std::byte, 10> data = {};
  Buf buf = Buf::Unowned(ByteSpan(data));
  EXPECT_EQ(buf.size(), 10u);
  EXPECT_EQ(buf.data(), data.data());
  EXPECT_EQ(buf.deallocator(), nullptr);
}

TEST_F(BufTest, BufUnownedFromPointerAndSize) {
  std::array<std::byte, 10> data = {};
  Buf buf = Buf::Unowned(data.data(), 10);
  EXPECT_EQ(buf.size(), 10u);
  EXPECT_EQ(buf.data(), data.data());
}

TEST_F(BufTest, BufUnownedFromSpanWithOffsetAndSize) {
  std::array<std::byte, 10> data = {};
  Buf buf = Buf::Unowned(ByteSpan(data), 2, 5);
  EXPECT_EQ(buf.size(), 5u);
  EXPECT_EQ(buf.data(), data.data() + 2);
}

TEST_F(BufTest, BufUnownedFromPointerWithOffsetAndSize) {
  std::array<std::byte, 10> data = {};
  Buf buf = Buf::Unowned(data.data(), 3, 4);
  EXPECT_EQ(buf.size(), 4u);
  EXPECT_EQ(buf.data(), data.data() + 3);
}

TEST_F(BufTest, BufMoveConstructorAndAssignment) {
  auto unique_data = test_allocator_.MakeUnique<std::byte[]>(10);
  std::byte* raw_ptr = unique_data.get();

  Buf original(std::move(unique_data));
  Buf moved(std::move(original));

  EXPECT_EQ(moved.size(), 10u);
  EXPECT_EQ(moved.data(), raw_ptr);
  EXPECT_TRUE(original.empty());  // NOLINT(bugprone-use-after-move)

  Buf assigned;
  assigned = std::move(moved);

  EXPECT_EQ(assigned.size(), 10u);
  EXPECT_EQ(assigned.data(), raw_ptr);
  EXPECT_TRUE(moved.empty());  // NOLINT(bugprone-use-after-move)
}

TEST_F(BufTest, BufAccessAndIterators) {
  auto unique_data = test_allocator_.MakeUnique<std::byte[]>(5);
  std::byte* raw_ptr = unique_data.get();
  for (size_t i = 0; i < 5; ++i) {
    raw_ptr[i] = std::byte(i);
  }

  Buf buf(std::move(unique_data));

  // Mutable access
  buf[2] = std::byte(99);
  EXPECT_EQ(buf[2], std::byte(99));

  // Read-only access on const ref
  const Buf& const_ref = buf;
  EXPECT_EQ(const_ref[2], std::byte(99));

  // Iterators
  size_t index = 0;
  for (std::byte& val : buf) {
    if (index == 2) {
      EXPECT_EQ(val, std::byte(99));
    } else {
      EXPECT_EQ(val, std::byte(index));
    }
    index++;
  }
}

TEST_F(BufTest, BufSpanConversion) {
  auto unique_data = test_allocator_.MakeUnique<std::byte[]>(5);
  std::byte* raw_ptr = unique_data.get();

  Buf buf(std::move(unique_data));
  ByteSpan span = buf;

  EXPECT_EQ(span.size(), 5u);
  EXPECT_EQ(span.data(), raw_ptr);
}

// Slicing and reclaiming

TEST_F(BufTest, SliceConstBuf) {
  auto unique_data = test_allocator_.MakeUnique<std::byte[]>(10);
  std::byte* raw_ptr = unique_data.get();
  for (size_t i = 0; i < 10; ++i) {
    raw_ptr[i] = std::byte(i);
  }

  ConstBuf cb = Buf(std::move(unique_data));

  // Slice with offset and length
  ConstBuf sliced = Slice(std::move(cb), 2, 5);
  EXPECT_EQ(sliced.size(), 5u);
  EXPECT_EQ(sliced.data(), raw_ptr + 2);
  EXPECT_EQ(sliced[0], std::byte(2));

  // Slice with offset only
  ConstBuf sliced_to_end = Slice(std::move(sliced), 1);
  EXPECT_EQ(sliced_to_end.size(), 4u);
  EXPECT_EQ(sliced_to_end.data(), raw_ptr + 3);
}

TEST_F(BufTest, SliceBuf) {
  auto unique_data = test_allocator_.MakeUnique<std::byte[]>(10);
  std::byte* raw_ptr = unique_data.get();
  for (size_t i = 0; i < 10; ++i) {
    raw_ptr[i] = std::byte(i);
  }

  Buf buf(std::move(unique_data));

  // Slice with offset and length
  Buf sliced = Slice(std::move(buf), 2, 5);
  EXPECT_EQ(sliced.size(), 5u);
  EXPECT_EQ(sliced.data(), raw_ptr + 2);

  // Slice with offset only
  Buf sliced_to_end = Slice(std::move(sliced), 1);
  EXPECT_EQ(sliced_to_end.size(), 4u);
  EXPECT_EQ(sliced_to_end.data(), raw_ptr + 3);
}

TEST_F(BufTest, TruncateConstBufAndBuf) {
  auto unique_data1 = test_allocator_.MakeUnique<std::byte[]>(10);
  ConstBuf cb = Buf(std::move(unique_data1));
  ConstBuf truncated_cb = Truncate(std::move(cb), 4);
  EXPECT_EQ(truncated_cb.size(), 4u);

  auto unique_data2 = test_allocator_.MakeUnique<std::byte[]>(10);
  Buf buf(std::move(unique_data2));
  Buf truncated_buf = Truncate(std::move(buf), 6);
  EXPECT_EQ(truncated_buf.size(), 6u);
}

TEST_F(BufTest, ReclaimPrefixAndSuffix) {
  auto unique_data = test_allocator_.MakeUnique<std::byte[]>(10);
  std::byte* raw_ptr = unique_data.get();
  for (size_t i = 0; i < 10; ++i) {
    raw_ptr[i] = std::byte(i);
  }

  // Create a sliced Buf
  Buf buf = Slice(Buf(std::move(unique_data)), 2, 5);
  EXPECT_EQ(buf.size(), 5u);
  EXPECT_EQ(buf.data(), raw_ptr + 2);

  // Reclaim prefix
  buf = ReclaimPrefix(std::move(buf), 2);
  EXPECT_EQ(buf.size(), 7u);
  EXPECT_EQ(buf.data(), raw_ptr);

  // Reclaim suffix
  buf = ReclaimSuffix(std::move(buf), 3);
  EXPECT_EQ(buf.size(), 10u);
  EXPECT_EQ(buf.data(), raw_ptr);
}

TEST_F(BufTest, ReclaimCombined) {
  auto unique_data = test_allocator_.MakeUnique<std::byte[]>(10);
  std::byte* raw_ptr = unique_data.get();

  Buf buf = Slice(Buf(std::move(unique_data)), 3, 4);
  buf = Reclaim(std::move(buf), 3, 3);

  EXPECT_EQ(buf.size(), 10u);
  EXPECT_EQ(buf.data(), raw_ptr);
}

// Iterator conversion

TEST_F(BufTest, IteratorConversions) {
  auto unique_data = test_allocator_.MakeUnique<std::byte[]>(10);
  std::byte* raw_ptr = unique_data.get();

  Buf buf(std::move(unique_data));

  // Buf::iterator -> ConstBuf::iterator
  Buf::iterator it = buf.begin();
  ConstBuf::iterator const_it1 = it;
  EXPECT_EQ(const_it1.operator->(), raw_ptr);

  // Buf::const_iterator -> ConstBuf::iterator
  Buf::const_iterator cit = buf.cbegin();
  ConstBuf::iterator const_it2 = cit;
  EXPECT_EQ(const_it2.operator->(), raw_ptr);

  // Buf::iterator + offset -> ConstBuf::iterator
  ConstBuf::iterator const_it3 = buf.begin() + 3;
  EXPECT_EQ(const_it3.operator->(), raw_ptr + 3);
}

// Bounds checks

TEST_F(BufTest, EmptyUniquePtrOffset) {
  UniquePtr<std::byte[]> empty_buffer;
  ConstBuf const_buf = Buf(std::move(empty_buffer), 0);
  EXPECT_TRUE(const_buf.empty());
}

TEST_F(BufTest, EmptyUniquePtrOffsetOutOfBounds) {
  UniquePtr<std::byte[]> empty_buffer;
  EXPECT_DEATH_IF_SUPPORTED(Buf(std::move(empty_buffer), 5), ".*");
}

TEST_F(BufTest, BoundsChecking) {
  auto unique_data = test_allocator_.MakeUnique<std::byte[]>(10);
  Buf buf(std::move(unique_data));
  const ConstBuf& const_ref = buf;

  EXPECT_DEATH_IF_SUPPORTED(buf[10], ".*");
  EXPECT_DEATH_IF_SUPPORTED(const_ref[10], ".*");
}

TEST_F(BufTest, UniquePtrOffsetBounds) {
  auto unique_data = test_allocator_.MakeUnique<std::byte[]>(10);
  EXPECT_DEATH_IF_SUPPORTED(Buf(std::move(unique_data), 15), ".*");
}

TEST_F(BufTest, SelfAssignment) {
  auto unique_data = test_allocator_.MakeUnique<std::byte[]>(10);
  std::byte* raw_ptr = unique_data.get();

  Buf buf(std::move(unique_data));
  ConstBuf& const_ref = buf;

  // Self assignment should not destroy data
  const_ref = std::move(buf);

  EXPECT_EQ(const_ref.size(), 10u);
  EXPECT_EQ(const_ref.data(), raw_ptr);
}

TEST_F(BufTest, SliceConstBufOutOfBounds) {
  auto unique_data = test_allocator_.MakeUnique<std::byte[]>(10);
  ConstBuf cb = Buf(std::move(unique_data));

  EXPECT_DEATH_IF_SUPPORTED((void)Slice(std::move(cb), 11, 0), ".*");
  // Slice to end out of bounds
  auto unique_data2 = test_allocator_.MakeUnique<std::byte[]>(10);
  ConstBuf cb2 = Buf(std::move(unique_data2));
  EXPECT_DEATH_IF_SUPPORTED((void)Slice(std::move(cb2), 11), ".*");
}

TEST_F(BufTest, SliceBufOutOfBounds) {
  auto unique_data = test_allocator_.MakeUnique<std::byte[]>(10);
  Buf buf(std::move(unique_data));

  EXPECT_DEATH_IF_SUPPORTED((void)Slice(std::move(buf), 11, 0), ".*");
  // Slice to end out of bounds
  auto unique_data2 = test_allocator_.MakeUnique<std::byte[]>(10);
  Buf buf2(std::move(unique_data2));
  EXPECT_DEATH_IF_SUPPORTED((void)Slice(std::move(buf2), 11), ".*");
}

TEST_F(BufTest, ReclaimOutOfBounds) {
  auto unique_data = test_allocator_.MakeUnique<std::byte[]>(10);

  // Out of bounds prefix reclaim (max is 2)
  Buf buf_copy1 = Slice(Buf(std::move(unique_data)), 2, 5);
  EXPECT_DEATH_IF_SUPPORTED((void)ReclaimPrefix(std::move(buf_copy1), 3), ".*");
}

TEST_F(BufTest, ConstructorNullptrWithNonZeroSizeAsserts) {
  EXPECT_DEATH_IF_SUPPORTED(Buf(nullptr, 10, test_allocator_), ".*");
}

TEST_F(BufTest, UniquePtrOffsetAndSizeOutOfBounds) {
  auto unique_data = test_allocator_.MakeUnique<std::byte[]>(10);
  EXPECT_DEATH_IF_SUPPORTED(Buf(std::move(unique_data), 2, 9), ".*");
}

TEST_F(BufTest, SliceConstBufLengthExceedsRemainingSizeAsserts) {
  auto unique_data = test_allocator_.MakeUnique<std::byte[]>(10);
  ConstBuf cb = Buf(std::move(unique_data));
  EXPECT_DEATH_IF_SUPPORTED((void)Slice(std::move(cb), 2, 9), ".*");
}

TEST_F(BufTest, SliceBufLengthExceedsRemainingSizeAsserts) {
  auto unique_data = test_allocator_.MakeUnique<std::byte[]>(10);
  Buf buf(std::move(unique_data));
  EXPECT_DEATH_IF_SUPPORTED((void)Slice(std::move(buf), 2, 9), ".*");
}

TEST_F(BufTest, SelfMoveAssignment) {
  // Test ConstBuf self move-assignment
  auto unique_data1 = test_allocator_.MakeUnique<std::byte[]>(10);
  std::byte* raw_ptr1 = unique_data1.get();
  ConstBuf cb = Buf(std::move(unique_data1));
  ConstBuf* cb_ptr = &cb;
  cb = std::move(*cb_ptr);
  EXPECT_EQ(cb.size(), 10u);
  EXPECT_EQ(cb.data(), raw_ptr1);

  // Test Buf self move-assignment
  auto unique_data2 = test_allocator_.MakeUnique<std::byte[]>(10);
  std::byte* raw_ptr2 = unique_data2.get();
  Buf buf(std::move(unique_data2));
  Buf* buf_ptr = &buf;
  buf = std::move(*buf_ptr);
  EXPECT_EQ(buf.size(), 10u);
  EXPECT_EQ(buf.data(), raw_ptr2);
}

TEST_F(BufTest, ConstBufConversion) {
  auto unique_data = test_allocator_.MakeUnique<std::byte[]>(10);
  std::byte* raw_ptr = unique_data.get();
  const Buf buf(std::move(unique_data));

  // Test const Buf& -> const ConstBuf& conversion
  const ConstBuf& const_ref = buf;
  EXPECT_EQ(const_ref.size(), 10u);
  EXPECT_EQ(const_ref.data(), raw_ptr);

  // Test Buf&& -> ConstBuf&& conversion
  Buf buf2 = Buf::Unowned(raw_ptr, 10);
  ConstBuf const_moved(std::move(buf2));
  EXPECT_EQ(const_moved.size(), 10u);
}

TEST_F(BufTest, AllocateSuccess) {
  Buf buf = Buf::Allocate(test_allocator_, 10);
  EXPECT_EQ(buf.size(), 10u);
  EXPECT_NE(buf.data(), nullptr);
  EXPECT_EQ(buf.deallocator(), &test_allocator_);
}

TEST_F(BufTest, AllocateSuccessWithOffset) {
  Buf buf = Buf::Allocate(test_allocator_, 3, 7);
  EXPECT_EQ(buf.size(), 7u);
  EXPECT_EQ(buf.deallocator(), &test_allocator_);
}

TEST_F(BufTest, AllocateSuccessWithOffsetAndSize) {
  Buf buf = Buf::Allocate(test_allocator_, 2, 5);
  EXPECT_EQ(buf.size(), 5u);
  EXPECT_EQ(buf.deallocator(), &test_allocator_);
}

TEST_F(BufTest, AllocateFailureAsserts) {
  allocator::NullAllocator null_allocator;
  EXPECT_DEATH_IF_SUPPORTED((void)Buf::Allocate(null_allocator, 10), ".*");
}

TEST_F(BufTest, TryAllocateSuccess) {
  Buf buf = Buf::TryAllocate(test_allocator_, 10);
  EXPECT_EQ(buf.size(), 10u);
  EXPECT_NE(buf.data(), nullptr);
  EXPECT_EQ(buf.deallocator(), &test_allocator_);
}

TEST_F(BufTest, TryAllocateSuccessWithOffset) {
  Buf buf = Buf::TryAllocate(test_allocator_, 3, 7);
  EXPECT_EQ(buf.size(), 7u);
  EXPECT_EQ(buf.deallocator(), &test_allocator_);
}

TEST_F(BufTest, TryAllocateSuccessWithOffsetAndSize) {
  Buf buf = Buf::TryAllocate(test_allocator_, 2, 5);
  EXPECT_EQ(buf.size(), 5u);
  EXPECT_EQ(buf.deallocator(), &test_allocator_);
}

TEST_F(BufTest, TryAllocateFailureReturnsEmpty) {
  allocator::NullAllocator null_allocator;
  Buf buf = Buf::TryAllocate(null_allocator, 10);
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(buf.size(), 0u);
  EXPECT_EQ(buf.data(), nullptr);
  EXPECT_EQ(buf.deallocator(), nullptr);
}

TEST_F(BufTest, TryAllocateFailureWithOffsetReturnsEmpty) {
  allocator::NullAllocator null_allocator;
  Buf buf = Buf::TryAllocate(null_allocator, 3, 7);
  EXPECT_TRUE(buf.empty());
}

TEST_F(BufTest, TryAllocateFailureWithOffsetAndSizeReturnsEmpty) {
  allocator::NullAllocator null_allocator;
  Buf buf = Buf::TryAllocate(null_allocator, 2, 5);
  EXPECT_TRUE(buf.empty());
}

}  // namespace
}  // namespace pw
