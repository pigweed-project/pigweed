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

/// A memory allocator that always fails to allocate memory.
///
/// A null allocator may be useful as part of a larger framework if allocation
/// should be disallowed under certain circumstances. For example, a function
/// that returns different allocators based on an input parameter may return a
/// null allocator when given an invalid or unsupported parameter value.
class NullAllocator final : public AbstractAllocator {
 public:
  static constexpr Capabilities kCapabilities = 0;

  constexpr NullAllocator() : AbstractAllocator(kCapabilities) {}

 protected:
  /// @copydoc Allocator::Allocate
  void* DoAllocate([[maybe_unused]] Layout layout) override { return nullptr; }

  /// @copydoc Allocator::Deallocate
  void DoDeallocate([[maybe_unused]] void* ptr) override {}
};

/// Returns a reference to a NullAllocator singleton.
NullAllocator& GetNullAllocator();

/// @}

}  // namespace pw::allocator
