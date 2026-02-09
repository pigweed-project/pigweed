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

#include <cstddef>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_allocator/shared_ptr.h"
#include "pw_assert/assert.h"
#include "pw_bytes/span.h"

namespace pw::multibuf::v1_adapter {

// Forward declarations
class OwnedChunk;

namespace internal {
class BasicSimpleAllocator;
class ChunkAllocator;
}  // namespace internal

/// @submodule{pw_multibuf,v1_adapter}

/// A handle to a contiguous slice of data that mimics `v1::Chunk`.
///
/// This type can be used as a drop-in replacement for `v1::Chunk` while
/// migrating to using pw_multibuf/v2.
///
/// A limitation of v1_adapter chunks is that thay do not track unique ownership
/// of a subregion of memory. This means two chunks created from the same region
/// of memory, e.g. via TakePrefix and TakeSuffix, may be able to extend the
/// memory they refer to, e.g. via ClaimPrefix and ClaimSuffix, to include
/// memory referred to by another chunk.
///
/// Any calls to these methods that would succeed in v1 will still succeed, but
/// calls of the type described above which would fail in v1 may succeed with
/// this adapter.
class Chunk {
 public:
  Chunk() = delete;

  Chunk(const Chunk& other) { *this = other; }

  Chunk& operator=(const Chunk& other) {
    metadata_allocator_ = other.metadata_allocator_;
    region_ = other.region_;
    span_ = other.span_;
    return *this;
  }

  ~Chunk() = default;

  std::byte* data() { return span_.data(); }
  const std::byte* data() const { return span_.data(); }

  size_t size() const { return span_.size(); }

  [[nodiscard]] bool empty() const { return span_.empty(); }

  std::byte& operator[](size_t index) { return span_[index]; }
  const std::byte& operator[](size_t index) const { return span_[index]; }

  /// Returns the allocator used to manage metadata memory.
  ///
  /// This method does not match any in `v1::Chunk`, and is used by other
  /// `v1_adapter` types.
  constexpr Allocator* metadata_allocator() const {
    return metadata_allocator_;
  }

  /// Returns the shared memory region backing this chunk.
  ///
  /// This method does not match any in `v1::Chunk`, and is used by other
  /// `v1_adapter` types.
  constexpr const SharedPtr<std::byte[]>& region() const { return region_; }

  // Container declarations
  using element_type = std::byte;
  using value_type = std::byte;
  using size_type = size_t;
  using difference_type = ptrdiff_t;
  using pointer = std::byte*;
  using const_pointer = const std::byte*;
  using reference = std::byte&;
  using const_reference = const std::byte&;
  using iterator = std::byte*;
  using const_iterator = const std::byte*;
  using reverse_iterator = std::byte*;
  using const_reverse_iterator = const std::byte*;

  std::byte* begin() { return span_.data(); }
  const std::byte* begin() const { return cbegin(); }
  const std::byte* cbegin() const { return span_.data(); }
  std::byte* end() { return span_.data() + span_.size(); }
  const std::byte* end() const { return cend(); }
  const std::byte* cend() const { return span_.data() + span_.size(); }

  /// @copydoc pw::multibuf::v1::Chunk::CanMerge
  [[nodiscard]] bool CanMerge(const Chunk& next_chunk) const;

  /// @copydoc pw::multibuf::v1::Chunk::Merge
  bool Merge(OwnedChunk& next_chunk);

  /// @copydoc pw::multibuf::v1::Chunk::ClaimPrefix
  /// @warning See the note about subregion ownership in the class comments.
  [[nodiscard]] bool ClaimPrefix(size_t bytes_to_claim);

  /// @copydoc pw::multibuf::v1::Chunk::ClaimSuffix
  /// @warning See the note about subregion ownership in the class comments.
  [[nodiscard]] bool ClaimSuffix(size_t bytes_to_claim);

  /// @copydoc pw::multibuf::v1::Chunk::ClaimSuffix
  /// @warning See the note about subregion ownership in the class comments.
  void DiscardPrefix(size_t bytes_to_discard);

