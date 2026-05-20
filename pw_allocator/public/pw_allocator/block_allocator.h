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
#pragma once

#include <cstddef>
#include <optional>

#include "pw_allocator/allocator.h"
#include "pw_allocator/block/basic.h"
#include "pw_allocator/block/iterable.h"
#include "pw_allocator/block/poisonable.h"
#include "pw_allocator/block/result.h"
#include "pw_allocator/block/with_layout.h"
#include "pw_allocator/capability.h"
#include "pw_allocator/config.h"
#include "pw_allocator/fragmentation.h"
#include "pw_allocator/hardening.h"
#include "pw_assert/assert.h"
#include "pw_bytes/span.h"
#include "pw_result/result.h"
#include "pw_status/status.h"

namespace pw::allocator {
namespace internal {

/// Block-independent base class of ``BlockAllocator``.
///
/// This class contains static methods which do not depend on the template
/// parameters of ``BlockAllocator`` that are used to determine block/ type.
/// This allows the methods to be defined in a separate source file and use
/// macros that cannot be used in headers, e.g. PW_CHECK.
///
/// This class should not be used directly. Instead, use ``BlockAllocator`` or
/// one of its specializations.
class GenericBlockAllocator : public pw::Allocator {
 public:
  // Not copyable or movable.
  GenericBlockAllocator(const GenericBlockAllocator&) = delete;
  GenericBlockAllocator& operator=(const GenericBlockAllocator&) = delete;
  GenericBlockAllocator(GenericBlockAllocator&&) = delete;
  GenericBlockAllocator& operator=(GenericBlockAllocator&&) = delete;

 protected:
  template <typename BlockType>
  static constexpr Capabilities GetCapabilities();

  constexpr explicit GenericBlockAllocator(Capabilities capabilities)
      : pw::Allocator(capabilities) {}

  /// Crashes with an informational message that a given block is allocated.
  ///
  /// This method is meant to be called by ``SplitFreeListAllocator``s
  /// destructor. There must not be any outstanding allocations from an
  /// when it is destroyed.
  [[noreturn]] static void CrashOnAllocated(const void* allocated);

  /// Crashes with an informational message that a given pointer does not belong
  /// to this allocator.
  [[noreturn]] static void CrashOnOutOfRange(const void* freed);

  /// Crashes with an informational message that a given block was freed twice.
  [[noreturn]] static void CrashOnDoubleFree(const void* freed);
};

}  // namespace internal

namespace test {

// Forward declaration for friending.
template <typename, size_t>
class BlockAllocatorTest;

}  // namespace test

/// @submodule{pw_allocator,concrete_block}

/// A memory allocator that uses a list of blocks.
///
/// This class does not implement `ChooseBlock` and cannot be used directly.
/// Instead, use one of its specializations.
///
/// NOTE!! Do NOT use memory returned from this allocator as the backing for
/// another allocator. If this is done, the `Query` method may incorrectly
/// think pointers returned by that allocator were created by this one, and
/// report that this allocator can de/reallocate them.
template <typename BlockType_>
class BlockAllocator : public internal::GenericBlockAllocator {
 private:
  using Base = internal::GenericBlockAllocator;

 public:
  using BlockType = BlockType_;
  using Range = typename BlockType::Range;

  static constexpr Capabilities kCapabilities =
      Base::GetCapabilities<BlockType>();
  static constexpr size_t kPoisonInterval = PW_ALLOCATOR_BLOCK_POISON_INTERVAL;

  ~BlockAllocator() override;

  /// Returns a ``Range`` of blocks tracking the memory of this allocator.
  Range blocks() const { return Range(first_); }

  /// Sets the memory region to be used by this allocator.
  ///
  /// This method will instantiate an initial block using the memory region.
  ///
  /// @param[in]  region  Region of memory to use when satisfying allocation
  ///                     requests. The region MUST be valid as an argument to
  ///                     `BlockType::Init`.
  void Init(ByteSpan region);

