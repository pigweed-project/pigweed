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
#include "pw_allocator/guarded_allocator.h"

#include "lib/stdcompat/bit.h"
#include "pw_allocator/internal/bit.h"
#include "pw_assert/check.h"

namespace pw::allocator::internal {

void DefaultGuardErrorHandler::HandleError(GuardError error,
                                           const void* ptr1,
                                           size_t val1,
                                           const void* ptr2,
                                           size_t val2) {
  switch (error.value()) {
    case GuardError::kBadMagic:
      PW_CRASH("magic value mismatch: expected=%zu, actual=%zu", val1, val2);
      break;
    default:
      DefaultFrameErrorHandler::HandleError(error, ptr1, val1, ptr2, val2);
      break;
  }
}

}  // namespace pw::allocator::internal
