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
#include <optional>

#include "pw_multibuf/v1_adapter/multibuf.h"

namespace pw::multibuf::v1_adapter {

/// @submodule{pw_multibuf,v1_adapter}

/// Interface for allocating `v1_adapter::MultiBuf` objects that mimics
/// `v1::MultiBufAllocator`.
///
/// This type can be used as a drop-in replacement for `v1::MultiBufAllocator`
/// while migrating to using pw_multibuf/v2.
///
/// Unlike `v1::MultiBufAllocator`, this interface always allocates contiguous
/// memory.
class MultiBufAllocator {
 public:
  MultiBufAllocator() = default;

  /// `MultiBufAllocator` is not copyable or movable.
  MultiBufAllocator(MultiBufAllocator&) = delete;
  MultiBufAllocator& operator=(MultiBufAllocator&) = delete;
  MultiBufAllocator(MultiBufAllocator&&) = delete;
  MultiBufAllocator& operator=(MultiBufAllocator&&) = delete;

  virtual ~MultiBufAllocator() {}

  /// @copydoc pw::multibuf::v1::MultiBufAllocator::Allocate
  std::optional<MultiBuf> Allocate(size_t min_size) {
    return DoAllocate(min_size, min_size, /*contiguous=*/false);
  }

  /// @copydoc pw::multibuf::v1::MultiBufAllocator::Allocate
  std::optional<MultiBuf> Allocate(size_t min_size, size_t desired_size) {
    return DoAllocate(min_size, desired_size, /*contiguous=*/false);
  }

  /// @copydoc pw::multibuf::v1::MultiBufAllocator::AllocateContiguous
  std::optional<MultiBuf> AllocateContiguous(size_t min_size) {
    return DoAllocate(min_size, min_size, /*contiguous=*/true);
  }

  /// @copydoc pw::multibuf::v1::MultiBufAllocator::AllocateContiguous
  std::optional<MultiBuf> AllocateContiguous(size_t min_size,
                                             size_t desired_size) {
    return DoAllocate(min_size, desired_size, /*contiguous=*/true);
  }

  /// @copydoc pw::multibuf::v1::MultiBufAllocator::GetBackingCapacity
  std::optional<size_t> GetBackingCapacity() { return DoGetBackingCapacity(); }

 private:
  /// @copydoc pw::multibuf::v1::MultiBufAllocator::Allocate
  virtual std::optional<MultiBuf> DoAllocate(size_t min_size,
                                             size_t desired_size,
                                             bool contiguous) = 0;

  /// @copydoc pw::multibuf::v1::MultiBufAllocator::GetBackingCapacity
  virtual std::optional<size_t> DoGetBackingCapacity() = 0;
};

/// @}

}  // namespace pw::multibuf::v1_adapter
