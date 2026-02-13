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

#include "pw_allocator/deallocator.h"
#include "pw_allocator/unique_ptr.h"
#include "pw_bytes/span.h"
#include "pw_function/function.h"
#include "pw_multibuf/v1_adapter/chunk.h"
#include "pw_multibuf/v1_adapter/internal/chunk_allocator.h"
#include "pw_multibuf/v1_adapter/multibuf.h"

namespace pw::multibuf::v1_adapter {
namespace {

/// Self-destructing deallocator.
///
/// This helper type wraps the "deleter" function in a heap-allocated type that
/// can be referenced by a `SharedPtr`. When the associated memory is
/// deallocated, this object deletes itself.
class FromSpanChunkAllocator : public internal::SingleChunkAllocator {
 private:
  using Base = internal::SingleChunkAllocator;

 public:
  FromSpanChunkAllocator(Allocator& metadata_allocator,
                         ByteSpan region,
                         Function<void(ByteSpan)>&& deleter)
      : Base(region, metadata_allocator), deleter_(std::move(deleter)) {}

 private:
  /// @copydoc Deallocator::Allocate
  void DoDeallocate(void* ptr) override {
    Base::DoDeallocate(ptr);
    if (num_allocations() == 0 && control_block_free()) {
      // This corresponds to the call to `New` in `FromSpan`.
      metadata_allocator().Delete<FromSpanChunkAllocator>(this);
    }
  }

  /// @copydoc BasicChunkAllocator::TryDeallocateRegion
  size_t TryDeallocateRegion(void* ptr) override {
    size_t released = Base::TryDeallocateRegion(ptr);
    if (released != 0) {
      deleter_(buffer());
    }
    return released;
  }

  Function<void(ByteSpan)> deleter_;
};

}  // namespace

std::optional<MultiBuf> FromSpan(Allocator& metadata_allocator,
                                 ByteSpan region,
                                 Function<void(ByteSpan)>&& deleter) {
  MultiBuf buf(metadata_allocator);
  if (!buf->TryReserveChunks(1)) {
    return std::nullopt;
  }
  auto* allocator = metadata_allocator.New<FromSpanChunkAllocator>(
      metadata_allocator, region, std::move(deleter));
  if (allocator == nullptr) {
    return std::nullopt;
  }
  std::optional<OwnedChunk> chunk = allocator->AllocateChunk(region.size());
  return std::make_optional(MultiBuf::FromChunk(std::move(*chunk)));
}

}  // namespace pw::multibuf::v1_adapter
