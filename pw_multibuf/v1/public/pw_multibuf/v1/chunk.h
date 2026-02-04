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

#include <optional>
#include <utility>

#include "pw_assert/assert.h"
#include "pw_bytes/span.h"
#include "pw_multibuf/config.h"
#include "pw_sync/mutex.h"

namespace pw::multibuf::v1 {

/// @submodule{pw_multibuf,v1}

class ChunkRegionTracker;
class OwnedChunk;

/// A handle to a contiguous slice of data.
///
/// A `Chunk` is similar to a `ByteSpan`, but is aware of its underlying memory
/// region, and is able to split, shrink, and grow into neighboring empty space
/// within its region.
///
/// This class is optimized to allow multiple owners to write into neighboring
/// regions of the same allocation. In order to support zero-copy DMA of
/// communications buffers, allocators can create properly-aligned `Chunk`
/// regions inside an allocation.
///
/// One example usecase for this is communication drivers that want to reserve
/// space at the front or rear of a buffer for headers or footers. A driver can
/// `DiscardPrefix` in order to reserve bytes for headers, `Truncate` in order
/// to reserve bytes for footers, and then pass the `Chunk` to the user to fill
/// in. These discarded bytes are still held by the underlying region, so the
/// header and footer space can later be reclaimed using the `ClaimPrefix` and
/// `ClaimSuffix` methods. The region itself is only released once there are no
/// remaining Chunks within it.
class PW_MULTIBUF_DEPRECATED Chunk {
 public:
  Chunk() = delete;
  // Not copyable or movable.
  Chunk(Chunk&) = delete;
  Chunk& operator=(Chunk&) = delete;
  Chunk(Chunk&&) = delete;
  Chunk& operator=(Chunk&&) = delete;

  std::byte* data() { return span_.data(); }
  const std::byte* data() const { return span_.data(); }
  size_t size() const { return span_.size(); }
  [[nodiscard]] bool empty() const { return span_.empty(); }

  std::byte& operator[](size_t index) { return span_[index]; }
  const std::byte& operator[](size_t index) const { return span_[index]; }

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

  /// Returns if `next_chunk` is mergeable into the end of this `Chunk`.
  ///
  /// This will only succeed when the two `Chunk` s are adjacent in memory and
  /// originated from the same region.
  [[nodiscard]] bool CanMerge(const Chunk& next_chunk) const;

  /// Attempts to merge `next_chunk` into the end of this `Chunk`.
  ///
  /// If the chunks are successfully merged, this `Chunk` will be extended
  /// forwards to encompass the space of `next_chunk`, and `next_chunk` will be
  /// emptied and `Release`d.
  ///
  /// This will only succeed when the two `Chunk` s are adjacent in memory and
  /// originated from the same region.
  ///
  /// If the chunks are not mergeable, neither `Chunk` will be modified.
  bool Merge(OwnedChunk& next_chunk);

  /// Attempts to add `bytes_to_claim` to the front of this buffer by advancing
  /// its range backwards in memory. Returns `true` if the operation succeeded.
  ///
  /// This will only succeed if this `Chunk` points to a section of a region
  /// that has unreferenced bytes preceding it. For example, a `Chunk` which
  /// has been shrunk using `DiscardPrefix` can be re-expanded using
  /// `ClaimPrefix`.
  ///
  /// This method will acquire a mutex and is not IRQ safe.
  [[nodiscard]] bool ClaimPrefix(size_t bytes_to_claim);

  /// Attempts to add `bytes_to_claim` to the front of this buffer by advancing
  /// its range forwards in memory. Returns `true` if the operation succeeded.
  ///
  /// This will only succeed if this `Chunk` points to a section of a region
  /// that has unreferenced bytes following it. For example, a `Chunk` which has
  /// been shrunk using `Truncate` can be re-expanded using
  ///
  /// This method will acquire a mutex and is not IRQ safe.
  [[nodiscard]] bool ClaimSuffix(size_t bytes_to_claim);

  /// Shrinks this handle to refer to the data beginning at offset
  /// `bytes_to_discard`.
  ///
  /// Does not modify the underlying data. The discarded memory continues to be
  /// held by the underlying region as long as any `Chunk`s exist within it.
  /// This allows the memory to be later reclaimed using `ClaimPrefix`.
  ///
  /// This method will acquire a mutex and is not IRQ safe.
  void DiscardPrefix(size_t bytes_to_discard);

  /// Shrinks this handle to refer to data in the range `begin..<end`.
  ///
  /// Does not modify the underlying data. The discarded memory continues to be
  /// held by the underlying region as long as any `Chunk`s exist within it.
  /// This allows the memory to be later reclaimed using `ClaimPrefix` or
  /// `ClaimSuffix`.
  ///
  /// This method will acquire a mutex and is not IRQ safe.
  void Slice(size_t begin, size_t end);

