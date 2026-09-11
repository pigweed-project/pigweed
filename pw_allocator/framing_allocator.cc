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

namespace pw::allocator {
namespace internal {

bool BaseFramingAllocator::CrashOnUnalignedIfStrict(bool strict,
                                                    const void* data) {
  if (strict) {
    PW_CRASH("data pointer %p is not properly aligned to a %zu byte boundary",
             data,
             alignof(size_t));
  }
  return false;
}

bool BaseFramingAllocator::CrashOnBadDataIfStrict(bool strict,
                                                  const void* data) {
  if (strict) {
    PW_CRASH("data pointer %p cannot be converted to a frame", data);
  }
  return false;
}

bool BaseFramingAllocator::CrashOnBadPrefixOffsetIfStrict(bool strict,
                                                          const void* data,
                                                          size_t prefix_offset,
                                                          size_t min_size) {
  if (strict) {
    if (min_size == 0) {
      PW_CRASH(
          "data pointer %p has a prefix offset of %zu which appears corrupted: "
          "should be aligned to %zu bytes",
          data,
          prefix_offset,
          alignof(size_t));
    } else {
      PW_CRASH(
          "data pointer %p has a prefix offset of %zu which appears corrupted: "
          "should be at least %zu bytes and aligned to %zu bytes",
          data,
          prefix_offset,
          min_size,
          alignof(size_t));
    }
  }
  return false;
}

bool BaseFramingAllocator::CrashOnWrongPrefixOffsetIfStrict(
    bool strict,
    const void* data,
    size_t data_prefix_offset,
    const void* frame,
    size_t frame_prefix_offset) {
  if (strict) {
    PW_CRASH(
        "data pointer %p has a prefix offset of %zu which does not match the "
        "prefix offset of frame pointer %p, which is %zu",
        data,
        data_prefix_offset,
        frame,
        frame_prefix_offset);
  }
  return false;
}

bool BaseFramingAllocator::CrashOnBadSuffixOffsetIfStrict(bool strict,
                                                          const void* data,
                                                          size_t suffix_offset,
                                                          size_t usable_size) {
  if (strict) {
    PW_CRASH(
        "data pointer %p has a suffix offset of %zu which appears corrupted: "
        "exceeds usable memory of %zu bytes",
        data,
        suffix_offset,
        usable_size);
  }
  return false;
}

bool BaseFramingAllocator::CrashOnUnrecognized(bool strict, const void* frame) {
  if (strict) {
    PW_CRASH(
        "frame pointer %p is not recognized as an allocation from the "
        "underlying allocator",
        frame);
  }
  return false;
}

}  // namespace internal
}  // namespace pw::allocator
