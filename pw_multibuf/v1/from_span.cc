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

#include "pw_multibuf/v1/from_span.h"

namespace pw::multibuf::v1 {
namespace {

/// A simple ``ChunkRegionTracker`` that calls ``deleter`` for destruction.
class SpanTracker final : public ChunkRegionTracker {
 private:
  class PrivateConstructorType {};
  static constexpr PrivateConstructorType kPrivateConstructor{};

 public:
  static std::optional<OwnedChunk> New(Allocator& alloc,
                                       ByteSpan region,
                                       pw::Function<void(ByteSpan)>&& deleter) {
    SpanTracker* tracker = alloc.New<SpanTracker>(
        kPrivateConstructor, alloc, region, std::move(deleter));
    if (tracker == nullptr) {
      return std::nullopt;
    }
    std::optional<OwnedChunk> chunk = tracker->CreateFirstChunk();
    if (!chunk.has_value()) {
      // Delete without destroying the associated memory.
      alloc.Delete(tracker);
      return std::nullopt;
    }
    return chunk;
  }

  SpanTracker(PrivateConstructorType,
              pw::Allocator& alloc,
              ByteSpan region,
              pw::Function<void(ByteSpan)>&& deleter)
      : alloc_(alloc), region_(region), deleter_(std::move(deleter)) {}

  ~SpanTracker() final = default;

  // SpanTracker is not copyable nor movable.
  SpanTracker(const SpanTracker&) = delete;
  SpanTracker& operator=(const SpanTracker&) = delete;
  SpanTracker(SpanTracker&&) = delete;
  SpanTracker& operator=(SpanTracker&&) = delete;

 private:
  void Destroy() final {
    deleter_(region_);
    alloc_.Delete(this);
  }

  ByteSpan Region() const final { return region_; }

  void* AllocateChunkClass() final {
    return alloc_.Allocate(allocator::Layout::Of<Chunk>());
  }

  void DeallocateChunkClass(void* ptr) final { return alloc_.Deallocate(ptr); }

  pw::Allocator& alloc_;
  const ByteSpan region_;
  pw::Function<void(ByteSpan)> deleter_;
};

}  // namespace

std::optional<MultiBuf> FromSpan(pw::Allocator& metadata_allocator,
                                 ByteSpan region,
                                 pw::Function<void(ByteSpan)>&& deleter) {
  std::optional<OwnedChunk> chunk =
      SpanTracker::New(metadata_allocator, region, std::move(deleter));
  if (chunk == std::nullopt) {
    return std::nullopt;
  }
  return MultiBuf::FromChunk(std::move(*chunk));
}

}  // namespace pw::multibuf::v1
