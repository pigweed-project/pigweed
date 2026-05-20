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
#pragma once

#include <cstddef>
#include <mutex>

#include "pw_allocator/allocator.h"
#include "pw_allocator/first_fit.h"
#include "pw_allocator/hardening.h"
#include "pw_allocator/metrics.h"
#include "pw_allocator/tracking_allocator.h"
#include "pw_assert/assert.h"
#include "pw_bytes/span.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_tokenizer/tokenize.h"
#include "pw_unit_test/framework.h"

namespace pw::allocator::test {

/// @submodule{pw_allocator,impl_test}

static_assert(Hardening::kIncludesDebugChecks,
              "Tests must use a config that enables strict validation");

// A token that can be used in tests.
inline constexpr pw::tokenizer::Token kToken = PW_TOKENIZE_STRING("test");

/// Free all the blocks reachable by the given block. Useful for test cleanup.
template <typename BlockType>
void FreeAll(typename BlockType::Range range) {
  BlockType* block = *(range.begin());
  if (block == nullptr) {
    return;
  }

  // Rewind to the first block.
  BlockType* prev = block->Prev();
  while (prev != nullptr) {
    block = prev;
    prev = block->Prev();
  }

  // Free and merge blocks.
  while (block != nullptr) {
    if (!block->IsFree()) {
      auto result = BlockType::Free(std::move(block));
      block = result.block();
    }
    block = block->Next();
  }
}

/// An `AllocatorForTest` that is automatically initialized on construction.
template <size_t kBufferSize,
          typename BlockType_ = FirstFitBlock<uint32_t>,
          typename MetricsType_ = internal::AllMetrics>
class AllocatorForTest : public pw::Allocator {
 public:
  using BlockType = BlockType_;
  using MetricsType = MetricsType_;
  using AllocatorType = FirstFitAllocator<BlockType>;

  // Since the unbderlying first-fit allocator uses an intrusive free list, all
  // allocations will be at least this size.
  static constexpr size_t kMinSize = BlockType::kAlignment;

  AllocatorForTest()
      : Allocator(AllocatorType::kCapabilities),
        allocator_(buffer_),
        tracker_(kToken, allocator_) {
    ResetParameters();
  }

  ~AllocatorForTest() override { FreeAll<BlockType>(blocks()); }

  typename BlockType::Range blocks() const { return allocator_.blocks(); }
  typename BlockType::Range blocks() { return allocator_.blocks(); }

  const metric::Group& metric_group() const { return tracker_.metric_group(); }
  metric::Group& metric_group() { return tracker_.metric_group(); }

  const MetricsType& metrics() const { return tracker_.metrics(); }

  size_t allocate_size() const { return allocate_size_; }
  void* deallocate_ptr() const { return deallocate_ptr_; }
  size_t deallocate_size() const { return deallocate_size_; }
  void* resize_ptr() const { return resize_ptr_; }
  size_t resize_old_size() const { return resize_old_size_; }
  size_t resize_new_size() const { return resize_new_size_; }

  /// Resets the recorded parameters to an initial state.
  void ResetParameters() {
    allocate_size_ = 0;
    deallocate_ptr_ = nullptr;
    deallocate_size_ = 0;
    resize_ptr_ = nullptr;
    resize_old_size_ = 0;
    resize_new_size_ = 0;
    enable_measure_fragmentation_ = true;
    fragmentation_ = std::nullopt;
  }

  /// Allocates all the memory from this object.
  void Exhaust() {
    for (auto* block : allocator_.blocks()) {
      if (block->IsFree()) {
        auto result = BlockType::AllocLast(std::move(block),
                                           Layout(block->InnerSize(), 1));
        PW_ASSERT(result.status() == OkStatus());

        using Prev = internal::GenericBlockResult::Prev;
        PW_ASSERT(result.prev() == Prev::kUnchanged);

        using Next = internal::GenericBlockResult::Next;
        PW_ASSERT(result.next() == Next::kUnchanged);
      }
    }
  }

  /// Sets whether this allocator returns fragmentation information from
  /// ``MeasureFragmentation`` or ``std::nullopt``.
  void SetMeasureFragmentationEnabled(bool enabled) {
    enable_measure_fragmentation_ = enabled;
  }

  /// Sets a fake fragmentation struct to be returned by this allocator. This
  /// can be used to test that fragmentation info is properly forwarded by
  /// forwarding allocators regardless of the details of the underlying
  /// allocator.
  void SetFragmentation(const Fragmentation& fragmentation) {
    fragmentation_ = fragmentation;
  }

 protected:
  /// Returns the underlying tracking allocator.
  TrackingAllocator<MetricsType>& GetTracker() { return tracker_; }

  /// @copydoc Allocator::DoMeasureFragmentation
  std::optional<Fragmentation> DoMeasureFragmentation() const override {
    if (!enable_measure_fragmentation_) {
      return std::nullopt;
    }
    if (fragmentation_.has_value()) {
      return fragmentation_;
    }
    return allocator_.MeasureFragmentation();
  }

 private:
  /// @copydoc Allocator::Allocate
  void* DoAllocate(Layout layout) override {
    allocate_size_ = layout.size();
    void* ptr = tracker_.Allocate(layout);
    return ptr;
  }

  /// @copydoc Allocator::Deallocate
  void DoDeallocate(void* ptr) override {
    Result<Layout> requested = GetRequestedLayout(ptr);
    deallocate_ptr_ = ptr;
    deallocate_size_ = requested.ok() ? requested->size() : 0;
    tracker_.Deallocate(ptr);
  }

  /// @copydoc Allocator::Resize
  bool DoResize(void* ptr, size_t new_size) override {
    Result<Layout> requested = GetRequestedLayout(ptr);
    resize_ptr_ = ptr;
    resize_old_size_ = requested.ok() ? requested->size() : 0;
    resize_new_size_ = new_size;
    return tracker_.Resize(ptr, new_size);
  }

  /// @copydoc Allocator::GetAllocated
  size_t DoGetAllocated() const override { return tracker_.GetAllocated(); }

  /// @copydoc Deallocator::GetInfo
  Result<Layout> DoGetInfo(InfoType info_type, const void* ptr) const override {
    return GetInfo(tracker_, info_type, ptr);
  }

  alignas(BlockType::kAlignment) std::array<std::byte, kBufferSize> buffer_{};
  AllocatorType allocator_;
  TrackingAllocator<MetricsType> tracker_;
  size_t allocate_size_;
  void* deallocate_ptr_;
  size_t deallocate_size_;
  void* resize_ptr_;
  size_t resize_old_size_;
  size_t resize_new_size_;

  bool enable_measure_fragmentation_;
  std::optional<Fragmentation> fragmentation_;
};

/// @endsubmodule

}  // namespace pw::allocator::test