  /// Shrinks this handle to refer to only the first `len` bytes.
  ///
  /// Does not modify the underlying data. The discarded memory continues to be
  /// held by the underlying region as long as any `Chunk`s exist within it.
  /// This allows the memory to be later reclaimed using `ClaimSuffix`.
  ///
  /// This method will acquire a mutex and is not IRQ safe.
  void Truncate(size_t len);

  /// Attempts to shrink this handle to refer to the data beginning at offset
  /// `bytes_to_take`, returning the first `bytes_to_take` bytes as a new
  /// `OwnedChunk`.
  ///
  /// If the inner call to `AllocateChunkClass` fails, this function will return
  /// `std::nullopt` and this handle's span will not change.
  ///
  /// This method will acquire a mutex and is not IRQ safe.
  std::optional<OwnedChunk> TakePrefix(size_t bytes_to_take);

  /// Attempts to shrink this handle to refer only the first `len -
  /// bytes_to_take` bytes, returning the last `bytes_to_take` bytes as a new
  /// `OwnedChunk`.
  ///
  /// If the inner call to `AllocateChunkClass` fails, this function will return
  /// `std::nullopt` and this handle's span will not change.
  ///
  /// This method will acquire a mutex and is not IRQ safe.
  std::optional<OwnedChunk> TakeSuffix(size_t bytes_to_take);

 private:
  Chunk(ChunkRegionTracker* region_tracker, ByteSpan span)
      : region_tracker_(region_tracker),
        next_in_region_(nullptr),
        prev_in_region_(nullptr),
        next_in_buf_(nullptr),
        span_(span) {}

  // NOTE: these functions are logically
  // `PW_EXCLUSIVE_LOCKS_REQUIRED(region_tracker_->lock_)`, however this
  // annotation cannot be applied due to the forward declaration of
  // `region_tracker_`.
  void InsertAfterInRegionList(Chunk* new_chunk);
  void InsertBeforeInRegionList(Chunk* new_chunk);
  void RemoveFromRegionList();

  /// Frees this `Chunk`. Future accesses to this class after calls to `Free`
  /// are undefined behavior.
  ///
  /// Decrements the reference count on the underlying chunk of data.
  ///
  /// Does not modify the underlying data, but may cause it to be deallocated if
  /// this was the only remaining `Chunk` referring to its region.
  ///
  /// This method will acquire a mutex and is not IRQ safe.
  void Free();

  /// The region_tracker_ for this chunk.
  ChunkRegionTracker* const region_tracker_;

  /// Pointer to the next chunk in the same `region_tracker_`.
  ///
  /// Guarded by `region_tracker_->lock_` (but not annotated as such due to the
  /// fact that annotations aren't smart enough to understand that all chunks
  /// within a region share a tracker + lock).
  ///
  /// `nullptr` if this is the last `Chunk` in its `region_tracker_`.
  Chunk* next_in_region_;

  /// Pointer to the previous chunk in the same `region_tracker_`.
  ///
  /// Guarded by `region_tracker_->lock_` (but not annotated as such due to the
  /// fact that annotations aren't smart enough to understand that all chunks
  /// within a region share a tracker + lock).
  ///
  /// `nullptr` if this is the first `Chunk` in its `region_tracker_`.
  Chunk* prev_in_region_;

  /// Pointer to the next chunk in a `MultiBuf`.
  ///
  /// This is `nullptr` if this chunk is not in a `MultiBuf`, or is the last
  /// element of a `MultiBuf`.
  ///
  /// Reserved for use by the MultiBuf class.
  Chunk* next_in_buf_;

  /// Pointer to the sub-region to which this chunk has exclusive access.
  ///
  /// This `span_` is conceptually owned by this `Chunk` object, and may be read
  /// or written to by a `Chunk` user (the normal rules of thread compatibility
  /// apply-- `const` methods may be called concurrently, non-`const` methods
  /// may not).
  ///
  /// Neighboring `Chunk` objects in this region looking to expand via
  /// `ClaimPrefix` or `ClaimSuffix` may read this span's boundaries. These
  /// other `Chunk`s must acquire `region_tracker_->lock_` before reading, and
  /// this `Chunk` must acquire `region_tracker_->lock_` before changing
  /// `span_`.
  ByteSpan span_;

  friend class ChunkRegionTracker;  // For the constructor
  friend class OwnedChunk;          // for `Free`.
  friend class MultiBuf;            // for `next_in_buf_`.
  friend class MultiBufChunks;      // for `Free` and `next_in_buf_`.
};

/// An object that manages a single allocated region which is referenced by one
/// or more `Chunk` objects.
///
/// This class is typically implemented by `MultiBufAllocator` implementations
/// in order to customize the behavior of region deallocation.
///
/// `ChunkRegionTracker` s have three responsibilities:
///
/// - Tracking the region of memory into which `Chunk` s can expand. This is
///   reported via the `Region` method. `Chunk` s in this region can refer to
///   memory within this region sparsely, but they can grow or shrink so long as
///   they stay within the bounds of the `Region`.
///
/// - Deallocating the region and the `ChunkRegionTracker` itself. This is
///   implemented via the `Destroy` method, which is called once all of the
///   `Chunk` s in a region have been released.
///
/// - Allocating and deallocating space for `Chunk` classes. This is merely
///   allocating space for the `Chunk` object itself, not the memory to which it
///   refers. This can be implemented straightforwardly by delegating to an
///   existing generic allocator such as `malloc` or a
///   `pw::allocator::Allocator` implementation.
class PW_MULTIBUF_DEPRECATED ChunkRegionTracker {
 public:
  /// Creates the first `Chunk` referencing a whole region of memory.
  ///
  /// This must only be called once per `ChunkRegionTracker`, when the region is
  /// first created. Multiple calls will result in undefined behavior.
  ///
  /// Returns `std::nullopt` if `AllocateChunkStorage` returns `nullptr`.
  std::optional<OwnedChunk> CreateFirstChunk();

