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

#include <cstdint>

#include "pw_allocator/allocator.h"
#include "pw_bytes/span.h"
#include "pw_malloc/config.h"
#include "pw_preprocessor/util.h"

/// Replacement interface for standard libc dynamic memory operations
namespace pw::malloc {

/// @module{pw_malloc}

/// Sets the memory to be used by the system allocator.
///
/// A backend can implement this method to provide the allocator returned by
/// `GetSystemAllocator` with a region of memory for it to use.
///
/// @pre This function must be implemented by the `pw_malloc` backend, but may
/// be trivially empty if the backend provides its own storage.
///
/// @param  heap            The region of memory to use as a heap.
void InitSystemAllocator(ByteSpan heap);

/// Sets the memory to be used by the system allocator.
///
/// This method provides an alternate interface that may be more convenient to
/// call with symbols defined in linker scripts.
///
/// @param  heap_low_addr   The inclusive start of the region of memory to use
///                         as a heap. This MUST be less than `heap_high_addr`.
/// @param  heap_high_addr  The exclusive end of the region of memory to use as
///                         a heap. This MUST be greater than `heap_low_addr`.
void InitSystemAllocator(void* heap_low_addr, void* heap_high_addr);

/// Sets the memory to be used by the system allocator.
///
/// This method is a generic version of `InitSystemAllocator` that works with
/// allocator types that have an `Init(ByteSpan)` method. It can be used to
/// implement `InitSystemAllocator` for specific pw_malloc backends.
///
/// This method enforces the requirement that it is only called once.
template <typename AllocatorType>
void InitSystemAllocator(ByteSpan heap);

}  // namespace pw::malloc

#if __cplusplus
PW_EXTERN_C_START
#endif

/// Legacy name for `pw::malloc::InitSystemAllocator`.
void pw_MallocInit(uint8_t* heap_low_addr, uint8_t* heap_high_addr);

#if __cplusplus
PW_EXTERN_C_END
#endif

namespace pw::malloc {

/// Returns the system allocator.
///
/// This function must be implemented to return a pointer to an allocator with a
/// global lifetime. The returned allocator must be initialized and ready to
/// use. The facade will call this function at most once.
///
/// Backends may either implement this function directly with a concrete
/// allocator type, or delegate its implementation to consumers to allow them to
/// provide their own allocator types. Backends that implement it directly
/// should use `pw_malloc_Init` to provide the region from which to allocate
/// memory.
///
/// @pre This function must be implemented by the `pw_malloc` backend.
Allocator* GetSystemAllocator();

/// Returns the metrics for the system allocator using the configured type.
///
/// In order to be thread-safe, the system metrics are only updated when either
/// this method is called. Thus, callers should call this method each time they
/// wish to examine the system allocator metrics.
///
/// For example:
/// @code{.cpp}
///   DoSomethingWithMetrics(UpdateSystemMetrics());
///
///   PerformSomeAllocationsAndFrees();
///   DoSomethingWithMetrics(UpdateSystemMetrics());
/// @endcode
const PW_MALLOC_METRICS_TYPE& UpdateSystemMetrics();

// Template method implementations.

template <typename AllocatorType>
void InitSystemAllocator(ByteSpan heap) {
  static bool initialized = false;
  PW_ASSERT(!initialized);
  auto* allocator = static_cast<AllocatorType*>(GetSystemAllocator());
  allocator->Init(heap);
  initialized = true;
}

}  // namespace pw::malloc