  /// @copydoc pw::multibuf::v1::Chunk::Slice
  /// @warning See the note about subregion ownership in the class comments.
  void Slice(size_t begin, size_t end);

  /// @copydoc pw::multibuf::v1::Chunk::Slice
  /// @warning See the note about subregion ownership in the class comments.
  void Truncate(size_t len);

  /// @copydoc pw::multibuf::v1::Chunk::TakePrefix
  /// @warning See the note about subregion ownership in the class comments.
  std::optional<OwnedChunk> TakePrefix(size_t bytes_to_take);

  /// @copydoc pw::multibuf::v1::Chunk::TakePrefix
  /// @warning See the note about subregion ownership in the class comments.
  std::optional<OwnedChunk> TakeSuffix(size_t bytes_to_take);

 private:
  friend class OwnedChunk;
  friend class MultiBufChunks;

  constexpr Chunk(Allocator& metadata_allocator,
                  const SharedPtr<std::byte[]>& region)
      : metadata_allocator_(&metadata_allocator),
        region_(region),
        span_(region.get(), region.size()) {}

  Allocator* metadata_allocator_;
  SharedPtr<std::byte[]> region_;
  ByteSpan span_;
};

/// An RAII handle to a contiguous slice of data that mimics `v1::OwnedChunk`.
///
/// This type can be used as a drop-in replacement for `v1::OwnedChunk` while
/// migrating to using pw_multibuf/v2.
class OwnedChunk {
 public:
  using element_type = std::byte;
  using value_type = std::byte;
  using size_type = size_t;
  using difference_type = ptrdiff_t;
  using pointer = std::byte*;
  using const_pointer = const std::byte*;
  using reference = std::byte&;
  using const_reference = const std::byte&;
  using iterator = std::byte*;
  using const_iterator = const std::byte*;
  using reverse_iterator = std::byte*;
  using const_reverse_iterator = const std::byte*;

  constexpr OwnedChunk() = default;

  OwnedChunk(Allocator& metadata_allocator, const SharedPtr<std::byte[]>& data)
      : inner_(Chunk(metadata_allocator, data)) {}

  OwnedChunk(const OwnedChunk&) = delete;
  OwnedChunk& operator=(const OwnedChunk&) = delete;

  OwnedChunk(OwnedChunk&& other) = default;
  OwnedChunk& operator=(OwnedChunk&& other) = default;

  ~OwnedChunk() = default;

  // Accessors

  std::byte* data() { return inner_.has_value() ? inner_->data() : nullptr; }
  const std::byte* data() const {
    return inner_.has_value() ? inner_->data() : nullptr;
  }

  size_t size() const { return inner_.has_value() ? inner_->size() : 0; }

  std::byte& operator[](size_t index) { return data()[index]; }
  std::byte operator[](size_t index) const { return data()[index]; }

  Chunk& operator*() { return *inner_; }
  const Chunk& operator*() const { return *inner_; }
  Chunk* operator->() { return &(*inner_); }
  const Chunk* operator->() const { return &(*inner_); }

  // Iterators

  std::byte* begin() { return data(); }
  const std::byte* begin() const { return cbegin(); }
  const std::byte* cbegin() const { return data(); }
  std::byte* end() { return data() + size(); }
  const std::byte* end() const { return cend(); }
  const std::byte* cend() const { return data() + size(); }

  // Mutators

  /// @copydoc pw::multibuf::v1::OwnedChunk::Release
  void Release() { inner_.reset(); }

  /// Returns the memory contained by this object's `Chunk` and empties this
  /// `OwnedChunk` without releasing the underlying memory.
  ///
  /// This is similar to `v1::OwnedChunk::Take`, except that returns a
  /// `std::optional<Chunk>` instead of a `Chunk*`.
  std::optional<Chunk> Take() && {
    std::optional<Chunk> result = std::move(inner_);
    inner_.reset();
    return result;
  }

 private:
  friend class Chunk;

  std::optional<Chunk> inner_;
};

/// @}

}  // namespace pw::multibuf::v1_adapter
