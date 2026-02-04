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

#include "pw_containers/intrusive_forward_list.h"
#include "pw_multibuf/config.h"
#include "pw_multibuf/v1/multibuf.h"
#include "pw_result/result.h"
#include "pw_sync/mutex.h"

namespace pw::multibuf::v1 {

/// @submodule{pw_multibuf,v1}

enum class PW_MULTIBUF_DEPRECATED ContiguityRequirement {
  kAllowDiscontiguous,
  kNeedsContiguous,
};

inline constexpr ContiguityRequirement kAllowDiscontiguous =
    ContiguityRequirement::kAllowDiscontiguous;
inline constexpr ContiguityRequirement kNeedsContiguous =
    ContiguityRequirement::kNeedsContiguous;

/// Interface for allocating ``MultiBuf`` objects.
///
/// A ``MultiBufAllocator`` differs from a regular ``pw::allocator::Allocator``
/// in that they may provide support for:
/// - Asynchronous allocation.
/// - Non-contiguous buffer allocation.
/// - Internal header/footer reservation.
/// - Size-range allocation.
///
/// In order to accomplish this, they return ``MultiBuf`` objects rather than
/// arbitrary pieces of memory.
///
/// Additionally, ``MultiBufAllocator`` implementations may choose to store
/// their allocation metadata separately from the data itself. This allows for
/// things like allocation headers to be kept out of restricted DMA-capable or
/// shared-memory regions.
///
/// NOTE: ``MultiBufAllocator``s *must* outlive any futures created from them.
class PW_MULTIBUF_DEPRECATED MultiBufAllocator {
 public:
  MultiBufAllocator() = default;

  /// ```MultiBufAllocator`` is not copyable or movable.
  MultiBufAllocator(MultiBufAllocator&) = delete;
  MultiBufAllocator& operator=(MultiBufAllocator&) = delete;
  MultiBufAllocator(MultiBufAllocator&&) = delete;
  MultiBufAllocator& operator=(MultiBufAllocator&&) = delete;

  virtual ~MultiBufAllocator() {}

  /// Attempts to allocate a ``MultiBuf`` of exactly ``size`` bytes.
  ///
  /// Memory allocated by an arbitrary ``MultiBufAllocator`` does not provide
  /// any alignment requirements, preferring instead to allow the allocator
  /// maximum flexibility for placing regions (especially discontiguous
  /// regions).
  ///
  /// @retval ``MultiBuf`` if the allocation was successful.
  /// @retval ``nullopt_t`` if the memory is not currently available.
  std::optional<MultiBuf> Allocate(size_t size);

  /// Attempts to allocate a ``MultiBuf`` of at least ``min_size`` bytes and at
  /// most ``desired_size`` bytes.
  ///
  /// Memory allocated by an arbitrary ``MultiBufAllocator`` does not provide
  /// any alignment requirements, preferring instead to allow the allocator
  /// maximum flexibility for placing regions (especially discontiguous
  /// regions).
  ///
  /// @retval ``MultiBuf`` if the allocation was successful.
  /// @retval ``nullopt_t`` if the memory is not currently available.
  std::optional<MultiBuf> Allocate(size_t min_size, size_t desired_size);

  /// Attempts to allocate a contiguous ``MultiBuf`` of exactly ``size``
  /// bytes.
  ///
  /// Memory allocated by an arbitrary ``MultiBufAllocator`` does not provide
  /// any alignment requirements, preferring instead to allow the allocator
  /// maximum flexibility for placing regions (especially discontiguous
  /// regions).
  ///
  /// @retval ``MultiBuf`` with a single ``Chunk`` if the allocation was
  /// successful.
  /// @retval ``nullopt_t`` if the memory is not currently available.
  std::optional<MultiBuf> AllocateContiguous(size_t size);

