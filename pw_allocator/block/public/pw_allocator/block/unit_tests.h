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
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "lib/stdcompat/bit.h"
#include "pw_allocator/block/poisonable.h"
#include "pw_allocator/block/result.h"
#include "pw_allocator/block/testing.h"
#include "pw_allocator/block/with_layout.h"
#include "pw_allocator/layout.h"
#include "pw_bytes/span.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

namespace pw::allocator::test {

/// Includes block test cases for blocks that derive from `BasicBlock`.
#define PW_ALLOCATOR_BASIC_BLOCK_TESTS(BlockTestType)                     \
  TEST_F(BlockTestType, CanCreateSingleAlignedBlock) {                    \
    CanCreateSingleAlignedBlock();                                        \
  }                                                                       \
  TEST_F(BlockTestType, CanCreateUnalignedSingleBlock) {                  \
    CanCreateUnalignedSingleBlock();                                      \
  }                                                                       \
  TEST_F(BlockTestType, CannotCreateTooSmallBlock) {                      \
    CannotCreateTooSmallBlock();                                          \
  }                                                                       \
  TEST_F(BlockTestType, CanCheckValidBlock) { CanCheckValidBlock(); }     \
  TEST_F(BlockTestType, CanCheckInvalidBlock) { CanCheckInvalidBlock(); } \
  TEST_F(BlockTestType, CanGetBlockFromUsableSpace) {                     \
    CanGetBlockFromUsableSpace();                                         \
  }                                                                       \
  TEST_F(BlockTestType, CanGetConstBlockFromUsableSpace) {                \
    CanGetConstBlockFromUsableSpace();                                    \
  }

/// Includes block test cases for blocks that derive from `AllocatableBlock`.
#define PW_ALLOCATOR_ALLOCATABLE_BLOCK_TESTS(BlockTestType)                   \
  TEST_F(BlockTestType, CannotAllocFirst_Null) { CannotAllocFirst_Null(); }   \
  TEST_F(BlockTestType, CannotAllocFirst_ZeroSize) {                          \
    CannotAllocFirst_ZeroSize();                                              \
  }                                                                           \
  TEST_F(BlockTestType, CannotAllocFirst_Used) { CannotAllocFirst_Used(); }   \
  TEST_F(BlockTestType, CannotAllocFirst_TooSmall) {                          \
    CannotAllocFirst_TooSmall();                                              \
  }                                                                           \
  TEST_F(BlockTestType, CanAllocFirst_Exact_FirstBlock) {                     \
    CanAllocFirst_Exact_FirstBlock();                                         \
  }                                                                           \
  TEST_F(BlockTestType, CanAllocFirst_Exact_SubsequentBlock) {                \
    CanAllocFirst_Exact_SubsequentBlock();                                    \
  }                                                                           \
  TEST_F(BlockTestType, CanAllocFirst_NewNext_FirstBlock) {                   \
    CanAllocFirst_NewNext_FirstBlock();                                       \
  }                                                                           \
  TEST_F(BlockTestType, CanAllocFirst_NewNext_SubsequentBlock) {              \
    CanAllocFirst_NewNext_SubsequentBlock();                                  \
  }                                                                           \
  TEST_F(BlockTestType, CannotAllocLast_Null) { CannotAllocLast_Null(); }     \
  TEST_F(BlockTestType, CannotAllocLast_ZeroSize) {                           \
    CannotAllocLast_ZeroSize();                                               \
  }                                                                           \
  TEST_F(BlockTestType, CannotAllocLast_Used) { CannotAllocLast_Used(); }     \
  TEST_F(BlockTestType, CannotAllocLast_TooSmall) {                           \
    CannotAllocLast_TooSmall();                                               \
  }                                                                           \
  TEST_F(BlockTestType, CanAllocLast_Exact_FirstBlock) {                      \
    CanAllocLast_Exact_FirstBlock();                                          \
  }                                                                           \
  TEST_F(BlockTestType, CanAllocLast_Exact_SubsequentBlock) {                 \
    CanAllocLast_Exact_SubsequentBlock();                                     \
  }                                                                           \
  TEST_F(BlockTestType, CanAllocLast_NewPrev_FirstBlock) {                    \
    CanAllocLast_NewPrev_FirstBlock();                                        \
  }                                                                           \
  TEST_F(BlockTestType, CanAllocLast_NewPrev_SubsequentBlock) {               \
    CanAllocLast_NewPrev_SubsequentBlock();                                   \
  }                                                                           \
  TEST_F(BlockTestType, CannotAllocLastFromNull) {                            \
    CannotAllocLastFromNull();                                                \
  }                                                                           \
  TEST_F(BlockTestType, CannotAllocLastZeroSize) {                            \
    CannotAllocLastZeroSize();                                                \
  }                                                                           \
  TEST_F(BlockTestType, CannotAllocLastFromUsed) {                            \
    CannotAllocLastFromUsed();                                                \
  }                                                                           \
  TEST_F(BlockTestType, FreeingNullDoesNothing) { FreeingNullDoesNothing(); } \
  TEST_F(BlockTestType, FreeingFreeBlockDoesNothing) {                        \
    FreeingFreeBlockDoesNothing();                                            \
  }                                                                           \
  TEST_F(BlockTestType, CanFree) { CanFree(); }                               \
  TEST_F(BlockTestType, CanFreeBlockWithoutMerging) {                         \
    CanFreeBlockWithoutMerging();                                             \
  }                                                                           \
  TEST_F(BlockTestType, CanFreeBlockAndMergeWithPrev) {                       \
    CanFreeBlockAndMergeWithPrev();                                           \
  }                                                                           \
  TEST_F(BlockTestType, CanFreeBlockAndMergeWithNext) {                       \
    CanFreeBlockAndMergeWithNext();                                           \
  }                                                                           \
  TEST_F(BlockTestType, CanFreeBlockAndMergeWithBoth) {                       \
    CanFreeBlockAndMergeWithBoth();                                           \
  }                                                                           \
  TEST_F(BlockTestType, CanResizeBlockSameSize) { CanResizeBlockSameSize(); } \
  TEST_F(BlockTestType, CannotResizeFreeBlock) { CannotResizeFreeBlock(); }   \
  TEST_F(BlockTestType, CanResizeBlockSmallerWithNextFree) {                  \
    CanResizeBlockSmallerWithNextFree();                                      \
  }                                                                           \
  TEST_F(BlockTestType, CanResizeBlockLargerWithNextFree) {                   \
    CanResizeBlockLargerWithNextFree();                                       \
  }                                                                           \
  TEST_F(BlockTestType, CannotResizeBlockMuchLargerWithNextFree) {            \
    CannotResizeBlockMuchLargerWithNextFree();                                \
  }                                                                           \
  TEST_F(BlockTestType, CanResizeBlockSmallerWithNextUsed) {                  \
    CanResizeBlockSmallerWithNextUsed();                                      \
  }                                                                           \
  TEST_F(BlockTestType, CannotResizeBlockLargerWithNextUsed) {                \
    CannotResizeBlockLargerWithNextUsed();                                    \
  }

/// Includes block test cases for blocks that derive from `AlignableBlock`.
#define PW_ALLOCATOR_ALIGNABLE_BLOCK_TESTS(BlockTestType)                      \
  TEST_F(BlockTestType, CanAllocFirst_ExactAligned_FirstBlock) {               \
    CanAllocFirst_ExactAligned_FirstBlock();                                   \
  }                                                                            \
  TEST_F(BlockTestType, CanAllocFirst_ExactAligned_SubsequentBlock) {          \
    CanAllocFirst_ExactAligned_SubsequentBlock();                              \
  }                                                                            \
  TEST_F(BlockTestType, CanAllocFirst_NewPrev_FirstBlock) {                    \
    CanAllocFirst_NewPrev_FirstBlock();                                        \
  }                                                                            \
  TEST_F(BlockTestType, CanAllocFirst_NewPrev_SubsequentBlock) {               \
    CanAllocFirst_NewPrev_SubsequentBlock();                                   \
  }                                                                            \
  TEST_F(BlockTestType, CanAllocFirst_NewPrevAndNewNext_FirstBlock) {          \
    CanAllocFirst_NewPrevAndNewNext_FirstBlock();                              \
  }                                                                            \
  TEST_F(BlockTestType, CanAllocFirst_NewPrevAndNewNext_SubsequentBlock) {     \
    CanAllocFirst_NewPrevAndNewNext_SubsequentBlock();                         \
  }                                                                            \
  TEST_F(BlockTestType, CannotAllocFirst_ShiftToPrev_FirstBlock) {             \
    CannotAllocFirst_ShiftToPrev_FirstBlock();                                 \
  }                                                                            \
  TEST_F(BlockTestType, CanAllocFirst_ShiftToPrev_SubsequentBlock) {           \
    CanAllocFirst_ShiftToPrev_SubsequentBlock();                               \
  }                                                                            \
  TEST_F(BlockTestType, CannotAllocFirst_ShiftToPrevAndNewNext_FirstBlock) {   \
    CannotAllocFirst_ShiftToPrevAndNewNext_FirstBlock();                       \
  }                                                                            \
  TEST_F(BlockTestType, CanAllocFirst_ShiftToPrevAndNewNext_SubsequentBlock) { \
    CanAllocFirst_ShiftToPrevAndNewNext_SubsequentBlock();                     \
  }                                                                            \
  TEST_F(BlockTestType, CanAllocLast_ExactAligned_FirstBlock) {                \
    CanAllocLast_ExactAligned_FirstBlock();                                    \
  }                                                                            \
  TEST_F(BlockTestType, CanAllocLast_ExactAligned_SubsequentBlock) {           \
    CanAllocLast_ExactAligned_SubsequentBlock();                               \
  }                                                                            \
  TEST_F(BlockTestType, CanAllocLast_NewPrevAligned_FirstBlock) {              \
    CanAllocLast_NewPrevAligned_FirstBlock();                                  \
  }                                                                            \
  TEST_F(BlockTestType, CanAllocLast_NewPrevAligned_SubsequentBlock) {         \
    CanAllocLast_NewPrevAligned_SubsequentBlock();                             \
  }                                                                            \
  TEST_F(BlockTestType, CannotAllocLast_ShiftToPrev_FirstBlock) {              \
    CannotAllocLast_ShiftToPrev_FirstBlock();                                  \
  }                                                                            \
  TEST_F(BlockTestType, CanAllocLast_ShiftToPrev_SubsequentBlock) {            \
    CanAllocLast_ShiftToPrev_SubsequentBlock();                                \
  }                                                                            \
  TEST_F(BlockTestType, CannotAllocLastIfTooSmallForAlignment) {               \
    CannotAllocLastIfTooSmallForAlignment();                                   \
  }

/// Includes block test cases for blocks that derive from `PoisonableBlock`.
#define PW_ALLOCATOR_POISONABLE_BLOCK_TESTS(BlockTestType) \
  TEST_F(BlockTestType, CanCheckPoison) { CanCheckPoison(); }

/// Includes block test cases for blocks that derive from `BlockWithLayout`.
#define PW_ALLOCATOR_BLOCK_WITH_LAYOUT_TESTS(BlockTestType) \
  TEST_F(BlockTestType, CanGetAlignmentFromUsedBlock) {     \
    CanGetAlignmentFromUsedBlock();                         \
  }                                                         \
  TEST_F(BlockTestType, FreeBlocksHaveDefaultAlignment) {   \
    FreeBlocksHaveDefaultAlignment();                       \
  }                                                         \
  TEST_F(BlockTestType, ClearRequestedLayoutOnFree) {       \
    ClearRequestedLayoutOnFree();                           \
  }

/// A collection of block-related unit tests.
///
/// To use these tests, simply create a type-alias for your block, and call the
/// individual test cases using the `TEST_F` macro:
///
/// @code{.cpp}
/// using MyBlockTest = pw::allocator::test::BlockTest<MyBlock>;
/// TEST_F(MyBlockTest, CanFree) { CanFree(); }
/// @endcode
///
/// The unit tests are group into sections based on which block-mix-ins they
/// require. Include all tests for which the block implementation satisfies the
/// stated requirements.
template <typename BlockType>
class BlockTest : public ::testing::Test {
 protected:
  using BlockResultPrev = internal::GenericBlockResult::Prev;
  using BlockResultNext = internal::GenericBlockResult::Next;

