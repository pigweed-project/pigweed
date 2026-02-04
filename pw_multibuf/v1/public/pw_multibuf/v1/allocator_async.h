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

#include <optional>

#include "pw_async2/dispatcher.h"  // IWYU pragma: keep
#include "pw_async2/poll.h"
#include "pw_multibuf/config.h"
#include "pw_multibuf/v1/allocator.h"

namespace pw::multibuf::v1 {

/// @submodule{pw_multibuf,v1}

class MultiBufAllocationFuture;

class PW_MULTIBUF_DEPRECATED MultiBufAllocatorAsync {
  // : public MultiBufAllocator::MoreMemoryDelegate {
 public:
  MultiBufAllocatorAsync(MultiBufAllocator& mbuf_allocator)
      : mbuf_allocator_(mbuf_allocator) {}

  /// ```MultiBufAllocatorAsync`` is not copyable or movable.
  MultiBufAllocatorAsync(MultiBufAllocatorAsync&) = delete;
  MultiBufAllocatorAsync& operator=(MultiBufAllocatorAsync&) = delete;
  MultiBufAllocatorAsync(MultiBufAllocatorAsync&&) = delete;
  MultiBufAllocatorAsync& operator=(MultiBufAllocatorAsync&&) = delete;

  /// Asynchronously allocates a ``MultiBuf`` of exactly ``size`` bytes.
  ///
  /// Memory allocated by an arbitrary ``MultiBufAllocator`` does not provide
  /// any alignment requirements, preferring instead to allow the allocator
  /// maximum flexibility for placing regions (especially discontiguous
  /// regions).
  ///
  /// @retval A ``MultiBufAllocationFuture`` which will yield a ``MultiBuf``
  /// when one is available.
  MultiBufAllocationFuture AllocateAsync(size_t size);

  /// Asynchronously allocates a ``MultiBuf`` of at least
  /// ``min_size`` bytes and at most ``desired_size` bytes.
  ///
  /// Memory allocated by an arbitrary ``MultiBufAllocator`` does not provide
  /// any alignment requirements, preferring instead to allow the allocator
  /// maximum flexibility for placing regions (especially discontiguous
  /// regions).
  ///
  /// @retval A ``MultiBufAllocationFuture`` which will yield a ``MultiBuf``
  /// when one is available.
  MultiBufAllocationFuture AllocateAsync(size_t min_size, size_t desired_size);

  /// Asynchronously allocates a contiguous ``MultiBuf`` of exactly ``size``
  /// bytes.
  ///
  /// Memory allocated by an arbitrary ``MultiBufAllocator`` does not provide
  /// any alignment requirements, preferring instead to allow the allocator
  /// maximum flexibility for placing regions (especially discontiguous
  /// regions).
  ///
  /// @retval A ``MultiBufAllocationFuture`` which will yield an ``MultiBuf``
  /// consisting of a single ``Chunk`` when one is available.
  MultiBufAllocationFuture AllocateContiguousAsync(size_t size);

  /// Asynchronously allocates an ``OwnedChunk`` of at least
  /// ``min_size`` bytes and at most ``desired_size`` bytes.
  ///
  /// @retval A ``MultiBufAllocationFuture`` which will yield an ``MultiBuf``
  /// consisting of a single ``Chunk`` when one is available.
  MultiBufAllocationFuture AllocateContiguousAsync(size_t min_size,
                                                   size_t desired_size);

 private:
  MultiBufAllocator& mbuf_allocator_;
};

/// An object that asynchronously yields a ``MultiBuf`` when ``Pend``ed.
///
/// See ``pw::async2`` for details on ``Pend`` and how it is used to build
/// asynchronous tasks.
class PW_MULTIBUF_DEPRECATED MultiBufAllocationFuture
    : public MultiBufAllocator::MemoryAvailableDelegate {
 public:
  constexpr explicit MultiBufAllocationFuture(MultiBufAllocator& allocator)
      : allocator_(&allocator),
        min_size_(0),
        desired_size_(0),
        contiguity_requirement_(kAllowDiscontiguous) {}
  MultiBufAllocationFuture(MultiBufAllocator& allocator,
                           size_t min_size,
                           size_t desired_size,
                           ContiguityRequirement contiguity_requirement)
      : allocator_(&allocator),
        min_size_(min_size),
        desired_size_(desired_size),
        contiguity_requirement_(contiguity_requirement) {}

  MultiBufAllocationFuture(MultiBufAllocationFuture&&);
  MultiBufAllocationFuture& operator=(MultiBufAllocationFuture&&);
  ~MultiBufAllocationFuture() override;

  void SetDesiredSize(size_t min_size) {
    SetDesiredSizes(min_size, min_size, kAllowDiscontiguous);
  }
  void SetDesiredSizes(size_t min_size,
                       size_t desired_size,
                       ContiguityRequirement contiguity_requirement);
  async2::PollOptional<MultiBuf> Pend(async2::Context& cx);

  /// Returns the ``allocator`` associated with this future.
  MultiBufAllocator& allocator() { return *allocator_; }
  size_t min_size() const { return min_size_; }
  size_t desired_size() const { return desired_size_; }
  bool needs_contiguous() const {
    return contiguity_requirement_ == kNeedsContiguous;
  }

 private:
  friend class MultiBufAllocator;

  // Handle new memory being available. Return true if our need has been met (so
  // we can be destroyed by owner).
  bool HandleMemoryAvailable(MultiBufAllocator& alloc,
                             size_t size_available,
                             size_t contiguous_size_available) const override;

  /// Attempts to allocate with the stored parameters.
  async2::PollOptional<MultiBuf> TryAllocate();

  // The allocator this future is tied to.
  MultiBufAllocator* allocator_;

  // The waker to wake when a suitably-sized allocation becomes available.
  async2::Waker waker_;

  // The properties of the kind of allocation being waited for.
  //
  // These properties can only be mutated while holding the allocator's lock,
  // however the MultiBufAllocationFuture owner can freely read these values
  // without needing to acquire the lock.
  //
  // The allocator may read these values so long as this value is listed and
  // the allocator holds the lock.
  size_t min_size_;
  size_t desired_size_;
  ContiguityRequirement contiguity_requirement_;
};

/// @}

}  // namespace pw::multibuf::v1