  /// Attempts to allocate a contiguous ``MultiBuf`` of at least ``min_size``
  /// bytes and at most ``desired_size`` bytes.
  ///
  /// Memory allocated by an arbitrary ``MultiBufAllocator`` does not provide
  /// any alignment requirements, preferring instead to allow the allocator
  /// maximum flexibility for placing regions (especially discontiguous
  /// regions).
  ///
  /// @retval ``MultiBuf`` with a single ``Chunk`` if the allocation was
  /// successful.
  /// @retval ``nullopt_t`` if the memory is not currently available.
  std::optional<MultiBuf> AllocateContiguous(size_t min_size,
                                             size_t desired_size);

  /// Returns the total amount of memory provided by this object.
  ///
  /// This is an optional method. Some memory providers may not have an easily
  /// defined capacity, e.g. the system allocator.
  ///
  /// @retval the total memory if known.
  /// @retval ``nullopt_t`` if the total memory is not knowable.
  std::optional<size_t> GetBackingCapacity() { return DoGetBackingCapacity(); }

 protected:
  /// Awakens callers asynchronously waiting for allocations of at most
  /// ``size_available`` bytes or at most ``contiguous_size_available``
  /// contiguous bytes.
  ///
  /// This function should be invoked by implementations of
  /// ``MultiBufAllocator`` when more memory becomes available to allocate.
  void MoreMemoryAvailable(size_t size_available,
                           size_t contiguous_size_available);

 private:
  friend class MultiBufAllocationFuture;

  // Instances of this class are informed when more memory becomes available.
  class MemoryAvailableDelegate
      : public IntrusiveForwardList<MemoryAvailableDelegate>::Item {
   public:
    explicit MemoryAvailableDelegate() = default;
    MemoryAvailableDelegate(MemoryAvailableDelegate&) = delete;
    MemoryAvailableDelegate& operator=(MemoryAvailableDelegate&) = delete;
    MemoryAvailableDelegate(MemoryAvailableDelegate&&) = delete;
    MemoryAvailableDelegate& operator=(MemoryAvailableDelegate&&) = delete;
    virtual ~MemoryAvailableDelegate() = default;

    // Callback from allocator when new memory being available. Function should
    // return true if object's need has been met which also indicates the object
    // can be released by the allocator.
    virtual bool HandleMemoryAvailable(MultiBufAllocator& alloc,
                                       size_t size_available,
                                       size_t contiguous_size_available) const
        PW_EXCLUSIVE_LOCKS_REQUIRED(lock_) = 0;
  };

  void AddMemoryAvailableDelegate(MemoryAvailableDelegate& delegate)
      PW_EXCLUSIVE_LOCKS_REQUIRED(lock_) {
    mem_delegates_.push_front(delegate);
  }

  void RemoveMemoryAvailableDelegate(MemoryAvailableDelegate& delegate)
      PW_EXCLUSIVE_LOCKS_REQUIRED(lock_) {
    mem_delegates_.remove(delegate);
  }

  /// Attempts to allocate a `MultiBuf` of at least `min_size` bytes and at most
  /// `desired_size` bytes.
  ///
  /// @returns @Result{the buffer}
  /// * @OK: Returns the buffer if the allocation was successful.
  /// * @RESOURCE_EXHAUSTED: Insufficient memory is available currently.
  /// * @OUT_OF_RANGE: This amount of memory will not become possible to
  ///   allocate in the future, or this allocator is unable to signal via
  ///   `MoreMemoryAvailable` (this will result in asynchronous allocations
  ///   failing immediately on OOM).
  virtual pw::Result<MultiBuf> DoAllocate(
      size_t min_size,
      size_t desired_size,
      ContiguityRequirement contiguity_requirement) = 0;

  /// @copydoc MultiBufAllocator::GetBackingCapacity
  virtual std::optional<size_t> DoGetBackingCapacity() = 0;

  sync::Mutex lock_;
  IntrusiveForwardList<MemoryAvailableDelegate> mem_delegates_
      PW_GUARDED_BY(lock_);
};

/// @}

}  // namespace pw::multibuf::v1