  /// Iterates to each block reachable from the given one and asserts that it is
  /// valid.
  void CheckAllReachableBlocks(BlockType* block) {
    while (block->Prev() != nullptr) {
      block = block->Prev();
    }
    while (block != nullptr) {
      ASSERT_TRUE(block->IsValid());
      block = block->Next();
    }
  }

  // Unit tests.

  // Requires is_block_v<BlockType>
  void CanCreateSingleAlignedBlock();
  void CanCreateUnalignedSingleBlock();
  void CannotCreateTooSmallBlock();
  void CannotCreateTooLargeBlock();
  void CanCheckValidBlock();
  void CanCheckInvalidBlock();
  void CanGetBlockFromUsableSpace();
  void CanGetConstBlockFromUsableSpace();

  // Requires is_allocatable_v<BlockType>
  void CannotAllocFirst_Null();
  void CannotAllocFirst_ZeroSize();
  void CannotAllocFirst_Used();
  void CannotAllocFirst_TooSmall();
  void CanAllocFirst_Exact_FirstBlock();
  void CanAllocFirst_Exact_SubsequentBlock();
  void CanAllocFirst_NewNext_FirstBlock();
  void CanAllocFirst_NewNext_SubsequentBlock();
  void CannotAllocLast_Null();
  void CannotAllocLast_ZeroSize();
  void CannotAllocLast_Used();
  void CannotAllocLast_TooSmall();
  void CanAllocLast_Exact_FirstBlock();
  void CanAllocLast_Exact_SubsequentBlock();
  void CanAllocLast_NewPrev_FirstBlock();
  void CanAllocLast_NewPrev_SubsequentBlock();
  void CannotAllocLastFromNull();
  void CannotAllocLastZeroSize();
  void CannotAllocLastFromUsed();
  void FreeingNullDoesNothing();
  void FreeingFreeBlockDoesNothing();
  void CanFree();
  void CanFreeBlockWithoutMerging();
  void CanFreeBlockAndMergeWithPrev();
  void CanFreeBlockAndMergeWithNext();
  void CanFreeBlockAndMergeWithBoth();
  void CanResizeBlockSameSize();
  void CannotResizeFreeBlock();
  void CanResizeBlockSmallerWithNextFree();
  void CanResizeBlockLargerWithNextFree();
  void CannotResizeBlockMuchLargerWithNextFree();
  void CanResizeBlockSmallerWithNextUsed();
  void CannotResizeBlockLargerWithNextUsed();

  // Requires is_alignable_v<BlockType>
  void CanAllocFirst_ExactAligned_FirstBlock();
  void CanAllocFirst_ExactAligned_SubsequentBlock();
  void CanAllocFirst_NewPrev_FirstBlock();
  void CanAllocFirst_NewPrev_SubsequentBlock();
  void CanAllocFirst_NewPrevAndNewNext_FirstBlock();
  void CanAllocFirst_NewPrevAndNewNext_SubsequentBlock();
  void CannotAllocFirst_ShiftToPrev_FirstBlock();
  void CanAllocFirst_ShiftToPrev_SubsequentBlock();
  void CannotAllocFirst_ShiftToPrevAndNewNext_FirstBlock();
  void CanAllocFirst_ShiftToPrevAndNewNext_SubsequentBlock();
  void CanAllocLast_ExactAligned_FirstBlock();
  void CanAllocLast_ExactAligned_SubsequentBlock();
  void CanAllocLast_NewPrevAligned_FirstBlock();
  void CanAllocLast_NewPrevAligned_SubsequentBlock();
  void CannotAllocLast_ShiftToPrev_FirstBlock();
  void CanAllocLast_ShiftToPrev_SubsequentBlock();
  void CannotAllocLastIfTooSmallForAlignment();

  // Requires is_poisonable_v<BlockType>
  void CanCheckPoison();

  // Requires has_layout_v<BlockType>
  void CanGetAlignmentFromUsedBlock();
  void FreeBlocksHaveDefaultAlignment();
  void ClearRequestedLayoutOnFree();

