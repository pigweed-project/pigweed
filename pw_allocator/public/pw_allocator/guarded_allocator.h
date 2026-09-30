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

#include "lib/stdcompat/bit.h"
#include "pw_allocator/framing_allocator.h"
#include "pw_allocator/internal/bit.h"
#include "pw_allocator/layout.h"
#include "pw_allocator/synchronized_allocator.h"
#include "pw_result/result.h"
#include "pw_sync/borrow.h"

namespace pw::allocator {

// Forward declaration for friendship
template <typename BlockAllocatorType, typename LockType, typename ErrorHandler>
class GuardedAllocator;

namespace internal {

/// Like FrameError, with additional errors for guard validation errors.
class GuardError : public FrameError {
 public:
  enum Value : uint32_t {
    kBadMagic = FrameError::kMaxValue,
  };

  constexpr GuardError(Value v) : FrameError(static_cast<uint32_t>(v)) {}
  constexpr GuardError(FrameError::Value v) : FrameError(v) {}
};

/// Handles detected errors by calling PW_CRASH with a diagnostic message.
struct DefaultGuardErrorHandler final {
  static void HandleError(GuardError error,
                          const void* ptr1 = nullptr,
                          size_t val1 = 0,
                          const void* ptr2 = nullptr,
                          size_t val2 = 0);
};

/// Generic base class for a GuardedAllocator.
///
/// This allocator detects heap overflows by inserting "guard" values before and
/// after the usable allocated memory regions provided to the caller. The value
/// before the usable memory is an offset to the value after, which is a magic
/// value.
template <typename ErrorHandler>
class BasicGuardedAllocator
    : public FramingAllocator<void, size_t, ErrorHandler> {
 private:
  using Base = FramingAllocator<void, size_t, ErrorHandler>;

 protected:
  constexpr explicit BasicGuardedAllocator(pw::Allocator& allocator)
      : Base(allocator) {}

  /// @copydoc Allocator::Allocate
  void* DoAllocate(Layout layout) override;

  /// @copydoc Deallocator::Deallocate
  void DoDeallocate(void* ptr) override;

  /// @copydoc Allocator::Resize
  bool DoResize(void* ptr, size_t new_size) override;

  /// @copydoc Allocator::BeforeReallocate
  [[nodiscard]] bool DoBeforeReallocate(void* ptr, Layout new_layout) override;

  /// @copydoc Allocator::AfterReallocateCopy
  ///
  /// The call to `Allocate` sets the correct suffix, so this override simply
  /// prevents `FramingAllocator::DoAfterReallocateCopy` from clobbering it.
  void DoAfterReallocateCopy(void*, Layout, void*) override {}

 private:
  /// Returns whether the guard offset and value for the given frame is intact.
  [[nodiscard]] bool ValidateFramePtr(void* frame);

  /// Returns whether the guard value in the given suffix is intact.
  [[nodiscard]] bool ValidateSuffix(const void* data, const size_t* suffix);

  // Allow GuardedAllocator to get pointers and the underlying allocator.
  template <typename, typename, typename>
  friend class ::pw::allocator::GuardedAllocator;

  static constexpr size_t kNumZeros =
      internal::CountRZero(alignof(const void*));
  static constexpr size_t kMagic = static_cast<size_t>(0xDEFACEDC0DE15BADULL);
  static constexpr size_t kShift = sizeof(size_t) * 8 / 3;

  static constexpr size_t ExtractBits(const void* ptr) {
    return static_cast<size_t>(cpp20::bit_cast<uintptr_t>(ptr)) >> kNumZeros;
  }

  static constexpr size_t CalculateMagic(const void* allocator,
                                         const void* data,
                                         const size_t* suffix) {
    return kMagic ^ (ExtractBits(allocator) << (kShift * 0)) ^
           (ExtractBits(data) << (kShift * 1)) ^
           (ExtractBits(suffix) << (kShift * 2));
  }
};

}  // namespace internal

/// @submodule{pw_allocator,forwarding}

/// GuardedAllocator that can detect heap overflows in a thread-safe manner.
///
/// This class takes a `BlockAllocator` and manages concurrent access to it.
/// This allows a background thread to validate allocations using one of two
/// key methods:
///
/// * `ValidateOne` will validate a single block each time it is called.
///   Successive calls will eventually iterate over all blocks.
/// * `ValidateAll` vill validate all current blocks. Other allocator methods
///   will block until the validation is complete.
///
/// Both methods can be called explicitly, or used to create a thread that
/// periodically validates, e.g.
///
/// @code{.cpp}
/// Thread thread(options, [&guarded_allocator]() {
///   while (true) {
///     guarded_allocator.ValidateOne();
///     pw::this_thread::sleep_for(500ms);
///   }
/// });
/// @endcode
///
/// Note that while this allocator wraps a `BlockAllocator` it is NOT a
/// block allocator itself. In particular, pointers allocated from this
/// allocator MUST NOT be passed to methods like `BlockType::FromUsableSpace`.
///
/// @tparam   BlockAllocatorType  Allocator derived from BlockAllocator.
/// @tparam   LockType            Sync primitive link, e.g. sync::Mutex.
/// @tparam   ErrorHandler        Used to report guard validation errors.
template <typename BlockAllocatorType,
          typename LockType = sync::NoLock,
          typename ErrorHandler = internal::DefaultGuardErrorHandler>
class GuardedAllocator final : public ForwardingAllocator {
 public:
  using BlockType = typename BlockAllocatorType::BlockType;

