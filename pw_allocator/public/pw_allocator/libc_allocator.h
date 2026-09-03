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

#include "pw_allocator/abstract_allocator.h"
#include "pw_allocator/capability.h"

namespace pw::allocator {

/// @submodule{pw_allocator,concrete}

/// Memory allocator that uses `malloc` and `free`.
///
/// As a result of using `malloc`, this allocator always uses an alignment of
/// `std::align_max_t`.
class LibCAllocator final : public AbstractAllocator {
 public:
  static constexpr Capabilities kCapabilities = 0;

  constexpr LibCAllocator() : AbstractAllocator(kCapabilities) {}

 private:
  /// @copydoc Allocator::Allocate
  void* DoAllocate(Layout layout) override;

  /// @copydoc Deallocator::Deallocate
  void DoDeallocate(void* ptr) override;

  /// @copydoc Allocator::Reallocate
  void* DoReallocate(void* ptr, Layout new_layout) override;

  static LibCAllocator kSingleton;
};

/// Returns a reference to a LibCAllocator singleton.
LibCAllocator& GetLibCAllocator();

/// @}

}  // namespace pw::allocator
