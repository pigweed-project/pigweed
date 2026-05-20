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

#include "pw_allocator/metrics.h"

/// @module{pw_malloc}

#ifndef PW_MALLOC_LOCK_TYPE
/// Sets the type of synchronization primitive to use to mediate concurrent
/// allocations by the system allocator.
///
/// Defaults to `pw::allocator::NoSync`, which does no locking.
#include "pw_allocator/synchronized_allocator.h"
#define PW_MALLOC_LOCK_TYPE ::pw::allocator::NoSync
#endif  // PW_MALLOC_LOCK_TYPE

#ifndef PW_MALLOC_METRICS_TYPE
/// Sets the type of allocator metrics collected by the system allocator.
///
/// Defaults to `pw::allocator::NoMetrics`, which does no tracking.
#include "pw_allocator/tracking_allocator.h"
#define PW_MALLOC_METRICS_TYPE ::pw::allocator::NoMetrics

#elif defined(PW_MALLOC_METRICS_INCLUDE)

/// Sets the header that can be included to define the `PW_MALLOC_METRICS_TYPE`.
#include PW_MALLOC_METRICS_INCLUDE

#endif  // PW_MALLOC_METRICS_TYPE

#ifndef PW_MALLOC_BLOCK_OFFSET_TYPE
/// Sets the unsigned integer type used by `pw::allocator::BlockAllocator`s to
/// index blocks.
///
/// Larger types allow addressing more memory, but increase allocation overhead
/// from block metadata.
///
/// Defaults to platform's `uintptr_t` type.
#define PW_MALLOC_BLOCK_OFFSET_TYPE uintptr_t
#endif  // PW_MALLOC_BLOCK_OFFSET_TYPE

#ifndef PW_MALLOC_MIN_BUCKET_SIZE
/// Sets the size of the smallest ``pw::allocator::Bucket` used by an allocator.
///
/// See also `pw::allocator::BucketBlockAllocator` and
/// `pw::allocator::BuddyAllocator`.
///
/// Must be a power of two. Defaults to 32.
#define PW_MALLOC_MIN_BUCKET_SIZE 32
#endif  // PW_MALLOC_MIN_BUCKET_SIZE
static_assert(((PW_MALLOC_MIN_BUCKET_SIZE - 1) & PW_MALLOC_MIN_BUCKET_SIZE) ==
                  0,
              "PW_MALLOC_MIN_BUCKET_SIZE must be a power of two");

#ifndef PW_MALLOC_NUM_BUCKETS
/// Sets the number of ``pw::allocator::Bucket` used by an allocator.
///
/// See also `pw::allocator::BucketBlockAllocator` and
/// `pw::allocator::BuddyAllocator`.
///
/// Defaults to 5.
#define PW_MALLOC_NUM_BUCKETS 5
#endif  // PW_MALLOC_NUM_BUCKETS

#ifndef PW_MALLOC_DUAL_FIRST_FIT_THRESHOLD
/// Sets the threshold beyond which a
/// `pw::allocator::DualFirstFitBlockAllocator` considers allocations large.
///
/// See also `pw::allocator::DualFirstFitBlockAllocator`.
///
/// Defaults to 2KiB.
#define PW_MALLOC_DUAL_FIRST_FIT_THRESHOLD 2048
#endif  // PW_MALLOC_DUAL_FIRST_FIT_THRESHOLD

/// @endmodule