  constexpr explicit GuardedAllocator(BlockAllocatorType& allocator)
      : ForwardingAllocator(allocator.capabilities()),
        guarded_(allocator),
        synchronized_(guarded_) {
    ForwardingAllocator::Init(synchronized_);
  }

  /// Checks for heap overflows in an allocation.
  ///
  /// This method may be called explicitly, or repeatedly from a background
  /// thread. The individual allocation being checked is not specified, but
  /// repeated calls will eventually iterate over all blocks.
  ///
  /// @returns A pointer to the block with a corrupted prefix and/or suffix.
  BlockType* ValidateOne();

  /// Checks for heap overflows in all current allocations.
  ///
  /// This method may be called explicitly, or repeatedly from a background
  /// thread. Other allocator methods will block until the validation is
  /// complete.
  ///
  /// @returns A pointer to the block with a corrupted prefix and/or suffix.
  BlockType* ValidateAll();

 protected:
  /// @copydoc Allocator::Allocate
  void* DoAllocate(Layout layout) override {
    return synchronized_.Allocate(layout);
  }

  /// @copydoc Deallocator::Deallocate
  void DoDeallocate(void* ptr) override;

  /// @copydoc Allocator::Resize
  bool DoResize(void* ptr, size_t new_size) override;

  /// @copydoc Allocator::Reallocate
  void* DoReallocate(void* ptr, Layout new_layout) override;

 private:
  using BorrowedPointer =
      typename SynchronizedAllocator<LockType>::BorrowedPointer;

  /// Decrement the block iterator if a call to `Deallocate`, `Resize` or
  /// `Reallocate` would invalidate it.
  void CheckIterator(const void* ptr);

  internal::BasicGuardedAllocator<ErrorHandler> guarded_;
  SynchronizedAllocator<LockType> synchronized_;