  /// Returns the largest single allocation that can succeed, given the current
  /// state of the allocator.
  ///
  /// The largest allocation possible at any given time is the inner size of the
  /// largest free block. This method may be expensive to call if the block
  /// allocator implementation does not track its largest block. As a result, it
  /// should primarily be used for diagnostic purposes after an allocation
  /// failure, e.g.
  ///
  /// @code{.cpp}
  /// auto my_object = block_allocator.MakeUnique<MyObject>(my_args);
  /// if (my_object == nullptr) {
  ///   PW_LOG("failed to allocate: needed %zu bytes, but only have %zu",
  ///     sizeof(MyObject), block_allocator.GetMaxAllocatable());
  /// }
  /// @endcode
  ///
  /// Note that this method does not consider alignment. An allocation with a
  /// large alignment requirement may fail even when a large enough block is
  /// available if that block cannot satisfy the alignment requirement.
  size_t GetMaxAllocatable() { return DoGetMaxAllocatable(); }

 protected:
  constexpr explicit BlockAllocator() : Base(kCapabilities) {}

  /// @copydoc Allocator::Allocate
  void* DoAllocate(Layout layout) override;

  /// @copydoc Allocator::Deallocate
  void DoDeallocate(void* ptr) override;

  /// @copydoc Allocator::Resize
  bool DoResize(void* ptr, size_t new_size) override;

  /// @copydoc Allocator::GetAllocated
  size_t DoGetAllocated() const override { return allocated_; }

  /// @copydoc Allocator::DoMeasureFragmentation
  std::optional<Fragmentation> DoMeasureFragmentation() const override;

  /// @copydoc Deallocator::GetInfo
  Result<Layout> DoGetInfo(InfoType info_type, const void* ptr) const override;

  /// Sets the blocks to be used by this allocator.
  ///
  /// This method will use the sequence of blocks including and following
  /// `begin`. These blocks which must be valid.
  ///
  /// @param[in]  begin               The first block for this allocator.
  ///                                 The block must not have a previous block.
  void Init(BlockType* begin);

  /// Returns the block associated with a pointer.
  ///
  /// If the given pointer is to this allocator's memory region, but not to a
  /// valid block, the memory is corrupted and this method will crash to assist
  /// in uncovering the underlying bug.
  ///
  /// @param  ptr           Pointer to an allocated block's usable space.
  ///
  /// @returns
  /// * @OK: Result contains a pointer to the block.
  /// * @OUT_OF_RANGE: Given pointer is outside the allocator's memory.
  template <typename Ptr>
  internal::copy_const_ptr_t<Ptr, BlockType*> FromUsableSpace(Ptr ptr) const;

  /// Frees the given block.
  ///
  /// Derived classes may override this method to hook or even defer freeing
  /// blocks.
  virtual void DeallocateBlock(BlockType*&& block);

 private:
  using BlockResultPrev = internal::GenericBlockResult::Prev;
  using BlockResultNext = internal::GenericBlockResult::Next;

  // Let unit tests call internal methods in order to "preallocate" blocks..
  template <typename, size_t>
  friend class test::BlockAllocatorTest;

  /// @copydoc BlockAllocator::GetMaxAllocatable
  virtual size_t DoGetMaxAllocatable() = 0;

  /// Selects a free block to allocate from.
  ///
  /// This method represents the allocator-specific strategy of choosing which
  /// block should be used to satisfy allocation requests.
  ///
  /// @param  layout  Same as ``Allocator::Allocate``.
  /// @returns The chosen block, or an error status.
  virtual BlockResult<BlockType> ChooseBlock(Layout layout) = 0;

  /// Indicates that a block will no longer be free.
  ///
  /// Does nothing by default. Derived class may overload to do additional
  /// bookkeeeping.
  ///
  /// @param  block   The block being freed.
  virtual void ReserveBlock([[maybe_unused]] BlockType& block) {}

  /// Indicates that a block is now free.
  ///
  /// Does nothing by default. Derived class may overload to do additional
  /// bookkeeeping.
  ///
  /// @param  block   The block being freed.
  virtual void RecycleBlock([[maybe_unused]] BlockType& block) {}

  /// Completes any pending deallocations.
  ///
  /// After calling this method, all memory that has been passed to `Deallocate`
  /// will be free and available for subsequent allocations.
  ///
  /// An allocator implementation may wish to defer doing the work of freeing a
  /// block and potentially merging it with its neighbors in case it
  /// subsequently receives a request for the block's exact size. In this case
  /// it must override this method and clear any caches of freed blocks.
  ///
  /// Allocators that free blocks immediately upon a call to `DoDeallocate` can
  /// simply use the default implementation that does nothing.
  virtual void Flush() {}