 protected:
  ChunkRegionTracker() = default;
  virtual ~ChunkRegionTracker() = default;

  /// Destroys the `ChunkRegionTracker`.
  ///
  /// Typical implementations will call `std::destroy_at(this)` and then free
  /// the memory associated with the region and the tracker.
  virtual void Destroy() = 0;

  /// Returns the entire span of the region being managed.
  ///
  /// `Chunk` s referencing this tracker will not expand beyond this region, nor
  /// into one another's portions of the region.
  ///
  /// This region does not provide any alignment guarantees by default.
  ///
  /// This region must not change for the lifetime of this `ChunkRegionTracker`.
  virtual ByteSpan Region() const = 0;

  /// Returns a pointer to `sizeof(Chunk)` bytes with `alignas(Chunk)`. Returns
  /// `nullptr` on failure.
  virtual void* AllocateChunkClass() = 0;

  /// Deallocates a pointer returned by `AllocateChunkClass`.
  virtual void DeallocateChunkClass(void*) = 0;

 private:
  /// A lock used to manage an internal linked-list of `Chunk`s.
  ///
  /// This allows chunks to:
  ///
  /// - know whether they can expand to fill neighboring regions of memory.
  /// - know when the last chunk has been destructed, triggering `Destroy`.
  pw::sync::Mutex lock_;
  friend Chunk;
};

/// An RAII handle to a contiguous slice of data.
///
/// Note: `OwnedChunk` may acquire a `pw::sync::Mutex` during destruction, and
/// so must not be destroyed within ISR contexts.
class PW_MULTIBUF_DEPRECATED OwnedChunk {
 public:
  OwnedChunk() : inner_(nullptr) {}
  OwnedChunk(OwnedChunk&& other) noexcept : inner_(other.inner_) {
    other.inner_ = nullptr;
  }
  OwnedChunk& operator=(OwnedChunk&& other) noexcept {
    inner_ = other.inner_;
    other.inner_ = nullptr;
    return *this;
  }
  /// This method will acquire a mutex and is not IRQ safe.
  ~OwnedChunk() { Release(); }

  std::byte* data() {
    return const_cast<std::byte*>(std::as_const(*this).data());
  }
  const std::byte* data() const {
    return inner_ == nullptr ? nullptr : inner_->data();
  }

  size_t size() const { return inner_ == nullptr ? 0 : inner_->size(); }

  std::byte& operator[](size_t index) { return data()[index]; }
  std::byte operator[](size_t index) const { return data()[index]; }

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

  std::byte* begin() { return data(); }
  const std::byte* begin() const { return cbegin(); }
  const std::byte* cbegin() const { return data(); }
  std::byte* end() { return data() + size(); }
  const std::byte* end() const { return cend(); }
  const std::byte* cend() const { return data() + size(); }

  Chunk& operator*() { return *inner_; }
  const Chunk& operator*() const { return *inner_; }
  Chunk* operator->() { return inner_; }
  const Chunk* operator->() const { return inner_; }

  /// Decrements the reference count on the underlying chunk of data and empties
  /// this handle so that `span()` now returns an empty (zero-sized) span.
  ///
  /// Does not modify the underlying data, but may cause it to be deallocated if
  /// this was the only remaining `Chunk` referring to its region.
  ///
  /// This method is equivalent to `{ Chunk _unused = std::move(chunk_ref); }`
  ///
  /// This method will acquire a mutex and is not IRQ safe.
  void Release();

  /// Returns the contained `Chunk*` and empties this `OwnedChunk` without
  /// releasing the underlying `Chunk`.
  Chunk* Take() && {
    Chunk* result = inner_;
    inner_ = nullptr;
    return result;
  }

 private:
  /// Constructs a new `OwnedChunk` from an existing `Chunk*`.
  OwnedChunk(Chunk* inner) : inner_(inner) {}

  /// A pointer to the owned `Chunk`.
  Chunk* inner_;

  /// Allow `ChunkRegionTracker` and `MultiBuf` to create `OwnedChunk`s using
  /// the private constructor above.
  friend class Chunk;
  friend class ChunkRegionTracker;
  friend class MultiBuf;
  friend class MultiBufChunks;
};

/// @}

}  // namespace pw::multibuf::v1