 private:
  BlockTestUtilities<BlockType> util_;
};

////////////////////////////////////////////////////////////////////////////////
// Unit tests for blocks derived from `BasicBlock`.

template <typename BlockType>
void BlockTest<BlockType>::CanCreateSingleAlignedBlock() {
  static_assert(is_block_v<BlockType>);
  auto result = BlockType::Init(util_.bytes());
  ASSERT_EQ(result.status(), OkStatus());
  BlockType* block = *result;

  EXPECT_EQ(block->OuterSize(), kDefaultCapacity);
  EXPECT_EQ(block->InnerSize(), kDefaultCapacity - BlockType::kBlockOverhead);
  EXPECT_EQ(block->Prev(), nullptr);
  EXPECT_EQ(block->Next(), nullptr);
  EXPECT_TRUE(block->IsFree());
  EXPECT_EQ(block->Next(), nullptr);
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CanCreateUnalignedSingleBlock() {
  static_assert(is_block_v<BlockType>);
  ByteSpan aligned(util_.bytes());

  auto result = BlockType::Init(aligned.subspan(1));
  EXPECT_EQ(result.status(), OkStatus());
}

template <typename BlockType>
void BlockTest<BlockType>::CannotCreateTooSmallBlock() {
  static_assert(is_block_v<BlockType>);
  std::array<std::byte, 2> bytes;
  auto result = BlockType::Init(bytes);
  EXPECT_EQ(result.status(), Status::ResourceExhausted());
}

template <typename BlockType>
void BlockTest<BlockType>::CannotCreateTooLargeBlock() {
  static_assert(is_block_v<BlockType>);
  std::array<std::byte, kDefaultCapacity> bytes;
  auto result = BlockType::Init(bytes);
  EXPECT_EQ(result.status(), Status::OutOfRange());
}

template <typename BlockType>
void BlockTest<BlockType>::CanCheckValidBlock() {
  static_assert(is_block_v<BlockType>);
  constexpr size_t kOuterSize1 = 512;
  constexpr size_t kOuterSize2 = 256;

  auto* block = util_.Preallocate({
      {kOuterSize1, Preallocation::kUsed},
      {kOuterSize2, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  ASSERT_TRUE(block->IsValid());

  block = block->Next();
  ASSERT_TRUE(block->IsValid());

  block = block->Next();
  ASSERT_TRUE(block->IsValid());
}

template <typename BlockType>
void BlockTest<BlockType>::CanCheckInvalidBlock() {
  static_assert(is_block_v<BlockType>);
  constexpr size_t kOuterSize1 = 128;
  constexpr size_t kOuterSize2 = 384;

  auto* block1 = util_.Preallocate({
      {kOuterSize1, Preallocation::kUsed},
      {kOuterSize2, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  BlockType* block2 = block1->Next();
  BlockType* block3 = block2->Next();

  // Corrupt a Block header.
  // This must not touch memory outside the original region, or the test may
  // (correctly) abort when run with address sanitizer.
  // To remain as agostic to the internals of `Block` as possible, the test
  // copies a smaller block's header to a larger block, and ensure's the
  // contents of blocks are blank.
  std::memset(block1->UsableSpace(), 0, block1->InnerSize());
  std::memset(block2->UsableSpace(), 0, block2->InnerSize());
  std::memset(block3->UsableSpace(), 0, block3->InnerSize());
  EXPECT_TRUE(block1->IsValid());
  EXPECT_TRUE(block2->IsValid());
  EXPECT_TRUE(block3->IsValid());
  auto* src = cpp20::bit_cast<std::byte*>(block1);
  auto* dst = cpp20::bit_cast<std::byte*>(block2);
  std::memcpy(dst, src, sizeof(BlockType));
  EXPECT_FALSE(block1->IsValid());
  EXPECT_FALSE(block2->IsValid());
  EXPECT_FALSE(block3->IsValid());
}

template <typename BlockType>
void BlockTest<BlockType>::CanGetBlockFromUsableSpace() {
  static_assert(is_block_v<BlockType>);
  auto* block1 = util_.Preallocate({
      {Preallocation::kSizeRemaining, Preallocation::kFree},
  });

  void* ptr = block1->UsableSpace();
  BlockType* block2 = BlockType::FromUsableSpace(ptr);
  EXPECT_EQ(block1, block2);
}

template <typename BlockType>
void BlockTest<BlockType>::CanGetConstBlockFromUsableSpace() {
  static_assert(is_block_v<BlockType>);
  const auto* block1 = util_.Preallocate({
      {Preallocation::kSizeRemaining, Preallocation::kFree},
  });

  const void* ptr = block1->UsableSpace();
  const BlockType* block2 = BlockType::FromUsableSpace(ptr);
  EXPECT_EQ(block1, block2);
}

////////////////////////////////////////////////////////////////////////////////
// Unit testss for blocks derived from `AllocatableBlock`.

template <typename BlockType>
void BlockTest<BlockType>::CannotAllocFirst_Null() {
  static_assert(is_allocatable_v<BlockType>);
  constexpr Layout kLayout(1, 1);

  BlockType* block = nullptr;

  auto result = BlockType::AllocFirst(std::move(block), kLayout);
  EXPECT_EQ(result.status(), Status::InvalidArgument());
  EXPECT_EQ(result.block(), nullptr);
}

template <typename BlockType>
void BlockTest<BlockType>::CannotAllocFirst_ZeroSize() {
  static_assert(is_allocatable_v<BlockType>);
  constexpr Layout kLayout(0, 1);

  auto* block = util_.Preallocate({
      {Preallocation::kSizeRemaining, Preallocation::kFree},
  });

  auto result = BlockType::AllocFirst(std::move(block), kLayout);
  EXPECT_EQ(result.status(), Status::InvalidArgument());
  CheckAllReachableBlocks(result.block());
}

template <typename BlockType>
void BlockTest<BlockType>::CannotAllocFirst_Used() {
  static_assert(is_allocatable_v<BlockType>);
  constexpr Layout kLayout(1, 1);

  auto* block = util_.Preallocate({
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });

  auto result = BlockType::AllocFirst(std::move(block), kLayout);
  EXPECT_EQ(result.status(), Status::FailedPrecondition());
  CheckAllReachableBlocks(result.block());
}

template <typename BlockType>
void BlockTest<BlockType>::CannotAllocFirst_TooSmall() {
  static_assert(is_allocatable_v<BlockType>);
  constexpr Layout kLayout(256, 1);

  // Trim the buffer so that the layout does not fit.
  util_.TrimBytes(0, util_.GetOuterSize(kLayout.size()) - 1);

  auto* block = util_.Preallocate({
      {Preallocation::kSizeRemaining, Preallocation::kFree},
  });
  auto result = BlockType::AllocFirst(std::move(block), kLayout);
  EXPECT_EQ(result.status(), Status::ResourceExhausted());
  CheckAllReachableBlocks(result.block());
}

template <typename BlockType>
void BlockTest<BlockType>::CanAllocFirst_Exact_FirstBlock() {
  static_assert(is_allocatable_v<BlockType>);
  constexpr Layout kLayout(256, 1);

  // Leave enough space free for the requested block.
  size_t available = util_.GetOuterSize(kLayout.size());
  auto* block = util_.Preallocate({
      {available, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });

  // Allocate from the front of the block.
  auto result = BlockType::AllocFirst(std::move(block), kLayout);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kUnchanged);
  EXPECT_EQ(result.next(), BlockResultNext::kUnchanged);
  block = result.block();

  EXPECT_GE(block->InnerSize(), kLayout.size());
  EXPECT_FALSE(block->IsFree());
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CanAllocFirst_Exact_SubsequentBlock() {
  static_assert(is_allocatable_v<BlockType>);
  constexpr Layout kLayout(256, 1);

  // Preallocate a first block so that the next block is aligned.
  size_t leading = util_.GetFirstAlignedOffset(kLayout);

  // Leave enough space free for the requested block.
  size_t available = util_.GetOuterSize(kLayout.size());

  auto* block = util_.Preallocate({
      {leading, Preallocation::kUsed},
      {available, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  block = block->Next();

  // Allocate from the front of the block.
  auto result = BlockType::AllocFirst(std::move(block), kLayout);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kUnchanged);
  EXPECT_EQ(result.next(), BlockResultNext::kUnchanged);
  block = result.block();

  EXPECT_GE(block->InnerSize(), kLayout.size());
  EXPECT_FALSE(block->IsFree());
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CanAllocFirst_NewNext_FirstBlock() {
  static_assert(is_allocatable_v<BlockType>);
  constexpr Layout kLayout(256, 1);

  // Trim the front of the buffer so that the first block is aligned.
  util_.TrimAligned();

  // Leave enough space free for the requested block and one more block.
  size_t available = util_.GetOuterSize(kLayout.size()) + util_.GetOuterSize(1);

  auto* block = util_.Preallocate({
      {available, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });

  // Allocate from the front of the block.
  auto result = BlockType::AllocFirst(std::move(block), kLayout);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kUnchanged);
  EXPECT_EQ(result.next(), BlockResultNext::kSplitNew);
  block = result.block();

  EXPECT_GE(block->InnerSize(), kLayout.size());
  EXPECT_FALSE(block->IsFree());
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CanAllocFirst_NewNext_SubsequentBlock() {
  static_assert(is_allocatable_v<BlockType>);
  constexpr Layout kLayout(256, 1);

  // Preallocate a first block so that the next block is aligned.
  size_t leading = util_.GetFirstAlignedOffset(kLayout);

  // Leave enough space free for the requested block and one more block.
  size_t available = util_.GetOuterSize(kLayout.size()) + util_.GetOuterSize(1);

  auto* block = util_.Preallocate({
      {leading, Preallocation::kUsed},
      {available, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  block = block->Next();

  // Allocate from the front of the block.
  auto result = BlockType::AllocFirst(std::move(block), kLayout);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kUnchanged);
  EXPECT_EQ(result.next(), BlockResultNext::kSplitNew);
  block = result.block();

  EXPECT_GE(block->InnerSize(), kLayout.size());
  EXPECT_FALSE(block->IsFree());
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CannotAllocLast_Null() {
  static_assert(is_allocatable_v<BlockType>);
  constexpr Layout kLayout(1, 1);

  BlockType* block = nullptr;

  // Attempt and fail to allocate from the front of the block.
  auto result = BlockType::AllocLast(std::move(block), kLayout);
  EXPECT_EQ(result.status(), Status::InvalidArgument());
  EXPECT_EQ(result.block(), nullptr);
}

template <typename BlockType>
void BlockTest<BlockType>::CannotAllocLast_ZeroSize() {
  static_assert(is_allocatable_v<BlockType>);
  constexpr Layout kLayout(0, 1);

  auto* block = util_.Preallocate({
      {Preallocation::kSizeRemaining, Preallocation::kFree},
  });

  // Check if we expect this to succeed.
  auto can_alloc_last = block->CanAlloc(kLayout);
  EXPECT_EQ(can_alloc_last.status(), Status::InvalidArgument());

  // Attempt and fail to allocate from the front of the block.
  auto result = BlockType::AllocLast(std::move(block), kLayout);
  EXPECT_EQ(result.status(), Status::InvalidArgument());
  CheckAllReachableBlocks(result.block());
}

template <typename BlockType>
void BlockTest<BlockType>::CannotAllocLast_Used() {
  static_assert(is_allocatable_v<BlockType>);
  constexpr Layout kLayout(1, 1);

  auto* block = util_.Preallocate({
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });

  // Check if we expect this to succeed.
  auto can_alloc_last = block->CanAlloc(kLayout);
  EXPECT_EQ(can_alloc_last.status(), Status::FailedPrecondition());

  // Attempt and fail to allocate from the front of the block.
  auto result = BlockType::AllocLast(std::move(block), kLayout);
  EXPECT_EQ(result.status(), Status::FailedPrecondition());
  CheckAllReachableBlocks(result.block());
}

template <typename BlockType>
void BlockTest<BlockType>::CannotAllocLast_TooSmall() {
  constexpr Layout kLayout(256, 1);
  static_assert(is_allocatable_v<BlockType>);

  // Trim the buffer so that the layout does not fit.
  util_.TrimBytes(0, util_.GetOuterSize(kLayout.size()) - 1);

  auto* block = util_.Preallocate({
      {Preallocation::kSizeRemaining, Preallocation::kFree},
  });

  // Check if we expect this to succeed.
  auto can_alloc_last = block->CanAlloc(kLayout);
  EXPECT_EQ(can_alloc_last.status(), Status::ResourceExhausted());

  // Attempt and fail to allocate from the front of the block.
  auto result = BlockType::AllocLast(std::move(block), kLayout);
  EXPECT_EQ(result.status(), Status::ResourceExhausted());
  CheckAllReachableBlocks(result.block());
}

template <typename BlockType>
void BlockTest<BlockType>::CanAllocLast_Exact_FirstBlock() {
  constexpr Layout kLayout(256, 1);
  static_assert(is_allocatable_v<BlockType>);

  // Trim the front of the buffer so that the first block is aligned.
  util_.TrimAligned();

  // Leave enough space free for the requested block.
  size_t available = util_.GetOuterSize(kLayout.size());

  auto* block = util_.Preallocate({
      {available, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });

  // Check if we expect this to succeed.
  auto can_alloc_last = block->CanAlloc(kLayout);
  ASSERT_EQ(can_alloc_last.status(), OkStatus());
  EXPECT_EQ(can_alloc_last.size(), 0U);

  // Allocate from the back of the block.
  auto result = BlockType::AllocLast(std::move(block), kLayout);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kUnchanged);
  EXPECT_EQ(result.next(), BlockResultNext::kUnchanged);
  block = result.block();

  EXPECT_GE(block->InnerSize(), kLayout.size());
  EXPECT_FALSE(block->IsFree());
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CanAllocLast_Exact_SubsequentBlock() {
  static_assert(is_allocatable_v<BlockType>);
  constexpr Layout kLayout(256, 1);

  // Preallocate a first block so that the next block is aligned.
  size_t leading = util_.GetFirstAlignedOffset(kLayout);

  // Leave enough space free for the requested block.
  size_t available = util_.GetOuterSize(kLayout.size());

  auto* block = util_.Preallocate({
      {leading, Preallocation::kUsed},
      {available, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  block = block->Next();

  // Check if we expect this to succeed.
  auto can_alloc_last = block->CanAlloc(kLayout);
  ASSERT_EQ(can_alloc_last.status(), OkStatus());
  EXPECT_EQ(can_alloc_last.size(), 0U);

  // Allocate from the back of the block.
  auto result = BlockType::AllocLast(std::move(block), kLayout);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kUnchanged);
  EXPECT_EQ(result.next(), BlockResultNext::kUnchanged);
  block = result.block();

  EXPECT_GE(block->InnerSize(), kLayout.size());
  EXPECT_FALSE(block->IsFree());
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CanAllocLast_NewPrev_FirstBlock() {
  static_assert(is_allocatable_v<BlockType>);
  constexpr Layout kLayout(256, 1);

  // Leave enough space free for a block and the requested block.
  size_t available = util_.GetOuterSize(1) + util_.GetOuterSize(kLayout.size());

  auto* block = util_.Preallocate({
      {available, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });

  // Check if we expect this to succeed.
  auto can_alloc_last = block->CanAlloc(kLayout);
  ASSERT_EQ(can_alloc_last.status(), OkStatus());
  EXPECT_EQ(can_alloc_last.size(), util_.GetOuterSize(1));

  // Allocate from the back of the block.
  auto result = BlockType::AllocLast(std::move(block), kLayout);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kSplitNew);
  EXPECT_EQ(result.next(), BlockResultNext::kUnchanged);
  block = result.block();

  EXPECT_GE(block->InnerSize(), kLayout.size());
  EXPECT_FALSE(block->IsFree());
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CanAllocLast_NewPrev_SubsequentBlock() {
  static_assert(is_allocatable_v<BlockType>);
  constexpr Layout kLayout(256, 1);

  // Preallocate a first block with room for another block before the next
  // alignment boundary.
  size_t leading = util_.GetOuterSize(1);

  // Leave enough space free for a block and the requested block.
  size_t available = util_.GetOuterSize(1) + util_.GetOuterSize(kLayout.size());

  auto* block = util_.Preallocate({
      {leading, Preallocation::kUsed},
      {available, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  block = block->Next();

  // Check if we expect this to succeed.
  auto can_alloc_last = block->CanAlloc(kLayout);
  ASSERT_EQ(can_alloc_last.status(), OkStatus());
  EXPECT_EQ(can_alloc_last.size(), util_.GetOuterSize(1));

  // Allocate from the back of the block.
  auto result = BlockType::AllocLast(std::move(block), kLayout);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kSplitNew);
  EXPECT_EQ(result.next(), BlockResultNext::kUnchanged);
  block = result.block();

  EXPECT_GE(block->InnerSize(), kLayout.size());
  EXPECT_FALSE(block->IsFree());
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CannotAllocLastFromNull() {
  static_assert(is_allocatable_v<BlockType>);
  BlockType* block = nullptr;
  constexpr Layout kLayout(1, 1);
  auto result = BlockType::AllocLast(std::move(block), kLayout);
  EXPECT_EQ(result.status(), Status::InvalidArgument());
  EXPECT_EQ(result.block(), nullptr);
}

template <typename BlockType>
void BlockTest<BlockType>::CannotAllocLastZeroSize() {
  static_assert(is_allocatable_v<BlockType>);
  auto* block = util_.Preallocate({
      {Preallocation::kSizeRemaining, Preallocation::kFree},
  });
  constexpr Layout kLayout(0, 1);
  auto result = BlockType::AllocLast(std::move(block), kLayout);
  EXPECT_EQ(result.status(), Status::InvalidArgument());
  CheckAllReachableBlocks(result.block());
}

template <typename BlockType>
void BlockTest<BlockType>::CannotAllocLastFromUsed() {
  static_assert(is_allocatable_v<BlockType>);
  auto* block = util_.Preallocate({
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  constexpr Layout kLayout(1, 1);
  auto result = BlockType::AllocLast(std::move(block), kLayout);
  EXPECT_EQ(result.status(), Status::FailedPrecondition());
  CheckAllReachableBlocks(result.block());
}

template <typename BlockType>
void BlockTest<BlockType>::FreeingNullDoesNothing() {
  static_assert(is_allocatable_v<BlockType>);
  BlockType* block = nullptr;
  auto result = BlockType::Free(std::move(block));
  EXPECT_EQ(result.status(), Status::InvalidArgument());
}

template <typename BlockType>
void BlockTest<BlockType>::FreeingFreeBlockDoesNothing() {
  static_assert(is_allocatable_v<BlockType>);
  auto* block = util_.Preallocate({
      {Preallocation::kSizeRemaining, Preallocation::kFree},
  });
  auto result = BlockType::Free(std::move(block));
  ASSERT_EQ(result.status(), OkStatus());
  CheckAllReachableBlocks(result.block());
}

template <typename BlockType>
void BlockTest<BlockType>::CanFree() {
  static_assert(is_allocatable_v<BlockType>);
  auto* block = util_.Preallocate({
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });

  auto result = BlockType::Free(std::move(block));
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kUnchanged);
  EXPECT_EQ(result.next(), BlockResultNext::kUnchanged);

  block = result.block();
  EXPECT_TRUE(block->IsFree());
  EXPECT_EQ(block->OuterSize(), kDefaultCapacity);
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CanFreeBlockWithoutMerging() {
  static_assert(is_allocatable_v<BlockType>);
  constexpr size_t kOuterSize = 256;

  auto* block = util_.Preallocate({
      {kOuterSize, Preallocation::kUsed},
      {kOuterSize, Preallocation::kUsed},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  block = block->Next();
  BlockType* next = block->Next();
  BlockType* prev = block->Prev();

  auto result = BlockType::Free(std::move(block));
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kUnchanged);
  EXPECT_EQ(result.next(), BlockResultNext::kUnchanged);

  block = result.block();
  EXPECT_TRUE(block->IsFree());
  EXPECT_EQ(next, block->Next());
  EXPECT_EQ(prev, block->Prev());
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CanFreeBlockAndMergeWithPrev() {
  static_assert(is_allocatable_v<BlockType>);
  constexpr size_t kOuterSize = 256;

  auto* first = util_.Preallocate({
      {kOuterSize, Preallocation::kFree},
      {kOuterSize, Preallocation::kUsed},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  BlockType* block = first->Next();
  BlockType* next = block->Next();

  // Note that when merging with the previous free block, it is that previous
  // free block which is returned, and only the 'next' field indicates a merge.
  auto result = BlockType::Free(std::move(block));
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kUnchanged);
  EXPECT_EQ(result.next(), BlockResultNext::kMerged);

  block = result.block();
  EXPECT_EQ(block->Prev(), nullptr);
  EXPECT_EQ(block->Next(), next);
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CanFreeBlockAndMergeWithNext() {
  static_assert(is_allocatable_v<BlockType>);
  constexpr size_t kOuterSize = 256;

  auto* first = util_.Preallocate({
      {kOuterSize, Preallocation::kUsed},
      {kOuterSize, Preallocation::kUsed},
      {Preallocation::kSizeRemaining, Preallocation::kFree},
  });
  BlockType* block = first->Next();
  BlockType* prev = block->Prev();

  auto result = BlockType::Free(std::move(block));
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kUnchanged);
  EXPECT_EQ(result.next(), BlockResultNext::kMerged);

  block = result.block();
  EXPECT_TRUE(block->IsFree());
  EXPECT_EQ(block->Prev(), prev);
  EXPECT_EQ(block->Next(), nullptr);
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CanFreeBlockAndMergeWithBoth() {
  static_assert(is_allocatable_v<BlockType>);
  constexpr size_t kOuterSize = 128;

  auto* first = util_.Preallocate({
      {kOuterSize, Preallocation::kFree},
      {kOuterSize, Preallocation::kUsed},
      {Preallocation::kSizeRemaining, Preallocation::kFree},
  });
  BlockType* block = first->Next();

  auto result = BlockType::Free(std::move(block));
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kUnchanged);
  EXPECT_EQ(result.next(), BlockResultNext::kMerged);

  block = result.block();
  EXPECT_EQ(block->Prev(), nullptr);
  EXPECT_EQ(block->Next(), nullptr);
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CanResizeBlockSameSize() {
  static_assert(is_allocatable_v<BlockType>);
  constexpr size_t kOuterSize = 256;

  auto* block = util_.Preallocate({
      {kOuterSize, Preallocation::kUsed},
      {kOuterSize, Preallocation::kUsed},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  block = block->Next();

  auto result = block->Resize(block->InnerSize());
  EXPECT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kUnchanged);
  EXPECT_EQ(result.next(), BlockResultNext::kUnchanged);
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CannotResizeFreeBlock() {
  static_assert(is_allocatable_v<BlockType>);
  constexpr size_t kOuterSize = 256;

  auto* block = util_.Preallocate({
      {kOuterSize, Preallocation::kUsed},
      {kOuterSize, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  block = block->Next();

  auto result = block->Resize(block->InnerSize());
  EXPECT_EQ(result.status(), Status::FailedPrecondition());
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CanResizeBlockSmallerWithNextFree() {
  static_assert(is_allocatable_v<BlockType>);
  constexpr size_t kOuterSize = 256;

  auto* block = util_.Preallocate({
      {kOuterSize, Preallocation::kUsed},
      {kOuterSize, Preallocation::kUsed},
      {Preallocation::kSizeRemaining, Preallocation::kFree},
  });
  block = block->Next();
  size_t next_inner_size = block->Next()->InnerSize();

  // Shrink by a single alignment width.
  size_t new_inner_size = block->InnerSize() - BlockType::kAlignment;
  auto result = block->Resize(new_inner_size);
  EXPECT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kUnchanged);
  EXPECT_EQ(result.next(), BlockResultNext::kResized);
  EXPECT_EQ(block->InnerSize(), new_inner_size);

  BlockType* next = block->Next();
  EXPECT_TRUE(next->IsFree());
  EXPECT_EQ(next->InnerSize(), next_inner_size + BlockType::kAlignment);
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CanResizeBlockLargerWithNextFree() {
  static_assert(is_allocatable_v<BlockType>);
  constexpr size_t kOuterSize = 256;

  auto* block = util_.Preallocate({
      {kOuterSize, Preallocation::kUsed},
      {kOuterSize, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  size_t next_inner_size = block->Next()->InnerSize();

  // Grow by less than the minimum needed for a block. The extra should be
  // added to the subsequent block.
  size_t delta = BlockType::kBlockOverhead;
  size_t new_inner_size = block->InnerSize() + delta;
  auto result = block->Resize(new_inner_size);
  EXPECT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kUnchanged);
  EXPECT_EQ(result.next(), BlockResultNext::kResized);
  EXPECT_EQ(block->InnerSize(), new_inner_size);

  BlockType* next = block->Next();
  EXPECT_TRUE(next->IsFree());
  EXPECT_EQ(next->InnerSize(), next_inner_size - delta);
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CannotResizeBlockMuchLargerWithNextFree() {
  static_assert(is_allocatable_v<BlockType>);
  constexpr size_t kOuterSize = 256;

  auto* block = util_.Preallocate({
      {kOuterSize, Preallocation::kUsed},
      {kOuterSize, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });

  size_t new_inner_size = block->InnerSize() + kOuterSize + 1;
  auto result = block->Resize(new_inner_size);
  EXPECT_EQ(result.status(), Status::ResourceExhausted());
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CanResizeBlockSmallerWithNextUsed() {
  static_assert(is_allocatable_v<BlockType>);
  constexpr Layout kLayout(256, kAlign);
  constexpr size_t kOuterSize = BlockType::kBlockOverhead + kLayout.size();

  auto* block = util_.Preallocate({
      {kOuterSize, Preallocation::kUsed},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });

  // Shrink the block.
  size_t delta = kLayout.size() / 2;
  size_t new_inner_size = block->InnerSize() - delta;
  auto result = block->Resize(new_inner_size);
  EXPECT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kUnchanged);
  EXPECT_EQ(result.next(), BlockResultNext::kSplitNew);

  BlockType* next = block->Next();
  EXPECT_TRUE(next->IsFree());
  EXPECT_EQ(next->OuterSize(), delta);
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CannotResizeBlockLargerWithNextUsed() {
  static_assert(is_allocatable_v<BlockType>);
  constexpr size_t kOuterSize = 256;

  auto* block = util_.Preallocate({
      {kOuterSize, Preallocation::kUsed},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });

  size_t delta = BlockType::kBlockOverhead / 2;
  size_t new_inner_size = block->InnerSize() + delta;
  auto result = block->Resize(new_inner_size);
  EXPECT_EQ(result.status(), Status::ResourceExhausted());
}

////////////////////////////////////////////////////////////////////////////////
// Unit tests for blocks derived from `AlignableBlock`.

template <typename BlockType>
void BlockTest<BlockType>::CanAllocFirst_ExactAligned_FirstBlock() {
  static_assert(is_alignable_v<BlockType>);
  constexpr Layout kLayout(256, kAlign);

  // Trim the front of the buffer so that the first block is aligned.
  util_.TrimAligned();

  // Leave enough space free for the requested block.
  size_t available = util_.GetOuterSize(kLayout.size());
  auto* block = util_.Preallocate({
      {available, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });

  // Allocate from the front of the block.
  auto result = BlockType::AllocFirst(std::move(block), kLayout);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kUnchanged);
  EXPECT_EQ(result.next(), BlockResultNext::kUnchanged);
  block = result.block();

  EXPECT_GE(block->InnerSize(), kLayout.size());
  auto addr = cpp20::bit_cast<uintptr_t>(block->UsableSpace());
  EXPECT_EQ(addr % kAlign, 0U);
  EXPECT_FALSE(block->IsFree());
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CanAllocFirst_ExactAligned_SubsequentBlock() {
  static_assert(is_alignable_v<BlockType>);
  constexpr Layout kLayout(256, kAlign);

  // Preallocate a first block so that the next block is aligned.
  size_t leading = util_.GetFirstAlignedOffset(kLayout);

  // Leave enough space free for the requested block.
  size_t available = util_.GetOuterSize(kLayout.size());

  auto* block = util_.Preallocate({
      {leading, Preallocation::kUsed},
      {available, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  block = block->Next();

  // Allocate from the front of the block.
  auto result = BlockType::AllocFirst(std::move(block), kLayout);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kUnchanged);
  EXPECT_EQ(result.next(), BlockResultNext::kUnchanged);
  block = result.block();

  EXPECT_GE(block->InnerSize(), kLayout.size());
  auto addr = cpp20::bit_cast<uintptr_t>(block->UsableSpace());
  EXPECT_EQ(addr % kAlign, 0U);
  EXPECT_FALSE(block->IsFree());
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CanAllocFirst_NewPrev_FirstBlock() {
  static_assert(is_alignable_v<BlockType>);
  constexpr Layout kLayout(256, kAlign);

  // Trim the front of the buffer so that there is room for a block before the
  // first alignment boundary.
  util_.TrimAligned(kAlign - util_.GetOuterSize(1));
  // util_.TrimAligned();

  // Leave enough space free for a block and the requested block.
  size_t available = util_.GetOuterSize(1) + util_.GetOuterSize(kLayout.size());

  auto* block = util_.Preallocate({
      {available, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });

  // Allocate from the front of the block.
  auto result = BlockType::AllocFirst(std::move(block), kLayout);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kSplitNew);
  EXPECT_EQ(result.next(), BlockResultNext::kUnchanged);
  block = result.block();

  EXPECT_GE(block->InnerSize(), kLayout.size());
  auto addr = cpp20::bit_cast<uintptr_t>(block->UsableSpace());
  EXPECT_EQ(addr % kAlign, 0U);
  EXPECT_FALSE(block->IsFree());
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CanAllocFirst_NewPrev_SubsequentBlock() {
  static_assert(is_alignable_v<BlockType>);
  constexpr Layout kLayout(256, kAlign);

  // Preallocate a first block with room for another block before the next
  // alignment boundary.
  size_t leading =
      util_.GetFirstAlignedOffset(kLayout) + kAlign - util_.GetOuterSize(1);

  // Leave enough space free for a block and the requested block.
  size_t available = util_.GetOuterSize(1) + util_.GetOuterSize(kLayout.size());

  auto* block = util_.Preallocate({
      {leading, Preallocation::kUsed},
      {available, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  block = block->Next();

  // Allocate from the front of the block.
  auto result = BlockType::AllocFirst(std::move(block), kLayout);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kSplitNew);
  EXPECT_EQ(result.next(), BlockResultNext::kUnchanged);
  block = result.block();

  EXPECT_GE(block->InnerSize(), kLayout.size());
  auto addr = cpp20::bit_cast<uintptr_t>(block->UsableSpace());
  EXPECT_EQ(addr % kAlign, 0U);
  EXPECT_FALSE(block->IsFree());
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CanAllocFirst_NewPrevAndNewNext_FirstBlock() {
  static_assert(is_alignable_v<BlockType>);
  constexpr Layout kLayout(256, kAlign);

  // Trim the front of the buffer so that there is room for a block before the
  // first alignment boundary.
  util_.TrimAligned(kAlign - util_.GetOuterSize(1));

  // Leave enough space free for a block, the requested block, and one more
  // block.
  size_t available = util_.GetOuterSize(1) +
                     util_.GetOuterSize(kLayout.size()) + util_.GetOuterSize(1);

  auto* block = util_.Preallocate({
      {available, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });

  // Allocate from the front of the block.
  auto result = BlockType::AllocFirst(std::move(block), kLayout);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kSplitNew);
  EXPECT_EQ(result.next(), BlockResultNext::kSplitNew);
  block = result.block();

  EXPECT_GE(block->InnerSize(), kLayout.size());
  auto addr = cpp20::bit_cast<uintptr_t>(block->UsableSpace());
  EXPECT_EQ(addr % kAlign, 0U);
  EXPECT_FALSE(block->IsFree());
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CanAllocFirst_NewPrevAndNewNext_SubsequentBlock() {
  static_assert(is_alignable_v<BlockType>);
  constexpr Layout kLayout(256, kAlign);

  // Preallocate a first block with room for another block before the next
  // alignment boundary.
  size_t leading =
      util_.GetFirstAlignedOffset(kLayout) + kAlign - util_.GetOuterSize(1);

  // Leave enough space free for a block and the requested block and one more
  // block.
  size_t available = kAlign + util_.GetOuterSize(kLayout.size());

  auto* block = util_.Preallocate({
      {leading, Preallocation::kUsed},
      {available, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  block = block->Next();

  auto result = BlockType::AllocFirst(std::move(block), kLayout);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kSplitNew);
  EXPECT_EQ(result.next(), BlockResultNext::kSplitNew);
  block = result.block();

  EXPECT_GE(block->InnerSize(), kLayout.size());
  auto addr = cpp20::bit_cast<uintptr_t>(block->UsableSpace());
  EXPECT_EQ(addr % kAlign, 0U);
  EXPECT_FALSE(block->IsFree());
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CannotAllocFirst_ShiftToPrev_FirstBlock() {
  static_assert(is_alignable_v<BlockType>);
  constexpr Layout kLayout(256, kAlign);

  // Trim the front of the buffer so that there is `kAlignment` bytes before
  // where the aligned block would start.
  util_.TrimAligned(kAlign - BlockType::kAlignment);

  // Leave enough space free for the `kAlignment` bytes and the requested
  // block.
  size_t available = BlockType::kAlignment + util_.GetOuterSize(kLayout.size());

  auto* block = util_.Preallocate({
      {available, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });

  // Attempt and fail to allocate from the front of the block.
  auto result = BlockType::AllocFirst(std::move(block), kLayout);
  EXPECT_EQ(result.status(), Status::ResourceExhausted());
  CheckAllReachableBlocks(result.block());
}

template <typename BlockType>
void BlockTest<BlockType>::CanAllocFirst_ShiftToPrev_SubsequentBlock() {
  static_assert(is_alignable_v<BlockType>);
  constexpr Layout kLayout(256, kAlign);

  // Preallocate a first block so that there is `kAlignment` bytes before
  // where the aligned block would start.
  size_t leading =
      util_.GetFirstAlignedOffset(kLayout) + kAlign - BlockType::kAlignment;

  // Leave enough space free for the `kAlignment` bytes and the requested
  // block.
  size_t available = BlockType::kAlignment + util_.GetOuterSize(kLayout.size());

  auto* first = util_.Preallocate({
      {leading, Preallocation::kUsed},
      {available, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  BlockType* block = first->Next();

  // Allocate from the front of the block.
  auto result = BlockType::AllocFirst(std::move(block), kLayout);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kResizedLarger);
  EXPECT_EQ(result.next(), BlockResultNext::kUnchanged);
  block = result.block();

  // Verify the previous block was padded.
  size_t old_requested_size = leading - BlockType::kBlockOverhead;
  if constexpr (has_layout_v<BlockType>) {
    Layout old_layout = first->RequestedLayout();
    EXPECT_EQ(old_layout.size(), old_requested_size);
  }

  // Resize the first block.
  size_t new_requested_size = old_requested_size + 1;
  result = first->Resize(new_requested_size);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kUnchanged);
  EXPECT_EQ(result.next(), BlockResultNext::kUnchanged);

  // Verify the padding is updated.
  if constexpr (has_layout_v<BlockType>) {
    Layout new_layout = first->RequestedLayout();
    EXPECT_EQ(new_layout.size(), new_requested_size);
  }

  EXPECT_GE(block->InnerSize(), kLayout.size());
  auto addr = cpp20::bit_cast<uintptr_t>(block->UsableSpace());
  EXPECT_EQ(addr % kAlign, 0U);
  EXPECT_FALSE(block->IsFree());

  // Verify that freeing the subsequent block does not reclaim bytes that were
  // resized.
  result = BlockType::Free(std::move(block));
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kUnchanged);
  EXPECT_EQ(result.next(), BlockResultNext::kUnchanged);
  CheckAllReachableBlocks(first);
}

template <typename BlockType>
void BlockTest<BlockType>::CannotAllocFirst_ShiftToPrevAndNewNext_FirstBlock() {
  static_assert(is_alignable_v<BlockType>);
  constexpr Layout kLayout(256, kAlign);

  // Trim the front of the buffer so that there is `kAlignment` bytes before
  // where the aligned block would start.
  util_.TrimAligned(kAlign - BlockType::kAlignment);

  // Leave enough space free for the `kAlignment` bytes, the requested block,
  // and one more block.
  size_t available = BlockType::kAlignment +
                     util_.GetOuterSize(kLayout.size()) + util_.GetOuterSize(1);

  auto* block = util_.Preallocate({
      {available, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });

  // Attempt and fail to allocate from the front of the block.
  auto result = BlockType::AllocFirst(std::move(block), kLayout);
  EXPECT_EQ(result.status(), Status::ResourceExhausted());
  CheckAllReachableBlocks(result.block());
}

template <typename BlockType>
void BlockTest<
    BlockType>::CanAllocFirst_ShiftToPrevAndNewNext_SubsequentBlock() {
  static_assert(is_alignable_v<BlockType>);
  constexpr Layout kLayout(256, kAlign);

  // Preallocate a first block so that there is `kAlignment` bytes before
  // where the aligned block would start.
  size_t leading =
      util_.GetFirstAlignedOffset(kLayout) + kAlign - BlockType::kAlignment;

  // Leave enough space free for the `kAlignment` bytes, the requested block,
  // and one more block.
  size_t available = BlockType::kAlignment +
                     util_.GetOuterSize(kLayout.size()) + util_.GetOuterSize(1);

  auto* block = util_.Preallocate({
      {leading, Preallocation::kUsed},
      {available, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  block = block->Next();

  // Allocate from the front of the block.
  auto result = BlockType::AllocFirst(std::move(block), kLayout);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kResizedLarger);
  EXPECT_EQ(result.next(), BlockResultNext::kSplitNew);
  block = result.block();

  EXPECT_GE(block->InnerSize(), kLayout.size());
  auto addr = cpp20::bit_cast<uintptr_t>(block->UsableSpace());
  EXPECT_EQ(addr % kAlign, 0U);
  EXPECT_FALSE(block->IsFree());
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CanAllocLast_ExactAligned_FirstBlock() {
  static_assert(is_alignable_v<BlockType>);
  constexpr Layout kLayout(256, kAlign);

  // Trim the front of the buffer so that the first block is aligned.
  util_.TrimAligned();

  // Leave enough space free for the requested block.
  size_t available = util_.GetOuterSize(kLayout.size());

  auto* block = util_.Preallocate({
      {available, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });

  // Check if we expect this to succeed.
  auto can_alloc_last = block->CanAlloc(kLayout);
  ASSERT_EQ(can_alloc_last.status(), OkStatus());
  EXPECT_EQ(can_alloc_last.size(), 0U);

  // Allocate from the back of the block.
  auto result = BlockType::AllocLast(std::move(block), kLayout);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kUnchanged);
  EXPECT_EQ(result.next(), BlockResultNext::kUnchanged);
  block = result.block();

  EXPECT_GE(block->InnerSize(), kLayout.size());
  auto addr = cpp20::bit_cast<uintptr_t>(block->UsableSpace());
  EXPECT_EQ(addr % kAlign, 0U);
  EXPECT_FALSE(block->IsFree());
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CanAllocLast_ExactAligned_SubsequentBlock() {
  static_assert(is_alignable_v<BlockType>);
  constexpr Layout kLayout(256, kAlign);

  // Preallocate a first block so that the next block is aligned.
  size_t leading = util_.GetFirstAlignedOffset(kLayout);

  // Leave enough space free for the requested block.
  size_t available = util_.GetOuterSize(kLayout.size());

  auto* block = util_.Preallocate({
      {leading, Preallocation::kUsed},
      {available, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  block = block->Next();

  // Check if we expect this to succeed.
  auto can_alloc_last = block->CanAlloc(kLayout);
  ASSERT_EQ(can_alloc_last.status(), OkStatus());
  EXPECT_EQ(can_alloc_last.size(), 0U);

  // Allocate from the back of the block.
  auto result = BlockType::AllocLast(std::move(block), kLayout);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kUnchanged);
  EXPECT_EQ(result.next(), BlockResultNext::kUnchanged);
  block = result.block();

  EXPECT_GE(block->InnerSize(), kLayout.size());
  auto addr = cpp20::bit_cast<uintptr_t>(block->UsableSpace());
  EXPECT_EQ(addr % kAlign, 0U);
  EXPECT_FALSE(block->IsFree());
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CanAllocLast_NewPrevAligned_FirstBlock() {
  static_assert(is_alignable_v<BlockType>);
  constexpr Layout kLayout(256, kAlign);

  // Trim the front of the buffer so that there is room for a block before the
  // first alignment boundary.
  util_.TrimAligned(kAlign - util_.GetOuterSize(1));

  // Leave enough space free for a block and the requested block.
  size_t available = util_.GetOuterSize(1) + util_.GetOuterSize(kLayout.size());

  auto* block = util_.Preallocate({
      {available, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });

  // Check if we expect this to succeed.
  auto can_alloc_last = block->CanAlloc(kLayout);
  ASSERT_EQ(can_alloc_last.status(), OkStatus());
  EXPECT_EQ(can_alloc_last.size(), util_.GetOuterSize(1));

  // Allocate from the back of the block.
  auto result = BlockType::AllocLast(std::move(block), kLayout);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kSplitNew);
  EXPECT_EQ(result.next(), BlockResultNext::kUnchanged);
  block = result.block();

  EXPECT_GE(block->InnerSize(), kLayout.size());
  auto addr = cpp20::bit_cast<uintptr_t>(block->UsableSpace());
  EXPECT_EQ(addr % kAlign, 0U);
  EXPECT_FALSE(block->IsFree());
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CanAllocLast_NewPrevAligned_SubsequentBlock() {
  static_assert(is_alignable_v<BlockType>);
  constexpr Layout kLayout(256, kAlign);

  // Preallocate a first block with room for another block before the next
  // alignment boundary.
  size_t leading =
      util_.GetFirstAlignedOffset(kLayout) + kAlign - util_.GetOuterSize(1);

  // Leave enough space free for a block and the requested block.
  size_t available = util_.GetOuterSize(1) + util_.GetOuterSize(kLayout.size());

  auto* block = util_.Preallocate({
      {leading, Preallocation::kUsed},
      {available, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  block = block->Next();

  // Check if we expect this to succeed.
  auto can_alloc_last = block->CanAlloc(kLayout);
  ASSERT_EQ(can_alloc_last.status(), OkStatus());
  EXPECT_EQ(can_alloc_last.size(), util_.GetOuterSize(1));

  // Allocate from the back of the block.
  auto result = BlockType::AllocLast(std::move(block), kLayout);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kSplitNew);
  EXPECT_EQ(result.next(), BlockResultNext::kUnchanged);
  block = result.block();

  EXPECT_GE(block->InnerSize(), kLayout.size());
  auto addr = cpp20::bit_cast<uintptr_t>(block->UsableSpace());
  EXPECT_EQ(addr % kAlign, 0U);
  EXPECT_FALSE(block->IsFree());
  CheckAllReachableBlocks(block);
}

template <typename BlockType>
void BlockTest<BlockType>::CannotAllocLast_ShiftToPrev_FirstBlock() {
  static_assert(is_alignable_v<BlockType>);
  constexpr Layout kLayout(256, kAlign);

  // Trim the front of the buffer so that there is `kAlignment` bytes before
  // where the aligned block would start.
  util_.TrimAligned(kAlign - BlockType::kAlignment);

  // Leave enough space free for the `kAlignment` bytes and the requested
  // block.
  size_t available = BlockType::kAlignment + util_.GetOuterSize(kLayout.size());

  auto* block = util_.Preallocate({
      {available, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });

  // Check if we expect this to succeed.
  auto can_alloc_last = block->CanAlloc(kLayout);
  EXPECT_EQ(can_alloc_last.status(), Status::ResourceExhausted());

  // Attempt and fail to allocate from the back of the block.
  auto result = BlockType::AllocFirst(std::move(block), kLayout);
  EXPECT_EQ(result.status(), Status::ResourceExhausted());
  CheckAllReachableBlocks(result.block());
}

template <typename BlockType>
void BlockTest<BlockType>::CanAllocLast_ShiftToPrev_SubsequentBlock() {
  static_assert(is_alignable_v<BlockType>);
  constexpr Layout kLayout(256, kAlign);

  // Preallocate a first block so that there is `kAlignment` bytes before
  // where the aligned block would start.
  size_t leading =
      util_.GetFirstAlignedOffset(kLayout) + kAlign - BlockType::kAlignment;

  // Leave enough space free for the `kAlignment` bytes and the requested
  // block.
  size_t available = BlockType::kAlignment + util_.GetOuterSize(kLayout.size());

  auto* block = util_.Preallocate({
      {leading, Preallocation::kUsed},
      {available, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  block = block->Next();

  // Check if we expect this to succeed.
  auto can_alloc_last = block->CanAlloc(kLayout);
  ASSERT_EQ(can_alloc_last.status(), OkStatus());
  EXPECT_EQ(can_alloc_last.size(), BlockType::kAlignment);

  // Allocate from the back of the block.
  auto result = BlockType::AllocLast(std::move(block), kLayout);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kResizedLarger);
  EXPECT_EQ(result.next(), BlockResultNext::kUnchanged);
  EXPECT_EQ(result.size(), BlockType::kAlignment);
  block = result.block();

  EXPECT_GE(block->InnerSize(), kLayout.size());
  auto addr = cpp20::bit_cast<uintptr_t>(block->UsableSpace());
  EXPECT_EQ(addr % kAlign, 0U);
  EXPECT_FALSE(block->IsFree());

  // Deallocate the block.
  result = BlockType::Free(std::move(block));
  ASSERT_EQ(result.status(), OkStatus());

  // If the block tracks its original layout, verify the bytes are reclaimed.
  if constexpr (has_layout_v<BlockType>) {
    EXPECT_EQ(result.prev(), BlockResultPrev::kResizedSmaller);
    EXPECT_EQ(result.size(), BlockType::kAlignment);
  } else {
    EXPECT_EQ(result.prev(), BlockResultPrev::kUnchanged);
  }
  EXPECT_EQ(result.next(), BlockResultNext::kUnchanged);

  CheckAllReachableBlocks(result.block());
}

template <typename BlockType>
void BlockTest<BlockType>::CannotAllocLastIfTooSmallForAlignment() {
  static_assert(is_alignable_v<BlockType>);
  constexpr Layout kLayout(256, kAlign);
  constexpr size_t kOuterSize = BlockType::kBlockOverhead + kLayout.size();

  // Make sure the block's usable space is not aligned.
  size_t outer_size = util_.GetFirstAlignedOffset(kLayout) + 1;
  auto* block = util_.Preallocate({
      {outer_size, Preallocation::kUsed},
      {kOuterSize, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  block = block->Next();

  // Cannot allocate without room to a split a block for alignment.
  auto result = BlockType::AllocLast(std::move(block), kLayout);
  EXPECT_EQ(result.status(), Status::ResourceExhausted());
  CheckAllReachableBlocks(result.block());
}

////////////////////////////////////////////////////////////////////////////////
// Unit tests for blocks derived from `PoisonableBlock`.

template <typename BlockType>
void BlockTest<BlockType>::CanCheckPoison() {
  static_assert(is_poisonable_v<BlockType>);
  auto* block = util_.Preallocate({
      {Preallocation::kSizeRemaining, Preallocation::kFree},
  });

  ByteSpan data = util_.bytes();

  // Modify a byte in the middle of a free block.
  // Without poisoning, the modification is undetected.
  EXPECT_TRUE(block->IsFree());
  data[kDefaultCapacity / 2] = std::byte(0x7f);
  EXPECT_TRUE(block->IsValid());

  // Modify a byte in the middle of a free block.
  // With poisoning, the modification is detected.
  block->Poison();
  data[kDefaultCapacity / 2] = std::byte(0x7f);
  EXPECT_FALSE(block->IsValid());
}

////////////////////////////////////////////////////////////////////////////////
// Unit tests for blocks derived from `BlockWithLayout`.

template <typename BlockType>
void BlockTest<BlockType>::CanGetAlignmentFromUsedBlock() {
  static_assert(has_layout_v<BlockType>);
  constexpr Layout kLayout1(128, kAlign);
  constexpr Layout kLayout2(384, kAlign * 2);

  auto* block1 = util_.Preallocate({
      {Preallocation::kSizeRemaining, Preallocation::kFree},
  });

  auto result = BlockType::AllocLast(std::move(block1), kLayout1);
  ASSERT_EQ(result.status(), OkStatus());
  block1 = result.block();

  BlockType* block2 = block1->Prev();
  result = BlockType::AllocLast(std::move(block2), kLayout2);
  ASSERT_EQ(result.status(), OkStatus());
  block2 = result.block();

  Layout block1_layout = block1->RequestedLayout();
  Layout block2_layout = block2->RequestedLayout();
  EXPECT_EQ(block1_layout.alignment(), kAlign);
  EXPECT_EQ(block2_layout.alignment(), kAlign * 2);
}

template <typename BlockType>
void BlockTest<BlockType>::FreeBlocksHaveDefaultAlignment() {
  static_assert(has_layout_v<BlockType>);
  constexpr Layout kLayout1(128, kAlign);
  constexpr Layout kLayout2(384, kAlign * 2);

  auto* block1 = util_.Preallocate({
      {Preallocation::kSizeRemaining, Preallocation::kFree},
  });

  auto result = BlockType::AllocLast(std::move(block1), kLayout1);
  ASSERT_EQ(result.status(), OkStatus());
  block1 = result.block();

  BlockType* block2 = block1->Prev();
  result = BlockType::AllocLast(std::move(block2), kLayout2);
  ASSERT_EQ(result.status(), OkStatus());

  Layout layout = block1->RequestedLayout();
  EXPECT_EQ(layout.alignment(), kAlign);

  result = BlockType::Free(std::move(block1));
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kUnchanged);
  EXPECT_EQ(result.next(), BlockResultNext::kMerged);
  block1 = result.block();
}

template <typename BlockType>
void BlockTest<BlockType>::ClearRequestedLayoutOnFree() {
  static_assert(has_layout_v<BlockType>);
  constexpr Layout kLayout(256, kAlign);

  // Preallocate a first block so that there is `kAlignment` bytes before
  // where the aligned block would start.
  size_t leading =
      util_.GetFirstAlignedOffset(kLayout) + kAlign - BlockType::kBlockOverhead;

  // Leave enough space free for the `kAlignment` bytes and the requested
  // block.
  size_t available =
      BlockType::kBlockOverhead + util_.GetOuterSize(kLayout.size());

  auto* leading_block = util_.Preallocate({
      {leading, Preallocation::kUsed},
      {available, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  auto* block = leading_block->Next();

  // Check if we expect this to succeed.
  auto can_alloc_last = block->CanAlloc(kLayout);
  ASSERT_EQ(can_alloc_last.status(), OkStatus());
  EXPECT_EQ(can_alloc_last.size(), BlockType::kBlockOverhead);

  // Allocate from the back of the block.
  auto result = BlockType::AllocLast(std::move(block), kLayout);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.prev(), BlockResultPrev::kResizedLarger);
  EXPECT_EQ(result.next(), BlockResultNext::kUnchanged);
  EXPECT_EQ(result.size(), BlockType::kBlockOverhead);
  block = result.block();

  // Free the leading block; which should clear its requested layout
  Layout layout = leading_block->RequestedLayout();
  result = BlockType::Free(std::move(leading_block));
  ASSERT_EQ(result.status(), OkStatus());
  leading_block = result.block();

  // Allocate the back portion of the leading block, leaving a residual block
  // smaller than the amount previously shifted to it.
  leading = BlockType::kBlockOverhead + BlockType::kAlignment;
  layout = Layout(leading_block->InnerSize() - leading, 1);
  result = BlockType::AllocLast(std::move(leading_block), layout);
  ASSERT_EQ(result.status(), OkStatus());
  leading_block = result.block()->Prev();

  // Regression test for b/446769988: If the requested layout is *NOT* cleared
  // when a block is freed, this allocation may use incorrectly try to read the
  // previous requested size when allocationg on the highly aligned path.
  // This read would be triggered even if the allocation failed.
  layout = Layout(leading_block->InnerSize(), BlockType::kAlignment * 2);
  result = BlockType::AllocLast(std::move(leading_block), layout);
  CheckAllReachableBlocks(result.block());
}

}  // namespace pw::allocator::test