  // Ideally, this would be annotated as being guarded `lock_`. However, the
  // lock is always acquired via `borrowable_`, which does not support thread
  // safety analysis.
  //
  // Adjusted by `CheckIterator` on calls to `Deallocate`, `Resize`, or
  // `Reallocate` to avoid being invalidated by block modifications.
  BlockType* block_ = nullptr;
};

/// @}

// Template method implementations.

// BasicGuardedAllocator ///////////////////////////////////////////////////////

namespace internal {

template <typename ErrorHandler>
void* BasicGuardedAllocator<ErrorHandler>::DoAllocate(Layout layout) {
  void* ptr = Base::DoAllocate(layout);
  if (ptr == nullptr) {
    return nullptr;
  }
  size_t* suffix = Base::GetSuffix(ptr);
  PW_ASSERT(suffix != nullptr);
  *suffix = CalculateMagic(this, ptr, suffix);
  return ptr;
}

template <typename ErrorHandler>
void BasicGuardedAllocator<ErrorHandler>::DoDeallocate(void* ptr) {
  size_t* suffix = Base::GetSuffix(ptr);
  if (suffix == nullptr || !ValidateSuffix(ptr, suffix)) {
    return;
  }
  *suffix = 0;
  Base::DoDeallocate(ptr);
}

template <typename ErrorHandler>
bool BasicGuardedAllocator<ErrorHandler>::DoResize(void* ptr, size_t new_size) {
  // We need to zero the current value before calling `Base::DoResize`, since
  // doing so afterwards may clobber new block metadata. To zero it beforehand,
  // we must also save the old value to be able to restore it should resizing
  // fail.
  size_t* old_suffix = Base::GetSuffix(ptr);
  if (old_suffix == nullptr || !ValidateSuffix(ptr, old_suffix)) {
    return false;
  }
  size_t old_magic = *old_suffix;
  *old_suffix = 0;
  if (!Base::DoResize(ptr, new_size)) {
    *old_suffix = old_magic;
    return false;
  }
  size_t* new_suffix = Base::GetSuffix(ptr);
  PW_ASSERT(new_suffix != nullptr);
  *new_suffix = CalculateMagic(this, ptr, new_suffix);
  return true;
}

template <typename ErrorHandler>
bool BasicGuardedAllocator<ErrorHandler>::DoBeforeReallocate(
    void* ptr, Layout new_layout) {
  size_t* suffix = Base::GetSuffix(ptr);
  return ValidateSuffix(ptr, suffix) &&
         Base::DoBeforeReallocate(ptr, new_layout);
}

template <typename ErrorHandler>
bool BasicGuardedAllocator<ErrorHandler>::ValidateFramePtr(void* frame) {
  if (frame == nullptr) {
    ErrorHandler::HandleError(FrameError::kFrameNull);
    return false;
  }
  Result<size_t> frame_size = Base::GetFrameSize(frame);
  const void* data = Base::Base::GetData(frame, frame_size, Layout::Of<void>());
  const void* frame_alt = Base::Base::GetFrame(data, /*has_suffix=*/true);
  if (frame != frame_alt) {
    ErrorHandler::HandleError(
        FrameError::kFramePointerMismatch, frame, 0, frame_alt);
    return false;
  }
  size_t data_size = *frame_size - Base::Distance(frame, data);
  const void* suffix_ptr =
      Base::Base::GetSuffix(data, data_size, Layout::Of<size_t>());
  if (suffix_ptr == nullptr) {
    return false;
  }
  return ValidateSuffix(data, static_cast<const size_t*>(suffix_ptr));
}

template <typename ErrorHandler>
bool BasicGuardedAllocator<ErrorHandler>::ValidateSuffix(const void* data,
                                                         const size_t* suffix) {
  if (suffix == nullptr) {
    return false;
  }
  auto magic = CalculateMagic(this, data, suffix);
  if (*suffix != magic) {
    ErrorHandler::HandleError(
        GuardError::kBadMagic, nullptr, magic, nullptr, *suffix);
    return false;
  }
  return true;
}

}  // namespace internal

// GuardedAllocator ////////////////////////////////////////////////////////////

template <typename BlockAllocatorType, typename LockType, typename ErrorHandler>
auto GuardedAllocator<BlockAllocatorType, LockType, ErrorHandler>::ValidateOne()
    -> BlockType* {
  BorrowedPointer borrowed = synchronized_.Borrow();
  auto& allocator = static_cast<BlockAllocatorType&>(guarded_.allocator());

  // Find the bounds of the block range.
  auto range = allocator.blocks();
  BlockType* begin = *(range.begin());
  BlockType* end = *(range.end());

  // Ensure there is at least one block.
  if (begin == end) {
    return nullptr;
  }

  // Ensure we are starting from a block.
  if (block_ == nullptr || block_ == end) {
    block_ = begin;
  }

  // Find the next used block.
  BlockType* prev = block_;
  while (block_->IsValid() && block_->IsFree()) {
    BlockType* next = block_->Next();
    if (next == end) {
      // Loop around.
      next = begin;
    }
    if (next == prev) {
      // All blocks are free.
      return nullptr;
    }
    block_ = next;
  }

  // Validate the block.
  if (!block_->IsValid()) {
    return std::exchange(block_, nullptr);
  }
  void* frame = block_->UsableSpace();
  if (!guarded_.ValidateFramePtr(frame)) {
    return std::exchange(block_, nullptr);
  }
  block_ = block_->Next();
  return nullptr;
}

template <typename BlockAllocatorType, typename LockType, typename ErrorHandler>
auto GuardedAllocator<BlockAllocatorType, LockType, ErrorHandler>::ValidateAll()
    -> BlockType* {
  BorrowedPointer borrowed = synchronized_.Borrow();
  auto& allocator = static_cast<BlockAllocatorType&>(guarded_.allocator());
  for (BlockType* block : allocator.blocks()) {
    if (block->IsFree()) {
      continue;
    }
    void* frame = block->UsableSpace();
    if (!guarded_.ValidateFramePtr(frame)) {
      return block;
    }
  }
  return nullptr;
}

template <typename BlockAllocatorType, typename LockType, typename ErrorHandler>
void GuardedAllocator<BlockAllocatorType, LockType, ErrorHandler>::DoDeallocate(
    void* ptr) {
  BorrowedPointer borrowed = synchronized_.Borrow();
  CheckIterator(ptr);
  guarded_.Deallocate(ptr);
}

template <typename BlockAllocatorType, typename LockType, typename ErrorHandler>
bool GuardedAllocator<BlockAllocatorType, LockType, ErrorHandler>::DoResize(
    void* ptr, size_t new_size) {
  BorrowedPointer borrowed = synchronized_.Borrow();
  CheckIterator(ptr);
  return guarded_.Resize(ptr, new_size);
}

template <typename BlockAllocatorType, typename LockType, typename ErrorHandler>
void* GuardedAllocator<BlockAllocatorType, LockType, ErrorHandler>::
    DoReallocate(void* ptr, Layout new_layout) {
  BorrowedPointer borrowed = synchronized_.Borrow();
  CheckIterator(ptr);
  return guarded_.Reallocate(ptr, new_layout);
}

template <typename BlockAllocatorType, typename LockType, typename ErrorHandler>
void GuardedAllocator<BlockAllocatorType, LockType, ErrorHandler>::
    CheckIterator(const void* ptr) {
  const void* frame = guarded_.GetFrame(ptr);
  if (frame == nullptr) {
    return;
  }
  auto* block = BlockType::FromUsableSpace(frame);
  if (block_ == block || block_ == block->Next()) {
    block_ = block->Prev();
  }
}

}  // namespace pw::allocator
