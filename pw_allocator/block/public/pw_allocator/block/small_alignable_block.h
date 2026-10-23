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
#include <cstdint>
#include <utility>

#include "pw_allocator/block/alignable.h"
#include "pw_allocator/block/allocatable.h"
#include "pw_allocator/block/result.h"
#include "pw_allocator/block/small_block_base.h"
#include "pw_allocator/layout.h"
#include "pw_status/status_with_size.h"

namespace pw::allocator {

/// @submodule{pw_allocator,block_impl}

/// An alignable version of `SmallBlock`.
class SmallAlignableBlock
    : public SmallBlockBase<SmallAlignableBlock, uint32_t, 0>,
      public AlignableBlock<SmallAlignableBlock> {
 private:
  friend SmallBlockBase<SmallAlignableBlock, uint32_t, 0>;
  constexpr explicit SmallAlignableBlock(size_t outer_size)
      : SmallBlockBase(outer_size) {}

  using Allocatable = AllocatableBlock<SmallAlignableBlock>;
  friend Allocatable;

  // `Alignable` overrides.
  using Alignable = AlignableBlock<SmallAlignableBlock>;
  friend Alignable;

  constexpr StatusWithSize DoCanAlloc(Layout layout) const {
    return Alignable::DoCanAlloc(layout);
  }

  static constexpr BlockResult<SmallAlignableBlock> DoAllocFirst(
      SmallAlignableBlock*&& block, Layout layout) {
    return Alignable::DoAllocFirst(std::move(block), layout);
  }

  static constexpr BlockResult<SmallAlignableBlock> DoAllocLast(
      SmallAlignableBlock*&& block, Layout layout) {
    return Alignable::DoAllocLast(std::move(block), layout);
  }
};

/// @}

}  // namespace pw::allocator
