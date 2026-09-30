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

#include "pw_allocator/framing_allocator.h"

#include "pw_assert/check.h"

namespace pw::allocator::internal {

void DefaultFrameErrorHandler::HandleError(FrameError error,
                                           const void* ptr1,
                                           size_t val1,
                                           const void* ptr2,
                                           size_t val2) {
  switch (error.value()) {
    case FrameError::kDataNotAligned:
      PW_CRASH(
          "data pointer %p is not aligned to a %zu byte boundary", ptr1, val1);
      break;
    case FrameError::kDataTooSmall:
      PW_CRASH("data pointer %p is too low to get prefix offset", ptr1);
      break;
    case FrameError::kDataTooSmallForPrefix:
      PW_CRASH("data pointer %p is too low for prefix offset %zu", ptr1, val1);
      break;
    case FrameError::kFrameNull:
      PW_CRASH("frame pointer is null");
      break;
    case FrameError::kFrameNotAligned:
      PW_CRASH(
          "frame pointer %p is not aligned to a %zu byte boundary", ptr1, val1);
      break;
    case FrameError::kFrameTooSmall:
      PW_CRASH(
          "frame pointer %p has a size of %zu that is too small to get a frame "
          "offset",
          ptr1,
          val1);
      break;
    case FrameError::kFrameOffsetTooSmall:
      PW_CRASH(
          "frame pointer %p has a size of %zu that is smaller than its frame "
          "offset of %zu",
          ptr1,
          val1,
          val2);
      break;
    case FrameError::kSuffixNotAligned:
      PW_CRASH("suffix pointer 0x%zx is not aligned to a %zu byte boundary",
               val1,
               val2);
      break;
    case FrameError::kDataTooSmallForSuffix:
      PW_CRASH(
          "data pointer %p has a size of %zu that is too small for a suffix "
          "offset of %zu",
          ptr1,
          val1,
          val2);
      break;
    case FrameError::kUnrecognizedFrame:
      PW_CRASH("frame pointer %p not recognized by allocator", ptr1);
      break;
    case FrameError::kFramePointerMismatch:
      PW_CRASH(
          "frame pointer %p does not match GetFrame(GetData(%p)), which is %p",
          ptr1,
          ptr1,
          ptr2);
      break;
    case FrameError::kDataPointerMismatch:
      PW_CRASH(
          "data pointer %p does not match GetData(GetFrame(%p)), which is %p",
          ptr1,
          ptr1,
          ptr2);
      break;
    case FrameError::kMaxValue:
      // No-op; handled by chained error handlers.
      break;
  }
}

}  // namespace pw::allocator::internal
