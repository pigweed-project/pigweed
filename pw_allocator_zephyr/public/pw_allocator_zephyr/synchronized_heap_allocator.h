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

#include "pw_allocator/abstract_allocator.h"
#include "pw_allocator/capability.h"
#include "pw_span/span.h"
#include "zephyr/kernel.h"

namespace pw::allocator_zephyr {

/// Constructs and uses a Zephyr `k_heap` object for memory allocation.
///
/// The bytes use for the heap are provided as a byte span to the constructor.
///
/// @see
/// https://docs.zephyrproject.org/latest/kernel/memory_management/heap.html#synchronized-heap-allocator
class SynchronizedHeapAllocator final
    : public pw::allocator::AbstractAllocator {
 public:
  static constexpr pw::allocator::Capabilities kCapabilities = 0;

  /// Creates a Zephyr heap for the given byte span. All allocations from this
  /// instance come from those bytes.
  /// @param[in]  buffer    The span of bytes to use for the heap.
  explicit SynchronizedHeapAllocator(pw::span<std::byte> buffer);

  /// Destroys the Zephyr heap.
  ~SynchronizedHeapAllocator() = default;

  // Disallow dynamic heap allocation of the allocator instance itself.
  static void* operator new(size_t) = delete;

  // Provide a no-op class-specific operator delete to satisfy the compiler's
  // deleting destructor vtable slot (required because AbstractAllocator has a
  // virtual destructor). On bare-metal targets without a C library heap (such
  // as picolibc without CONFIG_COMMON_LIBC_MALLOC), calling global ::operator
  // delete would pull in libstdc++'s default del_ops -> free() -> sbrk, leading
  // to undefined reference errors for __heap_start and __heap_end.
  static void operator delete(void*) noexcept {}
  static void operator delete(void*, size_t) noexcept {}

  SynchronizedHeapAllocator(const SynchronizedHeapAllocator&) = delete;
  SynchronizedHeapAllocator& operator=(const SynchronizedHeapAllocator&) =
      delete;
  SynchronizedHeapAllocator(SynchronizedHeapAllocator&&) = delete;
  SynchronizedHeapAllocator& operator=(SynchronizedHeapAllocator&&) = delete;

 private:
  /// @copydoc Allocator::Allocate
  void* DoAllocate(pw::allocator::Layout layout) override;

  /// @copydoc Allocator::Deallocate
  void DoDeallocate(void* ptr) override;

  /// @copydoc Allocator::Reallocate
  void* DoReallocate(void* ptr, pw::allocator::Layout new_layout) override;

 private:
  struct k_heap heap_;
};

}  // namespace pw::allocator_zephyr
