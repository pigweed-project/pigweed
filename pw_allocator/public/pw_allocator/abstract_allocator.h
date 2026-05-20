// Copyright 2026 The Pigweed Authors
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

#include "pw_allocator/allocator.h"
#include "pw_allocator/capability.h"

namespace pw::allocator {

/// @submodule{pw_allocator,concrete}

/// A memory allocator that provides default implementations for optional
/// methods.
///
/// This type isn't instantiated directly, but is provided to make it easier to
/// define other allocator types.
class AbstractAllocator : public pw::Allocator {
 protected:
  constexpr explicit AbstractAllocator(Capabilities capabilities)
      : pw::Allocator(capabilities) {}

  /// @copydoc Allocator::Resize
  bool DoResize([[maybe_unused]] void* ptr,
                [[maybe_unused]] size_t new_size) override {
    return false;
  }

  /// @copydoc Allocator::GetAllocated
  size_t DoGetAllocated() const override { return size_t(-1); }

  /// @copydoc Allocator::MeasureFragmentation
  std::optional<Fragmentation> DoMeasureFragmentation() const override {
    return std::nullopt;
  }

  /// @copydoc Deallocator::GetInfo
  Result<Layout> DoGetInfo(InfoType, const void*) const override {
    return Status::Unimplemented();
  }
};

/// @}

}  // namespace pw::allocator
