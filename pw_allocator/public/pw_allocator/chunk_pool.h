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
#include <cstdint>

#include "pw_allocator/abstract_pool.h"
#include "pw_allocator/capability.h"
#include "pw_allocator/layout.h"
#include "pw_bytes/span.h"
#include "pw_result/result.h"
#include "pw_status/status.h"

namespace pw::allocator {

/// @submodule{pw_allocator,concrete}

/// Implementation of ``Pool`` that uses a list of free chunks.
///
/// The first ``sizeof(void*)`` bytes of each free chunk is used to store a
/// pointer to the next free chunk, or null for the last free chunk.
class ChunkPool : public AbstractPool {
 public:
  static constexpr Capabilities kCapabilities =
      kImplementsGetRequestedLayout | kImplementsGetUsableLayout |
      kImplementsGetAllocatedLayout | kImplementsGetCapacity |
      kImplementsRecognizes;
  static constexpr size_t kMinSize = sizeof(void*);
  static constexpr size_t kMinAlignment = alignof(void*);

  /// Construct a `Pool` that allocates from a region of memory.
  ///
  /// @param  region      The memory to allocate from. Must be large enough to
  ///                     allocate at least one chunk with the given layout.
  /// @param  layout      The size and alignment of the memory to be returned
  ///                     from this pool.
  ChunkPool(ByteSpan region, const Layout& layout);

 protected:
  /// @copydoc Pool::Allocate
  void* DoAllocate() override;

  /// @copydoc Deallocator::Deallocate
  void DoDeallocate(void* ptr) override;

  /// @copydoc Deallocator::GetInfo
  Result<Layout> DoGetInfo(InfoType info_type, const void* ptr) const override;

 private:
  const Layout allocated_layout_;
  uintptr_t start_;
  uintptr_t end_;
  std::byte* next_;
};

/// @}

}  // namespace pw::allocator