  /// Returns if the previous block exists and is free.
  static bool PrevIsFree(const BlockType* block) {
    auto* prev = block->Prev();
    return prev != nullptr && prev->IsFree();
  }

  /// Returns if the next block exists and is free.
  static bool NextIsFree(const BlockType* block) {
    auto* next = block->Next();
    return next != nullptr && next->IsFree();
  }

  /// Ensures the pointer to the last block is correct after the given block is
  /// allocated or freed.
  void UpdateLast(BlockType* block);

  // Represents the range of blocks for this allocator.
  size_t capacity_ = 0;
  size_t allocated_ = 0;
  BlockType* first_ = nullptr;
  BlockType* last_ = nullptr;
  uint16_t unpoisoned_ = 0;
};

/// @}

// Template method implementations

namespace internal {

template <typename BlockType>
constexpr Capabilities GenericBlockAllocator::GetCapabilities() {
  Capabilities common = kImplementsGetUsableLayout |
                        kImplementsGetAllocatedLayout | kImplementsGetCapacity |
                        kImplementsRecognizes;
  if constexpr (has_layout_v<BlockType>) {
    return common | kImplementsGetRequestedLayout;
  } else {
    return common;
  }
}

}  // namespace internal

template <typename BlockType>
BlockAllocator<BlockType>::~BlockAllocator() {
  if constexpr (Hardening::kIncludesRobustChecks) {
    for (auto* block : blocks()) {
      if (!block->IsFree()) {
        CrashOnAllocated(block);
      }
    }
  }
}

template <typename BlockType>
void BlockAllocator<BlockType>::Init(ByteSpan region) {
  Result<BlockType*> result = BlockType::Init(region);
  PW_ASSERT(result.ok());
  Init(*result);
}

template <typename BlockType>
void BlockAllocator<BlockType>::Init(BlockType* begin) {
  if constexpr (Hardening::kIncludesRobustChecks) {
    PW_ASSERT(begin != nullptr);
    PW_ASSERT(begin->Prev() == nullptr);
  }
  first_ = begin;
  for (auto* block : blocks()) {
    last_ = block;
    capacity_ += block->OuterSize();
    if (block->IsFree()) {
      RecycleBlock(*block);
    }
  }
}

template <typename BlockType>
void* BlockAllocator<BlockType>::DoAllocate(Layout layout) {
  if (capacity_ == 0) {
    // Not initialized.
    return nullptr;
  }

  if constexpr (Hardening::kIncludesDebugChecks) {
    PW_ASSERT(last_->Next() == nullptr);
  }
  auto result = ChooseBlock(layout);
  if (!result.ok()) {
    // No valid block for request.
    return nullptr;
  }
  BlockType* block = result.block();
  allocated_ += block->OuterSize();
  switch (result.prev()) {
    case BlockResultPrev::kSplitNew:
      // New free blocks may be created when allocating.
      RecycleBlock(*(block->Prev()));
      break;
    case BlockResultPrev::kResizedLarger:
      // Extra bytes may be appended to the previous block.
      allocated_ += result.size();
      break;
    case BlockResultPrev::kUnchanged:
    case BlockResultPrev::kResizedSmaller:
      break;
  }
  if (result.next() == BlockResultNext::kSplitNew) {
    RecycleBlock(*(block->Next()));
  }

  UpdateLast(block);
  if constexpr (Hardening::kIncludesDebugChecks) {
    PW_ASSERT(block <= last_);
  }

  return block->UsableSpace();
}

template <typename BlockType>
void BlockAllocator<BlockType>::DoDeallocate(void* ptr) {
  BlockType* block = FromUsableSpace(ptr);
  if (block->IsFree()) {
    if constexpr (Hardening::kIncludesBasicChecks) {
      CrashOnDoubleFree(block);
    } else {
      return;
    }
  }
  DeallocateBlock(std::move(block));
}

template <typename BlockType>
void BlockAllocator<BlockType>::DeallocateBlock(BlockType*&& block) {
  // Neighboring blocks may be merged when freeing.
  if (auto* prev = block->Prev(); prev != nullptr && prev->IsFree()) {
    ReserveBlock(*prev);
  }
  if (auto* next = block->Next(); next != nullptr && next->IsFree()) {
    ReserveBlock(*next);
  }

  // Free the block and merge it with its neighbors, if possible.
  allocated_ -= block->OuterSize();
  auto free_result = BlockType::Free(std::move(block));
  block = free_result.block();
  UpdateLast(block);

  if (free_result.prev() == BlockResultPrev::kResizedSmaller) {
    // Bytes were reclaimed from the previous block.
    allocated_ -= free_result.size();
  }

  if constexpr (is_poisonable_v<BlockType> && kPoisonInterval != 0) {
    ++unpoisoned_;
    if (unpoisoned_ >= kPoisonInterval) {
      block->Poison();
      unpoisoned_ = 0;
    }
  }

  RecycleBlock(*block);
}

template <typename BlockType>
bool BlockAllocator<BlockType>::DoResize(void* ptr, size_t new_size) {
  BlockType* block = FromUsableSpace(ptr);

  // Neighboring blocks may be merged when resizing.
  if (auto* next = block->Next(); next != nullptr && next->IsFree()) {
    ReserveBlock(*next);
  }

  size_t old_size = block->OuterSize();
  if (!block->Resize(new_size).ok()) {
    return false;
  }
  allocated_ -= old_size;
  allocated_ += block->OuterSize();
  UpdateLast(block);

  if (auto* next = block->Next(); next != nullptr && next->IsFree()) {
    RecycleBlock(*next);
  }

  return true;
}

template <typename BlockType>
Result<Layout> BlockAllocator<BlockType>::DoGetInfo(InfoType info_type,
                                                    const void* ptr) const {
  // Handle types not related to a block first.
  if (info_type == InfoType::kCapacity) {
    return Layout(capacity_);
  }
  // Get a block from the given pointer.
  if (ptr < first_->UsableSpace() || last_->UsableSpace() < ptr) {
    return Status::NotFound();
  }
  const auto* block = BlockType::FromUsableSpace(ptr);
  if (!block->IsValid()) {
    return Status::DataLoss();
  }
  if (block->IsFree()) {
    return Status::FailedPrecondition();
  }
  if constexpr (kCapabilities.has(kImplementsGetRequestedLayout)) {
    if (info_type == InfoType::kRequestedLayoutOf) {
      return block->RequestedLayout();
    }
  }
  switch (info_type) {
    case InfoType::kUsableLayoutOf:
      return Layout(block->InnerSize(), BlockType::kAlignment);
    case InfoType::kAllocatedLayoutOf:
      return Layout(block->OuterSize(), BlockType::kAlignment);
    case InfoType::kRecognizes:
      return Layout();
    case InfoType::kCapacity:
    case InfoType::kRequestedLayoutOf:
    default:
      return Status::Unimplemented();
  }
}

template <typename BlockType>
std::optional<Fragmentation> BlockAllocator<BlockType>::DoMeasureFragmentation()
    const {
  Fragmentation fragmentation;
  for (auto block : blocks()) {
    if (block->IsFree()) {
      fragmentation.AddFragment(block->InnerSize() / BlockType::kAlignment);
    }
  }
  return fragmentation;
}

template <typename BlockType>
template <typename Ptr>
internal::copy_const_ptr_t<Ptr, BlockType*>
BlockAllocator<BlockType>::FromUsableSpace(Ptr ptr) const {
  if (ptr < first_->UsableSpace() || last_->UsableSpace() < ptr) {
    if constexpr (Hardening::kIncludesBasicChecks) {
      CrashOnOutOfRange(ptr);
    }
    return nullptr;
  }
  auto* block = BlockType::FromUsableSpace(ptr);
  if (!block->IsValid()) {
    if constexpr (Hardening::kIncludesBasicChecks) {
      block->CheckInvariants();
    }
    return nullptr;
  }
  return block;
}

template <typename BlockType>
void BlockAllocator<BlockType>::UpdateLast(BlockType* block) {
  BlockType* next = block->Next();
  if (next == nullptr) {
    last_ = block;
  } else if (next->Next() == nullptr) {
    last_ = next;
  }
}

}  // namespace pw::allocator
