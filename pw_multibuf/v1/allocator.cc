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

#include "pw_multibuf/v1/allocator.h"

#include <mutex>

#include "pw_multibuf/v1/multibuf.h"

namespace pw::multibuf::v1 {

// ########## MultiBufAllocator

std::optional<MultiBuf> MultiBufAllocator::Allocate(size_t size) {
  return Allocate(size, size);
}

std::optional<MultiBuf> MultiBufAllocator::Allocate(size_t min_size,
                                                    size_t desired_size) {
  pw::Result<MultiBuf> result =
      DoAllocate(min_size, desired_size, kAllowDiscontiguous);
  if (result.ok()) {
    return std::move(*result);
  }
  return std::nullopt;
}

std::optional<MultiBuf> MultiBufAllocator::AllocateContiguous(size_t size) {
  return AllocateContiguous(size, size);
}

std::optional<MultiBuf> MultiBufAllocator::AllocateContiguous(
    size_t min_size, size_t desired_size) {
  pw::Result<MultiBuf> result =
      DoAllocate(min_size, desired_size, kNeedsContiguous);
  if (result.ok()) {
    return std::move(*result);
  }
  return std::nullopt;
}

void MultiBufAllocator::MoreMemoryAvailable(size_t size_available,
                                            size_t contiguous_size_available) {
  std::lock_guard lock(lock_);
  mem_delegates_.remove_if(
      [this, size_available, contiguous_size_available](
          const MemoryAvailableDelegate& future)
      // We properly acquire lock_ as needed for HandleMemoryAvailable in outer
      // scope, but still need PW_NO_LOCK_SAFETY_ANALYSIS because compiler can't
      // be sure when this lambda is called within that outer scope.
      PW_NO_LOCK_SAFETY_ANALYSIS {
        return future.HandleMemoryAvailable(
            *this, size_available, contiguous_size_available);
      });
}

}  // namespace pw::multibuf::v1
