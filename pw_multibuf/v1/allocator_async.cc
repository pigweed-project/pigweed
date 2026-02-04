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

#include "pw_multibuf/v1/allocator_async.h"

#include <mutex>

#include "pw_assert/check.h"       // IWYU pragma: keep
#include "pw_async2/dispatcher.h"  // IWYU pragma: keep
#include "pw_multibuf/v1/allocator.h"

namespace pw::multibuf::v1 {

using ::pw::async2::Context;
using ::pw::async2::Poll;
using ::pw::async2::PollOptional;
using ::pw::async2::Waker;

// ########## MultiBufAllocatorAsync

MultiBufAllocationFuture MultiBufAllocatorAsync::AllocateAsync(size_t size) {
  return MultiBufAllocationFuture(
      mbuf_allocator_, size, size, kAllowDiscontiguous);
}

MultiBufAllocationFuture MultiBufAllocatorAsync::AllocateAsync(
    size_t min_size, size_t desired_size) {
  return MultiBufAllocationFuture(
      mbuf_allocator_, min_size, desired_size, kAllowDiscontiguous);
}

MultiBufAllocationFuture MultiBufAllocatorAsync::AllocateContiguousAsync(
    size_t size) {
  return MultiBufAllocationFuture(
      mbuf_allocator_, size, size, kNeedsContiguous);
}

MultiBufAllocationFuture MultiBufAllocatorAsync::AllocateContiguousAsync(
    size_t min_size, size_t desired_size) {
  return MultiBufAllocationFuture(
      mbuf_allocator_, min_size, desired_size, kNeedsContiguous);
}

// ########## MultiBufAllocationFuture

bool MultiBufAllocationFuture::HandleMemoryAvailable(
    MultiBufAllocator& alloc,
    size_t size_available,
    size_t contiguous_size_available) const {
  PW_CHECK_PTR_EQ(allocator_, &alloc);
  bool should_wake_and_remove =
      ((min_size_ <= contiguous_size_available) ||
       (contiguity_requirement_ == kAllowDiscontiguous &&
        min_size_ <= size_available));
  if (should_wake_and_remove) {
    const_cast<Waker&>(waker_).Wake();
  }
  return should_wake_and_remove;
}

MultiBufAllocationFuture::MultiBufAllocationFuture(
    MultiBufAllocationFuture&& other)
    : allocator_(other.allocator_),
      waker_(),
      min_size_(other.min_size_),
      desired_size_(other.desired_size_),
      contiguity_requirement_(other.contiguity_requirement_) {
  std::lock_guard lock(allocator_->lock_);
  if (!other.unlisted()) {
    allocator_->RemoveMemoryAvailableDelegate(other);
    allocator_->AddMemoryAvailableDelegate(*this);
    // We must move the waker under the lock in order to ensure that there is no
    // race between swapping ``MultiBufAllocationFuture``s and the waker being
    // awoken by the allocator.
    waker_ = std::move(other.waker_);
  }
}

MultiBufAllocationFuture& MultiBufAllocationFuture::operator=(
    MultiBufAllocationFuture&& other) {
  {
    std::lock_guard lock(allocator_->lock_);
    if (!this->unlisted()) {
      allocator_->RemoveMemoryAvailableDelegate(*this);
    }
  }

  allocator_ = other.allocator_;
  min_size_ = other.min_size_;
  desired_size_ = other.desired_size_;
  contiguity_requirement_ = other.contiguity_requirement_;

  std::lock_guard lock(allocator_->lock_);
  if (!other.unlisted()) {
    allocator_->RemoveMemoryAvailableDelegate(other);
    allocator_->AddMemoryAvailableDelegate(*this);
    // We must move the waker under the lock in order to ensure that there is no
    // race between swapping ``MultiBufAllocationFuture``s and the waker being
    // awoken by the allocator.
    waker_ = std::move(other.waker_);
  }

  return *this;
}

MultiBufAllocationFuture::~MultiBufAllocationFuture() {
  std::lock_guard lock(allocator_->lock_);
  if (!this->unlisted()) {
    allocator_->RemoveMemoryAvailableDelegate(*this);
  }
}

void MultiBufAllocationFuture::SetDesiredSizes(
    size_t new_min_size,
    size_t new_desired_size,
    ContiguityRequirement new_contiguity_requirement) {
  // No-op if the sizes are unchanged.
  if (new_min_size == min_size_ && new_desired_size == desired_size_ &&
      new_contiguity_requirement == contiguity_requirement_) {
    return;
  }
  // Acquire the lock so the allocator doesn't touch the sizes while we're
  // modifying them.
  std::lock_guard lock(allocator_->lock_);

  // If our needs decreased, try allocating again next time rather than
  // waiting for a wake.
  if (new_min_size < min_size_ ||
      ((new_contiguity_requirement == kAllowDiscontiguous) &&
       (contiguity_requirement_ == kNeedsContiguous))) {
    if (!this->unlisted()) {
      allocator_->RemoveMemoryAvailableDelegate(*this);
    }
  }
  min_size_ = new_min_size;
  desired_size_ = new_desired_size;
  contiguity_requirement_ = new_contiguity_requirement;
}

PollOptional<MultiBuf> MultiBufAllocationFuture::Pend(Context& cx) {
  std::lock_guard lock(allocator_->lock_);
  // If we're still listed waiting for a wakeup, don't bother to try again.
  if (this->unlisted()) {
    auto result = TryAllocate();
    if (result.IsReady()) {
      return result;
    }
    allocator_->AddMemoryAvailableDelegate(*this);
  }
  // We set the waker while still holding the lock to ensure there is no gap
  // between us checking TryAllocate above and the waker being reset here.
  PW_ASYNC_STORE_WAKER(
      cx,
      waker_,
      "MultiBufAllocationFuture is waiting for memory to become available");
  return async2::Pending();
}

PollOptional<MultiBuf> MultiBufAllocationFuture::TryAllocate() {
  Result<MultiBuf> buf_opt =
      allocator_->DoAllocate(min_size_, desired_size_, contiguity_requirement_);
  if (buf_opt.ok()) {
    return async2::Ready<std::optional<MultiBuf>>(std::move(*buf_opt));
  }
  if (buf_opt.status().IsOutOfRange()) {
    return async2::Ready<std::optional<MultiBuf>>(std::nullopt);
  }
  return async2::Pending();
}

}  // namespace pw::multibuf::v1
