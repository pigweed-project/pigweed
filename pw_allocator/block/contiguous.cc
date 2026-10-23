// Copyright 2020 The Pigweed Authors
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

#include "pw_allocator/block/contiguous.h"

#include "pw_allocator/hardening.h"
#include "pw_assert/check.h"

namespace pw::allocator::internal {

// TODO: b/234875269 - Add stack tracing to locate which call to the heap
// operation caused the corruption in the methods below.

void CheckNextMisaligned(const void* block,
                         const void* next,
                         bool next_is_aligned) {
  if constexpr (Hardening::kIncludesDebugChecks) {
    PW_CHECK(
        next_is_aligned,
        "A block (%p) is corrupted: it has a 'next' field (%p) that is not "
        "properly aligned.",
        block,
        next);
  }
}

void CheckNextPrevMismatched(const void* block,
                             const void* next,
                             const void* next_prev,
                             bool next_prev_matches) {
  if constexpr (Hardening::kIncludesDebugChecks) {
    PW_CHECK(next_prev_matches,
             "A block (%p) is corrupted: its 'next' field (%p) has a 'prev' "
             "field (%p) that does not match the block.",
             block,
             next,
             next_prev);
  }
}

void CheckPrevMisaligned(const void* block,
                         const void* prev,
                         bool prev_is_aligned) {
  if constexpr (Hardening::kIncludesDebugChecks) {
    PW_CHECK(
        prev_is_aligned,
        "A block (%p) is corrupted: it has a 'prev' field (%p) that is not "
        "properly aligned.",
        block,
        prev);
  }
}

void CheckPrevNextMismatched(const void* block,
                             const void* prev,
                             const void* prev_next,
                             bool prev_next_matches) {
  if constexpr (Hardening::kIncludesDebugChecks) {
    PW_CHECK(prev_next_matches,
             "A block (%p) is corrupted: its 'prev' field (%p) has a 'next' "
             "field (%p) that does not match the block.",
             block,
             prev,
             prev_next);
  }
}

}  // namespace pw::allocator::internal
