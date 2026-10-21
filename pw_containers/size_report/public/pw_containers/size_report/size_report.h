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

#include <array>    // IWYU pragma: export
#include <cstddef>  // IWYU pragma: export
#include <cstdint>  // IWYU pragma: export
#include <utility>  // IWYU pragma: export

#include "pw_allocator/layout.h"          // IWYU pragma: export
#include "pw_allocator/libc_allocator.h"  // IWYU pragma: export
#include "pw_bloat/bloat_this_binary.h"   // IWYU pragma: export

namespace pw::containers::size_report {

inline constexpr size_t kNumItems = 10;

// A few type aliases for convenience in the size reports.
using K1 = uint32_t;
using K2 = uint16_t;
using V1 = uint32_t;
using V2 = uint64_t;

/// Returns a reference to a std::array of statically-allocated items.
template <typename T>
std::array<T, kNumItems>& GetItems() {
  static std::array<T, kNumItems> items = {
      T(0), T(1), T(2), T(3), T(4), T(5), T(6), T(7), T(8), T(9)};
  return items;
}

/// Returns a reference to a std::array of statically-allocated pairs of keys
/// and values.
template <typename PairType>
std::array<PairType, kNumItems>& GetPairs() {
  static std::array<PairType, kNumItems> pairs = {
      PairType{0, 1},
      PairType{1, 1},
      PairType{2, 2},
      PairType{3, 3},
      PairType{4, 5},
      PairType{5, 8},
      PairType{6, 13},
      PairType{7, 21},
      PairType{8, 34},
      PairType{9, 55},
  };
  return pairs;
}

/// Returns a statically allocated container of the given type.
template <typename Container>
Container& GetContainer() {
  static Container container;
  return container;
}

/// Returns a statically allocated container of the given type, constructed with
/// the provided arguments.
template <typename Container, typename... Args>
Container& GetContainer(Args&&... args) {
  static Container container(std::forward<Args>(args)...);
  return container;
}

/// Measures the size of common functions and data without any containers.
inline uint32_t SetBaseline(uint32_t mask) {
  pw::bloat::BloatThisBinary();
  PW_BLOAT_COND(!GetItems<K1>().empty(), mask);
  PW_BLOAT_COND(!GetItems<K2>().empty(), mask);
  PW_BLOAT_COND(!GetItems<V1>().empty(), mask);
  PW_BLOAT_COND(!GetItems<V2>().empty(), mask);

  volatile size_t size = 123;
  void* alloc = allocator::GetLibCAllocator().Allocate(allocator::Layout(size));
  PW_BLOAT_COND(alloc != nullptr, mask);
  volatile size_t resize = 128;
  PW_BLOAT_COND(pw::allocator::GetLibCAllocator().Resize(alloc, resize), mask);
  pw::allocator::GetLibCAllocator().Deallocate(alloc);

  volatile size_t count = 3;
  alloc = allocator::GetLibCAllocator().Allocate(
      allocator::Layout::Of<V1[]>(count));
  PW_BLOAT_COND(alloc != nullptr, mask);
  pw::allocator::GetLibCAllocator().Deallocate(alloc);

  alloc = allocator::GetLibCAllocator().Allocate(
      allocator::Layout::Of<V2[]>(count));
  PW_BLOAT_COND(alloc != nullptr, mask);
  pw::allocator::GetLibCAllocator().Deallocate(alloc);
  return mask;
}

/// Invokes methods common to all containers.
template <typename Container>
uint32_t MeasureContainer(Container& container, uint32_t mask) {
  PW_BLOAT_COND(container.empty(), mask);
  PW_BLOAT_COND(container.size() <= container.max_size(), mask);
  return mask;
}

}  // namespace pw::containers::size_report
