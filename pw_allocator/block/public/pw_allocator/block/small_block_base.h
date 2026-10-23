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

#include <algorithm>
#include <cstddef>
#include <limits>

#include "pw_allocator/block/allocatable.h"
#include "pw_allocator/block/basic.h"
#include "pw_allocator/block/contiguous.h"
#include "pw_allocator/block/iterable.h"
#include "pw_allocator/bucket/fast_sorted.h"
#include "pw_bytes/span.h"

namespace pw::allocator {

/// @submodule{pw_allocator,block}

/// CRTP-style base class for block implementations with limited code size and
/// memory overhead.
///
/// This implementation encodes its metadata in a header consisting of only two
/// firelds of type `T`. It implements only the block mix-ins necessary to be
/// used with a `BlockAllocator`.
///
/// @tparam   Derived   Block implementation derived from this class.
/// @tparam   T         Field type used to store metadara.
/// @tparam   kShift    Encoded sizes are left shifted by this amount to
///                     produce actual sizes. A larger value allows a larger
///                     maximum addressable size, at the cost of a larger
///                     minimum allocatable size.
template <typename Derived, typename T, size_t kShift = 0>
struct SmallBlockBase : public BasicBlock<Derived>,
                        public ContiguousBlock<Derived>,
                        public IterableBlock<Derived>,
                        public AllocatableBlock<Derived> {
 protected:
  constexpr explicit SmallBlockBase(size_t outer_size)
      : prev_and_free_(1U),
        next_and_last_(static_cast<T>(outer_size >> kShift) | 1U) {}

 private:
  using Basic = BasicBlock<Derived>;
  using Contiguous = ContiguousBlock<Derived>;
  using Allocatable = AllocatableBlock<Derived>;

  // `Basic` required methods.
  friend Basic;

  static constexpr size_t DefaultAlignment() {
    return std::max(alignof(GenericFastSortedItem), size_t(2u) << kShift);
  }

  static constexpr size_t BlockOverhead() { return sizeof(Derived); }

  static constexpr size_t MaxAddressableSize() {
    return size_t(std::numeric_limits<T>::max()) << kShift;
  }

  static inline Derived* AsBlock(ByteSpan bytes) {
    return ::new (bytes.data()) Derived(bytes.size());
  }

  static constexpr size_t MinInnerSize() { return std::max(2U, 1U << kShift); }

  static constexpr size_t ReservedWhenFree() {
    return sizeof(GenericFastSortedItem);
  }

  size_t OuterSizeUnchecked() const { return (next_and_last_ & ~1U) << kShift; }

  // `Basic` overrides.
  bool DoCheckInvariants(bool strict) const {
    return Basic::DoCheckInvariants(strict) &&
           Contiguous::DoCheckInvariants(strict);
  }

  // `Contiguous` required methods.
  friend Contiguous;

  constexpr bool IsLastUnchecked() const { return (next_and_last_ & 1U) != 0; }

  constexpr void SetNext(size_t outer_size, Derived* next);

  constexpr size_t PrevOuterSizeUnchecked() const {
    return (prev_and_free_ & ~1U) << kShift;
  }

  // `Allocatable` required methods.
  friend Allocatable;

  constexpr bool IsFreeUnchecked() const { return (prev_and_free_ & 1U) != 0; }

  constexpr void SetFree(bool is_free) {
    prev_and_free_ =
        static_cast<T>((prev_and_free_ & ~1U) | (is_free ? 1U : 0U));
  }

  T prev_and_free_;
  T next_and_last_;
};

/// @}

// Template and inline method implementations.

template <typename Derived, typename T, size_t kShift>
constexpr void SmallBlockBase<Derived, T, kShift>::SetNext(size_t outer_size,
                                                           Derived* next) {
  auto packed_size = static_cast<T>(outer_size >> kShift);
  if (next == nullptr) {
    next_and_last_ = packed_size | 1U;
  } else {
    next_and_last_ = packed_size;
    next->prev_and_free_ = packed_size | (next->prev_and_free_ & 1U);
  }
}

}  // namespace pw::allocator
